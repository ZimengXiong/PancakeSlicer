#include "Rests.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "MultiplanarConfirm.hpp"

#include "ConformalShells.hpp"
#include "Multiplanar.hpp"

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/log/trivial.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {

constexpr double Step = 0.25, CellXY = 1., CellZ = 0.5, OwnRun = 2.;

struct Sample
{
    Vec3f    p;
    float    along;
    float    width;
    float    length;
    uint32_t run;
    bool     support, sparse;
    bool     touch;
};
struct Probe
{
    Vec3d    p, axis;
    double   along, height;
    uint32_t run;
};

int64_t key(int x, int y, int z) { return (int64_t(x) * 73856093) ^ (int64_t(y) * 19349663) ^ (int64_t(z) * 83492791); }

class Walker
{
public:
    Walker(double bed, double layer, double spacing, bool rotary, double interface = -1.)
        : m_bed(bed), m_layer(layer), m_spacing(spacing), m_rotary(rotary), m_interface(interface)
    {}

    void path(const ExtrusionPath &path, double z)
    {
        if (path.mm3_per_mm <= 0. || path.is_force_no_extrusion() || path.polyline.points.size() < 2)
            return;
        const ExtrusionRole role = path.role();
        if (role == erIroning)
            return;
        const auto         *conformal = dynamic_cast<const ExtrusionPathConformal *>(&path);
        const Points3      &pts       = path.polyline.points;
        const uint32_t      run       = m_runs++;
        double              along     = 0.;
        for (size_t i = 1; i < pts.size(); ++i) {
            double h = path.height, w = path.width;
            if (conformal != nullptr) {
                if (i < conformal->bead_height.size() && (conformal->bead_height[i - 1] <= 0.f || conformal->bead_height[i] <= 0.f))
                    continue;
                if (i < conformal->bead_height.size())
                    h = 0.5 * (conformal->bead_height[i - 1] + conformal->bead_height[i]);
                if (!conformal->bead_width.empty() && i < conformal->bead_width.size())
                    w = 0.5 * (conformal->bead_width[i - 1] + conformal->bead_width[i]);
            }
            Vec3d ax = Vec3d::UnitZ();
            if (m_rotary && conformal != nullptr && i < conformal->tool_axis.size() && conformal->tool_axis[i].squaredNorm() > 0.f)
                ax = conformal->tool_axis[i].cast<double>().normalized();
            auto at = [&](size_t j) -> Vec3d {
                return Vec3d(unscale<double>(pts[j].x()), unscale<double>(pts[j].y()),
                             conformal != nullptr || path.z_contoured ? z + unscale<double>(pts[j].z()) : z) - 0.5 * h * ax;
            };
            this->segment(at(i - 1), at(i), ax, w, h, role, run, along);
        }
    }
    uint32_t new_run() { return m_runs++; }
    void     move(const Vec3d &from, const Vec3d &to, double w, double h, ExtrusionRole role, uint32_t run, double &along,
                  const Vec3d &ax = Vec3d::UnitZ())
    {
        const Vec3d down = 0.5 * h * ax;
        this->segment(from - down, to - down, ax, w, h, role, run, along);
    }
    void segment(const Vec3d &a, const Vec3d &b, const Vec3d &ax, double w, double h, ExtrusionRole role, uint32_t run, double &along)
    {
        const bool   checked = role != erInternalInfill && role != erBridgeInfill && role != erInternalBridgeInfill &&
                             role != erSolidInfill && role != erSupportMaterialInterface && role != erSupportTransition;
        const bool   surface = role == erTopSolidInfill || role == erBottomSurface;
        const double l       = (b - a).norm();
        const int    n       = std::max(1, int(std::ceil(l / Step)));
        for (int k = 0; k < n; ++k) {
            const Vec3d  p = a + (b - a) * ((k + 0.5) / n);
            const double s = along + l * (k + 0.5) / n;
            if (checked)
                m_pending.push_back({ Probe{ p, ax, s, h, run }, w, uint32_t(m_samples.size()), surface, role });
            this->add(Sample{ p.cast<float>(), float(s), float(w), float(l / n), run, is_support(role), role == erInternalInfill,
                              role == erSupportMaterialInterface || p.z() <= m_bed + 0.75 * m_layer });
        }
        along += l;
        if (m_pending.size() >= PendingBatch)
            this->judge();
    }
    void raised(const ExtrusionEntity &ee, double z)
    {
        if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&ee)) {
            for (const ExtrusionPath &p : loop->raised)
                this->path(p, z);
        } else if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(&ee)) {
            for (const ExtrusionEntity *e : coll->entities)
                this->raised(*e, z);
        }
    }
    void entity(const ExtrusionEntity &ee, double z)
    {
        std::vector<const ExtrusionPath *> paths;
        collect_paths(&ee, paths);
        for (const ExtrusionPath *p : paths)
            this->path(*p, z);
    }

    Floating floating()
    {
        this->judge();
        Floating out;
        for (size_t i : m_floating)
            (m_samples[i].support ? out.support : out.part) += m_samples[i].length;
        constexpr double    ClusterReach = 1., MaxSpan = 5.;
        std::vector<size_t> part;
        for (size_t i : m_floating)
            if (!m_samples[i].support)
                part.push_back(i);
        std::vector<size_t> parent(part.size());
        std::iota(parent.begin(), parent.end(), size_t(0));
        const auto find = [&parent](size_t i) {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        const auto cell = [](const Vec3f &p, int dx, int dy, int dz) {
            return key(int(std::floor(p.x() / ClusterReach)) + dx, int(std::floor(p.y() / ClusterReach)) + dy,
                       int(std::floor(p.z() / ClusterReach)) + dz);
        };
        std::unordered_map<int64_t, std::vector<size_t>> grid;
        for (size_t k = 0; k < part.size(); ++k) {
            const Vec3f &p = m_samples[part[k]].p;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (auto it = grid.find(cell(p, dx, dy, dz)); it != grid.end())
                            for (size_t o : it->second)
                                if ((m_samples[part[o]].p - p).norm() <= ClusterReach)
                                    parent[find(k)] = find(o);
            grid[cell(p, 0, 0, 0)].push_back(k);
        }
        struct Cluster { Vec3f lo, hi; double road = 0.; };
        std::unordered_map<size_t, Cluster> clusters;
        for (size_t k = 0; k < part.size(); ++k) {
            const Sample &s = m_samples[part[k]];
            auto [it, fresh] = clusters.try_emplace(find(k), Cluster{ s.p, s.p, 0. });
            it->second.lo = it->second.lo.cwiseMin(s.p);
            it->second.hi = it->second.hi.cwiseMax(s.p);
            it->second.road += s.length;
        }
        for (const auto &[root, c] : clusters) {
            const Vec3f extent = c.hi - c.lo;
            if (extent.z() >= m_layer || std::max(extent.x(), extent.y()) > MaxSpan)
                out.air += c.road;
        }
        return out;
    }

private:
    struct Pending
    {
        Probe         probe;
        double        width;
        uint32_t      id;
        bool          surface;
        ExtrusionRole role;
    };
    static constexpr size_t PendingBatch = 1 << 16;
    void judge()
    {
        if (m_pending.empty())
            return;
        std::vector<char> floats(m_pending.size(), 0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, m_pending.size()), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const Pending &q = m_pending[i];
                floats[i] = !this->rests(q.probe, q.probe.height, q.width, q.id) && !(q.surface && this->spans(q.probe, q.id));
            }
        });
        static const bool dump = research_switch("NP_FLOAT_DUMP") != nullptr;
        for (size_t i = 0; i < m_pending.size(); ++i)
            if (floats[i]) {
                m_floating.push_back(m_pending[i].id);
                if (dump) {
                    const Vec3d &p = m_pending[i].probe.p;
                    BOOST_LOG_TRIVIAL(warning) << "float " << ExtrusionEntity::role_to_string(m_pending[i].role) << " " << p.x() << " "
                                               << p.y() << " " << p.z() << " h " << m_pending[i].probe.height;
                }
            }
        m_pending.clear();
    }

    template<class Fn> bool any_near(const Probe &s, uint32_t limit, double reach, Fn &&fn, double below = -1.) const
    {
        const int x0 = int(std::floor((s.p.x() - reach) / CellXY)), x1 = int(std::floor((s.p.x() + reach) / CellXY));
        const int y0 = int(std::floor((s.p.y() - reach) / CellXY)), y1 = int(std::floor((s.p.y() + reach) / CellXY));
        const bool window = below > 0. && s.axis == Vec3d::UnitZ();
        const int z0 = int(std::floor((s.p.z() - (window ? below : reach)) / CellZ)),
                  z1 = int(std::floor((s.p.z() + (window ? -0.01 : reach)) / CellZ));
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (auto it = m_cells.find(key(x, y, z)); it != m_cells.end())
                        for (uint32_t id : it->second) {
                            if (id >= limit)
                                break;
                            if (fn(m_samples[id]))
                                return true;
                        }
        return false;
    }
    bool rests(const Probe &s, double h, double w, uint32_t limit) const
    {
        if (s.p.z() <= m_bed + 0.75 * m_layer)
            return true;
        constexpr double LateralSlack = 0.05, WidestBead = 1.5;
        const double deep = std::max(2. * std::max(h, m_layer) + 0.1, m_interface >= 0. ? 0.5 * h + m_interface : 0.), own = 0.5 * w + h;
        const double side = std::max(own, 0.5 * (w + WidestBead) + LateralSlack);
        return this->any_near(s, limit, std::sqrt(deep * deep + side * side), [&](const Sample &m) {
            const Vec3d  v     = m.p.cast<double>() - s.p;
            const double along = v.dot(s.axis);
            if (along > -0.01 || along < -deep || (v - along * s.axis).norm() > (m.touch ? std::max(own, 0.5 * (w + m.width) + LateralSlack) : own))
                return false;
            return m.run != s.run || s.along - m.along >= OwnRun;
        }, deep);
    }
    bool spans(const Probe &s, uint32_t limit) const
    {
        const double       deep = 2. * std::max(s.height, m_layer) + 0.1;
        std::vector<Vec3d> dirs;
        return this->any_near(s, limit, std::sqrt(deep * deep + m_spacing * m_spacing), [&](const Sample &m) {
            if (!m.sparse)
                return false;
            const Vec3d  v     = m.p.cast<double>() - s.p;
            const double along = v.dot(s.axis);
            const Vec3d  u     = v - along * s.axis;
            const double n     = u.norm();
            if (along > -0.01 || along < -deep || n > m_spacing || n <= 1e-6)
                return false;
            for (const Vec3d &d : dirs)
                if (u.dot(d) < 0.)
                    return true;
            if (dirs.size() < 300)
                dirs.push_back(u / n);
            return false;
        });
    }
    void add(const Sample &s)
    {
        m_cells[key(int(std::floor(s.p.x() / CellXY)), int(std::floor(s.p.y() / CellXY)), int(std::floor(s.p.z() / CellZ)))].push_back(
            uint32_t(m_samples.size()));
        m_samples.push_back(s);
    }

    double                                             m_bed, m_layer, m_spacing;
    bool                                               m_rotary;
    double                                             m_interface = -1.;
    uint32_t                                           m_runs = 0;
    std::vector<Sample>                                m_samples;
    std::vector<uint32_t>                              m_floating;
    std::vector<Pending>                               m_pending;
    std::unordered_map<int64_t, std::vector<uint32_t>> m_cells;
};

}

