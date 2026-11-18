#include "Seams.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <cstddef>
#include <vector>
#include "ShellSeams.hpp"

#include "HeightField.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <cmath>
#include <cstdint>
#include <limits>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <utility>

namespace Slic3r::Nonplanar {

namespace {

Points resample_closed(const Polygon &loop, double step)
{
    Points        out;
    const Points &pts  = loop.points;
    double        pos  = 0.;
    double        next = 0.;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec2d  a = pts[i].cast<double>(), b = pts[(i + 1) % pts.size()].cast<double>();
        const double l = (b - a).norm();
        for (; next < pos + l; next += step)
            out.emplace_back((a + (b - a) * ((next - pos) / l)).cast<coord_t>());
        pos += l;
    }
    if (out.empty() && !pts.empty())
        out.emplace_back(pts.front());
    return out;
}

double dist2_to_segment(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  d = b - a;
    const double t = d.squaredNorm() > 0. ? std::clamp((p - a).dot(d) / d.squaredNorm(), 0., 1.) : 0.;
    return (a + t * d - p).squaredNorm();
}

constexpr double PaintedSeamReach = 10.;

double facet_dist2(const std::vector<SeamPaint::Facet> &facets, const Point &p, double z)
{
    const Vec2d q    = p.cast<double>();
    double      best = std::numeric_limits<double>::max();
    for (const SeamPaint::Facet &f : facets) {
        if (z < f.z_min - 2. || z > f.z_max + 2.)
            continue;
        if (f.xy.contains(p))
            return 0.;
        const Points &v = f.xy.points;
        for (size_t i = 0; i < v.size(); ++i)
            best = std::min(best, dist2_to_segment(q, v[i].cast<double>(), v[(i + 1) % v.size()].cast<double>()));
    }
    return best;
}

bool near_facet(const std::vector<SeamPaint::Facet> &facets, const Point &p, double z, double reach)
{
    return facet_dist2(facets, p, z) <= reach * reach;
}

}

namespace {

bool segments_cross(const Point &a, const Point &b, const Point &c, const Point &d)
{
    auto orient = [](const Point &p, const Point &q, const Point &r) {
        const double v = double(q.x() - p.x()) * double(r.y() - p.y()) - double(q.y() - p.y()) * double(r.x() - p.x());
        return (v > 0.) - (v < 0.);
    };
    return orient(a, b, c) * orient(a, b, d) < 0 && orient(c, d, a) * orient(c, d, b) < 0;
}

}

template<typename Fn> bool RoadGrid::visit(const Point &a, const Point &b, Fn &&fn) const
{
    const int64_t x0 = std::min(a.x(), b.x()) / m_cell - 1, x1 = std::max(a.x(), b.x()) / m_cell + 1;
    const int64_t y0 = std::min(a.y(), b.y()) / m_cell - 1, y1 = std::max(a.y(), b.y()) / m_cell + 1;
    for (int64_t ix = x0; ix <= x1; ++ix)
        for (int64_t iy = y0; iy <= y1; ++iy)
            if (fn((uint64_t(uint32_t(ix)) << 32) | uint32_t(iy)))
                return true;
    return false;
}

void RoadGrid::add(const Point &a, const Point &b)
{
    if (a != b)
        this->visit(a, b, [&](uint64_t key) { m_cells[key].emplace_back(a, b); return false; });
}

void RoadGrid::add(const Polyline &road)
{
    for (size_t i = 0; i + 1 < road.points.size(); ++i)
        this->add(road.points[i], road.points[i + 1]);
}

void RoadGrid::add(const ExtrusionPathConformal &path)
{
    const Points3 &pts = path.polyline.points;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (path.bead_height[i] > 0.f && path.bead_height[i + 1] > 0.f)
            this->add(pts[i].to_point(), pts[i + 1].to_point());
}

bool RoadGrid::crosses(const Point &a, const Point &b) const
{
    return this->visit(a, b, [&](uint64_t key) {
        const auto it = m_cells.find(key);
        if (it != m_cells.end())
            for (const auto &[c, d] : it->second)
                if (segments_cross(a, b, c, d))
                    return true;
        return false;
    });
}

bool RoadGrid::crosses(const Polyline &route) const
{
    for (size_t i = 0; i + 1 < route.points.size(); ++i)
        if (this->crosses(route.points[i], route.points[i + 1]))
            return true;
    return false;
}

SeamPaint seam_paint(const PrintObject &object)
{
    SeamPaint         out;
    const Transform3d trafo    = object.trafo_sliced();
    const double      z_offset = object.slicing_parameters().object_print_z_min;
    for (const ModelVolume *mv : object.model_object()->volumes) {
        if (!mv->is_model_part())
            continue;
        const Transform3d t = trafo * mv->get_matrix();
        for (const EnforcerBlockerType type : { EnforcerBlockerType::ENFORCER, EnforcerBlockerType::BLOCKER }) {
            const indexed_triangle_set its = mv->seam_facets.get_facets_strict(*mv, type);
            for (const stl_triangle_vertex_indices &tri : its.indices) {
                SeamPaint::Facet facet;
                facet.z_min = std::numeric_limits<float>::max();
                facet.z_max = std::numeric_limits<float>::lowest();
                for (int i = 0; i < 3; ++i) {
                    const Vec3d v = t * its.vertices[size_t(tri[i])].cast<double>();
                    facet.xy.points.emplace_back(Point::new_scale(v.x(), v.y()));
                    facet.z_min = std::min(facet.z_min, float(v.z() + z_offset));
                    facet.z_max = std::max(facet.z_max, float(v.z() + z_offset));
                }
                (type == EnforcerBlockerType::ENFORCER ? out.enforcers : out.blockers).emplace_back(std::move(facet));
            }
        }
    }
    return out;
}

