/**********************************************************************************************
 *
 * VRScene.cpp
 *
 * Site B VR: feeds the game world to the GPU renderer.  See VRScene.h.
 *
 * - Meshes are converted once (cached by mesh) into GPU vertex/index buffers, split by
 *   surface; polygon winding is made consistent from each polygon's plane.
 * - Skinned meshes (dinosaurs, Anne) are skinned on the CPU each frame from the game's own
 *   joint transforms and sent as per-frame meshes.
 * - Textures are converted from the game's palettised (and bump-mapped) formats once.
 * - The terrain is sampled into a height grid when a level starts, and its textures (the
 *   level's terrain texture objects) are painted into one large atlas on the GPU.
 *
 **********************************************************************************************/

#include "precomp.h"
#pragma hdrstop

#include "VRScene.h"
#include "VRRender.h"
#include "VRBackend.h"

#include "Lib/Control/VRShared.hpp"
#include "Lib/EntityDBase/WorldDBase.hpp"
#include "Lib/EntityDBase/Instance.hpp"
#include "Lib/EntityDBase/Animate.hpp"
#include "Lib/EntityDBase/PhysicsInfo.hpp"
#include "Lib/EntityDBase/Query/QRenderer.hpp"
#include "Lib/EntityDBase/Query/QTerrain.hpp"
#include "Lib/GeomDBase/Partition.hpp"
#include "Lib/GeomDBase/Mesh.hpp"
#include "Lib/GeomDBase/Skeleton.hpp"
#include "Lib/GeomDBase/Terrain.hpp"
#include "Lib/GeomDBase/TerrainObj.hpp"
#include "Lib/GeomDBase/WaveletQuadTreeQuery.hpp"
#include "Lib/Physics/InfoSkeleton.hpp"
#include "Lib/Physics/Magnet.hpp"
#include "Lib/Renderer/Camera.hpp"
#include "Lib/Renderer/Light.hpp"
#include "Lib/Renderer/Texture.hpp"
#include "Lib/Renderer/Fog.hpp"
#include "Lib/Renderer/Sky.hpp"
#include "Lib/Renderer/Particles.hpp"
#include "Lib/Renderer/LightBlend.hpp"
#include "Lib/Renderer/PipeLine.hpp"
#include "Lib/EntityDBase/Animal.hpp"
#include "Lib/EntityDBase/Water.hpp"
#include "Lib/View/Raster.hpp"
#include "Lib/View/Palette.hpp"
#include "Lib/View/Clut.hpp"
#include "Lib/Loader/TextureManager.hpp"
#include "Lib/Sys/VirtualMem.hpp"
#include "Game/DesignDaemon/Player.hpp"
#include "Game/DesignDaemon/Gun.hpp"
#include "Lib/Trigger/Trigger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>

namespace
{
	//******************************************************************************************
	//
	// Caches.
	//
	struct SSubset
	{
		int                    iFirst;
		int                    iCount;
		const CMesh::SSurface* psf;
	};

	struct SMeshEntry
	{
		int                   iHandle = 0;		// Static GPU mesh (rigid meshes).
		uint32                u4Points = 0, u4Polys = 0;
		float                 fRadius = 0;
		bool                  bSkinned = false;
		std::vector<SSubset>  asub;
		std::vector<uint32_t> au4Indices;		// All indices, by subset.
		std::vector<int>      aiTriPoly;		// Polygon of each triangle (skinned only).
		float                 afUV0[2] = { 0, 0 };	// First vertex's texture coordinates when cached (scrolling).
	};

	// Animated textures (surf, waterfalls, flickering): advanced once a frame.
	std::unordered_set<const CMesh*> setAnimatedThisFrame;

	std::unordered_map<const CMesh*, SMeshEntry> mapMesh;
	std::unordered_map<const void*, int>         mapTex;		// Raster (or texture) -> handle.
	std::unordered_map<const void*, bool>        mapTexAlpha;

	const CTerrain* ptrrLevel = nullptr;
	const void*     pvLevelPlayer = nullptr;
	bool            bReady = false;
	double          dSkyTime = 0;
	int             iSkyTexture = 0;
	const void*     pvSkyTexture = nullptr;
	char            strStats[512] = "";
	int             iObjects = 0, iSkinned = 0, iTexturesWaiting = 0;

	std::vector<SVRRVertex> avScratch;
	std::vector<uint32_t>   au4Scratch;

	//******************************************************************************************
	inline void SceneLog(const char* str_fmt, double d0 = 0, double d1 = 0, double d2 = 0, double d3 = 0)
	{
		if (gVR.pfnLog)
			gVR.pfnLog(str_fmt, d0, d1, d2, d3);
	}

	inline uint32_t u4RGBA(int r, int g, int b, int a = 255)
	{
		return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
	}

