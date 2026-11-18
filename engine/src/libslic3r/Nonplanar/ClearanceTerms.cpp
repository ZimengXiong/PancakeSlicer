#include "ClearanceTerms.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "FieldObjective.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <unsupported/Eigen/AutoDiff>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace Slic3r::Nonplanar {
namespace {

constexpr double HeadSlack = 0.1, HeadNorm = 1e-5;
constexpr double AxisNorm = 0.05 * 0.05, AxisTimeNorm = 0.1;
constexpr double BRate = 0.5, CRate = 1., RoadSpeed = 60.;
constexpr double Pi = 3.14159265358979323846;

template<int N> using Dual = Eigen::AutoDiffScalar<Eigen::Matrix<double, N, 1>>;
template<class S> using Vector = Eigen::Matrix<S, 3, 1>;
inline double scalar(double x) { return x; }
template<int N> double scalar(const Dual<N> &x) { return x.value(); }

template<class S> S length(const Vector<S> &v, double floor = 1e-9)
{
    const S n2 = v.squaredNorm();
    using std::sqrt;
    return scalar(n2) <= floor * floor ? S(floor) : S(sqrt(n2));
}

inline double angle(double y, double x) { return std::atan2(y, x); }
template<int N> Dual<N> angle(const Dual<N> &y, const Dual<N> &x)
{
    Dual<N> out(std::atan2(y.value(), x.value()));
    out.derivatives() = (x.value() * y.derivatives() - y.value() * x.derivatives()) /
                        (x.value() * x.value() + y.value() * y.value());
    return out;
}

template<int N> Vector<Dual<N>> variables(const Vec3d &g, int offset)
{
    Vector<Dual<N>> out;
    for (int a = 0; a < 3; ++a) {
        out[a] = Dual<N>(g[a]);
        out[a].derivatives()[offset + a] = 1.;
    }
    return out;
}

struct Pair
{
    size_t i, j;
    int axis;
};

template<class S> S pair_cost(const Vector<S> &gi, const Vector<S> &gj, int axis, double spacing, bool time)
{
    const Vector<S> di = gi / length(gi), dj = gj / length(gj);
    const Vector<S> sum = di + dj;
    const Vector<S> dm = sum / length(sum);
    const S inl_raw = S(1.) - dm[axis] * dm[axis];
    const S inl = scalar(inl_raw) > 0. ? inl_raw : S(0.);
    if (!time)
        return inl * (di - dj).squaredNorm() / (spacing * spacing * AxisNorm);
    using std::sqrt;
    const S ri = sqrt(di.x() * di.x() + di.y() * di.y() + S(1e-8));
    const S rj = sqrt(dj.x() * dj.x() + dj.y() * dj.y() + S(1e-8));
    const S bi = angle(ri, S(di.z())), bj = angle(rj, S(dj.z()));
    const S dx = di.x() / ri - dj.x() / rj, dy = di.y() / ri - dj.y() / rj;
    const S s2 = scalar(ri) <= scalar(rj) ? S(ri * ri) : S(rj * rj);
    const double sin_b0 = std::sin(3. * Pi / 180.);
    const S dc = sqrt(dx * dx + dy * dy + S(1e-8)) * s2 / (s2 + S(sin_b0 * sin_b0));
    const S db = (bi - bj) / BRate;
    const S ctime = dc / CRate;
    const S excess = sqrt(db * db + ctime * ctime + S(1e-12)) * (RoadSpeed / spacing) - S(1.);
    return scalar(excess) > 0. ? S(inl * excess / AxisTimeNorm) : S(0.);
}

class AxisTerm final : public FieldTerm
{
public:
    AxisTerm(const ObjectiveGeometry &g, bool time) : m_time(time)
    {
        const size_t stride[3] = { g.dims[1] * g.dims[2], g.dims[2], 1 };
        for (size_t i = 0; i < g.dims[0]; ++i)
            for (size_t j = 0; j < g.dims[1]; ++j)
                for (size_t k = 0; k < g.dims[2]; ++k) {
                    const size_t id = g.index(i, j, k), at[3] = { i, j, k };
                    if (!g.occ[id])
                        continue;
                    for (int a = 0; a < 3; ++a)
                        if (at[a] + 1 < g.dims[size_t(a)] && g.occ[id + stride[a]])
                            m_pairs.push_back({ id, id + stride[a], a });
                }
    }

