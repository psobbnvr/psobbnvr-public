// A snapshot of the held weapon's geometry, taken once per equip.
//
// The game draws the held weapon's model straight out of the model's own
// memory: each strip entry of the model's draw lists goes to the device as
// one DrawIndexedPrimitiveUP call (the loop at 0x83230C dispatching the
// 20-byte strip entries, device slot +0x124 at 0x83236F and 0x832480), with
// the vertex stream and the 16-bit index list pointing into the model.
// The device proxies see those calls inside the weapon's bracket
// (weapongrip::ExemptingDraws()) together with the part matrix the bracket
// set just before, so a copy of the arguments is exactly the shape on
// screen. The same strips can also arrive through the plain DrawPrimitiveUP
// path, with the index list already expanded into the vertex stream; the
// recorder takes both, and the snapshot log line says which path supplied the
// parts.
//
// Positions and normals are stored in the weapon's model space with every
// node transform applied: part matrix times the inverse of the weapon's seat
// root (the same relation the re-seat computes before it scales and seats the
// part). The off-hand's part is relative to its own root.
//
// The recorder costs one flag test per draw while nothing is being taken.
// It is armed by a weapon change (or the twin split latching, or
// RequestRetake), collects every pass's draws, and publishes once
// the draw list has repeated unchanged for a few passes while the owner
// stands idle. The published geometry is immutable; a later snapshot
// replaces it with a new object and a higher generation.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "psobbvr_log.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weaponsnap_core.hpp"

namespace weaponsnap {

// The player entity's action mode word: 1 = idle, 5..7 = melee attack modes.
constexpr uintptr_t ENTITY_ACTION_MODE_OFFSET = 0x32E;
constexpr int16_t ACTION_MODE_IDLE = 1;
// Passes with an unchanged draw list before a snapshot is taken.
constexpr int STABLE_PASSES = 4;
// A weapon that should split into two hands waits this many stable passes for
// the split to latch before it is taken as it is.
constexpr int SPLIT_WAIT_PASSES = 240;
// Passes a weapon may stay armed before the snapshot is abandoned.
constexpr int GIVE_UP_PASSES = 3000;
// Limits of one snapshot; a weapon over any of them is not taken.
constexpr size_t MAX_DRAWS = 96;
constexpr size_t MAX_VERTICES = 8192;
constexpr size_t MAX_INDICES = 24576;
constexpr unsigned MAX_DRAW_INDICES = 6000;
constexpr uint8_t KIND_KNUCKLE = 0x00;  // worn over the hand: not taken
constexpr uint8_t KIND_NONE = 0xFF;
// Direct3D primitive types and the 16-bit index format of the draw calls.
constexpr unsigned PRIM_TRIANGLE_LIST = 4;
constexpr unsigned PRIM_TRIANGLE_STRIP = 5;
constexpr unsigned INDEX_FORMAT_16 = 101;

// What the weapon module knows each pass.
struct FrameInfo {
    void* weapon = nullptr;     // the equipped weapon object, null when bare-handed
    uintptr_t owner = 0;        // the player entity
    uint8_t category = 0;
    uint8_t kind = KIND_NONE;
    uint8_t variant = 0;
    int split_index = -1;       // first off-hand part of a funnel twin, -1 = none
    bool seat_on_fist = false;  // seated on a bone other than the hand (claws)
    bool expects_split = false; // a funnel twin whose split may still latch
};

namespace state {
struct WorkDraw {
    HeldDraw d;
    uint8_t side = 0;
};
// The published geometry, one per hand.
inline std::shared_ptr<const HeldGeometry> held[2];
inline std::atomic<uint32_t> generation{0};

inline bool armed = false;              // the one flag the draw proxies test
inline bool retake_request = false;
// Identity of what the armed snapshot is for.
inline void* key_weapon = reinterpret_cast<void*>(~uintptr_t(0));
inline uint8_t key_category = 0, key_variant = 0;
inline int key_split = -1;
inline uintptr_t owner = 0;
inline void* skip_logged_for = nullptr;
inline uint8_t kind_now = KIND_NONE, category_now = 0;
// Progress.
inline int stable = 0, armed_passes = 0;
inline uint64_t last_sig = 0;
inline bool gave_up_logged = false;
// This pass.
inline std::vector<WorkDraw> wdraws;
inline std::vector<float> wpos, wnrm;
inline std::vector<uint16_t> widx;
inline uint64_t sig = 0;
inline bool pass_started = false, pass_idle = false, overflow = false;
inline unsigned bad_draws = 0;
inline bool seen_main = false, seen_left = false;
// This bracket.
inline bool bracket_left = false, skip_bracket = true, cur_valid = false;
inline uint8_t cur_side = 0, cur_part = 0;
inline D3DMATRIX cur_rel;
inline D3DMATRIX tail_root_inv;
// Scratch for one draw.
inline std::vector<int32_t> remap;
inline std::vector<uint32_t> src, tidx, tout;
inline std::vector<double> tpos, tnrm;
}  // namespace state

// The published geometry of one hand, or null. Safe to keep: it is never
// changed after publication.
inline std::shared_ptr<const HeldGeometry> Held(int side) {
    if (side < 0 || side > 1)
        return nullptr;
    return std::atomic_load(&state::held[side]);
}
// Counts every publication (and every clearing of a published snapshot).
inline uint32_t Generation() { return state::generation.load(); }
// True while the draw proxies should hand their calls to the recorder.
inline bool Collecting() { return state::armed; }
// Take the snapshot again (the weapon is unchanged).
inline void RequestRetake() { state::retake_request = true; }

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v;
    return h * 1099511628211ull;
}

