#pragma once

// The VR interface: everything the game-hook code may know about VR.
// The backends (psobbvr_vr_openvr.cpp, psobbvr_vr_openxr.cpp) own the
// runtime and an in-process D3D11 device: neither runtime accepts D3D9
// textures, so the eye render target (shareable, on our D3D9Ex device) is
// opened on the D3D11 device via its shared handle and submitted from
// there. Game-hook code never calls OpenVR/OpenXR directly.
//
// Frame flow (all on the game's render thread):
//   device Present -> [stereo eyes already rendered + composited]
//                  -> flush D3D9 (event query)  [caller's job]
//                  -> SubmitFrame()             [backend: D3D11 -> runtime]
//                  -> WaitPoses()               [blocks for next head pose]
//   next frame's BeginEye() appends GetEyeViewAppend(eye) to the game view
//   and replaces perspective projections with GetEyeProjection(eye).
//
// psobbvr.ini, next to the game exe:
//   [vr]
//   enabled=1        ; start VR at launch (needs SteamVR + headset)
//   world_scale=9    ; game world units per meter (PSO ~ decimeters)
//   near_m=0.05      ; near/far plane in meters for the VR projection
//   far_m=1500       ;   (fallback only - the eye projections splice the
//                    ;   game's own depth rows when available)
//   hud_distance_m=1.5  ; 2D UI virtual screen: how far in front of the head
//   hud_width_deg=60    ; ...and the horizontal angle it spans

#include <windows.h>
#include <d3d9.h>

