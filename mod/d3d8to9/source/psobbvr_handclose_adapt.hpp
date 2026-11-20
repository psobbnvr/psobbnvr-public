#pragma once

// The glue between the game's captured data and the fist-closing maths, with
// no game or Windows dependencies:
// a welded hand mesh and a weapon snapshot in, one move and one normal per
// welded vertex of the hand mesh out.
//
// The core welds in first-appearance order and the hand mesh welds in draw
// order, so the hand goes through the core's
// own weld as a single part, and the welding map carries each result back to
// the mesh's vertex numbers (a mesh vertex no triangle uses does not move).

#include <vector>

#include "psobbvr_handclose_core.hpp"
#include "psobbvr_handweld.hpp"
#include "psobbvr_weaponsnap_core.hpp"

namespace handclose {
namespace adapt {

// The hand mesh as one part: its welded vertices and triangles.
inline Part HandPart(const handweld::Mesh& m) {
    Part p;
    p.positions.resize((size_t)m.vertex_count * 3);
    for (int i = 0; i < m.vertex_count; i++)
        for (int c = 0; c < 3; c++)
            p.positions[(size_t)i * 3 + c] = m.pos[i][c];
    p.tris.reserve((size_t)m.tri_count * 3);
    for (int t = 0; t < m.tri_count; t++)
        for (int c = 0; c < 3; c++)
            p.tris.push_back(m.tri[t][c]);
    return p;
}

// One Part per recorded draw of a weapon snapshot (positions from the
// draw's vertex range, triangles from its index range, local to the draw).
inline std::vector<Part> WeaponParts(const weaponsnap::HeldGeometry& g) {
    std::vector<Part> parts;
    for (const weaponsnap::HeldDraw& d : g.draws) {
        if (!d.vertex_count)
            continue;
        if ((size_t)(d.first_vertex + d.vertex_count) * 3 > g.positions.size() ||
            (size_t)(d.first_index + d.index_count) > g.indices.size())
            continue;
        Part p;
        p.positions.resize((size_t)d.vertex_count * 3);
        for (size_t i = 0; i < p.positions.size(); i++)
            p.positions[i] = g.positions[(size_t)d.first_vertex * 3 + i];
        p.tris.resize(d.index_count);
        for (uint32_t i = 0; i < d.index_count; i++)
            p.tris[i] = g.indices[d.first_index + i];
        parts.push_back(std::move(p));
    }
    return parts;
}

// What one closing produces, per vertex of the hand mesh.
struct Closed {
    FistResult fist;                  // in the core's welding order
    std::vector<float> moves;         // 3 per mesh vertex, grip frame
    std::vector<float> normals;       // the reshaped hand's, 3 per mesh vertex
    std::vector<float> normals_rest;  // the unmoved hand's, 3 per mesh vertex
};

// Close the hand mesh around weapon parts placed by T (see weapon_in_hand);
// axis may be null.
inline Closed Close(const handweld::Mesh& mesh, const std::vector<Part>& weapon_parts, const double T[16],
                    const Axis* axis, double cm_per_unit, const Options& options) {
    Closed out;
    const Welded hand = weld({HandPart(mesh)});
    const Weapon weapon = weapon_in_hand(weapon_parts, T, axis);
    out.fist = close_fist(hand, weapon, cm_per_unit, options);
    const std::vector<double> rest = mesh_normals(hand.pos, hand.tris, hand.count);
    const size_t n = (size_t)mesh.vertex_count;
    out.moves.assign(n * 3, 0.0f);
    out.normals.assign(n * 3, 0.0f);
    out.normals_rest.assign(n * 3, 0.0f);
    const std::vector<int>& map = hand.map[0];
    for (size_t i = 0; i < n && i < map.size(); i++) {
        const int w = map[i];
        if (w < 0)
            continue;
        for (int c = 0; c < 3; c++) {
            out.moves[i * 3 + c] = (float)out.fist.disp[(size_t)w * 3 + c];
            out.normals[i * 3 + c] = (float)out.fist.normals[(size_t)w * 3 + c];
            out.normals_rest[i * 3 + c] = (float)rest[(size_t)w * 3 + c];
        }
    }
    return out;
}

}  // namespace adapt
}  // namespace handclose
