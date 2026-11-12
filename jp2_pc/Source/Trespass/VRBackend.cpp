/**********************************************************************************************
 *
 * VRBackend.cpp
 *
 * OpenVR + Direct3D 11 backend for the VR mod.  See VRBackend.h.
 *
 * This file must NOT include any game headers (and is excluded from the precompiled header),
 * because the game's bundled DirectX 7 headers and modern d3d11.h don't mix well.
 *
 **********************************************************************************************/

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "openvr.h"
#include "VRBackend.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
	vr::IVRSystem*        pvrSystem     = nullptr;
	ID3D11Device*         pd3dDevice    = nullptr;
	ID3D11DeviceContext*  pd3dContext   = nullptr;
	ID3D11Texture2D*      apd3dEyeTex[2] = { nullptr, nullptr };
	int                   aiTexW[2] = { 0, 0 }, aiTexH[2] = { 0, 0 };
	std::vector<uint32_t> au4Convert;		// RGBA8 staging buffer.
	bool                  bQuit = false;

	// 16-bit -> RGBA8 lookup table, rebuilt if the masks change.
	std::vector<uint32_t> au4Lut16;
	uint32_t              au4LutMasks[3] = { 0, 0, 0 };

	vr::TrackedDevicePose_t atdpPoses[vr::k_unMaxTrackedDeviceCount];

	// Input action handles.
	vr::VRActionSetHandle_t hSetMain = vr::k_ulInvalidActionSetHandle;
	vr::VRActionHandle_t hMove = 0, hTurn = 0, hHand = 0, hTrigger = 0, hDrop = 0, hJump = 0,
	                     hCrouch = 0, hThrow = 0, hStow = 0, hPunch = 0, hRecenter = 0, hRun = 0,
	                     hMenu = 0, hPoseRight = 0, hPoseLeft = 0, hHapticRight = 0, hHapticLeft = 0;
	bool bInputOK = false;

	//******************************************************************************************
	template<class T> void SafeRelease(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}

	//******************************************************************************************
	// Converts a 3x4 OpenVR matrix to quaternion + position.
	void PoseFromMatrix(const vr::HmdMatrix34_t& m, SVRPose& pose)
	{
		const float (*a)[4] = m.m;
		float tr = a[0][0] + a[1][1] + a[2][2];
		float w, x, y, z;
		if (tr > 0)
		{
			float s = std::sqrt(tr + 1.0f) * 2;
			w = 0.25f * s;
			x = (a[2][1] - a[1][2]) / s;
			y = (a[0][2] - a[2][0]) / s;
			z = (a[1][0] - a[0][1]) / s;
		}
		else if (a[0][0] > a[1][1] && a[0][0] > a[2][2])
		{
			float s = std::sqrt(1.0f + a[0][0] - a[1][1] - a[2][2]) * 2;
			w = (a[2][1] - a[1][2]) / s;
			x = 0.25f * s;
			y = (a[0][1] + a[1][0]) / s;
			z = (a[0][2] + a[2][0]) / s;
		}
		else if (a[1][1] > a[2][2])
		{
			float s = std::sqrt(1.0f + a[1][1] - a[0][0] - a[2][2]) * 2;
			w = (a[0][2] - a[2][0]) / s;
			x = (a[0][1] + a[1][0]) / s;
			y = 0.25f * s;
			z = (a[1][2] + a[2][1]) / s;
		}
		else
		{
			float s = std::sqrt(1.0f + a[2][2] - a[0][0] - a[1][1]) * 2;
			w = (a[1][0] - a[0][1]) / s;
			x = (a[0][2] + a[2][0]) / s;
			y = (a[1][2] + a[2][1]) / s;
			z = 0.25f * s;
		}
		float len = std::sqrt(w * w + x * x + y * y + z * z);
		if (len < 1e-6f)
		{
			w = 1; x = y = z = 0; len = 1;
		}
		pose.afQ[0] = w / len;
		pose.afQ[1] = x / len;
		pose.afQ[2] = y / len;
		pose.afQ[3] = z / len;
		pose.afP[0] = a[0][3];
		pose.afP[1] = a[1][3];
		pose.afP[2] = a[2][3];
		pose.bValid = true;
	}

	//******************************************************************************************
	// a * b for affine 3x4 matrices (column-vector convention: p' = a * (b * p)).
	vr::HmdMatrix34_t mxMul(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b)
	{
		vr::HmdMatrix34_t r;
		for (int i = 0; i < 3; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
				if (j == 3)
					r.m[i][j] += a.m[i][3];
			}
		}
		return r;
	}

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
	void WriteTextFile(const std::string& str_path, const char* str_text)
	{
		FILE* pf = std::fopen(str_path.c_str(), "wb");
		if (pf)
		{
			std::fwrite(str_text, 1, std::strlen(str_text), pf);
			std::fclose(pf);
		}
	}

	//******************************************************************************************
	//
	// SteamVR Input manifest and default bindings.  Written next to the exe at startup, so the
	// mod needs no extra files.  Players can rebind everything in SteamVR's binding UI.
	//
	const char* strACTIONS = R"JSON({
  "default_bindings": [
    { "controller_type": "knuckles",        "binding_url": "bindings_knuckles.json" },
    { "controller_type": "oculus_touch",    "binding_url": "bindings_oculus_touch.json" },
    { "controller_type": "vive_controller", "binding_url": "bindings_vive_controller.json" }
  ],
  "action_sets": [ { "name": "/actions/main", "usage": "leftright" } ],
  "actions": [
    { "name": "/actions/main/in/Move",      "type": "vector2" },
    { "name": "/actions/main/in/Turn",      "type": "vector2" },
    { "name": "/actions/main/in/Hand",      "type": "boolean" },
    { "name": "/actions/main/in/Trigger",   "type": "boolean" },
    { "name": "/actions/main/in/Drop",      "type": "boolean" },
    { "name": "/actions/main/in/Jump",      "type": "boolean" },
    { "name": "/actions/main/in/Crouch",    "type": "boolean" },
    { "name": "/actions/main/in/Throw",     "type": "boolean" },
    { "name": "/actions/main/in/Stow",      "type": "boolean" },
    { "name": "/actions/main/in/Punch",     "type": "boolean" },
    { "name": "/actions/main/in/Recenter",  "type": "boolean" },
    { "name": "/actions/main/in/Run",       "type": "boolean" },
    { "name": "/actions/main/in/Menu",      "type": "boolean" },
    { "name": "/actions/main/in/PoseRight", "type": "pose" },
    { "name": "/actions/main/in/PoseLeft",  "type": "pose" },
    { "name": "/actions/main/out/HapticRight", "type": "vibration" },
    { "name": "/actions/main/out/HapticLeft",  "type": "vibration" }
  ],
  "localization": [ {
    "language_tag": "en_US",
    "/actions/main": "Trespasser",
    "/actions/main/in/Move": "Move",
    "/actions/main/in/Turn": "Turn",
    "/actions/main/in/Hand": "Raise hand (hold)",
    "/actions/main/in/Trigger": "Grab / use / fire",
    "/actions/main/in/Drop": "Grab / drop",
    "/actions/main/in/Jump": "Jump",
    "/actions/main/in/Crouch": "Crouch",
    "/actions/main/in/Throw": "Throw (hold to wind up)",
    "/actions/main/in/Stow": "Stow / swap item",
    "/actions/main/in/Punch": "Punch / swing",
    "/actions/main/in/Recenter": "Recenter view",
    "/actions/main/in/Run": "Toggle run",
    "/actions/main/in/Menu": "Pause menu",
    "/actions/main/in/PoseRight": "Right hand",
    "/actions/main/in/PoseLeft": "Left hand",
    "/actions/main/out/HapticRight": "Right haptics",
    "/actions/main/out/HapticLeft": "Left haptics"
  } ]
})JSON";

