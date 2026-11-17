/**********************************************************************************************
 *
 * VRRender.cpp
 *
 * Site B VR: modern GPU renderer (Direct3D 11).  See VRRender.h.
 *
 * Per eye: sky (full-screen ray cast through the cloud layer), terrain (height grid textured
 * from a "terrain atlas" painted once per level from the game's terrain textures), then all
 * objects.  Reversed infinite depth, multisample anti-aliasing with alpha-to-coverage for
 * see-through textures (foliage, fences), a stabilised sun shadow map around the player, and
 * the game's fog curve applied per pixel by true distance.
 *
 **********************************************************************************************/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <array>
#include "VRRender.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	template<class T> void SafeRelease(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}

	//******************************************************************************************
	//
	// Shaders.
	//
	const char* strSHADERS = R"HLSL(
cbuffer CBEye : register(b0)
{
	float4 gRow[4];
	float4 gEyePos;
	float4 gEyeX;
	float4 gEyeY;
	float4 gEyeZ;
	float4 gTan;		// left, right, down, up
};

cbuffer CBFrame : register(b1)
{
	float4 gSun;			// xyz toward the sun, w brightness
	float4 gLight;			// x ambient, y overall brightness
	float4 gFogColour;		// rgb, w = 1 / fog distance
	float4 gFogTable[8];
	float4 gShadowRow[3];
	float4 gShadowParams;	// x texel size, y bias, z enabled
	float4 gSky0;			// height, uv scale, offset u, offset v
	float4 gSky1;			// fog near, fog scale, has texture, horizon
	float4 gSkyFog;			// rgb
	float4 gAtlas;			// x0, y0, 1 / size x, 1 / size y
	float4 gAtlasOn;		// x: atlas present
};

Texture2D              gTex        : register(t0);
Texture2D<float>       gShadow     : register(t1);
Texture2D              gAtlasTex   : register(t2);
SamplerState           gSamp       : register(s0);
SamplerComparisonState gShadowSamp : register(s1);
SamplerState           gClamp      : register(s2);
Texture2DArray         gDetail     : register(t3);
Texture2D<uint>        gIdMap      : register(t4);
StructuredBuffer<float4> gDecal    : register(t5);
Texture2D<float2>      gUvMap      : register(t6);
Texture2D              gNormalTex  : register(t7);

struct VSObjIn
{
	float3 p     : POSITION;
	float3 n     : NORMAL;
	float2 uv    : TEXCOORD0;
	float4 r0    : TEXCOORD1;
	float4 r1    : TEXCOORD2;
	float4 r2    : TEXCOORD3;
	uint   flags : TEXCOORD4;
	float2 uvoff : TEXCOORD5;		// Scrolling textures (surf, waterfalls).
};

struct PSObjIn
{
	float4 pos : SV_Position;
	float3 wp  : TEXCOORD0;
	float3 wn  : TEXCOORD1;
	float2 uv  : TEXCOORD2;
	nointerpolation uint flags : TEXCOORD3;
};

float3 WorldPos(VSObjIn i)
{
	float4 p = float4(i.p, 1);
	return float3(dot(i.r0, p), dot(i.r1, p), dot(i.r2, p));
}

float3 WorldNormal(VSObjIn i)
{
	return float3(dot(i.r0.xyz, i.n), dot(i.r1.xyz, i.n), dot(i.r2.xyz, i.n));
}

float4 ClipPos(float3 w)
{
	float4 p = float4(w, 1);
	return float4(dot(gRow[0], p), dot(gRow[1], p), dot(gRow[2], p), dot(gRow[3], p));
}

float3 ShadowPos(float3 w)
{
	float4 p = float4(w, 1);
	return float3(dot(gShadowRow[0], p), dot(gShadowRow[1], p), dot(gShadowRow[2], p));
}

// Haze: builds up gently with distance (none close by), thinner higher up, never a solid wall.
float FogAt(float3 wp)
{
	// The game's own fog curve over the fog distance, a little thinner higher up.
	float d = clamp(distance(wp, gEyePos.xyz) * gFogColour.w * 31.0, 0.0, 31.0);
	int i0 = (int)d;
	int i1 = min(i0 + 1, 31);
	float a = gFogTable[i0 >> 2][i0 & 3];
	float b = gFogTable[i1 >> 2][i1 & 3];
	float f = lerp(a, b, d - i0);
	float h = max(wp.z - gEyePos.z, 0.0);
	f *= lerp(1.0, 0.75, saturate(h / 300.0));

	// Low mist: thicker in hollows and over the sea, thinning with height, so the land seems
	// to sit in the haze rather than behind a flat curtain.
	float dist = distance(wp, gEyePos.xyz);
	float low = exp(-max(wp.z - gEyePos.z + 25.0, 0.0) / 70.0);
	float mist = 1.0 - exp(-dist / 900.0 * low);
	return 1.0 - (1.0 - f) * (1.0 - mist * 0.55);
}

// The haze colour looking along dir: the game's fog colour tinted toward the sky, bluer when
// looking up, warmer toward the sun.
float3 FogColourDir(float3 dir)
{
	float3 haze = lerp(gFogColour.rgb, float3(0.55, 0.68, 0.85), 0.35);
	float3 col = lerp(haze, haze * 0.8 + float3(0.0, 0.03, 0.1), saturate(dir.z * 3.0));
	float s = pow(saturate(dot(dir, gSun.xyz)), 6.0);
	col += float3(0.25, 0.2, 0.12) * s * gSun.w;
	return col * gLight.y;
}

float3 ApplyFog(float3 col, float3 wp)
{
	float3 dir = normalize(wp - gEyePos.xyz);
	return lerp(col, FogColourDir(dir), FogAt(wp));
}

float ShadowAt(float3 wp, float3 n)
{
	if (gShadowParams.z < 0.5)
		return 1.0;
	float3 s = ShadowPos(wp + n * 0.04);
	float2 edge = min(s.xy, 1.0 - s.xy);
	if (min(edge.x, edge.y) <= 0.0)
		return 1.0;
	float sum = 0;
	[unroll] for (int y = -1; y <= 1; y++)
		[unroll] for (int x = -1; x <= 1; x++)
			sum += gShadow.SampleCmpLevelZero(gShadowSamp, s.xy + float2(x, y) * gShadowParams.x, s.z - gShadowParams.y);
	// Fade out towards the edge of the shadowed area.
	float fade = saturate(min(edge.x, edge.y) * 12.0);
	return lerp(1.0, sum / 9.0, fade);
}

float3 Shade(float3 albedo, float3 n, float3 wp, bool unlit)
{
	if (unlit)
		return albedo * gLight.y;
	float ndl = saturate(dot(n, gSun.xyz));
	float sh = ndl > 0.0 ? ShadowAt(wp, n) : 1.0;
	return albedo * (gLight.x + gSun.w * ndl * sh) * gLight.y;
}

PSObjIn VSObject(VSObjIn i)
{
	PSObjIn o;
	o.wp = WorldPos(i);
	o.wn = WorldNormal(i);
	o.uv = i.uv + i.uvoff;
	o.flags = i.flags;
	o.pos = ClipPos(o.wp);
	return o;
}

float4 PSObject(PSObjIn i, bool front : SV_IsFrontFace) : SV_Target
{
	float4 c = gTex.Sample(gSamp, i.uv);
	if (i.flags & 1)
	{
		// Alpha to coverage, sharpened so edges stay crisp at a distance.
		c.a = saturate((c.a - 0.5) / max(fwidth(c.a), 0.0001) + 0.5);
		clip(c.a - 0.004);
	}
	else if (!(i.flags & 32))
		c.a = 1.0;
	// The shading normal is turned to the side of the surface you are looking at (the true
	// face direction decides, so smooth normals near outlines are left alone).  Fixes dark
	// patches on models whose normals point inward or that are seen from behind.
	float3 n = normalize(i.wn);
	float3 dpx = ddx(i.wp), dpy = ddy(i.wp);
	float3 gn = cross(dpx, dpy);
	float3 to_eye = gEyePos.xyz - i.wp;
	if (dot(gn, to_eye) < 0)
		gn = -gn;
	if (dot(n, gn) < 0)
		n = -n;
	if (i.flags & 128)
	{
		// Bump map: the texel's tilt, in the surface's texture directions (from how the
		// texture coordinates change across the screen).
		float3 tn = gNormalTex.Sample(gSamp, i.uv).xyz * 2.0 - 1.0;
		float2 duvx = ddx(i.uv), duvy = ddy(i.uv);
		float3 t = dpx * duvy.y - dpy * duvx.y;
		float3 b = dpy * duvx.x - dpx * duvy.x;
		float det = duvx.x * duvy.y - duvx.y * duvy.x;
		if (abs(det) > 1e-12 && dot(t, t) > 1e-20 && dot(b, b) > 1e-20)
		{
			t = normalize(t - n * dot(n, t)) * sign(det);
			b = normalize(b - n * dot(n, b)) * sign(det);
			n = normalize(t * tn.x + b * tn.y + n * max(tn.z, 0.2));
		}
	}
	float3 col = Shade(c.rgb, n, i.wp, (i.flags & 8) != 0);
	if (i.flags & 64)
	{
		// Water: clearer looking straight down, mirror-like toward the horizon, with the sun's
		// glint.  Small moving ripples break up the surface.
		float3 v = normalize(to_eye);
		float t = gAtlasOn.z;
		float2 q = i.wp.xy * 0.9;
		float3 wn = normalize(float3(0.06 * sin(q.x * 1.7 + t * 1.3) + 0.04 * sin(q.y * 2.3 - t * 1.1),
		                             0.06 * cos(q.y * 1.9 + t * 1.2) + 0.04 * cos(q.x * 2.1 - t * 0.9), 1.0));
		float fres = pow(1.0 - saturate(dot(wn, v)), 4.0);
		float3 r = reflect(-v, wn);
		float spec = pow(saturate(dot(r, gSun.xyz)), 200.0) * gSun.w * 2.0;
		// The sky it reflects: blue overhead blending to the haze colour at the horizon.
		float3 sky = lerp(gSkyFog.rgb, float3(0.32, 0.52, 0.78), saturate(r.z * 2.5)) * gLight.y;
		col = lerp(col, sky, saturate(fres * 0.6)) + spec;
		c.a = saturate(0.6 + fres * 0.3);
	}
	if (!(i.flags & 16))
		col = ApplyFog(col, i.wp);
	return float4(col, c.a);
}

struct VSTerIn
{
	float3 p : POSITION;
	float3 n : NORMAL;
	float4 c : COLOR;
};

struct PSTerIn
{
	float4 pos : SV_Position;
	float3 wp  : TEXCOORD0;
	float3 wn  : TEXCOORD1;
	float4 c   : COLOR;
};

PSTerIn VSTerrain(VSTerIn i)
{
	PSTerIn o;
	o.wp = i.p;
	o.wn = i.n;
	o.c = i.c;
	o.pos = ClipPos(i.p);
	return o;
}

