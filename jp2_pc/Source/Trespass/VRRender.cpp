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

struct VSObjIn
{
	float3 p     : POSITION;
	float3 n     : NORMAL;
	float2 uv    : TEXCOORD0;
	float4 r0    : TEXCOORD1;
	float4 r1    : TEXCOORD2;
	float4 r2    : TEXCOORD3;
	uint   flags : TEXCOORD4;
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

float FogAt(float3 wp)
{
	float d = clamp(distance(wp, gEyePos.xyz) * gFogColour.w * 31.0, 0.0, 31.0);
	int i0 = (int)d;
	int i1 = min(i0 + 1, 31);
	float a = gFogTable[i0 >> 2][i0 & 3];
	float b = gFogTable[i1 >> 2][i1 & 3];
	return lerp(a, b, d - i0);
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
	o.uv = i.uv;
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
	else
		c.a = 1.0;
	float3 n = normalize(i.wn);
	if (!front)
		n = -n;
	float3 col = Shade(c.rgb, n, i.wp, (i.flags & 8) != 0);
	if (!(i.flags & 16))
		col = lerp(col, gFogColour.rgb, FogAt(i.wp));
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

float4 PSTerrain(PSTerIn i) : SV_Target
{
	float3 albedo = i.c.rgb;
	if (gAtlasOn.x > 0.5)
		albedo = gAtlasTex.Sample(gSamp, (i.wp.xy - gAtlas.xy) * gAtlas.zw).rgb;
	float3 col = Shade(albedo, normalize(i.wn), i.wp, false);
	col = lerp(col, gFogColour.rgb, FogAt(i.wp));
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
	float3 col = gSkyFog.rgb;
	if (d.z > gSky1.w && gSky1.z > 0.5)
	{
		float t = gSky0.x / d.z;
		float2 uv = (gEyePos.xy + d.xy * t) * gSky0.y + gSky0.zw;
		float3 c = gTex.SampleGrad(gSamp, uv, ddx(uv), ddy(uv)).rgb;
		float f = saturate((1.0 - d.z - gSky1.x) * gSky1.y);
		col = lerp(c, gSkyFog.rgb, f);
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
	ID3D11Buffer*             pibTerrain = nullptr;
	int                       iTerrainIndices = 0;
	float                     afAtlas[4] = { 0, 0, 0, 0 };
	int                       iAtlasSize = 0;
	ID3D11Texture2D*          ptexAtlas = nullptr;
	ID3D11ShaderResourceView* psrvAtlas = nullptr;
	ID3D11RenderTargetView*   prtvAtlas = nullptr;
	bool                      bAtlasBuilt = false;
	std::vector<SVRRDraw>     adrwDecal;

	int iLastDraws = 0;

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
			td.Format = DXGI_FORMAT_D32_FLOAT;
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
		dd.DepthFunc = D3D11_COMPARISON_LESS;
		pDev->CreateDepthStencilState(&dd, &pdsShadow);
		dd.DepthEnable = FALSE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		pDev->CreateDepthStencilState(&dd, &pdsOff);

		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = FALSE;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		pDev->CreateBlendState(&bd, &pbsOpaque);
		bd.AlphaToCoverageEnable = TRUE;
		pDev->CreateBlendState(&bd, &pbsA2C);

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
			};
			D3D11_INPUT_ELEMENT_DESC aied_ter[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			b_ok = SUCCEEDED(pDev->CreateInputLayout(aied_obj, 7, pblob_obj->GetBufferPointer(), pblob_obj->GetBufferSize(), &pilObject)) &&
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

		int i_bound_mesh = -1, i_bound_tex = -1, i_base = 0, i_first = 0;
		int i_state = -1;
		for (int k = 0; k < (int)aiOrder.size(); k++)
		{
			int i = aiOrder[k];
			const SVRRDraw& drw = adrw[i];
			if (b_shadow && (drw.u4Flags & evrrNO_SHADOW))
				continue;

			if (drw.iMesh != i_bound_mesh)
			{
				if (!bBindMesh(drw.iMesh, i_base, i_first))
					continue;
				i_bound_mesh = drw.iMesh;
			}

			bool b_alpha = (drw.u4Flags & evrrALPHA_TEST) != 0;
			bool b_two = (drw.u4Flags & evrrTWO_SIDED) != 0;
			int i_new_state = (b_alpha ? 1 : 0) | (b_two ? 2 : 0);
			if (i_new_state != i_state)
			{
				i_state = i_new_state;
				if (b_shadow)
					pCtx->PSSetShader(b_alpha ? ppsShadowAlpha : nullptr, nullptr, 0);
				else
				{
					pCtx->RSSetState(b_two ? prsNone : prsBack);
					pCtx->OMSetBlendState(b_alpha && iMSAA > 1 ? pbsA2C : pbsOpaque, nullptr, 0xFFFFFFFF);
				}
			}

			if (drw.iTexture != i_bound_tex && (!b_shadow || b_alpha))
			{
				ID3D11ShaderResourceView* psrv = psrvTex(drw.iTexture);
				pCtx->PSSetShaderResources(0, 1, &psrv);
				i_bound_tex = drw.iTexture;
			}

			pCtx->DrawIndexedInstanced(drw.iNumIndices, 1, i_first + drw.iFirstIndex, i_base, i);
		}
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
	SafeRelease(prtvAtlas);
	SafeRelease(psrvAtlas);
	SafeRelease(ptexAtlas);
	bAtlasBuilt = false;
	adrwDecal.clear();
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
		prsBack, prsNone, prsShadow, prsAtlas, pdsTest, pdsOff, pdsShadow, pbsOpaque, pbsA2C, pssWrap, pssShadow, pssClamp,
		pcbEye, pcbFrame
	};
	for (ID3D11DeviceChild* p : ap)
		if (p)
			p->Release();
	pvsObject = nullptr; ppsObject = nullptr; pvsTerrain = nullptr; ppsTerrain = nullptr;
	pvsAtlasTerrain = nullptr; ppsAtlasTerrain = nullptr; pvsAtlasDecal = nullptr; ppsAtlasDecal = nullptr;
	pvsSky = nullptr; ppsSky = nullptr; pvsShadowObject = nullptr; ppsShadowAlpha = nullptr; pvsShadowTerrain = nullptr;
	pilObject = nullptr; pilTerrain = nullptr;
	prsBack = nullptr; prsNone = nullptr; prsShadow = nullptr; prsAtlas = nullptr;
	pdsTest = nullptr; pdsOff = nullptr; pdsShadow = nullptr; pbsOpaque = nullptr; pbsA2C = nullptr;
	pssWrap = nullptr; pssShadow = nullptr; pssClamp = nullptr; pcbEye = nullptr; pcbFrame = nullptr;

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
void VRR_TerrainAddDecal(const SVRRDraw& drw)
{
	if (bReady && drw.iNumIndices > 0)
		adrwDecal.push_back(drw);
}

//**********************************************************************************************
bool VRR_HasTerrain()
{
	return pvbTerrain != nullptr;
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
	adrwDecal.clear();
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
			std::memcpy(ainst[i].afRow, adrw[i].afWorld, sizeof(ainst[i].afRow));
			ainst[i].u4Flags = adrw[i].u4Flags;
		}
		if (pvbInst)
			bUpload(pvbInst, ainst.data(), sizeof(SInstance) * ainst.size());
	}

	// Opaque first, then see-through; grouped by mesh and texture.
	aiOrder.resize(adrw.size());
	for (size_t i = 0; i < adrw.size(); i++)
		aiOrder[i] = (int)i;
	std::sort(aiOrder.begin(), aiOrder.end(), [](int a, int b)
	{
		const SVRRDraw& da = adrw[a];
		const SVRRDraw& db = adrw[b];
		uint32_t ka = da.u4Flags & (evrrALPHA_TEST | evrrTWO_SIDED), kb = db.u4Flags & (evrrALPHA_TEST | evrrTWO_SIDED);
		if (ka != kb)
			return ka < kb;
		if (da.iMesh != db.iMesh)
			return da.iMesh < db.iMesh;
		return da.iTexture < db.iTexture;
	});

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
		pCtx->ClearDepthStencilView(pdsvDepth, D3D11_CLEAR_DEPTH, 0.0f, 0);
		D3D11_VIEWPORT vp = { 0, 0, (float)iWidth, (float)iHeight, 0, 1 };
		pCtx->RSSetViewports(1, &vp);

		ID3D11ShaderResourceView* apsrv[3] = { psrvTex(fr.iSkyTexture), b_shadows ? psrvShadow : nullptr, bAtlasBuilt ? psrvAtlas : nullptr };
		pCtx->PSSetShaderResources(0, 3, apsrv);

		// Sky.
		pCtx->RSSetState(prsNone);
		pCtx->OMSetDepthStencilState(pdsOff, 0);
		pCtx->OMSetBlendState(pbsOpaque, nullptr, 0xFFFFFFFF);
		pCtx->IASetInputLayout(nullptr);
		pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		pCtx->VSSetShader(pvsSky, nullptr, 0);
		pCtx->PSSetShader(ppsSky, nullptr, 0);
		pCtx->Draw(3, 0);

		// Terrain.
		pCtx->OMSetDepthStencilState(pdsTest, 0);
		pCtx->RSSetState(prsBack);
		pCtx->VSSetShader(pvsTerrain, nullptr, 0);
		pCtx->PSSetShader(ppsTerrain, nullptr, 0);
		DrawTerrain();

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
void VRR_GetStats(int& ri_draws, int& ri_textures, int& ri_meshes, float& rf_gpu_mb)
{
	ri_draws = iLastDraws;
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
