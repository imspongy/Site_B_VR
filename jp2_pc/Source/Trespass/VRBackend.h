/**********************************************************************************************
 *
 * VRBackend.h
 *
 * Thin wrapper around OpenVR (SteamVR) + Direct3D 11.  Deliberately knows nothing about the
 * game: it hands out headset/controller poses in OpenVR tracking space and accepts finished
 * eye images as CPU pixel buffers (the game renders with DirectX 7, which cannot share
 * textures with modern APIs, so frames are copied through system memory).
 *
 * OpenVR tracking space: right-handed, +x right, +y up, -z forward, metres.
 *
 **********************************************************************************************/

#pragma once

#include <cstdint>

struct SVRPose
{
	bool  bValid = false;
	float afQ[4] = { 1, 0, 0, 0 };	// Orientation (w, x, y, z).
	float afP[3] = { 0, 0, 0 };		// Position.
};

struct SVRFov
{
	// Tangents of the half angles.  Left and down are negative.
	float fTanLeft = -1, fTanRight = 1, fTanUp = 1, fTanDown = -1;
};

struct SVRControls
{
	float fMoveX = 0, fMoveY = 0;	// Locomotion stick.
	float fTurnX = 0, fTurnY = 0;	// Turn stick.

	bool bHand       = false;		// Right grip: raise / motion-control the hand.
	bool bTrigger    = false;		// Right trigger: grab when empty, use/fire when holding.
	bool bDrop       = false;		// Right A: grab / drop.
	bool bJump       = false;		// Right B.
	bool bCrouch     = false;		// Left grip.
	bool bThrow      = false;		// Left trigger.
	bool bStow       = false;		// Left Y.
	bool bPunch      = false;		// Left X.
	bool bRecenter   = false;		// Left stick click.
	bool bRun        = false;		// Right stick click (toggle handled by the mod).
	bool bMenu       = false;		// Left menu button.
};

struct SVRFrame
{
	SVRPose     posHead;
	SVRPose     aposEye[2];			// 0 = left, 1 = right.
	SVRFov      afovEye[2];
	SVRPose     posHandRight;		// Grip pose.
	SVRPose     posHandLeft;
	SVRControls ctl;
};

// Starts SteamVR.  Returns false (with a reason in str_err) if no headset/runtime.
bool VRB_Init(int* pi_eye_width, int* pi_eye_height, char* str_err, int i_err_len);

void VRB_Shutdown();

bool VRB_IsRunning();

// Waits for the compositor, then fetches predicted poses and controller state.
// Returns false if the frame should be skipped (e.g. SteamVR is quitting).
bool VRB_BeginFrame(SVRFrame& frame);

// Uploads one eye image and submits it.  i_bits = 16 or 32; the masks describe the pixel.
bool VRB_SubmitEye
(
	int i_eye,
	const void* pv_pixels, int i_pitch_bytes, int i_width, int i_height,
	int i_bits, uint32_t u4_red_mask, uint32_t u4_green_mask, uint32_t u4_blue_mask
);

// Shows the given image on a floating screen in front of the player (for menus).
// b_place = true positions it in front of the current head direction.
void VRB_ShowOverlay
(
	const void* pv_pixels, int i_pitch_bytes, int i_width, int i_height,
	int i_bits, uint32_t u4_red_mask, uint32_t u4_green_mask, uint32_t u4_blue_mask,
	float f_width_m, float f_distance_m, bool b_place
);

void VRB_HideOverlay();

// Short rumble on the right (b_right) or left controller.
void VRB_Haptic(bool b_right, float f_seconds, float f_amplitude);

// True when SteamVR asked the application to quit.
bool VRB_QuitRequested();
