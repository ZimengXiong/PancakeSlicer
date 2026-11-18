#include "OrientationField.hpp"
#include "Polyline.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Nonplanar/HeadEnvelope.hpp"
#include "libslic3r/Nonplanar/LayerFieldOpt.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <Eigen/SparseCore>
#include <cstdint>
#include <igl/Hit.h>
#include <math.h>
#include <memory>
#include <string>
#include <vector>

#include "ConformalShells.hpp"
#include "FieldOrder.hpp"
#include "Multiplanar.hpp"
#include "NpField.hpp"
#include "OneShotField.hpp"
#include "PatchTools.hpp"

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include <boost/log/trivial.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <new>
#include <chrono>
#include <cstdlib>
#include <array>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace Slic3r::Nonplanar {

namespace {
thread_local bool nested_field_slice_flag = false;
}

bool nested_field_slice() { return nested_field_slice_flag; }
NestedFieldSlice::NestedFieldSlice() : m_previous(nested_field_slice_flag) { nested_field_slice_flag = true; }
NestedFieldSlice::~NestedFieldSlice() { nested_field_slice_flag = m_previous; }

namespace {

constexpr double UprightSnap = 20. * M_PI / 180.;
constexpr double UprightFull = 28. * M_PI / 180.;

Vec3d snap_upright(const Vec3d &a)
{
    const double tilt = std::acos(std::clamp(a.z(), -1., 1.));
    if (tilt < UprightSnap)
        return Vec3d::UnitZ();
    if (tilt >= UprightFull)
        return a;
    const double s = (tilt - UprightSnap) / (UprightFull - UprightSnap);
    const Vec2d  u = a.head<2>().normalized();
    const double t = s * tilt;
    return Vec3d(std::sin(t) * u.x(), std::sin(t) * u.y(), std::cos(t));
}

Vec3d clamp_tilt(const Vec3d &d, double max_tilt)
{
    if (max_tilt <= 0.)
        return Vec3d::UnitZ();
    const double tilt = std::acos(std::clamp(d.z(), -1., 1.));
    if (tilt <= max_tilt)
        return d;
    const Vec2d h = d.head<2>();
    if (h.squaredNorm() < 1e-24)
        return Vec3d::UnitZ();
    const Vec2d u = h.normalized();
    return Vec3d(std::sin(max_tilt) * u.x(), std::sin(max_tilt) * u.y(), std::cos(max_tilt));
}

Vec3d lift_overhang(const Vec3d &g, const Vec3d &n, double sin_threshold, const Vec3d &outward)
{
    if (n.dot(g) >= -sin_threshold)
        return g;
    Vec3d tangential = g - n.dot(g) * n;
    if (tangential.squaredNorm() < 1e-12)
        tangential = outward - n.dot(outward) * n;
    if (tangential.squaredNorm() < 1e-12)
        tangential = Vec3d::UnitX() - n.x() * n;
    return (-sin_threshold * n + std::sqrt(1. - sin_threshold * sin_threshold) * tangential.normalized()).normalized();
}

struct FacetSample
{
    Vec3d  p;
    Vec3d  n;
    double area;
    size_t facet;
};

}

void OrientationField::locate(const Vec3d &p, int &i, int &j, int &k, Vec3d &t) const
{
    const Vec3d g = (p - m_origin) / m_pitch;
    auto        axis = [](double x, int n, int &c, double &f) {
        c = std::clamp(int(std::floor(x)), 0, n - 2);
        f = std::clamp(x - double(c), 0., 1.);
    };
    axis(g.x(), m_nx, i, t.x());
    axis(g.y(), m_ny, j, t.y());
    axis(g.z(), m_nz, k, t.z());
}

template<class T> T OrientationField::trilinear(const std::vector<T> &f, const Vec3d &p) const
{
    int   i, j, k;
    Vec3d t;
    this->locate(p, i, j, k, t);
    const size_t n000 = node(i, j, k), dx = 1, dy = size_t(m_nx), dz = size_t(m_nx) * size_t(m_ny);
    const double x = t.x(), y = t.y(), z = t.z();
    auto lerp = [](const T &a, const T &b, double s) -> T { return a + (b - a) * s; };
    const T c00 = lerp(f[n000], f[n000 + dx], x), c10 = lerp(f[n000 + dy], f[n000 + dy + dx], x);
    const T c01 = lerp(f[n000 + dz], f[n000 + dz + dx], x), c11 = lerp(f[n000 + dz + dy], f[n000 + dz + dy + dx], x);
    return lerp(lerp(c00, c10, y), lerp(c01, c11, y), z);
}

double OrientationField::phi(const Vec3d &p) const { return this->trilinear(m_phi, p); }

Vec3d OrientationField::gradient(const Vec3d &p) const { return this->trilinear(m_grad_phi, p).cast<double>(); }

Vec3d OrientationField::orientation(const Vec3d &p) const
{
    const Vec3d g = this->gradient(p);
    if (g.squaredNorm() < 1e-24)
        return Vec3d::UnitZ();
    return snap_upright(clamp_tilt(g.normalized(), m_tilt_budget));
}

Vec3d OrientationField::map_axis(const Vec3d &p, const Vec3d &deformed_axis) const
{
    const Vec3d a = this->jacobian(p).transpose() * deformed_axis;
    return a.squaredNorm() < 1e-24 ? this->orientation(p) : clamp_tilt(a.normalized(), m_tilt_budget);
}

std::vector<double> OrientationField::slice_knots() const
{
    std::vector<double> z, f;
    for (int k = 0; k < m_nz; ++k) {
        double sum = 0.;
        size_t n   = 0;
        for (int j = 0; j < m_ny; ++j)
            for (int i = 0; i < m_nx; ++i)
                if (const size_t id = node(i, j, k); m_inside[id])
                    sum += m_phi[id], ++n;
        if (n > 0 && (f.empty() || sum / double(n) > f.back()))
            z.push_back(node_pos(0, 0, k).z()), f.push_back(sum / double(n));
    }
    auto interp = [](const std::vector<double> &x, const std::vector<double> &y, double v) {
        if (x.size() < 2)
            return v;
        const size_t i = std::clamp<size_t>(size_t(std::upper_bound(x.begin(), x.end(), v) - x.begin()), 1, x.size() - 1);
        return y[i - 1] + (y[i] - y[i - 1]) * (v - x[i - 1]) / std::max(x[i] - x[i - 1], 1e-12);
    };
    for (const double anchor : { m_bed_band, 0. })
        if (z.empty() || anchor < z.front() - 1e-9)
            z.insert(z.begin(), anchor), f.insert(f.begin(), anchor);
    std::vector<double> knots;
    if (f.size() < 2)
        return knots;
    for (int j = 1; m_first_layer + (j - 1) * m_layer_height < f.back(); ++j) {
        const double lo = interp(f, z, m_first_layer + (j - 1) * m_layer_height), hi = interp(f, z, m_first_layer + j * m_layer_height);
        knots.push_back(interp(z, f, 0.5 * (lo + hi)));
    }
    return knots;
}

Vec3d OrientationField::forward(const Vec3d &p) const
{
    return Vec3d(this->trilinear(m_u, p), this->trilinear(m_v, p), this->trilinear(m_phi, p));
}

Matrix3d OrientationField::jacobian(const Vec3d &p) const
{
    Matrix3d j;
    j.row(0) = this->trilinear(m_grad_u, p).cast<double>().transpose();
    j.row(1) = this->trilinear(m_grad_v, p).cast<double>().transpose();
    j.row(2) = this->trilinear(m_grad_phi, p).cast<double>().transpose();
    return j;
}

bool OrientationField::inside(const Vec3d &p) const
{
    int   i, j, k;
    Vec3d t;
    this->locate(p, i, j, k, t);
    double v = 0.;
    for (int c = 0; c < 8; ++c) {
        const int    a = c & 1, b = (c >> 1) & 1, d = (c >> 2) & 1;
        const double w = (a ? t.x() : 1. - t.x()) * (b ? t.y() : 1. - t.y()) * (d ? t.z() : 1. - t.z());
        v += w * double(m_inside[node(i + a, j + b, k + d)] != 0);
    }
    return v >= 0.5;
}

void OrientationField::compute_gradients()
{
    const size_t n = m_phi.size();
    m_grad_phi.assign(n, Vec3f::Zero());
    m_grad_u.assign(n, Vec3f::Zero());
    m_grad_v.assign(n, Vec3f::Zero());
    tbb::parallel_for(tbb::blocked_range<int>(0, m_nz), [&](const tbb::blocked_range<int> &range) {
        for (int k = range.begin(); k < range.end(); ++k)
            for (int j = 0; j < m_ny; ++j)
                for (int i = 0; i < m_nx; ++i) {
                    const size_t id = node(i, j, k);
                    auto diff = [&](const std::vector<double> &f) {
                        Vec3f g;
                        const int lo[3] = { std::max(i - 1, 0), std::max(j - 1, 0), std::max(k - 1, 0) };
                        const int hi[3] = { std::min(i + 1, m_nx - 1), std::min(j + 1, m_ny - 1), std::min(k + 1, m_nz - 1) };
                        g.x() = float((f[node(hi[0], j, k)] - f[node(lo[0], j, k)]) / (double(hi[0] - lo[0]) * m_pitch));
                        g.y() = float((f[node(i, hi[1], k)] - f[node(i, lo[1], k)]) / (double(hi[1] - lo[1]) * m_pitch));
                        g.z() = float((f[node(i, j, hi[2])] - f[node(i, j, lo[2])]) / (double(hi[2] - lo[2]) * m_pitch));
                        return g;
                    };
                    m_grad_phi[id] = diff(m_phi);
                    m_grad_u[id]   = diff(m_u);
                    m_grad_v[id]   = diff(m_v);
                }
    });
}

void OrientationField::build_inverse()
{
    std::vector<uint8_t> near(m_inside.size(), 0);
    for (int k = 0; k < m_nz; ++k)
        for (int j = 0; j < m_ny; ++j)
            for (int i = 0; i < m_nx; ++i)
                if (m_inside[node(i, j, k)])
                    for (int c = -2; c <= 2; ++c)
                        for (int b = -2; b <= 2; ++b)
                            for (int a = -2; a <= 2; ++a) {
                                const int x = i + a, y = j + b, z = k + c;
                                if (x >= 0 && y >= 0 && z >= 0 && x < m_nx && y < m_ny && z < m_nz)
                                    near[node(x, y, z)] = 1;
                            }
    Vec3d lo = Vec3d::Constant(std::numeric_limits<double>::max()), hi = -lo;
    std::vector<uint32_t> cells;
    for (int k = 0; k + 1 < m_nz; ++k)
        for (int j = 0; j + 1 < m_ny; ++j)
            for (int i = 0; i + 1 < m_nx; ++i) {
                bool any = false;
                for (int c = 0; c < 8 && !any; ++c)
                    any = near[node(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1))] != 0;
                if (!any)
                    continue;
                cells.push_back(uint32_t(node(i, j, k)));
                for (int c = 0; c < 8; ++c) {
                    const size_t id = node(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                    const Vec3d  q(m_u[id], m_v[id], m_phi[id]);
                    lo = lo.cwiseMin(q);
                    hi = hi.cwiseMax(q);
                }
            }
    m_inv_bins.clear();
    if (cells.empty())
        return;
    m_inv_pitch  = m_pitch;
    m_inv_origin = lo - Vec3d::Constant(m_inv_pitch);
    m_inv_nx     = int(std::ceil((hi.x() - m_inv_origin.x()) / m_inv_pitch)) + 2;
    m_inv_ny     = int(std::ceil((hi.y() - m_inv_origin.y()) / m_inv_pitch)) + 2;
    m_inv_nz     = int(std::ceil((hi.z() - m_inv_origin.z()) / m_inv_pitch)) + 2;
    m_inv_bins.assign(size_t(m_inv_nx) * size_t(m_inv_ny) * size_t(m_inv_nz), {});
    const size_t sx = size_t(m_nx), sxy = size_t(m_nx) * size_t(m_ny);
    for (uint32_t cell : cells) {
        Vec3d clo = Vec3d::Constant(std::numeric_limits<double>::max()), chi = -clo;
        for (int c = 0; c < 8; ++c) {
            const size_t id = cell + size_t(c & 1) + size_t((c >> 1) & 1) * sx + size_t((c >> 2) & 1) * sxy;
            const Vec3d  q(m_u[id], m_v[id], m_phi[id]);
            clo = clo.cwiseMin(q);
            chi = chi.cwiseMax(q);
        }
        const Vec3i32 a = ((clo - m_inv_origin) / m_inv_pitch).array().floor().cast<int32_t>();
        const Vec3i32 b = ((chi - m_inv_origin) / m_inv_pitch).array().floor().cast<int32_t>();
        for (int z = std::max(a.z(), 0); z <= std::min(b.z(), m_inv_nz - 1); ++z)
            for (int y = std::max(a.y(), 0); y <= std::min(b.y(), m_inv_ny - 1); ++y)
                for (int x = std::max(a.x(), 0); x <= std::min(b.x(), m_inv_nx - 1); ++x)
                    m_inv_bins[(size_t(z) * size_t(m_inv_ny) + size_t(y)) * size_t(m_inv_nx) + size_t(x)].push_back(cell);
    }
}

bool OrientationField::inverse(const Vec3d &q, Vec3d &p) const
{
    if (m_inv_bins.empty())
        return false;
    const Vec3i32 b = ((q - m_inv_origin) / m_inv_pitch).array().floor().cast<int32_t>();
    if (b.x() < 0 || b.y() < 0 || b.z() < 0 || b.x() >= m_inv_nx || b.y() >= m_inv_ny || b.z() >= m_inv_nz)
        return false;
    const std::vector<uint32_t> &bin = m_inv_bins[(size_t(b.z()) * size_t(m_inv_ny) + size_t(b.y())) * size_t(m_inv_nx) + size_t(b.x())];
    const size_t sx = size_t(m_nx), sxy = size_t(m_nx) * size_t(m_ny);
    bool         found = false;
    for (uint32_t cell : bin) {
        std::array<Vec3d, 8> c;
        for (int n = 0; n < 8; ++n) {
            const size_t id = cell + size_t(n & 1) + size_t((n >> 1) & 1) * sx + size_t((n >> 2) & 1) * sxy;
            c[n] = Vec3d(m_u[id], m_v[id], m_phi[id]);
        }
        Vec3d t(0.5, 0.5, 0.5);
        Vec3d f;
        for (int it = 0; it < 16; ++it) {
            const double x = t.x(), y = t.y(), z = t.z();
            f = Vec3d::Zero();
            Matrix3d j = Matrix3d::Zero();
            for (int n = 0; n < 8; ++n) {
                const double a = (n & 1) ? x : 1. - x, sa = (n & 1) ? 1. : -1.;
                const double bb = ((n >> 1) & 1) ? y : 1. - y, sb = ((n >> 1) & 1) ? 1. : -1.;
                const double cc = ((n >> 2) & 1) ? z : 1. - z, sc = ((n >> 2) & 1) ? 1. : -1.;
                f += a * bb * cc * c[n];
                j.col(0) += sa * bb * cc * c[n];
                j.col(1) += a * sb * cc * c[n];
                j.col(2) += a * bb * sc * c[n];
            }
            const double det = j.determinant();
            if (std::abs(det) < 1e-18)
                break;
            const Vec3d dt = j.inverse() * (q - f);
            t = (t + dt).cwiseMax(Vec3d::Constant(-0.5)).cwiseMin(Vec3d::Constant(1.5));
            if (dt.squaredNorm() < 1e-20)
                break;
        }
        constexpr double tol = 1e-4;
        if ((t.array() < -tol).any() || (t.array() > 1. + tol).any())
            continue;
        const size_t k  = cell / sxy, j = (cell % sxy) / sx, i = cell % sx;
        const Vec3d  pc = node_pos(int(i), int(j), int(k)) + m_pitch * t.cwiseMax(Vec3d::Zero()).cwiseMin(Vec3d::Ones());
        if ((this->forward(pc) - q).norm() > 1e-3)
            continue;
        p = pc;
        if (this->inside(pc))
            return true;
        found = true;
    }
    return found;
}