Point shell_seam(const ExPolygons &area, const HeightField &surface, SeamPosition seam, unsigned salt, const SeamPaint *paint, double reach)
{
    struct Candidate
    {
        Point  p;
        double z, y, concave;
    };
    std::vector<Candidate> cands;
    const double           step   = scale_(0.5);
    const int              window = 4;
    for (const ExPolygon &expoly : area) {
        Polygon contour = expoly.contour;
        contour.make_counter_clockwise();
        const Points pts = resample_closed(contour, step);
        const int    n   = int(pts.size());
        for (int i = 0; i < n; ++i) {
            const Vec2d  back  = pts[size_t((i - window % n + n) % n)].cast<double>();
            const Vec2d  here  = pts[size_t(i)].cast<double>();
            const Vec2d  ahead = pts[size_t((i + window) % n)].cast<double>();
            const Vec2d  d1 = here - back, d2 = ahead - here;
            const double turn = n >= 3 ? std::atan2(d1.x() * d2.y() - d1.y() * d2.x(), d1.dot(d2)) : 0.;
            const float  s = surface.sample(unscale(pts[size_t(i)]));
            cands.push_back({ pts[size_t(i)], std::isnan(s) ? std::numeric_limits<double>::max() : double(s),
                              unscale<double>(pts[size_t(i)].y()), -turn });
        }
    }
    if (cands.empty())
        return Point(0, 0);

    if (paint != nullptr && !paint->empty()) {
        const double r = scale_(reach);
        std::vector<Candidate> enforced, free;
        for (const Candidate &c : cands) {
            if (near_facet(paint->enforcers, c.p, c.z, r))
                enforced.emplace_back(c);
            else if (!near_facet(paint->blockers, c.p, c.z, r))
                free.emplace_back(c);
        }
        if (enforced.empty() && !paint->enforcers.empty()) {
            std::vector<double> d2(cands.size());
            double              best = scale_(PaintedSeamReach) * scale_(PaintedSeamReach);
            for (size_t i = 0; i < cands.size(); ++i)
                best = std::min(best, d2[i] = facet_dist2(paint->enforcers, cands[i].p, cands[i].z));
            const double limit = std::pow(std::sqrt(best) + r, 2.);
            for (size_t i = 0; i < cands.size(); ++i)
                if (d2[i] <= limit && d2[i] <= scale_(PaintedSeamReach) * scale_(PaintedSeamReach))
                    enforced.emplace_back(cands[i]);
        }
        if (!enforced.empty())
            cands = std::move(enforced);
        else if (!free.empty())
            cands = std::move(free);
    }

    if (seam == spRandom) {
        const unsigned h = (salt + 1u) * 2654435761u;
        return cands[size_t(h % cands.size())].p;
    }
    auto lowest_then_rear = [](const Candidate &a, const Candidate &b) {
        return a.z < b.z - 0.02 || (a.z <= b.z + 0.02 && a.y > b.y);
    };
    if (seam == spRear || seam == spAlignedBack)
        return std::min_element(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) {
                   return a.y > b.y + 0.05 || (a.y >= b.y - 0.05 && a.z < b.z);
               })->p;
    const double deepest = std::max_element(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) {
                               return a.concave < b.concave;
                           })->concave;
    constexpr double Corner = 0.52;
    if (deepest >= Corner) {
        std::vector<Candidate> corners;
        for (const Candidate &c : cands)
            if (c.concave >= 0.8 * deepest)
                corners.emplace_back(c);
        return std::min_element(corners.begin(), corners.end(), lowest_then_rear)->p;
    }
    double y_min = std::numeric_limits<double>::max(), y_max = -y_min;
    for (const Candidate &c : cands) {
        y_min = std::min(y_min, c.y);
        y_max = std::max(y_max, c.y);
    }
    std::vector<double> rear, curvature, z;
    for (const Candidate &c : cands) {
        rear.emplace_back(y_max > y_min ? (c.y - y_min) / (y_max - y_min) : 1.);
        curvature.emplace_back(rear.back() >= 0.75 ? meridian_curvature(surface, seam_line(surface, c.p, area)) : 0.);
        z.emplace_back(c.z);
    }
    return cands[convex_seam(rear, curvature, z)].p;
}

