#include "FieldObjective.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "FieldTerm.hpp"
#include "Point.hpp"
#include "libslic3r.h"
#include "Nonplanar/HeadEnvelope.hpp"
#include "LocalesUtils.hpp"

#include <Eigen/Core>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <math.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <Eigen/SparseCore>
#include <functional>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <type_traits>
#include <vector>
#include <utility>

namespace Slic3r::Nonplanar {

constexpr double SmoothTimeMax = 1.03;

bool smooth_layers()
{
    static const bool on = [] { const char *v = research_switch("NP_SMOOTH_LAYERS"); return v == nullptr || *v != '0'; }();
    return on;
}

void cap_layer_change(std::vector<double> &t, double dz)
{
    auto grow = [dz](double t) {
        const double c = std::min(LayerChangeMax, LayerChangeAbs / std::max(t, 1e-6));
        return dz > 0. ? std::pow(1. + c, dz / std::max(t, 1e-6)) : 1. + c;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 1; i < t.size(); ++i)
            t[i] = std::min(t[i], t[i - 1] * grow(t[i - 1]));
        for (size_t i = t.size(); i-- > 1;)
            t[i - 1] = std::min(t[i - 1], t[i] * grow(t[i]));
    }
}

namespace {

const double Cos5       = std::cos(5. * M_PI / 180.);
const double Cos10      = std::cos(10. * M_PI / 180.);
const double ShellPlanarCos = std::cos(0.5 * M_PI / 180.);

constexpr double HuberDelta = 0.1;
constexpr double Irls       = 1e-3;
constexpr double Hard       = 1e4;
constexpr double Regular    = 1e-3;

constexpr double WallK = 0.31;
constexpr double WallC = 0.0213;
constexpr double TiltK = 1.0;
constexpr double CeilK = 1.0;
constexpr double SupportAllowance = 1.0;
constexpr double UpFloor = 0.011;
constexpr double ZOnlySpread = 0.05;
constexpr double PlanarTol = 1e-4;

double e_ref(const ObjectiveMachine &m) { return m.h * std::sin(20. * M_PI / 180.) / std::sqrt(12.); }

std::pair<double, double> grad_bounds(const ObjectiveMachine &m, size_t )
{
    return { m.h / m.thickness_max, m.h / m.thickness_min };
}

struct Ijk
{
    size_t i, j, k;
};
Ijk ijk_of(const ObjectiveGeometry &g, size_t id) { return { id / (g.dims[1] * g.dims[2]), (id / g.dims[2]) % g.dims[1], id % g.dims[2] }; }

size_t bed_layers(const ObjectiveGeometry &g, const ObjectiveMachine &m)
{
    const double planar = m.rotary ? m.first_layer : m.first_layer + 2. * m.h;
    return std::min(g.dims[2] - 1, size_t(std::floor(planar / g.spacing.z() + 1e-9)) + 1);
}

Vec3d node_pos(const ObjectiveGeometry &g, size_t id)
{
    const Ijk c = ijk_of(g, id);
    return g.origin + Vec3d(double(c.i), double(c.j), double(c.k)).cwiseProduct(g.spacing);
}

struct Coef
{
    size_t node;
    Vec3d  g;
    double v;
};

void node_gradient_stencil(const ObjectiveGeometry &g, size_t id, double scale, std::vector<Coef> &out)
{
    const Ijk    c         = ijk_of(g, id);
    const size_t at[3]     = { c.i, c.j, c.k };
    const size_t stride[3] = { g.dims[1] * g.dims[2], g.dims[2], 1 };
    for (int a = 0; a < 3; ++a) {
        const size_t n = g.dims[size_t(a)];
        if (n < 2)
            continue;
        Vec3d e = Vec3d::Zero();
        if (at[a] == 0) {
            e[a] = scale / g.spacing[a];
            out.push_back({ id + stride[a], e, 0. });
            out.push_back({ id, -e, 0. });
        } else if (at[a] + 1 == n) {
            e[a] = scale / g.spacing[a];
            out.push_back({ id, e, 0. });
            out.push_back({ id - stride[a], -e, 0. });
        } else {
            e[a] = 0.5 * scale / g.spacing[a];
            out.push_back({ id + stride[a], e, 0. });
            out.push_back({ id - stride[a], -e, 0. });
        }
    }
}

void corners(const ObjectiveGeometry &g, const Vec3d &p, size_t ids[8], double w[8])
{
    size_t c[3];
    double t[3];
    for (int a = 0; a < 3; ++a) {
        const double n = double(g.dims[size_t(a)]);
        const double x = std::clamp((p[a] - g.origin[a]) / g.spacing[a], 0., n - 1.);
        c[a]           = std::min(size_t(x), size_t(n) - 2);
        t[a]           = x - double(c[a]);
    }
    for (int q = 0; q < 8; ++q) {
        const size_t a = q & 1, b = (q >> 1) & 1, e = (q >> 2) & 1;
        ids[q] = g.index(c[0] + a, c[1] + b, c[2] + e);
        w[q]   = (a ? t[0] : 1. - t[0]) * (b ? t[1] : 1. - t[1]) * (e ? t[2] : 1. - t[2]);
    }
}

template<class F> auto interp(const ObjectiveGeometry &g, const Vec3d &p, F value)
{
    size_t ids[8];
    double w[8];
    corners(g, p, ids, w);
    std::decay_t<decltype(value(ids[0]))> v = value(ids[0]) * w[0];
    for (int q = 1; q < 8; ++q)
        v += value(ids[q]) * w[q];
    return v;
}

std::vector<Coef> point_stencil(const ObjectiveGeometry &g, const Vec3d &p)
{
    size_t ids[8];
    double w[8];
    corners(g, p, ids, w);
    std::vector<Coef> raw;
    for (int q = 0; q < 8; ++q) {
        if (w[q] == 0.)
            continue;
        node_gradient_stencil(g, ids[q], w[q], raw);
        raw.push_back({ ids[q], Vec3d::Zero(), w[q] });
    }
    std::sort(raw.begin(), raw.end(), [](const Coef &a, const Coef &b) { return a.node < b.node; });
    std::vector<Coef> out;
    for (const Coef &c : raw)
        if (!out.empty() && out.back().node == c.node) {
            out.back().g += c.g;
            out.back().v += c.v;
        } else
            out.push_back(c);
    return out;
}

std::vector<Vec3d> node_gradients(const ObjectiveGeometry &g, const std::vector<float> &phi)
{
    std::vector<Vec3d> out(g.nodes());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, out.size(), 4096), [&](const tbb::blocked_range<size_t> &range) {
        std::vector<Coef> st;
        for (size_t id = range.begin(); id < range.end(); ++id) {
            st.clear();
            node_gradient_stencil(g, id, 1., st);
            Vec3d v = Vec3d::Zero();
            for (const Coef &c : st)
                v += c.g * double(phi[c.node]);
            out[id] = v;
        }
    });
    return out;
}

double stair(const Vec3d &n, const Vec3d &gs, double ps, const ObjectiveMachine &m, bool &flat, double &r)
{
    const double gl = std::max(gs.norm(), 1e-12);
    const double nd = n.dot(gs) / gl;
    flat            = std::abs(nd) > Cos5;
    if (flat) {
        r = ps - m.first_layer - std::nearbyint((ps - m.first_layer) / m.h) * m.h;
        return std::abs(r) / gl;
    }
    r = 0.;
    return m.h / gl * std::abs(nd) / std::sqrt(12.);
}

bool walled(int8_t cls, double nd) { return cls == 1 || (cls == 2 && std::abs(nd) < std::cos(70. * M_PI / 180.)); }

double wall_error(int8_t cls, double e, double t, double nd, double dz)
{
    if (!walled(cls, nd))
        return e;
    const double flank = WallK * t / 2. * std::sqrt(std::max(0., 1. - nd * nd));
    const double tilt2 = cls == 1 ? TiltK * TiltK * t * t * std::max(0., 1. - dz * dz) : 0.;
    return std::sqrt(e * e + flank * flank + WallC * WallC + tilt2);
}

double up_floor(int8_t cls, double e) { return cls == 0 ? std::sqrt(e * e + UpFloor * UpFloor) : e; }

double covered(const ObjectiveGeometry &g, size_t s, double e, const ObjectiveMachine &m, const ObjectiveCover &c, bool &skinned, double dz)
{
    skinned = false;
    const Vec3d &n = g.n[s];
    if (c.shell_rms >= 0. && g.cls[s] == 0 && std::acos(std::clamp(n.z(), -1., 1.)) <= c.shell_slope_cap &&
        (m.rotary || dz >= ShellPlanarCos))
        e = std::min(e, c.shell_rms);
    if (c.skin_rms >= 0. && c.skin_rms < e) {
        const double tilt = std::acos(std::clamp(n.z(), -1., 1.));
        if (c.skin_classes ? (c.skin_classes >> g.cls[s]) & 1 : tilt >= c.skin_tilt_lo && tilt <= c.skin_tilt_hi && n.z() >= c.skin_min_nz) {
            e       = c.skin_rms;
            skinned = true;
        }
    }
    return e;
}

double class_weight(const ObjectiveWeights &w, int8_t c) { return c == 0 ? w.up : c == 1 ? w.wall : w.down; }

Matrix3d rotation_to(const Vec3d &axis)
{
    const Vec3d  v = Vec3d::UnitZ().cross(axis);
    const double c = axis.z();
    if (c <= -1. + 1e-9)
        return Vec3d(1., -1., -1.).asDiagonal();
    Matrix3d K;
    K << 0., -v.z(), v.y(), v.z(), 0., -v.x(), -v.y(), v.x(), 0.;
    return Matrix3d::Identity() + K + K * K * (1. / (1. + c));
}

std::vector<uint8_t> near_material(const ObjectiveGeometry &g)
{
    std::vector<uint8_t> out = g.occ;
    const size_t stride[3] = { g.dims[1] * g.dims[2], g.dims[2], 1 };
    for (size_t id = 0; id < g.nodes(); ++id) {
        if (!g.occ[id])
            continue;
        const Ijk    c     = ijk_of(g, id);
        const size_t at[3] = { c.i, c.j, c.k };
        for (int a = 0; a < 3; ++a) {
            if (at[a] > 0)
                out[id - stride[a]] = 1;
            if (at[a] + 1 < g.dims[size_t(a)])
                out[id + stride[a]] = 1;
        }
    }
    return out;
}

double slope_cap_at(const ObjectiveMachine &m, const Vec3d &gr)
{
    const double az = std::fmod(std::atan2(-gr.y(), -gr.x()) * 180. / M_PI + 360., 360.);
    const double x  = az / 15.;
    const size_t a  = std::min<size_t>(size_t(x), 23);
    const double t  = x - double(a);
    return (1. - t) * m.slope_cap[a] + t * m.slope_cap[(a + 1) % 24];
}

constexpr double AxisAccel             = 1.815;
constexpr double TravelS               = 0.06;
constexpr double TravelPerIslandLayer  = 3.72;
constexpr double TravelPerMRoad        = 19.7;
constexpr double PieceMm               = 5.;
const double     Sin10                 = std::sin(10. * M_PI / 180.);
const double     Tilted                = 2. * M_PI / 180.;


std::vector<double> inside_distance(const ObjectiveGeometry &g)
{
    const size_t P[3] = { g.dims[0] + 2, g.dims[1] + 2, g.dims[2] + 2 };
    auto         at   = [&P](size_t i, size_t j, size_t k) { return (i * P[1] + j) * P[2] + k; };
    const double big  = 1e20;
    std::vector<double> f(P[0] * P[1] * P[2], 0.);
    for (size_t i = 0; i < g.dims[0]; ++i)
        for (size_t j = 0; j < g.dims[1]; ++j)
            for (size_t k = 0; k < g.dims[2]; ++k)
                f[at(i + 1, j + 1, k + 1)] = g.occ[g.index(i, j, k)] ? big : 0.;
    std::vector<double> line, out_line, z;
    std::vector<size_t> v;
    for (int axis = 0; axis < 3; ++axis) {
        const size_t n = P[axis];
        const double s2 = g.spacing[axis] * g.spacing[axis];
        line.resize(n), out_line.resize(n), v.resize(n), z.resize(n + 1);
        const int    o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
        for (size_t a1 = 0; a1 < P[o1]; ++a1)
            for (size_t a2 = 0; a2 < P[o2]; ++a2) {
                auto idx = [&](size_t q) {
                    size_t c[3];
                    c[axis] = q, c[o1] = a1, c[o2] = a2;
                    return at(c[0], c[1], c[2]);
                };
                for (size_t q = 0; q < n; ++q)
                    line[q] = f[idx(q)];
                size_t kk = 0;
                v[0] = 0, z[0] = -big, z[1] = big;
                for (size_t q = 1; q < n; ++q) {
                    double sx;
                    while (true) {
                        const size_t r = v[kk];
                        sx = ((line[q] + s2 * double(q) * double(q)) - (line[r] + s2 * double(r) * double(r))) / (2. * s2 * (double(q) - double(r)));
                        if (sx <= z[kk] && kk > 0)
                            --kk;
                        else
                            break;
                    }
                    if (sx <= z[kk]) {
                        v[0] = q, z[0] = -big, z[1] = big;
                        continue;
                    }
                    ++kk;
                    v[kk] = q, z[kk] = sx, z[kk + 1] = big;
                }
                kk = 0;
                for (size_t q = 0; q < n; ++q) {
                    while (z[kk + 1] < double(q))
                        ++kk;
                    const double d = double(q) - double(v[kk]);
                    out_line[q]    = s2 * d * d + line[v[kk]];
                }
                for (size_t q = 0; q < n; ++q)
                    f[idx(q)] = out_line[q];
            }
    }
    std::vector<double> out(g.nodes());
    for (size_t i = 0; i < g.dims[0]; ++i)
        for (size_t j = 0; j < g.dims[1]; ++j)
            for (size_t k = 0; k < g.dims[2]; ++k)
                out[g.index(i, j, k)] = std::sqrt(f[at(i + 1, j + 1, k + 1)]);
    return out;
}