indexed_triangle_set OrientationField::deform(const indexed_triangle_set &mesh, double max_edge) const
{
    indexed_triangle_set its = mesh;
    const float          limit2 = float(max_edge * max_edge);
    for (int pass = 0; pass < 12; ++pass) {
        std::unordered_map<uint64_t, int> mid;
        auto key = [](int a, int b) { return (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b)); };
        auto midpoint = [&](int a, int b) {
            const auto it = mid.find(key(a, b));
            if (it != mid.end())
                return it->second;
            const int id = int(its.vertices.size());
            its.vertices.push_back(0.5f * (its.vertices[a] + its.vertices[b]));
            mid.emplace(key(a, b), id);
            return id;
        };
        std::vector<stl_triangle_vertex_indices> out;
        out.reserve(its.indices.size() * 2);
        bool split = false;
        for (const stl_triangle_vertex_indices &f : its.indices) {
            std::array<int, 3> m { -1, -1, -1 };
            int                count = 0;
            for (int e = 0; e < 3; ++e)
                if ((its.vertices[f[e]] - its.vertices[f[(e + 1) % 3]]).squaredNorm() > limit2) {
                    m[e] = midpoint(f[e], f[(e + 1) % 3]);
                    ++count;
                }
            if (count == 0) {
                out.push_back(f);
                continue;
            }
            split = true;
            if (count == 3) {
                out.emplace_back(f[0], m[0], m[2]);
                out.emplace_back(m[0], f[1], m[1]);
                out.emplace_back(m[2], m[1], f[2]);
                out.emplace_back(m[0], m[1], m[2]);
            } else if (count == 1) {
                const int e = m[0] >= 0 ? 0 : m[1] >= 0 ? 1 : 2;
                out.emplace_back(f[e], m[e], f[(e + 2) % 3]);
                out.emplace_back(m[e], f[(e + 1) % 3], f[(e + 2) % 3]);
            } else {
                const int k  = m[0] < 0 ? 0 : m[1] < 0 ? 1 : 2;
                const int a  = f[k], b = f[(k + 1) % 3], c = f[(k + 2) % 3];
                const int mb = m[(k + 1) % 3];
                const int mc = m[(k + 2) % 3];
                out.emplace_back(mb, c, mc);
                out.emplace_back(a, b, mb);
                out.emplace_back(a, mb, mc);
            }
        }
        its.indices = std::move(out);
        if (!split)
            break;
    }
    tbb::parallel_for(tbb::blocked_range<size_t>(0, its.vertices.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i)
            its.vertices[i] = this->forward(its.vertices[i].cast<double>()).cast<float>();
    });
    return its;
}

NpField OrientationField::to_npfield() const
{
    NpField f;
    f.origin      = m_origin;
    f.spacing     = Vec3d::Constant(m_pitch);
    f.dims        = { size_t(m_nx), size_t(m_ny), size_t(m_nz) };
    f.h           = m_layer_height;
    f.first_layer = m_first_layer;
    f.mesh_min    = m_mesh_min;
    f.solver      = m_given ? "given" : "OrientationField support-free";
    f.phi.resize(m_phi.size());
    for (int i = 0; i < m_nx; ++i)
        for (int j = 0; j < m_ny; ++j)
            for (int k = 0; k < m_nz; ++k)
                f.phi[f.index(size_t(i), size_t(j), size_t(k))] = float(m_phi[this->node(i, j, k)]);
    make_z_monotone(f);
    const OrientationFieldStats &st = m_stats;
    f.terms = { { "stat_overhang_area_mm2", st.overhang_area },     { "stat_planar_overhang_area_mm2", st.planar_overhang_area },
                { "stat_support_volume_mm3", st.support_volume },   { "stat_planar_support_volume_mm3", st.planar_support_volume },
                { "stat_step_area_mm2", st.step_area },             { "stat_planar_step_area_mm2", st.planar_step_area },
                { "stat_max_tilt_deg", st.max_tilt * 180. / M_PI }, { "stat_min_thickness", st.min_thickness },
                { "stat_max_thickness", st.max_thickness },         { "stat_identity_below_mm", m_bed_band } };
    return f;
}

struct OrientationFieldBuilder
{
    OrientationField                &F;
    const indexed_triangle_set      &mesh;
    const OrientationFieldParams    &params;
    const std::function<void()>     &cancel;

    std::vector<FacetSample> samples;
    std::vector<Vec3d>       facet_normal;
    std::vector<double>      facet_area;
    std::vector<uint8_t>     facet_bed;
    std::vector<Vec3d>       facet_lift;
    std::vector<uint8_t>     fixed;
    std::vector<double>      tilt_cap;
    std::vector<double>      scale;
    std::vector<Vec2d>       block_dir;
    std::vector<double>      block_scale;
    std::vector<double>      table_drop;
    std::vector<Vec3d>       d;
    std::vector<Vec3d>       corr_target;
    std::vector<double>      corr_weight;
    Vec3d                    center { Vec3d::Zero() };
    double                   sin_threshold = 0.;
    AABBTreeIndirect::Tree3f tree;

    OrientationFieldBuilder(OrientationField &f, const indexed_triangle_set &m, const OrientationFieldParams &p,
                            const std::function<void()> &c)
        : F(f), mesh(m), params(p), cancel(c)
    {}

    void check_cancel() const
    {
        if (cancel)
            cancel();
    }

    size_t nodes() const { return F.m_phi.size(); }

