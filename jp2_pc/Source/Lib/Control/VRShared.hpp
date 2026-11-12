/**********************************************************************************************
 *
 * VRShared.hpp
 *
 * State shared between the VR mod (Trespass/VRMod.cpp) and the game libraries
 * (input deemone, player, renderer).  Plain data only, so that every library can include it
 * without pulling in OpenVR or Direct3D 11.
 *
 * All vectors and quaternions are in GAME world space (x right/east, y forward/north, z up,
 * metres).  Quaternions are stored as (w, x, y, z) with the engine's CRotate3 semantics.
 *
 * Hands: index 0 = Anne's real (right) arm, driven by the right controller through the game's
 * own arm physics.  Index 1 = the second (left) arm, driven by the left controller through a
 * physics hand box added by the mod.
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

	// Right controller (Anne's real arm), world space.
	bool  bHandValid = false;
	float afHandPosWorld[3] = { 0, 0, 0 };
	float afHandRotWorld[4] = { 1, 0, 0, 0 };

	// Motion-controlled hand enabled (ini: MotionHand).
	bool  bMotionHand = true;

	// Right hand is holding an object (trigger fires/uses instead of grabbing).
	bool  bHolding = false;

	//
	// Written by the VR mod, read by the player.
	//

	// Right-hand finger curl request 0 (open) .. 1 (fist), used when the hand is empty.
	float fFingerCurl = 0;

	// Right-hand holster gesture: -1 = none, 0 = hip, 1 = back.  Player clears it.
	int   iHolsterRequest = -1;

	// Holster positions (world space), published by the player for the VR mod's proximity test.
	float aafHolsterPosWorld[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };

	// Force grab, per hand (0 = right, 1 = left): aiming (trigger held, empty hand) and pull
	// (flick detected this frame), with the pointing ray in world space.
	bool  abForceAim[2]  = { false, false };
	bool  abForcePull[2] = { false, false };
	float aafAimPosWorld[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
	float aafAimDirWorld[2][3] = { { 0, 1, 0 }, { 0, 1, 0 } };

	// Left controller (second arm), world space.
	bool  bLeftValid = false;
	float afLeftPosWorld[3] = { 0, 0, 0 };
	float afLeftRotWorld[4] = { 1, 0, 0, 0 };
	float afLeftVelWorld[3] = { 0, 0, 0 };
	float fLeftCurl = 0;			// 0 open .. 1 fist.
	bool  bLeftGrip = false;		// Left grip held.
	bool  bLeftTrigger = false;		// Left trigger held.
	int   iLeftHolsterRequest = -1;	// Left grip pressed (empty) or released (holding) at a holster.
	bool  bLeftArm = true;			// Second arm enabled (ini: SecondHand).
	float fCurlSign = 1.0f;			// Debug: flips the second hand's finger curl direction.

	// Two-handed gun: 0 = none, 1 = left hand supports the right hand's gun,
	// 2 = right hand supports the left hand's gun.
	int   iTwoHand = 0;
	bool  bTwoHandPistol = false;	// The supported gun is a pistol (support hand under the grip).

	// The instance rendered mirrored (the second arm); its polygons have reversed winding.
	const void* pvMirrorInstance = nullptr;

	// The left hand's physics box (bullets pass through it).
	const void* pvLeftHandBox = nullptr;

	//
	// Written by the player, read by the VR mod (feedback).
	//

	bool  bHoldingGun = false;		// Right hand holds a gun.
	bool  bLeftHolding = false;		// Left hand holds something.
	bool  bLeftHoldingGun = false;	// Left hand holds a gun.
	float fHeldMass = 0;			// Mass of the right hand's object, kg (0 if none).
	float fLeftHeldMass = 0;		// Mass of the left hand's object, kg (0 if none).
	bool  abForceTarget[2] = { false, false };		// Something is targeted for force grab.
	int   aiForceTargetSerial[2] = { 0, 0 };		// Changes when the target changes (haptic tick).
	int   iShotCount = 0;			// Incremented when a gun in the right hand fires.
	int   iLeftShotCount = 0;		// Incremented when a gun in the left hand fires.
	int   iPickupCount = 0;			// Incremented when the right hand picks something up.
	int   iLeftPickupCount = 0;		// Incremented when the left hand picks something up.
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
