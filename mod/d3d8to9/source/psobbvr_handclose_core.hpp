#pragma once

// Closing the fist around the handle: the pure maths. The hand is a rigid
// mitten; its inner curl is reshaped around the weapon it holds. Everything
// runs in the grip frame (bone 48's) on the welded vertices of the hand, so
// a vertex shared by several draws moves once and every draw gets the same
// move. No game, D3D or Windows dependencies: standard library only, double
// precision, nothing here is meant for a per-frame path.
//
// Conventions: positions are flat x, y, z; triangles are flat index triples;
// matrices are row-vector (point x matrix) with the translation in elements
// 12-14; +y runs from the fingers toward the wrist; distances are in game
// units and converted with cm_per_unit.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace handclose {

// The shaping settings (distances in cm).
//   gap_cm      how far the inner surface rests off the weapon's surface
//   reach_cm    a vertex outside the weapon is pulled in only from this near
//   cap_cm      the largest move of any vertex
//   fade_cm     the pulling in fades to zero over this length toward the wrist
//   inner_cos   a vertex is "inner" when its normal points at the handle's
//               axis within this cosine (0.2 = about 78 degrees)
//   wrist_share the share of the hand's length, at its +y end, that is the
//               wrist ring (held still)
struct Options {
    double gap_cm = 1.0;
    double reach_cm = 4.0;
    double cap_cm = 2.0;
    double fade_cm = 3.0;
    double inner_cos = 0.2;
    double wrist_share = 0.15;
};

// One draw's vertices and triangles.
struct Part {
    std::vector<double> positions;
    std::vector<uint32_t> tris;
};

// Distinct vertices of several parts, the triangles over them (collapsed
// ones dropped) and, per part, each vertex's welded index.
struct Welded {
    std::vector<double> pos;
    std::vector<uint32_t> tris;
    std::vector<std::vector<int>> map;
    int count = 0;
};

// A triangle mesh ready for distance queries: per triangle one corner and
// its two edges. Zero-area triangles are skipped.
struct Surface {
    int n = 0;
    std::vector<double> a, ab, ac;
};

struct Axis {
    double o[3] = {0, 0, 0};  // a point on it
    double a[3] = {0, 0, 1};  // its direction (need not be unit)
};

// The weapon placed in the grip frame.
struct Weapon {
    std::vector<Part> parts;         // each draw, in the grip frame
    std::vector<Surface> solid;      // one surface per draw (non-empty ones)
    std::vector<double> verts;       // welded vertices, flat
    int vert_count = 0;
    double T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool has_axis = false;           // the axis override below is in use
    Axis axis;
};

struct Stats {
    int inner = 0, engaged = 0, inside = 0, unresolved = 0, refined = 0, capped = 0, wrist = 0;
    double max_move_cm = 0;
};

struct FistResult {
    std::vector<double> pos;      // the moved vertices
    std::vector<double> normals;  // of the moved mesh
    std::vector<double> disp;     // per welded vertex, in units
    std::vector<uint8_t> inner, engaged, ring;
    Stats stats;
    Options options;
};

constexpr double kWeld = 1e-4;  // welding tolerance, units

// ---------------------------------------------------------------- welding

inline Welded weld(const std::vector<Part>& parts) {
    Welded w;
    std::map<std::array<long long, 3>, int> key;
    for (const Part& p : parts) {
        std::vector<int> m(p.positions.size() / 3, -1);
        auto id = [&](uint32_t v) -> int {
            if (m[v] >= 0) return m[v];
            const double x = p.positions[3 * v], y = p.positions[3 * v + 1], z = p.positions[3 * v + 2];
            // halves round up
            const std::array<long long, 3> k = {(long long)std::floor(x / kWeld + 0.5),
                                                (long long)std::floor(y / kWeld + 0.5),
                                                (long long)std::floor(z / kWeld + 0.5)};
            auto it = key.find(k);
            int i;
            if (it == key.end()) {
                i = (int)(w.pos.size() / 3);
                key.emplace(k, i);
                w.pos.push_back(x); w.pos.push_back(y); w.pos.push_back(z);
            } else {
                i = it->second;
            }
            m[v] = i;
            return i;
        };
        for (size_t t = 0; t + 2 < p.tris.size(); t += 3) {
            const int a = id(p.tris[t]), b = id(p.tris[t + 1]), c = id(p.tris[t + 2]);
            if (a != b && b != c && a != c) {
                w.tris.push_back((uint32_t)a); w.tris.push_back((uint32_t)b); w.tris.push_back((uint32_t)c);
            }
        }
        w.map.push_back(std::move(m));
    }
    w.count = (int)(w.pos.size() / 3);
    return w;
}

// ---------------------------------------------------------------- distance

