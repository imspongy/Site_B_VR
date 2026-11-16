/**********************************************************************************************
 *
 * VRMod.cpp
 *
 * Site B VR: game-side integration.
 *
 * How it works
 * ------------
 * - Each frame, VRMod_RenderStereo() asks SteamVR for the predicted headset pose, then renders
 *   the world twice through the game's own camera: once per eye, with an asymmetric (off-axis)
 *   frustum matching the headset lens.  The off-axis part uses the camera viewport's virtual
 *   origin, which the engine turns into a shear in the normalising transform.
 * - The game renders with DirectX 7 into its back buffer.  After each eye, the back buffer is
 *   locked and copied to a Direct3D 11 texture that is submitted to SteamVR (VRBackend.cpp).
 *
 * Character controller (modern VR style)
 * --------------------------------------
 * - The view is NOT Anne's physics head.  The room (SteamVR tracking space) sits in the game
 *   world at v3Origin, rotated by r3Yaw.  The headset, both controllers and the eyes are placed
 *   1:1 from that, so your view and hands never lag.
 * - The stick moves the room (with smooth acceleration, speed proportional to stick push);
 *   turning rotates the room around your head.  Walking around your real room moves you too.
 * - Anne's body chases the point under your headset (Player.cpp, BodyFollow).  If the body is
 *   blocked (a wall, a rock), the room is pulled back so you stay within a short leash of it.
 *   The view height follows the body (stairs, slopes, crouch, jumps) plus your real height.
 * - Both hands are published to the player (Player.cpp), which holds objects exactly at the
 *   controllers and draws both arms with IK.
 * - Menus and loading screens are shown on a floating virtual screen (SteamVR overlay) that you
 *   can point at with a laser and click with the trigger.
 *
 * Coordinates: OpenVR is x right, y up, -z forward.  The game is x right, y forward, z up.
 *   game = (vr.x, -vr.z, vr.y);  quaternion (w, x, y, z) -> (w, x, -z, y).
 * Hands: the shared state (gVR) uses 0 = right, 1 = left; VRBackend uses 0 = left, 1 = right.
 *
 **********************************************************************************************/

#include "precomp.h"
#pragma hdrstop

#include "VRMod.h"
#include "VRBackend.h"
#include "VRScene.h"
#include "VRRender.h"
#include "Game/AI/AIMain.hpp"

static void HideLoadingIfDone();

#include "Lib/Control/VRShared.hpp"
#include "Lib/Renderer/Camera.hpp"
#include "Lib/Renderer/ScreenRenderAuxD3D.hpp"
#include "Lib/W95/Direct3D.hpp"
#include "Lib/EntityDBase/Query/QRenderer.hpp"
#include "Lib/EntityDBase/Instance.hpp"
#include "Game/DesignDaemon/Player.hpp"
#include "Lib/Renderer/Sky.hpp"
#include "Lib/Renderer/Occlude.hpp"
#include "Lib/Sys/RegInit.hpp"
#include "Lib/Sys/Profile.hpp"
#include "Lib/Std/StringEx.hpp"
#include "Lib/EntityDBase/QualitySettings.hpp"
#include "Lib/Renderer/RenderCacheInterface.hpp"
#include "Lib/Types/FixedP.hpp"
#include "Lib/Renderer/Primitives/DrawTriangle.hpp"
#include "Lib/Renderer/Primitives/Walk.hpp"
#include "Lib/Renderer/Primitives/IndexT.hpp"
#include "Lib/Renderer/Primitives/IndexPerspectiveT.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <thread>
#include <vector>

extern HWND g_hwnd;
extern float fPerspectivePixelError;
extern float fAltPerspectivePixelError;

namespace
{
	const float fDEG = 3.14159265358979f / 180.0f;

	//******************************************************************************************
	//
	// Settings (TrespasserVR.ini, [VR] section, next to OpenTrespasser.exe).
	// The file is re-read automatically when it changes, so it can be edited while playing.
	//
	const int iINI_VERSION = 7;		// Older files are replaced with the new defaults.

	struct SSettings
	{
		bool  bEnabled        = true;
		bool  bLeftHanded     = false;	// Sticks and face buttons swapped.
		bool  bGripToggle     = false;	// false = hold grip to hold; true = press to grab/drop.
		bool  bForceGrab      = true;
		bool  bTwoHandAim     = true;
		bool  bSecondHand     = true;	// Left arm.
		float fPistolMaxMass  = 2.5f;	// Guns lighter than this get the pistol support grip.
		bool  bFlipSecondCurl = false;	// Debug: fingers bend the wrong way.
		float fHolsterRadius  = 0.25f;	// Metres from holster point that counts as "at" it.
		float fHaptics        = 1.0f;	// Haptic strength multiplier (0 = off).
		float fHandPitchDeg   = 0.0f;	// Controller -> in-game hand orientation offset.
		float fHandYawDeg     = 0.0f;
		float fHandRollDeg    = 0.0f;
		float fHandScale      = 0.75f;	// Drawn hand size (Anne's original hand is oversized).
		bool  bLeftArmTest    = false;	// Debug.

		bool  bMoveWithHand   = false;	// Stick moves toward where the off hand points.
		float fMoveSpeed      = 2.2f;	// m/s at full stick.
		float fRunSpeed       = 4.5f;	// m/s at full stick with run on.
		float fMoveAccel      = 0.15f;	// Seconds to (mostly) reach the stick's speed.
		float fMoveDeadzone   = 0.12f;
		bool  bSmoothTurn     = true;	// false = snap turn.
		float fSnapTurnDeg    = 45.0f;	// Degrees per snap.
		float fSmoothTurnDeg  = 150.0f;	// Degrees per second at full stick.
		float fTurnDeadzone   = 0.2f;

		float fBodyLeash      = 0.4f;	// How far your head may get from Anne's body (blocked).
		float fHeightSmoothing = 0.1f;	// Seconds; smooths view height on stairs and slopes.
		int   iSkyMode        = 2;		// 0 = solid colour, 1 = gradient, 2 = clouds.
		bool  bImageCaches    = false;	// The game's distant-object image caches.
		bool  bRadialFog      = true;	// Fog by true distance (doesn't change as you turn).
		float fDrawDistance   = 8.0f;
		float fDrawDistSmall  = 3.0f;	// The same, for small objects.	// Multiplier on how far away objects are drawn.
		bool  bSharpTextures  = true;
		bool  bNewRenderer    = true;	// Draw the world with the modern GPU renderer.
		float fRenderScale    = 1.0f;	// Eye image size relative to SteamVR's recommendation.
		int   iMSAA           = 4;		// Anti-aliasing samples.
		int   iShadowMapSize  = 4096;
		float fShadowDistance = 60.0f;
		float fBrightness     = 1.0f;
		float fTerrainDetail  = 1.0f;
		float fFogScale       = 1.3f;
		float fExtraHeight    = 0.2f;
		bool  bBumpMaps       = true;
		bool  bTutorial       = true;
		float fRecoil         = 1.0f;	// Metres added to Anne's eye height.
		bool  bOcclusion      = true;	// The game's hiding of objects behind big ones.	// More accurate texture perspective (less wobble).
		bool  bMirrorToMonitor = false;	// Also show the game on the monitor (costs speed).
		float fLeanRadius     = 0.6f;	// Cutscenes: how far you can lean from the camera.
		float fOverlayWidth   = 2.4f;	// Menu screen width in metres.
		float fOverlayDist    = 2.0f;	// Menu screen distance in metres.
		bool  bFlipFovX       = false;	// Debug: invert the off-axis direction.
		bool  bFlipFovY       = false;
		bool  bMenuFlipY      = false;	// Debug: laser pointer Y inverted on the menu screen.
	};

	SSettings setVR;

	//******************************************************************************************
	//
	// State.
	//
	bool        bStarted    = false;	// Startup attempted.
	bool        bActive     = false;	// Session running.
	bool        bStereoThisFrame = false;
	bool        bHardwareWanted = false;	// ini: HardwareRendering.
	bool        bOverlayShown = false;
	int         iOverlayX0 = 0, iOverlayY0 = 0, iOverlayH = 480;	// Part of the screen shown on the menu overlay.

	SVRFrame    frLast;					// Most recent frame data (also read by the input hook).

	// Tracking space (your room) -> world:  world = v3Origin + track * r3Yaw.
	CRotate3<>  r3Yaw;
	CVector3<>  v3Origin(0, 0, 0);
	CVector3<>  v3Calib(0, 0, 0);		// Tracking-space head position at recentre.
	bool        bNeedAlign  = true;
	const void* pvLastCamAttach = nullptr;
	bool        bLastCamHead = false;

	// Locomotion.
	CVector3<>  v3MoveVel(0, 0, 0);		// Smoothed stick velocity (world, horizontal).
	float       fBaseZ = 0;				// Smoothed game eye height (world).
	bool        bBaseZInit = false;
	double      dPrevFrameMs = 0;

	// Hands in world space, shared-state indices (0 = right, 1 = left).
	bool        abHandValid[2] = { false, false };
	CVector3<>  av3HandWorld[2];
	CRotate3<>  ar3HandWorld[2];
	CVector3<>  av3HandVelTrack[2];		// Controller velocity relative to the room (world axes).
	CVector3<>  v3HeadWorld(0, 0, 0);
	bool        abGripPhys[2] = { false, false };
	bool        abGripLogical[2] = { false, false };

	// Input edge state.
	SVRControls ctlPrevStep;
	SVRControls ctlPrevFrame;
	bool        bRunToggle    = false;
	bool        bCrouchToggle = false;
	bool        bSnapArmed    = true;

	// Haptic feedback state.
	int         aiSeenShots[2] = { 0, 0 };
	int         aiSeenPickups[2] = { 0, 0 };
	int         aiSeenForceSerial[2] = { 0, 0 };
	int         aiLastHolsterNear[2] = { -1, -1 };

	// ini hot-reload.
	FILETIME    ftIni = { 0, 0 };
	DWORD       u4LastIniCheck = 0;

	FILE*       pfLog = nullptr;

	//******************************************************************************************
	std::string strExeDir()
	{
		char str_path[MAX_PATH] = { 0 };
		GetModuleFileNameA(nullptr, str_path, MAX_PATH);
		std::string s(str_path);
		size_t pos = s.find_last_of("\\/");
		return pos == std::string::npos ? std::string(".") : s.substr(0, pos);
	}

	std::string strIniPath()
	{
		return strExeDir() + "\\TrespasserVR.ini";
	}

	//******************************************************************************************
	void Log(const char* str_fmt, ...)
	{
		if (!pfLog)
			pfLog = std::fopen((strExeDir() + "\\TrespasserVR.log").c_str(), "w");
		if (!pfLog)
			return;
		va_list args;
		va_start(args, str_fmt);
		std::vfprintf(pfLog, str_fmt, args);
		va_end(args);
		std::fputc('\n', pfLog);
		std::fflush(pfLog);
	}

	//******************************************************************************************
	//
	// Performance timing, written to the log every few seconds.
	//
	double dNowMs()
	{
		static LARGE_INTEGER li_freq = { 0 };
		if (!li_freq.QuadPart)
			QueryPerformanceFrequency(&li_freq);
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return (double)li.QuadPart * 1000.0 / (double)li_freq.QuadPart;
	}

	enum ETiming { etWAIT, etPAINT, etREADBACK, etSUBMIT, etFRAME, etGAP, etOVERLAY, etCOUNT };
	const char* astrTIMING[etCOUNT] = { "wait poses", "paint (both eyes)", "readback+convert", "submit", "whole VR frame", "game outside render", "menu screen update" };
	double adTimingMs[etCOUNT] = { 0 };
	int    iTimingFrames = 0, iTimingFlips = 0;
	double dTimingStart = 0, dLastFrameEnd = 0;

