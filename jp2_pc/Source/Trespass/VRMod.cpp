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
 * - The headset orientation is fed back to the player (Player.cpp) so the in-game head, aiming,
 *   walking direction and arm all follow where you are actually looking.  The main-hand
 *   controller drives Anne's (physics-simulated) arm directly, so weight, collisions and recoil
 *   all come from the original physics.
 * - Menus and loading screens are shown on a floating virtual screen (SteamVR overlay) that you
 *   can point at with a laser and click with the trigger.
 *
 * Coordinates: OpenVR is x right, y up, -z forward.  The game is x right, y forward, z up.
 *   game = (vr.x, -vr.z, vr.y);  quaternion (w, x, y, z) -> (w, x, -z, y).
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

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern HWND g_hwnd;

namespace
{
	const float fDEG = 3.14159265358979f / 180.0f;

	//******************************************************************************************
	//
	// Settings (TrespasserVR.ini, [VR] section, next to OpenTrespasser.exe).
	// The file is re-read automatically when it changes, so it can be edited while playing.
	//
	struct SSettings
	{
		bool  bEnabled        = true;
		bool  bMotionHand     = true;
		bool  bLeftHanded     = false;	// Arm follows the left controller; controls mirrored.
		bool  bSmoothTurn     = false;	// false = snap turn.
		float fSnapTurnDeg    = 45.0f;	// Degrees per snap.
		float fSmoothTurnDeg  = 150.0f;	// Degrees per second at full stick.
		float fTurnDeadzone   = 0.2f;
		bool  bMoveWithHand   = false;	// Stick moves toward where the off hand points.
		float fWalkSpeed      = 0.6f;	// Stick speed when not running (1.0 = game's run).
		float fMoveDeadzone   = 0.15f;
		bool  bGripToggle     = false;	// false = hold grip to hold; true = press to grab/drop.
		bool  bForceGrab      = true;
		bool  bTwoHandAim     = true;
		bool  bSecondHand     = true;	// Second arm, driven by the left controller.
		float fPistolMaxMass  = 2.5f;	// Guns lighter than this get the pistol support grip.
		bool  bFlipSecondCurl = false;	// Debug: second hand's fingers bend the wrong way.
		float fHolsterRadius  = 0.25f;	// Metres from holster point that counts as "at" it.
		float fHaptics        = 1.0f;	// Haptic strength multiplier (0 = off).
		float fHandPitchDeg   = 0.0f;	// Controller -> in-game hand orientation offset.
		float fHandYawDeg     = 0.0f;
		float fHandRollDeg    = 0.0f;
		float fLeanRadius     = 0.6f;	// Max physical head offset from the recentre point.
		float fOverlayWidth   = 2.4f;	// Menu screen width in metres.
		float fOverlayDist    = 2.0f;	// Menu screen distance in metres.
		bool  bFlipFovX       = false;	// Debug: invert the off-axis direction.
		bool  bFlipFovY       = false;
		bool  bMenuFlipY      = false;	// Debug: laser pointer Y inverted on the menu screen.
		bool  bMirrorToMonitor = false;	// Also show the game on the monitor (costs speed).
	};

	SSettings setVR;

	//******************************************************************************************
	//
	// State.
	//
	bool        bStarted    = false;	// Startup attempted.
	bool        bActive     = false;	// Session running.
	bool        bStereoThisFrame = false;
	bool        bOverlayShown = false;

	SVRFrame    frLast;					// Most recent frame data (also read by the input hook).
	CRotate3<>  r3Yaw;					// Tracking space -> world yaw.
	CVector3<>  v3Calib(0, 0, 0);		// Tracking-space head position at recentre.
	bool        bNeedAlign  = true;
	const void* pvLastCamAttach = nullptr;
	bool        bLastCamHead = false;

	// World-space hands (main = arm hand, off = support hand), updated each render frame.
	bool        abHandValid[2] = { false, false };
	CVector3<>  av3HandWorld[2];
	CRotate3<>  ar3HandWorld[2];
	CVector3<>  av3HandVel[2];			// Tracking-space velocity, game axes (m/s).
	CVector3<>  av3HandPrevTrack[2];
	CVector3<>  v3HeadWorld(0, 0, 0);
	DWORD       u4PrevFrameTime = 0;