inline Surface make_surface(const std::vector<double>& pos, const std::vector<uint32_t>& tris) {
    Surface s;
    const size_t n = tris.size() / 3;
    s.a.resize(n * 3); s.ab.resize(n * 3); s.ac.resize(n * 3);
    int used = 0;
    for (size_t t = 0; t < n; t++) {
        const size_t i0 = 3 * (size_t)tris[3 * t], i1 = 3 * (size_t)tris[3 * t + 1], i2 = 3 * (size_t)tris[3 * t + 2];
        const double e1[3] = {pos[i1] - pos[i0], pos[i1 + 1] - pos[i0 + 1], pos[i1 + 2] - pos[i0 + 2]};
        const double e2[3] = {pos[i2] - pos[i0], pos[i2 + 1] - pos[i0 + 1], pos[i2 + 2] - pos[i0 + 2]};
        const double l = std::hypot(std::hypot(e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2]),
                                    e1[0] * e2[1] - e1[1] * e2[0]);
        if (!(l > 1e-12)) continue;
        const size_t o = 3 * (size_t)used++;
        for (int c = 0; c < 3; c++) { s.a[o + c] = pos[i0 + c]; s.ab[o + c] = e1[c]; s.ac[o + c] = e2[c]; }
    }
    s.n = used;
    s.a.resize((size_t)used * 3); s.ab.resize((size_t)used * 3); s.ac.resize((size_t)used * 3);
    return s;
}

// Closest point of triangle t of a surface to p (Ericson's regions): the
// squared distance, and the point in q.
inline double closest_on_triangle(const Surface& s, int t, const double p[3], double q[3]) {
    const size_t i = 3 * (size_t)t;
    const double ax = s.a[i], ay = s.a[i + 1], az = s.a[i + 2];
    const double abx = s.ab[i], aby = s.ab[i + 1], abz = s.ab[i + 2];
    const double acx = s.ac[i], acy = s.ac[i + 1], acz = s.ac[i + 2];
    const double apx = p[0] - ax, apy = p[1] - ay, apz = p[2] - az;
    const double d1 = abx * apx + aby * apy + abz * apz, d2 = acx * apx + acy * apy + acz * apz;
    auto at = [&](double u, double v) {
        q[0] = ax + u * abx + v * acx; q[1] = ay + u * aby + v * acy; q[2] = az + u * abz + v * acz;
    };
    if (d1 <= 0 && d2 <= 0) {
        at(0, 0);
    } else {
        const double bpx = apx - abx, bpy = apy - aby, bpz = apz - abz;
        const double d3 = abx * bpx + aby * bpy + abz * bpz, d4 = acx * bpx + acy * bpy + acz * bpz;
        if (d3 >= 0 && d4 <= d3) {
            at(1, 0);
        } else {
            const double vc = d1 * d4 - d3 * d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0) {
                at(d1 / (d1 - d3), 0);
            } else {
                const double cpx = apx - acx, cpy = apy - acy, cpz = apz - acz;
                const double d5 = abx * cpx + aby * cpy + abz * cpz, d6 = acx * cpx + acy * cpy + acz * cpz;
                if (d6 >= 0 && d5 <= d6) {
                    at(0, 1);
                } else {
                    const double vb = d5 * d2 - d1 * d6;
                    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
                        at(0, d2 / (d2 - d6));
                    } else {
                        const double va = d3 * d6 - d5 * d4;
                        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
                            const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                            at(1 - w, w);
                        } else {
                            const double den = 1 / (va + vb + vc);
                            at(vb * den, vc * den);
                        }
                    }
                }
            }
        }
    }
    const double dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return dx * dx + dy * dy + dz * dz;
}

// The winding number of a surface around p: the sum of the solid angles its
// triangles subtend (Van Oosterom and Strackee) over 4 pi. +-1 inside a
// closed surface, 0 outside; for an open one it falls off smoothly, which is
// what makes it usable on the weapons (several draws are not quite closed)
// and on the hand (open at the wrist).
inline double winding_number(const Surface& s, const double p[3]) {
    double total = 0;
    for (int t = 0; t < s.n; t++) {
        const size_t i = 3 * (size_t)t;
        const double ax = s.a[i] - p[0], ay = s.a[i + 1] - p[1], az = s.a[i + 2] - p[2];
        const double bx = ax + s.ab[i], by = ay + s.ab[i + 1], bz = az + s.ab[i + 2];
        const double cx = ax + s.ac[i], cy = ay + s.ac[i + 1], cz = az + s.ac[i + 2];
        const double la = std::sqrt(ax * ax + ay * ay + az * az);
        const double lb = std::sqrt(bx * bx + by * by + bz * bz);
        const double lc = std::sqrt(cx * cx + cy * cy + cz * cz);
        const double det = ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx);
        const double den = la * lb * lc + (ax * bx + ay * by + az * bz) * lc + (ax * cx + ay * cy + az * cz) * lb +
                           (bx * cx + by * cy + bz * cz) * la;
        total += 2 * std::atan2(det, den);
    }
    return total / (4 * 3.14159265358979323846);
}

struct Signed {
    bool ok = false;  // false: nothing to measure against
    double d = 0;     // distance, negative inside
    double q[3] = {0, 0, 0};  // the closest point on the surface
};