inline void ClearWork() {
    using namespace state;
    wdraws.clear();
    wpos.clear();
    wnrm.clear();
    widx.clear();
    sig = 1469598103934665603ull;
    pass_started = false;
    pass_idle = false;
    overflow = false;
    bad_draws = 0;
    seen_main = seen_left = false;
    cur_valid = false;
    skip_bracket = true;
}

inline void Arm(const char* why) {
    using namespace state;
    armed = true;
    stable = 0;
    armed_passes = 0;
    last_sig = 0;
    gave_up_logged = false;
    ClearWork();
    (void)why;
}

// A bracket of the weapon's draw opened (the held draw, or one half of a
// composite). The first matrix settles whether it is worth recording.
inline void OnBracket(bool left) {
    using namespace state;
    if (!armed)
        return;
    bracket_left = left;
    skip_bracket = true;
    cur_valid = false;
}

// Every part matrix the weapon bracket sets, with its number in the bracket,
// the seat root's inverse for this bracket, and whether the part is seated
// on a bone other than the hand (not recorded).
inline void NoteMatrix(const D3DMATRIX& in, int part, bool left, int split_index,
                       const D3DMATRIX& root_inv, bool on_fist, bool boxed) {
    using namespace state;
    if (!armed)
        return;
    if (part == 0) {
        if (boxed || on_fist) {
            skip_bracket = true;
            cur_valid = false;
            return;
        }
        // A weapon drawn again in the same pass (a second view of the
        // scene) is recorded once.
        bool& seen = left ? seen_left : seen_main;
        skip_bracket = seen;
        seen = true;
        if (!skip_bracket && !pass_started) {
            pass_started = true;
            pass_idle = false;
            if (owner != 0 && diag::Accessible(owner + ENTITY_ACTION_MODE_OFFSET, 2, false))
                pass_idle = *reinterpret_cast<const int16_t*>(owner + ENTITY_ACTION_MODE_OFFSET) ==
                            ACTION_MODE_IDLE;
        }
    }
    if (skip_bracket || on_fist || part > 255) {
        cur_valid = false;
        return;
    }
    if (left) {
        cur_side = 1;
        cur_rel = vrmod::Multiply(in, root_inv);
    } else if (split_index > 0 && part >= split_index) {
        cur_side = 1;
        if (part == split_index)
            tail_root_inv = vrmod::RigidInverse(in);
        cur_rel = vrmod::Multiply(in, tail_root_inv);
    } else {
        cur_side = 0;
        cur_rel = vrmod::Multiply(in, root_inv);
    }
    cur_part = (uint8_t)part;
    cur_valid = true;
}