Vec3d field_gradient(const ObjectiveGeometry &g, const std::vector<double> &f, size_t id)
{
    std::vector<Coef> st;
    node_gradient_stencil(g, id, 1., st);
    Vec3d v = Vec3d::Zero();
    for (const Coef &c : st)
        v += c.g * f[c.node];
    return v;
}

struct Roads
{
    Vec3d  u[2];
    double weight[2];
    int    count = 0;
};

Roads road_directions(const ObjectiveGeometry &g, size_t id, const Vec3d &D, const std::vector<double> &inside)
{
    Roads r;
    if (g.shell[id]) {
        const Vec3d w = D.cross(field_gradient(g, inside, id));
        if (w.norm() > 1e-6) {
            r.u[0] = w.normalized(), r.weight[0] = 1., r.count = 1;
            return r;
        }
    }
    const Vec3d ex = std::abs(D.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    const Vec3d e1 = (ex - ex.dot(D) * D).normalized(), e2 = D.cross(e1);
    r.u[0] = (e1 + e2) / std::sqrt(2.), r.u[1] = (e1 - e2) / std::sqrt(2.);
    r.weight[0] = r.weight[1] = 0.5, r.count = 2;
    return r;
}

struct Turn
{
    Vec3d  du;
    double dB, dC;
    Vec3d  dB_du, dC_du;
};

Turn turn_along(const Vec3d &D, const Vec3d &du)
{
    Turn         t;
    t.du         = du;
    const double s = std::hypot(D.x(), D.y());
    const double h = std::hypot(du.x(), du.y());
    if (s > 1e-3) {
        t.dB    = -du.z() / s;
        t.dB_du = Vec3d(0., 0., -1. / s);
        const double cross = D.x() * du.y() - D.y() * du.x();
        t.dC    = std::abs(cross) / s / std::max(s, Sin10);
        t.dC_du = (cross >= 0. ? 1. : -1.) * Vec3d(-D.y(), D.x(), 0.) / s / std::max(s, Sin10);
    } else {
        t.dB    = h;
        t.dB_du = h > 1e-12 ? Vec3d(du.x() / h, du.y() / h, 0.) : Vec3d(Vec3d::Zero());
        t.dC    = h / std::max(s, Sin10);
        t.dC_du = t.dB_du / std::max(s, Sin10);
    }
    return t;
}

Vec3d du_at(const ObjectiveGeometry &g, size_t id, const Vec3d &u, const std::vector<Vec3d> &G,
            std::vector<std::pair<size_t, double>> *coef = nullptr, const Vec3d &w = Vec3d::Zero(),
            const std::vector<std::vector<Coef>> *node_st = nullptr)
{
    std::vector<Coef> st;
    node_gradient_stencil(g, id, 1., st);
    Vec3d du = Vec3d::Zero();
    for (const Coef &c : st) {
        const double alpha = c.g.dot(u);
        if (alpha == 0.)
            continue;
        const double gn = std::max(G[c.node].norm(), 1e-12);
        const Vec3d  dn = G[c.node] / gn;
        du += alpha * dn;
        if (coef) {
            const Vec3d v = alpha * (w - w.dot(dn) * dn) / gn;
            for (const Coef &e : (*node_st)[c.node])
                coef->emplace_back(e.node, v.dot(e.g));
        }
    }
    return du;
}

}

std::vector<std::pair<std::string, double>> ObjectiveTerms::named() const
{
    std::vector<std::pair<std::string, double>> out = { { "J", J },
             { "J_S", S },
             { "J_E", E },
             { "J_T", T },
             { "J_E_up", E_up },
             { "J_E_wall", E_wall },
             { "J_E_down", E_down },
             { "J_T_roads", T_roads },
             { "J_T_minlayer", T_minlayer },
             { "J_T_travel", T_travel },
             { "J_T_axis", T_axis },
             { "J_T_unwind", T_unwind },
             { "J_T_skin", T_skin },
             { "stat_n_travel", n_travel },
             { "stat_hop_mm", hop_mm },
             { "stat_road_pieces", road_pieces },
             { "stat_S_smooth", S_smooth },
             { "stat_rms", rms },
             { "stat_rms_up", rms_up },
             { "stat_rms_wall", rms_wall },
             { "stat_rms_down", rms_down },
             { "stat_island_layers", island_layers },
             { "stat_layers", layers },
             { "stat_H_thickness", double(H_thickness) },
             { "stat_H_slope", double(H_slope) },
             { "stat_H_head", double(H_head) },
             { "stat_H_bed", double(H_bed) },
             { "stat_H_time", double(H_time) }, { "stat_T_ratio", T_ratio }, { "stat_T_cap", T_cap }, { "stat_spread", spread },
             { "stat_F_wallp", F_wallp }, { "stat_F_upp", F_upp }, { "stat_F_flatx", F_flatx }, { "stat_Ti", Ti }, { "stat_F_first", F_first },
             { "stat_F_wallt", F_wallt }, { "stat_F_upt", F_upt }, { "stat_Li", Li }, { "stat_F_seg", F_seg },
             { "stat_F_stair", F_stair }, { "stat_E_stair", E_stair } };
    out.insert(out.end(), offers.begin(), offers.end());
    out.emplace_back("stat_infill_separate", infill_separate ? 1. : 0.);
    for (const auto &[name, c] : field_terms)
        out.emplace_back("J_term_" + name, c);
    return out;
}

double TimeModel::operator()(const ObjectiveTerms &t) const
{
    return c0 + T * t.T + F_wallp * t.F_wallp + F_upp * t.F_upp + F_downp * t.F_downp + F_flatx * t.F_flatx + F_shells_top * t.F_shells_top +
           Ti * t.Ti + layers * t.layers + island_layers * t.island_layers + T_minlayer * t.T_minlayer + F_first * t.F_first +
           T_axis * t.T_axis + T_unwind * t.T_unwind + road_pieces * t.road_pieces + hop_mm * t.hop_mm + F_wallt * t.F_wallt +
           F_upt * t.F_upt + Li * t.Li + F_seg * t.F_seg;
}

std::vector<HeadSamples> head_points(const HeadEnvelope &head, double pitch, double clip, double dilate)
{
    std::vector<std::pair<Vec3d, Vec3d>> boxes;
    for (HeadBox b : head.boxes) {
        const Vec2d radius = b.min.head<2>().cwiseAbs().cwiseMax(b.max.head<2>().cwiseAbs());
        if (!b.nozzle) {
            b.min.head<2>().array() -= head.lateral_margin + dilate;
            b.max.head<2>().array() += head.lateral_margin + dilate;
            b.min.z() -= head.margin + dilate;
            b.max.z() += head.margin + dilate;
        } else {
            if (!b.infinite_x && !b.infinite_y && radius.norm() <= head.tip_allowance.radius)
                b.min.z() = std::max(b.min.z(), head.tip_allowance.height);
            b.min.head<2>().array() -= dilate;
            b.max.head<2>().array() += dilate;
            b.max.z() += dilate;
        }
        if (b.infinite_x)
            b.min.x() = -clip, b.max.x() = clip;
        if (b.infinite_y)
            b.min.y() = -clip, b.max.y() = clip;
        b.min = b.min.cwiseMax(Vec3d::Constant(-clip));
        b.max = b.max.cwiseMin(Vec3d::Constant(clip));
        if ((b.max.array() > b.min.array()).all())
            boxes.emplace_back(b.min, b.max);
    }
    auto covered = [&boxes](const Vec3d &q) {
        return std::any_of(boxes.begin(), boxes.end(), [&q](const auto &b) {
            return (q.array() >= b.first.array()).all() && (q.array() <= b.second.array()).all();
        });
    };
    auto interior = [&covered](Vec3d q) {
        for (int a = 0; a < 3; ++a)
            for (double e : { -1e-6, 1e-6 }) {
                q[a] += e;
                const bool in = covered(q);
                q[a] -= e;
                if (!in)
                    return false;
            }
        return true;
    };
    std::vector<HeadSamples> out;
    for (const auto &[lo, hi] : boxes) {
        const Vec3d   ext = hi - lo;
        const Vec3i32 n((ext / pitch).array().ceil().max(1.).cast<int>());
        for (int a = 0; a < 3; ++a) {
            const int u = (a + 1) % 3, v = (a + 2) % 3;
            for (int side = 0; side < 2; ++side)
                for (int i0 = 0; i0 <= n[u]; i0 += 8)
                    for (int j0 = 0; j0 <= n[v]; j0 += 8) {
                        HeadSamples hs;
                        for (int i = i0; i <= std::min(i0 + 7, n[u]); ++i)
                            for (int j = j0; j <= std::min(j0 + 7, n[v]); ++j) {
                                Vec3d q;
                                q[a] = side ? hi[a] : lo[a];
                                q[u] = lo[u] + ext[u] * i / n[u];
                                q[v] = lo[v] + ext[v] * j / n[v];
                                if (!interior(q))
                                    hs.points.push_back(q);
                            }
                        if (hs.points.empty())
                            continue;
                        Vec3d plo = hs.points.front(), phi = plo;
                        for (const Vec3d &q : hs.points)
                            plo = plo.cwiseMin(q), phi = phi.cwiseMax(q);
                        hs.centre = 0.5 * (plo + phi);
                        for (const Vec3d &q : hs.points)
                            hs.radius = std::max(hs.radius, (q - hs.centre).norm());
                        out.push_back(std::move(hs));
                    }
        }
    }
    return out;
}


namespace {

double support_volume(const ObjectiveGeometry &g, const ObjectiveMachine &m)
{
    const double sin_t = std::sin(m.printable_overhang);
    double       volume = 0.;
    for (size_t s = 0; s < g.p.size(); ++s) {
        const Vec3d &p = g.p[s], &n = g.n[s];
        if (!(-n.z() > sin_t))
            continue;
        const size_t i = size_t(std::clamp(long(std::lround((p.x() - g.origin.x()) / g.spacing.x())), 0L, long(g.dims[0]) - 1));
        const size_t j = size_t(std::clamp(long(std::lround((p.y() - g.origin.y()) / g.spacing.y())), 0L, long(g.dims[1]) - 1));
        double top = 0.;
        for (size_t k = g.dims[2]; k-- > 0;) {
            const double z = g.origin.z() + double(k) * g.spacing.z();
            if (z < p.z() - 1.5 * g.spacing.z() && g.occ[g.index(i, j, k)]) {
                top = z + 0.5 * g.spacing.z();
                break;
            }
        }
        volume += g.a[s] * std::abs(n.z()) * std::max(0., p.z() - top);
    }
    return volume;
}

double overhang_area(const ObjectiveGeometry &g, const ObjectiveMachine &m)
{
    const double sin_t = std::sin(m.printable_overhang);
    double       area  = 0.;
    for (size_t s = 0; s < g.p.size(); ++s)
        if (-g.n[s].z() > sin_t)
            area += g.a[s];
    return area;
}

}

double planar_support_time(const ObjectiveGeometry &g, const ObjectiveMachine &m)
{
    return support_volume(g, m) * 0.15 / (m.line_width * m.h * m.sparse_speed) * m.road_factor;
}

double support_time(const ObjectiveGeometry &g, const ObjectiveMachine &m)
{
    if (!m.support_on)
        return 0.;
    if ((m.support_cA >= 0. || m.support_cV >= 0.) && m.support_cos <= 1. &&
        std::none_of(g.n.begin(), g.n.end(), [&m](const Vec3d &n) { return -n.z() >= m.support_cos; }))
        return 0.;
    if (m.support_cV >= 0.)
        return m.support_cV * support_volume(g, m) + m.support_cS * overhang_area(g, m);
    if (m.support_cA < 0.)
        return SupportAllowance * planar_support_time(g, m);
    const double sin45 = std::sin(M_PI / 4.), z0 = std::isfinite(g.mesh_min.z()) ? g.mesh_min.z() : g.origin.z();
    double       A = 0., AH = 0.;
    for (size_t s = 0; s < g.p.size(); ++s)
        if (g.n[s].z() < -sin45) {
            A += g.a[s];
            AH += g.a[s] * std::max(0., g.p[s].z() - z0);
        }
    return m.support_cA * A + m.support_cH * AH;
}


namespace {

std::vector<double> native_tops(const ObjectiveGeometry &g, const ObjectiveMachine &m, const std::vector<float> &phi)
{
    std::vector<double> col(g.dims[2]), z(g.dims[2]);
    for (size_t k = 0; k < g.dims[2]; ++k) {
        col[k] = std::max(double(phi[g.index(g.dims[0] / 2, g.dims[1] / 2, k)]), k > 0 ? col[k - 1] : -std::numeric_limits<double>::infinity());
        z[k]   = g.origin.z() + double(k) * g.spacing.z();
    }
    auto inverse = [&](double x) {
        if (x <= col.front())
            return z.front();
        if (x >= col.back())
            return z.back();
        const size_t k = size_t(std::upper_bound(col.begin(), col.end(), x) - col.begin());
        return z[k - 1] + (z[k] - z[k - 1]) * (x - col[k - 1]) / (col[k] - col[k - 1]);
    };
    const double        top = g.mesh_max.z();
    std::vector<double> tops;
    double              zp = 0.;
    for (int j = 0; zp < top - 1e-6; ++j) {
        double       zj = j > 0 ? inverse(m.first_layer + j * m.h) : m.first_layer;
        const double hj = std::clamp(zj - zp, m.thickness_min, m.thickness_max);
        zj              = zp + hj < top - 0.5 * m.thickness_min ? std::min(zp + hj, top) : top;
        tops.push_back(zj);
        zp = zj;
    }
    if (const size_t n = tops.size(); n >= 3 && tops[n - 1] - tops[n - 2] < m.thickness_min) {
        const double rest = tops[n - 1] - tops[n - 3], last = tops[n - 1], below = tops[n - 3];
        tops.resize(n - 2);
        if (rest > m.thickness_max)
            tops.push_back(below + rest / 2.);
        tops.push_back(last);
    }
    return tops;
}

std::vector<double> infill_share(const std::vector<double> &tops, double cap)
{
    std::vector<double> share(tops.size(), 1.);
    if (cap <= 0. || tops.size() < 2)
        return share;
    auto close = [&](size_t first, size_t last, double height) {
        for (size_t j = first; j < last; ++j)
            share[j] = (tops[j] - tops[j - 1]) / height;
    };
    size_t first = 1;
    double height = 0.;
    for (size_t j = 1; j < tops.size(); ++j) {
        const double hj = tops[j] - tops[j - 1];
        if (height + hj >= cap + EPSILON) {
            close(first, j, height);
            first = j, height = 0.;
        }
        height += hj;
    }
    close(first, tops.size(), height);
    return share;
}

struct EvaluationCache
{
    const ObjectiveGeometry &g;
    std::vector<double> inside_values;
    std::vector<uint8_t> near_values;
    std::vector<int> distance_values;