#define POSES_AND_HAPTICS \
R"JSON(      "poses": [
        { "output": "/actions/main/in/PoseRight", "path": "/user/hand/right/pose/handgrip" },
        { "output": "/actions/main/in/PoseLeft",  "path": "/user/hand/left/pose/handgrip" }
      ],
      "haptics": [
        { "output": "/actions/main/out/HapticRight", "path": "/user/hand/right/output/haptic" },
        { "output": "/actions/main/out/HapticLeft",  "path": "/user/hand/left/output/haptic" }
      ],)JSON"

#define BTN(hand, input, action) \
	"        { \"path\": \"/user/hand/" hand "/input/" input "\", \"mode\": \"button\", \"inputs\": { \"click\": { \"output\": \"/actions/main/in/" action "\" } } },\n"
#define TRIG(hand, action) \
	"        { \"path\": \"/user/hand/" hand "/input/trigger\", \"mode\": \"trigger\", \"inputs\": { \"click\": { \"output\": \"/actions/main/in/" action "\" } } },\n"
#define STICK(hand, input, action, clickaction) \
	"        { \"path\": \"/user/hand/" hand "/input/" input "\", \"mode\": \"joystick\", \"inputs\": { \"position\": { \"output\": \"/actions/main/in/" action "\" }, \"click\": { \"output\": \"/actions/main/in/" clickaction "\" } } },\n"