	void TimingReport()
	{
		double d_now = dNowMs();
		if (dTimingStart == 0)
		{
			dTimingStart = d_now;
			return;
		}
		if (d_now - dTimingStart < 5000)
			return;

		double d_secs = (d_now - dTimingStart) / 1000.0;
		char str[700];
		int i_len = std::snprintf(str, sizeof(str), "Perf: %.1f VR fps, %.1f flips/s.", iTimingFrames / d_secs, iTimingFlips / d_secs);
		for (int i = 0; i < etCOUNT; i++)
		{
			int i_n = i == etOVERLAY ? iTimingFlips : iTimingFrames;
			if (i_n > 0 && adTimingMs[i] > 0 && i_len < (int)sizeof(str) - 64)
				i_len += std::snprintf(str + i_len, sizeof(str) - i_len, " %s %.1fms;", astrTIMING[i], adTimingMs[i] / i_n);
		}
		if (i_len < (int)sizeof(str) - 96)
			i_len += std::snprintf(str + i_len, sizeof(str) - i_len, " monitor %.1fms/flip; game step %.1fms x%d/s;",
				iTimingFlips ? gVR.dFlipMs / iTimingFlips : 0.0,
				gVR.iSteps ? gVR.dStepMs / gVR.iSteps : 0.0, int(gVR.iSteps / d_secs));
		if (iTimingFrames > 0 && gVR.iBatchPolys > 0 && i_len < (int)sizeof(str) - 64)
			i_len += std::snprintf(str + i_len, sizeof(str) - i_len, " %d polygons in %d draw calls a frame;",
				gVR.iBatchPolys / iTimingFrames, gVR.iBatchFlushes / iTimingFrames);
		gVR.iBatchPolys = gVR.iBatchFlushes = 0;
		if (VRScene_Ready() && setVR.bNewRenderer)
			Log("%s", VRScene_Stats());
		if (gVR.iNearDistCulled || gVR.iNearOccluded)
			Log("Near objects hidden (within 8 m, per frame): %.1f by draw distance (e.g. %.1f m away, limit %.1f m), %.1f by occlusion.",
				gVR.iNearDistCulled / (double)(std::max)(1, iTimingFrames), gVR.fNearCullDist, gVR.fNearCullLimit,
				gVR.iNearOccluded / (double)(std::max)(1, iTimingFrames));
		gVR.iNearDistCulled = gVR.iNearOccluded = 0;
		gVR.dFlipMs = gVR.dStepMs = 0;
		gVR.iSteps = 0;
		Log("%s", str);

		for (int i = 0; i < etCOUNT; i++)
			adTimingMs[i] = 0;
		iTimingFrames = iTimingFlips = 0;
		dTimingStart = d_now;
	}

	//******************************************************************************************
	float fIni(const std::string& str_file, const char* str_key, float f_default)
	{
		char str_def[32], str_val[64];
		std::snprintf(str_def, sizeof(str_def), "%g", f_default);
		GetPrivateProfileStringA("VR", str_key, str_def, str_val, sizeof(str_val), str_file.c_str());
		return (float)std::atof(str_val);
	}
	//******************************************************************************************
	void WriteDefaultIni(const std::string& str_file)
	{
		FILE* pf = std::fopen(str_file.c_str(), "w");
		if (!pf)
			return;
		std::fprintf(pf,
			"; Site B VR settings.  Changes take effect within a couple of seconds, even in game.\n"
			"; Delete this file to restore defaults.\n"
			"[VR]\n"
			"Version=%d\n"
			"; 0 = play on the monitor as normal.\n"
			"Enabled=1\n"
			"\n", iINI_VERSION);
		std::fputs(
			"; --- Movement ---\n"
			"; Walking speed (metres per second) with the stick pushed all the way.\n"
			"MoveSpeed=2.2\n"
			"; Sprint speed, 4.5 at most (click the walk stick to sprint; letting go of the stick stops it).\n"
			"RunSpeed=4.5\n"
			"; How quickly you speed up and slow down (seconds). 0 = instant.\n"
			"MoveAcceleration=0.15\n"
			"MoveDeadzone=0.12\n"
			"; Jump height (1 = original game).\n"
			"JumpHeight=2\n"
			"; 0 = walk where you look, 1 = walk where your off hand points.\n"
			"MoveWithHand=0\n"
			"; How far (metres) your head can go from Anne's body when she is blocked by a wall.\n"
			"BodyLeash=0.4\n"
			"; Seconds of smoothing on view height over stairs and slopes (0 = none).\n"
			"HeightSmoothing=0.1\n"
			"\n"
			"; --- Turning ---\n"
			"; 0 = snap turn, 1 = smooth turn.\n"
			"SmoothTurn=1\n"
			"; Degrees per snap.\n"
			"SnapTurnDegrees=45\n"
			"; Degrees per second at full stick (smooth turn).\n"
			"SmoothTurnSpeed=150\n"
			"TurnDeadzone=0.2\n"
			"\n"
			"; --- Hands ---\n"
			"; 1 = swap the sticks and face buttons for left-handed play. Both hands always work the same.\n"
			"LeftHanded=0\n"
			"; 0 = hold the grip to keep holding things (let go to drop).\n"
			"; 1 = press grip once to grab, press again to drop.\n"
			"GripToggle=0\n"
			"; Point at an item, hold the trigger and flick your wrist to pull it to you.\n"
			"ForceGrab=1\n"
			"; Grab the other hand's gun to aim it with both hands.\n"
			"TwoHandAim=1\n"
			"; Anne's left arm on your left controller.\n"
			"SecondHand=1\n"
			"; Guns lighter than this (kg) count as pistols for the two-handed grip.\n"
			"PistolMaxMass=2.5\n"
			"; How close (metres) your hand must be to a holster (hip / over shoulder).\n"
			"HolsterRadius=0.25\n"
			"; Vibration strength. 0 = off, 1 = normal, 2 = strong.\n"
			"Haptics=1\n"
			"; Rotate the in-game hand relative to your controller (degrees).\n"
			"; If guns aim too high, try HandPitch=-20.  (0 = fingers pointing along the controller.)\n"
			"HandPitch=0\n"
			"HandYaw=0\n"
			"HandRoll=0\n"
			"; Size of the drawn hands (1 = Anne's original, rather large, hands).\n"
			"HandScale=0.75\n"
			"\n"
			"; --- View ---\n"
			"; Sky: 0 = solid colour, 1 = smooth gradient, 2 = clouds.\n"
			"SkyMode=2\n"
			"; 1 = use the game's low-detail stand-ins for distant objects (faster, but causes\n"
			";     pop-in and blocky distant objects in VR).\n"
			"ImageCaches=0\n"
			"; 1 = haze depends on real distance, so it doesn't change as you turn your head.\n"
			"RadialFog=1\n"
			"; How far away objects and buildings are drawn, and how far they keep full detail\n"
			";     (1 = original game, 3 = three times as far).  Higher costs frame rate.\n"
			"DrawDistance=8\n"
			"; 1 = let the game skip objects it thinks are hidden behind big ones (walls, rocks).  Off by\n"
			";     default in VR: it can hide things right in front of you.\n"
			"Occlusion=1\n"
			"; 1 = more accurate texture perspective: less wobble and shimmer up close (a bit slower).\n"
			"SharpTextures=1\n"
			"; 1 = also show the game on your monitor (much slower). Use SteamVR's\n"
			";     'Display VR View' to watch or record instead.\n"
			"MirrorToMonitor=0\n"
			"; Cutscenes: how far (metres) you can lean before the view stops following.\n"
			"LeanRadius=0.6\n"
			"; Size and distance (metres) of the floating menu screen.\n"
			"MenuScreenWidth=2.4\n"
			"MenuScreenDistance=2.0\n"
			"\n"
			"; EXPERIMENTAL: 1 = draw with the graphics card (the game's Direct3D mode) instead of\n"
			";     the CPU.  Needed for high resolutions at high frame rates.  Takes effect on restart.\n"
			"HardwareRendering=0\n"
			"\n"
			"; --- Graphics (modern renderer) ---\n"
			"; 1 = draw the world with the modern GPU renderer: headset resolution, anti-aliasing,\n"
			";     real-time shadows, high frame rates.  0 = the game's original renderer.\n"
			";     Takes effect on restart.\n"
			"NewRenderer=1\n"
			"; Eye image size relative to what SteamVR recommends for your headset (1 = native).\n"
			"RenderScale=1.0\n"
			"; Anti-aliasing samples: 1 (off), 2, 4 or 8.\n"
			"MSAA=4\n"
			"; Shadow map size (0 = no shadows) and how far around you shadows are drawn (metres).\n"
			"ShadowMapSize=4096\n"
			"ShadowDistance=60\n"
			"; Overall brightness.\n"
			"Brightness=1.0\n"
			"; Terrain mesh and texture detail (0.5 = half, 2 = double).  Applies on the next level load.\n"
			"TerrainDetail=1.0\n"
			"; The game's fog, spread over this many times its normal view distance (1 = original).\n"
			"FogScale=1.3\n"
			"; Light bump-mapped surfaces (rocks, bark, dinosaur skin) with their relief: 1 on, 0 off.\n"
			"BumpMaps=1\n"
			"; Extra height for Anne (metres): raises your view and hands above hers.\n"
			"ExtraHeight=0.2\n"
			"\n"
			"; --- Guns and throwing ---\n"
			"; How far guns sit back into the palm and across toward its middle (metres), and how big\n"
			"; held guns are drawn (1 = the game's size).\n"
			"GunBack=0.04\n"
			"GunSide=-0.02\n"
			"GunScale=0.85\n"
			"; Thrown things go this much faster than your hand.\n"
			"ThrowScale=2.0\n"
			"; How much guns kick (0 = none).\n"
			"Recoil=1.0\n"
			"; How much held things trail your hand and sag with their weight (0 = none).\n"
			"HeldWeight=1.0\n"
			"; Melee damage (bats, pipes, sticks swung in the hand): 1 = useful as a last resort.\n"
			"MeleeDamage=1.0\n"
			"; Anne's weight (kg) on planks, seesaws and loose things she stands on.\n"
			"BodyWeight=70\n"
			"; Voice-over subtitles floating below your view: 1 on, 0 off.\n"
			"Subtitles=1\n"
			"; Tutorial and hint messages (with VR controls) in front of you: 1 on, 0 off.\n"
			"Tutorial=1\n"
			"\n"
			"; --- Debug ---\n"
			"; Set to 1 if the image looks skewed or doubled.\n"
			"FlipFovX=0\n"
			"FlipFovY=0\n"
			"; Set to 1 if the menu laser moves up when you point down.\n"
			"MenuFlipY=0\n"
			"; Set to 1 if the left hand's fingers bend backwards when you squeeze.\n"
			"FlipSecondHandCurl=0\n"
			"; Set to 1 to draw a test copy of the right arm 25 cm left of your right hand.\n"
			"LeftArmTest=0\n", pf);
		std::fclose(pf);
	}