Polyline seam_line(const HeightField &surface, const Point &seam, const ExPolygons &area)
{
    auto trace = [&](double dir) {
        Polyline out;
        out.points.emplace_back(seam);
        const double step = surface.pitch();
        Vec2d        p    = unscale(seam);
        float        z    = surface.sample(p);
        if (std::isnan(z))
            return out;
        for (int i = 0; i < 100000; ++i) {
            const Vec2d g = surface.gradient(p);
            if (std::isnan(g.x()) || std::isnan(g.y()) || g.norm() < 1e-3)
                break;
            const Vec2d q  = p + dir * step * g.normalized();
            const float zq = surface.sample(q);
            const Point pq = Point::new_scale(q.x(), q.y());
            if (std::isnan(zq) || dir * (zq - z) <= 0.f ||
                (i > 0 && std::none_of(area.begin(), area.end(), [&pq](const ExPolygon &e) { return e.contains(pq); })))
                break;
            p = q;
            z = zq;
            out.points.emplace_back(pq);
        }
        return out;
    };
    Polyline up = trace(1.);
    return up.points.size() > 1 ? up : trace(-1.);
}

namespace {

template<typename Fn> bool each_inner_sample(const Vec2d &a, const Vec2d &b, double step, Fn &&fn)
{
    const int k = std::max(1, int(std::ceil((b - a).norm() / step)));
    for (int j = 1; j < k; ++j)
        if (!fn(a + (b - a) * (double(j) / k)))
            return false;
    return true;
}

class GlideRouter
{
public:
    static constexpr double EndSlack = 0.75;

    GlideRouter(const HeightField &grid, const GlideZ &glide_z, double step) : m_grid(grid), m_glide_z(glide_z), m_step(step) {}

    bool route(const Vec2d &a, const Vec2d &b, double cap, Points3 &route)
    {
        route.clear();
        const int nx = m_grid.nx(), ny = m_grid.ny();
        if (nx <= 0 || ny <= 0 || cap <= 0.)
            return false;
        if (m_verdict.empty()) {
            m_verdict.assign(size_t(nx) * ny, Unknown);
            m_g.assign(size_t(nx) * ny, std::numeric_limits<float>::infinity());
            m_parent.assign(size_t(nx) * ny, -1);
        }
        const double pitch = m_grid.pitch(), diagonal = std::sqrt(2.) * pitch;
        auto snaps = [&](const Vec2d &p, std::vector<std::pair<int, float>> &out) {
            out.clear();
            const Vec2d o = (p - m_grid.origin()) / pitch;
            const int   r = int(std::ceil(RouteSnap / pitch));
            const int   cx = int(std::lround(o.x())), cy = int(std::lround(o.y()));
            for (int iy = std::max(0, cy - r); iy <= std::min(ny - 1, cy + r); ++iy)
                for (int ix = std::max(0, cx - r); ix <= std::min(nx - 1, cx + r); ++ix) {
                    const int    n = iy * nx + ix;
                    const double d = (m_grid.node(ix, iy) - p).norm();
                    if (d <= RouteSnap && this->ok(n) && this->step_clear(p, m_grid.node(ix, iy)))
                        out.emplace_back(n, float(d));
                }
        };
        snaps(a, m_seeds);
        if (m_seeds.empty())
            return false;
        snaps(b, m_goals);
        if (m_goals.empty())
            return false;
        for (const auto &[n, d] : m_goals)
            m_goal_cost[n] = d;

        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        auto h = [&](int n) { return float((m_grid.node(n % nx, n / nx) - b).norm()); };
        auto touch = [&](int n, float g, int parent) {
            if (m_g[size_t(n)] == std::numeric_limits<float>::infinity())
                m_touched.emplace_back(n);
            m_g[size_t(n)]      = g;
            m_parent[size_t(n)] = parent;
        };
        for (const auto &[n, d] : m_seeds)
            if (d < m_g[size_t(n)] && d + h(n) <= cap) {
                touch(n, d, -1);
                open.emplace(d + h(n), n);
            }
        int found = -1;
        while (!open.empty()) {
            const auto [f, n] = open.top();
            open.pop();
            if (n < 0) {
                found = -1 - n;
                break;
            }
            const float g = m_g[size_t(n)];
            if (f > g + h(n) + 1e-4f)
                continue;
            if (const auto it = m_goal_cost.find(n); it != m_goal_cost.end() && g + it->second <= cap)
                open.emplace(g + it->second, -1 - n);
            const int ix = n % nx, iy = n / nx;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int jx = ix + dx, jy = iy + dy;
                    if ((dx == 0 && dy == 0) || jx < 0 || jy < 0 || jx >= nx || jy >= ny)
                        continue;
                    const int m = jy * nx + jx;
                    if (!this->ok(m) || (dx != 0 && dy != 0 && !(this->ok(iy * nx + jx) && this->ok(jy * nx + ix))))
                        continue;
                    const float gm = g + float(dx != 0 && dy != 0 ? diagonal : pitch);
                    if (gm < m_g[size_t(m)] && gm + h(m) <= cap) {
                        touch(m, gm, n);
                        open.emplace(gm + h(m), m);
                    }
                }
        }
        std::vector<Vec2d> nodes;
        for (int n = found; n >= 0; n = m_parent[size_t(n)])
            nodes.emplace_back(m_grid.node(n % nx, n / nx));
        for (const int n : m_touched) {
            m_g[size_t(n)]      = std::numeric_limits<float>::infinity();
            m_parent[size_t(n)] = -1;
        }
        m_touched.clear();
        m_goal_cost.clear();
        if (nodes.empty())
            return false;
        std::reverse(nodes.begin(), nodes.end());
        std::vector<Vec2d> pulled{ nodes.front() };
        for (size_t i = 0; i + 1 < nodes.size();) {
            size_t j = nodes.size() - 1;
            while (j > i + 1 && !this->clear(nodes[i], nodes[j]))
                --j;
            pulled.emplace_back(nodes[j]);
            i = j;
        }
        for (size_t i = 0; i < pulled.size(); ++i)
            if ((i > 0 && !each_inner_sample(pulled[i - 1], pulled[i], m_step, [&](const Vec2d &p) { return this->emit(p, route); })) ||
                !this->emit(pulled[i], route)) {
                route.clear();
                return false;
            }
        return true;
    }

private:
    enum : int8_t { Unknown = -1, No = 0, Range = 1, Ok = 2 };