#define PAD(hand, action, clickaction) \
	"        { \"path\": \"/user/hand/" hand "/input/trackpad\", \"mode\": \"trackpad\", \"inputs\": { \"position\": { \"output\": \"/actions/main/in/" action "\" }, \"click\": { \"output\": \"/actions/main/in/" clickaction "\" } } },\n"
#define GRAB(hand, action) \
	"        { \"path\": \"/user/hand/" hand "/input/grip\", \"mode\": \"grab\", \"inputs\": { \"grab\": { \"output\": \"/actions/main/in/" action "\" } } },\n"
#define LAST "        { \"path\": \"/user/hand/left/input/trigger\", \"mode\": \"trigger\", \"inputs\": { \"click\": { \"output\": \"/actions/main/in/Throw\" } } }\n"

	const char* strBIND_KNUCKLES =
		"{\n  \"controller_type\": \"knuckles\",\n  \"name\": \"Trespasser VR (Index)\",\n  \"bindings\": {\n    \"/actions/main\": {\n"
		POSES_AND_HAPTICS
		"      \"sources\": [\n"
		STICK("left", "thumbstick", "Move", "Recenter")
		STICK("right", "thumbstick", "Turn", "Run")
		GRAB("right", "Hand")
		TRIG("right", "Trigger")
		BTN("right", "a", "Drop")
		BTN("right", "b", "Jump")
		GRAB("left", "Crouch")
		BTN("left", "a", "Stow")
		BTN("left", "b", "Menu")
		LAST
		"      ]\n    }\n  }\n}\n";

	const char* strBIND_TOUCH =
		"{\n  \"controller_type\": \"oculus_touch\",\n  \"name\": \"Trespasser VR (Touch)\",\n  \"bindings\": {\n    \"/actions/main\": {\n"
		POSES_AND_HAPTICS
		"      \"sources\": [\n"
		STICK("left", "joystick", "Move", "Recenter")
		STICK("right", "joystick", "Turn", "Run")
		BTN("right", "grip", "Hand")
		TRIG("right", "Trigger")
		BTN("right", "a", "Drop")
		BTN("right", "b", "Jump")
		BTN("left", "grip", "Crouch")
		BTN("left", "x", "Stow")
		BTN("left", "y", "Menu")
		LAST
		"      ]\n    }\n  }\n}\n";

	const char* strBIND_VIVE =
		"{\n  \"controller_type\": \"vive_controller\",\n  \"name\": \"Trespasser VR (Vive)\",\n  \"bindings\": {\n    \"/actions/main\": {\n"
		POSES_AND_HAPTICS
		"      \"sources\": [\n"
		PAD("left", "Move", "Recenter")
		PAD("right", "Turn", "Drop")
		BTN("right", "grip", "Hand")
		TRIG("right", "Trigger")
		BTN("right", "application_menu", "Jump")
		BTN("left", "grip", "Crouch")
		BTN("left", "application_menu", "Menu")
		LAST
		"      ]\n    }\n  }\n}\n";

	//******************************************************************************************
	void InitInput()
	{
		vr::IVRInput* pin = vr::VRInput();
		if (!pin)
			return;

		std::string str_dir = strExeDir() + "\\vr_input";
		CreateDirectoryA(str_dir.c_str(), nullptr);
		WriteTextFile(str_dir + "\\actions.json",                   strACTIONS);
		WriteTextFile(str_dir + "\\bindings_knuckles.json",         strBIND_KNUCKLES);
		WriteTextFile(str_dir + "\\bindings_oculus_touch.json",     strBIND_TOUCH);
		WriteTextFile(str_dir + "\\bindings_vive_controller.json",  strBIND_VIVE);

		if (pin->SetActionManifestPath((str_dir + "\\actions.json").c_str()) != vr::VRInputError_None)
			return;

		pin->GetActionSetHandle("/actions/main", &hSetMain);
		pin->GetActionHandle("/actions/main/in/Move",      &hMove);
		pin->GetActionHandle("/actions/main/in/Turn",      &hTurn);
		pin->GetActionHandle("/actions/main/in/Hand",      &hHand);
		pin->GetActionHandle("/actions/main/in/Trigger",   &hTrigger);
		pin->GetActionHandle("/actions/main/in/Drop",      &hDrop);
		pin->GetActionHandle("/actions/main/in/Jump",      &hJump);
		pin->GetActionHandle("/actions/main/in/Crouch",    &hCrouch);
		pin->GetActionHandle("/actions/main/in/Throw",     &hThrow);
		pin->GetActionHandle("/actions/main/in/Stow",      &hStow);
		pin->GetActionHandle("/actions/main/in/Punch",     &hPunch);
		pin->GetActionHandle("/actions/main/in/Recenter",  &hRecenter);
		pin->GetActionHandle("/actions/main/in/Run",       &hRun);
		pin->GetActionHandle("/actions/main/in/Menu",      &hMenu);
		pin->GetActionHandle("/actions/main/in/PoseRight", &hPoseRight);
		pin->GetActionHandle("/actions/main/in/PoseLeft",  &hPoseLeft);
		pin->GetActionHandle("/actions/main/out/HapticRight", &hHapticRight);
		pin->GetActionHandle("/actions/main/out/HapticLeft",  &hHapticLeft);
		bInputOK = true;
	}

	//******************************************************************************************
	bool bDigital(vr::VRActionHandle_t h)
	{
		vr::InputDigitalActionData_t d = {};
		if (vr::VRInput()->GetDigitalActionData(h, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None)
			return false;
		return d.bActive && d.bState;
	}

	//******************************************************************************************
	void Analog(vr::VRActionHandle_t h, float& x, float& y)
	{
		vr::InputAnalogActionData_t d = {};
		x = y = 0;
		if (vr::VRInput()->GetAnalogActionData(h, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && d.bActive)
		{
			x = d.x;
			y = d.y;
		}
	}

	//******************************************************************************************
	void Pose(vr::VRActionHandle_t h, SVRPose& pose)
	{
		vr::InputPoseActionData_t d = {};
		pose.bValid = false;
		if (vr::VRInput()->GetPoseActionDataForNextFrame(h, vr::TrackingUniverseStanding, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
			d.bActive && d.pose.bPoseIsValid)
			PoseFromMatrix(d.pose.mDeviceToAbsoluteTracking, pose);
	}

	//******************************************************************************************
	bool bCreateDevice()
	{
		int32_t i_adapter = -1;
		pvrSystem->GetDXGIOutputInfo(&i_adapter);

		IDXGIAdapter1* padapter = nullptr;
		if (i_adapter >= 0)
		{
			IDXGIFactory1* pfactory = nullptr;
			if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pfactory)))
			{
				pfactory->EnumAdapters1(i_adapter, &padapter);
				pfactory->Release();
			}
		}

		D3D_FEATURE_LEVEL afl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
		HRESULT hr = D3D11CreateDevice
		(
			padapter, padapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			afl, sizeof(afl) / sizeof(afl[0]), D3D11_SDK_VERSION, &pd3dDevice, nullptr, &pd3dContext
		);
		SafeRelease(padapter);
		return SUCCEEDED(hr);
	}

	//******************************************************************************************

	//******************************************************************************************
	int iShift(uint32_t u4_mask)
	{
		int i = 0;
		if (!u4_mask)
			return 0;
		while (!(u4_mask & 1))
		{
			u4_mask >>= 1;
			i++;
		}
		return i;
	}

	//******************************************************************************************
	// Expands a masked component to 8 bits.
	inline uint32_t u4Expand(uint32_t u4_pix, uint32_t u4_mask, int i_shift, uint32_t u4_max)
	{
		if (!u4_max)
			return 0;
		return (((u4_pix & u4_mask) >> i_shift) * 255 + u4_max / 2) / u4_max;
	}

	//******************************************************************************************
	void BuildLut16(uint32_t r, uint32_t g, uint32_t b)
	{
		if (au4Lut16.size() == 65536 && au4LutMasks[0] == r && au4LutMasks[1] == g && au4LutMasks[2] == b)
			return;

		au4Lut16.resize(65536);
		int rs = iShift(r), gs = iShift(g), bs = iShift(b);
		uint32_t rm = r >> rs, gm = g >> gs, bm = b >> bs;
		for (uint32_t u = 0; u < 65536; u++)
		{
			uint32_t cr = u4Expand(u, r, rs, rm);
			uint32_t cg = u4Expand(u, g, gs, gm);
			uint32_t cb = u4Expand(u, b, bs, bm);
			au4Lut16[u] = cr | (cg << 8) | (cb << 16) | 0xFF000000;
		}
		au4LutMasks[0] = r;
		au4LutMasks[1] = g;
		au4LutMasks[2] = b;
	}

	//******************************************************************************************
	// Converts a game back buffer to RGBA8 in au4Convert.
	bool bConvert
	(
		const void* pv_pixels, int i_pitch_bytes, int i_width, int i_height,
		int i_bits, uint32_t u4_red_mask, uint32_t u4_green_mask, uint32_t u4_blue_mask
	)
	{
		au4Convert.resize((size_t)i_width * i_height);
		uint32_t* pu4_dst = au4Convert.data();

		if (i_bits == 16)
		{
			BuildLut16(u4_red_mask, u4_green_mask, u4_blue_mask);
			const uint32_t* pu4_lut = au4Lut16.data();
			for (int y = 0; y < i_height; y++)
			{
				const uint16_t* pu2_src = (const uint16_t*)((const uint8_t*)pv_pixels + (size_t)y * i_pitch_bytes);
				uint32_t* pu4_row = pu4_dst + (size_t)y * i_width;
				for (int x = 0; x < i_width; x++)
					pu4_row[x] = pu4_lut[pu2_src[x]];
			}
		}
		else if (i_bits == 32 || i_bits == 24)
		{
			int rs = iShift(u4_red_mask), gs = iShift(u4_green_mask), bs = iShift(u4_blue_mask);
			uint32_t rm = u4_red_mask >> rs, gm = u4_green_mask >> gs, bm = u4_blue_mask >> bs;
			bool b_fast = rm == 0xFF && gm == 0xFF && bm == 0xFF;
			int i_bytes = i_bits / 8;
			for (int y = 0; y < i_height; y++)
			{
				const uint8_t* pu1_src = (const uint8_t*)pv_pixels + (size_t)y * i_pitch_bytes;
				uint32_t* pu4_row = pu4_dst + (size_t)y * i_width;
				for (int x = 0; x < i_width; x++, pu1_src += i_bytes)
				{
					uint32_t u = i_bytes == 4 ? *(const uint32_t*)pu1_src
					                          : (uint32_t)pu1_src[0] | ((uint32_t)pu1_src[1] << 8) | ((uint32_t)pu1_src[2] << 16);
					uint32_t cr, cg, cb;
					if (b_fast)
					{
						cr = (u >> rs) & 0xFF;
						cg = (u >> gs) & 0xFF;
						cb = (u >> bs) & 0xFF;
					}
					else
					{
						cr = u4Expand(u, u4_red_mask, rs, rm);
						cg = u4Expand(u, u4_green_mask, gs, gm);
						cb = u4Expand(u, u4_blue_mask, bs, bm);
					}
					pu4_row[x] = cr | (cg << 8) | (cb << 16) | 0xFF000000;
				}
			}
		}
		else
		{
			return false;
		}
		return true;
	}

	//******************************************************************************************
	bool bEnsureTex(ID3D11Texture2D*& ptex, int& i_tw, int& i_th, int i_w, int i_h)
	{
		if (ptex && i_tw == i_w && i_th == i_h)
			return true;
		SafeRelease(ptex);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width            = i_w;
		desc.Height           = i_h;
		desc.MipLevels        = 1;
		desc.ArraySize        = 1;
		desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage            = D3D11_USAGE_DEFAULT;
		desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(pd3dDevice->CreateTexture2D(&desc, nullptr, &ptex)))
			return false;
		i_tw = i_w;
		i_th = i_h;
		return true;
	}

	ID3D11Texture2D*         pd3dOverlayTex = nullptr;
	int                      iOverlayW = 0, iOverlayH = 0;
	vr::VROverlayHandle_t    hOverlay = vr::k_ulOverlayHandleInvalid;
}