    explicit EvaluationCache(const ObjectiveGeometry &geometry) : g(geometry) {}
    const std::vector<double> &inside()
    {
        if (inside_values.empty())
            inside_values = inside_distance(g);
        return inside_values;
    }
    const std::vector<uint8_t> &near()
    {
        if (near_values.empty())
            near_values = near_material(g);
        return near_values;
    }
    const std::vector<int> &distance()
    {
        if (distance_values.empty())
            distance_values = material_distance();
        return distance_values;
    }
private:
    std::vector<int> material_distance() const
    {
        std::vector<int> dist(g.nodes(), std::numeric_limits<int>::max() / 2);
        for (size_t id = 0; id < g.nodes(); ++id)
            if (g.occ[id])
                dist[id] = 0;
        for (int pass = 0; pass < 2; ++pass) {
            const long d0 = long(g.dims[0]), d1 = long(g.dims[1]), d2 = long(g.dims[2]);
            for (long n = 0; n < d0 * d1 * d2; ++n) {
                const long  id = pass ? d0 * d1 * d2 - 1 - n : n;
                const long  i = id / (d1 * d2), j = id / d2 % d1, k = id % d2;
                int        &d = dist[size_t(id)];
                for (long oi = -1; oi <= 1; ++oi)
                    for (long oj = -1; oj <= 1; ++oj)
                        for (long ok = -1; ok <= 1; ++ok) {
                            if ((oi * 9 + oj * 3 + ok < 0) == bool(pass) || (oi == 0 && oj == 0 && ok == 0))
                                continue;
                            const long ni = i + oi, nj = j + oj, nk = k + ok;
                            if (ni >= 0 && ni < d0 && nj >= 0 && nj < d1 && nk >= 0 && nk < d2)
                                d = std::min(d, dist[size_t((ni * d1 + nj) * d2 + nk)] + 1);
                        }
            }
        }
        return dist;
    }
};

}

static ObjectiveTerms evaluate_objective_cached(const ObjectiveGeometry &g, EvaluationCache &cache, const ObjectiveMachine &m, const ObjectiveWeights &w,
                                  const std::vector<float> &phi, bool hard, std::vector<HeadPair> *pairs = nullptr, std::vector<double> *errors = nullptr)
{
    ObjectiveTerms           t;
    if (errors != nullptr)
        errors->clear();
    const std::vector<Vec3d> G     = node_gradients(g, phi);
    const double             sin_t = std::sin(m.printable_overhang), er = e_ref(m);
    double area_c[3] = { 0., 0., 0. }, sq_c[3] = { 0., 0., 0. }, E_c[3] = { 0., 0., 0. }, area = 0., sq = 0., skin_area = 0.;
    double dev = 0.;
    for (size_t id = 0; id < g.nodes(); ++id)
        dev = std::max(dev, std::abs(double(phi[id]) - node_pos(g, id).z()));
    const ObjectiveCover &cover = dev < PlanarTol ? m.cover_planar : m.cover;
    double spread = 0.;
    for (size_t k = 0; k < g.dims[2]; ++k) {
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (size_t i = 0; i < g.dims[0]; ++i)
            for (size_t j = 0; j < g.dims[1]; ++j) {
                lo = std::min(lo, double(phi[g.index(i, j, k)]));
                hi = std::max(hi, double(phi[g.index(i, j, k)]));
            }
        spread = std::max(spread, 0.5 * (hi - lo));
    }
    t.spread = spread;
    const std::vector<double> tops = spread < ZOnlySpread && std::isfinite(g.mesh_max.z()) ? native_tops(g, m, phi) : std::vector<double>();
    auto native_error = [&](size_t s) {
        const double zs = g.p[s].z() - g.mesh_min.z(), nz = std::abs(g.n[s].z());
        const size_t jj = std::min(size_t(std::lower_bound(tops.begin(), tops.end(), zs) - tops.begin()), tops.size() - 1);
        const double lo = jj > 0 ? tops[jj - 1] : 0.;
        return nz > Cos5 ? std::min(std::abs(zs - tops[jj]), std::abs(zs - lo)) : (tops[jj] - lo) * nz / std::sqrt(12.);
    };
    const double stair_s_min = w.stair_s_min >= 0. ? w.stair_s_min : 2. * m.line_width;
    for (size_t s = 0; s < g.p.size(); ++s) {
        const Vec3d  gs = interp(g, g.p[s], [&G](size_t id) { return G[id]; });
        const double gl = std::max(gs.norm(), 1e-12);
        if (g.cls[s] == 0) {
            const double theta = std::acos(std::clamp(std::abs(g.n[s].z()), 0., 1.));
            double       th    = m.h / gl;
            if (!tops.empty()) {
                const double zs = g.p[s].z() - g.mesh_min.z();
                const size_t jj = std::min(size_t(std::lower_bound(tops.begin(), tops.end(), zs) - tops.begin()), tops.size() - 1);
                th              = tops[jj] - (jj > 0 ? tops[jj - 1] : 0.);
            }
            if (theta >= w.stair_theta_min && th > 0. && th / std::tan(theta) >= stair_s_min)
                t.F_stair += g.a[s] * std::sin(theta) / th;
        }
        const double nd = g.n[s].dot(gs) / gl;
        if (-nd > sin_t)
            t.S += g.a[s];
        t.S_smooth += g.a[s] * std::max(0., -nd - sin_t) / (1. - sin_t);
        if (g.n[s].z() < -Cos10) {
            const double dz = gs.z() / gl;
            t.S += CeilK * g.a[s] * std::sqrt(std::max(0., 1. - dz * dz)) * gl / m.h;
        }
        bool         flat, skinned;
        double       r;
        const double ps = interp(g, g.p[s], [&phi](size_t id) { return double(phi[id]); });
        const double e0 = g.cls[s] != 1 && !tops.empty() ? native_error(s) :
                                                           wall_error(g.cls[s], stair(g.n[s], gs, ps, m, flat, r), m.h / gl, nd, gs.z() / gl);
        const double e  = covered(g, s, up_floor(g.cls[s], e0), m, cover,
                                  skinned, gs.z() / gl);
        if (skinned)
            skin_area += g.a[s];
        if (errors != nullptr)
            errors->push_back(e);
        const double nz = g.n[s].z();
        t.F_wallp += g.a[s] * std::sqrt(std::max(0., 1. - nz * nz)) * gl / m.h;
        t.F_upp += g.a[s] * std::max(nz, 0.) * gl / m.h;
        t.F_downp += g.a[s] * std::max(-nz, 0.) * gl / m.h;
        const double vcap = m.max_volumetric / (m.line_width * m.h / gl);
        t.F_wallt += g.a[s] * std::sqrt(std::max(0., 1. - nz * nz)) * gl / m.h / std::min(m.shell_speed, vcap);
        t.F_upt += g.a[s] * std::max(nz, 0.) * gl / m.h / std::min(m.top_speed, vcap);
        if (std::abs(nz) > Cos10) {
            const double dz = gs.z() / gl;
            t.F_flatx += g.a[s] * std::sqrt(std::max(0., 1. - dz * dz)) * gl / m.h;
        }
        const int c = g.cls[s];
        E_c[c] += g.a[s] * (e / er) * (e / er);
        area_c[c] += g.a[s];
        sq_c[c] += g.a[s] * e * e;
        area += g.a[s];
        sq += g.a[s] * e * e;
    }
    t.E_stair = w.stair_k * std::pow(0.25 * m.h / er, 2.) * m.line_width * t.F_stair;
    E_c[0] += t.E_stair;
    t.E_up = E_c[0], t.E_wall = E_c[1], t.E_down = E_c[2];
    t.E        = w.up * E_c[0] + w.wall * E_c[1] + w.down * E_c[2];
    t.rms_up   = std::sqrt(sq_c[0] / std::max(area_c[0], 1e-12));
    t.rms_wall = std::sqrt(sq_c[1] / std::max(area_c[1], 1e-12));
    t.rms_down = std::sqrt(sq_c[2] / std::max(area_c[2], 1e-12));
    t.rms      = std::sqrt(sq / std::max(area, 1e-12));

    const double        V  = g.spacing.prod();
    const double        wd = m.line_width;
    double              lo = std::numeric_limits<double>::infinity(), hi = -lo;
    std::vector<double> road(g.nodes(), 0.), feed(g.nodes(), 1.), t_node(g.nodes(), 0.);
    const std::vector<double> share = tops.empty() ? std::vector<double>() : infill_share(tops, m.infill_combine);
    double                    sparse_volume = 0.;
    for (size_t id = 0; id < g.nodes(); ++id) {
        if (!g.occ[id])
            continue;
        const double gn     = G[id].norm();
        const double fs     = g.shell[id];
        double       ks     = 1.;
        if (!share.empty()) {
            const double zs = node_pos(g, id).z() - g.mesh_min.z();
            ks              = share[std::min(size_t(std::lower_bound(tops.begin(), tops.end(), zs) - tops.begin()), tops.size() - 1)];
        }
        const double cap    = m.max_volumetric / (wd * m.h / std::max(gn, 1e-12));
        const double cap_s  = cap * ks;
        const double unit   = V * gn / (m.h * wd);
        const double ti     = unit * ks * (1. - fs) * m.infill_density / std::min(m.sparse_speed, cap_s);
        road[id]            = unit * (fs + ks * (1. - fs) * m.infill_density);
        t_node[id]          = unit * fs / std::min(m.shell_speed, cap) + ti;
        feed[id]            = road[id] / std::max(t_node[id], 1e-12);
        t.T_roads += t_node[id];
        t.road_mm += road[id];
        t.Ti += ti;
        t.Li += unit * ks * (1. - fs) * m.infill_density;
        sparse_volume += V * (1. - fs);
        lo = std::min(lo, double(phi[id])), hi = std::max(hi, double(phi[id]));
    }
    t.T_roads *= m.road_factor;
    {
        const double height = std::isfinite(g.mesh_max.z()) ? g.mesh_max.z() - g.mesh_min.z() : 1.;
        t.F_seg             = t.Li / std::sqrt(std::max(sparse_volume / std::max(height, 1.), 1.));
    }
    t.F_first = m.first_layer_speed > 0. ? g.bed_area / (wd * m.first_layer_speed) : 0.;
    {
        const double             pb = g.spacing.minCoeff();
        std::map<int64_t, double> per_bin;
        for (size_t id = 0; id < g.nodes(); ++id)
            if (g.occ[id])
                per_bin[int64_t(std::floor((double(phi[id]) - m.first_layer) / pb))] += m.road_factor * t_node[id];
        for (const auto &[b, tb] : per_bin)
            if (tb > 0.)
                t.T_minlayer += std::max(0., m.min_layer_time * pb / m.h - tb);
    }
    t.layers = std::isfinite(lo) ? (hi - lo) / m.h + 1. : 0.;
    double tilted_mm = 0.;
    if (m.rotary) {
        const std::vector<double> &inside = cache.inside();
        for (size_t id = 0; id < g.nodes(); ++id) {
            if (!g.occ[id])
                continue;
            const Vec3d  D   = G[id] / std::max(G[id].norm(), 1e-12);
            const double tau = std::acos(std::clamp(D.z(), -1., 1.));
            if (tau > Tilted)
                tilted_mm += road[id];
            const Roads  rd = road_directions(g, id, D, inside);
            double       ex = 0.;
            for (int k = 0; k < rd.count; ++k) {
                const Turn   tr = turn_along(D, du_at(g, id, rd.u[k], G));
                const double x0 = 1. / feed[id];
                ex += rd.weight[k] * (std::max({ x0, std::abs(tr.dB) / m.b_rate, tr.dC / m.c_rate }) - x0);
            }
            t.T_axis += road[id] * ex;
        }
    }
    t.road_pieces = tilted_mm / PieceMm;
    {
        const double         pitch = g.spacing.minCoeff(), per = pitch / m.h;
        std::vector<int64_t> bin(g.nodes());
        for (size_t id = 0; id < bin.size(); ++id)
            bin[id] = int64_t(std::floor((double(phi[id]) - m.first_layer) / pitch));
        std::vector<int>    label(g.nodes(), -1);
        std::vector<size_t> stack;
        struct Island
        {
            int64_t bin;
            Vec3d   sum { Vec3d::Zero() };
            double  count = 0.;
            std::vector<double> az;
        };
        std::vector<Island> islands;
        for (size_t id = 0; id < g.nodes(); ++id) {
            if (!g.occ[id] || label[id] >= 0)
                continue;
            Island isl;
            isl.bin = bin[id];
            label[id] = int(islands.size());
            stack.assign(1, id);
            while (!stack.empty()) {
                const size_t q = stack.back();
                stack.pop_back();
                const Ijk c = ijk_of(g, q);
                isl.sum += Vec3d(double(c.i), double(c.j), double(c.k));
                isl.count += 1.;
                if (m.rotary && !m.c_continuous && g.shell[q]) {
                    const Vec3d D = G[q] / std::max(G[q].norm(), 1e-12);
                    if (std::acos(std::clamp(D.z(), -1., 1.)) > 10. * M_PI / 180.)
                        isl.az.push_back(std::atan2(D.y(), D.x()) * 180. / M_PI);
                }
                for (int di = -1; di <= 1; ++di)
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int dk = -1; dk <= 1; ++dk) {
                            const long a2 = long(c.i) + di, b2 = long(c.j) + dj, e2 = long(c.k) + dk;
                            if (a2 < 0 || b2 < 0 || e2 < 0 || a2 >= long(g.dims[0]) || b2 >= long(g.dims[1]) || e2 >= long(g.dims[2]))
                                continue;
                            const size_t n2 = g.index(size_t(a2), size_t(b2), size_t(e2));
                            if (g.occ[n2] && label[n2] < 0 && bin[n2] == isl.bin) {
                                label[n2] = label[id];
                                stack.push_back(n2);
                            }
                        }
            }
            islands.push_back(std::move(isl));
        }
        std::map<int64_t, std::vector<Vec3d>> by_bin;
        for (Island &isl : islands) {
            t.island_layers += per;
            by_bin[isl.bin].push_back((isl.sum / isl.count).cwiseProduct(g.spacing));
            if (isl.az.size() >= 8) {
                std::sort(isl.az.begin(), isl.az.end());
                double gap = 360. - (isl.az.back() - isl.az.front());
                for (size_t k = 1; k < isl.az.size(); ++k)
                    gap = std::max(gap, isl.az[k] - isl.az[k - 1]);
                if (gap < 90.)
                    t.T_unwind += per;
            }
        }
        for (const auto &[b, cen] : by_bin)
            if (cen.size() > 1)
                for (size_t i = 0; i < cen.size(); ++i) {
                    double best = std::numeric_limits<double>::infinity();
                    for (size_t j = 0; j < cen.size(); ++j)
                        if (j != i)
                            best = std::min(best, (cen[i] - cen[j]).norm());
                    t.hop_mm += best * per;
                }
        if (m.rotary && !m.c_continuous)
            t.T_unwind *= 2. * M_PI / m.c_rate;
        else
            t.T_unwind = 0.;
    }
    t.n_travel = TravelPerIslandLayer * t.island_layers + TravelPerMRoad * t.road_mm / 1000. + t.road_pieces;
    t.T_travel = TravelS * t.n_travel + t.hop_mm / m.travel_speed;
    t.T_skin   = cover.skin_time * skin_area;
    t.T        = t.T_roads + t.T_minlayer + t.T_travel + AxisAccel * (t.T_axis + t.T_unwind) + t.T_skin;
    t.J        = w.sup * t.S + t.E + w.time * t.T;
    for (const ActiveFieldTerm &ft : active_field_terms(g)) {
        const double c = ft.weight * ft.term->cost_field({ g, phi }, G, nullptr, nullptr);
        t.field_terms.emplace_back(ft.name, c);
        t.J += c;
    }
    if (!hard)
        return t;

    const bool       curved = t.spread >= ZOnlySpread;
    if (curved && m.shells && m.top_speed > 0.) {
        double A_top = 0.;
        for (size_t s = 0; s < g.p.size(); ++s)
            if (g.n[s].z() > 0.05)
                A_top += g.a[s] * g.n[s].z();
        t.F_shells_top = A_top * m.top_shell_layers / (wd * m.top_speed);
    }
    const TimeModel &tm     = curved ? m.time_curved : m.time_zonly;
    const double     base   = curved ? m.time_base_curved : m.time_base_zonly;
    t.T_ratio               = base > 0. ? (tm(t) + (curved ? 0. : m.time_support_zonly)) / base : 0.;
    t.T_cap                 = tm.cap;
    t.H_time                = t.T_ratio > tm.cap;
    std::vector<size_t> occupied;
    for (size_t id = 0; id < g.nodes(); ++id) {
        if (!g.occ[id])
            continue;
        occupied.push_back(id);
        const double gn = G[id].norm();
        const auto [gmin, gmax] = grad_bounds(m, ijk_of(g, id).k);
        t.H_thickness += gn < gmin || gn > gmax;
        if (m.rotary)
            t.H_slope += std::acos(std::clamp(G[id].z() / std::max(gn, 1e-12), -1., 1.)) > m.tilt_max;
        else
            t.H_slope += std::atan2(std::hypot(G[id].x(), G[id].y()), G[id].z()) > slope_cap_at(m, G[id]);
        if (ijk_of(g, id).k <= bed_layers(g, m))
            t.H_bed += std::abs(double(phi[id]) - (m.anchor.size() == g.nodes() ? double(m.anchor[id]) : node_pos(g, id).z())) > 0.01;
        else if (const size_t k = ijk_of(g, id).k; m.pinned(k))
            t.H_bed += std::abs(double(phi[id]) - double(m.pinned_v[k])) > 0.01;
    }
    {
        const std::vector<uint8_t> &near = cache.near();
        for (size_t id = 0; id < g.nodes(); ++id)
            if (ijk_of(g, id).k + 1 < g.dims[2] && (near[id] || near[id + 1])) {
                const double dphi = double(phi[id + 1]) - double(phi[id]);
                const double hi   = g.spacing.z() * grad_bounds(m, ijk_of(g, id).k).second;
                t.H_thickness += dphi <= 0. || dphi > hi;
            }
    }
    const std::vector<int> &dist = cache.distance();
    const double smin = g.spacing.minCoeff(), diag = g.spacing.norm();
    std::vector<double> phi_min(g.dims[2], std::numeric_limits<double>::infinity());
    double phi_scale = 1.;
    for (size_t id = 0; id < g.nodes(); ++id) {
        phi_min[id % g.dims[2]] = std::min(phi_min[id % g.dims[2]], double(phi[id]));
        phi_scale = std::max(phi_scale, std::abs(double(phi[id])));
    }
    std::vector<double> phi_above = phi_min;
    for (size_t k = phi_above.size(); k-- > 1;)
        phi_above[k - 1] = std::min(phi_above[k - 1], phi_above[k]);
    const double phi_roundoff = 32. * std::numeric_limits<double>::epsilon() * phi_scale;
    auto clear_above = [&](double level) {
        const auto next = std::upper_bound(phi_above.begin(), phi_above.end(), level + phi_roundoff);
        if (next == phi_above.begin())
            return -std::numeric_limits<double>::infinity();
        if (next == phi_above.end())
            return std::numeric_limits<double>::infinity();
        const size_t k = size_t(next - phi_above.begin());
        const double t = (level + phi_roundoff - phi_min[k - 1]) / (phi_min[k] - phi_min[k - 1]);
        const double z = g.origin.z() + g.spacing.z() * (double(k - 1) + t);
        return z + 64. * std::numeric_limits<double>::epsilon() * (std::abs(z) + g.spacing.z());
    };
    struct PatchBounds
    {
        Vec3d lo, hi;
        double roundoff_scale;
    };
    std::vector<PatchBounds> patch_bounds;
    patch_bounds.reserve(m.head.size());
    for (const HeadSamples &box : m.head) {
        Vec3d lo = box.centre, hi = box.centre;
        for (const Vec3d &point : box.points) {
            lo = lo.cwiseMin(point);
            hi = hi.cwiseMax(point);
        }
        patch_bounds.push_back({ lo, hi, lo.cwiseAbs().cwiseMax(hi.cwiseAbs()).sum() });
    }
    auto far_from_material = [&](const Vec3d &c, double radius) {
        Vec3d idx = ((c - g.origin).cwiseQuotient(g.spacing)).array().round().matrix();
        for (int a = 0; a < 3; ++a)
            idx[a] = std::clamp(idx[a], 0., double(g.dims[size_t(a)] - 1));
        return double(dist[g.index(size_t(idx[0]), size_t(idx[1]), size_t(idx[2]))]) * smin - 0.5 * diag > radius + diag;
    };
    const size_t step = std::max<size_t>(1, (occupied.size() + 4095) / 4096);
    const Vec3d  top  = (Vec3d(double(g.dims[0]), double(g.dims[1]), double(g.dims[2])) - Vec3d::Ones()).cwiseProduct(g.spacing);
    std::vector<size_t> probes;
    for (size_t k = 0; k < occupied.size(); k += step)
        probes.push_back(occupied[k]);
    t.H_head_probes = int(probes.size());
    std::vector<uint8_t> hits(probes.size(), 0);
    std::vector<double>  worst(probes.size(), 0.);
    std::vector<Vec3d>   worst_q(probes.size(), Vec3d::Zero());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, probes.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t k = range.begin(); k < range.end(); ++k) {
            const size_t   id    = probes[k];
            const Vec3d    P     = node_pos(g, id);
            const double   level = double(phi[id]) + m.h;
            const double   z_clear = clear_above(level);
            const Matrix3d R     = m.rotary ? rotation_to(G[id] / std::max(G[id].norm(), 1e-12)) : Matrix3d(Matrix3d::Identity());
            bool           hit   = false;
            for (size_t patch = 0; patch < m.head.size(); ++patch) {
                const HeadSamples &box = m.head[patch];
                const PatchBounds &bounds = patch_bounds[patch];
                double z_min = P.z();
                for (int a = 0; a < 3; ++a)
                    z_min += R(2, a) * (R(2, a) < 0. ? bounds.hi[a] : bounds.lo[a]);
                z_min -= 64. * std::numeric_limits<double>::epsilon() * (std::abs(P.z()) + 1. + bounds.roundoff_scale);
                if (!(m.rotary && z_min < 0.) && z_min > z_clear)
                    continue;
                const Vec3d c = P + R * box.centre;
                if (!(m.rotary && c.z() < box.radius) && far_from_material(c, box.radius))
                    continue;
                for (const Vec3d &h : box.points) {
                    const Vec3d Q = P + R * h;
                    if (m.rotary && Q.z() < 0. && std::hypot(Q.x(), Q.y()) < m.plate_radius + 10.) {
                        hit = true;
                        if (!pairs)
                            break;
                    }
                    const Vec3d rel = Q - g.origin;
                    if (!(rel.array() >= 0.).all() || !(rel.array() <= top.array()).all())
                        continue;
                    size_t ids[8];
                    double weights[8];
                    corners(g, Q, ids, weights);
                    double occ = double(g.occ[ids[0]]) * weights[0];
                    for (int q = 1; q < 8; ++q)
                        occ += double(g.occ[ids[q]]) * weights[q];
                    if (occ < 0.5)
                        continue;
                    double value = double(phi[ids[0]]) * weights[0];
                    for (int q = 1; q < 8; ++q)
                        value += double(phi[ids[q]]) * weights[q];
                    const double v = value - level;
                    if (v < 0.) {
                        hit = true;
                        if (!pairs)
                            break;
                        if (v < worst[k])
                            worst[k] = v, worst_q[k] = Q;
                    }
                }
                if (hit && !pairs)
                    break;
            }
            hits[k] = hit;
        }
    });
    for (size_t k = 0; k < probes.size(); ++k) {
        t.H_head += hits[k];
        if (pairs && worst[k] < 0.)
            pairs->push_back({ probes[k], worst_q[k] });
    }
    return t;
}

