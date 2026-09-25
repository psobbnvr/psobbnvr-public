/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "d3d8to9.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_resolution.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_d3d9ex.hpp"
#include "psobbvr_vr.hpp"

static const D3DFORMAT AdapterFormats[] = {
	D3DFMT_A8R8G8B8,
	D3DFMT_X8R8G8B8,
	D3DFMT_R5G6B5,
	D3DFMT_X1R5G5B5,
	D3DFMT_A1R5G5B5
};

Direct3D8::Direct3D8(IDirect3D9 *ProxyInterface) :
	ProxyInterface(ProxyInterface)
{
	D3DDISPLAYMODE pMode;

	CurrentAdapterCount = ProxyInterface->GetAdapterCount();
	if (CurrentAdapterCount > MAX_ADAPTERS)
		CurrentAdapterCount = MAX_ADAPTERS;

	for (UINT Adapter = 0; Adapter < CurrentAdapterCount; Adapter++)
	{
		for (D3DFORMAT Format : AdapterFormats)
		{
			const UINT ModeCount = ProxyInterface->GetAdapterModeCount(Adapter, Format);

			for (UINT Mode = 0; Mode < ModeCount; Mode++)
			{
				ProxyInterface->EnumAdapterModes(Adapter, Format, Mode, &pMode);
				CurrentAdapterModes[Adapter].push_back(pMode);
				CurrentAdapterModeCount[Adapter]++;
			}
		}
	}
}
Direct3D8::~Direct3D8()
{
}

HRESULT STDMETHODCALLTYPE Direct3D8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3D8) ||
		riid == __uuidof(IUnknown))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3D8 *>(this);

		return S_OK;
	}

	return ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
}
ULONG STDMETHODCALLTYPE Direct3D8::AddRef()
{
	return ProxyInterface->AddRef();
}
ULONG STDMETHODCALLTYPE Direct3D8::Release()
{
	const ULONG LastRefCount = ProxyInterface->Release();

	if (LastRefCount == 0)
		delete this;

	return LastRefCount;
}

