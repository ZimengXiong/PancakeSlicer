#include "libslic3r/Nonplanar/CurvedTopsScaffold.hpp"
#include "ClearanceRepair.hpp"
#include "libslic3r/Nonplanar/ConformalLayers.hpp"
#include "libslic3r/Nonplanar/StageCache.hpp"
#include "Point.hpp"
#include <cstddef>
#include <memory>

#include "ClearanceVerifier.hpp"
#include "GCodeProcessor.hpp"
#include "ToolKinematics.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/HeapTrim.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Nonplanar/AutoMode.hpp"
#include "libslic3r/Nonplanar/ConformalShells.hpp"
#include "libslic3r/Nonplanar/CurvedTopsSurface.hpp"
#include "libslic3r/Nonplanar/HeadBoxLabel.hpp"
#include "libslic3r/Nonplanar/HeadEnvelope.hpp"
#include "libslic3r/Nonplanar/PatchTools.hpp"
#include "libslic3r/Nonplanar/MultiplanarConfirm.hpp"
#include "libslic3r/Nonplanar/OneShotField.hpp"
#include "libslic3r/Nonplanar/Rests.hpp"
#include "libslic3r/Nonplanar/Wave.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {

using MoveVertex = GCodeProcessorResult::MoveVertex;

constexpr double Inf = std::numeric_limits<double>::infinity();
constexpr double UniformRadius = 3.;
constexpr double UprightRadius = 1.5;
constexpr double SkinUprightRadius = 0.6;

bool adaptive_top(const PrintObject &object, int patch)
{
    if (patch < 0)
        return false;
    const ConformalPatch &p = object.conformal().patches[size_t(patch)];
    return p.kind == PatchKind::Top && p.region_id >= 0 &&
           object.printing_region(size_t(p.region_id)).config().nonplanar_adaptive_bands.value;
}
constexpr double Extra = 0.05;
constexpr double CellSlack = 0.25;

bool physical(const MoveVertex &m)
{
    return !m.timing_only && m.type != EMoveType::Seam && m.type != EMoveType::Color_change && m.type != EMoveType::Tool_change &&
           m.type != EMoveType::Pause_Print && m.type != EMoveType::Custom_GCode;
}

Vec3d tip_of(const GCodeProcessorResult &result, size_t i)
{
    return result.has_joints() ? result.joints[i].xyz : Vec3d(result.moves[i].position.cast<double>());
}

struct Box { std::string name; Vec3d min, max; bool inf_x, inf_y, nozzle; };
std::vector<Box> verifier_boxes(const HeadEnvelope &head, bool allowance)
{
    std::vector<Box> out;
    for (const HeadBox &b : head.boxes) {
        const Vec2d radius = b.min.head<2>().cwiseAbs().cwiseMax(b.max.head<2>().cwiseAbs());
        const bool  nozzle = b.nozzle || (!b.infinite_x && !b.infinite_y && b.min.z() == 0. && b.min.head<2>().maxCoeff() <= 0. &&
                                         b.max.head<2>().minCoeff() >= 0. && radius.norm() <= head.tip_allowance.radius);
        Box box { b.name, b.min, b.max, b.infinite_x, b.infinite_y, nozzle };
        if (!nozzle) {
            box.min.head<2>().array() -= head.lateral_margin;
            box.max.head<2>().array() += head.lateral_margin;
            box.min.z() -= head.margin;
        } else if (allowance && !b.infinite_x && !b.infinite_y && radius.norm() <= head.tip_allowance.radius)
            box.min.z() = std::max({ box.min.z(), head.tip_allowance.height, 0.05 });
        if (box.min.z() <= box.max.z())
            out.emplace_back(std::move(box));
    }
    return out;
}

struct Segment { size_t move; size_t run; Vec3d a, b; double half_width; };
class SegmentGrid
{
public:
    SegmentGrid(const GCodeProcessorResult &result, const std::vector<BoundingBoxf> *reach, const std::vector<size_t> &blamed)
    {
        std::unordered_set<int64_t> cells;
        if (reach != nullptr)
            for (const BoundingBoxf &r : *reach)
                visit(r, [&](int64_t key) { cells.insert(key); });
        std::unordered_set<size_t> blamed_moves(blamed.begin(), blamed.end());
        size_t last = 0, run = 0;
        for (size_t i = 1; i < result.moves.size(); ++i) {
            const MoveVertex &m = result.moves[i];
            if (!physical(m))
                continue;
            const Vec3d a = tip_of(result, last), b = tip_of(result, i);
            last = i;
            if (m.type != EMoveType::Extrude) {
                ++run;
                continue;
            }
            if (blamed_moves.count(i))
                m_run_of[i] = run;
            const Segment segment { i, run, a, b, 0.5 * m.width };
            bool in_reach = reach == nullptr;
            if (!in_reach)
                visit(bounds(segment), [&](int64_t key) { in_reach = in_reach || cells.count(key) > 0; });
            if (!in_reach)
                continue;
            const size_t id = m_segments.size();
            m_segments.push_back(segment);
            visit(bounds(segment), [&](int64_t key) { m_cells[key].push_back(id); });
        }
    }
    size_t run_of(size_t move) const { auto it = m_run_of.find(move); return it == m_run_of.end() ? size_t(-1) : it->second; }
    const std::vector<Segment> &segments() const { return m_segments; }
    template<class Fn> void query(const BoundingBoxf &rect, Fn fn) const
    {
        if (!rect.min.allFinite() || !rect.max.allFinite()) {
            for (const Segment &s : m_segments) fn(s);
            return;
        }
        std::vector<size_t> ids;
        visit(rect, [&](int64_t key) {
            if (auto it = m_cells.find(key); it != m_cells.end())
                ids.insert(ids.end(), it->second.begin(), it->second.end());
        });
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (size_t id : ids)
            if (bounds(m_segments[id]).overlap(rect))
                fn(m_segments[id]);
    }

private:
    static BoundingBoxf bounds(const Segment &s)
    {
        const Vec2d r = Vec2d::Constant(s.half_width);
        return BoundingBoxf(s.a.head<2>().cwiseMin(s.b.head<2>()) - r, s.a.head<2>().cwiseMax(s.b.head<2>()) + r);
    }
    template<class Fn> static void visit(const BoundingBoxf &r, Fn fn)
    {
        const int x0 = int(std::floor(r.min.x() / 2.)), x1 = int(std::floor(r.max.x() / 2.));
        const int y0 = int(std::floor(r.min.y() / 2.)), y1 = int(std::floor(r.max.y() / 2.));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) fn((int64_t(x) << 32) ^ int64_t(uint32_t(y)));
    }
    std::vector<Segment>                              m_segments;
    std::unordered_map<int64_t, std::vector<size_t>> m_cells;
    std::unordered_map<size_t, size_t>               m_run_of;
};

struct Blame
{
    const Segment *culprit = nullptr;
    double         depth   = -Inf;
    double         floor   = 0.;
    double         box_lift = 0.;
    BoundingBoxf   rect;
    Vec3d          tip { Vec3d::Zero() };
};

Blame blame(const GCodeProcessorResult &result, const SegmentGrid &grid, size_t move_id, size_t prev_id,
            const std::vector<Box> &boxes, const std::string &component)
{
    Blame best;
    const MoveVertex &m  = result.moves[move_id];
    const bool extruding = m.type == EMoveType::Extrude;
    const size_t run     = grid.run_of(move_id);
    const Vec3d a = tip_of(result, prev_id), b = tip_of(result, move_id);
    const int count = std::max(1, int(std::ceil((b - a).norm() / 0.25)));
    for (int j = 0; j < count; ++j) {
        const Vec3d p0 = a + double(j) / count * (b - a), p1 = a + double(j + 1) / count * (b - a);
        for (const Box &box : boxes) {
            if (!component.empty() && box.name != component)
                continue;
            BoundingBoxf rect(p0.head<2>().cwiseMin(p1.head<2>()) + box.min.head<2>() - Vec2d::Constant(CellSlack),
                              p0.head<2>().cwiseMax(p1.head<2>()) + box.max.head<2>() + Vec2d::Constant(CellSlack));
            if (box.inf_x) rect.min.x() = -Inf, rect.max.x() = Inf;
            if (box.inf_y) rect.min.y() = -Inf, rect.max.y() = Inf;
            const double floor = std::min(p0.z(), p1.z()) + box.min.z();
            grid.query(rect, [&](const Segment &s) {
                if (s.move >= move_id || (box.nozzle && extruding && s.run == run))
                    return;
                const double depth = std::max(s.a.z(), s.b.z()) - floor;
                if (depth > best.depth || (depth == best.depth && best.culprit && s.move > best.culprit->move))
                    best = Blame { &s, depth, floor, box.min.z(), rect, p1 };
            });
        }
    }
    return best;
}

struct Site
{
    PrintObject *object   = nullptr;
    int          patch    = -1;
    int          shell    = -1;
    Vec2d        xy { Vec2d::Zero() };
};