ObjectiveTerms evaluate_objective(const ObjectiveGeometry &g, const ObjectiveMachine &m, const ObjectiveWeights &w,
                                  const std::vector<float> &phi, bool hard, std::vector<HeadPair> *pairs, std::vector<double> *errors)
{
    EvaluationCache cache(g);
    return evaluate_objective_cached(g, cache, m, w, phi, hard, pairs, errors);
}

namespace {

struct System
{
    std::vector<int>                    col;
    int                                 unknowns = 0;
    std::vector<Eigen::Triplet<double>> trip;
    std::vector<double>                 rhs;
    std::vector<double>                 lin;

    System(const ObjectiveGeometry &g, size_t held, bool tie_z = false, const ObjectiveMachine *m = nullptr)
    {
        col.assign(g.nodes(), -1);
        auto free = [&](size_t k) { return k > held && !(m && m->pinned(k)); };
        if (tie_z) {
            std::vector<int> unknown_of(g.dims[2], -1);
            for (size_t k = 0; k < g.dims[2]; ++k)
                if (free(k))
                    unknown_of[k] = unknowns++;
            for (size_t id = 0; id < g.nodes(); ++id)
                col[id] = unknown_of[ijk_of(g, id).k];
        } else
            for (size_t id = 0; id < g.nodes(); ++id)
                if (free(ijk_of(g, id).k))
                    col[id] = unknowns++;
        lin.assign(size_t(unknowns), 0.);
    }
    void row(const std::vector<std::pair<size_t, double>> &c, double r0, double weight)
    {
        const double sw = std::sqrt(weight);
        const int    r  = int(rhs.size());
        for (const auto &[node, v] : c)
            if (col[node] >= 0 && v != 0.)
                trip.emplace_back(r, col[node], sw * v);
        rhs.push_back(-sw * r0);
    }
    void linear(const std::vector<std::pair<size_t, double>> &c, double scale)
    {
        for (const auto &[node, v] : c)
            if (col[node] >= 0)
                lin[size_t(col[node])] += scale * v;
    }
};

struct Surrogate
{
    const ObjectiveGeometry       &g;
    const ObjectiveMachine        &m;
    const ObjectiveWeights        &w;
    const ObjectiveParams         &p;
    const std::vector<HeadPair>   &pairs;
    std::vector<std::vector<Coef>> sample_st, node_st, pair_st;
    std::vector<size_t>            occupied;
    double                         hard = Hard;
    std::vector<uint8_t>           near;
    std::vector<double>            inside;

    Surrogate(const ObjectiveGeometry &g_, const ObjectiveMachine &m_, const ObjectiveWeights &w_, const ObjectiveParams &p_,
              const std::vector<HeadPair> &pairs_, EvaluationCache &cache)
        : g(g_), m(m_), w(w_), p(p_), pairs(pairs_)
    {
        for (const Vec3d &s : g.p)
            sample_st.push_back(point_stencil(g, s));
        node_st.resize(g.nodes());
        for (size_t id = 0; id < g.nodes(); ++id) {
            node_gradient_stencil(g, id, 1., node_st[id]);
            if (g.occ[id])
                occupied.push_back(id);
        }
        for (const HeadPair &hp : pairs)
            pair_st.push_back(point_stencil(g, hp.q));
        if (m.rotary)
            inside = cache.inside();
        const std::vector<uint8_t> &dil = cache.near();
        near.assign(g.nodes(), 0);
        for (size_t id = 0; id < g.nodes(); ++id)
            near[id] = ijk_of(g, id).k + 1 < g.dims[2] && (dil[id] || dil[id + 1]);
    }