namespace vrmod {

struct Config {
    bool enabled = true;
    // [vr] backend=openvr|openxr. false = OpenVR (any SteamVR), true =
    // in-process OpenXR (needs SteamVR's 32-bit OpenXR runtime, 2.17.2+).
    bool backend_openxr = true;
    float world_scale = 9.0f;
    float near_m = 0.05f;
    float far_m = 1500.0f;
    // 2D UI (RHW draws) on a virtual screen: identical pixels in both eyes
    // cannot fuse under asymmetric VR frustums, so each eye's UI vertices
    // are remapped so the 640x480 UI plane floats hud_distance_m ahead,
    // hud_width_deg wide, at the same 3D place for both eyes. Distance in
    // meters; height follows from 4:3. Pure angles - world scale plays no
    // part.
    float hud_distance_m = 1.5f;
    float hud_width_deg = 60.0f;
    // Combat-text size multiplier. 1.0 = the flat game's angular size
    // (see ProjectCombatTextVertices).
    float text_scale = 0.5f;
    // Draw the 2D UI (and boxed 3D passes such as the radar) once into a
    // dedicated texture shown as a compositor quad layer, repositioned at
    // display rate; a baked-in virtual screen swims under reprojection.
    // OpenXR only (OpenVR keeps the baked per-eye remap). 0 = always bake.
    bool hud_layer = true;
    // HUD texture width in pixels (height = 3/4 of it).
    int hud_tex_width = 1600;
    // HUD lock ([vr] hud_lock): 0 = head-locked. 1 = level,
    // in front of the character's facing: head turns leave it in place, it
    // turns with the character (smoothed). 2 = fixed in the room along the
    // view's forward. Locked modes follow the head's position. OpenXR quad
    // layer only.
    int hud_lock = 1;
    // On-screen keyboard (psobbvr_vrkeyboard.hpp): a
    // panel below eye level while the game has a text field open, driven
    // by the controllers' aim rays and triggers. Placement: distance
    // ahead, drop below eye level and width, in meters.
    bool vr_keyboard = true;
    float keyboard_distance_m = 0.7f;
    float keyboard_drop_m = 0.3f;
    float keyboard_width_m = 0.8f;
    // Developer-build requests consumed by vrkeyboard::Tick: panel up
    // with no text field, and text / a key to type (cleared once queued).
    bool keyboard_force_show = false;
    char type_text_request[128] = {};
    char type_key_request[16] = {};
    // Menu world-lock: on non-gameplay frames, pin the 2D content to a
    // world-anchored screen so reprojection smooths look-around to display
    // rate (head-locked content updates at 30 Hz). Layers: opaque
    // below-quad (2D drawn before 3D), alpha-blended projection layers
    // (menu 3D in stereo), and the HUD texture as a transparent above-quad
    // (2D drawn after 3D). Anchored at the wearer's position + yaw at menu
    // entry / recenter. OpenXR only.
    bool menu_lock = true;
    // World-locked menu screen: distance ahead of the anchor (m) and
    // horizontal span (degrees).
    float menu_distance_m = 2.0f;
    float menu_width_deg = 70.0f;
    // The below quad (backdrop) sits deeper at the same angular span, so
    // menu 3D content reads between the layers.
    float menu_below_distance_m = 3.5f;
    // Stereo-baseline multiplier for menu 3D content: >1 compresses its
    // depth by that factor without changing the framing. 1 = natural depth.
    float menu_depth_scale = 1.0f;
    // Minimum fraction of a 2D draw's area over the menu 3D content before
    // it floats at the near quad.
    float menu_float_overlap = 0.4f;
    // On menu frames, additive 2D drawn after the 3D content goes into the
    // opaque below texture, where it adds over the backdrop like the flat
    // game. A glow overlapping the 3D character renders behind it.
    bool glow_below = true;
    // Burst-transition tunnel (char select -> lobby, lobby -> game):
    // recognized frames drop the menu world-lock and use the world-sprite
    // route, so the tunnel gets per-eye depth (stereo.hpp burst block).
    bool burst_3d = true;
    // Register OpenXR controller actions at VR start (poses, buttons,
    // sticks, haptics; profiles: hp mixed reality when offered, Oculus
    // Touch, Vive, Microsoft motion controller, khr/simple). 0 = no action
    // set.
    bool controllers = true;
    // The launch preflight refuses an OpenXR runtime that is not SteamVR
    // (untested elsewhere). 1 = start anyway, logged.
    bool allow_any_runtime = false;
    // Show a message box when VR fails to start at launch. 0 = log only
    // and run flat; the PSOBBVR_QUIET environment variable also suppresses
    // it.
    bool vr_fail_message = true;
    // After the failure box is dismissed, exit instead of running flat in
    // the 5760x2160 window (which grabs the mouse once clicked). Only when
    // the box was shown.
    bool vr_fail_exit = true;
    // Hotkey-bar arming: a hotkey chord whose slot holds a tech (or, with
    // a melee weapon, a normal attack) arms the number key for the
    // physical swing instead of pressing it; other slots press through.
    // The arm expires after hotkey_arm_timeout_s without a swing.
    bool hotkey_arm = true;
    // Developer-build diagnostic: hand draws left to log (psobbvr_hands.hpp).
    int hand_trace = 0;
    float hotkey_arm_timeout_s = 10.0f;
    // Developer-build diagnostic: frames of verbose controller-state
    // logging left, consumed by the OpenXR backend's action sync.
    int xr_input_log_frames = 0;
    // Controller mapping (psobbvr_controller.hpp):
    // Right-stick turn with stick_locomotion off: deflection maps through a
    // squared curve to a turn-rate scale between these two (units of
    // turn_speed_scale; 1 = the game's 337 deg/s).
    float controller_turn_min = 0.1f;
    float controller_turn_max = 0.4f;
    // Radial stick deadzone and the analog press threshold for
    // trigger/squeeze acting as buttons.
    float controller_deadzone = 0.15f;
    float controller_press = 0.6f;
    // Sign of "stick right" on the game's side axis (+1/-1).
    float controller_side_sign = 1.0f;
    // 1 = analog strafe: the left stick is the movement vector with the
    // facing decoupled, the right stick a facing yaw rate, and speed comes
    // from deflection via the entity speed-field scale (direction always
    // injected at full run magnitude). 0 = the stick faked as keys.
    bool stick_locomotion = true;
    // Max smooth-turn rate at full right-stick deflection, deg/s (squared
    // curve below).
    float stick_turn_deg_s = 140.0f;
    // Speed scale at the lightest walk deflection (curve: floor +
    // (1-floor)*d^2).
    float stick_speed_floor = 0.15f;
    // Game-camera takeover: write the head pose into the game's camera
    // object every frame, so billboards, culling and audio follow the
    // real view.
    //   0 = off (head pose appended to the chase-cam view)
    //   1 = on while VR runs (default)
    //   2 = on even without VR, head pose = identity (flat first-person
    //       debug view)
    int game_camera = 1;
    // Seated origin height above the character's feet, meters (the
    // entity's own head point is chest height). Fallback for while
    // eye_height_auto is off or has no measurement yet; meters for a
    // game-unit dimension, so it fits only one build and world scale.
    float eye_height_m = 1.58f;
    // 1 = the camera and head-trim anchor use the character's measured
    // standing head height (psobbvr_eyeheight.hpp) instead of the fixed
    // meters knobs.
    int eye_height_auto = 1;
    // Eyes above the measured head-matrix origin (the head pivot), meters.
    // eye_height_auto only.
    float eye_offset_m = 0.36f;
    // Developer-build one-shot requests, kept here because the developer
    // tools cannot include eyeheight/gamecam (include cycle): eyeheight
    // report, trace and capture; recenter_request re-anchors in
    // gamecam::Apply after a live world-scale change.
    int eyeheight_report_request = 0;
    int eyeheight_trace_request = 0;
    bool eyeheight_capture_request = false;
    bool recenter_request = false;
    // Eyes forward of the tracked head center, meters, along the head's
    // facing.
    float eye_forward_m = 0.16f;
    // Extra forward camera shift at a full run, meters, along the
    // character's facing (the run pitches the head forward). Ramps with
    // feet speed; 0 disables.
    float run_forward_m = 0.26f;
    // Downward camera shift at a full run, meters, same ramp. 0 disables.
    float run_down_m = 0.0f;
    // The seated frame turns with the character's facing (smoothed), so
    // turning the character turns the view (gamecam::Apply). An attack's
    // turn toward the locked target does not rotate the view; the
    // difference drains while walking.
    bool attack_view_hold = true;
    // Stick locomotion's forward: 0 = the character's facing; 1 = the
    // head's yaw, the character keeps its facing; 2 = the head's yaw, and
    // the character turns to face it while walking, with that turn kept
    // out of the view (gamecam::follow_offset).
    int head_move = 0;
    // Over-render margin (OpenXR only): scales the per-eye frustum
    // tangents and stamps the layer with the same widened FOV, so
    // reprojection reveals rendered content instead of black at the edges.
    // 1 = off; dilutes angular resolution by the same factor.
    float fov_margin = 1.15f;
    // Scale on the game's FOV divisor while the takeover drives, widening
    // its CPU visibility culling to the headset's view
    // (psobbvr_cullfov.hpp). <1 = wider; 1 (or 0) = original.
    float cull_fov_scale = 0.3f;
    // The same in town (floor id 0, Pioneer 2), where culling is the perf
    // cost and nothing needs it wide. 0 = no override.
    float cull_fov_scale_town = 0.7f;
    // Scale on the game's environment draw distance (per-episode clip
    // multipliers, psobbvr_drawdist.hpp). >1 = further; 1 (or 0) = stock.
    float draw_distance_scale = 4.0f;
    // Route the game's per-object screen-edge visibility tests through its
    // distance-only path while the takeover drives (psobbvr_objvis.hpp),
    // so objects don't vanish at the headset view's edges. Range only: a
    // box behind the wearer keeps its visible flag (the box react handler
    // consults it, e.g. for a Zonde lock).
    bool object_vis_fix = true;
    // Hold the mag for the one tick its bone target glitches on a
    // look-back (psobbvr_objvis.hpp HookMagUpdate).
    bool mag_glitch_fix = true;
    // Run every mag's update under the root (camera) matrix its owner's
    // bones were posed with, not this tick's (psobbvr_objvis.hpp
    // RunMagUpdate); otherwise other players' mags swing with our head.
    bool mag_root_sync = true;
    // Player name labels placed per eye at other characters' heads, like
    // combat text (psobbvr_namelabel.hpp). 0 = the game's sprite route.
    bool name_labels = true;
    // Bullet-trail ribbon: give its quads their nodes' real depth so the
    // world-sprite route places them (psobbvr_trail.hpp).
    bool trail_depth_fix = true;
    // With it: a second copy of each trail quad turned 90 degrees about
    // the path, so the streak reads solid from any angle.
    bool trail_cross = true;
    // Brightness of both ribbons while the cross is on (their overlap
    // doubles the core; 0.5 = original core brightness).
    float trail_cross_gain = 0.6f;
    // Developer-build perf diagnostic (not in the ini): 1 = scene draws to
    // the left eye only, to measure the per-draw eye duplication cost.
    int one_eye_debug = 0;
    // Scale on the keyboard turn speed while the takeover drives
    // (psobbvr_movement.hpp): the standing rate (stock 0x800 BAMS/frame =
    // 337 deg/s) and the while-moving facing chase. 1 = stock.
    float turn_speed_scale = 0.4f;
    // Back input walks backwards instead of turning 180 (keyboard path,
    // psobbvr_movement.hpp).
    bool back_strafe = true;
    // Speed multiplier while walking backwards (the base speed float at
    // entity+0x6B8, saved and restored).
    float back_speed_scale = 0.85f;
    // Turn-key-only input turns in place instead of running 90 degrees to
    // the side (psobbvr_movement.hpp).
    bool side_turn = true;
    // Movement-speed multiplier while side_turn holds (near 0 = brake on
    // the spot).
    float side_turn_speed_scale = 0.05f;
    // Hide the sun glow + lens-flare chain while VR drives the view (see
    // stereo::IsHiddenEffect); the game positions it from its own flare
    // state, so it drifts in VR. Flatscreen is unaffected.
    bool hide_sun = true;
    // First-person head trim (psobbvr_trim.hpp). trim_radius_m is the
    // learn zone (idle head content; the mag never comes closer than
    // 0.38 m); trim_wide_m is the coarse gate for learned-texture
    // suppression (covers animation excursions).
    bool trim_head = true;
    float trim_radius_m = 0.25f;
    float trim_wide_m = 1.2f;
    // Trim anchor height above the feet: the character's real head height,
    // independent of eye_height_m (raising it empties the learn zone).
    // Meters for a game-unit dimension (1.6 m = 16 units only at
    // world_scale 10). Fallback: with eye_height_auto the measured head
    // height is used (psobbvr_eyeheight.hpp).
    float trim_anchor_m = 1.6f;
    // Backface culling on the player-body draws (root matrix at the feet)
    // so the torso interior vanishes when looking down through it: 0 =
    // off, 1 = cull clockwise, 2 = cull counterclockwise.
    int body_cull = 1;
    // Developer-build one-shot request, consumed by trim::OnFrame (the
    // developer tools cannot include the trim header - include cycle).
    bool trim_reset_pending = false;
    // Developer-build diagnostic: frames of trim logging left.
    int trim_probe_request = 0;
    // Developer-build diagnostic: samples left of the swing-motion trace
    // (grip speed, tracked state, triggers; one line per pose change).
    int swing_trace_frames = 0;
    // Armed swings. With a melee weapon, a held trigger arms its palette
    // attack (right/left/both = bottom/left/right palette) and the
    // physical swing executes it at motion onset; guns and bare hands
    // keep the direct press. Hand noise stays under ~1.3 m/s, deliberate
    // swings peak 2-13 m/s; the glitch cap discards runtime pose snaps
    // (17-55 m/s while still flagged tracked).
    // Mode 2 (default): only the regular attack (right trigger alone) arms
    // on the swing; heavy/special fire on the chord press, so the game's
    // charge wind-up becomes the swing cue. Mode 1 = all three chords
    // armed. Mode 0 = direct press for every attack (techniques follow
    // cast_swing).
    int swing_attack = 2;
    // Bare hands count as a melee weapon for the swing gates (armed
    // swing, charge hold, swing warp).
    bool swing_unarmed = true;
    float swing_onset = 1.5f;         // m/s: fire at >= this...
    int swing_confirm = 2;            // ...for this many consecutive samples
    float swing_release = 1.0f;       // m/s: hand must slow below this
    float swing_refractory_s = 0.4f;  // min time between fires (wobble guard)
    float swing_glitch_cap = 15.0f;   // m/s: frames above this are discarded
    // Charge hold (controller::ChargeHold; needs swing_attack 2): freeze a
    // charging heavy/special at the end of its charge until the physical
    // swing releases it.
    bool charge_hold = true;
    float charge_hold_timeout_s = 3.0f;  // no swing by then: the charge is
                                         // cancelled, nothing fires;
                                         // 0 = hold forever
    // Swing warp (controller::SwingWarp; needs swing_attack 2): a
    // swing-fired normal attack skips the wind-up. Melee damage is sampled
    // on the tick the animation clock (float at entity+0xC0, advancing by
    // entity+0xC8 per tick) crosses the weapon's damage frame (getter
    // 0x6AC48C), so the clock jumps to just before that frame and the hit
    // lands mid-swing. Margin in clock steps below the damage frame.
    // Keyboard attacks are untouched.
    float swing_warp_margin = 0.5f;
    // Developer-build diagnostic: lines left of the per-frame attack
    // animation trace; not an ini setting.
    int swing_warp_trace = 0;
    // Developer-build diagnostic: lines left of the per-frame
    // technique-cast trace (action modes 8/9, psobbvr_techcast.hpp); not
    // an ini setting.
    int tech_trace = 0;
    // Stretch the combo next-input window for our swing-fired normals: the
    // warp removed the wind-up, so the vanilla window (follow-through
    // countdown, int at entity+0x8BC, ~9 ticks for a saber) is too short
    // for the physical swing cadence. Adds N ticks once per step, on the
    // tick the window opens (lifecycle bit 1); skipped on the chain's
    // final step. An unchained step roots N ticks longer. 0 disables.
    int combo_window_bonus = 6;
    // Post-hit clock hold (controller::SwingWarp, always on with the
    // warp): after a step's last hit, hold the animation clock for exactly
    // the ticks the warp skipped (per kind and step, doubled under slow),
    // so the clip end, follow-through and chain land at vanilla timing and
    // hits are never faster than the original game. Safe because the
    // crossing test 0x7AAA30 counts a frame only in [clock - step, clock),
    // and the advance routine 0x7AA094 sets the clip-end bit only once the
    // clock reaches the clip length.
    // Per-row hold for multi-hit kinds (dagger 2/2/2, double saber 2/1/3,
    // twin sword 1/2/2 strikes per step): after a strike that is not the
    // step's last, the clock runs to just under the next strike's damage
    // frame and waits for the next swing (or the cap). Margin in clock
    // steps below the frame (>= 1, or the crossing window straddles it);
    // cap in ticks, 0 = no wait.
    bool swing_row_hold = true;
    float swing_row_hold_margin = 1.5f;
    int swing_row_hold_cap = 30;
    // Twin weapons (dagger, knuckle and twin-sword kinds): the left hand's
    // swing counts like the right's (controller::LeftSwingActive).
    bool swing_left_hand = true;
    // Swing-timing indicator (psobbvr_swingind.hpp): a HUD sprite of the
    // game's hexagon art saying what the next swing does (green starts an
    // attack or the combo's next step; blue lands the next strike; grey
    // not yet; red flash = poisoned step), with a ring that shrinks onto
    // it as the swing comes due, shown only while the game's command
    // palette is showing (controller::PaletteVisible). Position and sizes
    // in 640x480 UI space.
    bool swing_indicator = true;
    float swing_indicator_x = 320.0f;
    float swing_indicator_y = 76.0f;  // level with the HP/TP bars
    float swing_indicator_ring = 100.0f;     // ring half-size at the start
    float swing_indicator_ring_end = 28.0f;  // ...and when it touches the hexagon
    float swing_indicator_scale = 2.0f;      // hexagon draw scale (cell 34x26)
    int swing_indicator_spin = 1;            // game ticks per ring frame
    int swing_indicator_trace = 0;           // developer build: state changes left to log
    int cast_trace = 0;                      // developer build: per-tick lines left to log across casts
    bool swing_indicator_load_request = false;  // developer build: reload the indicator art
    bool palette_window_request = false;  // developer build: log the read next frame
    // Developer-build one-shot: clears the pause bit on the player entity and
    // resets the hold state, in case a hold leaves the animation paused.
    // Consumed by controller::SwingWarp.
    bool swing_hold_clear_request = false;
    // The both-trigger special forms only when the second trigger arrives
    // within this window of the first; slower is a switch.
    float trig_pair_window_s = 0.25f;
    // Developer-build one-shot: log the palette read next frame.
    bool palette_dump = false;
    // Target steering (psobbvr_targetaim.hpp): the candidate-test hook
    // swaps the player facing to our aim yaw for the test. Bitmask: 1 =
    // guns aim with the right aim ray, 2 = melee/unarmed with the gaze,
    // 4 = tech banks (category 3) with the left aim ray (the tech bank is
    // omni in yaw, so bit 4 also writes a cone, cast_aim_cone_deg).
    int target_aim = 7;
    // Gun yaw-cone scale (1.0 = the weapon's own cone; <1 tightens).
    // Only while the gun ray aims and only on cones <= 90 degrees.
    float target_aim_cone_scale = 0.6f;
    // Developer-build diagnostics: force a fixed world yaw in degrees
    // (<= -400 = off) and a count of swapped candidate tests to log.
    float target_aim_force_deg = -999.0f;
    int target_aim_trace = 0;
    // VR spellcasting: tech palette slots (types 3/5) arm on the trigger
    // chord and cast on the physical right-hand swing. Works with any
    // weapon; with a gun only tech chords consult the swing. 0 =
    // press-to-cast.
    bool cast_swing = true;
    // A left-hand swing casts too, but only techniques the left hand does
    // not aim (support techs and ones that pick their own target: Resta,
    // Shifta, Deband, Zonde, Grants, Gifoie...). Aimed ones stay right-hand
    // casts (controller::TechLeftCasts).
    bool cast_left_hand = true;
    // Cast facing chase: the tech tick machine 0x6A00AC turns the
    // character toward the target every tick (the same 0x7A8CB8 chase as
    // melee). 0 = suppressed for our casts (cosmetic: the target comes
    // from the pair array / aim point). 1 = vanilla.
    bool cast_facing_snap = false;
    // Yaw cone half-angle for single-target directional techs (Foie,
    // Barta, Megid) steered by the left ray, applied only when tighter
    // than the game's (Foie 30, Barta 50, Megid 40).
    float cast_aim_cone_deg = 15.0f;
    // Which techniques the left ray may steer. -1 = every tech the game
    // gives a directional cone (read off the configured bank,
    // psobbvr_targetaim.hpp). Otherwise a bitmask over tech ids restricting
    // that set (bit n = id n: 0 Foie 1 Gifoie 2 Rafoie 3 Barta
    // 4 Gibarta 5 Rabarta 6 Zonde 7 Gizonde 8 Razonde 9 Grants 10 Deband
    // 11 Jellen 12 Zalure 13 Shifta 14 Ryuker 15 Resta 16 Anti
    // 17 Reverser 18 Megid); 0 = never. Omni techs are never steered.
    int cast_aim_techs = -1;
    // Gun bullet origin (psobbvr_gunfire.hpp). The shot choke point
    // 0x5E6A88 defaults a null source to entity+0x300 (the character), so
    // the hook moves it to the right hand's barrel ray + gun_muzzle_offset_m.
    // Targeted shots fly barrel -> enemy; untargeted ones along the ray for
    // the weapon's fallback range, wall-clipped, mechanically inert. Needs
    // weapon_grip.
    bool gun_fire_origin = true;
    float gun_muzzle_offset_m = 0.10f;
    // The free-fire ray (and gun steering yaw) is the barrel frame: grip
    // pose x the gun grip tuple, the weapon's -Y axis, so bullets leave
    // along the visible barrel on every controller family.
    // gun_barrel_pitch_deg trims it about the weapon's side axis (positive
    // = down).
    float gun_barrel_pitch_deg = 0.0f;
    // Attack-start facing snap with a gun: 0 = suppressed (cosmetic for
    // guns; bullets home on the target). The snap-distance threshold float
    // 0x93C754, read only by the four snap sites, is held at +huge while
    // suppressed. 1 = vanilla.
    bool gun_facing_snap = false;
    // Combo re-target: a chained combo step re-locks onto the enemy the
    // gaze steering picks now (the game keeps the first swing's lock).
    // 0 = vanilla.
    bool attack_retarget = true;
    // Half-angle around the gaze a chained step may re-lock within,
    // degrees.
    float retarget_cone_deg = 45.0f;
    int gun_fire_trace = 0;  // developer-build shot logging; not an ini setting
    // Per-hand mechguns (psobbvr_gunfire.hpp). The mechgun fire overrides
    // shoot once per barrel per burst bullet from muzzle vec3s on the
    // weapon; mechgun_dual writes our two hand muzzles there, fires the
    // left barrel at the left hand's own lock, and every bullet deals half
    // damage (both hands on one target = the vanilla total; the hit report
    // packet 0x46 lists both victims on a split). mechgun_swap_barrels
    // flips the barrel-to-hand mapping. mechgun_reticle draws the game's
    // reticle again at the left lock, rotated 180 degrees. The left lock
    // is the game's candidate test on a copy of the attack bank with the
    // left ray (psobbvr_targetaim.hpp), same cone as the right.
    // mechgun_union: when the right ray sees nothing, the left candidates
    // go to the attack bank so the game locks the left hand's enemy.
    bool mechgun_dual = true;
    bool mechgun_swap_barrels = false;
    bool mechgun_reticle = true;
    bool mechgun_union = true;
    // With a dual mechgun, two of our damage popups on the same enemy
    // within a tick become one showing their sum. The first popup is held
    // one tick (the popup constructor runs from the next frame).
    bool mechgun_merge_popups = true;
    int mechgun_trace = 0;    // developer-build logging; not an ini setting
    bool hitpairs_dump = false;  // developer-build one-shot; not an ini setting
    bool banks_dump = false;     // developer-build one-shot; not an ini setting
    // Combat haptics (psobbvr_haptics.hpp; gun buzz in psobbvr_gunfire.hpp
    // and psobbvr_controller.hpp). gun_haptic: 0 = off, 1 = pulse on the
    // trigger-pull edge while a gun is held, 2 = pulse per fired bullet
    // from the shot hooks (per bullet for mechgun bursts; the twin split
    // pulses the firing hand).
    int gun_haptic = 2;
    float gun_haptic_amp = 1.0f;   // 0..1 pulse strength
    float gun_haptic_s = 0.05f;    // pulse length, seconds
    // Melee hit pulse (weapon hand, on our landed hits) and the both-hands
    // pulse when we take damage.
    bool hit_haptic = true;
    float hit_haptic_amp = 1.0f;
    float hit_haptic_s = 0.08f;
    bool hurt_haptic = true;
    float hurt_haptic_amp = 0.7f;
    float hurt_haptic_s = 0.12f;
    // World-sprite near floor in view-depth units (psobbvr_stereo.hpp
    // IsWorldRhw): additive quads closer than this go to the HUD path.
    // Lens-flare chain ~2, HUD glows ~3, world effects 10+, the muzzle
    // flash at the hand ~5-8.
    float sprite_near_floor = 4.0f;
    // UI by caller: a 2D draw issued by the game's window system is UI,
    // never a world sprite, whatever its depth (a window's z is its place
    // in the window list, 0.3 per window). The caller is the return
    // address of the game's three generic 2D quad drawers
    // (stereo::DrawSiteFromReturnSlot): window and text code
    // 0x700000..0x761000; world effects draw from 0x80xxxx, flares from
    // 0x5007xx.
    bool ui_caller_rule = true;
    // Alpha-blended world sprites (src SRCALPHA / dest INVSRCALPHA,
    // depth-tested, textured - Forest grass at site 0x802412, the CCA fog
    // cloud at 0x50891F) take the world route too, under the same depth
    // floor. Excluded: window-system callers and two fixed-depth HUD draws
    // with the same blend (the screen fade 0x8047A4 and the equipped-weapon
    // icon 0x81A29D).
    bool alpha_sprite_rule = true;
    // Alpha sprites' own near floor (view-depth units): between this and
    // sprite_near_floor they are still world sprites; closer, the draw is
    // dropped rather than sent to the HUD, where a near fog puff would
    // darken the whole HUD.
    float alpha_sprite_floor = 1.0f;
    // Upright world sprites: the world route plants each effect quad
    // square to the view direction, so a tall sprite for a vertical thing
    // (the telepipe beam) leans with head pitch. Such quads are rebuilt
    // about their centre with world up for height: sprites from the
    // particle pools in sprite_upright_pools (the game's 108 fixed effect
    // pools, psobbvr_particlepool.hpp). false = off.
    bool sprite_upright = true;
    unsigned int sprite_upright_pool_mask[4] = {};
    bool UprightPool(int pool) const {
        return pool >= 0 && pool < 128 &&
               (sprite_upright_pool_mask[pool >> 5] >> (pool & 31)) & 1u;
    }
    // Comma/space separated pool indices; "" = none.
    void SetUprightPools(const char* list) {
        for (unsigned int& w : sprite_upright_pool_mask) w = 0;
        if (list == nullptr) return;
        const char* p = list;
        while (*p) {
            while (*p == ',' || *p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char* end = nullptr;
            const long v = strtol(p, &end, 10);
            if (end == p) break;
            if (v >= 0 && v < 128)
                sprite_upright_pool_mask[v >> 5] |= 1u << (v & 31);
            p = end;
        }
    }
    // Hide the character's arms and hands in first person. The body draws
    // as one order-stable run under the root matrix at the feet, and the
    // arm chunks come first: the first hide_arms_count draws (58 on the
    // HUmar; the hand capture learns it per character). Gameplay frames
    // only.
    bool hide_arms = true;
    int hide_arms_count = 58;
    // The equipped weapon rides the right controller's grip pose
    // (psobbvr_weapongrip.hpp). grip_* aligns the weapon's authored grip to
    // the controller grip in felt axes (pitch = tip up/down, roll = spin
    // about the long axis, yaw = swing left/right; see GripOffsetMatrix),
    // then cm forward/up along the grip. gun_* is the same for weapons that
    // fire (PMT projectile type nonzero).
    bool weapon_grip = true;
    // Split dual-wield weapons (twin daggers/swords/mechguns) so the
    // off-hand half rides the left grip. Learned per weapon from the draw
    // data (parts rigid to the bracket root = main hand; the tail cluster
    // moving relative to it = off hand). Only two-handed kinds split
    // (weapongrip::KindSplitsFunnel/Composite).
    bool twin_split = true;
    // Uniform scale on the held weapon about the grip anchor (PSO weapons
    // are oversized for third person). 1 = authored size. Boxed passes and
    // the animation-riding fallback keep the authored size.
    float weapon_scale = 0.7f;
    float grip_pitch_deg = -10.0f;
    float grip_roll_deg = 180.0f;
    float grip_yaw_deg = 0.0f;
    float grip_fwd_cm = -3.0f;
    float grip_up_cm = 0.0f;
    float gun_pitch_deg = 40.0f;
    float gun_roll_deg = 180.0f;
    float gun_yaw_deg = 0.0f;
    float gun_fwd_cm = 5.0f;
    float gun_up_cm = 0.0f;
    // Fist trim: a delta (grip axes; zero = the game's placement) for a
    // weapon attached to the fist bone instead of the hand bone (claws,
    // weapongrip::seat_on_fist). side_cm = along the palm normal.
    float fist_pitch_deg = -40.0f;
    float fist_roll_deg = 0.0f;
    float fist_yaw_deg = 0.0f;
    float fist_fwd_cm = 11.0f;
    float fist_up_cm = -12.0f;
    float fist_side_cm = -1.0f;
    // Developer-build one-shot pass count for the part census
    // (psobbvr_partmap.hpp).
    int partmap_request = 0;
    // Developer-build one-shot pass count: log the caller chain of
    // every equipment world-matrix set near the player.
    int stackcap_request = 0;
    // Developer-build one-shot pass count: log every world matrix
    // inside the held-weapon draw bracket.
    int gripdump_request = 0;
    // Pass count for the per-draw hand-band classifier
    // (psobbvr_partmap.hpp); set by the automatic hand capture.
    int handband_request = 0;
    // Motion-controlled hands (psobbvr_hands.hpp): the character's captured
    // hand geometry re-rendered at the controller grips. The hand is
    // captured automatically, and the learned band sets hide_arms_count
    // per character.
    bool hand_presence = true;
    // Uniform scale about the hand bone.
    float hand_scale = 0.7f;
    // Swap the hand replay's front/back cull convention.
    bool hand_cull_flip = false;
    // Hand orientation trim, a delta on the melee grip tuple (same felt
    // axes as grip_*).
    float hand_pitch_deg = -10.0f;
    float hand_roll_deg = 0.0f;
    float hand_yaw_deg = 0.0f;
    float hand_fwd_cm = 0.0f;
    float hand_up_cm = 0.0f;
    // Developer-build one-shots: capture status, and finding the hand
    // bone in the entity's bone array.
    int handcaps_request = 0;
    int handbone_request = 0;
    // Developer-build manual capture band (body-run draw indices from a
    // hand-band run), consumed by hands::OnFrame.
    int handcap_first = 0;
    int handcap_last = 0;
};

inline Config config;

void LoadConfig();
// Launch-time VR failure report (psobbvr_vr_openvr.cpp): always logs;
// shows the message box unless vr_fail_message=0 or PSOBBVR_QUIET.
void ReportLaunchFailure(const char* error);

// ---- hand-bone world matrices ------------------------------------------
// The raw first matrix of the held-weapon draw bracket is the hand bone's
// world matrix. weapongrip stores it each pass; partmap's hand-band
// classifier reads it (the two must not include each other). The left
// bone exists only while a dual weapon's off-hand half draws. Valid only
// while the flag is true; weapongrip clears the flags one pass after
// brackets stop.
inline D3DMATRIX hand_bone_world_right = {};
inline bool have_hand_bone_right = false;
inline D3DMATRIX hand_bone_world_left = {};
inline bool have_hand_bone_left = false;

// ---- gun aim state for the targeting hooks ----
// Set per frame by gunfire::OnFrame, read by targetaim's hooks (targetaim
// must not include gunfire). mech_hand_origin = each hand's barrel origin
// in game units; mech_dual_active = a dual mechgun rides both hands;
// mech_left_yaw_bams = the left barrel ray's yaw, used for the left
// hand's candidate-test pass.
inline bool mech_dual_active = false;
inline bool mech_hand_origin_valid[2] = {false, false};
inline float mech_hand_origin[2][3] = {};
inline unsigned int mech_left_yaw_bams = 0;

inline double NowSeconds() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}


// ---- damage-applier scope (haptics -> gunfire) ----
// The victim while the game's damage applier (vtbl+0x5C, base 0x7732B8)
// runs for one of our hits, else 0. Set by haptics::ApplyHitHook, read by
// gunfire's popup constructor hook to recognize our damage popups.
inline uintptr_t apply_scope_victim = 0;

// ---- row-vector rigid-transform helpers ---------------------------------

inline D3DMATRIX Identity() {
    D3DMATRIX m = {};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

inline D3DMATRIX Multiply(const D3DMATRIX &a, const D3DMATRIX &b) {
    D3DMATRIX r;
    const float *pa = &a._11, *pb = &b._11;
    float *pr = &r._11;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            pr[i * 4 + j] = pa[i * 4 + 0] * pb[0 * 4 + j] + pa[i * 4 + 1] * pb[1 * 4 + j] +
                            pa[i * 4 + 2] * pb[2 * 4 + j] + pa[i * 4 + 3] * pb[3 * 4 + j];
    return r;
}

// Inverse of a rigid transform [R; t]: p' = p*R + t  =>  R^T, -t*R^T.
inline D3DMATRIX RigidInverse(const D3DMATRIX &m) {
    D3DMATRIX r = Identity();
    r._11 = m._11; r._12 = m._21; r._13 = m._31;
    r._21 = m._12; r._22 = m._22; r._23 = m._32;
    r._31 = m._13; r._32 = m._23; r._33 = m._33;
    r._41 = -(m._41 * r._11 + m._42 * r._21 + m._43 * r._31);
    r._42 = -(m._41 * r._12 + m._42 * r._22 + m._43 * r._32);
    r._43 = -(m._41 * r._13 + m._42 * r._23 + m._43 * r._33);
    return r;
}

// One frame's controller snapshot (0 = left, 1 = right). Buttons are held
// state, not edges. Poses come from GetHandPose / GetHandAimPose.
struct ControllerState {
    bool active[2] = {};        // controller on + bound
    bool tracked[2] = {};       // grip pose POSITION_TRACKED (not merely
                                // valid - runtimes extrapolate out of view)
    float trigger[2] = {};      // 0..1
    float squeeze[2] = {};      // 0..1 (grip)
    float stick_x[2] = {};      // -1..1, right positive
    float stick_y[2] = {};      // -1..1, up positive
    bool stick_click[2] = {};
    bool primary[2] = {};       // X (left hand) / A (right hand)
    bool secondary[2] = {};     // Y (left hand) / B (right hand)
    bool menu[2] = {};
};

class VRInterface {
public:
    virtual ~VRInterface() {}

    // Start the VR runtime. Returns false and fills 'error' on failure; the
    // game keeps rendering flat either way. quiet_probe: check for a
    // headset without starting SteamVR where the backend can (OpenVR).
    virtual bool Init(char *error, size_t error_len, bool quiet_probe) = 0;
    virtual void Shutdown() = 0;
    virtual bool Ready() const = 0;

    // The shared handle of the double-wide eye render target (from
    // CreateRenderTarget on the D3D9Ex device): 2*eye_width x eye_height,
    // left eye in the left half. Called again whenever the target is
    // recreated (device reset).
    virtual bool AttachEyeTextures(HANDLE shared_wide,
                                   UINT eye_width, UINT eye_height) = 0;

    // Submit the frame in the eye texture. The caller must have flushed
    // the D3D9 work first.
    virtual bool SubmitFrame() = 0;

    // Block until the compositor hands out the head pose for the next frame
    // and precompute the per-eye matrices below from it.
    virtual void WaitPoses() = 0;

    // Row-vector matrix appended to the game's view matrix for one eye:
    // view_eye = game_view * M (inverse head pose then inverse eye-to-head,
    // translations scaled by world_scale). eye: 0 = left, 1 = right.
    virtual void GetEyeViewAppend(int eye, D3DMATRIX &out) = 0;

    // The head pose from the last WaitPoses (row-vector, seated tracking
    // space, game units). False until a first valid pose arrived.
    virtual bool GetHeadPose(D3DMATRIX &out) = 0;

    // One eye's eye-to-head transform in the same convention.
    virtual void GetEyeToHead(int eye, D3DMATRIX &out) = 0;

    // True once after the runtime reported a recenter; consumers re-anchor
    // anything derived from the seated origin. Cleared by the call.
    virtual bool PollRecentered() = 0;

    // Per-eye projection from the runtime's frustum tangents in the game's
    // convention (row-vector, right-handed, z clip [0,1]) with
    // [near_m, far_m] * world_scale.
    virtual void GetEyeProjection(int eye, D3DMATRIX &out) = 0;

    // ---- dedicated HUD quad layer -----------------------------------------
    // True = the backend composites the HUD texture as its own layer at
    // hud_distance_m/hud_width_deg (OpenXR quad layer), and the game side
    // draws UI once into that texture instead of baking it per eye.
    virtual bool SupportsHudLayer() const { return false; }

    // Shared handle of the HUD render target (on the D3D9Ex device). Called
    // after AttachEyeTextures; re-called on recreation.
    virtual bool AttachHudTexture(HANDLE /*shared*/, UINT /*width*/,
                                  UINT /*height*/) { return false; }

    // Whether the next SubmitFrame shows the HUD layer (false when nothing
    // was drawn into it this frame).
    virtual void SetHudLayerVisible(bool /*visible*/) {}

    // HUD lock (Config::hud_lock): the level forward direction in seated
    // tracking space (unit xz) the gameplay HUD quad faces along, sent every
    // gamecam::Apply. valid=false (or hud_lock 0) keeps it head-locked.
    virtual void SetHudAnchor(bool /*valid*/, float /*fx*/, float /*fz*/) {}

    // ---- Menu world-lock -------------------------------------------------
    // True = the backend can composite non-gameplay frames world-locked
    // (Config::menu_lock).
    virtual bool SupportsMenuLock() const { return false; }

    // Shared handle of the below-quad render target (the opaque 2D screen;
    // the above-quad reuses the HUD texture). Called after
    // AttachHudTexture; re-called on recreation.
    virtual bool AttachMenuTexture(HANDLE /*shared*/, UINT /*width*/,
                                   UINT /*height*/) { return false; }

    // Whether the next SubmitFrame is a menu frame and which quads have
    // fresh content. false/false = normal gameplay compositing.
    virtual void SetMenuLayerState(bool /*below*/, bool /*above*/) {}

    // Drop the menu anchor so the next menu frame re-captures it. Called on
    // the menu->gameplay edge (the backend also drops it on a recenter).
    virtual void InvalidateMenuAnchor() {}

    // The menu anchor as a rigid transform (row-vector, game units); false
    // until captured. Used to anchor menu 3D content where the menu quads
    // are pinned.
    virtual bool GetMenuAnchor(D3DMATRIX& /*out*/) { return false; }

    // Virtual-screen mapping for 2D UI: scale/offset such that
    //   x_target = scale_x * x_game + offset_x   (and alike for y)
    // takes a 640x480 UI vertex to where the head-locked virtual screen's
    // point projects in this eye's target. Exact as an affine map because
    // the screen is a plane at one fixed depth. z and rhw are untouched.
    virtual void GetHudRemap(int eye, UINT eye_width, UINT eye_height,
                             float &scale_x, float &offset_x,
                             float &scale_y, float &offset_y) = 0;

    // The last action sync's controller snapshot. False when there is no
    // controller support or the session is unfocused - callers must treat
    // false as all-neutral so synthetic input decays instead of latching.
    virtual bool GetControllerState(ControllerState & /*out*/) { return false; }

    // One hand's grip pose (0 = left, 1 = right) in GetHeadPose's
    // convention, located at the same predicted display time as the head.
    // False while unavailable (asleep, unfocused, OpenVR backend). An
    // extrapolated pose still returns true; ControllerState::tracked is
    // the honest bit.
    virtual bool GetHandPose(int /*hand*/, D3DMATRIX & /*out*/) { return false; }

    // One hand's aim pose (the profile's pointing transform, -z forward),
    // same convention and timing; the gun target-steering ray.
    virtual bool GetHandAimPose(int /*hand*/, D3DMATRIX & /*out*/) { return false; }

    // One haptic pulse on a hand (0 = left, 1 = right), `seconds` long,
    // amplitude 0..1. No-op without haptics or while unfocused.
    virtual void HapticPulse(int /*hand*/, float /*seconds*/,
                             float /*amplitude*/) {}

    // Tuning readout (psobbvr_hotkeys.hpp): two short text lines on a small
    // head-locked quad for `seconds`; re-calling replaces the text and
    // restarts the timer.
    virtual void ShowTuneText(const char* /*line1*/, const char* /*line2*/,
                              float /*seconds*/) {}

    // Teleport menu (psobbvr_teleport.hpp): a titled list on a head-locked
    // quad, `selected` highlighted, up until HideMenuText.
    virtual void ShowMenuText(const char* /*title*/,
                              const char* const* /*items*/, int /*count*/,
                              int /*selected*/) {}
    virtual void HideMenuText() {}

    // On-screen keyboard panel (psobbvr_vrkeyboard.hpp): a BGRA image
    // (straight alpha, top-down rows) as a quad at `pose_m` in seated
    // tracking space (row-vector rigid transform in meters; +z faces the
    // viewer), `width_m` wide. Calling again re-uploads and moves it; up
    // until HidePanel.
    virtual void ShowPanel(const unsigned int* /*bgra*/, UINT /*width*/,
                           UINT /*height*/, const D3DMATRIX& /*pose_m*/,
                           float /*width_m*/) {}
    virtual void HidePanel() {}
    // The keyboard's hand rays: a thin beam quad per hand (`pose_m` as for
    // ShowPanel; y runs along the beam with the image's top row at the
    // hand), drawn only while the panel is up. `visible` false hides it.
    virtual void SetPanelRay(int /*hand*/, bool /*visible*/,
                             const D3DMATRIX& /*pose_m*/, float /*length_m*/,
                             float /*width_m*/) {}

    // One-line state for the overlay panel / logs.
    virtual const char *StatusLine() const = 0;
};

// The process-wide backend, chosen by config.backend_openxr. Never null.
VRInterface *Get();

// The two implementations (process-wide singletons; only the one Get()
// selects is ever initialized).
VRInterface *GetOpenVRBackend();
VRInterface *GetOpenXRBackend();


} // namespace vrmod
