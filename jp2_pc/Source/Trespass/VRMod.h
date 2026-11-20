/**********************************************************************************************
 *
 * VRMod.h
 *
 * Trespasser VR: 6DOF head tracking, stereo rendering and motion controls via SteamVR.
 *
 **********************************************************************************************/

#pragma once

#include <functional>

// Starts VR if enabled in TrespasserVR.ini and a headset is present.  Safe to call repeatedly;
// only the first call does anything.
// Call before the display is set up: applies the VR mod's renderer choice (hardware/software).
void VRMod_PreInit();

void VRMod_Startup();

void VRMod_Shutdown();

// A level is about to load (scene file name, e.g. "be.scn") / has finished loading.
void VRMod_LevelLoading(const char* str_scene);
void VRMod_LevelLoaded();

// True while the game is being rendered to a headset.
bool VRMod_Active();

// Renders the 3D scene once per eye.  paint_scene must paint the world and end the frame
// (gmlGameLoop.Paint() + EndFrame), leaving the image in the back buffer.
// Returns false if VR is not active, in which case the caller should paint normally.
bool VRMod_RenderStereo(const std::function<void()>& paint_scene);

// VR settings menu (pause menu): read a TrespasserVR.ini value, set one, and apply the file.
float VRMod_IniGet(const char* str_key, float f_default);
void VRMod_IniSet(const char* str_key, float f_value);
void VRMod_IniApply();

// Writes a line to the VR log.
void VRMod_Note(const char* str);