// Signed distance from p to a surface: the distance to the closest triangle,
// negative when the winding number's magnitude passes one half. The sign of
// the winding number follows the triangles' winding, so a surface whose
// triangles do not all wind the same way (a piece turned inward among
// outward ones) cancels itself and misjudges "inside"; the weapon snapshot
// is wound consistently and outward before it gets here.
inline Signed signed_distance(const Surface& s, const double p[3]) {
    Signed r;
    if (!s.n) return r;
    double best = INFINITY, bq[3] = {0, 0, 0}, q[3];
    for (int t = 0; t < s.n; t++) {
        const double d2 = closest_on_triangle(s, t, p, q);
        if (d2 < best) { best = d2; bq[0] = q[0]; bq[1] = q[1]; bq[2] = q[2]; }
    }
    const double dist = std::sqrt(best);
    const bool inside = dist > 1e-12 && std::fabs(winding_number(s, p)) > 0.5;
    r.ok = true;
    r.d = inside ? -dist : dist;
    r.q[0] = bq[0]; r.q[1] = bq[1]; r.q[2] = bq[2];
    return r;
}

// A solid made of several surfaces (a weapon's draws): the union, so the
// signed distance is the smallest of the parts' (negative inside any).
inline Signed solid_distance(const std::vector<Surface>& solid, const double p[3]) {
    Signed best;
    for (const Surface& s : solid) {
        const Signed r = signed_distance(s, p);
        if (r.ok && (!best.ok || r.d < best.d)) best = r;
    }
    return best;
}

// Where a ray (origin o, direction d) crosses a surface: the distances t
// greater than a hair, appended unsorted. Both sides of a triangle count.
inline void ray_hits(const Surface& s, const double o[3], const double d[3], std::vector<double>& hits) {
    for (int t = 0; t < s.n; t++) {
        const size_t i = 3 * (size_t)t;
        const double e1x = s.ab[i], e1y = s.ab[i + 1], e1z = s.ab[i + 2];
        const double e2x = s.ac[i], e2y = s.ac[i + 1], e2z = s.ac[i + 2];
        const double px = d[1] * e2z - d[2] * e2y, py = d[2] * e2x - d[0] * e2z, pz = d[0] * e2y - d[1] * e2x;
        const double det = e1x * px + e1y * py + e1z * pz;
        if (std::fabs(det) < 1e-14) continue;
        const double inv = 1 / det;
        const double tx = o[0] - s.a[i], ty = o[1] - s.a[i + 1], tz = o[2] - s.a[i + 2];
        const double u = (tx * px + ty * py + tz * pz) * inv;
        if (u < 0 || u > 1) continue;
        const double qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x;
        const double v = (d[0] * qx + d[1] * qy + d[2] * qz) * inv;
        if (v < 0 || u + v > 1) continue;
        const double dist = (e2x * qx + e2y * qy + e2z * qz) * inv;
        if (dist > 1e-9) hits.push_back(dist);
    }
}

// ----------------------------------------------------------- the weapon

// The weapon's draws (model space, with node transforms baked) placed in the
// grip frame by T (row-vector: weapon scale x the seat's matrix; the handle
// axis is row 2 of T through its translation). axis, when given, is the
// handle's axis instead (guns, whose model axis is not their grip).
inline Weapon weapon_in_hand(const std::vector<Part>& model_parts, const double T[16], const Axis* axis = nullptr) {
    Weapon w;
    for (int i = 0; i < 16; i++) w.T[i] = T[i];
    for (const Part& mp : model_parts) {
        const size_t count = mp.positions.size() / 3;
        if (!count) continue;
        Part p;
        p.positions.resize(mp.positions.size());
        for (size_t v = 0; v < count; v++) {
            const double x = mp.positions[3 * v], y = mp.positions[3 * v + 1], z = mp.positions[3 * v + 2];
            p.positions[3 * v] = x * T[0] + y * T[4] + z * T[8] + T[12];
            p.positions[3 * v + 1] = x * T[1] + y * T[5] + z * T[9] + T[13];
            p.positions[3 * v + 2] = x * T[2] + y * T[6] + z * T[10] + T[14];
        }
        for (size_t t = 0; t + 2 < mp.tris.size(); t += 3)
            if (mp.tris[t] < count && mp.tris[t + 1] < count && mp.tris[t + 2] < count) {
                p.tris.push_back(mp.tris[t]); p.tris.push_back(mp.tris[t + 1]); p.tris.push_back(mp.tris[t + 2]);
            }
        w.parts.push_back(std::move(p));
    }
    const Welded wd = weld(w.parts);
    w.verts = wd.pos;
    w.vert_count = wd.count;
    for (const Part& p : w.parts) {
        Surface s = make_surface(p.positions, p.tris);
        if (s.n) w.solid.push_back(std::move(s));
    }
    if (axis) { w.has_axis = true; w.axis = *axis; }
    return w;
}

// ------------------------------------------------------------- the hand