//**********************************************************************************************
bool VRB_Init(int* pi_eye_width, int* pi_eye_height, char* str_err, int i_err_len)
{
	auto fail = [&](const char* str)
	{
		if (str_err && i_err_len > 0)
			std::snprintf(str_err, i_err_len, "%s", str);
		VRB_Shutdown();
		return false;
	};

	if (!vr::VR_IsRuntimeInstalled())
		return fail("SteamVR is not installed.");
	if (!vr::VR_IsHmdPresent())
		return fail("No VR headset detected.");

	vr::EVRInitError err = vr::VRInitError_None;
	pvrSystem = vr::VR_Init(&err, vr::VRApplication_Scene);
	if (err != vr::VRInitError_None || !pvrSystem)
	{
		pvrSystem = nullptr;
		return fail(vr::VR_GetVRInitErrorAsEnglishDescription(err));
	}

	if (!vr::VRCompositor())
		return fail("SteamVR compositor unavailable.");

	vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseStanding);

	if (!bCreateDevice())
		return fail("Could not create a Direct3D 11 device on the headset's GPU.");

	uint32_t u_w = 0, u_h = 0;
	pvrSystem->GetRecommendedRenderTargetSize(&u_w, &u_h);
	if (pi_eye_width)
		*pi_eye_width = (int)u_w;
	if (pi_eye_height)
		*pi_eye_height = (int)u_h;

	InitInput();
	bQuit = false;
	return true;
}

