// OpenXR backend. Each frame is submitted with the pose and FOV it was
// rendered with, so the compositor re-projects it to the current head
// pose at display rate while the game stays at 30 FPS. Needs SteamVR
// 2.17.2 or later (the first with a 32-bit OpenXR runtime).
//
// Frame protocol on the Present tail
// (composite -> flush -> SubmitFrame -> WaitPoses):
//   WaitPoses    = pump XR events; xrWaitFrame + xrBeginFrame for the frame
//                  the game is ABOUT to render; xrLocateViews at its
//                  predicted display time; cache pose+FOV and derive the
//                  game-convention matrices from them.
//   SubmitFrame  = CopyResource each eye's shared texture (the D3D9Ex ->
//                  D3D11 bridge) into the XR swapchain; xrEndFrame stamped
//                  with the CACHED pose+FOV - exactly what the game
//                  rendered with.
// The single cached view set is always the right one: WaitPoses(N) caches
// what frame N+1 renders with, and SubmitFrame(N+1) runs before
// WaitPoses(N+1) refreshes the cache.
//
// Colors: the game's output is display-referred sRGB in UNORM textures
// (OpenVR path submits ColorSpace_Gamma). Equivalent here: an sRGB-typed
// swapchain + CopyResource (same-typeless-family copies preserve bits; the
// sRGB type only changes interpretation). No shaders anywhere.
//
// openxr_loader.dll is delay-loaded and vendored like openvr_api.dll: a
// missing DLL or missing 32-bit runtime registration degrades to "VR
// unavailable" with a message, never a failed game start.

#include "psobbvr_vr.hpp"
#include "psobbvr_probe.hpp"
// Preflight, session and controller-input lines go to diag::Log
// (psobbvr-vr.log, always on); probe::Log writes only on probe-armed
// launches.
#include "psobbvr_log.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <tlhelp32.h>