class Locator
{
public:
    explicit Locator(Print &print) : m_print(print)
    {
        m_offset = Vec2d(print.get_plate_origin().head<2>()) + print.get_imex_slice_offset();
        m_z_offset = print.config().z_offset.value;
    }
    Vec2d to_object(const Vec3d &tip, const PrintObject &object, size_t instance, unsigned extruder) const
    {
        const Vec2d extruder_offset = extruder < m_print.config().extruder_offset.values.size() ?
                                          m_print.config().extruder_offset.values[extruder] : Vec2d::Zero();
        return tip.head<2>() + m_offset + extruder_offset - unscale(object.instances()[instance].shift);
    }
    double to_object_z(double z) const { return z - m_z_offset; }
    Vec3d from_preview(const MoveVertex &m) const
    {
        const Vec2d extruder_offset = m.extruder_id < m_print.config().extruder_offset.values.size() ?
                                          m_print.config().extruder_offset.values[m.extruder_id] : Vec2d::Zero();
        const Vec2d xy = m.position.head<2>().cast<double>() - Vec2d(m_print.get_plate_origin().head<2>()) - extruder_offset;
        return Vec3d(xy.x(), xy.y(), double(m.position.z()) + m_z_offset);
    }
    double to_gcode_z(double z) const { return z + m_z_offset; }
    PrintObject *object_at(const Vec3d &tip, unsigned extruder) const
    {
        for (PrintObject *object : m_print.objects())
            if (!object->conformal().patches.empty())
                for (size_t instance = 0; instance < object->instances().size(); ++instance)
                    if (unscaled(object->bounding_box()).contains(to_object(tip, *object, instance, extruder)))
                        return object;
        return nullptr;
    }
    Site locate(const GCodeProcessorResult &result, size_t move_id, const Vec3d &tip) const
    {
        Site best;
        const MoveVertex &m = result.moves[move_id];
        const int shell = result.state(m).nonplanar_patch_shell;
        if (shell == 0xFF)
            return best;
        double best_dz = 0.5;
        const double z = to_object_z(tip.z());
        for (PrintObject *object : m_print.objects())
            for (size_t instance = 0; instance < object->instances().size(); ++instance) {
                const Vec2d q = to_object(tip, *object, instance, m.extruder_id);
                for (const ConformalPatch &patch : object->conformal().patches) {
                    if (patch.kind == PatchKind::Skin) {
                        if (best.object == nullptr && unscaled(object->bounding_box()).contains(q))
                            best = Site { object, patch.id, shell, q };
                        continue;
                    }
                    const float s = patch.surface.sample(q);
                    if (std::isnan(s) || shell > patch.shells)
                        continue;
                    const double expected = patch.kind == PatchKind::Top ? s - std::max(0, patch.shells - 1 - shell) * patch.shell_height :
                                                                           s + (shell + 1) * patch.shell_height;
                    if (const double dz = std::abs(z - expected); dz < best_dz)
                        best_dz = dz, best = Site { object, patch.id, shell, q };
                }
            }
        return best;
    }

private:
    Print &m_print;
    Vec2d  m_offset { Vec2d::Zero() };
    double m_z_offset = 0.;
};

}

ClearanceReport ClearanceRepair::verify(const Print &print, const GCodeProcessorResult &result, const ClearanceParams &params,
                                        const ToolKinematics &kinematics)
{
    const auto cancel = [&print] { print.throw_if_canceled(); };
    if (print.m_clearance_repair == nullptr)
        return verify_clearance(result, params, kinematics, cancel);
    return print.m_clearance_repair->m_memo->verify(result, params, kinematics, cancel);
}

ClearanceRepair::ClearanceRepair(Print &print) : m_print(print), m_memo(std::make_unique<ClearanceMemo>())
{
    m_max_passes = print.config().nonplanar_repair_iterations.value;
    m_guard_warnings = stage_cache_guard_warnings(print);
    print.m_clearance_repair = this;
}

ClearanceRepair::~ClearanceRepair() { m_print.m_clearance_repair = nullptr; }

void ClearanceRepair::check_export(Print &print, GCodeProcessorResult &result)
{
    trim_free_memory();
    if (wave_enabled(print)) {
        ClearanceParams params;
        params.head = resolve_head_envelope(print.config());
        params.contact_bar = true;
        params.cone_tolerance = 0.05;
        result.clearance_report = verify(print, result, params, *make_tool_kinematics(print.config()));
        result.clearance = result.clearance_report.per_move;
        if (result.clearance_report.hit_moves > 0) {
            const std::string message = Slic3r::format(_u8L("Non-planar waves: %1% moves hit printed material."),
                                                      result.clearance_report.hit_moves);
            print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, message);
            if (print.config().nonplanar_collision_check.value == NonplanarCollisionCheck::Block)
                throw Slic3r::RuntimeError(message);
        }
        return;
    }
    if (rotary_auto_measurement() || rotary_measurement_unverified() ||
        rotary_measurement_too_slow(result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time))
        return;
    ClearanceRepair *self = print.m_clearance_repair;
    const auto       mode = print.config().nonplanar_collision_check.value;
    if (self != nullptr && self->curved_tops_step(result))
        return;
    if (self != nullptr) {
        for (const std::string &warning : self->m_guard_warnings)
            print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, warning);
        stage_cache_note_guard_warnings(print, self->m_guard_warnings);
    }
    for (const PrintObject *object : print.objects())
        if (!object->auto_kept_warning().empty() && !object->m_auto_planar)
            print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
                                          Slic3r::format(_u8L("%1%: printed curved; %2%."), object->model_object()->name, object->auto_kept_warning()));
    for (const PrintObject *object : print.objects())
        if (object->m_auto_planar && !object->auto_plain_reason().empty()) {
            const auto *set  = object->model_object()->config.get().option<ConfigOptionEnum<NonplanarMode>>("nonplanar_mode");
            const bool  shot = (set ? set->value : print.default_object_config().nonplanar_mode.value) == NonplanarMode::OneShot;
            print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
                                          Slic3r::format(shot ? _u8L("%1%: Switched from One-shot to flat layers (%2%)") : _u8L("%1%: Switched from Auto to flat layers (%2%)"),
                                                         object->model_object()->name, object->auto_plain_reason()));
        }
    if (print.config().nonplanar_kinematics.value != RotaryLayout::None) {
        if (self != nullptr && self->oneshot_step(result)) {
            if (self->m_repick.empty())
                self->contact_guard(result);
            return;
        }
        if (self != nullptr && result.nonplanar && effective_collision_check(print) != NonplanarCollisionCheck::Off &&
            self->m_pass < self->m_max_passes) {
            ClearanceParams params;
            params.head = resolve_head_envelope(print.config());
            const auto      started = std::chrono::steady_clock::now();
            ClearanceReport report  = verify(print, result, params, *make_tool_kinematics(print.config()));
            const auto      checked = std::chrono::steady_clock::now();
            if (self->m_pass == 0)
                self->m_first_hits = report.hit_moves;
            self->m_planned.clear();
            const bool repaired = report.hit_moves > 0 && self->plan_upright(result, report);
            BOOST_LOG_TRIVIAL(warning) << "Rotary clearance pass " << self->m_pass << ": " << result.moves.size() << " moves, "
                                    << report.hit_moves << " hits, verify " << std::chrono::duration<double>(checked - started).count()
                                    << " s, plan " << std::chrono::duration<double>(std::chrono::steady_clock::now() - checked).count() << " s";
            if (report.hit_moves == 0 || repaired) {
                if (report.hit_moves == 0 && self->m_pass > 0)
                    print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, self->summary(0));
                result.clearance_report = std::move(report);
                result.clearance        = result.clearance_report.per_move;
                if (!repaired)
                    self->contact_guard(result);
                return;
            }
        }
        if (self != nullptr)
            self->m_planned.clear();
        check_export_clearance(print, result);
        if (self != nullptr)
            self->contact_guard(result);
        return;
    }
    if (self != nullptr && self->oneshot_step(result))
        return;
    if (self == nullptr || !result.nonplanar || mode == NonplanarCollisionCheck::Off) {
        check_export_clearance(print, result);
        return;
    }
    const bool auto_only = std::any_of(print.objects().begin(), print.objects().end(),
                                       [](const PrintObject *o) { return o->config().nonplanar_mode.value == NonplanarMode::Auto; }) &&
                           std::all_of(print.objects().begin(), print.objects().end(), [](const PrintObject *o) {
                               return o->config().nonplanar_mode.value == NonplanarMode::Auto || o->config().nonplanar_mode.value == NonplanarMode::Off;
                           });
    ClearanceParams params;
    params.head        = resolve_head_envelope(print.config());
    params.contact_bar = auto_only;
    if (std::any_of(print.objects().begin(), print.objects().end(),
                    [](const PrintObject *o) { return o->config().nonplanar_mode.value == NonplanarMode::SmoothMorph; }))
        params.cone_tolerance = curved_tops_cone_tolerance(print);
    if (const char *tol = std::getenv("NP_CONTACT_CONE_TOL"); tol != nullptr)
        params.cone_tolerance = std::max(0., std::atof(tol));
    ClearanceReport report = verify(print, result, params, ThreeAxisKinematics());
    const size_t hits = report.hit_moves;
    self->m_planned.clear();
    const bool stalled = self->m_pass > 0 && hits == self->m_last_hits && report.min_clearance == self->m_last_min;
    std::string stop;
    if (params.contact_bar && hits > 0) {
        self->m_stale_passes = self->m_pass > 0 && hits >= self->m_best_hits ? self->m_stale_passes + 1 : 0;
        if (self->m_pass == 0 || hits < self->m_best_hits)
            self->m_best_hits = hits;
        if (self->m_stale_passes >= 2)
            stop = Slic3r::format("passes %1% and %2% struck no fewer moves than the best before them (%3%)", self->m_pass, self->m_pass + 1,
                                  self->m_best_hits);
        for (const PrintObject *object : print.objects()) {
            if (object->config().nonplanar_mode.value != NonplanarMode::Auto || object->m_auto_curved_floating < 0.)
                continue;
            if (self->m_pass == 0)
                self->m_first_floating[object] = object->m_auto_curved_floating;
            else if (const auto first = self->m_first_floating.find(object); first != self->m_first_floating.end() && stop.empty() &&
                     floats_past_bar({ object->m_auto_curved_floating }, { first->second }))
                stop = Slic3r::format("pass %1% floats %2% %3$.0f mm of part road, past its first pass's %4$.0f mm", self->m_pass + 1,
                                      object->model_object()->name, object->m_auto_curved_floating, first->second);
        }
        if (!stop.empty())
            BOOST_LOG_TRIVIAL(warning) << "Non-planar repair stopped: " << stop;
    }
    self->m_last_hits  = hits;
    self->m_last_min   = report.min_clearance;
    constexpr int BarRepairPasses = 5;
    int           max_passes      = params.contact_bar && self->m_max_passes > 0 ? std::max(self->m_max_passes, BarRepairPasses) : self->m_max_passes;
    if (const char *passes = std::getenv("NP_BAR_REPAIR_PASSES"); passes != nullptr && params.contact_bar)
        max_passes = std::max(0, std::atoi(passes));
    if (hits > 0 && !stalled && stop.empty() && self->plan(result, report, params.head, params.contact_bar) && self->m_pass < max_passes) {
        result.clearance_report = std::move(report);
        result.clearance        = result.clearance_report.per_move;
        return;
    }
    self->m_planned.clear();
    size_t conformal_hits = 0;
    for (size_t i = 0; i < result.moves.size(); ++i)
        if (report.per_move[i] < 0.f && result.state(result.moves[i]).nonplanar_patch_shell != 0xFF && result.moves[i].type == EMoveType::Extrude)
            ++conformal_hits;
    result.clearance_report = std::move(report);
    result.clearance        = result.clearance_report.per_move;
    mark_clearance_override(print, result);
    if (self->m_pass == 0 && hits == 0)
        return;
    if (params.contact_bar && hits > 0 && self->contact_guard(result, hits))
        return;
    std::string message = self->summary(hits);
    if (conformal_hits > 0) {
        const auto worst = std::find_if(result.clearance_report.worst.begin(), result.clearance_report.worst.end(), [&result](const ClearanceHit &hit) {
            return result.state(result.moves[hit.move_id]).nonplanar_patch_shell != 0xFF && result.moves[hit.move_id].type == EMoveType::Extrude;
        });
        message += " " + (worst == result.clearance_report.worst.end() ?
                              Slic3r::format(_u8L("Curved moves that still hit printed material: %1%."), conformal_hits) :
                              Slic3r::format(_u8L("Curved moves that still hit printed material: %1% (worst %2$.3f mm, %3%, G-code line %4%)."),
                                             conformal_hits, worst->penetration, head_box_label(worst->component), worst->gcode_line));
    }
    print.active_step_add_warning(conformal_hits > 0 ? PrintStateBase::WarningLevel::CRITICAL : PrintStateBase::WarningLevel::NON_CRITICAL, message);
    if (mode == NonplanarCollisionCheck::Block && conformal_hits > 0)
        throw RuntimeError(message + " " + _u8L("Export blocked by the head clearance check. Review the path or set the check to Warn to export it."));
}

