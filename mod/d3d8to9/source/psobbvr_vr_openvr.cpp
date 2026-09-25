// OpenVR backend of the VR interface (psobbvr_vr.hpp), plus LoadConfig and
// the launch failure report.
//
// OpenVR's Submit rejects D3D9 textures, so the eye render target (shareable,
// on our D3D9Ex device) is opened on an in-process D3D11 device via
// OpenSharedResource and submitted as TextureType_DirectX. The D3D11 device
// is created on the default adapter (assumes one GPU).
//
// openvr_api.dll is delay-loaded: Init() probes for it with LoadLibrary
// first, so a missing DLL means "VR unavailable" instead of the game failing
// to start.
//
// Matrices: the game's projection has _34 = -1 (right-handed, -z forward,
// like OpenVR). OpenVR's HmdMatrix34_t is column-vector and the game is D3D
// row-vector, so conversion is a transpose, with translations scaled by
// config.world_scale (game units per meter).

#include "psobbvr_vr.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <d3d11.h>
#include <openvr.h>

namespace vrmod {

void LoadConfig() {
    char path[MAX_PATH];
    if (!GetCurrentDirectoryA(MAX_PATH, path))
        return;
    strcat_s(path, "\\psobbvr.ini");
    config.enabled = GetPrivateProfileIntA("vr", "enabled", 1, path) != 0;
    char backend[16] = "";
    GetPrivateProfileStringA("vr", "backend", "openxr", backend, sizeof(backend), path);
    config.backend_openxr = _stricmp(backend, "openxr") == 0;
    const int gamecam = GetPrivateProfileIntA("vr", "game_camera", 1, path);
    if (gamecam >= 0 && gamecam <= 2)
        config.game_camera = gamecam;
    config.hide_sun = GetPrivateProfileIntA("vr", "hide_sun", 1, path) != 0;
    config.trim_head = GetPrivateProfileIntA("vr", "trim_head", 1, path) != 0;
    const int body_cull = GetPrivateProfileIntA("vr", "body_cull", 1, path);
    if (body_cull >= 0 && body_cull <= 2)
        config.body_cull = body_cull;
    char value[32];
    auto read_float = [&](const char *key, float min, float max, float &out) {
        if (GetPrivateProfileStringA("vr", key, "", value, sizeof(value), path)) {
            const float parsed = (float)atof(value);
            if (parsed > min && parsed < max)
                out = parsed;
        }
    };
    read_float("world_scale", 0.01f, 1000.0f, config.world_scale);
    read_float("near_m", 0.001f, 100.0f, config.near_m);
    read_float("far_m", 1.0f, 100000.0f, config.far_m);
    read_float("hud_distance_m", 0.1f, 50.0f, config.hud_distance_m);
    read_float("hud_width_deg", 10.0f, 160.0f, config.hud_width_deg);
    read_float("text_scale", 0.049f, 3.001f, config.text_scale);
    config.hud_layer = GetPrivateProfileIntA("vr", "hud_layer", 1, path) != 0;
    const int hud_tex_w = GetPrivateProfileIntA("vr", "hud_tex_width", 1600, path);
    if (hud_tex_w >= 320 && hud_tex_w <= 4096)
        config.hud_tex_width = hud_tex_w;
    const int hud_lock = GetPrivateProfileIntA("vr", "hud_lock", 1, path);
    if (hud_lock >= 0 && hud_lock <= 2)
        config.hud_lock = hud_lock;
    config.vr_keyboard = GetPrivateProfileIntA("vr", "vr_keyboard", 1, path) != 0;
    read_float("keyboard_distance_m", 0.2f, 3.0f, config.keyboard_distance_m);
    read_float("keyboard_drop_m", -1.0f, 1.5f, config.keyboard_drop_m);
    read_float("keyboard_width_m", 0.2f, 3.0f, config.keyboard_width_m);
    config.menu_lock = GetPrivateProfileIntA("vr", "menu_lock", 1, path) != 0;
    read_float("menu_distance_m", 0.3f, 50.0f, config.menu_distance_m);
    read_float("menu_width_deg", 10.0f, 160.0f, config.menu_width_deg);
    read_float("menu_below_distance_m", 0.3f, 50.0f, config.menu_below_distance_m);
    read_float("menu_depth_scale", 0.199f, 10.001f, config.menu_depth_scale);
    read_float("menu_float_overlap", 0.009f, 1.001f, config.menu_float_overlap);
    config.glow_below = GetPrivateProfileIntA("vr", "glow_below", 1, path) != 0;
    config.burst_3d = GetPrivateProfileIntA("vr", "burst_3d", 1, path) != 0;
    config.object_vis_fix = GetPrivateProfileIntA("vr", "object_vis_fix", 1, path) != 0;
    config.mag_glitch_fix = GetPrivateProfileIntA("vr", "mag_glitch_fix", 1, path) != 0;
    config.mag_root_sync = GetPrivateProfileIntA("vr", "mag_root_sync", 1, path) != 0;
    config.name_labels = GetPrivateProfileIntA("vr", "name_labels", 1, path) != 0;
    config.trail_depth_fix = GetPrivateProfileIntA("vr", "trail_depth_fix", 1, path) != 0;
    config.trail_cross = GetPrivateProfileIntA("vr", "trail_cross", 1, path) != 0;
    config.controllers = GetPrivateProfileIntA("vr", "controllers", 1, path) != 0;
    config.allow_any_runtime =
        GetPrivateProfileIntA("vr", "allow_any_runtime", 0, path) != 0;
    config.vr_fail_message =
        GetPrivateProfileIntA("vr", "vr_fail_message", 1, path) != 0;
    config.vr_fail_exit =
        GetPrivateProfileIntA("vr", "vr_fail_exit", 1, path) != 0;
    config.hotkey_arm = GetPrivateProfileIntA("vr", "hotkey_arm", 1, path) != 0;
    read_float("hotkey_arm_timeout_s", 0.2f, 30.0f, config.hotkey_arm_timeout_s);
    read_float("controller_turn_min", 0.049f, 1.001f, config.controller_turn_min);
    read_float("controller_turn_max", 0.049f, 1.001f, config.controller_turn_max);
    read_float("controller_deadzone", 0.009f, 0.6f, config.controller_deadzone);
    read_float("controller_press", 0.199f, 0.95f, config.controller_press);
    if (GetPrivateProfileIntA("vr", "controller_side_sign", 1, path) < 0)
        config.controller_side_sign = -1.0f;
    config.stick_locomotion = GetPrivateProfileIntA("vr", "stick_locomotion", 1, path) != 0;
    read_float("stick_turn_deg_s", 9.9f, 360.001f, config.stick_turn_deg_s);
    read_float("stick_speed_floor", 0.009f, 1.001f, config.stick_speed_floor);
    const int head_move = GetPrivateProfileIntA("vr", "head_move", 0, path);
    if (head_move >= 0 && head_move <= 2)
        config.head_move = head_move;
    read_float("eye_height_m", 0.1f, 5.0f, config.eye_height_m);
    // Dynamic eye height (see Config::eye_height_auto).
    config.eye_height_auto =
        GetPrivateProfileIntA("vr", "eye_height_auto", 1, path) != 0 ? 1 : 0;
    read_float("eye_offset_m", -0.5f, 0.5f, config.eye_offset_m);
    read_float("trail_cross_gain", -0.01f, 1.01f, config.trail_cross_gain);
    read_float("eye_forward_m", -0.5f, 0.5f, config.eye_forward_m);
    read_float("run_forward_m", -0.001f, 0.5f, config.run_forward_m);
    read_float("run_down_m", -0.001f, 0.5f, config.run_down_m);
    read_float("fov_margin", 0.999f, 2.001f, config.fov_margin);
    read_float("cull_fov_scale", 0.05f, 1.0f, config.cull_fov_scale);
    read_float("cull_fov_scale_town", 0.0f, 1.0f, config.cull_fov_scale_town);
    read_float("draw_distance_scale", 0.24f, 16.001f, config.draw_distance_scale);
    read_float("turn_speed_scale", 0.049f, 1.001f, config.turn_speed_scale);
    config.back_strafe = GetPrivateProfileIntA("vr", "back_strafe", 1, path) != 0;
    read_float("back_speed_scale", 0.299f, 1.001f, config.back_speed_scale);
    config.side_turn = GetPrivateProfileIntA("vr", "side_turn", 1, path) != 0;
    read_float("side_turn_speed_scale", 0.009f, 1.001f, config.side_turn_speed_scale);
    read_float("trim_radius_m", 0.01f, 2.0f, config.trim_radius_m);
    read_float("trim_wide_m", 0.1f, 5.0f, config.trim_wide_m);
    read_float("trim_anchor_m", 0.5f, 3.0f, config.trim_anchor_m);
    // Target-selection aim steering (see Config::target_aim). 0-7 bitmask
    // (bit 4 = tech banks on the left ray); out-of-range falls back to 7.
    {
        const int ta = GetPrivateProfileIntA("vr", "target_aim", 7, path);
        config.target_aim = (ta >= 0 && ta <= 7) ? ta : 7;
    }
    read_float("target_aim_cone_scale", 0.199f, 1.501f,
               config.target_aim_cone_scale);
    // VR spellcasting (see Config::cast_swing).
    config.cast_swing =
        GetPrivateProfileIntA("vr", "cast_swing", 1, path) != 0;
    config.cast_left_hand =
        GetPrivateProfileIntA("vr", "cast_left_hand", 1, path) != 0;
    config.cast_facing_snap =
        GetPrivateProfileIntA("vr", "cast_facing_snap", 0, path) != 0;
    read_float("cast_aim_cone_deg", 4.999f, 90.001f,
               config.cast_aim_cone_deg);
    // -1 = auto, else a tech-id bitmask (see Config::cast_aim_techs);
    // decimal or 0x hex.
    {
        char buf[32] = {};
        GetPrivateProfileStringA("vr", "cast_aim_techs", "-1", buf,
                                 sizeof(buf), path);
        char* end = nullptr;
        const long m = strtol(buf, &end, 0);
        config.cast_aim_techs =
            (end != buf && m >= -1 && m <= 0x7FFFF) ? (int)m : -1;
    }
    // Honest gun bullet origin (see Config::gun_fire_origin).
    config.gun_fire_origin =
        GetPrivateProfileIntA("vr", "gun_fire_origin", 1, path) != 0;
    read_float("gun_muzzle_offset_m", -0.501f, 0.501f,
               config.gun_muzzle_offset_m);
    read_float("gun_barrel_pitch_deg", -45.001f, 45.001f,
               config.gun_barrel_pitch_deg);
    config.gun_facing_snap =
        GetPrivateProfileIntA("vr", "gun_facing_snap", 0, path) != 0;
    config.attack_view_hold =
        GetPrivateProfileIntA("vr", "attack_view_hold", 1, path) != 0;
    config.attack_retarget =
        GetPrivateProfileIntA("vr", "attack_retarget", 1, path) != 0;
    read_float("retarget_cone_deg", 4.999f, 180.001f, config.retarget_cone_deg);
    // Per-hand mechguns (see Config::mechgun_dual).
    config.mechgun_dual =
        GetPrivateProfileIntA("vr", "mechgun_dual", 1, path) != 0;
    config.mechgun_swap_barrels =
        GetPrivateProfileIntA("vr", "mechgun_swap_barrels", 0, path) != 0;
    config.mechgun_reticle =
        GetPrivateProfileIntA("vr", "mechgun_reticle", 1, path) != 0;
    config.mechgun_union =
        GetPrivateProfileIntA("vr", "mechgun_union", 1, path) != 0;
    config.mechgun_merge_popups =
        GetPrivateProfileIntA("vr", "mechgun_merge_popups", 1, path) != 0;
    // Combat haptics (see Config::gun_haptic and psobbvr_haptics.hpp).
    {
        const int gh = GetPrivateProfileIntA("vr", "gun_haptic", 2, path);
        config.gun_haptic = (gh >= 0 && gh <= 2) ? gh : 2;
    }
    read_float("gun_haptic_amp", 0.049f, 1.001f, config.gun_haptic_amp);
    read_float("gun_haptic_s", 0.009f, 0.501f, config.gun_haptic_s);
    config.hit_haptic =
        GetPrivateProfileIntA("vr", "hit_haptic", 1, path) != 0;
    read_float("hit_haptic_amp", 0.049f, 1.001f, config.hit_haptic_amp);
    read_float("hit_haptic_s", 0.009f, 0.501f, config.hit_haptic_s);
    config.hurt_haptic =
        GetPrivateProfileIntA("vr", "hurt_haptic", 1, path) != 0;
    read_float("hurt_haptic_amp", 0.049f, 1.001f, config.hurt_haptic_amp);
    read_float("hurt_haptic_s", 0.009f, 0.501f, config.hurt_haptic_s);
    read_float("sprite_near_floor", 0.499f, 20.001f,
               config.sprite_near_floor);
    config.ui_caller_rule = GetPrivateProfileIntA("vr", "ui_caller_rule", 1, path) != 0;
    config.alpha_sprite_rule = GetPrivateProfileIntA("vr", "alpha_sprite_rule", 1, path) != 0;
    read_float("alpha_sprite_floor", -0.001f, 20.001f, config.alpha_sprite_floor);
    config.sprite_upright = GetPrivateProfileIntA("vr", "sprite_upright", 1, path) != 0;
    {
        char buf[256] = {};
        // Default 24 = the telepipe beam pool.
        GetPrivateProfileStringA("vr", "sprite_upright_pools", "24", buf, sizeof(buf), path);
        config.SetUprightPools(buf);
    }
    // Armed-swing mode (see Config::swing_attack).
    {
        const int sw = GetPrivateProfileIntA("vr", "swing_attack", 2, path);
        config.swing_attack = (sw >= 0 && sw <= 2) ? sw : 2;
    }
    config.swing_unarmed = GetPrivateProfileIntA("vr", "swing_unarmed", 1, path) != 0;
    read_float("swing_onset", 0.199f, 10.001f, config.swing_onset);
    read_float("swing_release", 0.049f, 10.001f, config.swing_release);
    read_float("swing_refractory_s", -0.001f, 2.001f, config.swing_refractory_s);
    read_float("swing_glitch_cap", 1.999f, 100.001f, config.swing_glitch_cap);
    // Charge hold (see Config::charge_hold).
    config.charge_hold =
        GetPrivateProfileIntA("vr", "charge_hold", 1, path) != 0;
    read_float("charge_hold_timeout_s", -0.001f, 60.001f,
               config.charge_hold_timeout_s);
    // Swing warp (see Config::swing_warp_margin).
    read_float("swing_warp_margin", -0.001f, 10.001f,
               config.swing_warp_margin);
    // Combo-window stretch (see Config::combo_window_bonus).
    {
        const int cw =
            GetPrivateProfileIntA("vr", "combo_window_bonus", 6, path);
        if (cw >= 0 && cw <= 60)
            config.combo_window_bonus = cw;
    }
    // The multi-hit per-row hold (see Config::swing_row_hold).
    config.swing_row_hold =
        GetPrivateProfileIntA("vr", "swing_row_hold", 1, path) != 0;
    read_float("swing_row_hold_margin", 0.999f, 10.001f,
               config.swing_row_hold_margin);
    {
        const int cap =
            GetPrivateProfileIntA("vr", "swing_row_hold_cap", 30, path);
        if (cap >= 0 && cap <= 300)
            config.swing_row_hold_cap = cap;
    }
    // The left hand's swing with a twin weapon (see Config::swing_left_hand).
    config.swing_left_hand =
        GetPrivateProfileIntA("vr", "swing_left_hand", 1, path) != 0;
    // Swing-timing indicator (see Config::swing_indicator).
    config.swing_indicator =
        GetPrivateProfileIntA("vr", "swing_indicator", 1, path) != 0;
    read_float("swing_indicator_x", -0.001f, 640.001f, config.swing_indicator_x);
    read_float("swing_indicator_y", -0.001f, 480.001f, config.swing_indicator_y);
    read_float("swing_indicator_ring", 0.999f, 400.001f, config.swing_indicator_ring);
    read_float("swing_indicator_ring_end", -0.001f, 400.001f,
               config.swing_indicator_ring_end);
    read_float("swing_indicator_scale", 0.099f, 10.001f, config.swing_indicator_scale);
    {
        const int spin =
            GetPrivateProfileIntA("vr", "swing_indicator_spin", 1, path);
        if (spin >= 1 && spin <= 30)
            config.swing_indicator_spin = spin;
    }
    read_float("trig_pair_window_s", 0.049f, 1.001f,
               config.trig_pair_window_s);
    const int sw_conf = GetPrivateProfileIntA("vr", "swing_confirm", 2, path);
    if (sw_conf >= 1 && sw_conf <= 10)
        config.swing_confirm = sw_conf;
    // First-person arm hide (see Config::hide_arms).
    config.hide_arms = GetPrivateProfileIntA("vr", "hide_arms", 1, path) != 0;
    const int arm_n = GetPrivateProfileIntA("vr", "hide_arms_count", 58, path);
    if (arm_n >= 0 && arm_n <= 400)
        config.hide_arms_count = arm_n;
    // Weapon on the right grip pose (see Config::weapon_grip). Offsets
    // may legitimately be zero or negative.
    config.weapon_grip = GetPrivateProfileIntA("vr", "weapon_grip", 1, path) != 0;
    // Dual-wield split - off-hand half onto the left grip pose.
    config.twin_split = GetPrivateProfileIntA("vr", "twin_split", 1, path) != 0;
    read_float("weapon_scale", 0.099f, 2.001f, config.weapon_scale);
    // Motion-controlled hands (psobbvr_hands.hpp).
    config.hand_presence = GetPrivateProfileIntA("vr", "hand_presence", 1, path) != 0;
    read_float("hand_scale", 0.099f, 2.001f, config.hand_scale);
    config.hand_cull_flip = GetPrivateProfileIntA("vr", "hand_cull_flip", 0, path) != 0;
    read_float("hand_pitch_deg", -180.001f, 180.001f, config.hand_pitch_deg);
    read_float("hand_roll_deg", -180.001f, 180.001f, config.hand_roll_deg);
    read_float("hand_yaw_deg", -180.001f, 180.001f, config.hand_yaw_deg);
    read_float("hand_fwd_cm", -50.001f, 50.001f, config.hand_fwd_cm);
    read_float("hand_up_cm", -50.001f, 50.001f, config.hand_up_cm);
    read_float("grip_pitch_deg", -180.001f, 180.001f, config.grip_pitch_deg);
    read_float("grip_roll_deg", -180.001f, 180.001f, config.grip_roll_deg);
    read_float("grip_yaw_deg", -180.001f, 180.001f, config.grip_yaw_deg);
    read_float("grip_fwd_cm", -50.001f, 50.001f, config.grip_fwd_cm);
    read_float("grip_up_cm", -50.001f, 50.001f, config.grip_up_cm);
    read_float("gun_pitch_deg", -180.001f, 180.001f, config.gun_pitch_deg);
    read_float("gun_roll_deg", -180.001f, 180.001f, config.gun_roll_deg);
    read_float("gun_yaw_deg", -180.001f, 180.001f, config.gun_yaw_deg);
    read_float("gun_fwd_cm", -50.001f, 50.001f, config.gun_fwd_cm);
    read_float("gun_up_cm", -50.001f, 50.001f, config.gun_up_cm);
    read_float("fist_pitch_deg", -180.001f, 180.001f, config.fist_pitch_deg);
    read_float("fist_roll_deg", -180.001f, 180.001f, config.fist_roll_deg);
    read_float("fist_yaw_deg", -180.001f, 180.001f, config.fist_yaw_deg);
    read_float("fist_fwd_cm", -50.001f, 50.001f, config.fist_fwd_cm);
    read_float("fist_up_cm", -50.001f, 50.001f, config.fist_up_cm);
    read_float("fist_side_cm", -50.001f, 50.001f, config.fist_side_cm);

    // Launch summary for bug reports: the build, the ini values that shape
    // a session, and the files the VR path depends on (psobbvr-vr.log).
#ifndef PSOBBVR_GIT_REV
#define PSOBBVR_GIT_REV "unknown"
#endif
    char section[512];
    const DWORD bind_n =
        GetPrivateProfileSectionA("bindings", section, sizeof(section), path);
    diag::Log("launch: psobbvr d3d8 build %s (compiled %s %s); ini %s",
              PSOBBVR_GIT_REV, __DATE__, __TIME__, path);
    diag::Log("launch: [vr] enabled=%d backend=%s controllers=%d "
              "stick_locomotion=%d head_move=%d world_scale=%.2f eye_height_auto=%d "
              "eye_offset_m=%.2f hand_presence=%d hud_layer=%d hud_lock=%d menu_lock=%d "
              "allow_any_runtime=%d vr_fail_message=%d vr_fail_exit=%d; [bindings] %s",
              config.enabled ? 1 : 0, config.backend_openxr ? "openxr" : "openvr",
              config.controllers ? 1 : 0, config.stick_locomotion ? 1 : 0,
              config.head_move,
              config.world_scale, config.eye_height_auto, config.eye_offset_m,
              config.hand_presence ? 1 : 0, config.hud_layer ? 1 : 0,
              config.hud_lock, config.menu_lock ? 1 : 0, config.allow_any_runtime ? 1 : 0,
              config.vr_fail_message ? 1 : 0, config.vr_fail_exit ? 1 : 0,
              bind_n > 0 ? "section present" : "section absent (defaults)");
    diag::Log("launch: openvr_api.dll %s, openxr_loader.dll %s",
              GetFileAttributesA("openvr_api.dll") != INVALID_FILE_ATTRIBUTES
                  ? "present" : "MISSING",
              GetFileAttributesA("openxr_loader.dll") != INVALID_FILE_ATTRIBUTES
                  ? "present" : "MISSING");
}

// The box was shown and dismissed - leave now rather than run flat.
// We are inside the game's first CreateDevice; nothing of the game's own
// state exists yet, so ExitProcess is clean enough (the log file handles
// are flushed per line).
static void ExitAfterFailureBox() {
    if (!config.vr_fail_exit)
        return;
    diag::Log("vr: exiting after the failure box (vr_fail_exit=1)");
    ExitProcess(1);
}

void ReportLaunchFailure(const char* error) {
    diag::Log("vr: launch init failed: %s", error);
    if (!config.vr_fail_message ||
        GetEnvironmentVariableA("PSOBBVR_QUIET", nullptr, 0) != 0) {
        diag::Log("vr: failure message box suppressed (%s)",
                  config.vr_fail_message ? "PSOBBVR_QUIET" : "vr_fail_message=0");
        return;
    }
    // "SteamVR not running" gets a short box with just that instruction.
    // The full box mentions the flat run only when it will happen
    // (vr_fail_exit=0).
    const char* after = config.vr_fail_exit
        ? ""
        : "The game will run FLAT in a window (no VR) this time.\n\n";
    if (strncmp(error, "SteamVR not running", 19) == 0) {
        MessageBoxA(nullptr, error, "PSOBB VR - VR did not start",
                    MB_ICONWARNING | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
        ExitAfterFailureBox();
        return;
    }
    char text[1400];
    snprintf(text, sizeof(text),
             "PSOBB VR could not start the headset:\n\n%s\n\n%s"
             "Checklist:\n"
             "  1. SteamVR is installed and up to date (2.17 or later; "
             "the stable channel is fine).\n"
             "  2. SteamVR is set as the OpenXR runtime (SteamVR Settings > "
             "OpenXR > Set SteamVR as OpenXR runtime). The Meta Quest Link "
             "app switches this back to its own runtime after updates.\n"
             "  3. The headset is connected and SteamVR shows it (Quest: "
             "connect through Steam Link, then launch the game).\n\n"
             "Then relaunch. The psobbvr-*.log files in the game folder have "
             "the details - send them with a bug report.",
             error, after);
    MessageBoxA(nullptr, text, "PSOBB VR - VR did not start",
                MB_ICONWARNING | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
    ExitAfterFailureBox();
}

namespace {

// OpenVR 3x4 (column-vector) -> D3D row-vector rigid transform; translation
// scaled from meters to game units.
D3DMATRIX FromHmd34(const vr::HmdMatrix34_t &m, float scale) {
    D3DMATRIX r = Identity();
    r._11 = m.m[0][0]; r._12 = m.m[1][0]; r._13 = m.m[2][0];
    r._21 = m.m[0][1]; r._22 = m.m[1][1]; r._23 = m.m[2][1];
    r._31 = m.m[0][2]; r._32 = m.m[1][2]; r._33 = m.m[2][2];
    r._41 = m.m[0][3] * scale; r._42 = m.m[1][3] * scale; r._43 = m.m[2][3] * scale;
    return r;
}

// ---- the backend --------------------------------------------------------

class OpenVRBackend : public VRInterface {
public:
    bool Init(char *error, size_t error_len, bool quiet_probe) override {
        if (ready_) {
            SetStatus("VR already running");
            return true;
        }
        // Delay-load guard: touching vr::VR_Init without the DLL present
        // would raise a loader exception instead of failing cleanly.
        if (LoadLibraryA("openvr_api.dll") == nullptr) {
            Fail(error, error_len, "openvr_api.dll not found next to the game exe");
            return false;
        }
        if (quiet_probe && !vr::VR_IsHmdPresent()) {
            Fail(error, error_len,
                 "no headset detected - VR not started (panel button forces a try)");
            return false;
        }
        vr::EVRInitError init_error = vr::VRInitError_None;
        system_ = vr::VR_Init(&init_error, vr::VRApplication_Scene);
        if (system_ == nullptr) {
            Fail(error, error_len, "VR_Init: %s",
                 vr::VR_GetVRInitErrorAsEnglishDescription(init_error));
            return false;
        }
        if (vr::VRCompositor() == nullptr) {
            Fail(error, error_len, "no compositor (is SteamVR fully running?)");
            vr::VR_Shutdown();
            system_ = nullptr;
            return false;
        }
        vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseSeated);

        if (d3d11_device_ == nullptr) {
            const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
            const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                 levels, 2, D3D11_SDK_VERSION,
                                                 &d3d11_device_, nullptr, &d3d11_context_);
            if (FAILED(hr)) {
                Fail(error, error_len, "D3D11CreateDevice failed 0x%08lX", hr);
                vr::VR_Shutdown();
                system_ = nullptr;
                return false;
            }
        }

        for (int eye = 0; eye < 2; eye++)
            eye_view_append_[eye] = Identity();
        RefreshEyeData();
        ready_ = true;
        SetStatus("VR running, no eye textures attached yet");
        probe::Log("vr: OpenVR initialized");

        // Diagnostics: the driver's per-eye tangents, and the runtime's own
        // projection (transposed to row-vector) next to ours. A sign
        // mismatch in _31/_32, or a positive eye_to_head_x for the left
        // eye, points at swapped/mirrored eyes.
        for (int eye = 0; eye < 2; eye++) {
            const vr::EVREye e = eye == 0 ? vr::Eye_Left : vr::Eye_Right;
            float left, right, top, bottom;
            system_->GetProjectionRaw(e, &left, &right, &top, &bottom);
            const vr::HmdMatrix34_t eth = system_->GetEyeToHeadTransform(e);
            probe::Log("vr: eye %d raw l=%.4f r=%.4f t=%.4f b=%.4f eye_to_head_x=%.4f",
                       eye, left, right, top, bottom, eth.m[0][3]);
            const vr::HmdMatrix44_t rp = system_->GetProjectionMatrix(
                e, config.near_m * config.world_scale, config.far_m * config.world_scale);
            D3DMATRIX mine;
            GetEyeProjection(eye, mine);
            probe::Log("vr: eye %d proj runtime _11=%.4f _31=%+.4f _22=%.4f _32=%+.4f _33=%.4f _43=%.2f _34=%+.1f",
                       eye, rp.m[0][0], rp.m[0][2], rp.m[1][1], rp.m[1][2],
                       rp.m[2][2], rp.m[2][3], rp.m[3][2]);
            probe::Log("vr: eye %d proj ours    _11=%.4f _31=%+.4f _22=%.4f _32=%+.4f _33=%.4f _43=%.2f _34=%+.1f",
                       eye, mine._11, mine._31, mine._22, mine._32,
                       mine._33, mine._43, mine._34);
        }
        return true;
    }

    void Shutdown() override {
        ReleaseEyeTextures();
        if (d3d11_context_) { d3d11_context_->Release(); d3d11_context_ = nullptr; }
        if (d3d11_device_) { d3d11_device_->Release(); d3d11_device_ = nullptr; }
        if (system_ != nullptr) {
            vr::VR_Shutdown();
            system_ = nullptr;
        }
        eye_data_valid_ = false;
        head_pose_valid_ = false;
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
            reinterpret_cast<void **>(&wide_texture_));
        if (FAILED(hr) || wide_texture_ == nullptr) {
            SetStatus("OpenSharedResource(wide) failed 0x%08lX", hr);
            probe::Log("vr: %s", status_);
            ReleaseEyeTextures();
            return false;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        wide_texture_->GetDesc(&desc);
        SetStatus("VR running, wide %ux%u attached (D3D11 fmt %d)",
                  desc.Width, desc.Height, desc.Format);
        probe::Log("vr: attached double-wide eye texture %ux%u dxgi_fmt=%d (eye %ux%u)",
                   desc.Width, desc.Height, desc.Format, eye_width, eye_height);
        return true;
    }

    bool SubmitFrame() override {
        if (!ready_ || wide_texture_ == nullptr)
            return false;
        bool ok = true;
        for (int eye = 0; eye < 2; eye++) {
            vr::Texture_t texture = { wide_texture_, vr::TextureType_DirectX,
                                      vr::ColorSpace_Gamma };
            // Each eye is one half of the wide texture (u in [0,0.5] left,
            // [0.5,1] right).
            vr::VRTextureBounds_t bounds = { eye == 0 ? 0.0f : 0.5f, 0.0f,
                                             eye == 0 ? 0.5f : 1.0f, 1.0f };
            const vr::EVRCompositorError err = vr::VRCompositor()->Submit(
                eye == 0 ? vr::Eye_Left : vr::Eye_Right, &texture, &bounds);
            if (err != vr::VRCompositorError_None) {
                ok = false;
                if (err != last_submit_error_) {
                    last_submit_error_ = err;
                    SetStatus("Submit(eye %d) error %d", eye, (int)err);
                    probe::Log("vr: %s", status_);
                }
            }
        }
        if (ok && last_submit_error_ != vr::VRCompositorError_None) {
            last_submit_error_ = vr::VRCompositorError_None;
            SetStatus("VR running, submitting");
        }
        return ok;
    }

    void WaitPoses() override {
        if (!ready_)
            return;
        // Drain runtime events; a seated-zero reset (SteamVR-menu recenter)
        // is latched for PollRecentered so the game-camera anchor can
        // re-capture against the new origin.
        vr::VREvent_t event;
        while (system_->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_SeatedZeroPoseReset ||
                event.eventType == vr::VREvent_StandingZeroPoseReset ||
                event.eventType == vr::VREvent_ChaperoneUniverseHasChanged) {
                recentered_ = true;
                probe::Log("vr: runtime recenter event (%u)", event.eventType);
            }
        }
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        if (!poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
            return;  // keep last good pose
        head_pose_ = FromHmd34(
            poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking,
            config.world_scale);
        head_pose_valid_ = true;
        const D3DMATRIX head_inverse = RigidInverse(head_pose_);
        for (int eye = 0; eye < 2; eye++) {
            eye_to_head_[eye] = FromHmd34(
                system_->GetEyeToHeadTransform(eye == 0 ? vr::Eye_Left : vr::Eye_Right),
                config.world_scale);
            eye_view_append_[eye] = Multiply(head_inverse, RigidInverse(eye_to_head_[eye]));
        }
        // Cheap once per frame; keeps the HUD mapping current if the user
        // adjusts the headset's IPD mid-session.
        RefreshEyeData();
    }

    void GetEyeViewAppend(int eye, D3DMATRIX &out) override {
        out = eye_view_append_[eye & 1];
    }

    bool GetHeadPose(D3DMATRIX &out) override {
        if (!head_pose_valid_)
            return false;
        out = head_pose_;
        return true;
    }

    void GetEyeToHead(int eye, D3DMATRIX &out) override {
        out = eye_to_head_[eye & 1];
    }

    bool PollRecentered() override {
        const bool was = recentered_;
        recentered_ = false;
        return was;
    }

    void GetEyeProjection(int eye, D3DMATRIX &out) override {
        out = Identity();
        const float n = config.near_m * config.world_scale;
        const float f = config.far_m * config.world_scale;
        float left = -1.0f, right = 1.0f, top = -1.0f, bottom = 1.0f;
        if (system_ != nullptr)
            system_->GetProjectionRaw(eye == 0 ? vr::Eye_Left : vr::Eye_Right,
                                      &left, &right, &top, &bottom);
        // OpenVR raw tangents come y-down (top negative); up/down here are
        // y-up view-space tangents.
        const float up = -top, down = -bottom;
        out._11 = 2.0f / (right - left);
        out._22 = 2.0f / (up - down);
        out._44 = 0.0f;
        // Right-handed, -z forward, z clip [0,1] (the game's convention).
        out._31 = (right + left) / (right - left);
        out._32 = (up + down) / (up - down);
        out._33 = f / (n - f);
        out._34 = -1.0f;
        out._43 = n * f / (n - f);
    }

    // The virtual-screen UI mapping. All of it happens in meters in the
    // head's own frame (OpenVR convention: y up, -z forward), so world_scale
    // and the game's view matrix never enter into it: the screen hangs a
    // fixed distance in front of the face, and each eye projects its edges
    // through that eye's real frustum. The result collapses to one affine
    // scale/offset per axis because the screen plane sits at constant depth.
    void GetHudRemap(int eye, UINT eye_width, UINT eye_height,
                     float &scale_x, float &offset_x,
                     float &scale_y, float &offset_y) override {
        // Fallback (VR data missing or degenerate): plain full-target
        // scaling.
        scale_x = eye_width / 640.0f;
        offset_x = 0.0f;
        scale_y = eye_height / 480.0f;
        offset_y = 0.0f;
        eye &= 1;
        if (!eye_data_valid_)
            return;
        const float l = raw_tangent_[eye][0], r = raw_tangent_[eye][1];
        // Raw tangents come y-down; up/down here are y-up view tangents.
        const float up = -raw_tangent_[eye][2], down = -raw_tangent_[eye][3];
        const float tx = eye_offset_m_[eye][0];
        const float ty = eye_offset_m_[eye][1];
        // Forward distance from THIS eye to the screen plane: the plane sits
        // hud_distance_m in front of the head origin, and the eye itself
        // sits eye_offset_m_ from that origin (z positive = backward).
        const float dist = config.hud_distance_m + eye_offset_m_[eye][2];
        if (dist < 0.05f || r - l < 1e-3f || up - down < 1e-3f)
            return;
        // Screen half-size in meters from the angle it spans; 4:3 like the
        // game's 640x480 UI canvas.
        const float half_w = config.hud_distance_m *
                             tanf(config.hud_width_deg * (3.14159265f / 360.0f));
        const float half_h = half_w * (480.0f / 640.0f);
        // View tangent -> NDC -> target pixel for the screen's edges; the
        // affine coefficients follow from the two ends of each axis.
        auto ndc_x = [&](float u) {  // u: -1 = UI left edge, +1 = right
            const float tan_x = (u * half_w - tx) / dist;
            return (2.0f * tan_x - r - l) / (r - l);
        };
        auto ndc_y = [&](float v) {  // v: +1 = UI top edge, -1 = bottom
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

    const char *StatusLine() const override { return status_; }

private:
    // Per-eye frustum tangents and eye-to-head translation (meters), the
    // inputs to the HUD mapping, cached off the runtime once per frame.
    void RefreshEyeData() {
        if (system_ == nullptr)
            return;
        for (int eye = 0; eye < 2; eye++) {
            const vr::EVREye e = eye == 0 ? vr::Eye_Left : vr::Eye_Right;
            system_->GetProjectionRaw(e, &raw_tangent_[eye][0], &raw_tangent_[eye][1],
                                      &raw_tangent_[eye][2], &raw_tangent_[eye][3]);
            const vr::HmdMatrix34_t eth = system_->GetEyeToHeadTransform(e);
            eye_offset_m_[eye][0] = eth.m[0][3];
            eye_offset_m_[eye][1] = eth.m[1][3];
            eye_offset_m_[eye][2] = eth.m[2][3];
        }
        eye_data_valid_ = true;
    }

    void ReleaseEyeTextures() {
        if (wide_texture_) {
            wide_texture_->Release();
            wide_texture_ = nullptr;
        }
    }

    void SetStatus(const char *format, ...) {
        va_list args;
        va_start(args, format);
        vsnprintf(status_, sizeof(status_), format, args);
        va_end(args);
    }

    void Fail(char *error, size_t error_len, const char *format, ...) {
        va_list args;
        va_start(args, format);
        vsnprintf(status_, sizeof(status_), format, args);
        va_end(args);
        if (error != nullptr && error_len > 0)
            strncpy_s(error, error_len, status_, _TRUNCATE);
        probe::Log("vr: init failed: %s", status_);
    }

    vr::IVRSystem *system_ = nullptr;
    ID3D11Device *d3d11_device_ = nullptr;
    ID3D11DeviceContext *d3d11_context_ = nullptr;
    ID3D11Texture2D *wide_texture_ = nullptr;  // both eyes side by side
    D3DMATRIX eye_view_append_[2] = {};
    D3DMATRIX head_pose_ = Identity();     // seated space, game units
    D3DMATRIX eye_to_head_[2] = { Identity(), Identity() };
    bool head_pose_valid_ = false;
    bool recentered_ = false;
    float raw_tangent_[2][4] = {};   // per eye: left, right, top, bottom (y-down raw)
    float eye_offset_m_[2][3] = {};  // eye-to-head translation, meters
    bool eye_data_valid_ = false;
    vr::EVRCompositorError last_submit_error_ = vr::VRCompositorError_None;
    bool ready_ = false;
    char status_[192] = "VR not started";
};

} // namespace

VRInterface *GetOpenVRBackend() {
    static OpenVRBackend backend;
    return &backend;
}

VRInterface *Get() {
    return config.backend_openxr ? GetOpenXRBackend() : GetOpenVRBackend();
}


} // namespace vrmod