// Unit vertex normals of a welded mesh (area-weighted), pointing out of the
// hand. The winding is judged from the mesh's signed volume about its
// centroid (for the open hand, the volume as if it were closed): a negative
// volume means the triangles wind inward, and the normals flip.
inline std::vector<double> mesh_normals(const std::vector<double>& pos, const std::vector<uint32_t>& tris, int count) {
    std::vector<double> n((size_t)count * 3, 0.0);
    double cx = 0, cy = 0, cz = 0;
    for (int v = 0; v < count; v++) { cx += pos[3 * v]; cy += pos[3 * v + 1]; cz += pos[3 * v + 2]; }
    const double dn = count ? (double)count : 1.0;
    cx /= dn; cy /= dn; cz /= dn;
    double volume = 0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const size_t a = 3 * (size_t)tris[t], b = 3 * (size_t)tris[t + 1], c = 3 * (size_t)tris[t + 2];
        const double e1[3] = {pos[b] - pos[a], pos[b + 1] - pos[a + 1], pos[b + 2] - pos[a + 2]};
        const double e2[3] = {pos[c] - pos[a], pos[c + 1] - pos[a + 1], pos[c + 2] - pos[a + 2]};
        const double fx = e1[1] * e2[2] - e1[2] * e2[1], fy = e1[2] * e2[0] - e1[0] * e2[2], fz = e1[0] * e2[1] - e1[1] * e2[0];
        volume += (pos[a] - cx) * fx + (pos[a + 1] - cy) * fy + (pos[a + 2] - cz) * fz;
        for (size_t k : {a, b, c}) { n[k] += fx; n[k + 1] += fy; n[k + 2] += fz; }
    }
    const double sign = volume < 0 ? -1.0 : 1.0;
    for (int v = 0; v < count; v++) {
        double l = std::sqrt(n[3 * v] * n[3 * v] + n[3 * v + 1] * n[3 * v + 1] + n[3 * v + 2] * n[3 * v + 2]);
        if (l == 0) l = 1;
        n[3 * v] *= sign / l; n[3 * v + 1] *= sign / l; n[3 * v + 2] *= sign / l;
    }
    return n;
}

struct Ring {
    std::vector<uint8_t> ring;
    double y_max = 0, y_min = 0, y_edge = 0;
};

// The wrist ring: the vertices in the top `share` of the hand's y extent.
inline Ring wrist_ring(const std::vector<double>& pos, int count, double share) {
    double y_max = -INFINITY, y_min = INFINITY;
    for (int v = 0; v < count; v++) { y_max = std::max(y_max, pos[3 * v + 1]); y_min = std::min(y_min, pos[3 * v + 1]); }
    Ring r;
    r.ring.assign((size_t)count, 0);
    for (int v = 0; v < count; v++)
        if (pos[3 * v + 1] >= y_max - share * (y_max - y_min) - 1e-9) r.ring[v] = 1;
    r.y_max = y_max; r.y_min = y_min; r.y_edge = y_max - share * (y_max - y_min);
    return r;
}

struct Target {
    bool valid = false;     // false: left alone
    bool has_move = false;  // false (with valid): inside but no way out found
    bool inside = false;
    double move[3] = {0, 0, 0};
};

// The move one hand vertex wants. axis: a point on the handle's axis and its
// unit direction; solid: the weapon's surfaces; gap and reach in units;
// pull_in: whether a vertex outside the weapon may be pulled in (the inner
// ones).
// - A vertex inside the weapon moves out until it leaves the surface, plus
//   the gap. Two ways out are tried and the shorter move is taken: along the
//   ray from the axis through the vertex (the handle's own radial direction),
//   and straight through the nearest point of the surface (which wins where
//   the weapon is not round, or the radial way runs on through a thick body).
// - A vertex outside it, pull_in set, within reach moves in along the ray
//   toward the axis to the surface, less the gap.
inline Target fist_target(const double v[3], const Axis& axis, const std::vector<Surface>& solid, double gap, double reach,
                          bool pull_in) {
    Target tg;
    const double t = (v[0] - axis.o[0]) * axis.a[0] + (v[1] - axis.o[1]) * axis.a[1] + (v[2] - axis.o[2]) * axis.a[2];
    const double r[3] = {v[0] - axis.o[0] - t * axis.a[0], v[1] - axis.o[1] - t * axis.a[1], v[2] - axis.o[2] - t * axis.a[2]};
    const double rho = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    const bool has_out = rho > 1e-6;
    const double out[3] = {has_out ? r[0] / rho : 0, has_out ? r[1] / rho : 0, has_out ? r[2] / rho : 0};
    const Signed sd = solid_distance(solid, v);
    if (!sd.ok) return tg;
    if (sd.d < 0) {
        bool have = false;
        double best_len = 0, best_move[3] = {0, 0, 0};
        if (has_out) {
            std::vector<double> hits;
            for (const Surface& s : solid) ray_hits(s, v, out, hits);
            std::sort(hits.begin(), hits.end());
            for (double h : hits) {
                const double e = 1e-4;
                const double p[3] = {v[0] + (h + e) * out[0], v[1] + (h + e) * out[1], v[2] + (h + e) * out[2]};
                const Signed q = solid_distance(solid, p);
                if (q.ok && q.d > 0) {
                    const double len = h + gap;
                    for (int c = 0; c < 3; c++) best_move[c] = out[c] * len;
                    best_len = len; have = true;
                    break;
                }
            }
        }
        const double dx = sd.q[0] - v[0], dy = sd.q[1] - v[1], dz = sd.q[2] - v[2];
        const double dl = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dl > 1e-9) {
            const double len = dl + gap;
            if (!have || len < best_len) {
                best_move[0] = dx / dl * len; best_move[1] = dy / dl * len; best_move[2] = dz / dl * len;
                best_len = len; have = true;
            }
        }
        tg.valid = true; tg.inside = true; tg.has_move = have;
        if (have) for (int c = 0; c < 3; c++) tg.move[c] = best_move[c];
        return tg;
    }
    if (!pull_in || !has_out) return tg;
    const double back[3] = {-out[0], -out[1], -out[2]};
    double first = INFINITY;
    for (const Surface& s : solid) {
        std::vector<double> h;
        ray_hits(s, v, back, h);
        if (!h.empty()) first = std::min(first, *std::min_element(h.begin(), h.end()));
    }
    if (!(first <= reach)) return tg;
    const double m = first - gap;  // inward when positive, outward if the vertex sits closer than the gap
    tg.valid = true; tg.has_move = true; tg.inside = false;
    tg.move[0] = -out[0] * m; tg.move[1] = -out[1] * m; tg.move[2] = -out[2] * m;
    return tg;
}