// One draw's vertices and triangles into the work buffers. src_ids are the
// source vertex numbers in first-appearance order, tidx the draw's strip (or
// list) in ids into src_ids.
inline void EmitDraw(uint8_t path, const uint8_t* vbase, unsigned stride, bool strip,
                     uint64_t sig_part) {
    using namespace state;
    const size_t nv = src.size();
    const bool normals = stride >= 24;
    if (wdraws.size() >= MAX_DRAWS || wpos.size() / 3 + nv > MAX_VERTICES) {
        overflow = true;
        return;
    }
    tpos.assign(nv * 3, 0.0);
    tnrm.assign(normals ? nv * 3 : 0, 0.0);
    const D3DMATRIX& m = cur_rel;
    for (size_t i = 0; i < nv; i++) {
        const float* v = reinterpret_cast<const float*>(vbase + (size_t)src[i] * stride);
        const double x = v[0], y = v[1], z = v[2];
        tpos[3 * i + 0] = x * m._11 + y * m._21 + z * m._31 + m._41;
        tpos[3 * i + 1] = x * m._12 + y * m._22 + z * m._32 + m._42;
        tpos[3 * i + 2] = x * m._13 + y * m._23 + z * m._33 + m._43;
        if (normals) {
            const double nx = v[3], ny = v[4], nz = v[5];
            double d[3] = {nx * m._11 + ny * m._21 + nz * m._31,
                           nx * m._12 + ny * m._22 + nz * m._32,
                           nx * m._13 + ny * m._23 + nz * m._33};
            const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int k = 0; k < 3; k++)
                tnrm[3 * i + k] = len > 0.0 ? d[k] / len : 0.0;
        }
    }
    tout.clear();
    if (strip) {
        core::OrientedTriangles(tidx.data(), tidx.size(), tpos.data(),
                                normals ? tnrm.data() : nullptr, tout);
    } else {
        for (size_t t = 0; t + 2 < tidx.size(); t += 3) {
            double f[3];
            core::Face(&tpos[3 * tidx[t]], &tpos[3 * tidx[t + 1]], &tpos[3 * tidx[t + 2]], f);
            if (f[0] * f[0] + f[1] * f[1] + f[2] * f[2] > 1e-12)
                tout.insert(tout.end(), tidx.begin() + t, tidx.begin() + t + 3);
        }
    }
    const size_t turned = core::MakeConsistent(tpos.data(), normals ? tnrm.data() : nullptr, tout);
    if (widx.size() + tout.size() > MAX_INDICES) {
        overflow = true;
        return;
    }
    WorkDraw w;
    w.side = cur_side;
    w.d.part = cur_part;
    w.d.path = path;
    w.d.has_normals = normals ? 1 : 0;
    w.d.first_vertex = (uint32_t)(wpos.size() / 3);
    w.d.vertex_count = (uint32_t)nv;
    w.d.first_index = (uint32_t)widx.size();
    w.d.index_count = (uint32_t)tout.size();
    w.d.flipped = (uint32_t)turned;
    for (size_t i = 0; i < nv * 3; i++)
        wpos.push_back((float)tpos[i]);
    for (size_t i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++)
            wnrm.push_back(normals ? (float)tnrm[3 * i + k] : 0.0f);
    for (uint32_t id : tout)
        widx.push_back((uint16_t)id);
    wdraws.push_back(w);
    sig = Mix(sig, sig_part);
}

inline bool Ready(bool boxed) {
    using namespace state;
    return armed && cur_valid && !skip_bracket && !boxed;
}

// DrawIndexedPrimitiveUP, called with the proxy's arguments.
inline void RecordIndexed(unsigned type, unsigned min_vertex, unsigned num_vertices,
                          unsigned prim_count, const void* index_data, unsigned index_format,
                          const void* vertex_data, unsigned stride, bool boxed) {
    using namespace state;
    if (!Ready(boxed))
        return;
    unsigned n = 0;
    if (type == PRIM_TRIANGLE_STRIP)
        n = prim_count + 2;
    else if (type == PRIM_TRIANGLE_LIST)
        n = prim_count * 3;
    if (n == 0 || n > MAX_DRAW_INDICES || index_format != INDEX_FORMAT_16 ||
        index_data == nullptr || vertex_data == nullptr || stride < 12 || stride > 64 ||
        num_vertices == 0 ||
        !diag::Accessible(reinterpret_cast<uintptr_t>(index_data), (size_t)n * 2, false) ||
        !diag::Accessible(reinterpret_cast<uintptr_t>(vertex_data),
                          (size_t)(min_vertex + num_vertices) * stride, false)) {
        bad_draws++;
        return;
    }
    if (remap.empty())
        remap.assign(65536, -1);
    const uint16_t* idx = static_cast<const uint16_t*>(index_data);
    src.clear();
    tidx.clear();
    bool ok = true;
    for (unsigned k = 0; k < n; k++) {
        const uint32_t s = idx[k];
        if (s < min_vertex || s >= min_vertex + num_vertices) {
            ok = false;
            break;
        }
        if (remap[s] < 0) {
            remap[s] = (int32_t)src.size();
            src.push_back(s);
        }
        tidx.push_back((uint32_t)remap[s]);
    }
    for (uint32_t s : src)
        remap[s] = -1;
    if (!ok) {
        bad_draws++;
        return;
    }
    uint64_t sp = Mix(0, cur_side);
    sp = Mix(sp, cur_part);
    sp = Mix(sp, 1);
    sp = Mix(sp, reinterpret_cast<uintptr_t>(vertex_data));
    sp = Mix(sp, reinterpret_cast<uintptr_t>(index_data));
    sp = Mix(sp, num_vertices);
    sp = Mix(sp, prim_count);
    sp = Mix(sp, stride);
    EmitDraw(1, static_cast<const uint8_t*>(vertex_data), stride, type == PRIM_TRIANGLE_STRIP, sp);
}

