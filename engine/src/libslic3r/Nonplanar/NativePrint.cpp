#include "NativePrint.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "ConformalLayers.hpp"
#include "libslic3r/GCode/ClearanceRepair.hpp"

#include "ConformalShells.hpp"
#include "FieldChart.hpp"
#include "FieldOrder.hpp"
#include "HeadEnvelope.hpp"
#include "HeightField.hpp"
#include "Multiplanar.hpp"
#include "NativeLayers.hpp"
#include "NpField.hpp"
#include "OneShotField.hpp"
#include "OrientationField.hpp"
#include "PatchTools.hpp"
#include "Slant.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCode/ConformalEmit.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MinAreaBoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/format.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace Slic3r::Nonplanar {

namespace {

std::mutex                                     g_override_mutex;
std::map<std::string, std::vector<ExPolygons>> g_overrides;

double tilt_budget(const ToolKinematics &kinematics)
{
    double tilt = 0.5 * M_PI;
    for (int s = 0; s < 72; ++s)
        tilt = std::min(tilt, kinematics.max_tilt(2. * M_PI * s / 72.));
    return tilt;
}

class NativeChart : public FieldChart
{
public:
    NativeChart(std::shared_ptr<const LayerField> field, double tilt_cap) : m_field(std::move(field)), m_tilt_cap(tilt_cap) {}
    Vec3d forward(const Vec3d &p) const override { return Vec3d(p.x(), p.y(), m_field->phi(p)); }
    bool  inverse(const Vec3d &q, Vec3d &p) const override
    {
        p = Vec3d(q.x(), q.y(), m_field->z_at(q.head<2>(), q.z()));
        return std::isfinite(p.z());
    }
    Vec3d orientation(const Vec3d &p) const override { return Native::tool_axis(*m_field, p, m_tilt_cap); }
    bool  given() const override { return true; }

private:
    std::shared_ptr<const LayerField> m_field;
    double                            m_tilt_cap;
};

using SeamCost = std::function<double(const Point &)>;

void flatten(const ExtrusionEntity *ee, std::vector<const ExtrusionPath *> &out, const SeamCost *seam_cost = nullptr,
             std::deque<ExtrusionLoop> *seamed = nullptr)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
        for (const ExtrusionEntity *e : c->entities)
            flatten(e, out, seam_cost, seamed);
    } else if (const auto *l = dynamic_cast<const ExtrusionLoop *>(ee)) {
        const ExtrusionLoop *loop = l;
        if (seam_cost && seamed) {
            ExtrusionLoop &copy = seamed->emplace_back(*l);
            const Polygon  poly = copy.polygon();
            if (!poly.points.empty()) {
                size_t best = 0;
                double best_cost = std::numeric_limits<double>::max();
                for (size_t i = 0; i < poly.points.size(); ++i)
                    if (const double cost = (*seam_cost)(poly.points[i]); cost < best_cost)
                        best = i, best_cost = cost;
                copy.split_at(poly.points[best], false);
            }
            loop = &copy;
        }
        for (const ExtrusionPath &p : loop->paths)
            out.push_back(&p);
    } else if (const auto *m = dynamic_cast<const ExtrusionMultiPath *>(ee)) {
        for (const ExtrusionPath &p : m->paths)
            out.push_back(&p);
    } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(ee)) {
        out.push_back(p);
    }
}

}

bool native_requested()
{
    const char *v = research_switch("NP_NATIVE");
    return v != nullptr && *v != 0 && *v != '0';
}

double native_axis_key(const char *name, double rotary_default, bool rotary)
{
    if (const char *v = research_switch(name))
        return std::max(0., std::atof(v));
    return rotary ? rotary_default : 0.;
}

std::shared_ptr<const LayerField> native_field(PrintObject &object)
{
    const NonplanarMode mode = object.config().nonplanar_mode.value;
    if (!object.config().nonplanar_atom_toolpath.value.empty())
        return {};
    if (mode == NonplanarMode::ConformalLayers)
        return conformal_layers_field(object);
    if (mode == NonplanarMode::OneShot) {
        const OneShotState *state = std::as_const(object).oneshot_state();
        if (state == nullptr || !state->field() || state->z_only)
            return {};
        return std::make_shared<const NpLayerField>(*state->solved);
    }
    const std::string &path = object.config().nonplanar_field_file.value;
    if (mode != NonplanarMode::Field || path.empty())
        return {};
    BoundingBoxf3 box;
    for (const stl_vertex &v : slicing_frame_mesh(object).vertices)
        box.merge(v.cast<double>());
    try {
        return std::make_shared<const NpLayerField>(load_npfield_for(path, box, plate_offset(object)));
    } catch (const RuntimeError &ex) {
        throw Slic3r::SlicingError(ex.what());
    }
}

NativeSliceOverride::NativeSliceOverride(std::string name, std::vector<ExPolygons> slices) : m_name(std::move(name))
{
    std::lock_guard<std::mutex> lock(g_override_mutex);
    g_overrides[m_name] = std::move(slices);
}

NativeSliceOverride::~NativeSliceOverride()
{
    std::lock_guard<std::mutex> lock(g_override_mutex);
    g_overrides.erase(m_name);
}

const std::vector<ExPolygons> *NativeSliceOverride::find(const std::string &name)
{
    std::lock_guard<std::mutex> lock(g_override_mutex);
    const auto                  it = g_overrides.find(name);
    return it == g_overrides.end() ? nullptr : &it->second;
}

double adaptive_level_ratio()
{
    static const double r = [] {
        const char *v = research_switch("NP_NATIVE_ADAPTIVE");
        return v == nullptr ? 0. : std::clamp(std::atof(v), 1., 2.);
    }();
    return r;
}

std::vector<double> adaptive_levels(const indexed_triangle_set &mesh, const LayerField &field, double first, double top, double h, double t_max)
{
    constexpr double Bin = 0.02;
    const size_t     nb  = size_t(std::max(1., std::ceil((top - first) / Bin))) + 2;
    std::vector<double> allow(nb, t_max * h);
    for (const stl_triangle_vertex_indices &f : mesh.indices) {
        const Vec3d a = mesh.vertices[size_t(f[0])].cast<double>(), b = mesh.vertices[size_t(f[1])].cast<double>(),
                    c = mesh.vertices[size_t(f[2])].cast<double>();
        Vec3d n = (b - a).cross(c - a);
        if (n.squaredNorm() < 1e-18)
            continue;
        n.normalize();
        Vec3d g = field.gradient((a + b + c) / 3.);
        if (g.squaredNorm() < 1e-18)
            continue;
        g.normalize();
        const double ng = std::abs(n.dot(g));
        const double t  = h * (ng >= 0.5 ? 1. : ng <= 0.25 ? t_max : 1. + (t_max - 1.) * (0.5 - ng) / 0.25);
        const double pa = field.phi(a), pb = field.phi(b), pc = field.phi(c);
        const double lo = std::min({ pa, pb, pc }), hi = std::max({ pa, pb, pc });
        const size_t i0 = size_t(std::clamp((lo - first) / Bin, 0., double(nb - 1)));
        const size_t i1 = size_t(std::clamp((hi - first) / Bin, 0., double(nb - 1)));
        for (size_t i = i0; i <= i1; ++i)
            allow[i] = std::min(allow[i], t);
    }
    std::vector<double> tops{ first };
    double              prev = h;
    while (tops.back() < top - 1e-6) {
        const double phi = tops.back();
        double       t   = std::min(t_max * h, 1.15 * prev);
        for (int it = 0; it < 8; ++it) {
            double       least = t;
            const size_t i0    = size_t(std::clamp((phi - first) / Bin, 0., double(nb - 1)));
            const size_t i1    = size_t(std::clamp((phi + t - first) / Bin, 0., double(nb - 1)));
            for (size_t i = i0; i <= i1; ++i)
                least = std::min(least, allow[i]);
            if (least >= t - 1e-9)
                break;
            t = std::max(h, least);
        }
        if (phi + t > top - 0.5 * h)
            t = std::max(top - phi, 0.5 * h);
        tops.push_back(phi + t);
        prev = t;
    }
    BOOST_LOG_TRIVIAL(info) << "Native adaptive levels: " << tops.size() << " levels to " << tops.back() << " (uniform " << h << ": "
                            << size_t(std::ceil((top - first) / h - 1e-6) + 1.) << ")";
    return tops;
}