// Spread the held vertices' moves over the rest of the mesh: each free
// vertex takes the mean of its neighbours' moves (Gauss-Seidel sweeps over
// the triangles' edges), the held ones keep theirs. d: flat moves, changed
// in place; held: 0/1 per vertex.
inline void spread_moves(int count, const std::vector<uint32_t>& tris, std::vector<double>& d,
                         const std::vector<uint8_t>& held, int sweeps) {
    std::vector<std::vector<uint32_t>> nb((size_t)count);
    auto add = [&](uint32_t a, uint32_t b) {
        if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end()) nb[a].push_back(b);
    };
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        for (int e = 0; e < 3; e++) {
            const uint32_t a = tris[t + e], b = tris[t + (e + 1) % 3];
            add(a, b); add(b, a);
        }
    for (int it = 0; it < sweeps; it++)
        for (int v = 0; v < count; v++) {
            if (held[v] || nb[v].empty()) continue;
            double x = 0, y = 0, z = 0;
            for (uint32_t u : nb[v]) { x += d[3 * u]; y += d[3 * u + 1]; z += d[3 * u + 2]; }
            const double k = (double)nb[v].size();
            d[3 * v] = x / k; d[3 * v + 1] = y / k; d[3 * v + 2] = z / k;
        }
}

inline double smoothstep01(double x) { return x * x * (3 - 2 * x); }

