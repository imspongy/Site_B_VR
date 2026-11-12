/**********************************************************************************************
 *
 * VRMod.cpp
 *
 * Trespasser VR: game-side integration.
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
 *   walking direction and arm all follow where you are actually looking.  The right controller
 *   drives the hand target directly.
 * - Menus and loading screens are shown on a floating virtual screen (SteamVR overlay).
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
	//
	struct SSettings
	{
		bool  bEnabled        = true;
		bool  bMotionHand     = true;
		float fSnapTurnDeg    = 45.0f;	// 0 = smooth turn.
		float fSmoothTurnDeg  = 150.0f;	// Degrees per second for smooth turn.
		float fHandPitchDeg   = 0.0f;	// Controller -> in-game hand orientation offset.
		float fHandYawDeg     = 0.0f;
		float fHandRollDeg    = 0.0f;
		float fWalkSpeed      = 0.6f;	// Stick speed when not running (1.0 = game's run).
		float fLeanRadius     = 0.6f;	// Max physical head offset from the recentre point.
		float fOverlayWidth   = 2.4f;	// Menu screen width in metres.
		float fOverlayDist    = 2.0f;	// Menu screen distance in metres.
		bool  bFlipFovX       = false;	// Debug: invert the off-axis direction.
		bool  bFlipFovY       = false;
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

	// Input edge state.
	uint32      u4PrevButtons = 0;
	bool        bPrevRecenter = false;
	bool        bPrevMenu     = false;
	bool        bPrevRunClick = false;
	bool        bRunToggle    = false;
	bool        bSnapArmed    = true;

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
	float fIni(const std::string& str_file, const char* str_key, float f_default)
	{
		char str_def[32], str_val[64];
		std::snprintf(str_def, sizeof(str_def), "%g", f_default);
		GetPrivateProfileStringA("VR", str_key, str_def, str_val, sizeof(str_val), str_file.c_str());
		return (float)std::atof(str_val);
	}

	//******************************************************************************************
	void LoadSettings()
	{
		std::string str_file = strExeDir() + "\\TrespasserVR.ini";

		if (GetFileAttributesA(str_file.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			FILE* pf = std::fopen(str_file.c_str(), "w");
			if (pf)
			{
				std::fputs(
					"; Trespasser VR settings. Delete this file to restore defaults.\n"
					"[VR]\n"
					"; 0 = play on the monitor as normal.\n"
					"Enabled=1\n"
					"; Right controller moves Anne's arm directly.\n"
					"MotionHand=1\n"
					"; Degrees per snap turn on the right stick. 0 = smooth turning.\n"
					"SnapTurnDegrees=45\n"
					"SmoothTurnSpeed=150\n"
					"; Walking speed when run is toggled off (right stick click toggles run).\n"
					"WalkSpeed=0.6\n"
					"; Rotate the in-game hand relative to your controller (degrees).\n"
					"; If guns aim too high, try HandPitch=-20.\n"
					"HandPitch=0\n"
					"HandYaw=0\n"
					"HandRoll=0\n"
					"; How far (metres) you can physically lean before the view stops following.\n"
					"LeanRadius=0.6\n"
					"; Size and distance (metres) of the floating menu screen.\n"
					"MenuScreenWidth=2.4\n"
					"MenuScreenDistance=2.0\n"
					"; Debug: set to 1 if the image looks skewed or doubled.\n"
					"FlipFovX=0\n"
					"FlipFovY=0\n", pf);
				std::fclose(pf);
			}
		}

		setVR.bEnabled       = fIni(str_file, "Enabled", 1) != 0;
		setVR.bMotionHand    = fIni(str_file, "MotionHand", 1) != 0;
		setVR.fSnapTurnDeg   = fIni(str_file, "SnapTurnDegrees", 45);
		setVR.fSmoothTurnDeg = fIni(str_file, "SmoothTurnSpeed", 150);
		setVR.fWalkSpeed     = fIni(str_file, "WalkSpeed", 0.6f);
		setVR.fHandPitchDeg  = fIni(str_file, "HandPitch", 0);
		setVR.fHandYawDeg    = fIni(str_file, "HandYaw", 0);
		setVR.fHandRollDeg   = fIni(str_file, "HandRoll", 0);
		setVR.fLeanRadius    = fIni(str_file, "LeanRadius", 0.6f);
		setVR.fOverlayWidth  = fIni(str_file, "MenuScreenWidth", 2.4f);
		setVR.fOverlayDist   = fIni(str_file, "MenuScreenDistance", 2.0f);
		setVR.bFlipFovX      = fIni(str_file, "FlipFovX", 0) != 0;
		setVR.bFlipFovY      = fIni(str_file, "FlipFovY", 0) != 0;
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

		bool b_was_locked = prasMainScreen->bIsLocked();
		prasMainScreen->Lock();
		if (!prasMainScreen->pSurface)
		{
			if (!b_was_locked)
				prasMainScreen->Unlock();
			return false;
		}

		f(prasMainScreen->pSurface,
		  prasMainScreen->iLinePixels * prasMainScreen->iPixelBytes(),
		  prasMainScreen->iWidth, prasMainScreen->iHeight, prasMainScreen->iPixelBits,
		  (uint32_t)sd.ddpfPixelFormat.dwRBitMask,
		  (uint32_t)sd.ddpfPixelFormat.dwGBitMask,
		  (uint32_t)sd.ddpfPixelFormat.dwBBitMask);

		if (!b_was_locked)
			prasMainScreen->Unlock();
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
		Log("Aligned view (yaw %.1f deg)", (fYawOf(r3_world) - fYawOf(r3_head)) * 57.2958f);
	}

	//******************************************************************************************
	// SteamVR controller -> game control message.
	void InputHook(SInput& tin)
	{
		if (!bActive)
			return;

		const SVRControls& c = frLast.ctl;

		// Mouse look is replaced by the headset.
		tin.v2Rotate = CVector2<>(0, 0);

		// Left stick: walk relative to where you are looking (the game walks along head facing).
		float f_x = c.fMoveX, f_y = c.fMoveY;
		float f_len = std::sqrt(f_x * f_x + f_y * f_y);
		if (f_len > 0.15f)
		{
			float f_speed = bRunToggle ? 1.0f : setVR.fWalkSpeed;
			tin.v2Move.tY = f_y > 0 ? f_y * f_speed : f_y * 0.4f;
			tin.v2Move.tX = f_x * 0.6f;
		}

		// Run toggle on right stick click.
		if (c.bRun && !bPrevRunClick)
			bRunToggle = !bRunToggle;
		bPrevRunClick = c.bRun;

		// Turning.
		if (setVR.fSnapTurnDeg > 0)
		{
			if (bSnapArmed && std::fabs(c.fTurnX) > 0.65f)
			{
				r3Yaw = r3Yaw * r3YawRad((c.fTurnX > 0 ? -1.0f : 1.0f) * setVR.fSnapTurnDeg * fDEG);
				bSnapArmed = false;
			}
			else if (std::fabs(c.fTurnX) < 0.3f)
				bSnapArmed = true;
		}
		else if (std::fabs(c.fTurnX) > 0.15f)
		{
			r3Yaw = r3Yaw * r3YawRad(-c.fTurnX * setVR.fSmoothTurnDeg * fDEG * tin.fElapsedTime);
		}

		// Buttons.
		uint32 u4 = 0;
		if (c.bHand)
			u4 |= uCMD_HAND;
		if (c.bTrigger)
			u4 |= gVR.bHolding ? uCMD_USE : (uCMD_HAND | uCMD_GRAB);
		if (c.bDrop)
			u4 |= uCMD_GRAB;
		if (c.bJump)
			u4 |= uCMD_JUMP;
		if (c.bCrouch)
			u4 |= uCMD_CROUCH;
		if (c.bThrow)
			u4 |= uCMD_THROW;
		if (c.bStow)
			u4 |= uCMD_STOW;
		if (c.bPunch)
			u4 |= uCMD_PUNCH;

		tin.u4ButtonState |= u4;
		tin.u4ButtonHit   |= u4 & ~u4PrevButtons;
		u4PrevButtons = u4;

		// Pause menu.
		if (c.bMenu && !bPrevMenu && g_hwnd)
		{
			PostMessage(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
			PostMessage(g_hwnd, WM_KEYUP,   VK_ESCAPE, 0xC0010001);
		}
		bPrevMenu = c.bMenu;
	}

	//******************************************************************************************
	// Called at the start of every CRasterWin::Flip(): shows menus on a virtual screen.
	void FlipHook()
	{
		if (!bActive)
			return;

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
			bool b_place = !bOverlayShown;
			bReadBackBuffer([&](const void* pv, int i_pitch, int i_w, int i_h, int i_bits, uint32_t r, uint32_t g, uint32_t b)
			{
				VRB_ShowOverlay(pv, i_pitch, i_w, i_h, i_bits, r, g, b,
				                setVR.fOverlayWidth, setVR.fOverlayDist, b_place);
			});
			bOverlayShown = true;
		}
		bStereoThisFrame = false;
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

	SVRFrame fr;
	if (!VRB_BeginFrame(fr))
	{
		if (VRB_QuitRequested())
		{
			Log("SteamVR asked us to quit VR; continuing on the monitor.");
			VRMod_Shutdown();
		}
		return false;
	}
	frLast = fr;

	if (!fr.posHead.bValid)
	{
		gVR.bHeadValid = false;
		return false;
	}

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

	if (fr.ctl.bRecenter && !bPrevRecenter)
		Align(b_player_view && gpPlayer ? gpPlayer->r3Rot() : r3_cam_rot, fr, true);
	else if (bNeedAlign)
		Align(r3_cam_rot, fr, true);
	bPrevRecenter = fr.ctl.bRecenter;

	CVector3<> v3_head_track = v3Game(fr.posHead.afP);
	CVector3<> v3_lean       = v3Lean(v3_head_track);

	// Tracking point -> world.
	auto v3World = [&](const CVector3<>& v3_track) -> CVector3<>
	{
		return v3_cam_pos + (v3_lean + (v3_track - v3_head_track)) * r3Yaw;
	};

	// Publish head and hand for the player's next step.
	StoreQ(gVR.afHeadRotWorld, r3Game(fr.posHead.afQ) * r3Yaw);
	gVR.bHeadValid = true;

	gVR.bHandValid = fr.posHandRight.bValid;
	if (fr.posHandRight.bValid)
	{
		const float f_deg = fDEG;
		CRotate3<> r3_offset = r3AxisAngle(1, 0, 0, setVR.fHandPitchDeg * f_deg) *
		                       r3AxisAngle(0, 1, 0, setVR.fHandRollDeg  * f_deg) *
		                       r3AxisAngle(0, 0, 1, setVR.fHandYawDeg   * f_deg);
		CRotate3<> r3_hand = r3_offset * r3Game(fr.posHandRight.afQ) * r3Yaw;
		CVector3<> v3_hand = v3World(v3Game(fr.posHandRight.afP));
		StoreQ(gVR.afHandRotWorld, r3_hand);
		gVR.afHandPosWorld[0] = v3_hand.tX;
		gVR.afHandPosWorld[1] = v3_hand.tY;
		gVR.afHandPosWorld[2] = v3_hand.tZ;
	}

	//
	// Render each eye.
	//
	CCamera::SProperties camprop_orig = pcam->campropGetProperties();
	pcam->SetAttached(0);

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

		paint_scene();

		bReadBackBuffer([&](const void* pv, int i_pitch, int i_w, int i_h, int i_bits, uint32_t r, uint32_t g, uint32_t b)
		{
			if (!b_logged_format)
			{
				Log("Back buffer %dx%d, %d bpp, masks R%08X G%08X B%08X.", i_w, i_h, i_bits, r, g, b);
				b_logged_format = true;
			}
			VRB_SubmitEye(i_eye, pv, i_pitch, i_w, i_h, i_bits, r, g, b);
		});
	}

	// Restore the camera.
	pcam->SetProperties(camprop_orig);
	pcam->Move(CPlacement3<>(r3_cam_rot, v3_cam_pos));
	pcam->SetAttached(pins_camera_on, b_camera_on_head);
	pcam->SetRelative(p3_camera_relative);

	bStereoThisFrame = true;
	return true;
}