    int8_t verdict(int n)
    {
        int8_t &v = m_verdict[size_t(n)];
        if (v == Unknown) {
            double z = 0.;
            const GlideAt at = m_glide_z(m_grid.node(n % m_grid.nx(), n / m_grid.nx()), z);
            v = at == GlideAt::Ok ? Ok : at == GlideAt::Range ? Range : No;
        }
        return v;
    }
    bool ok(int n) { return this->verdict(n) == Ok; }
    bool clear(const Vec2d &a, const Vec2d &b)
    {
        return each_inner_sample(a, b, m_step, [&](const Vec2d &p) {
            double z = 0.;
            return m_glide_z(p, z) == GlideAt::Ok;
        });
    }
    bool step_clear(const Vec2d &a, const Vec2d &b)
    {
        return each_inner_sample(a, b, 0.5 * m_step, [&](const Vec2d &p) {
            double z = 0.;
            return (p - a).squaredNorm() <= EndSlack * EndSlack || m_glide_z(p, z) != GlideAt::No;
        });
    }
    bool emit(const Vec2d &p, Points3 &out)
    {
        double z = 0.;
        if (m_glide_z(p, z) != GlideAt::Ok)
            return false;
        out.emplace_back(coord_t(scale_(p.x())), coord_t(scale_(p.y())), coord_t(scale_(z)));
        return true;
    }

    const HeightField                      &m_grid;
    const GlideZ                           &m_glide_z;
    double                                  m_step;
    std::vector<int8_t>                     m_verdict;
    std::vector<float>                      m_g;
    std::vector<int>                        m_parent;
    std::vector<int>                        m_touched;
    std::vector<std::pair<int, float>>      m_seeds, m_goals;
    std::unordered_map<int, float>          m_goal_cost;
};

bool glide_route_from(const Point3 &from, const Point3 &to, const GlideZ &glide_z, double max_glide, double step,
                      GlideRouter &router, Points3 &pts)
{
    const Vec2d  pa = unscale(from.to_point()), pb = unscale(to.to_point());
    const double l  = (pb - pa).norm();
    if (l > std::max(max_glide, MaxRoute))
        return false;
    pts.assign(1, from);
    auto straight = [&]() {
        if (l > max_glide)
            return false;
        const int k = std::max(1, int(std::ceil(l / step)));
        for (int j = 0; j < k; ++j) {
            const Vec2d p = pa + (pb - pa) * (double(j) / k);
            double      z = 0.;
            if (glide_z(p, z) != GlideAt::Ok)
                return false;
            const Point3 q(coord_t(scale_(p.x())), coord_t(scale_(p.y())), coord_t(scale_(z)));
            if (j == 0 && q.z() < from.z())
                return false;
            if (q != pts.back())
                pts.emplace_back(q);
        }
        return true;
    };
    if (!straight()) {
        Points3 route;
        if (!router.route(pa, pb, std::min(RouteRatio * l + RouteSnap, MaxRoute), route))
            return false;
        pts.assign(1, from);
        pts.insert(pts.end(), route.begin(), route.end());
    }
    pts.emplace_back(to);
    return true;
}

}

namespace {

size_t first_bead(const ExtrusionPathConformal &p)
{
    size_t i = 0;
    while (i + 1 < p.bead_height.size() && p.bead_height[i] == 0.f)
        ++i;
    return i;
}
size_t last_bead(const ExtrusionPathConformal &p)
{
    size_t i = p.bead_height.size() - 1;
    while (i > 0 && p.bead_height[i] == 0.f)
        --i;
    return i;
}
bool extruded(const ExtrusionPathConformal &p)
{
    return p.polyline.points.size() >= 2 && std::any_of(p.bead_height.begin(), p.bead_height.end(), [](float h) { return h > 0.f; });
}

bool joint(const ExtrusionPathConformal &s, const ExtrusionPathConformal &o, double &distance, int &mode, Point &a, Point &b)
{
    if (&s == &o || !extruded(s) || !extruded(o) || unscale<double>(s.polyline.length()) >= ShortRoad || s.role() != o.role() ||
        s.width != o.width || s.mm3_per_mm != o.mm3_per_mm || !s.tool_axis.empty() || !o.tool_axis.empty())
        return false;
    const Point ss = s.polyline.points[first_bead(s)].to_point(), se = s.polyline.points[last_bead(s)].to_point();
    const Point os = o.polyline.points[first_bead(o)].to_point(), oe = o.polyline.points[last_bead(o)].to_point();
    const std::tuple<double, int, Point, Point> cand[4] = {
        { (ss - oe).cast<double>().norm(), 0, oe, ss },
        { (se - oe).cast<double>().norm(), 1, oe, se },
        { (os - se).cast<double>().norm(), 2, os, se },
        { (os - ss).cast<double>().norm(), 3, os, ss },
    };
    distance = 2. * scale_(s.width);
    bool found = false;
    for (const auto &[d, m, pa, pb] : cand)
        if (d <= distance) {
            distance = d;
            mode     = m;
            a        = pa;
            b        = pb;
            found    = true;
        }
    return found;
}

}