    void setup_grid()
    {
        BoundingBoxf3 box;
        for (const stl_vertex &v : mesh.vertices)
            box.merge(v.cast<double>());
        const Vec3d  size   = box.size();
        const double volume = std::max(1., (size.x() + 6.) * (size.y() + 6.) * (size.z() + 3.));
        double       pitch  = params.pitch > 0. ? params.pitch : std::clamp(std::cbrt(volume / 160000.), 0.6, 2.5);
        if (params.given) {
            static constexpr double MaxGivenNodes = 8e6;
            auto grid_nodes = [&box](double p) {
                const double m = 3. * p;
                return (std::ceil((box.size().x() + 2. * m) / p) + 2.) * (std::ceil((box.size().y() + 2. * m) / p) + 2.) *
                       (std::ceil((box.max.z() + m) / p) + 2.);
            };
            const Vec3d sp      = params.given->file().spacing;
            std::array<double, 3> candidates { sp.x(), sp.y(), sp.z() };
            std::sort(candidates.begin(), candidates.end());
            const auto fit = std::find_if(candidates.begin(), candidates.end(), [&](double p) { return grid_nodes(p) <= MaxGivenNodes; });
            if (fit == candidates.end())
                throw Slic3r::SlicingError(Slic3r::format(_u8L("The layer field file's grid (%1% x %2% x %3% mm) is too fine for this part: "
                                                               "%4$.0f million nodes at its coarsest spacing, more than %5$.0f million."),
                                                          sp.x(), sp.y(), sp.z(), grid_nodes(candidates.back()) / 1e6, MaxGivenNodes / 1e6));
            pitch = *fit;
            if (fit != candidates.begin())
                BOOST_LOG_TRIVIAL(warning) << "Layer field: the file's spacing " << sp.transpose() << " mm is sampled at " << pitch
                                           << " mm on every axis (" << grid_nodes(candidates.front()) / 1e6 << " M nodes at "
                                           << candidates.front() << " mm)";
        }
        F.m_pitch           = pitch;
        const double margin = 3. * pitch;
        F.m_origin          = Vec3d(box.min.x() - margin, box.min.y() - margin, 0.);
        if (params.given) {
            const Vec3d o = params.given->file().origin;
            for (int a = 0; a < 3; ++a)
                F.m_origin[a] = o[a] - std::ceil((o[a] - F.m_origin[a]) / pitch - 1e-9) * pitch;
        }
        F.m_nx              = int(std::ceil((box.max.x() + margin - F.m_origin.x()) / pitch)) + 1;
        F.m_ny              = int(std::ceil((box.max.y() + margin - F.m_origin.y()) / pitch)) + 1;
        F.m_nz              = int(std::ceil((box.max.z() + margin - F.m_origin.z()) / pitch)) + 1;
        const size_t n      = size_t(F.m_nx) * size_t(F.m_ny) * size_t(F.m_nz);
        F.m_phi.assign(n, 0.);
        F.m_u.assign(n, 0.);
        F.m_v.assign(n, 0.);
        F.m_inside.assign(n, 0);
        fixed.assign(n, 0);
        scale.assign(n, std::clamp(params.tilt_scale, 0., 1.));
        tilt_cap.assign(n, params.max_tilt);
        d.assign(n, Vec3d::UnitZ());
        center = box.center();
        F.m_bed_band = params.first_layer_height;
        F.m_tilt_budget = params.max_tilt;
        F.m_layer_height = params.layer_height;
        F.m_first_layer  = params.first_layer_height;
        F.m_mesh_min     = box.min;
        F.m_given        = params.given != nullptr;
        const double zone = params.first_layer_height + pitch + 1e-6;
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i) {
                    const size_t id = F.node(i, j, k);
                    const Vec3d  p  = F.node_pos(i, j, k);
                    F.m_phi[id] = p.z();
                    F.m_u[id]   = p.x();
                    F.m_v[id]   = p.y();
                    fixed[id]   = p.z() <= zone;
                }
        F.m_stats.tilt_scale = std::clamp(params.tilt_scale, 0., 1.);
        const int r = std::max(1, int(std::ceil(6. / pitch)));
        block_dir.assign(n, Vec2d::Zero());
        block_scale.assign(n, 1.);
        std::vector<uint8_t> mark(n);
        std::vector<Vec2d>   side(n);
        for (size_t round = 0; round < params.relax_rounds.size(); ++round) {
            std::fill(mark.begin(), mark.end(), 0);
            std::fill(side.begin(), side.end(), Vec2d::Zero());
            const std::vector<Vec3d> &points = params.relax_rounds[round];
            for (size_t pi = 0; pi < points.size(); ++pi) {
                const Vec3d u  = round < params.relax_dirs.size() && pi < params.relax_dirs[round].size() ? params.relax_dirs[round][pi] : Vec3d::Zero();
                const Vec3d c  = (points[pi] - F.m_origin) / pitch;
                const int   i0 = int(std::lround(c.x())), j0 = int(std::lround(c.y())), k0 = int(std::lround(c.z()));
                for (int k = std::max(0, k0 - r); k <= std::min(F.m_nz - 1, k0 + r); ++k)
                    for (int j = std::max(0, j0 - r); j <= std::min(F.m_ny - 1, j0 + r); ++j)
                        for (int i = std::max(0, i0 - r); i <= std::min(F.m_nx - 1, i0 + r); ++i)
                            if ((i - i0) * (i - i0) + (j - j0) * (j - j0) + (k - k0) * (k - k0) <= r * r) {
                                const size_t id = F.node(i, j, k);
                                if (u.head<2>().squaredNorm() < 1e-12)
                                    mark[id] = 2;
                                else {
                                    mark[id] = std::max<uint8_t>(mark[id], 1);
                                    side[id] += u.head<2>();
                                }
                            }
            }
            for (size_t i = 0; i < n; ++i)
                if (mark[i] == 2)
                    scale[i] *= 0.5;
                else if (mark[i] == 1 && side[i].squaredNorm() > 1e-12) {
                    const Vec2d u = side[i].normalized();
                    block_dir[i]  = block_dir[i].squaredNorm() > 0. ? Vec2d((block_dir[i] + u).normalized()) : u;
                    block_scale[i] *= 0.5;
                }
        }
    }

    void voxelize()
    {
        std::vector<float> zs(size_t(F.m_nz));
        BoundingBoxf3      box;
        for (const stl_vertex &v : mesh.vertices)
            box.merge(v.cast<double>());
        for (int k = 0; k < F.m_nz; ++k)
            zs[size_t(k)] = float(std::clamp(F.m_origin.z() + k * F.m_pitch, box.min.z() + 1e-3, box.max.z() - 1e-3));
        const std::vector<ExPolygons> layers = slice_mesh_ex(mesh, zs);
        tbb::parallel_for(tbb::blocked_range<int>(0, F.m_nz), [&](const tbb::blocked_range<int> &range) {
            for (int k = range.begin(); k < range.end(); ++k) {
                const double z = F.m_origin.z() + k * F.m_pitch;
                if (z > box.max.z() + 1e-6)
                    continue;
                std::vector<std::pair<Vec2d, Vec2d>> edges;
                for (const ExPolygon &ex : layers[size_t(k)]) {
                    auto add = [&edges](const Polygon &poly) {
                        for (size_t a = 0; a < poly.points.size(); ++a)
                            edges.emplace_back(unscale(poly.points[a]), unscale(poly.points[(a + 1) % poly.points.size()]));
                    };
                    add(ex.contour);
                    for (const Polygon &hole : ex.holes)
                        add(hole);
                }
                std::vector<double> xs;
                for (int j = 0; j < F.m_ny; ++j) {
                    const double y = F.m_origin.y() + j * F.m_pitch;
                    xs.clear();
                    for (const auto &[a, b] : edges)
                        if ((a.y() <= y) != (b.y() <= y))
                            xs.push_back(a.x() + (y - a.y()) / (b.y() - a.y()) * (b.x() - a.x()));
                    std::sort(xs.begin(), xs.end());
                    for (size_t s = 0; s + 1 < xs.size(); s += 2) {
                        const int i0 = int(std::ceil((xs[s] - F.m_origin.x()) / F.m_pitch));
                        const int i1 = int(std::floor((xs[s + 1] - F.m_origin.x()) / F.m_pitch));
                        for (int i = std::max(i0, 0); i <= std::min(i1, F.m_nx - 1); ++i)
                            F.m_inside[F.node(i, j, k)] = 1;
                    }
                }
            }
        });
    }

    void make_samples()
    {
        const size_t nf = mesh.indices.size();
        facet_normal.assign(nf, Vec3d::UnitZ());
        facet_area.assign(nf, 0.);
        facet_bed.assign(nf, 0);
        const double step = 0.75 * F.m_pitch;
        for (size_t f = 0; f < nf; ++f) {
            const Vec3d a = mesh.vertices[mesh.indices[f][0]].cast<double>();
            const Vec3d b = mesh.vertices[mesh.indices[f][1]].cast<double>();
            const Vec3d c = mesh.vertices[mesh.indices[f][2]].cast<double>();
            const Vec3d cr = (b - a).cross(c - a);
            const double area2 = cr.norm();
            if (area2 < 1e-12)
                continue;
            const Vec3d n = cr / area2;
            facet_normal[f] = n;
            facet_area[f]   = 0.5 * area2;
            facet_bed[f]    = n.z() < -0.95 && std::max({ a.z(), b.z(), c.z() }) < 0.5 * params.first_layer_height + 0.05;
            std::vector<std::array<Vec3d, 3>> pieces { { a, b, c } };
            while (!pieces.empty()) {
                const std::array<Vec3d, 3> t = pieces.back();
                pieces.pop_back();
                int    e = 0;
                double longest = 0.;
                for (int k = 0; k < 3; ++k)
                    if (const double l = (t[(k + 1) % 3] - t[k]).norm(); l > longest)
                        longest = l, e = k;
                if (longest <= step) {
                    samples.push_back({ (t[0] + t[1] + t[2]) / 3., n, 0.5 * (t[1] - t[0]).cross(t[2] - t[0]).norm(), f });
                    continue;
                }
                const Vec3d mid = 0.5 * (t[e] + t[(e + 1) % 3]);
                pieces.push_back({ t[e], mid, t[(e + 2) % 3] });
                pieces.push_back({ mid, t[(e + 1) % 3], t[(e + 2) % 3] });
            }
        }
        cluster_lifts();
        corr_target.assign(samples.size(), Vec3d::UnitZ());
        corr_weight.assign(samples.size(), 0.);
        tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices);
    }

    void cluster_lifts()
    {
        const size_t nf = mesh.indices.size();
        facet_lift.assign(nf, Vec3d::Zero());
        std::vector<uint8_t> over(nf, 0);
        for (size_t f = 0; f < nf; ++f)
            over[f] = facet_area[f] > 0. && !facet_bed[f] && facet_normal[f].z() < -sin_threshold;
        const std::vector<Vec3i32> nb = its_face_neighbors(mesh);
        std::vector<int>           cluster(nf, -1);
        const double               sin_target = std::sin(0.5 * M_PI - params.overhang_threshold - Geometry::deg2rad(5.));
        int                        next = 0;
        for (size_t seed = 0; seed < nf; ++seed) {
            if (!over[seed] || cluster[seed] >= 0)
                continue;
            std::vector<size_t> members { seed }, stack { seed };
            cluster[seed] = next;
            while (!stack.empty()) {
                const size_t f = stack.back();
                stack.pop_back();
                for (int e = 0; e < 3; ++e) {
                    const int g = nb[f][e];
                    if (g >= 0 && over[size_t(g)] && cluster[size_t(g)] < 0) {
                        cluster[size_t(g)] = next;
                        members.push_back(size_t(g));
                        stack.push_back(size_t(g));
                    }
                }
            }
            ++next;
            Vec2d  mean = Vec2d::Zero(), centroid = Vec2d::Zero();
            double area = 0., spread = 0.;
            for (size_t f : members) {
                const Vec3d c = (mesh.vertices[mesh.indices[f][0]] + mesh.vertices[mesh.indices[f][1]] + mesh.vertices[mesh.indices[f][2]]).cast<double>() / 3.;
                mean += facet_area[f] * facet_normal[f].head<2>();
                spread += facet_area[f] * facet_normal[f].head<2>().norm();
                centroid += facet_area[f] * c.head<2>();
                area += facet_area[f];
            }
            centroid /= area;
            Vec2d u = mean.norm() > 0.3 * spread && mean.norm() > 1e-9 ? Vec2d(mean.normalized()) : Vec2d(centroid - center.head<2>());
            u = u.norm() > 1e-6 ? Vec2d(u.normalized()) : Vec2d(Vec2d::UnitX());
            double var = 0.;
            for (size_t f : members) {
                const Vec3d c = (mesh.vertices[mesh.indices[f][0]] + mesh.vertices[mesh.indices[f][1]] + mesh.vertices[mesh.indices[f][2]]).cast<double>() / 3.;
                var += facet_area[f] * (c.head<2>() - centroid).squaredNorm();
            }
            const bool ring = params.source == OrientationFieldSource::SupportFree && mean.norm() <= 0.3 * spread &&
                              (centroid - center.head<2>()).norm() < 0.5 * std::sqrt(var / area);
            for (size_t f : members) {
                const Vec3d &n   = facet_normal[f];
                Vec2d        uf  = u;
                if (ring) {
                    const Vec2d r = ((mesh.vertices[mesh.indices[f][0]] + mesh.vertices[mesh.indices[f][1]] + mesh.vertices[mesh.indices[f][2]])
                                         .cast<double>() / 3.).head<2>() - centroid;
                    if (r.norm() > 1e-6)
                        uf = r.normalized();
                }
                const double s    = std::clamp((n.head<2>().norm() - 0.2) / 0.3, 0., 1.);
                const double w    = s * s * (3. - 2. * s);
                Vec2d        dir  = (1. - w) * uf + (n.head<2>().norm() > 1e-9 ? Vec2d(w * n.head<2>().normalized()) : Vec2d::Zero());
                dir               = dir.norm() > 1e-6 ? Vec2d(dir.normalized()) : uf;
                double       tau = params.max_tilt;
                for (int deg = 0; deg <= 90; ++deg) {
                    const double t = Geometry::deg2rad(double(deg));
                    if (std::sin(t) * n.head<2>().dot(dir) + std::cos(t) * n.z() >= -sin_target) {
                        tau = t;
                        break;
                    }
                }
                tau          = std::min(tau, params.max_tilt);
                facet_lift[f] = Vec3d(std::sin(tau) * dir.x(), std::sin(tau) * dir.y(), std::cos(tau));
            }
        }
    }

    void table_limits()
    {
        table_drop.assign(91, std::numeric_limits<double>::infinity());
        for (int deg = 0; deg <= 90; ++deg) {
            const double tau  = Geometry::deg2rad(double(deg));
            double       drop = -std::numeric_limits<double>::infinity();
            bool         ok   = true;
            for (int s = 0; s < 24 && ok; ++s) {
                const double psi = 2. * M_PI * s / 24.;
                const Vec3d  axis(std::sin(tau) * std::cos(psi), std::sin(tau) * std::sin(psi), std::cos(tau));
                if (params.kinematics != nullptr && params.kinematics->rotary_count() > 0) {
                    JointPose joints;
                    if (!params.kinematics->inverse(ToolPose{ Vec3d(0., 0., 50.), axis }, nullptr, joints)) {
                        ok = false;
                        break;
                    }
                }
                const Matrix3d h = field_head_frame(params.kinematics, Vec3d(0., 0., 50.), axis);
                for (const HeadBox &box : params.head.boxes) {
                    if (box.infinite_x || box.infinite_y)
                        continue;
                    const double grow = box.nozzle ? 0. : params.head.lateral_margin;
                    const double low  = box.nozzle ? 0. : params.head.margin;
                    for (int c = 0; c < 8; ++c) {
                        const Vec3d corner((c & 1) ? box.max.x() + grow : box.min.x() - grow,
                                           ((c >> 1) & 1) ? box.max.y() + grow : box.min.y() - grow,
                                           ((c >> 2) & 1) ? box.max.z() : box.min.z() - low);
                        drop = std::max(drop, -(h.transpose() * corner).z());
                    }
                }
            }
            table_drop[size_t(deg)] = ok ? std::max(drop, 0.) : std::numeric_limits<double>::infinity();
        }
    }

    double table_tilt(double z) const
    {
        double best = 0.;
        for (int deg = 0; deg <= 90; ++deg) {
            if (table_drop[size_t(deg)] > z)
                break;
            best = Geometry::deg2rad(double(deg));
        }
        return best;
    }

    void update_caps()
    {
        for (int k = 0; k < F.m_nz; ++k) {
            const double cap = std::min(params.max_tilt, table_tilt(F.m_origin.z() + k * F.m_pitch));
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i) {
                    const size_t id = F.node(i, j, k);
                    tilt_cap[id]    = fixed[id] ? 0. : cap * scale[id];
                }
        }
    }

    template<class EdgeTarget>
    void solve(std::vector<double> &x, const std::vector<double> &c, const std::vector<double> &ct, EdgeTarget edge,
               double tol = 1e-6, int max_iter = 3000) const
    {
        const int    nx = F.m_nx, ny = F.m_ny, nz = F.m_nz;
        const size_t n = x.size(), sy = size_t(nx), sz = size_t(nx) * size_t(ny);
        std::vector<double> b(n, 0.), r(n, 0.), z(n, 0.), p(n, 0.), ap(n, 0.), diag(n, 1.);
        auto for_nodes = [&](auto fn) {
            tbb::parallel_for(tbb::blocked_range<int>(0, nz), [&](const tbb::blocked_range<int> &range) {
                for (int k = range.begin(); k < range.end(); ++k)
                    for (int j = 0; j < ny; ++j)
                        for (int i = 0; i < nx; ++i)
                            fn(i, j, k, F.node(i, j, k));
            });
        };
        for_nodes([&](int i, int j, int k, size_t id) {
            if (fixed[id]) {
                b[id]    = 0.;
                diag[id] = 1.;
                return;
            }
            double rhs = c[id] * ct[id], deg = 0.;
            auto nb = [&](bool ok, size_t jd, int axis, int sign) {
                if (!ok)
                    return;
                deg += 1.;
                rhs -= edge(id, jd, axis, sign);
                if (fixed[jd])
                    rhs += x[jd];
            };
            nb(i > 0, id - 1, 0, -1);
            nb(i + 1 < nx, id + 1, 0, 1);
            nb(j > 0, id - sy, 1, -1);
            nb(j + 1 < ny, id + sy, 1, 1);
            nb(k > 0, id - sz, 2, -1);
            nb(k + 1 < nz, id + sz, 2, 1);
            b[id]    = rhs;
            diag[id] = deg + c[id];
        });
        auto apply = [&](const std::vector<double> &v, std::vector<double> &out) {
            for_nodes([&](int i, int j, int k, size_t id) {
                if (fixed[id]) {
                    out[id] = 0.;
                    return;
                }
                double s = diag[id] * v[id];
                auto nb = [&](bool ok, size_t jd) {
                    if (ok && !fixed[jd])
                        s -= v[jd];
                };
                nb(i > 0, id - 1);
                nb(i + 1 < nx, id + 1);
                nb(j > 0, id - sy);
                nb(j + 1 < ny, id + sy);
                nb(k > 0, id - sz);
                nb(k + 1 < nz, id + sz);
                out[id] = s;
            });
        };
        auto dot = [&](const std::vector<double> &a, const std::vector<double> &bb) {
            return tbb::parallel_deterministic_reduce(
                tbb::blocked_range<size_t>(0, n, 8192), 0.,
                [&](const tbb::blocked_range<size_t> &range, double s) {
                    for (size_t i = range.begin(); i < range.end(); ++i)
                        s += a[i] * bb[i];
                    return s;
                },
                std::plus<double>());
        };
        auto each = [&](auto fn) {
            tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 8192), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t i = range.begin(); i < range.end(); ++i)
                    fn(i);
            });
        };
        std::vector<double> xf = x;
        for (size_t i = 0; i < n; ++i)
            if (fixed[i])
                xf[i] = 0.;
        apply(xf, ap);
        for (size_t i = 0; i < n; ++i) {
            r[i] = fixed[i] ? 0. : b[i] - ap[i];
            z[i] = r[i] / diag[i];
            p[i] = z[i];
        }
        const double bnorm = std::sqrt(std::max(dot(b, b), 1e-30));
        double       rz    = dot(r, z);
        for (int it = 0; it < max_iter; ++it) {
            if (std::sqrt(dot(r, r)) < tol * bnorm)
                break;
            if ((it & 63) == 0)
                check_cancel();
            apply(p, ap);
            const double pap = dot(p, ap);
            if (std::abs(pap) < 1e-300)
                break;
            const double alpha = rz / pap;
            each([&](size_t i) {
                xf[i] += alpha * p[i];
                r[i] -= alpha * ap[i];
                z[i] = r[i] / diag[i];
            });
            const double rz_new = dot(r, z);
            const double beta   = rz_new / rz;
            rz                  = rz_new;
            each([&](size_t i) { p[i] = z[i] + beta * p[i]; });
        }
        for (size_t i = 0; i < n; ++i)
            if (!fixed[i])
                x[i] = xf[i];
    }

    static constexpr double TargetWeight = 24.;
    void splat(const Vec3d &p, const Vec3d &target, double weight, std::vector<Vec3d> &acc, std::vector<double> &wsum) const
    {
        weight *= TargetWeight / (F.m_pitch * F.m_pitch);
        int   i, j, k;
        Vec3d t;
        F.locate(p, i, j, k, t);
        for (int c = 0; c < 8; ++c) {
            const int    a = c & 1, b = (c >> 1) & 1, e = (c >> 2) & 1;
            const double w = (a ? t.x() : 1. - t.x()) * (b ? t.y() : 1. - t.y()) * (e ? t.z() : 1. - t.z());
            const size_t id = F.node(i + a, j + b, k + e);
            acc[id] += w * weight * target;
            wsum[id] += w * weight;
        }
    }

    Vec3d outward(const Vec3d &p) const
    {
        const Vec3d o(p.x() - center.x(), p.y() - center.y(), 0.);
        return o.squaredNorm() > 1e-12 ? Vec3d(o.normalized()) : Vec3d(Vec3d::UnitX());
    }

    Vec3d layer_normal(const Vec3d &p) const
    {
        const double e = 0.5 * F.m_pitch;
        Vec3d        g(F.phi(p + Vec3d(e, 0., 0.)) - F.phi(p - Vec3d(e, 0., 0.)),
                       F.phi(p + Vec3d(0., e, 0.)) - F.phi(p - Vec3d(0., e, 0.)),
                       F.phi(p + Vec3d(0., 0., e)) - F.phi(p - Vec3d(0., 0., e)));
        g /= 2. * e;
        return g;
    }

    void base_targets(std::vector<Vec3d> &acc, std::vector<double> &wsum) const
    {
        const double cos_flat = std::cos(Geometry::deg2rad(3.));
        for (size_t s = 0; s < samples.size(); ++s) {
            const FacetSample &fs = samples[s];
            if (facet_bed[fs.facet] || fs.p.z() <= F.m_bed_band + 1e-3)
                continue;
            const Vec3d &n = fs.n;
            if (n.z() < -sin_threshold) {
                splat(fs.p, facet_lift[fs.facet], 4. * fs.area, acc, wsum);
            } else if (params.conform_tops && n.z() > 0.) {
                if (n.z() >= cos_flat)
                    splat(fs.p, Vec3d::UnitZ(), 0.5 * fs.area, acc, wsum);
                else if (n.z() < std::cos(Geometry::deg2rad(80.))) {
                } else {
                    const double slope = std::acos(std::clamp(n.z(), -1., 1.));
                    const Vec2d  away  = -n.head<2>().normalized();
                    const double tau   = std::min(0.5 * M_PI - slope, params.max_tilt);
                    splat(fs.p, Vec3d(std::sin(tau) * away.x(), std::sin(tau) * away.y(), std::cos(tau)), fs.area, acc, wsum);
                }
            }
        }
    }

    void diffuse_orientation()
    {
        const size_t        n = nodes();
        std::vector<Vec3d>  acc(n, Vec3d::Zero());
        std::vector<double> wsum(n, 0.);
        base_targets(acc, wsum);
        for (size_t s = 0; s < samples.size(); ++s)
            if (corr_weight[s] > 0.)
                splat(samples[s].p, corr_target[s], corr_weight[s], acc, wsum);
        const double reg = 0.02;
        std::vector<double> c(n), ct(n), x(n);
        for (int axis = 0; axis < 3; ++axis) {
            for (size_t i = 0; i < n; ++i) {
                c[i]  = wsum[i] + reg;
                ct[i] = (acc[i][axis] + reg * (axis == 2 ? 1. : 0.)) / c[i];
                x[i]  = fixed[i] ? (axis == 2 ? 1. : 0.) : d[i][axis];
            }
            solve(x, c, ct, [](size_t, size_t, int, int) { return 0.; }, 1e-4, 1500);
            for (size_t i = 0; i < n; ++i)
                d[i][axis] = x[i];
        }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                Vec3d v = d[i];
                if (v.squaredNorm() < 1e-12 || v.z() <= 0.)
                    v = Vec3d::UnitZ();
                v = clamp_tilt(v.normalized(), tilt_cap[i]);
                if (const double t = v.head<2>().dot(block_dir[i]); t > 0. && block_scale[i] < 1.) {
                    Vec2d hz = v.head<2>() - (1. - block_scale[i]) * t * block_dir[i];
                    v        = Vec3d(hz.x(), hz.y(), std::sqrt(std::max(0., 1. - hz.squaredNorm())));
                }
                d[i] = v;
            }
        });
    }

    void integrate_phi(double tol = 1e-6)
    {
        const size_t        n = nodes();
        std::vector<double> c(n, 0.), ct(n, 0.);
        const double        h = F.m_pitch;
        solve(F.m_phi, c, ct, [&](size_t i, size_t j, int axis, int sign) { return sign * h * 0.5 * (d[i][axis] + d[j][axis]); }, tol, 4000);
    }

    void integrate_uv()
    {
        const size_t        n = nodes();
        std::vector<Vec3d>  ex(n), ey(n);
        for (size_t i = 0; i < n; ++i) {
            const Vec3d &a = d[i];
            const double k = 1. / (1. + a.z());
            ex[i]          = Vec3d(1. - a.x() * a.x() * k, -a.x() * a.y() * k, -a.x());
            ey[i]          = Vec3d(-a.x() * a.y() * k, 1. - a.y() * a.y() * k, -a.y());
        }
        std::vector<double> c(n, 0.), ct(n, 0.);
        const double        h = F.m_pitch;
        solve(F.m_u, c, ct, [&](size_t i, size_t j, int axis, int sign) { return sign * h * 0.5 * (ex[i][axis] + ex[j][axis]); }, 1e-7, 3000);
        solve(F.m_v, c, ct, [&](size_t i, size_t j, int axis, int sign) { return sign * h * 0.5 * (ey[i][axis] + ey[j][axis]); }, 1e-7, 3000);
    }

    struct Eval
    {
        double overhang_area = 0., support_volume = 0., step_area = 0.;
        double shell_step_area = 0.;
    };

    template<class Normal, class Phi> Eval evaluate(Normal g_of, Phi phi_of, bool support) const
    {
        Eval         e;
        const double h = params.layer_height;
        for (const FacetSample &fs : samples) {
            if (facet_bed[fs.facet] || fs.p.z() <= F.m_bed_band + 1e-3)
                continue;
            const Vec3d  g  = g_of(fs.p);
            const double ng = fs.n.dot(g);
            if (ng >= -sin_threshold)
                continue;
            e.overhang_area += fs.area;
            if (!support)
                continue;
            const Vec3d  dir    = -g;
            const Vec3d  origin = fs.p + 0.02 * dir;
            igl::Hit<float> hit;
            double       dist   = dir.z() < -1e-6 ? origin.z() / -dir.z() : 1e3;
            if (AABBTreeIndirect::intersect_ray_first_hit(mesh.vertices, mesh.indices, tree, origin, dir, hit))
                dist = std::min(dist, double(hit.t));
            e.support_volume += fs.area * (-ng) * std::min(dist, 1e3);
        }
        for (size_t f = 0; f < mesh.indices.size(); ++f) {
            const Vec3d &n = facet_normal[f];
            if (facet_area[f] <= 0. || n.z() <= 0.05)
                continue;
            const Vec3d a = mesh.vertices[mesh.indices[f][0]].cast<double>();
            const Vec3d b = mesh.vertices[mesh.indices[f][1]].cast<double>();
            const Vec3d c = mesh.vertices[mesh.indices[f][2]].cast<double>();
            const Vec3d m = (a + b + c) / 3.;
            const Vec3d g = g_of(m);
            const double ng = n.dot(g);
            if (ng <= 0.05)
                continue;
            const std::array<Vec3d, 7> pts { a, b, c, 0.5 * (a + b), 0.5 * (b + c), 0.5 * (c + a), m };
            double lo = std::numeric_limits<double>::max(), hi = -lo, e2 = 0.;
            for (const Vec3d &p : pts) {
                const double v = phi_of(p) - params.first_layer_height;
                lo             = std::min(lo, v);
                hi             = std::max(hi, v);
                const double r = v - h * std::round(v / h);
                e2 += r * r;
            }
            const double phase = hi - lo >= h ? 1. : std::min(1., 12. * e2 / (7. * h * h));
            e.step_area += facet_area[f] * ng * ng * phase;
            e.shell_step_area += facet_area[f] * ng * ng * phase * (n.z() >= std::cos(0.25 * M_PI) ? 0.15 : 1.);
        }
        return e;
    }

    Eval evaluate_field(bool support) const
    {
        return evaluate([this](const Vec3d &p) {
                            const Vec3d g = layer_normal(p);
                            return g.squaredNorm() > 1e-24 ? Vec3d(g.normalized()) : Vec3d(Vec3d::UnitZ());
                        },
                        [this](const Vec3d &p) { return F.phi(p); }, support);
    }

    double correct_overhangs()
    {
        const double sin_target = std::sin(0.5 * M_PI - params.overhang_threshold - Geometry::deg2rad(5.));
        double       area       = 0.;
        for (size_t s = 0; s < samples.size(); ++s) {
            const FacetSample &fs = samples[s];
            if (facet_bed[fs.facet] || fs.p.z() <= F.m_bed_band + 1e-3)
                continue;
            Vec3d g = layer_normal(fs.p);
            if (g.squaredNorm() < 1e-24)
                continue;
            g.normalize();
            if (fs.n.dot(g) >= -sin_threshold)
                continue;
            area += fs.area;
            corr_target[s] = lift_overhang(g, fs.n, sin_target, outward(fs.p));
            corr_weight[s] = corr_weight[s] <= 0. ? 4. * fs.area : std::min(corr_weight[s] * 2., 256. * fs.area);
        }
        return area;
    }

    std::vector<size_t> head_conflicts() const
    {
        const std::vector<Vec3d> pts = head_samples(params.head, std::max(1.5, F.m_pitch));
        std::vector<uint8_t> near(F.m_inside.size(), 0);
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i)
                    if (F.m_inside[F.node(i, j, k)])
                        for (int c = -1; c <= 1; ++c)
                            for (int b = -1; b <= 1; ++b)
                                for (int a = -1; a <= 1; ++a) {
                                    const int x = i + a, y = j + b, z = k + c;
                                    if (x >= 0 && y >= 0 && z >= 0 && x < F.m_nx && y < F.m_ny && z < F.m_nz)
                                        near[F.node(x, y, z)] = 1;
                                }
        std::vector<uint8_t> hit(near.size(), 0);
        tbb::parallel_for(tbb::blocked_range<int>(0, F.m_nz), [&](const tbb::blocked_range<int> &range) {
            for (int k = range.begin(); k < range.end(); ++k)
                for (int j = 0; j < F.m_ny; ++j)
                    for (int i = 0; i < F.m_nx; ++i) {
                        const size_t id = F.node(i, j, k);
                        if (!near[id] || fixed[id] || ((i + j + k) & 1))
                            continue;
                        const Vec3d p = F.node_pos(i, j, k);
                        Vec3d       g = layer_normal(p);
                        if (g.squaredNorm() < 1e-24)
                            continue;
                        const Matrix3d h = field_head_frame(params.kinematics, p, snap_upright(clamp_tilt(g.normalized(), F.m_tilt_budget))).transpose();
                        for (const Vec3d &s : pts)
                            if ((p + h * s).z() < 0.) {
                                hit[id] = 1;
                                break;
                            }
                    }
        });
        std::vector<size_t> out;
        for (size_t i = 0; i < hit.size(); ++i)
            if (hit[i])
                out.push_back(i);
        if (!out.empty())
            F.notes.emplace_back("head check: " + std::to_string(out.size()) + " poses reach below the table");
        return out;
    }

    void relax(const std::vector<size_t> &conflicts, int round)
    {
        if (round >= 3) {
            for (double &s : scale)
                s *= 0.5;
            F.m_stats.tilt_scale *= 0.5;
            if (round >= 6)
                std::fill(scale.begin(), scale.end(), 0.);
            return;
        }
        const int            r = std::max(1, int(std::ceil(6. / F.m_pitch)));
        std::vector<uint8_t> mark(scale.size(), 0);
        const size_t         sx = size_t(F.m_nx), sxy = size_t(F.m_nx) * size_t(F.m_ny);
        for (size_t id : conflicts) {
            const int k = int(id / sxy), j = int((id % sxy) / sx), i = int(id % sx);
            for (int c = -r; c <= r; ++c)
                for (int b = -r; b <= r; ++b)
                    for (int a = -r; a <= r; ++a) {
                        if (a * a + b * b + c * c > r * r)
                            continue;
                        const int x = i + a, y = j + b, z = k + c;
                        if (x >= 0 && y >= 0 && z >= 0 && x < F.m_nx && y < F.m_ny && z < F.m_nz)
                            mark[F.node(x, y, z)] = 1;
                    }
        }
        for (size_t i = 0; i < scale.size(); ++i)
            if (mark[i])
                scale[i] *= 0.5;
    }

    bool solve_with_layer_solver()
    {
        LayerFieldParams lp;
        lp.layer_height      = params.layer_height;
        lp.min_layer_height *= params.layer_height / 0.2;
        lp.max_layer_height *= params.layer_height / 0.2;
        lp.flat_base_height  = F.m_bed_band + F.m_pitch;
        lp.overhang_angle    = 90. - Geometry::rad2deg(params.overhang_threshold);
        lp.head              = &params.head;
        lp.throw_if_canceled = cancel;
        lp.tool_axis         = [this](const Vec3d &p) {
            const Vec3d v = F.trilinear(d, p);
            return v.squaredNorm() > 1e-24 ? Vec3d(v.normalized()) : Vec3d(Vec3d::UnitZ());
        };
        LayerFieldReport      report;
        const StackLayerField field = solve_layer_field(mesh, lp, &report);
        if (field.empty())
            return false;
        tbb::parallel_for(tbb::blocked_range<int>(0, F.m_nz), [&](const tbb::blocked_range<int> &range) {
            for (int k = range.begin(); k < range.end(); ++k)
                for (int j = 0; j < F.m_ny; ++j)
                    for (int i = 0; i < F.m_nx; ++i) {
                        const size_t id = F.node(i, j, k);
                        if (!fixed[id])
                            F.m_phi[id] = field.phi(F.node_pos(i, j, k));
                    }
        });
        BOOST_LOG_TRIVIAL(info) << "Orientation field: layer solver " << report.solve_seconds << " s, conformal top "
                                << report.field.conformal_top << ", axis deviation mean " << report.field.axis_dev_mean << " deg";
        return true;
    }

    void choose_layers()
    {
        F.m_stats.solver_field = false;
        if (!params.layer_solver || !params.relax_rounds.empty() || params.tilt_scale < 1.)
            return;
        const Eval                own   = evaluate_field(false);
        const std::vector<double> saved = F.m_phi;
        bool ok = false;
        try {
            ok = solve_with_layer_solver();
        } catch (const CanceledException &) {
            throw;
        } catch (const std::exception &ex) {
            F.notes.emplace_back(std::string("layer solver failed: ") + ex.what());
        }
        if (!ok) {
            F.m_phi = saved;
            return;
        }
        double lfo_tilt = 0.;
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i)
                    if (const size_t id = F.node(i, j, k); F.m_inside[id] && !fixed[id]) {
                        const Vec3d g = layer_normal(F.node_pos(i, j, k));
                        if (g.squaredNorm() > 1e-24)
                            lfo_tilt = std::max(lfo_tilt, std::acos(std::clamp(g.z() / g.norm(), -1., 1.)));
                    }
        if (lfo_tilt > params.max_tilt + Geometry::deg2rad(2.)) {
            F.notes.emplace_back("layer solver refused: its layers tilt " + std::to_string(int(std::lround(Geometry::rad2deg(lfo_tilt)))) +
                                 " deg, past the budget");
            F.m_phi = saved;
            return;
        }
        const Eval   lfo   = evaluate_field(false);
        const double s_own = own.overhang_area + 0.5 * own.step_area, s_lfo = lfo.overhang_area + 0.5 * lfo.step_area;
        F.notes.emplace_back("layers: own integral " + std::to_string(s_own) + ", layer solver " + std::to_string(s_lfo));
        if (s_lfo <= s_own)
            F.m_stats.solver_field = true;
        else
            F.m_phi = saved;
    }

    bool planar_below()
    {
        const double cos_min = std::cos(Geometry::deg2rad(2.));
        double       z_tilt  = std::numeric_limits<double>::infinity();
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i)
                    if (const size_t id = F.node(i, j, k); F.m_inside[id] && !fixed[id] && d[id].z() < cos_min)
                        z_tilt = std::min(z_tilt, F.node_pos(i, j, k).z());
        if (!std::isfinite(z_tilt))
            return false;
        const double h    = params.layer_height;
        const int    kcut = int(std::floor((z_tilt - 2. * F.m_pitch - params.first_layer_height) / h + 1e-6));
        if (kcut <= 0)
            return false;
        const double cut = params.first_layer_height + kcut * h;
        if (cut <= F.m_bed_band + 1e-6)
            return false;
        F.m_bed_band = cut;
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i) {
                    const size_t id = F.node(i, j, k);
                    const Vec3d  p  = F.node_pos(i, j, k);
                    if (p.z() <= cut + F.m_pitch + 1e-6) {
                        fixed[id]   = 1;
                        d[id]       = Vec3d::UnitZ();
                        F.m_phi[id] = p.z();
                        F.m_u[id]   = p.x();
                        F.m_v[id]   = p.y();
                    }
                }
        F.notes.emplace_back("planar below " + std::to_string(cut) + " mm, the lowest tilt at " + std::to_string(z_tilt) + " mm");
        return true;
    }


    std::vector<size_t> material_conflicts(std::vector<Vec3d> &reached) const
    {
        const std::vector<Vec3d> pts = head_samples(params.head, std::max(1.5, F.m_pitch));
        const double             hop = std::max(0.4, params.head.margin);
        std::vector<uint8_t>     near(F.m_inside.size(), 0);
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i)
                    if (F.m_inside[F.node(i, j, k)])
                        for (int c = -1; c <= 1; ++c)
                            for (int b = -1; b <= 1; ++b)
                                for (int a = -1; a <= 1; ++a) {
                                    const int x = i + a, y = j + b, z = k + c;
                                    if (x >= 0 && y >= 0 && z >= 0 && x < F.m_nx && y < F.m_ny && z < F.m_nz)
                                        near[F.node(x, y, z)] = 1;
                                }
        std::vector<uint8_t> hit(near.size(), 0);
        reached.assign(near.size(), Vec3d::Zero());
        tbb::parallel_for(tbb::blocked_range<int>(0, F.m_nz), [&](const tbb::blocked_range<int> &range) {
            for (int k = range.begin(); k < range.end(); ++k)
                for (int j = 0; j < F.m_ny; ++j)
                    for (int i = 0; i < F.m_nx; ++i) {
                        const size_t id = F.node(i, j, k);
                        if (!near[id] || fixed[id] || ((i + j + k) & 1))
                            continue;
                        const Vec3d p = F.node_pos(i, j, k);
                        Vec3d       g = layer_normal(p);
                        if (g.squaredNorm() < 1e-24)
                            continue;
                        const Matrix3d h     = field_head_frame(params.kinematics, p, snap_upright(clamp_tilt(g.normalized(), F.m_tilt_budget))).transpose();
                        const double   level = F.phi(p) + hop;
                        for (const Vec3d &s : pts) {
                            const Vec3d q = p + h * s;
                            if (q.z() < 0. || (F.inside(q) && F.phi(q) < level)) {
                                hit[id]     = 1;
                                reached[id] = q;
                                break;
                            }
                        }
                    }
        });
        std::vector<size_t> out;
        for (size_t i = 0; i < hit.size(); ++i)
            if (hit[i])
                out.push_back(i);
        return out;
    }

    static constexpr double SfRegion       = 10.;
    static constexpr double SfRegionWeight = 4.;
    static constexpr double SfAnchorNear   = 0.02;
    static constexpr double SfAnchorFar    = 1.;
    static constexpr double SfAnchorStart  = 1.;
    static constexpr double SfAnchorRamp   = 12.;
    static constexpr double SfEdgeWeight   = 0.05;
    static constexpr double SfOutsideCell  = 0.1;
    static constexpr double SfThickLo      = 0.625;
    static constexpr double SfThickHi      = 1.9;
    static constexpr double SfThickWeight  = 20.;
    static constexpr double SfPairWeight   = 30.;
    static constexpr double SfCapDecay     = 0.75;
    static constexpr double SfDecayRadius  = 3.;

    struct HeadPair
    {
        size_t tip;
        Vec3d  reached;
    };
    std::vector<HeadPair> head_pairs;
    std::vector<double>   scale_at_start;

    Vec3d pos_of(size_t id) const
    {
        const size_t sxy = size_t(F.m_nx) * size_t(F.m_ny);
        return F.node_pos(int(id % size_t(F.m_nx)), int((id % sxy) / size_t(F.m_nx)), int(id / sxy));
    }

    void nearest_seed(const std::vector<size_t> &seeds, double radius, std::vector<double> &dist, std::vector<int> &src) const
    {
        const size_t n = nodes();
        dist.assign(n, std::numeric_limits<double>::infinity());
        src.assign(n, -1);
        using Item = std::pair<double, size_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        auto offer = [&](size_t id, int s) {
            const double dd = (pos_of(id) - samples[size_t(s)].p).norm();
            if (dd < dist[id] && dd <= radius) {
                dist[id] = dd;
                src[id]  = s;
                queue.emplace(dd, id);
            }
        };
        for (size_t s : seeds) {
            int   i, j, k;
            Vec3d t;
            F.locate(samples[s].p, i, j, k, t);
            for (int c = 0; c < 8; ++c)
                offer(F.node(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1)), int(s));
        }
        const size_t sx = size_t(F.m_nx), sxy = size_t(F.m_nx) * size_t(F.m_ny);
        while (!queue.empty()) {
            const auto [dd, id] = queue.top();
            queue.pop();
            if (dd > dist[id])
                continue;
            const int k = int(id / sxy), j = int((id % sxy) / sx), i = int(id % sx);
            for (int c = -1; c <= 1; ++c)
                for (int b = -1; b <= 1; ++b)
                    for (int a = -1; a <= 1; ++a) {
                        const int x = i + a, y = j + b, z = k + c;
                        if ((a | b | c) != 0 && x >= 0 && y >= 0 && z >= 0 && x < F.m_nx && y < F.m_ny && z < F.m_nz)
                            offer(F.node(x, y, z), src[id]);
                    }
        }
    }

    bool overhang_sample(const FacetSample &fs) const
    {
        return !facet_bed[fs.facet] && fs.p.z() > F.m_bed_band + 1e-3 && fs.n.z() < -sin_threshold;
    }

    void diffuse_support_free()
    {
        const size_t        n = nodes();
        std::vector<Vec3d>  acc(n, Vec3d::Zero());
        std::vector<double> wsum(n, 0.);
        std::vector<size_t> over, seeds;
        for (size_t s = 0; s < samples.size(); ++s) {
            const bool o = overhang_sample(samples[s]);
            if (o) {
                over.push_back(s);
                splat(samples[s].p, facet_lift[samples[s].facet], 4. * samples[s].area, acc, wsum);
            }
            if (corr_weight[s] > 0.)
                splat(samples[s].p, corr_target[s], corr_weight[s], acc, wsum);
            if (o || corr_weight[s] > 0.)
                seeds.push_back(s);
        }
        std::vector<double> dist;
        std::vector<int>    src;
        nearest_seed(over, SfRegion, dist, src);
        for (size_t i = 0; i < n; ++i) {
            if (src[i] < 0 || fixed[i])
                continue;
            const FacetSample &fs = samples[size_t(src[i])];
            if ((pos_of(i) - fs.p).dot(-fs.n) <= -1.5 * F.m_pitch)
                continue;
            const double t = std::clamp(dist[i] / SfRegion, 0., 1.);
            const double w = SfRegionWeight * (1. - 0.7 * t * t * (3. - 2. * t));
            acc[i] += w * facet_lift[fs.facet];
            wsum[i] += w;
        }
        nearest_seed(seeds, SfAnchorStart + SfAnchorRamp, dist, src);
        std::vector<double> c(n), ct(n), x(n);
        for (int axis = 0; axis < 3; ++axis) {
            for (size_t i = 0; i < n; ++i) {
                const double t      = std::clamp((dist[i] - SfAnchorStart) / SfAnchorRamp, 0., 1.);
                const double anchor = SfAnchorNear + (SfAnchorFar - SfAnchorNear) * t * t * (3. - 2. * t);
                c[i]  = wsum[i] + anchor;
                ct[i] = (acc[i][axis] + anchor * (axis == 2 ? 1. : 0.)) / c[i];
                x[i]  = fixed[i] ? (axis == 2 ? 1. : 0.) : d[i][axis];
            }
            solve(x, c, ct, [](size_t, size_t, int, int) { return 0.; }, 1e-5, 800);
            for (size_t i = 0; i < n; ++i)
                d[i][axis] = x[i];
        }
        for (size_t i = 0; i < n; ++i) {
            Vec3d v = d[i];
            if (v.squaredNorm() < 1e-12 || v.z() <= 0.)
                v = Vec3d::UnitZ();
            d[i] = clamp_tilt(v.normalized(), tilt_cap[i]);
        }
    }

    void solve_phi_support_free()
    {
        using Sparse = Eigen::SparseMatrix<double>;
        using Trip   = Eigen::Triplet<double>;
        const size_t n = nodes();
        std::vector<int> col(n, -1);
        int              m = 0;
        for (size_t i = 0; i < n; ++i)
            if (!fixed[i])
                col[i] = m++;
        struct Rows
        {
            std::vector<Trip>   trip;
            std::vector<double> rhs;
        };
        auto add = [&](Rows &r, const size_t *ids, const double *coef, int cnt, double target, double weight) {
            const int    row = int(r.rhs.size());
            const double sw  = std::sqrt(weight);
            for (int a = 0; a < cnt; ++a) {
                if (fixed[ids[a]])
                    target -= coef[a] * F.m_phi[ids[a]];
                else
                    r.trip.emplace_back(row, col[ids[a]], sw * coef[a]);
            }
            r.rhs.push_back(sw * target);
        };
        auto system = [&](const Rows &r, Sparse &M, Eigen::VectorXd &b) {
            Sparse A(Eigen::Index(r.rhs.size()), m);
            A.setFromTriplets(r.trip.begin(), r.trip.end());
            const Eigen::Map<const Eigen::VectorXd> rhs(r.rhs.data(), Eigen::Index(r.rhs.size()));
            M = A.transpose() * A;
            b = A.transpose() * rhs;
        };
        Rows base;
        const size_t stride[3] = { 1, size_t(F.m_nx), size_t(F.m_nx) * size_t(F.m_ny) };
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i) {
                    const size_t id = F.node(i, j, k);
                    const int    at[3] = { i + 1 < F.m_nx, j + 1 < F.m_ny, k + 1 < F.m_nz };
                    for (int axis = 0; axis < 3; ++axis)
                        if (at[axis]) {
                            const size_t ids[2]  = { id + stride[axis], id };
                            const double coef[2] = { 1., -1. };
                            add(base, ids, coef, 2, F.m_pitch * 0.5 * (d[ids[0]][axis] + d[id][axis]), SfEdgeWeight);
                        }
                }
        struct Cell
        {
            size_t ids[8];
            double dd[8];
            double weight;
        };
        std::vector<Cell> cells;
        cells.reserve(n);
        const double g = 0.25 / F.m_pitch;
        for (int k = 0; k + 1 < F.m_nz; ++k)
            for (int j = 0; j + 1 < F.m_ny; ++j)
                for (int i = 0; i + 1 < F.m_nx; ++i) {
                    Cell  cell;
                    Vec3d dc     = Vec3d::Zero();
                    bool  inside = false;
                    for (int c = 0; c < 8; ++c) {
                        cell.ids[c] = F.node(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                        dc += d[cell.ids[c]];
                        inside |= F.m_inside[cell.ids[c]] != 0;
                    }
                    dc          = dc.norm() > 1e-12 ? Vec3d(dc.normalized()) : Vec3d(Vec3d::UnitZ());
                    cell.weight = inside ? 1. : SfOutsideCell;
                    double cross[3][8];
                    for (int c = 0; c < 8; ++c) {
                        const Vec3d e((c & 1) ? g : -g, ((c >> 1) & 1) ? g : -g, ((c >> 2) & 1) ? g : -g);
                        const Vec3d x = e.cross(dc);
                        for (int r = 0; r < 3; ++r)
                            cross[r][c] = x[r];
                        cell.dd[c] = e.dot(dc);
                    }
                    for (int r = 0; r < 3; ++r)
                        add(base, cell.ids, cross[r], 8, 0., cell.weight);
                    cells.push_back(cell);
                }
        Sparse          M0;
        Eigen::VectorXd b0;
        system(base, M0, b0);
        Eigen::VectorXd x(m);
        for (size_t i = 0; i < n; ++i)
            if (col[i] >= 0)
                x[col[i]] = F.m_phi[i];
        std::vector<int8_t> thick(cells.size(), 0);
        std::vector<uint8_t> pair_on(head_pairs.size(), 0);
        auto value = [&](const size_t *ids, const double *coef, int cnt) {
            double v = 0.;
            for (int a = 0; a < cnt; ++a)
                v += coef[a] * (fixed[ids[a]] ? F.m_phi[ids[a]] : x[col[ids[a]]]);
            return v;
        };
        auto pair_row = [&](const HeadPair &hp, size_t *ids, double *coef) {
            int   i, j, k;
            Vec3d t;
            F.locate(hp.reached, i, j, k, t);
            for (int c = 0; c < 8; ++c) {
                const int a = c & 1, b = (c >> 1) & 1, e = (c >> 2) & 1;
                ids[c]  = F.node(i + a, j + b, k + e);
                coef[c] = (a ? t.x() : 1. - t.x()) * (b ? t.y() : 1. - t.y()) * (e ? t.z() : 1. - t.z());
            }
            ids[8]  = hp.tip;
            coef[8] = -1.;
        };
        const double hop = std::max(0.4, params.head.margin);
        for (int inner = 0; inner < 6; ++inner) {
            Rows act;
            for (size_t c = 0; c < cells.size(); ++c)
                if (thick[c] != 0)
                    add(act, cells[c].ids, cells[c].dd, 8, thick[c] < 0 ? 1.05 * SfThickLo : 0.95 * SfThickHi,
                        SfThickWeight * cells[c].weight);
            for (size_t p = 0; p < head_pairs.size(); ++p)
                if (pair_on[p]) {
                    size_t ids[9];
                    double coef[9];
                    pair_row(head_pairs[p], ids, coef);
                    add(act, ids, coef, 9, hop, SfPairWeight);
                }
            Sparse          M = M0;
            Eigen::VectorXd b = b0;
            if (!act.rhs.empty()) {
                Sparse          Ma;
                Eigen::VectorXd ba;
                system(act, Ma, ba);
                M += Ma;
                b += ba;
            }
            Eigen::ConjugateGradient<Sparse, Eigen::Lower | Eigen::Upper> cg;
            cg.setTolerance(1e-7);
            cg.setMaxIterations(600);
            cg.compute(M);
            x = cg.solveWithGuess(b, x);
            check_cancel();
            bool grown = false;
            for (size_t c = 0; c < cells.size(); ++c)
                if (thick[c] == 0) {
                    const double v = value(cells[c].ids, cells[c].dd, 8);
                    if (v < SfThickLo || v > SfThickHi) {
                        thick[c] = v < SfThickLo ? -1 : 1;
                        grown    = true;
                    }
                }
            for (size_t p = 0; p < head_pairs.size(); ++p)
                if (!pair_on[p]) {
                    size_t ids[9];
                    double coef[9];
                    pair_row(head_pairs[p], ids, coef);
                    if (value(ids, coef, 9) < hop) {
                        pair_on[p] = 1;
                        grown      = true;
                    }
                }
            if (!grown)
                break;
        }
        for (size_t i = 0; i < n; ++i)
            if (col[i] >= 0)
                F.m_phi[i] = x[col[i]];
    }

    void run_support_free(std::vector<size_t> &conflicts)
    {
        if (scale_at_start.empty())
            scale_at_start = scale;
        scale = scale_at_start;
        struct Kept
        {
            std::vector<double>        phi;
            std::vector<Vec3d>         d;
            std::vector<size_t>        conflicts;
            std::vector<FieldConflict> listed;
            double                     area = std::numeric_limits<double>::max();
        } kept;
        for (int round = 0; round < 8; ++round) {
            update_caps();
            double best  = std::numeric_limits<double>::max();
            double area  = 0.;
            int    stale = 0;
            for (int it = 0; it < 3; ++it) {
                check_cancel();
                diffuse_support_free();
                solve_phi_support_free();
                area = correct_overhangs();
                BOOST_LOG_TRIVIAL(debug) << "Orientation field: support-free round " << round << " iteration " << it << " overhang " << area << " mm2";
                if (area < 0.5)
                    break;
                if (area < best * 0.98) {
                    best  = area;
                    stale = 0;
                } else if (++stale >= 2)
                    break;
            }
            std::vector<Vec3d>  reached;
            std::vector<size_t> found = material_conflicts(reached);
            std::vector<FieldConflict> listed;
            for (size_t id : found) {
                const Vec3d tip = pos_of(id), q = reached[id];
                const Vec3d dir = q - tip;
                listed.push_back({ tip, q, dir.norm() > 1e-9 ? Vec3d(dir.normalized()) : Vec3d(Vec3d::UnitZ()), q.z() < 0. });
            }
            const bool better = round == 0 || (found.size() < kept.conflicts.size() && area <= 1.1 * kept.area + 0.5);
            if (!better)
                break;
            kept = { F.m_phi, d, found, std::move(listed), area };
            F.m_stats.relax_rounds = round;
            if (found.empty())
                break;
            F.notes.emplace_back("support-free round " + std::to_string(round) + ": " + std::to_string(found.size()) +
                                 " poses reach earlier material or the table, overhang " + std::to_string(area) + " mm2");
            const int r = std::max(1, int(std::ceil(SfDecayRadius / F.m_pitch)));
            std::vector<uint8_t> mark(nodes(), 0);
            const size_t sx = size_t(F.m_nx), sxy = size_t(F.m_nx) * size_t(F.m_ny);
            for (size_t id : found) {
                if (reached[id].z() >= 0.) {
                    head_pairs.push_back({ id, reached[id] });
                    continue;
                }
                const int k = int(id / sxy), j = int((id % sxy) / sx), i = int(id % sx);
                for (int c = std::max(k - r, 0); c <= std::min(k + r, F.m_nz - 1); ++c)
                    for (int b = std::max(j - r, 0); b <= std::min(j + r, F.m_ny - 1); ++b)
                        for (int a = std::max(i - r, 0); a <= std::min(i + r, F.m_nx - 1); ++a)
                            mark[F.node(a, b, c)] = 1;
            }
            for (size_t i = 0; i < nodes(); ++i)
                if (mark[i])
                    scale[i] *= SfCapDecay;
        }
        F.m_phi     = std::move(kept.phi);
        d           = std::move(kept.d);
        conflicts   = std::move(kept.conflicts);
        F.m_conflicts = std::move(kept.listed);
        for (size_t i = 0; i < nodes(); ++i) {
            const Vec3d g = layer_normal(pos_of(i));
            d[i]          = fixed[i] || g.squaredNorm() < 1e-24 || g.z() <= 0. ? Vec3d(Vec3d::UnitZ()) : Vec3d(g.normalized());
        }
    }

    void follow_given()
    {
        const NpLayerField &given = *params.given;
        const double        top   = F.m_origin.z() + (F.m_nz - 1) * F.m_pitch;
        double              band  = given.identity_below();
        for (const auto &[key, value] : given.file().terms)
            if (key == "stat_identity_below_mm" && std::isfinite(value) && value < band)
                band = value;
        F.m_bed_band = std::clamp(band, params.first_layer_height, top + params.layer_height);
        if (F.m_bed_band > params.first_layer_height + 1e-9)
            F.notes.emplace_back("planar below " + std::to_string(F.m_bed_band) + " mm (the given field's own planar part)");
        for (int k = 0; k < F.m_nz; ++k)
            for (int j = 0; j < F.m_ny; ++j)
                for (int i = 0; i < F.m_nx; ++i) {
                    const size_t id = F.node(i, j, k);
                    const Vec3d  p  = F.node_pos(i, j, k);
                    fixed[id]       = fixed[id] || p.z() <= F.m_bed_band + 1e-6;
                    F.m_phi[id]     = fixed[id] ? p.z() : given.phi(p);
                }
        for (size_t i = 0; i < nodes(); ++i) {
            const Vec3d g = layer_normal(pos_of(i));
            d[i]          = fixed[i] || g.squaredNorm() < 1e-24 || g.z() <= 0. ? Vec3d(Vec3d::UnitZ()) : Vec3d(g.normalized());
        }
    }

    void solve_field()
    {
        update_caps();
        double best = std::numeric_limits<double>::max();
        int    stale = 0;
        for (int it = 0; it < 10; ++it) {
            check_cancel();
            diffuse_orientation();
            integrate_phi();
            const double area = correct_overhangs();
            BOOST_LOG_TRIVIAL(debug) << "Orientation field: iteration " << it << " overhang " << area << " mm2";
            if (area < 0.5)
                break;
            if (area < best * 0.98) {
                best  = area;
                stale = 0;
            } else if (++stale >= 2)
                break;
        }
    }

    void run()
    {
        sin_threshold = std::sin(0.5 * M_PI - params.overhang_threshold);
        setup_grid();
        voxelize();
        check_cancel();
        make_samples();
        table_limits();
        const Eval planar = evaluate([](const Vec3d &) { return Vec3d(Vec3d::UnitZ()); }, [](const Vec3d &p) { return p.z(); }, true);
        OrientationFieldStats &st = F.m_stats;
        st.planar_overhang_area  = planar.overhang_area;
        st.planar_support_volume = planar.support_volume;
        st.planar_step_area      = planar.step_area;
        st.shell_step_area       = planar.shell_step_area;
        std::vector<size_t> conflicts;
        if (params.given) {
            follow_given();
            conflicts = head_conflicts();
        } else if (params.source == OrientationFieldSource::SupportFree) {
            std::vector<size_t> left;
            run_support_free(left);
            if (params.planar_below && planar_below())
                run_support_free(left);
            conflicts       = head_conflicts();
            st.relax_rounds = 0;
        } else
        for (int round = 0; round < 8; ++round) {
            solve_field();
            choose_layers();
            if (round == 0 && params.planar_below && planar_below()) {
                solve_field();
                choose_layers();
            }
            conflicts = head_conflicts();
            st.relax_rounds = round;
            if (conflicts.empty())
                break;
            F.notes.emplace_back("round " + std::to_string(round) + ": " + std::to_string(conflicts.size()) +
                                 " poses reach below the table; tilt relaxed there");
            relax(conflicts, round);
        }
        st.head_conflicts = conflicts.size();
        if (!st.solver_field && params.source == OrientationFieldSource::Diffuse)
            integrate_phi(1e-7);
        integrate_uv();
        F.compute_gradients();
        F.build_inverse();
        const Eval field = evaluate_field(true);
        st.overhang_area  = field.overhang_area;
        st.support_volume = field.support_volume;
        st.step_area      = field.step_area;
        st.nodes          = nodes();
        for (size_t i = 0; i < nodes(); ++i) {
            if (!F.m_inside[i])
                continue;
            const Vec3d  g = F.m_grad_phi[i].cast<double>();
            const double l = g.norm();
            if (l < 1e-9)
                continue;
            st.max_tilt      = std::max(st.max_tilt, std::acos(std::clamp(g.z() / l, -1., 1.)));
            st.min_thickness = std::min(st.min_thickness, 1. / l);
            st.max_thickness = std::max(st.max_thickness, 1. / l);
        }
    }
};