bool ClearanceRepair::plan(const GCodeProcessorResult &result, const ClearanceReport &report, const HeadEnvelope &head, bool bar)
{
    const std::vector<Box> contact_boxes = verifier_boxes(head, !bar), travel_boxes = verifier_boxes(head, false);
    auto component_of = [](const ClearanceHit &hit) {
        const std::string &c = hit.component;
        return c.size() > 5 && c.compare(c.size() - 5, 5, "_fine") == 0 ? c.substr(0, c.size() - 5) : c;
    };
    auto previous_physical = [&](size_t i) { while (i > 0 && !physical(result.moves[--i])) {} return i; };
    auto next_extrusion    = [&](size_t i) {
        while (++i < result.moves.size())
            if (!result.moves[i].timing_only && result.moves[i].type == EMoveType::Extrude) return i;
        return size_t(0);
    };
    auto previous_extrusion = [&](size_t i) {
        while (i > 0)
            if (const auto &move = result.moves[--i]; !move.timing_only && move.type == EMoveType::Extrude) return i;
        return size_t(0);
    };
    std::vector<BoundingBoxf> reach;
    std::vector<size_t>       blamed;
    bool                      unbounded = false;
    for (const ClearanceHit &hit : report.worst)
        for (const size_t id : { hit.move_id, next_extrusion(hit.move_id) }) {
            if (id == 0)
                continue;
            blamed.push_back(id);
            const Vec3d  a = tip_of(result, previous_physical(id)), b = tip_of(result, id);
            BoundingBoxf r(a.head<2>().cwiseMin(b.head<2>()), a.head<2>().cwiseMax(b.head<2>()));
            Vec2d        lo = Vec2d::Zero(), hi = Vec2d::Zero();
            for (const std::vector<Box> *list : { &contact_boxes, &travel_boxes })
                for (const Box &box : *list)
                    if (hit.component.empty() || box.name == component_of(hit)) {
                        unbounded = unbounded || box.inf_x || box.inf_y;
                        lo = lo.cwiseMin(box.min.head<2>());
                        hi = hi.cwiseMax(box.max.head<2>());
                    }
            r.min += lo - Vec2d::Constant(CellSlack);
            r.max += hi + Vec2d::Constant(CellSlack);
            reach.push_back(r);
        }
    const SegmentGrid      grid(result, unbounded ? nullptr : &reach, blamed);
    if (m_pass == 0)
        m_first_hits = report.hit_moves;
    const Locator          locator(m_print);
    auto hints_of = [this](PrintObject *object) -> RepairHints & {
        for (Planned &p : m_planned)
            if (p.object == object)
                return p.hints;
        m_planned.push_back({ object, {} });
        return m_planned.back().hints;
    };

    size_t caps = 0, bands = 0, drops = 0, lifts = 0, planars = 0, unrepaired = 0;
    constexpr double PlanarRadius = 2.5;
    auto droppable = [](PatchKind kind) { return kind == PatchKind::Top || kind == PatchKind::Overhang || kind == PatchKind::Bottom; };
    auto disc_radius = [&head](double depth) {
        double r = 0.;
        for (const HeadBox &box : head.boxes)
            if (box.nozzle && !box.infinite_x && !box.infinite_y && box.min.z() <= depth + EPSILON)
                r = std::max({ r, -box.min.x(), box.max.x(), -box.min.y(), box.max.y() });
        return PlanarRadius + r;
    };
    std::map<std::string, size_t> unrepaired_by_cause;
    if (bar) {
        std::map<const PrintObject *, std::set<std::pair<int, int>>> placed;
        const double cell = 0.5 * PlanarRadius;
        auto planar_site = [&](size_t i) {
            const Vec3d tip = tip_of(result, i);
            for (PrintObject *object : m_print.objects())
                if (std::any_of(object->conformal().patches.begin(), object->conformal().patches.end(),
                                [&droppable](const ConformalPatch &patch) { return droppable(patch.kind); }))
                    for (size_t instance = 0; instance < object->instances().size(); ++instance)
                        if (const Vec2d q = locator.to_object(tip, *object, instance, result.moves[i].extruder_id);
                            unscaled(object->bounding_box()).contains(q))
                            return Site { object, -1, -1, q };
            return Site {};
        };
        for (size_t i = 0; i < report.per_move.size() && i < result.moves.size(); ++i) {
            if (!(report.per_move[i] < 0.f) || result.moves[i].type != EMoveType::Extrude)
                continue;
            const bool curved = result.state(result.moves[i]).nonplanar_patch_shell != 0xFF;
            const Site site   = curved ? locator.locate(result, i, tip_of(result, i)) : planar_site(i);
            if (site.object == nullptr || (curved && !droppable(site.object->conformal().patches[size_t(site.patch)].kind)))
                continue;
            if (!placed[site.object].emplace(int(std::floor(site.xy.x() / cell)), int(std::floor(site.xy.y() / cell))).second)
                continue;
            hints_of(site.object).planar.push_back({ site.xy, disc_radius(-double(report.per_move[i])) });
            ++planars;
        }
    }
    for (const ClearanceHit &hit : report.worst) {
        size_t move_id = hit.move_id;
        const MoveVertex *m = &result.moves[move_id];
        const bool extruding = m->type == EMoveType::Extrude;
        const size_t prev = previous_physical(move_id);
        const Vec3d  a = tip_of(result, prev), b = tip_of(result, move_id);
        const double penetration = hit.penetration;

        if (!extruding) {
            const size_t next = next_extrusion(move_id);
            const bool vertical = (b.head<2>() - a.head<2>()).norm() < 1e-3;
            if (vertical && next != 0 && result.state(result.moves[next]).nonplanar_patch_shell != 0xFF) {
                move_id = next;
                m = &result.moves[move_id];
            } else {
                const size_t last = previous_extrusion(move_id);
                Site site = locator.locate(result, last, tip_of(result, last));
                if (site.object == nullptr && next != 0)
                    site = locator.locate(result, next, tip_of(result, next));
                if (site.object == nullptr)
                    site.object = locator.object_at(b, m->extruder_id);
                if (site.object != nullptr) {
                    RepairHints &h = hints_of(site.object);
                    if (adaptive_top(*site.object, site.patch))
                        h.uniform_beads.push_back({ site.xy, UniformRadius });
                    const double base = site.object->nonplanar_repair() ? site.object->nonplanar_repair()->travel_lift : 0.;
                    h.travel_lift = std::max(h.travel_lift, base + penetration + Extra);
                    const auto &layers = site.object->layers();
                    const double z     = locator.to_object_z(b.z());
                    const auto   it    = std::lower_bound(layers.begin(), layers.end(), z - EPSILON,
                                                          [](const Layer *layer, double v) { return layer->print_z < v; });
                    if (it != layers.end())
                        h.travel_layers.push_back((*it)->id());
                    ++lifts;
                    static const bool travel_disc = std::getenv("NP_CT1_TRAVEL_DISC") != nullptr;
                    if (travel_disc && bar && site.patch >= 0 &&
                        size_t(site.patch) < site.object->conformal().patches.size() &&
                        droppable(site.object->conformal().patches[size_t(site.patch)].kind)) {
                        h.planar.push_back({ site.xy, disc_radius(penetration) });
                        ++planars;
                    }
                } else {
                    ++unrepaired, ++unrepaired_by_cause["planar travel"];
                }
                continue;
            }
        }

        const Site site = locator.locate(result, move_id, tip_of(result, move_id));
        if (site.object != nullptr) {
            const ConformalPatch &patch = site.object->conformal().patches[size_t(site.patch)];
            if (site.shell == patch.shells) {
                hints_of(site.object).ironing_drops.push_back({ site.xy, std::max(1., 2. * double(m->width)) });
                ++drops;
                continue;
            }
        }

        if (bar && extruding && site.object != nullptr && droppable(site.object->conformal().patches[size_t(site.patch)].kind)) {
            hints_of(site.object).planar.push_back({ site.xy, disc_radius(penetration) });
            ++planars;
            continue;
        }
        const Blame what = blame(result, grid, move_id, previous_physical(move_id), m->type == EMoveType::Extrude || site.object ? contact_boxes : travel_boxes,
                                 component_of(hit));
        if (what.culprit == nullptr) {
            ++unrepaired, ++unrepaired_by_cause["no material found"];
            continue;
        }
        const Site        source  = locator.locate(result, what.culprit->move, what.culprit->b);
        if (bar && source.object != nullptr && droppable(source.object->conformal().patches[size_t(source.patch)].kind)) {
            hints_of(source.object).planar.push_back({ source.xy, disc_radius(penetration) });
            ++planars;
            continue;
        }
        if (source.object != nullptr && source.object->conformal().patches[size_t(source.patch)].kind == PatchKind::Top) {
            const ConformalPatch &patch = source.object->conformal().patches[size_t(source.patch)];
            const double drop = std::max(0, patch.shells - 1 - std::min(source.shell, patch.shells - 1)) * patch.shell_height;
            const double cap  = locator.to_object_z(what.floor - 2. * Extra) + drop;
            RepairHints &h    = hints_of(source.object);
            if (adaptive_top(*source.object, source.patch))
                h.uniform_beads.push_back({ source.xy, UniformRadius });
            if (site.object == source.object && adaptive_top(*site.object, site.patch))
                h.uniform_beads.push_back({ site.xy, UniformRadius });
            const Vec3d sa = what.culprit->a, sb = what.culprit->b;
            const int   n  = std::max(1, int(std::ceil((sb - sa).head<2>().norm() / 0.2)));
            const double radius = what.culprit->half_width + patch.surface.pitch();
            Vec3d nearest = sb;
            bool  inside  = false;
            for (int k = 0; k <= n; ++k) {
                const Vec3d p = sa + double(k) / n * (sb - sa);
                if (what.rect.contains(Vec2d(p.head<2>())))
                    h.surface_caps.push_back({ source.xy + (p - sb).head<2>(), radius, cap }), inside = true;
                else if ((p.head<2>() - what.rect.center()).norm() < (nearest.head<2>() - what.rect.center()).norm())
                    nearest = p;
            }
            if (!inside)
                h.surface_caps.push_back({ source.xy + (nearest - sb).head<2>(), radius, cap });
            ++caps;
        } else if (site.object != nullptr && site.object->conformal().patches[size_t(site.patch)].kind == PatchKind::Top) {
            double layer_height = 0.;
            for (const Layer *layer : site.object->layers())
                layer_height = std::max(layer_height, layer->height);
            const double height = what.box_lift - Extra - layer_height;
            if (height < layer_height) {
                ++unrepaired, ++unrepaired_by_cause["material beside the nozzle"];
                continue;
            }
            hints_of(site.object).order_bands.push_back({ site.xy, height });
            if (adaptive_top(*site.object, site.patch))
                hints_of(site.object).uniform_beads.push_back({ site.xy, UniformRadius });
            ++bands;
        } else {
            ++unrepaired, ++unrepaired_by_cause[extruding ? "planar extrusion" : "travel"];
        }
    }

    std::ostringstream action;
    action << "pass " << m_pass + 1 << ": " << report.hit_moves << " hits";
    if (caps) action << ", " << caps << " lowered";
    if (bands) action << ", " << bands << " re-banded";
    if (lifts) action << ", " << lifts << " travel lifted";
    if (drops) action << ", " << drops << " ironing dropped";
    if (planars) action << ", " << planars << " printed planar";
    for (const auto &[cause, n] : unrepaired_by_cause) action << ", " << n << " unrepaired (" << cause << ")";
    if (report.machine_hit_moves) action << ", " << report.machine_hit_moves << " in start/end G-code (not repairable)";
    m_actions.emplace_back(action.str());
    BOOST_LOG_TRIVIAL(info) << "Non-planar repair " << m_actions.back();
    return caps + bands + lifts + drops + planars > 0;
}