	//******************************************************************************************
	void LoadSettings()
	{
		std::string str_file = strIniPath();

		if (GetFileAttributesA(str_file.c_str()) == INVALID_FILE_ATTRIBUTES)
			WriteDefaultIni(str_file);
		else if ((int)fIni(str_file, "Version", 0) < iINI_VERSION)
		{
			// Settings from an older build: the controls changed, start from the new defaults.
			CopyFileA(str_file.c_str(), (str_file + ".old").c_str(), FALSE);
			WriteDefaultIni(str_file);
			Log("TrespasserVR.ini was from an older version: replaced with new defaults (old one saved as TrespasserVR.ini.old).");
		}

		SSettings s;
		s.bEnabled        = fIni(str_file, "Enabled", 1) != 0;
		s.bLeftHanded     = fIni(str_file, "LeftHanded", 0) != 0;
		s.bGripToggle     = fIni(str_file, "GripToggle", 0) != 0;
		s.bForceGrab      = fIni(str_file, "ForceGrab", 1) != 0;
		s.bTwoHandAim     = fIni(str_file, "TwoHandAim", 1) != 0;
		s.bSecondHand     = fIni(str_file, "SecondHand", 1) != 0;
		s.fPistolMaxMass  = fIni(str_file, "PistolMaxMass", 2.5f);
		gVR.fGunBack      = fIni(str_file, "GunBack", 0.04f);
		gVR.fThrowScale   = fIni(str_file, "ThrowScale", 2.0f);
		s.fRecoil         = fIni(str_file, "Recoil", 1.0f);
		gVR.fHeldWeight   = fIni(str_file, "HeldWeight", 1.0f);
		gVR.fMeleeScale   = fIni(str_file, "MeleeDamage", 1.0f);
		gVR.fBodyWeight   = fIni(str_file, "BodyWeight", 70.0f);
		gVR.bSubtitles    = fIni(str_file, "Subtitles", 1.0f) > 0.5f;
		s.bTutorial       = fIni(str_file, "Tutorial", 1.0f) > 0.5f;
		gVR.fGunSide      = fIni(str_file, "GunSide", -0.02f);
		gVR.fGunScale     = fIni(str_file, "GunScale", 0.85f);
		s.bFlipSecondCurl = fIni(str_file, "FlipSecondHandCurl", 0) != 0;
		s.fHolsterRadius  = fIni(str_file, "HolsterRadius", 0.25f);
		s.fHaptics        = fIni(str_file, "Haptics", 1);
		s.fHandPitchDeg   = fIni(str_file, "HandPitch", 0);
		s.fHandYawDeg     = fIni(str_file, "HandYaw", 0);
		s.fHandRollDeg    = fIni(str_file, "HandRoll", 0);
		s.fHandScale      = fIni(str_file, "HandScale", 0.75f);
		s.bLeftArmTest    = fIni(str_file, "LeftArmTest", 0) != 0;
		s.bRadialFog      = fIni(str_file, "RadialFog", 1) != 0;
		s.bSharpTextures  = fIni(str_file, "SharpTextures", 1) != 0;
		s.bNewRenderer    = fIni(str_file, "NewRenderer", 1) != 0;
		s.fRenderScale    = (std::min)(2.0f, (std::max)(0.3f, fIni(str_file, "RenderScale", 1.0f)));
		s.iMSAA           = (int)fIni(str_file, "MSAA", 4);
		s.iShadowMapSize  = (int)fIni(str_file, "ShadowMapSize", 4096);
		s.fShadowDistance = fIni(str_file, "ShadowDistance", 60);
		s.fBrightness     = fIni(str_file, "Brightness", 1.0f);
		s.fTerrainDetail  = fIni(str_file, "TerrainDetail", 1.0f);
		s.fFogScale       = fIni(str_file, "FogScale", 1.3f);
		s.fExtraHeight    = fIni(str_file, "ExtraHeight", 0.2f);
		s.bBumpMaps       = fIni(str_file, "BumpMaps", 1.0f) > 0.5f;
		s.bOcclusion      = fIni(str_file, "Occlusion", 1) != 0;
		s.fDrawDistance   = (std::min)((std::max)(fIni(str_file, "DrawDistance", 8), 0.5f), 30.0f);
		s.bMoveWithHand   = fIni(str_file, "MoveWithHand", 0) != 0;
		s.fMoveSpeed      = fIni(str_file, "MoveSpeed", 2.2f);
		s.fRunSpeed       = fIni(str_file, "RunSpeed", 4.5f);
		s.fMoveAccel      = fIni(str_file, "MoveAcceleration", 0.15f);
		s.fMoveDeadzone   = fIni(str_file, "MoveDeadzone", 0.12f);
		s.bSmoothTurn     = fIni(str_file, "SmoothTurn", 1) != 0;
		s.fSnapTurnDeg    = fIni(str_file, "SnapTurnDegrees", 45);
		s.fSmoothTurnDeg  = fIni(str_file, "SmoothTurnSpeed", 150);
		s.fTurnDeadzone   = fIni(str_file, "TurnDeadzone", 0.2f);
		s.fBodyLeash      = fIni(str_file, "BodyLeash", 0.4f);
		s.fHeightSmoothing = fIni(str_file, "HeightSmoothing", 0.1f);
		s.iSkyMode        = (int)fIni(str_file, "SkyMode", 2);
		s.bImageCaches    = fIni(str_file, "ImageCaches", 0) != 0;
		s.bMirrorToMonitor = fIni(str_file, "MirrorToMonitor", 0) != 0;
		s.fLeanRadius     = fIni(str_file, "LeanRadius", 0.6f);
		s.fOverlayWidth   = fIni(str_file, "MenuScreenWidth", 2.4f);
		s.fOverlayDist    = fIni(str_file, "MenuScreenDistance", 2.0f);
		s.bFlipFovX       = fIni(str_file, "FlipFovX", 0) != 0;
		s.bFlipFovY       = fIni(str_file, "FlipFovY", 0) != 0;
		s.bMenuFlipY      = fIni(str_file, "MenuFlipY", 0) != 0;

		// Keep the numbers sane.
		s.fMoveDeadzone = (std::min)((std::max)(s.fMoveDeadzone, 0.0f), 0.9f);
		s.fTurnDeadzone = (std::min)((std::max)(s.fTurnDeadzone, 0.0f), 0.9f);
		s.fBodyLeash    = (std::max)(s.fBodyLeash, 0.1f);
		// Anne's legs top out at about 3.7 m/s (the original game's run).
		s.fRunSpeed     = (std::min)(s.fRunSpeed, 4.5f);
		s.fMoveSpeed    = (std::min)(s.fMoveSpeed, 4.5f);
		setVR = s;

		gVR.bMotionHand  = true;
		gVR.bSkipMonitor = !setVR.bMirrorToMonitor;
		gVR.bLeftArm     = setVR.bSecondHand;
		gVR.fCurlSign    = setVR.bFlipSecondCurl ? -1.0f : 1.0f;
		gVR.fFingerCurl  = 0;
		gVR.fJumpHeight  = 0.55f * (std::min)((std::max)(fIni(str_file, "JumpHeight", 2), 0.5f), 6.0f);

		// Draw distance: scale the game's quality tables (all levels) from their original values.
		{
			static QualityData aqd_orig[5];
			static bool b_saved = false;
			if (!b_saved)
			{
				for (int i = 0; i < 5; i++)
					aqd_orig[i] = qdQualitySettings[i];
				b_saved = true;
			}
			float f_dd = setVR.bEnabled ? setVR.fDrawDistance : 1.0f;
			gVR.fCullScaleSqr = f_dd * f_dd;
			{
				float f_small = (std::min)(f_dd, (std::max)(0.5f, fIni(str_file, "DrawDistanceSmall", 8)));
				gVR.fCullScaleSmallSqr = setVR.bEnabled ? f_small * f_small : 1.0f;
			}
			for (int i = 0; i < 5; i++)
			{
				qdQualitySettings[i].fCullDistScale        = aqd_orig[i].fCullDistScale;
				qdQualitySettings[i].fCullShadowDistScale  = aqd_orig[i].fCullShadowDistScale * (std::min)(f_dd, 2.0f);
				qdQualitySettings[i].fDetailReductionScale = aqd_orig[i].fDetailReductionScale * f_dd * f_dd;
				// (Fog is a fraction of the far clip distance, so the haze moves out with it.)
				qdQualitySettings[i].fFarClipScale         = f_dd <= 1.0f ? aqd_orig[i].fFarClipScale * f_dd : (std::min)(3.0f, 1.0f + (f_dd - 1.0f) * 0.3f);
				qdQualitySettings[i].fTerrainDistanceScale = aqd_orig[i].fTerrainDistanceScale * (std::min)(f_dd, 2.0f);
			}
		}
		gVR.fHandScale   = (std::min)((std::max)(setVR.fHandScale, 0.3f), 1.5f);
		gVR.bLeftArmTest = setVR.bLeftArmTest;

		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (GetFileAttributesExA(str_file.c_str(), GetFileExInfoStandard, &fad))
			ftIni = fad.ftLastWriteTime;
	}

	//******************************************************************************************
	void CheckIniReload()
	{
		DWORD u4_now = GetTickCount();
		if (u4_now - u4LastIniCheck < 1500)
			return;
		u4LastIniCheck = u4_now;

		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (GetFileAttributesExA(strIniPath().c_str(), GetFileExInfoStandard, &fad) &&
			CompareFileTime(&fad.ftLastWriteTime, &ftIni) != 0)
		{
			LoadSettings();
			Log("Settings reloaded.");
		}
	}

	//******************************************************************************************
	//
	// Maths helpers.
	//
	inline CVector3<> v3Game(const float* af_p)
	{
		return CVector3<>(af_p[0], -af_p[2], af_p[1]);
	}

	inline CRotate3<> r3Game(const float* af_q)
	{
		return CRotate3<>(af_q[0], af_q[1], -af_q[3], af_q[2], true);
	}

	inline CRotate3<> r3AxisAngle(float x, float y, float z, float f_rad)
	{
		float s = std::sin(f_rad * 0.5f);
		return CRotate3<>(std::cos(f_rad * 0.5f), x * s, y * s, z * s, true);
	}

	// Rotation about world up (+z); positive turns left.
	inline CRotate3<> r3YawRad(float f_rad)
	{
		return r3AxisAngle(0, 0, 1, f_rad);
	}

	// Heading of a rotation (angle about +z of its forward vector).
	inline float fYawOf(const CRotate3<>& r3)
	{
		CVector3<> v3_fwd = CVector3<>(0, 1, 0) * r3;
		return std::atan2(-v3_fwd.tX, v3_fwd.tY);
	}

	inline void StoreQ(float* af, const CRotate3<>& r3)
	{
		af[0] = r3.tC;
		af[1] = r3.v3S.tX;
		af[2] = r3.v3S.tY;
		af[3] = r3.v3S.tZ;
	}

	inline void StoreV(float* af, const CVector3<>& v3)
	{
		af[0] = v3.tX;
		af[1] = v3.tY;
		af[2] = v3.tZ;
	}

	inline float fDot(const CVector3<>& a, const CVector3<>& b)
	{
		return a.tX * b.tX + a.tY * b.tY + a.tZ * b.tZ;
	}

	inline CVector3<> v3Cross(const CVector3<>& a, const CVector3<>& b)
	{
		return CVector3<>(a.tY * b.tZ - a.tZ * b.tY, a.tZ * b.tX - a.tX * b.tZ, a.tX * b.tY - a.tY * b.tX);
	}

	inline float fLen(const CVector3<>& v)
	{
		return std::sqrt(fDot(v, v));
	}

	// Rotation whose forward (+y) is d3_fwd and whose up (+z) is as close as possible to v3_up.
	CRotate3<> r3LookAt(const CVector3<>& v3_fwd, const CVector3<>& v3_up)
	{
		CVector3<> f = v3_fwd * (1.0f / (std::max)(fLen(v3_fwd), 1e-5f));
		CRotate3<> r3_base(d3YAxis, CDir3<>(f));
		CVector3<> v3_z = CVector3<>(0, 0, 1) * r3_base;

		// Project desired up onto the plane perpendicular to forward.
		CVector3<> u = v3_up - f * fDot(v3_up, f);
		if (fLen(u) < 1e-4f)
			return r3_base;
		u = u * (1.0f / fLen(u));

		float f_angle = std::atan2(fDot(v3Cross(v3_z, u), f), fDot(v3_z, u));
		return r3_base * r3AxisAngle(f.tX, f.tY, f.tZ, f_angle);
	}


	//******************************************************************************************
	// Controller index (VRBackend: 0 = left, 1 = right) for a shared-state hand (0 = right).
	inline int iCtl(int i_hand)
	{
		return i_hand == 0 ? 1 : 0;
	}

	// Controllers for the sticks and face buttons: "main" turns, jumps and crouches; "off" walks.
	inline int iMainCtl()
	{
		return setVR.bLeftHanded ? 0 : 1;
	}

	inline int iOffCtl()
	{
		return setVR.bLeftHanded ? 1 : 0;
	}

	inline void Haptic(int i_hand, float f_seconds, float f_freq, float f_amp)
	{
		if (setVR.fHaptics > 0)
		{
			// Some controllers (PlayStation VR2 Sense among them) barely respond to low
			// frequencies or very short pulses.
			f_freq = (std::max)(f_freq, 160.0f);
			f_seconds = (std::max)(f_seconds, 0.04f);
			int i_err = VRB_Haptic(iCtl(i_hand), f_seconds, f_freq, (std::min)(1.0f, f_amp * setVR.fHaptics));
			static int iLogged = 0;
			if (iLogged < 3)
			{
				iLogged++;
				Log("Haptics: pulse on the %s hand (%.2f s), SteamVR result %d.", i_hand == 0 ? "right" : "left", (double)f_seconds, i_err);
			}
		}
	}

	inline CVector3<> v3Flat(const CVector3<>& v3)
	{
		return CVector3<>(v3.tX, v3.tY, 0);
	}

	// Horizontal heading of a rotation (forward, or the top if looking straight up/down).
	CVector3<> v3Heading(const CRotate3<>& r3)
	{
		CVector3<> v3 = v3Flat(CVector3<>(0, 1, 0) * r3);
		if (fLen(v3) < 0.25f)
			v3 = v3Flat(CVector3<>(0, 0, 1) * r3) * (CVector3<>(0, 1, 0) * r3).tZ * -1.0f + v3;
		float f = fLen(v3);
		return f > 1e-4f ? v3 * (1.0f / f) : CVector3<>(0, 1, 0);
	}

	//******************************************************************************************
	// Copies the game's back buffer out through a callback.
	template<class F> bool bReadBackBuffer(F f)
	{
		if (!prasMainScreen)
			return false;

		if (srd3dRenderer.bIsBusy())
			srd3dRenderer.EndScene();

		IDirectDrawSurface4* pdds = prasMainScreen->GetRenderTargetSurface();
		if (!pdds)
			return false;

		DDSURFACEDESC2 sd;
		std::memset(&sd, 0, sizeof(sd));
		sd.dwSize = sizeof(sd);
		if (pdds->GetSurfaceDesc(&sd) != DD_OK)
			return false;

		uint32_t u4_r = sd.ddpfPixelFormat.dwRBitMask;
		uint32_t u4_g = sd.ddpfPixelFormat.dwGBitMask;
		uint32_t u4_b = sd.ddpfPixelFormat.dwBBitMask;

		if (prasMainScreen->bIsLocked() && prasMainScreen->pSurface)
		{
			// The renderer still has it locked; use that.
			f(prasMainScreen->pSurface,
			  prasMainScreen->iLinePixels * prasMainScreen->iPixelBytes(),
			  prasMainScreen->iWidth, prasMainScreen->iHeight, prasMainScreen->iPixelBits,
			  u4_r, u4_g, u4_b);
			return true;
		}

		// Read-only lock: nothing gets written back to the GPU when we unlock.
		DDSURFACEDESC2 sdl;
		std::memset(&sdl, 0, sizeof(sdl));
		sdl.dwSize = sizeof(sdl);
		HRESULT hr = pdds->Lock(nullptr, &sdl, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR | DDLOCK_NOSYSLOCK, nullptr);
		if (hr != DD_OK || !sdl.lpSurface)
			return false;

		f(sdl.lpSurface, (int)sdl.lPitch,
		  (std::min)((int)sdl.dwWidth,  prasMainScreen->iWidth),
		  (std::min)((int)sdl.dwHeight, prasMainScreen->iHeight),
		  (int)sdl.ddpfPixelFormat.dwRGBBitCount,
		  u4_r, u4_g, u4_b);

		pdds->Unlock(nullptr);
		return true;
	}