std::shared_ptr<OrientationField> OrientationField::build(const indexed_triangle_set &mesh, const OrientationFieldParams &params,
                                                          const std::function<void()> &throw_if_canceled)
{
    auto field = std::make_shared<OrientationField>();
    if (mesh.indices.empty())
        return field;
    OrientationFieldBuilder builder(*field, mesh, params, throw_if_canceled);
    builder.run();
    return field;
}

Vec3d settle_axis(const Vec3d &axis, double &plane, double none, double full)
{
    const double tilt = std::acos(std::clamp(axis.z(), -1., 1.));
    if (tilt >= full) {
        plane = std::atan2(axis.y(), axis.x());
        return axis;
    }
    const Vec2d u(std::cos(plane), std::sin(plane));
    const Vec3d in_plane = Vec3d(u.x() * u.dot(axis.head<2>()), u.y() * u.dot(axis.head<2>()), axis.z()).normalized();
    const double s = std::clamp((tilt - none) / (full - none), 0., 1.);
    const double w = s * s * (3. - 2. * s);
    const Vec3d  out = ((1. - w) * in_plane + w * axis).normalized();
    if (w > 0. && out.head<2>().squaredNorm() > 1e-6)
        plane = std::atan2(out.y(), out.x());
    return out;
}

static double field_tilt_budget(const ToolKinematics &kinematics)
{
    double tilt = 0.5 * M_PI;
    for (int s = 0; s < 72; ++s)
        tilt = std::min(tilt, kinematics.max_tilt(2. * M_PI * s / 72.));
    return tilt;
}