// The terrain's own textures at full resolution: which texture covers each spot comes from
// the id map, and each texture is laid on the ground by its own mapping, tiled.  The four
// nearest map texels are blended so borders between textures stay soft.
float3 TerrainDetail(float3 wp, float3 atlas_col)
{
	uint w, h;
	gIdMap.GetDimensions(w, h);
	float2 t = (wp.xy - gAtlas.xy) * gAtlas.zw * float2(w, h) - 0.5;
	int2 b = (int2)floor(t);
	float2 f = t - b;
	float2 dx = ddx(wp.xy), dy = ddy(wp.xy);

	// The four nearest map texels: which texture, its stored coordinates, and their weights.
	uint ids[4];
	float2 uvs[4];
	float wts[4];
	[unroll] for (int j = 0; j < 4; j++)
	{
		int2 c = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(w - 1, h - 1));
		ids[j] = gIdMap.Load(int3(c, 0));
		uvs[j] = gUvMap.Load(int3(c, 0));
		wts[j] = ((j & 1) ? f.x : 1.0 - f.x) * ((j >> 1) ? f.y : 1.0 - f.y);
	}

	float3 sum = 0;
	[unroll] for (int k = 0; k < 4; k++)
	{
		uint id = ids[k];
		uint idc = id < 0xFFFE ? id : 0;
		float4 m0 = gDecal[idc * 2], m1 = gDecal[idc * 2 + 1];

		// Flat-projected textures: coordinates straight from the position.
		float2 uv_a = float2(dot(m0.xy, wp.xy) + m0.z, dot(m1.xy, wp.xy) + m1.z);
		float2 gx_a = float2(dot(m0.xy, dx), dot(m1.xy, dx));
		float2 gy_a = float2(dot(m0.xy, dy), dot(m1.xy, dy));

		// Textures that follow a shape (roads, paths): coordinates stored in the map, blended
		// between the texels that belong to the same texture.
		float2 uvp = 0;
		float ws = 0;
		[unroll] for (int q = 0; q < 4; q++)
		{
			float m = ids[q] == id ? wts[q] : 0.0;
			uvp += uvs[q] * m;
			ws += m;
		}
		float2 uv_m = uvp / max(ws, 1e-5);
		float2 gx_m = ddx(uv_m), gy_m = ddy(uv_m);

		float3 col = atlas_col;
		if (id < 0xFFFE)
		{
			bool b_map = m1.w > 0.5;
			float2 uv = b_map ? uv_m : uv_a;
			float2 gx = b_map ? gx_m : gx_a;
			float2 gy = b_map ? gy_m : gy_a;
			// See-through parts of a terrain texture show what was painted underneath.
			float4 tx = gDetail.SampleGrad(gSamp, float3(uv, m0.w), gx, gy);
			col = lerp(atlas_col, tx.rgb, tx.a);
		}
		sum += col * wts[k];
	}
	return sum;
}

float4 PSTerrain(PSTerIn i) : SV_Target
{
	float3 albedo = i.c.rgb;
	if (gAtlasOn.x > 0.5)
		albedo = gAtlasTex.Sample(gSamp, (i.wp.xy - gAtlas.xy) * gAtlas.zw).rgb;
	if (gAtlasOn.y > 0.5)
		albedo = TerrainDetail(i.wp, albedo);
	float3 col = Shade(albedo, normalize(i.wn), i.wp, false);
	col = ApplyFog(col, i.wp);
	return float4(col, 1);
}

// Terrain atlas: painted from above.
float4 AtlasPos(float3 w)
{
	float2 t = (w.xy - gAtlas.xy) * gAtlas.zw;
	return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.5, 1.0);
}

struct PSAtlasIn
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
	float4 c   : COLOR;
};

PSAtlasIn VSAtlasTerrain(VSTerIn i)
{
	PSAtlasIn o;
	o.pos = AtlasPos(i.p);
	o.uv = float2(0, 0);
	o.c = i.c;
	return o;
}

float4 PSAtlasTerrain(PSAtlasIn i) : SV_Target
{
	return float4(i.c.rgb, 1);
}

PSAtlasIn VSAtlasDecal(VSObjIn i)
{
	PSAtlasIn o;
	o.pos = AtlasPos(WorldPos(i));
	o.uv = i.uv;
	o.c = float4(1, 1, 1, 1);
	return o;
}

float4 PSAtlasDecal(PSAtlasIn i) : SV_Target
{
	float4 c = gTex.Sample(gSamp, i.uv);
	clip(c.a - 0.5);
	return float4(c.rgb, 1);
}

// Terrain id map: which terrain texture is on top at each spot.
struct PSIdIn
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
	nointerpolation uint id : TEXCOORD1;
};

PSIdIn VSIdDecal(VSObjIn i)
{
	PSIdIn o;
	o.pos = AtlasPos(WorldPos(i));
	o.uv = i.uv;
	o.id = i.flags;
	return o;
}

struct PSIdOut
{
	uint   id : SV_Target0;
	float2 uv : SV_Target1;
};

PSIdOut PSIdDecal(PSIdIn i)
{
	clip(gTex.Sample(gSamp, i.uv).a - 0.5);
	PSIdOut o;
	o.id = i.id;
	o.uv = i.uv;
	return o;
}

// Copies a texture into a slice of the terrain texture array (resized).
struct PSBlitIn
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

PSBlitIn VSBlit(uint id : SV_VertexID)
{
	PSBlitIn o;
	float2 t = float2((id << 1) & 2, id & 2);
	o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = t;
	return o;
}

float4 PSBlit(PSBlitIn i) : SV_Target
{
	return gTex.Sample(gSamp, i.uv);
}

// Sky: a ray through each pixel to the cloud layer.
struct PSSkyIn
{
	float4 pos : SV_Position;
	float2 ndc : TEXCOORD0;
};

PSSkyIn VSSky(uint id : SV_VertexID)
{
	PSSkyIn o;
	float2 t = float2((id << 1) & 2, id & 2);
	o.ndc = t * float2(2, 2) - float2(1, 1);
	o.pos = float4(o.ndc, 0, 1);
	return o;
}

float4 PSSky(PSSkyIn i) : SV_Target
{
	float tx = lerp(gTan.x, gTan.y, i.ndc.x * 0.5 + 0.5);
	float tz = lerp(gTan.z, gTan.w, i.ndc.y * 0.5 + 0.5);
	float3 d = normalize(gEyeY.xyz + gEyeX.xyz * tx + gEyeZ.xyz * tz);
	float3 col = FogColourDir(d);
	if (d.z > gSky1.w && gSky1.z > 0.5)
	{
		float t = gSky0.x / d.z;
		float2 uv = (gEyePos.xy + d.xy * t) * gSky0.y + gSky0.zw;
		float3 c = gTex.SampleGrad(gSamp, uv, ddx(uv), ddy(uv)).rgb;
		float f = saturate((1.0 - d.z - gSky1.x) * gSky1.y);
		// The clouds fade into the same haze the land does, so the horizon has no seam.
		col = lerp(c, FogColourDir(d), f);
	}
	return float4(col, 1);
}

// Shadow map.
struct PSShadowIn
{
	float4 pos : SV_Position;
	float2 uv  : TEXCOORD0;
};

float4 ShadowClip(float3 w)
{
	float3 s = ShadowPos(w);
	return float4(s.x * 2.0 - 1.0, 1.0 - s.y * 2.0, s.z, 1.0);
}

PSShadowIn VSShadowObject(VSObjIn i)
{
	PSShadowIn o;
	o.pos = ShadowClip(WorldPos(i));
	o.uv = i.uv;
	return o;
}

void PSShadowAlpha(PSShadowIn i)
{
	clip(gTex.Sample(gSamp, i.uv).a - 0.5);
}