bool ClearanceRepair::plan_upright(const GCodeProcessorResult &result, const ClearanceReport &report)
{
    const Locator locator(m_print);
    const double  radius = UprightRadius * double(m_pass + 1);
    size_t        discs  = 0;
    for (size_t i = 1; i < result.moves.size() && i < report.per_move.size(); ++i) {
        const MoveVertex &m = result.moves[i];
        if (report.per_move[i] >= 0.f || result.state(m).nonplanar_patch_shell == 0xFF)
            continue;
        const Vec3d tip  = locator.from_preview(m);
        const Site  site = locator.locate(result, i, tip);
        if (site.object == nullptr)
            continue;
        std::vector<RepairHints::Disc> *upright = nullptr;
        for (Planned &p : m_planned)
            if (p.object == site.object)
                upright = &p.hints.upright;
        if (upright == nullptr) {
            m_planned.push_back({ site.object, {} });
            upright = &m_planned.back().hints.upright;
        }
        size_t previous = i;
        while (previous > 0 && !physical(result.moves[--previous])) {}
        const Vec2d from = site.xy + (result.moves[previous].position - m.position).head<2>().cast<double>();
        const bool   skin = site.patch >= 0 && site.object->conformal().patches[size_t(site.patch)].kind == PatchKind::Skin;
        const double nan  = std::numeric_limits<double>::quiet_NaN();
        const double z_at = skin ? locator.to_object_z(tip.z()) : nan;
        const double z_from = skin ? locator.to_object_z(double(result.moves[previous].position.z())) : nan;
        const double r = skin ? SkinUprightRadius : radius;
        for (const auto &[p, z] : { std::make_pair(from, z_from), std::make_pair(site.xy, z_at) })
            if (std::none_of(upright->begin(), upright->end(), [&p = p, z = z, r](const RepairHints::Disc &d) {
                    return (d.p - p).norm() < 0.5 * r && (std::isnan(z) == std::isnan(d.z)) && (std::isnan(z) || std::abs(d.z - z) < 0.5 * r);
                }))
                upright->push_back({ p, r, z }), ++discs;
    }
    if (discs > 0)
        m_actions.push_back(std::to_string(discs) + " spots printed upright");
    return discs > 0;
}

bool ClearanceRepair::oneshot_step(GCodeProcessorResult &result)
{
    for (const PrintObject *object : m_print.m_objects)
        if (const OneShotState *s = object->oneshot_state(); s != nullptr)
            if (const std::string why = may_need_support(object->model_object()->name, Floating{ s->planar_air < 0. ? -1. : 0., 0., s->planar_air });
                !why.empty())
                m_print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, why);
    std::vector<PrintObject *> objects;
    for (PrintObject *object : m_print.m_objects)
        if (oneshot_requested(*object))
            objects.push_back(object);
    if (objects.empty())
        return false;
    const double    time = result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    ClearanceParams params;
    params.head            = resolve_head_envelope(m_print.config());
    params.contact_bar     = true;
    ClearanceReport report = verify(m_print, result, params, *make_tool_kinematics(m_print.config()));
    const size_t    hits   = report.hit_moves;
    for (PrintObject *object : objects) {
        OneShotState &s = object->oneshot_state();
        s.decision      = Slic3r::format("one-shot field: %1% clearance hits, time ratio %2$.3f (model) %6% cap %3$.3f, slicer %4$.0f s%5%", hits,
                                         s.time_ratio(), s.time_cap(), time, std::string(s.z_only ? ", z-only: planar layers of varying height" : "") + s.offers(),
                                         s.time_ratio() <= s.time_cap() ? "<=" : ">");
        if (std::isfinite(s.measured_ratio) && m_print.objects().size() > 1)
            s.decision += Slic3r::format(", measured %1$.3f x planar + support (private exports; %2% re-picks%3%, %4$.0f s)", s.measured_ratio,
                                         s.measured_repicks, s.measured_anchor ? ", the anchor" : "", s.measured_seconds) +
                          s.floating_decision();
        if (!s.route_log.empty())
            s.decision += (s.route ? ", a tilted route; " : ", the body over its routes; ") + s.route_log;
        BOOST_LOG_TRIVIAL(info) << object->model_object()->name << ": " << s.decision;
    }
    for (const ClearanceHit &hit : report.worst)
        BOOST_LOG_TRIVIAL(warning) << "One-shot field: hit at G-code line " << hit.gcode_line << ", layer " << hit.layer << ", "
                                   << hit.component << " " << hit.penetration << " mm into material, role "
                                   << (hit.move_id < result.moves.size() ? ExtrusionEntity::role_to_string(result.moves[hit.move_id].extrusion_role) : "-");
    if (hits > 0) {
        for (PrintObject *object : objects) {
            OneShotState &s = object->oneshot_state();
            if (s.offer == OneShotState::Offer::Control || s.injected || m_repicks[object] > MaxRepicks + 1)
                continue;
            ++m_repicks[object];
            s.offer = OneShotState::Offer::Control;
            ++s.measured_repicks;
            s.solved.reset();
            s.decision += Slic3r::format(", %1% moves the contact bar strikes -> control", hits);
            m_guard_warnings.push_back(Slic3r::format(_u8L("%1%: the non-planar print would touch printed plastic with the nozzle in %2% "
                                                           "moves, so it prints planar layers with support instead."),
                                                      object->model_object()->name, hits));
            m_repick.push_back(object);
        }
        if (!m_repick.empty()) {
            result.clearance_report = std::move(report);
            result.clearance        = result.clearance_report.per_move;
            return true;
        }
    }
    if (hits > 0)
        throw Slic3r::SlicingError(Slic3r::format(_u8L("One-shot field: %1% moves strike printed material (a solver bug: the solved field must "
                                                       "satisfy its constraints)."),
                                                  hits));
    const bool exported_support = std::any_of(result.moves.begin(), result.moves.end(), [](const auto &mv) {
        return mv.type == EMoveType::Extrude && is_support(mv.extrusion_role);
    });
    for (PrintObject *object : objects) {
        const bool planned = std::any_of(object->support_layers().begin(), object->support_layers().end(),
                                         [](const SupportLayer *layer) { return !layer->support_fills.empty(); }) ||
                             (!object->native_summary().empty() && exported_support);
        if (object->auto_support_need() != NoNeedSupp && !planned && object->config().enable_support.value)
            throw Slic3r::SlicingError(Slic3r::format(_u8L("One-shot field: %1% needs support (%2%), and it prints none (support blockers "
                                                           "or settings forbid it)."),
                                                      object->model_object()->name,
                                                      object->auto_support_need() == SharpTail  ? _u8L("floating regions") :
                                                      object->auto_support_need() == Cantilever ? _u8L("floating cantilever") :
                                                                                                    _u8L("large overhangs")));
        if (planned)
            object->oneshot_state().decision +=
                object->auto_support_applied() ?
                    Slic3r::format(", support added: it would float without it (%1$.0f mm3)", object->auto_support_volume()) :
                    std::string(", support on (the user's)");
    }
    auto refuse_forced_route = [](const OneShotState &s) {
        if (const char *force = std::getenv("NP_ONESHOT_ROUTE_FORCE"); force != nullptr && *force != 0 && s.route && route_forced(s.route_cuts))
            throw Slic3r::SlicingError(Slic3r::format("FORCED %1% NOT ADOPTED: %2% did not print (the export's checks re-pick it%3%%4%)",
                                                      std::string(force).rfind("{A:", 0) == 0 ? "ARM" : "ROUTE", force, s.floating_decision(),
                                                      s.band_note + s.auto_note));
    };
    if (m_print.objects().size() == 1 && std::getenv("NP_ONESHOT_NO_MEASURED_CHECK") == nullptr && !rotary_auto_measurement()) {
        PrintObject  *object = objects.front();
        OneShotState &s      = object->oneshot_state();
        const MeasureDeadline deadline(measure_deadline(s));
        const MeasureWork     work(measure_work(*object, s));
        const Floating floating = floating_road(result, *object);
        const bool     support  = std::any_of(object->support_layers().begin(), object->support_layers().end(),
                                              [](const SupportLayer *layer) { return !layer->support_fills.empty(); });
        if (s.z_only) {
            const auto     t0       = std::chrono::steady_clock::now();
            const double   t_ps     = measured_time(*object, NonplanarMode::Off, {}, {}, false);
            const bool     budget   = m_repicks[object] <= MaxRepicks;
            bool           repick   = budget && measured_repick(*object, s, time / t_ps, floating, support);
            if (budget && !repick && !m_band_tried[object]) {
                m_band_tried[object] = true;
                repick               = band_control(*object, s);
            }
            repick = repick || (budget && (auto_offer(*object, s, t_ps, export_normal_rms(*object, result), time) || body_earns_control(*object, s, floating) ||
                                                           control_past_cap(*object, s) || planar_keeps_pick(*object, s, floating, net_support_mm3(m_print, result))));
            s.measured_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (repick) {
                refuse_forced_route(s);
                ++m_repicks[object];
                s.solved.reset();
                m_repick.push_back(object);
            }
            s.decision += Slic3r::format(", measured %1$.3f x planar + support (the export; %2% re-picks%3%, %4$.0f s)", s.measured_ratio,
                                         s.measured_repicks, s.measured_anchor ? ", the anchor" : "", s.measured_seconds) +
                          s.floating_decision() + s.band_note + s.auto_note;
        } else {
            if (m_repicks[object] <= MaxRepicks && (measured_repick(*object, s, std::numeric_limits<double>::quiet_NaN(), floating, support) ||
                                                    planar_keeps_pick(*object, s, floating, net_support_mm3(m_print, result)))) {
                refuse_forced_route(s);
                ++m_repicks[object];
                s.solved.reset();
                m_repick.push_back(object);
            }
            s.decision += s.floating_decision();
        }
    }
    result.clearance_report = std::move(report);
    result.clearance        = result.clearance_report.per_move;
    return true;
}