Floating floating_road(const PrintObject &object, const ConformalResult *result)
{
    const auto layers  = object.layers();
    const auto support = object.support_layers();
    if (layers.empty())
        return {};
    const Layer *first = layers.front();
    double       bed   = first->print_z - 0.5 * first->height;
    if (!support.empty())
        bed = std::min(bed, support.front()->print_z - 0.5 * support.front()->height);
    const PrintRegionConfig &rc      = object.printing_region(0).config();
    const double             nozzle  = object.print()->config().nozzle_diameter.get_at(0);
    const double             density = 0.01 * rc.sparse_infill_density.value;
    const double             spacing = density > 0. ? 2. * rc.sparse_infill_line_width.get_abs_value(nozzle) / density : 6.;
    Walker walker(bed, object.config().layer_height.value, spacing, make_tool_kinematics(object.print()->config())->rotary_count() > 0);
    struct Event { double z; int kind; size_t index; };
    std::vector<Event> events;
    for (size_t i = 0; i < support.size(); ++i)
        events.push_back({ support[i]->print_z, 0, i });
    for (size_t i = 0; i < layers.size(); ++i)
        events.push_back({ layers[i]->print_z, 1, i });
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.z < b.z || (a.z == b.z && a.kind < b.kind); });
    auto patches = [&](const std::vector<int> &ids, double z) {
        if (result == nullptr)
            return;
        for (int id : ids)
            if (id >= 0 && size_t(id) < result->patches.size())
                for (const ExtrusionEntityCollection &shell : result->patches[size_t(id)].shell_paths)
                    walker.entity(shell, z);
    };
    for (const Event &e : events) {
        if (e.kind == 0) {
            walker.entity(support[e.index]->support_fills, e.z);
            continue;
        }
        const Layer *layer = layers[e.index];
        patches(layer->conformal_patches_before, e.z);
        for (const LayerRegion *layerm : layer->regions()) {
            walker.entity(layerm->perimeters, e.z);
            walker.entity(layerm->fills, e.z);
        }
        patches(layer->conformal_patches_after, e.z);
        for (const LayerRegion *layerm : layer->regions())
            walker.raised(layerm->perimeters, e.z);
    }
    return walker.floating();
}