bool field_motion_supported(const PrintConfig &config, std::string *why)
{
    const std::unique_ptr<ToolKinematics> kinematics = make_tool_kinematics(config);
    auto fail = [why](const std::string &reason) {
        if (why != nullptr)
            *why = reason;
        return false;
    };
    if (kinematics == nullptr || kinematics->rotary_count() < 2)
        return fail(_u8L("Rotary curved layers need a printer with two rotary axes."));
    if (!kinematics->inverse_time_feed() && !kinematics->firmware_rtcp())
        return fail(_u8L("Rotary curved layers need continuous rotary motion — turn on inverse-time feed (G93) or firmware RTCP in your printer settings."));
    const double budget = field_tilt_budget(*kinematics);
    if (budget < Geometry::deg2rad(10.)) {
        double best = 0.;
        for (int s = 0; s < 72; ++s)
            best = std::max(best, kinematics->max_tilt(2. * M_PI * s / 72.));
        return fail(format(_u8L("Rotary curved layers need at least 10° of tilt all around, but your rotary limits only give %1%° in the tightest spot (%2%° in the widest)."),
                           int(std::lround(Geometry::rad2deg(budget))), int(std::lround(Geometry::rad2deg(best)))));
    }
    return true;
}

namespace {

bool field_requested(PrintObject &object)
{
    const NonplanarMode mode = object.config().nonplanar_mode.value;
    if (!object.config().nonplanar_atom_toolpath.value.empty())
        return false;
    return mode == NonplanarMode::Field || (mode == NonplanarMode::OneShot && object.oneshot_state().field());
}

OrientationFieldParams field_params(const PrintObject &object, const ToolKinematics &kinematics)
{
    OrientationFieldParams params;
    params.layer_height       = object.config().layer_height.value;
    params.first_layer_height = object.print()->config().initial_layer_print_height.value;
    params.overhang_threshold = Geometry::deg2rad(object.config().multiplanar_overhang_threshold.value);
    params.max_tilt   = std::max(0., field_tilt_budget(kinematics) - Geometry::deg2rad(5.));
    params.head       = resolve_head_envelope(object.print()->config());
    params.kinematics = &kinematics;
    return params;
}

std::shared_ptr<const NpLayerField> given_field(PrintObject &object, const indexed_triangle_set &mesh, double max_tilt)
{
    if (object.config().nonplanar_mode.value == NonplanarMode::OneShot) {
        solve_oneshot(object);
        NpField f = *object.oneshot_state().solved;
        return std::make_shared<const NpLayerField>(std::move(f));
    }
    const std::string &path = object.config().nonplanar_field_file.value;
    if (path.empty())
        return {};
    try {
        BoundingBoxf3 box;
        for (const stl_vertex &v : mesh.vertices)
            box.merge(v.cast<double>());
        return std::make_shared<const NpLayerField>(load_npfield_for(path, box, plate_offset(object)));
    } catch (const RuntimeError &ex) {
        throw Slic3r::SlicingError(ex.what());
    }
}

}