	inline uint32_t u4From565(uint16 u)
	{
		int r = (u >> 11) & 31, g = (u >> 5) & 63, b = u & 31;
		return u4RGBA((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
	}

	//******************************************************************************************
	//
	// Textures.
	//
	int iTexture(const CTexture* ptex, bool& rb_alpha)
	{
		rb_alpha = false;
		if (!ptex)
			return 0;

		bool b_transparent = ptex->seterfFeatures[erfTRANSPARENT];
		bool b_alpha_colour = ptex->seterfFeatures[erfALPHA_COLOUR];

		// Solid colour.
		if (ptex->iGetNumMipLevels() <= 0)
		{
			auto it = mapTex.find(ptex);
			if (it != mapTex.end())
				return it->second;
			uint32_t u4 = u4From565(ptex->tpSolid);
			if (!b_alpha_colour && (((uint32)ptex->d3dpixColour >> 24) != 0))
			{
				// The exact colour the card would use (the screen-format one may not be 565).
				uint32 u = (uint32)ptex->d3dpixColour;
				u4 = u4RGBA((u >> 16) & 255, (u >> 8) & 255, u & 255);
			}
			if (b_alpha_colour)
			{
				// See-through colour: stored as the card's 0xAARRGGBB.
				uint32 u = (uint32)ptex->d3dpixColour;
				u4 = u4RGBA((u >> 16) & 255, (u >> 8) & 255, u & 255, (u >> 24) & 255);
			}
			int i_h = VRR_TextureCreate(1, 1, &u4);
			mapTex[ptex] = i_h;
			return i_h;
		}

		rptr<CRaster> pras = ptex->prasGetTexture(0);
		const CRaster* praw = pras.ptGet();
		if (!praw || !praw->pSurface || praw->iWidth <= 0 || praw->iHeight <= 0)
			return 0;

		auto it = mapTex.find(praw);
		if (it != mapTex.end())
		{
			rb_alpha = mapTexAlpha[praw];
			return it->second;
		}

		// The game streams texture pixels in from disk as they are needed; until a texture's
		// pixels are in memory, ask for them and draw it plain for now.
		{
			uint32 u4_bytes = (uint32)praw->iLinePixels * (uint32)praw->iHeight * (uint32)(praw->iPixelBits / 8);
			if (gtxmTexMan.pvmeTextures && !gtxmTexMan.bAllPresent() && !gtxmTexMan.bIsAvailable(praw->pSurface, u4_bytes))
			{
				gtxmTexMan.RequestMemory(praw->pSurface, u4_bytes);
				iTexturesWaiting++;
				return 0;
			}
		}

		const CPal* ppal = praw->pxf.ppalAttached;
		int i_pal = ppal ? (int)ppal->aclrPalette.uLen : 0;
		auto pal = [&](int i) -> uint32_t
		{
			if (i < 0 || i >= i_pal)
				return u4RGBA(128, 128, 128);
			const CColour& clr = ppal->aclrPalette[i];
			return u4RGBA(clr.u1Red, clr.u1Green, clr.u1Blue);
		};

		int i_w = praw->iWidth, i_h = praw->iHeight, i_stride = praw->iLinePixels;
		std::vector<uint32_t> au4((size_t)i_w * i_h);
		bool b_any_clear = false;
		const uint8* pu1_row = (const uint8*)praw->pSurface;
		int i_bytes = praw->iPixelBits / 8;
		for (int y = 0; y < i_h; y++)
		{
			const uint8* pu1 = pu1_row + (size_t)y * i_stride * i_bytes;
			uint32_t* pu4 = &au4[(size_t)y * i_w];
			for (int x = 0; x < i_w; x++)
			{
				uint32_t u4;
				int i_index = -1;
				switch (praw->iPixelBits)
				{
					case 8:
						i_index = pu1[x];
						u4 = pal(i_index);
						break;
					case 16:
					{
						uint16 u = ((const uint16*)pu1)[x];
						if (b_alpha_colour)
						{
							// See-through textures (flashes, glass): 4 bits each of alpha, red,
							// green, blue.
							u4 = u4RGBA(((u >> 8) & 15) * 17, ((u >> 4) & 15) * 17, (u & 15) * 17, ((u >> 12) & 15) * 17);
						}
						else if (ptex->seterfFeatures[erfBUMP] && ppal)
						{
							i_index = (u >> 10) & 63;
							u4 = pal(i_index);
						}
						else
							u4 = u4From565(u);
						break;
					}
					case 32:
					{
						uint32 u = ((const uint32*)pu1)[x];
						if (ppal)
						{
							i_index = (int)(u >> 24);
							u4 = pal(i_index);
						}
						else
							u4 = u4RGBA((u >> 16) & 255, (u >> 8) & 255, u & 255);
						break;
					}
					default:
						u4 = u4RGBA(128, 128, 128);
						break;
				}
				// (Bump-mapped textures have no see-through texels: colour 0 is a real colour.)
				if (b_transparent && i_index == 0 && !ptex->seterfFeatures[erfBUMP])
				{
					u4 &= 0x00FFFFFF;
					b_any_clear = true;
				}
				pu4[x] = u4;
			}
		}

		// See-through texels take the colour of the nearest solid texel (spreading outward a
		// ring at a time), so filtering and mipmaps never pull in the key colour (the blue
		// fringes and splotches).
		if (b_any_clear)
		{
			std::vector<uint8_t> au1_done((size_t)i_w * i_h, 0);
			std::vector<int> ai_front, ai_next;
			for (int i = 0; i < i_w * i_h; i++)
				if (au4[i] >> 24)
				{
					au1_done[i] = 1;
					ai_front.push_back(i);
				}
			if (ai_front.empty())
			{
				for (uint32_t& u4 : au4)
					u4 &= 0x00808080;		// Wholly see-through: a neutral colour.
			}
			while (!ai_front.empty())
			{
				ai_next.clear();
				for (int i : ai_front)
				{
					int x = i % i_w, y = i / i_w;
					const int ai_dx[4] = { 1, -1, 0, 0 }, ai_dy[4] = { 0, 0, 1, -1 };
					for (int k = 0; k < 4; k++)
					{
						int nx = (x + ai_dx[k] + i_w) % i_w, ny = (y + ai_dy[k] + i_h) % i_h;
						int n = ny * i_w + nx;
						if (au1_done[n])
							continue;
						au1_done[n] = 1;
						au4[n] = au4[i] & 0x00FFFFFF;
						ai_next.push_back(n);
					}
				}
				ai_front.swap(ai_next);
			}
		}

		int i_handle = VRR_TextureCreate(i_w, i_h, au4.data());
		mapTex[praw] = i_handle;
		mapTexAlpha[praw] = b_any_clear;
		rb_alpha = b_any_clear;
		return i_handle;
	}

	//******************************************************************************************
	//
	// Meshes.
	//
	SMeshEntry* pmeGet(const CMesh* pmsh, bool b_skinned)
	{
		auto it = mapMesh.find(pmsh);
		if (it != mapMesh.end() && it->second.u4Points == pmsh->pav3Points.uLen && it->second.u4Polys == pmsh->pampPolygons.uLen)
			return &it->second;

		SMeshEntry me;
		me.u4Points = pmsh->pav3Points.uLen;
		me.u4Polys = pmsh->pampPolygons.uLen;
		me.bSkinned = b_skinned;
		if (pmsh->pamvVertices.uLen > 0)
		{
			me.afUV0[0] = pmsh->pamvVertices[0].tcTex.tX;
			me.afUV0[1] = pmsh->pamvVertices[0].tcTex.tY;
		}

		const CMesh::SVertex* pmv0 = pmsh->pamvVertices.atArray;
		int i_verts = (int)pmsh->pamvVertices.uLen;

		// Group polygons by surface.
		std::vector<std::pair<const CMesh::SSurface*, int>> apoly;
		apoly.reserve(pmsh->pampPolygons.uLen);
		for (uint u = 0; u < pmsh->pampPolygons.uLen; u++)
		{
			const CMesh::SPolygon& mp = pmsh->pampPolygons[u];
			if (mp.bOcclude || mp.bCache || mp.papmvVertices.uLen < 3)
				continue;
			apoly.push_back(std::make_pair((const CMesh::SSurface*)mp.pSurface, (int)u));
		}
		std::stable_sort(apoly.begin(), apoly.end(), [](const std::pair<const CMesh::SSurface*, int>& a, const std::pair<const CMesh::SSurface*, int>& b)
		{
			return a.first < b.first;
		});

		for (size_t k = 0; k < apoly.size(); k++)
		{
			if (me.asub.empty() || me.asub.back().psf != apoly[k].first)
			{
				SSubset sub = { (int)me.au4Indices.size(), 0, apoly[k].first };
				me.asub.push_back(sub);
			}
			const CMesh::SPolygon& mp = pmsh->pampPolygons[apoly[k].second];
			int i_n = (int)mp.papmvVertices.uLen;

			// Winding: counter-clockwise around the polygon's outward normal.
			const CVector3<>& p0 = *mp.papmvVertices[0]->pv3Point;
			CVector3<> v3_sum(0, 0, 0);
			for (int i = 1; i + 1 < i_n; i++)
				v3_sum += (*mp.papmvVertices[i]->pv3Point - p0) ^ (*mp.papmvVertices[i + 1]->pv3Point - p0);
			bool b_reverse = (v3_sum * mp.plPlane.d3Normal) < 0;

			for (int i = 1; i + 1 < i_n; i++)
			{
				int a = (int)(mp.papmvVertices[0] - pmv0);
				int b = (int)(mp.papmvVertices[i] - pmv0);
				int c = (int)(mp.papmvVertices[i + 1] - pmv0);
				if (a < 0 || b < 0 || c < 0 || a >= i_verts || b >= i_verts || c >= i_verts)
					continue;
				if (b_reverse)
					std::swap(b, c);
				me.au4Indices.push_back(a);
				me.au4Indices.push_back(b);
				me.au4Indices.push_back(c);
				me.asub.back().iCount += 3;
				if (b_skinned)
					me.aiTriPoly.push_back(apoly[k].second);
			}
		}

		// Radius.
		float f_r2 = 0;
		for (uint u = 0; u < pmsh->pav3Points.uLen; u++)
			f_r2 = std::max(f_r2, (float)pmsh->pav3Points[u].tLenSqr());
		me.fRadius = std::sqrt(f_r2);

		if (!b_skinned && !me.au4Indices.empty())
		{
			avScratch.resize(i_verts);
			for (int i = 0; i < i_verts; i++)
			{
				const CMesh::SVertex& mv = pmv0[i];
				SVRRVertex& v = avScratch[i];
				v.afPos[0] = mv.pv3Point->tX; v.afPos[1] = mv.pv3Point->tY; v.afPos[2] = mv.pv3Point->tZ;
				v.afNormal[0] = mv.d3Normal.tX; v.afNormal[1] = mv.d3Normal.tY; v.afNormal[2] = mv.d3Normal.tZ;
				v.afUV[0] = mv.tcTex.tX; v.afUV[1] = mv.tcTex.tY;
			}
			me.iHandle = VRR_MeshCreate(avScratch.data(), i_verts, me.au4Indices.data(), (int)me.au4Indices.size());
			me.au4Indices.clear();
			me.au4Indices.shrink_to_fit();
		}

		mapMesh[pmsh] = me;
		return &mapMesh[pmsh];
	}

	//******************************************************************************************
	void WorldRows(const CPresence3<>& pr3, float af[12])
	{
		CVector3<> ax = CVector3<>(pr3.rScale, 0, 0) * pr3.r3Rot;
		CVector3<> ay = CVector3<>(0, pr3.rScale, 0) * pr3.r3Rot;
		CVector3<> az = CVector3<>(0, 0, pr3.rScale) * pr3.r3Rot;
		af[0] = ax.tX; af[1] = ay.tX; af[2]  = az.tX; af[3]  = pr3.v3Pos.tX;
		af[4] = ax.tY; af[5] = ay.tY; af[6]  = az.tY; af[7]  = pr3.v3Pos.tY;
		af[8] = ax.tZ; af[9] = ay.tZ; af[10] = az.tZ; af[11] = pr3.v3Pos.tZ;
	}

	// Queues draws of every subset of a mesh.
	void AddSubsets(const SMeshEntry& me, int i_mesh, const float af_world[12], uint32_t u4_flags, bool b_decal, uint32_t u4_force = 0, int i_tex_force = -1,
	                const float* pf_uv = nullptr)
	{
		for (const SSubset& sub : me.asub)
		{
			if (sub.iCount <= 0)
				continue;
			bool b_alpha = false;
			const CTexture* ptex = sub.psf ? sub.psf->ptexTexture.ptGet() : nullptr;
			SVRRDraw drw;
			drw.iMesh = i_mesh;
			drw.iFirstIndex = sub.iFirst;
			drw.iNumIndices = sub.iCount;
			drw.iTexture = iTexture(ptex, b_alpha);
			std::memcpy(drw.afWorld, af_world, sizeof(drw.afWorld));
			drw.u4Flags = u4_flags | (b_alpha ? (evrrALPHA_TEST | evrrTWO_SIDED) : 0);
			if (ptex && ptex->seterfFeatures[erfALPHA_COLOUR] && !b_decal)
				drw.u4Flags = (u4_flags & ~evrrALPHA_TEST) | evrrBLEND | evrrUNLIT | evrrTWO_SIDED | evrrNO_SHADOW;
			if (u4_force)
				drw.u4Flags = u4_force;
			if (i_tex_force >= 0)
				drw.iTexture = i_tex_force;
			if (pf_uv)
			{
				drw.afUVOffset[0] = pf_uv[0];
				drw.afUVOffset[1] = pf_uv[1];
			}
			if (b_decal)
				VRR_TerrainAddDecal(drw);
			else
				VRR_AddDraw(drw);
		}
	}

	//******************************************************************************************
	//
	// Skinned meshes: positions from the game's joint transforms, every frame.
	//
	void DrawSkinned(CInstance* pins, const CBioMesh* pbm, SMeshEntry& me, const float af_world[12])
	{
		CAnimate* pani = ptCast<CAnimate>(pins);
		if (!pani)
			return;

		const CPhysicsInfo* pphi = pins->pphiGetPhysicsInfo();
		const CPhysicsInfoSkeleton* pphis = pphi ? pphi->ppisCast() : nullptr;
		if (pphis)
			pphis->UpdateSkeleton(pins);

		int i_transforms = (int)pani->sriBones.patf3JointTransforms.size();
		if (i_transforms <= 0)
			return;

		const CMesh::SVertex* pmv0 = pbm->pamvVertices.atArray;
		int i_verts = (int)pbm->pamvVertices.uLen;
		avScratch.resize(i_verts);
		for (int i = 0; i < i_verts; i++)
		{
			const CMesh::SVertex& mv = pmv0[i];
			int i_pt = (int)(mv.pv3Point - pbm->pav3Points.atArray);
			int i_j = (i_pt >= 0 && i_pt < (int)pbm->pauJoints.uLen) ? (int)pbm->pauJoints[i_pt] : 0;
			if (i_j < 0 || i_j >= i_transforms)
				i_j = 0;
			const CTransform3<>& tf3 = pani->sriBones.patf3JointTransforms[i_j];
			CVector3<> v3 = *mv.pv3Point * tf3;
			CVector3<> v3_n = ((const CVector3<>&)mv.d3Normal) * tf3.mx3Mat;
			SVRRVertex& v = avScratch[i];
			v.afPos[0] = v3.tX; v.afPos[1] = v3.tY; v.afPos[2] = v3.tZ;
			v.afNormal[0] = v3_n.tX; v.afNormal[1] = v3_n.tY; v.afNormal[2] = v3_n.tZ;
			v.afUV[0] = mv.tcTex.tX; v.afUV[1] = mv.tcTex.tY;
		}

		// Anne's own left arm stub is left out while the VR left arm is shown.
		const uint8* pu1_hide = nullptr;
		if (pins == gVR.pvStubInstance && gVR.pu1StubPolys && gVR.iStubPolys == (int)pbm->pampPolygons.uLen)
			pu1_hide = gVR.pu1StubPolys;

		const std::vector<uint32_t>* pau4 = &me.au4Indices;
		std::vector<SSubset> asub_local;
		const std::vector<SSubset>* pasub = &me.asub;
		if (pu1_hide)
		{
			au4Scratch.clear();
			for (const SSubset& sub : me.asub)
			{
				SSubset s = { (int)au4Scratch.size(), 0, sub.psf };
				for (int t = sub.iFirst / 3; t < (sub.iFirst + sub.iCount) / 3; t++)
				{
					if (pu1_hide[me.aiTriPoly[t]])
						continue;
					au4Scratch.push_back(me.au4Indices[t * 3]);
					au4Scratch.push_back(me.au4Indices[t * 3 + 1]);
					au4Scratch.push_back(me.au4Indices[t * 3 + 2]);
					s.iCount += 3;
				}
				asub_local.push_back(s);
			}
			pau4 = &au4Scratch;
			pasub = &asub_local;
		}
		if (pau4->empty())
			return;

		int i_mesh = VRR_MeshDynamic(avScratch.data(), i_verts, pau4->data(), (int)pau4->size());
		if (!i_mesh)
			return;

		// Characters are drawn from both sides: animated skins have polygons whose stored facing
		// is wrong (parts of dinosaurs and fingers vanished with one-sided drawing).
		uint32_t u4_flags = evrrTWO_SIDED;

		SMeshEntry me_draw;
		me_draw.asub = *pasub;
		AddSubsets(me_draw, i_mesh, af_world, u4_flags, false);
		iSkinned++;
	}

	//******************************************************************************************
	//
	// World walk.
	//
	struct SWalk
	{
		CVector3<> v3Eye;
		CVector3<> v3Fwd;
		CVector3<> v3ShadowCentre;
		float      fFar;
		float      fShadow;
		std::vector<CInstance*>* papinsTerrainObjs;		// Collect terrain objects (level start).
	};

	// Counters for the log: why instances were not drawn.
	struct SWalkStats
	{
		int iSeen, iHidden, iNoShape, iNotMesh, iEmpty, iFar, iBehind;
	};
	SWalkStats wsLast = {};

	//******************************************************************************************
	// Particles (blood, dust, splashes): streaks facing you, one dynamic mesh per colour.
	struct SParticleBuild
	{
		CVector3<> v3Eye;
		std::unordered_map<const CTexture*, std::vector<SVRRVertex>> mapQuads;
	};

	static void AddParticle(void* pv, const CVector3<>& v3_a, const CVector3<>& v3_b, float f_size, const CTexture* ptex)
	{
		SParticleBuild& pb = *(SParticleBuild*)pv;
		CVector3<> v3_mid = (v3_a + v3_b) * 0.5f;
		CVector3<> v3_view = pb.v3Eye - v3_mid;
		float f_view = v3_view.tLen();
		if (f_view < 0.05f || f_view > 300.0f)
			return;
		// The game keeps particles between 1 and 5 pixels of its 640-wide view: specks of
		// blood and dust, not lumps.  The same angular sizes here.
		const float fPIXEL = 2.0f / 640.0f;
		f_size = std::max(fPIXEL * 0.7f * f_view, std::min(f_size, fPIXEL * 4.0f * f_view));
		float f_half = f_size * 0.5f;
		v3_view /= f_view;
		CVector3<> v3_d = v3_b - v3_a;
		float f_len = v3_d.tLen();
		if (f_len < 1e-4f)
		{
			// Not moving: a square facing you.
			v3_d = CVector3<>(0, 0, 1) ^ v3_view;
			if (v3_d.tLen() < 1e-3f)
				v3_d = CVector3<>(1, 0, 0);
			v3_d.Normalise();
		}
		else
			v3_d /= f_len;
		CVector3<> v3_side = v3_d ^ v3_view;
		if (v3_side.tLen() < 1e-4f)
			return;
		v3_side.Normalise();
		v3_side *= f_half;
		CVector3<> v3_p0 = v3_a - v3_d * f_half, v3_p1 = v3_b + v3_d * f_half;
		CVector3<> av3[4] = { v3_p0 - v3_side, v3_p0 + v3_side, v3_p1 + v3_side, v3_p1 - v3_side };
		static const float af_uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		std::vector<SVRRVertex>& av = pb.mapQuads[ptex];
		for (int i = 0; i < 4; i++)
		{
			SVRRVertex v;
			v.afPos[0] = av3[i].tX; v.afPos[1] = av3[i].tY; v.afPos[2] = av3[i].tZ;
			v.afNormal[0] = v3_view.tX; v.afNormal[1] = v3_view.tY; v.afNormal[2] = v3_view.tZ;
			v.afUV[0] = af_uv[i][0] * 0.5f + 0.25f; v.afUV[1] = af_uv[i][1] * 0.5f + 0.25f;
			av.push_back(v);
		}
	}

	//******************************************************************************************
	// Glints: guns and the things the story sends you for (key cards and the like: whatever a
	// pickup trigger waits for) sparkle now and then until you first pick them up.
	struct SGlint
	{
		CVector3<> v3Pos;
		float      fPhase;		// 0..1, so they don't all flash at once.
	};
	std::vector<SGlint>          aglGlints;
	std::unordered_set<uint32>   setGlintHandles;	// Pickup-trigger objects.
	std::unordered_set<const void*> setPickedUp;		// Picked up once: no more glint.
	DWORD u4GlintHandlesTick = 0;
	int   iGlintTex = 0;
	bool  gbGlints = true;

	void UpdateGlintHandles(bool b_new_level)
	{
		if (b_new_level)
			setPickedUp.clear();
		for (int h = 0; h < 2; h++)
			if (gVR.apvHeldFirm[h])
				setPickedUp.insert(gVR.apvHeldFirm[h]);
		DWORD u4_now = GetTickCount();
		if (!b_new_level && u4_now - u4GlintHandlesTick < 2000)
			return;
		u4GlintHandlesTick = u4_now;
		std::vector<uint32> au4;
		CObjectTrigger::VRGetPickupObjects(au4);
		setGlintHandles.clear();
		setGlintHandles.insert(au4.begin(), au4.end());
	}

	void ConsiderGlint(CInstance* pins, const CPresence3<>& pr3, float f_radius, const CVector3<>& v3_eye)
	{
		if (!gbGlints || setPickedUp.count(pins))
			return;
		CVector3<> v3_to = pr3.v3Pos - v3_eye;
		float f_dist = v3_to.tLen();
		if (f_dist > 30.0f || f_dist < 0.3f)
			return;
		if (!ptCast<CGun>(pins) && !setGlintHandles.count(pins->u4GetUniqueHandle()))
			return;
		if (pins == (CInstance*)gVR.apvHeldFirm[0] || pins == (CInstance*)gVR.apvHeldFirm[1])
			return;
		// Just in front of the object (toward you), so it isn't hidden inside it.
		float f_out = std::min(f_radius, 0.3f);
		SGlint gl;
		gl.v3Pos = pr3.v3Pos - v3_to * (f_out / f_dist);
		gl.fPhase = (float)(pins->u4GetUniqueHandle() % 997) / 997.0f;
		aglGlints.push_back(gl);
	}

	void DrawGlints(const CVector3<>& v3_eye)
	{
		if (aglGlints.empty())
			return;
		if (!iGlintTex)
		{
			// A four-pointed star with a soft core.
			const int iN = 64;
			std::vector<uint32_t> au4((size_t)iN * iN);
			for (int y = 0; y < iN; y++)
				for (int x = 0; x < iN; x++)
				{
					float fx = (x + 0.5f) / iN * 2 - 1, fy = (y + 0.5f) / iN * 2 - 1;
					float r = std::sqrt(fx * fx + fy * fy);
					float core = std::exp(-r * r * 18.0f);
					float rays = std::exp(-std::fabs(fx) * 40.0f) * std::max(0.0f, 1 - std::fabs(fy)) +
					             std::exp(-std::fabs(fy) * 40.0f) * std::max(0.0f, 1 - std::fabs(fx));
					float a = std::min(1.0f, core + rays * 0.9f + std::exp(-r * r * 4.0f) * 0.25f);
					au4[(size_t)y * iN + x] = u4RGBA(255, 250, 225, (int)(a * 255));
				}
			iGlintTex = VRR_TextureCreate(iN, iN, au4.data());
		}
		const float fCYCLE = 2.6f, fFLASH = 0.55f;
		float f_t = (float)(GetTickCount() % 2600000) / 1000.0f;
		std::vector<SVRRVertex> av;
		for (const SGlint& gl : aglGlints)
		{
			float u = std::fmod(f_t + gl.fPhase * fCYCLE, fCYCLE);
			if (u > fFLASH)
				continue;
			float f_a = std::sin(3.14159f * u / fFLASH);
			CVector3<> v3_view = v3_eye - gl.v3Pos;
			float f_dist = v3_view.tLen();
			if (f_dist < 1e-3f)
				continue;
			v3_view /= f_dist;
			CVector3<> v3_right = CVector3<>(0, 0, 1) ^ v3_view;
			if (v3_right.tLen() < 1e-3f)
				v3_right = CVector3<>(1, 0, 0);
			v3_right.Normalise();
			CVector3<> v3_up = v3_view ^ v3_right;
			// Turns a little as it flashes.
			float f_ang = u / fFLASH * 0.6f;
			CVector3<> v3_r = v3_right * std::cos(f_ang) + v3_up * std::sin(f_ang);
			CVector3<> v3_u = v3_up * std::cos(f_ang) - v3_right * std::sin(f_ang);
			float f_size = std::max(0.09f, f_dist * 0.011f) * (0.25f + 0.75f * f_a);
			const CVector3<> av3[4] =
			{
				gl.v3Pos - v3_r * f_size - v3_u * f_size, gl.v3Pos + v3_r * f_size - v3_u * f_size,
				gl.v3Pos + v3_r * f_size + v3_u * f_size, gl.v3Pos - v3_r * f_size + v3_u * f_size,
			};
			const float af_uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
			for (int i = 0; i < 4; i++)
			{
				SVRRVertex v;
				v.afPos[0] = av3[i].tX; v.afPos[1] = av3[i].tY; v.afPos[2] = av3[i].tZ;
				v.afNormal[0] = v3_view.tX; v.afNormal[1] = v3_view.tY; v.afNormal[2] = v3_view.tZ;
				v.afUV[0] = af_uv[i][0]; v.afUV[1] = af_uv[i][1];
				av.push_back(v);
			}
		}
		aglGlints.clear();
		int i_quads = (int)av.size() / 4;
		if (i_quads <= 0)
			return;
		std::vector<uint32_t> au4((size_t)i_quads * 6);
		for (int q = 0; q < i_quads; q++)
		{
			uint32_t b = q * 4;
			uint32_t* pu = &au4[(size_t)q * 6];
			pu[0] = b; pu[1] = b + 1; pu[2] = b + 2; pu[3] = b; pu[4] = b + 2; pu[5] = b + 3;
		}
		int i_mesh = VRR_MeshDynamic(av.data(), (int)av.size(), au4.data(), (int)au4.size());
		if (!i_mesh)
			return;
		static const float af_identity[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
		SVRRDraw drw;
		drw.iMesh = i_mesh;
		drw.iFirstIndex = 0;
		drw.iNumIndices = (int)au4.size();
		drw.iTexture = iGlintTex;
		std::memcpy(drw.afWorld, af_identity, sizeof(drw.afWorld));
		drw.u4Flags = evrrBLEND | evrrUNLIT | evrrNO_FOG | evrrNO_SHADOW | evrrTWO_SIDED;
		VRR_AddDraw(drw);
	}

	void DrawParticles(const CVector3<>& v3_eye)
	{
		SParticleBuild pb;
		pb.v3Eye = v3_eye;
		Particles.ForEachVR(&AddParticle, &pb);
		static const float af_identity[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
		for (auto& pr : pb.mapQuads)
		{
			std::vector<SVRRVertex>& av = pr.second;
			int i_quads = (int)av.size() / 4;
			if (i_quads <= 0)
				continue;
			std::vector<uint32_t> au4((size_t)i_quads * 6);
			for (int q = 0; q < i_quads; q++)
			{
				uint32_t b = q * 4;
				uint32_t* pu = &au4[(size_t)q * 6];
				pu[0] = b; pu[1] = b + 1; pu[2] = b + 2; pu[3] = b; pu[4] = b + 2; pu[5] = b + 3;
			}
			int i_mesh = VRR_MeshDynamic(av.data(), (int)av.size(), au4.data(), (int)au4.size());
			if (!i_mesh)
				continue;
			bool b_alpha = false;
			SVRRDraw drw;
			drw.iMesh = i_mesh;
			drw.iFirstIndex = 0;
			drw.iNumIndices = (int)au4.size();
			drw.iTexture = iTexture(pr.first, b_alpha);
			std::memcpy(drw.afWorld, af_identity, sizeof(drw.afWorld));
			// The game draws particles flat, in their colour, unlit.
			drw.u4Flags = evrrTWO_SIDED | evrrNO_SHADOW | evrrUNLIT;
			if (pr.first && pr.first->seterfFeatures[erfALPHA_COLOUR])
				drw.u4Flags |= evrrBLEND | evrrUNLIT;
			VRR_AddDraw(drw);
		}
	}

	//******************************************************************************************
	// The game's per-frame "about to be drawn" step for an instance.  Water uses it to run its
	// waves (which also float things), and dinosaurs to wake their AI when seen.  Only the
	// camera in the context is used by these.
	void PreRenderInstance(CInstance* pins)
	{
		CCamera* pcam = CWDbQueryActiveCamera().tGet();
		if (!pcam)
			return;
		alignas(16) static char ac_unused[64];
		CRenderContext renc(*reinterpret_cast<const CRenderer*>(ac_unused), *reinterpret_cast<CPipelineHeap*>(ac_unused),
		                    *pcam, *reinterpret_cast<CLightList*>(ac_unused));
		pins->PreRender(renc);
	}

	std::unordered_map<const void*, int> mapWaterTex;	// Water instance -> its plain colour (fallback).

	//******************************************************************************************
	//
	// Water, as the game draws it: its wave simulation paints each water surface's texture
	// every frame, with pixels that are entries of the water colour table (colour + see-
	// through amount).  Those textures are decoded and sent to the card each frame.
	//
	std::vector<uint32_t> au4WaterLUT;
	uint32_t u4WaterLUTSum = 0;

	void UpdateWaterLUT()
	{
		const int i_n = 1 << iRAMP_SHIFT_BITS;
		uint32_t u4_sum = 0;
		for (int i = 0; i < i_n; i++)
			u4_sum = u4_sum * 31 + au2IntensityToAlpha[i];
		if (!au4WaterLUT.empty() && u4_sum == u4WaterLUTSum)
			return;
		u4WaterLUTSum = u4_sum;
		au4WaterLUT.assign(65536, 0);
		for (int i = 0; i < i_n; i++)
		{
			int j = (i * 16) / i_n;
			const CLightBlend::SLightBlendSettings& lbs = lbAlphaWater.lpsSettings[j];
			int a = (int)(std::max(0.0f, std::min(1.0f, lbs.fAlpha)) * 255.0f + 0.5f);
			au4WaterLUT[au2IntensityToAlpha[i]] = u4RGBA(lbs.clrBlendColour.u1Red, lbs.clrBlendColour.u1Green, lbs.clrBlendColour.u1Blue, a);
		}
		// Anything else (shouldn't happen): the middle colour.
		const CLightBlend::SLightBlendSettings& lbs_mid = lbAlphaWater.lpsSettings[8];
		uint32_t u4_mid = u4RGBA(lbs_mid.clrBlendColour.u1Red, lbs_mid.clrBlendColour.u1Green, lbs_mid.clrBlendColour.u1Blue, 160);
		std::vector<bool> ab_set(65536, false);
		for (int i = 0; i < i_n; i++)
			ab_set[au2IntensityToAlpha[i]] = true;
		for (int i = 0; i < 65536; i++)
			if (!ab_set[i])
				au4WaterLUT[i] = u4_mid;
	}

	struct SWaterTex { int iHandle; int iW, iH; };
	std::unordered_map<const void*, SWaterTex> mapWaterRaster;
	std::vector<uint32_t> au4WaterScratch;

	// The texture for one of the water's rasters, refreshed now; 0 if it can't be read.
	int iWaterTexture(const CTexture* ptex)
	{
		if (!ptex || ptex->iGetNumMipLevels() <= 0)
			return 0;
		rptr<CRaster> pras = ptex->prasGetTexture(0);
		const CRaster* praw = pras.ptGet();
		if (!praw || !praw->pSurface || praw->iPixelBits != 16 || praw->iWidth <= 0 || praw->iHeight <= 0)
			return 0;
		int i_w = praw->iWidth, i_h = praw->iHeight;
		au4WaterScratch.resize((size_t)i_w * i_h);
		for (int y = 0; y < i_h; y++)
		{
			const uint16* pu2 = (const uint16*)praw->pSurface + (size_t)y * praw->iLinePixels;
			uint32_t* pu4 = &au4WaterScratch[(size_t)y * i_w];
			for (int x = 0; x < i_w; x++)
				pu4[x] = au4WaterLUT[pu2[x]];
		}
		auto it = mapWaterRaster.find(praw);
		if (it != mapWaterRaster.end() && it->second.iW == i_w && it->second.iH == i_h &&
		    VRR_TextureUpdate(it->second.iHandle, i_w, i_h, au4WaterScratch.data()))
			return it->second.iHandle;
		int i_handle = VRR_TextureCreate(i_w, i_h, au4WaterScratch.data());
		mapWaterRaster[praw] = { i_handle, i_w, i_h };
		return i_handle;
	}

	// Draws the water's current surface (its mesh and textures change as the simulation runs).
	bool bDrawWater(const CMesh* pmsh, const float af_world[12])
	{
		UpdateWaterLUT();
		std::unordered_map<const CTexture*, std::vector<SVRRVertex>> map_tris;
		for (uint u = 0; u < pmsh->pampPolygons.uLen; u++)
		{
			const CMesh::SPolygon& mp = pmsh->pampPolygons[u];
			if (mp.papmvVertices.uLen < 3 || !mp.pSurface)
				continue;
			const CTexture* ptex = mp.pSurface->ptexTexture.ptGet();
			std::vector<SVRRVertex>& av = map_tris[ptex];
			for (uint k = 1; k + 1 < mp.papmvVertices.uLen; k++)
			{
				const CMesh::SVertex* apmv[3] = { mp.papmvVertices[0], mp.papmvVertices[k], mp.papmvVertices[k + 1] };
				for (int v = 0; v < 3; v++)
				{
					SVRRVertex vx;
					const CVector3<>& p = *apmv[v]->pv3Point;
					vx.afPos[0] = p.tX; vx.afPos[1] = p.tY; vx.afPos[2] = p.tZ;
					vx.afNormal[0] = 0; vx.afNormal[1] = 0; vx.afNormal[2] = 1;
					vx.afUV[0] = apmv[v]->tcTex.tX; vx.afUV[1] = apmv[v]->tcTex.tY;
					av.push_back(vx);
				}
			}
		}
		bool b_any = false;
		for (auto& pr : map_tris)
		{
			int i_tex = iWaterTexture(pr.first);
			if (!i_tex)
				continue;
			std::vector<SVRRVertex>& av = pr.second;
			std::vector<uint32_t> au4(av.size());
			for (size_t i = 0; i < au4.size(); i++)
				au4[i] = (uint32_t)i;
			int i_mesh = VRR_MeshDynamic(av.data(), (int)av.size(), au4.data(), (int)au4.size());
			if (!i_mesh)
				continue;
			SVRRDraw drw;
			drw.iMesh = i_mesh;
			drw.iFirstIndex = 0;
			drw.iNumIndices = (int)au4.size();
			drw.iTexture = i_tex;
			std::memcpy(drw.afWorld, af_world, sizeof(drw.afWorld));
			drw.u4Flags = evrrBLEND | evrrUNLIT | evrrTWO_SIDED | evrrNO_SHADOW;
			VRR_AddDraw(drw);
			b_any = true;
		}
		return b_any;
	}

	//******************************************************************************************
	// Something held in a hand (and anything magneted to it, like a gun's parts) is drawn where
	// the controller is now, not where the last physics step left it.
	bool bHeldPresence(CInstance* pins, CPresence3<>& rpr3)
	{
		for (int h = 0; h < 2; h++)
		{
			CInstance* pins_held = (CInstance*)gVR.apvHeldFirm[h];
			// (Only guns: other things trail the hand with their weight, drawn where they are.)
			if (!pins_held || !gVR.abHandValid[h] || !gVR.abHoldingGun[h])
				continue;
			if (pins != pins_held)
			{
				const CPhysicsInfo* pphi = pins->pphiGetPhysicsInfo();
				if (!pphi || !pphi->bIsMovable())
					continue;
				if ((pins->v3Pos() - pins_held->v3Pos()).tLenSqr() > 4.0f)
					continue;
				if (NMagnetSystem::pinsFindMaster(pins) != pins_held)
					continue;
			}
			const float* af_q = gVR.aafHandRot[h];
			const float* af_p = gVR.aafHandPos[h];
			const float* af_g = gVR.afHeldGrab[h];
			CPlacement3<> p3_ctrl(CRotate3<>(af_q[0], af_q[1], af_q[2], af_q[3], true), CVector3<>(af_p[0], af_p[1], af_p[2]));
			CPlacement3<> p3_grab(CRotate3<>(af_g[3], af_g[4], af_g[5], af_g[6], true), CVector3<>(af_g[0], af_g[1], af_g[2]));
			CPlacement3<> p3_render = p3_grab * p3_ctrl;
			CPlacement3<> p3_inst = pins->p3GetPlacement() * ~pins_held->p3GetPlacement() * p3_render;
			// Guns are drawn a little smaller, shrunk about the hand, to suit her hands.
			float f_k = gVR.abHoldingGun[h] ? std::max(0.5f, std::min(gVR.fGunScale, 1.5f)) : 1.0f;
			CVector3<> v3_pos = p3_ctrl.v3Pos + (p3_inst.v3Pos - p3_ctrl.v3Pos) * f_k;
			rpr3 = CPresence3<>(p3_inst.r3Rot, rpr3.rScale * f_k, v3_pos);
			return true;
		}
		return false;
	}

	void DrawInstance(CInstance* pins, SWalk& wk, SWalkStats& ws)
	{
		ws.iSeen++;
		if (!pins->bIsVisible())
		{
			ws.iHidden++;
			return;
		}

		rptr_const<CRenderType> prdt = pins->prdtGetRenderInfo();
		if (!prdt)
		{
			ws.iNoShape++;
			return;
		}
		rptr_const<CMesh> pmsh = ptCastRenderType<CMesh>(prdt);
		if (!pmsh)
		{
			ws.iNotMesh++;
			return;
		}
		rptr_const<CBioMesh> pbm = prdt->rpbmCast();

		const CPresence3<>& pr3 = pins->pr3Presence();
		SMeshEntry* pme = pmeGet(pmsh.ptGet(), pbm.ptGet() != nullptr);
		if (!pme || pme->asub.empty())
		{
			ws.iEmpty++;
			return;
		}
		ConsiderGlint(pins, pr3, pme->fRadius * std::fabs(pr3.rScale), wk.v3Eye);

		// Out of sight: too far, or behind you (unless it can cast a shadow you see).
		float f_r = pme->fRadius * std::fabs(pr3.rScale) + 1.0f;
		CVector3<> v3_to = pr3.v3Pos - wk.v3Eye;
		float f_dist = v3_to.tLen();
		if (f_dist - f_r > wk.fFar)
		{
			ws.iFar++;
			return;
		}
		if (f_dist > f_r + 2.0f && (v3_to * wk.v3Fwd) < -0.6f * f_dist)
		{
			if (wk.fShadow <= 0 || (pr3.v3Pos - wk.v3ShadowCentre).tLen() > wk.fShadow + f_r)
			{
				ws.iBehind++;
				return;
			}
		}

		float af_world[12];
		CPresence3<> pr3_draw = pr3;
		if (!pbm.ptGet() && (gVR.apvHeldFirm[0] || gVR.apvHeldFirm[1]))
			bHeldPresence(pins, pr3_draw);
		WorldRows(pr3_draw, af_world);
		if (pbm.ptGet() && ptCast<CAnimal>(pins))
			PreRenderInstance(pins);
		if (pbm.ptGet())
			DrawSkinned(pins, pbm.ptGet(), *pme, af_world);
		else if (pme->iHandle)
		{
			uint32_t u4_flags = 0;
			if (pr3.rScale < 0)
				u4_flags |= evrrTWO_SIDED;
			if (ptCast<CEntityWater>(pins))
			{
				// Water: the game's own wave simulation runs (it floats things); drawn see-through
				// in its original colour, with a reflection of the sky at low angles.
				auto it = mapWaterTex.find(pins);
				if (it == mapWaterTex.end())
				{
					bool b_alpha = false;
					const CTexture* ptex = pmsh->pasfSurfaces.uLen ? pmsh->pasfSurfaces[0].ptexTexture.ptGet() : nullptr;
					it = mapWaterTex.emplace(pins, iTexture(ptex, b_alpha)).first;
				}
				PreRenderInstance(pins);
				if (!bDrawWater(pmsh.ptGet(), af_world))
					AddSubsets(*pme, pme->iHandle, af_world, 0, false, evrrBLEND | evrrWATER | evrrTWO_SIDED | evrrNO_SHADOW, it->second);
				iObjects++;
				return;
			}
			float af_uv[2] = { 0, 0 };
			if (pmsh->bIsAnimated())
			{
				// The game animates these as it draws them: swap texture frames, scroll.
				const CMeshAnimating* pma = static_cast<const CMeshAnimating*>(pmsh.ptGet());
				if (setAnimatedThisFrame.insert(pmsh.ptGet()).second)
					pma->Animate();
				if (pma->bScrolls() && pmsh->pamvVertices.uLen > 0)
				{
					af_uv[0] = pmsh->pamvVertices[0].tcTex.tX - pme->afUV0[0];
					af_uv[1] = pmsh->pamvVertices[0].tcTex.tY - pme->afUV0[1];
				}
			}
			AddSubsets(*pme, pme->iHandle, af_world, u4_flags, false, 0, -1, af_uv);
			iObjects++;
		}
	}

	std::unordered_set<const void*> setBadInstances;

	int iDrawInstanceSafe(CInstance* pins, SWalk* pwk, SWalkStats* pws)
	{
		__try
		{
			DrawInstance(pins, *pwk, *pws);
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 1;
		}
	}

	// Every instance in the world's partition tree.  An instance can itself hold other
	// partitions, so children are always visited, whether or not the parent is drawn.
	void Visit(CPartition* ppart, SWalk& wk, SWalkStats& ws)
	{
		CInstance* pins = ptCast<CInstance>(ppart);
		if (pins)
		{
			if (ptCast<CTerrainObj>(pins))
			{
				if (wk.papinsTerrainObjs)
					wk.papinsTerrainObjs->push_back(pins);
			}
			else if (!setBadInstances.count(pins))
			{
				if (iDrawInstanceSafe(pins, &wk, &ws) != 0)
				{
					// Something about this instance's data upset the renderer: leave it out from
					// now on rather than lose the whole frame.
					setBadInstances.insert(pins);
					SceneLog("GPU renderer: skipping an instance that caused an error (%.0f so far).", (double)setBadInstances.size());
				}
			}
		}

		CPartition* ppartc = ppart->ppartChildren();
		if (ppartc)
		{
			for (CPartition::iterator it = ppartc->begin(); it != ppartc->end(); ++it)
				Visit(*it, wk, ws);
		}
	}

	//******************************************************************************************
	// The texture mapping of a terrain texture object, as u, v from world x, y (least squares
	// over its vertices).  False if the mapping isn't a flat projection from above.
	bool bFitDecalMapping(const CMesh* pmsh, const CMesh::SSurface* psf, const float af_world[12], float af_map[6])
	{
		double ad_ata[3][3] = {}, ad_atu[3] = {}, ad_atv[3] = {};
		std::vector<std::array<double, 4>> apt;
		for (uint u = 0; u < pmsh->pampPolygons.uLen; u++)
		{
			const CMesh::SPolygon& mp = pmsh->pampPolygons[u];
			if ((const CMesh::SSurface*)mp.pSurface != psf || mp.bOcclude || mp.bCache)
				continue;
			for (uint k = 0; k < mp.papmvVertices.uLen; k++)
			{
				const CMesh::SVertex* pmv = mp.papmvVertices[k];
				const CVector3<>& p = *pmv->pv3Point;
				double x = af_world[0] * p.tX + af_world[1] * p.tY + af_world[2] * p.tZ + af_world[3];
				double y = af_world[4] * p.tX + af_world[5] * p.tY + af_world[6] * p.tZ + af_world[7];
				apt.push_back({ x, y, (double)pmv->tcTex.tX, (double)pmv->tcTex.tY });
			}
		}
		if (apt.size() < 3)
			return false;
		for (const auto& a : apt)
		{
			double r[3] = { a[0], a[1], 1.0 };
			for (int i = 0; i < 3; i++)
			{
				for (int j = 0; j < 3; j++)
					ad_ata[i][j] += r[i] * r[j];
				ad_atu[i] += r[i] * a[2];
				ad_atv[i] += r[i] * a[3];
			}
		}
		// Solve the 3x3 systems (Cramer).
		auto det3 = [](const double m[3][3])
		{
			return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		};
		double d = det3(ad_ata);
		if (std::fabs(d) < 1e-9)
			return false;
		double ad_u[3], ad_v[3];
		for (int c = 0; c < 3; c++)
		{
			double m[3][3];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					m[i][j] = ad_ata[i][j];
			for (int i = 0; i < 3; i++)
				m[i][c] = ad_atu[i];
			ad_u[c] = det3(m) / d;
			for (int i = 0; i < 3; i++)
				m[i][c] = ad_atv[i];
			ad_v[c] = det3(m) / d;
		}
		// Check the fit: within a twentieth of a texture repeat everywhere.
		for (const auto& a : apt)
		{
			double eu = ad_u[0] * a[0] + ad_u[1] * a[1] + ad_u[2] - a[2];
			double ev = ad_v[0] * a[0] + ad_v[1] * a[1] + ad_v[2] - a[3];
			if (std::fabs(eu) > 0.05 || std::fabs(ev) > 0.05)
				return false;
		}
		// Keep the offsets small so the shader keeps its precision.
		af_map[0] = (float)ad_u[0]; af_map[1] = (float)ad_u[1];
		af_map[2] = (float)(ad_u[2] - std::floor(ad_u[2]));
		af_map[3] = (float)ad_v[0]; af_map[4] = (float)ad_v[1];
		af_map[5] = (float)(ad_v[2] - std::floor(ad_v[2]));
		return true;
	}

	//******************************************************************************************
	//
	// Fine terrain near the player.  The level-wide grid is a few metres between points, which
	// cuts the tops off bumps and leaves things standing on the real ground looking like they
	// float.  Around the player the ground is sampled every half metre, in patches built a
	// little each frame as you move.
	//
	const float fPATCH_SIZE = 32.0f;
	const int   iPATCH_N = 65;				// Vertices along a side (0.5 m apart).
	const int   iPATCH_RADIUS = 4;			// Patches either side of yours that are kept ready (about 140 m).
	const int   iPATCH_SLOTS = 128;

	struct SPatch
	{
		int  iX, iY;
		bool bUsed;
	};
	SPatch apatch[iPATCH_SLOTS];
	float  fPatchX0 = 0, fPatchY0 = 0;

	// The patch being built.
	struct SPatchJob
	{
		bool  bActive = false;
		int   iX = 0, iY = 0, iSlot = -1;
		int   iNext = 0;					// Next sample.
		std::vector<float> afH;				// (N + 2)^2 heights including a border.
	};
	SPatchJob pjob;

	void ResetPatches()
	{
		for (int i = 0; i < iPATCH_SLOTS; i++)
		{
			apatch[i].bUsed = false;
			VRR_TerrainPatchClear(i);
		}
		pjob.bActive = false;
	}

	void ReleaseQueries(CTerrain* ptrr);

	void UpdatePatches(CTerrain* ptrr, const CVector3<>& v3_at, double d_budget_ms)
	{
		if (!VRR_HasTerrain())
			return;
		int i_px = (int)std::floor((v3_at.tX - fPatchX0) / fPATCH_SIZE);
		int i_py = (int)std::floor((v3_at.tY - fPatchY0) / fPATCH_SIZE);

		// Drop patches that are well out of range.
		for (int i = 0; i < iPATCH_SLOTS; i++)
		{
			if (apatch[i].bUsed && (std::abs(apatch[i].iX - i_px) > iPATCH_RADIUS + 1 || std::abs(apatch[i].iY - i_py) > iPATCH_RADIUS + 1))
			{
				apatch[i].bUsed = false;
				VRR_TerrainPatchClear(i);
				if (pjob.bActive && pjob.iSlot == i)
					pjob.bActive = false;
			}
		}

		LARGE_INTEGER li_f, li_0, li_now;
		QueryPerformanceFrequency(&li_f);
		QueryPerformanceCounter(&li_0);
		auto d_elapsed_ms = [&]()
		{
			QueryPerformanceCounter(&li_now);
			return double(li_now.QuadPart - li_0.QuadPart) * 1000.0 / double(li_f.QuadPart);
		};

		for (;;)
		{
			// Pick the nearest missing patch.
			if (!pjob.bActive)
			{
				int i_best_d = 1 << 30, i_bx = 0, i_by = 0;
				for (int dy = -iPATCH_RADIUS; dy <= iPATCH_RADIUS; dy++)
					for (int dx = -iPATCH_RADIUS; dx <= iPATCH_RADIUS; dx++)
					{
						int i_x = i_px + dx, i_y = i_py + dy;
						bool b_have = false;
						for (int i = 0; i < iPATCH_SLOTS; i++)
							if (apatch[i].bUsed && apatch[i].iX == i_x && apatch[i].iY == i_y)
								b_have = true;
						int i_d = dx * dx + dy * dy;
						if (!b_have && i_d < i_best_d)
						{
							i_best_d = i_d;
							i_bx = i_x;
							i_by = i_y;
						}
					}
				if (i_best_d == (1 << 30))
					break;
				int i_slot = -1;
				for (int i = 0; i < iPATCH_SLOTS && i_slot < 0; i++)
					if (!apatch[i].bUsed)
						i_slot = i;
				if (i_slot < 0)
					break;
				pjob.bActive = true;
				pjob.iX = i_bx;
				pjob.iY = i_by;
				pjob.iSlot = i_slot;
				pjob.iNext = 0;
				pjob.afH.assign((size_t)(iPATCH_N + 2) * (iPATCH_N + 2), 0.0f);
			}

			// Sample.
			const int i_side = iPATCH_N + 2;
			const int i_total = i_side * i_side;
			const float f_step = fPATCH_SIZE / (iPATCH_N - 1);
			float f_ox = fPatchX0 + pjob.iX * fPATCH_SIZE, f_oy = fPatchY0 + pjob.iY * fPATCH_SIZE;
			int i_since = 0;
			while (pjob.iNext < i_total)
			{
				int i_x = pjob.iNext % i_side - 1, i_y = pjob.iNext / i_side - 1;
				pjob.afH[pjob.iNext] = (float)ptrr->rHeight(f_ox + i_x * f_step, f_oy + i_y * f_step);
				pjob.iNext++;
				if (++i_since >= 400)
				{
					ReleaseQueries(ptrr);
					i_since = 0;
					if (d_elapsed_ms() > d_budget_ms)
						break;
				}
			}
			ReleaseQueries(ptrr);
			if (pjob.iNext < i_total)
				break;

			// Finished: vertices with normals and base colours.
			std::vector<SVRRTerrainVertex> atv((size_t)iPATCH_N * iPATCH_N);
			for (int y = 0; y < iPATCH_N; y++)
				for (int x = 0; x < iPATCH_N; x++)
				{
					auto h = [&](int i_x, int i_y) { return pjob.afH[(size_t)(i_y + 1) * i_side + (i_x + 1)]; };
					float nx = h(x - 1, y) - h(x + 1, y), ny = h(x, y - 1) - h(x, y + 1), nz = 2.0f * f_step;
					float f_len = std::sqrt(nx * nx + ny * ny + nz * nz);
					SVRRTerrainVertex& tv = atv[(size_t)y * iPATCH_N + x];
					float f_x = f_ox + x * f_step, f_y = f_oy + y * f_step;
					tv.afPos[0] = f_x; tv.afPos[1] = f_y; tv.afPos[2] = h(x, y);
					tv.afNormal[0] = nx / f_len; tv.afNormal[1] = ny / f_len; tv.afNormal[2] = nz / f_len;
					CColour clr = ptrr->clrGetBaseTextureColour(f_x, f_y);
					tv.u4Colour = u4RGBA(clr.u1Red, clr.u1Green, clr.u1Blue);
				}
			if (VRR_TerrainPatchSet(pjob.iSlot, iPATCH_N, atv.data()))
			{
				apatch[pjob.iSlot].bUsed = true;
				apatch[pjob.iSlot].iX = pjob.iX;
				apatch[pjob.iSlot].iY = pjob.iY;
			}
			pjob.bActive = false;
			if (d_elapsed_ms() > d_budget_ms)
				break;
		}
	}

	//******************************************************************************************
	// Frees the parts of the game's terrain height tree that no query uses.
	void ReleaseQueries(CTerrain* ptrr)
	{
		NMultiResolution::CQuadRootQuery* pqnq = ptrr->pqnqGetQueryRoot();
		if (pqnq)
		{
			pqnq->Update();
			pqnq->Update();
		}
	}

	//******************************************************************************************
	//
	// Terrain: height grid and texture atlas, once per level.
	//
	void BuildTerrain(CTerrain* ptrr, std::vector<CInstance*>& apins_trr, float f_detail)
	{
		DWORD u4_start = GetTickCount();

		// Area: everything covered by the terrain textures.
		float f_x0 = 1e9f, f_y0 = 1e9f, f_x1 = -1e9f, f_y1 = -1e9f;
		for (CInstance* pins : apins_trr)
		{
			rptr_const<CMesh> pmsh = ptCastRenderType<CMesh>(pins->prdtGetRenderInfo());
			if (!pmsh)
				continue;
			const CPresence3<>& pr3 = pins->pr3Presence();
			for (uint u = 0; u < pmsh->pav3Points.uLen; u++)
			{
				CVector3<> v3 = pmsh->pav3Points[u] * pr3;
				f_x0 = std::min(f_x0, (float)v3.tX); f_x1 = std::max(f_x1, (float)v3.tX);
				f_y0 = std::min(f_y0, (float)v3.tY); f_y1 = std::max(f_y1, (float)v3.tY);
			}
		}
		if (f_x1 <= f_x0 || f_y1 <= f_y0)
		{
			CVector3<> v3 = gpPlayer ? gpPlayer->v3Pos() : CVector3<>(0, 0, 0);
			f_x0 = v3.tX - 1000; f_x1 = v3.tX + 1000;
			f_y0 = v3.tY - 1000; f_y1 = v3.tY + 1000;
		}

		// Grid: about a million points at normal detail.
		float f_w = f_x1 - f_x0, f_h = f_y1 - f_y0;
		float f_spacing = std::sqrt(f_w * f_h / (1000000.0f * f_detail));
		f_spacing = std::max(1.0f, std::min(f_spacing, 8.0f));
		// The game's height lookups can be slow: time a few and keep the build under ~6 seconds.
		{
			LARGE_INTEGER li_f, li_0, li_1;
			QueryPerformanceFrequency(&li_f);
			QueryPerformanceCounter(&li_0);
			for (int i = 0; i < 400; i++)
				ptrr->rHeight(f_x0 + (i % 20) * f_w / 20.0f, f_y0 + (i / 20) * f_h / 20.0f);
			QueryPerformanceCounter(&li_1);
			ReleaseQueries(ptrr);
			double d_each = double(li_1.QuadPart - li_0.QuadPart) / double(li_f.QuadPart) / 400.0;
			double d_points = (f_w / f_spacing) * (f_h / f_spacing);
			if (d_each * d_points > 6.0)
			{
				f_spacing *= (float)std::sqrt(d_each * d_points / 6.0);
				f_spacing = std::min(f_spacing, 8.0f);
			}
			SceneLog("GPU renderer: terrain height lookup %.1f microseconds.", d_each * 1e6);
		}
		int i_nx = (int)(f_w / f_spacing) + 2, i_ny = (int)(f_h / f_spacing) + 2;

		std::vector<float> af_h((size_t)i_nx * i_ny);
		int i_since_release = 0;
		std::vector<SVRRTerrainVertex> atv((size_t)i_nx * i_ny);
		for (int y = 0; y < i_ny; y++)
		{
			for (int x = 0; x < i_nx; x++)
			{
				float f_x = std::min(f_x0 + x * f_spacing, f_x1), f_y = std::min(f_y0 + y * f_spacing, f_y1);
				af_h[(size_t)y * i_nx + x] = (float)ptrr->rHeight(f_x, f_y);

				// Each lookup builds part of the game's height tree in a fixed-size memory
				// area; let it free what isn't needed any more before that fills up.
				if (++i_since_release >= 500)
				{
					ReleaseQueries(ptrr);
					i_since_release = 0;
				}
			}
		}
		ReleaseQueries(ptrr);
		for (int y = 0; y < i_ny; y++)
		{
			for (int x = 0; x < i_nx; x++)
			{
				size_t i = (size_t)y * i_nx + x;
				float f_x = f_x0 + x * f_spacing, f_y = f_y0 + y * f_spacing;
				float h_l = af_h[(size_t)y * i_nx + std::max(x - 1, 0)];
				float h_r = af_h[(size_t)y * i_nx + std::min(x + 1, i_nx - 1)];
				float h_d = af_h[(size_t)std::max(y - 1, 0) * i_nx + x];
				float h_u = af_h[(size_t)std::min(y + 1, i_ny - 1) * i_nx + x];
				float nx = (h_l - h_r), ny = (h_d - h_u), nz = 2.0f * f_spacing;
				float f_len = std::sqrt(nx * nx + ny * ny + nz * nz);
				SVRRTerrainVertex& tv = atv[i];
				tv.afPos[0] = f_x; tv.afPos[1] = f_y; tv.afPos[2] = af_h[i];
				tv.afNormal[0] = nx / f_len; tv.afNormal[1] = ny / f_len; tv.afNormal[2] = nz / f_len;
				CColour clr = ptrr->clrGetBaseTextureColour(f_x, f_y);
				tv.u4Colour = u4RGBA(clr.u1Red, clr.u1Green, clr.u1Blue);
			}
		}

		// Atlas: about 4 texels a metre at normal detail.
		float f_texels = std::max(f_w, f_h) * 4.0f * f_detail;
		int i_atlas = 1024;
		while (i_atlas < f_texels && i_atlas < 16384)
			i_atlas *= 2;

		fPatchX0 = f_x0;
		fPatchY0 = f_y0;
		if (!VRR_TerrainSet(i_nx, i_ny, f_x0, f_y0, f_spacing, atv.data(), i_atlas))
		{
			SceneLog("GPU renderer: could not create the terrain.");
			return;
		}

		// Terrain textures, lowest first.
		std::stable_sort(apins_trr.begin(), apins_trr.end(), [](CInstance* a, CInstance* b)
		{
			CTerrainObj* pa = ptCast<CTerrainObj>(a);
			CTerrainObj* pb = ptCast<CTerrainObj>(b);
			return pa->iHeight < pb->iHeight;
		});
		int i_decals = 0, i_mapped = 0;
		for (CInstance* pins : apins_trr)
		{
			rptr_const<CMesh> pmsh = ptCastRenderType<CMesh>(pins->prdtGetRenderInfo());
			if (!pmsh)
				continue;
			SMeshEntry* pme = pmeGet(pmsh.ptGet(), false);
			if (!pme || !pme->iHandle)
				continue;
			float af_world[12];
			WorldRows(pins->pr3Presence(), af_world);
			for (const SSubset& sub : pme->asub)
			{
				if (sub.iCount <= 0)
					continue;
				bool b_alpha = false;
				const CTexture* ptex = sub.psf ? sub.psf->ptexTexture.ptGet() : nullptr;
				SVRRDraw drw;
				drw.iMesh = pme->iHandle;
				drw.iFirstIndex = sub.iFirst;
				drw.iNumIndices = sub.iCount;
				drw.iTexture = iTexture(ptex, b_alpha);
				std::memcpy(drw.afWorld, af_world, sizeof(drw.afWorld));
				drw.u4Flags = 0;
				float af_map[6];
				bool b_map = bFitDecalMapping(pmsh.ptGet(), sub.psf, af_world, af_map);
				VRR_TerrainAddDecal(drw, b_map ? af_map : nullptr);
				if (b_map)
					i_mapped++;
				i_decals++;
			}
		}
		VRR_TerrainBuildAtlas();

		SceneLog("GPU renderer: terrain %.0f x %.0f m, grid %.1f m.", f_w, f_h, f_spacing);
		SceneLog("GPU renderer: %.0f terrain textures, %.0f at full resolution.", i_decals, i_mapped);
		SceneLog("GPU renderer: terrain atlas %.0f pixels; built in %.1f s.", i_atlas, (GetTickCount() - u4_start) / 1000.0);
	}
}

//**********************************************************************************************
bool VRScene_Init(int i_width, int i_height, int i_msaa, int i_shadow_size, char* str_err, int i_err_len)
{
	void* pv_dev = nullptr;
	void* pv_ctx = nullptr;
	VRB_GetD3D11(&pv_dev, &pv_ctx);
	bReady = VRR_Init(pv_dev, pv_ctx, i_width, i_height, i_msaa, i_shadow_size, str_err, i_err_len);
	return bReady;
}

//**********************************************************************************************
void VRScene_Shutdown()
{
	mapMesh.clear();
	mapTex.clear();
	mapTexAlpha.clear();
	mapWaterTex.clear();
	mapWaterRaster.clear();
	setBadInstances.clear();
	ptrrLevel = nullptr;
	pvLevelPlayer = nullptr;
	iSkyTexture = 0;
	pvSkyTexture = nullptr;
	VRR_Shutdown();
	bReady = false;
}

//**********************************************************************************************
bool VRScene_Ready()
{
	return bReady && VRR_Ready();
}

//**********************************************************************************************
bool VRScene_RenderAndSubmit(const SVRSceneEyes& eyes, const SVRSceneSettings& set)
{
	if (!VRScene_Ready() || !pwWorld)
		return false;

	CCamera* pcam = CWDbQueryActiveCamera().tGet();
	CTerrain* ptrr = CWDbQueryTerrain().tGet();

	//
	// New level: start again.
	//
	bool b_new_level = ptrr != ptrrLevel || (const void*)gpPlayer != pvLevelPlayer;
	std::vector<CInstance*> apins_trr;
	if (b_new_level)
	{
		mapMesh.clear();
		mapTex.clear();
		mapTexAlpha.clear();
	mapWaterTex.clear();
	mapWaterRaster.clear();
	setBadInstances.clear();
		iSkyTexture = 0;
		pvSkyTexture = nullptr;
		iGlintTex = 0;
		VRR_ClearAll();

		// Load all of the level's textures now (the game normally streams them in a little at
		// a time as its renderer asks for them; this renderer converts each one once).
		CVirtualMem* pvme = gtxmTexMan.pvmeTextures;
		if (pvme && pvme->bFileBased() && !pvme->bAllPagesPresent())
		{
			DWORD u4_t0 = GetTickCount();
			pvme->CommitAll();
			SceneLog("GPU renderer: loaded all textures in %.1f s.", (GetTickCount() - u4_t0) / 1000.0);
		}
		ptrrLevel = ptrr;
		pvLevelPlayer = gpPlayer;
		SceneLog("GPU renderer: new level.");
	}

	SWalk wk;
	wk.v3Eye = CVector3<>((eyes.afPos[0][0] + eyes.afPos[1][0]) * 0.5f, (eyes.afPos[0][1] + eyes.afPos[1][1]) * 0.5f, (eyes.afPos[0][2] + eyes.afPos[1][2]) * 0.5f);
	wk.v3Fwd = CVector3<>(eyes.afAxes[0][3], eyes.afAxes[0][4], eyes.afAxes[0][5]);
	wk.v3ShadowCentre = gpPlayer ? gpPlayer->v3Pos() : wk.v3Eye;
	float f_far = pcam ? (float)pcam->campropGetProperties().rFarClipPlaneDist : 500.0f;
	wk.fFar = std::max(50.0f, f_far * set.fDrawDistance);
	wk.fShadow = set.fShadowDistance;
	wk.papinsTerrainObjs = b_new_level ? &apins_trr : nullptr;

	iObjects = iSkinned = iTexturesWaiting = 0;
	setAnimatedThisFrame.clear();
	UpdateGlintHandles(b_new_level);
	gbGlints = set.bGlints;
	aglGlints.clear();
	SWalkStats ws = {};
	Visit(pwWorld->ppartPartitionList(), wk, ws);
	if (b_new_level && pwWorld->ppartTerrainPartitionList())
		Visit(pwWorld->ppartTerrainPartitionList(), wk, ws);
	wsLast = ws;

	DrawParticles(wk.v3Eye);
	DrawGlints(wk.v3Eye);

	if (b_new_level && ptrr)
	{
		BuildTerrain(ptrr, apins_trr, std::max(0.25f, set.fTerrainDetail));
		ResetPatches();
		if (gpPlayer)
			UpdatePatches(ptrr, gpPlayer->v3Pos(), 1.0e9);		// The ones around you, now.
	}
	else if (ptrr && gpPlayer)
		UpdatePatches(ptrr, gpPlayer->v3Pos(), 2.0);			// Then about 2 ms a frame.

	//
	// Frame parameters.
	//
	SVRRFrame fr = {};
	for (int i = 0; i < 2; i++)
	{
		for (int k = 0; k < 3; k++)
			fr.afEyePos[i][k] = eyes.afPos[i][k];
		for (int k = 0; k < 9; k++)
			fr.afEyeAxes[i][k] = eyes.afAxes[i][k];
		for (int k = 0; k < 4; k++)
			fr.afEyeTan[i][k] = eyes.afTan[i][k];
	}
	fr.fNear = 0.03f;

	// Lights.
	fr.afSunDir[0] = 0.3f; fr.afSunDir[1] = 0.2f; fr.afSunDir[2] = 0.9f;
	fr.fSun = 0.0f;
	fr.fAmbient = 0.0f;
	{
		CWDbQueryLights wql;
		for (std::list<CInstance*>::const_iterator it = wql.begin(); it != wql.end(); ++it)
		{
			rptr<CLight> plt = ptCastRenderType<CLight>((*it)->prdtGetRenderInfo());
			if (!plt)
				continue;
			SLightInfo lti = plt->ltiGetInfo(CVector3<>(0, 0, 0));
			if (plt->bIsDirectional())
			{
				CVector3<> v3 = (CVector3<>)lti.d3Light * (*it)->pr3Presence().r3Rot;
				if (lti.lvStrength > fr.fSun)
				{
					fr.fSun = lti.lvStrength;
					fr.afSunDir[0] = v3.tX; fr.afSunDir[1] = v3.tY; fr.afSunDir[2] = v3.tZ;
				}
			}
			else if (!plt->bIsPositional())
				fr.fAmbient += lti.lvStrength;
		}
		if (fr.fSun <= 0 && fr.fAmbient <= 0)
		{
			fr.fSun = 0.7f;
			fr.fAmbient = 0.35f;
		}
		if (fr.afSunDir[2] < 0)
		{
			// Some levels store the light's travel direction instead.
			fr.afSunDir[0] = -fr.afSunDir[0]; fr.afSunDir[1] = -fr.afSunDir[1]; fr.afSunDir[2] = -fr.afSunDir[2];
		}
	}
	fr.fBrightness = set.fBrightness;

	// Fog: the game's own curve, by distance.
	fr.afFogColour[0] = clrDefEndDepth.u1Red / 255.0f;
	fr.afFogColour[1] = clrDefEndDepth.u1Green / 255.0f;
	fr.afFogColour[2] = clrDefEndDepth.u1Blue / 255.0f;
	// The game's own fog, stretched over its view distance times FogScale.
	fr.fFogFar = std::max(50.0f, f_far * std::max(0.5f, set.fFogScale));
	{
		float f_max = fogFog.fGetFogLevel(1.0f);
		if (f_max <= 0)
			f_max = 1;
		for (int i = 0; i < 32; i++)
			fr.afFogTable[i] = std::min(1.0f, std::max(0.0f, fogFog.fGetFogLevel(i / 31.0f) / f_max));
	}

	// Sky.
	for (int k = 0; k < 3; k++)
		fr.afSkyFogColour[k] = fr.afFogColour[k];
	if (gpskyRender)
	{
		CSkyRender::SVRSkyInfo si;
		gpskyRender->GetVRInfo(si);
		uint32_t u4_fog = u4From565(si.u2Fog);
		fr.afSkyFogColour[0] = (u4_fog & 255) / 255.0f;
		fr.afSkyFogColour[1] = ((u4_fog >> 8) & 255) / 255.0f;
		fr.afSkyFogColour[2] = ((u4_fog >> 16) & 255) / 255.0f;
		if (si.bTextured)
		{
			if (si.pu1Tex != pvSkyTexture)
			{
				std::vector<uint32_t> au4((size_t)si.iWidth * si.iHeight);
				for (int y = 0; y < si.iHeight; y++)
					for (int x = 0; x < si.iWidth; x++)
					{
						int i_c = si.pu1Tex[(size_t)y * si.iStride + x];
						au4[(size_t)y * si.iWidth + x] = u4From565(si.pu2Colours[std::min(i_c, si.iColours - 1)]);
					}
				iSkyTexture = VRR_TextureCreate(si.iWidth, si.iHeight, au4.data());
				pvSkyTexture = si.pu1Tex;
			}
			dSkyTime += 1.0 / 90.0;
			fr.iSkyTexture = iSkyTexture;
			fr.fSkyHeight = si.fHeight;
			float f_tex = (float)std::max(si.iWidth, 1);
			fr.fSkyUVScale = si.fScale / 65536.0f / f_tex;
			fr.afSkyOffset[0] = (float)std::fmod(si.fOffsetU / 65536.0f / f_tex + si.fDriftU * dSkyTime, 1.0);
			fr.afSkyOffset[1] = (float)std::fmod(si.fOffsetV / 65536.0f / f_tex + si.fDriftV * dSkyTime, 1.0);
			fr.fSkyFogNear = si.fFogNear;
			fr.fSkyFogScale = si.fFogPerZ;
		}
	}

	// Shadows around the player.
	fr.afShadowCentre[0] = wk.v3ShadowCentre.tX;
	fr.afShadowCentre[1] = wk.v3ShadowCentre.tY;
	fr.afShadowCentre[2] = wk.v3ShadowCentre.tZ;
	fr.fShadowRadius = set.fShadowDistance;

	VRR_RenderFrame(fr);
	VRB_SubmitTexture(0, VRR_EyeTexture(0));
	VRB_SubmitTexture(1, VRR_EyeTexture(1));

	int i_draws, i_batches, i_tex, i_meshes;
	float f_mb;
	VRR_GetStats(i_draws, i_batches, i_tex, i_meshes, f_mb);
	std::snprintf(strStats, sizeof(strStats), "GPU renderer: %d objects, %d characters, %d draws in %d card calls (3 passes), %d textures (%.0f MB), %d meshes, %d textures loading; "
	              "walk: %d instances, %d hidden, %d no shape, %d not mesh, %d empty, %d far, %d behind.",
	              iObjects, iSkinned, i_draws, i_batches, i_tex, f_mb, i_meshes, iTexturesWaiting,
	              wsLast.iSeen, wsLast.iHidden, wsLast.iNoShape, wsLast.iNotMesh, wsLast.iEmpty, wsLast.iFar, wsLast.iBehind);
	return true;
}

//**********************************************************************************************
const char* VRScene_Stats()
{
	return strStats;
}

//**********************************************************************************************
void VRScene_AbortFrame()
{
	VRR_AbortFrame();
}