// DrawPrimitiveUP, called with the proxy's arguments (the chunk-model path).
inline void RecordPlain(unsigned type, unsigned prim_count, const void* vertex_data,
                        unsigned stride, bool boxed) {
    using namespace state;
    if (!Ready(boxed))
        return;
    unsigned n = 0;
    if (type == PRIM_TRIANGLE_STRIP)
        n = prim_count + 2;
    else if (type == PRIM_TRIANGLE_LIST)
        n = prim_count * 3;
    if (n == 0 || n > MAX_DRAW_INDICES || vertex_data == nullptr || stride < 12 || stride > 64 ||
        !diag::Accessible(reinterpret_cast<uintptr_t>(vertex_data), (size_t)n * stride, false)) {
        bad_draws++;
        return;
    }
    // The index list was expanded into the vertex stream: its stitches are
    // repeated vertex records, which NumberStream turns back into repeated
    // indices so the runs split where the model's strips do.
    core::NumberStream(static_cast<const uint8_t*>(vertex_data), n, stride, src, tidx);
    uint64_t sp = Mix(0, cur_side);
    sp = Mix(sp, cur_part);
    sp = Mix(sp, 2);
    sp = Mix(sp, reinterpret_cast<uintptr_t>(vertex_data));
    sp = Mix(sp, n);
    sp = Mix(sp, stride);
    EmitDraw(2, static_cast<const uint8_t*>(vertex_data), stride, type == PRIM_TRIANGLE_STRIP, sp);
}

// Build one hand's geometry from the work buffers; null if it has no draws.
inline std::shared_ptr<const HeldGeometry> Build(int side, uint32_t gen) {
    using namespace state;
    auto g = std::make_shared<HeldGeometry>();
    g->side = side;
    g->generation = gen;
    g->category = category_now;
    g->kind = kind_now;
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
    bool first = true;
    for (const WorkDraw& w : wdraws) {
        if (w.side != side)
            continue;
        HeldDraw d = w.d;
        const uint32_t v0 = d.first_vertex, i0 = d.first_index;
        d.first_vertex = (uint32_t)(g->positions.size() / 3);
        d.first_index = (uint32_t)g->indices.size();
        g->positions.insert(g->positions.end(), wpos.begin() + 3 * v0,
                            wpos.begin() + 3 * (v0 + d.vertex_count));
        g->normals.insert(g->normals.end(), wnrm.begin() + 3 * v0,
                          wnrm.begin() + 3 * (v0 + d.vertex_count));
        g->indices.insert(g->indices.end(), widx.begin() + i0, widx.begin() + i0 + d.index_count);
        g->draws.push_back(d);
        (d.path == 1 ? g->draws_indexed : g->draws_plain)++;
        g->flipped += d.flipped;
        mix(d.part);
        mix(d.path);
        mix(d.vertex_count);
        mix(d.index_count);
        for (uint32_t i = 0; i < d.vertex_count * 3; i++) {
            const float p = g->positions[3 * d.first_vertex + i];
            mix((uint32_t)(int32_t)std::lround(p * 1e4f));
            const int c = (int)(i % 3);
            if (first) {
                g->box_min[c] = g->box_max[c] = p;
                if (c == 2)
                    first = false;
            } else {
                if (p < g->box_min[c]) g->box_min[c] = p;
                if (p > g->box_max[c]) g->box_max[c] = p;
            }
        }
        for (uint32_t i = 0; i < d.index_count; i++)
            mix(g->indices[d.first_index + i]);
    }
    if (g->draws.empty())
        return nullptr;
    g->hash = h;
    return g;
}

