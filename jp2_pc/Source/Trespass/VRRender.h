/**********************************************************************************************
 *
 * VRRender.h
 *
 * Site B VR: modern GPU renderer (Direct3D 11).
 *
 * Draws the game world straight into the headset's eye textures on the graphics card, at the
 * headset's resolution, with multisample anti-aliasing, real-time sun shadows and per-pixel
 * fog.  It replaces the game's 1998 renderer while in VR.
 *
 * This interface uses plain data only (no game or Direct3D headers), so that the game-side code
 * (VRScene.cpp) and the Direct3D side (VRRender.cpp) stay separate.
 *
 * Coordinates are the game's world space: x east, y north, z up, metres.
 *
 **********************************************************************************************/

#pragma once

#include <cstdint>

// A mesh vertex.
struct SVRRVertex
{
	float afPos[3];
	float afNormal[3];
	float afUV[2];
};

// A terrain grid vertex.
struct SVRRTerrainVertex
{
	float    afPos[3];
	float    afNormal[3];
	uint32_t u4Colour;		// 0xAABBGGRR.
};

// Flags for a draw.
enum
{
	evrrALPHA_TEST = 1,		// Texture has see-through texels.
	evrrTWO_SIDED  = 2,		// Draw both sides (also used for mirrored instances).
	evrrNO_SHADOW  = 4,		// Doesn't cast a shadow.
	evrrUNLIT      = 8,		// Full brightness.
	evrrNO_FOG     = 16,
};

// One draw: part of a mesh, with a texture and a placement.
struct SVRRDraw
{
	int      iMesh;			// Mesh handle (static or dynamic).
	int      iFirstIndex;
	int      iNumIndices;
	int      iTexture;		// Texture handle (0 = white).
	float    afWorld[12];	// Object -> world: rows (x', y', z') of a 3x4 matrix.
	uint32_t u4Flags;
};

// Per frame parameters.
struct SVRRFrame
{
	// Eyes: position, axes (right, forward, up) and field of view tangents (left, right, down, up).
	float afEyePos[2][3];
	float afEyeAxes[2][9];
	float afEyeTan[2][4];
	float fNear;

	// Lighting.
	float afSunDir[3];		// Toward the sun.
	float fSun;				// Sun brightness 0..1.
	float fAmbient;			// Ambient brightness 0..1.
	float fBrightness;		// Overall multiplier.

	// Fog.
	float afFogColour[3];	// 0..1.
	float fFogFar;			// Distance at which the fog table ends (metres).
	float afFogTable[32];	// Fog amount 0..1 at distance i/31 * fFogFar.

	// Sky.
	int   iSkyTexture;		// 0 = no texture (fog colour only).
	float fSkyHeight;		// Height of the cloud layer above the eye (metres).
	float fSkyUVScale;		// World metres -> texture coordinates.
	float afSkyOffset[2];	// Texture coordinate offset.
	float fSkyFogNear;		// Fog on the sky: amount = (1 - dir.z - near) * scale.
	float fSkyFogScale;
	float afSkyFogColour[3];

	// Shadows: centred here.
	float afShadowCentre[3];
	float fShadowRadius;	// Metres; 0 = no shadows.
};

// Starts the renderer on the given Direct3D 11 device (from VRBackend).  Eye images are
// i_width x i_height with i_msaa samples (1, 2, 4 or 8).
bool  VRR_Init(void* pv_device, void* pv_context, int i_width, int i_height, int i_msaa, int i_shadow_size, char* str_err, int i_err_len);
void  VRR_Shutdown();
bool  VRR_Ready();

// Releases all meshes, textures and the terrain (new level).
void  VRR_ClearAll();

// Textures: RGBA8 pixels (0xAABBGGRR), mipmapped on the card.  Returns a handle (> 0), or 0.
int   VRR_TextureCreate(int i_width, int i_height, const uint32_t* pu4_pixels);

// Static meshes.  Returns a handle (> 0), or 0.
int   VRR_MeshCreate(const SVRRVertex* pv, int i_verts, const uint32_t* pu4_indices, int i_indices);

// A mesh that only exists for this frame (skinned characters).  Returns a handle, or 0.
int   VRR_MeshDynamic(const SVRRVertex* pv, int i_verts, const uint32_t* pu4_indices, int i_indices);

// Queues a draw for this frame.
void  VRR_AddDraw(const SVRRDraw& drw);

// Terrain: a regular grid (i_nx * i_ny vertices, row by row along x) and an "atlas" texture
// covering the same area, painted from above with the base colours and the decal draws given
// to VRR_TerrainAddDecal (in order: later ones on top).  Call VRR_TerrainBuildAtlas once after.
bool  VRR_TerrainSet(int i_nx, int i_ny, float f_x0, float f_y0, float f_spacing, const SVRRTerrainVertex* ptv, int i_atlas_size);
void  VRR_TerrainAddDecal(const SVRRDraw& drw);
void  VRR_TerrainBuildAtlas();
bool  VRR_HasTerrain();

// Draws the queued draws for both eyes.  Then the eye images are ready for submission.
void  VRR_RenderFrame(const SVRRFrame& fr);

// The finished eye image (an ID3D11Texture2D*).
void* VRR_EyeTexture(int i_eye);

// Debug snapshot of the left eye, i_size x i_size, as 0x00RRGGBB.  The copy is made on the
// card now and read back on the next call (so the game never waits for the card); returns
// false if there was no earlier copy to read.
bool  VRR_CaptureEye(int i_size, uint32_t* pu4_out);

// Statistics for the log.
void  VRR_GetStats(int& ri_draws, int& ri_textures, int& ri_meshes, float& rf_gpu_mb);