void solve_oneshot(PrintObject &object)
{
    OneShotState &state = object.oneshot_state();
    if (state.solved || take_injected_oneshot(state))
        return;
    auto solve = [&object, &state]() {
        const std::unique_ptr<ToolKinematics> kinematics = make_tool_kinematics(object.print()->config());
        const double max_tilt = std::max(0., field_tilt_budget(*kinematics) - Geometry::deg2rad(5.));
        OneShotAbortReason   abort;
        NpField              field;
        std::vector<NpField> time_cut;
        const bool           anchor = state.measured_anchor || state.offer == OneShotState::Offer::Control;
        if (state.raw && state.raw->cap_scale == state.cap_scale && state.raw->anchor == anchor) {
            field    = state.raw->field;
            time_cut = state.raw->time_cut;
        } else try {
            field = one_shot_field(object, slicing_frame_mesh(object), max_tilt, [&object]() { throw_if_canceled(object); }, state.student,
                                   state.polish_k, abort, &time_cut);
            if (abort.kind == OneShotAbort::None)
                state.raw = std::make_shared<const OneShotState::RawSolve>(OneShotState::RawSolve{ field, time_cut, state.cap_scale, anchor });
        } catch (const std::bad_alloc &) {
            throw Slic3r::SlicingError(Slic3r::format(_u8L("One-shot field: %1% is too large for the solve (out of memory)."),
                                                      object.model_object()->name));
        }
        if (abort.kind == OneShotAbort::Infeasible)
            throw Slic3r::SlicingError(Slic3r::format(_u8L("One-shot field: the solve of %1% returned a field outside its own limits (%2%): a solver bug."),
                                                      object.model_object()->name, abort.what));
        if (abort.kind != OneShotAbort::None)
            throw Slic3r::SlicingError(Slic3r::format(_u8L("One-shot field: the solve of %1% was stopped by its safety cap (%2%): a solver bug."),
                                                      object.model_object()->name, abort.what));
        state.solved                  = std::make_shared<const NpField>(std::move(field));
        state.z_only                  = in_plane_spread(*state.solved) < OneShotState::ZOnlySpread;
        state.max_time_ratio_override = max_time_ratio_override();
        state.time_cut_note.clear();
        state.auto_weighed = false;
        state.route.reset();
        state.plinth_layers = 0;
        admit_lean_offer(object, state, time_cut);
        admit_time_cut(object, state, time_cut);
        if (state.offer == OneShotState::Offer::Auto)
            return;
        if (state.offer != OneShotState::Offer::Control)
            choose_route(object, state);
    };
    solve();
    measured_export_check(object, state, [&state, &solve]() {
        state.solved.reset();
        solve();
    });
}

std::shared_ptr<const OrientationField> clip_field_slices(PrintObject &object)
{
    if (!field_requested(object))
        return {};
    if (!field_motion_supported(object.print()->config()))
        return {};
    const std::unique_ptr<ToolKinematics> kinematics = make_tool_kinematics(object.print()->config());
    OrientationFieldParams       params = field_params(object, *kinematics);
    const indexed_triangle_set   mesh   = slicing_frame_mesh(object);
    params.given                        = given_field(object, mesh, params.max_tilt);
    std::shared_ptr<OrientationField> field = OrientationField::build(mesh, params, [&object]() { throw_if_canceled(object); });
    if (params.given) {
        const NpField &f = params.given->file();
        const auto     t = std::find_if(f.terms.begin(), f.terms.end(), [](const auto &kv) { return kv.first == "stat_top_shells"; });
        field->given_shells = t != f.terms.end() && t->second > 0.5 && in_plane_spread(f) >= OneShotState::ZOnlySpread;
        if (field->given_shells)
            field->notes.emplace_back("curved top shells (the field's stat_top_shells)");
    }
    const OrientationFieldStats &st = field->stats();
    for (const std::string &note : field->notes)
        BOOST_LOG_TRIVIAL(info) << "Orientation field " << object.model_object()->name << ": " << note;
    BOOST_LOG_TRIVIAL(info) << "Orientation field " << object.model_object()->name << ": " << st.nodes << " nodes, overhang "
                            << st.overhang_area << " mm2 (planar " << st.planar_overhang_area << "), support volume "
                            << st.support_volume << " mm3 (planar " << st.planar_support_volume << "), stair-step area "
                            << st.step_area << " mm2 (planar " << st.planar_step_area << "), max tilt "
                            << Geometry::rad2deg(st.max_tilt) << " deg, layer thickness " << st.min_thickness << ".."
                            << st.max_thickness << " h, head conflicts " << st.head_conflicts << ", relax rounds "
                            << st.relax_rounds << ", tilt scale " << st.tilt_scale << ", layers from " << (st.solver_field ? "the layer solver" : "the own integral");
    if (st.head_conflicts > 0)
        throw Slic3r::SlicingError(Slic3r::format(_u8L("Rotary curved layers: %1% poses still reach below the table "
                                                       "after relaxing the tilt. Use Multiplanar or Auto instead."),
                                                  st.head_conflicts));
    return field;
}

