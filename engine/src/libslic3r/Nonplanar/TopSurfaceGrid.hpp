#pragma once
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <nlohmann/json.hpp>
#include <tbb/parallel_for.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Slic3r::Nonplanar {

class TopSurfaceGrid
{
public:
    using Json = nlohmann::json;
    static constexpr double pitch = 0.25;
    Vec2d origin;
    int nx, ny;
    std::vector<double> mesh, normal, bead;
    std::vector<unsigned char> conformal, ironed, eligible;
    std::vector<uint16_t> owner;
    explicit TopSurfaceGrid(const indexed_triangle_set &its, const std::vector<uint16_t> &face_owner = {})
    {
        BoundingBoxf box;
        for (const auto &v : its.vertices) box.merge(Vec2d(v.head<2>().cast<double>()));
        origin = box.min;
        nx = std::max(1, int(std::ceil(box.size().x() / pitch)));
        ny = std::max(1, int(std::ceil(box.size().y() / pitch)));
        const size_t cells = size_t(nx) * ny;
        mesh.assign(cells, -std::numeric_limits<double>::infinity());
        normal.assign(cells, 0.);
        bead = mesh;
        conformal.assign(cells, 0); ironed.assign(cells, 0); eligible.assign(cells, 0);
        if (!face_owner.empty()) owner.assign(cells, 65535);
        for (size_t face = 0; face < its.indices.size(); ++face) {
            const auto &f = its.indices[face];
            const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            const double d = n.z();
            if (d <= 1e-12) continue;
            visit(a.head<2>().cwiseMin(b.head<2>()).cwiseMin(c.head<2>()),
                  a.head<2>().cwiseMax(b.head<2>()).cwiseMax(c.head<2>()), [&](size_t k, const Vec2d &p) {
                const double u = ((b.x() - p.x()) * (c.y() - p.y()) - (b.y() - p.y()) * (c.x() - p.x())) / d;
                const double v = ((c.x() - p.x()) * (a.y() - p.y()) - (c.y() - p.y()) * (a.x() - p.x())) / d;
                if (u < -1e-9 || v < -1e-9 || u + v > 1. + 1e-9) return;
                const double z = u * a.z() + v * b.z() + (1. - u - v) * c.z();
                if (z > mesh[k]) {
                    mesh[k] = z; normal[k] = d / n.norm();
                    if (!owner.empty()) owner[k] = face_owner[face];
                }
            });
        }
        const int inset = 8;
        for (int y = inset; y < ny - inset; ++y) for (int x = inset; x < nx - inset; ++x) {
            bool ok = true;
            for (int dy = -inset; ok && dy <= inset; ++dy) for (int dx = -inset; dx <= inset; ++dx) {
                if (dx * dx + dy * dy > inset * inset) continue;
                const size_t k = size_t(y + dy) * nx + x + dx;
                if (!std::isfinite(mesh[k]) || normal[k] < std::cos(70. * 3.14159265358979323846 / 180.)) { ok = false; break; }
            }
            eligible[size_t(y) * nx + x] = ok;
        }
    }
    template<class F> void visit(const Vec2d &lo, const Vec2d &hi, F fn) { visit(lo, hi, 0, ny, fn); }
    template<class F> void visit(const Vec2d &lo, const Vec2d &hi, int row0, int row1, F fn)
    {
        const int x0 = std::max(0, int(std::floor((lo.x() - origin.x()) / pitch))),
                  x1 = std::min(nx - 1, int(std::floor((hi.x() - origin.x()) / pitch))),
                  y0 = std::max(row0, int(std::floor((lo.y() - origin.y()) / pitch))),
                  y1 = std::min(row1 - 1, int(std::floor((hi.y() - origin.y()) / pitch)));
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x)
            fn(size_t(y) * nx + x, origin + pitch * Vec2d(x + 0.5, y + 0.5));
    }
    void deposit(const GCodeProcessorResult &result)
    {
        const int bands = std::min(ny, 64);
        if (bands <= 0)
            return;
        std::vector<int> band_of(static_cast<size_t>(ny));
        for (int band = 0; band < bands; ++band)
            for (int y = band * ny / bands; y < (band + 1) * ny / bands; ++y)
                band_of[size_t(y)] = band;
        std::vector<std::vector<uint32_t>> moves(static_cast<size_t>(bands));
        for (size_t i = 1; i < result.moves.size(); ++i) {
            const auto &m = result.moves[i];
            if (!deposits(m))
                continue;
            const Vec2d a = result.moves[i - 1].position.cast<double>().head<2>(), b = m.position.cast<double>().head<2>();
            const double radius = m.width / 2.;
            const Vec2d lo = a.cwiseMin(b) - Vec2d::Constant(radius), hi = a.cwiseMax(b) + Vec2d::Constant(radius);
            const int x0 = std::max(0, int(std::floor((lo.x() - origin.x()) / pitch))),
                      x1 = std::min(nx - 1, int(std::floor((hi.x() - origin.x()) / pitch))),
                      y0 = std::max(0, int(std::floor((lo.y() - origin.y()) / pitch))),
                      y1 = std::min(ny - 1, int(std::floor((hi.y() - origin.y()) / pitch)));
            if (x0 > x1 || y0 > y1)
                continue;
            for (int band = band_of[size_t(y0)]; band <= band_of[size_t(y1)]; ++band)
                moves[size_t(band)].push_back(uint32_t(i));
        }
        tbb::parallel_for(0, bands, [&](int band) {
            const int row0 = band * ny / bands, row1 = (band + 1) * ny / bands;
            for (uint32_t i : moves[size_t(band)])
                deposit_move(result, i, row0, row1);
        });
    }
    void deposit(const GCodeProcessorResult &result, int row0, int row1)
    {
        for (size_t i = 1; i < result.moves.size(); ++i)
            if (deposits(result.moves[i]))
                deposit_move(result, i, row0, row1);
    }
    static bool deposits(const GCodeProcessorResult::MoveVertex &m)
    {
        return !(m.type != EMoveType::Extrude || m.extrusion_role == erCustom || m.extrusion_role == erSkirt || m.extrusion_role == erBrim ||
                 m.extrusion_role == erNone || m.extrusion_role == erWipeTower || is_support(m.extrusion_role));
    }
    void deposit_move(const GCodeProcessorResult &result, size_t i, int row0, int row1)
    {
        const auto &m = result.moves[i];
        const Vec3d a = result.moves[i - 1].position.cast<double>(), b = m.position.cast<double>();
        const Vec2d aa = a.head<2>(), bb = b.head<2>(), d = bb - aa;
        const double radius = m.width / 2.;
        visit(aa.cwiseMin(bb) - Vec2d::Constant(radius), aa.cwiseMax(bb) + Vec2d::Constant(radius), row0, row1,
              [&](size_t k, const Vec2d &p) {
            const double t = d.squaredNorm() > 1e-12 ? std::clamp((p - aa).dot(d) / d.squaredNorm(), 0., 1.) : 0.;
            if ((p - aa - t * d).squaredNorm() > radius * radius) return;
            const double z = a.z() + t * (b.z() - a.z());
            if (m.extrusion_role == erIroning) { if (std::abs(z - mesh[k]) <= 1.) ironed[k] = 1; return; }
            if (z > bead[k]) { bead[k] = z; conformal[k] = result.state(m).nonplanar_patch_shell != 0xFF; }
        });
    }
    template<class F> Json measure(F accepts) const
    {
        double squared = 0., squared_conformal = 0.;
        size_t cells = 0, finite = 0, gaps = 0, covered = 0, iron = 0;
        for (int y = 0; y < ny; ++y) for (int x = 0; x < nx; ++x) {
            const size_t k = size_t(y) * nx + x;
            if (!eligible[k] || !accepts(origin + pitch * Vec2d(x + 0.5, y + 0.5))) continue;
            ++cells;
            const double error = bead[k] - mesh[k];
            if (std::isfinite(error)) { squared += error * error; ++finite; }
            if (error < -1.) ++gaps;
            if (conformal[k] && std::abs(error) <= 1.) { ++covered; squared_conformal += error * error; }
            iron += ironed[k];
        }
        return {{"rms_vs_mesh_mm", finite ? Json(std::sqrt(squared / finite)) : Json(nullptr)},
                {"coverage", cells ? Json(double(covered) / cells) : Json(nullptr)},
                {"rms_covered_mm", covered ? Json(std::sqrt(squared_conformal / covered)) : Json(nullptr)},
                {"rms_uncovered_mm", finite > covered ? Json(std::sqrt(std::max(0., squared - squared_conformal) / (finite - covered))) : Json(nullptr)},
                {"gap_cells", gaps}, {"ironed_area_mm2", iron * pitch * pitch}, {"eval_cells", cells},
                {"finite_cells", finite}};
    }
};

}
