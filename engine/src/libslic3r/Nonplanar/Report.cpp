#include "Report.hpp"
#include "CurvedTopsSurface.hpp"
#include "OneShotField.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "AutoMode.hpp"
#include "ConformalLayers.hpp"
#include "ConformalShells.hpp"
#include "HeadEnvelope.hpp"
#include "HeightField.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ClearanceRepair.hpp"
#include "libslic3r/GCode/ClearanceVerifier.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace Slic3r::Nonplanar {

namespace {
std::vector<std::string> warning_messages(const std::vector<CodedWarning> &warnings)
{
    std::vector<std::string> out;
    out.reserve(warnings.size());
    for (const CodedWarning &warning : warnings)
        out.push_back(warning.message);
    return out;
}

std::string fallback_code(const std::string &limit)
{
    if (limit == "access" || limit == "hits")
        return "head_strike";
    return limit.empty() ? "no_curved_top" : limit;
}
}


namespace {

const char *pattern_name(NonplanarTopPattern pattern)
{
    return pattern == NonplanarTopPattern::Contour ? "contour" : pattern == NonplanarTopPattern::Concentric ? "concentric" : "rectilinear";
}

bool is_underside(const std::string &reason) { return reason == "bottom" || boost::starts_with(reason, "overhang"); }

float surface_z(const HeightField &surface, const Vec2d &p)
{
    if (const float z = surface.sample(p); !std::isnan(z))
        return z;
    const Vec2d g  = (p - surface.origin()) / surface.pitch();
    const int   ix = int(std::floor(g.x())), iy = int(std::floor(g.y()));
    float       best = HeightField::NoData;
    double      best_d = std::numeric_limits<double>::max();
    for (int y = iy - 1; y <= iy + 2; ++y)
        for (int x = ix - 1; x <= ix + 2; ++x)
            if (x >= 0 && y >= 0 && x < surface.nx() && y < surface.ny() && !std::isnan(surface.at(x, y)))
                if (const double d = (surface.node(x, y) - p).squaredNorm(); d < best_d)
                    best_d = d, best = surface.at(x, y);
    return best;
}

}

std::string patch_kind_name(int kind)
{
    static const char *names[] = { "top", "bottom", "volume", "overhang", "chunk", "support interface", "bridge", "field", "skin" };
    return kind >= 0 && kind < int(std::size(names)) ? names[kind] : "patch";
}

namespace {
struct ReasonInfo
{
    const char *cls;
    const char *text;
};
const std::map<std::string, ReasonInfo> &reasons()
{
    static const std::map<std::string, ReasonInfo> map{
        { "flat", { "flat", L("flat surface: flat layers print it cleanly") } },
        { "planar_patch", { "flat", L("tilted flat plane: printed with Z-contoured flat layers") } },
        { "flat_ground", { "flat", L("level surface: flat layers hit its exact height") } },
        { "flat_sliver", { "flat", L("narrow strip beside a flat area: printed with it") } },
        { "steep", { "wall", L("too steep for a curved line: printed flat with Z contouring") } },
        { "steep_strip", { "wall", L("steep step between curved tops: printed flat with Z contouring") } },
        { "narrow", { "wall", L("narrower than half a line: Z-contoured flat layers look cleaner") } },
        { "outline", { "outline", L("outline between surface samples") } },
        { "near_bed", { "fallback", L("too close to the build plate for a curved shell") } },
        { "contact", { "fallback", L("print head would touch nearby plastic: kept flat") } },
        { "no_transition_layer", { "fallback", L("no flat base layer close enough underneath to anchor the first curved shell") } },
        { "no_pieces", { "fallback", L("too narrow for a curved pass to fit") } },
        { "shell_height", { "unreachable", L("curved shell would be too thick for this nozzle") } },
        { "edge", { "fallback", L("edge of a curved top: too close to the bed, too steep, or thinner than one line") } },
        { "head", { "fallback", L("the print head cannot reach it") } },
        { "bottom", { "fallback", L("covered by standard flat bottom layers") } },
        { "overhang_self_supporting", { "wall", L("self-supporting angle: flat layers print it") } },
        { "overhang_unfit", { "wall", L("tiny overhang sliver under a toolpath") } },
        { "overhang_roof", { "wall", L("flat roof: printed as a bridge") } },
        { "overhang_no_root", { "unreachable", L("floating overhang with nothing underneath to build out from") } },
        { "overhang_rootless", { "unreachable", L("floating overhang with nothing underneath to build out from") } },
        { "overhang_resting", { "unreachable", L("on or just above the bed") } },
        { "overhang_unsampled", { "unreachable", L("resting on solid plastic below") } },
        { "overhang_supported", { "unreachable", L("too steep for curved layers: printed on supports") } },
        { "overhang_small", { "fallback", L("overhang is smaller than the minimum patch size") } },
        { "overhang_no_beads", { "fallback", L("too tight to fit a line on this overhang") } },
        { "overhang_no_pieces", { "fallback", L("no overhang path needed here") } },
        { "overhang_narrow", { "wall", L("overhang is narrower than a single line") } },
        { "overhang_outline", { "outline", L("overhang edge falls between surface samples") } },
        { "overhang_rim", { "outline", L("flat overhang rim with no slope to follow") } },
        { "untagged", { "fallback", L("no reason recorded") } },
    };
    return map;
}
}