inline void LogSide(const char* name, const HeldGeometry* g) {
    if (g == nullptr) {
        diag::Log("weaponsnap:   %s: none", name);
        return;
    }
    diag::Log("weaponsnap:   %s: %zu draws (indexed %u, plain %u) %u vertices %u triangles, "
              "%u turned by the consistency pass, hash %08X, box model (%.3f %.3f %.3f) to (%.3f %.3f %.3f)",
              name, g->draws.size(), g->draws_indexed, g->draws_plain, g->VertexCount(),
              g->TriangleCount(), g->flipped, g->hash, g->box_min[0], g->box_min[1], g->box_min[2],
              g->box_max[0], g->box_max[1], g->box_max[2]);
}

inline void Publish(const FrameInfo& f) {
    using namespace state;
    const uint32_t gen = generation.fetch_add(1) + 1;
    auto main_geo = Build(0, gen);
    auto off_geo = Build(1, gen);
    std::atomic_store(&held[0], main_geo);
    std::atomic_store(&held[1], off_geo);
    armed = false;
    diag::Log("weaponsnap: snapshot %u taken: weapon %p class 0x%02X kind 0x%02X split %d, "
              "%d stable passes, owner idle",
              gen, f.weapon, f.category, f.kind, f.split_index, stable);
    LogSide("main hand", main_geo.get());
    LogSide("off-hand", off_geo.get());
    ClearWork();
}

// Drop what is published (the weapon changed or went away).
inline void Unpublish(const char* why) {
    using namespace state;
    if (held[0] == nullptr && held[1] == nullptr)
        return;
    std::atomic_store(&held[0], std::shared_ptr<const HeldGeometry>());
    std::atomic_store(&held[1], std::shared_ptr<const HeldGeometry>());
    const uint32_t gen = generation.fetch_add(1) + 1;
    diag::Log("weaponsnap: snapshot cleared (%s), generation %u", why, gen);
}

// Once per scene pass, after the weapon module has classified the weapon.
inline void OnFrame(const FrameInfo& f) {
    using namespace state;
    owner = f.owner;
    kind_now = f.kind;
    category_now = f.category;
    const bool eligible = f.weapon != nullptr && f.kind != KIND_KNUCKLE && f.kind != KIND_NONE &&
                          !f.seat_on_fist;
    const bool weapon_changed = f.weapon != key_weapon || f.category != key_category ||
                                f.variant != key_variant;
    if (weapon_changed) {
        key_weapon = f.weapon;
        key_category = f.category;
        key_variant = f.variant;
        key_split = f.split_index;
        Unpublish("weapon changed");
        armed = false;
        ClearWork();
        if (eligible)
            Arm("weapon");
        return;
    }
    if (!eligible) {
        if (armed) {
            armed = false;
            ClearWork();
        }
        if (f.weapon != nullptr && skip_logged_for != f.weapon &&
            (f.kind == KIND_KNUCKLE || f.seat_on_fist)) {
            skip_logged_for = f.weapon;
            diag::Log("weaponsnap: weapon %p class 0x%02X kind 0x%02X not recorded (%s)", f.weapon,
                      f.category, f.kind,
                      f.seat_on_fist ? "seated on the fist bone" : "worn over the hand");
        }
        return;
    }
    if (f.split_index != key_split) {
        key_split = f.split_index;
        Unpublish("twin split changed");
        Arm("split");
        return;
    }
    if (retake_request) {
        retake_request = false;
        Arm("request");
        diag::Log("weaponsnap: retake requested");
        return;
    }
    if (!armed)
        return;
    if (wdraws.empty() && !overflow && bad_draws == 0) {
        ClearWork();  // a pass that drew no weapon
        return;
    }
    armed_passes++;
    if (armed_passes > GIVE_UP_PASSES) {
        armed = false;
        ClearWork();
        diag::Log("weaponsnap: gave up on weapon %p class 0x%02X (never stable and idle)",
                  f.weapon, f.category);
        return;
    }
    bool take = false;
    if (overflow || bad_draws != 0) {
        if (!gave_up_logged) {
            gave_up_logged = true;
            diag::Log("weaponsnap: weapon %p class 0x%02X: %s (%u draws not read)", f.weapon,
                      f.category, overflow ? "over the size limit" : "a draw could not be read",
                      bad_draws);
        }
        stable = 0;
    } else if (!pass_idle) {
        stable = 0;
    } else {
        if (sig == last_sig)
            stable++;
        else {
            last_sig = sig;
            stable = 1;
        }
        take = stable >= STABLE_PASSES &&
               (!f.expects_split || f.split_index >= 0 || stable >= SPLIT_WAIT_PASSES);
    }
    if (take)
        Publish(f);
    else
        ClearWork();
}

}  // namespace weaponsnap
