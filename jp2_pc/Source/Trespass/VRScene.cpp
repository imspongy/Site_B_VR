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
#include "Lib/EntityDBase/MessageTypes/MsgStep.hpp"
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
#include "Lib/Physics/InfoBox.hpp"
#include "Lib/Physics/InfoCompound.hpp"
#include "Lib/EntityDBase/Query/QPhysics.hpp"
#include "Lib/Renderer/Camera.hpp"
#include "Lib/Renderer/Light.hpp"
#include "Lib/Renderer/Texture.hpp"
#include "Lib/Renderer/Fog.hpp"
#include "Lib/Renderer/Sky.hpp"
#include "Lib/Renderer/Particles.hpp"
#include "Lib/Renderer/Primitives/FastBump.hpp"
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
	std::vector<float> afTriScratch;
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
	bool            bTerrainBuilt = false;
	int             iTerrainTries = 0;

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
	std::unordered_map<uint32_t, int> mapSolidTex;	// Plain colour -> its 1x1 texture.

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
			{
				int r = u4 & 255, g = (u4 >> 8) & 255, bl = (u4 >> 16) & 255;
				static int i_logged = 0;
				if (bl > 160 && r < 80 && g < 80 && i_logged < 20 && gVR.pfnLog)
				{
					i_logged++;
					gVR.pfnLog("Texture check: plain colour %d %d %d (game colour %04X, card colour %08X, %s).", r, g, bl,
						(unsigned)ptex->tpSolid, (unsigned)(uint32)ptex->d3dpixColour, b_transparent ? "see-through" : "solid");
				}
			}
			// The key blue as a plain colour on a solid surface is the game's black (the black
			// obelisk: game colour 00FF, palette entry 0).
			if (!b_transparent && !b_alpha_colour)
			{
				int r = u4 & 255, g = (u4 >> 8) & 255, bl = (u4 >> 16) & 255;
				if (bl > 200 && r < 40 && g < 60)
					u4 = u4RGBA(0, 0, 0);
			}
			// Kept by the colour itself, not by the texture's address: an address can be
			// reused by another texture (sparks once drew with whatever image had been there).
			auto it = mapSolidTex.find(u4);
			if (it != mapSolidTex.end())
				return it->second;
			int i_h = VRR_TextureCreate(1, 1, &u4);
			mapSolidTex[u4] = i_h;
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
		// Bump-mapped skins (raptors) that aren't see-through still have texels of colour 0, the
		// blue key colour, in the gaps between the parts of the skin; filtering and mipmaps
		// pulled that blue into the edges, seen as a blue sheen at an angle.  Those texels take
		// the nearest skin colour too, but stay solid.
		bool b_pad_key = false;
		if (!b_transparent && ptex->seterfFeatures[erfBUMP] && i_pal > 0)
		{
			const CColour& clr0 = ppal->aclrPalette[0];
			b_pad_key = clr0.u1Blue > clr0.u1Red + 60 && clr0.u1Blue > clr0.u1Green + 60;
		}
		bool b_key_black = false;
		if (!b_transparent && !b_pad_key && i_pal > 0)
		{
			const CColour& clr0 = ppal->aclrPalette[0];
			b_key_black = clr0.u1Red < 64 && clr0.u1Green < 64 && clr0.u1Blue > 192;	// The pure key blue only.
		}
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
				// See-through texels: colour 0, as the game treats them (bump-mapped textures too:
				// skipping those left their blue key colour in raptor mouths, stego sails, glass).
				if ((b_transparent || b_pad_key) && i_index == 0)
				{
					u4 &= 0x00FFFFFF;
					b_any_clear = true;
				}
				// In a solid texture, colour 0 is the blue key colour standing for black (the
				// game's colour tables keep entry 0 black): the black obelisk came out blue.
				else if (b_key_black && i_index == 0)
					u4 = u4RGBA(0, 0, 0);
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

		if (b_pad_key && b_any_clear)
		{
			for (uint32_t& u4 : au4)
				u4 |= 0xFF000000u;
			b_any_clear = false;
		}

		// Texture check (the black obelisk drew blue): textures that are nearly all pure blue.
		{
			size_t u_blue = 0;
			for (uint32_t u : au4)
			{
				int r = u & 255, g = (u >> 8) & 255, b = (u >> 16) & 255;
				if (b > 160 && r < 80 && g < 80)
					u_blue++;
			}
			if (u_blue * 10 > au4.size() * 9 && gVR.pfnLog)
			{
				const CColour* pclr0 = i_pal > 0 ? &ppal->aclrPalette[0] : nullptr;
				gVR.pfnLog("Texture check: %dx%d, %d bits, %s%s%s%s, palette %d (first %d %d %d), mostly blue.", i_w, i_h, praw->iPixelBits,
					b_transparent ? "see-through" : "solid", ptex->seterfFeatures[erfBUMP] ? ", bump" : "",
					b_alpha_colour ? ", alpha colour" : "", b_key_black ? ", key drawn black" : "", i_pal,
					pclr0 ? pclr0->u1Red : -1, pclr0 ? pclr0->u1Green : -1, pclr0 ? pclr0->u1Blue : -1);
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

	// The normal map (relief) of a bump-mapped texture, made when first asked for; 0 if none.
	// Only animated surfaces (sea, surf, waterfalls) use these: their movement is in the relief.
	std::unordered_map<const void*, int> mapNormalTex;
	int iNormalTexture(const CTexture* ptex)
	{
		if (!ptex || ptex->iGetNumMipLevels() <= 0 || !ptex->seterfFeatures[erfBUMP])
			return 0;
		rptr<CRaster> pras = ptex->prasGetTexture(0);
		const CRaster* praw = pras.ptGet();
		if (!praw || !praw->pSurface || praw->iWidth <= 0 || praw->iHeight <= 0)
			return 0;
		auto it = mapNormalTex.find(praw);
		if (it != mapNormalTex.end())
			return it->second;
		int i_bytes = praw->iPixelBits / 8;
		int i_n = 0;
		if (i_bytes == 2 || i_bytes == 4)
		{
			int i_w = praw->iWidth, i_h = praw->iHeight, i_stride = praw->iLinePixels;
			std::vector<uint32_t> au4_n((size_t)i_w * i_h);
			const uint8* pu1_r = (const uint8*)praw->pSurface;
			for (int y = 0; y < i_h; y++, pu1_r += (size_t)i_stride * i_bytes)
				for (int x = 0; x < i_w; x++)
				{
					CBumpAnglePair bang;
					bang.br = i_bytes == 2 ? (TBumpRes)((const uint16*)pu1_r)[x] : (TBumpRes)((const uint32*)pu1_r)[x];
					CDir3<> d3 = bang.dir3MakeNormal();
					auto enc = [](float f) { return (uint32_t)std::max(0, std::min(255, (int)(f * 127.5f + 127.5f))); };
					au4_n[(size_t)y * i_w + x] = enc(d3.tX) | (enc(d3.tY) << 8) | (enc(d3.tZ) << 16) | 0xFF000000u;
				}
			i_n = VRR_TextureCreate(i_w, i_h, au4_n.data());
		}
		mapNormalTex[praw] = i_n;
		return i_n;
	}

	//******************************************************************************************
	// Physics view (ShowPhysics=1): the game's collision boxes near you, see-through red
	// (tangible) or blue (not), so you can see where things really are to the physics.
	// A capture also logs them.
	bool gbShowPhysics = false;
	std::vector<CInstance*> apinsNear, apinsNearLast;	// Drawn within 8 m (this frame, last frame).
	int  iPhysTexRed = 0, iPhysTexBlue = 0;
	CVector3<> v3LastEye;

	// Each box: world placement and half extents (min/max corners in box space).
	template<class F> void ForEachPhysicsBox(const CVector3<>& v3_centre, float f_radius, F f)
	{
		CBoundVolSphere bvs(f_radius);
		CPresence3<> pr3_q;
		pr3_q.v3Pos = v3_centre;
		for (CWDbQueryPhysics wqph(bvs, pr3_q); wqph.bIsNotEnd(); wqph++)
		{
			CInstance* pins = *wqph;
			const CPhysicsInfo* pphi = pins ? pins->pphiGetPhysicsInfo() : nullptr;
			if (!pphi || pins == gpPlayer)
				continue;
			CPlacement3<> p3_ins = pins->pr3Presence();
			if (const CPhysicsInfoCompound* pphic = pphi->pphicCast())
			{
				for (int i = 0; i < pphic->iNumSubmodels; i++)
				{
					const CPhysicsInfo* pphi_s = pphic->apphiSubmodels[i];
					const CBoundVolBox* pbvb = pphi_s && pphi_s->pbvGetCollideVol() ? pphi_s->pbvGetCollideVol()->pbvbCast() : nullptr;
					if (pbvb)
						f(pins, pphi, i, CPlacement3<>(pphic->apr3SubmodelPresences[i]) * p3_ins, *pbvb);
				}
			}
			else
			{
				const CBoundVolBox* pbvb = pphi->pbvGetCollideVol() ? pphi->pbvGetCollideVol()->pbvbCast() : nullptr;
				if (pbvb)
					f(pins, pphi, -1, p3_ins, *pbvb);
			}
		}
	}

	void DrawPhysicsBoxes(const CVector3<>& v3_eye)
	{
		if (!iPhysTexRed)
		{
			uint32_t u4_r = u4RGBA(255, 40, 30, 70), u4_b = u4RGBA(40, 90, 255, 55);
			iPhysTexRed = VRR_TextureCreate(1, 1, &u4_r);
			iPhysTexBlue = VRR_TextureCreate(1, 1, &u4_b);
		}
		std::vector<SVRRVertex> aav[2];
		ForEachPhysicsBox(v3_eye, 12.0f, [&](CInstance* pins, const CPhysicsInfo* pphi, int, const CPlacement3<>& p3, const CBoundVolBox& bvb)
		{
			float ax[2] = { bvb[ebeMIN_X], bvb[ebeMAX_X] }, ay[2] = { bvb[ebeMIN_Y], bvb[ebeMAX_Y] }, az[2] = { bvb[ebeMIN_Z], bvb[ebeMAX_Z] };
			CVector3<> av3[8];
			for (int c = 0; c < 8; c++)
				av3[c] = CVector3<>(ax[c & 1], ay[(c >> 1) & 1], az[(c >> 2) & 1]) * p3;
			static const int aiFace[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
			std::vector<SVRRVertex>& av = aav[pphi->bIsTangible() ? 0 : 1];
			for (int fc = 0; fc < 6; fc++)
				for (int k = 0; k < 4; k++)
				{
					SVRRVertex v;
					const CVector3<>& c = av3[aiFace[fc][k]];
					v.afPos[0] = c.tX; v.afPos[1] = c.tY; v.afPos[2] = c.tZ;
					v.afNormal[0] = 0; v.afNormal[1] = 0; v.afNormal[2] = 1;
					v.afUV[0] = 0.5f; v.afUV[1] = 0.5f;
					av.push_back(v);
				}
		});
		static const float af_identity[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
		for (int t = 0; t < 2; t++)
		{
			std::vector<SVRRVertex>& av = aav[t];
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
			SVRRDraw drw;
			drw.iMesh = i_mesh;
			drw.iFirstIndex = 0;
			drw.iNumIndices = (int)au4.size();
			drw.iTexture = t == 0 ? iPhysTexRed : iPhysTexBlue;
			std::memcpy(drw.afWorld, af_identity, sizeof(drw.afWorld));
			drw.u4Flags = evrrBLEND | evrrUNLIT | evrrNO_FOG | evrrNO_SHADOW | evrrTWO_SIDED;
			VRR_AddDraw(drw);
		}
	}

	// Queues draws of every subset of a mesh.
	void AddSubsets(const SMeshEntry& me, int i_mesh, const float af_world[12], uint32_t u4_flags, bool b_decal, uint32_t u4_force = 0, int i_tex_force = -1,
	                const float* pf_uv = nullptr, bool b_bump = false)
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
			if (b_bump && i_tex_force < 0 && !b_decal)
			{
				drw.iNormalTex = iNormalTexture(ptex);
				if (drw.iNormalTex)
					drw.u4Flags |= evrrBUMP;
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
		// Anne's hands: the game's own hand shape for what each hand holds.  Her body carries the
		// right hand and the mirrored arm the left; both are drawn from the same mesh, so the
		// shape is put on just before each is skinned.
		if (pins && (pins == (CInstance*)gpPlayer || (const void*)pins == gVR.pvMirrorInstance))
		{
			int h_shape = (const void*)pins == gVR.pvMirrorInstance ? 1 : 0;
			// A hand holding something has that thing's grip shape; an empty hand has the shape
			// the level asks for (the pointing finger at phones and keypads).
			int i_shape = gVR.abHolding[h_shape] ? gVR.aiHandShape[h_shape] : gVR.iGameHandShape;
			// Each shape only replaces the points it lists, and the game also changes the shape of
			// this same mesh for its own hand: so start from the plain hand every time, then put
			// this hand's shape on (otherwise a hand kept parts of the shape before it, and the
			// rifle hand looked like the Benelli's).
			CSubBioMesh* psbm = dynamic_cast<CSubBioMesh*>(const_cast<CBioMesh*>(pbm));
			if (psbm && !psbm->lpsubSubstitutes.empty())
			{
				psbm->Substitute(0);
				if (i_shape > 0 && i_shape < (int)psbm->lpsubSubstitutes.size())
					psbm->Substitute(i_shape);
			}
		}
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
		float      fDetailCull;
		std::vector<CInstance*>* papinsTerrainObjs;		// Collect terrain objects (level start).
	};

	// Counters for the log: why instances were not drawn.
	struct SWalkStats
	{
		int iSeen, iHidden, iNoShape, iNotMesh, iEmpty, iFar, iBehind, iSmall;
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

	// Bats, pipes and similar weapons: light, movable, long and thin (narrower than a plank).
	std::unordered_map<const CInstance*, bool> mapMelee;
	bool bLooksMelee(CInstance* pins)
	{
		auto it = mapMelee.find(pins);
		if (it != mapMelee.end())
			return it->second;
		bool b = false;
		const CPhysicsInfo* pphi = pins->pphiGetPhysicsInfo();
		const CBoundVolBox* pbvb = pins->prdtGetRenderInfo() ? pins->prdtGetRenderInfo()->bvGet().pbvbCast() : nullptr;
		if (pphi && pphi->bIsMovable() && pbvb)
		{
			float f_s = std::fabs(pins->fGetScale());
			float af_d[3] = { ((*pbvb)[ebeMAX_X] - (*pbvb)[ebeMIN_X]) * f_s, ((*pbvb)[ebeMAX_Y] - (*pbvb)[ebeMIN_Y]) * f_s, ((*pbvb)[ebeMAX_Z] - (*pbvb)[ebeMIN_Z]) * f_s };
			std::sort(af_d, af_d + 3);
			float f_mass = pphi->fMass(pins);
			b = af_d[2] >= 0.5f && af_d[2] <= 1.4f && af_d[1] <= 0.12f && f_mass <= 8.0f;
		}
		mapMelee[pins] = b;
		return b;
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
			if (!pins_held || !gVR.abHandValid[h])
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
			float f_k = gVR.abHoldingGun[h] ? std::max(0.5f, std::min(gVR.fGunScale, 1.5f))
			          : gVR.abHoldingMelee[h] ? gVR.fMeleeDrawScale : 1.0f;
			CVector3<> v3_pos = p3_ctrl.v3Pos + (p3_inst.v3Pos - p3_ctrl.v3Pos) * f_k;
			rpr3 = CPresence3<>(p3_inst.r3Rot, rpr3.rScale * f_k, v3_pos);
			return true;
		}
		// Holstered things: drawn on her body exactly where her body is drawn this frame.
		if (gVR.bBodyDrawnPosValid)
			for (int i = 0; i < 4; i++)
			{
				if (!gVR.apvHolstered[i] || pins != (CInstance*)gVR.apvHolstered[i])
					continue;
				const float* af = gVR.aafStowRel[i];
				const float* af_b = gVR.afBodyDrawnPlace;
				CPlacement3<> p3_rel(CRotate3<>(af[3], af[4], af[5], af[6], true), CVector3<>(af[0], af[1], af[2]));
				CPlacement3<> p3_body(CRotate3<>(af_b[3], af_b[4], af_b[5], af_b[6], true), CVector3<>(af_b[0], af_b[1], af_b[2]));
				CPlacement3<> p3 = p3_rel * p3_body;
				rpr3 = CPresence3<>(p3.r3Rot, rpr3.rScale, p3.v3Pos);
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

		// (For the capture log: everything near you, physics or not.)
		if (f_dist - f_r < 8.0f && apinsNear.size() < 200)
			apinsNear.push_back(pins);
		if (f_dist - f_r > wk.fFar)
		{
			ws.iFar++;
			return;
		}
		// Too small to see from here (a few pixels across): not drawn.  Level 2's thousands of
		// small things far off cost more to draw than they show.  (Dinosaurs and anything in or
		// on your hands are always drawn.)
		if (wk.fDetailCull > 0 && !pbm.ptGet())
		{
			float f_size = pme->fRadius * std::fabs(pr3.rScale);
			if (f_size < wk.fDetailCull * f_dist && f_dist > 3.0f)
			{
				ws.iSmall++;
				return;
			}
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
		if (!pbm.ptGet() && (gVR.apvHeldFirm[0] || gVR.apvHeldFirm[1] || gVR.apvHolstered[0] || gVR.apvHolstered[1] || gVR.apvHolstered[2] || gVR.apvHolstered[3]))
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
				// (Once per game step, as the game itself did: its waves step with the game, and
				// re-running them for every drawn frame re-sized their arrays far more often.)
				{
					static std::unordered_map<const void*, TSec> mapWaterStepped;
					TSec s_now = CMessageStep::sStaticTotal;
					auto it_s = mapWaterStepped.find(pins);
					if (gVR.bWaterWaves && (it_s == mapWaterStepped.end() || it_s->second != s_now))
					{
						mapWaterStepped[pins] = s_now;
						PreRenderInstance(pins);
					}
				}
				if (!bDrawWater(pmsh.ptGet(), af_world))
					AddSubsets(*pme, pme->iHandle, af_world, 0, false, evrrBLEND | evrrWATER | evrrTWO_SIDED | evrrNO_SHADOW, it->second);
				iObjects++;
				return;
			}
			float af_uv[2] = { 0, 0 };
			bool b_animated = pmsh->bIsAnimated();
			if (b_animated)
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
			AddSubsets(*pme, pme->iHandle, af_world, u4_flags, false, 0, -1, af_uv, b_animated);
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
	// The coarse grid's heights, for meeting it exactly at the edge of the fine patches.
	std::vector<float> afCoarseH;
	int   iCoarseNX = 0, iCoarseNY = 0;
	float fCoarseX0 = 0, fCoarseY0 = 0, fCoarseStep = 1;
	float fCoarseHeight(float f_x, float f_y, bool& rb_ok)
	{
		rb_ok = false;
		if (afCoarseH.empty())
			return 0;
		float fx = (f_x - fCoarseX0) / fCoarseStep, fy = (f_y - fCoarseY0) / fCoarseStep;
		int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
		if (ix < 0 || iy < 0 || ix >= iCoarseNX - 1 || iy >= iCoarseNY - 1)
			return 0;
		float u = fx - ix, v = fy - iy;
		float ha = afCoarseH[(size_t)iy * iCoarseNX + ix], hb = afCoarseH[(size_t)iy * iCoarseNX + ix + 1];
		float hc = afCoarseH[(size_t)(iy + 1) * iCoarseNX + ix], hd = afCoarseH[(size_t)(iy + 1) * iCoarseNX + ix + 1];
		rb_ok = true;
		// Same split as the coarse grid's triangles (a-b-d, a-d-c).
		return u >= v ? ha + u * (hb - ha) + v * (hd - hb) : ha + v * (hc - ha) + u * (hd - hc);
	}
	float fPATCH_SIZE = 32.0f;			// Set per level: 64 steps of the game's own terrain point spacing.
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
	float fTerrainHeight(CTerrain* ptrr, float f_x, float f_y);
	bool bBaseColourSafe(CTerrain* ptrr, float f_x, float f_y, CColour* pclr);
	uint32_t u4BaseColour(CTerrain* ptrr, float f_x, float f_y);

	//******************************************************************************************
	// Animated ground textures (surf washing up the beach, and the like): the game animates
	// these as it draws them.  Each frame: advance the animation, lay the mesh on the ground
	// under it (as the game does: ground textures follow the terrain), draw it.
	std::vector<CInstance*> apinsAnimTerrain;

	void DrawAnimatedTerrain(CTerrain* ptrr, const CVector3<>& v3_eye)
	{
		if (!ptrr || apinsAnimTerrain.empty())
			return;
		for (CInstance* pins : apinsAnimTerrain)
		{
			rptr_const<CMesh> pmsh = ptCastRenderType<CMesh>(pins->prdtGetRenderInfo());
			if (!pmsh || !pmsh->bIsAnimated())
				continue;
			const CPresence3<>& pr3 = pins->pr3Presence();
			float f_r = 0;
			for (uint u = 0; u < pmsh->pav3Points.uLen; u++)
				f_r = std::max(f_r, (float)pmsh->pav3Points[u].tLen());
			if ((CVector2<>(pr3.v3Pos) - CVector2<>(v3_eye)).tLen() > 250.0f + f_r * std::fabs(pr3.rScale))
				continue;
			const CMeshAnimating* pma = static_cast<const CMeshAnimating*>(pmsh.ptGet());
			if (setAnimatedThisFrame.insert(pmsh.ptGet()).second)
				pma->Animate();

			CTerrainObj* ptobj = ptCast<CTerrainObj>(pins);
			float f_lift = 0.03f + (ptobj ? ptobj->iHeight : 0) * 0.004f;
			const CMesh::SVertex* pmv0 = pmsh->pamvVertices.atArray;
			int i_verts = (int)pmsh->pamvVertices.uLen;
			std::vector<SVRRVertex> av((size_t)i_verts);
			for (int i = 0; i < i_verts; i++)
			{
				CVector3<> v3 = *pmv0[i].pv3Point * pr3;
				v3.tZ = fTerrainHeight(ptrr, v3.tX, v3.tY) + f_lift;
				SVRRVertex& v = av[i];
				v.afPos[0] = v3.tX; v.afPos[1] = v3.tY; v.afPos[2] = v3.tZ;
				v.afNormal[0] = 0; v.afNormal[1] = 0; v.afNormal[2] = 1;
				v.afUV[0] = pmv0[i].tcTex.tX; v.afUV[1] = pmv0[i].tcTex.tY;
			}
			ReleaseQueries(ptrr);

			// Triangles by surface (texture).
			std::vector<std::pair<const CMesh::SSurface*, std::vector<uint32_t>>> asurf;
			for (uint u = 0; u < pmsh->pampPolygons.uLen; u++)
			{
				const CMesh::SPolygon& mp = pmsh->pampPolygons[u];
				int i_n = (int)mp.papmvVertices.uLen;
				if (i_n < 3)
					continue;
				const CMesh::SSurface* psf = (const CMesh::SSurface*)mp.pSurface;
				if (asurf.empty() || asurf.back().first != psf)
					asurf.push_back(std::make_pair(psf, std::vector<uint32_t>()));
				for (int i = 1; i + 1 < i_n; i++)
				{
					int a = (int)(mp.papmvVertices[0] - pmv0), b = (int)(mp.papmvVertices[i] - pmv0), c = (int)(mp.papmvVertices[i + 1] - pmv0);
					if (a < 0 || b < 0 || c < 0 || a >= i_verts || b >= i_verts || c >= i_verts)
						continue;
					asurf.back().second.push_back(a);
					asurf.back().second.push_back(b);
					asurf.back().second.push_back(c);
				}
			}
			std::vector<uint32_t> au4;
			std::vector<std::pair<int, int>> arange;
			for (auto& pr : asurf)
			{
				arange.push_back(std::make_pair((int)au4.size(), (int)pr.second.size()));
				au4.insert(au4.end(), pr.second.begin(), pr.second.end());
			}
			if (au4.empty())
				continue;
			int i_mesh = VRR_MeshDynamic(av.data(), (int)av.size(), au4.data(), (int)au4.size());
			if (!i_mesh)
				continue;
			static const float af_identity[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
			for (size_t k = 0; k < asurf.size(); k++)
			{
				if (arange[k].second <= 0 || !asurf[k].first)
					continue;
				const CTexture* ptex = asurf[k].first->ptexTexture.ptGet();
				bool b_alpha = false;
				SVRRDraw drw;
				drw.iMesh = i_mesh;
				drw.iFirstIndex = arange[k].first;
				drw.iNumIndices = arange[k].second;
				drw.iTexture = iTexture(ptex, b_alpha);
				std::memcpy(drw.afWorld, af_identity, sizeof(drw.afWorld));
				drw.u4Flags = evrrTWO_SIDED | evrrNO_SHADOW | (b_alpha ? evrrALPHA_TEST : 0);
				VRR_AddDraw(drw);
			}
		}
	}

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
				pjob.afH.assign((size_t)(iPATCH_N + 2) * (iPATCH_N + 2) + (size_t)(iPATCH_N - 1) * (iPATCH_N - 1), 0.0f);
			}

			// Sample.
			const int i_side = iPATCH_N + 2;
			const int i_grid = i_side * i_side;
			const int i_total = i_grid + (iPATCH_N - 1) * (iPATCH_N - 1);	// Then each square's middle.
			const float f_step = fPATCH_SIZE / (iPATCH_N - 1);
			float f_ox = fPatchX0 + pjob.iX * fPATCH_SIZE, f_oy = fPatchY0 + pjob.iY * fPATCH_SIZE;
			int i_since = 0;
			while (pjob.iNext < i_total)
			{
				if (pjob.iNext < i_grid)
				{
					int i_x = pjob.iNext % i_side - 1, i_y = pjob.iNext / i_side - 1;
					pjob.afH[pjob.iNext] = fTerrainHeight(ptrr, f_ox + i_x * f_step, f_oy + i_y * f_step);
				}
				else
				{
					int i_c = pjob.iNext - i_grid;
					int i_x = i_c % (iPATCH_N - 1), i_y = i_c / (iPATCH_N - 1);
					pjob.afH[pjob.iNext] = fTerrainHeight(ptrr, f_ox + (i_x + 0.5f) * f_step, f_oy + (i_y + 0.5f) * f_step);
				}
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
					tv.u4Colour = u4BaseColour(ptrr, f_x, f_y);
				}
			// Each square split along the diagonal the real ground folds on (the one whose
			// middle matches the ground's middle), as the game's own triangles are.
			std::vector<uint8_t> au1_flip((size_t)(iPATCH_N - 1) * (iPATCH_N - 1), 0);
			for (int y = 0; y < iPATCH_N - 1; y++)
				for (int x = 0; x < iPATCH_N - 1; x++)
				{
					auto h = [&](int i_x, int i_y) { return pjob.afH[(size_t)(i_y + 1) * i_side + (i_x + 1)]; };
					float f_mid = pjob.afH[(size_t)i_grid + (size_t)y * (iPATCH_N - 1) + x];
					float f_ad = 0.5f * (h(x, y) + h(x + 1, y + 1));
					float f_bc = 0.5f * (h(x + 1, y) + h(x, y + 1));
					au1_flip[(size_t)y * (iPATCH_N - 1) + x] = std::fabs(f_bc - f_mid) + 0.001f < std::fabs(f_ad - f_mid) ? 1 : 0;
				}
			// Skirt heights: each edge point down below the coarse ground, or up to it.
			std::vector<float> af_skirt((size_t)4 * iPATCH_N);
			for (int e = 0; e < 4; e++)
				for (int k = 0; k < iPATCH_N; k++)
				{
					int x = e == 0 || e == 1 ? k : e == 2 ? 0 : iPATCH_N - 1;
					int y = e == 2 || e == 3 ? k : e == 0 ? 0 : iPATCH_N - 1;
					const SVRRTerrainVertex& tv = atv[(size_t)y * iPATCH_N + x];
					bool b_ok;
					float f_c = fCoarseHeight(tv.afPos[0], tv.afPos[1], b_ok);
					af_skirt[(size_t)e * iPATCH_N + k] = b_ok && f_c > tv.afPos[2] + 0.01f ? f_c + 0.05f : tv.afPos[2] - 3.0f;
				}
			if (VRR_TerrainPatchSet(pjob.iSlot, iPATCH_N, atv.data(), au1_flip.data(), af_skirt.data()))
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
	// The game's terrain colour map is made a moment after a level loads; read before then it
	// faulted and threw the whole frame (and the terrain build) away.  Nothing here needs
	// unwinding, so a plain structured guard is safe.
	bool bBaseColourSafe(CTerrain* ptrr, float f_x, float f_y, CColour* pclr)
	{
		__try
		{
			*pclr = ptrr->clrGetBaseTextureColour(f_x, f_y);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	uint32_t u4BaseColour(CTerrain* ptrr, float f_x, float f_y)
	{
		CColour clr;
		if (!bBaseColourSafe(ptrr, f_x, f_y, &clr))
			return u4RGBA(128, 128, 128);
		return u4RGBA(clr.u1Red, clr.u1Green, clr.u1Blue);
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
	// A height from the game's terrain tree.  Each lookup builds a path of nodes down to the
	// finest level (about 50) in the game's fixed-size terrain memory, which holds 7000; the
	// game doesn't check for running out, and when it did (hundreds of lookups between
	// clean-ups) it wrote past the end, which later crashed the AI's and physics' own terrain
	// and object lookups (level 3).  So clean up every few lookups.
	float fTerrainHeight(CTerrain* ptrr, float f_x, float f_y)
	{
		float f = (float)ptrr->rHeight(f_x, f_y);
		// Clean up once well short of the limit (a clean-up walks the whole tree, so not after
		// every lookup: that made each lookup three times slower).
		if (NMultiResolution::CQuadNodeQuery::iNumInUse() > (int)uDEFAULT_ALLOC_QUERY_NODES / 3)
			ReleaseQueries(ptrr);
		return f;
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
				fTerrainHeight(ptrr, f_x0 + (i % 20) * f_w / 20.0f, f_y0 + (i / 20) * f_h / 20.0f);
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
				af_h[(size_t)y * i_nx + x] = fTerrainHeight(ptrr, f_x, f_y);

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
				tv.u4Colour = u4BaseColour(ptrr, f_x, f_y);
			}
		}

		// Atlas: about 4 texels a metre at normal detail.
		float f_texels = std::max(f_w, f_h) * 4.0f * f_detail;
		int i_atlas = 1024;
		while (i_atlas < f_texels && i_atlas < 16384)
			i_atlas *= 2;

		// The fine patches lie on the game's own terrain points (the wavelet tree's finest grid),
		// so their corners are the real corners of the ground: sampled off that grid, a sharp
		// edge (a trench lip) was smeared up to a quarter metre and covered what sat against it.
		{
			float f_res = 0.5f, f_gx = f_x0, f_gy = f_y0;
			NMultiResolution::CQuadRootQuery* pqnq = ptrr->pqnqGetQueryRoot();
			if (pqnq)
			{
				CVector2<> v2_0 = CVector2<>(0, 0) * pqnq->mpConversions.tlr2QuadToWorld;
				CVector2<> v2_1 = CVector2<>(1, 1) * pqnq->mpConversions.tlr2QuadToWorld;
				if (v2_1.tX - v2_0.tX > 0.01f && v2_1.tX - v2_0.tX < 16.0f)
				{
					f_res = v2_1.tX - v2_0.tX;
					f_gx = v2_0.tX;
					f_gy = v2_0.tY;
				}
			}
			int i_mul = std::max(1, (int)std::floor(0.5f / f_res + 0.5f));
			float f_step = f_res * i_mul;
			fPATCH_SIZE = f_step * (iPATCH_N - 1);
			fPatchX0 = f_gx + std::floor((f_x0 - f_gx) / f_step) * f_step;
			fPatchY0 = f_gy + std::floor((f_y0 - f_gy) / f_step) * f_step;
			SceneLog("GPU renderer: the game's terrain points are %.3f m apart; fine ground every %.3f m.", f_res, f_step);
		}
		afCoarseH = af_h;
		iCoarseNX = i_nx; iCoarseNY = i_ny;
		fCoarseX0 = f_x0; fCoarseY0 = f_y0; fCoarseStep = f_spacing;
		if (!VRR_TerrainSet(i_nx, i_ny, f_x0, f_y0, f_spacing, atv.data(), i_atlas))
		{
			SceneLog("GPU renderer: could not create the terrain.");
			return;
		}
		VRR_TerrainPatchGrid(fPatchX0, fPatchY0, fPATCH_SIZE);

		// Terrain textures, lowest first.
		std::stable_sort(apins_trr.begin(), apins_trr.end(), [](CInstance* a, CInstance* b)
		{
			CTerrainObj* pa = ptCast<CTerrainObj>(a);
			CTerrainObj* pb = ptCast<CTerrainObj>(b);
			return pa->iHeight < pb->iHeight;
		});
		int i_decals = 0, i_mapped = 0;
		apinsAnimTerrain.clear();
		for (CInstance* pins : apins_trr)
		{
			rptr_const<CMesh> pmsh = ptCastRenderType<CMesh>(pins->prdtGetRenderInfo());
			if (!pmsh)
				continue;
			if (pmsh->bIsAnimated())
			{
				// Animated ground textures (surf on the sand and the like) move every frame:
				// drawn live (DrawAnimatedTerrain), not baked into the ground.
				const CMeshAnimating* pma = static_cast<const CMeshAnimating*>(pmsh.ptGet());
				apinsAnimTerrain.push_back(pins);
				if (gVR.pfnLog)
					gVR.pfnLog("Animated ground texture %s at (%.0f, %.0f): %d polygons, %s.", pins->strGetInstanceName(),
					           pins->v3Pos().tX, pins->v3Pos().tY, (int)pmsh->pampPolygons.uLen, pma->bScrolls() ? "scrolling" : "frames");
				continue;
			}
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
				// No single flat mapping (roads, paths, beaches that follow a shape): its own
				// triangles go with it, in the same order the GPU mesh has them, so it is laid at
				// full resolution exactly as the game lays it.
				afTriScratch.clear();
				if (!b_map)
				{
					const CMesh::SVertex* pmv0 = pmsh->pamvVertices.atArray;
					int i_verts = (int)pmsh->pamvVertices.uLen;
					CPresence3<> pr3 = pins->pr3Presence();
					// Same polygon order as pmeGet: polygons of this surface, in mesh order.
					for (uint u = 0; u < pmsh->pampPolygons.uLen; u++)
					{
						const CMesh::SPolygon& mp = pmsh->pampPolygons[u];
						if (mp.bOcclude || mp.bCache || mp.papmvVertices.uLen < 3 || (const CMesh::SSurface*)mp.pSurface != sub.psf)
							continue;
						int i_n = (int)mp.papmvVertices.uLen;
						const CVector3<>& p0 = *mp.papmvVertices[0]->pv3Point;
						CVector3<> v3_sum(0, 0, 0);
						for (int i = 1; i + 1 < i_n; i++)
							v3_sum += (*mp.papmvVertices[i]->pv3Point - p0) ^ (*mp.papmvVertices[i + 1]->pv3Point - p0);
						bool b_reverse = (v3_sum * mp.plPlane.d3Normal) < 0;
						for (int i = 1; i + 1 < i_n; i++)
						{
							int ai[3] = { (int)(mp.papmvVertices[0] - pmv0), (int)(mp.papmvVertices[i] - pmv0), (int)(mp.papmvVertices[i + 1] - pmv0) };
							if (ai[0] < 0 || ai[1] < 0 || ai[2] < 0 || ai[0] >= i_verts || ai[1] >= i_verts || ai[2] >= i_verts)
								continue;
							if (b_reverse)
								std::swap(ai[1], ai[2]);
							for (int k = 0; k < 3; k++)
							{
								const CMesh::SVertex& mv = pmv0[ai[k]];
								CVector3<> v3_w = *mv.pv3Point * pr3;
								afTriScratch.push_back(v3_w.tX);
								afTriScratch.push_back(v3_w.tY);
								afTriScratch.push_back(mv.tcTex.tX);
								afTriScratch.push_back(mv.tcTex.tY);
							}
						}
					}
					// Must match the GPU subset triangle for triangle.
					if ((int)afTriScratch.size() / 12 != sub.iCount / 3)
						afTriScratch.clear();
				}
				VRR_TerrainAddDecal(drw, b_map ? af_map : nullptr, afTriScratch.empty() ? nullptr : afTriScratch.data(), (int)afTriScratch.size() / 12);
				if (b_map)
					i_mapped++;
				i_decals++;
			}
		}
		VRR_TerrainBuildAtlas();

		SceneLog("GPU renderer: terrain %.0f x %.0f m, grid %.1f m.", f_w, f_h, f_spacing);
		SceneLog("GPU renderer: %.0f terrain textures, %.0f at full resolution flat, %.0f by their own triangles.", i_decals, i_mapped, VRR_TerrainTriDecals());
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
	mapSolidTex.clear();
	mapTexAlpha.clear();
	mapNormalTex.clear();
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
// A level is about to load: let go of the old level's textures and meshes first, so the old
// and new levels aren't both in memory at once.  (Rebuilt when the new level is drawn.)
void VRScene_ReleaseLevel()
{
	if (!bReady)
		return;
	mapMesh.clear();
	mapTex.clear();
	mapSolidTex.clear();
	mapTexAlpha.clear();
	mapNormalTex.clear();
	mapWaterTex.clear();
	mapWaterRaster.clear();
	setBadInstances.clear();
	iSkyTexture = 0;
	pvSkyTexture = nullptr;
	iPhysTexRed = iPhysTexBlue = 0;
	iGlintTex = 0;
	mapMelee.clear();
	apinsNear.clear();
	apinsNearLast.clear();
	ptrrLevel = nullptr;
	pvLevelPlayer = nullptr;
	VRR_ClearAll();
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
	mapSolidTex.clear();
		mapTexAlpha.clear();
		mapNormalTex.clear();
	mapWaterTex.clear();
	mapWaterRaster.clear();
	setBadInstances.clear();
		iSkyTexture = 0;
		pvSkyTexture = nullptr;
		iPhysTexRed = iPhysTexBlue = 0;
		iGlintTex = 0;
		mapMelee.clear();
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
		bTerrainBuilt = false;
		iTerrainTries = 0;
	}
	// The terrain is built again if building it failed (a texture not ready yet: the whole
	// terrain was missing for that level).
	bool b_build_terrain = ptrr && !bTerrainBuilt && iTerrainTries < 10;
	if (b_build_terrain)
	{
		// Wait (the loading notice stays up) until the game has made the terrain's colour map,
		// rather than drawing a level with its ground missing; at most about ten seconds.
		static int i_wait = 0;
		CColour clr;
		CVector3<> v3_mid = gpPlayer ? gpPlayer->v3Pos() : CVector3<>(0, 0, 0);
		if (!bBaseColourSafe(ptrr, v3_mid.tX, v3_mid.tY, &clr) && i_wait < 900)
		{
			if (i_wait++ % 90 == 0)
				SceneLog("GPU renderer: waiting for the game to finish the terrain (%d).", i_wait);
			return false;
		}
		i_wait = 0;
		iTerrainTries++;
	}

	SWalk wk;
	wk.v3Eye = CVector3<>((eyes.afPos[0][0] + eyes.afPos[1][0]) * 0.5f, (eyes.afPos[0][1] + eyes.afPos[1][1]) * 0.5f, (eyes.afPos[0][2] + eyes.afPos[1][2]) * 0.5f);
	wk.v3Fwd = CVector3<>(eyes.afAxes[0][3], eyes.afAxes[0][4], eyes.afAxes[0][5]);
	wk.v3ShadowCentre = gpPlayer ? gpPlayer->v3Pos() : wk.v3Eye;
	float f_far = pcam ? (float)pcam->campropGetProperties().rFarClipPlaneDist : 500.0f;
	wk.fFar = std::max(50.0f, f_far * set.fDrawDistance);
	wk.fShadow = set.fShadowDistance;
	wk.fDetailCull = set.fDetailCull;
	wk.papinsTerrainObjs = b_build_terrain ? &apins_trr : nullptr;

	iObjects = iSkinned = iTexturesWaiting = 0;
	setAnimatedThisFrame.clear();
	UpdateGlintHandles(b_new_level);
	gbGlints = set.bGlints;
	aglGlints.clear();
	apinsNearLast.swap(apinsNear);
	apinsNear.clear();
	SWalkStats ws = {};
	Visit(pwWorld->ppartPartitionList(), wk, ws);
	if (b_build_terrain && pwWorld->ppartTerrainPartitionList())
		Visit(pwWorld->ppartTerrainPartitionList(), wk, ws);
	wsLast = ws;

	DrawParticles(wk.v3Eye);
	DrawGlints(wk.v3Eye);
	v3LastEye = wk.v3Eye;
	gbShowPhysics = set.bShowPhysics;
	if (gbShowPhysics)
		DrawPhysicsBoxes(wk.v3Eye);

	if (b_build_terrain)
	{
		if (iTerrainTries > 1)
			SceneLog("GPU renderer: building the terrain again (try %d).", iTerrainTries);
		BuildTerrain(ptrr, apins_trr, std::max(0.25f, set.fTerrainDetail));
		bTerrainBuilt = true;
		ResetPatches();
		if (gpPlayer)
			UpdatePatches(ptrr, gpPlayer->v3Pos(), 1.0e9);		// The ones around you, now.
	}
	else if (ptrr && gpPlayer)
		UpdatePatches(ptrr, gpPlayer->v3Pos(), 1.0);			// Then about 1 ms a frame (2 ms caused audio stutters).
	if (ptrr)
		DrawAnimatedTerrain(ptrr, wk.v3Eye);

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
		// Lighting check: every light, whenever what's used changes (lighting differed between loads).
		static float af_last[5] = { -9, -9, -9, -9, -9 };
		float af_now[5] = { fr.afSunDir[0], fr.afSunDir[1], fr.afSunDir[2], fr.fSun, fr.fAmbient };
		bool b_changed = false;
		for (int k = 0; k < 5; k++)
			if (std::fabs(af_now[k] - af_last[k]) > 0.01f)
				b_changed = true;
		if (b_changed && gVR.pfnLog)
		{
			gVR.pfnLog("Lights: sun (%.2f %.2f %.2f) strength %.2f, ambient %.2f, shadows %.0f m.", af_now[0], af_now[1], af_now[2], af_now[3], af_now[4], set.fShadowDistance);
			for (std::list<CInstance*>::const_iterator it = wql.begin(); it != wql.end(); ++it)
			{
				rptr<CLight> plt = ptCastRenderType<CLight>((*it)->prdtGetRenderInfo());
				if (!plt)
					continue;
				SLightInfo lti = plt->ltiGetInfo(CVector3<>(0, 0, 0));
				CVector3<> v3 = (CVector3<>)lti.d3Light * (*it)->pr3Presence().r3Rot;
				gVR.pfnLog("  light %s: %s, strength %.2f, direction (%.2f %.2f %.2f).", (*it)->strGetInstanceName(),
					plt->bIsDirectional() ? "directional" : plt->bIsPositional() ? "positional" : "ambient", (float)lti.lvStrength, v3.tX, v3.tY, v3.tZ);
			}
			for (int k = 0; k < 5; k++)
				af_last[k] = af_now[k];
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
	              "walk: %d instances, %d hidden, %d no shape, %d not mesh, %d empty, %d far, %d behind, %d too small.",
	              iObjects, iSkinned, i_draws, i_batches, i_tex, f_mb, i_meshes, iTexturesWaiting,
	              wsLast.iSeen, wsLast.iHidden, wsLast.iNoShape, wsLast.iNotMesh, wsLast.iEmpty, wsLast.iFar, wsLast.iBehind, wsLast.iSmall);
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

//**********************************************************************************************
void VRPhysicsSlotReport();	// (InfoBox.cpp)
void VRIgnoreReport(void (*pfn_log)(const char*, ...));	// (PhysicsSystem.cpp)

void VRScene_LogPhysics()
{
	// Every collision box within 8 m of you: what it belongs to, whether it's solid and
	// movable, awake in the physics, where it is, its size, and the drawn mesh's size.
	if (!gVR.pfnLog)
		return;
	VRPhysicsSlotReport();
	VRIgnoreReport(gVR.pfnLog);
	gVR.pfnLog("Physics near you (%.1f, %.1f, %.1f):", v3LastEye.tX, v3LastEye.tY, v3LastEye.tZ);
	int i_n = 0;
	ForEachPhysicsBox(v3LastEye, 8.0f, [&](CInstance* pins, const CPhysicsInfo* pphi, int i_sub, const CPlacement3<>& p3, const CBoundVolBox& bvb)
	{
		if (++i_n > 80)
			return;
		CVector3<> v3_c = CVector3<>((bvb[ebeMIN_X] + bvb[ebeMAX_X]) * 0.5f, (bvb[ebeMIN_Y] + bvb[ebeMAX_Y]) * 0.5f, (bvb[ebeMIN_Z] + bvb[ebeMAX_Z]) * 0.5f) * p3;
		float f_mesh = 0;
		rptr_const<CRenderType> prdt = pins->prdtGetRenderInfo();
		if (prdt)
			f_mesh = prdt->bvGet().fMaxExtent() * pins->fGetScale();
		gVR.pfnLog("  %s sub %d: %s%s%s at (%.2f, %.2f, %.2f) size %.2f x %.2f x %.2f, scale %.2f, mesh %.2f, origin (%.2f, %.2f, %.2f)",
			pins->strGetInstanceName(), i_sub, pphi->bIsTangible() ? "solid" : "not solid", pphi->bIsMovable() ? ", movable" : "",
			pphi->bIsActive(pins) ? ", awake" : "", v3_c.tX, v3_c.tY, v3_c.tZ,
			bvb[ebeMAX_X] - bvb[ebeMIN_X], bvb[ebeMAX_Y] - bvb[ebeMIN_Y], bvb[ebeMAX_Z] - bvb[ebeMIN_Z],
			pins->fGetScale(), f_mesh, pins->v3Pos().tX, pins->v3Pos().tY, pins->v3Pos().tZ);
	});

	// Everything drawn near you, with its physics (or lack of it) and where its mesh really is.
	gVR.pfnLog("Objects drawn near you:");
	for (CInstance* pins : apinsNearLast)
	{
		rptr_const<CMesh> pmsh = ptCastRenderType<CMesh>(pins->prdtGetRenderInfo());
		if (!pmsh || pins == gpPlayer)
			continue;
		CVector3<> v3_min(1e9f, 1e9f, 1e9f), v3_max(-1e9f, -1e9f, -1e9f);
		for (uint u = 0; u < pmsh->pav3Points.uLen; u++)
		{
			CVector3<> v3 = pmsh->pav3Points[u] * pins->pr3Presence();
			v3_min.tX = std::min(v3_min.tX, v3.tX); v3_min.tY = std::min(v3_min.tY, v3.tY); v3_min.tZ = std::min(v3_min.tZ, v3.tZ);
			v3_max.tX = std::max(v3_max.tX, v3.tX); v3_max.tY = std::max(v3_max.tY, v3.tY); v3_max.tZ = std::max(v3_max.tZ, v3.tZ);
		}
		const CPhysicsInfo* pphi = pins->pphiGetPhysicsInfo();
		const char* str_phys = !pphi ? "no physics" : pphi == &phiDEFAULT ? "default (none)" :
		                       pphi->pphicCast() ? "compound" : pphi->ppibCast() ? "box" : pphi->ppisCast() ? "skeleton" : "other";
		gVR.pfnLog("  %s: %s%s%s, mesh from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f), origin (%.2f, %.2f, %.2f)%s",
			pins->strGetInstanceName(), str_phys, pphi && pphi->bIsTangible() ? ", solid" : "", pphi && pphi->bIsMovable() ? ", movable" : "",
			v3_min.tX, v3_min.tY, v3_min.tZ, v3_max.tX, v3_max.tY, v3_max.tZ, pins->v3Pos().tX, pins->v3Pos().tY, pins->v3Pos().tZ,
			ptCast<CTerrainObj>(pins) ? ", ground texture" : "");
	}
}