std::string fallback_reason_text(const std::string &reason)
{
    const auto it = reasons().find(reason);
    return it == reasons().end() ? reason : it->second.text;
}

std::string fallback_class(const std::string &reason)
{
    const auto it = reasons().find(reason);
    return it == reasons().end() ? "fallback" : it->second.cls;
}

bool fallback_is_exact(const std::string &reason) { return fallback_class(reason) != "fallback"; }

bool fallback_hatched(const std::string &reason) { return fallback_class(reason) != "outline"; }

void record_fallback_shape(PatchDiagnostics &diag, ExPolygons shape, const HeightField &surface)
{
    shape = opening_ex(shape, float(scale_(0.05)));
    if (shape.empty() || surface.empty())
        return;
    const BoundingBox bb   = get_extents(shape);
    const coord_t     step = scaled<coord_t>(1.5);
    Polylines         lines;
    for (coord_t c = bb.min.x() - bb.max.y(); c <= bb.max.x() - bb.min.y(); c += step)
        lines.emplace_back(Point(bb.min.y() + c, bb.min.y()), Point(bb.max.y() + c, bb.max.y()));
    lines = intersection_pl(lines, shape);
    for (const ExPolygon &expoly : shape)
        for (const Polygon &polygon : to_polygons(expoly))
            lines.emplace_back(polygon.split_at_first_point());
    const double sample = 0.5;
    for (const Polyline &line : lines) {
        std::vector<Vec3f> draped;
        auto flush = [&] {
            if (draped.size() > 1)
                diag.fallback_hatch.emplace_back(std::move(draped));
            draped.clear();
        };
        auto add = [&](const Vec2d &p) {
            const float z = surface_z(surface, p);
            if (std::isnan(z) || (!draped.empty() && std::abs(z - draped.back().z()) > 2. * sample))
                flush();
            if (!std::isnan(z))
                draped.emplace_back(float(p.x()), float(p.y()), z);
        };
        for (size_t i = 0; i + 1 < line.points.size(); ++i) {
            const Vec2d a = unscale(line.points[i]), b = unscale(line.points[i + 1]);
            const int   n = std::max(1, int(std::ceil((b - a).norm() / sample)));
            for (int k = 0; k < n; ++k)
                add(a + (b - a) * (double(k) / n));
        }
        if (!line.points.empty())
            add(unscale(line.points.back()));
        flush();
    }
    diag.fallback_shape = std::move(shape);
}