bool is_short_road(const ExtrusionPathConformal &s)
{
    return extruded(s) && s.tool_axis.empty() && unscale<double>(s.polyline.length()) < ShortRoad;
}

bool short_road_joint(const ExtrusionPathConformal &s, const ExtrusionPathConformal &o, double &distance, Point &a, Point &b)
{
    int mode = 0;
    return joint(s, o, distance, mode, a, b);
}

void extrude_short_glides(ExtrusionEntitiesPtr &paths, double floor, double ceiling)
{
    RoadGrid roads;
    for (const ExtrusionEntity *ee : paths)
        roads.add(*static_cast<const ExtrusionPathConformal *>(ee));
    const coord_t lo = coord_t(std::ceil(scale_(floor))), hi = coord_t(std::floor(scale_(ceiling)));
    for (ExtrusionEntity *ee : paths) {
        auto &path = *static_cast<ExtrusionPathConformal *>(ee);
        if (!path.tool_axis.empty())
            continue;
        Points3            &pts = path.polyline.points;
        std::vector<float> &h   = path.bead_height;
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            if (h[i] > 0.f || h[i - 1] <= 0.f)
                continue;
            size_t j = i;
            while (j < pts.size() && h[j] == 0.f)
                ++j;
            if (j == pts.size())
                break;
            const Point  a = pts[i - 1].to_point(), b = pts[j].to_point();
            const double d = (b - a).cast<double>().norm();
            if (d > ExtrudedJoin * scale_(path.width) || roads.crosses(a, b)) {
                i = j;
                continue;
            }
            const float ha = h[i - 1], hb = h[j];
            double      run = 0., total = 0.;
            for (size_t k = i; k <= j; ++k)
                total += (pts[k].to_point() - pts[k - 1].to_point()).cast<double>().norm();
            for (size_t k = i; k < j; ++k) {
                run += (pts[k].to_point() - pts[k - 1].to_point()).cast<double>().norm();
                h[k]          = ha + (hb - ha) * float(total > 0. ? run / total : 0.5);
                pts[k].z()    = std::clamp(pts[k].z(), lo, hi);
            }
            roads.add(a, b);
            i = j;
        }
    }
}

void absorb_short_roads(ExtrusionEntitiesPtr &paths)
{
    auto trim = [](ExtrusionPathConformal &p, size_t from, size_t to) {
        auto cut = [from, to](auto &v) {
            if (!v.empty()) {
                v.erase(v.begin() + to + 1, v.end());
                v.erase(v.begin(), v.begin() + from);
            }
        };
        cut(p.polyline.points);
        cut(p.bead_height);
        cut(p.bead_width);
    };
    auto splice = [&](ExtrusionPathConformal &head, ExtrusionPathConformal &tail) {
        trim(head, 0, last_bead(head));
        trim(tail, first_bead(tail), tail.polyline.points.size() - 1);
        if (!head.bead_width.empty() || !tail.bead_width.empty())
            for (ExtrusionPathConformal *path : { &head, &tail })
                if (path->bead_width.empty())
                    path->bead_width.assign(path->polyline.points.size(), float(path->width));
        append(head.polyline.points, tail.polyline.points);
        append(head.bead_height, tail.bead_height);
        if (!head.bead_width.empty())
            append(head.bead_width, tail.bead_width);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < paths.size() && !changed; ++i) {
            auto  *s    = static_cast<ExtrusionPathConformal *>(paths[i]);
            size_t best = paths.size();
            int    mode = 0;
            double best_d = std::numeric_limits<double>::max();
            for (size_t j = 0; j < paths.size(); ++j) {
                double d = 0.;
                int    m = 0;
                Point  pa, pb;
                if (j != i && joint(*s, *static_cast<ExtrusionPathConformal *>(paths[j]), d, m, pa, pb) && d < best_d) {
                    best_d = d;
                    best   = j;
                    mode   = m;
                }
            }
            if (best == paths.size())
                continue;
            auto *o = static_cast<ExtrusionPathConformal *>(paths[best]);
            if (mode == 1 || mode == 3)
                s->reverse();
            if (mode <= 1) {
                splice(*o, *s);
                delete s;
            } else {
                splice(*s, *o);
                delete o;
                paths[best] = s;
            }
            paths.erase(paths.begin() + i);
            changed = true;
        }
    }
}

