/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "d3d8to9.hpp"
#include "psobbvr_probe.hpp"

// psobbvr: tightly-packed D3D8-style layout of one mip level (bytes per
// stored row and number of stored rows - block rows for the DXT formats).
// Returns false for formats we do not know; callers then fall back to the
// plain pass-through lock.
static bool TightLevelLayout(const D3DSURFACE_DESC &Desc, UINT &RowBytes, UINT &Rows)
{
	switch (Desc.Format)
	{
	case D3DFMT_DXT1:
		RowBytes = ((Desc.Width + 3) / 4) * 8;
		Rows = (Desc.Height + 3) / 4;
		return true;
	case D3DFMT_DXT2:
	case D3DFMT_DXT3:
	case D3DFMT_DXT4:
	case D3DFMT_DXT5:
		RowBytes = ((Desc.Width + 3) / 4) * 16;
		Rows = (Desc.Height + 3) / 4;
		return true;
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8:
		RowBytes = Desc.Width * 4;
		Rows = Desc.Height;
		return true;
	case D3DFMT_R8G8B8:
		RowBytes = Desc.Width * 3;
		Rows = Desc.Height;
		return true;
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4:
	case D3DFMT_A8L8:
		RowBytes = Desc.Width * 2;
		Rows = Desc.Height;
		return true;
	case D3DFMT_A8:
	case D3DFMT_L8:
	case D3DFMT_P8:
	case D3DFMT_A4L4:
		RowBytes = Desc.Width;
		Rows = Desc.Height;
		return true;
	default:
		return false;
	}
}

Direct3DTexture8::Direct3DTexture8(Direct3DDevice8 *Device, IDirect3DTexture9 *ProxyInterface) :
	Device(Device), ProxyInterface(ProxyInterface)
{
	Device->ProxyAddressLookupTable->SaveAddress(this, ProxyInterface);
}
Direct3DTexture8::~Direct3DTexture8()
{
}

HRESULT STDMETHODCALLTYPE Direct3DTexture8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3DTexture8) ||
		riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDirect3DResource8) ||
		riid == __uuidof(IDirect3DBaseTexture8))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3DTexture8 *>(this);

		return S_OK;
	}

	const HRESULT hr = ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
	if (SUCCEEDED(hr))
		GenericQueryInterface(riid, ppvObj, Device);

	return hr;
}
ULONG STDMETHODCALLTYPE Direct3DTexture8::AddRef()
{
	return ProxyInterface->AddRef();
}
ULONG STDMETHODCALLTYPE Direct3DTexture8::Release()
{
	return ProxyInterface->Release();
}

