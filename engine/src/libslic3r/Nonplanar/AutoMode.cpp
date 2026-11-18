#include "AutoMode.hpp"
#include <nlohmann/json.hpp>
#include "libslic3r/Nonplanar/Research.hpp"

#include "ConformalIroning.hpp"
#include "ConformalShells.hpp"
#include "CurvedOverhang.hpp"
#include "HeadEnvelope.hpp"
#include "HeightField.hpp"
#include "MaterialRules.hpp"
#include "Multiplanar.hpp"
#include "PatchTools.hpp"
#include "Repair.hpp"
#include "Report.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/libslic3r.h"

#include <Eigen/Geometry>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {

bool flexible_top(const PrintObject &object, const PrintRegionConfig &config)
{
    const size_t idx = size_t(std::max(0, config.top_surface_filament_id.value - 1));
    return road_material(object.print()->config().filament_type.get_at(idx)) == RoadMaterial::Flexible;
}

constexpr int    MaxShells      = 6;
constexpr double LargeArea      = 400.;
constexpr double MinCoherence   = 0.1;
constexpr double MaxRoundAspect = 1.5;
constexpr double MultiplanarThinShare  = 0.6;
constexpr double MultiplanarSteepShare = 0.55;
constexpr double MultiplanarMinGain    = 0.02;
constexpr double MultiplanarMinRmsGain = 0.10;
constexpr double MultiplanarMaxTimeRatio = 1.5;

bool is_auto(const PrintObjectConfig &object) { return object.nonplanar_mode.value == NonplanarMode::Auto; }
bool is_manual(const PrintObjectConfig &object)
{
    return object.nonplanar_mode.value == NonplanarMode::Manual || object.nonplanar_mode.value == NonplanarMode::Multiplanar;
}

double percentile(std::vector<double> &v, double q)
{
    if (v.empty())
        return 0.;
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

std::optional<NonplanarTopPattern> user_top_pattern(const PrintObject &object, const PrintRegionConfig &config)
{
    const PrintRegionConfig &base = object.print()->default_region_config();
    if (config.nonplanar_top_pattern.value != base.nonplanar_top_pattern.value)
        return config.nonplanar_top_pattern.value;
    if (config.top_surface_pattern.value == base.top_surface_pattern.value)
        return std::nullopt;
    switch (config.top_surface_pattern.value) {
    case ipConcentric:
    case ipSpiralInset:
    case ipArchimedeanChords:
    case ipOctagramSpiral: return NonplanarTopPattern::Concentric;
    default: return NonplanarTopPattern::Rectilinear;
    }
}

}