NonplanarReport make_report(const Print &print, const GCodeProcessorResult &result, bool planar_too)
{
    NonplanarReport report;
    if (!planar_too && std::all_of(print.objects().begin(), print.objects().end(), [](const PrintObject *object) {
            return !curved_tops_mode(object->config().nonplanar_mode.value) && object->conformal().patches.empty() && object->conformal().diagnostics.empty() &&
                   !conformal_requested(*object) && morph_struck_hits(*object) == 0 &&
                   (object->oneshot_state() == nullptr || object->oneshot_state()->decision.empty());
        }))
        return report;
    std::string     head_warning;
    report.head = resolve_head_envelope(print.config(), &head_warning).id;
    for (const PrintObject *object : print.objects()) {
        const ConformalResult &conformal = object->conformal();
        const size_t  index = report.objects.size();
        ReportObject &out   = report.objects.emplace_back();
        out.name            = object->model_object()->name;
        out.model_object_id = object->model_object()->id().id;
        out.mode            = object->config().nonplanar_mode.serialize();
        if (const size_t hits = morph_struck_hits(*object); hits > 0) {
            out.mode            = "smooth_morph";
            out.morph_hit_moves = hits;
            out.planar_limit    = "hits";
            out.planar_reason   = "Watch out — Smooth morph would bump the print head into the part in " + std::to_string(hits) + " moves.";
        }
        if (conformal_requested(*object)) {
            out.mode           = "conformal_layers";
            out.conformal_rung = std::min(conformal_cap_step(*object->model_object()), ConformalSteps);
            out.conformal_variant = conformal_variant_text(conformal_variant(object->config(), out.conformal_rung));
            out.planar_limit   = conformal_planar_limit(*object, out.planar_reason);
            if (out.planar_limit.empty())
                if (const auto field = conformal_layers_field(*object))
                    for (const ConformalTop &t : field->tops) {
                        (t.fallback.empty() ? out.curved_top : out.planar_top) += t.area_mm2;
                        out.stepped_top += t.stepped_mm2;
                    }
        }
        out.layer_source         = conformal.layer_source;
        out.layer_source_refused = conformal.layer_source_refused;
        if (const OneShotState *oneshot = object->oneshot_state(); oneshot != nullptr) {
            out.oneshot          = oneshot->decision;
            out.oneshot_student  = oneshot->student;
            out.oneshot_polish_k = oneshot->polish_k;
            out.oneshot_max_time_ratio_override = oneshot->max_time_ratio_override;
        }
        for (size_t r = 0; r < object->num_printing_regions(); ++r) {
            const PrintRegionConfig &config = object->printing_region(r).config();
            std::string              keys;
            for (const char *key : { "top_shell_layers", "top_shell_thickness", "top_surface_pattern", "nonplanar_top_shells", "nonplanar_top_pattern", "nonplanar_top_order",
                                     "sparse_infill_density", "wall_loops", "fuzzy_skin", "ironing_type" })
                keys += std::string(keys.empty() ? "" : " ") + key + "=" + config.opt_serialize(key);
            out.regions.emplace_back(std::move(keys));
        }
        out.slant = object->slant_choice();
        if (curved_tops_mode(object->config().nonplanar_mode.value)) {
            out.curved_tops = curved_tops_stats(*object);
            const auto *asked = object->model_object()->config.get().option<ConfigOptionFloat>("nonplanar_curved_max_angle");
            out.cap_requested = asked != nullptr ? asked->value : print.default_object_config().nonplanar_curved_max_angle.value;
            out.curved_cone_tolerance = clearance_params(print.config(), print_clearance_mode(print), true).cone_tolerance;
            out.top_patches = out.curved_tops.kept;
            out.curved_top = out.curved_tops.footprint_mm2;
            for (const auto &surface : object->nonplanar_surfaces()) {
                ReportPatch &rp = report.patches.emplace_back();
                rp.object = index;
                rp.id = surface.stable_id;
                rp.kind = int(PatchKind::Top);
                rp.top = true;
                rp.shells = curved_top_layers(*object);
                const ExPolygons footprint = surface.horizontal_projection();
                const BoundingBox box = get_extents(footprint);
                rp.center = unscale(box.center());
                rp.area = area(footprint) * SCALING_FACTOR * SCALING_FACTOR;
                rp.pattern = "rectilinear";
                rp.summary = "Curved tops (3-axis)";
                for (const auto &instance : object->instances()) {
                    BoundingBox shifted = box;
                    shifted.translate(instance.shift);
                    rp.boxes.push_back(shifted);
                }
            }
        }
        for (const ConformalPatch &patch : conformal.patches) {
            const bool top = patch.kind == PatchKind::Top;
            ++(top ? out.top_patches : out.other_patches);
            ReportPatch &rp = report.patches.emplace_back();
            rp.object       = index;
            rp.id           = patch.id;
            rp.kind         = int(patch.kind);
            rp.top          = top;
            rp.region       = patch.region_id;
            rp.band         = patch.band;
            rp.shells       = patch.shells;
            rp.area         = area(patch.footprint) * SCALING_FACTOR * SCALING_FACTOR;
            const BoundingBox box = get_extents(patch.footprint);
            rp.center = unscale(box.center());
            for (const PrintInstance &instance : object->instances()) {
                BoundingBox shifted = box;
                shifted.translate(instance.shift);
                rp.boxes.emplace_back(shifted);
            }
            if (!top)
                continue;
            out.bands = std::max(out.bands, size_t(patch.band + 1));
            out.curved_top += rp.area;
            rp.pattern = pattern_name(top_pattern(patch, object->printing_region(size_t(patch.region_id)).config()));
            rp.ironing = !patch.ironing.entities.empty();
            rp.explain = explain_top_patch(*object, patch);
            while (!rp.explain.empty() && rp.explain.back() == '\n')
                rp.explain.pop_back();
            rp.summary = describe_top_patch(*object, patch);
        }
        for (const PatchDiagnostics &diag : conformal.diagnostics) {
            if (diag.fallback_area <= 0.)
                continue;
            ReportFallback &fb = report.fallbacks.emplace_back();
            fb.object          = index;
            fb.patch           = diag.patch_id;
            fb.reason          = diag.fallback_reason.empty() ? "untagged" : diag.fallback_reason;
            fb.area            = diag.fallback_area;
            fb.top             = !is_underside(fb.reason);
            fb.cls             = fallback_class(fb.reason);
            if (fb.top)
                (fallback_is_exact(fb.reason) ? out.flat_top : out.planar_top) += fb.area;
            for (const PrintInstance &instance : object->instances()) {
                for (ExPolygon expoly : diag.fallback_shape) {
                    expoly.translate(instance.shift);
                    fb.shape.emplace_back(std::move(expoly));
                }
                const Vec3f shift(float(unscale<double>(instance.shift.x())), float(unscale<double>(instance.shift.y())), 0.f);
                for (std::vector<Vec3f> line : diag.fallback_hatch) {
                    for (Vec3f &p : line)
                        p += shift;
                    fb.hatch.emplace_back(std::move(line));
                }
            }
        }
        ExPolygons supported;
        for (const ExPolygons &layer : conformal.self_supported)
            expolygons_append(supported, layer);
        out.support_free = area(union_ex(supported)) * SCALING_FACTOR * SCALING_FACTOR;
        out.curved_band  = object->curved_band();
        out.warnings     = object_warnings(*object, out);
        if (out.stepped_top > 0.1 * out.curved_top && out.stepped_top > 1.)
            out.warnings.push_back({ { "conformal_stepped_top", warning_object(out.name), nlohmann::json{ { "area_mm2", std::round(out.stepped_top) } }.dump() },
                                     Slic3r::format(_u8L("%1%: %2% mm² of the curved top steps between layers "
                                                         "because they can't thicken enough at this layer height to "
                                                         "stay on one layer. Try a smaller layer height to print it "
                                                         "as one smooth surface!"),
                                                    out.name, int(std::round(out.stepped_top))) });
        if (index == 0 && !head_warning.empty())
            out.warnings.insert(out.warnings.begin(), CodedWarning{ { "head_profile_fallback", {}, nlohmann::json{ { "head", report.head } }.dump() }, head_warning });
    }
    if (report.empty())
        return report;
    const auto &report_clearance = result.clearance_report;
    report.verified     = report_clearance.ran;
    report.hits         = report_clearance.hit_moves;
    report.body_hits    = report_clearance.hit_body_moves;
    report.cone_grazes  = report_clearance.cone_graze_moves;
    report.machine_hits = report_clearance.machine_hit_moves;
    report.clearance_override = report_clearance.clearance_override;
    report.duct_grazes  = report_clearance.duct_graze_moves;
    report.max_contact  = report_clearance.max_contact;
    const size_t normal = size_t(PrintEstimatedStatistics::ETimeMode::Normal);
    report.time_s       = result.print_statistics.modes[normal].time;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves)
        if (result.state(move).nonplanar_patch_shell != 0xFF)
            report.curved_time_s += move.time[normal];
    return report;
}