    static Vec3d grad(const std::vector<Coef> &st, const std::vector<float> &phi)
    {
        Vec3d v = Vec3d::Zero();
        for (const Coef &c : st)
            v += c.g * double(phi[c.node]);
        return v;
    }
    static double value(const std::vector<Coef> &st, const std::vector<float> &phi)
    {
        double v = 0.;
        for (const Coef &c : st)
            v += c.v * double(phi[c.node]);
        return v;
    }
    static std::vector<std::pair<size_t, double>> along(const std::vector<Coef> &st, const Vec3d &dg, double dv = 0.)
    {
        std::vector<std::pair<size_t, double>> out;
        out.reserve(st.size());
        for (const Coef &c : st)
            out.emplace_back(c.node, c.g.dot(dg) + c.v * dv);
        return out;
    }

    double run(const std::vector<float> &phi, const std::vector<float> &ref, System *sys) const
    {
        double       cost  = 0.;
        const double sin_t = std::sin(m.printable_overhang), er = e_ref(m);
        for (size_t s = 0; s < g.p.size(); ++s) {
            const Vec3d  gs = grad(sample_st[s], phi);
            const double gl = std::max(gs.norm(), 1e-12);
            const Vec3d  gh = gs / gl;
            const Vec3d &n  = g.n[s];
            const double nd = n.dot(gh);
            const double f  = (-nd - sin_t) / (1. - sin_t);
            if (f > 0.) {
                const Vec3d  df = -(n - nd * gh) / (gl * (1. - sin_t));
                const double wa = w.sup * g.a[s];
                if (f < HuberDelta) {
                    cost += wa * f * f / (2. * HuberDelta);
                    if (sys)
                        sys->row(along(sample_st[s], df), f, wa / (2. * HuberDelta));
                } else {
                    cost += wa * (f - 0.5 * HuberDelta);
                    if (sys)
                        sys->linear(along(sample_st[s], df), wa);
                }
            }
            bool         flat, skinned;
            double       r;
            const double ps = value(sample_st[s], phi);
            double       e0 = stair(n, gs, ps, m, flat, r);
            if (flat)
                e0 = m.h / M_PI * std::abs(std::sin(M_PI * r / m.h)) / gl;
            const double ew = wall_error(g.cls[s], e0, m.h / gl, nd, gh.z());
            const double ef = up_floor(g.cls[s], ew);
            const double e  = covered(g, s, ef, m, m.cover, skinned, gh.z());
            const double cw = class_weight(w, g.cls[s]) * g.a[s] / (er * er);
            cost += cw * e * e;
            if (sys && e == ef) {
                if (flat) {
                    const double es = m.h / M_PI * std::sin(M_PI * r / m.h) / gl;
                    sys->row(along(sample_st[s], -es / gl * gh, std::cos(M_PI * r / m.h) / gl), es, cw);
                }
                else if (e0 > 0.) {
                    const double ng = n.dot(gs);
                    const Vec3d  de = m.h / std::sqrt(12.) *
                                     ((ng >= 0. ? 1. : -1.) * n / (gl * gl) - 2. * std::abs(ng) * gs / (gl * gl * gl * gl));
                    sys->row(along(sample_st[s], de), e0, cw);
                }
                if (walled(g.cls[s], nd)) {
                    const double sn    = std::sqrt(std::max(1e-12, 1. - nd * nd));
                    const double flank = WallK * m.h / (2. * gl) * sn;
                    const Vec3d  dnd   = (n - nd * gh) / gl;
                    const Vec3d  df    = WallK * m.h / 2. * (-gh / (gl * gl) * sn - nd / (gl * sn) * dnd);
                    sys->row(along(sample_st[s], df), flank, cw);
                }
                if (g.cls[s] == 1) {
                    const double q = std::hypot(gs.x(), gs.y());
                    if (q > 1e-9) {
                        const Vec3d dq = Vec3d(gs.x() / q, gs.y() / q, 0.);
                        const Vec3d dt = TiltK * m.h * (dq / (gl * gl) - 2. * q * gs / (gl * gl * gl * gl));
                        sys->row(along(sample_st[s], dt), TiltK * m.h * q / (gl * gl), cw);
                    }
                }
            }
        }
        const std::vector<Vec3d> Gphi = m.rotary ? node_gradients(g, phi) : std::vector<Vec3d>();
        const double V = g.spacing.prod(), wd = m.line_width;
        auto dir = [&](size_t id, const std::vector<float> &f) {
            const Vec3d v = grad(node_st[id], f);
            return Vec3d(v / std::max(v.norm(), 1e-12));
        };
        for (size_t id : occupied) {
            const Vec3d  gv    = grad(node_st[id], phi);
            const double gn    = std::max(gv.norm(), 1e-12);
            const Vec3d  gh    = gv / gn;
            const double fs    = g.shell[id];
            const double road  = V * gn / (m.h * wd) * (fs + (1. - fs) * m.infill_density);
            double t_node = 0., dt_dgn = 0.;
            const double flow_cap = m.max_volumetric * gn / (wd * m.h);
            for (int part = 0; part < 2; ++part) {
                const double share = part == 0 ? fs : (1. - fs) * m.infill_density;
                const double speed = part == 0 ? m.shell_speed : m.sparse_speed;
                if (speed <= flow_cap) {
                    t_node += share * V * gn / (m.h * wd * speed);
                    dt_dgn += share * V / (m.h * wd * speed);
                } else
                    t_node += share * V / m.max_volumetric;
            }
            cost += w.time * m.road_factor * t_node;
            if (sys && dt_dgn > 0.)
                sys->linear(along(node_st[id], gh), w.time * m.road_factor * dt_dgn);
            const double speed = road / std::max(t_node, 1e-12);
            const Vec3d  rh = dir(id, ref);
            const double cp = p.tilt_sparsity * w.time * TravelS / PieceMm * road;
            const double sin2 = std::sin(Tilted), rx = std::hypot(gh.x(), gh.y()), rr = std::hypot(rh.x(), rh.y());
            cost += cp * std::min(rx / sin2, 1.);
            if (sys && rr < sin2) {
                const double iw = cp / (sin2 * 2. * (rr + Irls));
                for (int a = 0; a < 2; ++a) {
                    Vec3d e = Vec3d::Zero();
                    e[a]    = 1.;
                    sys->row(along(node_st[id], (e - gh[a] * gh) / gn), gh[a], iw);
                }
            }
            if (m.rotary) {
                const double x0 = 1. / speed, soft = p.axis_softness * x0;
                auto lse = [soft](double a, double b, double c) {
                    const double top = std::max({ a, b, c });
                    return top + soft * std::log(std::exp((a - top) / soft) + std::exp((b - top) / soft) + std::exp((c - top) / soft));
                };
                const double base = lse(x0, 0., 0.);
                const Roads  rd   = road_directions(g, id, gh, inside);
                for (int k = 0; k < rd.count; ++k) {
                    const Turn   tr = turn_along(gh, du_at(g, id, rd.u[k], Gphi));
                    const double xb = std::abs(tr.dB) / m.b_rate, xc = tr.dC / m.c_rate;
                    const double scale = w.time * AxisAccel * road * rd.weight[k];
                    cost += scale * (lse(x0, xb, xc) - base);
                    if (sys) {
                        const double top = std::max({ x0, xb, xc });
                        const double e0 = std::exp((x0 - top) / soft), eb = std::exp((xb - top) / soft), ec = std::exp((xc - top) / soft);
                        const double sum = e0 + eb + ec;
                        for (int which = 0; which < 2; ++which) {
                            const double share = (which == 0 ? eb : ec) / sum;
                            if (share < 1e-6)
                                continue;
                            const double rate = which == 0 ? m.b_rate : m.c_rate;
                            const double val  = which == 0 ? tr.dB : tr.dC;
                            const double iw   = scale * share / (rate * 2. * (std::abs(val) + Irls));
                            std::vector<std::pair<size_t, double>> c;
                            du_at(g, id, rd.u[k], Gphi, &c, which == 0 ? tr.dB_du : tr.dC_du, &node_st);
                            sys->row(c, val, iw);
                        }
                    }
                }
            }
            if (!m.rotary && p.roughness > 0.) {
                const Ijk    c         = ijk_of(g, id);
                const size_t at[3]     = { c.i, c.j, c.k };
                const size_t stride[3] = { g.dims[1] * g.dims[2], g.dims[2], 1 };
                const double cr        = w.time * p.roughness * road;
                for (int a = 0; a < 3; ++a) {
                    if (at[a] + 1 >= g.dims[size_t(a)] || !g.occ[id + stride[a]])
                        continue;
                    const size_t o   = id + stride[a];
                    const Vec3d  go  = grad(node_st[o], phi);
                    const double gon = std::max(go.norm(), 1e-12);
                    const Vec3d  oh  = go / gon;
                    const Vec3d  dd  = (oh - gh) / g.spacing[a];
                    const Vec3d  ddr = (dir(o, ref) - dir(id, ref)) / g.spacing[a];
                    cost += cr * dd.norm();
                    if (sys) {
                        const double   iw = cr / (2. * (ddr.norm() + Irls));
                        const Matrix3d P1 = (Matrix3d::Identity() - gh * gh.transpose()) / gn;
                        const Matrix3d P2 = (Matrix3d::Identity() - oh * oh.transpose()) / gon;
                        for (int r = 0; r < 3; ++r) {
                            auto c2 = along(node_st[o], Vec3d(P2.row(r).transpose()) / g.spacing[a]);
                            auto c1 = along(node_st[id], -Vec3d(P1.row(r).transpose()) / g.spacing[a]);
                            c2.insert(c2.end(), c1.begin(), c1.end());
                            sys->row(c2, dd[r], iw);
                        }
                    }
                }
            }
            const auto [gmin, gmax] = grad_bounds(m, ijk_of(g, id).k);
            if (gn < gmin || gn > gmax) {
                const double bound = gn < gmin ? gmin * 1.02 : gmax * 0.98;
                cost += hard * (gn - bound) * (gn - bound);
                if (sys)
                    sys->row(along(node_st[id], gh), gn - bound, hard);
            }
            const double cap = m.rotary ? m.tilt_max : slope_cap_at(m, gv);
            const double hv  = gv.z() - std::cos(cap) * gn;
            if (hv < 0.) {
                cost += hard * hv * hv;
                if (sys)
                    sys->row(along(node_st[id], Vec3d::UnitZ() - std::cos(cap) * gh), hv - 0.01, hard);
            }
        }
        {
            for (size_t id = 0; id < g.nodes(); ++id)
                if (near[id] && ijk_of(g, id).k + 1 < g.dims[2]) {
                    const auto [blo, bhi] = grad_bounds(m, ijk_of(g, id).k);
                    const double lo = g.spacing.z() * blo, hi = g.spacing.z() * bhi;
                    const double dphi = double(phi[id + 1]) - double(phi[id]);
                    if (dphi <= 0. || dphi > hi) {
                        const double bound = dphi <= 0. ? 1.02 * lo : 0.98 * hi;
                        cost += hard * (dphi - bound) * (dphi - bound);
                        if (sys)
                            sys->row({ { id + 1, 1. }, { id, -1. } }, dphi - bound, hard);
                    }
                }
        }
        for (size_t k = 0; k < pairs.size(); ++k) {
            const double v = value(pair_st[k], phi) - double(phi[pairs[k].node]) - m.h;
            if (v < 0.) {
                cost += hard * v * v;
                if (sys) {
                    std::vector<std::pair<size_t, double>> c;
                    for (const Coef &e : pair_st[k])
                        c.emplace_back(e.node, e.v);
                    c.emplace_back(pairs[k].node, -1.);
                    sys->row(c, v - 0.05, hard);
                }
            }
        }
        const size_t stride[3] = { g.dims[1] * g.dims[2], g.dims[2], 1 };
        for (size_t id = 0; id < g.nodes(); ++id) {
            const Ijk    c     = ijk_of(g, id);
            const size_t at[3] = { c.i, c.j, c.k };
            for (int a = 0; a < 3; ++a) {
                if (at[a] == 0 || at[a] + 1 >= g.dims[size_t(a)])
                    continue;
                const double d2 = double(phi[id + stride[a]]) - 2. * double(phi[id]) + double(phi[id - stride[a]]);
                const double wr = Regular * V / (g.spacing[a] * g.spacing[a]);
                cost += wr * d2 * d2;
                if (sys)
                    sys->row({ { id + stride[a], 1. }, { id, -2. }, { id - stride[a], 1. } }, d2, wr);
            }
        }
        if (const std::vector<ActiveFieldTerm> &terms = active_field_terms(g); !terms.empty()) {
            std::vector<Vec3d> G(g.nodes()), dG;
            std::vector<double> dphi;
            for (size_t id = 0; id < g.nodes(); ++id)
                G[id] = grad(node_st[id], phi);
            for (const ActiveFieldTerm &ft : terms) {
                dG.assign(g.nodes(), Vec3d::Zero());
                const bool field_values = sys && ft.term->needs_field_values();
                if (field_values)
                    dphi.assign(g.nodes(), 0.);
                cost += ft.weight * ft.term->cost_field({ g, phi }, G, field_values ? &dphi : nullptr, sys ? &dG : nullptr);
                if (field_values)
                    for (size_t id = 0; id < g.nodes(); ++id)
                        if (dphi[id] != 0.)
                            sys->linear({ { id, dphi[id] } }, ft.weight);
                if (sys)
                    for (size_t id : occupied)
                        if (dG[id].squaredNorm() > 0.)
                            sys->linear(along(node_st[id], dG[id]), ft.weight);
            }
        }
        return cost;
    }
};

bool step(const Surrogate &sur, std::vector<float> &phi, double &cost, double &damping)
{
    using Sparse = Eigen::SparseMatrix<double>;
    System sys(sur.g, bed_layers(sur.g, sur.m), sur.p.planar_adaptive, &sur.m);
    cost = sur.run(phi, phi, &sys);
    if (sys.unknowns == 0)
        return false;
    Sparse A(Eigen::Index(sys.rhs.size()), sys.unknowns);
    A.setFromTriplets(sys.trip.begin(), sys.trip.end());
    Sparse          M = Sparse(A.transpose()) * A;
    Eigen::VectorXd b = A.transpose() * Eigen::Map<const Eigen::VectorXd>(sys.rhs.data(), Eigen::Index(sys.rhs.size()));
    b -= 0.5 * Eigen::Map<const Eigen::VectorXd>(sys.lin.data(), Eigen::Index(sys.lin.size()));
    const Eigen::VectorXd diag = M.diagonal();
    const double          mean = std::max(diag.mean(), 1e-12);
    for (int i = 0; i < M.outerSize(); ++i)
        M.coeffRef(i, i) += damping * std::max(diag[i], 1e-3 * mean);
    Eigen::ConjugateGradient<Sparse, Eigen::Lower | Eigen::Upper> cg;
    cg.setTolerance(1e-6);
    cg.setMaxIterations(500);
    cg.compute(M);
    Eigen::VectorXd delta = cg.solve(b);
    if (const double top = delta.lpNorm<Eigen::Infinity>(); top > sur.p.trust)
        delta *= sur.p.trust / top;
    const std::vector<float> start = phi;
    for (double alpha = 1.; alpha > 0.03; alpha *= 0.5) {
        for (size_t id = 0; id < phi.size(); ++id)
            if (sys.col[id] >= 0)
                phi[id] = float(double(start[id]) + alpha * delta[sys.col[id]]);
        const double trial = sur.run(phi, start, nullptr);
        if (trial < cost) {
            cost    = trial;
            damping = std::max(1e-4, damping * (alpha == 1. ? 0.3 : 1.));
            return true;
        }
    }
    phi     = start;
    damping = std::min(1e4, damping * 10.);
    return false;
}

void integrate(const ObjectiveGeometry &g, const ObjectiveMachine &m, const std::vector<Vec3d> &d, std::vector<float> &phi)
{
    using Sparse = Eigen::SparseMatrix<double>;
    System       sys(g, bed_layers(g, m));
    const size_t stride[3] = { g.dims[1] * g.dims[2], g.dims[2], 1 };
    for (size_t id = 0; id < g.nodes(); ++id) {
        const Ijk    c     = ijk_of(g, id);
        const size_t at[3] = { c.i, c.j, c.k };
        for (int a = 0; a < 3; ++a) {
            if (at[a] + 1 >= g.dims[size_t(a)])
                continue;
            const size_t o = id + stride[a];
            const double t = g.spacing[a] * 0.5 * (d[o][a] + d[id][a]);
            sys.row({ { o, 1. }, { id, -1. } }, double(phi[o]) - double(phi[id]) - t, 1.);
        }
    }
    Sparse A(Eigen::Index(sys.rhs.size()), sys.unknowns);
    A.setFromTriplets(sys.trip.begin(), sys.trip.end());
    const Sparse          M = Sparse(A.transpose()) * A;
    const Eigen::VectorXd b = A.transpose() * Eigen::Map<const Eigen::VectorXd>(sys.rhs.data(), Eigen::Index(sys.rhs.size()));
    Eigen::ConjugateGradient<Sparse, Eigen::Lower | Eigen::Upper> cg;
    cg.setTolerance(1e-8);
    cg.setMaxIterations(2000);
    cg.compute(M);
    const Eigen::VectorXd delta = cg.solve(b);
    for (size_t id = 0; id < phi.size(); ++id)
        if (sys.col[id] >= 0)
            phi[id] = float(double(phi[id]) + delta[sys.col[id]]);
}

std::vector<double> smooth_column(const ObjectiveGeometry &g, const ObjectiveMachine &m, const std::vector<double> &col)
{
    const size_t n = g.dims[2], k0 = std::min(n - 1, bed_layers(g, m));
    if (n < 3 || k0 + 2 >= n)
        return col;
    const double        sz = g.spacing.z(), dmin = sz / m.thickness_max, dmax = sz / m.thickness_min;
    std::vector<double> d(n - 1);
    for (size_t k = 0; k + 1 < n; ++k)
        d[k] = std::clamp((col[k + 1] - col[k]) / m.h, dmin, dmax);
    auto ratio_ok = [&](double a, double b) {
        const double ta = sz / a, tb = sz / b, c = std::min(LayerChangeMax, LayerChangeAbs / std::min(ta, tb));
        return std::max(ta, tb) <= std::min(ta, tb) * (1. + c) + 1e-9;
    };
    std::vector<int> held_at;
    std::vector<double> held_v;
    for (size_t k = k0; k < n; ++k)
        if (k == k0 || m.pinned(k)) {
            held_at.push_back(int(k));
            held_v.push_back(k == k0 ? col[k] : m.pinned_v[k]);
        }
    auto project = [&]() {
        for (size_t r = 0; r + 1 < held_at.size(); ++r) {
            const size_t a = size_t(held_at[r]), b = size_t(held_at[r + 1]);
            double       sum = 0.;
            for (size_t k = a; k < b; ++k)
                sum += d[k];
            const double want = (held_v[r + 1] - held_v[r]) / m.h;
            if (sum > 1e-12 && want > 0.)
                for (size_t k = a; k < b; ++k)
                    d[k] *= want / sum;
        }
    };
    project();
    for (int it = 0; it < 4000; ++it) {
        bool ok = true;
        for (size_t k = std::max<size_t>(k0, 1); k < d.size(); ++k)
            if (!ratio_ok(d[k - 1], d[k])) {
                ok = false;
                break;
            }
        if (ok)
            break;
        std::vector<double> next = d;
        for (size_t k = k0; k < d.size(); ++k) {
            const double left = k > 0 ? d[k - 1] : d[k], right = k + 1 < d.size() ? d[k + 1] : d[k];
            next[k] = d[k] + 0.25 * (left - 2. * d[k] + right);
        }
        d.swap(next);
        project();
    }
    std::vector<double> out(col);
    for (size_t k = k0; k + 1 < n; ++k)
        out[k + 1] = out[k] + m.h * d[k];
    return out;
}

std::vector<double> hold_column(const ObjectiveGeometry &g, const ObjectiveMachine &m, const std::vector<double> &col);

std::vector<float> column_offer(const ObjectiveGeometry &g, const ObjectiveMachine &m, const std::vector<double> &col)
{
    std::vector<double> out = hold_column(g, m, smooth_layers() ? smooth_column(g, m, col) : col);
    for (int round = 1; smooth_layers() && round < 3; ++round)
        out = hold_column(g, m, smooth_column(g, m, out));
    std::vector<float> f(g.nodes());
    for (size_t id = 0; id < g.nodes(); ++id)
        f[id] = float(out[ijk_of(g, id).k]);
    return f;
}

std::vector<double> hold_column(const ObjectiveGeometry &g, const ObjectiveMachine &m, const std::vector<double> &col)
{
    const size_t         n = g.dims[2];
    std::vector<double>  held_v(n, 0.), out(col);
    std::vector<uint8_t> held(n, 0);
    for (size_t k = 0; k < n; ++k) {
        if (k <= bed_layers(g, m))
            held_v[k] = m.anchor.size() == g.nodes() ? double(m.anchor[g.index(0, 0, k)]) : g.origin.z() + double(k) * g.spacing.z(), held[k] = 1;
        else if (m.pinned(k))
            held_v[k] = m.pinned_v[k], held[k] = 1;
    }
    for (size_t a = 0; a < n;) {
        size_t b = a + 1;
        while (b < n && !held[b])
            ++b;
        out[a] = held_v[a];
        if (b < n) {
            const double span = col[b] - col[a];
            for (size_t k = a + 1; k < b; ++k)
                out[k] = held_v[a] + (span > 1e-9 ? (col[k] - col[a]) / span : double(k - a) / double(b - a)) * (held_v[b] - held_v[a]);
        } else
            for (size_t k = a + 1; k < n; ++k)
                out[k] = held_v[a] + (col[k] - col[a]);
        a = b;
    }
    return out;
}

struct AdaptiveOffer
{
    std::vector<double> A, B;
    std::vector<double> column(const ObjectiveGeometry &g, const ObjectiveMachine &m, double w_t, double lam) const
    {
        const double        gmin = m.h / m.thickness_max, gmax = m.h / m.thickness_min, sz = g.spacing.z();
        std::vector<double> col(g.dims[2]);
        col[0] = g.origin.z();
        for (size_t k = 0; k + 1 < g.dims[2]; ++k)
            col[k + 1] = col[k] + sz * std::clamp(std::cbrt(2. * A[k] / ((w_t + lam) * std::max(B[k], 1e-9))), gmin, gmax);
        return col;
    }
};

AdaptiveOffer adaptive_offer(const ObjectiveGeometry &g, const ObjectiveMachine &m, const ObjectiveWeights &w, double other)
{
    const size_t nk = g.dims[2] - 1;
    const double sz = g.spacing.z(), er = e_ref(m);
    AdaptiveOffer o;
    o.A.assign(nk, 0.);
    o.B.assign(nk, 0.);
    for (size_t s = 0; s < g.p.size(); ++s) {
        const double nz  = std::abs(g.n[s].z());
        const int    c   = g.cls[s];
        const double err = std::pow(m.h * nz / std::sqrt(12.), 2.) + (c == 1 ? std::pow(WallK * m.h / 2., 2.) * (1. - nz * nz) : 0.);
        const size_t k   = std::min(nk - 1, size_t(std::max(0., std::floor((g.p[s].z() - g.origin.z()) / sz))));
        o.A[k] += (c == 0 ? w.up : c == 1 ? w.wall : w.down) * g.a[s] * err / (er * er);
    }
    const double        wd = m.line_width, V = g.spacing.prod(), cap = m.max_volumetric / (wd * m.h);
    std::vector<double> node_t(g.dims[2], 0.), count(g.dims[2], 0.);
    for (size_t id = 0; id < g.nodes(); ++id)
        if (g.occ[id]) {
            const double fs = g.shell[id];
            count[ijk_of(g, id).k] += 1.;
            node_t[ijk_of(g, id).k] += m.road_factor * V / (m.h * wd) *
                                       (fs / std::min(m.shell_speed, cap) + (1. - fs) * m.infill_density / std::min(m.sparse_speed, cap));
        }
    double occupied = 0.;
    for (const double c : count)
        occupied += c;
    for (size_t k = 0; k < g.dims[2]; ++k)
        node_t[k] += other * count[k] / std::max(occupied, 1.);
    for (size_t k = 0; k < nk; ++k)
        o.B[k] = 0.5 * (node_t[k] + node_t[k + 1]);
    return o;
}

}