Mask auto_planar_nodes(const HeightField &surface, const Mask &candidate, double layer_height, double line_width, double max_slope)
{
    const int nx = surface.nx(), ny = surface.ny();
    Mask planar(candidate.size(), 0), seen(candidate.size(), 0), member(candidate.size(), 0);
    std::vector<Vec2d> gradients(candidate.size(), Vec2d::Constant(NAN));
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix)
            if (candidate[size_t(iy) * nx + ix])
                gradients[size_t(iy) * nx + ix] = surface.gradient(surface.node(ix, iy));
    std::vector<size_t> nodes, stack;
    auto point = [&](size_t i) {
        const Vec2d p = surface.node(int(i % size_t(nx)), int(i / size_t(nx)));
        return Vec3d(p.x(), p.y(), surface.data()[i]);
    };
    const double min_width = 4. * line_width;
    constexpr double CreaseJump = 0.4;
    for (size_t seed = 0; seed < candidate.size(); ++seed) {
        if (!candidate[seed] || seen[seed] || !gradients[seed].allFinite())
            continue;
        nodes.clear();
        stack.assign(1, seed);
        seen[seed] = 1;
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            nodes.push_back(i);
            const int ix = int(i % size_t(nx)), iy = int(i / size_t(nx));
            for (const auto &[dx, dy] : { std::pair{ 1, 0 }, std::pair{ -1, 0 }, std::pair{ 0, 1 }, std::pair{ 0, -1 } })
                if (const int jx = ix + dx, jy = iy + dy; jx >= 0 && jy >= 0 && jx < nx && jy < ny)
                    if (const size_t j = size_t(jy) * nx + jx; candidate[j] && !seen[j] && gradients[j].allFinite() &&
                        (gradients[i] - gradients[j]).norm() <= CreaseJump) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
        }
        if (nodes.size() < 3 || nodes.size() * surface.pitch() * surface.pitch() < min_width * min_width)
            continue;
        Vec3d mean = Vec3d::Zero();
        for (size_t i : nodes)
            mean += point(i);
        mean /= double(nodes.size());
        double xx = 0., xy = 0., yy = 0., xz = 0., yz = 0.;
        for (size_t i : nodes) {
            const Vec3d p = point(i) - mean;
            xx += p.x() * p.x(), xy += p.x() * p.y(), yy += p.y() * p.y();
            xz += p.x() * p.z(), yz += p.y() * p.z();
        }
        const double det = xx * yy - xy * xy;
        if (det <= 1e-12 * std::max(1., xx * yy))
            continue;
        const Vec2d gradient((xz * yy - yz * xy) / det, (yz * xx - xz * xy) / det);
        if (gradient.norm() <= 0.0175 || gradient.norm() > max_slope)
            continue;
        const double tolerance = 0.5 * layer_height * std::sqrt(1. + gradient.squaredNorm());
        const bool fits = std::all_of(nodes.begin(), nodes.end(), [&](size_t i) {
            const Vec3d p = point(i) - mean;
            return std::abs(p.z() - gradient.dot(p.head<2>())) < tolerance;
        });
        if (!fits)
            continue;
        for (size_t i : nodes)
            member[i] = 1;
        const bool wide = !offset_ex(mask_to_expolygons(member, surface), -float(scale_(0.5 * min_width))).empty();
        std::vector<double> normal_error;
        for (size_t i : nodes) {
            const int ix = int(i % size_t(nx)), iy = int(i / size_t(nx));
            if (ix > 0 && iy > 0 && ix + 1 < nx && iy + 1 < ny &&
                member[i - 1] && member[i + 1] && member[i - nx] && member[i + nx])
                normal_error.push_back((gradients[i] - gradient).norm());
        }
        for (size_t i : nodes)
            member[i] = 0;
        if (!wide || normal_error.size() < 3 || percentile(normal_error, 0.9) > 0.005)
            continue;
        for (size_t i : nodes)
            planar[i] = 1;
    }
    return planar;
}