namespace {
std::mutex       morph_refused_mutex;
std::set<size_t> morph_refused;
std::map<size_t, size_t> morph_struck;
}

size_t morph_struck_hits(const PrintObject &object)
{
    const auto *set = object.model_object()->config.get().option<ConfigOptionEnum<NonplanarMode>>("nonplanar_mode");
    const NonplanarMode requested = set ? set->value : object.print()->default_object_config().nonplanar_mode.value;
    if (requested != NonplanarMode::SmoothMorph || object.config().nonplanar_mode.value != NonplanarMode::Off)
        return 0;
    const std::lock_guard<std::mutex> lock(morph_refused_mutex);
    const auto it = morph_struck.find(object.model_object()->id().id);
    return it == morph_struck.end() ? 0 : it->second;
}

bool auto_morph_refused(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(morph_refused_mutex);
    return morph_refused.count(object.id().id) != 0;
}

bool top_picks_morph()
{
    static const bool on = std::getenv("NP_TOP_PICKS_MORPH") != nullptr && std::string(std::getenv("NP_TOP_PICKS_MORPH")) == "1";
    return on;
}


namespace {
std::mutex                                  conformal_step_mutex;
std::map<size_t, int>                       conformal_steps;
std::map<size_t, size_t>                    conformal_flat;
std::map<size_t, std::vector<Vec3d>>        conformal_struck;
std::map<size_t, int>                       conformal_refusals;
std::map<size_t, size_t>                    conformal_refused_at;
std::set<size_t>                            conformal_pending;
std::map<size_t, std::vector<std::pair<Vec3d, Vec3d>>> conformal_precede;
std::map<size_t, int>                       conformal_order_rounds;
std::map<size_t, size_t>                    conformal_order_hits;
}

size_t conformal_flat_hits(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(conformal_step_mutex);
    const auto it = conformal_flat.find(object.id().id);
    return it == conformal_flat.end() ? 0 : it->second;
}

void conformal_ladder_slice(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(conformal_step_mutex);
    const size_t id = object.id().id;
    if (conformal_pending.erase(id) != 0)
        return;
    conformal_steps.erase(id), conformal_flat.erase(id), conformal_struck.erase(id), conformal_refusals.erase(id), conformal_refused_at.erase(id);
    conformal_precede.erase(id), conformal_order_rounds.erase(id), conformal_order_hits.erase(id);
}

std::vector<std::pair<Vec3d, Vec3d>> conformal_precedence(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(conformal_step_mutex);
    const auto it = conformal_precede.find(object.id().id);
    return it == conformal_precede.end() ? std::vector<std::pair<Vec3d, Vec3d>>() : it->second;
}

std::vector<Vec3d> conformal_struck_points(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(conformal_step_mutex);
    const auto it = conformal_struck.find(object.id().id);
    return it == conformal_struck.end() ? std::vector<Vec3d>() : it->second;
}

int conformal_cap_step(const ModelObject &object)
{
    static const int first = std::getenv("NP_CONFORMAL_RUNG") ? std::atoi(std::getenv("NP_CONFORMAL_RUNG")) : 0;
    const std::lock_guard<std::mutex> lock(conformal_step_mutex);
    const auto it = conformal_steps.find(object.id().id);
    return std::max(first, it == conformal_steps.end() ? 0 : it->second);
}

bool ClearanceRepair::step_down_conformal(const GCodeProcessorResult &result)
{
    std::vector<PrintObject *> picked;
    for (PrintObject *object : m_print.m_objects)
        if (object->config().nonplanar_mode.value == NonplanarMode::ConformalLayers && conformal_cap_step(*object->model_object()) < ConformalSteps)
            picked.push_back(object);
    static const bool no_fallback = std::getenv("NP_CONFORMAL_NO_FALLBACK") != nullptr;
    if (picked.empty() || no_fallback)
        return false;
    bool changed = false;
    const auto go_flat = [this](PrintObject *object, size_t hits) {
        {
            const std::lock_guard<std::mutex> lock(conformal_step_mutex);
            conformal_steps[object->model_object()->id().id] = ConformalSteps;
            conformal_flat[object->model_object()->id().id]  = hits;
        }
        object->m_config.nonplanar_mode.value = NonplanarMode::Off;
        object->m_nonplanar_repair.reset();
        m_guard_warnings.push_back(Slic3r::format(_u8L("%1%: Conformal layers met the print head in %2% moves; printed with flat layers."),
                                                  object->model_object()->name, hits));
    };
    const auto replan = [this, &changed](PrintObject *object) {
        {
            const std::lock_guard<std::mutex> lock(conformal_step_mutex);
            conformal_pending.insert(object->model_object()->id().id);
        }
        m_planned.erase(std::remove_if(m_planned.begin(), m_planned.end(), [object](const Planned &p) { return p.object == object; }),
                        m_planned.end());
        object->invalidate_step(posSlice);
        changed = true;
    };
    for (PrintObject *&object : picked)
        if (object->conformal().patches.empty() && !conformal_struck_points(*object->model_object()).empty()) {
            go_flat(object, conformal_flat_hits(*object->model_object()));
            replan(object);
            BOOST_LOG_TRIVIAL(warning) << "Conformal layers fallback: " << object->model_object()->name << ", every top struck, printed flat";
            object = nullptr;
        }
    picked.erase(std::remove(picked.begin(), picked.end(), nullptr), picked.end());
    if (picked.empty() || !result.nonplanar)
        return changed;
    ClearanceParams params;
    params.head           = resolve_head_envelope(m_print.config());
    params.contact_bar    = true;
    params.cone_tolerance = curved_tops_cone_tolerance(m_print);
    const ClearanceReport report = verify(m_print, result, params, *make_tool_kinematics(m_print.config()));
    if (report.hit_moves == 0)
        return changed;
    const Locator locator(m_print);
    std::map<const PrintObject *, std::vector<Vec2d>> struck;
    {
        std::map<const PrintObject *, std::set<std::pair<int, int>>> seen;
        for (size_t i = 0; i < report.per_move.size() && i < result.moves.size(); ++i)
            if (report.per_move[i] < 0.f)
                for (const PrintObject *object : picked)
                    for (size_t instance = 0; instance < object->instances().size(); ++instance)
                        if (const Vec2d q = locator.to_object(tip_of(result, i), *object, instance, result.moves[i].extruder_id);
                            unscaled(object->bounding_box()).contains(q) &&
                            seen[object].emplace(int(std::floor(q.x() / 0.5)), int(std::floor(q.y() / 0.5))).second)
                            struck[object].push_back(q);
    }
    std::map<const PrintObject *, std::vector<std::pair<Vec3d, Vec3d>>> contacts;
    for (const ClearanceHit &hit : report.worst)
        if (hit.struck_move < result.moves.size())
            for (const PrintObject *object : picked)
                for (size_t instance = 0; instance < object->instances().size(); ++instance) {
                    const unsigned extruder = result.moves[hit.move_id].extruder_id;
                    const Vec3d    s        = tip_of(result, hit.struck_move);
                    if (const Vec2d a = locator.to_object(hit.tip, *object, instance, extruder), b = locator.to_object(s, *object, instance, extruder);
                        unscaled(object->bounding_box()).contains(a)) {
                        contacts[object].emplace_back(Vec3d(a.x(), a.y(), locator.to_object_z(hit.tip.z())),
                                                      Vec3d(b.x(), b.y(), locator.to_object_z(s.z())));
                        break;
                    }
                }
    constexpr int Refusals = 6;
    constexpr size_t OrderHits   = 30;
    constexpr int    OrderRounds = 1;
    for (PrintObject *object : picked) {
        const size_t id = object->model_object()->id().id;
        int  step;
        bool refused = false, ordered = false;
        {
            const std::lock_guard<std::mutex> lock(conformal_step_mutex);
            int &taken = conformal_steps[id];
            if (const auto it = contacts.find(object); it != contacts.end() && report.hit_moves <= OrderHits &&
                conformal_order_rounds[id] < OrderRounds && (conformal_order_rounds[id] == 0 || report.hit_moves < conformal_order_hits[id])) {
                append(conformal_precede[id], it->second);
                ++conformal_order_rounds[id];
                conformal_order_hits[id] = report.hit_moves;
                ordered = true;
            }
        }
        if (ordered) {
            replan(object);
            BOOST_LOG_TRIVIAL(warning) << "Conformal layers fallback: " << object->model_object()->name << ", " << report.hit_moves
                                       << " contact-bar hits, step " << conformal_cap_step(*object->model_object()) << ", roads reordered";
            continue;
        }
        {
            const std::lock_guard<std::mutex> lock(conformal_step_mutex);
            int &taken = conformal_steps[id];
            int  next  = report.hit_moves <= 200 ? taken + 1 : taken < 2 ? 2 : ConformalSteps;
            const bool converging = conformal_refusals[id] == 0 || report.hit_moves <= 2 * conformal_refused_at[id] + 5;
            if (const auto it = struck.find(object);
                next >= ConformalSteps && it != struck.end() && conformal_refusals[id] < Refusals && converging) {
                for (const Vec2d &q : it->second)
                    conformal_struck[id].emplace_back(q.x(), q.y(), double(conformal_refusals[id]));
                ++conformal_refusals[id];
                conformal_refused_at[id] = report.hit_moves;
                next    = std::min(taken, ConformalSteps - 1);
                refused = true;
            }
            if (next != taken)
                conformal_precede.erase(id), conformal_order_rounds.erase(id), conformal_order_hits.erase(id);
            taken = next;
            step  = taken;
            conformal_flat[id] = report.hit_moves;
        }
        if (step >= ConformalSteps)
            go_flat(object, report.hit_moves);
        replan(object);
        BOOST_LOG_TRIVIAL(warning) << "Conformal layers fallback: " << object->model_object()->name << ", " << report.hit_moves
                                   << " contact-bar hits, step " << step << (refused ? ", struck tops flat" : "")
                                   << (step >= ConformalSteps ? ", printed flat" : "");
    }
    return changed;
}