HRESULT STDMETHODCALLTYPE Direct3DTexture8::GetDevice(IDirect3DDevice8 **ppDevice)
{
	if (ppDevice == nullptr)
		return D3DERR_INVALIDCALL;

	Device->AddRef();
	*ppDevice = Device;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DTexture8::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags)
{
	return ProxyInterface->SetPrivateData(refguid, pData, SizeOfData, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DTexture8::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData)
{
	return ProxyInterface->GetPrivateData(refguid, pData, pSizeOfData);
}
HRESULT STDMETHODCALLTYPE Direct3DTexture8::FreePrivateData(REFGUID refguid)
{
	return ProxyInterface->FreePrivateData(refguid);
}
DWORD STDMETHODCALLTYPE Direct3DTexture8::SetPriority(DWORD PriorityNew)
{
	return ProxyInterface->SetPriority(PriorityNew);
}
DWORD STDMETHODCALLTYPE Direct3DTexture8::GetPriority()
{
	return ProxyInterface->GetPriority();
}
void STDMETHODCALLTYPE Direct3DTexture8::PreLoad()
{
	ProxyInterface->PreLoad();
}
D3DRESOURCETYPE STDMETHODCALLTYPE Direct3DTexture8::GetType()
{
	return D3DRTYPE_TEXTURE;
}

DWORD STDMETHODCALLTYPE Direct3DTexture8::SetLOD(DWORD LODNew)
{
	return ProxyInterface->SetLOD(LODNew);
}
DWORD STDMETHODCALLTYPE Direct3DTexture8::GetLOD()
{
	return ProxyInterface->GetLOD();
}
DWORD STDMETHODCALLTYPE Direct3DTexture8::GetLevelCount()
{
	return ProxyInterface->GetLevelCount();
}

HRESULT STDMETHODCALLTYPE Direct3DTexture8::GetLevelDesc(UINT Level, D3DSURFACE_DESC8 *pDesc)
{
	if (pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	D3DSURFACE_DESC SurfaceDesc;

	const HRESULT hr = ProxyInterface->GetLevelDesc(Level, &SurfaceDesc);
	if (FAILED(hr))
		return hr;

	ConvertSurfaceDesc(SurfaceDesc, *pDesc);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DTexture8::GetSurfaceLevel(UINT Level, IDirect3DSurface8 **ppSurfaceLevel)
{
	if (ppSurfaceLevel == nullptr)
		return D3DERR_INVALIDCALL;

	*ppSurfaceLevel = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetSurfaceLevel(Level, &SurfaceInterface);
	if (FAILED(hr))
		return hr;

	*ppSurfaceLevel = Device->ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DTexture8::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
	// psobbvr: the game's texture loader (0x8391FA/0x839236) locks mip
	// level 0 and memcpys the whole mip chain into it at once. That is valid
	// on D3D8 MANAGED textures, whose system-memory image stores the mips
	// contiguously and tightly packed, but overruns the heap on the D3D9Ex
	// MANAGED->DEFAULT+DYNAMIC remap, where a level-0 lock is one level's
	// allocation (the copy faults at 0x86CEFF). So hand out one contiguous
	// tightly-packed chain buffer (pre-filled so reads and partial writes
	// behave like real memory) and distribute it per level, pitch-aware, on
	// unlock. Only the remapped textures qualify (DEFAULT+DYNAMIC; the game
	// creates only MANAGED, so the plain-D3D9 path is unchanged).
	if (Level == 0 && pRect == nullptr && pLockedRect != nullptr && !ShadowActive)
	{
		D3DSURFACE_DESC Desc;
		const UINT LevelCount = ProxyInterface->GetLevelCount();
		if (LevelCount > 1 &&
			SUCCEEDED(ProxyInterface->GetLevelDesc(0, &Desc)) &&
			Desc.Pool == D3DPOOL_DEFAULT && (Desc.Usage & D3DUSAGE_DYNAMIC) != 0)
		{
			UINT Total = 0, Row0 = 0;
			bool LayoutKnown = true;
			for (UINT L = 0; L < LevelCount && LayoutKnown; L++)
			{
				D3DSURFACE_DESC LevelDesc;
				UINT RowBytes = 0, Rows = 0;
				LayoutKnown = SUCCEEDED(ProxyInterface->GetLevelDesc(L, &LevelDesc)) &&
					TightLevelLayout(LevelDesc, RowBytes, Rows);
				if (LayoutKnown)
				{
					if (L == 0)
						Row0 = RowBytes;
					Total += RowBytes * Rows;
				}
			}
			if (LayoutKnown && Total != 0)
			{
				ShadowChain.assign(Total, 0);
				ShadowLevels = LevelCount;
				uint8_t *Out = ShadowChain.data();
				for (UINT L = 0; L < LevelCount; L++)
				{
					D3DSURFACE_DESC LevelDesc;
					UINT RowBytes = 0, Rows = 0;
					ProxyInterface->GetLevelDesc(L, &LevelDesc);
					TightLevelLayout(LevelDesc, RowBytes, Rows);
					D3DLOCKED_RECT Src;
					if (SUCCEEDED(ProxyInterface->LockRect(L, &Src, nullptr, D3DLOCK_READONLY)))
					{
						for (UINT r = 0; r < Rows; r++)
							memcpy(Out + size_t(r) * RowBytes,
							       static_cast<const uint8_t *>(Src.pBits) + size_t(r) * Src.Pitch,
							       RowBytes);
						ProxyInterface->UnlockRect(L);
					}
					Out += size_t(RowBytes) * Rows;
				}
				ShadowActive = true;
				pLockedRect->pBits = ShadowChain.data();
				pLockedRect->Pitch = static_cast<INT>(Row0);
				probe::Log("texlock: shadow chain lock %ux%u lv=%u fmt=%d total=%u",
				           Desc.Width, Desc.Height, LevelCount, Desc.Format, Total);
				return D3D_OK;
			}
		}
	}
	return ProxyInterface->LockRect(Level, pLockedRect, pRect, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DTexture8::UnlockRect(UINT Level)
{
	// psobbvr: distribute the shadow chain (see LockRect) to the real
	// per-level surfaces, honoring each level's actual pitch.
	if (ShadowActive && Level == 0)
	{
		const uint8_t *In = ShadowChain.data();
		for (UINT L = 0; L < ShadowLevels; L++)
		{
			D3DSURFACE_DESC LevelDesc;
			UINT RowBytes = 0, Rows = 0;
			if (FAILED(ProxyInterface->GetLevelDesc(L, &LevelDesc)) ||
				!TightLevelLayout(LevelDesc, RowBytes, Rows))
				break;
			D3DLOCKED_RECT Dst;
			if (SUCCEEDED(ProxyInterface->LockRect(L, &Dst, nullptr, 0)))
			{
				for (UINT r = 0; r < Rows; r++)
					memcpy(static_cast<uint8_t *>(Dst.pBits) + size_t(r) * Dst.Pitch,
					       In + size_t(r) * RowBytes, RowBytes);
				ProxyInterface->UnlockRect(L);
			}
			In += size_t(RowBytes) * Rows;
		}
		ShadowActive = false;
		ShadowChain.clear();
		ShadowChain.shrink_to_fit();
		return D3D_OK;
	}
	return ProxyInterface->UnlockRect(Level);
}
HRESULT STDMETHODCALLTYPE Direct3DTexture8::AddDirtyRect(const RECT *pDirtyRect)
{
	return ProxyInterface->AddDirtyRect(pDirtyRect);
}

Direct3DCubeTexture8::Direct3DCubeTexture8(Direct3DDevice8 *device, IDirect3DCubeTexture9 *ProxyInterface) :
	Device(device),
	ProxyInterface(ProxyInterface)
{
	Device->ProxyAddressLookupTable->SaveAddress(this, ProxyInterface);
}
Direct3DCubeTexture8::~Direct3DCubeTexture8()
{
}

HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3DCubeTexture8) ||
		riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDirect3DResource8) ||
		riid == __uuidof(IDirect3DBaseTexture8))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3DCubeTexture8 *>(this);

		return S_OK;
	}

	const HRESULT hr = ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
	if (SUCCEEDED(hr))
		GenericQueryInterface(riid, ppvObj, Device);

	return hr;
}
ULONG STDMETHODCALLTYPE Direct3DCubeTexture8::AddRef()
{
	return ProxyInterface->AddRef();
}
ULONG STDMETHODCALLTYPE Direct3DCubeTexture8::Release()
{
	return ProxyInterface->Release();
}

HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::GetDevice(IDirect3DDevice8 **ppDevice)
{
	if (ppDevice == nullptr)
		return D3DERR_INVALIDCALL;

	Device->AddRef();
	*ppDevice = Device;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags)
{
	return ProxyInterface->SetPrivateData(refguid, pData, SizeOfData, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData)
{
	return ProxyInterface->GetPrivateData(refguid, pData, pSizeOfData);
}
HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::FreePrivateData(REFGUID refguid)
{
	return ProxyInterface->FreePrivateData(refguid);
}
DWORD STDMETHODCALLTYPE Direct3DCubeTexture8::SetPriority(DWORD PriorityNew)
{
	return ProxyInterface->SetPriority(PriorityNew);
}
DWORD STDMETHODCALLTYPE Direct3DCubeTexture8::GetPriority()
{
	return ProxyInterface->GetPriority();
}
void STDMETHODCALLTYPE Direct3DCubeTexture8::PreLoad()
{
	ProxyInterface->PreLoad();
}
D3DRESOURCETYPE STDMETHODCALLTYPE Direct3DCubeTexture8::GetType()
{
	return D3DRTYPE_CUBETEXTURE;
}

DWORD STDMETHODCALLTYPE Direct3DCubeTexture8::SetLOD(DWORD LODNew)
{
	return ProxyInterface->SetLOD(LODNew);
}
DWORD STDMETHODCALLTYPE Direct3DCubeTexture8::GetLOD()
{
	return ProxyInterface->GetLOD();
}
DWORD STDMETHODCALLTYPE Direct3DCubeTexture8::GetLevelCount()
{
	return ProxyInterface->GetLevelCount();
}

HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::GetLevelDesc(UINT Level, D3DSURFACE_DESC8 *pDesc)
{
	if (pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	D3DSURFACE_DESC SurfaceDesc;

	const HRESULT hr = ProxyInterface->GetLevelDesc(Level, &SurfaceDesc);
	if (FAILED(hr))
		return hr;

	ConvertSurfaceDesc(SurfaceDesc, *pDesc);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface8 **ppCubeMapSurface)
{
	if (ppCubeMapSurface == nullptr)
		return D3DERR_INVALIDCALL;

	*ppCubeMapSurface = nullptr;

	IDirect3DSurface9 *SurfaceInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetCubeMapSurface(FaceType, Level, &SurfaceInterface);
	if (FAILED(hr))
		return hr;

	*ppCubeMapSurface = Device->ProxyAddressLookupTable->FindAddress<Direct3DSurface8>(SurfaceInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
	return ProxyInterface->LockRect(FaceType, Level, pLockedRect, pRect, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level)
{
	return ProxyInterface->UnlockRect(FaceType, Level);
}
HRESULT STDMETHODCALLTYPE Direct3DCubeTexture8::AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect)
{
	return ProxyInterface->AddDirtyRect(FaceType, pDirtyRect);
}

Direct3DVolumeTexture8::Direct3DVolumeTexture8(Direct3DDevice8 *device, IDirect3DVolumeTexture9 *ProxyInterface) :
	Device(device),
	ProxyInterface(ProxyInterface)
{
	Device->ProxyAddressLookupTable->SaveAddress(this, ProxyInterface);
}
Direct3DVolumeTexture8::~Direct3DVolumeTexture8()
{
}

HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3DVolumeTexture8) ||
		riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDirect3DResource8) ||
		riid == __uuidof(IDirect3DBaseTexture8))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3DVolumeTexture8 *>(this);

		return S_OK;
	}

	const HRESULT hr = ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
	if (SUCCEEDED(hr))
		GenericQueryInterface(riid, ppvObj, Device);

	return hr;
}
ULONG STDMETHODCALLTYPE Direct3DVolumeTexture8::AddRef()
{
	return ProxyInterface->AddRef();
}
ULONG STDMETHODCALLTYPE Direct3DVolumeTexture8::Release()
{
	return ProxyInterface->Release();
}

HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::GetDevice(IDirect3DDevice8 **ppDevice)
{
	if (ppDevice == nullptr)
		return D3DERR_INVALIDCALL;

	Device->AddRef();
	*ppDevice = Device;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags)
{
	return ProxyInterface->SetPrivateData(refguid, pData, SizeOfData, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData)
{
	return ProxyInterface->GetPrivateData(refguid, pData, pSizeOfData);
}
HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::FreePrivateData(REFGUID refguid)
{
	return ProxyInterface->FreePrivateData(refguid);
}
DWORD STDMETHODCALLTYPE Direct3DVolumeTexture8::SetPriority(DWORD PriorityNew)
{
	return ProxyInterface->SetPriority(PriorityNew);
}
DWORD STDMETHODCALLTYPE Direct3DVolumeTexture8::GetPriority()
{
	return ProxyInterface->GetPriority();
}
void STDMETHODCALLTYPE Direct3DVolumeTexture8::PreLoad()
{
	ProxyInterface->PreLoad();
}
D3DRESOURCETYPE STDMETHODCALLTYPE Direct3DVolumeTexture8::GetType()
{
	return D3DRTYPE_VOLUMETEXTURE;
}

DWORD STDMETHODCALLTYPE Direct3DVolumeTexture8::SetLOD(DWORD LODNew)
{
	return ProxyInterface->SetLOD(LODNew);
}
DWORD STDMETHODCALLTYPE Direct3DVolumeTexture8::GetLOD()
{
	return ProxyInterface->GetLOD();
}
DWORD STDMETHODCALLTYPE Direct3DVolumeTexture8::GetLevelCount()
{
	return ProxyInterface->GetLevelCount();
}

HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::GetLevelDesc(UINT Level, D3DVOLUME_DESC8 *pDesc)
{
	if (pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	D3DVOLUME_DESC VolumeDesc;

	const HRESULT hr = ProxyInterface->GetLevelDesc(Level, &VolumeDesc);
	if (FAILED(hr))
		return hr;

	ConvertVolumeDesc(VolumeDesc, *pDesc);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::GetVolumeLevel(UINT Level, IDirect3DVolume8 **ppVolumeLevel)
{
	if (ppVolumeLevel == nullptr)
		return D3DERR_INVALIDCALL;

	*ppVolumeLevel = nullptr;

	IDirect3DVolume9 *VolumeInterface = nullptr;

	const HRESULT hr = ProxyInterface->GetVolumeLevel(Level, &VolumeInterface);
	if (FAILED(hr))
		return hr;

	*ppVolumeLevel = Device->ProxyAddressLookupTable->FindAddress<Direct3DVolume8>(VolumeInterface);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags)
{
	return ProxyInterface->LockBox(Level, pLockedVolume, pBox, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::UnlockBox(UINT Level)
{
	return ProxyInterface->UnlockBox(Level);
}
HRESULT STDMETHODCALLTYPE Direct3DVolumeTexture8::AddDirtyBox(const D3DBOX *pDirtyBox)
{
	return ProxyInterface->AddDirtyBox(pDirtyBox);
}
