/**********************************************************************************************
 *
 * VRScene.h
 *
 * Site B VR: feeds the game world to the GPU renderer (VRRender).  Converts the game's meshes,
 * skinned characters, textures, terrain, sky, lights and fog, and draws them for both eyes.
 *
 **********************************************************************************************/

#pragma once

struct SVRSceneEyes
{
	float afPos[2][3];		// World positions of the eyes.
	float afAxes[2][9];		// Right, forward, up of each eye (world).
	float afTan[2][4];		// Field of view tangents: left, right, down, up.
};

struct SVRSceneSettings
{
	float fFogScale = 1.3f;
	bool  bBumpMaps = true;		// Light bump-mapped surfaces with their relief.		// The game's fog, spread over this many times its view distance.
	float fDrawDistance = 1.0f;		// Multiplier on the game's view distance.
	float fShadowDistance = 60.0f;	// Metres of real-time shadows around the player (0 = off).
	float fBrightness = 1.0f;
	float fTerrainDetail = 1.0f;	// Terrain grid density and texture resolution.
};

// Starts the renderer (after the VR backend).  Eye images are i_width x i_height.
bool VRScene_Init(int i_width, int i_height, int i_msaa, int i_shadow_size, char* str_err, int i_err_len);
void VRScene_Shutdown();
bool VRScene_Ready();

// Draws the world for both eyes and submits the images to the headset.
bool VRScene_RenderAndSubmit(const SVRSceneEyes& eyes, const SVRSceneSettings& set);

// One line of statistics for the log.

// After an error part-way through a frame: drop whatever was queued for it.
void VRScene_AbortFrame();
const char* VRScene_Stats();