bool auto_z_contouring(const PrintObject &object, const std::vector<double> &layers)
{
    static thread_local bool probing = false;
    if (probing || !is_auto(object.config()) || layers.size() < 4 || object.print()->config().spiral_mode.value ||
        object.print()->config().belt_printer.value || object.print()->config().nonplanar_kinematics.value != RotaryLayout::None ||
        !object.config().nonplanar_atom_toolpath.value.empty())
        return false;
    const double min_z = PrintRegionConfig::defaults().zaa_min_z.value;
    for (size_t i = 2; i + 1 < layers.size(); i += 2)
        if (layers[i + 1] - layers[i] < min_z - EPSILON)
            return false;
    double ground_offset = 0.;
    const TriangleMesh mesh = object.object_frame_mesh(ground_offset);
    const double bottom = mesh.bounding_box().min.z();
    bool has_top = false;
    for (const auto &face : mesh.its.indices) {
        const Vec3d a = mesh.its.vertices[face[0]].cast<double>();
        const Vec3d b = mesh.its.vertices[face[1]].cast<double>();
        const Vec3d c = mesh.its.vertices[face[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        const double length = n.norm();
        if (length <= 1e-12)
            continue;
        const double nz = std::abs(n.z()) / length;
        if (nz > 1e-4 && n.head<2>().norm() / length > 1e-6)
            return false;
        has_top |= n.z() > 0. && nz > 0.5 && a.z() - bottom > layers[1] + EPSILON;
    }
    if (!has_top)
        return false;
    struct ProbeScope {
        bool &active;
        explicit ProbeScope(bool &active) : active(active) { active = true; }
        ~ProbeScope() { active = false; }
    } scope(probing);
    DynamicPrintConfig config = object.print()->full_print_config();
    config.apply(object.config(), true);
    config.apply(object.printing_region(0).config(), true);
    Model model;
    ModelObject *copy = model.add_object(*object.model_object());
    copy->clear_instances();
    copy->add_instance(*object.instances().front().model_instance);
    Print probe;
    probe.apply(model, config);
    probe.set_status_callback([&object](const PrintBase::SlicingStatus &) { throw_if_canceled(object); });
    try {
        probe.process();
    } catch (const std::exception &ex) {
        throw_if_canceled(object);
        BOOST_LOG_TRIVIAL(warning) << "Auto Z contouring keeps the shell schedule: " << ex.what();
        return false;
    }
    for (const PrintObject *part : probe.objects())
        for (const ConformalPatch &patch : part->conformal().patches)
            if (patch.kind == PatchKind::Top || patch.kind == PatchKind::Volume ||
                (patch.kind == PatchKind::Overhang && !offset_ex(patch.footprint, -float(scale_(BoreHalfSpan))).empty()))
                return false;
    return true;
}

bool top_shells_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object)
{
    return is_auto(object) || (is_manual(object) && config.nonplanar_top_shells.value != 0);
}

double auto_curved_underside_slope() { return std::tan(Geometry::deg2rad(10.)); }

bool bottom_shells_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object)
{
    return is_auto(object) ? object.enable_support.value : is_manual(object) && config.nonplanar_bottom_shells.value != 0;
}

double full_conformal_thickness(const PrintRegionConfig &config, const PrintObject &object)
{
    const double set = config.nonplanar_full_conformal_max_thickness.value;
    if (!is_auto(object.config()))
        return is_manual(object.config()) ? set : 0.;
    size_t open_edges = 0, facets = 0;
    for (const ModelVolume *volume : object.model_object()->volumes)
        if (volume->is_model_part()) {
            open_edges += size_t(std::max(0, volume->mesh().stats().open_edges));
            facets += volume->mesh().facets_count();
        }
    const bool open = open_edges > facets / 100;
    if (!open)
        return 0.;
    if (set > 0.)
        return set;
    const double h = config.nonplanar_shell_layer_height.value > 0. ? config.nonplanar_shell_layer_height.value : object.config().layer_height.value;
    return 15. * h;
}

bool overhangs_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object)
{
    return is_auto(object) || ((is_manual(object) || curved_tops_mode(object.nonplanar_mode.value)) && config.nonplanar_overhangs.value);
}

bool support_free(const PrintObjectConfig &object) { return is_auto(object); }

bool bridges_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object)
{
    return is_auto(object) || ((is_manual(object) || curved_tops_mode(object.nonplanar_mode.value)) && config.nonplanar_bridges.value);
}

double infill_depth(const PrintRegionConfig &config, const PrintObject &object)
{
    if (!is_auto(object.config()))
        return is_manual(object.config()) ? config.nonplanar_infill_depth.value : 0.;
    return 0.;
}

int planar_top_layers(const PrintRegionConfig &config, double shell_height)
{
    return std::max(config.top_shell_layers.value, int(std::ceil(config.top_shell_thickness.value / shell_height - EPSILON)));
}

NonplanarFallback effective_fallback(const PrintRegionConfig &config, const PrintObjectConfig &object)
{
    return is_auto(object) ? NonplanarFallback::None : config.nonplanar_fallback.value;
}

bool zaa_fallback(const PrintRegionConfig &config, const PrintObjectConfig &object)
{
    return top_shells_enabled(config, object) && effective_fallback(config, object) == NonplanarFallback::ZAA;
}

bool spiral_top_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object)
{
    return !is_auto(object) && config.nonplanar_spiral_top.value;
}