namespace {

void join_glide(ExtrusionPathConformal &current, ExtrusionPathConformal &path, const Points3 &route, double floor, double ceiling)
{
    Points3            &pts     = current.polyline.points;
    std::vector<float> &heights = current.bead_height;
    const Point3        a       = pts.back();
    const Point3        b       = path.polyline.points.front();
    std::vector<float> &widths  = current.bead_width;
    const size_t        n_before = pts.size();
    if (!widths.empty() || !path.bead_width.empty()) {
        if (widths.empty())
            widths.assign(pts.size(), float(current.width));
        if (path.bead_width.empty())
            path.bead_width.assign(path.polyline.points.size(), float(path.width));
    }
    constexpr double JoinClamp = 0.02;
    auto in_band = [floor, ceiling](const Point3 &q, double slack) {
        return unscale<double>(q.z()) >= floor - slack && unscale<double>(q.z()) <= ceiling + slack;
    };
    bool extrude = (unscale(b.to_point()) - unscale(a.to_point())).norm() <= ExtrudedJoin * current.width && in_band(a, JoinClamp) &&
                   in_band(b, JoinClamp);
    for (const Point3 &q : route)
        extrude = extrude && in_band(q, JoinClamp);
    if (extrude && a != b) {
        const float ha = heights.back(), hb = path.bead_height.front();
        const Vec2d pa = unscale(a.to_point());
        const double l = (unscale(b.to_point()) - pa).norm();
        for (Point3 q : route) {
            q.z() = std::clamp(q.z(), coord_t(std::ceil(scale_(floor))), coord_t(std::floor(scale_(ceiling))));
            pts.emplace_back(q);
            heights.emplace_back(ha + (hb - ha) * float((unscale(q.to_point()) - pa).norm() / l));
        }
    } else if (a != b) {
        pts.emplace_back(a);
        heights.emplace_back(0.f);
        for (const Point3 &q : route) {
            pts.emplace_back(q);
            heights.emplace_back(0.f);
        }
        pts.emplace_back(b);
        heights.emplace_back(0.f);
    }
    const bool same = (a == b || extrude) && heights.back() == path.bead_height.front() && pts.back() == b;
    if (!widths.empty()) {
        widths.insert(widths.end(), pts.size() - n_before, widths.back());
        widths.insert(widths.end(), path.bead_width.begin() + (same ? 1 : 0), path.bead_width.end());
    }
    pts.insert(pts.end(), path.polyline.points.begin() + (same ? 1 : 0), path.polyline.points.end());
    heights.insert(heights.end(), path.bead_height.begin() + (same ? 1 : 0), path.bead_height.end());
}

}