bool native_planar_support()
{
    static const bool on = [] { const char *v = research_switch("NP_NATIVE_PLANAR_SUPPORT"); return v == nullptr || std::atoi(v) != 0; }();
    return on;
}

std::vector<FieldRoad> native_planar_support_roads(const PrintObject &object, const LayerField &field, const std::vector<FieldRoad> &part,
                                                   const std::vector<ExPolygons> &domains, const std::vector<double> &tops, double first,
                                                   double h, double raft, double host_z, bool bead_columns = false,
                                                   const Vec2d &grid_phase = Vec2d::Zero(), bool planar_order = false,
                                                   double base_pad = 0.)
{
    std::vector<FieldRoad> out;
    const size_t K = domains.size();
    if (K < 3 || part.empty())
        return out;
    constexpr double Cell = 0.5;
    const double gap   = std::max(0., object.config().support_top_z_distance.value);
    const double w     = 1.125 * object.print()->config().nozzle_diameter.get_at(0);
    BoundingBox bb;
    for (const ExPolygons &d : domains)
        if (!d.empty())
            bb.merge(get_extents(d));
    if (!bb.defined)
        return out;
    bb.offset(scale_(3.));
    const HeightField grid = HeightField::layout(BoundingBoxf(Vec2d(unscale(bb.min) + grid_phase), Vec2d(unscale(bb.max) + grid_phase)), Cell);
    std::vector<Mask> inside(K);
    for (size_t k = 0; k < K; ++k)
        inside[k] = rasterize(offset_ex(domains[k], -float(scale_(0.5 * Cell))), grid);
    auto cell_of = [&](double x, double y, int &ix, int &iy) {
        const Vec2d o = (Vec2d(x, y) - grid.origin()) / Cell;
        ix = int(std::lround(o.x())), iy = int(std::lround(o.y()));
        return ix >= 0 && iy >= 0 && ix < grid.nx() && iy < grid.ny();
    };
    auto key = [](int x, int y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); };
    std::unordered_map<int64_t, std::vector<std::pair<Vec3d, size_t>>> part_at;
    auto each_point = [&](const FieldRoad &r, auto &&fn) {
        const auto *cp = dynamic_cast<const ExtrusionPathConformal *>(r.path.get());
        auto bead = [&](size_t i) {
            const double bw = cp && i < cp->bead_width.size() ? cp->bead_width[i] : cp ? cp->width : w;
            const double bh = cp && i < cp->bead_height.size() ? cp->bead_height[i] : h;
            return std::make_pair(bw, bh);
        };
        for (size_t i = 0; i < r.tip.size(); ++i) {
            const auto [bw, bh] = bead(i);
            const int n = i == 0 ? 0 : std::max(1, int(std::ceil((r.tip[i] - r.tip[i - 1]).norm() / 0.25)));
            for (int t = i == 0 ? 0 : 1; t <= n; ++t)
                fn(i == 0 ? r.tip[0] : Vec3d(r.tip[i - 1] + (r.tip[i] - r.tip[i - 1]) * (double(t) / n)), bw, bh);
        }
    };
    for (const FieldRoad &r : part)
        each_point(r, [&](const Vec3d &t, double, double) {
            int ix, iy;
            if (cell_of(t.x(), t.y(), ix, iy))
                part_at[key(ix, iy)].emplace_back(t, r.layer);
        });
    std::unordered_map<int64_t, std::vector<double>> under;
    static const char *bead_env = research_switch("NP_PSUP_BEAD");
    bead_columns = bead_env != nullptr && *bead_env != 0 ? std::atoi(bead_env) != 0 : bead_columns;
    struct BeadBottom { double x, y, bottom, w; };
    std::vector<BeadBottom> bead_bottoms;
    for (const FieldRoad &r : part)
        each_point(r, [&](const Vec3d &t, double bw, double bh) {
            int ix, iy;
            if (t.z() - bh <= first + 0.05 || !cell_of(t.x(), t.y(), ix, iy))
                return;
            const double lateral = 0.5 * bw + bh - 0.025, window = 2. * std::max(bh, h) + 0.1;
            const int    reach   = int(std::ceil(lateral / Cell));
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx)
                    if (auto it = part_at.find(key(ix + dx, iy + dy)); it != part_at.end())
                        for (const auto &[q, l] : it->second)
                            if (l < r.layer && t.z() - q.z() >= 0.01 && t.z() - q.z() <= window &&
                                (q.head<2>() - t.head<2>()).norm() <= lateral)
                                return;
            under[key(ix, iy)].push_back(t.z() - bh);
            if (bead_columns)
                bead_bottoms.push_back({ t.x(), t.y(), t.z() - bh, bw });
        });
    if (under.empty())
        return out;
    std::unordered_map<int64_t, std::vector<double>> tops_of;
    for (const auto &[k, bottoms] : under) {
        const int x = int(k >> 32), y = int(int32_t(uint32_t(k)));
        for (int b = -1; b <= 1; ++b)
            for (int a = -1; a <= 1; ++a) {
                const int64_t kk = key(x + a, y + b);
                if (kk != k && under.count(kk))
                    continue;
                std::vector<double> &t = tops_of[kk];
                for (double bottom : bottoms)
                    t.push_back(bottom - gap);
            }
    }
    struct Column { int x, y; double base, top; Polygon foot; };
    std::FILE *dump = research_switch("NP_NATIVE_PSUP_DUMP") ? std::fopen(research_switch("NP_NATIVE_PSUP_DUMP"), "w") : nullptr;
    std::vector<Column> columns;
    for (auto &[k, cell_tops] : tops_of) {
        const int x = int(k >> 32), y = int(int32_t(uint32_t(k)));
        if (x < 0 || y < 0 || x >= grid.nx() || y >= grid.ny())
            continue;
        const Vec2d c = grid.node(x, y);
        std::sort(cell_tops.begin(), cell_tops.end());
        for (size_t i = 0; i < cell_tops.size();) {
            const double top = cell_tops[i];
            while (i < cell_tops.size() && cell_tops[i] < top + 0.3)
                ++i;
            double base = 0.;
            for (size_t j = K; j-- > 0;)
                if (inside[j][size_t(y) * grid.nx() + x]) {
                    const double zj = field.z_at(c, tops[j]);
                    if (zj < top - h) {
                        base = zj + gap;
                        break;
                    }
                }
            if (top - base >= (base <= 0. ? first : 2. * h) - 1e-6)
                columns.push_back({ x, y, base, top, {} });
            else if (dump)
                std::fprintf(dump, "D %.3f %.3f %.3f %.3f\n", c.x(), c.y(), base, top);
        }
    }
    if (bead_columns) {
        std::unordered_map<int64_t, std::vector<double>> cell_top;
        for (const Column &col : columns)
            cell_top[key(col.x, col.y)].push_back(col.top);
        std::map<std::pair<int64_t, int64_t>, std::vector<const BeadBottom *>> buckets;
        for (const BeadBottom &b : bead_bottoms)
            buckets[{ std::llround(b.x / 0.2), std::llround(b.y / 0.2) }].push_back(&b);
        for (auto &[k, bs] : buckets) {
            std::sort(bs.begin(), bs.end(), [](const BeadBottom *a, const BeadBottom *b) { return a->bottom < b->bottom; });
            for (size_t i = 0; i < bs.size();) {
                const BeadBottom &lo = *bs[i];
                while (i < bs.size() && bs[i]->bottom < lo.bottom + 0.3)
                    ++i;
                int ix, iy;
                if (!cell_of(lo.x, lo.y, ix, iy))
                    continue;
                const double top  = lo.bottom - gap;
                if (auto it = cell_top.find(key(ix, iy)); it != cell_top.end() &&
                    std::any_of(it->second.begin(), it->second.end(), [top](double t) { return std::abs(t - top) < 0.1; }))
                    continue;
                double       base = 0.;
                for (size_t j = K; j-- > 0;)
                    if (inside[j][size_t(iy) * grid.nx() + ix]) {
                        const double zj = field.z_at(Vec2d(lo.x, lo.y), tops[j]);
                        if (zj < top - h) {
                            base = zj + gap;
                            break;
                        }
                    }
                if (top - base < (base <= 0. ? first : 2. * h) - 1e-6)
                    continue;
                const double r = 0.5 * std::max(lo.w, w);
                Polygon      foot;
                foot.points = { Point::new_scale(lo.x - r, lo.y - r), Point::new_scale(lo.x + r, lo.y - r), Point::new_scale(lo.x + r, lo.y + r),
                                Point::new_scale(lo.x - r, lo.y + r) };
                columns.push_back({ ix, iy, base, top, std::move(foot) });
            }
        }
    }
    if (dump) {
        for (const auto &[k, bottoms] : under) {
            const Vec2d c = grid.node(int(k >> 32), int(int32_t(uint32_t(k))));
            for (double bottom : bottoms)
                std::fprintf(dump, "F %.3f %.3f %.3f\n", c.x(), c.y(), bottom);
        }
        for (const Column &col : columns) {
            const Vec2d c = grid.node(col.x, col.y);
            std::fprintf(dump, "C %.3f %.3f %.3f %.3f\n", c.x(), c.y(), col.base, col.top);
        }
        std::fclose(dump);
    }
    if (columns.empty())
        return out;
    double zmax = 0.;
    for (const Column &c : columns)
        zmax = std::max(zmax, c.top);
    auto square = [&](int x, int y) {
        const Vec2d c = grid.node(x, y);
        Polygon     sq;
        sq.points = { Point::new_scale(c.x() - 0.5 * Cell, c.y() - 0.5 * Cell), Point::new_scale(c.x() + 0.5 * Cell, c.y() - 0.5 * Cell),
                      Point::new_scale(c.x() + 0.5 * Cell, c.y() + 0.5 * Cell), Point::new_scale(c.x() - 0.5 * Cell, c.y() + 0.5 * Cell) };
        return sq;
    };
    auto grid_top = [&](double top) { return std::max(first, first + std::floor((top - first) / h + 1e-6) * h); };
    auto cap_of   = [&](const Column &c) {
        const double z = std::floor(c.top / 0.05 + 1e-6) * 0.05;
        return z - grid_top(c.top) >= 0.06 ? z : -1.;
    };
    std::vector<std::pair<double, double>> specs;
    for (double z = first; z <= zmax + 1e-6; z += h)
        specs.emplace_back(z, z == first ? first : h);
    {
        std::vector<double> caps;
        for (const Column &c : columns)
            if (const double z = cap_of(c); z > 0.)
                caps.push_back(z);
        std::sort(caps.begin(), caps.end());
        caps.erase(std::unique(caps.begin(), caps.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), caps.end());
        for (double z : caps)
            specs.emplace_back(z, -(z - grid_top(z)));
        std::stable_sort(specs.begin(), specs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    }
    const double xy_user = std::max(0., object.config().support_object_xy_distance.value);
    const double xy_wall = xy_user + 0.5 * w, xy_tip = std::min(xy_user, 0.5 * w) + 0.5 * w;
    const Flow   sup_flow      = support_material_flow(&object, float(h));
    const double ws            = sup_flow.width(), sup_pitch = sup_flow.spacing();
    const double iface_pitch   = std::min(sup_pitch + std::max(0., object.config().support_interface_spacing.value), w + h);
    const double base_pitch    = sup_pitch + std::max(0., object.config().support_base_pattern_spacing.value);
    const int    iface_layers  = std::max(0, object.config().support_interface_top_layers.value);
    auto is_wall = [&](const Vec3d &q, size_t ql) {
        int ix, iy;
        if (q.z() <= first + 2. * h || !cell_of(q.x(), q.y(), ix, iy))
            return true;
        const double window = 2. * h + 0.1, lateral = 0.5 * w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (auto it = part_at.find(key(ix + dx, iy + dy)); it != part_at.end())
                    for (const auto &[p, l] : it->second)
                        if (l < ql && q.z() - p.z() >= 0.01 && q.z() - p.z() <= window && (p.head<2>() - q.head<2>()).norm() <= lateral)
                            return true;
        return false;
    };
    auto keep_off = [&](const std::vector<std::pair<int, int>> &cells, double z, double lh, bool tip) {
        Polygons                     discs;
        std::unordered_set<int64_t>  seen_cells;
        const int                    reach = int(std::ceil((xy_wall + 0.5 * Cell) / Cell));
        for (const auto &[x, y] : cells)
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int64_t k = key(x + dx, y + dy);
                    if (!seen_cells.insert(k).second)
                        continue;
                    if (auto it = part_at.find(k); it != part_at.end())
                        for (const auto &[q, l] : it->second) {
                            if (q.z() < z - lh - 1e-6 || q.z() > z + 0.5 * lh + 1e-6)
                                continue;
                            const double r = !tip && is_wall(q, l) ? xy_wall : xy_tip;
                            Polygon disc;
                            for (int i = 0; i < 12; ++i)
                                disc.points.push_back(Point::new_scale(q.x() + r * std::cos(i * M_PI / 6.), q.y() + r * std::sin(i * M_PI / 6.)));
                            discs.push_back(std::move(disc));
                        }
                }
        return union_(discs);
    };
    constexpr int PadLayers = 3;
    Polygons      pad_feet;
    if (base_pad > 0.) {
        std::vector<double> col_top;
        for (const Column &c : columns)
            if (c.base <= 0. && c.top >= first + PadLayers * h - 1e-6) {
                pad_feet.push_back(c.foot.empty() ? square(c.x, c.y) : c.foot);
                col_top.push_back(c.top);
            }
        if (const double min_ratio = object.config().native_support_base_pad_min_ratio.value; min_ratio > 0. && !pad_feet.empty()) {
            constexpr double Join   = 0.05;
            const ExPolygons plates = union_ex(offset(pad_feet, float(scale_(Join))));
            Polygons         tall;
            for (const ExPolygon &plate : plates) {
                const BoundingBox   pbb = get_extents(plate);
                double              top = 0.;
                std::vector<size_t> own;
                for (size_t i = 0; i < pad_feet.size(); ++i)
                    if (const Point p = pad_feet[i].bounding_box().center(); pbb.contains(p) && plate.contains(p)) {
                        own.push_back(i);
                        top = std::max(top, col_top[i]);
                    }
                const MinAreaBoundigBox box(plate);
                const double width = std::max(Cell, double(std::min(box.width(), box.height())) * SCALING_FACTOR - 2. * Join);
                BOOST_LOG_TRIVIAL(debug) << "Native base pad plate at " << unscale(pbb.center()).transpose() << ": height " << top
                                         << " mm, width " << width << " mm, ratio " << top / width;
                if (top / width > min_ratio)
                    for (size_t i : own)
                        tall.push_back(pad_feet[i]);
            }
            pad_feet = std::move(tall);
        }
    }
    std::vector<std::vector<size_t>> by_layer;
    int layer = 0;
    for (const auto &spec : specs) {
        const double z = spec.first, spec_h = spec.second;
        const bool   is_cap = spec_h < 0.;
        const double lh     = std::abs(spec_h);
        Polygons sparse, dense, contact;
        {
            std::vector<std::pair<int, int>> tip_cells, body_cells;
            Polygons                         cls[3][2];
            for (const Column &c : columns)
                if (is_cap ? std::abs(cap_of(c) - z) < 1e-6 : z <= c.top + 1e-6 && z - lh >= c.base - 1e-6) {
                    const bool tip = c.top - z < 3. * h;
                    (tip ? tip_cells : body_cells).emplace_back(c.x, c.y);
                    const int k = is_cap || (iface_layers > 0 && c.top - z < h - 1e-6) ? 0 : c.top - z < iface_layers * h - 1e-6 ? 1 : 2;
                    cls[k][tip ? 0 : 1].push_back(c.foot.empty() ? square(c.x, c.y) : c.foot);
                }
            const Polygons off_tip = tip_cells.empty() ? Polygons() : keep_off(tip_cells, z, lh, true);
            const Polygons off_body = body_cells.empty() ? Polygons() : keep_off(body_cells, z, lh, false);
            Polygons *dst[3] = { &contact, &dense, &sparse };
            for (int k = 0; k < 3; ++k) {
                Polygons parts = diff(union_(cls[k][0]), off_tip);
                append(parts, diff(union_(cls[k][1]), off_body));
                *dst[k] = union_(parts);
            }
        }
        std::vector<size_t> made;
        auto hatch = [&](const Polygons &cells, double spacing, int orient, ExtrusionRole role) {
            if (cells.empty())
                return;
            for (const ExPolygon &ex : union_ex(cells)) {
                const BoundingBox ebb = ex.contour.bounding_box();
                Polylines         lines;
                const bool        long_y  = ebb.size().y() >= ebb.size().x();
                const bool        along_x = orient == 0 || (orient == 1) == long_y;
                const coord_t     step    = coord_t(scale_(spacing));
                const coord_t o  = along_x ? bb.min.y() : bb.min.x(), lo = along_x ? ebb.min.y() : ebb.min.x();
                for (coord_t t = o + step / 2 + (lo > o ? (lo - o) / step * step : 0); t <= (along_x ? ebb.max.y() : ebb.max.x()); t += step)
                    lines.push_back(along_x ? Polyline(Point(ebb.min.x() - step, t), Point(ebb.max.x() + step, t)) :
                                              Polyline(Point(t, ebb.min.y() - step), Point(t, ebb.max.y() + step)));
                Polylines clipped = intersection_pl(lines, ex);
                if (std::none_of(clipped.begin(), clipped.end(), [](const Polyline &pl) { return pl.length() >= scale_(0.25); })) {
                    const coord_t t = along_x ? (ebb.min.y() + ebb.max.y()) / 2 : (ebb.min.x() + ebb.max.x()) / 2;
                    clipped = intersection_pl(Polylines{ along_x ? Polyline(Point(ebb.min.x() - step, t), Point(ebb.max.x() + step, t)) :
                                                                   Polyline(Point(t, ebb.min.y() - step), Point(t, ebb.max.y() + step)) }, ex);
                }
                for (size_t i = 0; i < clipped.size(); ++i) {
                    if (i & 1)
                        clipped[i].reverse();
                    if (clipped[i].length() < scale_(0.25))
                        continue;
                    const double mm3 = lh * (ws - lh * (1. - 0.25 * M_PI));
                    std::vector<Vec2d> qs;
                    const Points      &src = clipped[i].points;
                    for (size_t k = 0; k < src.size(); ++k) {
                        const int n = k == 0 ? 1 : std::max(1, int(std::ceil(unscale<double>((src[k] - src[k - 1]).cast<double>().norm()) / 0.5)));
                        for (int t = k == 0 ? 0 : 1; t <= (k == 0 ? 0 : n); ++t)
                            qs.push_back(k == 0 ? unscale(src[0]) : Vec2d(unscale(src[k - 1]) + (unscale(src[k]) - unscale(src[k - 1])) * (double(t) / n)));
                    }
                    for (size_t s0 = 0; s0 + 1 < qs.size(); s0 += 4) {
                        const size_t s1 = std::min(s0 + 4, qs.size() - 1);
                        FieldRoad    road;
                        Polyline3    polyline;
                        for (size_t k = s0; k <= s1; ++k) {
                            polyline.points.push_back(Point3::new_scale(qs[k].x(), qs[k].y(), z + raft - host_z));
                            road.tip.emplace_back(qs[k].x(), qs[k].y(), z);
                        }
                        const size_t n = polyline.points.size();
                        road.path = std::make_unique<ExtrusionPathConformal>(std::move(polyline), ExtrusionPath(role, mm3, float(ws), float(lh)),
                                                                             std::vector<float>(n, float(lh)), std::vector<Vec3f>(n, Vec3f::UnitZ()));
                        made.push_back(out.size());
                        out.push_back(std::move(road));
                    }
                }
            }
        };
        hatch(sparse, base_pitch, 0, erSupportMaterial);
        hatch(dense, iface_pitch, 2, erSupportMaterialInterface);
        hatch(contact, iface_pitch, 1, erSupportMaterialInterface);
        if (const int i = int(std::lround((z - first) / h)); !pad_feet.empty() && !is_cap && i < PadLayers) {
            Polygons near_part, own = sparse;
            for (size_t k = 0; k < K; ++k)
                if (tops[k] > z - lh + 1e-6 && tops[k] < z + gap + h - 1e-6)
                    append(near_part, to_polygons(domains[k]));
            append(own, dense);
            append(own, contact);
            const Polygons pad = diff(diff(offset(union_(pad_feet), float(scale_(base_pad * double(PadLayers - i) / PadLayers))),
                                           offset(near_part, float(scale_(xy_user + 0.5 * ws)))),
                                      own);
            hatch(pad, sup_pitch, 1, erSupportMaterial);
        }
        if (!made.empty())
            by_layer.push_back(std::move(made));
        ++layer;
    }
    if (planar_order)
        return out;
    const size_t groups = K - 1;
    std::unordered_map<int64_t, size_t> support_g;
    std::vector<size_t>                 floor_of(out.size(), 0);
    size_t cycles = 0;
    for (const std::vector<size_t> &layer_roads : by_layer) {
        std::vector<std::pair<size_t, size_t>> assigned;
        for (size_t ri : layer_roads) {
            size_t g = groups - 1, floor_g = 0;
            for (const Vec3d &q : out[ri].tip) {
                const double phi = field.phi(q);
                const size_t own = size_t(std::max<ptrdiff_t>(0, std::lower_bound(tops.begin(), tops.end(), phi) - tops.begin() - 1));
                g = std::min(g, std::min(own, groups - 1));
                int ix, iy;
                if (!cell_of(q.x(), q.y(), ix, iy))
                    continue;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int64_t k = key(ix + dx, iy + dy);
                        if (dx == 0 && dy == 0)
                            if (auto it = support_g.find(k); it != support_g.end())
                                floor_g = std::max(floor_g, it->second);
                        if (auto it = part_at.find(k); it != part_at.end())
                            for (const auto &[p, l] : it->second) {
                                const double z = p.z();
                                if (z > q.z() + 0.3 && z < q.z() + 4.)
                                    g = std::min(g, l);
                                if (z < q.z() - 0.05 && z > q.z() - 1.)
                                    floor_g = std::max(floor_g, l + 1);
                            }
                    }
            }
            if (floor_g > g)
                ++cycles;
            floor_of[ri] = std::min(floor_g, groups - 1);
            assigned.emplace_back(ri, std::min(std::max(g, floor_g), groups - 1));
        }
        for (const auto &[ri, g] : assigned) {
            out[ri].layer    = g;
            out[ri].deformed = out[ri].tip;
            for (Vec3d &d : out[ri].deformed) {
                d.z() = tops[g + 1];
                int ix, iy;
                if (cell_of(d.x(), d.y(), ix, iy)) {
                    size_t &sg = support_g[key(ix, iy)];
                    sg         = std::max(sg, g);
                }
            }
        }
    }
    constexpr double Above = 4.;
    const double     land  = 0.8 + 0.5 * w, tan30 = std::tan(M_PI / 6.);
    const int        reach = int(std::ceil((land + Above * tan30) / Cell));
    std::unordered_map<int64_t, std::vector<std::pair<double, size_t>>> higher;
    size_t                                                              head_cycles = 0;
    for (auto layer_it = by_layer.rbegin(); layer_it != by_layer.rend(); ++layer_it) {
        std::vector<std::pair<size_t, size_t>> capped;
        for (size_t ri : *layer_it) {
            size_t cap = out[ri].layer;
            for (const Vec3d &q : out[ri].tip) {
                int ix, iy;
                if (!cell_of(q.x(), q.y(), ix, iy))
                    continue;
                for (int dy = -reach; dy <= reach; ++dy)
                    for (int dx = -reach; dx <= reach; ++dx)
                        if (auto it = higher.find(key(ix + dx, iy + dy)); it != higher.end())
                            for (const auto &[z, g] : it->second) {
                                const double dz = z - q.z();
                                if (g < cap && dz > 0.12 && dz < Above &&
                                    (grid.node(ix + dx, iy + dy) - q.head<2>()).norm() < land + dz * tan30 + 0.5 * Cell)
                                    cap = g;
                            }
            }
            if (cap < floor_of[ri]) {
                ++head_cycles;
                out[ri].path.reset();
                out[ri].tip.clear();
                continue;
            }
            capped.emplace_back(ri, std::max(cap, floor_of[ri]));
        }
        for (const auto &[ri, g] : capped) {
            out[ri].layer = g;
            for (Vec3d &d : out[ri].deformed)
                d.z() = tops[g + 1];
            for (const Vec3d &q : out[ri].tip) {
                int ix, iy;
                if (cell_of(q.x(), q.y(), ix, iy))
                    higher[key(ix, iy)].emplace_back(q.z(), g);
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Native planar support " << object.model_object()->name << ": " << columns.size() << " columns, " << by_layer.size()
                            << " layers, " << out.size() << " roads, " << cycles << " order cycles, " << head_cycles << " head cycles";
    return out;
}

bool generate_planar_native_support(PrintObject &object)
{
    if (object.config().support_style.value != smsNative || !object.config().enable_support.value)
        return false;
    if (object.layers().size() < 3)
        return true;

    class PlanarField final : public LayerField
    {
    public:
        double phi(const Vec3d &p) const override { return p.z(); }
        double z_at(const Vec2d &, double level) const override { return level; }
        Vec3d gradient(const Vec3d &) const override { return Vec3d::UnitZ(); }
    } field;
    Vec2d phase = Vec2d::Zero();
    const char *phase_x = research_switch("NP_PLANAR_NATIVE_SUPPORT_PHASE_X");
    const char *phase_y = research_switch("NP_PLANAR_NATIVE_SUPPORT_PHASE_Y");
    if (phase_x)
        phase.x() = std::atof(phase_x);
    if (phase_y)
        phase.y() = std::atof(phase_y);
    if (!phase.allFinite() || phase.minCoeff() < 0. || phase.maxCoeff() >= 0.5)
        throw Slic3r::SlicingError("Native planar support grid phase must be in [0, 0.5) mm.");

    std::vector<FieldRoad> part;
    std::vector<ExPolygons> domains;
    std::vector<double> tops;
    for (const Layer *layer : object.layers()) {
        const size_t k = tops.size();
        tops.push_back(layer->print_z);
        domains.push_back(layer->lslices);
        std::vector<const ExtrusionPath *> paths;
        for (const LayerRegion *region : layer->regions()) {
            flatten(&region->perimeters, paths);
            flatten(&region->thin_fills, paths);
            flatten(&region->fills, paths);
        }
        for (const ExtrusionPath *path : paths) {
            FieldRoad road;
            road.layer = k;
            Polyline3 line;
            for (const Point3 &p : path->polyline.points) {
                const Vec3d tip(unscale<double>(p.x()), unscale<double>(p.y()), layer->print_z);
                road.tip.push_back(tip);
                line.points.push_back(Point3::new_scale(tip.x(), tip.y(), tip.z()));
            }
            if (road.tip.size() < 2)
                continue;
            const size_t n = road.tip.size();
            road.path = std::make_unique<ExtrusionPathConformal>(std::move(line), *path,
                std::vector<float>(n, path->height), std::vector<Vec3f>(n, Vec3f::UnitZ()));
            part.push_back(std::move(road));
        }
    }
    const double           base_pad = object.config().native_support_base_pad.value;
    std::vector<FieldRoad> roads    = native_planar_support_roads(object, field, part, domains, tops, tops.front(),
                                                                  object.config().layer_height.value, 0., 0., true, phase, true, base_pad);
    std::map<coord_t, std::vector<const FieldRoad *>> by_z;
    for (const FieldRoad &road : roads)
        if (road.path && road.tip.size() >= 2)
            by_z[scale_(road.tip.front().z())].push_back(&road);
    for (const auto &[z, layer_roads] : by_z) {
        SupportLayer *layer = object.add_support_layer(int(object.support_layers().size()),
            int(object.support_layers().size()), layer_roads.front()->path->height, unscale<double>(z));
        Polygons islands;
        for (const FieldRoad *road : layer_roads) {
            auto path = std::make_unique<ExtrusionPath>(road->path->role(), road->path->mm3_per_mm,
                                                       road->path->width, road->path->height);
            for (const Vec3d &tip : road->tip)
                path->polyline.points.push_back(Point3::new_scale(tip.x(), tip.y(), 0.));
            path->polygons_covered_by_width(islands, 0.f);
            layer->support_fills.entities.push_back(path.release());
        }
        layer->support_islands = union_ex(islands);
    }
    for (size_t i = 1; i < object.support_layers().size(); ++i) {
        object.support_layers()[i]->lower_layer = object.support_layers()[i - 1];
        object.support_layers()[i - 1]->upper_layer = object.support_layers()[i];
    }
    BOOST_LOG_TRIVIAL(info) << "Native planar support style " << object.model_object()->name << ": " << part.size()
                            << " unchanged part roads, " << by_z.size() << " support layers, base pad " << base_pad << " mm, grid phase " << phase.transpose();
    return true;
}

void build_native_patch(PrintObject &object, const std::shared_ptr<const LayerField> &field, ConformalResult &result)
{
    if (!field || object.layers().empty())
        return;
    const auto   t0      = std::chrono::steady_clock::now();
    const size_t host_id = object.layers().size() - 1;
    Layer       *host    = object.layers()[host_id];
    const double raft    = host->print_z - (host->slice_z + 0.5 * host->height);
    const double first   = object.print()->config().initial_layer_print_height.value;
    const double h       = object.config().layer_height.value;
    const std::unique_ptr<ToolKinematics> kinematics = make_tool_kinematics(object.print()->config());
    if (!kinematics)
        return;
    const double tilt_cap = std::max(0., tilt_budget(*kinematics) - Geometry::deg2rad(5.));

    const indexed_triangle_set mesh = slicing_frame_mesh(object);
    double                     top  = 0.;
    for (const stl_vertex &v : mesh.vertices)
        top = std::max(top, field->phi(v.cast<double>()));
    std::vector<double> tops, mids;
    const auto *conformal_field = dynamic_cast<const ConformalLayerField *>(field.get());
    const bool  banded = conformal_field != nullptr && std::abs(conformal_field->band_height() - conformal_field->body_height()) > EPSILON;
    if (const double t_max = adaptive_level_ratio(); t_max > 1.)
        tops = adaptive_levels(mesh, *field, first, top, h, t_max);
    else if (banded)
        tops = conformal_field->levels(first, top);
    else {
        const size_t K0 = size_t(std::max(1., std::ceil((top - first) / h - 1e-6) + 1.));
        for (size_t k = 0; k < K0; ++k)
            tops.push_back(first + double(k) * h);
    }
    const size_t K = tops.size();
    mids.resize(K);
    for (size_t k = 0; k < K; ++k)
        mids[k] = k == 0 ? 0.5 * first : 0.5 * (tops[k - 1] + tops[k]);
    Native::Params np;
    np.fixed_head    = true;
    np.mesh_edge_max = 0.5;
    const std::vector<Native::Layer> layers = Native::extract_layers(mesh, *field, mids, np);
    std::vector<ExPolygons>          domains(K);
    BoundingBox                      bbox;
    const auto *cut = dynamic_cast<const SlantField *>(field.get());
    if (cut != nullptr && !cut->cut())
        cut = nullptr;
    for (size_t k = 0; k < K; ++k)
        if (!layers[k].pieces.empty()) {
            domains[k] = layers[k].pieces.front().domain;
            if (cut != nullptr)
                if (const std::optional<double> limit = cut->cut_limit(int(std::lround((tops[k] - first) / h)))) {
                    const Vec2d u = cut->shape().u, v(-u.y(), u.x());
                    const double L = 1e4;
                    Polygon      half;
                    for (const Vec2d &q : { Vec2d(u * *limit - v * L), Vec2d(u * *limit + v * L), Vec2d(u * (*limit - L) + v * L),
                                            Vec2d(u * (*limit - L) - v * L) })
                        half.points.emplace_back(scaled(q));
                    half.make_counter_clockwise();
                    domains[k] = intersection_ex(domains[k], ExPolygons{ ExPolygon(half) });
                }
            bbox.merge(get_extents(domains[k]));
        }
    if (!bbox.defined)
        return;
    throw_if_canceled(object);

    const Point            c_scaled = bbox.center();
    const Vec2d            c        = unscale(c_scaled);
    std::vector<ExPolygons> shifted = domains;
    for (ExPolygons &d : shifted)
        for (ExPolygon &e : d)
            e.translate(-c_scaled.x(), -c_scaled.y());
    static std::atomic<int> serial { 0 };
    const std::string       name = object.model_object()->name + " native#" + std::to_string(serial++);
    DynamicPrintConfig      config = chunk_print_config(object);
    config.set_deserialize_strict("initial_layer_print_height", object.print()->config().opt_serialize("initial_layer_print_height"));
    config.set_deserialize_strict("ironing_type", "no ironing");
    const bool conformal = object.config().nonplanar_mode.value == NonplanarMode::ConformalLayers ||
                           object.config().nonplanar_mode.value == NonplanarMode::Slant;
    const bool slant = object.config().nonplanar_mode.value == NonplanarMode::Slant;
    if (conformal) {
        if (const PrintRegionConfig &rcfg = object.printing_region(0).config(); rcfg.nonplanar_top_order.value != NonplanarTopOrder::Follow)
            config.set_deserialize_strict("top_surface_pattern", nonplanar_top_monotonic(rcfg) ? "monotonic" : "rectilinear");
        config.set_deserialize_strict("internal_solid_infill_pattern", "rectilinear");
        config.set_deserialize_strict("infill_wall_overlap", "-30%");
        config.set_deserialize_strict("top_bottom_infill_wall_overlap", "-15%");
        if (const auto *pattern = config.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern");
            pattern != nullptr && (pattern->value == ipGrid || pattern->value == ipTriangles || pattern->value == ipStars || pattern->value == ipCubic))
            config.set_deserialize_strict("sparse_infill_pattern", "rectilinear");
        const double direction = object.printing_region(0).config().nonplanar_top_direction.value;
        config.set_deserialize_strict("top_layer_direction", direction >= 0. ? float_to_string_decimal_point(direction) : "-1");
        config.set_deserialize_strict("bottom_layer_direction", "-1");
        double dominant = -1.;
        config.set_deserialize_strict("solid_infill_rotate_template",
                                      contour_solid_angles(*field, domains, tops, object.printing_region(0).config().solid_infill_direction.value,
                                                           false, &dominant));
        if (dominant >= 0.)
            config.set_key_value("internal_bridge_angle", new ConfigOptionFloat(dominant < 1. ? 180. : dominant));
        if (const auto *pattern = config.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern");
            pattern != nullptr && (pattern->value == ipRectilinear || pattern->value == ipAlignedRectilinear || pattern->value == ipLine))
            config.set_deserialize_strict("sparse_infill_rotate_template",
                                          contour_solid_angles(*field, domains, tops, object.printing_region(0).config().infill_direction.value, true));
    }
    if (slant)
        config.set_deserialize_strict("gap_fill_target", "nowhere");
    for (const std::string &key : object.config().keys())
        if ((key.rfind("support_", 0) == 0 || key.rfind("tree_support", 0) == 0 || key == "enable_support") && config.has(key))
            config.set_key_value(key, object.config().option(key)->clone());
    config.set_deserialize_strict("independent_support_layer_height", "0");
    if (native_planar_support())
        config.set_key_value("enable_support", new ConfigOptionBool(false));
    const double            height = banded ? tops.back() : first + double(K - 1) * h;
    TriangleMesh            box    = make_cube(unscale<double>(bbox.size().x()), unscale<double>(bbox.size().y()), height);
    box.translate(float(unscale<double>(bbox.min.x())), float(unscale<double>(bbox.min.y())), 0.f);
    Model        model;
    ModelObject *model_object = model.add_object();
    model_object->name        = name;
    model_object->add_volume(std::move(box));
    if (banded) {
        const double anchor = std::max(first, conformal_field->level_anchor()), body = conformal_field->body_height();
        std::vector<coordf_t> profile { 0., first, first - 1e-4, first };
        if (anchor > first + EPSILON)
            append(profile, std::vector<coordf_t> { first, body, anchor - 1e-4, body });
        append(profile, std::vector<coordf_t> { anchor, conformal_field->band_height(), height, conformal_field->band_height() });
        model_object->layer_height_profile.set(profile);
    }
    model_object->add_instance();
    const NativeSliceOverride registered(name, std::move(shifted));
    Print                     print;
    const NestedFieldSlice    nested;
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
        throw Slic3r::SlicingError(std::string(_u8L("Native curved layers: slicing failed: ")) + print.slicing_errors_message(errors));
    } catch (const std::exception &ex) {
        throw Slic3r::SlicingError(std::string(_u8L("Native curved layers: slicing failed: ")) + ex.what());
    }
    throw_if_canceled(object);
    if (print.objects().empty())
        return;
    const PrintObject &sliced = *print.objects().front();
    const Vec2d        center = unscale(sliced.center_offset());
    if ((center - c).norm() > 1e-3)
        BOOST_LOG_TRIVIAL(warning) << "Native layers " << object.model_object()->name << ": nested centre " << center.transpose()
                                   << " against the areas' " << c.transpose();
    const size_t n = std::min(K, sliced.layers().size());
    if (sliced.layers().size() != K)
        BOOST_LOG_TRIVIAL(warning) << "Native layers " << object.model_object()->name << ": " << sliced.layers().size()
                                   << " nested layers for " << K << " field layers";

    std::vector<FieldRoad>  roads;
    std::vector<ExPolygons> internal;
    Native::Piece           over_xy;
    size_t                  points = 0;
    std::vector<std::vector<const ExtrusionPath *>> support(n);
    size_t                                          support_paths = 0;
    for (const SupportLayer *layer : sliced.support_layers()) {
        long k = std::lround((layer->print_z - first) / h);
        if (banded && layer->print_z <= tops.back() + h)
            k = long(std::min_element(tops.begin(), tops.end(), [z = layer->print_z](double a, double b) {
                         return std::abs(a - z) < std::abs(b - z);
                     }) - tops.begin());
        if (k >= 1 && size_t(k) < n) {
            flatten(&layer->support_fills, support[size_t(k)]);
            support_paths += support[size_t(k)].size();
        }
    }
    const SeamPosition        seam_pos = object.config().seam_position.value;
    const Vec2d               rear(0., unscale<double>(bbox.max.y() - c_scaled.y()) + 10.);
    size_t                    seam_k   = 0;
    const SeamCost            seam_cost = [&](const Point &p) {
        const Vec2d  q = unscale(p);
        const Vec2d  xy = q + c;
        const double z  = field->z_at(xy, tops[seam_k]);
        const double tilt = std::isfinite(z) ? Geometry::rad2deg(std::acos(std::clamp(Native::tool_axis(*field, Vec3d(xy.x(), xy.y(), z), tilt_cap).z(), -1., 1.))) : 0.;
        return (q - rear).norm() + tilt;
    };
    const SeamCost           *seam_at  = seam_pos == spNearest || seam_pos == spRandom ? nullptr : &seam_cost;
    std::deque<ExtrusionLoop> seamed;
    for (size_t k = 1; k < n; ++k) {
        seam_k = k;
        const Layer &layer = *sliced.layers()[k];
        internal.push_back(domains[k]);
        std::vector<const ExtrusionPath *> paths = support[k];
        for (const LayerRegion *region : layer.regions()) {
            flatten(&region->perimeters, paths, seam_at, &seamed);
            flatten(&region->thin_fills, paths);
        }
        for (const LayerRegion *region : layer.regions())
            flatten(&region->fills, paths);
        for (const ExtrusionPath *path : paths) {
            if (path->polyline.points.size() < 2)
                continue;
            Polyline road;
            for (const Point3 &p : path->polyline.points)
                road.points.emplace_back(p.x() + c_scaled.x(), p.y() + c_scaled.y());
            std::vector<Vec3d> tips = Native::lift_polyline(over_xy, road, *field, tops[k], 0.02);
            if (tips.size() < 2)
                continue;
            for (Vec3d &q : tips)
                q.z() = std::max(q.z(), first + 0.5 * h);
            const size_t       m = tips.size();
            std::vector<float> heights(m), widths(m);
            std::vector<Vec3f> axes(m);
            Polyline3          polyline;
            static const bool  upright_support = [] { const char *v = research_switch("NP_NATIVE_SUPPORT_UPRIGHT"); return !v || std::atoi(v) != 0; }();
            const bool         upright         = upright_support && is_support(path->role());
            for (size_t i = 0; i < m; ++i) {
                const Vec3d a = upright ? Vec3d::UnitZ() : Native::tool_axis(*field, tips[i], tilt_cap);
                axes[i]       = a.cast<float>();
                heights[i]    = float(std::clamp(Native::bead_height(*field, tips[i], tops[k - 1], tops[k], a), (cut != nullptr ? 0.1 : conformal ? 0.25 : 0.5) * h,
                                                 (conformal ? 1.8 : 1.6) * h));
                const Vec3d t = tips[std::min(i + 1, m - 1)] - tips[i > 0 ? i - 1 : 0];
                double      fac = 1.;
                const Vec3d g   = field->gradient(tips[i]);
                if (t.head<2>().squaredNorm() > 1e-12 && g.z() > 1e-9) {
                    const Vec2d td    = t.head<2>().normalized();
                    const Vec2d l(-td.y(), td.x());
                    const Vec2d slope = -g.head<2>() / g.z();
                    fac               = std::sqrt(1. + std::pow(slope.dot(l), 2));
                }
                widths[i] = float(path->width * std::clamp(fac, 0.6, 1.6));
                polyline.points.push_back(Point3::new_scale(tips[i].x(), tips[i].y(), tips[i].z() + raft - host->print_z));
            }
            const double             rate  = Geometry::deg2rad(is_infill(path->role()) && path->role() != erTopSolidInfill ? 3. : 5.);
            const std::vector<Vec3f> exact = axes;
            for (int pass = 0; pass < 2; ++pass)
                for (size_t q = 1; q < m; ++q) {
                    const size_t i = pass == 0 ? q : m - 1 - q, j = pass == 0 ? i - 1 : i + 1;
                    const Vec3d  a = axes[j].cast<double>(), b = axes[i].cast<double>();
                    const double allowed = rate * std::max((tips[i] - tips[j]).norm(), 1e-3);
                    const double angle   = std::acos(std::clamp(a.dot(b), -1., 1.));
                    if (angle > allowed)
                        axes[i] = (Eigen::Quaterniond::Identity().slerp(allowed / angle, Eigen::Quaterniond::FromTwoVectors(a, b)) * a).cast<float>();
                }
            const double max_lag = Geometry::deg2rad(10.);
            for (size_t i = 0; i < m; ++i) {
                const Vec3d  e = exact[i].cast<double>(), a = axes[i].cast<double>();
                const double off = std::acos(std::clamp(e.dot(a), -1., 1.));
                if (off > max_lag)
                    axes[i] = (Eigen::Quaterniond::Identity().slerp(1. - max_lag / off, Eigen::Quaterniond::FromTwoVectors(a, e)) * a).cast<float>();
            }
            const double axis_tube = Geometry::deg2rad(native_axis_key("NP_NATIVE_AXIS_TUBE", 0., kinematics->rotary_count() > 0));
            const double axis_smooth = native_axis_key("NP_NATIVE_AXIS_SMOOTH", 5., kinematics->rotary_count() > 0);
            if (axis_smooth > 0. && !upright)
                axes = Native::smooth_axes(tips, exact, axis_smooth, max_lag);
            if (axis_tube > 0. && !upright) {
                const auto &rot = kinematics->rotary_axes();
                axes = Native::tube_axes(axis_smooth > 0. ? axes : exact, axis_tube, tilt_cap, rot[0].max_speed_deg_s > 0. ? rot[0].max_speed_deg_s : 1.,
                                         rot[1].max_speed_deg_s > 0. ? rot[1].max_speed_deg_s : 1.);
            }
            FieldRoad r;
            r.path             = std::make_unique<ExtrusionPathConformal>(std::move(polyline), *path, std::move(heights), std::move(axes));
            r.path->bead_width = std::move(widths);
            r.deformed.reserve(m);
            for (const Vec3d &p : tips)
                r.deformed.emplace_back(p.x(), p.y(), mids[k]);
            r.tip   = std::move(tips);
            r.layer = k - 1;
            points += m;
            roads.push_back(std::move(r));
        }
    }
    if (native_planar_support() && object.config().enable_support.value) {
        std::vector<FieldRoad> planar = native_planar_support_roads(object, *field, roads, domains, tops, first, h, raft, host->print_z);
        for (FieldRoad &r : planar)
            if (r.path)
                roads.push_back(std::move(r));
    }
    FieldOrderParams op;
    op.head       = resolve_head_envelope(object.print()->config());
    op.kinematics = kinematics.get();
    op.max_tilt   = std::max(0., tilt_budget(*kinematics) - Geometry::deg2rad(1.));
    op.lean_max   = 0.;
    op.planar_support = native_planar_support() && object.config().enable_support.value;
    op.axis_blend     = native_axis_key("NP_NATIVE_AXIS_BLEND", 10., kinematics->rotary_count() > 0);
    op.layer_top.assign(tops.begin() + 1, tops.begin() + std::ptrdiff_t(n));
    op.layer_height.resize(n - 1);
    for (size_t k = 1; k < n; ++k)
        op.layer_height[k - 1] = tops[k] - tops[k - 1];
    const char *road_ids_env = research_switch("NP_NATIVE_ROAD_IDS");
    const bool road_ids = road_ids_env != nullptr && *road_ids_env != '\0' && std::string(road_ids_env) != "0";
    if (road_ids) {
        static std::atomic<uint64_t> next_road_id{0};
        for (FieldRoad &road : roads) {
            const uint64_t id = next_road_id.fetch_add(1, std::memory_order_relaxed);
            if (id >= UINT32_MAX)
                throw std::runtime_error("Native road identity exceeds uint32 range");
            if (road.path)
                road.path->native_road_id = uint32_t(id);
        }
    }
    const char *dump_prefix = research_switch("NP_NATIVE_DUMP_ROADS");
    auto dump = [&](const char *stage, const auto &visit) {
        if (dump_prefix == nullptr || *dump_prefix == '\0')
            return;
        const std::string path = std::string(dump_prefix) + "." +
                                 std::to_string(object.model_object()->id().id) + "." + stage + ".tsv";
        boost::nowide::ofstream out(path);
        if (!out)
            throw std::runtime_error("Cannot open NP_NATIVE_DUMP_ROADS file: " + path);
        out << std::setprecision(17)
            << "# native-road-v1 slicing frame, mm; ordered axes precede G-code settle/RTCP\n"
            << "group\troad\tpoint\tlayer\tphi_top\trole\tbase_width\tx\ty\tz"
               "\tax\tay\taz\tused_ax\tused_ay\tused_az\twidth\theight\tgx\tgy\tgz"
            << (road_ids ? "\tnative_road_id" : "") << '\n';
        auto write = [&](const ExtrusionPathConformal &path, size_t group, size_t road, size_t layer) {
            const bool upright = std::all_of(path.tool_axis.begin(), path.tool_axis.end(), [](const Vec3f &a) {
                return a.cast<double>().normalized().z() >= std::cos(Geometry::deg2rad(UprightRoadDeg));
            });
            for (size_t i = 0; i < path.polyline.points.size(); ++i) {
                const Point3 &point = path.polyline.points[i];
                const Vec3d p(unscale<double>(point.x()), unscale<double>(point.y()),
                              unscale<double>(point.z()) + host->print_z - raft);
                const Vec3d axis = i < path.tool_axis.size() ? Vec3d(path.tool_axis[i].cast<double>()) : Vec3d(Vec3d::UnitZ());
                const Vec3d used = upright ? Vec3d::UnitZ() : axis;
                const Vec3d g = field->gradient(p);
                const double width = i < path.bead_width.size() ? path.bead_width[i] : path.width;
                const double height = i < path.bead_height.size() ? path.bead_height[i] : path.height;
                out << group << '\t' << road << '\t' << i << '\t' << layer << '\t' << op.layer_top.at(layer)
                    << '\t' << int(path.role()) << '\t' << path.width
                    << '\t' << p.x() << '\t' << p.y() << '\t' << p.z()
                    << '\t' << axis.x() << '\t' << axis.y() << '\t' << axis.z()
                    << '\t' << used.x() << '\t' << used.y() << '\t' << used.z()
                    << '\t' << width << '\t' << height << '\t' << g.x() << '\t' << g.y() << '\t' << g.z();
                if (road_ids)
                    out << '\t' << path.native_road_id;
                out << '\n';
            }
        };
        visit(write);
        out.flush();
        if (!out)
            throw std::runtime_error("Cannot write NP_NATIVE_DUMP_ROADS file: " + path);
    };
    dump("raw", [&](const auto &write) {
        for (size_t ri = 0; ri < roads.size(); ++ri)
            if (roads[ri].path)
                write(*roads[ri].path, roads[ri].layer, ri, roads[ri].layer);
    });
    const size_t n_roads = roads.size();
    BandOrderStats band_stats;
    const size_t   conflicts = conformal ? layer_order_head_conflicts(roads, n - 1, op.head) : 0;
    if (conformal)
        BOOST_LOG_TRIVIAL(info) << "Conformal layers " << object.model_object()->name << ": " << conflicts
                                << " cells where whole layers would meet the head";
    const std::vector<std::pair<Vec3d, Vec3d>> precede = conformal ? conformal_precedence(*object.model_object()) :
                                                                     std::vector<std::pair<Vec3d, Vec3d>>();
    FieldOrder     order = conformal ? band_order_roads(roads, n - 1, conflicts > op.head.duct_graze_limit ? conformal_band_height(op.head, h) :
                                                                                      std::numeric_limits<double>::infinity(), &band_stats, &precede, &domains) :
                                       layer_order_field_roads(roads, op);
    if (conformal)
        BOOST_LOG_TRIVIAL(info) << "Conformal layers " << object.model_object()->name << ": band " << band_stats.band << " mm from z "
                                << band_stats.offset << ", " << band_stats.bands << " bands, " << band_stats.groups << " groups, "
                                << band_stats.cut_roads << " roads cut";

    dump("ordered", [&](const auto &write) {
        size_t ri = 0;
        for (size_t gi = 0; gi < order.groups.size(); ++gi)
            for (const ExtrusionEntity *entity : order.groups[gi].entities)
                write(*static_cast<const ExtrusionPathConformal *>(entity), gi, ri++, order.group_layers.at(gi));
    });

    ConformalPatch patch;
    patch.id           = int(result.patches.size());
    patch.kind         = PatchKind::Field;
    patch.native       = true;
    patch.banded       = conformal && band_stats.bands > 1;
    patch.shell_height = h;
    patch.host_layer   = host_id;
    patch.region_id    = 0;
    patch.field        = std::make_shared<const NativeChart>(field, tilt_cap);
    patch.z_min        = std::numeric_limits<double>::infinity();
    patch.z_max        = -std::numeric_limits<double>::infinity();
    for (size_t g = 0; g < order.groups.size(); ++g) {
        ExtrusionEntityCollection &group = order.groups[g];
        if (g < order.group_layers.size() && order.group_layers[g] < internal.size())
            patch.shell_internal.push_back(internal[order.group_layers[g]]);
        for (const ExtrusionEntity *ee : group.entities)
            for (const Point3 &p : static_cast<const ExtrusionPath *>(ee)->polyline.points) {
                const double z = unscale<double>(p.z()) + host->print_z;
                patch.z_min    = std::min(patch.z_min, z);
                patch.z_max    = std::max(patch.z_max, z);
            }
        patch.shell_paths.push_back(std::move(group));
    }
    object.set_native_summary(Slic3r::format("%1% layers, %2% roads, %3% support paths, tilt cap %4$.0f deg", K, n_roads, support_paths,
                                             Geometry::rad2deg(tilt_cap)));
    BOOST_LOG_TRIVIAL(info) << "Native layers " << object.model_object()->name << ": " << K << " layers to level " << top << ", "
                            << n_roads << " roads (" << support_paths << " support paths), " << points << " points, " << patch.shell_paths.size() << " groups, tilt cap "
                            << Geometry::rad2deg(tilt_cap) << " deg, "
                            << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s";
    for (size_t layer_id = 1; layer_id < object.layers().size(); ++layer_id) {
        Layer *layer = object.layers()[layer_id];
        layer->lslices.clear();
        layer->lslices_extrudable.clear();
        layer->lslices_bboxes.clear();
    }
    for (size_t layer_id = 1; layer_id < object.layers().size(); ++layer_id)
        for (LayerRegion *region : object.layers()[layer_id]->regions()) {
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
        object.layers()[layer_id]->conformal_covered = true;
    patch.shells = int(patch.shell_paths.size());
    host->conformal_patches_after.push_back(patch.id);
    result.patches.push_back(std::move(patch));
}

}