NonplanarCollisionCheck effective_collision_check(const Print &print)
{
    const NonplanarCollisionCheck set = print.config().nonplanar_collision_check.value;
    for (const PrintObject *object : print.objects())
        if (is_auto(object->config()))
            return std::max(set, NonplanarCollisionCheck::Warn);
    return set;
}

bool auto_multiplanar_candidate(double steep_top_share, double thin_top_share)
{
    return steep_top_share >= MultiplanarSteepShare || thin_top_share >= MultiplanarThinShare;
}

bool auto_multiplanar_confirmed(double chunks_rms, double planar_rms, double chunks_time, double planar_time)
{
    return std::isfinite(chunks_rms) && std::isfinite(planar_rms) && std::isfinite(planar_time) && planar_time > 0. &&
           chunks_rms <= (1. - MultiplanarMinRmsGain) * planar_rms && chunks_time < multiplanar_time_limit(planar_time);
}

double multiplanar_time_limit(double planar_time) { return MultiplanarMaxTimeRatio * planar_time; }

bool auto_multiplanar_chunks(const MultiplanarPlan &plan)
{
    return plan.chunks.size() > 1 && plan.step_area < (1. - MultiplanarMinGain) * plan.planar_step_area;
}

AutoObjectSettings auto_object_settings(const PrintObject &object, const PrintRegion &region, double shell_height)
{
    const PrintConfig &print_config = object.print()->config();
    const double       d            = smallest_nozzle(print_config);
    const double       w            = region.flow(object, frTopSolidInfill, shell_height).width();
    const HeadEnvelope head         = resolve_head_envelope(print_config);

    AutoObjectSettings s;
    s.max_error = std::clamp(0.25 * shell_height, 0.02, 0.1);
    const double area_w = w * std::min(1., TunedNozzle / d);
    s.min_area  = (10. * area_w) * (10. * area_w);
    const double gantry = head.gantry_clearance();
    s.max_patch_height  = std::isfinite(gantry) ? std::max(0., gantry - head.margin) : 0.;
    if (print_config.nonplanar_kinematics.value != RotaryLayout::None) {
        const double reach = std::max(shell_height, head_band_height(head) - head.margin);
        s.max_patch_height = s.max_patch_height > 0. ? std::min(s.max_patch_height, reach) : reach;
    }
    s.clip_shells      = AutoMinShells;
    s.bottom_shells = std::clamp(int(std::ceil(d / shell_height - 0.15)), AutoMinShells, MaxShells);
    return s;
}