// Reshape the hand around the weapon. hand: weld() of the hand's parts;
// weapon: weapon_in_hand; cm_per_unit = 0.01 * world_scale. Inner vertices
// are those inside the handle's span along the axis whose normal points at
// it; each gets fist_target's move, capped, faded toward the wrist
// (smoothstep over fade_cm from the wrist ring), then the moves spread over
// the rest of the mesh with the wrist ring held at zero. With no usable
// axis or weapon, nothing moves.
inline FistResult close_fist(const Welded& hand, const Weapon& weapon, double cm_per_unit, const Options& o) {
    const int n = hand.count;
    FistResult res;
    res.options = o;
    res.disp.assign((size_t)n * 3, 0.0);
    res.inner.assign((size_t)n, 0);
    res.engaged.assign((size_t)n, 0);
    res.ring.assign((size_t)n, 0);
    const double* T = weapon.T;
    const double wa[3] = {weapon.has_axis ? weapon.axis.a[0] : T[8], weapon.has_axis ? weapon.axis.a[1] : T[9],
                          weapon.has_axis ? weapon.axis.a[2] : T[10]};
    const double la = std::sqrt(wa[0] * wa[0] + wa[1] * wa[1] + wa[2] * wa[2]);
    const std::vector<double> normals0 = mesh_normals(hand.pos, hand.tris, n);
    if (!(la > 0) || !weapon.vert_count || weapon.solid.empty()) {
        res.pos = hand.pos;
        res.normals = normals0;
        return res;
    }
    Axis axis;
    if (weapon.has_axis) { for (int c = 0; c < 3; c++) axis.o[c] = weapon.axis.o[c]; }
    else { axis.o[0] = T[12]; axis.o[1] = T[13]; axis.o[2] = T[14]; }
    for (int c = 0; c < 3; c++) axis.a[c] = wa[c] / la;
    double t_min = INFINITY, t_max = -INFINITY;
    for (int v = 0; v < weapon.vert_count; v++) {
        const double t = (weapon.verts[3 * v] - axis.o[0]) * axis.a[0] + (weapon.verts[3 * v + 1] - axis.o[1]) * axis.a[1] +
                         (weapon.verts[3 * v + 2] - axis.o[2]) * axis.a[2];
        t_min = std::min(t_min, t); t_max = std::max(t_max, t);
    }
    const double gap = o.gap_cm * cm_per_unit, reach = o.reach_cm * cm_per_unit, cap = o.cap_cm * cm_per_unit;
    Ring wr = wrist_ring(hand.pos, n, o.wrist_share);
    res.ring = wr.ring;
    const double fade_len = std::max(o.fade_cm * cm_per_unit, 1e-9);
    std::vector<uint8_t> held((size_t)n, 0);
    Stats& st = res.stats;
    std::vector<double>& disp = res.disp;
    for (int v = 0; v < n; v++) {
        if (wr.ring[v]) { held[v] = 1; st.wrist++; continue; }
        const double p[3] = {hand.pos[3 * v], hand.pos[3 * v + 1], hand.pos[3 * v + 2]};
        const double t = (p[0] - axis.o[0]) * axis.a[0] + (p[1] - axis.o[1]) * axis.a[1] + (p[2] - axis.o[2]) * axis.a[2];
        const double r[3] = {p[0] - axis.o[0] - t * axis.a[0], p[1] - axis.o[1] - t * axis.a[1], p[2] - axis.o[2] - t * axis.a[2]};
        const double rho = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        const double facing = rho > 1e-6
            ? -(normals0[3 * v] * r[0] + normals0[3 * v + 1] * r[1] + normals0[3 * v + 2] * r[2]) / rho : -1.0;
        const bool is_inner = t >= t_min && t <= t_max && facing >= o.inner_cos;
        if (is_inner) { res.inner[v] = 1; st.inner++; }
        const Target tg = fist_target(p, axis, weapon.solid, gap, reach, is_inner);
        if (!tg.valid) continue;
        if (tg.inside) st.inside++;
        if (!tg.has_move) { st.unresolved++; continue; }
        double m[3] = {tg.move[0], tg.move[1], tg.move[2]};
        const double len = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        if (len > cap) { for (double& c : m) c = c * cap / len; st.capped++; }
        // The fade eases the pulling in; a vertex inside the weapon always
        // comes fully out, or it would still pass through the handle.
        const double x = std::min(1.0, std::max(0.0, (wr.y_edge - p[1]) / fade_len));
        const double w = tg.inside ? 1.0 : smoothstep01(x);
        disp[3 * v] = m[0] * w; disp[3 * v + 1] = m[1] * w; disp[3 * v + 2] = m[2] * w;
        res.engaged[v] = 1;
        held[v] = 1;
        st.engaged++;
    }
    spread_moves(n, hand.tris, disp, held, 300);
    // The spread can carry a free vertex into the weapon. A few rounds: any
    // free vertex now inside is moved out like the others, and the moves are
    // spread again.
    for (int round = 0; round < 4; round++) {
        int fixed = 0;
        for (int v = 0; v < n; v++) {
            if (held[v]) continue;
            const double p[3] = {hand.pos[3 * v] + disp[3 * v], hand.pos[3 * v + 1] + disp[3 * v + 1],
                                 hand.pos[3 * v + 2] + disp[3 * v + 2]};
            const Signed sd = solid_distance(weapon.solid, p);
            if (!sd.ok || !(sd.d < 0)) continue;
            const Target tg = fist_target(p, axis, weapon.solid, gap, reach, false);
            if (!tg.valid || !tg.has_move) continue;
            double m[3] = {disp[3 * v] + tg.move[0], disp[3 * v + 1] + tg.move[1], disp[3 * v + 2] + tg.move[2]};
            const double len = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            if (len > cap) { for (double& c : m) c = c * cap / len; st.capped++; }
            disp[3 * v] = m[0]; disp[3 * v + 1] = m[1]; disp[3 * v + 2] = m[2];
            held[v] = 1;
            res.engaged[v] = 1;
            st.engaged++;
            st.refined++;
            fixed++;
        }
        if (!fixed) break;
        spread_moves(n, hand.tris, disp, held, 300);
    }
    res.pos = hand.pos;
    for (int v = 0; v < n; v++) {
        for (int c = 0; c < 3; c++) res.pos[3 * v + c] += disp[3 * v + c];
        st.max_move_cm = std::max(st.max_move_cm,
            std::sqrt(disp[3 * v] * disp[3 * v] + disp[3 * v + 1] * disp[3 * v + 1] + disp[3 * v + 2] * disp[3 * v + 2]) / cm_per_unit);
    }
    res.normals = mesh_normals(res.pos, hand.tris, n);
    return res;
}

// ------------------------------------------------------------- self test

namespace detail {

inline Part box_mesh(const double lo[3], const double hi[3], bool flip = false) {
    Part p;
    for (int i = 0; i < 8; i++) {
        p.positions.push_back(i & 1 ? hi[0] : lo[0]);
        p.positions.push_back(i & 2 ? hi[1] : lo[1]);
        p.positions.push_back(i & 4 ? hi[2] : lo[2]);
    }
    // faces as quads of corner indices, wound outward
    const int quads[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    for (const auto& q : quads) {
        const uint32_t t[6] = {(uint32_t)q[0], (uint32_t)q[1], (uint32_t)q[2], (uint32_t)q[0], (uint32_t)q[2], (uint32_t)q[3]};
        if (flip) { p.tris.insert(p.tris.end(), {t[0], t[2], t[1], t[3], t[5], t[4]}); }
        else { p.tris.insert(p.tris.end(), t, t + 6); }
    }
    return p;
}

inline Part cylinder_mesh(int sides, double radius, double length) {
    Part p;
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < sides; i++) {
        const double a = 2 * pi * i / sides;
        p.positions.insert(p.positions.end(), {radius * std::cos(a), radius * std::sin(a), 0.0,
                                               radius * std::cos(a), radius * std::sin(a), length});
    }
    p.positions.insert(p.positions.end(), {0, 0, 0, 0, 0, length});
    for (int i = 0; i < sides; i++) {
        const uint32_t j = (uint32_t)((i + 1) % sides), k = (uint32_t)i;
        p.tris.insert(p.tris.end(), {2 * k, 2 * j, 2 * j + 1, 2 * k, 2 * j + 1, 2 * k + 1});
        p.tris.insert(p.tris.end(), {(uint32_t)(2 * sides), 2 * j, 2 * k});
        p.tris.insert(p.tris.end(), {(uint32_t)(2 * sides + 1), 2 * k + 1, 2 * j + 1});
    }
    return p;
}

inline Surface surface_of(const Part& p) { return make_surface(p.positions, p.tris); }

inline double sd_of(const Part& m, double x, double y, double z) {
    const double p[3] = {x, y, z};
    return signed_distance(surface_of(m), p).d;
}

}  // namespace detail

