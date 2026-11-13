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
	float afV[3] = { 0, 0, 0 };		// Velocity (m/s), controllers only.
};

struct SVRFov
{
	// Tangents of the half angles.  Left and down are negative.
	float fTanLeft = -1, fTanRight = 1, fTanUp = 1, fTanDown = -1;
};

// Raw controller state for one hand.  Button names follow the physical layout:
// "lower" = A / X / Cross / Square, "upper" = B / Y / Circle / Triangle.
struct SVRHandInput
{
	float fStickX = 0, fStickY = 0;
	bool  bStickClick = false;
	bool  bGrip = false;
	float fGrip = 0;
	bool  bTrigger = false;
	float fTrigger = 0;
	bool  bLower = false;
	bool  bUpper = false;
	bool  bMenu = false;
};

struct SVRControls
{
	SVRHandInput ahand[2];			// 0 = left, 1 = right.
};

struct SVRFrame
{
	SVRPose     posHead;
	SVRPose     aposEye[2];			// 0 = left, 1 = right.
	SVRFov      afovEye[2];
	SVRPose     aposHand[2];		// Grip poses, 0 = left, 1 = right.
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

// Rumble on a controller (0 = left, 1 = right).
void VRB_Haptic(int i_hand, float f_seconds, float f_frequency, float f_amplitude);

// Menu laser pointer events on the overlay (pixel coordinates, top-left origin).
struct SVROverlayMouse
{
	enum EType { eMOVE, eDOWN, eUP } eType;
	float fX, fY;
};

// Fetches the next laser-pointer event on the menu screen.  Returns false when none are left.
bool VRB_PollOverlayMouse(SVROverlayMouse& ev);

// SteamVR's name for the connected controllers (for the log), e.g. "oculus_touch".
const char* VRB_ControllerType();

// True when SteamVR asked the application to quit.
bool VRB_QuitRequested();