namespace {

struct FieldSlice
{
    std::vector<FieldRoad>  roads;
    std::vector<double>     top, height;
    std::vector<ExPolygons> internal;
    size_t                  unmapped = 0;
    double                  floating_mm = -1.;
};

FieldSlice slice_field(PrintObject &object, const OrientationField &field, const Layer &host, double raft, bool tilt_shells)
{
    FieldSlice   out;
    const double first = object.print()->config().initial_layer_print_height.value;
    DynamicPrintConfig config = chunk_print_config(object);
    config.set_deserialize_strict("initial_layer_print_height", object.print()->config().opt_serialize("initial_layer_print_height"));
    config.set_deserialize_strict("ironing_type", "no ironing");
    config.set_deserialize_strict("nonplanar_top_shells", field.given() && !field.given_shells ? std::string("0") :
                                                              object.printing_region(0).config().opt_serialize("nonplanar_top_shells"));
    config.set_deserialize_strict("nonplanar_kinematics", tilt_shells ? object.print()->config().opt_serialize("nonplanar_kinematics") : std::string("none"));

    const std::vector<double> knots = field.given() ? field.slice_knots() : std::vector<double>();
    const double              fl = field.first_layer(), lh = field.layer_height();
    auto remap = [&](double v, bool back) {
        const int j = int(std::floor((v - fl) / lh)) + 1;
        if (j < 1 || size_t(j) > knots.size())
            return v;
        const double lo = fl + (j - 1) * lh, hi = lo + lh, mid = 0.5 * (lo + hi), knot = std::clamp(knots[size_t(j - 1)], lo + 1e-4, hi - 1e-4);
        const double a = back ? mid : knot, b = back ? knot : mid;
        return v <= a ? lo + (v - lo) * (b - lo) / (a - lo) : b + (v - a) * (hi - b) / (hi - a);
    };
    auto to_field = [&](Vec3d q) {
        q.z() = remap(q.z(), true);
        return q;
    };
    indexed_triangle_set deformed = field.deform(slicing_frame_mesh(object), 0.75 * field.pitch());
    if (!knots.empty())
        for (stl_vertex &v : deformed.vertices)
            v.z() = float(remap(double(v.z()), false));
    Model        model;
    ModelObject *model_object = model.add_object();
    model_object->name        = object.model_object()->name + " field";
    model_object->add_volume(TriangleMesh(indexed_triangle_set(deformed)));
    model_object->add_instance();
    Print print;
    throw_if_canceled(object);
    const NestedFieldSlice nested;
    try {
        print.apply(model, config);
        const PrintBase *outer = object.print();
        print.set_status_callback([&print, outer](const PrintBase::SlicingStatus &) {
            if (outer->canceled())
                print.cancel();
        });
        print.process();
    } catch (const CanceledException &) {
        throw;
    } catch (const SlicingErrors &errors) {
        throw Slic3r::SlicingError(std::string(_u8L("Rotary curved layers: slicing failed: ")) + print.slicing_errors_message(errors));
    } catch (const std::exception &ex) {
        throw Slic3r::SlicingError(std::string(_u8L("Rotary curved layers: slicing failed: ")) + ex.what());
    }
    throw_if_canceled(object);
    if (print.objects().empty())
        return out;
    const PrintObject     &sliced = *print.objects().front();
    const ConformalResult &shells = sliced.conformal();
    {
        std::vector<ExPolygons> slices;
        std::vector<double>     heights;
        for (const Layer *layer : sliced.layers())
            slices.push_back(layer->lslices), heights.push_back(layer->height);
        out.floating_mm = wall_floating_mm(slices, heights, object.printing_region(0).config().wall_loops.value,
                                           object.config().line_width.get_abs_value(object.print()->config().nozzle_diameter.get_at(0)));
    }
    const Vec2d            center = unscale(sliced.center_offset());
    const double           step   = 0.5;
    auto map_road = [&](const ExtrusionPath &path, const std::vector<Vec3d> &dpts, const std::vector<float> &bead_h,
                        const std::vector<float> &bead_w, const std::vector<Vec3d> &bead_a, size_t layer) {
        std::vector<Vec3d> q, qa;
        std::vector<float> qh, qw;
        const bool         axes_given = !bead_a.empty();
        q.push_back(dpts.front());
        qh.push_back(bead_h.front());
        qw.push_back(bead_w.front());
        if (axes_given)
            qa.push_back(bead_a.front());
        for (size_t i = 1; i < dpts.size(); ++i) {
            const double l = (dpts[i] - dpts[i - 1]).norm();
            const int    n = std::max(1, int(std::ceil(l / step)));
            for (int s = 1; s <= n; ++s) {
                const double t = double(s) / n;
                q.push_back(dpts[i - 1] + (dpts[i] - dpts[i - 1]) * t);
                qh.push_back(float(bead_h[i - 1] + (bead_h[i] - bead_h[i - 1]) * t));
                qw.push_back(float(bead_w[i - 1] + (bead_w[i] - bead_w[i - 1]) * t));
                if (axes_given)
                    qa.push_back((bead_a[i - 1] + (bead_a[i] - bead_a[i - 1]) * t).normalized());
            }
        }
        std::vector<Vec3d> tips, dq, ta;
        std::vector<float> th, tw;
        auto flush = [&]() {
            if (tips.size() >= 2) {
                Polyline3          polyline;
                std::vector<float> heights(tips.size()), widths(tips.size());
                std::vector<Vec3f> axes(tips.size());
                for (size_t i = 0; i < tips.size(); ++i) {
                    const Matrix3d j     = field.jacobian(tips[i]);
                    const Vec3d    grad  = j.row(2).transpose();
                    const double   gnorm = std::max(grad.norm(), 1e-6);
                    Vec3d bottom;
                    const double span = field.given() && field.inverse(to_field(dq[i] - Vec3d(0., 0., th[i])), bottom) ?
                                            (tips[i] - bottom).dot(grad / gnorm) : -1.;
                    heights[i]        = float(span > 0. ? std::clamp(span, 0.5 * th[i], 1.6 * th[i]) : th[i] * std::clamp(1. / gnorm, 0.5, 1.6));
                    axes[i] = (ta.empty() ? field.orientation(tips[i]) : field.map_axis(tips[i], ta[i])).cast<float>();
                    const Vec3d t   = (dq[std::min(i + 1, dq.size() - 1)] - dq[i > 0 ? i - 1 : 0]);
                    double      fac = 1.;
                    if (t.head<2>().squaredNorm() > 1e-12 && std::abs(j.determinant()) > 1e-9) {
                        const Matrix3d ji  = j.inverse();
                        const Vec3d    tn  = Vec3d(t.x(), t.y(), 0.).normalized();
                        const Vec3d    lat = ji * Vec3d(-tn.y(), tn.x(), 0.);
                        const Vec3d    T   = (ji * tn).normalized();
                        const Vec3d    N   = grad / gnorm;
                        fac                = (lat - lat.dot(T) * T - lat.dot(N) * N).norm();
                    }
                    widths[i] = float(tw[i] * std::clamp(fac, 0.6, 1.6));
                    polyline.points.push_back(Point3::new_scale(tips[i].x(), tips[i].y(), tips[i].z() + raft - host.print_z));
                }
                const bool   inner = is_infill(path.role()) && path.role() != erTopSolidInfill;
                const double rate  = Geometry::deg2rad(inner ? 3. : 5.);
                const std::vector<Vec3f> exact = axes;
                for (int pass = 0; pass < 2; ++pass)
                    for (size_t k = 1; k < axes.size(); ++k) {
                        const size_t i = pass == 0 ? k : axes.size() - 1 - k, j = pass == 0 ? i - 1 : i + 1;
                        const Vec3d  a = axes[j].cast<double>(), b = axes[i].cast<double>();
                        const double allowed = rate * std::max((tips[i] - tips[j]).norm(), 1e-3);
                        const double angle   = std::acos(std::clamp(a.dot(b), -1., 1.));
                        if (angle > allowed)
                            axes[i] = (Eigen::Quaterniond::Identity().slerp(allowed / angle, Eigen::Quaterniond::FromTwoVectors(a, b)) * a).cast<float>();
                    }
                const double max_lag = Geometry::deg2rad(10.);
                for (size_t i = 0; i < axes.size(); ++i) {
                    const Vec3d  e = exact[i].cast<double>(), a = axes[i].cast<double>();
                    const double off = std::acos(std::clamp(e.dot(a), -1., 1.));
                    if (off > max_lag)
                        axes[i] = (Eigen::Quaterniond::Identity().slerp(1. - max_lag / off, Eigen::Quaterniond::FromTwoVectors(a, e)) * a).cast<float>();
                }
                FieldRoad road;
                road.path             = std::make_unique<ExtrusionPathConformal>(std::move(polyline), path, std::move(heights), std::move(axes));
                road.path->bead_width = std::move(widths);
                road.tip              = std::move(tips);
                road.deformed         = std::move(dq);
                road.layer            = layer;
                out.roads.push_back(std::move(road));
            }
            tips.clear();
            dq.clear();
            ta.clear();
            th.clear();
            tw.clear();
        };
        for (size_t i = 0; i < q.size(); ++i) {
            Vec3d p;
            if (field.inverse(to_field(q[i]), p)) {
                tips.push_back(p);
                dq.push_back(q[i]);
                th.push_back(qh[i]);
                tw.push_back(qw[i]);
                if (axes_given)
                    ta.push_back(qa[i]);
            } else {
                ++out.unmapped;
                flush();
            }
        }
        flush();
    };
    auto deformed_points = [&](const ExtrusionPath &path, bool reversed, double z0, std::vector<Vec3d> &dpts, std::vector<float> &h,
                               std::vector<float> &w, std::vector<Vec3d> &a, const ConformalPatch *shell_of = nullptr) {
        const Points3 &src  = path.polyline.points;
        const auto    *conf = dynamic_cast<const ExtrusionPathConformal *>(&path);
        dpts.clear();
        h.clear();
        w.clear();
        a.clear();
        for (size_t i = 0; i < src.size(); ++i) {
            const size_t  k  = reversed ? src.size() - 1 - i : i;
            const Point3 &pt = src[k];
            dpts.emplace_back(unscale<double>(pt.x()) + center.x(), unscale<double>(pt.y()) + center.y(), z0 + unscale<double>(pt.z()));
            h.push_back(conf != nullptr && k < conf->bead_height.size() ? conf->bead_height[k] : path.height);
            w.push_back(conf != nullptr && k < conf->bead_width.size() ? conf->bead_width[k] : path.width);
            if (tilt_shells && conf != nullptr && k < conf->tool_axis.size())
                a.push_back(conf->tool_axis[k].cast<double>());
            else if (tilt_shells && shell_of != nullptr && !shell_of->surface.empty()) {
                const Vec2d g = shell_of->surface.gradient(unscale(pt.to_point()));
                const Vec3d n = std::isfinite(g.x()) && std::isfinite(g.y()) ? Vec3d(Vec3d(-g.x(), -g.y(), 1.).normalized()) : Vec3d(Vec3d::UnitZ());
                a.push_back(n);
                h.back() *= float(n.z());
            }
        }
        if (!a.empty() && a.size() != dpts.size())
            a.clear();
    };

    std::vector<Vec3d> dpts, ba;
    std::vector<float> bh, bw;
    for (const Layer *layer : sliced.layers()) {
        if (layer->print_z <= std::max(first, field.identity_below()) + EPSILON)
            continue;
        throw_if_canceled(object);
        const size_t k = out.top.size();
        out.top.push_back(layer->print_z);
        out.height.push_back(layer->height);
        ExPolygons internal;
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->get_slices().surfaces)
                if (surface.is_internal())
                    internal.push_back(surface.expolygon);
        for (ExPolygon &e : internal)
            e.translate(Point::new_scale(center.x(), center.y()));
        out.internal.push_back(union_ex(internal));
        auto shell_roads = [&](const std::vector<int> &ids) {
            for (int id : ids) {
                if (id < 0 || size_t(id) >= shells.patches.size())
                    continue;
                for (const ExtrusionEntityCollection &shell : shells.patches[size_t(id)].shell_paths) {
                    std::vector<const ExtrusionPath *> paths;
                    collect_paths(&shell, paths);
                    for (const ExtrusionPath *path : paths)
                        if (path->polyline.points.size() >= 2 && path->role() != erIroning) {
                            deformed_points(*path, false, layer->print_z, dpts, bh, bw, ba, &shells.patches[size_t(id)]);
                            const size_t first_road = out.roads.size();
                            map_road(*path, dpts, bh, bw, ba, k);
                            for (size_t r = first_road; r < out.roads.size(); ++r) {
                                out.roads[r].shell     = path->role() == erTopSolidInfill || path->role() == erIroning;
                                out.roads[r].by_height = !out.roads[r].shell;
                            }
                        }
                }
            }
        };
        shell_roads(layer->conformal_patches_before);
        std::vector<const ExtrusionPath *> walls, fills;
        for (const LayerRegion *region : layer->regions()) {
            collect_paths(&region->perimeters, walls);
            collect_paths(&region->fills, fills);
        }
        std::vector<std::pair<const ExtrusionPath *, bool>> order;
        for (const ExtrusionPath *p : walls)
            order.emplace_back(p, false);
        {
            std::vector<char> used(fills.size(), 0);
            Point             at = walls.empty() ? Point(0, 0) : walls.back()->last_point();
            for (size_t n = 0; n < fills.size(); ++n) {
                size_t best = 0;
                bool   rev  = false;
                double bd   = std::numeric_limits<double>::max();
                for (size_t i = 0; i < fills.size(); ++i) {
                    if (used[i] || fills[i]->polyline.points.size() < 2)
                        continue;
                    const double d0 = (fills[i]->first_point() - at).cast<double>().squaredNorm();
                    const double d1 = (fills[i]->last_point() - at).cast<double>().squaredNorm();
                    if (d0 < bd) { bd = d0; best = i; rev = false; }
                    if (fills[i]->can_reverse() && d1 < bd) { bd = d1; best = i; rev = true; }
                }
                if (bd == std::numeric_limits<double>::max())
                    break;
                used[best] = 1;
                order.emplace_back(fills[best], rev);
                at = rev ? fills[best]->first_point() : fills[best]->last_point();
            }
        }
        for (const auto &[path, reversed] : order)
            if (path->polyline.points.size() >= 2) {
                deformed_points(*path, reversed, layer->print_z, dpts, bh, bw, ba);
                map_road(*path, dpts, bh, bw, ba, k);
            }
        shell_roads(layer->conformal_patches_after);
    }
    return out;
}