HRESULT STDMETHODCALLTYPE Direct3D8::RegisterSoftwareDevice(void *pInitializeFunction)
{
	return ProxyInterface->RegisterSoftwareDevice(pInitializeFunction);
}
UINT STDMETHODCALLTYPE Direct3D8::GetAdapterCount()
{
	return CurrentAdapterCount;
}
HRESULT STDMETHODCALLTYPE Direct3D8::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 *pIdentifier)
{
	if (pIdentifier == nullptr)
		return D3DERR_INVALIDCALL;

	D3DADAPTER_IDENTIFIER9 AdapterIndentifier;

	if ((Flags & D3DENUM_NO_WHQL_LEVEL) == 0)
	{
		Flags |= D3DENUM_WHQL_LEVEL;
	}
	else
	{
		Flags ^= D3DENUM_NO_WHQL_LEVEL;
	}

	const HRESULT hr = ProxyInterface->GetAdapterIdentifier(Adapter, Flags, &AdapterIndentifier);
	if (FAILED(hr))
		return hr;

	ConvertAdapterIdentifier(AdapterIndentifier, *pIdentifier);

	return D3D_OK;
}
UINT STDMETHODCALLTYPE Direct3D8::GetAdapterModeCount(UINT Adapter)
{
	return CurrentAdapterModeCount[Adapter];
}
HRESULT STDMETHODCALLTYPE Direct3D8::EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE *pMode)
{
	if (pMode == nullptr || !(Adapter < CurrentAdapterCount && Mode < CurrentAdapterModeCount[Adapter]))
		return D3DERR_INVALIDCALL;

	pMode->Format = CurrentAdapterModes[Adapter].at(Mode).Format;
	pMode->Height = CurrentAdapterModes[Adapter].at(Mode).Height;
	pMode->RefreshRate = CurrentAdapterModes[Adapter].at(Mode).RefreshRate;
	pMode->Width = CurrentAdapterModes[Adapter].at(Mode).Width;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3D8::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode)
{
	return ProxyInterface->GetAdapterDisplayMode(Adapter, pMode);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
{
	return ProxyInterface->CheckDeviceType(Adapter, CheckType, DisplayFormat, BackBufferFormat, bWindowed);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{
	if (CheckFormat == D3DFMT_UYVY ||
		CheckFormat == D3DFMT_YUY2 ||
		CheckFormat == MAKEFOURCC('Y', 'V', '1', '2') ||
		CheckFormat == MAKEFOURCC('N', 'V', '1', '2'))
	{
		return D3DERR_NOTAVAILABLE;
	}

	return ProxyInterface->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType)
{
	return ProxyInterface->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, nullptr);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
{
	return ProxyInterface->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
}
HRESULT STDMETHODCALLTYPE Direct3D8::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 *pCaps)
{
	if (pCaps == nullptr)
		return D3DERR_INVALIDCALL;

	D3DCAPS9 DeviceCaps;

	const HRESULT hr = ProxyInterface->GetDeviceCaps(Adapter, DeviceType, &DeviceCaps);
	if (FAILED(hr))
		return hr;

	ConvertCaps(DeviceCaps, *pCaps);

	return D3D_OK;
}
HMONITOR STDMETHODCALLTYPE Direct3D8::GetAdapterMonitor(UINT Adapter)
{
	return ProxyInterface->GetAdapterMonitor(Adapter);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS8 *pPresentationParameters, IDirect3DDevice8 **ppReturnedDeviceInterface)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3D8::CreateDevice" << "(" << this << ", " << Adapter << ", " << DeviceType << ", " << hFocusWindow << ", " << BehaviorFlags << ", " << pPresentationParameters << ", " << ppReturnedDeviceInterface << ")' ..." << std::endl;
#endif

	if (pPresentationParameters == nullptr || ppReturnedDeviceInterface == nullptr)
		return D3DERR_INVALIDCALL;

	*ppReturnedDeviceInterface = nullptr;

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);
	resolution::OverridePresentParameters(PresentParams);
	stereo::OverridePresentParameters(PresentParams);

	IDirect3DDevice9 *DeviceInterface = nullptr;

	// psobbvr: on the Ex interface the device must come from CreateDeviceEx -
	// only such a device accepts shared handles on its render targets (the
	// hand-off to D3D11 and the VR runtime). Falls back to a plain device.
	HRESULT hr;
	if (d3d9ex::active)
	{
		IDirect3DDevice9Ex *DeviceEx = nullptr;
		hr = static_cast<IDirect3D9Ex *>(ProxyInterface)->CreateDeviceEx(
			Adapter, DeviceType, hFocusWindow, BehaviorFlags, &PresentParams, nullptr, &DeviceEx);
		if (SUCCEEDED(hr))
		{
			DeviceInterface = DeviceEx;
		}
		else
		{
			diag::Log("d3d9ex: CreateDeviceEx failed hr=0x%08lX, falling back to a plain device", hr);
			d3d9ex::active = false;
			hr = ProxyInterface->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, &PresentParams, &DeviceInterface);
		}
	}
	else
	{
		hr = ProxyInterface->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, &PresentParams, &DeviceInterface);
	}
	// psobbvr: log the outcome (psobbvr-vr.log) - a failure here makes the
	// game retry once and exit, and the log must say why.
	if (FAILED(hr))
	{
		diag::Log("d3d: CreateDevice FAILED hr=0x%08lX (adapter %u, backbuffer %ux%u format %d, windowed %d, flags 0x%08lX)",
		          hr, Adapter, PresentParams.BackBufferWidth, PresentParams.BackBufferHeight,
		          (int)PresentParams.BackBufferFormat, (int)PresentParams.Windowed, (unsigned long)BehaviorFlags);
		return hr;
	}
	diag::Log("d3d: device created (%s, backbuffer %ux%u, windowed %d)",
	          d3d9ex::active ? "D3D9Ex" : "plain D3D9",
	          PresentParams.BackBufferWidth, PresentParams.BackBufferHeight, (int)PresentParams.Windowed);

	*ppReturnedDeviceInterface = new Direct3DDevice8(this, DeviceInterface, BehaviorFlags, PresentParams.EnableAutoDepthStencil ? PresentParams.AutoDepthStencilFormat : D3DFMT_UNKNOWN, (PresentParams.Flags & D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL) != 0);

	// psobbvr: seed the internal lost-device recovery (see HandlePresentResult)
	// with the parameters the device was actually created with.
	static_cast<Direct3DDevice8 *>(*ppReturnedDeviceInterface)->RememberPresentParameters(PresentParams);

	// Set default vertex declaration
	DeviceInterface->SetFVF(D3DFVF_XYZ);

	// psobbvr: [vr] enabled=1 starts the VR runtime as soon as the device is
	// up. A failure (no SteamVR, no headset) is logged and reported in a
	// plain-language message box (vrmod::ReportLaunchFailure), after which
	// the game exits ([vr] vr_fail_exit). With the box suppressed
	// (PSOBBVR_QUIET or vr_fail_message=0) the game runs flat and the
	// overlay panel can retry via PsobbvrVrStart.
	if (vrmod::config.enabled && !vrmod::Get()->Ready())
	{
		char vr_error[512] = {};
		if (!vrmod::Get()->Init(vr_error, sizeof(vr_error), true))
		{
			probe::Log("vr: launch init failed: %s", vr_error);
			vrmod::ReportLaunchFailure(vr_error);
		}
	}

	return D3D_OK;
}