void choose_patch_settings(const PrintObject &object, const HeadEnvelope &head, double max_error, double first_layer_top,
                           ConformalPatch &patch, const ExPolygons *steep_faces)
{
    const PrintRegionConfig &config = object.printing_region(size_t(patch.region_id)).config();
    if (!is_auto(object.config()) && config.nonplanar_top_shells.value >= 0)
        return;

    const HeightField &S     = patch.surface;
    const double       pitch = S.pitch();
    const double       h_c   = patch.shell_height;
    const double       d     = smallest_nozzle(object.print()->config());

    AutoPatchSettings a;
    a.max_error  = max_error;
    a.shell_area = area(diff_ex(patch.footprint, patch.wall_band)) * SCALING_FACTOR * SCALING_FACTOR;

    std::vector<double> slopes, curvatures;
    Mask shell_node = rasterize(diff_ex(patch.footprint, patch.wall_band), S);
    for (int iy = 0; iy + 1 < S.ny(); ++iy)
        for (int ix = 0; ix + 1 < S.nx(); ++ix)
            if (spans_wall(S, S.node(ix, iy) + Vec2d(0.5 * pitch, 0.5 * pitch)))
                for (int jy = iy; jy <= iy + 1; ++jy)
                    for (int jx = ix; jx <= ix + 1; ++jx)
                        shell_node[size_t(jy) * S.nx() + jx] = 0;
    const Mask          steep_node = steep_faces != nullptr && !steep_faces->empty() ? rasterize(*steep_faces, S) : Mask();
    std::vector<double> count_slopes;
    double jxx = 0., jxy = 0., jyy = 0.;
    double n = 0., mx = 0., my = 0., mxx = 0., mxy = 0., myy = 0.;
    float  z_lo = std::numeric_limits<float>::infinity();
    const int step = 2;
    auto      z_at = [&S](int ix, int iy) {
        return ix >= 0 && iy >= 0 && ix < S.nx() && iy < S.ny() ? S.at(ix, iy) : HeightField::NoData;
    };
    for (int iy = 0; iy < S.ny(); ++iy)
        for (int ix = 0; ix < S.nx(); ++ix) {
            const float z = S.at(ix, iy);
            if (std::isnan(z))
                continue;
            z_lo = std::min(z_lo, z);
            const Vec2d p = S.node(ix, iy);
            n += 1., mx += p.x(), my += p.y(), mxx += p.x() * p.x(), mxy += p.x() * p.y(), myy += p.y() * p.y();
            const Vec2d g = S.gradient(p);
            if (std::isnan(g.x()) || std::isnan(g.y()))
                continue;
            if (shell_node[size_t(iy) * S.nx() + ix]) {
                slopes.emplace_back(std::atan(g.norm()));
                if (steep_node.empty() || !steep_node[size_t(iy) * S.nx() + ix])
                    count_slopes.emplace_back(slopes.back());
            }
            jxx += g.x() * g.x(), jxy += g.x() * g.y(), jyy += g.y() * g.y();
            const float xm = z_at(ix - step, iy), xp = z_at(ix + step, iy), ym = z_at(ix, iy - step), yp = z_at(ix, iy + step);
            const float a0 = z_at(ix - step, iy - step), a1 = z_at(ix + step, iy - step), a2 = z_at(ix - step, iy + step),
                        a3 = z_at(ix + step, iy + step);
            if (std::isnan(xm + xp + ym + yp + a0 + a1 + a2 + a3))
                continue;
            const double hh  = (step * pitch) * (step * pitch);
            const double fxx = (xp - 2. * z + xm) / hh, fyy = (yp - 2. * z + ym) / hh, fxy = (a3 - a1 - a2 + a0) / (4. * hh);
            const double k = (std::abs(0.5 * (fxx + fyy)) + std::hypot(0.5 * (fxx - fyy), fxy)) / std::pow(1. + g.squaredNorm(), 1.5);
            curvatures.emplace_back(k);
        }
    a.slope90     = percentile(slopes, 0.9);
    a.curvature95 = percentile(curvatures, 0.95);
    const double curvature80 = percentile(curvatures, 0.8);
    a.curvature80 = curvature80;
    const double trace = jxx + jyy;
    a.coherence = trace > 1e-9 ? std::hypot(jxx - jyy, 2. * jxy) / trace : 0.;
    if (n > 0.) {
        const double cxx = mxx / n - (mx / n) * (mx / n), cyy = myy / n - (my / n) * (my / n), cxy = mxy / n - (mx / n) * (my / n);
        const double mid = 0.5 * (cxx + cyy), rad = std::hypot(0.5 * (cxx - cyy), cxy);
        a.aspect = mid - rad > 1e-12 ? std::sqrt((mid + rad) / (mid - rad)) : std::numeric_limits<double>::infinity();
    }

    const double scale   = bead_scale(object.print()->config());
    const bool   large   = a.shell_area >= LargeArea * scale * scale;
    const double T       = (large ? 1.5 : 1.0) * d;
    const int    n_depth = std::isfinite(z_lo) ? int(std::floor((z_lo - 0.5 * h_c - first_layer_top) / h_c + 1e-6)) : AutoMinShells;
    const double shell_slope = steep_node.empty() || count_slopes.empty() ? a.slope90 : percentile(count_slopes, 0.9);
    a.shells = std::clamp(int(std::ceil(T / (h_c * std::cos(shell_slope)) - 0.15)), AutoMinShells, MaxShells);
    a.shells = std::max(AutoMinShells, std::min({ a.shells, n_depth, planar_top_layers(config, h_c) }));
    if (const char *v = research_switch("NP_CT1_SHELLS"); v != nullptr && std::atoi(v) > 0)
        a.shells = std::clamp(std::atoi(v), 1, MaxShells);
    patch.shells = a.shells;
    if (!is_auto(object.config()))
        return;

    const bool banded = effective_fallback(config, object.config()) == NonplanarFallback::None;
    a.pattern = banded || a.coherence >= MinCoherence ? NonplanarTopPattern::Contour :
                a.aspect <= MaxRoundAspect            ? NonplanarTopPattern::Concentric :
                                                        NonplanarTopPattern::Rectilinear;
    if (const std::optional<NonplanarTopPattern> user = user_top_pattern(object, config))
        a.pattern = *user;
    const double r = head.tip_flat_radius;
    const bool ironing_off = config.ironing_type.value == IroningType::NoIroning &&
                             object.print()->default_region_config().ironing_type.value != IroningType::NoIroning;
    a.ironing = !ironing_off && conformal_ironing_enabled(config) && !flexible_top(object, config) &&
                0.5 * r * r * curvature80 <= 0.25 * h_c &&
                object.print()->config().nonplanar_kinematics.value == RotaryLayout::None;

    patch.automatic = std::make_shared<const AutoPatchSettings>(a);
    BOOST_LOG_TRIVIAL(info) << "Non-planar auto: patch " << patch.id << " area " << a.shell_area << " mm2, slope90 "
                            << Geometry::rad2deg(a.slope90) << " deg, curvature95 " << a.curvature95 << ", coherence "
                            << a.coherence << ", aspect " << a.aspect << ": " << auto_comment(patch);
}