const ReportPatch *NonplanarReport::find_patch(int id, const Point &pos) const
{
    const ReportPatch *best   = nullptr;
    double             best_d = std::numeric_limits<double>::max();
    for (const ReportPatch &patch : patches)
        if (patch.id == id)
            for (const BoundingBox &box : patch.boxes) {
                if (box.contains(pos))
                    return &patch;
                if (const double d = (box.center() - pos).cast<double>().squaredNorm(); d < best_d)
                    best_d = d, best = &patch;
            }
    return best;
}

std::string NonplanarReport::to_json(const std::map<size_t, std::string> &orientation) const
{
    using nlohmann::json;
    json j;
    j["version"] = 1;
    j["head"]    = head;
    j["clearance"] = verified ? json{ { "checked", true }, { "hits", hits }, { "body_hits", body_hits }, { "cone_graze_moves", cone_grazes }, { "machine_hits", machine_hits },
                                      { "max_contact_mm", max_contact }, { "duct_grazes", duct_grazes } } : json{ { "checked", false } };
    if (clearance_override) {
        j["clearance"]["clearance_override"] = true;
        j["clearance"]["duct_graze_moves"]   = duct_grazes;
    }
    j["time_s"]        = time_s;
    j["curved_time_s"] = curved_time_s;
    json objects_json  = json::array();
    for (size_t i = 0; i < objects.size(); ++i) {
        const ReportObject &o = objects[i];
        const auto  note      = orientation.find(o.model_object_id);
        json        jo{ { "name", o.name }, { "mode", o.mode }, { "top_patches", o.top_patches }, { "other_patches", o.other_patches },
                        { "bands", o.bands }, { "curved_top_mm2", o.curved_top }, { "planar_top_mm2", o.planar_top }, { "stepped_top_mm2", o.stepped_top },
                        { "flat_top_mm2", o.flat_top }, { "curved_share", o.curved_share() }, { "support_free_mm2", o.support_free },
                        { "orientation", note != orientation.end() ? note->second : o.orientation }, { "warnings", warning_messages(o.warnings) } };
        if (o.mode == "nonplanar_top") {
            jo["effective_cone_tolerance_mm"] = o.curved_cone_tolerance;
            jo["curved_surfaces_kept"] = o.curved_tops.kept;
            jo["curved_surfaces_candidates"] = o.curved_tops.candidates;
            jo["curved_extrusion_length_mm"] = o.curved_tops.extrusion_mm;
            jo["curved_footprint_mm2"] = o.curved_tops.footprint_mm2;
            jo["curved_top_area_fraction"] = o.curved_tops.area_fraction;
            jo["curved_top_area_denominator_mm2"] = o.curved_tops.upward_area_mm2;
            jo["curved_top_area_denominator"] = "sum of XY-projected areas of all upward-facing mesh facets";
            if (!o.curved_tops.planar_fallback_reason.empty()) {
                jo["planar_fallback_reason"] = o.curved_tops.planar_fallback_reason;
                jo["planar_fallback_code"]   = fallback_code(o.curved_tops.planar_limit);
            }
            jo["curved_max_angle_deg"] = o.curved_tops.max_angle_deg;
            jo["cap_requested"]        = o.cap_requested;
            jo["cap_used"]             = o.curved_tops.max_angle_deg;
            if (!o.curved_tops.planar_limit.empty()) jo["planar_limit"] = o.curved_tops.planar_limit;
            if (!o.curved_tops.access_by.empty()) jo["access_by"] = o.curved_tops.access_by;
            if (o.curved_band.second > o.curved_band.first)
                jo["curved_band"] = { { "from", o.curved_band.first }, { "to", o.curved_band.second }, { "layer_height", 0.2 } };
            if (o.curved_tops.reachable_slope_deg > 0.) jo["reachable_slope_deg"] = o.curved_tops.reachable_slope_deg;
        }
        if (o.conformal_rung >= 0) {
            jo["conformal_rung"]    = o.conformal_rung;
            jo["conformal_variant"] = o.conformal_variant;
            if (!o.planar_limit.empty()) {
                jo["planar_limit"]           = o.planar_limit;
                jo["planar_fallback_reason"] = o.planar_reason;
                jo["planar_fallback_code"]   = fallback_code(o.planar_limit);
            }
        }
        if (o.morph_hit_moves > 0) {
            jo["planar_limit"]           = o.planar_limit;
            jo["planar_fallback_reason"] = o.planar_reason;
            jo["planar_fallback_code"]   = fallback_code(o.planar_limit);
            jo["hit_moves"]              = o.morph_hit_moves;
        }
        if (o.slant) {
            const SlantChoice &c = *o.slant;
            jo["slant"] = json{ { "angle_deg", c.angle_deg }, { "direction_deg", c.direction_deg }, { "head_limit_deg", c.budget_deg },
                                { "start_mm", c.start_mm }, { "morph_layers", c.morph_layers }, { "angle_lowered_steps", c.lowered }, { "full_tilt_mm", c.full_tilt_mm },
                                { "morph_min_gap_mm", c.min_gap_mm }, { "morph_max_gap_mm", c.max_gap_mm },
                                { "stair_step_mm2", c.steps }, { "overhang_mm2", c.overhang },
                                { "planar_stair_step_mm2", c.planar_steps }, { "planar_overhang_mm2", c.planar_overhang },
                                { "requested_deg", c.requested_deg } };
            if (!c.limit.empty())
                jo["slant"]["limit"] = c.limit;
            if (!c.limit_reason.empty())
                jo["slant"]["limit_reason"] = c.limit_reason;
            if (!c.planar_limit.empty()) {
                jo["planar_limit"]           = c.planar_limit;
                jo["planar_fallback_reason"] = c.reason;
                jo["planar_fallback_code"]   = fallback_code(c.planar_limit);
            }
        }
        if (!o.oneshot.empty()) {
            jo["oneshot"] = json{ { "decision", o.oneshot }, { "student", o.oneshot_student }, { "polish_k", o.oneshot_polish_k } };
            if (o.oneshot_max_time_ratio_override > 0.)
                jo["oneshot"]["max_time_ratio_override"] = o.oneshot_max_time_ratio_override;
        }
        json patches_json = json::array();
        for (const ReportPatch &p : patches)
            if (p.object == i)
            {
                json jp{ { "id", p.id }, { "kind", patch_kind_name(p.kind) }, { "region", p.region }, { "band", p.band },
                         { "shells", p.shells }, { "area_mm2", p.area } };
                if (p.top) {
                    jp["pattern"] = p.pattern;
                    jp["ironing"] = p.ironing;
                    jp["explain"] = p.explain;
                }
                patches_json.push_back(std::move(jp));
            }
        json fallbacks_json = json::array();
        for (const ReportFallback &f : fallbacks)
            if (f.object == i)
                fallbacks_json.push_back({ { "reason", f.reason }, { "class", f.cls }, { "why", fallback_reason_text(f.reason) }, { "area_mm2", f.area },
                                           { "top", f.top }, { "patch", f.patch } });
        if (!o.layer_source.empty())
            jo["curved_layers"] = json{ { "source", o.layer_source == "on" ? "column" : o.layer_source }, { "bands", o.other_patches } };
        else if (!o.layer_source_refused.empty())
            jo["curved_layers"] = json{ { "refused", o.layer_source_refused } };
        jo["regions"]   = o.regions;
        jo["patches"]   = std::move(patches_json);
        jo["fallbacks"] = std::move(fallbacks_json);
        objects_json.push_back(std::move(jo));
    }
    j["objects"] = std::move(objects_json);
    return j.dump(1, ' ', false, json::error_handler_t::replace);
}

}
