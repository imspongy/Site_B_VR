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
 * Hands: index 0 = right, 1 = left.  In VR both hands are handled by the mod (not the game's
 * original arm physics): each has a small physics box that pushes things, objects are held
 * exactly at the controller, and the arms are drawn with IK to the controllers.
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

	// VR hands enabled (ini: MotionHand).  When off, the right hand works like the original.
	bool  bMotionHand = true;
	bool  bLeftArm = true;			// Second arm enabled (ini: SecondHand).
	float fCurlSign = 1.0f;			// Debug: flips finger curl direction on the arms.

	// Finger curl for the game's own (physics) hand; unused in VR, kept at 0.
	float fFingerCurl = 0;

	//
	// Written by the VR mod, read by the player.  [0] = right, [1] = left.
	//
	bool  abHandValid[2] = { false, false };
	float aafHandPos[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };		// Controller (grip) position.
	float aafHandRot[2][4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };	// Hand orientation (aim adjusted).
	float aafHandVel[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };		// Controller velocity.
	float afCurl[2] = { 0, 0 };				// 0 open .. 1 fist.
	bool  abGrip[2] = { false, false };		// Grip held.
	bool  abTrigger[2] = { false, false };	// Trigger held.
	int   aiHolsterRequest[2] = { -1, -1 };	// Grip pressed (empty) or released (holding) at holster.

	// Holster positions (world space) on Anne's right side, published by the player.
	float aafHolsterPosWorld[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };

	// Force grab per hand: aiming (trigger held, empty hand), pull (flick this frame), ray.
	bool  abForceAim[2]  = { false, false };
	bool  abForcePull[2] = { false, false };
	float aafAimPosWorld[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
	float aafAimDirWorld[2][3] = { { 0, 1, 0 }, { 0, 1, 0 } };

	// Two-handed gun: 0 = none, 1 = left hand supports the right hand's gun,
	// 2 = right hand supports the left hand's gun.
	int   iTwoHand = 0;
	bool  bTwoHandPistol = false;	// Support hand cups the bottom of the grip.

	// Where Anne's body should be (under the headset), world space.
	bool  bBodyTargetValid = false;
	float afBodyTarget[3] = { 0, 0, 0 };

	// Instances: the mirrored second arm (flipped culling) and the hand boxes (bullets pass).
	const void* pvMirrorInstance = nullptr;
	const void* apvHandBox[2] = { nullptr, nullptr };

	//
	// Written by the player, read by the VR mod (feedback).
	//
	bool  abHolding[2] = { false, false };
	bool  abHoldingGun[2] = { false, false };
	float afHeldMass[2] = { 0, 0 };
	bool  abForceTarget[2] = { false, false };
	int   aiForceTargetSerial[2] = { 0, 0 };
	int   aiShotCount[2] = { 0, 0 };
	int   aiPickupCount[2] = { 0, 0 };
	int   iShotCount = 0;				// Incremented by any gun firing (Gun.cpp).

	// Writes a line to TrespasserVR.log (null when the VR mod isn't running).
	void (*pfnLog)(const char* str_fmt, ...) = nullptr;

	// Performance diagnostics: accumulated milliseconds, read and reset by the VR mod.
	double dFlipMs = 0;				// Showing frames on the monitor.
	double dStepMs = 0;				// Game simulation steps.
	int    iSteps = 0;

	// Skip drawing to the monitor while in VR (ini: MirrorToMonitor=0).
	bool  bSkipMonitor = false;
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