namespace vrmod {

namespace {

// Is a process with this exe name alive? 1 yes, 0 no, -1 the snapshot
// itself failed (treat as unknown, never as "not running").
int ProcessRunning(const char* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    int found = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, exe_name) == 0) {
                found = 1;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// XR pose (meters, y-up, -z forward - same axes as OpenVR's seated space)
// to the game's row-vector rigid transform, translation scaled to game
// units. The row-vector matrix is the transpose of the standard
// column-vector rotation - written out directly.
D3DMATRIX FromXrPose(const XrPosef& pose, float scale) {
    const float x = pose.orientation.x, y = pose.orientation.y;
    const float z = pose.orientation.z, w = pose.orientation.w;
    D3DMATRIX r = Identity();
    r._11 = 1 - 2 * (y * y + z * z);
    r._12 = 2 * (x * y + w * z);
    r._13 = 2 * (x * z - w * y);
    r._21 = 2 * (x * y - w * z);
    r._22 = 1 - 2 * (x * x + z * z);
    r._23 = 2 * (y * z + w * x);
    r._31 = 2 * (x * z + w * y);
    r._32 = 2 * (y * z - w * x);
    r._33 = 1 - 2 * (x * x + y * y);
    r._41 = pose.position.x * scale;
    r._42 = pose.position.y * scale;
    r._43 = pose.position.z * scale;
    return r;
}

class OpenXRBackend : public VRInterface {
public:
    bool Init(char* error, size_t error_len, bool quiet_probe) override {
        if (ready_) {
            SetStatus("VR already running (OpenXR)");
            return true;
        }
        // Delay-load guard, same pattern as the OpenVR backend.
        if (LoadLibraryA("openxr_loader.dll") == nullptr) {
            Fail(error, error_len, "openxr_loader.dll not found next to the game exe");
            return false;
        }
        // The 32-bit runtime registration: absent means SteamVR's "set as
        // OpenXR runtime" prompt was never accepted (or SteamVR is older
        // than 2.17.2), and xrCreateInstance could only fail. quiet_probe
        // is unused: OpenXR has no headset-present query that avoids
        // starting the runtime.
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0,
                          KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) {
            Fail(error, error_len,
                 "No 32-bit OpenXR runtime is registered on this PC. Install "
                 "or update SteamVR (2.17 or later - the current stable "
                 "release is fine) and accept its 'set as OpenXR runtime' "
                 "prompt, or set it in SteamVR Settings > OpenXR.");
            return false;
        }
        // Log the registered 32-bit runtime before touching it: the Meta
        // Quest Link app silently re-registers its own runtime after
        // updates.
        {
            char json_path[MAX_PATH] = "";
            DWORD type = 0, size = sizeof(json_path);
            if (RegQueryValueExA(key, "ActiveRuntime", nullptr, &type,
                                 reinterpret_cast<BYTE*>(json_path), &size) == ERROR_SUCCESS &&
                type == REG_SZ)
                diag::Log("openxr: registered 32-bit runtime (HKLM WOW64 "
                          "ActiveRuntime): %s", json_path);
            else
                diag::Log("openxr: 32-bit OpenXR key present but ActiveRuntime "
                          "unreadable");
        }
        RegCloseKey(key);
        (void)quiet_probe;

        // SteamVR must already be running. Otherwise xrCreateInstance boots
        // vrserver itself, vrmonitor starts last and hangs in "Startup",
        // and SteamVR stays stuck while our init still succeeds. Checked
        // before xrEnumerateInstanceExtensionProperties too (the loader
        // maps the runtime DLL for it). A 32-bit Toolhelp snapshot lists
        // 64-bit processes by name.
        {
            const int running = ProcessRunning("vrserver.exe");
            if (running == 0) {
                Fail(error, error_len,
                     "SteamVR not running. Start SteamVR first, then launch "
                     "Psobb.exe");
                return false;
            }
            diag::Log(running > 0
                          ? "openxr: SteamVR is running (vrserver.exe found)"
                          : "openxr: process snapshot failed - cannot tell "
                            "whether SteamVR is running, continuing");
        }

        // Does this runtime speak the G2 controllers' own binding profile?
        // It lives behind an extension (the G2 controller has A/B/X/Y
        // buttons the classic WMR profile lacks); enumeration is
        // loader-level and legal before xrCreateInstance.
        const char* kHpExt = "XR_EXT_hp_mixed_reality_controller";
        hp_profile_available_ = false;
        {
            uint32_t n = 0;
            xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
            if (n > 0) {
                std::vector<XrExtensionProperties> props(
                    n, {XR_TYPE_EXTENSION_PROPERTIES});
                if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(
                        nullptr, n, &n, props.data()))) {
                    for (uint32_t i = 0; i < n; i++)
                        if (strcmp(props[i].extensionName, kHpExt) == 0)
                            hp_profile_available_ = true;
                }
            }
            diag::Log("openxr: runtime offers %u extensions; %s %s", n, kHpExt,
                      hp_profile_available_
                          ? "PRESENT (G2-native bindings)"
                          : "ABSENT (classic WMR profile fallback)");
        }
        const char* extensions[2] = {"XR_KHR_D3D11_enable", kHpExt};
        XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ici.applicationInfo.applicationName, "psobbvr");
        ici.applicationInfo.applicationVersion = 1;
        strcpy_s(ici.applicationInfo.engineName, "psobbvr");
        ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ici.enabledExtensionCount =
            (config.controllers && hp_profile_available_) ? 2 : 1;
        ici.enabledExtensionNames = extensions;
        XrResult xr = xrCreateInstance(&ici, &instance_);
        if (XR_FAILED(xr)) {
            Fail(error, error_len, "xrCreateInstance failed (%d)", (int)xr);
            return false;
        }
        XrInstanceProperties iprops = {XR_TYPE_INSTANCE_PROPERTIES};
        xrGetInstanceProperties(instance_, &iprops);
        const unsigned rt_major = XR_VERSION_MAJOR(iprops.runtimeVersion),
                       rt_minor = XR_VERSION_MINOR(iprops.runtimeVersion),
                       rt_patch = XR_VERSION_PATCH(iprops.runtimeVersion);
        probe::Log("openxr: runtime %s %u.%u.%u", iprops.runtimeName, rt_major,
                   rt_minor, rt_patch);
        diag::Log("openxr: runtime \"%s\" version %u.%u.%u", iprops.runtimeName,
                  rt_major, rt_minor, rt_patch);
        // Only SteamVR is tested. Any other runtime is refused with a
        // message naming it, unless [vr] allow_any_runtime=1.
        const bool is_steamvr = strstr(iprops.runtimeName, "SteamVR") != nullptr;
        if (!is_steamvr) {
            if (config.allow_any_runtime) {
                diag::Log("openxr: WARNING - runtime is not SteamVR; starting "
                          "anyway because allow_any_runtime=1 (untested path)");
            } else {
                Fail(error, error_len,
                     "The active OpenXR runtime is \"%s\", not SteamVR. This mod "
                     "is only tested on SteamVR. Set SteamVR as the OpenXR "
                     "runtime (SteamVR Settings > OpenXR); Quest users: the Meta "
                     "Quest Link app switches the runtime back to its own after "
                     "updates. (To try it anyway, set allow_any_runtime=1 under "
                     "[vr] in psobbvr.ini.)",
                     iprops.runtimeName);
                ShutdownInstanceOnly();
                return false;
            }
        } else if (rt_major < 2 || (rt_major == 2 && (rt_minor < 17 ||
                   (rt_minor == 17 && rt_patch < 2)))) {
            // Should not happen (the 32-bit entry exists from 2.17.2), and
            // the reported version scheme may differ from the release
            // numbering - so a warning, not a refusal.
            diag::Log("openxr: WARNING - SteamVR reports %u.%u.%u; the 32-bit "
                      "runtime needs SteamVR 2.17.2 or later", rt_major, rt_minor,
                      rt_patch);
        }

        XrSystemGetInfo sysinfo = {XR_TYPE_SYSTEM_GET_INFO};
        sysinfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xr = xrGetSystem(instance_, &sysinfo, &system_);
        if (XR_FAILED(xr)) {
            Fail(error, error_len,
                 "No headset found (xrGetSystem %d). Connect the headset and "
                 "make sure SteamVR shows it (Quest: connect through Steam "
                 "Link first), then relaunch.", (int)xr);
            ShutdownInstanceOnly();
            return false;
        }

        // D3D11 device on the adapter the runtime demands (LUID-matched).
        PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
        xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                              (PFN_xrVoidFunction*)&get_reqs);
        XrGraphicsRequirementsD3D11KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (get_reqs == nullptr || XR_FAILED(get_reqs(instance_, system_, &reqs))) {
            Fail(error, error_len, "xrGetD3D11GraphicsRequirementsKHR failed");
            ShutdownInstanceOnly();
            return false;
        }
        if (!CreateDeviceOnLuid(reqs.adapterLuid)) {
            Fail(error, error_len, "D3D11 device creation failed");
            ShutdownInstanceOnly();
            return false;
        }

        XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = d3d11_device_;
        XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &binding;
        sci.systemId = system_;
        xr = xrCreateSession(instance_, &sci, &session_);
        if (XR_FAILED(xr)) {
            Fail(error, error_len, "xrCreateSession failed (%d)", (int)xr);
            ShutdownInstanceOnly();
            return false;
        }
        XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;  // seated-like
        rsci.poseInReferenceSpace.orientation.w = 1.0f;
        xrCreateReferenceSpace(session_, &rsci, &local_space_);
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        xrCreateReferenceSpace(session_, &rsci, &view_space_);
        if (local_space_ == XR_NULL_HANDLE || view_space_ == XR_NULL_HANDLE) {
            Fail(error, error_len, "reference space creation failed");
            Shutdown();
            return false;
        }

        for (int eye = 0; eye < 2; eye++)
            eye_view_append_[eye] = Identity();

        // The controller action set. Failure is non-fatal - VR runs
        // without controller input, exactly like controllers=0.
        if (config.controllers) {
            if (!InitActions())
                diag::Log("xrinput: action setup FAILED - no controller input "
                          "this session (VR itself unaffected)");
        } else {
            diag::Log("xrinput: disabled ([vr] controllers=0)");
        }

        ready_ = true;
        SetStatus("OpenXR running, no eye textures attached yet");
        probe::Log("openxr: initialized (session created, waiting for READY)");
        return true;
    }

    void Shutdown() override {
        ReleaseSwapchains();
        ReleaseEyeTextures();
        ReleaseHudTexture();
        ReleaseMenuTexture();
        DestroyActions();
        if (session_running_) {
            xrEndSession(session_);
            session_running_ = false;
        }
        if (view_space_ != XR_NULL_HANDLE) { xrDestroySpace(view_space_); view_space_ = XR_NULL_HANDLE; }
        if (local_space_ != XR_NULL_HANDLE) { xrDestroySpace(local_space_); local_space_ = XR_NULL_HANDLE; }
        if (session_ != XR_NULL_HANDLE) { xrDestroySession(session_); session_ = XR_NULL_HANDLE; }
        if (d3d11_context_) { d3d11_context_->Release(); d3d11_context_ = nullptr; }
        if (d3d11_device_) { d3d11_device_->Release(); d3d11_device_ = nullptr; }
        ShutdownInstanceOnly();
        frame_open_ = false;
        head_pose_valid_ = false;
        eye_data_valid_ = false;
        ready_ = false;
        SetStatus("VR shut down");
    }

    bool Ready() const override { return ready_; }

    bool AttachEyeTextures(HANDLE shared_wide,
                           UINT eye_width, UINT eye_height) override {
        if (!ready_ || d3d11_device_ == nullptr)
            return false;
        ReleaseEyeTextures();
        const HRESULT hr = d3d11_device_->OpenSharedResource(
            shared_wide, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&wide_texture_));
        if (FAILED(hr) || wide_texture_ == nullptr) {
            SetStatus("OpenSharedResource(wide) failed 0x%08lX", hr);
            probe::Log("openxr: %s", status_);
            ReleaseEyeTextures();
            return false;
        }
        // Swapchains stay per-eye-sized; SubmitFrame copies each half of
        // the wide texture into its eye's swapchain image.
        if (!CreateSwapchains(eye_width, eye_height))
            return false;
        SetStatus("OpenXR running, eyes %ux%u attached (double-wide source)",
                  eye_width, eye_height);
        probe::Log("openxr: attached double-wide eye texture (eye %ux%u), swapchain fmt %lld",
                   eye_width, eye_height, (long long)swapchain_format_);
        return true;
    }

    // ---- dedicated HUD quad layer -----------------------------------------

    bool SupportsHudLayer() const override { return true; }

    bool AttachHudTexture(HANDLE shared, UINT width, UINT height) override {
        ReleaseHudTexture();
        if (!ready_ || d3d11_device_ == nullptr || shared == nullptr)
            return false;
        if (swapchain_format_ == 0) {
            // Called before a successful AttachEyeTextures - no format picked
            // yet (AttachToVr guarantees the order, so this is a guard only).
            probe::Log("openxr: hud attach before eye attach - skipped");
            return false;
        }
        const HRESULT hr = d3d11_device_->OpenSharedResource(
            shared, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&hud_texture_));
        if (FAILED(hr) || hud_texture_ == nullptr) {
            probe::Log("openxr: OpenSharedResource(hud) failed 0x%08lX", hr);
            ReleaseHudTexture();
            return false;
        }
        if (!CreateOneSwapchain(width, height, hud_swapchain_,
                                hud_swapchain_image_,
                                hud_swapchain_image_count_)) {
            probe::Log("openxr: hud swapchain creation failed");
            ReleaseHudTexture();
            return false;
        }
        hud_width_ = width;
        hud_height_ = height;
        probe::Log("openxr: attached hud texture %ux%u (quad layer ready)",
                   width, height);
        return true;
    }

    void SetHudLayerVisible(bool visible) override { hud_visible_ = visible; }

    void SetHudAnchor(bool valid, float fx, float fz) override {
        const float n = sqrtf(fx * fx + fz * fz);
        hud_anchor_valid_ = valid && n > 0.5f;
        if (hud_anchor_valid_) {
            hud_anchor_fx_ = fx / n;
            hud_anchor_fz_ = fz / n;
        }
    }

    // ---- Menu world-lock -------------------------------------------------

    bool SupportsMenuLock() const override { return true; }

    bool AttachMenuTexture(HANDLE shared, UINT width, UINT height) override {
        ReleaseMenuTexture();
        if (!ready_ || d3d11_device_ == nullptr || shared == nullptr)
            return false;
        if (swapchain_format_ == 0) {
            probe::Log("openxr: menu attach before eye attach - skipped");
            return false;
        }
        const HRESULT hr = d3d11_device_->OpenSharedResource(
            shared, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&menu_texture_));
        if (FAILED(hr) || menu_texture_ == nullptr) {
            probe::Log("openxr: OpenSharedResource(menu) failed 0x%08lX", hr);
            ReleaseMenuTexture();
            return false;
        }
        if (!CreateOneSwapchain(width, height, menu_swapchain_,
                                menu_swapchain_image_,
                                menu_swapchain_image_count_)) {
            probe::Log("openxr: menu swapchain creation failed");
            ReleaseMenuTexture();
            return false;
        }
        menu_width_ = width;
        menu_height_ = height;
        probe::Log("openxr: attached menu texture %ux%u (world-locked quad ready)",
                   width, height);
        return true;
    }

    void SetMenuLayerState(bool below, bool above) override {
        menu_below_visible_ = below;
        menu_above_visible_ = above;
    }

    void InvalidateMenuAnchor() override { menu_anchor_valid_ = false; }

    bool GetMenuAnchor(D3DMATRIX& out) override {
        if (!menu_anchor_valid_)
            return false;
        out = FromXrPose(menu_anchor_, config.world_scale);
        return true;
    }

    bool SubmitFrame() override {
        if (!ready_ || !session_running_ || !frame_open_ ||
            wide_texture_ == nullptr || swapchain_[0] == XR_NULL_HANDLE)
            return false;
        frame_open_ = false;  // this frame ends now, success or not

        XrCompositionLayerProjectionView proj_views[2];
        for (int eye = 0; eye < 2; eye++) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(xrAcquireSwapchainImage(swapchain_[eye], &acquire, &index)))
                return EndFrameEmpty();
            XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(xrWaitSwapchainImage(swapchain_[eye], &wait))) {
                XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(swapchain_[eye], &release);
                return EndFrameEmpty();
            }
            // The bridge: game's D3D9Ex wide target -> (shared handle,
            // already flushed by the caller) -> D3D11 copy of this eye's
            // HALF into the runtime's swapchain image.
            const D3D11_BOX src_box = { eye_width_ * (UINT)eye, 0, 0,
                                        eye_width_ * (UINT)(eye + 1), eye_height_, 1 };
            d3d11_context_->CopySubresourceRegion(swapchain_image_[eye][index], 0,
                                                  0, 0, 0,
                                                  wide_texture_, 0, &src_box);
            XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(swapchain_[eye], &release);

            proj_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            proj_views[eye].pose = render_view_[eye].pose;  // HONEST: what the game rendered with
            proj_views[eye].fov = render_view_[eye].fov;
            proj_views[eye].subImage.swapchain = swapchain_[eye];
            proj_views[eye].subImage.imageRect = {
                {0, 0}, {(int32_t)eye_width_, (int32_t)eye_height_}};
        }
        XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = local_space_;
        layer.viewCount = 2;
        layer.views = proj_views;
        const XrCompositionLayerBaseHeader* layers[8] = {};
        uint32_t layer_count = 0;

        // Menu world-lock: a non-gameplay frame composites world-locked at
        // the menu anchor (the wearer's position + yaw, captured at menu
        // entry, dropped on recenter / return to gameplay), so
        // reprojection can smooth it:
        //   1. below-quad (opaque): 2D drawn before any 3D this frame, over
        //      opaque black like the flat game.
        //   2. the projection layers, alpha-blended: the game's 0x00000000
        //      clear leaves alpha 0 and only real 3D draws write alpha, so
        //      they float above the 2D screen in stereo.
        //   3. above-quad (transparent, the HUD texture): 2D drawn after 3D.
        const bool menu_frame = (menu_below_visible_ || menu_above_visible_) &&
                                head_xr_valid_;
        if (menu_frame && !menu_anchor_valid_)
            CaptureMenuAnchor();
        XrCompositionLayerQuad below = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (menu_frame && menu_below_visible_ && menu_texture_ != nullptr &&
            menu_swapchain_ != XR_NULL_HANDLE &&
            CopyIntoSwapchain(menu_swapchain_, menu_swapchain_image_,
                              menu_texture_)) {
            below.layerFlags = 0;  // opaque: this screen IS the backdrop
            below.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            below.subImage.swapchain = menu_swapchain_;
            below.subImage.imageRect = {
                {0, 0}, {(int32_t)menu_width_, (int32_t)menu_height_}};
            // The backdrop sits deeper than the UI quad at the same angular
            // span, so 3D content between the layers reads in front of the
            // backdrop and behind the UI, matching the compositing order.
            PlaceMenuQuad(below, config.menu_below_distance_m);
            layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&below;
        }

        if (menu_frame)
            layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        if (render_views_valid_)
            layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&layer;

        // The HUD texture as its own quad layer above the world layer,
        // repositioned by the compositor every display refresh (a baked HUD
        // would swim under reprojection). Gameplay: head-locked in VIEW
        // space at hud_distance_m/hud_width_deg, or placed by the HUD lock.
        // Menu frames: the world-locked above-quad. Config is read per
        // frame so live tuning applies.
        //
        // Alpha: submitted premultiplied (no UNPREMULTIPLIED flag; UI
        // blended over a transparent-black clear is premultiplied). SteamVR
        // composites the quad by rgb*alpha regardless, so lit pixels with
        // alpha 0 vanish; visible draws need real coverage alpha
        // (stereo::ApplyHudLayerAlpha).
        XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        const bool want_hud_quad = menu_frame ? menu_above_visible_ : hud_visible_;
        if (want_hud_quad && hud_texture_ != nullptr &&
            hud_swapchain_ != XR_NULL_HANDLE &&
            CopyIntoSwapchain(hud_swapchain_, hud_swapchain_image_,
                              hud_texture_)) {
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = hud_swapchain_;
            quad.subImage.imageRect = {
                {0, 0}, {(int32_t)hud_width_, (int32_t)hud_height_}};
            if (menu_frame) {
                PlaceMenuQuad(quad, config.menu_distance_m);
            } else if (config.hud_lock != 0 && hud_anchor_valid_ &&
                       head_xr_valid_) {
                // HUD lock: level in LOCAL space, hud_distance_m from the
                // head along the published forward (Config::hud_lock),
                // facing the head. Yaw-only quaternion about +y; the quad's
                // local -z is its forward, so theta = atan2(-fx, -fz).
                const float d = config.hud_distance_m;
                const float theta = atan2f(-hud_anchor_fx_, -hud_anchor_fz_);
                quad.space = local_space_;
                quad.pose.orientation = {0.0f, sinf(0.5f * theta), 0.0f,
                                         cosf(0.5f * theta)};
                quad.pose.position = {
                    head_xr_pose_.position.x + hud_anchor_fx_ * d,
                    head_xr_pose_.position.y,
                    head_xr_pose_.position.z + hud_anchor_fz_ * d};
                quad.size.width = 2.0f * d *
                                  tanf(config.hud_width_deg * (3.14159265f / 360.0f));
                quad.size.height = quad.size.width * (480.0f / 640.0f);
            } else {
                quad.space = view_space_;
                quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quad.pose.position = {0.0f, 0.0f, -config.hud_distance_m};
                quad.size.width = 2.0f * config.hud_distance_m *
                                  tanf(config.hud_width_deg * (3.14159265f / 360.0f));
                quad.size.height = quad.size.width * (480.0f / 640.0f);
            }
            layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&quad;
        }

        // Tuning readout (ShowTuneText): a small head-locked quad below the
        // HUD, over everything, while a hotkey adjustment is fresh.
        XrCompositionLayerQuad tune = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (tune_visible_until_ms_ != 0) {
            if (GetTickCount64() >= tune_visible_until_ms_) {
                tune_visible_until_ms_ = 0;
            } else if (tune_texture_ != nullptr &&
                       tune_swapchain_ != XR_NULL_HANDLE &&
                       CopyIntoSwapchain(tune_swapchain_,
                                         tune_swapchain_image_,
                                         tune_texture_)) {
                tune.layerFlags =
                    XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                tune.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                tune.subImage.swapchain = tune_swapchain_;
                tune.subImage.imageRect = {
                    {0, 0}, {(int32_t)kTuneWidth, (int32_t)kTuneHeight}};
                tune.space = view_space_;
                tune.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                // ~25 degrees below the view center at 1.2 m - under the
                // HUD quad's bottom edge (1.5 m / 60 degrees, 4:3).
                tune.pose.position = {0.0f, -0.55f, -1.2f};
                tune.size.width = 0.62f;
                tune.size.height =
                    0.62f * (float)kTuneHeight / (float)kTuneWidth;
                layers[layer_count++] =
                    (const XrCompositionLayerBaseHeader*)&tune;
            }
        }

        // Teleport menu (ShowMenuText): a head-locked list quad at the view
        // center while open; rasterized only on ShowMenuText, re-submitted
        // here.
        XrCompositionLayerQuad tele = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (tele_visible_ && tele_texture_ != nullptr &&
            tele_swapchain_ != XR_NULL_HANDLE &&
            CopyIntoSwapchain(tele_swapchain_, tele_swapchain_image_,
                              tele_texture_)) {
            tele.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            tele.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            tele.subImage.swapchain = tele_swapchain_;
            tele.subImage.imageRect = {
                {0, 0}, {(int32_t)kTeleWidth, (int32_t)kTeleHeight}};
            tele.space = view_space_;
            tele.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            tele.pose.position = {0.0f, -0.05f, -1.1f};
            tele.size.width = 0.55f;
            tele.size.height = 0.55f * (float)kTeleHeight / (float)kTeleWidth;
            layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&tele;
        }

        // On-screen keyboard panel (ShowPanel): a quad pinned in LOCAL
        // space at the pose captured when it opened, composited over
        // everything while a text field is open.
        XrCompositionLayerQuad panel = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (panel_visible_ && panel_texture_ != nullptr &&
            panel_swapchain_ != XR_NULL_HANDLE &&
            CopyIntoSwapchain(panel_swapchain_, panel_swapchain_image_,
                              panel_texture_)) {
            panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            panel.subImage.swapchain = panel_swapchain_;
            panel.subImage.imageRect = {
                {0, 0}, {(int32_t)panel_width_, (int32_t)panel_height_}};
            panel.space = local_space_;
            panel.pose = panel_pose_;
            panel.size.width = panel_width_m_;
            panel.size.height =
                panel_width_m_ * (float)panel_height_ / (float)panel_width_;
            layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&panel;
        }

        // The keyboard's hand rays (SetPanelRay): one thin quad per hand
        // over the shared beam texture, after the panel so they read on
        // top of it, only while the panel is up.
        XrCompositionLayerQuad rays[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},
                                          {XR_TYPE_COMPOSITION_LAYER_QUAD}};
        if (panel_visible_ && (ray_visible_[0] || ray_visible_[1]) &&
            EnsureRayTexture() &&
            CopyIntoSwapchain(ray_swapchain_, ray_swapchain_image_, ray_texture_)) {
            for (int h = 0; h < 2; h++) {
                if (!ray_visible_[h])
                    continue;
                XrCompositionLayerQuad& q = rays[h];
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = ray_swapchain_;
                q.subImage.imageRect = {{0, 0}, {(int32_t)RAY_TEX_W, (int32_t)RAY_TEX_H}};
                q.space = local_space_;
                q.pose = ray_pose_[h];
                q.size.width = ray_width_[h];
                q.size.height = ray_length_[h];
                layers[layer_count++] = (const XrCompositionLayerBaseHeader*)&q;
            }
        }

        XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
        end.displayTime = predicted_display_time_;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = layer_count;
        end.layers = layers;
        const XrResult xr = xrEndFrame(session_, &end);
        if (XR_FAILED(xr)) {
            if (xr != last_submit_error_) {
                last_submit_error_ = xr;
                SetStatus("xrEndFrame error %d", (int)xr);
                probe::Log("openxr: %s", status_);
            }
            return false;
        }
        if (last_submit_error_ != XR_SUCCESS) {
            last_submit_error_ = XR_SUCCESS;
            SetStatus("OpenXR running, submitting");
        }
        // Submit cadence every 300 frames. LogRaw skips the probe line cap,
        // which per-draw capture exhausts in seconds.
        if (++submit_count_ % 300 == 0) {
            const DWORD now = GetTickCount();
            if (submit_window_start_ != 0)
                probe::LogRaw("openxr: 300 submits in %lu ms (%.1f fps)",
                              now - submit_window_start_,
                              300000.0f / (float)(now - submit_window_start_));
            submit_window_start_ = now;
        }
        return true;
    }

    void WaitPoses() override {
        if (!ready_)
            return;
        PumpEvents();
        if (!session_running_)
            return;
        if (frame_open_) {
            // SubmitFrame was skipped (e.g. targets not attached yet):
            // close the dangling frame so wait/begin stay paired.
            EndFrameEmpty();
        }
        XrFrameState state = {XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(session_, &wait, &state)))
            return;
        XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(session_, &begin)))
            return;
        frame_open_ = true;
        predicted_display_time_ = state.predictedDisplayTime;

        // Controller state once per frame, before the view locates so their
        // keep-last-pose early returns never starve input.
        SyncActions();

        // Locate what the game will render the NEXT frame with; these same
        // values are what SubmitFrame stamps on that frame's layer.
        XrViewState view_state = {XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = predicted_display_time_;
        locate.space = local_space_;
        uint32_t count = 0;
        if (XR_FAILED(xrLocateViews(session_, &locate, &view_state, 2, &count, views)) ||
            count != 2)
            return;
        const XrViewStateFlags need =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if ((view_state.viewStateFlags & need) != need)
            return;  // keep last good pose
        XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(view_space_, local_space_,
                                    predicted_display_time_, &head)) ||
            (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0)
            return;
        // Raw head pose in LOCAL space, kept for the menu anchor capture.
        head_xr_pose_ = head.pose;
        head_xr_valid_ = true;

        // Over-render margin: widen the FOV the game renders with and stamp
        // the layer with the same value (a mismatch mis-warps). Applied
        // here, where the projection, HUD remap and layer stamp all read.
        if (config.fov_margin > 1.001f) {
            for (int eye = 0; eye < 2; eye++) {
                XrFovf& fov = views[eye].fov;
                fov.angleLeft = atanf(tanf(fov.angleLeft) * config.fov_margin);
                fov.angleRight = atanf(tanf(fov.angleRight) * config.fov_margin);
                fov.angleUp = atanf(tanf(fov.angleUp) * config.fov_margin);
                fov.angleDown = atanf(tanf(fov.angleDown) * config.fov_margin);
            }
        }
        for (int eye = 0; eye < 2; eye++)
            render_view_[eye] = views[eye];
        render_views_valid_ = true;

        // Game-convention matrices (mirrors the OpenVR backend's math).
        // Meters first for the HUD inputs, then game units for the views.
        const D3DMATRIX head_m = FromXrPose(head.pose, 1.0f);
        head_pose_ = FromXrPose(head.pose, config.world_scale);
        head_pose_valid_ = true;
        const D3DMATRIX head_inverse = RigidInverse(head_pose_);
        for (int eye = 0; eye < 2; eye++) {
            const D3DMATRIX eye_m = FromXrPose(views[eye].pose, 1.0f);
            const D3DMATRIX eye_to_head_m = Multiply(eye_m, RigidInverse(head_m));
            eye_offset_m_[eye][0] = eye_to_head_m._41;
            eye_offset_m_[eye][1] = eye_to_head_m._42;
            eye_offset_m_[eye][2] = eye_to_head_m._43;
            eye_to_head_[eye] = eye_to_head_m;
            eye_to_head_[eye]._41 *= config.world_scale;
            eye_to_head_[eye]._42 *= config.world_scale;
            eye_to_head_[eye]._43 *= config.world_scale;
            eye_view_append_[eye] =
                Multiply(head_inverse, RigidInverse(eye_to_head_[eye]));
            // y-up view tangents, directly (OpenXR angles are y-up signed).
            raw_tangent_[eye][0] = tanf(views[eye].fov.angleLeft);
            raw_tangent_[eye][1] = tanf(views[eye].fov.angleRight);
            raw_tangent_[eye][2] = tanf(views[eye].fov.angleUp);
            raw_tangent_[eye][3] = tanf(views[eye].fov.angleDown);
        }
        eye_data_valid_ = true;
    }

    void GetEyeViewAppend(int eye, D3DMATRIX& out) override {
        out = eye_view_append_[eye & 1];
    }

    bool GetHeadPose(D3DMATRIX& out) override {
        if (!head_pose_valid_)
            return false;
        out = head_pose_;
        return true;
    }

    void GetEyeToHead(int eye, D3DMATRIX& out) override {
        out = eye_to_head_[eye & 1];
    }

    bool PollRecentered() override {
        const bool was = recentered_;
        recentered_ = false;
        return was;
    }

    void GetEyeProjection(int eye, D3DMATRIX& out) override {
        out = Identity();
        const float n = config.near_m * config.world_scale;
        const float f = config.far_m * config.world_scale;
        eye &= 1;
        float left = -1.0f, right = 1.0f, up = 1.0f, down = -1.0f;
        if (eye_data_valid_) {
            left = raw_tangent_[eye][0];
            right = raw_tangent_[eye][1];
            up = raw_tangent_[eye][2];
            down = raw_tangent_[eye][3];
        }
        out._11 = 2.0f / (right - left);
        out._22 = 2.0f / (up - down);
        out._44 = 0.0f;
        // Right-handed, -z forward, z clip [0,1] - the game's convention,
        // as in the OpenVR backend.
        out._31 = (right + left) / (right - left);
        out._32 = (up + down) / (up - down);
        out._33 = f / (n - f);
        out._34 = -1.0f;
        out._43 = n * f / (n - f);
    }

    void GetHudRemap(int eye, UINT eye_width, UINT eye_height,
                     float& scale_x, float& offset_x,
                     float& scale_y, float& offset_y) override {
        // Identical math to the OpenVR backend (same y-up tangent and
        // eye-offset inputs); see there for the derivation.
        scale_x = eye_width / 640.0f;
        offset_x = 0.0f;
        scale_y = eye_height / 480.0f;
        offset_y = 0.0f;
        eye &= 1;
        if (!eye_data_valid_)
            return;
        const float l = raw_tangent_[eye][0], r = raw_tangent_[eye][1];
        const float up = raw_tangent_[eye][2], down = raw_tangent_[eye][3];
        const float tx = eye_offset_m_[eye][0];
        const float ty = eye_offset_m_[eye][1];
        const float dist = config.hud_distance_m + eye_offset_m_[eye][2];
        if (dist < 0.05f || r - l < 1e-3f || up - down < 1e-3f)
            return;
        const float half_w = config.hud_distance_m *
                             tanf(config.hud_width_deg * (3.14159265f / 360.0f));
        const float half_h = half_w * (480.0f / 640.0f);
        auto ndc_x = [&](float u) {
            const float tan_x = (u * half_w - tx) / dist;
            return (2.0f * tan_x - r - l) / (r - l);
        };
        auto ndc_y = [&](float v) {
            const float tan_y = (v * half_h - ty) / dist;
            return (2.0f * tan_y - up - down) / (up - down);
        };
        const float px_left = (ndc_x(-1.0f) + 1.0f) * 0.5f * eye_width;
        const float px_right = (ndc_x(+1.0f) + 1.0f) * 0.5f * eye_width;
        const float py_top = (1.0f - ndc_y(+1.0f)) * 0.5f * eye_height;
        const float py_bottom = (1.0f - ndc_y(-1.0f)) * 0.5f * eye_height;
        scale_x = (px_right - px_left) / 640.0f;
        offset_x = px_left;
        scale_y = (py_bottom - py_top) / 480.0f;
        offset_y = py_top;
    }

    bool GetHandPose(int hand, D3DMATRIX& out) override {
        if (!hand_pose_valid_[hand & 1])
            return false;
        out = hand_pose_[hand & 1];
        return true;
    }

    bool GetHandAimPose(int hand, D3DMATRIX& out) override {
        if (!aim_pose_valid_[hand & 1])
            return false;
        out = aim_pose_[hand & 1];
        return true;
    }

    void HapticPulse(int hand, float seconds, float amplitude) override {
        const bool applied = input_focused_ && seconds > 0.0f;
        if (!applied)
            return;
        if (amplitude < 0.0f) amplitude = 0.0f;
        if (amplitude > 1.0f) amplitude = 1.0f;
        Pulse(hand, (XrDuration)(seconds * 1.0e9), amplitude);
        // Arm the defensive stop (StopStalePulses): the latest end wins.
        const int h = hand & 1;
        const double end = vrmod::NowSeconds() + (double)seconds;
        if (!pulse_live_[h] || end > pulse_end_[h])
            pulse_end_[h] = end;
        pulse_live_[h] = true;
    }

    // Tuning readout (psobbvr_hotkeys.hpp): rasterize two text lines into
    // a small texture shown as a head-locked quad below the HUD for
    // `seconds`. Render thread (as SubmitFrame), so no locking; swapchain
    // and texture are created on first use and live until
    // ReleaseSwapchains.
    void ShowTuneText(const char* line1, const char* line2,
                      float seconds) override {
        if (!ready_ || d3d11_device_ == nullptr || swapchain_format_ == 0)
            return;
        if (tune_swapchain_ == XR_NULL_HANDLE) {
            if (!CreateOneSwapchain(kTuneWidth, kTuneHeight, tune_swapchain_,
                                    tune_swapchain_image_,
                                    tune_swapchain_image_count_)) {
                probe::Log("openxr: tune swapchain creation failed");
                return;
            }
        }
        if (tune_texture_ == nullptr) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = kTuneWidth;
            desc.Height = kTuneHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = (DXGI_FORMAT)swapchain_format_;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr,
                                                      &tune_texture_))) {
                probe::Log("openxr: tune texture creation failed");
                return;
            }
        }
        RasterizeTuneText(line1, line2);
        tune_visible_until_ms_ =
            GetTickCount64() + (ULONGLONG)(seconds * 1000.0f);
    }

    // Teleport menu (psobbvr_teleport.hpp): rasterize a title + item list
    // into its own texture and keep the quad up until HideMenuText. Same
    // render-thread/lazy-creation contract as ShowTuneText above.
    void ShowMenuText(const char* title, const char* const* items, int count,
                      int selected) override {
        if (!ready_ || d3d11_device_ == nullptr || swapchain_format_ == 0)
            return;
        if (tele_swapchain_ == XR_NULL_HANDLE) {
            if (!CreateOneSwapchain(kTeleWidth, kTeleHeight, tele_swapchain_,
                                    tele_swapchain_image_,
                                    tele_swapchain_image_count_)) {
                probe::Log("openxr: teleport swapchain creation failed");
                return;
            }
        }
        if (tele_texture_ == nullptr) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = kTeleWidth;
            desc.Height = kTeleHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = (DXGI_FORMAT)swapchain_format_;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr,
                                                      &tele_texture_))) {
                probe::Log("openxr: teleport texture creation failed");
                return;
            }
        }
        RasterizeMenuText(title, items, count, selected);
        tele_visible_ = true;
    }

    // On-screen keyboard panel (psobbvr_vrkeyboard.hpp): upload the
    // caller's straight-alpha BGRA image into the panel texture (resources
    // created lazily, recreated on a size change) and pin the quad at the
    // given tracking-space pose. Render thread, like ShowMenuText.
    void ShowPanel(const uint32_t* bgra, UINT width, UINT height,
                   const D3DMATRIX& pose_m, float width_m) override {
        if (!ready_ || d3d11_device_ == nullptr || swapchain_format_ == 0 ||
            bgra == nullptr || width == 0 || height == 0)
            return;
        if (panel_swapchain_ != XR_NULL_HANDLE &&
            (panel_width_ != width || panel_height_ != height))
            ReleasePanel();
        if (panel_swapchain_ == XR_NULL_HANDLE) {
            if (!CreateOneSwapchain(width, height, panel_swapchain_,
                                    panel_swapchain_image_,
                                    panel_swapchain_image_count_)) {
                probe::Log("openxr: keyboard panel swapchain creation failed");
                return;
            }
            panel_width_ = width;
            panel_height_ = height;
        }
        if (panel_texture_ == nullptr) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = (DXGI_FORMAT)swapchain_format_;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr,
                                                      &panel_texture_))) {
                probe::Log("openxr: keyboard panel texture creation failed");
                return;
            }
        }
        d3d11_context_->UpdateSubresource(panel_texture_, 0, nullptr, bgra,
                                          width * 4, 0);
        panel_pose_ = ToXrPose(pose_m);
        panel_width_m_ = width_m;
        panel_visible_ = true;
    }

    void HidePanel() override {
        panel_visible_ = false;
        ray_visible_[0] = ray_visible_[1] = false;
    }

    // The keyboard's hand rays: remember each hand's beam for EndFrame.
    void SetPanelRay(int hand, bool visible, const D3DMATRIX& pose_m,
                     float length_m, float width_m) override {
        if (hand < 0 || hand > 1)
            return;
        ray_visible_[hand] = visible && length_m > 0.0f && width_m > 0.0f;
        if (!ray_visible_[hand])
            return;
        ray_pose_[hand] = ToXrPose(pose_m);
        ray_length_[hand] = length_m;
        ray_width_[hand] = width_m;
    }

    // The beam texture (straight-alpha BGRA, RAY_TEX_W across the beam x
    // RAY_TEX_H along it, row 0 = the hand end): a bright pale-cyan core
    // fading to clear at the edges, and thinning a little toward the
    // tip. Built once, lazily, with its own quad swapchain.
    static constexpr UINT RAY_TEX_W = 16, RAY_TEX_H = 256;
    bool EnsureRayTexture() {
        if (ray_texture_ != nullptr && ray_swapchain_ != XR_NULL_HANDLE)
            return true;
        if (!ready_ || d3d11_device_ == nullptr || swapchain_format_ == 0)
            return false;
        if (ray_swapchain_ == XR_NULL_HANDLE &&
            !CreateOneSwapchain(RAY_TEX_W, RAY_TEX_H, ray_swapchain_,
                                ray_swapchain_image_, ray_swapchain_image_count_)) {
            probe::Log("openxr: keyboard ray swapchain creation failed");
            return false;
        }
        if (ray_texture_ == nullptr) {
            static uint32_t px[RAY_TEX_W * RAY_TEX_H];
            for (UINT y = 0; y < RAY_TEX_H; y++) {
                const float along = ((float)y + 0.5f) / (float)RAY_TEX_H;
                for (UINT x = 0; x < RAY_TEX_W; x++) {
                    const float across = ((float)x + 0.5f) / (float)RAY_TEX_W * 2.0f - 1.0f;
                    float core = 1.0f - across * across;
                    core *= core;
                    const float a = core * (0.9f - 0.5f * along);
                    const uint32_t A = (uint32_t)(a * 255.0f + 0.5f);
                    const uint32_t R = (uint32_t)(90.0f + 165.0f * core);
                    const uint32_t G = (uint32_t)(200.0f + 55.0f * core);
                    const uint32_t B = 255u;
                    px[y * RAY_TEX_W + x] = (A << 24) | (R << 16) | (G << 8) | B;
                }
            }
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = RAY_TEX_W;
            desc.Height = RAY_TEX_H;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = (DXGI_FORMAT)swapchain_format_;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = {px, RAY_TEX_W * 4, 0};
            if (FAILED(d3d11_device_->CreateTexture2D(&desc, &init, &ray_texture_))) {
                probe::Log("openxr: keyboard ray texture creation failed");
                ray_texture_ = nullptr;
                return false;
            }
        }
        return true;
    }

    void ReleaseRays() {
        if (ray_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(ray_swapchain_);
            ray_swapchain_ = XR_NULL_HANDLE;
        }
        ray_swapchain_image_count_ = 0;
        memset(ray_swapchain_image_, 0, sizeof(ray_swapchain_image_));
        if (ray_texture_ != nullptr) {
            ray_texture_->Release();
            ray_texture_ = nullptr;
        }
        ray_visible_[0] = ray_visible_[1] = false;
    }

    void ReleasePanel() {
        ReleaseRays();
        if (panel_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(panel_swapchain_);
            panel_swapchain_ = XR_NULL_HANDLE;
        }
        panel_swapchain_image_count_ = 0;
        memset(panel_swapchain_image_, 0, sizeof(panel_swapchain_image_));
        if (panel_texture_ != nullptr) {
            panel_texture_->Release();
            panel_texture_ = nullptr;
        }
        panel_width_ = panel_height_ = 0;
        panel_visible_ = false;
    }

    // Game-convention rigid transform (row-vector, translation in meters)
    // -> XrPosef: the inverse of FromXrPose(pose, 1.0f) (standard
    // rotation-matrix-to-quaternion, largest-component form, on the
    // row-vector layout).
    static XrPosef ToXrPose(const D3DMATRIX& r) {
        XrPosef p;
        const float tr = r._11 + r._22 + r._33;
        float x, y, z, w;
        if (tr > 0.0f) {
            const float s = sqrtf(tr + 1.0f) * 2.0f;
            w = 0.25f * s;
            x = (r._23 - r._32) / s;
            y = (r._31 - r._13) / s;
            z = (r._12 - r._21) / s;
        } else if (r._11 > r._22 && r._11 > r._33) {
            const float s = sqrtf(1.0f + r._11 - r._22 - r._33) * 2.0f;
            w = (r._23 - r._32) / s;
            x = 0.25f * s;
            y = (r._12 + r._21) / s;
            z = (r._13 + r._31) / s;
        } else if (r._22 > r._33) {
            const float s = sqrtf(1.0f + r._22 - r._11 - r._33) * 2.0f;
            w = (r._31 - r._13) / s;
            x = (r._12 + r._21) / s;
            y = 0.25f * s;
            z = (r._23 + r._32) / s;
        } else {
            const float s = sqrtf(1.0f + r._33 - r._11 - r._22) * 2.0f;
            w = (r._12 - r._21) / s;
            x = (r._13 + r._31) / s;
            y = (r._23 + r._32) / s;
            z = 0.25f * s;
        }
        const float n = sqrtf(x * x + y * y + z * z + w * w);
        p.orientation = {x / n, y / n, z / n, w / n};
        p.position = {r._41, r._42, r._43};
        return p;
    }

    void HideMenuText() override { tele_visible_ = false; }

    bool GetControllerState(ControllerState& out) override {
        if (!actions_ready_ || !input_focused_)
            return false;
        out = cs_;
        return true;
    }

    const char* StatusLine() const override { return status_; }

