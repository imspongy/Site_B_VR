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
	float fHandScale = 0.75f;		// Size of the drawn hands relative to Anne's original hand.
	bool  bLeftArmTest = false;		// Debug: draw the second arm unmirrored next to the right hand.
	bool  bRadialFog = false;		// Fog by true distance (same in every direction).
	float afFogMat[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };	// Normalised camera -> camera-relative world (rows).
	float afFogFwd[3] = { 0, 1, 0 };	// Camera forward (world).

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
	bool  abHolsterFull[4] = { false, false, false, false };	// Right hip, right shoulder, left hip, left shoulder.
	const void* apvHolstered[4] = { nullptr, nullptr, nullptr, nullptr };	// What's in them (no knocking about while carried).
	bool  bHolsterSound = false;		// The holster knock is being sent (let it through).

	// Force grab per hand: aiming (trigger held, empty hand), pull (flick this frame), ray.
	bool  abForceAim[2]  = { false, false };
	bool  abForcePull[2] = { false, false };
	float aafAimPosWorld[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
	float aafAimDirWorld[2][3] = { { 0, 1, 0 }, { 0, 1, 0 } };

	// Two-handed gun: 0 = none, 1 = left hand supports the right hand's gun,
	// 2 = right hand supports the left hand's gun.
	int   iTwoHand = 0;
	bool  bTwoHandPistol = false;	// Support hand cups the bottom of the grip.
	float fGunBack = 0.04f;			// Guns sit this far back from the controller's grip point (m).
	float fGunSide = -0.05f;			// ... and this far across toward the middle of the palm (m).
	float fGunSideOther = 0.01f;		// The same for every gun but the rifle.
	float fPistolSide = 0.01f;			// Pistols.
	float fShotgunSide = 0.04f;			// The first shotgun (8 shells) ...
	float fShotgunUp = -0.01f;			// ... and how far up it sits along its own up (m).
	float fShotgun2Side = 0.01f;		// The level 2 shotgun (7 shells) ...
	float fShotgun2Up = 0.09f;			// ... and its height.
	// Per kind of gun (0 pistol, 1 shotgun, 2 shotgun 2, 3 rifle, 4 other long gun) and per hand
	// (0 right, 1 left): across toward the body's middle, and up (m).
	float aafGunAcross[5][2] = { { 0.01f, 0.01f }, { 0.04f, 0.04f }, { 0.01f, 0.01f }, { -0.05f, -0.05f }, { 0.01f, 0.01f } };
	float aafGunUp[5][2] = {};
	float aafGunLateral[5][2] = {};		// Left/right along the gun's own sideways direction (set with GunAdjust).
	int   aiHeldGunKind[2] = { -1, -1 };
	float aafGunAdjPending[2][2] = {};		// Stick adjustments not yet applied to the held gun.
	bool  abHoldingMelee[2] = { false, false };
	float fMeleeTiltDeg = 45.0f;		// Bats and pipes tipped up in the hand.
	float fMeleeScale = 1.0f;			// Melee hits (things swung in the hand) hurt this much more.
	float fMeleeDrawScale = 0.85f;		// Bats and pipes drawn this size in the hand.
	float fArmScale = 0.85f;			// Her arms are drawn this size (shorter and slimmer).
	float fGunScale = 0.85f;		// Held guns are drawn this size (her hands are small).

	// Where Anne's body should be (under the headset), world space.
	bool  bBodyTargetValid = false;
	float afBodyTarget[3] = { 0, 0, 0 };
	// VR character controller (Player.cpp): Anne's physics body is pinned to this each step.
	const void* pvKinInstance = nullptr;	// The player, while pinned.
	float afKinPos[3] = { 0, 0, 0 };		// Body origin.
	float afKinVel[3] = { 0, 0, 0 };
	bool  bKinGrounded = true;			// Controller: standing on something.
	bool  bJump = false;				// Jump button held.
	bool  bCrouch = false;				// Crouch toggled on.
	float fJumpHeight = 1.1f;			// Metres.
	float fBodyZShift = 0;				// Drawn body height minus physics body height (view smoothing).
	int   iPlayerPelvis = -1;			// Physics skeleton index of the player.
	float fJumpScale = 1.4f;			// Jump take-off speed multiplier.
	const void* pvStubInstance = nullptr;		// Instance whose flagged polygons are not drawn (Anne's arm stub).
	const unsigned char* pu1StubPolys = nullptr;	// One flag per mesh polygon.
	int   iStubPolys = 0;
	float fCullScaleSmallSqr = 1.0f;	// The same for small objects (bounding radius under ~0.7 m).
	float fCullScaleSqr = 1.0f;			// Multiplier on every object's (squared) draw distance.
	float fFallSpeed = 0;				// Anne's recent downward speed (m/s), for fall damage.
	float afBodyVel[2] = { 0, 0 };		// How fast the player is moving (stick), world x/y.

	// Instances: the mirrored second arm (flipped culling) and the hand boxes (bullets pass).
	const void* pvMirrorInstance = nullptr;
	int   iMirrorPolys = 0;			// Diagnostics: polygons of the mirrored arm seen / facing the eye.
	int   iMirrorPolysFront = 0;
	const void* apvHandBox[2] = { nullptr, nullptr };

	//
	// Written by the player, read by the VR mod (feedback).
	//
	bool  abHolding[2] = { false, false };
	bool  abHoldingGun[2] = { false, false };
	float afHeldLength[2] = { 0, 0 };
	float fKinFeetZ = 0;			// The character controller's feet (world height).
	float fBodyAboveFeet = 0;		// Her body origin's height above her feet (m).
	bool  bBodyFromView = true;		// Her visible body is placed so her head is at your eyes.
	float afBodyDrawShift[3] = { 0, 0, 0 };	// Her visible body relative to her physics body.
	float afBodyDrawnPos[3] = { 0, 0, 0 };	// Where her visible body is (world).
	bool  bBodyDrawnPosValid = false;
	float afBodyDrawnPlace[7] = { 0, 0, 0, 1, 0, 0, 0 };	// Her visible body: position, rotation (c x y z).
	float aafStowRel[4][7] = {};		// Each holstered thing relative to her body.
	bool  bViewEyeValid = false;
	float fViewEyeZ = 0;			// Your eyes (world height).
	float fHeadBelowEye = 0.07f;	// Her head joint sits this far below your eyes (m).
	float fKinCrouch = 0;			// How far the crouch button has lowered her (m).
	bool  bLogBodyRequest = false;	// A capture asks the body for its numbers.
	float fRealCrouch = 0;			// How far below your standing height your head is now (m).
	bool  bKinOnFloor = false;
	float fBodyScale = 1.0f;		// Anne's body is drawn this much bigger, to match your height.
	bool  bDamageGrace = false;		// Just after a level loads: Anne can't be hurt yet.
	bool  bSubtitles = true;			// Voice-over subtitles in the headset.
	float fShotgunSupportUp = 0.03f;	// The supporting hand on the shotgun is drawn this much higher (m).
	float fShotgun1SupportUp = 0.01f;	// The same on the first shotgun (8 shells).
	float aafCtrlUpInHand[2][3] = { { 0, 0, 1 }, { 0, 0, 1 } };	// The controller's up, in the game's hand frame.
	float fRifleSupportDown = 0.03f;	// ...and on the rifle this much lower.
	float aafHandVelPeak[2][3] = {};	// Fastest hand velocity over the last ~0.1 s (throwing).
	float fThrowScale = 2.0f;			// Throws go this much faster than the hand.
	// What each hand holds firmly, and where it sits relative to the controller (position,
	// then rotation as a quaternion c, x, y, z).  The renderer draws it at the controller's
	// latest pose, so it doesn't judder between physics steps.
	const void* apvHeldFirm[2] = { nullptr, nullptr };
	float afHeldGrab[2][7] = {};	// Longest side of what each hand holds (m): long guns get a fore grip.
	float afHeldMass[2] = { 0, 0 };
	bool  abForceTarget[2] = { false, false };
	int   aiForceTargetSerial[2] = { 0, 0 };
	int   aiShotCount[2] = { 0, 0 };
	int   aiPickupCount[2] = { 0, 0 };
	int   iShotCount = 0;				// Incremented by any gun firing (Gun.cpp).

	// Writes a line to TrespasserVR.log (null when the VR mod isn't running).
	void (*pfnLog)(const char* str_fmt, ...) = nullptr;
	// The game's on-screen text (tutorial, subtitles, hints), shown in the headset instead.
	// i_type: 1 unknown, 2 subtitle, 3 tutorial; b_chained: follows the previous line.
	void (*pfnShowText)(const char* str_text, float f_secs, bool b_chained, int i_type) = nullptr;
	void (*pfnRemoveText)(int i_type) = nullptr;		// 0 = all.

	// Performance diagnostics: accumulated milliseconds, read and reset by the VR mod.
	double dFlipMs = 0;				// Showing frames on the monitor.
	double dStepMs = 0;				// Game simulation steps.
	int    iSteps = 0;

	// Menu laser pointer position, in game screen pixels (independent of the monitor).
	bool  bMenuCursor = false;
	int   aiMenuCursor[2] = { 0, 0 };

	// Graphics-card rendering speed-ups (ini: FastDraw, HardwareBumpObjects).
	bool  bBatchDraw = false;			// Collect polygons into large draw calls.
	bool  bBumpHardware = false;
	int   iTextureRamMB = 0;			// Texture memory on the card for the game (0 = original 8 MB).		// Draw bump-mapped objects on the card (as plain textures).
	int   iNearDistCulled = 0;			// Diagnostics: objects within 8 m hidden by distance / by occlusion.
	int   iNearOccluded = 0;
	float fNearCullDist = 0, fNearCullLimit = 0;
	int   iBatchPolys = 0;				// Diagnostics: polygons drawn / draw calls sent.
	int   iBatchFlushes = 0;

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