    double cost(const ObjectiveGeometry &g, const std::vector<Vec3d> &G, std::vector<Vec3d> *dG) const override
    {
        if (m_pairs.empty())
            return 0.;
        const double scale = 1. / double(m_pairs.size());
        double sum = 0.;
        for (const Pair &p : m_pairs) {
            if (dG) {
                const Dual<6> v = pair_cost(variables<6>(G[p.i], 0), variables<6>(G[p.j], 3), p.axis, g.spacing[p.axis], m_time);
                sum += v.value();
                (*dG)[p.i] += scale * v.derivatives().template head<3>();
                (*dG)[p.j] += scale * v.derivatives().template tail<3>();
            } else
                sum += pair_cost<double>(G[p.i], G[p.j], p.axis, g.spacing[p.axis], m_time);
        }
        return scale * sum;
    }

private:
    std::vector<Pair> m_pairs;
    bool m_time;
};

class HeadTerm final : public FieldTerm
{
public:
    explicit HeadTerm(const ObjectiveGeometry &g)
    {
        for (size_t id = 0; id < g.nodes(); ++id)
            if (g.occ[id])
                m_nodes.push_back(id);
        size_t count = 3000;
        if (const char *env = research_switch("NS4_HEAD_P")) {
            const long value = std::strtol(env, nullptr, 10);
            if (value > 0)
                count = size_t(value);
        }
        if (m_nodes.size() > count) {
            std::mt19937 random(0);
            std::shuffle(m_nodes.begin(), m_nodes.end(), random);
            m_nodes.resize(count);
            std::sort(m_nodes.begin(), m_nodes.end());
        }
        auto ring = [this](double radius, double z) {
            for (int a = 0; a < 12; ++a) {
                const double az = 2. * Pi * double(a) / 12.;
                m_samples.emplace_back(radius * std::cos(az), radius * std::sin(az), z);
            }
        };
        for (int z = 1; z <= 6; ++z)
            ring(0.5 + double(z) * std::tan(Pi / 6.), double(z));
        for (double z : { 7., 8.5, 10. })
            ring(4.3, z);
        for (double radius : { 4.3, 6.3, 8.3, 10.4 })
            ring(radius, 9.7);
    }

    bool needs_field_values() const override { return true; }
    double cost(const ObjectiveGeometry &, const std::vector<Vec3d> &, std::vector<Vec3d> *) const override
    {
        throw std::logic_error("head clearance requires FieldTermField node values");
    }

    double cost_field(const FieldTermField &field, const std::vector<Vec3d> &G,
                      std::vector<double> *dphi, std::vector<Vec3d> *dG) const override
    {
        if (m_nodes.empty())
            return 0.;
        const ObjectiveGeometry &g = field.geometry;
        const double scale = 1. / (double(m_nodes.size()) * double(m_samples.size()) * HeadNorm);
        double sum = 0.;
        for (size_t id : m_nodes) {
            const size_t k = id % g.dims[2], j = (id / g.dims[2]) % g.dims[1], i = id / (g.dims[1] * g.dims[2]);
            const Vec3d p = g.origin + g.spacing.cwiseProduct(Vec3d(double(i), double(j), double(k)));
            const Vector<Dual<3>> gr = variables<3>(G[id], 0);
            const Vector<Dual<3>> d = gr / length(gr);
            Vector<Dual<3>> a;
            if (std::abs(d.z().value()) < 0.9)
                a << Dual<3>(0.), Dual<3>(0.), Dual<3>(1.);
            else
                a << Dual<3>(1.), Dual<3>(0.), Dual<3>(0.);
            const Vector<Dual<3>> cross = a.cross(d);
            const Vector<Dual<3>> e1 = cross / length(cross), e2 = d.cross(e1);
            for (const Vec3d &s : m_samples) {
                const Vector<Dual<3>> q = p.cast<Dual<3>>() + e1 * s.x() + e2 * s.y() + d * s.z();
                Vec3d position;
                Matrix3d jacobian;
                for (int axis = 0; axis < 3; ++axis) {
                    position[axis] = q[axis].value();
                    jacobian.row(axis) = q[axis].derivatives().transpose();
                }
                const FieldTermField::Sample hit = field.sample(position);
                const double r = double(field.phi[id]) + HeadSlack - hit.value;
                if (r <= 0.)
                    continue;
                sum += hit.occupancy * r * r;
                if (dphi) {
                    const double dr = scale * 2. * hit.occupancy * r;
                    (*dphi)[id] += dr;
                    for (size_t corner = 0; corner < 8; ++corner)
                        (*dphi)[hit.nodes[corner]] -= dr * hit.weights[corner];
                }
                if (dG)
                    (*dG)[id] += scale * jacobian.transpose() *
                                (r * r * hit.occupancy_gradient - 2. * hit.occupancy * r * hit.gradient);
            }
        }
        return scale * sum;
    }

private:
    std::vector<size_t> m_nodes;
    std::vector<Vec3d> m_samples;
};

}

std::shared_ptr<const FieldTerm> make_head_term(const ObjectiveGeometry &g) { return std::make_shared<const HeadTerm>(g); }
std::shared_ptr<const FieldTerm> make_axis_term(const ObjectiveGeometry &g) { return std::make_shared<const AxisTerm>(g, false); }
std::shared_ptr<const FieldTerm> make_axist_term(const ObjectiveGeometry &g) { return std::make_shared<const AxisTerm>(g, true); }
}
