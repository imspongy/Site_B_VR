/**********************************************************************************************
 *
 * VRShared.hpp
 *
 * State shared between the VR mod (Trespass/VRMod.cpp) and the game libraries
 * (input deemone, player).  Plain data only, so that every library can include it without
 * pulling in OpenVR or Direct3D 11.
 *
 * All vectors and quaternions are in GAME world space (x right/east, y forward/north, z up,
 * metres).  Quaternions are stored as (w, x, y, z) with the engine's CRotate3 semantics.
 *
 **********************************************************************************************/

#pragma once

struct SInput;

struct SVRShared
{
	// True while a VR session is running and the game is rendering to the headset.
	bool  bActive = false;

	// Headset orientation in world space (includes snap/smooth turn).  Valid when bHeadValid.
	bool  bHeadValid = false;
	float afHeadRotWorld[4] = { 1, 0, 0, 0 };

	// Right (weapon) hand controller in world space.  Valid when bHandValid.
	bool  bHandValid = false;
	float afHandPosWorld[3] = { 0, 0, 0 };
	float afHandRotWorld[4] = { 1, 0, 0, 0 };

	// Motion-controlled hand enabled (ini: MotionHand).
	bool  bMotionHand = true;

	// Player is holding an object (trigger fires/uses instead of grabbing).
	bool  bHolding = false;

	//
	// Written by the VR mod, read by the player.
	//

	// Finger curl request 0 (open) .. 1 (fist), used when the hand is empty.
	float fFingerCurl = 0;

	// Holster gesture: grip was just pressed with the controller near a holster.
	// -1 = none, 0 = hip (small items), 1 = back (large items).  Player clears it.
	int   iHolsterRequest = -1;

	// Holster positions (world space), published by the player for the VR mod's proximity test.
	float aafHolsterPosWorld[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };

	// Force grab: aiming (trigger held, empty hand) and pull (flick detected this frame).
	bool  bForceAim  = false;
	bool  bForcePull = false;
	// Pointing ray for force grab (world space).
	float afAimPosWorld[3] = { 0, 0, 0 };
	float afAimDirWorld[3] = { 0, 1, 0 };

	//
	// Written by the player, read by the VR mod (feedback).
	//

	bool  bForceTarget = false;		// Something is targeted for force grab.
	int   iForceTargetSerial = 0;	// Changes when the target changes (haptic tick).
	float fHeldMass = 0;			// Mass of held object, kg (0 if none).
	int   iShotCount = 0;			// Incremented when the held gun fires.
	int   iPickupCount = 0;			// Incremented when something is picked up.
};

extern SVRShared gVR;

// Hook called by CInputDeemone after reading keyboard/mouse, so the VR mod can merge
// controller input into the control message.  Null when no VR mod is running.
typedef void (*TPFNVRInputHook)(SInput& tin);
extern TPFNVRInputHook gpfnVRInputHook;

// Hook called at the start of CRasterWin::Flip(), so the VR mod can show menus/loading screens
// on a virtual screen when the 3D view isn't being rendered.
typedef void (*TPFNVRFlipHook)();
extern TPFNVRFlipHook gpfnVRFlipHook;