ObjectiveTerms solve_objective(const ObjectiveGeometry &g, const ObjectiveMachine &machine, const ObjectiveWeights &w,
                               const ObjectiveParams &params, std::vector<float> &phi, const std::function<void()> &cancel,
                               const std::function<void(const std::string &)> &log, ObjectiveTerms *given)
{
    EvaluationCache eval_cache(g);
    auto say = [&log](const std::string &s) {
        if (log)
            log(s);
    };
    const auto t_start = std::chrono::steady_clock::now();
    auto       spent   = [&]() {
        return params.budget > 0. && std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count() > params.budget;
    };
    std::vector<float> planar(g.nodes());
    for (size_t id = 0; id < g.nodes(); ++id)
        planar[id] = machine.anchor.size() == g.nodes() ? machine.anchor[id] : float(node_pos(g, id).z());
    ObjectiveMachine m = machine;
    if (m.time_base_zonly <= 0. || m.time_base_curved <= 0.) {
        ObjectiveMachine m_planar = m;
        m_planar.infill_combine   = 0.;
        const ObjectiveTerms tp      = evaluate_objective_cached(g, eval_cache, m_planar, w, planar, false);
        const double         support = support_time(g, m);
        if (m.time_base_zonly <= 0.) {
            m.time_base_zonly    = m.time_zonly(tp) + support;
            m.time_support_zonly = support;
        }
        if (m.time_base_curved <= 0.)
            m.time_base_curved = m.time_curved(tp) + support;
    }
    double limit = m.tilt_max;
    if (!m.rotary)
        limit = *std::min_element(m.slope_cap.begin(), m.slope_cap.end());
    const double start_tilt = std::min(params.start_tilt, 0.85 * limit), local_tilt = std::min(params.local_tilt, 0.85 * limit);
    auto report = [&](const std::string &tag, const ObjectiveTerms &t) {
        std::ostringstream line;
        line << tag << ": J " << t.J << " S " << t.S << " E " << t.E << " (up " << t.E_up << " wall " << t.E_wall << " down " << t.E_down
             << ") T " << t.T << " (roads " << t.T_roads << ", travel " << t.T_travel << ", axis " << t.T_axis << ", unwind " << t.T_unwind << ", skin " << t.T_skin
             << ") H " << t.H_thickness << "/" << t.H_slope << "/" << t.H_head << "/" << t.H_bed << "/" << t.H_time << (t.feasible() ? " feasible" : "");
        say(line.str());
    };
    auto project = [&](std::vector<float> &f) {
        double             lo = 0., hi = 1.;
        std::vector<float> trial(f.size());
        auto blend = [&](double a) {
            for (size_t id = 0; id < f.size(); ++id)
                trial[id] = float(double(planar[id]) + a * (double(f[id]) - double(planar[id])));
        };
        for (int b = 0; b < 8; ++b) {
            const double mid = 0.5 * (lo + hi);
            blend(mid);
            (evaluate_objective_cached(g, eval_cache, m, w, trial, true).feasible() ? lo : hi) = mid;
        }
        blend(lo);
        f = std::move(trial);
        return lo;
    };
    auto add_pairs = [](std::vector<HeadPair> &to, const std::vector<HeadPair> &from) {
        for (const HeadPair &hp : from)
            if (std::none_of(to.begin(), to.end(), [&hp](const HeadPair &o) { return o.node == hp.node; }))
                to.push_back(hp);
    };
    auto descend = [&](std::vector<float> &f, int iterations, const std::string &tag) {
        std::vector<HeadPair> pairs, struck;
        ObjectiveTerms        t = evaluate_objective_cached(g, eval_cache, m, w, f, true, &pairs);
        std::string           how;
        if (!t.feasible()) {
            report(tag + " (before projection)", t);
            how   = " (projected toward planar, blend " + std::to_string(project(f)) + ")";
            pairs.clear();
            t = evaluate_objective_cached(g, eval_cache, m, w, f, true, &pairs);
        }
        double             damping = 1e-2, cost = 0.;
        std::vector<float> trial;
        for (int it = 0; it < iterations && !spent(); ++it) {
            if (cancel)
                cancel();
            const double damping_before = damping;
            const size_t pairs_before = pairs.size();
            const Surrogate sur(g, m, w, params, pairs, eval_cache);
            trial = f;
            if (!step(sur, trial, cost, damping)) {
                if (damping >= 1e4)
                    break;
                continue;
            }
            struck.clear();
            const ObjectiveTerms tt = evaluate_objective_cached(g, eval_cache, m, w, trial, true, &struck);
            add_pairs(pairs, struck);
            if (tt.feasible() && tt.J < t.J) {
                f.swap(trial);
                t = tt;
            } else {
                damping = std::min(1e4, damping * 10.);
                if (pairs.size() == pairs_before && damping == damping_before) {
                    say(tag + " fixed-point exit at iteration " + std::to_string(it + 1));
                    break;
                }
            }
        }
        report(tag + how, t);
        return t;
    };
    auto better = [](const ObjectiveTerms &a, const ObjectiveTerms &b) { return a.feasible() != b.feasible() ? a.feasible() : a.J < b.J; };
    std::vector<float> best   = phi;
    ObjectiveTerms     best_t = evaluate_objective_cached(g, eval_cache, m, w, best, true);
    report("given", best_t);
    if (given)
        *given = best_t;
    std::vector<std::pair<std::string, double>> offers;
    auto note_offer = [&offers](const std::string &name, const ObjectiveTerms &t, bool any) {
        offers.emplace_back("stat_offer_J_" + name, any ? t.J : std::numeric_limits<double>::quiet_NaN());
        offers.emplace_back("stat_offer_feasible_" + name, any && t.feasible() ? 1. : 0.);
    };
    note_offer("anchor", best_t, true);
    ObjectiveMachine   m_sep = m;
    m_sep.infill_combine     = 0.;
    std::vector<float> best_sep;
    ObjectiveTerms     best_sep_t;
    std::vector<std::pair<std::string, ObjectiveTerms>> sep_offers;
    std::vector<TimeCutOffer> time_cut;
    auto note_time_cut = [&](const std::string &name, const std::vector<float> &f, const ObjectiveTerms &tf) {
        if (params.time_cut && tf.H_time > 0 && tf.H_thickness == 0 && tf.H_slope == 0 && tf.H_head == 0 && tf.H_bed == 0 &&
            tf.T_ratio <= tf.T_cap * params.time_cut_band)
            time_cut.push_back({ name, f, tf });
    };
    auto offer_sep = [&](const std::string &name, const std::vector<float> &f) {
        ObjectiveTerms tf = evaluate_objective_cached(g, eval_cache, m_sep, w, f, true);
        tf.infill_separate = true;
        note_time_cut(name + "_sep", f, tf);
        auto it = std::find_if(sep_offers.begin(), sep_offers.end(), [&name](const auto &o) { return o.first == name; });
        if (it == sep_offers.end())
            sep_offers.emplace_back(name, tf);
        else if (tf.feasible() && (!it->second.feasible() || tf.J < it->second.J))
            it->second = tf;
        if (tf.feasible() && (best_sep.empty() || tf.J < best_sep_t.J))
            best_sep = f, best_sep_t = tf;
    };
    if (params.only_offer == "anchor") {
        phi           = best;
        best_t.offers = std::move(offers);
        return best_t;
    }
    {
        ObjectiveTerms best_u;
        bool           any_u = false;
        std::vector<double> heights;
        for (int i = 2; i <= 7; ++i)
            if (const double t = 0.04 * i; t >= m.thickness_min - 1e-9 && t <= m.thickness_max + 1e-9)
                heights.push_back(t);
        const size_t users = heights.size();
        for (double t = m.thickness_min; t <= m.thickness_max + 1e-9; t += 0.02)
            if (std::none_of(heights.begin(), heights.begin() + users, [t](double u) { return std::abs(u - t) < 0.005; }))
                heights.push_back(t);
        std::sort(heights.begin(), heights.end());
        for (const double t : heights) {
            std::vector<double> col(g.dims[2]);
            for (size_t k = 0; k < g.dims[2]; ++k) {
                const double z = g.origin.z() + double(k) * g.spacing.z();
                col[k]         = z <= m.first_layer ? z : m.first_layer + (z - m.first_layer) * m.h / t;
            }
            std::vector<float>   f  = column_offer(g, m, col);
            const ObjectiveTerms tf = evaluate_objective_cached(g, eval_cache, m, w, f, true);
            note_time_cut("uniform " + Slic3r::float_to_string_decimal_point(t, 2), f, tf);
            offer_sep("uniform", f);
            if (tf.feasible() && (!any_u || tf.J < best_u.J))
                best_u = tf, any_u = true;
            if (tf.feasible() && better(tf, best_t)) {
                best   = std::move(f);
                best_t = tf;
                report("uniform " + std::to_string(t).substr(0, 4) + " mm", best_t);
            }
        }
        note_offer("uniform", best_u, any_u);
    }
    {
        std::vector<float> zf(g.nodes());
        for (size_t id = 0; id < g.nodes(); ++id)
            zf[id] = float(node_pos(g, id).z());
        const ObjectiveTerms tz = evaluate_objective_cached(g, eval_cache, m, w, zf, false);
        const AdaptiveOffer  ao = adaptive_offer(g, m, w, tz.T - tz.T_roads);
        auto at = [&](double lam, std::vector<float> &f) {
            f = column_offer(g, m, ao.column(g, m, w.time, lam));
            return evaluate_objective_cached(g, eval_cache, m, w, f, true);
        };
        std::vector<float> fa;
        ObjectiveTerms     ta = at(0., fa);
        note_time_cut("adaptive", fa, ta);
        if (!ta.feasible()) {
            double lo = 0., hi = w.time;
            bool   found = false;
            for (int i = 0; i < 12 && !found; ++i, hi *= 4.) {
                std::vector<float>   fh;
                const ObjectiveTerms th = at(hi, fh);
                if (th.feasible()) {
                    fa = std::move(fh), ta = th, found = true;
                    break;
                }
                lo = hi;
            }
            for (int i = 0; found && i < 10; ++i) {
                const double         mid = 0.5 * (lo + hi);
                std::vector<float>   fm;
                const ObjectiveTerms tm  = at(mid, fm);
                if (tm.feasible())
                    hi = mid, fa = std::move(fm), ta = tm;
                else
                    lo = mid;
            }
        }
        note_offer("adaptive", ta, true);
        report("offer adaptive", ta);
        offer_sep("adaptive", fa);
        if (params.only_offer == "adaptive") {
            phi       = std::move(fa);
            ta.offers = std::move(offers);
            return ta;
        }
        if (ta.feasible() && better(ta, best_t)) {
            best   = std::move(fa);
            best_t = ta;
        }
    }
    for (const auto &[name, field] : params.native_offers) {
        if (field.size() != g.nodes())
            continue;
        std::vector<double> col(g.dims[2]);
        for (size_t k = 0; k < g.dims[2]; ++k)
            col[k] = double(field[g.index(g.dims[0] / 2, g.dims[1] / 2, k)]);
        std::vector<float>   f  = column_offer(g, m, col);
        const ObjectiveTerms tf = evaluate_objective_cached(g, eval_cache, m, w, f, true);
        note_offer(name, tf, true);
        note_time_cut(name, f, tf);
        offer_sep(name, f);
        if (tf.feasible() && better(tf, best_t)) {
            best   = std::move(f);
            best_t = tf;
            report("offer " + name, best_t);
        }
    }
    if (!params.planar_adaptive && params.warm_adaptive) {
        ObjectiveParams pa = params;
        pa.planar_adaptive = true;
        pa.budget          = params.budget > 0. ? 0.25 * params.budget : 0.;
        std::vector<float>   f  = best;
        const ObjectiveTerms ta = solve_objective(g, machine, w, pa, f, cancel, [&say](const std::string &line) { say("adaptive " + line); });
        if (better(ta, best_t)) {
            best   = std::move(f);
            best_t = ta;
        }
    }
    const std::vector<float> incumbent = best;
    if (params.multi_start && !params.planar_adaptive) {
        Vec2d  centre = Vec2d::Zero();
        double count  = 0.;
        for (size_t id = 0; id < g.nodes(); ++id)
            if (g.occ[id]) {
                centre += node_pos(g, id).head<2>();
                ++count;
            }
        centre /= std::max(count, 1.);
        std::vector<std::pair<std::string, std::function<Vec2d(const Vec3d &)>>> fields;
        fields.emplace_back("upright", [](const Vec3d &) { return Vec2d(Vec2d::Zero()); });
        for (int a = 0; a < 8; ++a) {
            const Vec2d u(std::cos(a * M_PI / 4.), std::sin(a * M_PI / 4.));
            fields.emplace_back("azimuth " + std::to_string(45 * a), [u](const Vec3d &) { return u; });
        }
        for (const double sign : { 1., -1. })
            fields.emplace_back(sign > 0. ? "radial out" : "radial in", [centre, sign](const Vec3d &q) {
                const Vec2d r = q.head<2>() - centre;
                return r.norm() > 1e-6 ? Vec2d(sign * r.normalized()) : Vec2d(Vec2d::Zero());
            });
        if (params.max_starts >= 0 && size_t(params.max_starts) < fields.size())
            fields.resize(size_t(params.max_starts));
        struct Start
        {
            std::string        name;
            std::vector<float> phi;
            ObjectiveTerms     t;
        };
        std::vector<Start> tried;
        for (const auto &[name, field] : fields) {
            if (spent())
                break;
            std::vector<Vec3d> d(g.nodes(), Vec3d::UnitZ());
            for (size_t id = 0; id < g.nodes(); ++id) {
                const Vec3d  q   = node_pos(g, id);
                const double t   = std::clamp((q.z() - m.first_layer) / 5., 0., 1.);
                const double tau = start_tilt * t * t * (3. - 2. * t);
                const Vec2d  u   = field(q);
                if (u.squaredNorm() > 1e-12)
                    d[id] = Vec3d(std::sin(tau) * u.x(), std::sin(tau) * u.y(), std::cos(tau));
            }
            std::vector<float> tilt = planar;
            integrate(g, m, d, tilt);
            std::vector<float> f = incumbent;
            for (size_t id = 0; id < f.size(); ++id)
                f[id] = float(double(f[id]) + double(tilt[id]) - double(planar[id]));
            const ObjectiveTerms t = descend(f, params.lottery_iterations, "start " + name + " (lottery)");
            tried.push_back({ name, std::move(f), t });
        }
        std::sort(tried.begin(), tried.end(), [&better](const Start &a, const Start &b) { return better(a.t, b.t); });
        for (size_t k = 0; k < std::min<size_t>(tried.size(), size_t(params.lottery_keep)) && !spent(); ++k) {
            const ObjectiveTerms t = descend(tried[k].phi, params.start_iterations, "start " + tried[k].name);
            if (better(t, best_t)) {
                best   = tried[k].phi;
                best_t = t;
            }
        }
    }
    for (int round = 0; round < (params.planar_adaptive ? 0 : params.local_rounds) && !spent(); ++round) {
        const std::vector<Vec3d> G     = node_gradients(g, best);
        const double             sin_t = std::sin(m.printable_overhang);
        std::vector<Vec3d>       hang;
        for (size_t s = 0; s < g.p.size(); ++s) {
            const Vec3d gs = interp(g, g.p[s], [&G](size_t id) { return G[id]; });
            if (-g.n[s].dot(gs) / std::max(gs.norm(), 1e-12) > sin_t)
                hang.push_back(g.p[s]);
        }
        if (hang.empty())
            break;
        Vec3d centre = Vec3d::Zero();
        for (const Vec3d &q : hang)
            centre += q / double(hang.size());
        std::vector<double> near(g.nodes(), std::numeric_limits<double>::infinity());
        for (size_t id = 0; id < g.nodes(); ++id) {
            const Vec3d q = node_pos(g, id);
            for (const Vec3d &h : hang)
                near[id] = std::min(near[id], (q - h).squaredNorm());
        }
        bool improved = false;
        {
            std::vector<Vec3d> lean_dir;
            std::vector<double> lean_tilt;
            std::vector<Vec3d> lean_at;
            for (size_t s = 0; s < g.p.size(); ++s) {
                const Vec3d  gs = interp(g, g.p[s], [&G](size_t id) { return G[id]; });
                const Vec3d  dn = gs.norm() > 1e-12 ? Vec3d(gs.normalized()) : Vec3d(Vec3d::UnitZ());
                const double c  = -g.n[s].dot(dn);
                const Vec2d  nh = g.n[s].head<2>();
                if (c <= sin_t || nh.norm() < 1e-6)
                    continue;
                const double tau = std::min(m.rotary ? m.tilt_max : 0., std::asin(std::min(c, 1.)) - std::asin(sin_t) + 5. * M_PI / 180.);
                if (tau <= 0.)
                    continue;
                const Vec2d u = nh.normalized();
                lean_at.push_back(g.p[s]);
                lean_dir.emplace_back(std::sin(tau) * u.x(), std::sin(tau) * u.y(), std::cos(tau));
                lean_tilt.push_back(tau);
            }
            if (!lean_at.empty() && !spent()) {
                std::vector<Vec3d> d(g.nodes());
                const double       r2 = 2. * params.local_radius * params.local_radius;
                for (size_t id = 0; id < g.nodes(); ++id) {
                    const Vec3d q  = node_pos(g, id);
                    const Vec3d gb = G[id].norm() > 1e-12 ? Vec3d(G[id].normalized()) : Vec3d(Vec3d::UnitZ());
                    if (q.z() <= m.first_layer + g.spacing.z()) {
                        d[id] = gb;
                        continue;
                    }
                    Vec3d  acc = Vec3d::Zero();
                    double wsum = 0.;
                    for (size_t i = 0; i < lean_at.size(); ++i) {
                        const double w = std::exp(-(q - lean_at[i]).squaredNorm() / r2);
                        if (w > 1e-4) {
                            acc += w * lean_dir[i];
                            wsum += w;
                        }
                    }
                    const double wgt = std::min(wsum, 1.);
                    d[id]            = wsum > 0. ? Vec3d(((1. - wgt) * gb + wgt * (acc / wsum)).normalized()) : gb;
                }
                std::vector<float> f = best;
                integrate(g, m, d, f);
                const ObjectiveTerms t = descend(f, params.start_iterations, "local support-free");
                if (better(t, best_t)) {
                    best     = std::move(f);
                    best_t   = t;
                    improved = true;
                }
            }
        }
        for (int a = 0; a < 10 && !spent(); ++a) {
            std::vector<Vec3d> d(g.nodes());
            for (size_t id = 0; id < g.nodes(); ++id) {
                const Vec3d q  = node_pos(g, id);
                const Vec3d gb = G[id].norm() > 1e-12 ? Vec3d(G[id].normalized()) : Vec3d(Vec3d::UnitZ());
                Vec2d       u  = a < 8 ? Vec2d(std::cos(a * M_PI / 4.), std::sin(a * M_PI / 4.)) : Vec2d((q - centre).head<2>());
                if (a == 9)
                    u = -u;
                u = u.norm() > 1e-9 ? Vec2d(u.normalized()) : Vec2d(Vec2d::UnitX());
                const double wgt = q.z() <= m.first_layer + g.spacing.z() ? 0. : std::exp(-near[id] / (2. * params.local_radius * params.local_radius));
                const Vec3d  lean(std::sin(local_tilt) * u.x(), std::sin(local_tilt) * u.y(), std::cos(local_tilt));
                d[id] = ((1. - wgt) * gb + wgt * lean).normalized();
            }
            std::vector<float> f = best;
            integrate(g, m, d, f);
            const std::string    name = a < 8 ? "local azimuth " + std::to_string(45 * a) : a == 8 ? "local radial out" : "local radial in";
            const ObjectiveTerms t    = descend(f, params.start_iterations, name);
            if (better(t, best_t)) {
                best     = std::move(f);
                best_t   = t;
                improved = true;
            }
        }
        if (!improved)
            break;
    }
    std::vector<float>   polished = best;
    ObjectiveTerms       t        = descend(polished, params.planar_adaptive ? 4 * params.iterations : params.iterations, "polished");
    if (better(best_t, t)) {
        polished = std::move(best);
        t        = best_t;
    }
    auto ramp = [&](std::vector<float> &f, double z0, double c) {
        double zb = std::numeric_limits<double>::infinity();
        for (size_t kb = 0; kb < g.dims[2]; ++kb)
            if (m.pinned(kb) && g.origin.z() + double(kb) * g.spacing.z() > z0) {
                zb = g.origin.z() + double(kb) * g.spacing.z();
                break;
            }
        const double r = std::min(params.level_ramp, 0.5 * (zb - z0));
        if (r < g.spacing.z())
            return false;
        auto smooth = [](double x) { x = std::clamp(x, 0., 1.); return x * x * (3. - 2. * x); };
        for (size_t id = 0; id < g.nodes(); ++id) {
            const double z = node_pos(g, id).z();
            const double x = smooth((z - z0) / r) * (std::isfinite(zb) ? 1. - smooth((z - (zb - r)) / r) : 1.);
            if (x > 0. && ijk_of(g, id).k > bed_layers(g, m) && !m.pinned(ijk_of(g, id).k))
                f[id] = float(double(f[id]) + c * x);
        }
        return true;
    };
    for (int round = 0; params.level_steps > 1 && round < params.level_rounds && !spent(); ++round) {
        if (round > 0) {
            std::vector<float> again = polished;
            const ObjectiveTerms ta  = descend(again, params.iterations, "polished again");
            if (better(ta, t)) {
                polished = std::move(again);
                t        = ta;
            }
        }
        double top = m.first_layer;
        for (size_t id = 0; id < g.nodes(); ++id)
            if (g.occ[id])
                top = std::max(top, node_pos(g, id).z());
        std::vector<double> heights;
        for (double z0 = m.first_layer; z0 < top; z0 += 1.)
            heights.push_back(z0);
        int kept = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (const double z0 : heights) {
                if (spent())
                    break;
                if (cancel)
                    cancel();
                for (int k = 1 - params.level_steps / 2 + 1; k <= params.level_steps / 2; ++k) {
                    if (k == 0)
                        continue;
                    std::vector<float> f = polished;
                    if (!ramp(f, z0, k * m.h / params.level_steps))
                        continue;
                    const ObjectiveTerms tf = evaluate_objective_cached(g, eval_cache, m, w, f, true);
                    if (better(tf, t)) {
                        polished = std::move(f);
                        t        = tf;
                        ++kept;
                    }
                }
            }
        report("levels round " + std::to_string(round) + " (" + std::to_string(kept) + " moves over " + std::to_string(heights.size()) + " heights, two passes)", t);
        if (kept == 0)
            break;
    }
    if (t.spread < ZOnlySpread && std::isfinite(g.mesh_max.z())) {
        std::vector<std::pair<double, double>> up;
        for (size_t s = 0; s < g.p.size(); ++s)
            if (g.n[s].z() > Cos5)
                up.emplace_back(g.p[s].z() - g.mesh_min.z(), g.a[s]);
        std::sort(up.begin(), up.end());
        std::vector<double> flats;
        for (size_t i = 0, j = 0; i < up.size(); i = j + 1) {
            double a = up[i].second, za = up[i].first * up[i].second;
            for (j = i; j + 1 < up.size() && up[j + 1].first - up[j].first < 0.02; ++j)
                a += up[j + 1].second, za += up[j + 1].first * up[j + 1].second;
            if (a >= 100. && za / a > m.first_layer + 1e-3)
                flats.push_back(za / a);
        }
        auto off = [&](const std::vector<float> &f, double zf, double *th) {
            const std::vector<double> tops = native_tops(g, m, f);
            size_t jj = std::min(size_t(std::lower_bound(tops.begin(), tops.end(), zf) - tops.begin()), tops.size() - 1);
            if (jj > 0 && zf - tops[jj - 1] < tops[jj] - zf)
                --jj;
            if (th)
                *th = tops[jj] - (jj > 0 ? tops[jj - 1] : 0.);
            return tops[jj] - zf;
        };
        int landed = 0;
        for (const double zf : flats) {
            double th = m.h;
            const double d0 = off(polished, zf, &th);
            if (std::abs(d0) <= 0.01 || th <= 0.)
                continue;
            const double z0 = std::max(m.first_layer, zf - params.level_ramp);
            double c0 = 0., e0 = d0, c1 = d0 * m.h / th, e1 = 0.;
            std::vector<float> f;
            for (int it = 0; it < 2; ++it) {
                f = polished;
                if (!ramp(f, z0, c1))
                    break;
                e1 = off(f, zf, nullptr);
                if (std::abs(e1) <= 0.002 || std::abs(e1 - e0) < 1e-9)
                    break;
                const double c2 = c1 - e1 * (c1 - c0) / (e1 - e0);
                c0 = c1, e0 = e1, c1 = c2;
            }
            if (std::abs(off(f, zf, nullptr)) > 0.01)
                continue;
            const ObjectiveTerms tf = evaluate_objective_cached(g, eval_cache, m, w, f, true);
            if (better(tf, t)) {
                polished = std::move(f);
                t        = tf;
                ++landed;
            }
        }
        if (!flats.empty())
            report("flats landed " + std::to_string(landed) + " of " + std::to_string(flats.size()), t);
    }
    for (const auto &[name, ts] : sep_offers)
        note_offer(name + "_sep", ts, true);
    if (!best_sep.empty() && better(best_sep_t, t)) {
        polished = std::move(best_sep);
        t        = best_sep_t;
        report("per-layer infill offer", t);
    }
    for (size_t i = 0; i < params.exact_natives.size(); ++i) {
        const auto &[name, field] = params.exact_natives[i];
        if (field.size() != g.nodes())
            continue;
        ObjectiveMachine mx = m;
        mx.anchor           = field;
        mx.pinned_v.clear();
        for (int sep = 0; sep < 2; ++sep) {
            if (sep)
                mx.infill_combine = 0.;
            ObjectiveTerms tx  = evaluate_objective_cached(g, eval_cache, mx, w, field, true);
            tx.infill_separate = sep == 1;
            tx.exact_native    = int(i);
            const std::string tag = name + (sep ? "_sep" : "");
            note_offer(tag, tx, true);
            note_time_cut(tag, field, tx);
            if (tx.feasible() && better(tx, t)) {
                polished = field;
                t        = tx;
                report("exact native " + tag, t);
            }
        }
    }
    if (params.time_cut) {
        std::vector<TimeCutOffer> keep;
        for (TimeCutOffer &c : time_cut)
            if (c.terms.J < t.J)
                keep.push_back(std::move(c));
        std::stable_sort(keep.begin(), keep.end(), [](const TimeCutOffer &a, const TimeCutOffer &b) { return a.terms.J < b.terms.J; });
        if (keep.size() > 2)
            keep.resize(2);
        for (TimeCutOffer &c : keep) {
            c.terms.offers = offers;
            report("time-cut native " + c.name + " (ratio " + Slic3r::float_to_string_decimal_point(c.terms.T_ratio, 3) + ")", c.terms);
        }
        *params.time_cut = std::move(keep);
    }
    if (smooth_layers() && params.planar_adaptive && t.exact_native < 0) {
        std::vector<double> col(g.dims[2]);
        for (size_t k = 0; k < g.dims[2]; ++k)
            col[k] = double(polished[g.index(g.dims[0] / 2, g.dims[1] / 2, k)]);
        std::vector<float> f = column_offer(g, t.infill_separate ? m_sep : m, col);
        ObjectiveTerms     tf = evaluate_objective_cached(g, eval_cache, t.infill_separate ? m_sep : m, w, f, true);
        tf.infill_separate    = t.infill_separate;
        if (tf.feasible() && tf.T <= t.T * SmoothTimeMax) {
            polished = std::move(f);
            t        = tf;
            report("layers smoothed", t);
        } else
            report(tf.feasible() ? "layers smoothed: over the time allowance, kept unsmoothed" : "layers smoothed: infeasible, kept unsmoothed", tf);
    }
    phi      = std::move(polished);
    t.offers = std::move(offers);
    return t;
}