PSShadowIn VSShadowTerrain(VSTerIn i)
{
	PSShadowIn o;
	o.pos = ShadowClip(i.p);
	o.uv = float2(0, 0);
	return o;
}
)HLSL";

	//******************************************************************************************
	struct SEyeCB
	{
		float afRow[4][4];
		float afEyePos[4];
		float afEyeX[4];
		float afEyeY[4];
		float afEyeZ[4];
		float afTan[4];
	};

	struct SFrameCB
	{
		float afSun[4];
		float afLight[4];
		float afFogColour[4];
		float afFogTable[32];
		float afShadowRow[3][4];
		float afShadowParams[4];
		float afSky0[4];
		float afSky1[4];
		float afSkyFog[4];
		float afAtlas[4];
		float afAtlasOn[4];
	};

	struct SInstance
	{
		float    afRow[12];
		uint32_t u4Flags;
		float    afUV[2];
	};

	struct STexture
	{
		ID3D11Texture2D*          ptex = nullptr;
		ID3D11ShaderResourceView* psrv = nullptr;
		int                       iBytes = 0;
	};

	struct SMesh
	{
		ID3D11Buffer* pvb = nullptr;
		ID3D11Buffer* pib = nullptr;
	};

	struct SDynMesh
	{
		int iBaseVertex;
		int iFirstIndex;
	};

	const int iDYNAMIC_BASE = 1 << 24;

	//******************************************************************************************
	//
	// State.
	//
	ID3D11Device*           pDev = nullptr;
	ID3D11DeviceContext*    pCtx = nullptr;
	bool                    bReady = false;
	int                     iWidth = 0, iHeight = 0, iMSAA = 1, iShadowSize = 0;

	// Targets.
	ID3D11Texture2D*        ptexColour = nullptr;	// MSAA colour.
	ID3D11RenderTargetView* prtvColour = nullptr;
	ID3D11Texture2D*        ptexDepth = nullptr;
	ID3D11DepthStencilView* pdsvDepth = nullptr;
	ID3D11Texture2D*        aptexEye[2] = { nullptr, nullptr };	// Resolved eye images.
	ID3D11RenderTargetView* aprtvEye[2] = { nullptr, nullptr };

	ID3D11Texture2D*          ptexShadow = nullptr;
	ID3D11DepthStencilView*   pdsvShadow = nullptr;
	ID3D11ShaderResourceView* psrvShadow = nullptr;

	// Shaders.
	ID3D11VertexShader* pvsObject = nullptr;
	ID3D11PixelShader*  ppsObject = nullptr;
	ID3D11VertexShader* pvsTerrain = nullptr;
	ID3D11PixelShader*  ppsTerrain = nullptr;
	ID3D11VertexShader* pvsAtlasTerrain = nullptr;
	ID3D11PixelShader*  ppsAtlasTerrain = nullptr;
	ID3D11VertexShader* pvsAtlasDecal = nullptr;
	ID3D11PixelShader*  ppsAtlasDecal = nullptr;
	ID3D11VertexShader* pvsSky = nullptr;
	ID3D11PixelShader*  ppsSky = nullptr;
	ID3D11VertexShader* pvsShadowObject = nullptr;
	ID3D11PixelShader*  ppsShadowAlpha = nullptr;
	ID3D11VertexShader* pvsShadowTerrain = nullptr;
	ID3D11InputLayout*  pilObject = nullptr;
	ID3D11InputLayout*  pilTerrain = nullptr;

	// States.
	ID3D11RasterizerState*   prsBack = nullptr;
	ID3D11RasterizerState*   prsNone = nullptr;
	ID3D11RasterizerState*   prsShadow = nullptr;
	ID3D11RasterizerState*   prsAtlas = nullptr;
	ID3D11DepthStencilState* pdsTest = nullptr;
	ID3D11DepthStencilState* pdsOff = nullptr;
	ID3D11DepthStencilState* pdsShadow = nullptr;
	ID3D11DepthStencilState* pdsTestNoWrite = nullptr;	// Blended surfaces.
	ID3D11BlendState*        pbsBlend = nullptr;
	ID3D11DepthStencilState* pdsFine = nullptr;		// Fine terrain: marks the stencil.
	ID3D11DepthStencilState* pdsCoarse = nullptr;	// Coarse terrain: only where fine isn't.
	ID3D11BlendState*        pbsOpaque = nullptr;
	ID3D11BlendState*        pbsA2C = nullptr;
	ID3D11SamplerState*      pssWrap = nullptr;
	ID3D11SamplerState*      pssShadow = nullptr;
	ID3D11SamplerState*      pssClamp = nullptr;
	ID3D11Buffer*            pcbEye = nullptr;
	ID3D11Buffer*            pcbFrame = nullptr;

	// Resources.
	std::vector<STexture>   atex;			// Handle = index + 1.
	STexture                texWhite;
	std::vector<SMesh>      amesh;			// Handle = index + 1.

	std::vector<SVRRVertex> avDyn;
	std::vector<uint32_t>   au4DynIdx;
	std::vector<SDynMesh>   adynmesh;
	ID3D11Buffer*           pvbDyn = nullptr;
	ID3D11Buffer*           pibDyn = nullptr;
	int                     iDynVBCap = 0, iDynIBCap = 0;

	std::vector<SVRRDraw>   adrw;
	std::vector<int>        aiOrder;
	ID3D11Buffer*           pvbInst = nullptr;
	int                     iInstCap = 0;

	// Terrain.
	ID3D11Buffer*             pvbTerrain = nullptr;

	// Fine terrain patches near the player (square grids of iPatchN x iPatchN vertices).
	static const int          iMAX_PATCHES = 128;
	ID3D11Buffer*             apvbPatch[iMAX_PATCHES] = {};
	ID3D11Buffer*             pibPatch = nullptr;
	int                       iPatchN = 0, iPatchIndices = 0;
	ID3D11Buffer*             pibTerrain = nullptr;
	int                       iTerrainIndices = 0;
	float                     afAtlas[4] = { 0, 0, 0, 0 };
	int                       iAtlasSize = 0;
	ID3D11Texture2D*          ptexAtlas = nullptr;
	ID3D11ShaderResourceView* psrvAtlas = nullptr;
	ID3D11RenderTargetView*   prtvAtlas = nullptr;
	bool                      bAtlasBuilt = false;
	std::vector<SVRRDraw>     adrwDecal;
	std::vector<std::array<float, 6>> aafDecalMap;	// Texture mapping of each decal (u, v from world x, y).
	std::vector<bool>         abDecalMap;			// Whether the mapping is usable.

	// Terrain textures at full resolution.
	ID3D11Texture2D*          ptexDetail = nullptr;	// Array: one slice per terrain texture.
	ID3D11ShaderResourceView* psrvDetail = nullptr;
	ID3D11Texture2D*          ptexIdMap = nullptr;
	ID3D11ShaderResourceView* psrvIdMap = nullptr;
	ID3D11Texture2D*          ptexUvMap = nullptr;		// Texture coordinates, for shape-following terrain textures.
	ID3D11ShaderResourceView* psrvUvMap = nullptr;
	ID3D11Buffer*             pbufDecal = nullptr;
	ID3D11ShaderResourceView* psrvDecal = nullptr;
	bool                      bDetailBuilt = false;
	int                       iDetailSlices = 0, iDetailSize = 0, iIdMapSize = 0;

	ID3D11VertexShader*       pvsIdDecal = nullptr;
	ID3D11PixelShader*        ppsIdDecal = nullptr;
	ID3D11VertexShader*       pvsBlit = nullptr;
	ID3D11PixelShader*        ppsBlit = nullptr;

	void ReleaseDetail()
	{
		SafeRelease(psrvDetail);
		SafeRelease(ptexDetail);
		SafeRelease(psrvIdMap);
		SafeRelease(ptexIdMap);
		SafeRelease(psrvUvMap);
		SafeRelease(ptexUvMap);
		SafeRelease(psrvDecal);
		SafeRelease(pbufDecal);
		bDetailBuilt = false;
		iDetailSlices = iDetailSize = iIdMapSize = 0;
	}

	int iLastDraws = 0;
	int iLastBatches = 0, iBatchesShown = 0;	// Card draw calls for the objects (all passes).

	ID3D11Texture2D*        ptexCapture = nullptr;		// Staging copy of the left eye.
	bool                    bCapturePending = false;

	//******************************************************************************************
	void Err(char* str_err, int i_len, const char* str)
	{
		if (str_err && i_len > 0)
		{
			std::strncpy(str_err, str, i_len - 1);
			str_err[i_len - 1] = 0;
		}
	}

	//******************************************************************************************
	ID3DBlob* pblobCompile(const char* str_entry, const char* str_target, std::string& str_err)
	{
		ID3DBlob* pblob = nullptr;
		ID3DBlob* pblob_err = nullptr;
		HRESULT hr = D3DCompile(strSHADERS, std::strlen(strSHADERS), "SiteBVR", nullptr, nullptr, str_entry, str_target,
		                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pblob, &pblob_err);
		if (FAILED(hr))
		{
			str_err = std::string("shader ") + str_entry + ": ";
			if (pblob_err)
				str_err += std::string((const char*)pblob_err->GetBufferPointer(), pblob_err->GetBufferSize());
			SafeRelease(pblob_err);
			SafeRelease(pblob);
			return nullptr;
		}
		SafeRelease(pblob_err);
		return pblob;
	}

	bool bVS(const char* str_entry, ID3D11VertexShader*& pvs, std::string& str_err, ID3DBlob** ppblob_keep = nullptr)
	{
		ID3DBlob* pblob = pblobCompile(str_entry, "vs_5_0", str_err);
		if (!pblob)
			return false;
		HRESULT hr = pDev->CreateVertexShader(pblob->GetBufferPointer(), pblob->GetBufferSize(), nullptr, &pvs);
		if (ppblob_keep)
			*ppblob_keep = pblob;
		else
			pblob->Release();
		return SUCCEEDED(hr);
	}

	bool bPS(const char* str_entry, ID3D11PixelShader*& pps, std::string& str_err)
	{
		ID3DBlob* pblob = pblobCompile(str_entry, "ps_5_0", str_err);
		if (!pblob)
			return false;
		HRESULT hr = pDev->CreatePixelShader(pblob->GetBufferPointer(), pblob->GetBufferSize(), nullptr, &pps);
		pblob->Release();
		return SUCCEEDED(hr);
	}

	ID3D11Buffer* pbufCreate(UINT u_bytes, UINT u_bind, bool b_dynamic, const void* pv_init)
	{
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = (u_bytes + 15) & ~15u;
		bd.BindFlags = u_bind;
		bd.Usage = b_dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
		bd.CPUAccessFlags = b_dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
		D3D11_SUBRESOURCE_DATA sd = {};
		sd.pSysMem = pv_init;
		ID3D11Buffer* pbuf = nullptr;
		pDev->CreateBuffer(&bd, pv_init ? &sd : nullptr, &pbuf);
		return pbuf;
	}

	bool bUpload(ID3D11Buffer* pbuf, const void* pv, size_t u_bytes)
	{
		D3D11_MAPPED_SUBRESOURCE ms;
		if (FAILED(pCtx->Map(pbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			return false;
		std::memcpy(ms.pData, pv, u_bytes);
		pCtx->Unmap(pbuf, 0);
		return true;
	}

	//******************************************************************************************
	void ReleaseTargets()
	{
		SafeRelease(prtvColour);
		SafeRelease(ptexColour);
		SafeRelease(pdsvDepth);
		SafeRelease(ptexDepth);
		for (int i = 0; i < 2; i++)
		{
			SafeRelease(aprtvEye[i]);
			SafeRelease(aptexEye[i]);
		}
		SafeRelease(psrvShadow);
		SafeRelease(pdsvShadow);
		SafeRelease(ptexShadow);
	}

	bool bCreateTargets(std::string& str_err)
	{
		// Eye images (what is submitted to SteamVR).
		for (int i = 0; i < 2; i++)
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = iWidth;
			td.Height = iHeight;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(pDev->CreateTexture2D(&td, nullptr, &aptexEye[i])) ||
				FAILED(pDev->CreateRenderTargetView(aptexEye[i], nullptr, &aprtvEye[i])))
			{
				str_err = "could not create the eye images";
				return false;
			}
		}

		// Multisampled colour and depth.
		UINT u_quality = 0;
		while (iMSAA > 1 && (FAILED(pDev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, iMSAA, &u_quality)) || u_quality == 0))
			iMSAA /= 2;
		if (iMSAA > 1)
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = iWidth;
			td.Height = iHeight;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = iMSAA;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			if (FAILED(pDev->CreateTexture2D(&td, nullptr, &ptexColour)) ||
				FAILED(pDev->CreateRenderTargetView(ptexColour, nullptr, &prtvColour)))
			{
				str_err = "could not create the anti-aliasing buffer";
				return false;
			}
		}
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = iWidth;
			td.Height = iHeight;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;		// Stencil: fine terrain covers coarse.
			td.SampleDesc.Count = iMSAA;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(pDev->CreateTexture2D(&td, nullptr, &ptexDepth)) ||
				FAILED(pDev->CreateDepthStencilView(ptexDepth, nullptr, &pdsvDepth)))
			{
				str_err = "could not create the depth buffer";
				return false;
			}
		}

		// Shadow map.
		if (iShadowSize > 0)
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = iShadowSize;
			td.Height = iShadowSize;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32_TYPELESS;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
			dd.Format = DXGI_FORMAT_D32_FLOAT;
			dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = DXGI_FORMAT_R32_FLOAT;
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			if (FAILED(pDev->CreateTexture2D(&td, nullptr, &ptexShadow)) ||
				FAILED(pDev->CreateDepthStencilView(ptexShadow, &dd, &pdsvShadow)) ||
				FAILED(pDev->CreateShaderResourceView(ptexShadow, &sd, &psrvShadow)))
			{
				SafeRelease(psrvShadow);
				SafeRelease(pdsvShadow);
				SafeRelease(ptexShadow);
				iShadowSize = 0;
			}
		}
		return true;
	}

	//******************************************************************************************
	bool bCreateStates()
	{
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_BACK;
		rd.FrontCounterClockwise = TRUE;
		rd.DepthClipEnable = TRUE;
		rd.MultisampleEnable = TRUE;
		rd.AntialiasedLineEnable = FALSE;
		pDev->CreateRasterizerState(&rd, &prsBack);
		rd.CullMode = D3D11_CULL_NONE;
		pDev->CreateRasterizerState(&rd, &prsNone);
		rd.MultisampleEnable = FALSE;
		pDev->CreateRasterizerState(&rd, &prsAtlas);
		rd.DepthBias = 200;
		rd.SlopeScaledDepthBias = 2.0f;
		rd.DepthBiasClamp = 0.01f;
		pDev->CreateRasterizerState(&rd, &prsShadow);

		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = TRUE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dd.DepthFunc = D3D11_COMPARISON_GREATER;		// Reversed depth.
		pDev->CreateDepthStencilState(&dd, &pdsTest);
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		pDev->CreateDepthStencilState(&dd, &pdsTestNoWrite);
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dd.DepthFunc = D3D11_COMPARISON_LESS;
		pDev->CreateDepthStencilState(&dd, &pdsShadow);
		dd.DepthFunc = D3D11_COMPARISON_GREATER;
		dd.StencilEnable = TRUE;
		dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
		dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		dd.BackFace = dd.FrontFace;
		pDev->CreateDepthStencilState(&dd, &pdsFine);
		dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		dd.FrontFace.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
		dd.BackFace = dd.FrontFace;
		pDev->CreateDepthStencilState(&dd, &pdsCoarse);
		dd.StencilEnable = FALSE;
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		pDev->CreateDepthStencilState(&dd, &pdsOff);

		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = FALSE;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		pDev->CreateBlendState(&bd, &pbsOpaque);
		bd.AlphaToCoverageEnable = TRUE;
		pDev->CreateBlendState(&bd, &pbsA2C);
		{
			D3D11_BLEND_DESC bdb = {};
			bdb.RenderTarget[0].BlendEnable = TRUE;
			bdb.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			bdb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bdb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bdb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			bdb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			bdb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bdb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			pDev->CreateBlendState(&bdb, &pbsBlend);
		}

		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_ANISOTROPIC;
		sd.MaxAnisotropy = 16;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		pDev->CreateSamplerState(&sd, &pssWrap);
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		pDev->CreateSamplerState(&sd, &pssClamp);
		sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
		pDev->CreateSamplerState(&sd, &pssShadow);

		pcbEye = pbufCreate(sizeof(SEyeCB), D3D11_BIND_CONSTANT_BUFFER, true, nullptr);
		pcbFrame = pbufCreate(sizeof(SFrameCB), D3D11_BIND_CONSTANT_BUFFER, true, nullptr);

		uint32_t u4_white = 0xFFFFFFFF;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = td.Height = 1;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA srd = { &u4_white, 4, 0 };
		pDev->CreateTexture2D(&td, &srd, &texWhite.ptex);
		if (texWhite.ptex)
			pDev->CreateShaderResourceView(texWhite.ptex, nullptr, &texWhite.psrv);

		return prsBack && prsNone && prsShadow && prsAtlas && pdsTest && pdsOff && pdsShadow && pbsOpaque && pbsA2C &&
		       pssWrap && pssClamp && pssShadow && pcbEye && pcbFrame && texWhite.psrv;
	}

	//******************************************************************************************
	bool bCreateShaders(std::string& str_err)
	{
		ID3DBlob* pblob_obj = nullptr;
		ID3DBlob* pblob_ter = nullptr;
		bool b_ok =
			bVS("VSObject", pvsObject, str_err, &pblob_obj) &&
			bPS("PSObject", ppsObject, str_err) &&
			bVS("VSTerrain", pvsTerrain, str_err, &pblob_ter) &&
			bPS("PSTerrain", ppsTerrain, str_err) &&
			bVS("VSAtlasTerrain", pvsAtlasTerrain, str_err) &&
			bPS("PSAtlasTerrain", ppsAtlasTerrain, str_err) &&
			bVS("VSAtlasDecal", pvsAtlasDecal, str_err) &&
			bPS("PSAtlasDecal", ppsAtlasDecal, str_err) &&
			bVS("VSIdDecal", pvsIdDecal, str_err) &&
			bPS("PSIdDecal", ppsIdDecal, str_err) &&
			bVS("VSBlit", pvsBlit, str_err) &&
			bPS("PSBlit", ppsBlit, str_err) &&
			bVS("VSSky", pvsSky, str_err) &&
			bPS("PSSky", ppsSky, str_err) &&
			bVS("VSShadowObject", pvsShadowObject, str_err) &&
			bPS("PSShadowAlpha", ppsShadowAlpha, str_err) &&
			bVS("VSShadowTerrain", pvsShadowTerrain, str_err);

		if (b_ok)
		{
			D3D11_INPUT_ELEMENT_DESC aied_obj[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 4, DXGI_FORMAT_R32_UINT,           1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 5, DXGI_FORMAT_R32G32_FLOAT,       1, 52, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			};
			D3D11_INPUT_ELEMENT_DESC aied_ter[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			b_ok = SUCCEEDED(pDev->CreateInputLayout(aied_obj, 8, pblob_obj->GetBufferPointer(), pblob_obj->GetBufferSize(), &pilObject)) &&
			       SUCCEEDED(pDev->CreateInputLayout(aied_ter, 3, pblob_ter->GetBufferPointer(), pblob_ter->GetBufferSize(), &pilTerrain));
			if (!b_ok)
				str_err = "could not create the vertex layouts";
		}
		SafeRelease(pblob_obj);
		SafeRelease(pblob_ter);
		return b_ok;
	}

	//******************************************************************************************
	ID3D11ShaderResourceView* psrvTex(int i_handle)
	{
		if (i_handle > 0 && i_handle <= (int)atex.size() && atex[i_handle - 1].psrv)
			return atex[i_handle - 1].psrv;
		return texWhite.psrv;
	}

	// Binds a mesh's buffers; returns the base vertex and first index offsets.
	bool bBindMesh(int i_mesh, int& ri_base_vertex, int& ri_first_index)
	{
		UINT u_stride = sizeof(SVRRVertex), u_offset = 0;
		if (i_mesh >= iDYNAMIC_BASE)
		{
			int i = i_mesh - iDYNAMIC_BASE;
			if (i < 0 || i >= (int)adynmesh.size() || !pvbDyn || !pibDyn)
				return false;
			pCtx->IASetVertexBuffers(0, 1, &pvbDyn, &u_stride, &u_offset);
			pCtx->IASetIndexBuffer(pibDyn, DXGI_FORMAT_R32_UINT, 0);
			ri_base_vertex = adynmesh[i].iBaseVertex;
			ri_first_index = adynmesh[i].iFirstIndex;
			return true;
		}
		if (i_mesh <= 0 || i_mesh > (int)amesh.size() || !amesh[i_mesh - 1].pvb)
			return false;
		pCtx->IASetVertexBuffers(0, 1, &amesh[i_mesh - 1].pvb, &u_stride, &u_offset);
		pCtx->IASetIndexBuffer(amesh[i_mesh - 1].pib, DXGI_FORMAT_R32_UINT, 0);
		ri_base_vertex = 0;
		ri_first_index = 0;
		return true;
	}

	//******************************************************************************************
	// Draws the queued objects with the currently bound shaders.  Instance i of the instance
	// buffer belongs to draw i.
	void DrawObjects(bool b_shadow)
	{
		UINT u_stride = sizeof(SInstance), u_offset = 0;
		pCtx->IASetVertexBuffers(1, 1, &pvbInst, &u_stride, &u_offset);
		pCtx->IASetInputLayout(pilObject);
		pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		int i_bound_mesh = -1, i_bound_tex = -1, i_bound_ntex = -1, i_base = 0, i_first = 0;
		int i_state = -1;
		int i_count = (int)aiOrder.size();
		for (int k = 0; k < i_count; )
		{
			const SVRRDraw& drw = adrw[aiOrder[k]];
			// How many following draws are the same thing in other places.
			const uint32_t u4_run_key = evrrALPHA_TEST | evrrTWO_SIDED | evrrBLEND | evrrNO_SHADOW;
			int i_run = 1;
			if (!(drw.u4Flags & evrrBLEND))
			{
				while (k + i_run < i_count)
				{
					const SVRRDraw& d2 = adrw[aiOrder[k + i_run]];
					if (d2.iMesh != drw.iMesh || d2.iFirstIndex != drw.iFirstIndex || d2.iNumIndices != drw.iNumIndices ||
					    d2.iTexture != drw.iTexture || d2.iNormalTex != drw.iNormalTex ||
					    (d2.u4Flags & u4_run_key) != (drw.u4Flags & u4_run_key))
						break;
					i_run++;
				}
			}
			int i = k;
			k += i_run;
			if (b_shadow && (drw.u4Flags & evrrNO_SHADOW))
				continue;

			if (drw.iMesh != i_bound_mesh)
			{
				if (!bBindMesh(drw.iMesh, i_base, i_first))
					continue;
				i_bound_mesh = drw.iMesh;
			}

			bool b_blend = (drw.u4Flags & evrrBLEND) != 0;
			if (b_shadow && b_blend)
				continue;
			bool b_alpha = (drw.u4Flags & evrrALPHA_TEST) != 0;
			bool b_two = (drw.u4Flags & evrrTWO_SIDED) != 0;
			int i_new_state = (b_alpha ? 1 : 0) | (b_two ? 2 : 0) | (b_blend ? 4 : 0);
			if (i_new_state != i_state)
			{
				i_state = i_new_state;
				if (b_shadow)
					pCtx->PSSetShader(b_alpha ? ppsShadowAlpha : nullptr, nullptr, 0);
				else
				{
					pCtx->RSSetState(b_two ? prsNone : prsBack);
					if (b_blend)
					{
						pCtx->OMSetBlendState(pbsBlend, nullptr, 0xFFFFFFFF);
						pCtx->OMSetDepthStencilState(pdsTestNoWrite, 0);
					}
					else
					{
						pCtx->OMSetBlendState(b_alpha && iMSAA > 1 ? pbsA2C : pbsOpaque, nullptr, 0xFFFFFFFF);
						pCtx->OMSetDepthStencilState(pdsTest, 0);
					}
				}
			}

			if (drw.iTexture != i_bound_tex && (!b_shadow || b_alpha))
			{
				ID3D11ShaderResourceView* psrv = psrvTex(drw.iTexture);
				pCtx->PSSetShaderResources(0, 1, &psrv);
				i_bound_tex = drw.iTexture;
			}
			if (!b_shadow && drw.iNormalTex && drw.iNormalTex != i_bound_ntex)
			{
				ID3D11ShaderResourceView* psrv_n = psrvTex(drw.iNormalTex);
				pCtx->PSSetShaderResources(7, 1, &psrv_n);
				i_bound_ntex = drw.iNormalTex;
			}

			pCtx->DrawIndexedInstanced(drw.iNumIndices, i_run, i_first + drw.iFirstIndex, i_base, i);
			iLastBatches++;
		}
	}

	bool bAnyPatch()
	{
		for (int i = 0; i < iMAX_PATCHES; i++)
			if (apvbPatch[i])
				return true;
		return false;
	}

	void DrawPatches()
	{
		if (!pibPatch)
			return;
		UINT u_stride = sizeof(SVRRTerrainVertex), u_offset = 0;
		pCtx->IASetIndexBuffer(pibPatch, DXGI_FORMAT_R32_UINT, 0);
		pCtx->IASetInputLayout(pilTerrain);
		pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (int i = 0; i < iMAX_PATCHES; i++)
		{
			if (!apvbPatch[i])
				continue;
			pCtx->IASetVertexBuffers(0, 1, &apvbPatch[i], &u_stride, &u_offset);
			pCtx->DrawIndexed(iPatchIndices, 0, 0);
		}
	}

	void ReleasePatches()
	{
		for (int i = 0; i < iMAX_PATCHES; i++)
			SafeRelease(apvbPatch[i]);
		SafeRelease(pibPatch);
		iPatchN = iPatchIndices = 0;
	}

	void DrawTerrain()
	{
		if (!pvbTerrain)
			return;
		UINT u_stride = sizeof(SVRRTerrainVertex), u_offset = 0;
		pCtx->IASetVertexBuffers(0, 1, &pvbTerrain, &u_stride, &u_offset);
		pCtx->IASetIndexBuffer(pibTerrain, DXGI_FORMAT_R32_UINT, 0);
		pCtx->IASetInputLayout(pilTerrain);
		pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		pCtx->DrawIndexed(iTerrainIndices, 0, 0);
	}

	//******************************************************************************************
	void SetEye(const SVRRFrame& fr, int i_eye)
	{
		const float* e  = fr.afEyePos[i_eye];
		const float* ex = &fr.afEyeAxes[i_eye][0];
		const float* ey = &fr.afEyeAxes[i_eye][3];
		const float* ez = &fr.afEyeAxes[i_eye][6];
		float l = fr.afEyeTan[i_eye][0], r = fr.afEyeTan[i_eye][1];
		float d = fr.afEyeTan[i_eye][2], u = fr.afEyeTan[i_eye][3];
		float sx = 2.0f / (r - l), ox = (r + l) / (r - l);
		float sz = 2.0f / (u - d), oz = (u + d) / (u - d);

		SEyeCB cb = {};
		for (int k = 0; k < 3; k++)
		{
			cb.afRow[0][k] = sx * ex[k] - ox * ey[k];
			cb.afRow[1][k] = sz * ez[k] - oz * ey[k];
			cb.afRow[2][k] = 0;
			cb.afRow[3][k] = ey[k];
		}
		for (int i_row = 0; i_row < 4; i_row++)
		{
			if (i_row == 2)
				continue;
			cb.afRow[i_row][3] = -(cb.afRow[i_row][0] * e[0] + cb.afRow[i_row][1] * e[1] + cb.afRow[i_row][2] * e[2]);
		}
		cb.afRow[2][3] = fr.fNear;		// Reversed infinite depth: z = near / distance.
		for (int k = 0; k < 3; k++)
		{
			cb.afEyePos[k] = e[k];
			cb.afEyeX[k] = ex[k];
			cb.afEyeY[k] = ey[k];
			cb.afEyeZ[k] = ez[k];
		}
		cb.afTan[0] = l;
		cb.afTan[1] = r;
		cb.afTan[2] = d;
		cb.afTan[3] = u;
		bUpload(pcbEye, &cb, sizeof(cb));
	}

	//******************************************************************************************
	// Sun shadow basis: returns false if there is no sun.
	bool bShadowBasis(const SVRRFrame& fr, float af_right[3], float af_up[3], float af_fwd[3])
	{
		float lx = fr.afSunDir[0], ly = fr.afSunDir[1], lz = fr.afSunDir[2];
		float f_len = std::sqrt(lx * lx + ly * ly + lz * lz);
		if (f_len < 1e-4f || lz / f_len < 0.05f)
			return false;
		af_fwd[0] = -lx / f_len; af_fwd[1] = -ly / f_len; af_fwd[2] = -lz / f_len;
		// Right = fwd x world-north (or east when the sun is overhead).
		float ax = 0, ay = 1, az = 0;
		if (std::fabs(af_fwd[1]) > 0.9f)
		{
			ax = 1; ay = 0;
		}
		af_right[0] = af_fwd[1] * az - af_fwd[2] * ay;
		af_right[1] = af_fwd[2] * ax - af_fwd[0] * az;
		af_right[2] = af_fwd[0] * ay - af_fwd[1] * ax;
		f_len = std::sqrt(af_right[0] * af_right[0] + af_right[1] * af_right[1] + af_right[2] * af_right[2]);
		for (int k = 0; k < 3; k++)
			af_right[k] /= f_len;
		af_up[0] = af_right[1] * af_fwd[2] - af_right[2] * af_fwd[1];
		af_up[1] = af_right[2] * af_fwd[0] - af_right[0] * af_fwd[2];
		af_up[2] = af_right[0] * af_fwd[1] - af_right[1] * af_fwd[0];
		return true;
	}
}

