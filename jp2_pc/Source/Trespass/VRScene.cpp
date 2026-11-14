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
#include "Lib/Physics/InfoSkeleton.hpp"
#include "Lib/Renderer/Camera.hpp"
#include "Lib/Renderer/Light.hpp"
#include "Lib/Renderer/Texture.hpp"
#include "Lib/Renderer/Fog.hpp"
#include "Lib/Renderer/Sky.hpp"
#include "Lib/View/Raster.hpp"
#include "Lib/View/Palette.hpp"
#include "Lib/View/Clut.hpp"
#include "Game/DesignDaemon/Player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>

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
	};

	std::unordered_map<const CMesh*, SMeshEntry> mapMesh;
	std::unordered_map<const void*, int>         mapTex;		// Raster (or texture) -> handle.
	std::unordered_map<const void*, bool>        mapTexAlpha;

	const CTerrain* ptrrLevel = nullptr;
	const void*     pvLevelPlayer = nullptr;
	bool            bReady = false;
	double          dSkyTime = 0;
	int             iSkyTexture = 0;
	const void*     pvSkyTexture = nullptr;
	char            strStats[256] = "";
	int             iObjects = 0, iSkinned = 0;

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

		// Solid colour.
		if (ptex->iGetNumMipLevels() <= 0)
		{
			auto it = mapTex.find(ptex);
			if (it != mapTex.end())
				return it->second;
			uint32_t u4 = u4From565(ptex->tpSolid);
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
						if (ptex->seterfFeatures[erfBUMP] && ppal)
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
				if (b_transparent && i_index == 0)
				{
					u4 &= 0x00FFFFFF;
					b_any_clear = true;
				}
				pu4[x] = u4;
			}
		}

		// See-through texels take the colour of a neighbour, so filtering doesn't halo.
		if (b_any_clear)
		{
			for (int i_pass = 0; i_pass < 2; i_pass++)
			{
				for (int y = 0; y < i_h; y++)
				{
					for (int x = 0; x < i_w; x++)
					{
						uint32_t& u4 = au4[(size_t)y * i_w + x];
						if (u4 >> 24)
							continue;
						const int ai_dx[4] = { 1, -1, 0, 0 }, ai_dy[4] = { 0, 0, 1, -1 };
						for (int k = 0; k < 4; k++)
						{
							int nx = (x + ai_dx[k] + i_w) % i_w, ny = (y + ai_dy[k] + i_h) % i_h;
							uint32_t u4_n = au4[(size_t)ny * i_w + nx];
							if (u4_n >> 24)
							{
								u4 = u4_n & 0x00FFFFFF;
								break;
							}
						}
					}
				}
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
	void AddSubsets(const SMeshEntry& me, int i_mesh, const float af_world[12], uint32_t u4_flags, bool b_decal)
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

		uint32_t u4_flags = 0;
		if (pins == gVR.pvMirrorInstance)
			u4_flags |= evrrTWO_SIDED;

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

	void Visit(CPartition* ppart, SWalk& wk)
	{
		CInstance* pins = ptCast<CInstance>(ppart);
		if (pins)
		{
			if (ptCast<CTerrainObj>(pins))
			{
				if (wk.papinsTerrainObjs)
					wk.papinsTerrainObjs->push_back(pins);
				return;
			}
			if (!ppart->bIsVisible())
				return;

			rptr_const<CRenderType> prdt = pins->prdtGetRenderInfo();
			if (!prdt)
				return;
			rptr_const<CMesh> pmsh = ptCastRenderType<CMesh>(prdt);
			if (!pmsh)
				return;
			rptr_const<CBioMesh> pbm = prdt->rpbmCast();

			const CPresence3<>& pr3 = pins->pr3Presence();
			SMeshEntry* pme = pmeGet(pmsh.ptGet(), pbm.ptGet() != nullptr);
			if (!pme || pme->asub.empty())
				return;

			// Out of sight: too far, or behind you (unless it can cast a shadow you see).
			float f_r = pme->fRadius * pr3.rScale + 1.0f;
			CVector3<> v3_to = pr3.v3Pos - wk.v3Eye;
			float f_dist = v3_to.tLen();
			if (f_dist - f_r > wk.fFar)
				return;
			if (f_dist > f_r + 2.0f && (v3_to * wk.v3Fwd) < -0.6f * f_dist)
			{
				if (wk.fShadow <= 0 || (pr3.v3Pos - wk.v3ShadowCentre).tLen() > wk.fShadow + f_r)
					return;
			}

			float af_world[12];
			WorldRows(pr3, af_world);
			if (pbm.ptGet())
				DrawSkinned(pins, pbm.ptGet(), *pme, af_world);
			else if (pme->iHandle)
			{
				uint32_t u4_flags = 0;
				if (pr3.rScale < 0)
					u4_flags |= evrrTWO_SIDED;
				AddSubsets(*pme, pme->iHandle, af_world, u4_flags, false);
				iObjects++;
			}
		}

		CPartition* ppartc = ppart->ppartChildren();
		if (ppartc)
		{
			for (CPartition::iterator it = ppartc->begin(); it != ppartc->end(); ++it)
				Visit(*it, wk);
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
		int i_nx = (int)(f_w / f_spacing) + 2, i_ny = (int)(f_h / f_spacing) + 2;

		std::vector<float> af_h((size_t)i_nx * i_ny);
		std::vector<SVRRTerrainVertex> atv((size_t)i_nx * i_ny);
		for (int y = 0; y < i_ny; y++)
		{
			for (int x = 0; x < i_nx; x++)
			{
				float f_x = f_x0 + x * f_spacing, f_y = f_y0 + y * f_spacing;
				af_h[(size_t)y * i_nx + x] = (float)ptrr->rHeight(f_x, f_y);
			}
		}
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
		int i_decals = 0;
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
			AddSubsets(*pme, pme->iHandle, af_world, 0, true);
			i_decals++;
		}
		VRR_TerrainBuildAtlas();

		SceneLog("GPU renderer: terrain %.0f x %.0f m, grid %.1f m, %.0f terrain textures.", f_w, f_h, f_spacing, i_decals);
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
		iSkyTexture = 0;
		pvSkyTexture = nullptr;
		VRR_ClearAll();
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

	iObjects = iSkinned = 0;
	Visit(pwWorld->ppartPartitionList(), wk);

	if (b_new_level && ptrr)
		BuildTerrain(ptrr, apins_trr, std::max(0.25f, set.fTerrainDetail));

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
	fr.fFogFar = wk.fFar;
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

	int i_draws, i_tex, i_meshes;
	float f_mb;
	VRR_GetStats(i_draws, i_tex, i_meshes, f_mb);
	std::snprintf(strStats, sizeof(strStats), "GPU renderer: %d objects, %d characters, %d draws, %d textures (%.0f MB), %d meshes.",
	              iObjects, iSkinned, i_draws, i_tex, f_mb, i_meshes);
	return true;
}

//**********************************************************************************************
const char* VRScene_Stats()
{
	return strStats;
}
