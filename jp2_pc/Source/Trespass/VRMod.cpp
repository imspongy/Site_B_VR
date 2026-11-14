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

#include "Lib/Control/VRShared.hpp"
#include "Lib/Renderer/Camera.hpp"
#include "Lib/Renderer/ScreenRenderAuxD3D.hpp"
#include "Lib/W95/Direct3D.hpp"
#include "Lib/EntityDBase/Query/QRenderer.hpp"
#include "Lib/EntityDBase/Instance.hpp"
#include "Game/DesignDaemon/Player.hpp"
#include "Lib/Renderer/Sky.hpp"
#include "Lib/Sys/RegInit.hpp"
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
	const int iINI_VERSION = 4;		// Older files are replaced with the new defaults.

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
		bool  bSmoothTurn     = false;	// false = snap turn.
		float fSnapTurnDeg    = 45.0f;	// Degrees per snap.
		float fSmoothTurnDeg  = 150.0f;	// Degrees per second at full stick.
		float fTurnDeadzone   = 0.2f;

		float fBodyLeash      = 0.4f;	// How far your head may get from Anne's body (blocked).
		float fHeightSmoothing = 0.1f;	// Seconds; smooths view height on stairs and slopes.
		int   iSkyMode        = 2;		// 0 = solid colour, 1 = gradient, 2 = clouds.
		bool  bImageCaches    = false;	// The game's distant-object image caches.
		bool  bRadialFog      = true;	// Fog by true distance (doesn't change as you turn).
		float fDrawDistance   = 6.0f;	// Multiplier on how far away objects are drawn.
		bool  bSharpTextures  = true;	// More accurate texture perspective (less wobble).
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
		char str[512];
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
			"SmoothTurn=0\n"
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
			"; If guns aim too high, try HandPitch=-20.\n"
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
			"DrawDistance=6\n"
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
		s.fDrawDistance   = (std::min)((std::max)(fIni(str_file, "DrawDistance", 6), 0.5f), 8.0f);
		s.bMoveWithHand   = fIni(str_file, "MoveWithHand", 0) != 0;
		s.fMoveSpeed      = fIni(str_file, "MoveSpeed", 2.2f);
		s.fRunSpeed       = fIni(str_file, "RunSpeed", 4.5f);
		s.fMoveAccel      = fIni(str_file, "MoveAcceleration", 0.15f);
		s.fMoveDeadzone   = fIni(str_file, "MoveDeadzone", 0.12f);
		s.bSmoothTurn     = fIni(str_file, "SmoothTurn", 0) != 0;
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
			for (int i = 0; i < 5; i++)
			{
				qdQualitySettings[i].fCullDistScale        = aqd_orig[i].fCullDistScale * f_dd;
				qdQualitySettings[i].fCullShadowDistScale  = aqd_orig[i].fCullShadowDistScale * (std::min)(f_dd, 2.0f);
				qdQualitySettings[i].fDetailReductionScale = aqd_orig[i].fDetailReductionScale * f_dd * f_dd;
				qdQualitySettings[i].fFarClipScale         = (std::min)(1.0f, aqd_orig[i].fFarClipScale * f_dd);
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
			VRB_Haptic(iCtl(i_hand), f_seconds, f_freq, (std::min)(1.0f, f_amp * setVR.fHaptics));
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
			CVector3<> v3_h(gVR.aafHolsterPosWorld[i][0], gVR.aafHolsterPosWorld[i][1], gVR.aafHolsterPosWorld[i][2]);
			// The holster points are on Anne's right side; mirror them for the left hand.
			if (i_hand == 1 && gpPlayer)
			{
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
				Haptic(h, 0.09f, 90.0f, 1.0f);
				// The supporting hand feels it too.
				if (gVR.iTwoHand == (h == 0 ? 1 : 2))
					Haptic(1 - h, 0.07f, 90.0f, 0.6f);
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
			float f_y = setVR.bMenuFlipY ? (prasMainScreen->iHeight - ev.fY) : ev.fY;
			int i_x = (int)ev.fX;
			int i_y = (int)f_y;
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
					VRB_ShowOverlay(pv, i_pitch, i_w, i_h, i_bits, r, g, b,
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
	if (prasMainScreen)
		Log("Game render size %dx%d, %d bpp.", prasMainScreen->iWidth, prasMainScreen->iHeight, prasMainScreen->iPixelBits);
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
void VRMod_Shutdown()
{
	gpfnVRInputHook = nullptr;
	gpfnVRFlipHook  = nullptr;
	gVR.pfnLog      = nullptr;
	gVR.bActive     = false;
	gVR.bHeadValid  = false;
	gVR.bBodyTargetValid = false;
	gVR.abHandValid[0] = gVR.abHandValid[1] = false;
	if (bActive)
	{
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
		if (!bBaseZInit || std::fabs(f_target_z - fBaseZ) > 1.5f)
			fBaseZ = f_target_z;
		else if (setVR.fHeightSmoothing > 0.001f)
		{
			// Small changes (her walking bob) are smoothed away; big ones (crouch, jump, a
			// drop) are followed quickly.
			fBaseZ += (f_target_z - fBaseZ) * (1.0f - std::exp(-f_dt / setVR.fHeightSmoothing));
		}
		else
			fBaseZ = f_target_z;
		bBaseZInit = true;
		v3Origin.tZ = fBaseZ - v3Calib.tZ;
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
					bool b_pistol = gVR.afHeldMass[g] <= setVR.fPistolMaxMass;
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

	CRotate3<> r3_offset = r3AxisAngle(1, 0, 0, setVR.fHandPitchDeg * fDEG) *
	                       r3AxisAngle(0, 1, 0, setVR.fHandRollDeg  * fDEG) *
	                       r3AxisAngle(0, 0, 1, setVR.fHandYawDeg   * fDEG);
	// The same offset, mirrored for the left hand.
	CRotate3<> r3_offset_l(r3_offset.tC, r3_offset.v3S.tX, -r3_offset.v3S.tY, -r3_offset.v3S.tZ, true);

	CRotate3<> ar3_hand[2] = { ar3HandWorld[0], ar3HandWorld[1] };

	// Long guns aim along the line between the hands.
	if (gVR.iTwoHand == 1 && !gVR.bTwoHandPistol)
		ar3_hand[0] = r3LookAt(av3HandWorld[1] - av3HandWorld[0], CVector3<>(0, 0, 1) * ar3HandWorld[0]);
	else if (gVR.iTwoHand == 2 && !gVR.bTwoHandPistol)
		ar3_hand[1] = r3LookAt(av3HandWorld[0] - av3HandWorld[1], CVector3<>(0, 0, 1) * ar3HandWorld[1]);

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
	ctlPrevFrame = fr.ctl;

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

	bStereoThisFrame = true;
	iTimingFrames++;
	dLastFrameEnd = dNowMs();
	adTimingMs[etFRAME] += dLastFrameEnd - d_frame_start;
	return true;
}