void simplify_field_road(ExtrusionPathConformal &path, double tol, double angle)
{
    const Points3 &pts = path.polyline.points;
    if (pts.size() < 3)
        return;
    const double      stol = scale_(tol), cos_angle = std::cos(angle);
    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack { { 0, pts.size() - 1 } };
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        const Vec3d  pa = pts[a].cast<double>(), ab = pts[b].cast<double>() - pa;
        const double len = ab.squaredNorm();
        size_t       worst = a;
        double       wd    = stol;
        for (size_t i = a + 1; i < b; ++i) {
            const double t  = len > 0. ? std::clamp((pts[i].cast<double>() - pa).dot(ab) / len, 0., 1.) : 0.;
            double       d  = (pa + t * ab - pts[i].cast<double>()).norm();
            d = std::max(d, std::abs(scale_(path.bead_height[i] - (1. - t) * path.bead_height[a] - t * path.bead_height[b])));
            if (!path.bead_width.empty())
                d = std::max(d, std::abs(scale_(path.bead_width[i] - (1. - t) * path.bead_width[a] - t * path.bead_width[b])));
            if (!path.tool_axis.empty()) {
                const Vec3f ax = ((1.f - float(t)) * path.tool_axis[a] + float(t) * path.tool_axis[b]).normalized();
                if (ax.dot(path.tool_axis[i]) < cos_angle)
                    d = std::numeric_limits<double>::max();
            }
            if (d > wd) {
                wd    = d;
                worst = i;
            }
        }
        if (worst != a) {
            keep[worst] = 1;
            stack.emplace_back(a, worst);
            stack.emplace_back(worst, b);
        }
    }
    auto pick = [&keep](auto &v) {
        size_t o = 0;
        for (size_t i = 0; i < v.size(); ++i)
            if (keep[i])
                v[o++] = v[i];
        v.resize(o);
    };
    pick(path.polyline.points);
    pick(path.bead_height);
    if (!path.bead_width.empty())
        pick(path.bead_width);
    if (!path.tool_axis.empty())
        pick(path.tool_axis);
}

FieldSupport field_support(const PrintObject &object, const OrientationField &field, const std::vector<Vec3d> &floating,
                           const Layer &host, double raft)
{
    FieldSupport out;
    const double h     = object.config().layer_height.value;
    const double first = object.print()->config().initial_layer_print_height.value;
    const double gap   = std::max(0., object.config().support_top_z_distance.value);
    const double w     = 1.125 * object.print()->config().nozzle_diameter.get_at(0);
    constexpr double Cell = 0.5;
    std::unordered_map<int64_t, double> under;
    auto key = [](int x, int y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); };
    for (const Vec3d &p : floating) {
        auto [it, fresh] = under.emplace(key(int(std::floor(p.x() / Cell)), int(std::floor(p.y() / Cell))), p.z() - h);
        it->second       = std::min(it->second, p.z() - h);
    }
    struct Column { int x, y; double base, top; };
    std::vector<Column> columns;
    std::unordered_map<int64_t, double> tops;
    for (const auto &[k, bottom] : under) {
        const int x = int(k >> 32), y = int(int32_t(uint32_t(k)));
        for (int b = -1; b <= 1; ++b)
            for (int a = -1; a <= 1; ++a) {
                auto [it, fresh] = tops.emplace(key(x + a, y + b), bottom - gap);
                it->second       = std::min(it->second, bottom - gap);
            }
    }
    for (const auto &[k, top] : tops) {
        const int   x = int(k >> 32), y = int(int32_t(uint32_t(k)));
        const Vec2d c((x + 0.5) * Cell, (y + 0.5) * Cell);
        double base = 0.;
        for (double z = top - h; z > first; z -= 0.5 * h)
            if (field.inside(Vec3d(c.x(), c.y(), z))) {
                base = z + gap;
                break;
            }
        bool beside = false;
        for (double z = base + h; z <= top - 1. && !beside; z += 1.)
            for (int a = 0; a < 8 && !beside; ++a)
                beside = field.inside(Vec3d(c.x() + 1.5 * std::cos(a * M_PI / 4.), c.y() + 1.5 * std::sin(a * M_PI / 4.), z));
        if (top - base >= 2. * h && !beside)
            columns.push_back({ x, y, base, top });
    }
    if (columns.empty())
        return out;
    double zmax = 0.;
    for (const Column &c : columns)
        zmax = std::max(zmax, c.top);
    auto square = [](int x, int y) {
        Polygon sq;
        sq.points = { Point::new_scale(x * Cell, y * Cell), Point::new_scale((x + 1) * Cell, y * Cell),
                      Point::new_scale((x + 1) * Cell, (y + 1) * Cell), Point::new_scale(x * Cell, (y + 1) * Cell) };
        return sq;
    };
    int layer = 0;
    for (double z = first; z <= zmax + 1e-6; z += h, ++layer) {
        const double lh = layer == 0 ? first : h;
        Polygons     sparse, dense;
        for (const Column &c : columns)
            if (z <= c.top + 1e-6 && z - lh >= c.base - 1e-6)
                (c.top - z < 2. * h ? dense : sparse).push_back(square(c.x, c.y));
        const size_t j = out.top.size();
        bool         any = false;
        auto hatch = [&](const Polygons &cells, double spacing, ExtrusionRole role) {
            if (cells.empty())
                return;
            const ExPolygons area = offset_ex(union_ex(cells), -scale_(0.5 * w));
            for (const ExPolygon &ex : area) {
                const BoundingBox bb = ex.contour.bounding_box();
                Polylines         lines;
                const bool        along_x = spacing > w + EPSILON || (layer & 1) != 0;
                const coord_t     step    = coord_t(scale_(spacing));
                for (coord_t t = (along_x ? bb.min.y() : bb.min.x()) + step / 2; t <= (along_x ? bb.max.y() : bb.max.x()); t += step)
                    lines.push_back(along_x ? Polyline(Point(bb.min.x() - step, t), Point(bb.max.x() + step, t)) :
                                              Polyline(Point(t, bb.min.y() - step), Point(t, bb.max.y() + step)));
                Polylines clipped = intersection_pl(lines, ex);
                for (size_t i = 0; i < clipped.size(); ++i) {
                    if (i & 1)
                        clipped[i].reverse();
                    if (clipped[i].length() < scale_(0.5))
                        continue;
                    const double        mm3 = lh * (w - lh * (1. - 0.25 * M_PI));
                    const ExtrusionPath base(role, mm3, float(w), float(lh));
                    FieldRoad           road;
                    Polyline3           polyline;
                    const Points &src = clipped[i].points;
                    for (size_t k = 0; k < src.size(); ++k) {
                        const int n = k == 0 ? 1 : std::max(1, int(std::ceil(unscale<double>((src[k] - src[k - 1]).cast<double>().norm()) / 0.5)));
                        for (int t = k == 0 ? 0 : 1; t <= (k == 0 ? 0 : n); ++t) {
                            const Vec2d q = k == 0 ? unscale(src[0]) : Vec2d(unscale(src[k - 1]) + (unscale(src[k]) - unscale(src[k - 1])) * (double(t) / n));
                            polyline.points.push_back(Point3::new_scale(q.x(), q.y(), z + raft - host.print_z));
                            road.tip.emplace_back(q.x(), q.y(), z);
                        }
                    }
                    const size_t n = polyline.points.size();
                    road.path      = std::make_unique<ExtrusionPathConformal>(std::move(polyline), base, std::vector<float>(n, float(lh)),
                                                                         std::vector<Vec3f>(n, Vec3f::UnitZ()));
                    road.deformed  = road.tip;
                    road.layer     = j;
                    out.roads.push_back(std::move(road));
                    any = true;
                }
            }
        };
        hatch(sparse, 2., erSupportMaterial);
        hatch(dense, w, erSupportMaterialInterface);
        if (any) {
            out.top.push_back(z);
            out.height.push_back(lh);
        }
    }
    return out;
}

}

void build_field_patch(PrintObject &object, const std::shared_ptr<const OrientationField> &field_in, ConformalResult &result)
{
    if (!field_in || object.layers().empty())
        return;
    const size_t host_id = object.layers().size() - 1;
    Layer       *host    = object.layers()[host_id];
    const double raft    = host->print_z - (host->slice_z + 0.5 * host->height);
    const std::unique_ptr<ToolKinematics> kinematics = make_tool_kinematics(object.print()->config());
    OrientationFieldParams                params     = field_params(object, *kinematics);
    std::shared_ptr<const OrientationField> field = field_in;

    FieldOrder              order;
    size_t                  unmapped = 0;
    std::vector<ExPolygons> internal;
    bool tilt_shells = !field->given();
    constexpr double RelaxBudget = 720.;
    const auto       t_start     = std::chrono::steady_clock::now();
    bool             untilted    = false;
    for (int round = 0;; ++round) {
        const auto       t_slice = std::chrono::steady_clock::now();
        FieldSlice       slice = slice_field(object, *field, *host, raft, tilt_shells);
        const auto       t_order = std::chrono::steady_clock::now();
        FieldOrderParams op;
        op.head         = params.head;
        op.kinematics   = kinematics.get();
        op.max_tilt     = std::max(0., field_tilt_budget(*kinematics) - Geometry::deg2rad(1.));
        if (field->given())
            op.lean_max = 0.;
        op.layer_top    = std::move(slice.top);
        op.layer_height = std::move(slice.height);
        unmapped        = slice.unmapped;
        internal        = std::move(slice.internal);
        object.set_field_floating_mm(slice.floating_mm);
        order           = field->given() ? layer_order_field_roads(slice.roads, op) :
                                           order_field_roads(slice.roads, *field, op,
                                                             [&](const std::vector<Vec3d> &floating) { return field_support(object, *field, floating, *host, raft); },
                                                             [&object]() { throw_if_canceled(object); });
        const FieldOrderStats &st = order.stats;
        const auto seconds = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
        BOOST_LOG_TRIVIAL(info) << "Orientation field " << object.model_object()->name << ": round " << round << " sliced in "
                                << seconds(t_slice, t_order) << " s, ordered in " << seconds(t_order, std::chrono::steady_clock::now())
                                << " s, " << st.split_passes << " split passes";
        BOOST_LOG_TRIVIAL(info) << "Orientation field " << object.model_object()->name << ": order round " << round << ", "
                                << st.pieces << " pieces, " << st.support_edges << " support and " << st.head_edges
                                << " head edges, " << st.groups << " groups over " << st.layers << " layers, " << st.cycle_pieces
                                << " pieces in cycles, " << st.table_poses << " table poses, " << st.floating_mm
                                << " mm of road floating (" << st.support_layers << " support layers, " << st.support_pieces
                                << " pieces), max tilt " << Geometry::rad2deg(field->max_tilt()) << " deg, " << st.leaned_points
                                << " points leaned off the layer normal, " << st.blocked_points << " still blocked, " << st.shell_roads
                                << " top shell roads after the volume";
        if (order.relax_points.empty() || untilted || field->given())
            break;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count() > RelaxBudget) {
            BOOST_LOG_TRIVIAL(warning) << "Orientation field " << object.model_object()->name << ": " << order.stats.cycle_pieces
                                       << " pieces still in cycles after " << round + 1 << " rounds; the tilt is dropped";
            params.tilt_scale = 0.;
            tilt_shells       = false;
            untilted          = true;
            field = OrientationField::build(slicing_frame_mesh(object), params, [&object]() { throw_if_canceled(object); });
            continue;
        }
        if (round >= 7) {
            if (!tilt_shells || order.stats.cycle_pieces == 0)
                break;
            tilt_shells = false;
            continue;
        }
        std::unordered_map<int64_t, char> seen;
        std::vector<Vec3d>                points, dirs;
        for (size_t i = 0; i < order.relax_points.size(); ++i) {
            const Vec3d &q   = order.relax_points[i];
            const Vec3d  u   = i < order.relax_dirs.size() ? order.relax_dirs[i] : Vec3d::Zero();
            const int    oct = u.squaredNorm() > 0. ? 1 + int(std::lround(4. * (std::atan2(u.y(), u.x()) / M_PI + 1.))) % 8 : 0;
            const int64_t key = ((int64_t(std::floor(q.x() / 2.)) * 100003 + int64_t(std::floor(q.y() / 2.))) * 100019 + int64_t(std::floor(q.z() / 2.))) * 9 + oct;
            if (seen.emplace(key, 1).second) {
                points.push_back(q);
                dirs.push_back(u);
            }
        }
        params.relax_rounds.push_back(std::move(points));
        params.relax_dirs.push_back(std::move(dirs));
        if (round >= 4)
            params.tilt_scale *= 0.5;
        if (round >= 6)
            params.tilt_scale = 0.;
        const auto t_field = std::chrono::steady_clock::now();
        field = OrientationField::build(slicing_frame_mesh(object), params, [&object]() { throw_if_canceled(object); });
        BOOST_LOG_TRIVIAL(info) << "Orientation field " << object.model_object()->name << ": field rebuilt in "
                                << std::chrono::duration<double>(std::chrono::steady_clock::now() - t_field).count() << " s, max tilt "
                                << Geometry::rad2deg(field->max_tilt()) << " deg";
        if (field->stats().head_conflicts > 0)
            throw Slic3r::SlicingError(Slic3r::format(_u8L("Rotary curved layers: %1% poses still reach below the table "
                                                           "after relaxing the tilt. Use Multiplanar or Auto instead."),
                                                      field->stats().head_conflicts));
    }
    if (unmapped > 0)
        BOOST_LOG_TRIVIAL(warning) << "Orientation field " << object.model_object()->name << ": " << unmapped
                                   << " road points mapped nowhere and were dropped";
    if (order.stats.floating_mm > 0.)
        BOOST_LOG_TRIVIAL(warning) << "Orientation field " << object.model_object()->name << ": " << order.stats.floating_mm
                                   << " mm of road with nothing under it in the layer below";

    ConformalPatch patch;
    patch.id           = int(result.patches.size());
    patch.kind         = PatchKind::Field;
    patch.shell_height = object.config().layer_height.value;
    patch.host_layer   = host_id;
    patch.region_id    = 0;
    patch.field        = field;
    patch.z_min        = std::numeric_limits<double>::infinity();
    patch.z_max        = -std::numeric_limits<double>::infinity();
    for (size_t g = 0; g < order.groups.size(); ++g) {
        ExtrusionEntityCollection &group = order.groups[g];
        if (g < order.group_layers.size() && order.group_layers[g] < internal.size())
            patch.shell_internal.push_back(std::move(internal[order.group_layers[g]]));
        for (ExtrusionEntity *ee : group.entities)
            simplify_field_road(*static_cast<ExtrusionPathConformal *>(ee), 0.01, Geometry::deg2rad(1.));
        for (const ExtrusionEntity *ee : group.entities)
            for (const Point3 &p : static_cast<const ExtrusionPath *>(ee)->polyline.points) {
                const double z = unscale<double>(p.z()) + host->print_z;
                patch.z_min    = std::min(patch.z_min, z);
                patch.z_max    = std::max(patch.z_max, z);
            }
        patch.shell_paths.push_back(std::move(group));
    }
    const double cut = std::max(field->identity_below(), object.print()->config().initial_layer_print_height.value);
    for (size_t layer_id = 1; layer_id < object.layers().size(); ++layer_id)
        if (Layer *layer = object.layers()[layer_id]; layer->print_z > cut + EPSILON)
            for (LayerRegion *region : layer->regions()) {
                region->slices.set(ExPolygons(), stInternal);
                region->perimeters.clear();
                region->thin_fills.clear();
                region->fills.clear();
                region->fill_surfaces.clear();
                region->fill_expolygons.clear();
            }
    if (patch.shell_paths.empty())
        return;
    for (size_t layer_id = 1; layer_id < object.layers().size(); ++layer_id)
        if (Layer *layer = object.layers()[layer_id]; layer->print_z > cut + EPSILON)
            layer->conformal_covered = true;
    patch.shells = int(patch.shell_paths.size());
    host->conformal_patches_after.push_back(patch.id);
    result.patches.push_back(std::move(patch));
}

}
