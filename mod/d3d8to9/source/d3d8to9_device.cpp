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
#include "psobbvr_log.hpp"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_texguard.hpp"
#include "psobbvr_textsnap.hpp"
#include "psobbvr_controller.hpp"
#include "psobbvr_swingind.hpp"
#include "psobbvr_objvis.hpp"
#include "psobbvr_trail.hpp"
#include "psobbvr_particlepool.hpp"
#include "psobbvr_trim.hpp"
#include "psobbvr_partmap.hpp"
#include "psobbvr_weapongrip.hpp"
#include "psobbvr_hands.hpp"
#include "psobbvr_targetaim.hpp"
#include "psobbvr_gunfire.hpp"
#include "psobbvr_techcast.hpp"
#include "psobbvr_haptics.hpp"
#include "psobbvr_cullfov.hpp"
#include "psobbvr_drawdist.hpp"
#include "psobbvr_movement.hpp"
#include <regex>
#include <assert.h>
#include <intrin.h>  // psobbvr: _ReturnAddress / _AddressOfReturnAddress - draw call sites

struct VertexShaderInfo
{
	IDirect3DVertexShader9 *Shader = nullptr;
	IDirect3DVertexDeclaration9 *Declaration = nullptr;
};

Direct3DDevice8::Direct3DDevice8(Direct3D8 *d3d, IDirect3DDevice9 *ProxyInterface, DWORD BehaviorFlags, D3DFORMAT ZBufferFormat, BOOL EnableZBufferDiscarding) :
	D3D(d3d), ProxyInterface(ProxyInterface), ZBufferDiscarding(EnableZBufferDiscarding)
{
	ProxyAddressLookupTable = new AddressLookupTable(this);

	const HDC hDC = GetDC(nullptr);
	IsPaletteSupported = (::GetDeviceCaps(hDC, RASTERCAPS) & RC_PALETTE) != 0;
	ReleaseDC(nullptr, hDC);

	IsMixedVertexProcessingDevice = (BehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0;

	CurrentZBufferBitCount = GetDepthStencilBitCount(ZBufferFormat);

	// The default value of D3DRS_POINTSIZE_MIN is 0.0f in D3D8,
	// whereas in D3D9 it is 1.0f, so adjust it as needed
	ProxyInterface->SetRenderState(D3DRS_POINTSIZE_MIN, (DWORD)0.0f);
	// The DEPTHBIAS value of -0.0f works differently than 0.0f
	// Some games require defaulting to -0.0f to work correctly
	const float DepthBias = -0.0f;
	ProxyInterface->SetRenderState(D3DRS_DEPTHBIAS, *(const DWORD *)&DepthBias);
}
Direct3DDevice8::~Direct3DDevice8()
{
	// psobbvr: drop the stereo eye targets if they belong to this device,
	// and the swing indicator's textures (reloaded on the next draw).
	if (stereo::device == ProxyInterface)
		stereo::ReleaseTargets();
	swingind::ReleaseAssets();

	delete ProxyAddressLookupTable;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3DDevice8) ||
		riid == __uuidof(IUnknown))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3DDevice8 *>(this);

		return S_OK;
	}

	const HRESULT hr = ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
	if (SUCCEEDED(hr))
		GenericQueryInterface(riid, ppvObj, this);

	return hr;
}
ULONG STDMETHODCALLTYPE Direct3DDevice8::AddRef()
{
	ULONG LastRefCount = ProxyInterface->AddRef();

	// Shaders and state blocks increase ref counter in d3d9 but not in d3d8
	DWORD ExtraRefs = VertexShaderAndDeclarationCount + PixelShaderHandles.size() + StateBlockTokens.size();
	if (ExtraRefs <= LastRefCount)
	{
		LastRefCount = LastRefCount - ExtraRefs;
	}

	return LastRefCount;
}