// The core's own checks on synthetic shapes (a box, a cylinder, a hand box
// beside a cylinder handle). Appends one line per check to `report`;
// returns whether all passed.
inline bool self_test(std::string& report) {
    bool all = true;
    char buf[512];
    auto check = [&](const char* name, bool ok, const char* detail) {
        std::snprintf(buf, sizeof buf, "%s  %s: %s\n", ok ? "ok  " : "FAIL", name, detail);
        report += buf;
        if (!ok) all = false;
    };
    auto approx = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char det[400];

    const double z3[3] = {0, 0, 0}, o3[3] = {1, 1, 1};
    const Part unit = detail::box_mesh(z3, o3), unit_flip = detail::box_mesh(z3, o3, true);
    const double sd_box[8] = {detail::sd_of(unit, 0.5, 0.5, 0.5), detail::sd_of(unit, 0.1, 0.5, 0.5),
                              detail::sd_of(unit, 1.3, 0.5, 0.5), detail::sd_of(unit, 1.3, 1.4, 0.5),
                              detail::sd_of(unit, 1.2, 1.2, 1.2), detail::sd_of(unit, 1, 0.5, 0.5),
                              detail::sd_of(unit_flip, 0.1, 0.5, 0.5), detail::sd_of(unit_flip, 1.3, 0.5, 0.5)};
    const double sd_want[8] = {-0.5, -0.1, 0.3, 0.5, std::sqrt(0.12), 0, -0.1, 0.3};
    bool ok = true;
    for (int i = 0; i < 8; i++) ok = ok && approx(sd_box[i], sd_want[i], 1e-9);
    check("signed distance, box", ok, "centre, inside a face, off a face, off an edge, off a corner, on a face, reversed winding");

    const int N = 48;
    const Part cyl = detail::cylinder_mesh(N, 1.0, 2.0);
    const double c0 = detail::sd_of(cyl, 0.5, 0, 1), c1 = detail::sd_of(cyl, 1.5, 0, 1), c2 = detail::sd_of(cyl, 0, 0, 2.4),
                 c3 = detail::sd_of(cyl, 0, 0, 1);
    std::snprintf(det, sizeof det, "%.4f %.4f %.4f %.4f (expected about -0.5 0.5 0.4 -1)", c0, c1, c2, c3);
    check("signed distance, cylinder", approx(c0, -0.5, 0.01) && approx(c1, 0.5, 0.01) && approx(c2, 0.4, 1e-9) && approx(c3, -1, 0.01), det);

    // welding: two boxes sharing a face
    const double lo2[3] = {1, 0, 0}, hi2[3] = {2, 1, 1};
    const Part wa = unit, wb = detail::box_mesh(lo2, hi2);
    const Welded wd = weld({wa, wb});
    std::snprintf(det, sizeof det, "%d distinct vertices (expected 12), %zu triangle indices (72)", wd.count, wd.tris.size());
    check("welding", wd.count == 12 && wd.map.size() == 2 && wd.map[0][1] == wd.map[1][0] && wd.tris.size() == 72, det);

    // spreading: a strip of three triangles, the ends held
    std::vector<double> dp(15, 0.0);
    dp[12] = 4;
    spread_moves(5, {0, 1, 2, 1, 2, 3, 2, 3, 4}, dp, {1, 0, 0, 0, 1}, 500);
    std::snprintf(det, sizeof det, "%.4f %.4f %.4f (expected 1.5 2 2.5)", dp[3], dp[6], dp[9]);
    check("moves spread", approx(dp[3], 1.5, 1e-6) && approx(dp[6], 2, 1e-6) && approx(dp[9], 2.5, 1e-6), det);

    // the handle: the cylinder with the identity seat, axis z through the origin
    const double ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const Weapon cw = weapon_in_hand({cyl}, ident);
    const double apothem = std::cos(3.14159265358979323846 / N);
    const double mid[3] = {apothem, std::sin(3.14159265358979323846 / N), 0};
    const Axis axz;  // origin, +z
    const double gap_u = 0.02, reach_u = 0.3;
    auto at = [&](double r, double out[3]) { out[0] = mid[0] * r; out[1] = mid[1] * r; out[2] = 1; };
    double p_in[3], p_near[3], p_far[3];
    at(0.5, p_in); at(1.2, p_near); at(2.0, p_far);
    const Target t_in = fist_target(p_in, axz, cw.solid, gap_u, reach_u, true);
    const Target t_near = fist_target(p_near, axz, cw.solid, gap_u, reach_u, true);
    const Target t_far = fist_target(p_far, axz, cw.solid, gap_u, reach_u, true);
    auto radius_after = [](const double p[3], const Target& t) { return std::hypot(p[0] + t.move[0], p[1] + t.move[1]); };
    std::snprintf(det, sizeof det, "inside ends at %.4f, near at %.4f (side + gap %.4f), far %s", radius_after(p_in, t_in),
                  radius_after(p_near, t_near), apothem + gap_u, t_far.valid ? "MOVES" : "stays");
    check("fist target: inside out, near in, far alone",
          t_in.valid && t_in.inside && approx(radius_after(p_in, t_in), apothem + gap_u, 1e-6) && t_near.valid && !t_near.inside &&
          approx(radius_after(p_near, t_near), apothem + gap_u, 1e-6) && !t_far.valid, det);

    // a hand box beside the handle: x 0.4 to 1.4, y -1 to 3 (+y the wrist), z 0.2 to 1.8
    const double hlo[3] = {0.4, -1, 0.2}, hhi[3] = {1.4, 3, 1.8};
    const Welded hand = weld({detail::box_mesh(hlo, hhi)});
    Options base_o;
    base_o.gap_cm = 0.15; base_o.reach_cm = 1.5; base_o.cap_cm = 2; base_o.fade_cm = 0.01; base_o.inner_cos = 0.05;
    const FistResult base = close_fist(hand, cw, 0.09, base_o);
    std::vector<int> eng;
    for (int i = 0; i < hand.count; i++) if (base.engaged[i]) eng.push_back(i);
    bool ring_ok = eng.size() == 2;
    for (int i : eng) ring_ok = ring_ok && approx(hand.pos[3 * i + 1], -1, 1e-9) &&
        approx(std::hypot(base.pos[3 * i], base.pos[3 * i + 1]), apothem + 0.15 * 0.09, 0.003);
    std::snprintf(det, sizeof det, "%zu engaged (expected 2 at y = -1), wrist ring %d (4), inner %d (2)", eng.size(),
                  base.stats.wrist, base.stats.inner);
    check("fist: the inner corners rest on the handle", ring_ok && base.stats.wrist == 4 && base.stats.inner == 2, det);

    Options cap_o = base_o;
    cap_o.cap_cm = 0.05;
    const FistResult capped = close_fist(hand, cw, 0.09, cap_o);
    bool cap_ok = capped.stats.capped == 2;
    for (int i : eng)
        cap_ok = cap_ok && approx(std::sqrt(capped.disp[3 * i] * capped.disp[3 * i] + capped.disp[3 * i + 1] * capped.disp[3 * i + 1] +
                                          capped.disp[3 * i + 2] * capped.disp[3 * i + 2]), 0.05 * 0.09, 1e-9);
    bool wrist_still = true;
    for (int i = 0; i < hand.count; i++)
        if (approx(hand.pos[3 * i + 1], 3, 1e-9))
            wrist_still = wrist_still && base.disp[3 * i] == 0 && base.disp[3 * i + 1] == 0 && base.disp[3 * i + 2] == 0;
    std::snprintf(det, sizeof det, "%d capped (expected 2), wrist ring %s", capped.stats.capped, wrist_still ? "still" : "MOVED");
    check("fist: the cap and the wrist ring", cap_ok && wrist_still && base.stats.max_move_cm > 0, det);

    // the fade toward the wrist: the engaged corners sit 3.4 units below the ring's edge
    Options fade_o = base_o;
    fade_o.fade_cm = 100;
    const FistResult faded = close_fist(hand, cw, 0.09, fade_o);
    const double x4 = 3.4 / 9, sm = x4 * x4 * (3 - 2 * x4);
    auto mag = [](const FistResult& f, int i) { return std::sqrt(f.disp[3 * i] * f.disp[3 * i] + f.disp[3 * i + 1] * f.disp[3 * i + 1] + f.disp[3 * i + 2] * f.disp[3 * i + 2]); };
    const double full = eng.empty() ? 0 : mag(base, eng[0]), part = eng.empty() ? 0 : mag(faded, eng[0]);
    std::snprintf(det, sizeof det, "%.4f of the full move (expected %.4f)", full > 0 ? part / full : -1.0, sm);
    check("fist: the fade toward the wrist", full > 0 && approx(part / full, sm, 1e-9), det);

    Options none_o;
    none_o.reach_cm = 0.01; none_o.inner_cos = 0.05;
    const FistResult none = close_fist(hand, cw, 0.09, none_o);
    std::snprintf(det, sizeof det, "%d engaged, largest move %.4f cm", none.stats.engaged, none.stats.max_move_cm);
    check("fist: nothing in reach, nothing moves", none.stats.engaged == 0 && none.stats.max_move_cm == 0, det);

    // the same handle given as an axis override equals the seat's own axis
    Axis same;
    const Weapon cw2 = weapon_in_hand({cyl}, ident, &same);
    const FistResult over = close_fist(hand, cw2, 0.09, base_o);
    bool eq = over.disp == base.disp;
    check("fist: an axis override equal to the seat's axis changes nothing", eq, eq ? "identical moves" : "moves differ");
    return all;
}

}  // namespace handclose