Floating floating_road(const GCodeProcessorResult &result, const PrintObject &object)
{
    using Move = GCodeProcessorResult::MoveVertex;
    const auto bead = [](const Move &m) {
        return m.type == EMoveType::Extrude && m.delta_extruder > 0.f && m.width > 0.f && m.height > 0.f && !m.internal_only &&
               m.extrusion_role != erSkirt && m.extrusion_role != erBrim && m.extrusion_role != erWipeTower &&
               m.extrusion_role != erCustom && m.extrusion_role != erIroning && m.extrusion_role != erNone;
    };
    double bed = std::numeric_limits<double>::max(), interface_h = 0.;
    for (const Move &m : result.moves)
        if (!m.timing_only && bead(m)) {
            bed = std::min(bed, double(m.position.z()) - 0.5 * m.height);
            if (m.extrusion_role == erSupportMaterialInterface)
                interface_h = std::max(interface_h, double(m.height));
        }
    if (bed == std::numeric_limits<double>::max())
        return {};
    const PrintRegionConfig &rc      = object.printing_region(0).config();
    const double             nozzle  = object.print()->config().nozzle_diameter.get_at(0);
    const double             density = 0.01 * rc.sparse_infill_density.value;
    const double             spacing = density > 0. ? 2. * rc.sparse_infill_line_width.get_abs_value(nozzle) / density : 6.;
    const double             gap     = object.config().support_top_z_distance.value;
    const auto kinematics = make_tool_kinematics(object.print()->config());
    const bool rotary     = kinematics->rotary_count() > 0 && result.has_joints();
    Walker walker(bed, object.config().layer_height.value, spacing, rotary, interface_h > 0. ? gap + 0.5 * interface_h + 0.1 : -1.);
    std::optional<Vec3d> from;
    uint32_t             run    = 0;
    bool                 in_run = false;
    double               along  = 0.;
    for (size_t i = 0; i < result.moves.size(); ++i) {
        const Move &m = result.moves[i];
        if (m.timing_only)
            continue;
        const Vec3d to = m.position.cast<double>();
        if (from && bead(m)) {
            if (!in_run) {
                run    = walker.new_run();
                along  = 0.;
                in_run = true;
            }
            Vec3d ax = Vec3d::UnitZ();
            if (rotary && i < result.joints.size())
                if (const Vec3d a = kinematics->forward(result.joints[i]).axis; a.squaredNorm() > 0.)
                    ax = a.normalized();
            walker.move(*from, to, m.width, m.height, m.extrusion_role, run, along, ax);
        } else
            in_run = false;
        from = to;
    }
    return walker.floating();
}