NonplanarTopPattern top_pattern(const ConformalPatch &patch, const PrintRegionConfig &config)
{
    return patch.automatic ? patch.automatic->pattern : config.nonplanar_top_pattern.value;
}

bool ironing_enabled(const ConformalPatch &patch, const PrintRegionConfig &config)
{
    return patch.automatic ? patch.automatic->ironing : conformal_ironing_enabled(config);
}

std::string auto_comment(const ConformalPatch &patch)
{
    if (!patch.automatic)
        return {};
    const AutoPatchSettings &a = *patch.automatic;
    const char *pattern = a.pattern == NonplanarTopPattern::Contour ? "contour" :
                          a.pattern == NonplanarTopPattern::Concentric ? "concentric" : "rectilinear";
    char buf[160];
    std::snprintf(buf, sizeof(buf), "; nonplanar auto: patch %d shells=%d pattern=%s err=%.3g ironing=%d\n", patch.id, patch.shells,
                  pattern, a.max_error, int(a.ironing));
    return buf;
}

namespace {

struct TopPatchDecision
{
    bool                automatic = false;
    NonplanarMode       mode      = NonplanarMode::Off;
    NonplanarTopPattern pattern   = NonplanarTopPattern::Contour;
    const char         *pattern_rule = "region setting";
    bool                banded    = false;
    double              head_reach = 0.;
    double              object_cap = 0., material_cap = 0., repair_cap = 0., cap = 0.;
    double              pitch = 0., tolerance = 0.;
    bool                requested = false;
    bool                region_irons = false;
    double              deviation = 0.;
    bool                flexible  = false;
};

TopPatchDecision top_patch_decision(const PrintObject &object, const ConformalPatch &patch)
{
    TopPatchDecision d;
    const auto &region = object.printing_region(size_t(patch.region_id));
    const auto &config = region.config();
    const auto head = resolve_head_envelope(object.print()->config());
    d.mode      = object.config().nonplanar_mode.value;
    d.automatic = d.mode == NonplanarMode::Auto;
    d.pattern   = top_pattern(patch, config);
    if (patch.automatic) {
        const auto &a = *patch.automatic;
        d.pattern_rule = effective_fallback(config, object.config()) == NonplanarFallback::None ? "auto: Z-band schedule" :
                         a.coherence >= MinCoherence ? "auto: dominant gradient" :
                         a.aspect <= MaxRoundAspect ? "auto: round footprint" : "auto: elongated footprint";
    }
    d.banded = patch.band >= 0;
    if (d.banded) {
        double cap = d.automatic ? auto_object_settings(object, region, patch.shell_height).max_patch_height :
                                   effective_patch_height_cap(object);
        d.object_cap = cap;
        cap = multi_material_patch_height(object, head, cap);
        d.material_cap = cap;
        if (const auto *repair = object.nonplanar_repair())
            for (const auto &band : repair->order_bands)
                cap = std::min(cap > 0. ? cap : std::numeric_limits<double>::infinity(), band.height);
        d.repair_cap = cap;
        if (const double rotary = rotary_band_reach(object, head, patch.shell_height); rotary > 0.)
            cap = cap > 0. ? std::min(cap, rotary) : rotary;
        d.cap = cap;
        band_schedule(head, patch.shell_height, cap, d.pitch, d.tolerance);
        d.head_reach = head_band_height(head);
    }
    d.requested    = ironing_enabled(patch, config);
    d.region_irons = conformal_ironing_enabled(config);
    if (patch.automatic) {
        const double r = head.tip_flat_radius;
        d.deviation = 0.5 * r * r * patch.automatic->curvature80;
    }
    d.flexible = flexible_top(object, config);
    return d;
}

const char *pattern_key(NonplanarTopPattern pattern)
{
    return pattern == NonplanarTopPattern::Contour ? "contour" : pattern == NonplanarTopPattern::Concentric ? "concentric" : "rectilinear";
}

}