namespace {
std::mutex               slant_step_mutex;
std::map<size_t, int>    slant_steps;
std::map<size_t, size_t> slant_hits;
std::set<size_t>         slant_pending;
}

void slant_ladder_slice(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(slant_step_mutex);
    const size_t id = object.id().id;
    if (slant_pending.erase(id) != 0)
        return;
    slant_steps.erase(id), slant_hits.erase(id);
}

int slant_step(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(slant_step_mutex);
    const auto it = slant_steps.find(object.id().id);
    return it == slant_steps.end() ? 0 : it->second;
}

size_t slant_step_hits(const ModelObject &object)
{
    const std::lock_guard<std::mutex> lock(slant_step_mutex);
    const auto it = slant_hits.find(object.id().id);
    return it == slant_hits.end() ? 0 : it->second;
}

bool ClearanceRepair::step_down_slant(const GCodeProcessorResult &result)
{
    if (m_print.config().nonplanar_collision_check.value != NonplanarCollisionCheck::Block)
        return false;
    std::vector<PrintObject *> picked;
    for (PrintObject *object : m_print.m_objects)
        if (object->config().nonplanar_mode.value == NonplanarMode::Slant && object->slant_choice() && object->slant_choice()->angle_deg > 0. &&
            slant_step(*object->model_object()) < SlantSteps)
            picked.push_back(object);
    if (picked.empty() || !result.nonplanar)
        return false;
    ClearanceParams params;
    params.head           = resolve_head_envelope(m_print.config());
    params.contact_bar    = true;
    params.cone_tolerance = curved_tops_cone_tolerance(m_print);
    const ClearanceReport report = verify(m_print, result, params, *make_tool_kinematics(m_print.config()));
    if (report.hit_moves == 0)
        return false;
    for (PrintObject *object : picked) {
        int step;
        {
            const std::lock_guard<std::mutex> lock(slant_step_mutex);
            step = ++slant_steps[object->model_object()->id().id];
            slant_hits[object->model_object()->id().id] = report.hit_moves;
            slant_pending.insert(object->model_object()->id().id);
        }
        m_planned.erase(std::remove_if(m_planned.begin(), m_planned.end(), [object](const Planned &p) { return p.object == object; }),
                        m_planned.end());
        object->invalidate_step(posSlice);
        BOOST_LOG_TRIVIAL(warning) << "Slant fallback: " << object->model_object()->name << ", " << report.hit_moves << " head contacts at "
                                   << object->slant_choice()->angle_deg << " deg, step " << step;
    }
    return true;
}

bool ClearanceRepair::refuse_struck_morph(const GCodeProcessorResult &result)
{
    std::vector<PrintObject *> picked, chosen;
    for (PrintObject *object : m_print.m_objects) {
        const auto *set = object->model_object()->config.get().option<ConfigOptionEnum<NonplanarMode>>("nonplanar_mode");
        const NonplanarMode requested = set ? set->value : m_print.default_object_config().nonplanar_mode.value;
        if (object->config().nonplanar_mode.value != NonplanarMode::SmoothMorph)
            continue;
        if (requested == NonplanarMode::Auto || (requested == NonplanarMode::NonplanarTop && top_picks_morph()))
            picked.push_back(object);
        else if (requested == NonplanarMode::SmoothMorph)
            chosen.push_back(object);
    }
    if ((picked.empty() && chosen.empty()) || !result.nonplanar)
        return false;
    {
        const std::lock_guard<std::mutex> lock(morph_refused_mutex);
        for (const PrintObject *object : chosen)
            morph_struck.erase(object->model_object()->id().id);
    }
    ClearanceParams params;
    params.head           = resolve_head_envelope(m_print.config());
    params.contact_bar    = true;
    params.cone_tolerance = curved_tops_cone_tolerance(m_print);
    const size_t hits = verify(m_print, result, params, *make_tool_kinematics(m_print.config())).hit_moves;
    if (hits == 0)
        return false;
    for (PrintObject *object : picked) {
        {
            const std::lock_guard<std::mutex> lock(morph_refused_mutex);
            morph_refused.insert(object->model_object()->id().id);
        }
        object->m_config.nonplanar_mode.value = NonplanarMode::NonplanarTop;
        object->invalidate_step(posSlice);
        m_guard_warnings.push_back(Slic3r::format(_u8L("%1%: Auto printed Curved tops: Smooth morph's roads would touch the "
                                                       "nozzle in %2% moves."), object->model_object()->name, hits));
        BOOST_LOG_TRIVIAL(warning) << "Auto Smooth morph refused: " << object->model_object()->name << ", " << hits << " contact-bar hits";
    }
    for (PrintObject *object : chosen) {
        {
            const std::lock_guard<std::mutex> lock(morph_refused_mutex);
            morph_struck[object->model_object()->id().id] = hits;
        }
        object->m_config.nonplanar_mode.value = NonplanarMode::Off;
        object->invalidate_step(posSlice);
        m_guard_warnings.push_back(Slic3r::format(_u8L("%1%: Smooth morph would hit the part in %2% moves; printed flat."),
                                                  object->model_object()->name, hits));
        BOOST_LOG_TRIVIAL(warning) << "Smooth morph struck: " << object->model_object()->name << ", " << hits << " contact-bar hits; printed flat";
    }
    return true;
}

bool ClearanceRepair::step_down_morph_direction(const GCodeProcessorResult &result)
{
    std::vector<PrintObject *> picked;
    for (PrintObject *object : m_print.m_objects)
        if (object->config().nonplanar_mode.value == NonplanarMode::SmoothMorph &&
            object->printing_region(0).config().nonplanar_top_direction.value <= -2. &&
            (!object->m_nonplanar_repair || object->m_nonplanar_repair->top_angle_rungs.count(-1) == 0))
            picked.push_back(object);
    if (picked.empty() || !result.nonplanar)
        return false;
    ClearanceParams params;
    params.head           = resolve_head_envelope(m_print.config());
    params.contact_bar    = true;
    params.cone_tolerance = curved_tops_cone_tolerance(m_print);
    const size_t hits = verify(m_print, result, params, *make_tool_kinematics(m_print.config())).hit_moves;
    if (hits == 0)
        return false;
    for (PrintObject *object : picked) {
        if (!object->m_nonplanar_repair) object->m_nonplanar_repair = std::make_shared<RepairHints>();
        object->m_nonplanar_repair->top_angle_rungs[-1] = AutoTopCandidates;
        object->invalidate_step(posSlice);
        BOOST_LOG_TRIVIAL(warning) << "Smooth morph Curved top direction Auto: " << object->model_object()->name << ", " << hits
                                   << " contact-bar hits; the top takes the host's direction";
    }
    return true;
}

size_t ClearanceRepair::curved_contacts(const Print &print, const GCodeProcessorResult &result)
{
    ClearanceParams params;
    params.head        = resolve_head_envelope(print.config());
    params.contact_bar = true;
    const ClearanceReport report = verify(print, result, params, *make_tool_kinematics(print.config()));
    size_t contacts = 0;
    for (size_t i = 0; i < result.moves.size() && i < report.per_move.size(); ++i)
        if (report.per_move[i] < 0.f && result.moves[i].type == EMoveType::Extrude && result.state(result.moves[i]).nonplanar_patch_shell != 0xFF)
            ++contacts;
    return contacts;
}

