/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "d3dx9.hpp"
#include "d3d8to9.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_resolution.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_d3d9ex.hpp"
#include "psobbvr_gamecam.hpp"

PFN_D3DXAssembleShader D3DXAssembleShader = nullptr;
PFN_D3DXDisassembleShader D3DXDisassembleShader = nullptr;
PFN_D3DXLoadSurfaceFromSurface D3DXLoadSurfaceFromSurface = nullptr;

#ifndef D3D8TO9NOLOG
 // Very simple logging for the purpose of debugging only.
std::ofstream LOG;
#endif

extern "C" HRESULT WINAPI ValidatePixelShader(const DWORD* pPixelShader, const D3DCAPS8* pCaps, BOOL ReturnErrors, char** pErrorsString)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "ValidatePixelShader " << "(" << pPixelShader << ", " << pCaps << ", " << ReturnErrors << ", " << pErrorsString << ")' ..." << std::endl;
#endif

	HRESULT hr = E_FAIL;
	const char* errorMessage = "";

	if (!pPixelShader)
	{
		errorMessage = "Invalid code pointer.\n";
	}
	else
	{
		switch (*pPixelShader)
		{
		case D3DPS_VERSION(1, 0):
		case D3DPS_VERSION(1, 1):
		case D3DPS_VERSION(1, 2):
		case D3DPS_VERSION(1, 3):
		case D3DPS_VERSION(1, 4):
			if (pCaps && *pPixelShader > pCaps->PixelShaderVersion)
			{
				errorMessage = "Shader version not supported by caps.\n";
				break;
			}
			hr = S_OK;
			break;

		default:
			errorMessage = "Unsupported shader version.\n";
		}
	}

	if (!ReturnErrors)
	{
		errorMessage = "";
	}

	if (pErrorsString)
	{
		const size_t size = strlen(errorMessage) + 1;

		*pErrorsString = (char*) HeapAlloc(GetProcessHeap(), 0, size);
		if (*pErrorsString)
		{
			memcpy(*pErrorsString, errorMessage, size);
		}
	}

	return hr;
}

extern "C" HRESULT WINAPI ValidateVertexShader(const DWORD* pVertexShader, const DWORD* pVertexDecl, const D3DCAPS8* pCaps, BOOL ReturnErrors, char** pErrorsString)
{
	UNREFERENCED_PARAMETER(pVertexDecl);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "ValidateVertexShader " << "(" << pVertexShader << ", " << pVertexDecl << ", " << pCaps << ", " << ReturnErrors << ", " << pErrorsString << ")' ..." << std::endl;
#endif

	HRESULT hr = E_FAIL;
	const char* errorMessage = "";

	if (!pVertexShader)
	{
		errorMessage = "Invalid code pointer.\n";
	}
	else
	{
		switch (*pVertexShader)
		{
		case D3DVS_VERSION(1, 0):
		case D3DVS_VERSION(1, 1):
			if (pCaps && *pVertexShader > pCaps->VertexShaderVersion)
			{
				errorMessage = "Shader version not supported by caps.\n";
				break;
			}
			hr = S_OK;
			break;

		default:
			errorMessage = "Unsupported shader version.\n";
		}
	}

	if (!ReturnErrors)
	{
		errorMessage = "";
	}

	if (pErrorsString)
	{
		const size_t size = strlen(errorMessage) + 1;

		*pErrorsString = (char*) HeapAlloc(GetProcessHeap(), 0, size);
		if (*pErrorsString)
		{
			memcpy(*pErrorsString, errorMessage, size);
		}
	}

	return hr;
}

extern "C" void WINAPI DebugSetMute()
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "DebugSetMute" << "(" << ")' ..." << std::endl;
#endif
}

// psobbvr: our dinput8 overlay draws in real backbuffer pixels, not the
// game's 640x480 space, so it brackets its ImGui rendering with
// PsobbvrSetScalePassthrough(TRUE/FALSE) to keep the resolution override from
// rescaling its viewports and vertices. It resolves this export at runtime
// and no-ops if it is absent.
extern "C" void WINAPI PsobbvrSetScalePassthrough(BOOL enable)
{
	resolution::passthrough = enable != FALSE;
}