//**********************************************************************************************
bool VRR_Init(void* pv_device, void* pv_context, int i_width, int i_height, int i_msaa, int i_shadow_size, char* str_err, int i_err_len)
{
	VRR_Shutdown();
	pDev = (ID3D11Device*)pv_device;
	pCtx = (ID3D11DeviceContext*)pv_context;
	if (!pDev || !pCtx)
	{
		Err(str_err, i_err_len, "no Direct3D 11 device");
		return false;
	}
	if (pDev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
	{
		Err(str_err, i_err_len, "the graphics card does not support Direct3D 11");
		return false;
	}
	pDev->AddRef();
	pCtx->AddRef();

	iWidth = (std::max)(64, i_width);
	iHeight = (std::max)(64, i_height);
	iMSAA = i_msaa >= 8 ? 8 : i_msaa >= 4 ? 4 : i_msaa >= 2 ? 2 : 1;
	iShadowSize = i_shadow_size >= 512 ? (std::min)(i_shadow_size, 8192) : 0;

	std::string str;
	if (!bCreateShaders(str) || !bCreateStates() || !bCreateTargets(str))
	{
		if (str.empty())
			str = "could not create the renderer's resources";
		Err(str_err, i_err_len, str.c_str());
		VRR_Shutdown();
		return false;
	}
	bReady = true;
	return true;
}

//**********************************************************************************************
void VRR_ClearAll()
{
	for (auto& tex : atex)
	{
		SafeRelease(tex.psrv);
		SafeRelease(tex.ptex);
	}
	atex.clear();
	for (auto& msh : amesh)
	{
		SafeRelease(msh.pvb);
		SafeRelease(msh.pib);
	}
	amesh.clear();
	SafeRelease(pvbTerrain);
	SafeRelease(pibTerrain);
	iTerrainIndices = 0;
	ReleasePatches();
	SafeRelease(prtvAtlas);
	SafeRelease(psrvAtlas);
	SafeRelease(ptexAtlas);
	bAtlasBuilt = false;
	adrwDecal.clear();
	aafDecalMap.clear();
	abDecalMap.clear();
	ReleaseDetail();
	adrw.clear();
	avDyn.clear();
	au4DynIdx.clear();
	adynmesh.clear();
}

//**********************************************************************************************
void VRR_Shutdown()
{
	VRR_ClearAll();
	ReleaseTargets();
	SafeRelease(pvbDyn);
	SafeRelease(pibDyn);
	SafeRelease(pvbInst);
	iDynVBCap = iDynIBCap = iInstCap = 0;
	SafeRelease(texWhite.psrv);
	SafeRelease(texWhite.ptex);
	SafeRelease(ptexCapture);
	bCapturePending = false;

	ID3D11DeviceChild* ap[] =
	{
		pvsObject, ppsObject, pvsTerrain, ppsTerrain, pvsAtlasTerrain, ppsAtlasTerrain, pvsAtlasDecal, ppsAtlasDecal,
		pvsSky, ppsSky, pvsShadowObject, ppsShadowAlpha, pvsShadowTerrain, pilObject, pilTerrain,
		prsBack, prsNone, prsShadow, prsAtlas, pdsTest, pdsOff, pdsShadow, pdsFine, pdsCoarse, pdsTestNoWrite, pbsBlend, pbsOpaque, pbsA2C, pssWrap, pssShadow, pssClamp,
		pcbEye, pcbFrame, pvsIdDecal, ppsIdDecal, pvsBlit, ppsBlit
	};
	for (ID3D11DeviceChild* p : ap)
		if (p)
			p->Release();
	pvsObject = nullptr; ppsObject = nullptr; pvsTerrain = nullptr; ppsTerrain = nullptr;
	pvsAtlasTerrain = nullptr; ppsAtlasTerrain = nullptr; pvsAtlasDecal = nullptr; ppsAtlasDecal = nullptr;
	pvsSky = nullptr; ppsSky = nullptr; pvsShadowObject = nullptr; ppsShadowAlpha = nullptr; pvsShadowTerrain = nullptr;
	pilObject = nullptr; pilTerrain = nullptr;
	prsBack = nullptr; prsNone = nullptr; prsShadow = nullptr; prsAtlas = nullptr;
	pdsTest = nullptr; pdsOff = nullptr; pdsShadow = nullptr; pdsFine = nullptr; pdsCoarse = nullptr; pdsTestNoWrite = nullptr; pbsBlend = nullptr; pbsOpaque = nullptr; pbsA2C = nullptr;
	pssWrap = nullptr; pssShadow = nullptr; pssClamp = nullptr; pcbEye = nullptr; pcbFrame = nullptr;
	pvsIdDecal = nullptr; ppsIdDecal = nullptr; pvsBlit = nullptr; ppsBlit = nullptr;

	SafeRelease(pCtx);
	SafeRelease(pDev);
	bReady = false;
}

//**********************************************************************************************
bool VRR_Ready()
{
	return bReady;
}

//**********************************************************************************************
int VRR_TextureCreate(int i_width, int i_height, const uint32_t* pu4_pixels)
{
	if (!bReady || i_width <= 0 || i_height <= 0 || !pu4_pixels)
		return 0;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = i_width;
	td.Height = i_height;
	td.MipLevels = 0;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

	STexture tex;
	if (FAILED(pDev->CreateTexture2D(&td, nullptr, &tex.ptex)))
		return 0;
	if (FAILED(pDev->CreateShaderResourceView(tex.ptex, nullptr, &tex.psrv)))
	{
		SafeRelease(tex.ptex);
		return 0;
	}
	pCtx->UpdateSubresource(tex.ptex, 0, nullptr, pu4_pixels, i_width * 4, 0);
	pCtx->GenerateMips(tex.psrv);
	tex.iBytes = i_width * i_height * 4 * 4 / 3;
	atex.push_back(tex);
	return (int)atex.size();
}

//**********************************************************************************************
bool VRR_TextureUpdate(int i_handle, int i_width, int i_height, const uint32_t* pu4_pixels)
{
	if (!bReady || i_handle <= 0 || i_handle > (int)atex.size() || !pu4_pixels)
		return false;
	STexture& tex = atex[i_handle - 1];
	if (!tex.ptex)
		return false;
	D3D11_TEXTURE2D_DESC td;
	tex.ptex->GetDesc(&td);
	if ((int)td.Width != i_width || (int)td.Height != i_height)
		return false;
	pCtx->UpdateSubresource(tex.ptex, 0, nullptr, pu4_pixels, i_width * 4, 0);
	pCtx->GenerateMips(tex.psrv);
	return true;
}

//**********************************************************************************************
int VRR_MeshCreate(const SVRRVertex* pv, int i_verts, const uint32_t* pu4_indices, int i_indices)
{
	if (!bReady || i_verts <= 0 || i_indices <= 0)
		return 0;
	SMesh msh;
	msh.pvb = pbufCreate(sizeof(SVRRVertex) * i_verts, D3D11_BIND_VERTEX_BUFFER, false, pv);
	msh.pib = pbufCreate(sizeof(uint32_t) * i_indices, D3D11_BIND_INDEX_BUFFER, false, pu4_indices);
	if (!msh.pvb || !msh.pib)
	{
		SafeRelease(msh.pvb);
		SafeRelease(msh.pib);
		return 0;
	}
	amesh.push_back(msh);
	return (int)amesh.size();
}

//**********************************************************************************************
int VRR_MeshDynamic(const SVRRVertex* pv, int i_verts, const uint32_t* pu4_indices, int i_indices)
{
	if (!bReady || i_verts <= 0 || i_indices <= 0)
		return 0;
	SDynMesh dm;
	dm.iBaseVertex = (int)avDyn.size();
	dm.iFirstIndex = (int)au4DynIdx.size();
	avDyn.insert(avDyn.end(), pv, pv + i_verts);
	au4DynIdx.insert(au4DynIdx.end(), pu4_indices, pu4_indices + i_indices);
	adynmesh.push_back(dm);
	return iDYNAMIC_BASE + (int)adynmesh.size() - 1;
}

//**********************************************************************************************
void VRR_AddDraw(const SVRRDraw& drw)
{
	if (bReady && drw.iNumIndices > 0)
		adrw.push_back(drw);
}

//**********************************************************************************************
bool VRR_TerrainSet(int i_nx, int i_ny, float f_x0, float f_y0, float f_spacing, const SVRRTerrainVertex* ptv, int i_atlas_size)
{
	if (!bReady || i_nx < 2 || i_ny < 2 || !ptv)
		return false;
	SafeRelease(pvbTerrain);
	SafeRelease(pibTerrain);
	SafeRelease(prtvAtlas);
	SafeRelease(psrvAtlas);
	SafeRelease(ptexAtlas);
	bAtlasBuilt = false;
	adrwDecal.clear();
	aafDecalMap.clear();
	abDecalMap.clear();
	ReleaseDetail();

	std::vector<uint32_t> au4;
	au4.reserve((size_t)(i_nx - 1) * (i_ny - 1) * 6);
	for (int y = 0; y < i_ny - 1; y++)
	{
		for (int x = 0; x < i_nx - 1; x++)
		{
			uint32_t a = y * i_nx + x, b = a + 1, c = a + i_nx, d = c + 1;
			// Counter-clockwise seen from above.
			au4.push_back(a); au4.push_back(b); au4.push_back(d);
			au4.push_back(a); au4.push_back(d); au4.push_back(c);
		}
	}
	pvbTerrain = pbufCreate(sizeof(SVRRTerrainVertex) * i_nx * i_ny, D3D11_BIND_VERTEX_BUFFER, false, ptv);
	pibTerrain = pbufCreate((UINT)(sizeof(uint32_t) * au4.size()), D3D11_BIND_INDEX_BUFFER, false, au4.data());
	if (!pvbTerrain || !pibTerrain)
	{
		SafeRelease(pvbTerrain);
		SafeRelease(pibTerrain);
		return false;
	}
	iTerrainIndices = (int)au4.size();

	float f_sx = f_spacing * (i_nx - 1), f_sy = f_spacing * (i_ny - 1);
	afAtlas[0] = f_x0;
	afAtlas[1] = f_y0;
	afAtlas[2] = 1.0f / f_sx;
	afAtlas[3] = 1.0f / f_sy;

	// The atlas: one texture painted with the terrain's textures from above.
	iAtlasSize = (std::min)((std::max)(i_atlas_size, 256), 16384);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = td.Height = iAtlasSize;
	td.MipLevels = 0;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	while (FAILED(pDev->CreateTexture2D(&td, nullptr, &ptexAtlas)) && td.Width > 1024)
		td.Width = td.Height = td.Width / 2;
	if (ptexAtlas)
	{
		iAtlasSize = td.Width;
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rd.Texture2D.MipSlice = 0;
		if (FAILED(pDev->CreateShaderResourceView(ptexAtlas, nullptr, &psrvAtlas)) ||
			FAILED(pDev->CreateRenderTargetView(ptexAtlas, &rd, &prtvAtlas)))
		{
			SafeRelease(prtvAtlas);
			SafeRelease(psrvAtlas);
			SafeRelease(ptexAtlas);
		}
	}
	return true;
}

//**********************************************************************************************
void VRR_TerrainAddDecal(const SVRRDraw& drw, const float* pf_map)
{
	if (bReady && drw.iNumIndices > 0)
	{
		adrwDecal.push_back(drw);
		std::array<float, 6> af = {};
		if (pf_map)
			for (int k = 0; k < 6; k++)
				af[k] = pf_map[k];
		aafDecalMap.push_back(af);
		abDecalMap.push_back(pf_map != nullptr);
	}
}

//**********************************************************************************************
bool VRR_HasTerrain()
{
	return pvbTerrain != nullptr;
}

//**********************************************************************************************
// Terrain textures at full resolution: a texture array with every terrain texture, an id map
// saying which one is on top at each spot, and each one's mapping onto the ground.
static void BuildDetail()
{
	ReleaseDetail();
	int i_decals = (int)adrwDecal.size();
	if (i_decals == 0 || i_decals >= 0xFFFE || aafDecalMap.size() != adrwDecal.size())
		return;

	// One slice per distinct texture.
	std::map<int, int> map_slice;
	int i_max_dim = 64;
	for (int i = 0; i < i_decals; i++)
	{
		int i_tex = adrwDecal[i].iTexture;
		if (map_slice.count(i_tex))
			continue;
		int i_slice = (int)map_slice.size();
		map_slice[i_tex] = i_slice;
		if (i_tex > 0 && i_tex <= (int)atex.size() && atex[i_tex - 1].ptex)
		{
			D3D11_TEXTURE2D_DESC td;
			atex[i_tex - 1].ptex->GetDesc(&td);
			i_max_dim = (std::max)(i_max_dim, (int)(std::max)(td.Width, td.Height));
		}
	}
	if (map_slice.empty() || map_slice.size() > 2048)
		return;
	iDetailSlices = (int)map_slice.size();
	iDetailSize = 64;
	while (iDetailSize < i_max_dim && iDetailSize < 512)
		iDetailSize *= 2;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = td.Height = iDetailSize;
	td.MipLevels = 0;
	td.ArraySize = iDetailSlices;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	if (FAILED(pDev->CreateTexture2D(&td, nullptr, &ptexDetail)) ||
		FAILED(pDev->CreateShaderResourceView(ptexDetail, nullptr, &psrvDetail)))
	{
		ReleaseDetail();
		return;
	}

	// The id map covers the same area as the atlas, about 2 texels a metre.
	float f_extent = (std::max)(1.0f / afAtlas[2], 1.0f / afAtlas[3]);
	iIdMapSize = 1024;
	while (iIdMapSize < f_extent * 2.0f && iIdMapSize < 8192)
		iIdMapSize *= 2;
	D3D11_TEXTURE2D_DESC tdi = {};
	tdi.Width = tdi.Height = iIdMapSize;
	tdi.MipLevels = 1;
	tdi.ArraySize = 1;
	tdi.Format = DXGI_FORMAT_R16_UINT;
	tdi.SampleDesc.Count = 1;
	tdi.Usage = D3D11_USAGE_DEFAULT;
	tdi.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	ID3D11RenderTargetView* prtv_id = nullptr;
	ID3D11RenderTargetView* prtv_uv = nullptr;
	D3D11_TEXTURE2D_DESC tdu = tdi;
	tdu.Format = DXGI_FORMAT_R32G32_FLOAT;
	if (FAILED(pDev->CreateTexture2D(&tdi, nullptr, &ptexIdMap)) ||
		FAILED(pDev->CreateShaderResourceView(ptexIdMap, nullptr, &psrvIdMap)) ||
		FAILED(pDev->CreateRenderTargetView(ptexIdMap, nullptr, &prtv_id)) ||
		FAILED(pDev->CreateTexture2D(&tdu, nullptr, &ptexUvMap)) ||
		FAILED(pDev->CreateShaderResourceView(ptexUvMap, nullptr, &psrvUvMap)) ||
		FAILED(pDev->CreateRenderTargetView(ptexUvMap, nullptr, &prtv_uv)))
	{
		SafeRelease(prtv_id);
		SafeRelease(prtv_uv);
		ReleaseDetail();
		return;
	}

	// Decal table: two float4 per decal.
	std::vector<float> af_tab((size_t)i_decals * 8, 0.0f);
	for (int i = 0; i < i_decals; i++)
	{
		const std::array<float, 6>& m = aafDecalMap[i];
		float* pf = &af_tab[(size_t)i * 8];
		pf[0] = m[0]; pf[1] = m[1]; pf[2] = m[2];
		pf[3] = (float)map_slice[adrwDecal[i].iTexture];
		pf[4] = m[3]; pf[5] = m[4]; pf[6] = m[5];
		pf[7] = 0.0f;		// (1 would take coordinates from the coordinate map: off, it swirled.)
	}
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = (UINT)(af_tab.size() * sizeof(float));
	bd.Usage = D3D11_USAGE_IMMUTABLE;
	bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = 16;
	D3D11_SUBRESOURCE_DATA srd = { af_tab.data(), 0, 0 };
	if (FAILED(pDev->CreateBuffer(&bd, &srd, &pbufDecal)) ||
		FAILED(pDev->CreateShaderResourceView(pbufDecal, nullptr, &psrvDecal)))
	{
		SafeRelease(prtv_id);
		SafeRelease(prtv_uv);
		ReleaseDetail();
		return;
	}

	pCtx->OMSetDepthStencilState(pdsOff, 0);
	pCtx->OMSetBlendState(pbsOpaque, nullptr, 0xFFFFFFFF);
	pCtx->RSSetState(prsAtlas);
	pCtx->PSSetSamplers(0, 1, &pssWrap);

	// Copy each texture into its slice.
	pCtx->IASetInputLayout(nullptr);
	pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pCtx->VSSetShader(pvsBlit, nullptr, 0);
	pCtx->PSSetShader(ppsBlit, nullptr, 0);
	D3D11_VIEWPORT vp = { 0, 0, (float)iDetailSize, (float)iDetailSize, 0, 1 };
	pCtx->RSSetViewports(1, &vp);
	for (const auto& pr : map_slice)
	{
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
		rd.Texture2DArray.MipSlice = 0;
		rd.Texture2DArray.FirstArraySlice = pr.second;
		rd.Texture2DArray.ArraySize = 1;
		ID3D11RenderTargetView* prtv = nullptr;
		if (FAILED(pDev->CreateRenderTargetView(ptexDetail, &rd, &prtv)))
			continue;
		pCtx->OMSetRenderTargets(1, &prtv, nullptr);
		ID3D11ShaderResourceView* psrv = psrvTex(pr.first);
		pCtx->PSSetShaderResources(0, 1, &psrv);
		pCtx->Draw(3, 0);
		ID3D11RenderTargetView* prtv_null = nullptr;
		pCtx->OMSetRenderTargets(1, &prtv_null, nullptr);
		prtv->Release();
	}
	pCtx->GenerateMips(psrvDetail);

	// The id map: decals in order, later ones on top.  Where a decal's mapping could not be
	// worked out, the map says "use the atlas" (0xFFFE).
	float af_clear[4] = { 65535.0f, 0, 0, 0 };
	pCtx->ClearRenderTargetView(prtv_id, af_clear);
	float af_clear_uv[4] = { 0, 0, 0, 0 };
	pCtx->ClearRenderTargetView(prtv_uv, af_clear_uv);
	ID3D11RenderTargetView* aprtv_id[2] = { prtv_id, prtv_uv };
	pCtx->OMSetRenderTargets(2, aprtv_id, nullptr);
	D3D11_VIEWPORT vpi = { 0, 0, (float)iIdMapSize, (float)iIdMapSize, 0, 1 };
	pCtx->RSSetViewports(1, &vpi);

	SFrameCB cb = {};
	for (int k = 0; k < 4; k++)
		cb.afAtlas[k] = afAtlas[k];
	bUpload(pcbFrame, &cb, sizeof(cb));
	pCtx->VSSetConstantBuffers(1, 1, &pcbFrame);
	pCtx->PSSetConstantBuffers(1, 1, &pcbFrame);

	std::vector<SInstance> ainst(i_decals);
	for (int i = 0; i < i_decals; i++)
	{
		std::memcpy(ainst[i].afRow, adrwDecal[i].afWorld, sizeof(ainst[i].afRow));
		// Textures that don't lie flat on the ground (roads that follow a curve) use the
		// painted overview instead.
		ainst[i].u4Flags = abDecalMap[i] ? (uint32_t)i : 0xFFFEu;
	}
	ID3D11Buffer* pvb_inst = pbufCreate((UINT)(sizeof(SInstance) * ainst.size()), D3D11_BIND_VERTEX_BUFFER, false, ainst.data());
	if (pvb_inst)
	{
		UINT u_stride = sizeof(SInstance), u_offset = 0;
		pCtx->IASetVertexBuffers(1, 1, &pvb_inst, &u_stride, &u_offset);
		pCtx->IASetInputLayout(pilObject);
		pCtx->VSSetShader(pvsIdDecal, nullptr, 0);
		pCtx->PSSetShader(ppsIdDecal, nullptr, 0);
		int i_bound = -1;
		for (int i = 0; i < i_decals; i++)
		{
			const SVRRDraw& drw = adrwDecal[i];
			int i_base, i_first;
			if (!bBindMesh(drw.iMesh, i_base, i_first))
				continue;
			if (drw.iTexture != i_bound)
			{
				ID3D11ShaderResourceView* psrv = psrvTex(drw.iTexture);
				pCtx->PSSetShaderResources(0, 1, &psrv);
				i_bound = drw.iTexture;
			}
			pCtx->DrawIndexedInstanced(drw.iNumIndices, 1, i_first + drw.iFirstIndex, i_base, (UINT)i);
		}
		pvb_inst->Release();
	}
	ID3D11RenderTargetView* aprtv_null[2] = { nullptr, nullptr };
	pCtx->OMSetRenderTargets(2, aprtv_null, nullptr);
	prtv_id->Release();
	prtv_uv->Release();
	bDetailBuilt = true;
}

//**********************************************************************************************
void VRR_TerrainBuildAtlas()
{
	if (!bReady || !prtvAtlas || !pvbTerrain)
		return;

	SFrameCB cb = {};
	for (int k = 0; k < 4; k++)
		cb.afAtlas[k] = afAtlas[k];
	bUpload(pcbFrame, &cb, sizeof(cb));

	float af_clear[4] = { 0, 0, 0, 1 };
	pCtx->ClearRenderTargetView(prtvAtlas, af_clear);
	pCtx->OMSetRenderTargets(1, &prtvAtlas, nullptr);
	D3D11_VIEWPORT vp = { 0, 0, (float)iAtlasSize, (float)iAtlasSize, 0, 1 };
	pCtx->RSSetViewports(1, &vp);
	pCtx->RSSetState(prsAtlas);
	pCtx->OMSetDepthStencilState(pdsOff, 0);
	pCtx->OMSetBlendState(pbsOpaque, nullptr, 0xFFFFFFFF);
	pCtx->VSSetConstantBuffers(1, 1, &pcbFrame);
	pCtx->PSSetConstantBuffers(1, 1, &pcbFrame);
	pCtx->PSSetSamplers(0, 1, &pssWrap);

	// Base colours.
	pCtx->VSSetShader(pvsAtlasTerrain, nullptr, 0);
	pCtx->PSSetShader(ppsAtlasTerrain, nullptr, 0);
	DrawTerrain();

	// Terrain textures, lowest first.
	if (!adrwDecal.empty())
	{
		std::vector<SInstance> ainst(adrwDecal.size());
		for (size_t i = 0; i < adrwDecal.size(); i++)
		{
			std::memcpy(ainst[i].afRow, adrwDecal[i].afWorld, sizeof(ainst[i].afRow));
			ainst[i].u4Flags = 0;
		}
		ID3D11Buffer* pvb_inst = pbufCreate((UINT)(sizeof(SInstance) * ainst.size()), D3D11_BIND_VERTEX_BUFFER, false, ainst.data());
		if (pvb_inst)
		{
			UINT u_stride = sizeof(SInstance), u_offset = 0;
			pCtx->IASetVertexBuffers(1, 1, &pvb_inst, &u_stride, &u_offset);
			pCtx->IASetInputLayout(pilObject);
			pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			pCtx->VSSetShader(pvsAtlasDecal, nullptr, 0);
			pCtx->PSSetShader(ppsAtlasDecal, nullptr, 0);
			int i_bound = -1;
			for (size_t i = 0; i < adrwDecal.size(); i++)
			{
				const SVRRDraw& drw = adrwDecal[i];
				int i_base, i_first;
				if (!bBindMesh(drw.iMesh, i_base, i_first))
					continue;
				if (drw.iTexture != i_bound)
				{
					ID3D11ShaderResourceView* psrv = psrvTex(drw.iTexture);
					pCtx->PSSetShaderResources(0, 1, &psrv);
					i_bound = drw.iTexture;
				}
				pCtx->DrawIndexedInstanced(drw.iNumIndices, 1, i_first + drw.iFirstIndex, i_base, (UINT)i);
			}
			pvb_inst->Release();
		}
	}

	ID3D11RenderTargetView* prtv_null = nullptr;
	pCtx->OMSetRenderTargets(1, &prtv_null, nullptr);
	pCtx->GenerateMips(psrvAtlas);
	bAtlasBuilt = true;
	BuildDetail();
	adrwDecal.clear();
	aafDecalMap.clear();
	abDecalMap.clear();
}

//**********************************************************************************************
void VRR_RenderFrame(const SVRRFrame& fr)
{
	if (!bReady)
		return;

	//
	// Upload this frame's dynamic meshes and instances.
	//
	if (!avDyn.empty())
	{
		if ((int)avDyn.size() > iDynVBCap)
		{
			SafeRelease(pvbDyn);
			iDynVBCap = (int)avDyn.size() * 3 / 2 + 1024;
			pvbDyn = pbufCreate(sizeof(SVRRVertex) * iDynVBCap, D3D11_BIND_VERTEX_BUFFER, true, nullptr);
		}
		if ((int)au4DynIdx.size() > iDynIBCap)
		{
			SafeRelease(pibDyn);
			iDynIBCap = (int)au4DynIdx.size() * 3 / 2 + 4096;
			pibDyn = pbufCreate(sizeof(uint32_t) * iDynIBCap, D3D11_BIND_INDEX_BUFFER, true, nullptr);
		}
		if (pvbDyn)
			bUpload(pvbDyn, avDyn.data(), sizeof(SVRRVertex) * avDyn.size());
		if (pibDyn)
			bUpload(pibDyn, au4DynIdx.data(), sizeof(uint32_t) * au4DynIdx.size());
	}

	// Opaque first, then see-through; grouped by mesh and texture.
	aiOrder.resize(adrw.size());
	for (size_t i = 0; i < adrw.size(); i++)
		aiOrder[i] = (int)i;
	// Blended things last, farthest first.
	float f_ex = fr.afEyePos[0][0], f_ey = fr.afEyePos[0][1], f_ez = fr.afEyePos[0][2];
	std::sort(aiOrder.begin(), aiOrder.end(), [&](int a, int b)
	{
		const SVRRDraw& da = adrw[a];
		const SVRRDraw& db = adrw[b];
		const uint32_t u4_key = evrrALPHA_TEST | evrrTWO_SIDED | evrrBLEND;
		uint32_t ka = (da.u4Flags & u4_key) + ((da.u4Flags & evrrBLEND) ? 1024 : 0);
		uint32_t kb = (db.u4Flags & u4_key) + ((db.u4Flags & evrrBLEND) ? 1024 : 0);
		if ((ka >= 1024) != (kb >= 1024))
			return ka < kb;
		if (ka >= 1024)
		{
			auto d2 = [&](const SVRRDraw& d)
			{
				float x = d.afWorld[3] - f_ex, y = d.afWorld[7] - f_ey, z = d.afWorld[11] - f_ez;
				return x * x + y * y + z * z;
			};
			return d2(da) > d2(db);
		}
		if (ka != kb)
			return ka < kb;
		if (da.iMesh != db.iMesh)
			return da.iMesh < db.iMesh;
		if (da.iFirstIndex != db.iFirstIndex)
			return da.iFirstIndex < db.iFirstIndex;
		if (da.iTexture != db.iTexture)
			return da.iTexture < db.iTexture;
		if (da.iNormalTex != db.iNormalTex)
			return da.iNormalTex < db.iNormalTex;
		return (da.u4Flags & evrrNO_SHADOW) < (db.u4Flags & evrrNO_SHADOW);
	});

	// Instance data in draw order, so runs of the same mesh part and texture (trees, plants,
	// rocks) go to the card as one instanced draw.
	if (!adrw.empty())
	{
		if ((int)adrw.size() > iInstCap)
		{
			SafeRelease(pvbInst);
			iInstCap = (int)adrw.size() * 3 / 2 + 256;
			pvbInst = pbufCreate(sizeof(SInstance) * iInstCap, D3D11_BIND_VERTEX_BUFFER, true, nullptr);
		}
		std::vector<SInstance> ainst(adrw.size());
		for (size_t i = 0; i < adrw.size(); i++)
		{
			const SVRRDraw& d = adrw[aiOrder[i]];
			std::memcpy(ainst[i].afRow, d.afWorld, sizeof(ainst[i].afRow));
			ainst[i].u4Flags = d.u4Flags;
			ainst[i].afUV[0] = d.afUVOffset[0];
			ainst[i].afUV[1] = d.afUVOffset[1];
		}
		if (pvbInst)
			bUpload(pvbInst, ainst.data(), sizeof(SInstance) * ainst.size());
	}



	//
	// Frame constants.
	//
	SFrameCB cb = {};
	{
		float lx = fr.afSunDir[0], ly = fr.afSunDir[1], lz = fr.afSunDir[2];
		float f_len = std::sqrt(lx * lx + ly * ly + lz * lz);
		if (f_len < 1e-4f)
		{
			lx = 0; ly = 0; lz = 1; f_len = 1;
		}
		cb.afSun[0] = lx / f_len; cb.afSun[1] = ly / f_len; cb.afSun[2] = lz / f_len; cb.afSun[3] = fr.fSun;
		cb.afLight[0] = fr.fAmbient;
		cb.afLight[1] = fr.fBrightness;
		for (int k = 0; k < 3; k++)
			cb.afFogColour[k] = fr.afFogColour[k];
		cb.afFogColour[3] = fr.fFogFar > 1.0f ? 1.0f / fr.fFogFar : 0.0f;
		for (int k = 0; k < 32; k++)
			cb.afFogTable[k] = fr.afFogTable[k];
		cb.afSky0[0] = fr.fSkyHeight;
		cb.afSky0[1] = fr.fSkyUVScale;
		cb.afSky0[2] = fr.afSkyOffset[0];
		cb.afSky0[3] = fr.afSkyOffset[1];
		cb.afSky1[0] = fr.fSkyFogNear;
		cb.afSky1[1] = fr.fSkyFogScale;
		cb.afSky1[2] = fr.iSkyTexture > 0 ? 1.0f : 0.0f;
		cb.afSky1[3] = 0.02f;
		for (int k = 0; k < 3; k++)
			cb.afSkyFog[k] = fr.afSkyFogColour[k];
		for (int k = 0; k < 4; k++)
			cb.afAtlas[k] = afAtlas[k];
		cb.afAtlasOn[0] = bAtlasBuilt ? 1.0f : 0.0f;
		cb.afAtlasOn[1] = bDetailBuilt ? 1.0f : 0.0f;
		cb.afAtlasOn[2] = (float)(GetTickCount() % 3600000u) / 1000.0f;	// Seconds, for moving water.
	}

	//
	// Shadow map.
	//
	float af_right[3], af_up[3], af_fwd[3];
	bool b_shadows = iShadowSize > 0 && fr.fShadowRadius > 1.0f && fr.fSun > 0.01f && bShadowBasis(fr, af_right, af_up, af_fwd);
	if (b_shadows)
	{
		float f_r = fr.fShadowRadius;
		float f_depth = 1500.0f;
		// Snap the centre to whole shadow texels so the shadows don't crawl as you move.
		float f_texel = 2.0f * f_r / iShadowSize;
		const float* c = fr.afShadowCentre;
		float f_cr = c[0] * af_right[0] + c[1] * af_right[1] + c[2] * af_right[2];
		float f_cu = c[0] * af_up[0] + c[1] * af_up[1] + c[2] * af_up[2];
		float f_cf = c[0] * af_fwd[0] + c[1] * af_fwd[1] + c[2] * af_fwd[2];
		f_cr = std::floor(f_cr / f_texel) * f_texel;
		f_cu = std::floor(f_cu / f_texel) * f_texel;
		for (int k = 0; k < 3; k++)
		{
			cb.afShadowRow[0][k] = af_right[k] / (2.0f * f_r);
			cb.afShadowRow[1][k] = -af_up[k] / (2.0f * f_r);
			cb.afShadowRow[2][k] = af_fwd[k] / (2.0f * f_depth);
		}
		cb.afShadowRow[0][3] = 0.5f - f_cr / (2.0f * f_r);
		cb.afShadowRow[1][3] = 0.5f + f_cu / (2.0f * f_r);
		cb.afShadowRow[2][3] = 0.5f - f_cf / (2.0f * f_depth);
		cb.afShadowParams[0] = 1.0f / iShadowSize;
		cb.afShadowParams[1] = 0.0004f;
		cb.afShadowParams[2] = 1.0f;
	}
	bUpload(pcbFrame, &cb, sizeof(cb));

	ID3D11Buffer* apcb[2] = { pcbEye, pcbFrame };
	pCtx->VSSetConstantBuffers(0, 2, apcb);
	pCtx->PSSetConstantBuffers(0, 2, apcb);
	ID3D11SamplerState* apss[3] = { pssWrap, pssShadow, pssClamp };
	pCtx->PSSetSamplers(0, 3, apss);

	if (b_shadows)
	{
		ID3D11ShaderResourceView* apsrv_null[3] = { nullptr, nullptr, nullptr };
		pCtx->PSSetShaderResources(0, 3, apsrv_null);
		ID3D11RenderTargetView* prtv_null = nullptr;
		pCtx->OMSetRenderTargets(1, &prtv_null, pdsvShadow);
		pCtx->ClearDepthStencilView(pdsvShadow, D3D11_CLEAR_DEPTH, 1.0f, 0);
		D3D11_VIEWPORT vp = { 0, 0, (float)iShadowSize, (float)iShadowSize, 0, 1 };
		pCtx->RSSetViewports(1, &vp);
		pCtx->RSSetState(prsShadow);
		pCtx->OMSetDepthStencilState(pdsShadow, 0);
		pCtx->OMSetBlendState(pbsOpaque, nullptr, 0xFFFFFFFF);

		pCtx->VSSetShader(pvsShadowTerrain, nullptr, 0);
		pCtx->PSSetShader(nullptr, nullptr, 0);
		// The shadowed area lies inside the fine patches once they are there.
		if (bAnyPatch())
			DrawPatches();
		else
			DrawTerrain();

		if (!adrw.empty() && pvbInst)
		{
			pCtx->VSSetShader(pvsShadowObject, nullptr, 0);
			DrawObjects(true);
		}
		pCtx->OMSetRenderTargets(1, &prtv_null, nullptr);
	}

	//
	// Each eye.
	//
	for (int i_eye = 0; i_eye < 2; i_eye++)
	{
		SetEye(fr, i_eye);

		ID3D11RenderTargetView* prtv = iMSAA > 1 ? prtvColour : aprtvEye[i_eye];
		float af_clear[4] = { fr.afSkyFogColour[0], fr.afSkyFogColour[1], fr.afSkyFogColour[2], 1 };
		pCtx->OMSetRenderTargets(1, &prtv, pdsvDepth);
		pCtx->ClearRenderTargetView(prtv, af_clear);
		pCtx->ClearDepthStencilView(pdsvDepth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
		D3D11_VIEWPORT vp = { 0, 0, (float)iWidth, (float)iHeight, 0, 1 };
		pCtx->RSSetViewports(1, &vp);

		ID3D11ShaderResourceView* apsrv[7] =
		{
			psrvTex(fr.iSkyTexture), b_shadows ? psrvShadow : nullptr, bAtlasBuilt ? psrvAtlas : nullptr,
			bDetailBuilt ? psrvDetail : nullptr, bDetailBuilt ? psrvIdMap : nullptr, bDetailBuilt ? psrvDecal : nullptr,
			bDetailBuilt ? psrvUvMap : nullptr
		};
		pCtx->PSSetShaderResources(0, 7, apsrv);

		// Sky.
		pCtx->RSSetState(prsNone);
		pCtx->OMSetDepthStencilState(pdsOff, 0);
		pCtx->OMSetBlendState(pbsOpaque, nullptr, 0xFFFFFFFF);
		pCtx->IASetInputLayout(nullptr);
		pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		pCtx->VSSetShader(pvsSky, nullptr, 0);
		pCtx->PSSetShader(ppsSky, nullptr, 0);
		pCtx->Draw(3, 0);

		// Terrain: the fine patches near you first, then the coarse grid wherever they aren't.
		pCtx->RSSetState(prsBack);
		pCtx->VSSetShader(pvsTerrain, nullptr, 0);
		pCtx->PSSetShader(ppsTerrain, nullptr, 0);
		if (bAnyPatch())
		{
			pCtx->OMSetDepthStencilState(pdsFine, 1);
			DrawPatches();
			pCtx->OMSetDepthStencilState(pdsCoarse, 1);
		}
		else
			pCtx->OMSetDepthStencilState(pdsTest, 0);
		DrawTerrain();
		pCtx->OMSetDepthStencilState(pdsTest, 0);

		// Objects.
		if (!adrw.empty() && pvbInst)
		{
			pCtx->VSSetShader(pvsObject, nullptr, 0);
			pCtx->PSSetShader(ppsObject, nullptr, 0);
			DrawObjects(false);
		}

		if (iMSAA > 1)
			pCtx->ResolveSubresource(aptexEye[i_eye], 0, ptexColour, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	ID3D11ShaderResourceView* apsrv_null[3] = { nullptr, nullptr, nullptr };
	pCtx->PSSetShaderResources(0, 3, apsrv_null);
	ID3D11RenderTargetView* prtv_null = nullptr;
	pCtx->OMSetRenderTargets(1, &prtv_null, nullptr);

	// Ready for the next frame.
	iLastDraws = (int)adrw.size();
	iBatchesShown = iLastBatches;
	iLastBatches = 0;
	adrw.clear();
	avDyn.clear();
	au4DynIdx.clear();
	adynmesh.clear();
}

//**********************************************************************************************
void* VRR_EyeTexture(int i_eye)
{
	return (i_eye == 0 || i_eye == 1) ? aptexEye[i_eye] : nullptr;
}

//**********************************************************************************************
void VRR_GetStats(int& ri_draws, int& ri_batches, int& ri_textures, int& ri_meshes, float& rf_gpu_mb)
{
	ri_draws = iLastDraws;
	ri_batches = iBatchesShown;
	ri_textures = (int)atex.size();
	ri_meshes = (int)amesh.size();
	double d_bytes = 0;
	for (const auto& tex : atex)
		d_bytes += tex.iBytes;
	d_bytes += (double)iAtlasSize * iAtlasSize * 4 * 4 / 3;
	rf_gpu_mb = (float)(d_bytes / (1024.0 * 1024.0));
}

//**********************************************************************************************
bool VRR_CaptureEye(int i_size, uint32_t* pu4_out)
{
	if (!bReady || !aptexEye[0] || i_size <= 0 || !pu4_out)
		return false;
	if (!ptexCapture)
	{
		D3D11_TEXTURE2D_DESC td;
		aptexEye[0]->GetDesc(&td);
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.SampleDesc.Count = 1;
		td.SampleDesc.Quality = 0;
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		td.MiscFlags = 0;
		if (FAILED(pDev->CreateTexture2D(&td, nullptr, &ptexCapture)))
			return false;
	}

	bool b_read = false;
	if (bCapturePending)
	{
		D3D11_MAPPED_SUBRESOURCE ms;
		if (SUCCEEDED(pCtx->Map(ptexCapture, 0, D3D11_MAP_READ, 0, &ms)))
		{
			for (int y = 0; y < i_size; y++)
			{
				const uint32_t* pu4_row = (const uint32_t*)((const uint8_t*)ms.pData + (size_t)(y * iHeight / i_size) * ms.RowPitch);
				for (int x = 0; x < i_size; x++)
				{
					uint32_t u = pu4_row[x * iWidth / i_size];
					pu4_out[y * i_size + x] = ((u & 255) << 16) | (u & 0xFF00) | ((u >> 16) & 255);
				}
			}
			pCtx->Unmap(ptexCapture, 0);
			b_read = true;
		}
	}
	pCtx->CopyResource(ptexCapture, aptexEye[0]);
	bCapturePending = true;
	return b_read;
}

//**********************************************************************************************
bool VRR_TerrainPatchSet(int i_slot, int i_n, const SVRRTerrainVertex* ptv)
{
	if (!bReady || i_slot < 0 || i_slot >= iMAX_PATCHES || i_n < 2 || !ptv)
		return false;
	if (i_n != iPatchN || !pibPatch)
	{
		for (int i = 0; i < iMAX_PATCHES; i++)
			SafeRelease(apvbPatch[i]);
		SafeRelease(pibPatch);
		std::vector<uint32_t> au4;
		au4.reserve((size_t)(i_n - 1) * (i_n - 1) * 6);
		for (int y = 0; y < i_n - 1; y++)
			for (int x = 0; x < i_n - 1; x++)
			{
				uint32_t a = y * i_n + x, b = a + 1, c = a + i_n, d = c + 1;
				au4.push_back(a); au4.push_back(b); au4.push_back(d);
				au4.push_back(a); au4.push_back(d); au4.push_back(c);
			}
		pibPatch = pbufCreate((UINT)(sizeof(uint32_t) * au4.size()), D3D11_BIND_INDEX_BUFFER, false, au4.data());
		if (!pibPatch)
			return false;
		iPatchN = i_n;
		iPatchIndices = (int)au4.size();
	}
	SafeRelease(apvbPatch[i_slot]);
	apvbPatch[i_slot] = pbufCreate((UINT)(sizeof(SVRRTerrainVertex) * i_n * i_n), D3D11_BIND_VERTEX_BUFFER, false, ptv);
	return apvbPatch[i_slot] != nullptr;
}

//**********************************************************************************************
void VRR_TerrainPatchClear(int i_slot)
{
	if (i_slot >= 0 && i_slot < iMAX_PATCHES)
		SafeRelease(apvbPatch[i_slot]);
}

//**********************************************************************************************
void VRR_AbortFrame()
{
	adrw.clear();
	avDyn.clear();
	au4DynIdx.clear();
	adynmesh.clear();
	if (pCtx)
	{
		ID3D11RenderTargetView* aprtv_null[2] = { nullptr, nullptr };
		pCtx->OMSetRenderTargets(2, aprtv_null, nullptr);
	}
}