bool ClearanceRepair::contact_guard(const GCodeProcessorResult &result, size_t contacts)
{
    std::vector<PrintObject *> guarded;
    for (PrintObject *object : m_print.m_objects)
        if (oneshot_requested(*object) || object->config().nonplanar_mode.value == NonplanarMode::Auto || object->m_auto_planar ||
            auto_rotary_requested(*object))
            guarded.push_back(object);
    if (guarded.empty() || !result.nonplanar || m_guard_passes >= 2 * int(guarded.size()) || std::getenv("NP_CONTACT_GUARD_OFF") != nullptr)
        return false;
    if (contacts == 0) {
        ClearanceParams params;
        params.head        = resolve_head_envelope(m_print.config());
        params.contact_bar = true;
        contacts = verify(m_print, result, params, *make_tool_kinematics(m_print.config())).hit_moves;
    }
    if (contacts == 0)
        return false;
    for (PrintObject *object : guarded) {
        const std::string name = object->model_object()->name;
        if (OneShotState *s = object->m_oneshot.get(); s != nullptr && oneshot_requested(*object)) {
            if (s->offer == OneShotState::Offer::Auto) {
                s->offer = OneShotState::Offer::Body;
                s->decision += Slic3r::format(", Auto's print dropped: %1% moves the contact bar strikes", contacts);
            } else if (!s->measured_anchor) {
                s->measured_anchor = true;
                s->solved.reset();
                s->decision += Slic3r::format(", the pick dropped for the anchor: %1% moves the contact bar strikes", contacts);
            } else
                continue;
        } else {
            if (object->m_auto_floating_planar)
                continue;
            object->m_auto_floating_planar = true;
            object->m_auto_planar_exact    = true;
            object->m_nonplanar_repair.reset();
            object->note_auto_fallback(_u8L("the nozzle would touch printed plastic"));
            object->m_auto_planar_pass     = true;
        }
        BOOST_LOG_TRIVIAL(warning) << "Contact guard: " << name << ": " << contacts << " moves strike printed material under the "
                                   << "contact bar; it prints without them";
        if (oneshot_requested(*object))
            m_guard_warnings.push_back(Slic3r::format(_u8L("%1%: the non-planar print would touch printed plastic with the nozzle in %2% "
                                                           "moves, so it prints planar layers with support instead."),
                                                      name, contacts));
        m_repick.push_back(object);
        ++m_guard_passes;
    }
    return !m_repick.empty();
}

bool ClearanceRepair::curved_tops_step(GCodeProcessorResult &result)
{
    const bool curved = std::any_of(m_print.objects().begin(), m_print.objects().end(), [](const PrintObject *object) {
        return curved_tops_mode(object->config().nonplanar_mode.value);
    });
    if (!curved) return false;
    ClearanceParams params;
    params.head = resolve_head_envelope(m_print.config());
    params.contact_bar = true;
    params.cone_tolerance = curved_tops_cone_tolerance(m_print);
    const auto      started = std::chrono::steady_clock::now();
    ClearanceReport report = verify(m_print, result, params, ThreeAxisKinematics());
    BOOST_LOG_TRIVIAL(warning) << "Curved tops clearance pass " << m_curved_pass << ": " << report.hit_moves
                               << " hits, " << report.hit_body_moves << " body hits, " << report.cone_graze_moves << " grazes, verify "
                               << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s";
    if (report.hit_moves > 0) {
        for (const ClearanceHit &hit : report.worst) {
            const auto &move = result.moves[hit.move_id];
            BOOST_LOG_TRIVIAL(debug) << "Curved tops contact: line " << hit.gcode_line
                << " move " << int(move.type) << " role " << int(move.extrusion_role)
                << " shell " << int(result.state(move).nonplanar_patch_shell)
                << " component " << hit.component << " depth " << hit.penetration
                << " tip " << hit.tip.x() << "," << hit.tip.y() << "," << hit.tip.z();
        }
        for (size_t i = 0; i < result.moves.size() && i < report.per_move.size(); ++i) {
            if (report.per_move[i] >= -1e-5f || result.moves[i].extrusion_role == erCustom) continue;
            const Vec3d tip = tip_of(result, i);
            for (PrintObject *object : m_print.objects()) {
                if (!object->curved_fallback_zaa()) continue;
                for (const auto &instance : object->instances())
                    if (object->bounding_box().contains(Point::new_scale(Vec2d(tip.head<2>() - unscale(instance.shift))))) {
                        object->m_curved_fallback_zaa = false;
                        object->m_curved_fallback_off = true;
                        if (std::find(m_repick.begin(), m_repick.end(), object) == m_repick.end()) m_repick.push_back(object);
                        m_guard_warnings.push_back(object->model_object()->name +
                                                   ": its tops print flat layers instead of mid-layer Z contouring, which struck the head.");
                        break;
                    }
            }
        }
        if (!m_repick.empty()) {
            ++m_curved_pass;
            for (const auto &warning : m_guard_warnings)
                m_print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, warning);
            result.clearance_report = std::move(report);
            result.clearance = result.clearance_report.per_move;
            return true;
        }
        const bool nonplanar_top = std::any_of(m_print.objects().begin(), m_print.objects().end(), [](const PrintObject *object) {
            return curved_tops_mode(object->config().nonplanar_mode.value);
        });
        const int disc_passes = nonplanar_top ? (clearance_override_active(m_print.config()) ? 8 : 5) : 2;
        const int pass = m_curved_pass - m_curved_angle_passes;
        if (pass >= disc_passes + 1)
            throw Slic3r::SlicingError("Curved tops: head clearance still fails after its surface repair passes; export blocked.");
        bool angled = false, repaired = false;
        std::vector<CurvedHit> hits;
        for (size_t i = 0; i < result.moves.size() && i < report.per_move.size(); ++i) {
            const auto &move = result.moves[i];
            if (report.per_move[i] >= -1e-5f || move.extrusion_role == erCustom) continue;
            PrintObject *owner = nullptr;
            int surface_id = -1;
            double best = std::numeric_limits<double>::infinity();
            Vec2d owner_shift = Vec2d::Zero();
            const Vec3d tip = tip_of(result, i);
            for (PrintObject *object : m_print.objects()) {
                if (!curved_tops_mode(object->config().nonplanar_mode.value)) continue;
                for (const auto &instance : object->instances()) {
                    const Vec2d p = tip.head<2>() - unscale(instance.shift);
                    for (const auto &surface : object->nonplanar_surfaces()) {
                        const double dx = std::max({double(surface.stats.min.x) - p.x(), p.x() - double(surface.stats.max.x), 0.});
                        const double dy = std::max({double(surface.stats.min.y) - p.y(), p.y() - double(surface.stats.max.y), 0.});
                        const double dz = std::max({double(surface.stats.min.z) - tip.z(), tip.z() - double(surface.stats.max.z), 0.});
                        double score = dx * dx + dy * dy + dz * dz;
                        if (result.state(move).nonplanar_patch == surface.stable_id &&
                            result.state(move).nonplanar_patch_shell != 0xFF && dx < 1. && dy < 1.) score = -1.;
                        if (score < best && dx < params.head.reach() && dy < params.head.reach() && dz < 8.) {
                            best = score, owner = object, surface_id = surface.stable_id;
                            owner_shift = unscale(instance.shift);
                        }
                    }
                }
            }
            if (owner == nullptr) continue;
            hits.push_back({ owner, surface_id, owner_shift, tip, tip_of(result, i == 0 ? i : i - 1), report.per_move[i] });
        }
        std::map<SurfaceKey, size_t> surface_hits;
        for (const CurvedHit &hit : hits)
            ++surface_hits[{ hit.owner, hit.surface_id }];
        for (const auto &key : m_curved_roads_last)
            if (surface_hits[key] > 0 && surface_hits[key] >= m_curved_hits_last[key])
                m_curved_roads_stalled.insert(key);
        std::set<std::pair<const PrintObject *, int>> ordered_now;
        std::set<std::pair<const PrintObject *, int>> roads_now;
        auto repair_hit = [&](const CurvedHit &hit) {
            PrintObject *owner       = hit.owner;
            const int    surface_id  = hit.surface_id;
            const Vec2d &owner_shift = hit.shift;
            const Vec3d &tip         = hit.tip;
            repaired = true;
            if (curved_tops_mode(owner->config().nonplanar_mode.value)) {
                if (!owner->m_nonplanar_repair) owner->m_nonplanar_repair = std::make_shared<RepairHints>();
                RepairHints &hints = *owner->m_nonplanar_repair;
                const double band = head_band_height(params.head) + params.head.contact_tolerance;
                const auto   struck = std::find_if(owner->nonplanar_surfaces().begin(), owner->nonplanar_surfaces().end(),
                                                   [surface_id](const NonplanarSurface &s) { return s.stable_id == surface_id; });
                if (struck != owner->nonplanar_surfaces().end() && band > 0. && hints.banded_surfaces.count(surface_id) == 0 &&
                    struck->stats.max.z - struck->stats.min.z > band + EPSILON) {
                    hints.banded_surfaces.insert(surface_id);
                    hints.band_height = band;
                    if (std::find(m_repick.begin(), m_repick.end(), owner) == m_repick.end()) m_repick.push_back(owner);
                    return;
                }
                if (hints.ordered_surfaces.count(surface_id) == 0 && m_curved_roads_stalled.count({ owner, surface_id }) == 0) {
                    if (roads_now.count({ owner, surface_id }) == 0 && ordered_now.count({ owner, surface_id }) == 0) {
                        if (hints.ordered_road_passes[surface_id] < 3) {
                            roads_now.insert({ owner, surface_id });
                            ++hints.ordered_road_passes[surface_id];
                        } else {
                            hints.ordered_surfaces.insert(surface_id);
                            ordered_now.insert({ owner, surface_id });
                        }
                    }
                    if (roads_now.count({ owner, surface_id }) != 0)
                        hints.ordered_roads.push_back({ tip.head<2>() - owner_shift, 0.6, tip.z() });
                }
                if (roads_now.count({ owner, surface_id }) != 0 || ordered_now.count({ owner, surface_id }) != 0) {
                    if (std::find(m_repick.begin(), m_repick.end(), owner) == m_repick.end()) m_repick.push_back(owner);
                    return;
                }
            }
            if (pass < disc_passes) {
                int disc_pass = pass;
                if (curved_tops_mode(owner->config().nonplanar_mode.value)) {
                    disc_pass = 0;
                    if (owner->m_nonplanar_repair) {
                        const Vec2d xy = tip.head<2>() - owner_shift;
                        for (const RepairHints::Disc &disc : owner->m_nonplanar_repair->planar)
                            disc_pass += (disc.p - xy).norm() < disc.radius + 2.5 && std::abs(disc.z - tip.z()) < disc.radius + 2.5;
                    }
                }
                double radius = 2.5 * (std::max(0, disc_pass) + 1);
                for (const HeadBox &box : params.head.boxes)
                    if (box.nozzle && !box.infinite_x && !box.infinite_y &&
                        box.min.z() <= -double(hit.depth) + EPSILON)
                        radius = std::max(radius, 2.5 * (std::max(0, disc_pass) + 1) +
                            std::max({-box.min.x(), box.max.x(), -box.min.y(), box.max.y()}));
                if (!owner->m_nonplanar_repair) owner->m_nonplanar_repair = std::make_shared<RepairHints>();
                auto &discs = owner->m_nonplanar_repair->planar;
                const Vec3d &from = hit.from;
                const int samples = std::max(1, int(std::ceil((tip - from).head<2>().norm() / (0.5 * radius))));
                for (int k = 0; k <= samples; ++k) {
                    const Vec3d p = from + (double(k) / samples) * (tip - from);
                    const Vec2d xy = p.head<2>() - owner_shift;
                    if (std::none_of(discs.begin(), discs.end(), [&](const RepairHints::Disc &disc) {
                            return disc.radius >= radius && (disc.p - xy).norm() < 0.5 * radius &&
                                std::abs(disc.z - p.z()) < radius;
                        }))
                        discs.push_back({xy, radius, p.z()});
                }
            } else
                owner->m_curved_planar_surfaces.insert(surface_id);
            if (std::find(m_repick.begin(), m_repick.end(), owner) == m_repick.end()) m_repick.push_back(owner);
        };
        std::map<std::pair<PrintObject *, int>, std::vector<CurvedHit>> angled_now;
        for (const CurvedHit &hit : hits) {
            PrintObject *owner = hit.owner;
            if (owner->printing_region(0).config().nonplanar_top_direction.value <= -2. &&
                (!owner->m_nonplanar_repair || owner->m_nonplanar_repair->top_angle_settled.count(hit.surface_id) == 0)) {
                angled_now[{ owner, hit.surface_id }].push_back(hit);
                angled = true;
                continue;
            }
            repair_hit(hit);
        }
        for (auto &[key, struck] : angled_now) {
            PrintObject *owner = key.first;
            if (!owner->m_nonplanar_repair) owner->m_nonplanar_repair = std::make_shared<RepairHints>();
            RepairHints &hints = *owner->m_nonplanar_repair;
            int  &rung = hints.top_angle_rungs[key.second];
            auto &seen = hints.top_angle_hits[key.second];
            seen.resize(size_t(std::max(rung + 1, int(seen.size()))));
            seen[size_t(rung)] = struck.size();
            auto &rung_hits = m_curved_rung_hits[{ owner, key.second }];
            rung_hits[rung] = std::move(struck);
            if (rung < AutoTopCandidates)
                ++rung;
            else {
                rung = int(std::min_element(seen.begin(), seen.end()) - seen.begin());
                hints.top_angle_settled.insert(key.second);
                if (const auto it = rung_hits.find(rung); it != rung_hits.end()) {
                    for (const CurvedHit &hit : it->second)
                        repair_hit(hit);
                    surface_hits[key] = it->second.size();
                }
            }
            hints.refill_tops = true;
            if (std::find(m_repick.begin(), m_repick.end(), owner) == m_repick.end()) m_repick.push_back(owner);
        }
        m_curved_roads_last = roads_now;
        m_curved_hits_last  = std::move(surface_hits);
        if (m_repick.empty())
            throw Slic3r::SlicingError("Curved tops: a head clearance hit cannot be attributed to an active curved surface; export blocked.");
        for (PrintObject *object : m_repick)
            if (object->m_curved_planar_surfaces.size() >= object->curved_surface_candidates()) {
                const std::string warning = object->model_object()->name + ": Curved tops printed planar: every curved surface was struck by the head clearance check.";
                m_guard_warnings.push_back(warning);
                BOOST_LOG_TRIVIAL(warning) << warning;
            } else if (repaired && pass < disc_passes) {
                const std::string warning = object->model_object()->name +
                    ": Curved tops returned the head-strike neighbourhood to planar layers and retained the remaining cap for clearance verification.";
                m_guard_warnings.push_back(warning);
                BOOST_LOG_TRIVIAL(warning) << warning;
            }
        ++m_curved_pass;
        if (angled && !repaired)
            ++m_curved_angle_passes;
    }
    restore_planar_remainders();
    for (const auto &warning : m_guard_warnings)
        m_print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, warning);
    result.clearance_report = std::move(report);
    result.clearance = result.clearance_report.per_move;
    mark_clearance_override(m_print, result);
    return true;
}