// psobbvr: live control of the side-by-side stereo window from the
// overlay panel. See psobbvr_stereo.hpp for what these drive.
extern "C" void WINAPI PsobbvrStereoConfigure(BOOL enabled, float separation, BOOL cross_eye)
{
	stereo::enabled = enabled != FALSE;
	if (separation > 0.0f && separation < 100.0f)
		stereo::separation = separation;
	stereo::cross_eye = cross_eye != FALSE;
}

extern "C" void WINAPI PsobbvrStereoQuery(BOOL *enabled, float *separation, BOOL *cross_eye, BOOL *healthy, BOOL *double_wide)
{
	if (enabled) *enabled = stereo::enabled;
	if (separation) *separation = stereo::separation;
	if (cross_eye) *cross_eye = stereo::cross_eye;
	if (healthy) *healthy = !stereo::create_failed;
	if (double_wide) *double_wide = stereo::double_wide;
}

// psobbvr: the overlay calls this right before drawing its ImGui panel so
// the side-by-side composite lands on the backbuffer first and the panel is
// drawn on top of it.
extern "C" void WINAPI PsobbvrStereoFinishFrame(void)
{
	stereo::FinishFrame();
}

// psobbvr: arm a dump of the next presented frame to a .bmp file
// (see psobbvr_framedump.hpp). Null/empty path -> psobbvr-frame-NNN.bmp in
// the working directory. Developer build only; the export stays so the
// overlay's lookup still resolves.
extern "C" void WINAPI PsobbvrDumpFrame(const char *path)
{
	UNREFERENCED_PARAMETER(path);
}

// psobbvr: VR control from the overlay panel. Start returns
// FALSE and fills 'error' when the runtime cannot come up; the game keeps
// rendering flat. Starting VR forces the stereo path on (VR renders through
// it) and hands the backend the current eye textures.
extern "C" BOOL WINAPI PsobbvrVrStart(char *error, int error_len)
{
	if (!vrmod::Get()->Init(error, (size_t)error_len, false))
		return FALSE;
	stereo::enabled = true;
	stereo::AttachToVr();
	return TRUE;
}

extern "C" void WINAPI PsobbvrVrStop(void)
{
	vrmod::Get()->Shutdown();
}

extern "C" void WINAPI PsobbvrVrQuery(BOOL *ready, char *status, int status_len,
                                      float *world_scale,
                                      float *hud_distance_m, float *hud_width_deg,
                                      int *game_camera, float *eye_height_m)
{
	if (ready) *ready = vrmod::Get()->Ready();
	if (status && status_len > 0)
		strncpy_s(status, (size_t)status_len, vrmod::Get()->StatusLine(), _TRUNCATE);
	if (world_scale) *world_scale = vrmod::config.world_scale;
	if (hud_distance_m) *hud_distance_m = vrmod::config.hud_distance_m;
	if (hud_width_deg) *hud_width_deg = vrmod::config.hud_width_deg;
	if (game_camera) *game_camera = vrmod::config.game_camera;
	if (eye_height_m) *eye_height_m = vrmod::config.eye_height_m;
}

extern "C" void WINAPI PsobbvrVrTune(float world_scale,
                                     float hud_distance_m, float hud_width_deg,
                                     int game_camera, float eye_height_m)
{
	if (world_scale > 0.01f && world_scale < 1000.0f)
		vrmod::config.world_scale = world_scale;
	if (hud_distance_m > 0.1f && hud_distance_m < 50.0f)
		vrmod::config.hud_distance_m = hud_distance_m;
	if (hud_width_deg > 10.0f && hud_width_deg < 160.0f)
		vrmod::config.hud_width_deg = hud_width_deg;
	if (game_camera >= 0 && game_camera <= 2)
		vrmod::config.game_camera = game_camera;
	if (eye_height_m > 0.1f && eye_height_m < 5.0f)
		vrmod::config.eye_height_m = eye_height_m;
}