ULONG STDMETHODCALLTYPE Direct3DDevice8::Release()
{
	// Get current value before releasing the device reference
	ULONG LastRefCount = ProxyInterface->AddRef();
	LastRefCount = ProxyInterface->Release();

	// Shaders and StateBlocks are destroyed alongside the device that created them in D3D8 but not in D3D9
	// so we need to Release any remaining shaders or state blocks when the device is released to mirror that behaviour
	DWORD ExtraRefs = VertexShaderAndDeclarationCount + PixelShaderHandles.size() + StateBlockTokens.size();
	if (ExtraRefs <= LastRefCount)
	{
		LastRefCount = LastRefCount - ExtraRefs;
		if (LastRefCount == 1)
		{
			// Release shaders and state blocks when only one reference is left
			ReleaseShadersAndStateBlocks();
		}
	}

	// Release device reference
	LastRefCount = ProxyInterface->Release();

	if (LastRefCount == 0)
		delete this;

	return LastRefCount;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice8::TestCooperativeLevel()
{
	return ProxyInterface->TestCooperativeLevel();
}
UINT STDMETHODCALLTYPE Direct3DDevice8::GetAvailableTextureMem()
{
	return ProxyInterface->GetAvailableTextureMem();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ResourceManagerDiscardBytes(DWORD Bytes)
{
	UNREFERENCED_PARAMETER(Bytes);

	return ProxyInterface->EvictManagedResources();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDirect3D(IDirect3D8 **ppD3D8)
{
	if (ppD3D8 == nullptr)
		return D3DERR_INVALIDCALL;

	D3D->AddRef();
	*ppD3D8 = D3D;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDeviceCaps(D3DCAPS8 *pCaps)
{
	if (pCaps == nullptr)
		return D3DERR_INVALIDCALL;

	D3DCAPS9 DeviceCaps;

	const HRESULT hr = ProxyInterface->GetDeviceCaps(&DeviceCaps);
	if (FAILED(hr))
		return hr;

	ConvertCaps(DeviceCaps, *pCaps);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDisplayMode(D3DDISPLAYMODE *pMode)
{
	return ProxyInterface->GetDisplayMode(0, pMode);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
	return ProxyInterface->GetCreationParameters(pParameters);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 *pCursorBitmap)
{
	if (pCursorBitmap == nullptr)
		return D3DERR_INVALIDCALL;

	auto pCursorBitmapImpl = static_cast<Direct3DSurface8 *>(pCursorBitmap);
	return ProxyInterface->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmapImpl->GetProxyInterface());
}
void STDMETHODCALLTYPE Direct3DDevice8::SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace, DWORD Flags)
{
	ProxyInterface->SetCursorPosition(XScreenSpace, YScreenSpace, Flags);
}
BOOL STDMETHODCALLTYPE Direct3DDevice8::ShowCursor(BOOL bShow)
{
	return ProxyInterface->ShowCursor(bShow);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS8 *pPresentationParameters, IDirect3DSwapChain8 **ppSwapChain)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateAdditionalSwapChain" << "(" << this << ", " << pPresentationParameters << ", " << ppSwapChain << ")' ..." << std::endl;
#endif

	if (pPresentationParameters == nullptr || ppSwapChain == nullptr)
		return D3DERR_INVALIDCALL;

	*ppSwapChain = nullptr;

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);

	IDirect3DSwapChain9 *SwapChainInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateAdditionalSwapChain(&PresentParams, &SwapChainInterface);
	if (FAILED(hr))
		return hr;

	*ppSwapChain = ProxyAddressLookupTable->FindAddress<Direct3DSwapChain8>(SwapChainInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::Reset(D3DPRESENT_PARAMETERS8 *pPresentationParameters)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::Reset" << "(" << this << ", " << pPresentationParameters << ")' ..." << std::endl;
#endif

	if (pPresentationParameters == nullptr)
		return D3DERR_INVALIDCALL;

	CurrentZBiasRenderState = 0;

	const HRESULT deviceState = ProxyInterface->TestCooperativeLevel();

	if (deviceState == D3DERR_DEVICENOTRESET) {
		while (!StateBlockTokens.empty())
		{
			DWORD Token = *StateBlockTokens.begin();
			DeleteStateBlock(Token);
		}
	}

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);
	resolution::OverridePresentParameters(PresentParams);
	stereo::OverridePresentParameters(PresentParams);

	// psobbvr: the stereo eye targets and the swing indicator's assets live
	// in D3DPOOL_DEFAULT and would make Reset fail; they are recreated lazily.
	stereo::ReleaseTargets();
	swingind::ReleaseAssets();

	const HRESULT hr = ProxyInterface->Reset(&PresentParams);

	if (SUCCEEDED(hr))
	{
		// psobbvr: keep the recovery copy in sync with what the device
		// actually runs with now.
		RememberPresentParameters(PresentParams);
		// The default value of D3DRS_POINTSIZE_MIN is 0.0f in D3D8,
		// whereas in D3D9 it is 1.0f, so adjust it as needed
		ProxyInterface->SetRenderState(D3DRS_POINTSIZE_MIN, (DWORD) 0.0f);
		// The DEPTHBIAS value of -0.0f works differently than 0.0f
		// Some games require defaulting to -0.0f to work correctly
		float DepthBias = -0.0f;
		ProxyInterface->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&DepthBias);
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
{
	UNREFERENCED_PARAMETER(pDirtyRegion);

	// psobbvr: composite the stereo eyes if the dinput8 overlay has not
	// already done so this frame.
	stereo::FinishFrame();


	// psobbvr: draw the swing-timing indicator over the game's HUD
	// (psobbvr_swingind.hpp), then submit the eye textures to the headset
	// and wait for the next frame's head pose (no-op unless VR is running).
	swingind::Draw(ProxyInterface);
	stereo::VrEndOfFrame();

	// psobbvr: camera takeover, second write with the fresh pose: the
	// game's between-frames update (billboards, culling, audio) uses it;
	// the BeginScene write re-asserts it in case that update rewrote the
	// camera.
	gamecam::Apply();
	stereo::InvalidateEyeCache();  // the pose the eye views build from moved

	probe::OnPresent();


	const HRESULT hr = ProxyInterface->Present(pSourceRect, pDestRect, hDestWindowOverride, nullptr);
	stereo::OnFrameEnd();
	// psobbvr: keep the root matrix the bones were posed with this frame
	// for next tick's mag updates (psobbvr_objvis.hpp).
	objvis::OnFrameEnd();
	return HandlePresentResult(hr);
}
// psobbvr: PSO exits with the "display properties changed" dialog when
// Present returns D3DERR_DEVICELOST (exe code at 0x0083AE40, the only path
// to that dialog; SteamVR activating the headset display can cause it). So
// recover from a lost device here without telling the game. If recovery
// keeps failing for ~30 s (900 frames at 30 FPS), pass the real error
// through and let the game exit as it normally would.
HRESULT Direct3DDevice8::HandlePresentResult(HRESULT hr)
{
	if (SUCCEEDED(hr))
	{
		// Also flatten Ex success codes (S_PRESENT_MODE_CHANGED etc.) - the
		// game only understands D3D_OK.
		DeviceLossFrames = 0;
		return D3D_OK;
	}

	DeviceLossFrames++;
	if (DeviceLossFrames == 1)
		diag::Log("present: device lost (hr=0x%08lX), recovering internally", hr);

	bool try_reset = false;
	if (d3d9ex::active)
	{
		// An Ex device is never "not reset"; ResetEx is the recovery lever
		// and does not destroy resources. Retry it every second.
		try_reset = (DeviceLossFrames % 30) == 1;
	}
	else
	{
		// Plain D3D9: the classic dance - wait until the runtime reports
		// D3DERR_DEVICENOTRESET, then Reset.
		try_reset = ProxyInterface->TestCooperativeLevel() == D3DERR_DEVICENOTRESET;
	}

	if (try_reset && HaveLastPresentParams)
	{
		// Same preparation as the game-facing Reset: state blocks do not
		// survive one, and our default-pool eye targets must be released
		// first (they are recreated lazily on the next stereo frame).
		while (!StateBlockTokens.empty())
			DeleteStateBlock(*StateBlockTokens.begin());
		CurrentZBiasRenderState = 0;
		stereo::ReleaseTargets();
		swingind::ReleaseAssets();

		D3DPRESENT_PARAMETERS PresentParams = LastPresentParams;
		HRESULT reset;
		if (d3d9ex::active)
			reset = static_cast<IDirect3DDevice9Ex *>(ProxyInterface)->ResetEx(&PresentParams, nullptr);
		else
			reset = ProxyInterface->Reset(&PresentParams);
		probe::Log("present: internal %s hr=0x%08lX",
			d3d9ex::active ? "ResetEx" : "Reset", reset);

		if (SUCCEEDED(reset))
		{
			// Restore the two D3D8-vs-D3D9 default differences, like the
			// game-facing Reset does.
			ProxyInterface->SetRenderState(D3DRS_POINTSIZE_MIN, (DWORD) 0.0f);
			float DepthBias = -0.0f;
			ProxyInterface->SetRenderState(D3DRS_DEPTHBIAS, *(DWORD*)&DepthBias);
			diag::Log("present: device recovered after %d frames", DeviceLossFrames);
			DeviceLossFrames = 0;
			return D3D_OK;
		}
	}

	if (DeviceLossFrames == 900)
		diag::Log("present: device still lost after %d frames, passing the error to the game", DeviceLossFrames);
	return DeviceLossFrames < 900 ? D3D_OK : hr;
}
void Direct3DDevice8::RememberPresentParameters(const D3DPRESENT_PARAMETERS &PresentParams)
{
	LastPresentParams = PresentParams;
	HaveLastPresentParams = true;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer)
{
	if (ppBackBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppBackBuffer = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetBackBuffer(0, iBackBuffer, Type, &SurfaceInterface);
	if (FAILED(hr))
		return hr;

	*ppBackBuffer = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus)
{
	return ProxyInterface->GetRasterStatus(0, pRasterStatus);
}
void STDMETHODCALLTYPE Direct3DDevice8::SetGammaRamp(DWORD Flags, const D3DGAMMARAMP *pRamp)
{
	ProxyInterface->SetGammaRamp(0, Flags, pRamp);
}
void STDMETHODCALLTYPE Direct3DDevice8::GetGammaRamp(D3DGAMMARAMP *pRamp)
{
	ProxyInterface->GetGammaRamp(0, pRamp);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
	if (ppTexture == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppTexture = nullptr;

	if (Pool == D3DPOOL_DEFAULT)
	{
		D3DDEVICE_CREATION_PARAMETERS CreationParams;
		ProxyInterface->GetCreationParameters(&CreationParams);

		if ((Usage & D3DUSAGE_DYNAMIC) == 0 &&
			SUCCEEDED(D3D->GetProxyInterface()->CheckDeviceFormat(CreationParams.AdapterOrdinal, CreationParams.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, Format)))
		{
			Usage |= D3DUSAGE_RENDERTARGET;
		}
		else if (Usage != D3DUSAGE_DEPTHSTENCIL)
		{
			Usage |= D3DUSAGE_DYNAMIC;
		}
	}

	// psobbvr: after the branch above so a remapped MANAGED texture gets
	// DYNAMIC (lockable), never the RENDERTARGET upgrade (not lockable).
	d3d9ex::RemapPool(Pool, Usage);

	IDirect3DTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateTexture(Width, Height, Levels, Usage, Format, Pool, &TextureInterface, nullptr);
	probe::Log("CreateTexture %ux%u lv=%u usage=0x%lX fmt=%d pool=%d hr=0x%08lX",
	           Width, Height, Levels, Usage, Format, Pool, hr);
	if (FAILED(hr))
		return hr;

	*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DTexture8>(TextureInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture)
{
	if (ppVolumeTexture == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppVolumeTexture = nullptr;

	d3d9ex::RemapPool(Pool, Usage);

	IDirect3DVolumeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, &TextureInterface, nullptr);
	probe::Log("CreateVolumeTexture %ux%ux%u usage=0x%lX fmt=%d pool=%d hr=0x%08lX",
	           Width, Height, Depth, Usage, Format, Pool, hr);
	if (FAILED(hr))
		return hr;

	*ppVolumeTexture = ProxyAddressLookupTable->FindAddress<Direct3DVolumeTexture8>(TextureInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture)
{
	if (ppCubeTexture == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppCubeTexture = nullptr;

	d3d9ex::RemapPool(Pool, Usage);

	IDirect3DCubeTexture9 *TextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, &TextureInterface, nullptr);
	probe::Log("CreateCubeTexture edge=%u usage=0x%lX fmt=%d pool=%d hr=0x%08lX",
	           EdgeLength, Usage, Format, Pool, hr);
	if (FAILED(hr))
		return hr;

	*ppCubeTexture = ProxyAddressLookupTable->FindAddress<Direct3DCubeTexture8>(TextureInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
	if (ppVertexBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppVertexBuffer = nullptr;

	d3d9ex::RemapPool(Pool, Usage);

	IDirect3DVertexBuffer9 *BufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateVertexBuffer(Length, Usage, FVF, Pool, &BufferInterface, nullptr);
	probe::Log("CreateVertexBuffer len=%u usage=0x%lX fvf=0x%lX pool=%d hr=0x%08lX",
	           Length, Usage, FVF, Pool, hr);
	if (FAILED(hr))
		return hr;

	*ppVertexBuffer = ProxyAddressLookupTable->FindAddress<Direct3DVertexBuffer8>(BufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
	if (ppIndexBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	*ppIndexBuffer = nullptr;

	d3d9ex::RemapPool(Pool, Usage);

	IDirect3DIndexBuffer9 *BufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateIndexBuffer(Length, Usage, Format, Pool, &BufferInterface, nullptr);
	probe::Log("CreateIndexBuffer len=%u usage=0x%lX fmt=%d pool=%d hr=0x%08lX",
	           Length, Usage, Format, Pool, hr);
	if (FAILED(hr))
		return hr;

	*ppIndexBuffer = ProxyAddressLookupTable->FindAddress<Direct3DIndexBuffer8>(BufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppSurface = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateRenderTarget(Width, Height, Format, MultiSample, 0, Lockable, &SurfaceInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	*ppSurface = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateDepthStencilSurface(Width, Height, Format, MultiSample, 0, ZBufferDiscarding, &SurfaceInterface, nullptr);
	if (FAILED(hr))
		return hr;

	*ppSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateImageSurface(UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateImageSurface" << "(" << this << ", " << Width << ", " << Height << ", " << Format << ", " << ppSurface << ")' ..." << std::endl;
#endif

	if (ppSurface == nullptr)
		return D3DERR_INVALIDCALL;

	// Only 'CreateImageSurface' clears the content of ppSurface before checking if Format is equal to D3DFMT_UNKNOWN.
	*ppSurface = nullptr;

	if (Format == D3DFMT_UNKNOWN)
		return D3DERR_INVALIDCALL;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SYSTEMMEM, &SurfaceInterface, nullptr);

	if (FAILED(hr) && FAILED(ProxyInterface->CreateOffscreenPlainSurface(Width, Height, Format, D3DPOOL_SCRATCH, &SurfaceInterface, nullptr)))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> 'IDirect3DDevice9::CreateOffscreenPlainSurface' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
		return hr;
	}

	*ppSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CopyRects(IDirect3DSurface8 *pSourceSurface, const RECT *pSourceRectsArray, UINT cRects, IDirect3DSurface8 *pDestinationSurface, const POINT *pDestPointsArray)
{
	if (pSourceSurface == nullptr || pDestinationSurface == nullptr || pSourceSurface == pDestinationSurface)
		return D3DERR_INVALIDCALL;

	auto pSourceSurfaceImpl = static_cast<Direct3DSurface8 *>(pSourceSurface);
	auto pDestinationSurfaceImpl = static_cast<Direct3DSurface8 *>(pDestinationSurface);

	D3DSURFACE_DESC SourceDesc, DestinationDesc;
	pSourceSurfaceImpl->GetProxyInterface()->GetDesc(&SourceDesc);
	pDestinationSurfaceImpl->GetProxyInterface()->GetDesc(&DestinationDesc);

	if (SourceDesc.Format != DestinationDesc.Format)
		return D3DERR_INVALIDCALL;

	if (GetDepthStencilBitCount(SourceDesc.Format) != 0)
		return D3DERR_INVALIDCALL;

	HRESULT hr = D3DERR_INVALIDCALL;

	if (cRects == 0)
		cRects  = 1;

	for (UINT i = 0; i < cRects; i++)
	{
		RECT SourceRect, DestinationRect;

		if (pSourceRectsArray != nullptr)
		{
			SourceRect = pSourceRectsArray[i];
		}
		else
		{
			SourceRect.left = 0;
			SourceRect.right = SourceDesc.Width;
			SourceRect.top = 0;
			SourceRect.bottom = SourceDesc.Height;
		}

		if (pDestPointsArray != nullptr)
		{
			DestinationRect.left = pDestPointsArray[i].x;
			DestinationRect.right = DestinationRect.left + (SourceRect.right - SourceRect.left);
			DestinationRect.top = pDestPointsArray[i].y;
			DestinationRect.bottom = DestinationRect.top + (SourceRect.bottom - SourceRect.top);
		}
		else
		{
			DestinationRect = SourceRect;
		}

		if (SourceDesc.Pool == D3DPOOL_MANAGED || DestinationDesc.Pool != D3DPOOL_DEFAULT)
		{
			hr = D3DERR_INVALIDCALL;
			if (D3DXLoadSurfaceFromSurface != nullptr)
			{
				if (SUCCEEDED(D3DXLoadSurfaceFromSurface(pDestinationSurfaceImpl->GetProxyInterface(), nullptr, &DestinationRect, pSourceSurfaceImpl->GetProxyInterface(), nullptr, &SourceRect, D3DX_FILTER_NONE, 0)))
				{
					// Explicitly call AddDirtyRect on the surface
					void *pContainer = nullptr;
					if (SUCCEEDED(pDestinationSurfaceImpl->GetContainer(IID_IDirect3DTexture9, &pContainer)) && pContainer)
					{
						IDirect3DTexture9 *pTexture = (IDirect3DTexture9*)pContainer;
						pTexture->AddDirtyRect(&DestinationRect);
						pTexture->Release();
					}
					hr = D3D_OK;
				}
			}
		}
		else if (SourceDesc.Pool == D3DPOOL_DEFAULT)
		{
			hr = ProxyInterface->StretchRect(pSourceSurfaceImpl->GetProxyInterface(), &SourceRect, pDestinationSurfaceImpl->GetProxyInterface(), &DestinationRect, D3DTEXF_NONE);
		}
		else if (SourceDesc.Pool == D3DPOOL_SYSTEMMEM)
		{
			const POINT pt = { DestinationRect.left, DestinationRect.top };

			hr = ProxyInterface->UpdateSurface(pSourceSurfaceImpl->GetProxyInterface(), &SourceRect, pDestinationSurfaceImpl->GetProxyInterface(), &pt);
		}

		if (FAILED(hr))
		{
#ifndef D3D8TO9NOLOG
			LOG << "Failed to translate 'IDirect3DDevice8::CopyRects' call from '[" << SourceDesc.Width << "x" << SourceDesc.Height << ", " << SourceDesc.Format << ", " << SourceDesc.MultiSampleType << ", " << SourceDesc.Usage << ", " << SourceDesc.Pool << "]' to '[" << DestinationDesc.Width << "x" << DestinationDesc.Height << ", " << DestinationDesc.Format << ", " << DestinationDesc.MultiSampleType << ", " << DestinationDesc.Usage << ", " << DestinationDesc.Pool << "]'!" << std::endl;
#endif
			break;
		}
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::UpdateTexture(IDirect3DBaseTexture8 *pSourceTexture, IDirect3DBaseTexture8 *pDestinationTexture)
{
	if (pSourceTexture == nullptr || pDestinationTexture == nullptr || pSourceTexture->GetType() != pDestinationTexture->GetType())
		return D3DERR_INVALIDCALL;

	IDirect3DBaseTexture9 *SourceBaseTextureInterface, *DestinationBaseTextureInterface;

	switch (pSourceTexture->GetType())
	{
	case D3DRTYPE_TEXTURE:
		SourceBaseTextureInterface = static_cast<Direct3DTexture8 *>(pSourceTexture)->GetProxyInterface();
		DestinationBaseTextureInterface = static_cast<Direct3DTexture8 *>(pDestinationTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_VOLUMETEXTURE:
		SourceBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pSourceTexture)->GetProxyInterface();
		DestinationBaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pDestinationTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_CUBETEXTURE:
		SourceBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pSourceTexture)->GetProxyInterface();
		DestinationBaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pDestinationTexture)->GetProxyInterface();
		break;
	default:
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->UpdateTexture(SourceBaseTextureInterface, DestinationBaseTextureInterface);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetFrontBuffer(IDirect3DSurface8 *pDestSurface)
{
	if (pDestSurface == nullptr)
		return D3DERR_INVALIDCALL;

	auto pDestSurfaceImpl = static_cast<Direct3DSurface8 *>(pDestSurface);
	return ProxyInterface->GetFrontBufferData(0, pDestSurfaceImpl->GetProxyInterface());
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderTarget(IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pNewZStencil)
{
	HRESULT hr;

	if (pRenderTarget != nullptr)
	{
		auto pRenderTargetImpl = static_cast<Direct3DSurface8 *>(pRenderTarget);
		if (probe::enabled)
		{
			D3DSURFACE_DESC Desc;
			pRenderTargetImpl->GetProxyInterface()->GetDesc(&Desc);
			probe::Log("SetRenderTarget %ux%u fmt=%d", Desc.Width, Desc.Height, Desc.Format);
		}

		// psobbvr: binding the main target (the real backbuffer, which
		// GetRenderTarget hands out, or the wide eye target itself) resumes
		// eye mirroring; binding anything else pauses it for the game's own
		// render-to-texture pass (the lobby's reflection maps, 1024x1024 +
		// 128x128 every frame when the GRAPHICCTRL quality enables them).
		IDirect3DSurface9 *const target9 = pRenderTargetImpl->GetProxyInterface();
		bool is_main_target =
			target9 != nullptr && target9 == stereo::wide_rt;
		if (!is_main_target)
		{
			IDirect3DSurface9 *backbuffer = nullptr;
			if (SUCCEEDED(ProxyInterface->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) &&
			    backbuffer != nullptr)
			{
				is_main_target = (target9 == backbuffer);
				backbuffer->Release();
			}
		}
		stereo::paused = !is_main_target;
		// psobbvr: whatever is bound here is no longer our binding of the
		// wide target - the next BeginEye re-binds it.
		stereo::wide_bound = false;
		hr = ProxyInterface->SetRenderTarget(0, target9);
		if (FAILED(hr))
			return hr;
	}

	if (pNewZStencil != nullptr)
	{
		auto pNewZStencilImpl = static_cast<Direct3DSurface8 *>(pNewZStencil);
		hr = ProxyInterface->SetDepthStencilSurface(pNewZStencilImpl->GetProxyInterface());
		if (FAILED(hr))
			return hr;

		D3DSURFACE_DESC8 Desc = {};
		pNewZStencilImpl->GetDesc(&Desc);

		CurrentZBufferBitCount = GetDepthStencilBitCount(Desc.Format);

		ProxyInterface->SetRenderState(D3DRS_DEPTHBIAS, CalcDepthBias(CurrentZBiasRenderState, CurrentZBufferBitCount));
	}
	else
	{
		ProxyInterface->SetDepthStencilSurface(nullptr);
	}

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderTarget(IDirect3DSurface8 **ppRenderTarget)
{
	if (ppRenderTarget == nullptr)
		return D3DERR_INVALIDCALL;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetRenderTarget(0, &SurfaceInterface);
	if (FAILED(hr))
		return hr;

	// psobbvr: while draws are mirrored into the eye targets, the game must
	// keep believing it renders to the backbuffer - hand that back instead
	// of whichever eye target happens to be bound. The game captures this
	// around its reflection-map passes (lobby) and restores it afterwards;
	// SetRenderTarget recognizes the backbuffer as "restore the main
	// target" and resumes eye mirroring.
	if (SurfaceInterface == stereo::wide_rt)
	{
		IDirect3DSurface9 *backbuffer = nullptr;
		if (SUCCEEDED(ProxyInterface->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) &&
		    backbuffer != nullptr)
		{
			SurfaceInterface->Release();
			SurfaceInterface = backbuffer;  // carries GetBackBuffer's reference
		}
	}

	*ppRenderTarget = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetDepthStencilSurface(IDirect3DSurface8 **ppZStencilSurface)
{
	if (ppZStencilSurface == nullptr)
		return D3DERR_INVALIDCALL;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetDepthStencilSurface(&SurfaceInterface);
	if (FAILED(hr))
		return hr;

	// psobbvr: same illusion as GetRenderTarget - never hand out the eye
	// depth surface; the game's own depth-stencil is what it believes in.
	if (SurfaceInterface == stereo::wide_ds &&
	    stereo::game_ds != nullptr)
	{
		SurfaceInterface->Release();
		stereo::game_ds->AddRef();
		SurfaceInterface = stereo::game_ds;
	}

	*ppZStencilSurface = ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginScene()
{
	// psobbvr: game hooks, installed once on the first frame. Each is
	// unconditional and gated per call or per frame inside its module.
	// Null-guard on the game's texture-bind helper, a loading-screen crash
	// fix that applies with VR off too (psobbvr_texguard.hpp).
	texguard::Install();
	// psobbvr: combat-text world positions (psobbvr_textsnap.hpp).
	textsnap::Install();
	// psobbvr: per-eye placement of the name labels over other characters
	// (psobbvr_namelabel.hpp).
	namelabel::Install();
	// psobbvr: per-object screen-edge culling defeat (psobbvr_objvis.hpp).
	objvis::Install();
	// psobbvr: bullet-trail ribbon depth (psobbvr_trail.hpp).
	trail::Install();
	// psobbvr: particle-pool identity for the world-sprite route
	// (psobbvr_particlepool.hpp).
	particlepool::Install();
	// psobbvr: equipment draw bracket for the weapon-on-grip re-seat
	// (psobbvr_weapongrip.hpp).
	weapongrip::Install();
	// psobbvr: target-selection aim steering (psobbvr_targetaim.hpp).
	targetaim::Install();
	// psobbvr: gun bullet origin at the hand (psobbvr_gunfire.hpp).
	gunfire::Install();
	// psobbvr: the edit-box hook behind the VR keyboard's text-field
	// detection (psobbvr_vrkeyboard.hpp).
	vrkeyboard::Install();
	// psobbvr: technique-cast facing-chase suppression
	// (psobbvr_techcast.hpp; [vr] cast_facing_snap).
	techcast::Install();
	// psobbvr: the TakeHit hook behind the melee hit pulse and the hit
	// census (psobbvr_haptics.hpp).
	haptics::Install();
	// psobbvr: the camera takeover writes the head pose into the game's
	// camera object, after the developer tools' memory holds (the takeover
	// wins).
	gamecam::Apply();
	stereo::InvalidateEyeCache();  // the pose the eye views build from moved
	// psobbvr: head trim - recompute the character head point after Apply
	// so written_this_frame is this frame's (psobbvr_trim.hpp).
	trim::OnFrame();
	// psobbvr: part-identity census + hand markers (psobbvr_partmap.hpp);
	// also after Apply.
	partmap::OnFrame();
	// psobbvr: weapon-on-grip per-pass state (weapon slot pointer + this
	// frame's grip pose); after Apply so WorldFromTracking uses this
	// frame's takeover pose.
	weapongrip::OnFrame();
	// psobbvr: VR hands capture/replay per-pass state; after weapongrip
	// so the hand-bone stash validity flags are fresh.
	hands::OnFrame();
	// psobbvr: this frame's aim yaw for the target-selection facing swap;
	// after weapongrip (current_is_gun) and Apply (gaze/anchor).
	targetaim::OnFrame();
	// psobbvr: this frame's aim ray (game-world origin + forward) for the
	// gun-shot source override; same ordering as targetaim.
	gunfire::OnFrame();
	// psobbvr: technique-cast diagnostics (psobbvr_techcast.hpp); logs
	// only while the developer build's tech_trace is armed.
	techcast::OnFrame();
	// psobbvr: widen the game's culling FOV while the takeover drives
	// (psobbvr_cullfov.hpp); restores the original bytes when it stops.
	// Gameplay frames only: the patch also widens the projection the game
	// builds from its FOV, which only the takeover's per-eye substitution
	// masks - menu screens' 3D passes would be distorted.
	cullfov::OnFrame(gamecam::Enabled() && gamecam::DrivesView());
	// psobbvr: environmental-geometry draw-distance scale
	// (psobbvr_drawdist.hpp) - data-only float writes, applied whenever VR
	// is enabled (area loads read them on non-gameplay frames).
	drawdist::OnFrame();
	// psobbvr: controller -> game input mapping (psobbvr_controller.hpp).
	// Before movement::OnFrame so the right-stick turn-rate override is in
	// place when it refreshes the standing-rate immediate.
	controller::OnFrame(gamecam::Enabled() && gamecam::DrivesView());
	// psobbvr: swing-timing indicator state (psobbvr_swingind.hpp) - after
	// the controller, whose swing and hold state it reads.
	swingind::Update(gamecam::Enabled() && gamecam::DrivesView());
	// psobbvr: VR locomotion shaping - scaled turn speed + backwards walking
	// (psobbvr_movement.hpp); reverts to stock when the takeover stops.
	movement::OnFrame(gamecam::Enabled());
	// psobbvr: menu world-lock - each BeginScene pass re-arms the "2D goes
	// below the 3D" routing (the game renders two passes per tick and each
	// redraws its backdrop before its 3D content).
	stereo::OnBeginScenePass();
	return ProxyInterface->BeginScene();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndScene()
{
	// psobbvr: VR hands fallback for a pass without a 2D draw.
	hands::OnPassEnd(ProxyInterface);
	return ProxyInterface->EndScene();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
	probe::Log("Clear count=%u flags=0x%X color=0x%08X z=%.2f", Count, Flags, Color, Z);
	// psobbvr: a clear issued during a boxed 3D-UI pass (the
	// radar clears its box's depth for its private top-down camera) belongs
	// to the HUD texture when the quad layer is active. Not marked as
	// content - only draws make the quad show.
	if (stereo::WantsDuplication() && !resolution::passthrough &&
	    stereo::HudLayerActive() && stereo::GameViewportIsBoxed())
	{
		stereo::BindHudLayer(ProxyInterface, true, false);
		return ProxyInterface->Clear(Count, pRects, Flags, Color, Z, Stencil);
	}
	// psobbvr: mirror clears into both stereo eye targets.
	if (stereo::WantsDuplication() && !resolution::passthrough)
	{
		HRESULT hr = D3D_OK;
		stereo::Duplicate(ProxyInterface, [&](int) { hr = ProxyInterface->Clear(Count, pRects, Flags, Color, Z, Stencil); });
		return hr;
	}
	return ProxyInterface->Clear(Count, pRects, Flags, Color, Z, Stencil);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
	if (probe::enabled && pMatrix != nullptr &&
	    (State == D3DTS_VIEW || State == D3DTS_PROJECTION || State == D3DTS_WORLD))
		// psobbvr: ptr identifies the game's source slot (VIEW comes from
		// 0x00ACBF80); WORLD _41.._43 is each model part's world translation.
		probe::Log("SetTransform %s ptr=%p  _11=%.4f _22=%.4f _33=%.4f _34=%.4f _41=%.2f _42=%.2f _43=%.2f _44=%.4f",
			State == D3DTS_VIEW ? "VIEW" : State == D3DTS_PROJECTION ? "PROJ" : "WORLD", pMatrix,
			pMatrix->_11, pMatrix->_22, pMatrix->_33, pMatrix->_34,
			pMatrix->_41, pMatrix->_42, pMatrix->_43, pMatrix->_44);

	// psobbvr: head trim - test each world placement against the character's
	// head sphere; the world-space draw paths consult the result.
	D3DMATRIX weapon_reseat;
	if (State == D3DTS_WORLD && pMatrix != nullptr && !resolution::passthrough)
	{
		// psobbvr: Boxed = the minimap radar, whose player arrows must not
		// read as our own body (psobbvr_partmap.hpp arm hide).
		const bool Boxed = stereo::GameViewportIsBoxed();
		trim::OnWorldTransform(*pMatrix, Boxed);
		// psobbvr: arm hide, head height and the part census - full matrix
		// + the game's call site (psobbvr_partmap.hpp).
		partmap::OnWorldTransform(*pMatrix, _ReturnAddress(), Boxed);
		// psobbvr: while the weapon object's bracketed draw runs, re-seat
		// its part matrices onto the controller grip pose
		// (psobbvr_weapongrip.hpp). Boxed radar/menu passes are passed in
		// too (twin-split learning needs those matrices) but keep the
		// game's matrices.
		if (weapongrip::SubstituteWorld(*pMatrix, weapon_reseat, Boxed))
			pMatrix = &weapon_reseat;
	}

	// psobbvr: capture the game's own scene view (before substitution),
	// which its CPU code projects world sprites with, and its last
	// perspective projection; both feed the sprite re-projection. Only the
	// sets from its source slots count: UI passes set other views, and the
	// minimap radar writes its private top-down camera through the same
	// slots while its boxed corner viewport is bound.
	if (State == D3DTS_VIEW && !resolution::passthrough &&
	    reinterpret_cast<uintptr_t>(pMatrix) == gamecam::GAME_VIEW_SLOT_ADDR &&
	    !stereo::GameViewportIsBoxed()) {
		stereo::cpu_view = *pMatrix;
		stereo::cpu_view_valid = true;
	}
	if (State == D3DTS_PROJECTION && !resolution::passthrough &&
	    reinterpret_cast<uintptr_t>(pMatrix) == gamecam::GAME_PROJ_SLOT_ADDR &&
	    stereo::IsPerspective(*pMatrix) &&
	    !stereo::GameViewportIsBoxed()) {
		stereo::last_persp_proj = *pMatrix;
		stereo::last_persp_proj_valid = true;
	}

	// psobbvr: camera takeover - when the game sets its per-frame view from
	// its own slot (0x00ACBF80), swap in the exact inverse of the head
	// pose. The game builds that matrix from camera fields it smooths after
	// our write, so substituting here keeps the view exact (and carries
	// head roll, which the game's look-at cannot). The radar's boxed
	// writes keep its own camera.
	D3DMATRIX takeover_view;
	if (State == D3DTS_VIEW && !resolution::passthrough &&
	    !stereo::GameViewportIsBoxed() &&
	    gamecam::SubstituteGameView(pMatrix, takeover_view))
		pMatrix = &takeover_view;

	// psobbvr: remember the game's view matrix; stereo overrides the device's
	// actual view state per eye per draw, so this stored copy is the truth.
	// The overlay's own transform sets (during passthrough) are not the game's.
	if (State == D3DTS_VIEW && pMatrix != nullptr && !resolution::passthrough)
	{
		stereo::game_view = *pMatrix;
		stereo::InvalidateEyeCache();
	}

	// psobbvr: same for the projection - with VR running, perspective passes
	// get the headset's projection instead, so the game's own is kept here.
	if (State == D3DTS_PROJECTION && pMatrix != nullptr && !resolution::passthrough)
	{
		stereo::game_proj = *pMatrix;
		stereo::InvalidateEyeCache();
	}

	return ProxyInterface->SetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
	// psobbvr: while stereo runs, the device's view state holds a per-eye
	// offset matrix; report the game's own view matrix instead.
	if (State == D3DTS_VIEW && pMatrix != nullptr && stereo::enabled && !resolution::passthrough)
	{
		*pMatrix = stereo::game_view;
		return D3D_OK;
	}
	// psobbvr: likewise the projection may hold a per-eye VR frustum.
	if (State == D3DTS_PROJECTION && pMatrix != nullptr && stereo::VrDrives() && !resolution::passthrough)
	{
		*pMatrix = stereo::game_proj;
		return D3D_OK;
	}
	return ProxyInterface->GetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
	return ProxyInterface->MultiplyTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetViewport(const D3DVIEWPORT8 *pViewport)
{
	probe::Log("SetViewport %u,%u %ux%u z=%.2f..%.2f",
		pViewport->X, pViewport->Y, pViewport->Width, pViewport->Height, pViewport->MinZ, pViewport->MaxZ);

	// psobbvr: the game expresses viewports in its hardcoded 640x480 space;
	// scale them into the real backbuffer. Remember the unscaled one so
	// GetViewport keeps the illusion. Our overlay's calls arrive in real
	// backbuffer pixels already (passthrough) and must not be scaled or
	// recorded as the game's viewport.
	D3DVIEWPORT8 ScaledViewport;
	if (resolution::ScalingActive() && pViewport != nullptr)
	{
		resolution::last_game_viewport = *reinterpret_cast<const D3DVIEWPORT9 *>(pViewport);
		// psobbvr: menu-open detection for controller stick nav - latches
		// on the field menu's shrunken scene viewport.
		stereo::NoteGameViewport(resolution::last_game_viewport);
		ScaledViewport = *pViewport;
		ScaledViewport.X = (DWORD)(pViewport->X * resolution::scale_x + 0.5f);
		ScaledViewport.Y = (DWORD)(pViewport->Y * resolution::scale_y + 0.5f);
		ScaledViewport.Width = (DWORD)(pViewport->Width * resolution::scale_x + 0.5f);
		ScaledViewport.Height = (DWORD)(pViewport->Height * resolution::scale_y + 0.5f);
		pViewport = &ScaledViewport;
	}

	D3DVIEWPORT8 ClampedViewport;
	IDirect3DSurface9 *pCurrentRenderTarget = nullptr;
	if (SUCCEEDED(ProxyInterface->GetRenderTarget(0, &pCurrentRenderTarget)))
	{
		D3DSURFACE_DESC Desc;
		pCurrentRenderTarget->GetDesc(&Desc);

		pCurrentRenderTarget->Release();

		if (pViewport->X >= Desc.Width || pViewport->Y >= Desc.Height)
			return D3DERR_INVALIDCALL;

		// psobbvr: clamp oversized viewports to the render target instead of
		// rejecting them (upstream rejected). The game's widescreen-aware
		// title-screen pass sizes a viewport from the window client area,
		// which after our game-space scaling can exceed the target; a reject
		// would leave a stale viewport for those draws.
		if (pViewport->Y + pViewport->Height > Desc.Height || pViewport->X + pViewport->Width > Desc.Width)
		{
			ClampedViewport = *pViewport;
			if (ClampedViewport.X + ClampedViewport.Width > Desc.Width)
				ClampedViewport.Width = Desc.Width - ClampedViewport.X;
			if (ClampedViewport.Y + ClampedViewport.Height > Desc.Height)
				ClampedViewport.Height = Desc.Height - ClampedViewport.Y;
			pViewport = &ClampedViewport;
		}
	}

	const HRESULT hr = ProxyInterface->SetViewport(pViewport);

	// psobbvr: remember the game's applied (post-scaling) viewport so stereo
	// can re-apply it after each eye-target bind (binding a render target
	// resets the viewport). The overlay's clip viewports (passthrough) are
	// not the game's.
	if (SUCCEEDED(hr) && pViewport != nullptr && !resolution::passthrough)
		stereo::current_viewport = *reinterpret_cast<const D3DVIEWPORT9 *>(pViewport);

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetViewport(D3DVIEWPORT8 *pViewport)
{
	// psobbvr: report the viewport in the game's 640x480 space. During
	// overlay passthrough, report the real one instead.
	if (resolution::ScalingActive() && pViewport != nullptr)
	{
		*pViewport = *reinterpret_cast<const D3DVIEWPORT8 *>(&resolution::last_game_viewport);
		return D3D_OK;
	}
	return ProxyInterface->GetViewport(pViewport);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetMaterial(const D3DMATERIAL8 *pMaterial)
{
	return ProxyInterface->SetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetMaterial(D3DMATERIAL8 *pMaterial)
{
	return ProxyInterface->GetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetLight(DWORD Index, const D3DLIGHT8 *pLight)
{
	if (pLight == nullptr)
		return D3DERR_INVALIDCALL;

	D3DLIGHT8 Light = *pLight;

	// Make spot light work more like it did in Direct3D 8
	if (Light.Type == D3DLIGHTTYPE::D3DLIGHT_SPOT)
	{
		// Theta must be in the range from 0 through the value specified by Phi
		if (Light.Theta <= Light.Phi)
		{
			Light.Theta /= 1.75f;
		}
	}

	return ProxyInterface->SetLight(Index, &Light);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLight(DWORD Index, D3DLIGHT8 *pLight)
{
	return ProxyInterface->GetLight(Index, pLight);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::LightEnable(DWORD Index, BOOL Enable)
{
	return ProxyInterface->LightEnable(Index, Enable);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetLightEnable(DWORD Index, BOOL *pEnable)
{
	return ProxyInterface->GetLightEnable(Index, pEnable);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipPlane(DWORD Index, const float *pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
		return D3DERR_INVALIDCALL;

	memcpy(StoredClipPlanes[Index], pPlane, sizeof(StoredClipPlanes[0]));
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipPlane(DWORD Index, float *pPlane)
{
	if (pPlane == nullptr || Index >= MAX_CLIP_PLANES)
		return D3DERR_INVALIDCALL;

	memcpy(pPlane, StoredClipPlanes[Index], sizeof(StoredClipPlanes[0]));
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
	HRESULT hr;

	// psobbvr: body-interior culling shadows the game's cull mode so the
	// per-draw override can restore it exactly (psobbvr_trim.hpp).
	if (State == D3DRS_CULLMODE)
	{
		trim::game_cullmode = Value;
		probe::Log("SetRenderState CULLMODE=%lu", Value);
	}

	switch (static_cast<DWORD>(State))
	{
	case D3DRS_ZVISIBLE:
	case D3DRS_PATCHSEGMENTS:
	case D3DRS_LINEPATTERN:
		return D3D_OK;
	case D3DRS_SOFTWAREVERTEXPROCESSING:
		// SWVP can be modified by this render state only on devices
		// created with the D3DCREATE_MIXED_VERTEXPROCESSING flag
		if (IsMixedVertexProcessingDevice)
			return ProxyInterface->SetSoftwareVertexProcessing(static_cast<BOOL>(Value));
		return D3D_OK;
	case D3DRS_EDGEANTIALIAS:
		return ProxyInterface->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, Value);
	case D3DRS_CLIPPLANEENABLE:
		hr = ProxyInterface->SetRenderState(State, Value);
		if (SUCCEEDED(hr))
			ClipPlaneRenderState = Value;
		return hr;
	case D3DRS_ZBIAS:
		CurrentZBiasRenderState = Value;
		Value = CalcDepthBias(Value, CurrentZBufferBitCount);
		State = D3DRS_DEPTHBIAS;
	default:
		return ProxyInterface->SetRenderState(State, Value);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
	if (pValue == nullptr)
		return D3DERR_INVALIDCALL;

	*pValue = 0;

	switch (static_cast<DWORD>(State))
	{
	case D3DRS_ZVISIBLE:
	case D3DRS_LINEPATTERN:
		*pValue = 0;
		return D3D_OK;
	case D3DRS_EDGEANTIALIAS:
		return ProxyInterface->GetRenderState(D3DRS_ANTIALIASEDLINEENABLE, pValue);
	case D3DRS_ZBIAS:
		*pValue = CurrentZBiasRenderState;
		return D3D_OK;
	case D3DRS_SOFTWAREVERTEXPROCESSING:
		*pValue = static_cast<DWORD>(ProxyInterface->GetSoftwareVertexProcessing());
		return D3D_OK;
	case D3DRS_PATCHSEGMENTS:
		*pValue = 1;
		return D3D_OK;
	default:
		return ProxyInterface->GetRenderState(State, pValue);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::BeginStateBlock()
{
	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	HRESULT hr = ProxyInterface->BeginStateBlock();

	if (SUCCEEDED(hr))
		IsRecordingState = true;

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::EndStateBlock(DWORD *pToken)
{
	if (pToken == nullptr)
		return D3DERR_INVALIDCALL;

	if (!IsRecordingState)
		return D3DERR_INVALIDCALL;

	HRESULT hr = ProxyInterface->EndStateBlock(reinterpret_cast<IDirect3DStateBlock9**>(pToken));

	if (SUCCEEDED(hr))
	{
		StateBlockTokens.insert(*pToken);
		IsRecordingState = false;
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ApplyStateBlock(DWORD Token)
{
	if (Token == 0)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	if (StateBlockTokens.find(Token) == StateBlockTokens.end())
		return D3D_OK;

	const HRESULT hr = reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Apply();
	// psobbvr: a state block restores render state behind the wrapper's
	// back (the overlay does this every frame) - the probe records the cull
	// mode it left, next to the wrapper's shadow (psobbvr_trim.hpp).
	if (probe::enabled)
	{
		DWORD c = 0;
		if (SUCCEEDED(ProxyInterface->GetRenderState(D3DRS_CULLMODE, &c)))
			probe::Log("ApplyStateBlock -> CULLMODE=%lu (shadow %lu)", c, trim::game_cullmode);
	}
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CaptureStateBlock(DWORD Token)
{
	if (Token == 0)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	if (StateBlockTokens.find(Token) == StateBlockTokens.end())
		return D3D_OK;

	return reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Capture();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteStateBlock(DWORD Token)
{
	if (Token == 0)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	if (StateBlockTokens.find(Token) == StateBlockTokens.end())
		return D3D_OK;

	reinterpret_cast<IDirect3DStateBlock9 *>(Token)->Release();

	StateBlockTokens.erase(Token);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD *pToken)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateStateBlock" << "(" << Type << ", " << pToken << ")' ..." << std::endl;
#endif

	if (pToken == nullptr)
		return D3DERR_INVALIDCALL;

	if (IsRecordingState)
		return D3DERR_INVALIDCALL;

	HRESULT hr = ProxyInterface->CreateStateBlock(Type, reinterpret_cast<IDirect3DStateBlock9 **>(pToken));

	if (SUCCEEDED(hr))
		StateBlockTokens.insert(*pToken);

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetClipStatus(const D3DCLIPSTATUS8 *pClipStatus)
{
	return ProxyInterface->SetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetClipStatus(D3DCLIPSTATUS8 *pClipStatus)
{
	return ProxyInterface->GetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTexture(DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
	if (ppTexture == nullptr)
		return D3DERR_INVALIDCALL;

	*ppTexture = nullptr;

	IDirect3DBaseTexture9 *BaseTextureInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetTexture(Stage, &BaseTextureInterface);
	if (FAILED(hr))
		return hr;

	if (BaseTextureInterface != nullptr)
	{
		IDirect3DTexture9 *TextureInterface = nullptr;
		IDirect3DCubeTexture9 *CubeTextureInterface = nullptr;
		IDirect3DVolumeTexture9 *VolumeTextureInterface = nullptr;

		switch (BaseTextureInterface->GetType())
		{
		case D3DRTYPE_TEXTURE:
			BaseTextureInterface->QueryInterface(IID_PPV_ARGS(&TextureInterface));
			*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DTexture8>(TextureInterface);
			BaseTextureInterface->Release();
			break;
		case D3DRTYPE_VOLUMETEXTURE:
			BaseTextureInterface->QueryInterface(IID_PPV_ARGS(&VolumeTextureInterface));
			*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DVolumeTexture8>(VolumeTextureInterface);
			BaseTextureInterface->Release();
			break;
		case D3DRTYPE_CUBETEXTURE:
			BaseTextureInterface->QueryInterface(IID_PPV_ARGS(&CubeTextureInterface));
			*ppTexture = ProxyAddressLookupTable->FindAddress<Direct3DCubeTexture8>(CubeTextureInterface);
			BaseTextureInterface->Release();
			break;
		default:
			BaseTextureInterface->Release();
			return D3DERR_INVALIDCALL;
		}
	}

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTexture(DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
	// psobbvr: head trim tracks the stage-0 binding (identity only).
	trim::OnSetTexture(Stage, pTexture);
	if (pTexture == nullptr)
		return ProxyInterface->SetTexture(Stage, nullptr);

	IDirect3DBaseTexture9 *BaseTextureInterface;

	switch (pTexture->GetType())
	{
	case D3DRTYPE_TEXTURE:
		BaseTextureInterface = static_cast<Direct3DTexture8 *>(pTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_VOLUMETEXTURE:
		BaseTextureInterface = static_cast<Direct3DVolumeTexture8 *>(pTexture)->GetProxyInterface();
		break;
	case D3DRTYPE_CUBETEXTURE:
		BaseTextureInterface = static_cast<Direct3DCubeTexture8 *>(pTexture)->GetProxyInterface();
		break;
	default:
		return D3DERR_INVALIDCALL;
	}

	return ProxyInterface->SetTexture(Stage, BaseTextureInterface);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
	switch (static_cast<DWORD>(Type))
	{
	case D3DTSS_ADDRESSU:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_ADDRESSU, pValue);
	case D3DTSS_ADDRESSV:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_ADDRESSV, pValue);
	case D3DTSS_ADDRESSW:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_ADDRESSW, pValue);
	case D3DTSS_BORDERCOLOR:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_BORDERCOLOR, pValue);
	case D3DTSS_MAGFILTER:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MAGFILTER, pValue);
	case D3DTSS_MINFILTER:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MINFILTER, pValue);
	case D3DTSS_MIPFILTER:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MIPFILTER, pValue);
	case D3DTSS_MIPMAPLODBIAS:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MIPMAPLODBIAS, pValue);
	case D3DTSS_MAXMIPLEVEL:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MAXMIPLEVEL, pValue);
	case D3DTSS_MAXANISOTROPY:
		return ProxyInterface->GetSamplerState(Stage, D3DSAMP_MAXANISOTROPY, pValue);
	default:
		return ProxyInterface->GetTextureStageState(Stage, Type, pValue);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	switch (static_cast<DWORD>(Type))
	{
	case D3DTSS_ADDRESSU:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_ADDRESSU, Value);
	case D3DTSS_ADDRESSV:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_ADDRESSV, Value);
	case D3DTSS_ADDRESSW:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_ADDRESSW, Value);
	case D3DTSS_BORDERCOLOR:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_BORDERCOLOR, Value);
	case D3DTSS_MAGFILTER:
		if (Value == D3DTEXF_FLATCUBIC || Value == D3DTEXF_GAUSSIANCUBIC)
			Value = D3DTEXF_LINEAR;
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MAGFILTER, Value);
	case D3DTSS_MINFILTER:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MINFILTER, Value);
	case D3DTSS_MIPFILTER:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MIPFILTER, Value);
	case D3DTSS_MIPMAPLODBIAS:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MIPMAPLODBIAS, Value);
	case D3DTSS_MAXMIPLEVEL:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MAXMIPLEVEL, Value);
	case D3DTSS_MAXANISOTROPY:
		return ProxyInterface->SetSamplerState(Stage, D3DSAMP_MAXANISOTROPY, Value);
	default:
		return ProxyInterface->SetTextureStageState(Stage, Type, Value);
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ValidateDevice(DWORD *pNumPasses)
{
	return ProxyInterface->ValidateDevice(pNumPasses);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetInfo(DWORD DevInfoID, void *pDevInfoStruct, DWORD DevInfoStructSize)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetInfo" << "(" << this << ", " << DevInfoID << ", " << pDevInfoStruct << ", " << DevInfoStructSize << ")' ..." << std::endl;
#endif

	if (pDevInfoStruct == nullptr || DevInfoStructSize == 0)
		return D3DERR_INVALIDCALL;

	HRESULT hr;
	IDirect3DQuery9 *pQuery = nullptr;

	switch (DevInfoID)
	{
		case 0:
		case D3DDEVINFOID_TEXTUREMANAGER:
		case D3DDEVINFOID_D3DTEXTUREMANAGER:
		case D3DDEVINFOID_TEXTURING:
			return E_FAIL; // Unsupported query IDs

		case D3DDEVINFOID_VCACHE:
			hr = ProxyInterface->CreateQuery(D3DQUERYTYPE_VCACHE, &pQuery);

			if (FAILED(hr))
			{
				if (DevInfoStructSize != sizeof(D3DDEVINFO_VCACHE))
					return D3DERR_INVALIDCALL;

				// The contents of pDevInfoStruct are zeroed before return
				memset(pDevInfoStruct, 0, sizeof(D3DDEVINFO_VCACHE));
				return S_FALSE;
			}

			break;

		case D3DDEVINFOID_RESOURCEMANAGER:
			hr = ProxyInterface->CreateQuery(D3DQUERYTYPE_RESOURCEMANAGER, &pQuery);
			break;

		case D3DDEVINFOID_VERTEXSTATS:
			hr = ProxyInterface->CreateQuery(D3DQUERYTYPE_VERTEXSTATS, &pQuery);
			break;

		default: // D3DDEVINFOID_UNKNOWN
			return E_FAIL;
	}

	if ((FAILED(hr)))
	{
		if (hr == D3DERR_NOTAVAILABLE)
		{
			return E_FAIL;
		}
		else
		{
			return S_FALSE;
		}
	}

	if (pQuery != nullptr)
	{
		pQuery->Issue(D3DISSUE_END);
		hr = pQuery->GetData(pDevInfoStruct, DevInfoStructSize, D3DGETDATA_FLUSH);

		pQuery->Release();
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries)
{
	if (pEntries == nullptr)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->SetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries)
{
	if (pEntries == nullptr)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->GetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetCurrentTexturePalette(UINT PaletteNumber)
{
	if (!IsPaletteSupported)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->SetCurrentTexturePalette(PaletteNumber);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetCurrentTexturePalette(UINT *pPaletteNumber)
{
	if (!IsPaletteSupported)
		return D3DERR_INVALIDCALL;

	return ProxyInterface->GetCurrentTexturePalette(pPaletteNumber);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
	// psobbvr: VR hands at the grips - told about every draw BEFORE it
	// runs; they render at the first 2D (RHW) draw of the pass, once the
	// 3D scene is complete (psobbvr_hands.hpp).
	hands::MaybeDraw(ProxyInterface, !resolution::IsRhwFvf(probe::current_fvf));
	probe::Log("DrawPrimitive type=%d prims=%u fvf=0x%X", PrimitiveType, PrimitiveCount, probe::current_fvf);
	// psobbvr: warp-tunnel gate - count perspective scene draws per
	// present, before any suppression (a trimmed head strip is still scene
	// evidence). See the warp block in psobbvr_stereo.hpp.
	if (!resolution::IsRhwFvf(probe::current_fvf) && stereo::WantsDuplication() &&
	    !resolution::passthrough)
		stereo::NoteSceneDraw();
	// psobbvr: first-person head trim - world-space draws only (RHW draws
	// never read WORLD, and must not reach SuppressNow, which learns).
	// Re-seated weapon draws are exempt BEFORE the call: a weapon raised
	// to the face must not vanish, nor teach trim its textures.
	if (!resolution::IsRhwFvf(probe::current_fvf) && !resolution::passthrough &&
	    !weapongrip::ExemptingDraws() && trim::SuppressNow())
		return D3D_OK;
	// psobbvr: first-person arm hide: the first N draws of the player's
	// body run are the arms+hands (psobbvr_partmap.hpp).
	if (!resolution::IsRhwFvf(probe::current_fvf) && !resolution::passthrough && partmap::HideArmDrawNow())
		return D3D_OK;
	// psobbvr: menu world-lock - on non-gameplay frames a
	// perspective draw flips this pass to "3D seen" (later 2D routes above
	// the projection layer); the window-pixel letterbox pass is skipped
	// (no VR contribution; it would pollute the alpha channel the menu
	// projection layer composites by).
	if (!resolution::IsRhwFvf(probe::current_fvf) && stereo::WantsDuplication() &&
	    !resolution::passthrough && stereo::MenuNoteNonRhwDraw())
		return D3D_OK;
	// psobbvr: no body-interior culling on vertex-buffer draws (see
	// psobbvr_trim.hpp): the player body is only ever drawn from CPU-built
	// vertex arrays (the UP paths), while map pieces come from vertex
	// buffers, and a map piece whose placement pivot sits under the feet
	// passes the position-only body test.
	trim::ProbeCull(ProxyInterface, false);
	ApplyClipPlanes();
	// psobbvr: with the HUD quad layer active, boxed 3D-UI passes (radar)
	// draw once into the HUD texture (RHW vertex-buffer draws would land
	// here unscaled; the game issues none). Menu world-lock shares the
	// routing (BindHudLayer picks the below/above menu target there).
	if (stereo::WantsDuplication() && !resolution::passthrough &&
	    (stereo::HudLayerActive() || stereo::MenuLockActive()) &&
	    (resolution::IsRhwFvf(probe::current_fvf) || stereo::GameViewportIsBoxed() ||
	     stereo::MenuFlatten3dNow()))
	{
		stereo::BindHudLayer(ProxyInterface, !resolution::IsRhwFvf(probe::current_fvf), true);
		stereo::ApplyHudLayerAlpha(ProxyInterface);
		ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		stereo::RestoreHudLayerAlpha(ProxyInterface);
	}
	else if (stereo::WantsDuplication() && !resolution::passthrough)
	{
		// psobbvr: menu world-lock - non-RHW draws land in the alpha-
		// composited menu projection layer; force real coverage (the
		// char-select character writes alpha 0).
		const bool MenuEyeAlpha = !resolution::IsRhwFvf(probe::current_fvf) && stereo::MenuLockActive();
		if (MenuEyeAlpha)
		{
			stereo::NoteMenu3dRectUnknown();
			stereo::ApplyCoverageAlpha(ProxyInterface);
		}
		stereo::Duplicate(ProxyInterface, [&](int) { ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); },
		                  resolution::IsRhwFvf(probe::current_fvf));
		if (MenuEyeAlpha)
			stereo::RestoreHudLayerAlpha(ProxyInterface);
	}
	else
		ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
	// psobbvr: VR hands at the grips - told about every draw BEFORE it
	// runs; they render at the first 2D (RHW) draw of the pass, once the
	// 3D scene is complete (psobbvr_hands.hpp).
	hands::MaybeDraw(ProxyInterface, !resolution::IsRhwFvf(probe::current_fvf));
	probe::Log("DrawIndexedPrimitive type=%d prims=%u verts=%u fvf=0x%X", PrimitiveType, PrimitiveCount, NumVertices, probe::current_fvf);
	// psobbvr: warp-tunnel gate - same scene-draw count as DrawPrimitive.
	if (!resolution::IsRhwFvf(probe::current_fvf) && stereo::WantsDuplication() &&
	    !resolution::passthrough)
		stereo::NoteSceneDraw();
	// psobbvr: first-person head trim (see DrawPrimitive).
	if (!resolution::IsRhwFvf(probe::current_fvf) && !resolution::passthrough &&
	    !weapongrip::ExemptingDraws() && trim::SuppressNow())
		return D3D_OK;
	// psobbvr: arm hide (see DrawPrimitive).
	if (!resolution::IsRhwFvf(probe::current_fvf) && !resolution::passthrough && partmap::HideArmDrawNow())
		return D3D_OK;
	// psobbvr: menu world-lock - same 3D-note/letterbox-skip as DrawPrimitive.
	if (!resolution::IsRhwFvf(probe::current_fvf) && stereo::WantsDuplication() &&
	    !resolution::passthrough && stereo::MenuNoteNonRhwDraw())
		return D3D_OK;
	// psobbvr: no body-interior culling on vertex-buffer draws (see
	// psobbvr_trim.hpp): the player body is only ever drawn from CPU-built
	// vertex arrays (the UP paths), while map pieces come from vertex
	// buffers, and a map piece whose placement pivot sits under the feet
	// passes the position-only body test.
	trim::ProbeCull(ProxyInterface, false);
	ApplyClipPlanes();
	// psobbvr: same HUD-layer routing as DrawPrimitive.
	if (stereo::WantsDuplication() && !resolution::passthrough &&
	    (stereo::HudLayerActive() || stereo::MenuLockActive()) &&
	    (resolution::IsRhwFvf(probe::current_fvf) || stereo::GameViewportIsBoxed() ||
	     stereo::MenuFlatten3dNow()))
	{
		stereo::BindHudLayer(ProxyInterface, !resolution::IsRhwFvf(probe::current_fvf), true);
		stereo::ApplyHudLayerAlpha(ProxyInterface);
		ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
		stereo::RestoreHudLayerAlpha(ProxyInterface);
	}
	else if (stereo::WantsDuplication() && !resolution::passthrough)
	{
		// psobbvr: menu world-lock - same coverage forcing as DrawPrimitive.
		const bool MenuEyeAlpha = !resolution::IsRhwFvf(probe::current_fvf) && stereo::MenuLockActive();
		if (MenuEyeAlpha)
		{
			stereo::NoteMenu3dRectUnknown();
			stereo::ApplyCoverageAlpha(ProxyInterface);
		}
		stereo::Duplicate(ProxyInterface, [&](int) { ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount); },
		                  resolution::IsRhwFvf(probe::current_fvf));
		if (MenuEyeAlpha)
			stereo::RestoreHudLayerAlpha(ProxyInterface);
	}
	else
		ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	// psobbvr: VR hands at the grips - told about every draw BEFORE it
	// runs; they render at the first 2D (RHW) draw of the pass, once the
	// 3D scene is complete (psobbvr_hands.hpp).
	hands::MaybeDraw(ProxyInterface, !resolution::IsRhwFvf(probe::current_fvf));
	if (probe::enabled && resolution::IsRhwFvf(probe::current_fvf) && pVertexStreamZeroData != nullptr)
		probe::Log("DrawPrimitiveUP type=%d prims=%u stride=%u fvf=0x%X rhw0=%.5f", PrimitiveType, PrimitiveCount, VertexStreamZeroStride, probe::current_fvf,
		           reinterpret_cast<const float*>(pVertexStreamZeroData)[3]);
	else
		probe::Log("DrawPrimitiveUP type=%d prims=%u stride=%u fvf=0x%X", PrimitiveType, PrimitiveCount, VertexStreamZeroStride, probe::current_fvf);
	const bool Rhw = resolution::IsRhwFvf(probe::current_fvf);
	// psobbvr: warp-tunnel gate - same scene-draw count as DrawPrimitive.
	if (!Rhw && stereo::WantsDuplication() && !resolution::passthrough)
		stereo::NoteSceneDraw();
	// psobbvr: menu world-lock - same 3D-note/letterbox-skip as DrawPrimitive
	// (this is the path the char-select character's strips take).
	if (!Rhw && stereo::WantsDuplication() && !resolution::passthrough &&
	    stereo::MenuNoteNonRhwDraw())
		return D3D_OK;
	// psobbvr: first-person head trim (see DrawPrimitive) - this is the path
	// the character's strips actually take.
	if (!Rhw && !resolution::passthrough && !weapongrip::ExemptingDraws() &&
	    trim::SuppressNow())
		return D3D_OK;
	// psobbvr: unarmed hand frame - at the body run's first draw, publish
	// bone[48] x global root as the hand bone when no weapon bracket is
	// firing, so the classifier below has a bone.
	if (!Rhw && !resolution::passthrough)
		hands::PublishArrayBone();
	// psobbvr: hand-band classifier - observes body-run draws with their
	// vertex data. Must run just before HideArmDrawNow: it borrows the
	// body-run index that call is about to assign, and must see the draws
	// the arm hide swallows.
	if (!Rhw && !resolution::passthrough)
		partmap::HandBandObserve(pVertexStreamZeroData, VertexStreamZeroStride,
		                         resolution::VertexCountForPrimitives(PrimitiveType, PrimitiveCount));
	// psobbvr: hand-geometry capture rides the same spot (same index
	// coupling with HideArmDrawNow below).
	if (!Rhw && !resolution::passthrough)
		hands::CaptureObserve(ProxyInterface, pVertexStreamZeroData,
		                      VertexStreamZeroStride,
		                      resolution::VertexCountForPrimitives(PrimitiveType, PrimitiveCount),
		                      PrimitiveType, PrimitiveCount, probe::current_fvf);
	// psobbvr: arm hide (see DrawPrimitive) - the body's strips
	// come through here.
	if (!Rhw && !resolution::passthrough && partmap::HideArmDrawNow())
		return D3D_OK;
	// psobbvr: body-interior culling (see psobbvr_trim.hpp).
	const bool BodyCull = !Rhw && !resolution::passthrough &&
	                      !weapongrip::ExemptingDraws() && trim::BodyCullActive();
	if (BodyCull)
		ProxyInterface->SetRenderState(D3DRS_CULLMODE, trim::BodyCullMode());
	trim::ProbeCull(ProxyInterface, BodyCull);
	const UINT VertexCount = Rhw ? resolution::VertexCountForPrimitives(PrimitiveType, PrimitiveCount) : 0;
	const bool Duplicating = stereo::WantsDuplication() && !resolution::passthrough;
	// psobbvr: burst-transition recognition - count the frame's
	// CPU-projected tunnel-sprite population on non-gameplay frames; the
	// frame-end latch reads it (see the burst block in psobbvr_stereo.hpp).
	if (Duplicating && Rhw && VertexCount != 0 && pVertexStreamZeroData != nullptr)
		stereo::NoteBurstCandidate(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride);
	// psobbvr: RHW draws split three ways under VR - CPU-projected world
	// sprites (sun/glares, told apart by their perspective rhw) get
	// re-projected per eye; UI goes to the virtual screen; without VR both
	// keep the plain resolution scaling.
	// psobbvr: combat text (damage/MISS/XP) is re-placed by the mod: the
	// game projects it through its internal camera, which lags the head
	// pose. Draws are identified by the textsnap bracket (see the
	// combat-text block in psobbvr_stereo.hpp).
	const bool CombatText = Duplicating && Rhw && VertexCount != 0 &&
	                        stereo::EffectReprojActive() &&
	                        stereo::IsCombatText(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride);
	// psobbvr: 2D draws from the game's window system are UI whatever
	// their depth (stereo::IsWindowSystemSite; [vr] ui_caller_rule).
	uintptr_t* const RetSlot = static_cast<uintptr_t*>(_AddressOfReturnAddress());
	const uintptr_t DrawSite = Rhw ? stereo::DrawSiteFromReturnSlot(RetSlot) : 0;
	// psobbvr: bullet-trail ribbon quads carry a constant depth; substitute
	// each vertex's real one so the world route below places them
	// (psobbvr_trail.hpp). Only ever the ribbon drawer's draws.
	if (Rhw && Duplicating && pVertexStreamZeroData != nullptr && VertexCount != 0)
		pVertexStreamZeroData = trail::OverrideDepths(pVertexStreamZeroData, VertexCount, VertexStreamZeroStride, DrawSite);
	const bool UiCaller = Rhw && vrmod::config.ui_caller_rule && stereo::IsWindowSystemSite(DrawSite);
	const bool WorldRhw = !CombatText && !UiCaller && Duplicating && Rhw && VertexCount != 0 &&
	                      stereo::EffectReprojActive() &&
	                      stereo::IsWorldRhw(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride, DrawSite);
	// psobbvr: with the HUD quad layer active, UI draws (and boxed 3D-UI
	// passes) render once into the HUD texture, which the backend
	// composites as its own layer at display rate (placement: [vr]
	// hud_lock). World sprites and the hidden sun family are excluded as on
	// the per-eye remap path. Menu world-lock shares the routing
	// (BindHudLayer picks the target).
	const bool HudLayer = Duplicating && !CombatText && !WorldRhw &&
	                      (stereo::HudLayerActive() || stereo::MenuLockActive()) &&
	                      (Rhw || stereo::GameViewportIsBoxed() || stereo::MenuFlatten3dNow());
	const bool HudRemap = !CombatText && !WorldRhw && !HudLayer && Duplicating && Rhw && stereo::HudRemapActive() &&
	                      pVertexStreamZeroData != nullptr && VertexCount != 0;
	const bool DropAlphaSprite = stereo::drop_alpha_sprite;
	stereo::drop_alpha_sprite = false;
	// psobbvr: an alpha-blended world sprite closer than alpha_sprite_floor
	// (a fog puff at the face) is skipped rather than painted onto the HUD.
	if (DropAlphaSprite)
		return D3D_OK;
	// psobbvr: sun/lens-flare hide ([vr] hide_sun) - the one sprite family
	// that cannot be world-locked; skipped entirely while VR drives.
	if (Duplicating && Rhw && VertexCount != 0 &&
	    stereo::IsHiddenEffect(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride))
		return D3D_OK;
	if (CombatText)
	{
		// World-anchored per eye; z test off - combat text is never occluded.
		ApplyClipPlanes();
		DWORD SavedZEnable = D3DZB_TRUE;
		ProxyInterface->GetRenderState(D3DRS_ZENABLE, &SavedZEnable);
		ProxyInterface->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		stereo::Duplicate(ProxyInterface, [&](int eye) {
			const void *Data = stereo::ProjectCombatTextVertices(eye, pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
			if (Data == nullptr)
				return;  // burst behind this eye
			ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, Data, VertexStreamZeroStride);
		}, false, false, true);
		ProxyInterface->SetRenderState(D3DRS_ZENABLE, SavedZEnable);
		return D3D_OK;
	}
	if (HudLayer)
	{
		const void *Data = pVertexStreamZeroData;
		if (Rhw && pVertexStreamZeroData != nullptr && VertexCount != 0)
			Data = stereo::ScaleHudLayerVertices(pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
		ApplyClipPlanes();
		// Menu world-lock: the draw's screen rect steers the
		// additive below-routing (see stereo::BindHudLayer).
		stereo::MenuRect Rect{};
		const stereo::MenuRect* RectPtr = nullptr;
		if (Rhw && pVertexStreamZeroData != nullptr && VertexCount != 0) {
			Rect = stereo::ComputeRhwRect(pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
			RectPtr = &Rect;
		}
		stereo::BindHudLayer(ProxyInterface, !Rhw, true, RectPtr);
		stereo::ApplyHudLayerAlpha(ProxyInterface);
		ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, Data, VertexStreamZeroStride);
		stereo::RestoreHudLayerAlpha(ProxyInterface);
		if (BodyCull)
			ProxyInterface->SetRenderState(D3DRS_CULLMODE, trim::game_cullmode);
		return D3D_OK;
	}
	if (!HudRemap && !WorldRhw && resolution::ScalingActive() && Rhw && pVertexStreamZeroData != nullptr && VertexCount != 0)
		pVertexStreamZeroData = resolution::ScaleRhwVertices(pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
	ApplyClipPlanes();
	if (Duplicating)
	{
		// psobbvr: menu world-lock - non-RHW draws (the char-select
		// character's path) land in the ALPHA-composited menu projection
		// layer; force real coverage (see DrawPrimitive), and note the
		// content's projected screen bounds for the flat-vs-floating 2D
		// routing (see stereo::NoteMenu3dRect).
		const bool MenuEyeAlpha = !Rhw && stereo::MenuLockActive();
		if (MenuEyeAlpha)
		{
			stereo::NoteMenu3dRect(ProxyInterface, pVertexStreamZeroData,
			                       stereo::VertexCountForPrimitives(PrimitiveType, PrimitiveCount),
			                       VertexStreamZeroStride);
			stereo::ApplyCoverageAlpha(ProxyInterface);
		}
		stereo::Duplicate(ProxyInterface, [&](int eye) {
			const void *Data = pVertexStreamZeroData;
			if (WorldRhw) {
				Data = stereo::ReprojectEffectVertices(eye, pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
				if (Data == nullptr)
					return;  // sprite behind this eye - skip
			} else if (HudRemap) {
				Data = stereo::RemapHudVertices(eye, pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
			}
			ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, Data, VertexStreamZeroStride);
		}, Rhw, HudRemap, WorldRhw);
		if (MenuEyeAlpha)
			stereo::RestoreHudLayerAlpha(ProxyInterface);
	}
	else
		ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	if (BodyCull)
		ProxyInterface->SetRenderState(D3DRS_CULLMODE, trim::game_cullmode);
	// psobbvr: bullet-trail cross ribbon (psobbvr_trail.hpp) - the same quad
	// on the perpendicular arm, issued right after the game's with its
	// states (winding already matched to the game's).
	if (const void *Companion = trail::TakeCompanion())
		DrawPrimitiveUP(PrimitiveType, PrimitiveCount, Companion, VertexStreamZeroStride);
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertexIndices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	// psobbvr: VR hands at the grips - told about every draw BEFORE it
	// runs; they render at the first 2D (RHW) draw of the pass, once the
	// 3D scene is complete (psobbvr_hands.hpp).
	hands::MaybeDraw(ProxyInterface, !resolution::IsRhwFvf(probe::current_fvf));
	if (probe::enabled && resolution::IsRhwFvf(probe::current_fvf) && pVertexStreamZeroData != nullptr)
		probe::Log("DrawIndexedPrimitiveUP type=%d prims=%u verts=%u stride=%u fvf=0x%X rhw0=%.5f", PrimitiveType, PrimitiveCount, NumVertexIndices, VertexStreamZeroStride, probe::current_fvf,
		           reinterpret_cast<const float*>(pVertexStreamZeroData)[3]);
	else
		probe::Log("DrawIndexedPrimitiveUP type=%d prims=%u verts=%u stride=%u fvf=0x%X", PrimitiveType, PrimitiveCount, NumVertexIndices, VertexStreamZeroStride, probe::current_fvf);
	// psobbvr: same screen-space handling as DrawPrimitiveUP.
	const bool Rhw = resolution::IsRhwFvf(probe::current_fvf);
	// psobbvr: warp-tunnel gate - same scene-draw count as DrawPrimitive.
	if (!Rhw && stereo::WantsDuplication() && !resolution::passthrough)
		stereo::NoteSceneDraw();
	// psobbvr: first-person head trim (see DrawPrimitive).
	if (!Rhw && !resolution::passthrough && !weapongrip::ExemptingDraws() &&
	    trim::SuppressNow())
		return D3D_OK;
	// psobbvr: arm hide (see DrawPrimitive).
	if (!Rhw && !resolution::passthrough && partmap::HideArmDrawNow())
		return D3D_OK;
	// psobbvr: menu world-lock - same 3D-note/letterbox-skip as DrawPrimitive.
	if (!Rhw && stereo::WantsDuplication() && !resolution::passthrough &&
	    stereo::MenuNoteNonRhwDraw())
		return D3D_OK;
	// psobbvr: body-interior culling (see psobbvr_trim.hpp).
	const bool BodyCull = !Rhw && !resolution::passthrough &&
	                      !weapongrip::ExemptingDraws() && trim::BodyCullActive();
	if (BodyCull)
		ProxyInterface->SetRenderState(D3DRS_CULLMODE, trim::BodyCullMode());
	trim::ProbeCull(ProxyInterface, BodyCull);
	const UINT VertexCount = Rhw ? MinVertexIndex + NumVertexIndices : 0;
	const bool Duplicating = stereo::WantsDuplication() && !resolution::passthrough;
	// psobbvr: burst-transition recognition - same as DrawPrimitiveUP.
	if (Duplicating && Rhw && VertexCount != 0 && pVertexStreamZeroData != nullptr)
		stereo::NoteBurstCandidate(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride);
	// psobbvr: combat-text world-anchoring + classification - same as
	// DrawPrimitiveUP.
	const bool CombatText = Duplicating && Rhw && VertexCount != 0 &&
	                        stereo::EffectReprojActive() &&
	                        stereo::IsCombatText(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride);
	// psobbvr: 2D draws from the game's window system are UI whatever
	// their depth (stereo::IsWindowSystemSite; [vr] ui_caller_rule).
	uintptr_t* const RetSlot = static_cast<uintptr_t*>(_AddressOfReturnAddress());
	const uintptr_t DrawSite = Rhw ? stereo::DrawSiteFromReturnSlot(RetSlot) : 0;
	const bool UiCaller = Rhw && vrmod::config.ui_caller_rule && stereo::IsWindowSystemSite(DrawSite);
	const bool WorldRhw = !CombatText && !UiCaller && Duplicating && Rhw && VertexCount != 0 &&
	                      stereo::EffectReprojActive() &&
	                      stereo::IsWorldRhw(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride, DrawSite);
	// psobbvr: HUD-layer routing - same as DrawPrimitiveUP.
	const bool HudLayer = Duplicating && !CombatText && !WorldRhw &&
	                      (stereo::HudLayerActive() || stereo::MenuLockActive()) &&
	                      (Rhw || stereo::GameViewportIsBoxed() || stereo::MenuFlatten3dNow());
	const bool HudRemap = !CombatText && !WorldRhw && !HudLayer && Duplicating && Rhw && stereo::HudRemapActive() &&
	                      pVertexStreamZeroData != nullptr && VertexCount != 0;
	const bool DropAlphaSprite = stereo::drop_alpha_sprite;
	stereo::drop_alpha_sprite = false;
	if (DropAlphaSprite)
		return D3D_OK;
	// psobbvr: sun/lens-flare hide - same as DrawPrimitiveUP.
	if (Duplicating && Rhw && VertexCount != 0 &&
	    stereo::IsHiddenEffect(ProxyInterface, pVertexStreamZeroData, VertexStreamZeroStride))
		return D3D_OK;
	if (CombatText)
	{
		// World-anchored per eye; z test off - combat text is never occluded.
		ApplyClipPlanes();
		DWORD SavedZEnable = D3DZB_TRUE;
		ProxyInterface->GetRenderState(D3DRS_ZENABLE, &SavedZEnable);
		ProxyInterface->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		stereo::Duplicate(ProxyInterface, [&](int eye) {
			const void *Data = stereo::ProjectCombatTextVertices(eye, pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
			if (Data == nullptr)
				return;  // burst behind this eye
			ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, Data, VertexStreamZeroStride);
		}, false, false, true);
		ProxyInterface->SetRenderState(D3DRS_ZENABLE, SavedZEnable);
		return D3D_OK;
	}
	if (HudLayer)
	{
		const void *Data = pVertexStreamZeroData;
		if (Rhw && pVertexStreamZeroData != nullptr && VertexCount != 0)
			Data = stereo::ScaleHudLayerVertices(pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
		ApplyClipPlanes();
		// Menu world-lock: same rect steering as DrawPrimitiveUP.
		stereo::MenuRect Rect{};
		const stereo::MenuRect* RectPtr = nullptr;
		if (Rhw && pVertexStreamZeroData != nullptr && VertexCount != 0) {
			Rect = stereo::ComputeRhwRect(pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
			RectPtr = &Rect;
		}
		stereo::BindHudLayer(ProxyInterface, !Rhw, true, RectPtr);
		stereo::ApplyHudLayerAlpha(ProxyInterface);
		ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, Data, VertexStreamZeroStride);
		stereo::RestoreHudLayerAlpha(ProxyInterface);
		if (BodyCull)
			ProxyInterface->SetRenderState(D3DRS_CULLMODE, trim::game_cullmode);
		return D3D_OK;
	}
	if (!HudRemap && !WorldRhw && resolution::ScalingActive() && Rhw && pVertexStreamZeroData != nullptr && VertexCount != 0)
		pVertexStreamZeroData = resolution::ScaleRhwVertices(pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
	ApplyClipPlanes();
	if (Duplicating)
	{
		// psobbvr: menu world-lock - same coverage forcing and 3D-bounds
		// note as DrawPrimitiveUP.
		const bool MenuEyeAlpha = !Rhw && stereo::MenuLockActive();
		if (MenuEyeAlpha)
		{
			stereo::NoteMenu3dRect(ProxyInterface, pVertexStreamZeroData,
			                       MinVertexIndex + NumVertexIndices,
			                       VertexStreamZeroStride);
			stereo::ApplyCoverageAlpha(ProxyInterface);
		}
		stereo::Duplicate(ProxyInterface, [&](int eye) {
			const void *Data = pVertexStreamZeroData;
			if (WorldRhw) {
				Data = stereo::ReprojectEffectVertices(eye, pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
				if (Data == nullptr)
					return;  // sprite behind this eye - skip
			} else if (HudRemap) {
				Data = stereo::RemapHudVertices(eye, pVertexStreamZeroData, VertexCount, VertexStreamZeroStride);
			}
			ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, Data, VertexStreamZeroStride);
		}, Rhw, HudRemap, WorldRhw);
		if (MenuEyeAlpha)
			stereo::RestoreHudLayerAlpha(ProxyInterface);
	}
	else
		ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	if (BodyCull)
		ProxyInterface->SetRenderState(D3DRS_CULLMODE, trim::game_cullmode);
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer8 *pDestBuffer, DWORD Flags)
{
	if (pDestBuffer == nullptr)
		return D3DERR_INVALIDCALL;

	Direct3DVertexBuffer8 *pDestBufferImpl = static_cast<Direct3DVertexBuffer8 *>(pDestBuffer);
	return ProxyInterface->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBufferImpl->GetProxyInterface(), nullptr, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreateVertexShader(const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
	UNREFERENCED_PARAMETER(Usage);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreateVertexShader" << "(" << this << ", " << pDeclaration << ", " << pFunction << ", " << pHandle << ", " << Usage << ")' ..." << std::endl;
#endif

	if (pDeclaration == nullptr || pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = 0;

	UINT ElementIndex = 0;
	const UINT ElementLimit = 32;
	std::string ConstantsCode;
	WORD Stream = 0, Offset = 0;
	DWORD VertexShaderInputs[ElementLimit];
	D3DVERTEXELEMENT9 VertexElements[ElementLimit];

#ifndef D3D8TO9NOLOG
	LOG << "> Translating vertex declaration ..." << std::endl;
#endif

	static const BYTE DeclTypes[][2] =
	{
		{ D3DDECLTYPE_FLOAT1, 4 },
		{ D3DDECLTYPE_FLOAT2, 8 },
		{ D3DDECLTYPE_FLOAT3, 12 },
		{ D3DDECLTYPE_FLOAT4, 16 },
		{ D3DDECLTYPE_D3DCOLOR, 4 },
		{ D3DDECLTYPE_UBYTE4, 4 },
		{ D3DDECLTYPE_SHORT2, 4 },
		{ D3DDECLTYPE_SHORT4, 8 },
		{ D3DDECLTYPE_UBYTE4N, 4 },
		{ D3DDECLTYPE_SHORT2N, 4 },
		{ D3DDECLTYPE_SHORT4N, 8 },
		{ D3DDECLTYPE_USHORT2N, 4 },
		{ D3DDECLTYPE_USHORT4N, 8 },
		{ D3DDECLTYPE_UDEC3, 6 },
		{ D3DDECLTYPE_DEC3N, 6 },
		{ D3DDECLTYPE_FLOAT16_2, 8 },
		{ D3DDECLTYPE_FLOAT16_4, 16 }
	};
	static const BYTE DeclAddressUsages[][2] =
	{
		{ D3DDECLUSAGE_POSITION, 0 },
		{ D3DDECLUSAGE_BLENDWEIGHT, 0 },
		{ D3DDECLUSAGE_BLENDINDICES, 0 },
		{ D3DDECLUSAGE_NORMAL, 0 },
		{ D3DDECLUSAGE_PSIZE, 0 },
		{ D3DDECLUSAGE_COLOR, 0 },
		{ D3DDECLUSAGE_COLOR, 1 },
		{ D3DDECLUSAGE_TEXCOORD, 0 },
		{ D3DDECLUSAGE_TEXCOORD, 1 },
		{ D3DDECLUSAGE_TEXCOORD, 2 },
		{ D3DDECLUSAGE_TEXCOORD, 3 },
		{ D3DDECLUSAGE_TEXCOORD, 4 },
		{ D3DDECLUSAGE_TEXCOORD, 5 },
		{ D3DDECLUSAGE_TEXCOORD, 6 },
		{ D3DDECLUSAGE_TEXCOORD, 7 },
		{ D3DDECLUSAGE_POSITION, 1 },
		{ D3DDECLUSAGE_NORMAL, 1 }
	};

	while (ElementIndex < ElementLimit)
	{
		const DWORD Token = *pDeclaration;
		const DWORD TokenType = (Token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		if (Token == D3DVSD_END())
		{
			break;
		}
		else if (TokenType == D3DVSD_TOKEN_STREAM)
		{
			Stream = static_cast<WORD>((Token & D3DVSD_STREAMNUMBERMASK) >> D3DVSD_STREAMNUMBERSHIFT);
			Offset = 0;
		}
		else if (TokenType == D3DVSD_TOKEN_STREAMDATA && !(Token & 0x10000000))
		{
			VertexElements[ElementIndex].Stream = Stream;
			VertexElements[ElementIndex].Offset = Offset;
			const DWORD type = (Token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;
			VertexElements[ElementIndex].Type = DeclTypes[type][0];
			Offset += DeclTypes[type][1];
			VertexElements[ElementIndex].Method = D3DDECLMETHOD_DEFAULT;
			const DWORD Address = (Token & D3DVSD_VERTEXREGMASK) >> D3DVSD_VERTEXREGSHIFT;
			VertexElements[ElementIndex].Usage = DeclAddressUsages[Address][0];
			VertexElements[ElementIndex].UsageIndex = DeclAddressUsages[Address][1];

			VertexShaderInputs[ElementIndex++] = Address;
		}
		else if (TokenType == D3DVSD_TOKEN_STREAMDATA && (Token & 0x10000000))
		{
			Offset += ((Token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT) * sizeof(DWORD);
		}
		else if (TokenType == D3DVSD_TOKEN_TESSELLATOR && !(Token & 0x10000000))
		{
			VertexElements[ElementIndex].Stream = Stream;
			VertexElements[ElementIndex].Offset = Offset;

			const DWORD UsageType = (Token & D3DVSD_VERTEXREGINMASK) >> D3DVSD_VERTEXREGINSHIFT;

			for (UINT r = 0; r < ElementIndex; ++r)
			{
				if (VertexElements[r].Usage == DeclAddressUsages[UsageType][0] && VertexElements[r].UsageIndex == DeclAddressUsages[UsageType][1])
				{
					VertexElements[ElementIndex].Stream = VertexElements[r].Stream;
					VertexElements[ElementIndex].Offset = VertexElements[r].Offset;
					break;
				}
			}

			VertexElements[ElementIndex].Type = D3DDECLTYPE_FLOAT3;
			VertexElements[ElementIndex].Method = D3DDECLMETHOD_CROSSUV;
			const DWORD Address = (Token & 0xF);
			VertexElements[ElementIndex].Usage = DeclAddressUsages[Address][0];
			VertexElements[ElementIndex].UsageIndex = DeclAddressUsages[Address][1];

			if (VertexElements[ElementIndex].Usage == D3DDECLUSAGE_BLENDINDICES)
			{
				VertexElements[ElementIndex].Method = D3DDECLMETHOD_DEFAULT;
			}

			VertexShaderInputs[ElementIndex++] = Address;
		}
		else if (TokenType == D3DVSD_TOKEN_TESSELLATOR && (Token & 0x10000000))
		{
			VertexElements[ElementIndex].Stream = 0;
			VertexElements[ElementIndex].Offset = 0;
			VertexElements[ElementIndex].Type = D3DDECLTYPE_UNUSED;
			VertexElements[ElementIndex].Method = D3DDECLMETHOD_UV;
			const DWORD Address = (Token & 0xF);
			VertexElements[ElementIndex].Usage = DeclAddressUsages[Address][0];
			VertexElements[ElementIndex].UsageIndex = DeclAddressUsages[Address][1];

			if (VertexElements[ElementIndex].Usage == D3DDECLUSAGE_BLENDINDICES)
			{
				VertexElements[ElementIndex].Method = D3DDECLMETHOD_DEFAULT;
			}

			VertexShaderInputs[ElementIndex++] = Address;
		}
		else if (TokenType == D3DVSD_TOKEN_CONSTMEM)
		{
			const DWORD RegisterCount = 4 * ((Token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT);
			DWORD Address = (Token & D3DVSD_CONSTADDRESSMASK) >> D3DVSD_CONSTADDRESSSHIFT;

			for (DWORD RegisterIndex = 0; RegisterIndex < RegisterCount; RegisterIndex += 4, ++Address)
			{
				ConstantsCode += "    def c" + std::to_string(Address) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 1])) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 2])) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 3])) + ", " +
					std::to_string(*reinterpret_cast<const float *>(&pDeclaration[RegisterIndex + 4])) + " /* vertex declaration constant */\n";
			}

			pDeclaration += RegisterCount;
		}
		else
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed because token type '" << TokenType << "' is not supported!" << std::endl;
#endif

			return D3DERR_INVALIDCALL;
		}

		++pDeclaration;
	}

	const D3DVERTEXELEMENT9 Terminator = D3DDECL_END();
	VertexElements[ElementIndex] = Terminator;

	HRESULT hr;
	VertexShaderInfo *ShaderInfo;

	if (pFunction != nullptr)
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Disassembling shader and translating assembly to Direct3D 9 compatible code ..." << std::endl;
#endif

		if (*pFunction < D3DVS_VERSION(1, 0) || *pFunction > D3DVS_VERSION(1, 1))
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed because of version mismatch ('" << std::showbase << std::hex << *pFunction << std::dec << std::noshowbase << "')! Only 'vs_1_x' shaders are supported." << std::endl;
#endif

			return D3DERR_INVALIDCALL;
		}

		ID3DXBuffer *Disassembly = nullptr, *Assembly = nullptr, *ErrorBuffer = nullptr;

		if (D3DXDisassembleShader != nullptr)
		{
			hr = D3DXDisassembleShader(pFunction, FALSE, nullptr, &Disassembly);
		}
		else
		{
			hr = D3DERR_INVALIDCALL;
		}

		if (FAILED(hr))
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed to disassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif

			return hr;
		}

		std::string SourceCode;
		{
			const char* raw = static_cast<const char*>(Disassembly->GetBufferPointer());
			size_t rawSize = Disassembly->GetBufferSize();

			SourceCode.reserve(rawSize);

			for (size_t i = 0; i < rawSize; ++i)
			{
				unsigned char c = static_cast<unsigned char>(raw[i]);

				bool isAllowed =
					(c == '\t') ||
					(c == '\n') ||
					(c == '\r') ||
					(c >= ' ' && c <= '~');

				if (!isAllowed)
					continue;

				SourceCode.push_back(static_cast<char>(c));
			}
		}

#ifndef D3D8TO9NOLOG
		LOG << "> Dumping original shader assembly:" << std::endl << std::endl << SourceCode << std::endl;
#endif

		const size_t VersionPosition = SourceCode.find("vs_1_");

		assert(VersionPosition != std::string::npos);

		if (SourceCode.at(VersionPosition + 5) == '0')
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Replacing version 'vs_1_0' with 'vs_1_1' ..." << std::endl;
#endif

			SourceCode.replace(VersionPosition, 6, "vs_1_1");
		}

		size_t DeclPosition = VersionPosition + 7;

		for (UINT k = 0; k < ElementIndex; k++)
		{
			std::string DeclCode = "    ";

			switch (VertexElements[k].Usage)
			{
			case D3DDECLUSAGE_POSITION:
				DeclCode += "dcl_position";
				break;
			case D3DDECLUSAGE_BLENDWEIGHT:
				DeclCode += "dcl_blendweight";
				break;
			case D3DDECLUSAGE_BLENDINDICES:
				DeclCode += "dcl_blendindices";
				break;
			case D3DDECLUSAGE_NORMAL:
				DeclCode += "dcl_normal";
				break;
			case D3DDECLUSAGE_PSIZE:
				DeclCode += "dcl_psize";
				break;
			case D3DDECLUSAGE_COLOR:
				DeclCode += "dcl_color";
				break;
			case D3DDECLUSAGE_TEXCOORD:
				DeclCode += "dcl_texcoord";
				break;
			}

			if (VertexElements[k].UsageIndex > 0)
			{
				DeclCode += std::to_string(VertexElements[k].UsageIndex);
			}

			DeclCode += " v" + std::to_string(VertexShaderInputs[k]) + '\n';

			SourceCode.insert(DeclPosition, DeclCode);
			DeclPosition += DeclCode.length();
		}

		#pragma region Fill registers with default value
		SourceCode.insert(DeclPosition, ConstantsCode);

		// Get number of arithmetic instructions used
		const size_t InstructionPosition = SourceCode.find("instruction");
		size_t InstructionCount = InstructionPosition > 2 && InstructionPosition < SourceCode.size() ? strtoul(SourceCode.substr(InstructionPosition - 4, 4).c_str(), nullptr, 10) : 0;

		for (size_t j = 0; j < 8; j++)
		{
			const std::string reg = "oT" + std::to_string(j);

			if (SourceCode.find(reg) != std::string::npos && InstructionCount < 128)
			{
				++InstructionCount;
				SourceCode.insert(DeclPosition + ConstantsCode.size(), "    mov " + reg + ", c0 /* initialize output register " + reg + " */\n");
			}
		}
		for (size_t j = 0; j < 2; j++)
		{
			const std::string reg = "oD" + std::to_string(j);

			if (SourceCode.find(reg) != std::string::npos && InstructionCount < 128)
			{
				++InstructionCount;
				SourceCode.insert(DeclPosition + ConstantsCode.size(), "    mov " + reg + ", c0 /* initialize output register " + reg + " */\n");
			}
		}
		for (size_t j = 0; j < 12; j++)
		{
			const std::string reg = "r" + std::to_string(j);

			if (SourceCode.find(reg) != std::string::npos && InstructionCount < 128)
			{
				++InstructionCount;
				SourceCode.insert(DeclPosition + ConstantsCode.size(), "    mov " + reg + ", c0 /* initialize register " + reg + " */\n");
			}
		}
		#pragma endregion

		SourceCode = std::regex_replace(SourceCode, std::regex("    \\/\\/ vs\\.1\\.1\\n((?! ).+\\n)+"), "");
		SourceCode = std::regex_replace(SourceCode, std::regex("([^\\n]\\n)[\\s]*#line [0123456789]+.*\\n"), "$1");
		SourceCode = std::regex_replace(SourceCode, std::regex("(oFog|oPts)\\.x"), "$1 /* removed swizzle */");
		SourceCode = std::regex_replace(SourceCode, std::regex("(add|sub|mul|min|max) (oFog|oPts), ([cr][0-9]+), (.+)\\n"), "$1 $2, $3.x /* added swizzle */, $4\n");
		SourceCode = std::regex_replace(SourceCode, std::regex("(add|sub|mul|min|max) (oFog|oPts), (.+), ([cr][0-9]+)\\n"), "$1 $2, $3, $4.x /* added swizzle */\n");
		SourceCode = std::regex_replace(SourceCode, std::regex("(mov|mad) (oFog|oPts)(.*), (-?)([crv][0-9]+(?![\\.0-9]))"), "$1 $2$3, $4$5.x /* select single component */");

		// Destination register cannot be the same as first source register for m*x* instructions.
		if (std::regex_search(SourceCode, std::regex("m.x.")))
		{
			// Check for unused register
			size_t r;
			for (r = 0; r < 12; r++)
			{
				if (SourceCode.find("r" + std::to_string(r)) == std::string::npos) break;
			}

			// Check if first source register is the same as the destination register
			for (size_t j = 0; j < 12; j++)
			{
				const std::string reg = "(m.x.) (r" + std::to_string(j) + "), ((-?)r" + std::to_string(j) + "([\\.xyzw]*))(?![0-9])";

				while (std::regex_search(SourceCode, std::regex(reg)))
				{
					// If there is enough remaining instructions and an unused register then update to use a temp register
					if (r < 12 && InstructionCount < 128)
					{
						++InstructionCount;
						SourceCode = std::regex_replace(SourceCode, std::regex(reg),
							"mov r" + std::to_string(r) + ", $2 /* added line */\n    $1 $2, $4r" + std::to_string(r) + "$5 /* changed $3 to r" + std::to_string(r) + " */",
							std::regex_constants::format_first_only);
					}
					// Disable line to prevent assembly error
					else
					{
						SourceCode = std::regex_replace(SourceCode, std::regex("(.*" + reg + ".*)"), "/*$1*/ /* disabled this line */");
						break;
					}
				}
			}
		}

		// Vertex shader must minimally write all four components (xyzw) of oPos output register. (fix error X5350)
		if (std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.")) && !std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos,")))
		{
			bool xReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[y|z|w]*x"));
			bool yReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[x|z|w]*y"));
			bool zReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[x|y|w]*z"));
			bool wReg = std::regex_search(SourceCode, std::regex("    ([a-z2-4]*) oPos\\.[x|y|z]*w"));
			if (!xReg || !yReg || !zReg || !wReg)
			{
				SourceCode = std::regex_replace(SourceCode, std::regex("    ([a-z2-4]*) (oPos\\.[x|y|z|w]*,) ([^\\n]*)\\n"), "    $1 oPos, $3 /* removed oPos swizzles */\n");
			}
		}

#ifndef D3D8TO9NOLOG
		LOG << "> Dumping translated shader assembly:" << std::endl << std::endl << SourceCode << std::endl;
#endif

		if (D3DXAssembleShader != nullptr)
		{
			hr = D3DXAssembleShader(SourceCode.data(), static_cast<UINT>(SourceCode.size()), nullptr, nullptr, D3DXASM_FLAGS, &Assembly, &ErrorBuffer);
		}
		else
		{
			hr = D3DERR_INVALIDCALL;
		}

		Disassembly->Release();

		if (FAILED(hr))
		{
			if (ErrorBuffer != nullptr)
			{
#ifndef D3D8TO9NOLOG
				LOG << "> Failed to reassemble shader:" << std::endl << std::endl << static_cast<const char *>(ErrorBuffer->GetBufferPointer()) << std::endl;
#endif
				ErrorBuffer->Release();
			}
			else
			{
#ifndef D3D8TO9NOLOG
				LOG << "> Failed to reassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
			}

			return hr;
		}

		ShaderInfo = new VertexShaderInfo();

		hr = ProxyInterface->CreateVertexShader(static_cast<const DWORD *>(Assembly->GetBufferPointer()), &ShaderInfo->Shader);

		Assembly->Release();
	}
	else
	{
		ShaderInfo = new VertexShaderInfo();
		ShaderInfo->Shader = nullptr;

		hr = D3D_OK;
	}

	if (SUCCEEDED(hr))
	{
		hr = ProxyInterface->CreateVertexDeclaration(VertexElements, &ShaderInfo->Declaration);

		if (SUCCEEDED(hr))
		{
			// Since 'Shader' is at least 8 byte aligned, we can safely shift it to right and end up not overwriting the top bit
			assert((reinterpret_cast<DWORD>(ShaderInfo) & 1) == 0);
			const DWORD ShaderMagic = reinterpret_cast<DWORD>(ShaderInfo) >> 1;

			*pHandle = ShaderMagic | 0x80000000;

			VertexShaderHandles.insert(*pHandle);
			VertexShaderAndDeclarationCount++;
			if (ShaderInfo->Shader)
			{
				VertexShaderAndDeclarationCount++;
			}
		}
		else
		{
#ifndef D3D8TO9NOLOG
			LOG << "> 'IDirect3DDevice9::CreateVertexDeclaration' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
			if (ShaderInfo->Shader != nullptr) 
			{
				ShaderInfo->Shader->Release();
			}
		}
	}
	else
	{
#ifndef D3D8TO9NOLOG
		LOG << "> 'IDirect3DDevice9::CreateVertexShader' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
	}

	if (FAILED(hr))
	{
		delete ShaderInfo;
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShader(DWORD Handle)
{
	HRESULT hr;

	if ((Handle & 0x80000000) == 0)
	{
		probe::current_fvf = Handle;
		ProxyInterface->SetVertexShader(nullptr);
		ProxyInterface->SetVertexDeclaration(nullptr);
		hr = ProxyInterface->SetFVF(Handle);

		CurrentVertexShaderHandle = 0;
	}
	else
	{
		const DWORD handleMagic = Handle << 1;
		VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(handleMagic);

		hr = ProxyInterface->SetVertexShader(ShaderInfo->Shader);
		ProxyInterface->SetVertexDeclaration(ShaderInfo->Declaration);

		if (SUCCEEDED(hr))
			CurrentVertexShaderHandle = Handle;
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShader(DWORD *pHandle)
{
	if (pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	if (CurrentVertexShaderHandle == 0)
	{
		return ProxyInterface->GetFVF(pHandle);
	}
	else
	{
		*pHandle = CurrentVertexShaderHandle;
		return D3D_OK;
	}
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeleteVertexShader(DWORD Handle)
{
	if ((Handle & 0x80000000) == 0)
		return D3DERR_INVALIDCALL;

	if (VertexShaderHandles.erase(Handle) == 0)
		return D3DERR_INVALIDCALL;

	if (CurrentVertexShaderHandle == Handle)
	{
		ProxyInterface->SetVertexShader(nullptr);
		ProxyInterface->SetVertexDeclaration(nullptr);
		CurrentVertexShaderHandle = 0;
	}

	const DWORD HandleMagic = Handle << 1;
	VertexShaderInfo *const ShaderInfo = reinterpret_cast<VertexShaderInfo *>(HandleMagic);

	if (ShaderInfo->Shader != nullptr) 
	{
		ShaderInfo->Shader->Release();
		VertexShaderAndDeclarationCount--;
	}
	if (ShaderInfo->Declaration != nullptr)
	{
		ShaderInfo->Declaration->Release();
		VertexShaderAndDeclarationCount--;
	}

	delete ShaderInfo;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetVertexShaderConstant(DWORD Register, const void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->SetVertexShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->GetVertexShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderDeclaration(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
	UNREFERENCED_PARAMETER(Handle);
	UNREFERENCED_PARAMETER(pData);
	UNREFERENCED_PARAMETER(pSizeOfData);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetVertexShaderDeclaration" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
	LOG << "> 'IDirect3DDevice8::GetVertexShaderDeclaration' is not implemented!" << std::endl;
#endif

	return D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetVertexShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetVertexShaderFunction" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
#endif

	if ((Handle & 0x80000000) == 0)
		return D3DERR_INVALIDCALL;

	const DWORD HandleMagic = Handle << 1;
	IDirect3DVertexShader9 *VertexShaderInterface = reinterpret_cast<VertexShaderInfo *>(HandleMagic)->Shader;

	if (VertexShaderInterface == nullptr)
		return D3DERR_INVALIDCALL;

#ifndef D3D8TO9NOLOG
	LOG << "> Returning translated shader byte code." << std::endl;
#endif

	return VertexShaderInterface->GetFunction(pData, reinterpret_cast<UINT *>(pSizeOfData));
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
	IDirect3DVertexBuffer9 *pStreamDataImpl = nullptr;
	if (pStreamData != nullptr)
		pStreamDataImpl = static_cast<Direct3DVertexBuffer8 *>(pStreamData)->GetProxyInterface();

	return ProxyInterface->SetStreamSource(StreamNumber, pStreamDataImpl, 0, Stride);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
	if (ppStreamData == nullptr)
		return D3DERR_INVALIDCALL;

	*ppStreamData = nullptr;

	UINT StreamOffset = 0;
	IDirect3DVertexBuffer9 *VertexBufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetStreamSource(StreamNumber, &VertexBufferInterface, &StreamOffset, pStride);
	if (FAILED(hr))
		return hr;

	if (VertexBufferInterface != nullptr)
		*ppStreamData = ProxyAddressLookupTable->FindAddress<Direct3DVertexBuffer8>(VertexBufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetIndices(IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
	if (BaseVertexIndex > 0x7FFFFFFF)
		return D3DERR_INVALIDCALL;

	IDirect3DIndexBuffer9 *pIndexDataImpl = nullptr;
	if (pIndexData != nullptr)
		pIndexDataImpl = static_cast<Direct3DIndexBuffer8 *>(pIndexData)->GetProxyInterface();

	const HRESULT hr = ProxyInterface->SetIndices(pIndexDataImpl);
	if (FAILED(hr))
		return hr;

	CurrentBaseVertexIndex = static_cast<INT>(BaseVertexIndex);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetIndices(IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
	if (ppIndexData == nullptr)
		return D3DERR_INVALIDCALL;

	*ppIndexData = nullptr;

	if (pBaseVertexIndex != nullptr)
		*pBaseVertexIndex = static_cast<UINT>(CurrentBaseVertexIndex);

	IDirect3DIndexBuffer9 *IntexBufferInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetIndices(&IntexBufferInterface);
	if (FAILED(hr))
		return hr;

	if (IntexBufferInterface != nullptr)
		*ppIndexData = ProxyAddressLookupTable->FindAddress<Direct3DIndexBuffer8>(IntexBufferInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::CreatePixelShader(const DWORD *pFunction, DWORD *pHandle)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::CreatePixelShader" << "(" << this << ", " << pFunction << ", " << pHandle << ")' ..." << std::endl;
#endif

	if (pFunction == nullptr || pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = 0;

#ifndef D3D8TO9NOLOG
	LOG << "> Disassembling shader and translating assembly to Direct3D 9 compatible code ..." << std::endl;
#endif

	if (*pFunction < D3DPS_VERSION(1, 0) || *pFunction > D3DPS_VERSION(1, 4))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Failed because of version mismatch ('" << std::showbase << std::hex << *pFunction << std::dec << std::noshowbase << "')! Only 'ps_1_x' shaders are supported." << std::endl;
#endif
		return D3DERR_INVALIDCALL;
	}

	ID3DXBuffer *Disassembly = nullptr, *Assembly = nullptr, *ErrorBuffer = nullptr;

	HRESULT hr = D3DERR_INVALIDCALL;

	if (D3DXDisassembleShader != nullptr)
		hr = D3DXDisassembleShader(pFunction, FALSE, nullptr, &Disassembly);

	if (FAILED(hr))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Failed to disassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
		return hr;
	}

	std::string SourceCode;
	{
		const char* raw = static_cast<const char*>(Disassembly->GetBufferPointer());
		size_t rawSize = Disassembly->GetBufferSize();

		SourceCode.reserve(rawSize);

		for (size_t i = 0; i < rawSize; ++i)
		{
			unsigned char c = static_cast<unsigned char>(raw[i]);

			bool isAllowed =
				(c == '\t') ||
				(c == '\n') ||
				(c == '\r') ||
				(c >= ' ' && c <= '~');

			if (!isAllowed)
				continue;

			SourceCode.push_back(static_cast<char>(c));
		}
	}

	const size_t VersionPosition = SourceCode.find("ps_1_");

	assert(VersionPosition != std::string::npos);

	if (SourceCode.at(VersionPosition + 5) == '0')
	{
#ifndef D3D8TO9NOLOG
		LOG << "> Replacing version 'ps_1_0' with 'ps_1_1' ..." << std::endl;
#endif

		SourceCode.replace(VersionPosition, 6, "ps_1_1");
	}

	// Get number of arithmetic instructions used
	const size_t ArithmeticPosition = SourceCode.find("arithmetic");
	size_t ArithmeticCount = ArithmeticPosition > 2 && ArithmeticPosition < SourceCode.size() ? strtoul(SourceCode.substr(ArithmeticPosition - 2, 2).c_str(), nullptr, 10) : 0;
	ArithmeticCount = (ArithmeticCount != 0) ? ArithmeticCount : 10;	// Default to 10

	// Remove lines when "    // ps.1.1" string is found and the next line does not start with a space
	SourceCode = std::regex_replace(SourceCode,
		std::regex("    \\/\\/ ps\\.1\\.[1-4]\\n((?! ).+\\n)+"),
		"");

	// Remove debug lines
	SourceCode = std::regex_replace(SourceCode,
		std::regex("([^\\n]\\n)[\\s]*#line [0123456789]+.*\\n"),
		"$1");

	// Fix '-' modifier for constant values when using 'add' arithmetic by changing it to use 'sub'
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(add)([_satxd248]*) (r[0-9][\\.wxyz]*), ((1-|)[crtv][0-9][\\.wxyz_abdis2]*), (-)(c[0-9][\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)(?![_\\.wxyz])"),
		"sub$2 $3, $4, $7$8 /* changed 'add' to 'sub' removed modifier $6 */");

	// Create temporary varables for ps_1_4
	std::string SourceCode14 = SourceCode;
	int ArithmeticCount14 = ArithmeticCount;

	// Fix modifiers for constant values by using any remaining arithmetic places to add an instruction to move the constant value to a temporary register
	while (std::regex_search(SourceCode, std::regex("-c[0-9]|c[0-9][\\.wxyz]*_")) && ArithmeticCount < 8)
	{
		// Make sure that the dest register is not already being used
		const std::string normalizedSourceCode =
			std::regex_replace(
				std::regex_replace(SourceCode,
					std::regex("1?-(c[0-9])[\\._a-z0-9]*"), "-$1"),    // Find negative modifiers
				std::regex("(c[0-9])[\\.wxyz]*_[a-z0-9]*"), "-$1");    // Find swizzle modifiers
		std::string tmpLine = "\n" + normalizedSourceCode + "\n";
		size_t start = tmpLine.substr(0, tmpLine.find("-c")).rfind("\n") + 1;
		tmpLine = tmpLine.substr(start, tmpLine.find("\n", start) - start);
		const std::string destReg = std::regex_replace(tmpLine, std::regex("[ \\+]+[a-z_\\.0-9]+ (r[0-9]).*-c[0-9].*"),"$1");
		const std::string sourceReg = std::regex_replace(tmpLine, std::regex("[ \\+]+[a-z_\\.0-9]+ r[0-9][\\._a-z0-9]*, (.*)-c[0-9](.*)"), "$1$2");
		if (sourceReg.find(destReg) != std::string::npos)
		{
			break;
		}

		// Replace one constant modifier using the dest register as a temporary register
		size_t SourceSize = SourceCode.size();
		SourceCode = std::regex_replace(SourceCode,
			std::regex("    (...)(_[_satxd248]*|) (r[0-9])([\\.wxyz]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?((1?-)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)|(1?-?)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]))(?![_\\.wxyz])"),
			"    mov $3$4, $10$11$14$15 /* added line */\n    $1$2 $3$4, $5$6$9$13$3$12$16 /* changed $10$11$14$15 to $3 */", std::regex_constants::format_first_only);
		// Replace one constant modifier on coissued commands using the dest register as a temporary register
		if (SourceSize == SourceCode.size())
		{
			SourceCode = std::regex_replace(SourceCode,
				std::regex("(    .*\\n)  \\+ (...)(_[_satxd248]*|) (r[0-9])([\\.wxyz]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?(1?-?[crtv][0-9][\\.wxyz_abdis2]*, )?((1?-)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)|(1?-?)(c[0-9])([\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]))(?![_\\.wxyz])"),
				"    mov $4$5, $11$12$15$16 /* added line */\n$1  + $2$3 $4$5, $6$7$10$14$4$13$17 /* changed $11$12$15$16 to $4 */", std::regex_constants::format_first_only);
		}

		if (SourceSize == SourceCode.size())
			break;

		ArithmeticCount++;
	}

	// Check if this should be converted to ps_1_4
	if (std::regex_search(SourceCode, std::regex("-c[0-9]|c[0-9][\\.wxyz]*_")) &&	// Check for modifiers on constants
		!std::regex_search(SourceCode, std::regex("tex[bcdmr]")) &&					// Verify unsupported instructions are not used
		std::regex_search(SourceCode, std::regex("ps_1_[0-3]")))					// Verify PixelShader is using version 1.0 to 1.3
	{
		bool ConvertError = false;
		bool RegisterUsed[7] = { false, false, false, false, false, false, true };

		struct MyStrings
		{
			std::string dest;
			std::string source;
		};

		std::vector<MyStrings> ReplaceReg;
		std::string NewSourceCode = "    ps_1_4 /* converted */\n";

		// Ensure at least one command will be above the phase marker
		bool PhaseMarkerSet = (ArithmeticCount14 >= 8);
		if (SourceCode14.find("def c") == std::string::npos && !PhaseMarkerSet)
		{
			for (size_t j = 0; j < 8; j++)
			{
				const std::string reg = "c" + std::to_string(j);

				if (SourceCode14.find(reg) == std::string::npos)
				{
					PhaseMarkerSet = true;
					NewSourceCode.append("    def " + reg + ", 0, 0, 0, 0 /* added line */\n");
					break;
				}
			}
		}

		// Update registers to use different numbers from textures
		size_t FirstReg = 0;
		for (size_t j = 0; j < 2; j++)
		{
			const std::string reg = "r" + std::to_string(j);

			if (SourceCode14.find(reg) != std::string::npos)
			{
				while (SourceCode14.find("t" + std::to_string(FirstReg)) != std::string::npos ||
					(SourceCode14.find("r" + std::to_string(FirstReg)) != std::string::npos && j != FirstReg))
				{
					FirstReg++;
				}
				SourceCode14 = std::regex_replace(SourceCode14, std::regex(reg), "r" + std::to_string(FirstReg));
				FirstReg++;
			}
		}

		// Set phase location
		size_t PhasePosition = NewSourceCode.length();
		size_t TexturePosition = 0;

		// Loop through each line
		size_t LinePosition = 1;
		std::string NewLine = SourceCode14;
		while (true)
		{
			// Get next line
			size_t tmpLinePos = SourceCode14.find("\n", LinePosition) + 1;
			if (tmpLinePos == std::string::npos || tmpLinePos < LinePosition)
			{
				break;
			}
			LinePosition = tmpLinePos;
			NewLine = SourceCode14.substr(LinePosition, SourceCode14.length());
			tmpLinePos = NewLine.find("\n");
			if (tmpLinePos != std::string::npos)
			{
				NewLine.resize(tmpLinePos);
			}

			// Skip 'ps_x_x' lines
			if (std::regex_search(NewLine, std::regex("ps_._.")))
			{
				// Do nothing
			}

			// Check for 'def' and add before 'phase' statement
			else if (NewLine.find("def c") != std::string::npos)
			{
				PhaseMarkerSet = true;
				const std::string tmpLine = NewLine + "\n";
				NewSourceCode.insert(PhasePosition, tmpLine);
				PhasePosition += tmpLine.length();
			}

			// Check for 'tex' and update to 'texld'
			else if (NewLine.find("tex t") != std::string::npos)
			{
				const std::string regNum = std::regex_replace(NewLine, std::regex(".*tex t([0-9]).*"), "$1");
				const std::string tmpLine = "    texld r" + regNum + ", t" + regNum + "\n";

				// Mark as a texture register and add 'texld' statement before or after the 'phase' statement
				const unsigned long Num = strtoul(regNum.c_str(), nullptr, 10);
				RegisterUsed[(Num < 6) ? Num : 6] = true;
				NewSourceCode.insert(PhasePosition, tmpLine);
				if (PhaseMarkerSet)
				{
					TexturePosition += tmpLine.length();
				}
				else
				{
					PhaseMarkerSet = true;
					PhasePosition += tmpLine.length();
				}
			}

			// Other instructions
			else
			{
				// Check for constant modifiers and update them to use unused temp register
				if (std::regex_search(NewLine, std::regex("-c[0-9]|c[0-9][\\.wxyz]*_")))
				{
					for (size_t j = 0; j < 6; j++)
					{
						std::string reg = "r" + std::to_string(j);

						if (NewSourceCode.find(reg) == std::string::npos)
						{
							const std::string constReg = std::regex_replace(NewLine, std::regex(".*-(c[0-9]).*|.*(c[0-9])[\\.wxyz]*_.*"), "$1$2");

							// Check if this constant has modifiers in more than one line
							if (std::regex_search(SourceCode14.substr(LinePosition + NewLine.length(), SourceCode14.length()), std::regex("-" + constReg + "|" + constReg + "[\\.wxyz]*_")))
							{
								// Find an unused register
								while (j < 6 &&
									(NewSourceCode.find("r" + std::to_string(j)) != std::string::npos ||
									SourceCode14.find("r" + std::to_string(j)) != std::string::npos))
								{
									j++;
								}
								// Replace all constants with the unused register
								if (j < 6)
								{
									reg = "r" + std::to_string(j);
									SourceCode14 = std::regex_replace(SourceCode14, std::regex(constReg), reg);
								}
							}

							const std::string tmpLine = "    mov " + reg + ", " + constReg + "\n";

							// Update the constant in this line and add 'mov' statement before or after the 'phase' statement
							NewLine = std::regex_replace(NewLine, std::regex(constReg), reg);
							if (ArithmeticCount14 < 8)
							{
								NewSourceCode.insert(PhasePosition + TexturePosition, tmpLine);
								ArithmeticCount14++;
							}
							else
							{
								PhaseMarkerSet = true;
								NewSourceCode.insert(PhasePosition, tmpLine);
								PhasePosition += tmpLine.length();
							}
							break;
						}
					}
				}

				// Update register from vector once it is used for the last time
				if (ReplaceReg.size() > 0)
				{
					for (size_t x = 0; x < ReplaceReg.size(); x++)
					{
						// Check if register is used in this line
						if (NewLine.find(ReplaceReg[x].dest) != std::string::npos)
						{
							// Get position of all lines after this line
							size_t start = LinePosition + NewLine.length();
							// Move position to next line if the first line is a co-issed command
							start = (SourceCode14.substr(start, 4).find("+") == std::string::npos) ? start : SourceCode14.find("\n", start + 1);

							// Check if register is used in the code after this position
							if (SourceCode14.find(ReplaceReg[x].dest, start) == std::string::npos)
							{
								// Update dest register using source register from the vector
								NewLine = std::regex_replace(NewLine, std::regex("([ \\+]+[a-z_\\.0-9]+ )r[0-9](.*)"), "$1" + ReplaceReg[x].source + "$2");
								ReplaceReg.erase(ReplaceReg.begin() + x);
								break;
							}
						}
					}
				}

				// Check if texture is no longer being used and update the dest register
				if (std::regex_search(NewLine, std::regex("t[0-9]")))
				{
					const std::string texNum = std::regex_replace(NewLine, std::regex(".*t([0-9]).*"), "$1");

					// Get position of all lines after this line
					size_t start = LinePosition + NewLine.length();
					// Move position to next line if the first line is a co-issed command
					start = (SourceCode14.substr(start, 4).find("+") == std::string::npos) ? start : SourceCode14.find("\n", start + 1);

					// Check if texture is used in the code after this position
					if (SourceCode14.find("t" + texNum, start) == std::string::npos)
					{
						const std::string destRegNum = std::regex_replace(NewLine, std::regex("[ \\+]+[a-z_\\.0-9]+ r([0-9]).*"), "$1");

						// Check if destination register is already being used by a texture register
						const unsigned long Num = strtoul(destRegNum.c_str(), nullptr, 10);
						if (!RegisterUsed[(Num < 6) ? Num : 6])
						{
							// Check if line is using more than one texture and error out
							if (std::regex_search(std::regex_replace(NewLine, std::regex("t" + texNum), "r" + texNum), std::regex("t[0-9]")))
							{
								ConvertError = true;
								break;
							}
							// Check if this is the first or last time the register is used
							if (NewSourceCode.find("r" + destRegNum) == std::string::npos ||
								SourceCode14.find("r" + destRegNum, start) == std::string::npos)
							{
								// Update dest register using texture register
								NewLine = std::regex_replace(NewLine, std::regex("([ \\+]+[a-z_\\.0-9]+ )r[0-9](.*)"), "$1r" + texNum + "$2");
								// Update code replacing all regsiters after the marked position with the texture register
								const std::string tempSourceCode = std::regex_replace(SourceCode14.substr(start, SourceCode14.length()), std::regex("r" + destRegNum), "r" + texNum);
								SourceCode14.resize(start);
								SourceCode14.append(tempSourceCode);
							}
							else
							{
								// If register is still being used then add registers to vector to be replaced later
								RegisterUsed[(Num < 6) ? Num : 6] = true;
								MyStrings tempReplaceReg;
								tempReplaceReg.dest = "r" + destRegNum;
								tempReplaceReg.source = "r" + texNum;
								ReplaceReg.push_back(tempReplaceReg);
							}
						}
					}
				}

				// Add line to SourceCode
				NewLine = std::regex_replace(NewLine, std::regex("t([0-9])"), "r$1") + "\n";
				NewSourceCode.append(NewLine);
			}
		}

		// Add 'phase' instruction
		NewSourceCode.insert(PhasePosition, "    phase\n");

		// If no errors were encountered then check if code assembles
		if (!ConvertError && D3DXAssembleShader != nullptr)
		{
			// Test if ps_1_4 assembles
			if (SUCCEEDED(D3DXAssembleShader(NewSourceCode.data(), static_cast<UINT>(NewSourceCode.size()), nullptr, nullptr, 0, &Assembly, &ErrorBuffer)))
			{
				SourceCode = NewSourceCode;
				Assembly->Release();
				Assembly = nullptr;
			}
			else
			{
#ifndef D3D8TO9NOLOG
				LOG << "> Failed to convert shader to ps_1_4" << std::endl;
				LOG << "> Dumping translated shader assembly:" << std::endl << std::endl << NewSourceCode << std::endl;
#endif
				if (ErrorBuffer != nullptr)
				{
#ifndef D3D8TO9NOLOG
					LOG << "> Failed to reassemble shader:" << std::endl << std::endl << static_cast<const char*>(ErrorBuffer->GetBufferPointer()) << std::endl;
#endif
					ErrorBuffer->Release();
					ErrorBuffer = nullptr;
				}
			}
		}
	}

	// Change '-' modifier for constant values when using 'mad' arithmetic by changing it to use 'sub'
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(mad)([_satxd248]*) (r[0-9][\\.wxyz]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*), (1?-?[crtv][0-9][\\.wxyz_abdis2]*), (-)(c[0-9][\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa]|)(?![_\\.wxyz])"),
		"sub$2 $3, $4, $7$8 /* changed 'mad' to 'sub' removed $5 removed modifier $6 */");

	// Remove trailing modifiers for constant values
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(c[0-9][\\.wxyz]*)(_bx2|_bias|_x2|_d[zbwa])"),
		"$1 /* removed modifier $2 */");

	// Remove remaining modifiers for constant values
	SourceCode = std::regex_replace(SourceCode,
		std::regex("(1?-)(c[0-9][\\.wxyz]*(?![\\.wxyz]))"),
		"$2 /* removed modifier $1 */");

#ifndef D3D8TO9NOLOG
	LOG << "> Dumping translated shader assembly:" << std::endl << std::endl << SourceCode << std::endl;
#endif

	if (D3DXAssembleShader != nullptr)
	{
		hr = D3DXAssembleShader(SourceCode.data(), static_cast<UINT>(SourceCode.size()), nullptr, nullptr, D3DXASM_FLAGS, &Assembly, &ErrorBuffer);
	}
	else
	{
		hr = D3DERR_INVALIDCALL;
	}

	Disassembly->Release();

	if (FAILED(hr))
	{
		if (ErrorBuffer != nullptr)
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed to reassemble shader:" << std::endl << std::endl << static_cast<const char *>(ErrorBuffer->GetBufferPointer()) << std::endl;
#endif
			ErrorBuffer->Release();
		}
		else
		{
#ifndef D3D8TO9NOLOG
			LOG << "> Failed to reassemble shader with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
		}

		return hr;
	}

	hr = ProxyInterface->CreatePixelShader(static_cast<const DWORD *>(Assembly->GetBufferPointer()), reinterpret_cast<IDirect3DPixelShader9 **>(pHandle));

	Assembly->Release();

	if (FAILED(hr))
	{
#ifndef D3D8TO9NOLOG
		LOG << "> 'IDirect3DDevice9::CreatePixelShader' failed with error code " << std::hex << hr << std::dec << "!" << std::endl;
#endif
	}
	else
	{
		PixelShaderHandles.insert(*pHandle);
	}

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShader(DWORD Handle)
{
	const HRESULT hr = ProxyInterface->SetPixelShader(reinterpret_cast<IDirect3DPixelShader9 *>(Handle));
	if (FAILED(hr))
		return hr;

	CurrentPixelShaderHandle = Handle;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShader(DWORD *pHandle)
{
	if (pHandle == nullptr)
		return D3DERR_INVALIDCALL;

	*pHandle = CurrentPixelShaderHandle;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePixelShader(DWORD Handle)
{
	if (Handle == 0)
		return D3DERR_INVALIDCALL;

	if (PixelShaderHandles.erase(Handle) == 0)
		return D3DERR_INVALIDCALL;

	if (CurrentPixelShaderHandle == Handle)
		SetPixelShader(0);

	reinterpret_cast<IDirect3DPixelShader9 *>(Handle)->Release();

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::SetPixelShaderConstant(DWORD Register, const void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->SetPixelShaderConstantF(Register, static_cast<const float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderConstant(DWORD Register, void *pConstantData, DWORD ConstantCount)
{
	return ProxyInterface->GetPixelShaderConstantF(Register, static_cast<float *>(pConstantData), ConstantCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::GetPixelShaderFunction(DWORD Handle, void *pData, DWORD *pSizeOfData)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3DDevice8::GetPixelShaderFunction" << "(" << this << ", " << Handle << ", " << pData << ", " << pSizeOfData << ")' ..." << std::endl;
#endif

	if (Handle == 0)
		return D3DERR_INVALIDCALL;

	IDirect3DPixelShader9 *const PixelShaderInterface = reinterpret_cast<IDirect3DPixelShader9 *>(Handle);

#ifndef D3D8TO9NOLOG
	LOG << "> Returning translated shader byte code." << std::endl;
#endif

	return PixelShaderInterface->GetFunction(pData, reinterpret_cast<UINT *>(pSizeOfData));
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo)
{
	return ProxyInterface->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo)
{
	return ProxyInterface->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice8::DeletePatch(UINT Handle)
{
	return ProxyInterface->DeletePatch(Handle);
}

void Direct3DDevice8::ApplyClipPlanes()
{
	DWORD index = 0;
	for (const auto plane : StoredClipPlanes)
	{
		if ((ClipPlaneRenderState & (1 << index)) != 0)
			ProxyInterface->SetClipPlane(index, plane);

		index++;
	}
}

void Direct3DDevice8::ReleaseShadersAndStateBlocks()
{
	while (!PixelShaderHandles.empty())
	{
		DWORD Handle = *PixelShaderHandles.begin();
		DeletePixelShader(Handle);
	}

	while (!VertexShaderHandles.empty())
	{
		DWORD Handle = *VertexShaderHandles.begin();
		DeleteVertexShader(Handle);
	}

	VertexShaderAndDeclarationCount = 0;

	while (!StateBlockTokens.empty())
	{
		DWORD Token = *StateBlockTokens.begin();
		DeleteStateBlock(Token);
	}
}