std::string explain_top_patch(const PrintObject &object, const ConformalPatch &patch)
{
    if (patch.kind != PatchKind::Top)
        return {};
    const TopPatchDecision d = top_patch_decision(object, patch);
    std::ostringstream out;
    out << "patch " << patch.id << " mode=" << (d.automatic ? "auto" : d.mode == NonplanarMode::Multiplanar ? "multiplanar" : "manual")
        << " (object setting) pattern=" << pattern_key(d.pattern) << " (" << d.pattern_rule;
    out << ") shells=" << patch.shells << " shell_height=" << patch.shell_height << " mm";
    if (d.banded) {
        out << " band_height=" << d.pitch + d.tolerance << " mm (head reach=" << d.head_reach;
        if (d.automatic)
            out << "; auto gantry cap=" << d.object_cap;
        else if (d.object_cap > 0.)
            out << "; user cap=" << d.object_cap;
        if (d.material_cap != d.object_cap)
            out << "; multi-material cap=" << d.material_cap;
        if (d.repair_cap != d.material_cap)
            out << "; repair cap=" << d.repair_cap;
        if (d.cap != d.repair_cap)
            out << "; rotary tilt cap=" << d.cap;
        out << ") band_pitch=" << d.pitch << " mm band=" << patch.band;
    } else
        out << " band_height=" << patch.z_max - patch.z_min << " mm (unbanded patch relief)";
    out << " ironing=" << (patch.ironing.entities.empty() ? "off" : "on") << " (";
    if (patch.automatic) {
        const auto &a = *patch.automatic;
        if (!d.region_irons)
            out << "auto: region ironing off";
        else
            out << "auto: land deviation " << d.deviation << (d.deviation <= 0.25 * patch.shell_height ? " <= " : " > ")
                << 0.25 * patch.shell_height << " mm (h_c/4)";
        out << "; area=" << a.shell_area << " mm2 slope90=" << Geometry::rad2deg(a.slope90)
            << " deg curvature80=" << a.curvature80 << "/mm";
    } else
        out << (d.requested ? "region enabled" : "region disabled");
    if (d.requested && patch.ironing.entities.empty())
        out << (d.flexible ? "; flexible filament: curved tops are not ironed" :
                             "; no paths survived surface, wall, band or repair clipping");
    out << ")\n";
    return out.str();
}