bool floats_past_bar(const Floating &print, const Floating &control)
{
    constexpr double Share = 1.05, Slack = 2.;
    constexpr double MaterialExcess = 10.;
    return print.part > Share * std::max(control.part, 0.) + Slack + MaterialExcess;
}

std::string may_need_support(const std::string &name, const Floating &planar)
{
    constexpr double MayNeedSupportAir = 20.;
    if (planar.part < 0. || planar.air <= MayNeedSupportAir)
        return {};
    return Slic3r::format(_u8L("%1% may need support: printed without it, as planar layers would print it, %2$.0f mm of its road "
                               "would be laid in the air."),
                          name, planar.air);
}

Floating planar_control_floating(const PrintObject &object, bool support)
{
    DynamicPrintConfig config = object.print()->full_print_config();
    config.apply(object.config(), true);
    config.apply(object.printing_region(0).config(), true);
    std::vector<std::pair<const char *, const char *>> control = { { "nonplanar_mode", "off" }, { "enable_support", "0" } };
    if (support)
        control = { { "nonplanar_mode", "off" },     { "enable_support", "1" },
                    { "support_type", "tree(auto)" }, { "support_style", "tree_slim" },
                    { "support_critical_regions_only", "0" } };
    Model        model;
    ModelObject *copy = model.add_object(*object.model_object());
    DynamicPrintConfig &copy_config = const_cast<DynamicPrintConfig &>(copy->config.get());
    for (const auto &[key, value] : control) {
        config.set_deserialize_strict(key, value);
        if (copy_config.has(key))
            copy_config.set_deserialize_strict(key, value);
    }
    copy_config.set_key_value("nonplanar_mode", new ConfigOptionEnum<NonplanarMode>(NonplanarMode::Off));
    if (oneshot_plinth_applied(object)) {
        config.set_deserialize_strict("raft_layers", "0");
        if (copy_config.has("raft_layers"))
            copy_config.set_deserialize_strict("raft_layers", "0");
    }
    copy->clear_instances();
    copy->add_instance(*object.model_object()->instances.front());
    constexpr size_t                                     CachedControls = 4;
    static std::mutex                                    cache_mutex;
    static std::deque<std::pair<std::string, Floating>> cache;
    std::string key;
    for (const DynamicPrintConfig *c : std::initializer_list<const DynamicPrintConfig *>{ &config, &copy->config.get() })
        for (const std::string &opt : c->keys())
            key += opt + '=' + c->opt_serialize(opt) + '\n';
    for (const ModelVolume *volume : copy->volumes) {
        const indexed_triangle_set &its = volume->mesh().its;
        key.append(reinterpret_cast<const char *>(its.vertices.data()), its.vertices.size() * sizeof(stl_vertex));
        key.append(reinterpret_cast<const char *>(its.indices.data()), its.indices.size() * sizeof(stl_triangle_vertex_indices));
        key.append(reinterpret_cast<const char *>(volume->get_matrix().data()), 16 * sizeof(double));
        key += std::to_string(int(volume->type()));
    }
    key.append(reinterpret_cast<const char *>(copy->instances.front()->get_matrix().data()), 16 * sizeof(double));
    {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        for (const auto &[cached, floating] : cache)
            if (cached == key)
                return floating;
    }
    if (measure_work_spent() || measure_deadline_passed())
        return { -1., -1. };
    Print print;
    try {
        const DeadlineWatch watch(print);
        print.apply(model, config);
        print.set_status_silent();
        print.process();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "Auto floating " << object.model_object()->name << ": the planar control failed: " << ex.what();
        return { -1., -1. };
    }
    if (print.objects().empty())
        return { -1., -1. };
    const Floating                    measured = floating_road(*print.objects().front(), nullptr);
    const std::lock_guard<std::mutex> lock(cache_mutex);
    cache.emplace_back(std::move(key), measured);
    if (cache.size() > CachedControls)
        cache.pop_front();
    return measured;
}

}