// psobbvr: camera takeover - re-capture which world direction the seated
// pose's "forward" maps to (on the next frame the takeover is active).
extern "C" void WINAPI PsobbvrVrRecenter(void)
{
	gamecam::Recenter();
}

extern "C" IDirect3D8 *WINAPI Direct3DCreate8(UINT SDKVersion)
{
#ifndef D3D8TO9NOLOG
	static bool LogMessageFlag = true;

	if (!LOG.is_open())
	{
		LOG.open("d3d8.log", std::ios::trunc);
	}

	if (!LOG.is_open() && LogMessageFlag)
	{
		LogMessageFlag = false;
		MessageBox(nullptr, TEXT("Failed to open debug log file \"d3d8.log\"!"), nullptr, MB_ICONWARNING);
	}

	LOG << "Redirecting '" << "Direct3DCreate8" << "(" << SDKVersion << ")' ..." << std::endl;
	LOG << "> Passing on to 'Direct3DCreate9':" << std::endl;
#endif

	probe::Init();
	resolution::LoadConfig();
	stereo::LoadConfig();
	vrmod::LoadConfig();
	// psobbvr: VR renders through the per-eye stereo path, so [vr] enabled=1
	// turns it on (with its side-by-side debug window).
	if (vrmod::config.enabled)
		stereo::enabled = true;

	// psobbvr: prefer the D3D9Ex interface - the eye render targets must be
	// shareable with the in-process D3D11 device that submits to the VR
	// runtime, and shared handles require an Ex device. Plain D3D9 remains as fallback
	// (and via the PSOBBVR_NO_D3D9EX kill switch).
	IDirect3D9 *d3d = nullptr;
	if (!d3d9ex::Disabled())
	{
		IDirect3D9Ex *d3dex = nullptr;
		if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dex)) && d3dex != nullptr)
		{
			d3d = d3dex;
			d3d9ex::active = true;
		}
	}
	if (d3d == nullptr)
		d3d = Direct3DCreate9(D3D_SDK_VERSION);
	diag::Log("d3d9ex: interface %s", d3d9ex::active ? "Ex" : "plain (fallback)");

	if (d3d == nullptr)
	{
		return nullptr;
	}

	// Load D3DX
	if (!D3DXAssembleShader || !D3DXDisassembleShader || !D3DXLoadSurfaceFromSurface)
	{
		const HMODULE module = LoadLibrary(TEXT("d3dx9_43.dll"));

		if (module != nullptr)
		{
			D3DXAssembleShader = reinterpret_cast<PFN_D3DXAssembleShader>(GetProcAddress(module, "D3DXAssembleShader"));
			D3DXDisassembleShader = reinterpret_cast<PFN_D3DXDisassembleShader>(GetProcAddress(module, "D3DXDisassembleShader"));
			D3DXLoadSurfaceFromSurface = reinterpret_cast<PFN_D3DXLoadSurfaceFromSurface>(GetProcAddress(module, "D3DXLoadSurfaceFromSurface"));
		}
		else
		{
#ifndef D3D8TO9NOLOG
			LOG << "Failed to load d3dx9_43.dll! Some features will not work correctly." << std::endl;
#endif
			if (MessageBox(nullptr, TEXT(
					"Failed to load d3dx9_43.dll! Some features will not work correctly.\n\n"
					"It's required to install the \"Microsoft DirectX End-User Runtime\" in order to use d3d8to9, or alternatively get the DLLs from this NuGet package:\nhttps://www.nuget.org/packages/Microsoft.DXSDK.D3DX\n\n"
					"Please click \"OK\" to open the official download page or \"Cancel\" to continue anyway."), nullptr, MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND | MB_OKCANCEL | MB_DEFBUTTON1) == IDOK)
			{
				ShellExecute(nullptr, TEXT("open"), TEXT("https://www.microsoft.com/download/details.aspx?id=35"), nullptr, nullptr, SW_SHOW);

				return nullptr;
			}
		}
	}

	return new Direct3D8(d3d);
}