std::string describe_top_patch(const PrintObject &object, const ConformalPatch &patch)
{
    if (patch.kind != PatchKind::Top)
        return {};
    const TopPatchDecision d = top_patch_decision(object, patch);
    const std::string pattern = d.pattern == NonplanarTopPattern::Contour    ? _u8L("Contour") :
                                d.pattern == NonplanarTopPattern::Concentric ? _u8L("Concentric") : _u8L("Rectilinear");
    const std::string rule = !patch.automatic                                        ? _u8L("set in the process") :
                             std::strcmp(d.pattern_rule, "auto: Z-band schedule") == 0   ? _u8L("the only pattern continuous across height bands") :
                             std::strcmp(d.pattern_rule, "auto: dominant gradient") == 0 ? _u8L("the surface slopes mostly one way") :
                             std::strcmp(d.pattern_rule, "auto: round footprint") == 0   ? _u8L("the area is round") :
                                                                                           _u8L("the area is long and narrow");
    std::string out = format(_u8L("%1% lines: %2%."), pattern, rule) + "\n" +
                      format(_u8L("%1% shells, %2$.2f mm each."), patch.shells, patch.shell_height) + "\n";
    if (d.banded) {
        const std::string limit = d.cap != d.repair_cap                         ? _u8L("the rotary tilt") :
                                  d.repair_cap != d.material_cap                ? _u8L("the clearance repair") :
                                  d.material_cap != d.object_cap                ? _u8L("a filament change") :
                                  d.object_cap > 0. && d.object_cap < d.head_reach ? (d.automatic ? _u8L("the gantry") : _u8L("the maximum curved band height")) :
                                                                                  _u8L("how far the head reaches");
        out += format(_u8L("Height band %1% (%2$.2f mm tall), set by %3%."), patch.band + 1, d.pitch + d.tolerance, limit) + "\n";
    } else
        out += format(_u8L("Single pass over %1$.2f mm of height change."), patch.z_max - patch.z_min) + "\n";
    const bool ironed = !patch.ironing.entities.empty();
    if (ironed)
        out += patch.automatic ? _u8L("Ironed! Gentle enough for the flat nozzle tip.") : _u8L("Ironed (enabled in print settings).");
    else if (d.requested)
        out += d.flexible ? _u8L("Skipped ironing — flexible filament doesn't work with it.") : _u8L("Skipped ironing — this spot is too narrow for ironing passes.");
    else if (patch.automatic && d.region_irons)
        out += _u8L("Skipped ironing — the curve is a bit too steep for the flat nozzle tip.");
    else
        out += _u8L("Skipped ironing (it's turned off in print settings).");
    return out;
}

std::vector<CodedWarning> object_warnings(const PrintObject &object, const ReportObject &report)
{
    std::vector<CodedWarning> warnings;
    if (report.planar_top > 1. && report.curved_share() < 0.9) {
        std::map<std::string, double> by_reason;
        for (const PatchDiagnostics &diag : object.conformal().diagnostics)
            if (diag.fallback_area > 0. && !fallback_is_exact(diag.fallback_reason) && diag.fallback_reason != "bottom" &&
                diag.fallback_reason.rfind("overhang", 0) != 0)
                by_reason[diag.fallback_reason] += diag.fallback_area;
        const auto main = std::max_element(by_reason.begin(), by_reason.end(),
                                           [](const auto &a, const auto &b) { return a.second < b.second; });
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f %% (%.0f mm²)", 100. * (1. - report.curved_share()), report.planar_top);
        warnings.push_back({ { "curved_top_flat_share", warning_object(report.name),
                               nlohmann::json{ { "percent", std::round(100. * (1. - report.curved_share())) }, { "area_mm2", std::round(report.planar_top) },
                                               { "reason", main == by_reason.end() ? std::string() : main->first } }.dump() },
                             (boost::format(_u8L("%1%: %2% of the curved top is printing as flat layers (mostly because %3%).")) % report.name % buf %
                              (main == by_reason.end() ? std::string("-") : I18N::translate(fallback_reason_text(main->first))))
                                 .str() });
    }
    return warnings;
}

}