private:
    // ---- OpenXR actions (controller input) ---------------------------------
    //
    // Action set, per-profile suggested bindings, action spaces, per-frame
    // sync. Button/trigger edges are always logged; the developer build
    // can add a once-a-second state summary. Haptics are driven game-side through
    // HapticPulse.

    static const char* SessionStateName(XrSessionState s) {
        switch (s) {
            case XR_SESSION_STATE_IDLE: return "IDLE";
            case XR_SESSION_STATE_READY: return "READY";
            case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
            case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
            case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
            case XR_SESSION_STATE_STOPPING: return "STOPPING";
            case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
            case XR_SESSION_STATE_EXITING: return "EXITING";
            default: return "UNKNOWN";
        }
    }

    bool InitActions() {
        if (XR_FAILED(xrStringToPath(instance_, "/user/hand/left", &hand_path_[0])) ||
            XR_FAILED(xrStringToPath(instance_, "/user/hand/right", &hand_path_[1])))
            return false;

        XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(asci.actionSetName, "gameplay");
        strcpy_s(asci.localizedActionSetName, "Gameplay");
        if (XR_FAILED(xrCreateActionSet(instance_, &asci, &action_set_))) {
            diag::Log("xrinput: xrCreateActionSet failed");
            return false;
        }
        const struct {
            XrAction* out;
            const char* name;
            const char* loc;
            XrActionType type;
        } defs[] = {
            {&act_grip_pose_, "grip_pose", "Hand grip pose", XR_ACTION_TYPE_POSE_INPUT},
            {&act_aim_pose_, "aim_pose", "Hand aim pose", XR_ACTION_TYPE_POSE_INPUT},
            {&act_trigger_, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT},
            {&act_squeeze_, "squeeze", "Grip squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
            {&act_stick_, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
            {&act_stick_click_, "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
            {&act_primary_, "button_primary", "A / X button", XR_ACTION_TYPE_BOOLEAN_INPUT},
            {&act_secondary_, "button_secondary", "B / Y button", XR_ACTION_TYPE_BOOLEAN_INPUT},
            {&act_menu_, "menu", "Menu button", XR_ACTION_TYPE_BOOLEAN_INPUT},
            {&act_haptic_, "haptic", "Haptic output", XR_ACTION_TYPE_VIBRATION_OUTPUT},
        };
        for (const auto& def : defs) {
            XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
            aci.actionType = def.type;
            strcpy_s(aci.actionName, def.name);
            strcpy_s(aci.localizedActionName, def.loc);
            aci.countSubactionPaths = 2;
            aci.subactionPaths = hand_path_;
            if (XR_FAILED(xrCreateAction(action_set_, &aci, def.out))) {
                diag::Log("xrinput: xrCreateAction(%s) failed", def.name);
                DestroyActions();
                return false;
            }
        }

        // Suggested bindings, most-specific profile first. The runtime picks
        // ONE profile per controller at runtime; SteamVR's per-app binding
        // UI can remap on top of whichever is active.
        struct Bind {
            XrAction a;
            const char* path;
        };
        // The G2 controllers' own profile (behind the hp extension): full
        // A/B/X/Y and analog squeeze.
        const Bind hp[] = {
            {act_grip_pose_, "/user/hand/left/input/grip/pose"},
            {act_grip_pose_, "/user/hand/right/input/grip/pose"},
            {act_aim_pose_, "/user/hand/left/input/aim/pose"},
            {act_aim_pose_, "/user/hand/right/input/aim/pose"},
            {act_trigger_, "/user/hand/left/input/trigger/value"},
            {act_trigger_, "/user/hand/right/input/trigger/value"},
            {act_squeeze_, "/user/hand/left/input/squeeze/value"},
            {act_squeeze_, "/user/hand/right/input/squeeze/value"},
            {act_stick_, "/user/hand/left/input/thumbstick"},
            {act_stick_, "/user/hand/right/input/thumbstick"},
            {act_stick_click_, "/user/hand/left/input/thumbstick/click"},
            {act_stick_click_, "/user/hand/right/input/thumbstick/click"},
            {act_primary_, "/user/hand/left/input/x/click"},
            {act_primary_, "/user/hand/right/input/a/click"},
            {act_secondary_, "/user/hand/left/input/y/click"},
            {act_secondary_, "/user/hand/right/input/b/click"},
            {act_menu_, "/user/hand/left/input/menu/click"},
            {act_menu_, "/user/hand/right/input/menu/click"},
            {act_haptic_, "/user/hand/left/output/haptic"},
            {act_haptic_, "/user/hand/right/output/haptic"},
        };
        // Classic WMR profile: no A/B/X/Y (those actions stay unbound
        // there - legal), squeeze is a click.
        const Bind wmr[] = {
            {act_grip_pose_, "/user/hand/left/input/grip/pose"},
            {act_grip_pose_, "/user/hand/right/input/grip/pose"},
            {act_aim_pose_, "/user/hand/left/input/aim/pose"},
            {act_aim_pose_, "/user/hand/right/input/aim/pose"},
            {act_trigger_, "/user/hand/left/input/trigger/value"},
            {act_trigger_, "/user/hand/right/input/trigger/value"},
            {act_squeeze_, "/user/hand/left/input/squeeze/click"},
            {act_squeeze_, "/user/hand/right/input/squeeze/click"},
            {act_stick_, "/user/hand/left/input/thumbstick"},
            {act_stick_, "/user/hand/right/input/thumbstick"},
            {act_stick_click_, "/user/hand/left/input/thumbstick/click"},
            {act_stick_click_, "/user/hand/right/input/thumbstick/click"},
            {act_menu_, "/user/hand/left/input/menu/click"},
            {act_menu_, "/user/hand/right/input/menu/click"},
            {act_haptic_, "/user/hand/left/output/haptic"},
            {act_haptic_, "/user/hand/right/output/haptic"},
        };
        // Meta/Oculus Touch (Quest). SteamVR also auto-maps FROM this
        // table for controller families we have no table for (Index,
        // Pico, Steam Frame). Touch has the menu button on the LEFT hand
        // only (the right has "system", runtime-reserved); A/B right, X/Y
        // left.
        const Bind touch[] = {
            {act_grip_pose_, "/user/hand/left/input/grip/pose"},
            {act_grip_pose_, "/user/hand/right/input/grip/pose"},
            {act_aim_pose_, "/user/hand/left/input/aim/pose"},
            {act_aim_pose_, "/user/hand/right/input/aim/pose"},
            {act_trigger_, "/user/hand/left/input/trigger/value"},
            {act_trigger_, "/user/hand/right/input/trigger/value"},
            {act_squeeze_, "/user/hand/left/input/squeeze/value"},
            {act_squeeze_, "/user/hand/right/input/squeeze/value"},
            {act_stick_, "/user/hand/left/input/thumbstick"},
            {act_stick_, "/user/hand/right/input/thumbstick"},
            {act_stick_click_, "/user/hand/left/input/thumbstick/click"},
            {act_stick_click_, "/user/hand/right/input/thumbstick/click"},
            {act_primary_, "/user/hand/left/input/x/click"},
            {act_primary_, "/user/hand/right/input/a/click"},
            {act_secondary_, "/user/hand/left/input/y/click"},
            {act_secondary_, "/user/hand/right/input/b/click"},
            {act_menu_, "/user/hand/left/input/menu/click"},
            {act_haptic_, "/user/hand/left/output/haptic"},
            {act_haptic_, "/user/hand/right/output/haptic"},
        };
        // HTC Vive wands. The trackpad stands in for the thumbstick (touch
        // position = deflection, click = stick click); squeeze is a click;
        // no A/B/X/Y, so those actions stay unbound (the [bindings] section
        // can move their game actions onto other chords).
        const Bind vive[] = {
            {act_grip_pose_, "/user/hand/left/input/grip/pose"},
            {act_grip_pose_, "/user/hand/right/input/grip/pose"},
            {act_aim_pose_, "/user/hand/left/input/aim/pose"},
            {act_aim_pose_, "/user/hand/right/input/aim/pose"},
            {act_trigger_, "/user/hand/left/input/trigger/value"},
            {act_trigger_, "/user/hand/right/input/trigger/value"},
            {act_squeeze_, "/user/hand/left/input/squeeze/click"},
            {act_squeeze_, "/user/hand/right/input/squeeze/click"},
            {act_stick_, "/user/hand/left/input/trackpad"},
            {act_stick_, "/user/hand/right/input/trackpad"},
            {act_stick_click_, "/user/hand/left/input/trackpad/click"},
            {act_stick_click_, "/user/hand/right/input/trackpad/click"},
            {act_menu_, "/user/hand/left/input/menu/click"},
            {act_menu_, "/user/hand/right/input/menu/click"},
            {act_haptic_, "/user/hand/left/output/haptic"},
            {act_haptic_, "/user/hand/right/output/haptic"},
        };
        // The khr/simple floor: poses + one button (select -> trigger).
        const Bind simple[] = {
            {act_grip_pose_, "/user/hand/left/input/grip/pose"},
            {act_grip_pose_, "/user/hand/right/input/grip/pose"},
            {act_aim_pose_, "/user/hand/left/input/aim/pose"},
            {act_aim_pose_, "/user/hand/right/input/aim/pose"},
            {act_trigger_, "/user/hand/left/input/select/click"},
            {act_trigger_, "/user/hand/right/input/select/click"},
            {act_menu_, "/user/hand/left/input/menu/click"},
            {act_menu_, "/user/hand/right/input/menu/click"},
            {act_haptic_, "/user/hand/left/output/haptic"},
            {act_haptic_, "/user/hand/right/output/haptic"},
        };
        auto suggest = [&](const char* profile, const Bind* binds, size_t n) {
            XrPath profile_path = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(instance_, profile, &profile_path)))
                return false;
            XrActionSuggestedBinding sb[24];
            uint32_t count = 0;
            for (size_t i = 0; i < n && count < 24; i++) {
                XrPath p = XR_NULL_PATH;
                if (XR_FAILED(xrStringToPath(instance_, binds[i].path, &p)))
                    continue;
                sb[count].action = binds[i].a;
                sb[count].binding = p;
                count++;
            }
            XrInteractionProfileSuggestedBinding spb = {
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            spb.interactionProfile = profile_path;
            spb.countSuggestedBindings = count;
            spb.suggestedBindings = sb;
            const XrResult r = xrSuggestInteractionProfileBindings(instance_, &spb);
            diag::Log("xrinput: bindings for %s: %s", profile,
                      XR_SUCCEEDED(r) ? "accepted" : "REJECTED");
            return XR_SUCCEEDED(r);
        };
        bool any = false;
        if (hp_profile_available_)
            any |= suggest("/interaction_profiles/hp/mixed_reality_controller",
                           hp, _countof(hp));
        any |= suggest("/interaction_profiles/oculus/touch_controller",
                       touch, _countof(touch));
        any |= suggest("/interaction_profiles/htc/vive_controller",
                       vive, _countof(vive));
        any |= suggest("/interaction_profiles/microsoft/motion_controller",
                       wmr, _countof(wmr));
        any |= suggest("/interaction_profiles/khr/simple_controller",
                       simple, _countof(simple));
        if (!any) {
            DestroyActions();
            return false;
        }

        XrSessionActionSetsAttachInfo attach = {
            XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &action_set_;
        if (XR_FAILED(xrAttachSessionActionSets(session_, &attach))) {
            diag::Log("xrinput: xrAttachSessionActionSets failed");
            DestroyActions();
            return false;
        }
        for (int h = 0; h < 2; h++) {
            XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
            si.subactionPath = hand_path_[h];
            si.poseInActionSpace.orientation.w = 1.0f;
            si.action = act_grip_pose_;
            xrCreateActionSpace(session_, &si, &grip_space_[h]);
            si.action = act_aim_pose_;
            xrCreateActionSpace(session_, &si, &aim_space_[h]);
        }
        actions_ready_ = true;
        diag::Log("xrinput: action set attached (hp profile %s)",
                  hp_profile_available_ ? "enabled" : "unavailable");
        return true;
    }

    void DestroyActions() {
        for (int h = 0; h < 2; h++) {
            if (grip_space_[h] != XR_NULL_HANDLE) {
                xrDestroySpace(grip_space_[h]);
                grip_space_[h] = XR_NULL_HANDLE;
            }
            if (aim_space_[h] != XR_NULL_HANDLE) {
                xrDestroySpace(aim_space_[h]);
                aim_space_[h] = XR_NULL_HANDLE;
            }
        }
        if (action_set_ != XR_NULL_HANDLE) {
            xrDestroyActionSet(action_set_);  // destroys its actions too
            action_set_ = XR_NULL_HANDLE;
        }
        act_grip_pose_ = act_aim_pose_ = act_trigger_ = act_squeeze_ =
            act_stick_ = act_stick_click_ = act_primary_ = act_secondary_ =
                act_menu_ = act_haptic_ = XR_NULL_HANDLE;
        actions_ready_ = false;
    }

    void LogCurrentProfiles() {
        for (int h = 0; h < 2; h++) {
            XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
            if (XR_FAILED(xrGetCurrentInteractionProfile(session_, hand_path_[h], &st)))
                continue;
            char buf[256] = "(none - controller off or unbound)";
            if (st.interactionProfile != XR_NULL_PATH) {
                uint32_t n = 0;
                xrPathToString(instance_, st.interactionProfile, sizeof(buf), &n, buf);
            }
            diag::Log("xrinput: %s hand profile: %s", h ? "right" : "left", buf);
        }
    }

    // Every pulse is fixed-length and should end by itself; an explicit
    // xrStopHapticFeedback shortly after its end stops a motor the runtime
    // left running. Per frame from SyncActions.
    bool pulse_live_[2] = {false, false};
    double pulse_end_[2] = {0.0, 0.0};
    void StopStalePulses() {
        if (!actions_ready_)
            return;
        const double now = vrmod::NowSeconds();
        for (int h = 0; h < 2; h++) {
            if (!pulse_live_[h] || now < pulse_end_[h] + 0.05)
                continue;
            XrHapticActionInfo hai = {XR_TYPE_HAPTIC_ACTION_INFO};
            hai.action = act_haptic_;
            hai.subactionPath = hand_path_[h];
            xrStopHapticFeedback(session_, &hai);
            pulse_live_[h] = false;
        }
    }

    void Pulse(int hand, XrDuration duration_ns, float amplitude) {
        if (!actions_ready_)
            return;
        XrHapticVibration vib = {XR_TYPE_HAPTIC_VIBRATION};
        vib.duration = duration_ns;
        vib.frequency = XR_FREQUENCY_UNSPECIFIED;
        vib.amplitude = amplitude;
        XrHapticActionInfo hai = {XR_TYPE_HAPTIC_ACTION_INFO};
        hai.action = act_haptic_;
        hai.subactionPath = hand_path_[hand & 1];
        xrApplyHapticFeedback(session_, &hai,
                              reinterpret_cast<XrHapticBaseHeader*>(&vib));
    }

    void SyncActions() {
        if (!actions_ready_ || !session_running_)
            return;
        StopStalePulses();
        XrActiveActionSet active = {};
        active.actionSet = action_set_;
        active.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        const XrResult xr = xrSyncActions(session_, &sync);
        // XR_SESSION_NOT_FOCUSED is a success code meaning "no input for
        // you" (dashboard open, another app focused). Synthesized state
        // must DECAY TO NEUTRAL here, never latch - we stop reporting.
        const bool focused = (xr == XR_SUCCESS);
        if (focused != input_focused_) {
            input_focused_ = focused;
            diag::Log("xrinput: %s", focused
                          ? "input live (session focused)"
                          : "input suspended (session not focused - dashboard?)");
        }
        if (!focused) {
            cs_ = ControllerState();  // decay to neutral, never latch
            hand_pose_valid_[0] = hand_pose_valid_[1] = false;
            aim_pose_valid_[0] = aim_pose_valid_[1] = false;
            return;
        }
        if (!profiles_logged_) {
            profiles_logged_ = true;
            LogCurrentProfiles();
        }

        float pos[2][3] = {};
        float trig_v[2] = {}, sq_v[2] = {}, stick_v[2][2] = {};
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        for (int h = 0; h < 2; h++) {
            const char* hand = h ? "right" : "left";
            gi.subactionPath = hand_path_[h];

            XrActionStatePose pose_state = {XR_TYPE_ACTION_STATE_POSE};
            gi.action = act_grip_pose_;
            xrGetActionStatePose(session_, &gi, &pose_state);
            const bool active_now = pose_state.isActive != XR_FALSE;
            if (active_now != ctl_active_[h]) {
                ctl_active_[h] = active_now;
                diag::Log("xrinput: %s controller %s", hand,
                          active_now ? "ACTIVE" : "inactive");
            }
            cs_.active[h] = active_now;

            XrActionStateFloat fs = {XR_TYPE_ACTION_STATE_FLOAT};
            gi.action = act_trigger_;
            xrGetActionStateFloat(session_, &gi, &fs);
            trig_v[h] = fs.currentState;
            gi.action = act_squeeze_;
            fs = {XR_TYPE_ACTION_STATE_FLOAT};
            xrGetActionStateFloat(session_, &gi, &fs);
            sq_v[h] = fs.currentState;
            XrActionStateVector2f vs = {XR_TYPE_ACTION_STATE_VECTOR2F};
            gi.action = act_stick_;
            xrGetActionStateVector2f(session_, &gi, &vs);
            stick_v[h][0] = vs.currentState.x;
            stick_v[h][1] = vs.currentState.y;
            cs_.trigger[h] = trig_v[h];
            cs_.squeeze[h] = sq_v[h];
            cs_.stick_x[h] = stick_v[h][0];
            cs_.stick_y[h] = stick_v[h][1];

            // Trigger edge (hysteresis) - logging only. No haptic echo
            // here: it would be weapon-blind (buzzing in menus). Haptics
            // live in psobbvr_haptics.hpp / psobbvr_gunfire.hpp /
            // psobbvr_controller.hpp, all through HapticPulse above.
            if (!trigger_down_[h] && trig_v[h] > 0.85f) {
                trigger_down_[h] = true;
                diag::Log("xrinput: %s trigger PULLED (%.2f)",
                          hand, trig_v[h]);
            } else if (trigger_down_[h] && trig_v[h] < 0.15f) {
                trigger_down_[h] = false;
                diag::Log("xrinput: %s trigger released", hand);
            }

            const struct {
                XrAction a;
                const char* name;
                bool* prev;
            } buttons[] = {
                {act_stick_click_, "stick-click", &prev_stick_click_[h]},
                {act_primary_, h ? "A" : "X", &prev_primary_[h]},
                {act_secondary_, h ? "B" : "Y", &prev_secondary_[h]},
                {act_menu_, "menu", &prev_menu_[h]},
            };
            for (const auto& btn : buttons) {
                XrActionStateBoolean bs = {XR_TYPE_ACTION_STATE_BOOLEAN};
                gi.action = btn.a;
                xrGetActionStateBoolean(session_, &gi, &bs);
                const bool now = bs.currentState != XR_FALSE;
                if (now != *btn.prev) {
                    *btn.prev = now;
                    diag::Log("xrinput: %s %s %s", hand, btn.name,
                              now ? "pressed" : "released");
                }
                if (btn.a == act_stick_click_) cs_.stick_click[h] = now;
                else if (btn.a == act_primary_) cs_.primary[h] = now;
                else if (btn.a == act_secondary_) cs_.secondary[h] = now;
                else if (btn.a == act_menu_) cs_.menu[h] = now;
            }

            // Grip pose in the head pose's LOCAL space. Inside-out tracking
            // keeps VALID set while extrapolating an out-of-view hand, so
            // the TRACKED bit is the honest signal for swing detection.
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            int track = 0;  // 0 = lost, 1 = extrapolated, 2 = tracked
            hand_pose_valid_[h] = false;
            if (grip_space_[h] != XR_NULL_HANDLE &&
                XR_SUCCEEDED(xrLocateSpace(grip_space_[h], local_space_,
                                           predicted_display_time_, &loc))) {
                if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                    track = (loc.locationFlags &
                             XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? 2 : 1;
                    pos[h][0] = loc.pose.position.x;
                    pos[h][1] = loc.pose.position.y;
                    pos[h][2] = loc.pose.position.z;
                    // The full grip pose, game convention, at the head
                    // pose's predicted display time.
                    if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
                        hand_pose_[h] = FromXrPose(loc.pose, config.world_scale);
                        hand_pose_valid_[h] = true;
                    }
                }
            }
            if (track != grip_track_state_[h]) {
                grip_track_state_[h] = track;
                diag::Log("xrinput: %s grip pose %s", hand,
                          track == 2 ? "TRACKED"
                                     : track == 1 ? "EXTRAPOLATED (valid, camera lost it)"
                                                  : "LOST (no pose)");
            }
            cs_.tracked[h] = (track == 2);

            // The aim pose, same space/time as the grip (the gun ray
            // source), validity from its own flags.
            aim_pose_valid_[h] = false;
            if (aim_space_[h] != XR_NULL_HANDLE) {
                XrSpaceLocation aloc = {XR_TYPE_SPACE_LOCATION};
                if (XR_SUCCEEDED(xrLocateSpace(aim_space_[h], local_space_,
                                               predicted_display_time_, &aloc)) &&
                    (aloc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (aloc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                    aim_pose_[h] = FromXrPose(aloc.pose, config.world_scale);
                    aim_pose_valid_[h] = true;
                }
            }
        }

    }

    void PumpEvents() {
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed =
                    *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                probe::Log("openxr: session state -> %d", (int)changed.state);
                // Rare (a handful per session) - also to the always-on log,
                // since FOCUSED is the gate for controller input.
                diag::Log("openxr: session state -> %s",
                          SessionStateName(changed.state));
                if (changed.state == XR_SESSION_STATE_READY && !session_running_) {
                    XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(session_, &begin))) {
                        session_running_ = true;
                        SetStatus("OpenXR session running");
                    }
                } else if (changed.state == XR_SESSION_STATE_STOPPING &&
                           session_running_) {
                    xrEndSession(session_);
                    session_running_ = false;
                    frame_open_ = false;
                    SetStatus("OpenXR session stopped (SteamVR ending?)");
                }
            } else if (event.type ==
                       XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                // Which binding profile the runtime actually picked per hand
                // (hp / classic WMR / touch / vive / simple).
                diag::Log("xrinput: interaction profile changed:");
                profiles_logged_ = false;  // re-log at the next focused sync
                LogCurrentProfiles();
            } else if (event.type ==
                       XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                recentered_ = true;
                // Menu world-lock: re-anchor the menu screen at the wearer's
                // post-recenter pose.
                menu_anchor_valid_ = false;
                probe::Log("openxr: reference space change (recenter)");
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    // Acquire/copy/release one image of a quad-layer swapchain. False when
    // the image could not be filled this frame.
    bool CopyIntoSwapchain(XrSwapchain sc, ID3D11Texture2D* const images[8],
                           ID3D11Texture2D* src) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrAcquireSwapchainImage(sc, &acquire, &index)))
            return false;
        bool copied = false;
        if (XR_SUCCEEDED(xrWaitSwapchainImage(sc, &wait))) {
            d3d11_context_->CopyResource(images[index], src);
            copied = true;
        }
        XrSwapchainImageReleaseInfo release = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc, &release);
        return copied;
    }

    // Tuning readout: GDI-render line1/line2 white-on-dark into the tune
    // texture. Text is composited on the CPU over a 55%-black backing and
    // stored as straight alpha (SteamVR multiplies rgb*alpha). Gray values
    // make the BGRA/RGBA channel order irrelevant.
    void RasterizeTuneText(const char* line1, const char* line2) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = (LONG)kTuneWidth;
        bmi.bmiHeader.biHeight = -(LONG)kTuneHeight;  // top-down rows
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr)
            return;
        HBITMAP bmp =
            CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp == nullptr || bits == nullptr) {
            DeleteDC(dc);
            return;
        }
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        memset(bits, 0, kTuneWidth * kTuneHeight * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        // Grayscale antialiasing, not ClearType - subpixel color fringes
        // would survive into the coverage read below.
        HFONT title_font = CreateFontA(
            -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HFONT value_font = CreateFontA(
            -52, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        RECT title_rc = {0, 4, (LONG)kTuneWidth, 44};
        RECT value_rc = {0, 44, (LONG)kTuneWidth, (LONG)kTuneHeight - 4};
        HGDIOBJ old_font = SelectObject(dc, title_font);
        DrawTextA(dc, line1, -1, &title_rc,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, value_font);
        DrawTextA(dc, line2, -1, &value_rc,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old_font);
        GdiFlush();
        uint8_t* px = (uint8_t*)bits;
        for (UINT i = 0; i < kTuneWidth * kTuneHeight; i++, px += 4) {
            const float text = px[0] / 255.0f;  // white text: any channel
            const float alpha = text + 0.55f * (1.0f - text);
            const float gray = text / alpha;  // white-over-black, straight
            px[0] = px[1] = px[2] = (uint8_t)(gray * 255.0f + 0.5f);
            px[3] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
        d3d11_context_->UpdateSubresource(tune_texture_, 0, nullptr, bits,
                                          kTuneWidth * 4, 0);
        SelectObject(dc, old_bmp);
        DeleteObject(value_font);
        DeleteObject(title_font);
        DeleteObject(bmp);
        DeleteDC(dc);
    }

    // Teleport menu: GDI-render a title plus item rows into the teleport
    // texture; the selected row gets a filled white bar (its text pixels
    // read as dark through the same coverage math - white-on-black
    // conversion below, identical to RasterizeTuneText's).
    void RasterizeMenuText(const char* title, const char* const* items,
                           int count, int selected) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = (LONG)kTeleWidth;
        bmi.bmiHeader.biHeight = -(LONG)kTeleHeight;  // top-down rows
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr)
            return;
        HBITMAP bmp =
            CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp == nullptr || bits == nullptr) {
            DeleteDC(dc);
            return;
        }
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        memset(bits, 0, kTeleWidth * kTeleHeight * 4);
        SetBkMode(dc, TRANSPARENT);
        // Grayscale antialiasing, not ClearType (same reason as the tune
        // quad: subpixel fringes would survive into the coverage read).
        HFONT title_font = CreateFontA(
            -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HFONT item_font = CreateFontA(
            -28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        const LONG title_h = 46;
        const LONG row_h = 38;
        SetTextColor(dc, RGB(255, 255, 255));
        HGDIOBJ old_font = SelectObject(dc, title_font);
        RECT title_rc = {0, 4, (LONG)kTeleWidth, title_h};
        DrawTextA(dc, title, -1, &title_rc,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, item_font);
        for (int i = 0; i < count; i++) {
            RECT rc = {24, title_h + 6 + i * row_h, (LONG)kTeleWidth - 24,
                       title_h + 6 + (i + 1) * row_h};
            if (rc.bottom > (LONG)kTeleHeight)
                break;
            if (i == selected) {
                RECT bar = {8, rc.top, (LONG)kTeleWidth - 8, rc.bottom};
                HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
                FillRect(dc, &bar, white);
                SetTextColor(dc, RGB(0, 0, 0));
            } else {
                SetTextColor(dc, RGB(255, 255, 255));
            }
            DrawTextA(dc, items[i], -1, &rc,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
        SelectObject(dc, old_font);
        GdiFlush();
        uint8_t* px = (uint8_t*)bits;
        for (UINT i = 0; i < kTeleWidth * kTeleHeight; i++, px += 4) {
            const float text = px[0] / 255.0f;  // white marks: any channel
            const float alpha = text + 0.55f * (1.0f - text);
            const float gray = text / alpha;  // white-over-black, straight
            px[0] = px[1] = px[2] = (uint8_t)(gray * 255.0f + 0.5f);
            px[3] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
        d3d11_context_->UpdateSubresource(tele_texture_, 0, nullptr, bits,
                                          kTeleWidth * 4, 0);
        SelectObject(dc, old_bmp);
        DeleteObject(item_font);
        DeleteObject(title_font);
        DeleteObject(bmp);
        DeleteDC(dc);
    }

    // Menu world-lock: the anchor is the head's LOCAL-space position with
    // the YAW component of its orientation only (quaternion x/z zeroed and
    // renormalized - the twist about the vertical axis), so the menu screen
    // stands level no matter where the wearer was looking at capture.
    void CaptureMenuAnchor() {
        XrQuaternionf q = head_xr_pose_.orientation;
        const float len = sqrtf(q.y * q.y + q.w * q.w);
        if (len > 1e-4f) {
            q.x = 0.0f;
            q.z = 0.0f;
            q.y /= len;
            q.w /= len;
        } else {
            q = {0.0f, 0.0f, 0.0f, 1.0f};  // looking straight up/down
        }
        menu_anchor_.orientation = q;
        menu_anchor_.position = head_xr_pose_.position;
        menu_anchor_valid_ = true;
        probe::Log("openxr: menu anchor captured at (%.2f %.2f %.2f)",
                   menu_anchor_.position.x, menu_anchor_.position.y,
                   menu_anchor_.position.z);
    }

    // Menu world-lock: place a quad menu_distance_m ahead of the anchor,
    // facing back at it, menu_width_deg across (4:3).
    void PlaceMenuQuad(XrCompositionLayerQuad& q, float d) {
        const XrQuaternionf& a = menu_anchor_.orientation;
        // The anchor's forward (-z) axis; a is yaw-only, so this is level.
        const float fx = -(2.0f * (a.x * a.z + a.w * a.y));
        const float fy = -(2.0f * (a.y * a.z - a.w * a.x));
        const float fz = -(1.0f - 2.0f * (a.x * a.x + a.y * a.y));
        q.space = local_space_;
        q.pose.orientation = a;
        q.pose.position.x = menu_anchor_.position.x + fx * d;
        q.pose.position.y = menu_anchor_.position.y + fy * d;
        q.pose.position.z = menu_anchor_.position.z + fz * d;
        q.size.width = 2.0f * d * tanf(config.menu_width_deg * (3.14159265f / 360.0f));
        q.size.height = q.size.width * (480.0f / 640.0f);
    }

    // End the open frame submitting nothing (keeps wait/begin/end paired
    // when a frame can't be filled).
    bool EndFrameEmpty() {
        XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
        end.displayTime = predicted_display_time_;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = 0;
        xrEndFrame(session_, &end);
        frame_open_ = false;
        return false;
    }

    bool CreateDeviceOnLuid(LUID luid) {
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
            return false;
        IDXGIAdapter1* adapter = nullptr;
        IDXGIAdapter1* candidate = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &candidate) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 desc;
            candidate->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == luid.LowPart &&
                desc.AdapterLuid.HighPart == luid.HighPart) {
                adapter = candidate;
                break;
            }
            candidate->Release();
        }
        factory->Release();
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                            D3D_FEATURE_LEVEL_10_0};
        const HRESULT hr = D3D11CreateDevice(
            adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, levels, 2, D3D11_SDK_VERSION, &d3d11_device_, nullptr,
            &d3d11_context_);
        if (adapter)
            adapter->Release();
        return SUCCEEDED(hr);
    }

    bool CreateSwapchains(UINT width, UINT height) {
        ReleaseSwapchains();
        // sRGB-typed BGRA preferred: bit-preserving CopyResource from the
        // game's A8R8G8B8 targets, displayed as the sRGB values they are.
        uint32_t format_count = 0;
        xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr);
        int64_t formats[64] = {};
        if (format_count > 64)
            format_count = 64;
        xrEnumerateSwapchainFormats(session_, 64, &format_count, formats);
        const int64_t wanted[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                  DXGI_FORMAT_B8G8R8A8_UNORM};
        swapchain_format_ = 0;
        for (int64_t want : wanted) {
            for (uint32_t i = 0; i < format_count && swapchain_format_ == 0; i++)
                if (formats[i] == want)
                    swapchain_format_ = want;
            if (swapchain_format_ != 0)
                break;
        }
        if (swapchain_format_ == 0) {
            SetStatus("no BGRA swapchain format offered (%u formats)", format_count);
            probe::Log("openxr: %s", status_);
            return false;
        }
        if (swapchain_format_ == DXGI_FORMAT_B8G8R8A8_UNORM)
            probe::Log("openxr: WARNING sRGB format unavailable - colors may wash out");
        for (int eye = 0; eye < 2; eye++) {
            if (!CreateOneSwapchain(width, height, swapchain_[eye],
                                    swapchain_image_[eye],
                                    swapchain_image_count_[eye])) {
                SetStatus("xrCreateSwapchain(eye %d) failed", eye);
                probe::Log("openxr: %s", status_);
                ReleaseSwapchains();
                return false;
            }
        }
        eye_width_ = width;
        eye_height_ = height;
        return true;
    }

    // One color swapchain in the already-picked format; fills the runtime-
    // owned image pointers. Used for every swapchain.
    bool CreateOneSwapchain(UINT width, UINT height, XrSwapchain& swapchain,
                            ID3D11Texture2D* image_out[8], uint32_t& count_out) {
        XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = swapchain_format_;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session_, &info, &swapchain)))
            return false;
        XrSwapchainImageD3D11KHR images[8];
        for (auto& image : images)
            image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        uint32_t image_count = 0;
        if (XR_FAILED(xrEnumerateSwapchainImages(
                swapchain, 8, &image_count,
                (XrSwapchainImageBaseHeader*)images))) {
            xrDestroySwapchain(swapchain);
            swapchain = XR_NULL_HANDLE;
            return false;
        }
        count_out = image_count;
        for (uint32_t i = 0; i < image_count && i < 8; i++)
            image_out[i] = images[i].texture;  // runtime-owned
        return true;
    }

    void ReleaseSwapchains() {
        for (int eye = 0; eye < 2; eye++) {
            if (swapchain_[eye] != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain_[eye]);
                swapchain_[eye] = XR_NULL_HANDLE;
            }
            swapchain_image_count_[eye] = 0;
            memset(swapchain_image_[eye], 0, sizeof(swapchain_image_[eye]));
        }
        if (hud_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(hud_swapchain_);
            hud_swapchain_ = XR_NULL_HANDLE;
        }
        hud_swapchain_image_count_ = 0;
        memset(hud_swapchain_image_, 0, sizeof(hud_swapchain_image_));
        if (menu_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(menu_swapchain_);
            menu_swapchain_ = XR_NULL_HANDLE;
        }
        menu_swapchain_image_count_ = 0;
        memset(menu_swapchain_image_, 0, sizeof(menu_swapchain_image_));
        if (tune_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(tune_swapchain_);
            tune_swapchain_ = XR_NULL_HANDLE;
        }
        tune_swapchain_image_count_ = 0;
        memset(tune_swapchain_image_, 0, sizeof(tune_swapchain_image_));
        if (tune_texture_ != nullptr) {
            tune_texture_->Release();
            tune_texture_ = nullptr;
        }
        tune_visible_until_ms_ = 0;
        if (tele_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(tele_swapchain_);
            tele_swapchain_ = XR_NULL_HANDLE;
        }
        tele_swapchain_image_count_ = 0;
        memset(tele_swapchain_image_, 0, sizeof(tele_swapchain_image_));
        if (tele_texture_ != nullptr) {
            tele_texture_->Release();
            tele_texture_ = nullptr;
        }
        tele_visible_ = false;
        ReleasePanel();
    }

    void ReleaseEyeTextures() {
        if (wide_texture_) {
            wide_texture_->Release();
            wide_texture_ = nullptr;
        }
    }

    void ReleaseHudTexture() {
        if (hud_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(hud_swapchain_);
            hud_swapchain_ = XR_NULL_HANDLE;
        }
        hud_swapchain_image_count_ = 0;
        memset(hud_swapchain_image_, 0, sizeof(hud_swapchain_image_));
        if (hud_texture_ != nullptr) {
            hud_texture_->Release();
            hud_texture_ = nullptr;
        }
        hud_width_ = hud_height_ = 0;
        hud_visible_ = false;
    }

    void ReleaseMenuTexture() {
        if (menu_swapchain_ != XR_NULL_HANDLE) {
            xrDestroySwapchain(menu_swapchain_);
            menu_swapchain_ = XR_NULL_HANDLE;
        }
        menu_swapchain_image_count_ = 0;
        memset(menu_swapchain_image_, 0, sizeof(menu_swapchain_image_));
        if (menu_texture_ != nullptr) {
            menu_texture_->Release();
            menu_texture_ = nullptr;
        }
        menu_width_ = menu_height_ = 0;
        menu_below_visible_ = menu_above_visible_ = false;
    }

    void ShutdownInstanceOnly() {
        if (instance_ != XR_NULL_HANDLE) {
            xrDestroyInstance(instance_);
            instance_ = XR_NULL_HANDLE;
        }
        system_ = XR_NULL_SYSTEM_ID;
    }

    void SetStatus(const char* format, ...) {
        va_list args;
        va_start(args, format);
        vsnprintf(status_, sizeof(status_), format, args);
        va_end(args);
    }

    void Fail(char* error, size_t error_len, const char* format, ...) {
        va_list args;
        va_start(args, format);
        vsnprintf(status_, sizeof(status_), format, args);
        va_end(args);
        if (error != nullptr && error_len > 0)
            strncpy_s(error, error_len, status_, _TRUNCATE);
        probe::Log("openxr: init failed: %s", status_);
        diag::Log("openxr: init failed: %s", status_);
    }

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_space_ = XR_NULL_HANDLE;
    XrSpace view_space_ = XR_NULL_HANDLE;
    bool session_running_ = false;
    bool frame_open_ = false;
    XrTime predicted_display_time_ = 0;

    ID3D11Device* d3d11_device_ = nullptr;
    ID3D11DeviceContext* d3d11_context_ = nullptr;
    ID3D11Texture2D* wide_texture_ = nullptr;  // both eyes side by side
    XrSwapchain swapchain_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    ID3D11Texture2D* swapchain_image_[2][8] = {};  // runtime-owned, not Released
    uint32_t swapchain_image_count_[2] = {};
    int64_t swapchain_format_ = 0;
    UINT eye_width_ = 0, eye_height_ = 0;

    // The dedicated HUD texture (shared from the game's D3D9Ex device)
    // and the quad-layer swapchain it is copied into.
    ID3D11Texture2D* hud_texture_ = nullptr;
    XrSwapchain hud_swapchain_ = XR_NULL_HANDLE;
    ID3D11Texture2D* hud_swapchain_image_[8] = {};  // runtime-owned
    uint32_t hud_swapchain_image_count_ = 0;
    UINT hud_width_ = 0, hud_height_ = 0;
    bool hud_visible_ = false;

    // Menu world-lock: the below-quad texture + swapchain, the per-frame
    // quad visibility, and the captured anchor.
    ID3D11Texture2D* menu_texture_ = nullptr;
    XrSwapchain menu_swapchain_ = XR_NULL_HANDLE;
    ID3D11Texture2D* menu_swapchain_image_[8] = {};  // runtime-owned
    uint32_t menu_swapchain_image_count_ = 0;
    UINT menu_width_ = 0, menu_height_ = 0;
    bool menu_below_visible_ = false;
    bool menu_above_visible_ = false;
    XrPosef menu_anchor_ = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    bool menu_anchor_valid_ = false;
    XrPosef head_xr_pose_ = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    bool head_xr_valid_ = false;
    // HUD lock (SetHudAnchor): the level LOCAL-space forward the gameplay
    // HUD quad faces along while Config::hud_lock is on.
    bool hud_anchor_valid_ = false;
    float hud_anchor_fx_ = 0.0f, hud_anchor_fz_ = -1.0f;

    // On-screen keyboard panel (ShowPanel): a caller-rasterized image in
    // its own texture + quad swapchain, pinned in LOCAL space at
    // panel_pose_ while panel_visible_.
    XrSwapchain panel_swapchain_ = XR_NULL_HANDLE;
    ID3D11Texture2D* panel_swapchain_image_[8] = {};  // runtime-owned
    uint32_t panel_swapchain_image_count_ = 0;
    ID3D11Texture2D* panel_texture_ = nullptr;
    UINT panel_width_ = 0, panel_height_ = 0;
    bool panel_visible_ = false;
    XrPosef panel_pose_ = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float panel_width_m_ = 0.8f;
    // The keyboard's hand rays (SetPanelRay): two quads over one beam
    // texture, drawn only while the panel is up.
    XrSwapchain ray_swapchain_ = XR_NULL_HANDLE;
    ID3D11Texture2D* ray_swapchain_image_[8] = {};  // runtime-owned
    uint32_t ray_swapchain_image_count_ = 0;
    ID3D11Texture2D* ray_texture_ = nullptr;
    bool ray_visible_[2] = {};
    XrPosef ray_pose_[2] = {};
    float ray_length_[2] = {}, ray_width_[2] = {};

    // In-headset tuning readout (ShowTuneText): our own CPU-written
    // texture + the quad swapchain it is copied into; the quad shows
    // until the deadline passes (0 = hidden).
    static constexpr UINT kTuneWidth = 512, kTuneHeight = 128;
    // Teleport menu quad (ShowMenuText): title band + up to 18 item rows.
    static constexpr UINT kTeleWidth = 512, kTeleHeight = 768;
    XrSwapchain tele_swapchain_ = XR_NULL_HANDLE;
    ID3D11Texture2D* tele_swapchain_image_[8] = {};  // runtime-owned
    uint32_t tele_swapchain_image_count_ = 0;
    ID3D11Texture2D* tele_texture_ = nullptr;
    bool tele_visible_ = false;
    ID3D11Texture2D* tune_texture_ = nullptr;
    XrSwapchain tune_swapchain_ = XR_NULL_HANDLE;
    ID3D11Texture2D* tune_swapchain_image_[8] = {};  // runtime-owned
    uint32_t tune_swapchain_image_count_ = 0;
    ULONGLONG tune_visible_until_ms_ = 0;

    XrView render_view_[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool render_views_valid_ = false;
    D3DMATRIX eye_view_append_[2] = {};
    D3DMATRIX head_pose_ = Identity();
    D3DMATRIX eye_to_head_[2] = {Identity(), Identity()};
    bool head_pose_valid_ = false;
    bool recentered_ = false;
    float raw_tangent_[2][4] = {};   // per eye: left, right, up, down (y-up)
    float eye_offset_m_[2][3] = {};  // eye-to-head translation, meters
    bool eye_data_valid_ = false;
    // The controller action set + per-hand log/edge state.
    XrActionSet action_set_ = XR_NULL_HANDLE;
    XrAction act_grip_pose_ = XR_NULL_HANDLE, act_aim_pose_ = XR_NULL_HANDLE;
    XrAction act_trigger_ = XR_NULL_HANDLE, act_squeeze_ = XR_NULL_HANDLE;
    XrAction act_stick_ = XR_NULL_HANDLE, act_stick_click_ = XR_NULL_HANDLE;
    XrAction act_primary_ = XR_NULL_HANDLE, act_secondary_ = XR_NULL_HANDLE;
    XrAction act_menu_ = XR_NULL_HANDLE, act_haptic_ = XR_NULL_HANDLE;
    XrPath hand_path_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace grip_space_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aim_space_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool actions_ready_ = false;
    bool hp_profile_available_ = false;
    bool input_focused_ = false;
    bool profiles_logged_ = false;
    bool ctl_active_[2] = {};
    int grip_track_state_[2] = {};  // 0 lost, 1 extrapolated, 2 tracked
    bool trigger_down_[2] = {};
    bool prev_stick_click_[2] = {}, prev_primary_[2] = {};
    bool prev_secondary_[2] = {}, prev_menu_[2] = {};
    ControllerState cs_;  // last focused sync's snapshot
    // Grip poses in game convention (see GetHandPose).
    D3DMATRIX hand_pose_[2] = {Identity(), Identity()};
    bool hand_pose_valid_[2] = {};
    // Aim poses, same convention (see GetHandAimPose).
    D3DMATRIX aim_pose_[2] = {Identity(), Identity()};
    bool aim_pose_valid_[2] = {};

    XrResult last_submit_error_ = XR_SUCCESS;
    uint32_t submit_count_ = 0;
    DWORD submit_window_start_ = 0;
    bool ready_ = false;
    char status_[192] = "VR not started (OpenXR)";
};

}  // namespace

VRInterface* GetOpenXRBackend() {
    static OpenXRBackend backend;
    return &backend;
}

}  // namespace vrmod