	//******************************************************************************************
	//
	// Frame capture: the left eye's last ~12 seconds at 5 frames a second, kept in memory and
	// written to the VRCapture folder as pictures when both stick clicks are pressed together
	// (and one picture every 20 seconds automatically).  For diagnosing what you see in the
	// headset.
	//
	const int iCAP_SIZE   = 512;
	const int iCAP_FRAMES = 60;
	std::vector<uint32_t> au4CapRing;		// iCAP_FRAMES frames of iCAP_SIZE^2 pixels (0x00RRGGBB).
	int    iCapNext = 0, iCapCount = 0;
	double dCapLast = 0, dCapAutoLast = 0;
	int    iCapSession = 0;
	bool   bCapBusy = false;

	void WriteBmp(const std::string& str_file, const uint32_t* pu4, int i_size)
	{
		FILE* pf = std::fopen(str_file.c_str(), "wb");
		if (!pf)
			return;
		int i_row = i_size * 3;
		unsigned u_data = (unsigned)(i_row * i_size);
		unsigned char ah[54] = { 'B', 'M' };
		auto put4 = [&](int i_at, unsigned u) { for (int i = 0; i < 4; i++) ah[i_at + i] = (unsigned char)(u >> (8 * i)); };
		put4(2, 54 + u_data); put4(10, 54); put4(14, 40); put4(18, i_size); put4(22, (unsigned)i_size);
		ah[26] = 1; ah[28] = 24; put4(34, u_data);
		std::fwrite(ah, 1, 54, pf);
		std::vector<unsigned char> au1(i_row);
		for (int y = i_size - 1; y >= 0; y--)
		{
			for (int x = 0; x < i_size; x++)
			{
				uint32_t u = pu4[y * i_size + x];
				au1[x * 3 + 0] = (unsigned char)(u);
				au1[x * 3 + 1] = (unsigned char)(u >> 8);
				au1[x * 3 + 2] = (unsigned char)(u >> 16);
			}
			std::fwrite(&au1[0], 1, i_row, pf);
		}
		std::fclose(pf);
	}

	void CaptureCommit(const uint32_t* pu4, double d_now);
	uint32_t* pu4CaptureSlot();

	// Modern renderer: a picture of the left eye every 200 ms (read back a frame late).
	void CaptureNewRenderer()
	{
		double d_now = dNowMs();
		if (d_now - dCapLast < 200)
			return;
		dCapLast = d_now;
		uint32_t* pu4 = pu4CaptureSlot();
		if (VRR_CaptureEye(iCAP_SIZE, pu4))
			CaptureCommit(pu4, d_now);
	}

	// Store one eye image (16 bit 565) in the ring, scaled down.
	void CaptureEye(const void* pv, int i_pitch, int i_w, int i_h, int i_bits)
	{
		if (i_bits != 16)
			return;
		double d_now = dNowMs();
		if (d_now - dCapLast < 200)
			return;
		dCapLast = d_now;
		if (au4CapRing.empty())
			au4CapRing.resize((size_t)iCAP_FRAMES * iCAP_SIZE * iCAP_SIZE);
		uint32_t* pu4 = &au4CapRing[(size_t)iCapNext * iCAP_SIZE * iCAP_SIZE];
		for (int y = 0; y < iCAP_SIZE; y++)
		{
			const uint16_t* pu2 = (const uint16_t*)((const unsigned char*)pv + (size_t)(y * i_h / iCAP_SIZE) * i_pitch);
			for (int x = 0; x < iCAP_SIZE; x++)
			{
				uint16_t u = pu2[x * i_w / iCAP_SIZE];
				pu4[y * iCAP_SIZE + x] = ((uint32_t)((u >> 11) << 3) << 16) | ((uint32_t)(((u >> 5) & 63) << 2) << 8) | (uint32_t)((u & 31) << 3);
			}
		}
		CaptureCommit(pu4, d_now);
	}

	// Ring slot the next picture goes into.
	uint32_t* pu4CaptureSlot()
	{
		if (au4CapRing.empty())
			au4CapRing.resize((size_t)iCAP_FRAMES * iCAP_SIZE * iCAP_SIZE);
		return &au4CapRing[(size_t)iCapNext * iCAP_SIZE * iCAP_SIZE];
	}

	// The picture in the next slot is complete.
	void CaptureCommit(const uint32_t* pu4, double d_now)
	{
		iCapNext = (iCapNext + 1) % iCAP_FRAMES;
		if (iCapCount < iCAP_FRAMES)
			iCapCount++;

		// One picture every 20 seconds.
		if (d_now - dCapAutoLast > 20000 && !bCapBusy)
		{
			dCapAutoLast = d_now;
			std::string str_dir = strExeDir() + "\\VRCapture";
			CreateDirectoryA(str_dir.c_str(), nullptr);
			char str[64];
			std::snprintf(str, sizeof(str), "\\auto_%06d.bmp", (int)(d_now / 1000.0));
			WriteBmp(str_dir + str, pu4, iCAP_SIZE);
		}
	}

	// Write the ring to disk (in the background).
	void SaveCapture()
	{
		if (bCapBusy || iCapCount == 0)
			return;
		bCapBusy = true;
		int i_session = ++iCapSession;
		std::vector<uint32_t> au4_copy;
		int i_n = iCapCount, i_first = (iCapNext - iCapCount + iCAP_FRAMES) % iCAP_FRAMES;
		au4_copy.reserve((size_t)i_n * iCAP_SIZE * iCAP_SIZE);
		for (int i = 0; i < i_n; i++)
		{
			const uint32_t* pu4 = &au4CapRing[(size_t)((i_first + i) % iCAP_FRAMES) * iCAP_SIZE * iCAP_SIZE];
			au4_copy.insert(au4_copy.end(), pu4, pu4 + iCAP_SIZE * iCAP_SIZE);
		}
		std::string str_dir = strExeDir() + "\\VRCapture";
		Log("Capture %d: saving %d frames (the last %.0f seconds) to VRCapture.", i_session, i_n, i_n * 0.2);
		std::thread([au4_copy, i_n, i_session, str_dir]()
		{
			CreateDirectoryA(str_dir.c_str(), nullptr);
			for (int i = 0; i < i_n; i++)
			{
				char str[64];
				std::snprintf(str, sizeof(str), "\\cap%02d_%03d.bmp", i_session, i);
				WriteBmp(str_dir + str, &au4_copy[(size_t)i * iCAP_SIZE * iCAP_SIZE], iCAP_SIZE);
			}
			bCapBusy = false;
		}).detach();
		Haptic(0, 0.15f, 60.0f, 0.5f);
		Haptic(1, 0.15f, 60.0f, 0.5f);
	}

	//******************************************************************************************
	// Runs the modern renderer, catching a crash in it so the game can carry on with the
	// original renderer instead of closing.
	struct SSceneCall
	{
		const SVRSceneEyes*     peyes;
		const SVRSceneSettings* pset;
		bool                    bResult;
	};

	void SceneCall(SSceneCall* psc)
	{
		psc->bResult = VRScene_RenderAndSubmit(*psc->peyes, *psc->pset);
	}

	void* pvSceneCrashAddress = nullptr;

	int iSceneFilter(EXCEPTION_POINTERS* pep, DWORD* pu4_code)
	{
		*pu4_code = pep->ExceptionRecord->ExceptionCode;
		pvSceneCrashAddress = pep->ExceptionRecord->ExceptionAddress;
		return EXCEPTION_EXECUTE_HANDLER;
	}

	bool bSceneCallSafe(SSceneCall* psc, DWORD* pu4_code)
	{
		__try
		{
			SceneCall(psc);
			return true;
		}
		__except (iSceneFilter(GetExceptionInformation(), pu4_code))
		{
			return false;
		}
	}

	bool bNewRendererFailed = false;
	int  iSceneCrashes = 0;

	//******************************************************************************************
	// Which holster (0 hip, 1 back) the given hand is at, or -1.
	int iHolsterNear(int i_hand)
	{
		if (!abHandValid[i_hand])
			return -1;
		const CVector3<>& v3_hand = av3HandWorld[i_hand];
		int i_best = -1;
		float f_best = setVR.fHolsterRadius;
		for (int i = 0; i < 2; i++)
		{
			// Either hand can reach either holster, on its own side or across: the holster
			// point (on Anne's right) and its mirror image on her left both count.
			for (int m = 0; m < 2; m++)
			{
				CVector3<> v3_h(gVR.aafHolsterPosWorld[i][0], gVR.aafHolsterPosWorld[i][1], gVR.aafHolsterPosWorld[i][2]);
				if (m == 1)
				{
					if (!gpPlayer)
						continue;
					CPlacement3<> p3_body = gpPlayer->p3GetPlacement();
					CVector3<> v3_local = (v3_h - p3_body.v3Pos) / p3_body.r3Rot;
					v3_local.tX = -v3_local.tX;
					v3_h = v3_local * p3_body.r3Rot + p3_body.v3Pos;
				}
				// The back slot is hard to reach precisely; be more generous there.
				float f_dist = fLen(v3_hand - v3_h) * (i == 1 ? 0.7f : 1.0f);
				if (f_dist < f_best)
				{
					f_best = f_dist;
					i_best = i;
				}
			}
		}
		return i_best;
	}