bool ClearanceRepair::restore_planar_remainders()
{
    bool restored = false;
    for (PrintObject *object : m_print.objects()) {
        if (!curved_tops_mode(object->config().nonplanar_mode.value) ||
            object->nonplanar_surfaces().empty() ||
            curved_tops_stats(*object).kept != 0)
            continue;
        bool changed = false;
        for (const auto &surface : object->nonplanar_surfaces())
            changed |= object->m_curved_planar_surfaces.insert(surface.stable_id).second;
        if (changed) {
            restored = true;
            if (std::find(m_repick.begin(), m_repick.end(), object) == m_repick.end())
                m_repick.push_back(object);
            const std::string warning = object->model_object()->name +
                ": Curved tops printed planar: no curved top remains; restored the original planar body.";
            m_guard_warnings.push_back(warning);
            BOOST_LOG_TRIVIAL(warning) << warning;
        }
    }
    return restored;
}

bool ClearanceRepair::apply()
{
    if (!m_repick.empty()) {
        for (PrintObject *object : m_repick) {
            if (curved_tops_mode(object->config().nonplanar_mode.value)) {
                auto &surfaces = object->m_nonplanar_surfaces;
                surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [object](const NonplanarSurface &surface) {
                    return object->m_curved_planar_surfaces.count(surface.stable_id) != 0; }), surfaces.end());
                bool walls = false;
                for (size_t region_id = 0; region_id < object->num_printing_regions(); ++region_id)
                    walls |= object->printing_region(region_id).config().nonplanar_sublayer_walls.value;
                if (surfaces.empty() && !object->m_curved_planar_surfaces.empty() && !walls)
                    object->m_curved_fallback_zaa = !object->m_curved_fallback_off;
                const auto scaffold = object->m_nonplanar_repair ? object->m_nonplanar_repair->curved_scaffold : nullptr;
                const bool refill = object->m_nonplanar_repair && std::exchange(object->m_nonplanar_repair->refill_tops, false);
                if (!refill && scaffold && scaffold->owner == object && !scaffold->regions.empty() &&
                    !object->need_z_contouring() && !m_print.m_pipeline_plugin_active) {
                    scaffold->restore();
                    object->invalidate_steps({posNonplanarProjection, posNonplanar, posContouring});
                    m_print.invalidate_step(psGCodeExport);
                } else
                    object->invalidate_step(posPerimeters);
            } else
                object->invalidate_step(posSlice);
        }
        m_repick.clear();
        return true;
    }
    if (m_planned.empty())
        return false;
    for (Planned &p : m_planned) {
        PrintObject &object = *p.object;
        if (!object.m_nonplanar_repair)
            object.m_nonplanar_repair = std::make_shared<RepairHints>();
        RepairHints &h = *object.m_nonplanar_repair;
        const bool skin = std::any_of(object.conformal().patches.begin(), object.conformal().patches.end(),
                                      [](const ConformalPatch &patch) { return patch.kind == PatchKind::Skin; });
        const bool regenerate = !p.hints.surface_caps.empty() || !p.hints.ironing_drops.empty() || !p.hints.order_bands.empty() ||
                                !p.hints.uniform_beads.empty() || !p.hints.planar.empty() || (skin && !p.hints.upright.empty());
        h.surface_caps.insert(h.surface_caps.end(), p.hints.surface_caps.begin(), p.hints.surface_caps.end());
        h.ironing_drops.insert(h.ironing_drops.end(), p.hints.ironing_drops.begin(), p.hints.ironing_drops.end());
        h.order_bands.insert(h.order_bands.end(), p.hints.order_bands.begin(), p.hints.order_bands.end());
        h.uniform_beads.insert(h.uniform_beads.end(), p.hints.uniform_beads.begin(), p.hints.uniform_beads.end());
        h.upright.insert(h.upright.end(), p.hints.upright.begin(), p.hints.upright.end());
        h.planar.insert(h.planar.end(), p.hints.planar.begin(), p.hints.planar.end());
        h.travel_lift = std::max(h.travel_lift, p.hints.travel_lift);
        h.travel_layers.insert(h.travel_layers.end(), p.hints.travel_layers.begin(), p.hints.travel_layers.end());
        std::sort(h.travel_layers.begin(), h.travel_layers.end());
        h.travel_layers.erase(std::unique(h.travel_layers.begin(), h.travel_layers.end()), h.travel_layers.end());
        if (regenerate)
            object.invalidate_step(posPerimeters);
        else
            m_print.invalidate_step(psGCodeExport);
    }
    m_planned.clear();
    ++m_pass;
    return true;
}

std::string ClearanceRepair::summary(size_t hits_now) const
{
    std::ostringstream detail;
    detail << std::setprecision(2);
    for (const PrintObject *object : m_print.objects()) {
        const RepairHints *hints = object->nonplanar_repair();
        if (hints == nullptr || m_pass == 0)
            continue;
        for (const PatchDiagnostics &diag : object->conformal().diagnostics)
            if (diag.repaired_nodes > 0)
                detail << "; " << diag.repaired_nodes << " nodes lowered by <= " << diag.repair_lowering << " mm on patch " << diag.patch_id;
        if (!hints->order_bands.empty()) {
            double height = Inf;
            for (const RepairHints::OrderBand &b : hints->order_bands) height = std::min(height, b.height);
            detail << "; patches of " << object->model_object()->name << " printed in bands <= " << height << " mm";
        }
        if (hints->travel_lift > 0.)
            detail << "; travel lifted " << hints->travel_lift << " mm";
        if (!hints->ironing_drops.empty())
            detail << "; " << hints->ironing_drops.size() << " ironing spots dropped";
        if (!hints->uniform_beads.empty())
            detail << "; " << hints->uniform_beads.size() << " spots at the uniform shell height";
    }
    for (const std::string &action : m_actions)
        detail << "; " << action;
    BOOST_LOG_TRIVIAL(info) << "Non-planar repair: " << m_first_hits << " hits -> " << hits_now << " after " << m_pass << " passes" << detail.str();
    return m_pass == 0 ? Slic3r::format(_u8L("Head clearance check: %1% moves would hit printed material, none repaired."), hits_now) :
                         Slic3r::format(_u8L("Clearance repair passes: %1%. Moves that would hit printed material: %2% before, %3% after."),
                                        m_pass, m_first_hits, hits_now);
}

}