//**********************************************************************************************
void VRB_Shutdown()
{
	if (hOverlay != vr::k_ulOverlayHandleInvalid && vr::VROverlay())
		vr::VROverlay()->DestroyOverlay(hOverlay);
	hOverlay = vr::k_ulOverlayHandleInvalid;
	SafeRelease(pd3dOverlayTex);
	SafeRelease(apd3dEyeTex[0]);
	SafeRelease(apd3dEyeTex[1]);
	if (pd3dContext)
		pd3dContext->ClearState();
	SafeRelease(pd3dContext);
	SafeRelease(pd3dDevice);

	if (pvrSystem)
	{
		vr::VR_Shutdown();
		pvrSystem = nullptr;
	}
	bInputOK = false;
}

//**********************************************************************************************
bool VRB_IsRunning()
{
	return pvrSystem != nullptr;
}

//**********************************************************************************************
bool VRB_QuitRequested()
{
	return bQuit;
}

//**********************************************************************************************
bool VRB_BeginFrame(SVRFrame& frame)
{
	if (!pvrSystem)
		return false;

	// Events.
	vr::VREvent_t ev;
	while (pvrSystem->PollNextEvent(&ev, sizeof(ev)))
	{
		if (ev.eventType == vr::VREvent_Quit)
		{
			pvrSystem->AcknowledgeQuit_Exiting();
			bQuit = true;
		}
	}
	if (bQuit)
		return false;

	// Block until the compositor wants the next frame, and get predicted poses for it.
	if (vr::VRCompositor()->WaitGetPoses(atdpPoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0) != vr::VRCompositorError_None)
		return false;

	const vr::TrackedDevicePose_t& tdp_hmd = atdpPoses[vr::k_unTrackedDeviceIndex_Hmd];
	frame.posHead.bValid = false;
	frame.aposEye[0].bValid = frame.aposEye[1].bValid = false;

	if (tdp_hmd.bPoseIsValid)
	{
		PoseFromMatrix(tdp_hmd.mDeviceToAbsoluteTracking, frame.posHead);
		for (int i = 0; i < 2; i++)
		{
			vr::EVREye eye = i == 0 ? vr::Eye_Left : vr::Eye_Right;
			vr::HmdMatrix34_t mx_eye = mxMul(tdp_hmd.mDeviceToAbsoluteTracking, pvrSystem->GetEyeToHeadTransform(eye));
			PoseFromMatrix(mx_eye, frame.aposEye[i]);
		}
	}

	for (int i = 0; i < 2; i++)
	{
		float l, r, t, b;
		pvrSystem->GetProjectionRaw(i == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
		// OpenVR uses a y-down convention: top is negative for "up".
		frame.afovEye[i].fTanLeft  = l;
		frame.afovEye[i].fTanRight = r;
		frame.afovEye[i].fTanUp    = -t;
		frame.afovEye[i].fTanDown  = -b;
	}

	// Controllers.
	frame.ctl = SVRControls();
	frame.posHandRight.bValid = frame.posHandLeft.bValid = false;
	if (bInputOK)
	{
		vr::VRActiveActionSet_t set = {};
		set.ulActionSet = hSetMain;
		if (vr::VRInput()->UpdateActionState(&set, sizeof(set), 1) == vr::VRInputError_None)
		{
			SVRControls& c = frame.ctl;
			Analog(hMove, c.fMoveX, c.fMoveY);
			Analog(hTurn, c.fTurnX, c.fTurnY);
			c.bHand     = bDigital(hHand);
			c.bTrigger  = bDigital(hTrigger);
			c.bDrop     = bDigital(hDrop);
			c.bJump     = bDigital(hJump);
			c.bCrouch   = bDigital(hCrouch);
			c.bThrow    = bDigital(hThrow);
			c.bStow     = bDigital(hStow);
			c.bPunch    = bDigital(hPunch);
			c.bRecenter = bDigital(hRecenter);
			c.bRun      = bDigital(hRun);
			c.bMenu     = bDigital(hMenu);
			Pose(hPoseRight, frame.posHandRight);
			Pose(hPoseLeft,  frame.posHandLeft);
		}
	}

	return true;
}

//**********************************************************************************************
bool VRB_SubmitEye
(
	int i_eye,
	const void* pv_pixels, int i_pitch_bytes, int i_width, int i_height,
	int i_bits, uint32_t u4_red_mask, uint32_t u4_green_mask, uint32_t u4_blue_mask
)
{
	if (!pvrSystem || !pd3dDevice || i_eye < 0 || i_eye > 1 || !pv_pixels || i_width <= 0 || i_height <= 0)
		return false;

	if (!bEnsureTex(apd3dEyeTex[i_eye], aiTexW[i_eye], aiTexH[i_eye], i_width, i_height))
		return false;

	if (!bConvert(pv_pixels, i_pitch_bytes, i_width, i_height, i_bits, u4_red_mask, u4_green_mask, u4_blue_mask))
		return false;
	const uint32_t* pu4_dst = au4Convert.data();

	pd3dContext->UpdateSubresource(apd3dEyeTex[i_eye], 0, nullptr, pu4_dst, i_width * 4, 0);

	vr::Texture_t tex = { (void*)apd3dEyeTex[i_eye], vr::TextureType_DirectX, vr::ColorSpace_Gamma };
	vr::EVRCompositorError err = vr::VRCompositor()->Submit(i_eye == 0 ? vr::Eye_Left : vr::Eye_Right, &tex);

	if (i_eye == 1)
		pd3dContext->Flush();

	return err == vr::VRCompositorError_None;
}

//**********************************************************************************************
void VRB_Haptic(bool b_right, float f_seconds, float f_amplitude)
{
	if (!bInputOK)
		return;
	vr::VRInput()->TriggerHapticVibrationAction(b_right ? hHapticRight : hHapticLeft, 0, f_seconds, 160.0f, f_amplitude, vr::k_ulInvalidInputValueHandle);
}

//**********************************************************************************************
void VRB_ShowOverlay
(
	const void* pv_pixels, int i_pitch_bytes, int i_width, int i_height,
	int i_bits, uint32_t u4_red_mask, uint32_t u4_green_mask, uint32_t u4_blue_mask,
	float f_width_m, float f_distance_m, bool b_place
)
{
	vr::IVROverlay* pov = vr::VROverlay();
	if (!pvrSystem || !pd3dDevice || !pov)
		return;

	if (hOverlay == vr::k_ulOverlayHandleInvalid)
	{
		if (pov->CreateOverlay("opentrespasser.vr.screen", "Trespasser", &hOverlay) != vr::VROverlayError_None)
		{
			hOverlay = vr::k_ulOverlayHandleInvalid;
			return;
		}
	}

	if (!bConvert(pv_pixels, i_pitch_bytes, i_width, i_height, i_bits, u4_red_mask, u4_green_mask, u4_blue_mask))
		return;
	if (!bEnsureTex(pd3dOverlayTex, iOverlayW, iOverlayH, i_width, i_height))
		return;
	pd3dContext->UpdateSubresource(pd3dOverlayTex, 0, nullptr, au4Convert.data(), i_width * 4, 0);
	pd3dContext->Flush();

	if (b_place)
	{
		// Place the screen in front of the head, level, facing the player.
		vr::TrackedDevicePose_t tdp;
		pvrSystem->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &tdp, 1);
		float fx = 0, fz = -1, px = 0, py = 1.6f, pz = 0;
		if (tdp.bPoseIsValid)
		{
			const vr::HmdMatrix34_t& m = tdp.mDeviceToAbsoluteTracking;
			fx = -m.m[0][2];
			fz = -m.m[2][2];
			float f_len = std::sqrt(fx * fx + fz * fz);
			if (f_len < 1e-3f)
			{
				fx = 0; fz = -1;
			}
			else
			{
				fx /= f_len; fz /= f_len;
			}
			px = m.m[0][3]; py = m.m[1][3]; pz = m.m[2][3];
		}
		// Overlay faces +z; rotate about y so +z points back at the player (-forward).
		vr::HmdMatrix34_t mx = {};
		// x axis = right = (-fz, 0, fx); z axis = -forward = (-fx, 0, -fz).
		mx.m[0][0] = -fz; mx.m[0][1] = 0; mx.m[0][2] = -fx; mx.m[0][3] = px + fx * f_distance_m;
		mx.m[1][0] = 0;   mx.m[1][1] = 1; mx.m[1][2] = 0;   mx.m[1][3] = py;
		mx.m[2][0] = fx;  mx.m[2][1] = 0; mx.m[2][2] = -fz; mx.m[2][3] = pz + fz * f_distance_m;
		pov->SetOverlayTransformAbsolute(hOverlay, vr::TrackingUniverseStanding, &mx);
		pov->SetOverlayWidthInMeters(hOverlay, f_width_m);
	}

	vr::Texture_t tex = { (void*)pd3dOverlayTex, vr::TextureType_DirectX, vr::ColorSpace_Gamma };
	pov->SetOverlayTexture(hOverlay, &tex);
	pov->ShowOverlay(hOverlay);
}

//**********************************************************************************************
void VRB_HideOverlay()
{
	if (hOverlay != vr::k_ulOverlayHandleInvalid && vr::VROverlay())
		vr::VROverlay()->HideOverlay(hOverlay);
}