std::vector<std::vector<float>> overhang_local_leans(const ObjectiveGeometry &g, const ObjectiveMachine &m, const std::vector<float> &phi,
                                                     double radius, const std::vector<double> &scales)
{
    std::vector<std::vector<float>> out;
    if (!m.rotary || m.tilt_max <= 0. || phi.size() != g.nodes())
        return out;
    const std::vector<Vec3d> G     = node_gradients(g, phi);
    const double             sin_t = std::sin(m.printable_overhang);
    std::vector<Vec3d>  at, lean;
    std::vector<double> tau;
    std::vector<Vec3d>  walls;
    for (size_t s = 0; s < g.p.size(); ++s) {
        if (std::abs(g.n[s].z()) < std::sin(10. * M_PI / 180.))
            walls.push_back(g.p[s]);
        const Vec3d  gs = interp(g, g.p[s], [&G](size_t id) { return G[id]; });
        const Vec3d  dn = gs.norm() > 1e-12 ? Vec3d(gs.normalized()) : Vec3d(Vec3d::UnitZ());
        const double c  = -g.n[s].dot(dn);
        const Vec2d  nh = g.n[s].head<2>();
        if (c <= sin_t || nh.norm() < 1e-6)
            continue;
        const double t = std::min(m.tilt_max, std::asin(std::min(c, 1.)) - std::asin(sin_t) + 5. * M_PI / 180.);
        if (t <= 0.)
            continue;
        at.push_back(g.p[s]);
        lean.push_back(Vec3d(nh.normalized().x(), nh.normalized().y(), 0.));
        tau.push_back(t);
    }
    if (at.empty())
        return out;
    const double r2 = 2. * radius * radius, w2 = 2. * 1.5 * 1.5;
    for (const double scale : scales) {
        std::vector<Vec3d> d(g.nodes());
        for (size_t id = 0; id < g.nodes(); ++id) {
            const Vec3d q  = node_pos(g, id);
            const Vec3d gb = G[id].norm() > 1e-12 ? Vec3d(G[id].normalized()) : Vec3d(Vec3d::UnitZ());
            d[id]          = gb;
            if (q.z() <= m.first_layer + g.spacing.z())
                continue;
            Vec2d  dir = Vec2d::Zero();
            double tilt = 0., wsum = 0.;
            for (size_t i = 0; i < at.size(); ++i) {
                const double w = std::exp(-(q - at[i]).squaredNorm() / r2);
                if (w > 1e-4) {
                    dir += w * lean[i].head<2>();
                    tilt += w * tau[i];
                    wsum += w;
                }
            }
            if (wsum <= 0. || dir.norm() < 1e-9)
                continue;
            double wall = 0.;
            for (const Vec3d &p : walls)
                wall = std::max(wall, std::exp(-(q - p).squaredNorm() / w2));
            const double w = std::min(wsum, 1.) * (1. - wall);
            const double t = scale * tilt / wsum * w;
            const Vec2d  u = dir.normalized();
            d[id]          = Vec3d(std::sin(t) * u.x(), std::sin(t) * u.y(), std::cos(t));
        }
        std::vector<float> f = phi;
        integrate(g, m, d, f);
        out.push_back(std::move(f));
    }
    return out;
}

}