	// Input edge state.
	SVRControls ctlPrev;
	bool        bRunToggle    = false;
	bool        bCrouchToggle = false;
	bool        bSnapArmed    = true;
	bool        bSuppressGrab = false;	// Grip press was used for a holster; don't grab.
	int         iGrabPulse    = 0;		// Frames of a forced GRAB press (drop).
	int         aiLastHolsterNear[2] = { -1, -1 };
	bool        abPrevGripRender[2] = { false, false };

	// Haptic feedback state.
	int         iSeenShots = 0, iSeenLeftShots = 0, iSeenPickups = 0, iSeenLeftPickups = 0;
	int         aiSeenForceSerial[2] = { 0, 0 };
	CVector3<>  av3PrevVel[2];

	// Controller indices: the right controller always drives Anne's real (right) arm and the
	// left controller her second (left) arm.  Handedness only swaps sticks and face buttons.
	const int   iRIGHT = 1, iLEFT = 0;

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
		std::fputs(
			"; Site B VR settings.  Changes take effect within a couple of seconds, even in game.\n"
			"; Delete this file to restore defaults.\n"
			"[VR]\n"
			"; 0 = play on the monitor as normal.\n"
			"Enabled=1\n"
			"\n"
			"; --- Hands ---\n"
			"; 1 = swap the sticks and face buttons for left-handed play (move on the right stick,\n"
			";     jump/crouch on the left buttons). Both hands always work the same.\n"
			"LeftHanded=0\n"
			"; 0 = hold the grip to keep holding things (let go to drop).\n"
			"; 1 = press grip once to grab, press again to drop.\n"
			"GripToggle=0\n"
			"; Point at an item, hold the trigger and flick your wrist to pull it to you.\n"
			"ForceGrab=1\n"
			"; Grab the front of a gun with your other hand to aim it with both hands.\n"
			"TwoHandAim=1\n"
			"; Anne's second arm on your left controller (grab, carry, push, fire, holster).\n"
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
			"; 0 = Anne's arm moves like the original game (mouse style).\n"
			"MotionHand=1\n"
			"\n"
			"; --- Movement ---\n"
			"; 0 = walk where you look, 1 = walk where your off hand points.\n"
			"MoveWithHand=0\n"
			"; Walking speed when run is off (stick click toggles run). 1 = full run speed.\n"
			"WalkSpeed=0.6\n"
			"MoveDeadzone=0.15\n"
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
			"; --- Comfort / view ---\n"
			"; 1 = also show the game on your monitor (slower).\n"
			"MirrorToMonitor=0\n"
			"; How far (metres) you can physically lean before the view stops following.\n"
			"LeanRadius=0.6\n"
			"; Size and distance (metres) of the floating menu screen.\n"
			"MenuScreenWidth=2.4\n"
			"MenuScreenDistance=2.0\n"
			"\n"
			"; --- Debug ---\n"
			"; Set to 1 if the image looks skewed or doubled.\n"
			"FlipFovX=0\n"
			"FlipFovY=0\n"
			"; Set to 1 if the menu laser moves up when you point down.\n"
			"MenuFlipY=0\n"
			"; Set to 1 if the second hand's fingers bend backwards when you squeeze.\n"
			"FlipSecondHandCurl=0\n", pf);
		std::fclose(pf);
	}

	//******************************************************************************************
	void LoadSettings()
	{
		std::string str_file = strIniPath();

		if (GetFileAttributesA(str_file.c_str()) == INVALID_FILE_ATTRIBUTES)
			WriteDefaultIni(str_file);

		SSettings s;
		s.bEnabled       = fIni(str_file, "Enabled", 1) != 0;
		s.bMotionHand    = fIni(str_file, "MotionHand", 1) != 0;
		s.bLeftHanded    = fIni(str_file, "LeftHanded", 0) != 0;
		s.bGripToggle    = fIni(str_file, "GripToggle", 0) != 0;
		s.bForceGrab     = fIni(str_file, "ForceGrab", 1) != 0;
		s.bTwoHandAim    = fIni(str_file, "TwoHandAim", 1) != 0;
		s.bSecondHand    = fIni(str_file, "SecondHand", 1) != 0;
		s.fPistolMaxMass = fIni(str_file, "PistolMaxMass", 2.5f);
		s.bFlipSecondCurl = fIni(str_file, "FlipSecondHandCurl", 0) != 0;
		s.fHolsterRadius = fIni(str_file, "HolsterRadius", 0.25f);
		s.fHaptics       = fIni(str_file, "Haptics", 1);
		s.fHandPitchDeg  = fIni(str_file, "HandPitch", 0);
		s.fHandYawDeg    = fIni(str_file, "HandYaw", 0);
		s.fHandRollDeg   = fIni(str_file, "HandRoll", 0);
		s.bMoveWithHand  = fIni(str_file, "MoveWithHand", 0) != 0;
		s.fWalkSpeed     = fIni(str_file, "WalkSpeed", 0.6f);
		s.fMoveDeadzone  = fIni(str_file, "MoveDeadzone", 0.15f);
		s.bSmoothTurn    = fIni(str_file, "SmoothTurn", 0) != 0;
		s.fSnapTurnDeg   = fIni(str_file, "SnapTurnDegrees", 45);
		s.fSmoothTurnDeg = fIni(str_file, "SmoothTurnSpeed", 150);
		s.fTurnDeadzone  = fIni(str_file, "TurnDeadzone", 0.2f);
		s.fLeanRadius    = fIni(str_file, "LeanRadius", 0.6f);
		s.fOverlayWidth  = fIni(str_file, "MenuScreenWidth", 2.4f);
		s.fOverlayDist   = fIni(str_file, "MenuScreenDistance", 2.0f);
		s.bFlipFovX      = fIni(str_file, "FlipFovX", 0) != 0;
		s.bFlipFovY      = fIni(str_file, "FlipFovY", 0) != 0;
		s.bMenuFlipY     = fIni(str_file, "MenuFlipY", 0) != 0;
		s.bMirrorToMonitor = fIni(str_file, "MirrorToMonitor", 0) != 0;
		setVR = s;

		gVR.bMotionHand  = setVR.bMotionHand;
		gVR.bSkipMonitor = !setVR.bMirrorToMonitor;
		gVR.bLeftArm     = setVR.bSecondHand;
		gVR.fCurlSign    = setVR.bFlipSecondCurl ? -1.0f : 1.0f;

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

	// Physical head offset from the recentre point, limited to the lean radius.
	CVector3<> v3Lean(const CVector3<>& v3_head_track)
	{
		CVector3<> v3 = v3_head_track - v3Calib;
		float f_horiz = std::sqrt(v3.tX * v3.tX + v3.tY * v3.tY);
		if (f_horiz > setVR.fLeanRadius && f_horiz > 0)
		{
			float f = setVR.fLeanRadius / f_horiz;
			v3.tX *= f;
			v3.tY *= f;
		}
		if (v3.tZ > 0.4f)
			v3.tZ = 0.4f;
		if (v3.tZ < -1.2f)
			v3.tZ = -1.2f;
		return v3;
	}

	inline int iMain()
	{
		return setVR.bLeftHanded ? 0 : 1;
	}

	inline int iOff()
	{
		return setVR.bLeftHanded ? 1 : 0;
	}

	inline void Haptic(int i_hand, float f_seconds, float f_freq, float f_amp)
	{
		if (setVR.fHaptics > 0)
			VRB_Haptic(i_hand, f_seconds, f_freq, (std::min)(1.0f, f_amp * setVR.fHaptics));
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
	// Aligns tracking yaw so the headset's current heading matches the given world rotation.
	void Align(const CRotate3<>& r3_world, const SVRFrame& fr, bool b_reset_position)
	{
		CRotate3<> r3_head = r3Game(fr.posHead.afQ);
		r3Yaw = r3YawRad(fYawOf(r3_world) - fYawOf(r3_head));
		if (b_reset_position)
			v3Calib = v3Game(fr.posHead.afP);
		bNeedAlign = false;
		Log("Aligned view (yaw %.1f deg)", (fYawOf(r3_world) - fYawOf(r3_head)) / fDEG);
	}

	//******************************************************************************************
	// Which holster (0 hip, 1 back) the given controller is at, or -1.
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
			if (i_hand == iLEFT && gpPlayer)
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
	// SteamVR controllers -> game control message.  Called once per game step.
	// (The left hand is handled by the player directly from the shared state.)
	void InputHook(SInput& tin)
	{
		if (!bActive)
			return;

		// Sticks and face buttons: main/off swap with handedness.
		const SVRHandInput& m  = frLast.ctl.ahand[iMain()];
		const SVRHandInput& o  = frLast.ctl.ahand[iOff()];
		const SVRHandInput& mp = ctlPrev.ahand[iMain()];
		const SVRHandInput& op = ctlPrev.ahand[iOff()];

		// The right hand (Anne's real arm).
		const SVRHandInput& r  = frLast.ctl.ahand[iRIGHT];
		const SVRHandInput& rp = ctlPrev.ahand[iRIGHT];

		// Mouse look is replaced by the headset.
		tin.v2Rotate = CVector2<>(0, 0);

		//
		// Locomotion: off-hand stick.
		//
		float f_x = o.fStickX, f_y = o.fStickY;
		float f_len = std::sqrt(f_x * f_x + f_y * f_y);
		if (f_len > setVR.fMoveDeadzone)
		{
			// Rescale past the deadzone so movement starts smoothly.
			float f_scale = (std::min)(1.0f, (f_len - setVR.fMoveDeadzone) / (1.0f - setVR.fMoveDeadzone)) / f_len;
			f_x *= f_scale;
			f_y *= f_scale;

			if (setVR.bMoveWithHand && abHandValid[iOff()] && gVR.bHeadValid)
			{
				// The game walks relative to the head; rotate the stick into head space.
				CRotate3<> r3_head(gVR.afHeadRotWorld[0], gVR.afHeadRotWorld[1], gVR.afHeadRotWorld[2], gVR.afHeadRotWorld[3], true);
				float f_delta = fYawOf(ar3HandWorld[iOff()]) - fYawOf(r3_head);
				float c = std::cos(f_delta), s = std::sin(f_delta);
				float f_rx = f_x * c - f_y * s;
				float f_ry = f_x * s + f_y * c;
				f_x = f_rx;
				f_y = f_ry;
			}

			float f_speed = bRunToggle ? 1.0f : setVR.fWalkSpeed;
			tin.v2Move.tX = f_x * 0.6f;
			tin.v2Move.tY = f_y > 0 ? f_y * f_speed : f_y * 0.5f;
		}

		// Run toggle: turn-stick click.
		if (m.bStickClick && !mp.bStickClick)
			bRunToggle = !bRunToggle;

		//
		// Turning: main stick X.
		//
		if (!setVR.bSmoothTurn)
		{
			if (bSnapArmed && std::fabs(m.fStickX) > 0.65f)
			{
				r3Yaw = r3Yaw * r3YawRad((m.fStickX > 0 ? -1.0f : 1.0f) * setVR.fSnapTurnDeg * fDEG);
				bSnapArmed = false;
			}
			else if (std::fabs(m.fStickX) < 0.3f)
				bSnapArmed = true;
		}
		else if (std::fabs(m.fStickX) > setVR.fTurnDeadzone)
		{
			float f_t = (std::fabs(m.fStickX) - setVR.fTurnDeadzone) / (1.0f - setVR.fTurnDeadzone);
			f_t = f_t * f_t;	// Fine control near the centre.
			float f_sign = m.fStickX > 0 ? -1.0f : 1.0f;
			r3Yaw = r3Yaw * r3YawRad(f_sign * f_t * setVR.fSmoothTurnDeg * fDEG * tin.fElapsedTime);
		}

		uint32 u4 = 0, u4_hit = 0;

		//
		// Right hand.
		//
		if (setVR.bMotionHand)
			// The arm always follows the controller.
			u4 |= uCMD_HAND;

		int i_holster = iHolsterNear(iRIGHT);
		bool b_grip_down = r.bGrip && !rp.bGrip;
		bool b_grip_up   = !r.bGrip && rp.bGrip;

		// Steadying the left hand's gun: never grab it.
		if (gVR.iTwoHand == 2)
			bSuppressGrab = true;

		if (!setVR.bGripToggle)
		{
			// Hold mode.
			if (b_grip_down)
			{
				bSuppressGrab = gVR.iTwoHand == 2;
				if (!gVR.bHolding && i_holster >= 0)
				{
					// Draw from holster.
					gVR.iHolsterRequest = i_holster;
					bSuppressGrab = true;
				}
			}
			if (r.bGrip && !bSuppressGrab && !gVR.bHolding)
				u4 |= uCMD_GRAB;		// Seek: grab whatever the hand touches.
			if (b_grip_up)
			{
				if (gVR.bHolding)
				{
					if (i_holster >= 0)
						gVR.iHolsterRequest = i_holster;	// Let go at a holster: holster it.
					else
						iGrabPulse = 1;						// Let go: drop (or throw, if swinging).
				}
				bSuppressGrab = false;
			}
		}
		else
		{
			// Toggle mode.
			if (b_grip_down)
			{
				if (i_holster >= 0)
				{
					gVR.iHolsterRequest = i_holster;
					bSuppressGrab = true;
				}
				else if (gVR.bHolding)
					iGrabPulse = 1;
				else
					bSuppressGrab = gVR.iTwoHand == 2;
			}
			if (r.bGrip && !bSuppressGrab && !gVR.bHolding)
				u4 |= uCMD_GRAB;
			if (b_grip_up)
				bSuppressGrab = false;
		}

		if (iGrabPulse > 0)
		{
			// One step with GRAB pressed (starts the drop), then released (completes it).
			u4 |= uCMD_GRAB;
			u4_hit |= uCMD_GRAB;
			iGrabPulse--;
		}

		// Trigger: use / fire when holding.
		if (gVR.bHolding && r.bTrigger)
			u4 |= uCMD_USE;

		// Finger curl for the empty right hand.
		gVR.fFingerCurl = (std::max)(r.fGrip, (std::min)(1.0f, 0.4f * r.fTrigger + 0.6f * r.fGrip));

		//
		// Face buttons.
		//
		if (m.bLower)
			u4 |= uCMD_JUMP;
		if (m.bUpper && !mp.bUpper)
			bCrouchToggle = !bCrouchToggle;
		if (bCrouchToggle)
			u4 |= uCMD_CROUCH;
		if (o.bLower)
			u4 |= uCMD_STOW;

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
		tin.u4ButtonHit   |= (u4 & ~u4_prev) | u4_hit;
		u4_prev = u4 & ~u4_hit;		// A pulsed button reads as released next step.

		ctlPrev = frLast.ctl;
	}

	//******************************************************************************************
	// Haptics and force-grab gestures; called once per rendered frame.
	void UpdateFeedback(float f_dt)
	{
		if (gVR.iShotCount != iSeenShots)
		{
			iSeenShots = gVR.iShotCount;
			Haptic(iRIGHT, 0.09f, 90.0f, 1.0f);
			if (gVR.iTwoHand == 1)
				Haptic(iLEFT, 0.07f, 90.0f, 0.6f);
		}
		if (gVR.iLeftShotCount != iSeenLeftShots)
		{
			iSeenLeftShots = gVR.iLeftShotCount;
			Haptic(iLEFT, 0.09f, 90.0f, 1.0f);
			if (gVR.iTwoHand == 2)
				Haptic(iRIGHT, 0.07f, 90.0f, 0.6f);
		}
		if (gVR.iPickupCount != iSeenPickups)
		{
			iSeenPickups = gVR.iPickupCount;
			Haptic(iRIGHT, 0.05f, 60.0f, 0.5f);
		}
		if (gVR.iLeftPickupCount != iSeenLeftPickups)
		{
			iSeenLeftPickups = gVR.iLeftPickupCount;
			Haptic(iLEFT, 0.05f, 60.0f, 0.5f);
		}

		for (int i = 0; i < 2; i++)
		{
			// Controller i is right (index 1) or left (0); force-grab slot is 0 right, 1 left.
			int i_ctrl  = i == 0 ? iRIGHT : iLEFT;
			float f_mass = i == 0 ? gVR.fHeldMass : gVR.fLeftHeldMass;

			if (gVR.aiForceTargetSerial[i] != aiSeenForceSerial[i])
			{
				aiSeenForceSerial[i] = gVR.aiForceTargetSerial[i];
				Haptic(i_ctrl, 0.02f, 220.0f, 0.3f);
			}

			// Holster hover tick.
			int i_holster = iHolsterNear(i_ctrl);
			if (i_holster != aiLastHolsterNear[i])
			{
				if (i_holster >= 0)
					Haptic(i_ctrl, 0.03f, 160.0f, 0.35f);
				aiLastHolsterNear[i] = i_holster;
			}

			// Weight: heavier held objects rumble more when you swing them around.
			if (f_mass > 0 && f_dt > 0)
			{
				CVector3<> v3_acc = (av3HandVel[i_ctrl] - av3PrevVel[i]) * (1.0f / f_dt);
				float f_amp = fLen(v3_acc) * f_mass / 600.0f;
				if (f_amp > 0.04f)
					Haptic(i_ctrl, f_dt * 1.5f, 40.0f, (std::min)(f_amp, 0.7f));
			}
			av3PrevVel[i] = av3HandVel[i_ctrl];

			// Force grab: flick toward yourself (or upward) while aiming at a target.
			if (gVR.abForceAim[i] && gVR.abForceTarget[i] && abHandValid[i_ctrl])
			{
				CVector3<> v3_to_head = v3HeadWorld - av3HandWorld[i_ctrl];
				float f_dist = fLen(v3_to_head);
				CVector3<> v3_vel = av3HandVel[i_ctrl] * r3Yaw;
				float f_toward = f_dist > 0.01f ? fDot(v3_vel, v3_to_head) / f_dist : 0.0f;
				if (f_toward > 1.3f || v3_vel.tZ > 1.6f)
				{
					gVR.abForcePull[i] = true;
					Haptic(i_ctrl, 0.12f, 70.0f, 0.6f);
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

			RECT rc;
			GetClientRect(g_hwnd, &rc);
			float f_y = setVR.bMenuFlipY ? (prasMainScreen->iHeight - ev.fY) : ev.fY;
			int i_x = (int)(ev.fX * (rc.right - rc.left) / (std::max)(1, prasMainScreen->iWidth));
			int i_y = (int)(f_y  * (rc.bottom - rc.top) / (std::max)(1, prasMainScreen->iHeight));

			POINT pt = { i_x, i_y };
			ClientToScreen(g_hwnd, &pt);
			SetCursorPos(pt.x, pt.y);

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

	bActive          = true;
	bNeedAlign       = true;
	gVR.bActive      = true;
	gVR.bMotionHand  = setVR.bMotionHand;
	gpfnVRInputHook  = InputHook;
	gpfnVRFlipHook   = FlipHook;
}

//**********************************************************************************************
void VRMod_Shutdown()
{
	gpfnVRInputHook = nullptr;
	gpfnVRFlipHook  = nullptr;
	gVR.bActive     = false;
	gVR.bHeadValid  = false;
	gVR.bHandValid  = false;
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
		return false;
	}

	DWORD u4_now = GetTickCount();
	float f_dt = u4PrevFrameTime ? (u4_now - u4PrevFrameTime) * 0.001f : 0.0f;
	if (f_dt > 0.1f)
		f_dt = 0.1f;
	u4PrevFrameTime = u4_now;

	CCamera* pcam = CWDbQueryActiveCamera().tGet();
	if (!pcam)
		return false;

	pcam->UpdateFromAttached();

	CInstance*    pins_camera_on      = pcam->pinsAttached();
	CPlacement3<> p3_camera_relative  = pcam->p3Relative();
	bool          b_camera_on_head    = pcam->bHead();
	CVector3<>    v3_cam_pos          = pcam->pr3Presence().v3Pos;
	CRotate3<>    r3_cam_rot          = pcam->pr3Presence().r3Rot;
	bool          b_player_view       = pins_camera_on && pins_camera_on == (CInstance*)gpPlayer && b_camera_on_head;

	// Re-align when the camera changes owner (cutscene/death cams), or on first frame.
	if (pins_camera_on != pvLastCamAttach || b_camera_on_head != bLastCamHead)
	{
		bNeedAlign = true;
		pvLastCamAttach = pins_camera_on;
		bLastCamHead    = b_camera_on_head;
	}

	// Recenter: off-hand stick click.
	const SVRHandInput& hi_off = fr.ctl.ahand[iOff()];
	static bool b_prev_recenter = false;
	if (hi_off.bStickClick && !b_prev_recenter)
		Align(b_player_view && gpPlayer ? gpPlayer->r3Rot() : r3_cam_rot, fr, true);
	else if (bNeedAlign)
		Align(r3_cam_rot, fr, true);
	b_prev_recenter = hi_off.bStickClick;

	CVector3<> v3_head_track = v3Game(fr.posHead.afP);
	CVector3<> v3_lean       = v3Lean(v3_head_track);

	// Tracking point -> world.
	auto v3World = [&](const CVector3<>& v3_track) -> CVector3<>
	{
		return v3_cam_pos + (v3_lean + (v3_track - v3_head_track)) * r3Yaw;
	};

	v3HeadWorld = v3World(v3_head_track);

	// Publish head for the player's next step.
	StoreQ(gVR.afHeadRotWorld, r3Game(fr.posHead.afQ) * r3Yaw);
	gVR.bHeadValid = true;

	// Hands in world space.
	for (int i = 0; i < 2; i++)
	{
		abHandValid[i] = fr.aposHand[i].bValid;
		if (!abHandValid[i])
			continue;
		CVector3<> v3_track = v3Game(fr.aposHand[i].afP);
		if (f_dt > 0)
			av3HandVel[i] = (v3_track - av3HandPrevTrack[i]) * (1.0f / f_dt);
		av3HandPrevTrack[i] = v3_track;
		av3HandWorld[i] = v3World(v3_track);
		ar3HandWorld[i] = r3Game(fr.aposHand[i].afQ) * r3Yaw;
	}

	//
	// Two-handed guns: one hand holds the gun, the other grips near it.
	//
	{
		const SVRHandInput& hr = fr.ctl.ahand[iRIGHT];
		const SVRHandInput& hl = fr.ctl.ahand[iLEFT];
		bool b_r_down = hr.bGrip && !abPrevGripRender[iRIGHT];
		bool b_l_down = hl.bGrip && !abPrevGripRender[iLEFT];
		abPrevGripRender[iRIGHT] = hr.bGrip;
		abPrevGripRender[iLEFT]  = hl.bGrip;

		float f_gap = (abHandValid[0] && abHandValid[1]) ? fLen(av3HandWorld[iLEFT] - av3HandWorld[iRIGHT]) : 99.0f;

		if (gVR.iTwoHand == 0 && setVR.bTwoHandAim)
		{
			if (b_l_down && gVR.bHoldingGun && !gVR.bLeftHolding)
			{
				bool b_pistol = gVR.fHeldMass <= setVR.fPistolMaxMass;
				if (f_gap < (b_pistol ? 0.25f : 0.7f))
				{
					gVR.iTwoHand = 1;
					gVR.bTwoHandPistol = b_pistol;
					Haptic(iLEFT, 0.04f, 120.0f, 0.4f);
				}
			}
			else if (b_r_down && gVR.bLeftHoldingGun && !gVR.bHolding)
			{
				bool b_pistol = gVR.fLeftHeldMass <= setVR.fPistolMaxMass;
				if (f_gap < (b_pistol ? 0.25f : 0.7f))
				{
					gVR.iTwoHand = 2;
					gVR.bTwoHandPistol = b_pistol;
					Haptic(iRIGHT, 0.04f, 120.0f, 0.4f);
				}
			}
		}
		else if (gVR.iTwoHand == 1 && (!hl.bGrip || !gVR.bHoldingGun))
			gVR.iTwoHand = 0;
		else if (gVR.iTwoHand == 2 && (!hr.bGrip || !gVR.bLeftHoldingGun))
			gVR.iTwoHand = 0;
	}

	CRotate3<> r3_offset = r3AxisAngle(1, 0, 0, setVR.fHandPitchDeg * fDEG) *
	                       r3AxisAngle(0, 1, 0, setVR.fHandRollDeg  * fDEG) *
	                       r3AxisAngle(0, 0, 1, setVR.fHandYawDeg   * fDEG);
	// The same offset, mirrored for the left hand.
	CRotate3<> r3_offset_l(r3_offset.tC, r3_offset.v3S.tX, -r3_offset.v3S.tY, -r3_offset.v3S.tZ, true);

	CRotate3<> r3_right = ar3HandWorld[iRIGHT];
	CRotate3<> r3_left  = ar3HandWorld[iLEFT];

	// Long guns aim along the line between the hands.
	if (gVR.iTwoHand == 1 && !gVR.bTwoHandPistol)
		r3_right = r3LookAt(av3HandWorld[iLEFT] - av3HandWorld[iRIGHT], CVector3<>(0, 0, 1) * ar3HandWorld[iRIGHT]);
	else if (gVR.iTwoHand == 2 && !gVR.bTwoHandPistol)
		r3_left = r3LookAt(av3HandWorld[iRIGHT] - av3HandWorld[iLEFT], CVector3<>(0, 0, 1) * ar3HandWorld[iLEFT]);

	// Right controller -> Anne's arm.
	gVR.bHandValid = abHandValid[iRIGHT];
	if (abHandValid[iRIGHT])
	{
		const SVRHandInput& hr = fr.ctl.ahand[iRIGHT];
		StoreQ(gVR.afHandRotWorld, r3_offset * r3_right);
		StoreV(gVR.afHandPosWorld, av3HandWorld[iRIGHT]);

		// Force grab: trigger held with an empty hand and no grip.
		gVR.abForceAim[0] = setVR.bForceGrab && !gVR.bHolding && hr.bTrigger && !hr.bGrip;
		StoreV(gVR.aafAimPosWorld[0], av3HandWorld[iRIGHT]);
		StoreV(gVR.aafAimDirWorld[0], CVector3<>(0, 1, 0) * ar3HandWorld[iRIGHT]);
	}
	else
		gVR.abForceAim[0] = false;

	// Left controller -> second arm.
	gVR.bLeftValid = abHandValid[iLEFT] && setVR.bMotionHand;
	if (abHandValid[iLEFT])
	{
		const SVRHandInput& hl = fr.ctl.ahand[iLEFT];
		bool b_grip_down = hl.bGrip && !gVR.bLeftGrip;
		bool b_grip_up   = !hl.bGrip && gVR.bLeftGrip;

		StoreV(gVR.afLeftPosWorld, av3HandWorld[iLEFT]);
		StoreQ(gVR.afLeftRotWorld, r3_offset_l * r3_left);
		StoreV(gVR.afLeftVelWorld, av3HandVel[iLEFT] * r3Yaw);
		gVR.fLeftCurl = (std::max)(hl.fGrip, (std::min)(1.0f, 0.4f * hl.fTrigger + 0.6f * hl.fGrip));
		gVR.bLeftTrigger = hl.bTrigger && gVR.bLeftHolding;

		// Holsters for the left hand (the player decides whether there's anything to do).
		int i_holster = iHolsterNear(iLEFT);
		if (i_holster >= 0 && ((b_grip_down && !gVR.bLeftHolding) || (b_grip_up && gVR.bLeftHolding)))
			gVR.iLeftHolsterRequest = i_holster;
		gVR.bLeftGrip = hl.bGrip;

		gVR.abForceAim[1] = setVR.bForceGrab && !gVR.bLeftHolding && hl.bTrigger && !hl.bGrip;
		StoreV(gVR.aafAimPosWorld[1], av3HandWorld[iLEFT]);
		StoreV(gVR.aafAimDirWorld[1], CVector3<>(0, 1, 0) * ar3HandWorld[iLEFT]);
	}
	else
	{
		gVR.bLeftGrip = false;
		gVR.bLeftTrigger = false;
		gVR.abForceAim[1] = false;
	}

	UpdateFeedback(f_dt);

	//
	// Render each eye.
	//
	CCamera::SProperties camprop_orig = pcam->campropGetProperties();
	pcam->SetAttached(0);

	// Second arm: posed and put in the world for this frame only.
	if (gpPlayer && b_player_view)
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