void order_band_pieces(ExtrusionEntitiesPtr &pieces, Point &cursor, RoadGrid &band_roads, const GlideZ &glide_z, double max_glide,
                       double step, bool hidden, double floor, double ceiling, const HeightField &grid, const Point3 *from)
{
    std::vector<ExtrusionPathConformal *> todo;
    for (ExtrusionEntity *ee : pieces)
        todo.emplace_back(static_cast<ExtrusionPathConformal *>(ee));
    std::vector<bool>  used(todo.size(), false);
    ExtrusionEntitiesPtr out;
    ExtrusionPathConformal *current = nullptr;
    RoadGrid           roads;
    const double       reach_mm = std::max(max_glide, MaxRoute);
    const double       reach    = scale_(reach_mm);
    constexpr size_t   Tries = 8;

    struct End
    {
        double d;
        size_t i;
        bool   reversed;
    };
    std::vector<End> ends;
    GlideRouter      router(grid, glide_z, step);
    auto glide_to = [&](const Point3 &a, const Point3 &b, Points3 &route) {
        route.clear();
        const Vec2d  pa = unscale(a.to_point()), pb = unscale(b.to_point());
        const double l  = (pb - pa).norm();
        if (l > reach_mm)
            return false;
        if (l <= max_glide && (hidden || !roads.crosses(a.to_point(), b.to_point())) && each_inner_sample(pa, pb, step, [&](const Vec2d &p) {
                double z = 0.;
                if (glide_z(p, z) != GlideAt::Ok)
                    return false;
                route.emplace_back(coord_t(scale_(p.x())), coord_t(scale_(p.y())), coord_t(scale_(z)));
                return true;
            }))
            return true;
        if (!router.route(pa, pb, std::min(RouteRatio * l + RouteSnap, MaxRoute), route))
            return false;
        if (!hidden) {
            Polyline xy;
            xy.points.reserve(route.size() + 2);
            xy.points.emplace_back(a.to_point());
            for (const Point3 &q : route)
                xy.points.emplace_back(q.to_point());
            xy.points.emplace_back(b.to_point());
            if (roads.crosses(xy))
                return false;
        }
        return true;
    };
    auto mergeable = [](const ExtrusionPathConformal &a, const ExtrusionPathConformal &b) {
        return a.role() == b.role() && a.width == b.width && a.mm3_per_mm == b.mm3_per_mm && a.tool_axis.empty() && b.tool_axis.empty();
    };

    auto splice = [&](ExtrusionPathConformal &piece) {
        struct Slot
        {
            double                  cost;
            ExtrusionPathConformal *path;
            size_t                  g0, g1;
            bool                    reversed;
        };
        std::vector<Slot> slots;
        auto dist = [](const Point3 &a, const Point3 &b) { return unscale<double>((a.to_point() - b.to_point()).cast<double>().norm()); };
        for (ExtrusionEntity *ee : out) {
            auto *path = static_cast<ExtrusionPathConformal *>(ee);
            if (!mergeable(*path, piece))
                continue;
            const Points3            &pts = path->polyline.points;
            const std::vector<float> &h   = path->bead_height;
            for (size_t g0 = 1; g0 < pts.size(); ++g0) {
                if (h[g0] != 0.f || h[g0 - 1] == 0.f)
                    continue;
                size_t g1 = g0;
                while (g1 + 1 < pts.size() && h[g1 + 1] == 0.f)
                    ++g1;
                if (g1 + 1 == pts.size())
                    break;
                const Point3 &x = pts[g0 - 1], &y = pts[g1 + 1];
                for (const bool reversed : { false, true }) {
                    if (reversed && !piece.can_reverse())
                        continue;
                    const Point3 &s0 = reversed ? piece.polyline.points.back() : piece.polyline.points.front();
                    const Point3 &s1 = reversed ? piece.polyline.points.front() : piece.polyline.points.back();
                    if (dist(x, s0) <= reach_mm && dist(s1, y) <= reach_mm)
                        slots.push_back({ dist(x, s0) + dist(s1, y) - dist(x, y), path, g0, g1, reversed });
                }
                g0 = g1;
            }
        }
        std::sort(slots.begin(), slots.end(), [](const Slot &a, const Slot &b) { return a.cost < b.cost; });
        constexpr size_t SpliceTries = 6;
        Points3          in, back;
        for (size_t t = 0; t < std::min(slots.size(), SpliceTries); ++t) {
            const Slot   &slot = slots[t];
            Points3      &pts  = slot.path->polyline.points;
            const Point3 &s0   = slot.reversed ? piece.polyline.points.back() : piece.polyline.points.front();
            const Point3 &s1   = slot.reversed ? piece.polyline.points.front() : piece.polyline.points.back();
            if (!glide_to(pts[slot.g0 - 1], s0, in) || !glide_to(s1, pts[slot.g1 + 1], back))
                continue;
            if (slot.reversed)
                piece.reverse();
            roads.add(piece);
            band_roads.add(piece);
            Points3            mid{ pts[slot.g0 - 1] };
            std::vector<float> mid_h{ 0.f };
            auto glide_points = [&](const Points3 &route, const Point3 &to) {
                mid.insert(mid.end(), route.begin(), route.end());
                mid.emplace_back(to);
                mid_h.resize(mid.size(), 0.f);
            };
            glide_points(in, piece.polyline.points.front());
            mid.insert(mid.end(), piece.polyline.points.begin(), piece.polyline.points.end());
            mid_h.insert(mid_h.end(), piece.bead_height.begin(), piece.bead_height.end());
            mid.emplace_back(piece.polyline.points.back());
            mid_h.emplace_back(0.f);
            glide_points(back, pts[slot.g1 + 1]);
            std::vector<float> &h = slot.path->bead_height;
            std::vector<float> &w = slot.path->bead_width;
            if (!w.empty() || !piece.bead_width.empty()) {
                if (w.empty())
                    w.assign(pts.size(), float(slot.path->width));
                std::vector<float> mid_w(mid.size(), w[slot.g0 - 1]);
                const size_t       s0 = in.size() + 2;
                for (size_t i = 0; i < piece.polyline.points.size(); ++i)
                    mid_w[s0 - 1 + i] = piece.bead_width.empty() ? float(piece.width) : piece.bead_width[i];
                std::fill(mid_w.begin() + (s0 - 1 + piece.polyline.points.size()), mid_w.end(), mid_w[s0 - 2 + piece.polyline.points.size()]);
                w.erase(w.begin() + slot.g0, w.begin() + slot.g1 + 1);
                w.insert(w.begin() + slot.g0, mid_w.begin(), mid_w.end());
            }
            pts.erase(pts.begin() + slot.g0, pts.begin() + slot.g1 + 1);
            h.erase(h.begin() + slot.g0, h.begin() + slot.g1 + 1);
            pts.insert(pts.begin() + slot.g0, mid.begin(), mid.end());
            h.insert(h.begin() + slot.g0, mid_h.begin(), mid_h.end());
            return true;
        }
        return false;
    };
    constexpr size_t SpliceLeft = 4;

    static const double climb = [] {
        const char *v = research_switch("NP_B2_CLIMB");
        return v != nullptr && *v != 0 ? std::atof(v) : -1.;
    }();
    auto lowest_bead = [](const ExtrusionPathConformal &p) {
        coord_t z = std::numeric_limits<coord_t>::max();
        for (size_t i = 0; i < p.polyline.points.size(); ++i)
            if (i >= p.bead_height.size() || p.bead_height[i] > 0.f)
                z = std::min(z, p.polyline.points[i].z());
        return z;
    };
    std::vector<coord_t> low;
    if (climb >= 0. && !hidden)
        for (const ExtrusionPathConformal *p : todo)
            low.push_back(lowest_bead(*p));

    Points3 route;
    for (size_t left = todo.size(); left > 0; --left) {
        ends.clear();
        coord_t limit = std::numeric_limits<coord_t>::max();
        if (!low.empty()) {
            coord_t lowest = std::numeric_limits<coord_t>::max();
            for (size_t i = 0; i < todo.size(); ++i)
                if (!used[i])
                    lowest = std::min(lowest, low[i]);
            limit = lowest + coord_t(scale_(climb));
        }
        for (size_t i = 0; i < todo.size(); ++i)
            if (!used[i] && (low.empty() || low[i] <= limit)) {
                const bool lower_last = !low.empty() && todo[i]->can_reverse() && todo[i]->polyline.points.back().z() < todo[i]->polyline.points.front().z();
                if (!lower_last)
                    ends.push_back({ (todo[i]->first_point() - cursor).cast<double>().norm(), i, false });
                if (todo[i]->can_reverse() && (low.empty() || lower_last))
                    ends.push_back({ (todo[i]->last_point() - cursor).cast<double>().norm(), i, true });
            }
        const size_t head = std::min(ends.size(), Tries);
        std::partial_sort(ends.begin(), ends.begin() + head, ends.end(), [](const End &a, const End &b) { return a.d < b.d; });

        int  pick  = -1;
        bool glide = false;
        Points3 lead;
        if (current == nullptr && out.empty() && from != nullptr)
            for (size_t k = 0; k < head && ends[k].d <= reach && pick < 0; ++k) {
                const ExtrusionPathConformal &cand = *todo[ends[k].i];
                if (cand.tool_axis.empty() &&
                    glide_route_from(*from, ends[k].reversed ? cand.polyline.points.back() : cand.polyline.points.front(), glide_z,
                                     max_glide, step, router, lead))
                    pick = int(k);
            }
        if (pick < 0)
            lead.clear();
        if (current != nullptr)
            for (size_t k = 0; k < head && ends[k].d <= reach; ++k) {
                ExtrusionPathConformal &cand = *todo[ends[k].i];
                if (!mergeable(*current, cand))
                    continue;
                const Point3 b = ends[k].reversed ? cand.polyline.points.back() : cand.polyline.points.front();
                if (glide_to(current->polyline.points.back(), b, route)) {
                    pick  = int(k);
                    glide = true;
                    break;
                }
            }
        if (pick < 0 && hidden && current != nullptr && left <= SpliceLeft) {
            size_t spliced = 0;
            for (size_t i = 0; i < todo.size(); ++i)
                if (!used[i] && splice(*todo[i])) {
                    used[i] = true;
                    delete todo[i];
                    ++spliced;
                }
            if (spliced > 0) {
                left -= spliced - 1;
                continue;
            }
        }
        if (pick < 0) {
            for (size_t k = 0; k < head && pick < 0; ++k) {
                const ExtrusionPathConformal &cand = *todo[ends[k].i];
                if (!band_roads.crosses(cursor, ends[k].reversed ? cand.last_point() : cand.first_point()))
                    pick = int(k);
            }
            if (pick < 0)
                pick = 0;
        }

        const End &e = ends[size_t(pick)];
        used[e.i]    = true;
        ExtrusionPathConformal *path = todo[e.i];
        if (e.reversed)
            path->reverse();
        if (!lead.empty()) {
            path->polyline.points.insert(path->polyline.points.begin(), lead.begin(), lead.end());
            path->bead_height.insert(path->bead_height.begin(), lead.size(), 0.f);
            if (!path->bead_width.empty())
                path->bead_width.insert(path->bead_width.begin(), lead.size(), path->bead_width.front());
        }
        {
            Points3            &pts = path->polyline.points;
            std::vector<float> &h   = path->bead_height;
            for (size_t i = 0; !hidden && path->tool_axis.empty() && i + 1 < pts.size(); ++i)
                if (h[i] == 0.f && h[i + 1] == 0.f && roads.crosses(pts[i].to_point(), pts[i + 1].to_point())) {
                    size_t g0 = i, g1 = i + 1;
                    while (g0 > 0 && h[g0 - 1] == 0.f)
                        --g0;
                    while (g1 + 1 < pts.size() && h[g1 + 1] == 0.f)
                        ++g1;
                    Points3            tail(pts.begin() + g1, pts.end());
                    std::vector<float> tail_h(h.begin() + g1, h.end());
                    if (g0 < 2) {
                        pts = std::move(tail);
                        h   = std::move(tail_h);
                        if (!path->bead_width.empty())
                            path->bead_width.erase(path->bead_width.begin(), path->bead_width.begin() + g1);
                        i   = size_t(-1);
                        continue;
                    }
                    std::vector<float> tail_w;
                    if (!path->bead_width.empty()) {
                        tail_w.assign(path->bead_width.begin() + g1, path->bead_width.end());
                        path->bead_width.resize(g0);
                    }
                    pts.resize(g0);
                    h.resize(g0);
                    if (tail.size() >= 2) {
                        auto *rest       = new ExtrusionPathConformal(Polyline3(std::move(tail)), *path, std::move(tail_h));
                        rest->bead_width = std::move(tail_w);
                        rest->patch_id = path->patch_id;
                        rest->shell    = path->shell;
                        todo.emplace_back(rest);
                        used.emplace_back(false);
                        ++left;
                    }
                    break;
                }
        }
        roads.add(*path);
        band_roads.add(*path);
        cursor = path->last_point();
        if (!glide) {
            out.emplace_back(path);
            current = path;
            continue;
        }
        join_glide(*current, *path, route, floor, ceiling);
        delete path;
    }
    pieces = std::move(out);
    if (floor <= ceiling) {
        absorb_short_roads(pieces);
        extrude_short_glides(pieces, floor, ceiling);
    }
}

}
