#pragma once

// The glue between the mod's captured data and the hand fit
// (psobbvr_handfit_core.hpp), with no game, Windows or Direct3D
// dependencies: the welded hand mesh, its role row and fingerprint, the held
// weapon's snapshot and the seat in, the fit's Input out. Also writes a
// fit's inputs and result as a text case file.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "psobbvr_handfit_core.hpp"
#include "psobbvr_handfit_roles.hpp"
#include "psobbvr_handweld.hpp"
#include "psobbvr_weaponsnap_core.hpp"

namespace handfit {
namespace adapt {

// The welded hand as the fit reads it, with the triangle part of its
// fingerprint.
struct Hand {
    std::vector<double> points;   // 3 per welded point, grip frame, game units
    std::vector<uint32_t> tris;   // 3 per welded triangle, in weld order
    uint32_t triangle_hash = 0;   // TriangleHash of the list as 16-bit indices
};

inline uint32_t HashTriangles(const std::vector<uint32_t>& tris) {
    std::vector<uint16_t> t16(tris.size());
    for (size_t i = 0; i < tris.size(); i++)
        t16[i] = (uint16_t)tris[i];
    return TriangleHash(t16.data(), t16.size());
}

// The captured hand mesh (handweld::Build's welding: positions are the first
// draw vertex that made each point, as floats).
inline Hand HandFromMesh(const handweld::Mesh& m) {
    Hand h;
    h.points.resize((size_t)m.vertex_count * 3);
    for (int i = 0; i < m.vertex_count; i++)
        for (int c = 0; c < 3; c++)
            h.points[(size_t)i * 3 + c] = (double)m.pos[i][c];
    h.tris.resize((size_t)m.tri_count * 3);
    for (int t = 0; t < m.tri_count; t++)
        for (int c = 0; c < 3; c++)
            h.tris[(size_t)t * 3 + c] = m.tri[t][c];
    h.triangle_hash = TriangleHash(&m.tri[0][0], (size_t)m.tri_count * 3);
    return h;
}

// A hand given as doubles, in the same order and form.
inline Hand HandFromPoints(const std::vector<double>& points, const std::vector<uint32_t>& tris) {
    Hand h;
    h.points = points;
    h.tris = tris;
    h.triangle_hash = HashTriangles(tris);
    return h;
}

// Whether the role table describes a captured hand.
enum class RoleMatch : uint8_t {
    Ok = 0,
    NoRow = 1,      // no row for the class and side
    Mismatch = 2,   // a row, but the hand's fingerprint differs from it
};
inline const char* RoleMatchName(RoleMatch m) {
    return m == RoleMatch::Ok ? "matches" : m == RoleMatch::NoRow ? "no row for the class and hand" : "differs";
}

// The fingerprint test of the mesh alone (welded count, triangle count and
// the triangle list's hash), without building a Hand.
inline RoleMatch MatchMesh(int class_id, int side, const handweld::Mesh& m) {
    const RoleRow* row = Find(class_id, side);
    if (row == nullptr)
        return RoleMatch::NoRow;
    if (row->point_count != m.vertex_count || row->triangle_count != m.tri_count || m.vertex_count > kMaxPoints ||
        row->triangle_hash != TriangleHash(&m.tri[0][0], (size_t)m.tri_count * 3))
        return RoleMatch::Mismatch;
    return RoleMatch::Ok;
}

// The roles of a hand's points (side 0 = left, 1 = right), when the class's
// row matches the hand's fingerprint; roles is empty otherwise.
inline RoleMatch MatchRoles(int class_id, int side, const Hand& hand, double keep_tol_cm, double world_scale,
                            std::vector<Role>& roles) {
    roles.clear();
    const RoleRow* row = Find(class_id, side);
    if (row == nullptr)
        return RoleMatch::NoRow;
    const size_t n = hand.points.size() / 3;
    if (row->point_count != n || row->triangle_count != hand.tris.size() / 3 || n > (size_t)kMaxPoints ||
        row->triangle_hash != hand.triangle_hash)
        return RoleMatch::Mismatch;
    Role r[kMaxPoints];
    ResolveRoles(*row, keep_tol_cm, world_scale, r);
    roles.assign(r, r + n);
    return RoleMatch::Ok;
}

// One Part per recorded draw of a weapon snapshot (positions from the draw's
// vertex range, triangles from its index range, local to the draw); empty or
// malformed draws are left out.
inline std::vector<Part> WeaponParts(const weaponsnap::HeldGeometry& g) {
    std::vector<Part> parts;
    for (const weaponsnap::HeldDraw& d : g.draws) {
        if (!d.vertex_count || !d.index_count)
            continue;
        if ((size_t)(d.first_vertex + d.vertex_count) * 3 > g.positions.size() ||
            (size_t)(d.first_index + d.index_count) > g.indices.size())
            continue;
        Part p;
        p.positions.resize((size_t)d.vertex_count * 3);
        for (size_t i = 0; i < p.positions.size(); i++)
            p.positions[i] = (double)g.positions[(size_t)d.first_vertex * 3 + i];
        p.tris.resize(d.index_count - d.index_count % 3);
        for (size_t i = 0; i < p.tris.size(); i++)
            p.tris[i] = g.indices[d.first_index + i];
        parts.push_back(std::move(p));
    }
    return parts;
}

// The weapon's matrix into the grip frame as doubles: weapon scale x seat
// (row-vector; the scale applies to the three axis rows, not the
// translation).
inline void WeaponMatrix(const float seat[16], float scale, double out[16]) {
    for (int i = 0; i < 16; i++)
        out[i] = (i < 12) ? (double)seat[i] * (double)scale : (double)seat[i];
}

// Where the weapon sits on the hand, in the hand's own grip frame.
struct Placement {
    double seat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};    // the seat, without the weapon scale
    double weapon[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // weapon scale x seat
    bool has_channel = false;               // the fist's handle line below is given
    double channel_point[2] = {0, 0};       // where it crosses the grip frame's z = 0 plane
    double channel_dir[3] = {0, 0, 1};      // its direction
    double world_scale = 9;
};

// The fit's inputs for a hand (side 0 = left, 1 = right) holding weapon at a
// placement, as the hand's own problem (a left hand goes to SolveMirrored
// as it is). Fails, leaving in empty, when the role table does not describe
// the hand.
inline RoleMatch MakeInput(int class_id, int side, const Hand& hand, std::vector<Part> weapon, const Placement& at,
                           const Options& options, Input& in) {
    in = Input();
    const RoleMatch m = MatchRoles(class_id, side, hand, options.keep_tol_cm, at.world_scale, in.roles);
    if (m != RoleMatch::Ok)
        return m;
    in.points = hand.points;
    in.tris = hand.tris;
    in.has_channel = at.has_channel;
    for (int k = 0; k < 2; k++)
        in.channel_point[k] = at.channel_point[k];
    for (int k = 0; k < 3; k++)
        in.channel_dir[k] = at.channel_dir[k];
    for (int k = 0; k < 16; k++) {
        in.seat_matrix[k] = at.seat[k];
        in.weapon_matrix[k] = at.weapon[k];
    }
    in.weapon = std::move(weapon);
    in.world_scale = at.world_scale;
    in.options = options;
    return RoleMatch::Ok;
}

// ------------------------------------------------------------------ case files

// What a case says about itself (for reading only, except the side word).
struct CaseInfo {
    std::string name = "live";
    std::string comment;           // the first comment line, without the '#'
    std::string class_name = "unknown";
    std::string side = "right";
    std::string build = "live";
    double pad[2] = {0.5, 0.5};
    std::string model = "unknown";
    int kind = 0;
    std::string seat = "live";
    double weapon_scale = 0.7;
};

namespace detail {
inline void Append(std::string& s, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    va_list b;
    va_copy(b, a);
    const int n = vsnprintf(nullptr, 0, fmt, a);
    va_end(a);
    if (n > 0) {
        const size_t at = s.size();
        s.resize(at + (size_t)n + 1);
        vsnprintf(&s[at], (size_t)n + 1, fmt, b);
        s.resize(at + (size_t)n);
    }
    va_end(b);
}
// A word token: no whitespace, never empty.
inline std::string Word(const std::string& w) {
    std::string o = w.empty() ? "-" : w;
    for (char& c : o)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            c = '_';
    return o;
}
inline void Nums(std::string& s, const double* v, size_t n) {
    for (size_t i = 0; i < n; i++)
        Append(s, i ? " %.17g" : "%.17g", v[i]);
    s += '\n';
}
inline void Inds(std::string& s, const std::vector<uint32_t>& v) {
    for (size_t i = 0; i < v.size(); i++)
        Append(s, i ? " %u" : "%u", (unsigned)v[i]);
    s += '\n';
}
inline const char* RoleWord(Role r) {
    switch (r) {
    case Role::Index: return "index";
    case Role::Outer: return "outer";
    case Role::Thumb: return "thumb";
    default: return "kept";
    }
}
inline const char* UnclearWord(Unclear u) { return u == Unclear::Rest ? "rest" : "least"; }
// +1 when the triangles wind outward (positive enclosed volume), else -1.
inline int Outward(const Input& in) {
    double v = 0;
    for (size_t t = 0; t + 2 < in.tris.size(); t += 3) {
        const double* a = &in.points[3 * (size_t)in.tris[t]];
        const double* b = &in.points[3 * (size_t)in.tris[t + 1]];
        const double* c = &in.points[3 * (size_t)in.tris[t + 2]];
        v += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    return v >= 0 ? 1 : -1;
}
}  // namespace detail

// The case text for inputs as Solve runs them and, when given and ok, their
// fit as the expected lines (no trace).
inline std::string WriteCase(const CaseInfo& info, const Input& in, const Fit* fit) {
    using namespace detail;
    std::string s;
    if (!info.comment.empty())
        s += "# " + info.comment + "\n";
    s += "handfit 1\n";
    s += "case " + Word(info.name) + "\n";
    s += "class " + Word(info.class_name) + "\n";
    s += "side " + Word(info.side) + "\n";
    s += "build " + Word(info.build) + "\n";
    Append(s, "pad %.17g %.17g\n", info.pad[0], info.pad[1]);
    Append(s, "model %s %d\n", Word(info.model).c_str(), info.kind < 0 ? 0 : info.kind);
    s += "seat " + Word(info.seat) + "\n";
    Append(s, "world_scale %.17g\n", in.world_scale);
    Append(s, "weapon_scale %.17g\n", info.weapon_scale);
    const Options& o = in.options;
    s += "option blockRule hinge\noption thumbRule flat\noption spike false\n";
    Append(s, "option fitWeapon %s\n", o.fit_weapon ? "true" : "false");
    Append(s, "option followSeat %s\n", o.follow_seat ? "true" : "false");
    Append(s, "option keepTolCm %.17g\n", o.keep_tol_cm);
    Append(s, "option thumbFlatGapCm %.17g\n", o.gap_cm);
    Append(s, "option thumbSampleCm %.17g\n", o.sample_cm);
    Append(s, "option blockMaxDeg %.17g\n", o.block_max_deg);
    Append(s, "option blockStepDeg %.17g\n", o.block_step_deg);
    Append(s, "option blockJudgeShare %.17g\n", o.block_judge_share);
    Append(s, "option blockUnclear %s\n", UnclearWord(o.block_unclear));
    // the unclear limits are written only when not 1 (an absent one means 1)
    if (o.block_unclear_limit != 1)
        Append(s, "option blockUnclearLimit %.17g\n", o.block_unclear_limit);
    Append(s, "option thumbFlatMaxCm %.17g\n", o.thumb_max_cm);
    Append(s, "option thumbFlatStepCm %.17g\n", o.thumb_step_cm);
    Append(s, "option thumbShareFrom %.17g\n", o.thumb_share_from);
    Append(s, "option thumbShareTo %.17g\n", o.thumb_share_to);
    Append(s, "option thumbJudgeShare %.17g\n", o.thumb_judge_share);
    Append(s, "option thumbUnclear %s\n", UnclearWord(o.thumb_unclear));
    if (o.thumb_unclear_limit != 1)
        Append(s, "option thumbUnclearLimit %.17g\n", o.thumb_unclear_limit);
    // the thumb's sink is written only when not 0 (an absent one means 0)
    if (o.thumb_sink_cm != 0)
        Append(s, "option thumbSinkCm %.17g\n", o.thumb_sink_cm);
    // each block to its own angle: written only when on (an absent one means off)
    if (o.block_each)
        Append(s, "option blockEach true\n");
    if (in.has_channel)
        Append(s, "channel 1 %.17g %.17g  %.17g %.17g %.17g\n", in.channel_point[0], in.channel_point[1],
               in.channel_dir[0], in.channel_dir[1], in.channel_dir[2]);
    else
        s += "channel 0\n";
    const size_t n = in.points.size() / 3;
    Append(s, "hand %zu %zu\n", n, in.tris.size() / 3);
    Nums(s, in.points.data(), in.points.size());
    Inds(s, in.tris);
    Append(s, "outward %d\n", Outward(in));
    Append(s, "roles %zu\n", in.roles.size());
    for (size_t v = 0; v < in.roles.size(); v++)
        Append(s, v ? " %s" : "%s", RoleWord(in.roles[v]));
    s += '\n';
    Append(s, "weapon_parts %zu\n", in.weapon.size());
    for (const Part& p : in.weapon) {
        Append(s, "part %zu %zu\n", p.positions.size() / 3, p.tris.size() / 3);
        Nums(s, p.positions.data(), p.positions.size());
        Inds(s, p.tris);
    }
    s += "seat_matrix ";
    Nums(s, in.seat_matrix, 16);
    s += "weapon_matrix ";
    Nums(s, in.weapon_matrix, 16);
    if (fit != nullptr && fit->ok) {
        const Frame& F = fit->frame;
        Append(s, "expect_frame %.17g %.17g %.17g  %.17g %.17g %.17g  %.17g %.17g %.17g  %.17g %.17g %.17g  %.17g\n",
               F.origin[0], F.origin[1], F.origin[2], F.ef[0], F.ef[1], F.ef[2], F.eh[0], F.eh[1], F.eh[2], F.es[0],
               F.es[1], F.es[2], F.cm_per_unit);
        if (fit->fingers.ran)
            Append(s, "expect_fingers %.17g %d %d %d\n", fit->fingers.value, fit->fingers.bad, fit->fingers.cuts,
                   fit->fingers.clean ? 1 : 0);
        else
            s += "expect_fingers - - - -\n";
        static const char* const kNames[2] = {"index", "outer"};
        for (int b = 0; b < 2; b++) {
            const BlockFit& B = fit->blocks[b];
            if (!B.present)
                continue;
            if (B.sign != 0)
                Append(s, "expect_block %s %d %.17g  %.17g %.17g %.17g  %d\n", kNames[b], B.sign, B.deg, B.pivot[0],
                       B.pivot[1], B.pivot[2], B.tip);
            else
                Append(s, "expect_block %s - %.17g  %.17g %.17g %.17g  %d\n", kNames[b], B.deg, B.pivot[0], B.pivot[1],
                       B.pivot[2], B.tip);
        }
        if (fit->thumb.present) {
            const ThumbFit& T = fit->thumb;
            Append(s, "expect_thumb %.17g %d %d %d\n", T.search.value, T.search.bad, T.search.cuts,
                   T.search.clean ? 1 : 0);
            Append(s, "expect_thumb_frame %.17g %.17g %.17g  %.17g %.17g %.17g  %d %.17g\n", T.pivot[0], T.pivot[1],
                   T.pivot[2], T.axis[0], T.axis[1], T.axis[2], T.tip, T.reach);
        }
        Append(s, "expect_points %zu\n", fit->points.size() / 3);
        Nums(s, fit->points.data(), fit->points.size());
    }
    s += "end\n";
    return s;
}

// The case for a fit as the mod runs it. A case holds the problem Solve ran:
// for a left hand (mirrored: solved by SolveMirrored) that is the hand
// mirrored into a right one, so the inputs are written mirrored and the
// points are the mirrored problem's; the side word still says which hand it
// was. as_given is the hand's own problem, as passed to Solve or
// SolveMirrored.
inline std::string WriteSolvedCase(const CaseInfo& info, const Input& as_given, bool mirrored, const Fit* fit) {
    if (!mirrored)
        return WriteCase(info, as_given, fit);
    const Input m = MirrorInput(as_given);
    if (fit == nullptr)
        return WriteCase(info, m, nullptr);
    Fit f = *fit;
    for (size_t i = 0; i < f.points.size(); i += 3)
        f.points[i] = -f.points[i];
    return WriteCase(info, m, &f);
}

}  // namespace adapt
}  // namespace handfit