	//******************************************************************************************
	// SteamVR buttons -> game control message.  Called once per game step.  Walking, turning
	// and both hands are handled by the VR mod and the player directly, not through here.
	void InputHook(SInput& tin)
	{
		if (!bActive)
			return;

		const SVRHandInput& m  = frLast.ctl.ahand[iMainCtl()];
		const SVRHandInput& o  = frLast.ctl.ahand[iOffCtl()];
		const SVRHandInput& mp = ctlPrevStep.ahand[iMainCtl()];
		const SVRHandInput& op = ctlPrevStep.ahand[iOffCtl()];

		// The headset and the VR character controller replace the mouse and walking keys.
		tin.v2Rotate = CVector2<>(0, 0);
		tin.v2Move   = CVector2<>(0, 0);
		tin.u4ButtonState &= ~(uCMD_HAND | uCMD_GRAB | uCMD_USE);
		tin.u4ButtonHit   &= ~(uCMD_HAND | uCMD_GRAB | uCMD_USE);

		uint32 u4 = 0;

		// Jump and crouch go to the VR character controller (Player.cpp).
		gVR.bJump = m.bLower;
		if (m.bUpper && !mp.bUpper)
			bCrouchToggle = !bCrouchToggle;
		gVR.bCrouch = bCrouchToggle;
		tin.u4ButtonState &= ~(uCMD_JUMP | uCMD_CROUCH);
		tin.u4ButtonHit   &= ~(uCMD_JUMP | uCMD_CROUCH);

		// Pause menu.
		bool b_menu = o.bUpper || o.bMenu || m.bMenu;
		bool b_menu_prev = op.bUpper || op.bMenu || mp.bMenu;
		if (b_menu && !b_menu_prev && g_hwnd)
		{
			PostMessage(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
			PostMessage(g_hwnd, WM_KEYUP,   VK_ESCAPE, 0xC0010001);
		}

		tin.u4ButtonState |= u4;
		static uint32 u4_prev = 0;
		tin.u4ButtonHit   |= u4 & ~u4_prev;
		u4_prev = u4;

		ctlPrevStep = frLast.ctl;
	}

	//******************************************************************************************
	// Haptics and force-grab gestures; called once per rendered frame.
	void UpdateFeedback()
	{
		for (int h = 0; h < 2; h++)
		{
			if (gVR.aiShotCount[h] != aiSeenShots[h])
			{
				aiSeenShots[h] = gVR.aiShotCount[h];
				Haptic(h, 0.12f, 160.0f, 1.0f);
				// The supporting hand feels it too.
				if (gVR.iTwoHand == (h == 0 ? 1 : 2))
					Haptic(1 - h, 0.09f, 160.0f, 0.7f);
			}
			if (gVR.aiPickupCount[h] != aiSeenPickups[h])
			{
				aiSeenPickups[h] = gVR.aiPickupCount[h];
				float f_mass = gVR.afHeldMass[h];
				Haptic(h, 0.05f, 60.0f, 0.35f + (std::min)(f_mass / 40.0f, 0.5f));
			}
			if (gVR.aiForceTargetSerial[h] != aiSeenForceSerial[h])
			{
				aiSeenForceSerial[h] = gVR.aiForceTargetSerial[h];
				Haptic(h, 0.02f, 220.0f, 0.3f);
			}

			// Holster hover tick.
			int i_holster = iHolsterNear(h);
			if (i_holster != aiLastHolsterNear[h])
			{
				if (i_holster >= 0)
					Haptic(h, 0.03f, 160.0f, 0.35f);
				aiLastHolsterNear[h] = i_holster;
			}

			// Force grab: flick toward yourself (or upward) while aiming at a target.
			if (gVR.abForceAim[h] && gVR.abForceTarget[h] && abHandValid[h])
			{
				CVector3<> v3_to_head = v3HeadWorld - av3HandWorld[h];
				float f_dist = fLen(v3_to_head);
				const CVector3<>& v3_vel = av3HandVelTrack[h];
				float f_toward = f_dist > 0.01f ? fDot(v3_vel, v3_to_head) / f_dist : 0.0f;
				if (f_toward > 1.3f || v3_vel.tZ > 1.6f)
				{
					if (!gVR.abForcePull[h])
						Haptic(h, 0.12f, 70.0f, 0.6f);
					gVR.abForcePull[h] = true;
				}
			}
		}
	}

	//******************************************************************************************
	// Menu screen laser pointer -> game window mouse.
	void PumpMenuLaser()
	{
		SVROverlayMouse ev;
		static bool b_down = false;
		while (VRB_PollOverlayMouse(ev))
		{
			if (!g_hwnd || !prasMainScreen)
				continue;

			// In game screen pixels: the menus don't depend on the monitor or window size.
			float f_y = setVR.bMenuFlipY ? (iOverlayH - ev.fY) : ev.fY;
			int i_x = (int)ev.fX + iOverlayX0;
			int i_y = (int)f_y + iOverlayY0;
			gVR.aiMenuCursor[0] = i_x;
			gVR.aiMenuCursor[1] = i_y;
			gVR.bMenuCursor = true;

			LPARAM lp = MAKELPARAM(i_x, i_y);
			switch (ev.eType)
			{
			case SVROverlayMouse::eMOVE:
				PostMessage(g_hwnd, WM_MOUSEMOVE, b_down ? MK_LBUTTON : 0, lp);
				break;
			case SVROverlayMouse::eDOWN:
				b_down = true;
				PostMessage(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
				break;
			case SVROverlayMouse::eUP:
				b_down = false;
				PostMessage(g_hwnd, WM_LBUTTONUP, 0, lp);
				break;
			}
		}
	}

	//******************************************************************************************
	// Called at the start of every CRasterWin::Flip(): shows menus on a virtual screen.
	void FlipHook()
	{
		if (!bActive)
			return;

		CheckIniReload();

		if (bStereoThisFrame)
		{
			if (bOverlayShown)
			{
				VRB_HideOverlay();
				bOverlayShown = false;
			}
		}
		else
		{
			// Menu screen: at most ~45 updates a second.
			static double d_last_update = 0;
			double d_start = dNowMs();
			if (!bOverlayShown || d_start - d_last_update > 22)
			{
				bool b_place = !bOverlayShown;
				bReadBackBuffer([&](const void* pv, int i_pitch, int i_w, int i_h, int i_bits, uint32_t r, uint32_t g, uint32_t b)
				{
					// Menus, loading screens and videos are 640x480 in the middle of the screen:
					// show just that part, so they stay the same size at any render resolution.
					int i_cw = (std::min)(i_w, 656), i_ch = (std::min)(i_h, 496);
					iOverlayX0 = (i_w - i_cw) / 2;
					iOverlayY0 = (i_h - i_ch) / 2;
					iOverlayH  = i_ch;
					const void* pv_crop = (const unsigned char*)pv + (size_t)iOverlayY0 * i_pitch + (size_t)iOverlayX0 * (i_bits / 8);
					HideLoadingIfDone();
					VRB_ShowOverlay(pv_crop, i_pitch, i_cw, i_ch, i_bits, r, g, b,
					                setVR.fOverlayWidth, setVR.fOverlayDist, b_place);
				});
				bOverlayShown = true;
				d_last_update = d_start;
				adTimingMs[etOVERLAY] += dNowMs() - d_start;
			}
			PumpMenuLaser();
		}
		bStereoThisFrame = false;
		iTimingFlips++;
		TimingReport();
	}
}

//**********************************************************************************************
void VRMod_PreInit()
{
	std::string str_file = strIniPath();
	if (GetFileAttributesA(str_file.c_str()) == INVALID_FILE_ATTRIBUTES)
		return;
	if (fIni(str_file, "Enabled", 1) == 0)
		return;

	bHardwareWanted = fIni(str_file, "HardwareRendering", 0) != 0;
	if (bHardwareWanted)
	{
		// (Batching, bump objects on the card and the texture memory changes are not in this
		// build: it is back to the renderer of the build where everything was drawn correctly.)
		// (The game is a 32-bit program: it keeps a copy of every texture in its own memory, so
		// this can't go very high.)
		SetRecommendedTextureMaxDim(128);
		// The standard hardware (HAL) Direct3D device on the primary display.
		static const GUID guid_hal = { 0x84E63dE0, 0x46AA, 0x11CF, { 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E } };
		GUID guid_primary;
		std::memset(&guid_primary, 0, sizeof(guid_primary));
		SetD3D(TRUE);
		WriteDDGUID(guid_primary);
		WriteD3DGUID(guid_hal);
		SetZBufferBitdepth(16);
	}
	else if (bGetD3D())
		SetD3D(FALSE);
}

//**********************************************************************************************
void VRMod_Startup()
{
	if (bStarted)
		return;
	bStarted = true;

	LoadSettings();
	if (!setVR.bEnabled)
	{
		Log("VR disabled in TrespasserVR.ini.");
		return;
	}

	char str_err[256] = { 0 };
	int i_w = 0, i_h = 0;
	if (!VRB_Init(&i_w, &i_h, str_err, sizeof(str_err)))
	{
		Log("VR not started: %s", str_err);
		return;
	}

	Log("SteamVR started. Recommended eye size %dx%d.", i_w, i_h);

	if (setVR.bNewRenderer)
	{
		char str_err2[512] = { 0 };
		int i_ew = (int)(i_w * setVR.fRenderScale), i_eh = (int)(i_h * setVR.fRenderScale);
		if (VRScene_Init(i_ew, i_eh, setVR.iMSAA, setVR.iShadowMapSize, str_err2, sizeof(str_err2)))
			Log("Modern renderer started: %dx%d per eye, %dx anti-aliasing, %d shadow map.", i_ew, i_eh, setVR.iMSAA, setVR.iShadowMapSize);
		else
			Log("Modern renderer could not start (%s); using the game's renderer.", str_err2);
	}
	if (prasMainScreen)
		Log("Game render size %dx%d, %d bpp.", prasMainScreen->iWidth, prasMainScreen->iHeight, prasMainScreen->iPixelBits);
	Log("Fast draw %s, bump objects on the card %s.", gVR.bBatchDraw ? "on" : "off", gVR.bBumpHardware ? "on" : "off");
	Log("Renderer: %s%s.", d3dDriver.bUseD3D() ? "graphics card (Direct3D)" : "CPU (software)",
		bHardwareWanted && !d3dDriver.bUseD3D() ? " - hardware rendering was requested but could not be started" : "");

	bActive          = true;
	bNeedAlign       = true;
	gVR.bActive      = true;
	gpfnVRInputHook  = InputHook;
	gpfnVRFlipHook   = FlipHook;
	gVR.pfnLog       = Log;
}

//**********************************************************************************************
//**********************************************************************************************
namespace
{
	bool bLoadingShown = false;		// The loading notice is up; hide it once the new level is drawn.
	bool bLevelLoaded = false;
	DWORD u4LevelLoadedTick = 0;

	// Level title card, faded in once the level is on screen.
	std::string strTitleLevel, strTitleName;
	bool  bTitlePending = false;
	DWORD u4TitleStart = 0;

	// The game's tutorial/hint lines (0) and voice-over subtitles (1), queued.
	struct STextItem { std::string str; float fSecs; };
	std::deque<STextItem> atqText[2];
	DWORD au4TextStart[2] = { 0, 0 };

	std::string strSquash(const char* psz)
	{
		std::string str;
		bool b_space = false;
		for (const char* p = psz; *p; p++)
		{
			unsigned char c = (unsigned char)*p;
			if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
			{
				b_space = !str.empty();
				continue;
			}
			if (b_space)
				str += ' ';
			b_space = false;
			str += (char)c;
		}
		return str;
	}

	// The level 1 tutorial, retold for VR controls.
	std::string strVRText(const std::string& str_in)
	{
		struct SPair { const char* strFrom; const char* strTo; };
		static const SPair apairs[] =
		{
			{ "To look around, move your mouse.", "To look around, just turn your head. The right stick turns you too." },
			{ "To move forward, press and hold 'W'.", "To move, push the left stick. Click it in to sprint." },
			{ "Press 'Q' to jump up.", "Press Cross (A) to jump up." },
			{ "Press 'A' and 'D' to step left and right.", "Push the left stick sideways to step left and right." },
			{ "And press 'Z' to crouch.", "And crouch down for real, or press Circle (B) to crouch." },
			{ "You can use your hand to move and carry objects.", "You can use your hands to move and carry objects." },
			{ "Try pressing and holding the left mouse button.", "Reach out with either hand." },
			{ "Now, moving the mouse moves your hand.", "Your hands follow your controllers." },
			{ "To pick up an object, move your hand near it....", "To pick up an object, put your hand on it..." },
			{ "Then click the right mouse button.", "...then squeeze and hold the grip." },
			{ "Good! Hold the left mouse button and move the mouse to wield the two-by-four.", "Good! Swing your arm to wield the two-by-four." },
			{ "Press the right button to drop it.", "Let go of the grip to drop it." },
			{ "You may have to crouch to reach them if they're on the ground.", "Crouch down, or press Circle (B), to reach things on the ground." },
			{ "To fire a gun, pick it up, and press the space bar.", "To fire a gun, pick it up and pull the trigger. Grab the barrel with your other hand to steady it." },
			{ "You can aim the gun using the sights.", "Aim down the gun's sights." },
			{ "To adjust your wrist, press shift + left mouse button, and move the mouse.", "Let go of a gun at your hip or over your shoulder to holster it." },
			{ "You can rotate your arm by holding control + the left mouse button, and moving the mouse.", "Squeeze the grip there with an empty hand to draw it again." },
			{ "Press 'F' to throw an object you are holding.", "To throw something, swing your arm and let go of the grip." },
			{ "You will always throw toward the center of your view.", "It flies the way your hand was moving." },
			{ "To strike with an object, pick up the object and press the space bar.", "To strike with an object, hold it and swing it hard." },
			{ "You can always press F1 for a hint about what to do next", "If you get stuck, press F1 on the keyboard for a hint." },
		};
		for (const SPair& pr : apairs)
			if (_stricmp(str_in.c_str(), strSquash(pr.strFrom).c_str()) == 0)
				return pr.strTo;
		// Anything else: swap the mouse and keyboard words for the controllers'.
		static const SPair arepl[] =
		{
			{ "left mouse button", "grip" }, { "right mouse button", "grip" }, { "mouse button", "grip" },
			{ "space bar", "trigger" }, { "spacebar", "trigger" }, { "the mouse", "your hand" },
		};
		std::string str = str_in;
		for (const SPair& pr : arepl)
		{
			std::string str_lo = str;
			for (char& c : str_lo) c = (char)tolower((unsigned char)c);
			size_t i = str_lo.find(pr.strFrom);
			if (i != std::string::npos)
				str = str.substr(0, i) + pr.strTo + str.substr(i + strlen(pr.strFrom));
		}
		return str;
	}

	void ShowGameText(const char* str_text, float f_secs, bool b_chained, int i_type)
	{
		if (!str_text || !*str_text)
			return;
		int q = i_type == 2 ? 1 : 0;		// Subtitles; everything else.
		if (q == 1 && !gVR.bSubtitles)
			return;
		if (q == 0 && !setVR.bTutorial)
			return;
		std::string str = strSquash(str_text);
		if (str.empty() || str.find("TUTORIAL") == 0)
			return;
		if (q == 0)
			str = strVRText(str);
		Log("Text (%d%s, %.1f s): %s", i_type, b_chained ? ", next" : "", f_secs, str.c_str());
		if (!b_chained)
		{
			atqText[q].clear();
			au4TextStart[q] = 0;
		}
		atqText[q].push_back({ str, (std::max)(f_secs, q == 0 ? 3.5f : 1.0f) });
	}

	void RemoveGameText(int i_type)
	{
		if (i_type == 0 || i_type == 2)
			atqText[1].clear();
		if (i_type == 0 || i_type == 3)
			atqText[0].clear();
	}

	void UpdateTextPanels()
	{
		DWORD u4_now = GetTickCount();

		// Level title: fades in a second after the level appears, holds, fades away.
		if (bTitlePending && bLevelLoaded && !bLoadingShown)
		{
			if (!u4TitleStart)
			{
				u4TitleStart = u4_now + 1200;
				VRB_TextPanelSet(0, strTitleLevel.c_str(), strTitleName.c_str());
			}
			float t = (float)((int)(u4_now - u4TitleStart)) / 1000.0f;
			const float fIN = 1.6f, fHOLD = 4.0f, fOUT = 1.8f;
			if (t < 0)
				VRB_TextPanelShow(0, 0, 1.6f, 3.0f, 0, false, true);
			else if (t < fIN + fHOLD + fOUT)
			{
				float f_a = t < fIN ? t / fIN : (t < fIN + fHOLD ? 1.0f : 1.0f - (t - fIN - fHOLD) / fOUT);
				f_a = f_a * f_a * (3.0f - 2.0f * f_a);
				// Drifts toward you and widens slowly as it appears.
				float f_grow = (std::min)(t / (fIN + fHOLD + fOUT), 1.0f);
				VRB_TextPanelShow(0, f_a, 1.7f + 0.15f * f_grow, 3.0f, 0.05f, false, t < 0.05f);
			}
			else
			{
				VRB_TextPanelShow(0, 0, 1.6f, 3.0f, 0, false, false);
				bTitlePending = false;
			}
		}

		// Messages (panel 1) and subtitles (panel 2).
		for (int q = 0; q < 2; q++)
		{
			int i_panel = q + 1;
			if (atqText[q].empty() || bLoadingShown)
			{
				VRB_TextPanelShow(i_panel, 0, 1, 1, 0, true, false);
				continue;
			}
			if (!au4TextStart[q])
			{
				au4TextStart[q] = u4_now;
				VRB_TextPanelSet(i_panel, "", atqText[q].front().str.c_str());
			}
			float t = (float)(u4_now - au4TextStart[q]) / 1000.0f;
			float f_len = atqText[q].front().fSecs;
			if (t >= f_len)
			{
				atqText[q].pop_front();
				au4TextStart[q] = 0;
				if (atqText[q].empty())
					VRB_TextPanelShow(i_panel, 0, 1, 1, 0, true, false);
				continue;
			}
			float f_a = (std::min)((std::min)(t / 0.3f, (f_len - t) / 0.4f), 1.0f);
			if (q == 0)
				VRB_TextPanelShow(1, f_a, 0.9f, 1.3f, 0.32f, true, false);
			else
				VRB_TextPanelShow(2, f_a * 0.95f, 0.85f, 1.3f, 0.45f, true, false);
		}
	}
}

void VRMod_LevelLoading(const char* str_scene)
{
	if (!VRB_IsRunning())
		return;
	// The level's name from its file.
	struct SName { const char* strFile; const char* strLevel; const char* strName; };
	static const SName anames[] =
	{
		{ "be",  "Level 1", "The Beach" },
		{ "jr",  "Level 2", "The Jungle Road" },
		{ "ij",  "Level 3", "The Industrial Jungle" },
		{ "it",  "Level 4", "The Town" },
		{ "lab", "Level 5", "The Lab" },
		{ "as",  "Level 6", "The Ascent Part 1" },
		{ "as2", "Level 7", "The Ascent Part 2" },
		{ "sum", "Level 8", "The Summit" },
	};
	std::string str_file = str_scene ? str_scene : "";
	size_t i_slash = str_file.find_last_of("\\/");
	if (i_slash != std::string::npos)
		str_file = str_file.substr(i_slash + 1);
	size_t i_dot = str_file.find('.');
	std::string str_base = i_dot != std::string::npos ? str_file.substr(0, i_dot) : str_file;
	for (char& c : str_base)
		c = (char)tolower((unsigned char)c);
	std::string str_name = str_base, str_level;
	for (const SName& n : anames)
		if (str_base == n.strFile)
		{
			str_name = n.strName;
			str_level = n.strLevel;
		}
	Log("Loading level %s (%s %s).", str_file.c_str(), str_level.c_str(), str_name.c_str());
	VRB_ShowLoading("Loading...", "");
	bLoadingShown = true;
	bLevelLoaded = false;
	gVR.pfnShowText = ShowGameText;
	gVR.pfnRemoveText = RemoveGameText;
	strTitleLevel = str_level;
	strTitleName = str_name;
	u4TitleStart = 0;
	bTitlePending = !str_level.empty();
	atqText[0].clear();
	atqText[1].clear();
}

void VRMod_LevelLoaded()
{
	bLevelLoaded = true;
	u4LevelLoadedTick = GetTickCount();
	gVR.bDamageGrace = true;
}

// Called after each VR frame is drawn: the new level is on screen, so the notice can go.
static void HideLoadingIfDone()
{
	gVR.pfnShowText = ShowGameText;
	gVR.pfnRemoveText = RemoveGameText;
	UpdateTextPanels();
	// Anne's grace period ends 14 seconds after loading (about 8 after it is on screen).
	if (gVR.bDamageGrace && GetTickCount() - u4LevelLoadedTick > 14000)
		gVR.bDamageGrace = false;
	if (bLoadingShown && bLevelLoaded)
	{
		VRB_HideLoading();
		bLoadingShown = false;
	}
}

void VRMod_Shutdown()
{
	gpfnVRInputHook = nullptr;
	gpfnVRFlipHook  = nullptr;
	gVR.pfnLog      = nullptr;
	gVR.pfnShowText = nullptr;
	gVR.pfnRemoveText = nullptr;
	gVR.bActive     = false;
	gVR.bHeadValid  = false;
	gVR.bBodyTargetValid = false;
	gVR.abHandValid[0] = gVR.abHandValid[1] = false;
	if (bActive)
	{
		VRScene_Shutdown();
		VRB_Shutdown();
		Log("SteamVR shut down.");
	}
	bActive = false;
}

//**********************************************************************************************
bool VRMod_Active()
{
	return bActive;
}

//**********************************************************************************************
bool VRMod_RenderStereo(const std::function<void()>& paint_scene)
{
	if (!bActive)
		return false;

	double d_frame_start = dNowMs();
	if (dLastFrameEnd > 0)
		adTimingMs[etGAP] += d_frame_start - dLastFrameEnd;

	SVRFrame fr;
	bool b_frame_ok = VRB_BeginFrame(fr);
	adTimingMs[etWAIT] += dNowMs() - d_frame_start;
	if (!b_frame_ok)
	{
		if (VRB_QuitRequested())
		{
			Log("SteamVR asked us to quit VR; continuing on the monitor.");
			VRMod_Shutdown();
		}
		return false;
	}
	frLast = fr;

	static bool b_logged_controller = false;
	if (!b_logged_controller && (fr.aposHand[0].bValid || fr.aposHand[1].bValid))
	{
		Log("Controllers: %s", VRB_ControllerType());
		b_logged_controller = true;
	}

	CheckIniReload();

	if (!fr.posHead.bValid)
	{
		gVR.bHeadValid = false;
		gVR.bBodyTargetValid = false;
		return false;
	}

	double d_now = dNowMs();
	float f_dt = dPrevFrameMs > 0 ? (float)((d_now - dPrevFrameMs) * 0.001) : 0.0f;
	if (f_dt > 0.1f)
		f_dt = 0.1f;
	dPrevFrameMs = d_now;

	CCamera* pcam = CWDbQueryActiveCamera().tGet();
	if (!pcam)
		return false;

	pcam->UpdateFromAttached();

	CInstance*    pins_camera_on      = pcam->pinsAttached();
	CPlacement3<> p3_camera_relative  = pcam->p3Relative();
	bool          b_camera_on_head    = pcam->bHead();
	const CVector3<> v3_cam_pos       = pcam->pr3Presence().v3Pos;
	CRotate3<>    r3_cam_rot          = pcam->pr3Presence().r3Rot;
	bool          b_player_view       = gpPlayer && pins_camera_on && pins_camera_on == (CInstance*)gpPlayer && b_camera_on_head;

	// Re-align when switching between the player's own view and a scripted camera
	// (cutscenes, death), or between scripted cameras.
	static bool b_last_player_view = false;
	if (b_player_view != b_last_player_view ||
		(!b_player_view && (pins_camera_on != pvLastCamAttach || b_camera_on_head != bLastCamHead)))
		bNeedAlign = true;
	b_last_player_view = b_player_view;
	pvLastCamAttach = pins_camera_on;
	bLastCamHead    = b_camera_on_head;

	const CVector3<> v3_head_track = v3Game(fr.posHead.afP);
	const CRotate3<> r3_head_track = r3Game(fr.posHead.afQ);

	// Recenter: turn-stick click.  Sprint: walk-stick click.
	const SVRHandInput& hi_off  = fr.ctl.ahand[iOffCtl()];
	const SVRHandInput& hi_main = fr.ctl.ahand[iMainCtl()];
	bool b_recenter = hi_main.bStickClick && !ctlPrevFrame.ahand[iMainCtl()].bStickClick;
	if (hi_off.bStickClick && !ctlPrevFrame.ahand[iOffCtl()].bStickClick)
		bRunToggle = !bRunToggle;

	if (bNeedAlign || b_recenter)
	{
		// Face the way Anne (or the scripted camera) faces, standing where she stands.
		CRotate3<> r3_ref = b_player_view ? gpPlayer->r3Rot() : r3_cam_rot;
		r3Yaw = r3YawRad(fYawOf(r3_ref) - fYawOf(r3_head_track));
		v3Calib = v3_head_track;
		if (b_player_view)
		{
			CVector3<> v3_body = gpPlayer->v3Pos();
			CVector3<> v3_h = v3_head_track * r3Yaw;
			v3Origin = CVector3<>(v3_body.tX - v3_h.tX, v3_body.tY - v3_h.tY, 0);
		}
		v3MoveVel = CVector3<>(0, 0, 0);
		bBaseZInit = false;
		bNeedAlign = false;
		Log("Aligned view (%s, yaw %.1f deg).", b_player_view ? "player" : "scripted camera",
			(fYawOf(r3_ref) - fYawOf(r3_head_track)) / fDEG);
	}

	CVector3<> v3_org;		// This frame's tracking-space origin in the world.

	if (b_player_view)
	{
		//
		// Turning: rotate the room around your head.
		//
		float f_turn = 0;
		if (!setVR.bSmoothTurn)
		{
			if (bSnapArmed && std::fabs(hi_main.fStickX) > 0.65f)
			{
				f_turn = (hi_main.fStickX > 0 ? -1.0f : 1.0f) * setVR.fSnapTurnDeg * fDEG;
				bSnapArmed = false;
			}
			else if (std::fabs(hi_main.fStickX) < 0.3f)
				bSnapArmed = true;
		}
		else if (std::fabs(hi_main.fStickX) > setVR.fTurnDeadzone)
		{
			float f_t = (std::fabs(hi_main.fStickX) - setVR.fTurnDeadzone) / (1.0f - setVR.fTurnDeadzone);
			f_t = f_t * f_t;	// Fine control near the centre.
			f_turn = (hi_main.fStickX > 0 ? -1.0f : 1.0f) * f_t * setVR.fSmoothTurnDeg * fDEG * f_dt;
		}
		if (f_turn != 0)
		{
			CVector3<> v3_old = v3_head_track * r3Yaw;
			r3Yaw = r3Yaw * r3YawRad(f_turn);
			CVector3<> v3_new = v3_head_track * r3Yaw;
			v3Origin.tX += v3_old.tX - v3_new.tX;
			v3Origin.tY += v3_old.tY - v3_new.tY;
			// Keep moving the same way relative to you.
			v3MoveVel = v3MoveVel * r3YawRad(f_turn);
		}

		//
		// Stick locomotion: speed in proportion to how far the stick is pushed, with smooth
		// acceleration and braking.
		//
		CVector3<> v3_want(0, 0, 0);
		{
			float f_x = hi_off.fStickX, f_y = hi_off.fStickY;
			float f_len = std::sqrt(f_x * f_x + f_y * f_y);
			if (f_len > setVR.fMoveDeadzone)
			{
				float f_mag = (std::min)(1.0f, (f_len - setVR.fMoveDeadzone) / (1.0f - setVR.fMoveDeadzone));
				f_x *= f_mag / f_len;
				f_y *= f_mag / f_len;

				int i_off_hand = setVR.bLeftHanded ? 0 : 1;		// Shared-state index of the walk hand.
				CRotate3<> r3_dir = r3_head_track * r3Yaw;
				if (setVR.bMoveWithHand && fr.aposHand[iCtl(i_off_hand)].bValid)
					r3_dir = r3Game(fr.aposHand[iCtl(i_off_hand)].afQ) * r3Yaw;

				CVector3<> v3_fwd = v3Heading(r3_dir);
				CVector3<> v3_right(v3_fwd.tY, -v3_fwd.tX, 0);
				float f_speed = bRunToggle ? setVR.fRunSpeed : setVR.fMoveSpeed;
				if (f_y < 0)
					f_y *= 0.8f;	// A little slower backwards.
				v3_want = (v3_fwd * f_y + v3_right * f_x) * f_speed;
			}
			else
				bRunToggle = false;		// Sprint ends when you let go of the stick.
		}
		float f_k = setVR.fMoveAccel > 0.001f ? 1.0f - std::exp(-f_dt / setVR.fMoveAccel) : 1.0f;
		v3MoveVel += (v3_want - v3MoveVel) * f_k;
		if (fLen(v3MoveVel) < 0.01f && fLen(v3_want) == 0)
			v3MoveVel = CVector3<>(0, 0, 0);
		v3Origin += v3MoveVel * f_dt;

		//
		// Height: Anne's (smoothed) eye height, plus how much higher or lower your real head is
		// than when you recentred.
		//
		float f_target_z = v3_cam_pos.tZ;
		if (gVR.pvKinInstance)
		{
			// The game's camera rides her physics head, which sometimes drifts up off her body.
			// With the character controller, her eyes are a fixed height above its body origin
			// (learned when she first stands); the camera may go lower (crouching), not higher.
			static float f_eye_above = -1.0f;
			static const void* pv_last_kin = nullptr;
			float f_kin_z = gVR.afKinPos[2];
			if (pv_last_kin != gVR.pvKinInstance)
			{
				pv_last_kin = gVR.pvKinInstance;
				f_eye_above = -1.0f;
			}
			float f_now = v3_cam_pos.tZ - f_kin_z;
			if (f_eye_above < 0 && gVR.bKinGrounded && f_now > 0.2f && f_now < 1.3f)
				f_eye_above = f_now;
			if (f_eye_above > 0)
				f_target_z = f_kin_z + (std::min)(f_now, f_eye_above);
		}
		if (!bBaseZInit || std::fabs(f_target_z - fBaseZ) > 1.5f)
			fBaseZ = f_target_z;
		else if (gVR.pvKinInstance)
		{
			// Character controller: jumps and falls are followed exactly; steps and crouching
			// are eased just enough not to jolt.
			if (!gVR.bKinGrounded)
				fBaseZ = f_target_z;
			else
				fBaseZ += (f_target_z - fBaseZ) * (1.0f - std::exp(-f_dt / 0.07f));
		}
		else if (setVR.fHeightSmoothing > 0.001f)
		{
			// Small changes (her walking bob) are smoothed away; big ones (crouch, jump, a
			// drop) are followed quickly.
			fBaseZ += (f_target_z - fBaseZ) * (1.0f - std::exp(-f_dt / setVR.fHeightSmoothing));
		}
		else
			fBaseZ = f_target_z;
		bBaseZInit = true;
		v3Origin.tZ = fBaseZ - v3Calib.tZ + setVR.fExtraHeight;
		gVR.fBodyZShift = fBaseZ - f_target_z;

		//
		// Body: Anne stands under your head.  If she can't get there (blocked), pull the room
		// back so you can't drift away from her.
		//
		CVector3<> v3_body = gpPlayer->v3Pos();
		CRotate3<> r3_head_world = r3_head_track * r3Yaw;
		CVector3<> v3_back = v3Heading(r3_head_world) * 0.08f;		// Eyes are ahead of the neck.

		CVector3<> v3_neck = v3Origin + v3_head_track * r3Yaw - v3_back;
		CVector3<> v3_err = v3Flat(v3_neck - v3_body);
		float f_err = fLen(v3_err);
		if (f_err > 3.0f)
		{
			// Anne was moved (level start, scripted move, respawn): jump to her.
			v3Origin -= v3_err;
			v3MoveVel = CVector3<>(0, 0, 0);
			Log("View jumped to Anne (%.1f m away).", f_err);
		}
		else if (f_err > setVR.fBodyLeash)
			v3Origin -= v3_err * ((f_err - setVR.fBodyLeash) / f_err);

		v3_neck = v3Origin + v3_head_track * r3Yaw - v3_back;
		CVector3<> v3_target = v3_neck;
		gVR.afBodyVel[0] = v3MoveVel.tX;
		gVR.afBodyVel[1] = v3MoveVel.tY;
		gVR.afBodyTarget[0] = v3_target.tX;
		gVR.afBodyTarget[1] = v3_target.tY;
		gVR.afBodyTarget[2] = v3_body.tZ;
		gVR.bBodyTargetValid = true;

		v3_org = v3Origin;
	}
	else
	{
		// Scripted camera: your head sits at the camera, and you can lean a little.
		CVector3<> v3_lean = v3_head_track - v3Calib;
		float f_horiz = std::sqrt(v3_lean.tX * v3_lean.tX + v3_lean.tY * v3_lean.tY);
		if (f_horiz > setVR.fLeanRadius && f_horiz > 0)
		{
			v3_lean.tX *= setVR.fLeanRadius / f_horiz;
			v3_lean.tY *= setVR.fLeanRadius / f_horiz;
		}
		v3_lean.tZ = (std::min)((std::max)(v3_lean.tZ, -1.2f), 0.4f);
		v3_org = v3_cam_pos + (v3_lean - v3_head_track) * r3Yaw;
		gVR.bBodyTargetValid = false;
		v3MoveVel = CVector3<>(0, 0, 0);
	}

	// Tracking point -> world.
	auto v3World = [&](const CVector3<>& v3_track) -> CVector3<>
	{
		return v3_org + v3_track * r3Yaw;
	};

	v3HeadWorld = v3World(v3_head_track);
	StoreQ(gVR.afHeadRotWorld, r3_head_track * r3Yaw);
	gVR.bHeadValid = true;

	//
	// Hands, 1:1 with the controllers.
	//
	bool ab_grip_down[2] = { false, false };
	for (int h = 0; h < 2; h++)
	{
		const SVRPose& pose = fr.aposHand[iCtl(h)];
		abHandValid[h] = pose.bValid;
		if (pose.bValid)
		{
			av3HandWorld[h]    = v3World(v3Game(pose.afP));
			ar3HandWorld[h]    = r3Game(pose.afQ) * r3Yaw;
			av3HandVelTrack[h] = v3Game(pose.afV) * r3Yaw;
		}

		const SVRHandInput& hi = fr.ctl.ahand[iCtl(h)];
		ab_grip_down[h] = hi.bGrip && !abGripPhys[h];
		abGripPhys[h] = hi.bGrip;
		if (setVR.bGripToggle)
		{
			if (ab_grip_down[h])
				abGripLogical[h] = !abGripLogical[h];
		}
		else
			abGripLogical[h] = hi.bGrip;
	}

	//
	// Two-handed guns: one hand holds the gun, the other grabs near it.
	//
	{
		float f_gap = (abHandValid[0] && abHandValid[1]) ? fLen(av3HandWorld[1] - av3HandWorld[0]) : 99.0f;
		for (int g = 0; g < 2; g++)
		{
			int s = 1 - g;					// Supporting hand.
			int i_mode = g == 0 ? 1 : 2;
			if (gVR.iTwoHand == 0 && setVR.bTwoHandAim)
			{
				if (ab_grip_down[s] && gVR.abHoldingGun[g] && !gVR.abHolding[s])
				{
					// Long guns (shotgun, rifles) are held under the barrel; short ones by cupping the grip.
					bool b_pistol = gVR.afHeldLength[g] > 0.0f ? gVR.afHeldLength[g] < 0.55f : gVR.afHeldMass[g] <= setVR.fPistolMaxMass;
					if (f_gap < (b_pistol ? 0.25f : 0.7f))
					{
						gVR.iTwoHand = i_mode;
						gVR.bTwoHandPistol = b_pistol;
						Haptic(s, 0.04f, 120.0f, 0.4f);
					}
				}
			}
			else if (gVR.iTwoHand == i_mode && (!abGripLogical[s] || !gVR.abHoldingGun[g] || gVR.abHolding[s]))
				gVR.iTwoHand = 0;
		}
	}

	CRotate3<> r3_offset = r3AxisAngle(1, 0, 0, (setVR.fHandPitchDeg - 90.0f) * fDEG) *
	                       r3AxisAngle(0, 1, 0, setVR.fHandRollDeg  * fDEG) *
	                       r3AxisAngle(0, 0, 1, (setVR.fHandYawDeg - 90.0f) * fDEG);
	// The same offset, mirrored for the left hand.
	CRotate3<> r3_offset_l(r3_offset.tC, r3_offset.v3S.tX, -r3_offset.v3S.tY, -r3_offset.v3S.tZ, true);

	CRotate3<> ar3_hand[2] = { ar3HandWorld[0], ar3HandWorld[1] };

	// Long guns aim along the line between the hands: the gun hand turns (the least it can, so
	// the gun keeps its roll) until the barrel points at the supporting hand.
	if (gVR.iTwoHand && !gVR.bTwoHandPistol)
	{
		int g = gVR.iTwoHand == 1 ? 0 : 1, s = 1 - g;
		const float* af_g = gVR.afHeldGrab[g];
		CRotate3<> r3_grab(af_g[3], af_g[4], af_g[5], af_g[6], true);
		// (The game's hand frame includes the grip offset rotation, as stored below.)
		CVector3<> v3_barrel = CVector3<>(0, 1, 0) * r3_grab * (g == 0 ? r3_offset : r3_offset_l) * ar3HandWorld[g];
		CVector3<> v3_want = av3HandWorld[s] - av3HandWorld[g];
		if (fLen(v3_want) > 0.1f && fLen(v3_barrel) > 0.5f)
			ar3_hand[g] = ar3HandWorld[g] * CRotate3<>(CDir3<>(v3_barrel), CDir3<>(v3_want));
	}

	//
	// Recoil: each shot kicks the gun hand's muzzle up and the gun back, easing off over a
	// fraction of a second.  Harder held in one hand; a supporting hand steadies it.
	//
	{
		static int ai_seen[2] = { 0, 0 };
		static float af_kick[2] = { 0, 0 };		// 0..1, decaying.
		static double d_last = 0;
		double d_now = dNowMs();
		float f_dt_k = d_last > 0 ? (float)((d_now - d_last) / 1000.0) : 0.0f;
		d_last = d_now;
		for (int h = 0; h < 2; h++)
		{
			if (gVR.aiShotCount[h] != ai_seen[h])
			{
				ai_seen[h] = gVR.aiShotCount[h];
				if (gVR.abHoldingGun[h])
					af_kick[h] = 1.0f;
			}
			af_kick[h] *= std::exp(-f_dt_k / 0.09f);
			if (af_kick[h] < 0.01f || !gVR.abHoldingGun[h])
			{
				af_kick[h] = 0;
				continue;
			}
			bool b_steadied = gVR.iTwoHand == (h == 0 ? 1 : 2);
			bool b_long = gVR.afHeldLength[h] >= 0.55f;
			float f_deg = (b_steadied ? 5.0f : (b_long ? 16.0f : 11.0f)) * setVR.fRecoil;
			float f_back = (b_steadied ? 0.02f : 0.05f) * setVR.fRecoil;
			const float* af_g = gVR.afHeldGrab[h];
			CRotate3<> r3_grab(af_g[3], af_g[4], af_g[5], af_g[6], true);
			CVector3<> v3_barrel = CVector3<>(0, 1, 0) * r3_grab * (h == 0 ? r3_offset : r3_offset_l) * ar3_hand[h];
			CVector3<> v3_axis = v3_barrel ^ CVector3<>(0, 0, 1);
			if (fLen(v3_axis) < 0.05f || fLen(v3_barrel) < 0.5f)
				continue;
			// Turn the barrel toward up by the kick angle (rotation in world space, after the hand).
			CVector3<> v3_b = v3_barrel / fLen(v3_barrel);
			float f_rad = f_deg * af_kick[h] * fDEG;
			CVector3<> v3_up_dir = CVector3<>(0, 0, 1) - v3_b * v3_b.tZ;
			if (fLen(v3_up_dir) > 0.05f)
			{
				v3_up_dir = v3_up_dir / fLen(v3_up_dir);
				CVector3<> v3_kicked = v3_b * std::cos(f_rad) + v3_up_dir * std::sin(f_rad);
				ar3_hand[h] = ar3_hand[h] * CRotate3<>(CDir3<>(v3_b), CDir3<>(v3_kicked));
			}
			av3HandWorld[h] = av3HandWorld[h] - v3_b * (f_back * af_kick[h]);
		}
	}

	for (int h = 0; h < 2; h++)
	{
		const SVRHandInput& hi = fr.ctl.ahand[iCtl(h)];
		bool b_enabled = abHandValid[h] && (h == 0 || setVR.bSecondHand);
		gVR.abHandValid[h] = b_enabled;
		if (!b_enabled)
		{
			gVR.abGrip[h] = gVR.abTrigger[h] = false;
			gVR.abForceAim[h] = false;
			gVR.aiHolsterRequest[h] = -1;
			continue;
		}

		StoreV(gVR.aafHandPos[h], av3HandWorld[h]);
		StoreQ(gVR.aafHandRot[h], (h == 0 ? r3_offset : r3_offset_l) * ar3_hand[h]);
		StoreV(gVR.aafHandVel[h], av3HandVelTrack[h] + v3MoveVel);
		{
			// Peak speed over the last ~0.1 s, for throws.
			static double ad_peak_t[2] = { 0, 0 };
			static CVector3<> av3_peak[2];
			CVector3<> v3_now = av3HandVelTrack[h] + v3MoveVel;
			double d_t = dNowMs();
			if (fLen(v3_now) >= fLen(av3_peak[h]) || d_t - ad_peak_t[h] > 100.0)
			{
				av3_peak[h] = v3_now;
				ad_peak_t[h] = d_t;
			}
			StoreV(gVR.aafHandVelPeak[h], av3_peak[h]);
		}
		gVR.afCurl[h]    = (std::max)(hi.fGrip, (std::min)(1.0f, 0.4f * hi.fTrigger + 0.6f * hi.fGrip));
		gVR.abGrip[h]    = abGripLogical[h];
		gVR.abTrigger[h] = hi.bTrigger;

		// The player draws from / puts into a holster when the grip changes there.
		gVR.aiHolsterRequest[h] = iHolsterNear(h);

		// Force grab: trigger held with an empty hand and no grip.
		gVR.abForceAim[h] = setVR.bForceGrab && !gVR.abHolding[h] && hi.bTrigger && !abGripLogical[h];
		StoreV(gVR.aafAimPosWorld[h], av3HandWorld[h]);
		StoreV(gVR.aafAimDirWorld[h], CVector3<>(0, 1, 0) * ar3HandWorld[h]);
		if (!gVR.abForceAim[h])
			gVR.abForcePull[h] = false;
	}

	UpdateFeedback();

	// Both stick clicks together: save the last few seconds of what you saw.
	{
		bool b_both = fr.ctl.ahand[0].bStickClick && fr.ctl.ahand[1].bStickClick;
		bool b_both_prev = ctlPrevFrame.ahand[0].bStickClick && ctlPrevFrame.ahand[1].bStickClick;
		if (b_both && !b_both_prev)
			SaveCapture();
	}
	ctlPrevFrame = fr.ctl;

	//
	// Modern renderer: draws the world on the graphics card straight into the headset images.
	//
	if (setVR.bNewRenderer && VRScene_Ready() && !bNewRendererFailed)
	{
		if (b_player_view)
			gpPlayer->VRShowLeftArm(true);

		SVRSceneEyes eyes;
		for (int i_eye = 0; i_eye < 2; i_eye++)
		{
			CRotate3<> r3_eye = r3Game(fr.aposEye[i_eye].afQ) * r3Yaw;
			CVector3<> v3_eye = v3World(v3Game(fr.aposEye[i_eye].afP));
			CVector3<> av3[3] = { CVector3<>(1, 0, 0) * r3_eye, CVector3<>(0, 1, 0) * r3_eye, CVector3<>(0, 0, 1) * r3_eye };
			StoreV(eyes.afPos[i_eye], v3_eye);
			for (int k = 0; k < 3; k++)
				StoreV(&eyes.afAxes[i_eye][k * 3], av3[k]);
			const SVRFov& fov = fr.afovEye[i_eye];
			eyes.afTan[i_eye][0] = fov.fTanLeft;
			eyes.afTan[i_eye][1] = fov.fTanRight;
			eyes.afTan[i_eye][2] = fov.fTanDown;
			eyes.afTan[i_eye][3] = fov.fTanUp;
		}

		SVRSceneSettings set;
		set.fShadowDistance = setVR.iShadowMapSize > 0 ? setVR.fShadowDistance : 0.0f;
		set.fBrightness = setVR.fBrightness;
		set.fTerrainDetail = setVR.fTerrainDetail;
		set.fFogScale = setVR.fFogScale;
		set.bBumpMaps = setVR.bBumpMaps;

		double d_t0 = dNowMs();
		SSceneCall sc = { &eyes, &set, false };
		DWORD u4_code = 0;
		bool b_ok = false;
		if (bSceneCallSafe(&sc, &u4_code))
			b_ok = sc.bResult;
		else
		{
			// A bad frame is dropped and the next one tried (the game's old renderer can't
			// safely take over mid-level); only repeated failures switch it off.
			VRScene_AbortFrame();
			iSceneCrashes++;
			unsigned u_offset = (unsigned)((uintptr_t)pvSceneCrashAddress - (uintptr_t)GetModuleHandleA(nullptr));
			Log("Modern renderer error %d (code %08X at exe offset %08X); frame skipped.", iSceneCrashes, (unsigned)u4_code, u_offset);
			if (iSceneCrashes >= 30)
			{
				bNewRendererFailed = true;
				Log("Modern renderer: too many errors; switching to the game's renderer for this session.");
			}
		}
		adTimingMs[etPAINT] += dNowMs() - d_t0;

		if (gpPlayer)
			gpPlayer->VRShowLeftArm(false);

		// The game runs its dinosaur AI from inside its own renderer (while it waits for the
		// card); that renderer is skipped here, so run it now.
		if (gpaiSystem)
			gpaiSystem->ProcessPending();

		if (b_ok)
		{
			HideLoadingIfDone();
			CaptureNewRenderer();
			bStereoThisFrame = true;
			iTimingFrames++;
			dLastFrameEnd = dNowMs();
			adTimingMs[etFRAME] += dLastFrameEnd - d_frame_start;
			return true;
		}
	}

	//
	// Render each eye.
	//
	CCamera::SProperties camprop_orig = pcam->campropGetProperties();
	pcam->SetAttached(0);

	// Sky and image-cache overrides for VR, restored afterwards.
	CSkyRender::SkyDrawMode sdm_orig = gpskyRender ? gpskyRender->sdmGetDrawMode() : CSkyRender::sdmTextured;
	if (gpskyRender && setVR.iSkyMode >= 0 && setVR.iSkyMode <= 2)
		gpskyRender->SetDrawMode((CSkyRender::SkyDrawMode)setVR.iSkyMode);
	ERenderCacheModes ercm_orig = rcsRenderCacheSettings.erctMode;
	if (!setVR.bImageCaches)
		rcsRenderCacheSettings.erctMode = ercmCACHE_OFF;

	// Texture perspective: the original tolerances wobble badly in a headset.
	float f_persp_orig = fPerspectivePixelError, f_alt_persp_orig = fAltPerspectivePixelError;
	int i_sub_orig = persetSettings.iMinSubdivision, i_alt_sub_orig = persetSettings.iAltMinSubdivision;
	int i_adapt_orig = persetSettings.iAdaptiveMinSubdivision;
	if (setVR.bSharpTextures)
	{
		fPerspectivePixelError    = (std::min)(fPerspectivePixelError, 0.35f);
		fAltPerspectivePixelError = (std::min)(fAltPerspectivePixelError, 0.5f);
		persetSettings.iMinSubdivision         = (std::min)(persetSettings.iMinSubdivision, 16);
		persetSettings.iAltMinSubdivision      = (std::min)(persetSettings.iAltMinSubdivision, 16);
		persetSettings.iAdaptiveMinSubdivision = (std::min)(persetSettings.iAdaptiveMinSubdivision, 16);
	}

	// Bump-mapped objects on the card are lit and drawn as ordinary textured objects.
	CSet<ERenderFeature> seterf_orig = prenMain->pSettings->seterfState;
	if (gVR.bBumpHardware && d3dDriver.bUseD3D())
		prenMain->pSettings->seterfState -= erfBUMP;

	// Occlusion culling.
	bool b_occ_obj = COcclude::bUseObjectOcclusion, b_occ_poly = COcclude::bUsePolygonOcclusion;
	if (!setVR.bOcclusion)
		COcclude::bUseObjectOcclusion = COcclude::bUsePolygonOcclusion = false;

	// Both arms: posed to the controllers for this frame.
	if (b_player_view)
		gpPlayer->VRShowLeftArm(true);

	static bool b_logged_format = false;

	for (int i_eye = 0; i_eye < 2; i_eye++)
	{
		const SVRFov& fov = fr.afovEye[i_eye];

		CCamera::SProperties camprop = camprop_orig;
		float f_half_w = (fov.fTanRight - fov.fTanLeft) * 0.5f;
		float f_half_h = (fov.fTanUp    - fov.fTanDown) * 0.5f;
		float f_cx     = (fov.fTanRight + fov.fTanLeft) * 0.5f;
		float f_cy     = (fov.fTanUp    + fov.fTanDown) * 0.5f;
		if (setVR.bFlipFovX)
			f_cx = -f_cx;
		if (setVR.bFlipFovY)
			f_cy = -f_cy;

		camprop.rViewWidth   = f_half_w;
		camprop.fAspectRatio = f_half_w / f_half_h;
		camprop.fZoomFactor  = 1.0f;
		camprop.vpViewport.SetVirtualOrigin(f_cx, f_cy);
		pcam->SetProperties(camprop);

		CRotate3<> r3_eye = r3Game(fr.aposEye[i_eye].afQ) * r3Yaw;
		CVector3<> v3_eye = v3World(v3Game(fr.aposEye[i_eye].afP));
		pcam->Move(CPlacement3<>(r3_eye, v3_eye));

		// Radial fog: how to get from normalised camera space back to real distances.
		if (setVR.bRadialFog)
		{
			CTransform3<> tf3_inv = ~pcam->tf3ToNormalisedCamera();
			const CMatrix3<>& mx3 = tf3_inv.mx3Mat;
			float* m = gVR.afFogMat;
			m[0] = mx3.v3X.tX; m[1] = mx3.v3X.tY; m[2] = mx3.v3X.tZ;
			m[3] = mx3.v3Y.tX; m[4] = mx3.v3Y.tY; m[5] = mx3.v3Y.tZ;
			m[6] = mx3.v3Z.tX; m[7] = mx3.v3Z.tY; m[8] = mx3.v3Z.tZ;
			StoreV(gVR.afFogFwd, CVector3<>(0, 1, 0) * r3_eye);
			gVR.bRadialFog = true;
		}

		// The first eye's image is still in the colour/Z buffers; clear before the second.
		if (i_eye == 1)
			d3dDriver.ClearZBuffer();

		double d_t0 = dNowMs();
		paint_scene();
		double d_t1 = dNowMs();
		adTimingMs[etPAINT] += d_t1 - d_t0;

		bReadBackBuffer([&](const void* pv, int i_pitch, int i_w, int i_h, int i_bits, uint32_t r, uint32_t g, uint32_t b)
		{
			if (i_eye == 0)
				CaptureEye(pv, i_pitch, i_w, i_h, i_bits);
			if (!b_logged_format)
			{
				Log("Back buffer %dx%d, %d bpp, masks R%08X G%08X B%08X.", i_w, i_h, i_bits, r, g, b);
				b_logged_format = true;
			}
			double d_t2 = dNowMs();
			adTimingMs[etREADBACK] += d_t2 - d_t1;
			VRB_SubmitEye(i_eye, pv, i_pitch, i_w, i_h, i_bits, r, g, b);
			adTimingMs[etSUBMIT] += dNowMs() - d_t2;
		});
	}

	if (gpPlayer)
		gpPlayer->VRShowLeftArm(false);

	prenMain->pSettings->seterfState = seterf_orig;
	COcclude::bUseObjectOcclusion = b_occ_obj;
	COcclude::bUsePolygonOcclusion = b_occ_poly;
	gVR.bRadialFog = false;
	fPerspectivePixelError    = f_persp_orig;
	fAltPerspectivePixelError = f_alt_persp_orig;
	persetSettings.iMinSubdivision         = i_sub_orig;
	persetSettings.iAltMinSubdivision      = i_alt_sub_orig;
	persetSettings.iAdaptiveMinSubdivision = i_adapt_orig;

	if (gpskyRender)
		gpskyRender->SetDrawMode(sdm_orig);
	rcsRenderCacheSettings.erctMode = ercm_orig;

	// Restore the camera.
	pcam->SetProperties(camprop_orig);
	pcam->Move(CPlacement3<>(r3_cam_rot, v3_cam_pos));
	pcam->SetAttached(pins_camera_on, b_camera_on_head);
	pcam->SetRelative(p3_camera_relative);

#if VER_TIMING_STATS
	// (Release exe only.)  The engine's own profile of where the frame time goes.
	{
		static CCycleTimer ctmr_frame;
		static int i_profile_frames = 0;
		proProfile.psFrame.Add(ctmr_frame(), 1);
		if (++i_profile_frames >= 400)
		{
			i_profile_frames = 0;
			CStrBuffer strbuf(60000);
			proProfile.psFrame.WriteToBuffer(strbuf, 0, true);
			std::string str(&strbuf[0], strbuf.uLen);
			Log("Profile (per frame, both eyes):");
			size_t u_pos = 0;
			while (u_pos < str.size())
			{
				size_t u_end = str.find('\n', u_pos);
				if (u_end == std::string::npos)
					u_end = str.size();
				if (u_end > u_pos)
					Log("  %s", str.substr(u_pos, u_end - u_pos).c_str());
				u_pos = u_end + 1;
			}
			proProfile.psFrame.Reset();
		}
	}
#endif

	bStereoThisFrame = true;
	iTimingFrames++;
	dLastFrameEnd = dNowMs();
	adTimingMs[etFRAME] += dLastFrameEnd - d_frame_start;
	return true;
}
