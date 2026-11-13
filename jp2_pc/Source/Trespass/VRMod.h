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

// True while the game is being rendered to a headset.
bool VRMod_Active();

// Renders the 3D scene once per eye.  paint_scene must paint the world and end the frame
// (gmlGameLoop.Paint() + EndFrame), leaving the image in the back buffer.
// Returns false if VR is not active, in which case the caller should paint normally.
bool VRMod_RenderStereo(const std::function<void()>& paint_scene);
