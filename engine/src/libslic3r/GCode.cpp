#include <atomic>
#include "libslic3r/Nonplanar/Research.hpp"
#include <fstream>
#include "ArcFitter.hpp"
#include "BoundingBox.hpp"
#include "Circle.hpp"
#include "Config.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "GCode/ConformalEmit.hpp"
#include "GCode/ClearanceRepair.hpp"
#include "Nonplanar/NativePrint.hpp"
#include "Nonplanar/CurvedTopsOrder.hpp"
#include "Nonplanar/CurvedTopsSurface.hpp"
#include "Nonplanar/Repair.hpp"
#include "Nonplanar/Wave.hpp"
#include "GCode/ClearanceVerifier.hpp"
#include "GCode/ToolKinematics.hpp"
#include "Nonplanar/HeadEnvelope.hpp"
#include "Nonplanar/Multiplanar.hpp"
#include "Nonplanar/AutoMode.hpp"
#include "Nonplanar/HeightField.hpp"
#include "Nonplanar/MaterialRules.hpp"
#include "Nonplanar/ConformalBottom.hpp"
#include "Nonplanar/ConformalShells.hpp"
#include "Nonplanar/FieldChart.hpp"
#include "Nonplanar/OrientationField.hpp"
#include "Nonplanar/PatchTools.hpp"
#include "Flow.hpp"
#include "Nonplanar/BeadModel.hpp"
#include "Nonplanar/SpiralTop.hpp"
#include "Nonplanar/PatchTools.hpp"
#include "Nonplanar/Validate.hpp"
#include "Nonplanar/ShellSeams.hpp"
#include "GCode/ThumbnailData.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/PressureEqualizer.hpp"
#include "GCode/SmallAreaInfillFlowCompensator.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/AdaptivePAProcessor.hpp"
#include "CustomGCode.hpp"
#include "GCode/TimelapsePosPicker.hpp"
#include "ExPolygon.hpp"
#include "GCode/WipePathHelpers.hpp"
#include "GCodeReader.hpp"
#include "GCodeWriter.hpp"
#include "Point.hpp"
#include "Line.hpp"
#include "ObjectID.hpp"
#include "Layer.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "PrintBase.hpp"
#include "ConnectedBodies.hpp"
#include "PrintConfig.hpp"
#include "enum_bitmask.hpp"
#include "libslic3r.h"
#include "I18N.hpp"
#include "IMEXHelpers.hpp"
#include "GCode.hpp"
#include "Feature/FuzzySkin/FuzzySkin.hpp"
#include "Nonplanar/OneShotField.hpp"
#include <cstdio>
#include "Exception.hpp"
#include "LifecycleEvents.hpp"
#include "ExtrusionEntity.hpp"
#include "EdgeGrid.hpp"
#include "BeltTransform.hpp"
#include "Geometry.hpp"
#include "Geometry/ConvexHull.hpp"
#include "GCode/PrintExtents.hpp"
#include "GCode/Thumbnails.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTower2.hpp"
#include "ShortestPath.hpp"
#include "GCode/OrderingStrategies.hpp"
#include "Print.hpp"
#include "BuildLabel.hpp"
#include "Utils.hpp"
#include "HeapTrim.hpp"
#include "ClipperUtils.hpp"
#include "LocalesUtils.hpp"
#include "libslic3r/format.hpp"
#include "Time.hpp"
#include "GCode/ExtrusionProcessor.hpp"
#include "GCode/ConformalCooling.hpp"
#include "Feature/FuzzySkin/FuzzySkin.hpp"
#include "PerimeterGenerator.hpp"
#include <Eigen/Geometry>
#include <Shiny/ShinyMacros.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <deque>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <math.h>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <set>
#include <optional>
#include <stdlib.h>
#include <string>
#include <system_error>
#include <unordered_set>
#include <type_traits>
#include <utility>
#include <string_view>
#include <sstream>

#include <regex>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>

#include "SVG.hpp"

#include <tbb/parallel_for.h>
#include <vector>
#include "calib.hpp"
#include "libslic3r_version.h"
#include "GCode/BeltKinematics.hpp"
#if ! defined(TBB_VERSION_MAJOR)
    #include <tbb/version.h>
#endif
#if ! defined(TBB_VERSION_MAJOR)
    static_assert(false, "TBB_VERSION_MAJOR not defined");
#endif
#if TBB_VERSION_MAJOR >= 2021
    #include <tbb/parallel_pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter_mode;
#else
    #include <tbb/pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter;
#endif

#include <Shiny/Shiny.h>
#include <stdio.h>


using namespace std::literals::string_view_literals;

#if 0
#define DEBUG
#define _DEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include "AABBTreeLines.hpp"
#include "Extruder.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "FilamentMixer.hpp"
#include "Format/STEP.hpp"
#include "Model.hpp"
#include "MultiNozzleUtils.hpp"
#include "Slicing.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {

struct NonplanarEmissionKey {
    bool  is_nonplanar = false;
    float shell_offset = -std::numeric_limits<float>::infinity();
};

static void collect_nonplanar_emission_key(const ExtrusionEntity &entity, NonplanarEmissionKey &key)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (path->nonplanar_source) {
            key.is_nonplanar = true;
            key.shell_offset = std::max(key.shell_offset, path->distance_to_top);
        }
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            collect_nonplanar_emission_key(path, key);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            collect_nonplanar_emission_key(path, key);
    } else if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_nonplanar_emission_key(*child, key);
    }
}

static NonplanarEmissionKey nonplanar_emission_key(const ExtrusionEntity &entity)
{
    NonplanarEmissionKey key;
    collect_nonplanar_emission_key(entity, key);
    return key;
}

static bool nonplanar_before_planar(const ExtrusionEntity &left, const ExtrusionEntity &right)
{
    const NonplanarEmissionKey a = nonplanar_emission_key(left);
    const NonplanarEmissionKey b = nonplanar_emission_key(right);
    if (a.is_nonplanar != b.is_nonplanar)
        return a.is_nonplanar;
    if (a.is_nonplanar && std::abs(a.shell_offset - b.shell_offset) > EPSILON)
        return a.shell_offset > b.shell_offset;
    return false;
}

static bool same_nonplanar_emission_group(const ExtrusionEntity &left, const ExtrusionEntity &right)
{
    const NonplanarEmissionKey a = nonplanar_emission_key(left);
    const NonplanarEmissionKey b = nonplanar_emission_key(right);
    return a.is_nonplanar == b.is_nonplanar &&
        (!a.is_nonplanar || std::abs(a.shell_offset - b.shell_offset) <= EPSILON);
}

static bool has_nonplanar_source(const ExtrusionEntity &entity)
{
    return nonplanar_emission_key(entity).is_nonplanar;
}

static void compensate_nonplanar_slope(Polyline3 &polyline, double raise, double lower, double tip_radius)
{
    if (polyline.points.size() < 2)
        return;
    {
        Points3 dense { polyline.points.front() };
        for (size_t i = 1; i < polyline.points.size(); ++ i) {
            const Point3 &a = polyline.points[i - 1], &b = polyline.points[i];
            const double  length = std::hypot(unscale<double>(b.x() - a.x()), unscale<double>(b.y() - a.y()));
            std::vector<double> ts;
            for (double d : { 0.25, 0.5, 1.0 })
                if (length > 4. * d) {
                    ts.push_back(d / length);
                    ts.push_back(1. - d / length);
                }
            std::sort(ts.begin(), ts.end());
            for (double t : ts)
                dense.emplace_back(Point3((a.cast<double>() + t * (b - a).cast<double>()).cast<coord_t>()));
            dense.push_back(b);
        }
        polyline.points = std::move(dense);
    }
    Points3 &pts = polyline.points;
    const size_t n = pts.size();
    std::vector<double> dxy(n - 1), dz(n - 1), seg(n - 1, 0.);
    for (size_t i = 0; i + 1 < n; ++ i) {
        dxy[i] = std::hypot(unscale<double>(pts[i + 1].x() - pts[i].x()), unscale<double>(pts[i + 1].y() - pts[i].y()));
        dz[i]  = unscale<double>(pts[i + 1].z() - pts[i].z());
        if (dxy[i] > 1e-3) {
            const double slope = dz[i] / dxy[i];
            seg[i] = slope < 0. ? raise * tip_radius * (- slope) : - lower * tip_radius * slope;
        }
    }
    std::vector<double> off(n);
    off.front() = seg.front();
    off.back()  = seg.back();
    for (size_t i = 1; i + 1 < n; ++ i)
        off[i] = dxy[i - 1] + dxy[i] > 0. ? (dxy[i - 1] * seg[i - 1] + dxy[i] * seg[i]) / (dxy[i - 1] + dxy[i]) : 0.;
    const double max_gradient = std::tan(11.5 * M_PI / 180.);
    const double share = raise > 0. && lower > 0. ? 0.5 : 1.;
    std::vector<double> lo(n - 1), hi(n - 1);
    for (size_t i = 0; i + 1 < n; ++ i) {
        const double allowed = std::max(max_gradient * dxy[i], std::abs(dz[i]));
        lo[i] = share * (- allowed - dz[i]);
        hi[i] = share * (  allowed - dz[i]);
    }
    auto envelope = [n](const std::vector<double> &target, const std::vector<double> &l, const std::vector<double> &h) {
        std::vector<double> e(target);
        for (size_t i = 1; i < n; ++ i)
            e[i] = std::max(e[i], e[i - 1] + l[i - 1]);
        for (size_t i = n - 1; i > 0; -- i)
            e[i - 1] = std::max(e[i - 1], e[i] - h[i - 1]);
        return e;
    };
    std::vector<double> up(n), down(n), neg_lo(n - 1), neg_hi(n - 1);
    for (size_t i = 0; i < n; ++ i) {
        up[i]   = std::max(off[i], 0.);
        down[i] = std::max(- off[i], 0.);
    }
    for (size_t i = 0; i + 1 < n; ++ i) {
        neg_lo[i] = - hi[i];
        neg_hi[i] = - lo[i];
    }
    auto end_cap = [n](std::vector<double> e, const std::vector<double> &l, const std::vector<double> &h) {
        std::vector<double> from_start(n), from_end(n);
        from_start.front() = 0.;
        for (size_t i = 1; i < n; ++ i)
            from_start[i] = from_start[i - 1] + h[i - 1];
        from_end.back() = 0.;
        for (size_t i = n - 1; i > 0; -- i)
            from_end[i - 1] = from_end[i] - l[i - 1];
        for (size_t i = 0; i < n; ++ i)
            e[i] = std::max(0., std::min({ e[i], from_start[i], from_end[i] }));
        return e;
    };
    const std::vector<double> raised  = end_cap(envelope(up, lo, hi), lo, hi);
    const std::vector<double> lowered = end_cap(envelope(down, neg_lo, neg_hi), neg_lo, neg_hi);
    for (size_t i = 0; i < n; ++ i)
        off[i] = raised[i] - lowered[i];
    for (size_t i = 0; i < n; ++ i)
        pts[i].z() += coord_t(std::lround(scale_(off[i])));
}

static bool path_has_absolute_z(const ExtrusionPath &path)
{
    return path.nonplanar_source ||
        (! path.z_contoured && ! path.polyline.points.empty() && path.polyline.points.front().z() > 0);
}

struct NonplanarIslandKey {
    bool   has_nonplanar = false;
    float  shell_offset  = -std::numeric_limits<float>::infinity();
    size_t source_layer  = std::numeric_limits<size_t>::max();
};

static void collect_nonplanar_island_key(const ExtrusionEntity &entity, NonplanarIslandKey &key)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        if (path->nonplanar_source) {
            key.has_nonplanar = true;
            key.shell_offset = std::max(key.shell_offset, path->distance_to_top);
            key.source_layer = std::min(key.source_layer, path->source_layer_id);
        }
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            collect_nonplanar_island_key(path, key);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            collect_nonplanar_island_key(path, key);
    } else if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_nonplanar_island_key(*child, key);
    }
}

static bool nonplanar_island_before(const NonplanarIslandKey &left, const NonplanarIslandKey &right)
{
    if (left.has_nonplanar != right.has_nonplanar)
        return left.has_nonplanar;
    if (!left.has_nonplanar)
        return false;
    if (std::abs(left.shell_offset - right.shell_offset) > EPSILON)
        return left.shell_offset > right.shell_offset;
    return left.source_layer < right.source_layer;
}

static double polyline_length_scaled(const Polyline3 &polyline)
{
    double length = 0.;
    for (size_t i = 1; i < polyline.points.size(); ++ i)
        length += (polyline.points[i].to_point() - polyline.points[i-1].to_point()).cast<double>().norm();
    return length;
}

static Polyline3 sub_polyline(const Polyline3 &source, double a, double b)
{
    Polyline3 out;
    const Points3 &src = source.points;
    double walked = 0.;
    auto at = [](const Point3 &p, const Point3 &q, double t) {
        const Vec2d xy = p.to_point().cast<double>() + t * (q.to_point() - p.to_point()).cast<double>();
        return Point3(coord_t(std::round(xy.x())), coord_t(std::round(xy.y())),
                      coord_t(std::round(double(p.z()) + t * double(q.z() - p.z()))));
    };
    for (size_t i = 1; i < src.size(); ++ i) {
        const double len = (src[i].to_point() - src[i-1].to_point()).cast<double>().norm();
        const double s0 = walked, s1 = walked + len;
        walked = s1;
        if (s1 < a || len <= 0.)
            continue;
        if (s0 > b)
            break;
        if (out.points.empty())
            out.points.push_back(at(src[i-1], src[i], std::clamp((a - s0) / len, 0., 1.)));
        if (s1 <= b) {
            if (out.points.back() != src[i])
                out.points.push_back(src[i]);
        } else {
            out.points.push_back(at(src[i-1], src[i], (b - s0) / len));
            break;
        }
    }
    return out;
}

static std::vector<std::tuple<int, double, double>> plan_pieces(const NonplanarDeferral::PathPlan &plan, bool reversed, double length)
{
    std::vector<std::tuple<int, double, double>> out;
    const double scale = plan.length > 0. ? length / plan.length : 1.;
    for (size_t k = 0; k < plan.breaks.size(); ++ k) {
        const double a = plan.breaks[k].first * scale;
        const double b = (k + 1 < plan.breaks.size() ? plan.breaks[k + 1].first : plan.length) * scale;
        if (b - a <= scaled<double>(0.01))
            continue;
        out.emplace_back(plan.breaks[k].second, reversed ? length - b : a, reversed ? length - a : b);
    }
    if (reversed)
        std::reverse(out.begin(), out.end());
    return out;
}

#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

static const float g_min_purge_volume = 100.f;
static const float g_purge_volume_one_time = 135.f;
static const int g_max_flush_count = 4;
static const size_t g_max_label_object = 64;

static bool is_multi_nozzle_printer(const FullPrintConfig &config)
{
    return std::any_of(config.extruder_max_nozzle_count.values.begin(),
                       config.extruder_max_nozzle_count.values.end(),
                       [](int v) { return v > 1; });
}

static int hotend_id_for_gcode_placeholder(const std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> &group_result,
                                           bool is_bbl_printer, int filament_id, int extruder_id, int layer_id = -1)
{
    if (!is_bbl_printer)
        return extruder_id;
    if (group_result && group_result->is_support_dynamic_nozzle_map() && filament_id >= 0)
        return group_result->get_nozzle_id(filament_id, layer_id);
    return -1;
}

static int nozzle_id_for_gcode_placeholder(const std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> &group_result,
                                           int filament_id, int extruder_id, int layer_id = -1)
{
    if (group_result && filament_id >= 0)
        return group_result->get_nozzle_id(filament_id, layer_id);
    return extruder_id;
}

static int first_hotend_id_for_gcode_placeholder(const std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> &group_result,
                                                 bool is_bbl_printer, int filament_id, int extruder_id)
{
    if (!is_bbl_printer)
        return extruder_id;
    if (group_result && group_result->is_support_dynamic_nozzle_map() && filament_id >= 0) {
        auto nozzle = group_result->get_first_nozzle_for_filament(filament_id);
        if (nozzle)
            return nozzle->group_id;
    }
    return -1;
}

static int first_nozzle_id_for_gcode_placeholder(const std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> &group_result,
                                                 int filament_id, int extruder_id)
{
    if (group_result && filament_id >= 0) {
        auto nozzle = group_result->get_first_nozzle_for_filament(filament_id);
        if (nozzle)
            return nozzle->group_id;
    }
    return extruder_id;
}

static std::vector<double> get_nozzle_diameters_by_nozzle_id(const MultiNozzleUtils::NozzleGroupResultBase *group_result)
{
    std::vector<double> diameters;
    if (!group_result)
        return diameters;
    for (int id = 0;; ++id) {
        auto nozzle = group_result->get_nozzle_from_id(id);
        if (!nozzle)
            break;
        diameters.push_back(std::stod(nozzle->diameter));
    }
    return diameters;
}

static std::vector<std::string> get_nozzle_volume_types_by_nozzle_id(const MultiNozzleUtils::NozzleGroupResultBase *group_result)
{
    std::vector<std::string> volume_types;
    if (!group_result)
        return volume_types;

    int max_nozzle_id = -1;
    for (unsigned int filament_id : group_result->get_used_filaments()) {
        for (const auto &nozzle : group_result->get_nozzles_for_filament(filament_id)) {
            if (nozzle.group_id > max_nozzle_id)
                max_nozzle_id = nozzle.group_id;
        }
    }
    if (max_nozzle_id < 0)
        max_nozzle_id = 0;

    volume_types.resize(max_nozzle_id + 1, get_nozzle_volume_type_string(NozzleVolumeType::nvtStandard));
    for (int id = 0; id <= max_nozzle_id; ++id) {
        auto nozzle = group_result->get_nozzle_from_id(id);
        if (nozzle)
            volume_types[id] = get_nozzle_volume_type_string(nozzle->volume_type);
    }
    return volume_types;
}

Vec2d travel_point_1;
Vec2d travel_point_2;
Vec2d travel_point_3;
static std::vector<Vec2d> get_path_of_change_filament(const Print& print)
{
    std::vector<Vec2d> out_points;
    out_points.emplace_back(Vec2d(54, 0));
    out_points.emplace_back(Vec2d(54, 0));
    out_points.emplace_back(Vec2d(54, 245));

    Pointfs points = print.config().start_end_points.values;
    if (points.size() != 2)
        return out_points;

    Vec2d start_point  = points[0];
    Vec2d end_point    = points[1];

    Pointfs excluse_area = print.config().bed_exclude_area.values;
    if (excluse_area.size() != 4)
        return out_points;

    double cutter_area_x = excluse_area[2].x() + 2;
    double cutter_area_y = excluse_area[2].y() + 2;

    double start_x_position = start_point.x();
    double end_x_position   = end_point.x();
    double end_y_position   = end_point.y();

    bool can_travel_form_left = true;

    std::vector<std::pair<double, double>> object_intervals;
    for (PrintObject *print_object : print.objects()) {
        const PrintInstances &print_instances = print_object->instances();
        BoundingBoxf3 bounding_box = print_instances[0].model_instance->get_object()->bounding_box_exact();

        if (bounding_box.min.x() < start_x_position && bounding_box.min.y() < cutter_area_y)
            can_travel_form_left = false;

        std::pair<double, double> object_scope = std::make_pair(bounding_box.min.x() - 2, bounding_box.max.x() + 2);
        if (object_intervals.empty())
            object_intervals.push_back(object_scope);
        else {
            std::vector<std::pair<double, double>> new_object_intervals;
            bool intervals_intersect = false;
            std::pair<double, double>              new_merged_scope;
            for (auto object_interval : object_intervals) {
                if (object_interval.second >= object_scope.first && object_interval.first <= object_scope.second) {
                    if (intervals_intersect) {
                        new_merged_scope = std::make_pair(std::min(object_interval.first, new_merged_scope.first), std::max(object_interval.second, new_merged_scope.second));
                    } else {
                        new_merged_scope = std::make_pair(std::min(object_interval.first, object_scope.first), std::max(object_interval.second, object_scope.second));
                    }
                    intervals_intersect = true;
                } else {
                    new_object_intervals.push_back(object_interval);
                }
            }

            if (intervals_intersect) {
                new_object_intervals.push_back(new_merged_scope);
                object_intervals = new_object_intervals;
            } else
                object_intervals.push_back(object_scope);
        }
    }

    std::sort(object_intervals.begin(), object_intervals.end(),
              [](const std::pair<double, double> &left, const std::pair<double, double> &right) {
            return left.first < right.first;
    });
    std::vector<std::pair<double, double>> available_intervals;
    double                                 start_position = 0;
    for (auto object_interval : object_intervals) {
        if (object_interval.first > start_position)
            available_intervals.push_back(std::make_pair(start_position, object_interval.first));
        start_position = object_interval.second;
    }
    available_intervals.push_back(std::make_pair(start_position, 255));

    double new_path     = 255;
    for (auto available_interval : available_intervals) {
        if (available_interval.first > end_x_position) {
            double distance = available_interval.first - end_x_position;
            new_path        = abs(end_x_position - new_path) < distance ? new_path : available_interval.first;
            break;
        } else {
            if (available_interval.second >= end_x_position) {
                new_path = end_x_position;
                break;
            } else if (!can_travel_form_left && available_interval.second < start_x_position) {
                continue;
            } else {
                new_path     = available_interval.second;
            }
        }
    }

    Vec2d out_point_1;
    Vec2d out_point_2;
    Vec2d out_point_3;
    if (new_path < start_x_position) {
        out_point_1 = Vec2d(start_x_position, cutter_area_y);
        out_point_2 = Vec2d(new_path, cutter_area_y);
        out_point_3 = Vec2d(new_path, end_y_position);
    } else {
        out_point_1 = Vec2d(new_path, 0);
        out_point_2 = Vec2d(new_path, 0);
        out_point_3 = Vec2d(new_path, end_y_position);
    }

    out_points.clear();
    out_points.emplace_back(out_point_1);
    out_points.emplace_back(out_point_2);
    out_points.emplace_back(out_point_3);

    return out_points;
}

    static inline void check_add_eol(std::string& gcode)
    {
        if (!gcode.empty() && gcode.back() != '\n')
            gcode += '\n';
    }


    static bool custom_gcode_changes_tool(const std::string& custom_gcode, const std::string& tch_prefix, unsigned next_extruder)
    {
        bool ok = false;
        size_t from_pos = 0;
        size_t pos = 0;
        while ((pos = custom_gcode.find(tch_prefix, from_pos)) != std::string::npos) {
            if (pos + 1 == custom_gcode.size())
                break;
            from_pos = pos + 1;
            while (--pos < custom_gcode.size() && custom_gcode[pos] != '\n') {
                if (!std::isspace(custom_gcode[pos]))
                    goto NEXT;
            }
            {
                std::istringstream ss(custom_gcode.substr(from_pos, std::string::npos));
                unsigned num = 0;
                if (ss >> num)
                    ok = (num == next_extruder);
            }
        NEXT:;
        }
        return ok;
    }

    std::string OozePrevention::pre_toolchange(GCode& gcodegen)
    {
        std::string gcode;

        unsigned int extruder_id = gcodegen.writer().filament()->id();
        const auto& filament_idle_temp = gcodegen.config().idle_temperature;
        if (filament_idle_temp.get_at(extruder_id) == 0) {
            if (gcodegen.config().standby_temperature_delta.value != 0) {
                gcode += gcodegen.writer().set_temperature
                (this->_get_temp(gcodegen) + gcodegen.config().standby_temperature_delta.value, false, extruder_id);
                gcode.pop_back();
                gcode += " ;cooldown\n";
            }
        } else {
            gcode += gcodegen.writer().set_temperature(filament_idle_temp.get_at(extruder_id), false, extruder_id);
            gcode.pop_back();
            gcode += " ;cooldown\n";
        }

        return gcode;
    }

    std::string OozePrevention::post_toolchange(GCode& gcodegen)
    {
        return (gcodegen.config().standby_temperature_delta.value != 0) ?
            gcodegen.writer().set_temperature(this->_get_temp(gcodegen), true, gcodegen.writer().filament()->id()) :
            std::string();
    }

    int OozePrevention::_get_temp(const GCode &gcodegen) const
    {
        size_t fi = gcodegen.get_filament_config_index((int)gcodegen.writer().filament()->id());
        return (gcodegen.layer() == nullptr || gcodegen.layer()->id() == 0
             || gcodegen.config().nozzle_temperature.get_at(fi) == 0)
            ? gcodegen.config().nozzle_temperature_initial_layer.get_at(fi)
            : gcodegen.config().nozzle_temperature.get_at(fi);
    }
    
    Wipe::RetractionValues Wipe::calculateWipeRetractionLengths(GCode& gcodegen, bool toolchange) {
        auto& writer = gcodegen.writer();
        auto& config = gcodegen.config();
        auto extruder = writer.filament();
        auto last_pos = gcodegen.last_pos();
        
        double retraction_length_remaining = 0,
            retraction_length_before_wipe = 0,
            retraction_length_during_wipe = 0,
            retraction_length_after_wipe = 0;
        
        retraction_length_remaining = toolchange ? 
            extruder->retract_length_toolchange() : extruder->retraction_length();
        
        if (retraction_length_remaining <= EPSILON)
            return { 0.f, 0.f, 0.f };
        
        retraction_length_before_wipe = retraction_length_remaining * extruder->retract_before_wipe();
        retraction_length_after_wipe = retraction_length_remaining * extruder->retract_after_wipe();

        retraction_length_remaining -= retraction_length_before_wipe + retraction_length_after_wipe;

        if (retraction_length_remaining <= EPSILON) 
            return { retraction_length_before_wipe, 0., retraction_length_after_wipe };
        
        double wipe_speed = config.role_based_wipe_speed ? writer.get_current_speed() / 60.0 : config.get_abs_value("wipe_speed", gcodegen.config().travel_speed.get_at(gcodegen.get_nozzle_config_index(gcodegen.writer().filament()->id())));
        wipe_speed = std::max(wipe_speed, 10.0);

        double wipe_dist = scale_(config.wipe_distance.get_at(extruder->config_index()));
        Polyline wipe_path = {last_pos};
        wipe_path.append(this->path.points.begin() + 1, this->path.points.end());
        double wipe_path_length = std::min(wipe_path.length(), wipe_dist);

        retraction_length_during_wipe = config.retraction_speed.get_at(extruder->config_index()) *
            unscale_(wipe_path_length) / wipe_speed;

        if (retraction_length_during_wipe <= EPSILON) 
            return { retraction_length_before_wipe, 0., retraction_length_after_wipe };
        
        if (retraction_length_during_wipe - retraction_length_remaining > EPSILON) 
            return { retraction_length_before_wipe, retraction_length_remaining, retraction_length_after_wipe };
        
        retraction_length_before_wipe += retraction_length_remaining - retraction_length_during_wipe;

        return { retraction_length_before_wipe, retraction_length_during_wipe, retraction_length_after_wipe };
    }

    std::string transform_gcode(const std::string &gcode, Vec2f pos, const Vec2f &translation, float angle)
    {
        Vec2f              extruder_offset(0, 0);
        std::istringstream gcode_str(gcode);
        std::string        gcode_out;
        std::string        line;
        Vec2f              transformed_pos = pos;
        Vec2f              old_pos(-1000.1f, -1000.1f);

        while (gcode_str) {
            std::getline(gcode_str, line);

            if (line.find("G1 ") == 0) {
                bool never_skip = false;
                auto it         = line.find(WipeTower::never_skip_tag());
                if (it != std::string::npos) {
                    never_skip = true;
                    line.erase(it, it + WipeTower::never_skip_tag().size());
                }
                std::ostringstream line_out;
                std::istringstream line_str(line);
                line_str >> std::noskipws;
                char ch = 0;
                while (line_str >> ch) {
                    if (ch == 'X' || ch == 'Y')
                        line_str >> (ch == 'X' ? pos.x() : pos.y());
                    else
                        line_out << ch;
                }

                transformed_pos = Eigen::Rotation2Df(angle) * pos + translation;

                if (transformed_pos != old_pos || never_skip) {
                    line = line_out.str();
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3) << "G1 ";
                    if (transformed_pos.x() != old_pos.x() || never_skip) oss << " X" << transformed_pos.x() - extruder_offset.x();
                    if (transformed_pos.y() != old_pos.y() || never_skip) oss << " Y" << transformed_pos.y() - extruder_offset.y();
                    oss << " ";
                    line.replace(line.find("G1 "), 3, oss.str());
                    old_pos = transformed_pos;
                }
            }

            gcode_out += line + "\n";
        }
        return gcode_out;
    }

    float get_wipe_avoid_pos_x(const Vec2f &wt_min, const Vec2f &wt_max, float offset)
    {
        float left = 100, right = 250;
        float default_value = 110.f;
        float a = 0.f, b = 0.f;
        a = wt_max.x() + offset;
        b = wt_min.x() - offset;
        if (a > left && a < right) return a;
        if (b > left && b < right) return b;
        return default_value;
    }

    void Wipe::update_path(const ExtrusionPaths &paths, bool reverse)
    {
        reset_path();
        for (const ExtrusionPath& extrusion_path : paths)
            path.append(extrusion_path.polyline.to_polyline());
        if (reverse)
            path.reverse();
    }

    std::string Wipe::wipe(GCode& gcodegen,double length, bool toolchange, bool is_last)
    {
        std::string gcode;

        double _wipe_speed = gcodegen.config().get_abs_value("wipe_speed", gcodegen.config().travel_speed.get_at(gcodegen.get_nozzle_config_index(gcodegen.writer().filament()->id())));
        if(gcodegen.config().role_based_wipe_speed)
            _wipe_speed = gcodegen.writer().get_current_speed() / 60.0;
        if(_wipe_speed < 10)
            _wipe_speed = 10;


        if (length >= 0)
        {
            double wipe_dist = scale_(gcodegen.config().wipe_distance.get_at(gcodegen.get_filament_config_index((int)gcodegen.writer().filament()->id())));

            Polyline wipe_path;
            wipe_path.append(gcodegen.last_pos());
            wipe_path.append(
                this->path.points.begin() + 1,
                this->path.points.end()
            );

            wipe_path.clip_end(wipe_path.length() - wipe_dist);

            if (!wipe_path.empty()) {
                if (wipe_path.length() < wipe_dist) {
                    wipe_dist = wipe_path.length();
                    wipe_dist = wipe_dist < EPSILON ? EPSILON : wipe_dist;
                }

                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";
                std::string cooling_mark = "";
                if (gcodegen.enable_cooling_markers() && !is_last)
                    cooling_mark = ";_WIPE";

                gcode += gcodegen.writer().set_speed(_wipe_speed * 60, "", cooling_mark);
                for (const Line& line : wipe_path.lines()) {
                    double segment_length = line.length();
                    double dE = length * (segment_length / wipe_dist);
                    gcode += gcodegen.writer().extrude_to_xy(
                        gcodegen.point_to_gcode(line.b),
                        -dE,
                        "wipe and retract"
                    );
                }
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
                gcodegen.set_last_pos(wipe_path.points.back());
            }

            this->reset_path();
        }

        return gcode;
    }

    static inline Point wipe_tower_point_to_object_point(GCode& gcodegen, const Vec2f& wipe_tower_pt)
    {
        return Point(scale_(wipe_tower_pt.x() - gcodegen.origin()(0)), scale_(wipe_tower_pt.y() - gcodegen.origin()(1)));
    }

    static float get_outer_wall_volumetric_speed(const FullPrintConfig& config, const Print& print, int filament_id, int filament_variant_idx, int extruder_id) {
        float outer_wall_volumetric_speed = 0;
        float filament_max_volumetric_speed = config.filament_max_volumetric_speed.get_at(filament_variant_idx);
        const double filament_diameter = config.filament_diameter.get_at(filament_id);
        float outer_wall_line_width = print.default_region_config().get_abs_value("outer_wall_line_width", filament_diameter);
        if (outer_wall_line_width == 0.0) {
            float default_line_width = print.default_object_config().get_abs_value("line_width", filament_diameter);
            outer_wall_line_width = default_line_width == 0.0 ? filament_diameter : default_line_width;
        }
        Flow outer_wall_flow = Flow(outer_wall_line_width, config.layer_height, config.nozzle_diameter.get_at(extruder_id));
        float outer_wall_speed = print.default_region_config().outer_wall_speed.get_at(extruder_id);
        outer_wall_volumetric_speed = outer_wall_speed * outer_wall_flow.mm3_per_mm();
        if (outer_wall_volumetric_speed > filament_max_volumetric_speed)
            outer_wall_volumetric_speed = filament_max_volumetric_speed;
        return outer_wall_volumetric_speed;
    }


    static bool custom_gcode_sets_temperature(const std::string &gcode, const int mcode_set_temp_dont_wait, const int mcode_set_temp_and_wait, const bool include_g10, int &temp_out)
    {
        temp_out = -1;
        if (gcode.empty())
            return false;

        const char *ptr = gcode.data();
        bool temp_set_by_gcode = false;
        while (*ptr != 0) {
            for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
            if (*ptr == 'M' ||
                (*ptr == 'G' && include_g10)) {
                bool is_gcode = *ptr == 'G';
                ++ ptr;
                char *endptr = nullptr;
                int mgcode = int(strtol(ptr, &endptr, 10));
                if (endptr != nullptr && endptr != ptr &&
                    is_gcode ?
                    mgcode == 10 :
                    (mgcode == mcode_set_temp_dont_wait || mgcode == mcode_set_temp_and_wait)) {
                    ptr = endptr;
                    if (! is_gcode)
                        temp_set_by_gcode = true;
                    while (strchr(";\r\n\0", *ptr) == nullptr) {
                        for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                        if (*ptr == 'S') {
                            for (++ ptr; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                            endptr = nullptr;
                            long temp_parsed = strtol(ptr, &endptr, 10);
                            if (endptr > ptr) {
                                ptr = endptr;
                                temp_out = temp_parsed;
                                temp_set_by_gcode = true;
                            }
                        } else {
                            for (; strchr(" \t;\r\n\0", *ptr) == nullptr; ++ ptr);
                        }
                    }
                }
            }
            for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ ptr);
            for (; *ptr == '\r' || *ptr == '\n'; ++ ptr);
        }
        return temp_set_by_gcode;
    }

    struct CustomGCodeMotionStateChanges
    {
        bool acceleration = false;
        bool jerk         = false;
    };

    static bool custom_gcode_line_has_xy_parameter(const std::string &raw)
    {
        const size_t comment_pos = raw.find(';');
        const std::string_view code(raw.data(), comment_pos == std::string::npos ? raw.size() : comment_pos);
        return code.find_first_of("XxYy") != std::string_view::npos;
    }

    static CustomGCodeMotionStateChanges custom_gcode_motion_state_changes(const std::string &gcode)
    {
        CustomGCodeMotionStateChanges changes;
        GCodeReader parser;
        parser.parse_buffer(gcode, [&changes](GCodeReader &parser, const GCodeReader::GCodeLine &line) {
            const std::string_view cmd = line.cmd();
            if (ascii_iequals(cmd, "M204") || ascii_iequals(cmd, "M201") ||
                ascii_iequals(cmd, "M202"))
                changes.acceleration = true;
            else if ((ascii_iequals(cmd, "M205") || ascii_iequals(cmd, "M207") || ascii_iequals(cmd, "M566")) &&
                     custom_gcode_line_has_xy_parameter(line.raw()))
                changes.jerk = true;
            else if (ascii_iequals(cmd, "SET_VELOCITY_LIMIT")) {
                changes.acceleration |= boost::icontains(line.raw(), "ACCEL=");
                changes.jerk         |= boost::icontains(line.raw(), "SQUARE_CORNER_VELOCITY=");
            }

            if (changes.acceleration && changes.jerk)
                parser.quit_parsing();
        });
        return changes;
    }

    static constexpr float wipe_tower_routing_clearance = 2.f;

    Polyline WipeTowerIntegration::generate_path_to_wipe_tower(const Point& start_pos,const Point &end_pos , const BoundingBox& avoid_polygon , const Polygons& bed_polygons) const
    {
        Polyline    res;
        coord_t         alpha = scaled(wipe_tower_routing_clearance);
        BoundingBox avoid_polygon_inner = avoid_polygon;
        avoid_polygon_inner.offset(alpha);
        coord_t width = avoid_polygon_inner.max[0] - avoid_polygon_inner.min[0];
        Vec2f v(1, 0);
        if (abs(end_pos[0] - avoid_polygon_inner.min[0]) < width / 2) v = -v;
        Points avoid_points  = avoid_polygon_inner.polygon().points;
        const bool is_bbx_in_bed = std::all_of(avoid_points.begin(), avoid_points.end(),
            [&bed_polygons](const Point &pt) { return contains(bed_polygons, pt, false); });
        if (!is_bbx_in_bed) {
            res.points.push_back(end_pos);
            return res;
        }
        auto ray_intersetion_line = [](const Vec2d &a, const Vec2d &v1, const Vec2d &b, const Vec2d &c) -> std::pair<bool, Point> {
            const Vec2d v2    = c - b;
            double      denom = cross2(v1, v2);
            if (fabs(denom) < EPSILON) return {false, Point(0, 0)};
            const Vec2d v12    = (a - b);
            double      nume_a = cross2(v2, v12);
            double      nume_b = cross2(v1, v12);
            double      t1     = nume_a / denom;
            double      t2     = nume_b / denom;
            if (t1 >= 0 && t2 >= 0 && t2 <= 1.) {
                Vec2d res = a + t1 * v1;
                return std::pair<bool, Point>(true, scaled(res));
            }
            return std::pair<bool, Point>(false, {0, 0});
        };
        struct Inter_info
        {
            int   inter_idx0 = -1;
            Point inter_p;
        };
        auto calc_path_len = [](Points &points, Inter_info &beg_info, Inter_info &end_info, bool is_add) -> std::pair<std::vector<Point>, double> {
            int                beg = is_add ? (beg_info.inter_idx0 + 1) % points.size() : beg_info.inter_idx0;
            int                end = is_add ? end_info.inter_idx0 : (end_info.inter_idx0 + 1) % points.size();
            int                i   = beg;
            double             len = 0;
            std::vector<Point> path;
            path.push_back(beg_info.inter_p);
            len += (unscale(beg_info.inter_p) - unscale(points[beg])).squaredNorm();
            while (i != end) {
                int  ni = is_add ? (i + 1) % points.size() : (i - 1 + points.size()) % points.size();
                auto a  = unscale(points[i]);
                auto b  = unscale(points[ni]);
                len += (a - b).squaredNorm();
                path.push_back(points[i]);
                i = ni;
            }
            path.push_back(points[end]);
            path.push_back(end_info.inter_p);
            len += (unscale(end_info.inter_p) - unscale(points[end])).squaredNorm();
            return {path, len};
        };
        Inter_info inter_info;
        for (size_t i = 0; i < avoid_points.size(); i++) {
            const auto &a                  = avoid_points[i];
            const auto &b                  = avoid_points[(i + 1) % avoid_points.size()];
            auto [is_inter, inter_p] = ray_intersetion_line(unscale(end_pos), v.cast<double>(), unscale(a), unscale(b));
            if (is_inter) {
                inter_info.inter_idx0 = i;
                inter_info.inter_p    = inter_p;
                break;
            }
        }
        if (inter_info.inter_idx0 == -1) {
            res.points.push_back(end_pos);
            return res;
        }
        Inter_info inter_info2;
        Linef      start_to_p(unscale(start_pos), unscale(inter_info.inter_p));
        for (size_t i = 0; i < avoid_points.size(); i++) {
            if (i == inter_info.inter_idx0) continue;
            Vec2d a = unscale(avoid_points[i]);
            Vec2d b = unscale(avoid_points[(i + 1) % avoid_points.size()]);
            Linef tower_edge(a, b);
            Vec2d inter;
            if (line_alg::intersection(start_to_p, tower_edge, &inter)) {
                inter_info2.inter_p    = scaled(inter);
                inter_info2.inter_idx0 = i;
                break;
            }
        }
        if (inter_info2.inter_idx0 == -1) {
            res.points.push_back(inter_info.inter_p);
        } else {
            std::vector<Point> path;
            auto [path1, len1] = calc_path_len(avoid_points, inter_info2, inter_info, true);
            auto [path2, len2] = calc_path_len(avoid_points, inter_info2, inter_info, false);
            path               = len1 < len2 ? path1 : path2;
            for (size_t i = 0; i < path.size(); i++) {
                res.points.push_back(path[i]);
            }
        }
        res.points.push_back(end_pos);
        return res;
    }

    Vec2f WipeTowerIntegration::transform_wt2_pt(const Vec2f &pt) const
    {
        const float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);
        return Eigen::Rotation2Df(alpha) * (pt + m_rib_offset) + m_wipe_tower_pos;
    }

    Polygons WipeTowerIntegration::shared_printable_area(GCode &gcodegen) const
    {
        const Point offset = wipe_tower_point_to_object_point(gcodegen, Vec2f(m_plate_origin(0), m_plate_origin(1)));
        Polygons    bed_polygons = gcodegen.m_print->get_extruder_shared_printable_polygon();
        for (Polygon &poly : bed_polygons)
            poly.translate(offset);
        return bed_polygons;
    }

    std::string WipeTowerIntegration::travel_to_tower_gap(GCode &gcodegen, const Point &route_start, const Point &start_wipe_pos) const
    {
        if (!WipeTower2::use_gap_wall(gcodegen.m_config))
            return {};
        const Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));
        auto tower_polygon = [&](const BoundingBoxf &bbx) {
            Polygon poly = scaled(bbx).polygon();
            for (Point &p : poly.points)
                p = wipe_tower_point_to_object_point(gcodegen, transform_wt2_pt(unscale(p).cast<float>()) + plate_origin_2d);
            return poly;
        };
        const float body_width = gcodegen.m_config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib ? m_wipe_tower_depth : m_right;
        if (tower_polygon(BoundingBoxf(Vec2d(0., 0.), Vec2d(body_width, m_wipe_tower_depth))).contains(route_start))
            return {};

        const Polygons bed       = shared_printable_area(gcodegen);
        BoundingBox    avoid_bbx = get_extents(tower_polygon(m_wipe_tower_bbx));
        BoundingBox clamp_bbx = get_extents(bed);
        clamp_bbx.offset(-(scaled(wipe_tower_routing_clearance) + SCALED_EPSILON));
        avoid_bbx.min = avoid_bbx.min.cwiseMax(clamp_bbx.min);
        avoid_bbx.max = avoid_bbx.max.cwiseMin(clamp_bbx.max);
        if (avoid_bbx.min.x() >= avoid_bbx.max.x() || avoid_bbx.min.y() >= avoid_bbx.max.y())
            return {};

        Polyline    travel_polyline = generate_path_to_wipe_tower(route_start, start_wipe_pos, avoid_bbx, bed);
        std::string gcode;
        const double route_z = gcodegen.m_nonplanar_ceiling > 0. ?
            std::max(gcodegen.writer().get_position().z(), gcodegen.nonplanar_travel_z()) : DBL_MAX;
        for (size_t i = 0; i + 1 < travel_polyline.points.size(); ++i)
            gcode += gcodegen.travel_to(travel_polyline.points[i], erMixed, "Travel to a Wipe Tower", route_z);
        return gcode;
    }

    std::string WipeTowerIntegration::tower_height_tag(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr,
                                                       const std::string &tcr_gcode) const
    {
        const std::string tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height);
        if (! m_sparse_layers_combined || std::abs(gcodegen.m_last_height - tcr.layer_height) <= EPSILON ||
            tcr_gcode.find(tag) != std::string::npos)
            return {};
        gcodegen.m_last_height = tcr.layer_height;
        char buf[64];
        sprintf(buf, "%s%g\n", tag.c_str(), tcr.layer_height);
        return buf;
    }

    void WipeTowerIntegration::record_nonplanar_material(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, double z) const
    {
        if (!gcodegen.m_material_columns || tcr.extrusions.empty())
            return;
        const bool type2 = gcodegen.wipe_tower_type() == WipeTowerType::Type2;
        const Eigen::Rotation2Df rotation(m_wipe_tower_rotation / 180.f * float(M_PI));
        auto point = [&](const Vec2f &local, unsigned tool) {
            Vec2f xy = local;
            if (!tcr.priming)
                xy = type2 ? transform_wt2_pt(local) : Vec2f(rotation * local + m_wipe_tower_pos);
            if (!type2)
                xy += m_rib_offset;
            const Vec2d offset = m_extruder_offsets[m_single_extruder_multi_material ? 0 : tool];
            return Vec3d(double(xy.x()) - offset.x(), double(xy.y()) - offset.y(), z);
        };
        Vec3d previous = point(tcr.start_pos, tcr.initial_tool);
        for (const WipeTower::Extrusion &road : tcr.extrusions) {
            const Vec3d next = point(road.pos, road.tool);
            if (road.width > 0.)
                gcodegen.m_material_columns->deposit(previous, next, road.width);
            previous = next;
        }
        gcodegen.m_nonplanar_ceiling = std::max(gcodegen.m_nonplanar_ceiling, z);
    }

    std::string WipeTowerIntegration::prepare_nonplanar_visit(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr,
                                                               const Point &start, double &z) const
    {
        if ((gcodegen.m_nonplanar_ceiling <= 0. && !Nonplanar::requests_conformal_shells(*gcodegen.m_print)) || tcr.priming)
            return {};
        if (z == -1.)
            z = tcr.print_z + gcodegen.config().z_offset.value;
        if (Nonplanar::sequential_shell_tower(*gcodegen.m_print))
            gcodegen.m_nonplanar_ceiling = std::max(gcodegen.m_nonplanar_ceiling, z);
        gcodegen.m_max_layer_z = std::max(gcodegen.m_max_layer_z, float(z - gcodegen.config().z_offset.value));
        gcodegen.m_wipe.reset_path();
        std::string gcode = gcodegen.retract();
        const double safe = std::max({ gcodegen.writer().get_position().z(), z, gcodegen.nonplanar_travel_z() });
        gcode += gcodegen.writer().travel_to_z(safe, "lift over shells for wipe tower", true, gcodegen.conformal_max_speed_z());
        if (gcodegen.wipe_tower_type() == WipeTowerType::Type2 && gcodegen.last_pos_defined())
            gcode += travel_to_tower_gap(gcodegen, gcodegen.last_pos(), start);
        gcode += gcodegen.writer().travel_to_xy(gcodegen.point_to_gcode(start), "move above planar wipe tower");
        gcodegen.set_last_pos(start);
        gcode += gcodegen.writer().travel_to_z(z, "restore planar wipe tower Z", true, gcodegen.conformal_max_speed_z());
        return gcode;
    }

    std::string WipeTowerIntegration::append_tcr(GCode& gcodegen, const WipeTower::ToolChangeResult& tcr, int new_filament_id, double z) const
    {
        if (new_filament_id != -1 && new_filament_id != tcr.new_tool)
            throw Slic3r::InvalidArgument("Error: WipeTowerIntegration::append_tcr was asked to do a toolchange it didn't expect.");

        int new_extruder_id = get_extruder_index(*m_print_config, new_filament_id);

        auto group_result = gcodegen.m_print->get_layered_nozzle_group_result();

        bool is_nozzle_change = !tcr.nozzle_change_result.gcode.empty() && (gcodegen.config().nozzle_diameter.size() > 1);

        std::string gcode;

        float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);

        auto transform_wt_pt = [&alpha, this](const Vec2f& pt) -> Vec2f {
            Vec2f out = Eigen::Rotation2Df(alpha) * pt;
            out += m_wipe_tower_pos + m_rib_offset;
            return out;
        };

        Vec2f start_pos = tcr.start_pos;
        Vec2f end_pos   = tcr.end_pos;
        Vec2f tool_change_start_pos = start_pos;
        if (tcr.is_tool_change)
            tool_change_start_pos = tcr.tool_change_start_pos;
        if (!tcr.priming) {
            start_pos = transform_wt_pt(start_pos);
            end_pos   = transform_wt_pt(end_pos);
            tool_change_start_pos = transform_wt_pt(tool_change_start_pos);
        }

        Vec2f wipe_tower_offset   = (tcr.priming ? Vec2f::Zero() : m_wipe_tower_pos) + m_rib_offset;
        float wipe_tower_rotation = tcr.priming ? 0.f : alpha;

        std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

        Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));

        gcode += prepare_nonplanar_visit(gcodegen, tcr,
            wipe_tower_point_to_object_point(gcodegen, tool_change_start_pos + plate_origin_2d), z);
        const double travel_target_z = !tcr.priming &&
            (gcodegen.m_nonplanar_ceiling > 0. || Nonplanar::requests_conformal_shells(*gcodegen.m_print)) ? z : DBL_MAX;

        if (!tcr.priming && tcr.is_finish_first) {
            gcode += gcodegen.retract();
            gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
            gcode += gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d), erMixed,
                                        "Travel to a Wipe Tower", travel_target_z);
            gcode += gcodegen.unretract();
        }


        double current_z = gcodegen.writer().get_position().z();
        if (z == -1.)
            z = gcodegen.tower_layer_z();
        const bool defer_compacted_descend = m_sparse_layers_skipped
            && !tcr.priming && !tcr.is_finish_first && (current_z - z) > EPSILON;
        if (!is_approx(z, current_z) && !defer_compacted_descend) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
            gcode += gcodegen.writer().unretract();
        }
        const bool compacted_below_object = m_sparse_layers_skipped && z >= 0. && (tcr.print_z - z) > EPSILON;

        bool        add_change_filament_624 = false;
        std::string end_filament_gcode_str;
        if (gcodegen.writer().filament() != nullptr) {
            unsigned int        old_filament_id = gcodegen.writer().filament()->id();
            const std::string& filament_end_gcode = gcodegen.config().filament_end_gcode.get_at(old_filament_id);
            if (gcodegen.writer().filament() != nullptr && !filament_end_gcode.empty()) {
                DynamicConfig config;
                config.set_key_value("current_filament_id", new ConfigOptionInt((int) old_filament_id));
                config.set_key_value("current_nozzle_id", new ConfigOptionInt(nozzle_id_for_gcode_placeholder(group_result, (int) old_filament_id, (int) gcodegen.writer().filament()->extruder_id(), m_layer_idx)));
                config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
                config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
                config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
                config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
                if (!gcodegen.m_filament_instances_code.empty()) {
                    end_filament_gcode_str += ("M624 " + gcodegen.m_filament_instances_code + "\n");
                    gcodegen.m_filament_instances_code = "";
                    add_change_filament_624 = true;
                }
                end_filament_gcode_str += gcodegen.placeholder_parser_process("filament_end_gcode", filament_end_gcode, old_filament_id, &config);
                check_add_eol(end_filament_gcode_str);
            }
        }

        std::string toolchange_gcode_str;

        ZHopType z_hope_type = ZHopType(gcodegen.config().z_hop_types.get_at(gcodegen.get_filament_config_index((int)gcodegen.writer().filament()->id())));
        LiftType auto_lift_type = LiftType::NormalLift;
        if (z_hope_type == ZHopType::zhtAuto || z_hope_type == ZHopType::zhtSpiral || z_hope_type == ZHopType::zhtSlope)
            auto_lift_type = LiftType::SpiralLift;

        std::string toolchange_retract_str = gcodegen.retract(tcr.is_tool_change && !is_nozzle_change, false, auto_lift_type, true);
        check_add_eol(toolchange_retract_str);

        std::string object_end_label_temp;
        gcodegen.m_writer.add_object_end_labels(object_end_label_temp);

        std::string change_filament_gcode = gcodegen.config().change_filament_gcode.value;

        bool is_used_travel_avoid_perimeter = gcodegen.m_config.prime_tower_skip_points.value;
        if (is_nozzle_change && !tcr.nozzle_change_result.is_extruder_change) is_used_travel_avoid_perimeter = false;

        std::string nozzle_change_gcode_trans;
        if (is_nozzle_change) {
            const bool compact_intower_nc_travel = compacted_below_object
                && (tcr.print_z - gcodegen.writer().get_position().z()) > EPSILON;
            std::string start_pos_str;
            start_pos_str = gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(tcr.nozzle_change_result.start_pos) + plate_origin_2d), erMixed,
                "Move to nozzle change start pos", travel_target_z != DBL_MAX ? travel_target_z :
                    (compact_intower_nc_travel ? z : DBL_MAX));
            check_add_eol(start_pos_str);
            nozzle_change_gcode_trans += start_pos_str;
            if (!compact_intower_nc_travel && compacted_below_object) {
                std::string nc_z_descend = gcodegen.writer().travel_to_z(z, "Descend to compacted wipe tower z (no sparse layers)");
                check_add_eol(nc_z_descend);
                nozzle_change_gcode_trans += nc_z_descend;
            }
            nozzle_change_gcode_trans += gcodegen.unretract();
            nozzle_change_gcode_trans += transform_gcode(tcr.nozzle_change_result.gcode, tcr.nozzle_change_result.start_pos, wipe_tower_offset, wipe_tower_rotation);
            gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(tcr.nozzle_change_result.end_pos) + plate_origin_2d));
            gcodegen.m_wipe.reset_path();
            for (const Vec2f& wipe_pt : tcr.nozzle_change_result.wipe_path)
                gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt) + plate_origin_2d));
            nozzle_change_gcode_trans += gcodegen.retract(tcr.is_tool_change, false, auto_lift_type, true);
            end_filament_gcode_str = nozzle_change_gcode_trans + end_filament_gcode_str;
        }

        end_filament_gcode_str = toolchange_retract_str + object_end_label_temp + end_filament_gcode_str;

        std::string wipe_next_start_point_str;
        bool        need_travel_after_change_filament_gcode = false;
        if (! change_filament_gcode.empty()) {
            DynamicConfig config;
            int old_filament_id = gcodegen.writer().filament() ? (int)gcodegen.writer().filament()->id() : -1;
            int old_extruder_id = gcodegen.writer().filament() ? (int)gcodegen.writer().filament()->extruder_id() : -1;
            int old_nozzle_id  = nozzle_id_for_gcode_placeholder(group_result, old_filament_id, old_extruder_id, m_layer_idx);
            int next_nozzle_id = nozzle_id_for_gcode_placeholder(group_result, new_filament_id, new_extruder_id, m_layer_idx);

            config.set_key_value("previous_extruder", new ConfigOptionInt(old_filament_id));
            config.set_key_value("next_extruder", new ConfigOptionInt(new_filament_id));
            const bool is_bbl_printer = gcodegen.m_print->is_BBL_printer();
            config.set_key_value("current_hotend", new ConfigOptionInt(
                hotend_id_for_gcode_placeholder(group_result, is_bbl_printer, old_filament_id, old_extruder_id, m_layer_idx)));
            config.set_key_value("next_hotend", new ConfigOptionInt(
                hotend_id_for_gcode_placeholder(group_result, is_bbl_printer, new_filament_id, (int) gcodegen.get_extruder_id(new_filament_id), m_layer_idx)));
            config.set_key_value("current_nozzle_id", new ConfigOptionInt(old_nozzle_id));
            config.set_key_value("next_nozzle_id", new ConfigOptionInt(next_nozzle_id));
            config.set_key_value("current_filament_id", new ConfigOptionInt(old_filament_id));
            config.set_key_value("next_filament_id", new ConfigOptionInt(new_filament_id));
            {
                const auto &extruder_variants = m_print_config->printer_extruder_variant.values;
                config.set_key_value("old_extruder_variant", new ConfigOptionString(
                    (old_extruder_id >= 0 && old_extruder_id < (int) extruder_variants.size()) ? extruder_variants[old_extruder_id] : std::string()));
                config.set_key_value("new_extruder_variant", new ConfigOptionString(
                    (new_extruder_id >= 0 && new_extruder_id < (int) extruder_variants.size()) ? extruder_variants[new_extruder_id] : std::string()));
            }
            config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
            config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
            config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
            config.set_key_value("toolchange_z", new ConfigOptionFloat(z));
            if (gcodegen.m_nonplanar_ceiling > 0.)
                config.set_key_value("max_layer_z", new ConfigOptionFloat(std::max(gcodegen.m_nonplanar_ceiling,
                    double(gcodegen.m_max_layer_z) + gcodegen.config().z_offset.value)));
            {
                GCodeWriter& gcode_writer = gcodegen.m_writer;
                FullPrintConfig& full_config = gcodegen.m_config;

                size_t old_fi = (old_filament_id != -1) ? gcodegen.get_filament_config_index(old_filament_id) : 0;
                size_t new_fi = gcodegen.get_filament_config_index(new_filament_id);

                float outer_wall_volumetric_speed = get_outer_wall_volumetric_speed(full_config, *gcodegen.m_print, new_filament_id, (int)new_fi, gcodegen.get_extruder_id(new_filament_id));
                config.set_key_value("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_volumetric_speed));

                float old_retract_length = (old_filament_id != -1) ? full_config.retraction_length.get_at(old_fi) : 0;
                float new_retract_length = full_config.retraction_length.get_at(new_fi);
                float old_retract_length_toolchange = (old_filament_id != -1) ? full_config.retract_length_toolchange.get_at(old_fi) : 0;
                float new_retract_length_toolchange = full_config.retract_length_toolchange.get_at(new_fi);
                int old_filament_temp = (old_filament_id != -1) ? (gcodegen.on_first_layer()? full_config.nozzle_temperature_initial_layer.get_at(old_fi) : full_config.nozzle_temperature.get_at(old_fi)) : 210;
                int new_filament_temp = gcodegen.on_first_layer() ? full_config.nozzle_temperature_initial_layer.get_at(new_fi) : full_config.nozzle_temperature.get_at(new_fi);
                Vec3d nozzle_pos = gcode_writer.get_position();

                float purge_volume = tcr.purge_volume < EPSILON ? 0 : std::max(tcr.purge_volume, g_min_purge_volume);
                float filament_area = float((M_PI / 4.f) * pow(full_config.filament_diameter.get_at(new_filament_id), 2));
                float purge_length = purge_volume / filament_area;

                int old_filament_e_feedrate = (old_filament_id != -1) ? (int)(60.0 * full_config.filament_max_volumetric_speed.get_at(old_fi) / filament_area) : 200;
                old_filament_e_feedrate = old_filament_e_feedrate == 0 ? 100 : old_filament_e_feedrate;
                int new_filament_e_feedrate = (int)(60.0 * full_config.filament_max_volumetric_speed.get_at(new_fi) / filament_area);
                new_filament_e_feedrate = new_filament_e_feedrate == 0 ? 100 : new_filament_e_feedrate;
                float wipe_avoid_pos_x      = 0.f;
                {
                    Vec2f box_min = transform_wt_pt(m_wipe_tower_bbx.min.cast<float>());
                    Vec2f box_max = transform_wt_pt(m_wipe_tower_bbx.max.cast<float>());
                    wipe_avoid_pos_x = get_wipe_avoid_pos_x(box_min, box_max, 3.f);
                }

                Vec2f stop_pos = tool_change_start_pos;
                {
                    BoundingBoxf bbx = m_wipe_tower_bbx;
                    bbx.translate((m_wipe_tower_pos + m_rib_offset).cast<double>());
                    stop_pos.x() += (stop_pos.x() < bbx.center().x()) ? -2.f : 2.f;
                    auto printer_bbx = unscaled(get_extents(gcodegen.m_print->get_extruder_shared_printable_polygon()));
                    if (stop_pos.x() < printer_bbx.min[0]) stop_pos.x() = float(printer_bbx.min[0]);
                    if (stop_pos.x() > printer_bbx.max[0]) stop_pos.x() = float(printer_bbx.max[0]);
                }
                config.set_key_value("wipe_tower_center_pos_x", new ConfigOptionFloat(stop_pos.x()));
                config.set_key_value("wipe_tower_center_pos_y", new ConfigOptionFloat(stop_pos.y()));
                config.set_key_value("wipe_tower_center_pos_valid", new ConfigOptionBool(true));

                config.set_key_value("max_layer_z", new ConfigOptionFloat(gcodegen.m_max_layer_z));
                config.set_key_value("relative_e_axis", new ConfigOptionBool(full_config.use_relative_e_distances));
                config.set_key_value("toolchange_count", new ConfigOptionInt((int) gcodegen.m_toolchange_count + 1));
                config.set_key_value("fan_speed", new ConfigOptionInt((int) 0));
                config.set_key_value("old_retract_length", new ConfigOptionFloat(old_retract_length));
                config.set_key_value("new_retract_length", new ConfigOptionFloat(new_retract_length));
                config.set_key_value("filament_retract_length_nc", new ConfigOptionFloat(
                    (old_filament_id != -1) ? (float) full_config.filament_retract_length_nc.get_at(old_fi) : 0.f));
                config.set_key_value("old_retract_length_toolchange", new ConfigOptionFloat(old_retract_length_toolchange));
                config.set_key_value("new_retract_length_toolchange", new ConfigOptionFloat(new_retract_length_toolchange));
                config.set_key_value("new_extruder_retracted_length",
                    new ConfigOptionFloat(gcode_writer.get_extruder_retracted_length((int) new_filament_id)));
                config.set_key_value("old_filament_temp", new ConfigOptionInt(old_filament_temp));
                int interface_temp = full_config.filament_tower_interface_print_temp.get_at(new_filament_id);
                if (interface_temp == -1)
                    interface_temp = full_config.nozzle_temperature_range_high.get_at(new_fi);
                if (full_config.enable_tower_interface_features && tcr.is_contact)
                    new_filament_temp = interface_temp;
                config.set_key_value("new_filament_temp", new ConfigOptionInt(new_filament_temp));
                if (full_config.enable_tower_interface_features && tcr.is_contact) {
                    size_t num_filaments = full_config.filament_type.values.size();
                    std::vector<int> temps(num_filaments);
                    std::vector<int> first_layer_temps(num_filaments);
                    for (size_t i = 0; i < num_filaments; ++i) {
                        size_t fi_i = gcodegen.get_filament_config_index((int)i);
                        temps[i]             = full_config.nozzle_temperature.get_at(fi_i);
                        first_layer_temps[i] = full_config.nozzle_temperature_initial_layer.get_at(fi_i);
                    }
                    if (new_filament_id >= 0 && new_filament_id < (int)temps.size())
                        temps[new_filament_id] = interface_temp;
                    config.set_key_value("temperature", new ConfigOptionInts(temps));
                    if (new_filament_id >= 0 && new_filament_id < (int)first_layer_temps.size())
                        first_layer_temps[new_filament_id] = interface_temp;
                    config.set_key_value("first_layer_temperature", new ConfigOptionInts(first_layer_temps));
                }
                config.set_key_value("x_after_toolchange", new ConfigOptionFloat(tool_change_start_pos(0)));
                config.set_key_value("y_after_toolchange", new ConfigOptionFloat(tool_change_start_pos(1)));
                const double return_z = gcodegen.m_nonplanar_ceiling > 0. && !tcr.priming ?
                    std::max({ nozzle_pos(2), gcodegen.m_nonplanar_ceiling,
                               double(gcodegen.m_max_layer_z) + full_config.z_offset.value }) + gcodegen.nonplanar_head().margin : nozzle_pos(2);
                config.set_key_value("z_after_toolchange", new ConfigOptionFloat(return_z));
                config.set_key_value("first_flush_volume", new ConfigOptionFloat(purge_length / 2.f));
                config.set_key_value("second_flush_volume", new ConfigOptionFloat(purge_length / 2.f));
                config.set_key_value("old_filament_e_feedrate", new ConfigOptionInt(old_filament_e_feedrate));
                config.set_key_value("new_filament_e_feedrate", new ConfigOptionInt(new_filament_e_feedrate));
                config.set_key_value("travel_point_1_x", new ConfigOptionFloat(float(travel_point_1.x())));
                config.set_key_value("travel_point_1_y", new ConfigOptionFloat(float(travel_point_1.y())));
                config.set_key_value("travel_point_2_x", new ConfigOptionFloat(float(travel_point_2.x())));
                config.set_key_value("travel_point_2_y", new ConfigOptionFloat(float(travel_point_2.y())));
                config.set_key_value("travel_point_3_x", new ConfigOptionFloat(float(travel_point_3.x())));
                config.set_key_value("travel_point_3_y", new ConfigOptionFloat(float(travel_point_3.y())));

                {
                    size_t num_filaments = m_print_config->filament_type.values.size();
                    bool   use_fast_flush = m_print_config->prime_volume_mode == PrimeVolumeMode::pvmFast;
                    std::vector<double> flush_v_speed(num_filaments);
                    std::vector<int>    flush_temps(num_filaments);
                    std::vector<double> filament_cooling_before_tower(num_filaments);
                    for (size_t idx = 0; idx < num_filaments; ++idx) {
                        size_t fi = gcodegen.get_filament_config_index(idx);
                        flush_v_speed[idx] = m_print_config->filament_flush_volumetric_speed.get_at(fi);
                        if (flush_v_speed[idx] == 0)
                            flush_v_speed[idx] = m_print_config->filament_max_volumetric_speed.get_at(fi);
                        flush_temps[idx] = use_fast_flush ? m_print_config->filament_flush_temp_fast.get_at(fi)
                                                          : m_print_config->filament_flush_temp.get_at(fi);
                        if (flush_temps[idx] == 0)
                            flush_temps[idx] = m_print_config->nozzle_temperature_range_high.get_at(fi);
                        filament_cooling_before_tower[idx] = m_print_config->filament_cooling_before_tower.get_at(fi);
                    }
                    if (tcr.is_contact || gcodegen.m_layer_index == 0)
                        std::fill(filament_cooling_before_tower.begin(), filament_cooling_before_tower.end(), 0);
                    config.set_key_value("flush_volumetric_speeds", new ConfigOptionFloats(flush_v_speed));
                    config.set_key_value("flush_temperatures", new ConfigOptionInts(flush_temps));
                    config.set_key_value("filament_cooling_before_tower", new ConfigOptionFloats(filament_cooling_before_tower));
                }
                config.set_key_value("flush_length", new ConfigOptionFloat(purge_length));
                config.set_key_value("wipe_avoid_perimeter", new ConfigOptionBool(is_used_travel_avoid_perimeter));
                config.set_key_value("wipe_avoid_pos_x", new ConfigOptionFloat(wipe_avoid_pos_x));
                config.set_key_value("is_prime_tower_interface", new ConfigOptionBool(tcr.is_contact));
                config.set_key_value("filament_tower_interface_purge_volume", new ConfigOptionFloat(full_config.filament_tower_interface_purge_volume.get_at(new_filament_id)));
                config.set_key_value("filament_tower_interface_print_temp", new ConfigOptionInt(interface_temp));

                int   flush_count = std::min(g_max_flush_count, (int) std::round(purge_volume / g_purge_volume_one_time));
                float flush_unit  = purge_length / flush_count;
                int   flush_idx   = 0;
                for (; flush_idx < flush_count; flush_idx++) {
                    char key_value[64] = {0};
                    snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
                    config.set_key_value(key_value, new ConfigOptionFloat(flush_unit));
                }

                for (; flush_idx < g_max_flush_count; flush_idx++) {
                    char key_value[64] = {0};
                    snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
                    config.set_key_value(key_value, new ConfigOptionFloat(0.f));
                }
            }
            toolchange_gcode_str = gcodegen.placeholder_parser_process("change_filament_gcode", change_filament_gcode, new_filament_id, &config);

            check_add_eol(toolchange_gcode_str);

            {
                check_add_eol(toolchange_gcode_str);
                toolchange_gcode_str += ";_FORCE_RESUME_FAN_SPEED\n";
                gcodegen.writer().set_current_position_clear(false);
                double temp_z_after_tool_change;
                if (GCodeProcessor::get_last_z_from_gcode(toolchange_gcode_str, temp_z_after_tool_change)) {
                    Vec3d pos = gcodegen.writer().get_position();
                    pos(2)    = temp_z_after_tool_change;
                    gcodegen.writer().set_position(pos);
                }
            }
            need_travel_after_change_filament_gcode = true;
        }

        std::string toolchange_command;
        if (tcr.priming || (new_filament_id >= 0 && gcodegen.writer().need_toolchange(new_filament_id)))
            toolchange_command = gcodegen.writer().toolchange(new_filament_id,
                nozzle_id_for_gcode_placeholder(group_result, new_filament_id, new_extruder_id, m_layer_idx));
        if (!custom_gcode_changes_tool(toolchange_gcode_str, gcodegen.writer().toolchange_prefix(), new_filament_id))
            toolchange_gcode_str += toolchange_command;
        else {
        }

        if (need_travel_after_change_filament_gcode) {
            if (!is_used_travel_avoid_perimeter) {
                std::string start_pos_str = gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, tool_change_start_pos + plate_origin_2d), erMixed, "Move to start pos", travel_target_z);
                check_add_eol(start_pos_str);
                wipe_next_start_point_str = start_pos_str;
            } else {
                Vec3f gcode_last_pos;
                GCodeProcessor::get_last_position_from_gcode(toolchange_gcode_str, gcode_last_pos);
                Vec2f       gcode_last_pos2d{gcode_last_pos[0], gcode_last_pos[1]};
                Point       gcode_last_pos2d_object = gcodegen.gcode_to_point(gcode_last_pos2d.cast<double>() + plate_origin_2d.cast<double>());
                Point       start_wipe_pos          = wipe_tower_point_to_object_point(gcodegen, tool_change_start_pos + plate_origin_2d);
                BoundingBox avoid_bbx;
                {
                    avoid_bbx            = scaled(m_wipe_tower_bbx);
                    Polygon avoid_points = avoid_bbx.polygon();
                    for (auto &p : avoid_points.points) {
                        Vec2f pp = transform_wt_pt(unscale(p).cast<float>());
                        p        = wipe_tower_point_to_object_point(gcodegen, pp + plate_origin_2d);
                    }
                    avoid_bbx = BoundingBox(avoid_points.points);
                }
                std::string travel_to_wipe_tower_gcode;
                Polyline    travel_polyline = generate_path_to_wipe_tower(gcode_last_pos2d_object, start_wipe_pos, avoid_bbx, shared_printable_area(gcodegen));

                const double route_z = gcodegen.m_nonplanar_ceiling > 0. ?
                    std::max({ gcodegen.writer().get_position().z(), gcodegen.m_nonplanar_ceiling,
                               double(gcodegen.m_max_layer_z) + gcodegen.config().z_offset.value }) + gcodegen.nonplanar_head().margin : DBL_MAX;
                for (size_t i = 0; i < travel_polyline.points.size(); ++i) {
                    const auto &p = travel_polyline.points[i];
                    if (i == travel_polyline.points.size() - 1) {
                        wipe_next_start_point_str = gcodegen.travel_to(p, erMixed, "Move to start pos", travel_target_z);
                        check_add_eol(wipe_next_start_point_str);
                        break;
                    }
                    travel_to_wipe_tower_gcode += gcodegen.travel_to(p, erMixed, "Move to start pos", route_z);
                    check_add_eol(travel_to_wipe_tower_gcode);
                }
                toolchange_gcode_str += travel_to_wipe_tower_gcode;
                gcodegen.set_last_pos(start_wipe_pos);
            }
        }

        const ConfigOptionBool* has_filament_switcher_opt = gcodegen.m_print->full_print_config().option<ConfigOptionBool>("has_filament_switcher");
        bool is_contact_pre_extrusion = tcr.is_contact && gcodegen.m_config.enable_tower_interface_features;
        bool is_petg_pre_extrusion    = !is_contact_pre_extrusion
                                        && gcodegen.config().filament_type.get_at(tcr.new_tool) == "PETG"
                                        && has_filament_switcher_opt && has_filament_switcher_opt->value;
        float extra_unretract = 0.f;
        if (is_contact_pre_extrusion)
            extra_unretract = gcodegen.m_config.filament_tower_interface_pre_extrusion_length.get_at(tcr.new_tool);
        else if (is_petg_pre_extrusion)
            extra_unretract = 2.f;
        std::string toolchange_unretract_str = (extra_unretract > 0.f) ? gcodegen.unretract(extra_unretract) : gcodegen.unretract();
        check_add_eol(toolchange_unretract_str);

        gcodegen.placeholder_parser().set("current_extruder", new_filament_id);
        gcodegen.placeholder_parser().set("current_filament_id", new_filament_id);
        gcodegen.placeholder_parser().set("current_extruder_id", new_extruder_id);
        gcodegen.placeholder_parser().set("current_nozzle_id",
            nozzle_id_for_gcode_placeholder(group_result, new_filament_id, new_extruder_id, m_layer_idx));
        gcodegen.placeholder_parser().set("current_hotend",
            hotend_id_for_gcode_placeholder(group_result, gcodegen.m_print->is_BBL_printer(), new_filament_id, new_extruder_id, m_layer_idx));
        {
            size_t fi = gcodegen.get_filament_config_index(new_filament_id);
            gcodegen.placeholder_parser().set("retraction_distance_when_cut", gcodegen.m_config.retraction_distances_when_cut.get_at(fi));
            gcodegen.placeholder_parser().set("long_retraction_when_cut", gcodegen.m_config.long_retractions_when_cut.get_at(fi));
            gcodegen.placeholder_parser().set("retraction_distance_when_ec", gcodegen.m_config.retraction_distances_when_ec.get_at(fi));
            gcodegen.placeholder_parser().set("long_retraction_when_ec", gcodegen.m_config.long_retractions_when_ec.get_at(fi));
        }

        std::string start_filament_gcode_str;
        const std::string &filament_start_gcode = gcodegen.config().filament_start_gcode.get_at(new_filament_id);
        if (!filament_start_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(new_filament_id));
            config.set_key_value("current_filament_id", new ConfigOptionInt(new_filament_id));
            config.set_key_value("current_nozzle_id", new ConfigOptionInt(nozzle_id_for_gcode_placeholder(group_result, new_filament_id, new_extruder_id, m_layer_idx)));
            config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
            config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
            start_filament_gcode_str = gcodegen.placeholder_parser_process("filament_start_gcode", filament_start_gcode, new_filament_id, &config);
            gcodegen.m_start_gcode_pa[new_filament_id] = Nonplanar::parse_pressure_advance(start_filament_gcode_str);
            if (add_change_filament_624) {
                start_filament_gcode_str += "M625\n";
                add_change_filament_624 = false;
            }
            check_add_eol(start_filament_gcode_str);
        }

        start_filament_gcode_str = start_filament_gcode_str + wipe_next_start_point_str + toolchange_unretract_str;

        if (compacted_below_object) {
            std::string z_descend = gcodegen.writer().travel_to_z(z, "Descend to compacted wipe tower z (no sparse layers)");
            check_add_eol(z_descend);
            start_filament_gcode_str += z_descend;
        }

        DynamicConfig config;
        config.set_key_value("filament_end_gcode", new ConfigOptionString(end_filament_gcode_str));
        config.set_key_value("change_filament_gcode", new ConfigOptionString(toolchange_gcode_str));
        config.set_key_value("filament_start_gcode", new ConfigOptionString(start_filament_gcode_str));
        std::string tcr_gcode, tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_filament_id, &config);
        unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
        gcode += tower_height_tag(gcodegen, tcr, tcr_gcode);
        gcode += tcr_gcode;
        record_nonplanar_material(gcodegen, tcr, z);
        if (custom_gcode_changes_tool(tcr_gcode, gcodegen.writer().toolchange_prefix(), new_filament_id))
            gcodegen.m_toolchange_count++;
        check_add_eol(toolchange_gcode_str);

        gcode += gcodegen.set_filament_pressure_advance(new_filament_id);

        gcodegen.writer().travel_to_xy((end_pos + plate_origin_2d).cast<double>());
        gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos + plate_origin_2d));
        if (!is_approx(z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
            gcode += gcodegen.writer().unretract();
        }

        else {
            gcodegen.m_wipe.reset_path();
            for (const Vec2f &wipe_pt : tcr.wipe_path)
                gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt)));
            gcode += gcodegen.retract(false, false, auto_lift_type, true);
        }

        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        return gcode;
    }

    std::string WipeTowerIntegration::append_tcr2(GCode                             &gcodegen,
                                                  const WipeTower::ToolChangeResult &tcr,
                                                  int                                new_extruder_id,
                                                  double                             z) const
    {
        if (new_extruder_id != -1 && new_extruder_id != tcr.new_tool)
            throw Slic3r::InvalidArgument("Error: WipeTowerIntegration::append_tcr was asked to do a toolchange it didn't expect.");

        std::string gcode;

        float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);

        Vec2f start_pos = tcr.start_pos;
        Vec2f end_pos   = tcr.end_pos;
        if (!tcr.priming) {
            start_pos = transform_wt2_pt(start_pos);
            end_pos   = transform_wt2_pt(end_pos);
        }

        Vec2f wipe_tower_offset   = tcr.priming ? Vec2f::Zero() : Vec2f(m_wipe_tower_pos + Eigen::Rotation2Df(alpha) * m_rib_offset);
        float wipe_tower_rotation = tcr.priming ? 0.f : alpha;
        Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));


        std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

        gcode += prepare_nonplanar_visit(gcodegen, tcr,
            wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d), z);
        const double travel_target_z = !tcr.priming &&
            (gcodegen.m_nonplanar_ceiling > 0. || Nonplanar::requests_conformal_shells(*gcodegen.m_print)) ? z : DBL_MAX;

        gcode += gcodegen.writer().unlift();

        double current_z = gcodegen.writer().get_position().z();


        if (z == -1.)
            z = gcodegen.tower_layer_z();

        const bool needs_toolchange = gcodegen.writer().need_toolchange(new_extruder_id);
        const bool will_go_down     = !is_approx(z, current_z);
        const bool is_ramming       = (gcodegen.config().single_extruder_multi_material) ||
                                (!gcodegen.config().single_extruder_multi_material &&
                                 gcodegen.config().filament_multitool_ramming.get_at(gcodegen.get_filament_config_index(tcr.initial_tool)));
        const bool tool_change_on_wipe_tower = gcodegen.config().tool_change_on_wipe_tower.value;
        const bool should_travel_to_tower = !tcr.priming && (tcr.force_travel
                                                             || !needs_toolchange
                                                             || will_go_down
                                                             || is_ramming
                                                             || tool_change_on_wipe_tower);

        const Point start_wipe_pos     = wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d);
        const bool travel_to_tower_now = should_travel_to_tower || gcodegen.m_need_change_layer_lift_z;
        if (travel_to_tower_now) {
            LiftType lift_type = LiftType::NormalLift;
            if (gcodegen.writer().filament() != nullptr && gcodegen.writer().is_current_position_clear()) {
                ZHopType z_hop_type = ZHopType(gcodegen.config().z_hop_types.get_at(
                    gcodegen.get_filament_config_index((int) gcodegen.writer().filament()->id())));
                if (z_hop_type == ZHopType::zhtAuto)
                    z_hop_type = ZHopType::zhtSpiral;
                lift_type = gcodegen.to_lift_type(z_hop_type);
            }
            gcode += gcodegen.retract(false, false, lift_type);
            gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
            if (!tcr.priming && gcodegen.last_pos_defined())
                gcode += travel_to_tower_gap(gcodegen, gcodegen.last_pos(), start_wipe_pos);
            gcode += gcodegen.travel_to(start_wipe_pos, erMixed, "Travel to a Wipe Tower", travel_target_z);
            gcode += gcodegen.unretract();
        } else {
        }

        if (will_go_down) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
            gcode += gcodegen.writer().unretract();
        }

        std::string toolchange_gcode_str;
        std::string deretraction_str;
        int toolchange_temp_override = -1;
        int interface_temp = -1;
        if (tcr.priming || (new_extruder_id >= 0 && needs_toolchange)) {
            if (is_ramming)
                gcodegen.m_wipe.reset_path();
            if (gcodegen.config().enable_tower_interface_features && tcr.is_contact) {
                interface_temp = gcodegen.config().filament_tower_interface_print_temp.get_at(new_extruder_id);
                if (interface_temp == -1)
                    interface_temp = gcodegen.config().nozzle_temperature_range_high.get_at(gcodegen.get_filament_config_index(new_extruder_id));
                toolchange_temp_override = interface_temp;
            }
            toolchange_gcode_str = gcodegen.set_extruder(new_extruder_id, tcr.print_z, false, toolchange_temp_override,
                                                         WipeTower2::wait_for_temp_enabled(gcodegen.m_config));
            if (!travel_to_tower_now && !tcr.priming && WipeTower2::use_gap_wall(gcodegen.m_config)) {
                Vec3f last_gcode_pos = gcodegen.writer().get_position().cast<float>();
                Point route_start;
                bool  have_start = false;
                if (GCodeProcessor::get_last_position_from_gcode(toolchange_gcode_str, last_gcode_pos)) {
                    route_start = gcodegen.gcode_to_point(Vec2d(last_gcode_pos.x(), last_gcode_pos.y()) + plate_origin_2d.cast<double>());
                    have_start  = true;
                } else if (gcodegen.last_pos_defined()) {
                    route_start = gcodegen.last_pos();
                    have_start  = true;
                }
                if (have_start) {
                    gcodegen.set_last_pos(route_start);
                    gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
                    std::string travel = travel_to_tower_gap(gcodegen, route_start, start_wipe_pos);
                    travel += gcodegen.travel_to(start_wipe_pos, erMixed, "Travel to a Wipe Tower", travel_target_z);
                    check_add_eol(travel);
                    toolchange_gcode_str += travel;
                    gcodegen.set_last_pos(start_wipe_pos);
                }
            }
            if (gcodegen.config().enable_prime_tower) {
                deretraction_str += gcodegen.writer().travel_to_z(z, "Force restore layer Z", true);
                Vec3d position{gcodegen.writer().get_position()};
                position.z() = z;
                gcodegen.writer().set_position(position);
                deretraction_str += gcodegen.unretract();
            }
        }

        if (toolchange_temp_override > 0) {
            size_t new_fi = gcodegen.get_filament_config_index(new_extruder_id);
            int base_temp = gcodegen.on_first_layer() ? gcodegen.config().nozzle_temperature_initial_layer.get_at(new_fi)
                                                      : gcodegen.config().nozzle_temperature.get_at(new_fi);
            if (std::abs(tcr.print_z) < EPSILON)
                base_temp = gcodegen.config().nozzle_temperature_initial_layer.get_at(new_fi);
            const int heater_id = imex_physical_heater_for(
                gcodegen.config().is_imex.value, gcodegen.config().physical_extruder_map, new_extruder_id);
            const std::string t_token = " T" + std::to_string(heater_id);
            std::string out;
            out.reserve(toolchange_gcode_str.size());
            size_t pos = 0;
            while (pos < toolchange_gcode_str.size()) {
                size_t line_end = toolchange_gcode_str.find('\n', pos);
                if (line_end == std::string::npos)
                    line_end = toolchange_gcode_str.size();
                std::string line = toolchange_gcode_str.substr(pos, line_end - pos);
                std::string trimmed = line;
                trimmed.erase(0, trimmed.find_first_not_of(" \t"));
                bool skip_line = false;
                if (boost::starts_with(trimmed, "M109")) {
                    bool matches_extruder = trimmed.find(t_token) != std::string::npos;
                    if (!matches_extruder) {
                        size_t t_pos = trimmed.find('T');
                        if (t_pos != std::string::npos) {
                            size_t t_end = trimmed.find_first_not_of("0123456789", t_pos + 1);
                            const std::string t_val = trimmed.substr(t_pos + 1, t_end == std::string::npos ? std::string::npos : t_end - (t_pos + 1));
                            if (!t_val.empty()) {
                                try {
                                    matches_extruder = std::stoi(t_val) == heater_id;
                                } catch (...) {
                                    matches_extruder = false;
                                }
                            }
                        }
                    }
                    if (matches_extruder) {
                        size_t s_pos = trimmed.find('S');
                        if (s_pos != std::string::npos) {
                            size_t s_end = trimmed.find_first_not_of("0123456789", s_pos + 1);
                            const std::string s_val = trimmed.substr(s_pos + 1, s_end == std::string::npos ? std::string::npos : s_end - (s_pos + 1));
                            if (!s_val.empty()) {
                                try {
                                    skip_line = std::stoi(s_val) == base_temp;
                                } catch (...) {
                                    skip_line = false;
                                }
                            }
                        }
                    }
                }
                if (!skip_line) {
                    out.append(line);
                    if (line_end < toolchange_gcode_str.size())
                        out.push_back('\n');
                }
                pos = line_end + 1;
            }
            toolchange_gcode_str.swap(out);
        }

        if (toolchange_temp_override > 0 && interface_temp > 0) {
            const std::string t_token = " T" + std::to_string(new_extruder_id);
            std::string out;
            out.reserve(tcr_rotated_gcode.size());
            size_t pos = 0;
            while (pos < tcr_rotated_gcode.size()) {
                size_t line_end = tcr_rotated_gcode.find('\n', pos);
                if (line_end == std::string::npos)
                    line_end = tcr_rotated_gcode.size();
                std::string line = tcr_rotated_gcode.substr(pos, line_end - pos);
                std::string trimmed = line;
                trimmed.erase(0, trimmed.find_first_not_of(" \t"));
                bool skip_line = false;
                if (boost::starts_with(trimmed, "M109") && trimmed.find(WipeTower2::wait_for_temp_tag()) == std::string::npos) {
                    bool matches_extruder = true;
                    if (trimmed.find('T') != std::string::npos)
                        matches_extruder = trimmed.find(t_token) != std::string::npos;
                    if (matches_extruder) {
                        size_t s_pos = trimmed.find('S');
                        if (s_pos != std::string::npos) {
                            size_t s_end = trimmed.find_first_not_of("0123456789", s_pos + 1);
                            const std::string s_val = trimmed.substr(s_pos + 1, s_end == std::string::npos ? std::string::npos : s_end - (s_pos + 1));
                            if (!s_val.empty()) {
                                try {
                                    skip_line = std::stoi(s_val) == interface_temp;
                                } catch (...) {
                                    skip_line = false;
                                }
                            }
                        }
                    }
                }
                if (!skip_line) {
                    out.append(line);
                    if (line_end < tcr_rotated_gcode.size())
                        out.push_back('\n');
                }
                pos = line_end + 1;
            }
            tcr_rotated_gcode.swap(out);
        }

        DynamicConfig config;
        config.set_key_value("change_filament_gcode", new ConfigOptionString(toolchange_gcode_str));
        config.set_key_value("deretraction_from_wipe_tower_generator", new ConfigOptionString(deretraction_str));
        config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
        config.set_key_value("toolchange_z", new ConfigOptionFloat(z));

        std::string tcr_gcode,
            tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_extruder_id, &config);
        unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
        gcode += tower_height_tag(gcodegen, tcr, tcr_gcode);
        gcode += tcr_gcode;
        record_nonplanar_material(gcodegen, tcr, z);
        check_add_eol(toolchange_gcode_str);

        if (new_extruder_id != -1)
            gcode += gcodegen.set_filament_pressure_advance(new_extruder_id);

        gcodegen.writer().travel_to_xy((end_pos + plate_origin_2d).cast<double>());
        gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos + plate_origin_2d));
        if (!is_approx(z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
            gcode += gcodegen.writer().unretract();
        }

        else {
            gcodegen.m_wipe.reset_path();
            for (const Vec2f& wipe_pt : tcr.wipe_path)
                gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt2_pt(wipe_pt) + plate_origin_2d));
        }

        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        return gcode;
    }

    std::string WipeTowerIntegration::post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const
    {
        Vec2f extruder_offset;
        if (m_single_extruder_multi_material)
            extruder_offset = m_extruder_offsets[0].cast<float>();
        else
            extruder_offset = m_extruder_offsets[tcr.initial_tool].cast<float>();

        std::istringstream gcode_str(tcr.gcode);
        std::string gcode_out;
        std::string line;
        Vec2f pos = tcr.start_pos;
        auto  trans_pos = [wt_rot = Eigen::Rotation2Df(angle), &translation](const Vec2f& p) -> Vec2f { return wt_rot * p + translation; };
        Vec2f transformed_pos = trans_pos(pos);
        Vec2f old_pos(-1000.1f, -1000.1f);

        while (gcode_str) {
            std::getline(gcode_str, line);
            if (line.find("G1 ") == 0 || line.find("G2 ") == 0 || line.find("G3 ") == 0) {
                std::string cur_gcode_start = line.find("G1 ") == 0 ? "G1 " : (line.find("G2 ") == 0 ? "G2 " : "G3 ");
                bool        never_skip      = false;
                auto        it              = line.find(WipeTower::never_skip_tag());
                if (it != std::string::npos) {
                    never_skip = true;
                    line.erase(it, it + WipeTower::never_skip_tag().size());
                }
                std::ostringstream line_out;
                std::istringstream line_str(line);
                line_str >> std::noskipws;
                char ch = 0;
                while (line_str >> ch) {
                    if (ch == 'X' || ch == 'Y')
                        line_str >> (ch == 'X' ? pos.x() : pos.y());
                    else
                        line_out << ch;
                }
                line = line_out.str();

                transformed_pos = trans_pos(pos);

                if (transformed_pos != old_pos || never_skip) {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3) << cur_gcode_start;
                    if (transformed_pos.x() != old_pos.x() || never_skip)
                        oss << " X" << transformed_pos.x() - extruder_offset.x();
                    if (transformed_pos.y() != old_pos.y() || never_skip)
                        oss << " Y" << transformed_pos.y() - extruder_offset.y();
                    oss << " ";
                    line.replace(line.find(cur_gcode_start), 3, oss.str());
                    old_pos = transformed_pos;
                }
            }

            gcode_out += line + "\n";

            if (line == "[change_filament_gcode]") {
                if (!m_single_extruder_multi_material) {
                    extruder_offset = m_extruder_offsets[tcr.new_tool].cast<float>();

                    if (extruder_offset != m_extruder_offsets[tcr.initial_tool].cast<float>()) {
                        std::ostringstream oss;
                        oss << std::fixed << std::setprecision(3)
                            << "G1 X" << transformed_pos.x() - extruder_offset.x()
                            << " Y" << transformed_pos.y() - extruder_offset.y()
                            << "\n";
                        gcode_out += oss.str();
                    }
                }
                old_pos = Vec2f{-1000.1f, -1000.1f};
                pos     = tcr.tool_change_start_pos;
                transformed_pos = trans_pos(pos);
            }
        }
        return gcode_out;
    }

    std::string WipeTowerIntegration::prime(GCode &gcodegen)
    {
        std::string gcode;
        if (gcodegen.wipe_tower_type() == WipeTowerType::Type2) {
            for (const WipeTower::ToolChangeResult &tcr : m_priming) {
                if (!tcr.extrusions.empty())
                    gcode += append_tcr2(gcodegen, tcr, tcr.new_tool);
            }
        }
        return gcode;
    }

    std::string WipeTowerIntegration::tool_change(GCode &gcodegen, int extruder_id, bool finish_layer)
    {
        std::string gcode;

        assert(m_layer_idx >= 0);
        if (m_layer_idx >= (int) m_tool_changes.size())
            return gcode;
        if (gcodegen.wipe_tower_type() == WipeTowerType::Type2) {
            if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
                if (m_layer_idx < (int) m_tool_changes.size()) {
                    if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                        throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

                    double wipe_tower_z  = -1;
                    bool   ignore_sparse = wipe_tower_layer_is_combined_away(m_tool_changes[m_layer_idx]);
                    if (m_sparse_layers_skipped) {
                        wipe_tower_z  = m_last_wipe_tower_print_z;
                        ignore_sparse = wipe_tower_layer_is_sparse(m_tool_changes[m_layer_idx]) && m_layer_idx != 0;
                        if (m_tool_change_idx == 0 && !ignore_sparse)
                        wipe_tower_z = m_last_wipe_tower_print_z + m_tool_changes[m_layer_idx].front().layer_height;
                    }

                    if (!ignore_sparse) {
                        gcode += append_tcr2(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
                        m_last_wipe_tower_print_z = wipe_tower_z;
                    }
                }
            }
        } else {
            double wipe_tower_z  = -1;
            bool   ignore_sparse = wipe_tower_layer_is_combined_away(m_tool_changes[m_layer_idx]);
            if (m_sparse_layers_skipped) {
                ignore_sparse = wipe_tower_layer_is_sparse(m_tool_changes[m_layer_idx]);
                wipe_tower_z  = m_compacted_tower_z[m_layer_idx];
            }

            if ((m_enable_timelapse_print || m_enable_wrapping_detection) && m_is_first_print) {
                gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][0], m_tool_changes[m_layer_idx][0].new_tool, wipe_tower_z);
                m_tool_change_idx++;
                m_is_first_print = false;
            }

            if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
                if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                    throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

                if (!ignore_sparse)
                    gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
            }
        }

        return gcode;
    }

    bool WipeTowerIntegration::is_empty_wipe_tower_gcode(GCode &gcodegen, int extruder_id, bool finish_layer)
    {
        assert(m_layer_idx >= 0);
        if (m_layer_idx >= (int) m_tool_changes.size())
            return true;

        bool   ignore_sparse = wipe_tower_layer_is_combined_away(m_tool_changes[m_layer_idx]);
        if (m_sparse_layers_skipped)
            ignore_sparse = wipe_tower_layer_is_sparse(m_tool_changes[m_layer_idx]);

        if ((m_enable_timelapse_print || m_enable_wrapping_detection) && m_is_first_print) {
            return false;
        }

        if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
            if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

            if (!ignore_sparse) {
                return false;
            }
        }

        return true;
    }

    std::string WipeTowerIntegration::finalize(GCode &gcodegen)
    {
        std::string gcode;
        if (gcodegen.wipe_tower_type() == WipeTowerType::Type2 && !m_final_purge.gcode.empty()) {
            if (m_sparse_layers_skipped && Nonplanar::requests_conformal_shells(*gcodegen.m_print)) {
                const bool new_layer = !m_tool_changes.empty() && !m_tool_changes.back().empty() &&
                    m_final_purge.print_z > m_tool_changes.back().front().print_z + EPSILON;
                gcode += append_tcr2(gcodegen, m_final_purge, -1,
                    m_last_wipe_tower_print_z + (new_layer ? m_final_purge.layer_height : 0.));
            } else {
                if (std::abs(gcodegen.writer().get_position().z() - m_final_purge.print_z) > EPSILON)
                    gcode += gcodegen.change_layer(m_final_purge.print_z);
                gcode += append_tcr2(gcodegen, m_final_purge, -1);
            }
        }

        return gcode;
    }

    const std::vector<std::string> ColorPrintColors::Colors = { "#C0392B", "#E67E22", "#F1C40F", "#27AE60", "#1ABC9C", "#2980B9", "#9B59B6" };

#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_writer.filament()->extruder_id())
#define FILAMENT_CONFIG(OPT) m_config.OPT.get_at(get_filament_config_index(m_writer.filament()->id()))
#define NOZZLE_CONFIG(OPT) m_config.OPT.get_at(get_nozzle_config_index(m_writer.filament()->id()))

void GCode::PlaceholderParserIntegration::reset()
{
    this->failed_templates.clear();
    this->output_config.clear();
    this->opt_position = nullptr;
    this->opt_zhop      = nullptr;
    this->opt_e_position = nullptr;
    this->opt_e_retracted = nullptr;
    this->opt_e_restart_extra = nullptr;
    this->opt_extruded_volume = nullptr;
    this->opt_extruded_weight = nullptr;
    this->opt_extruded_volume_total = nullptr;
    this->opt_extruded_weight_total = nullptr;
    this->num_extruders = 0;
    this->position.clear();
    this->e_position.clear();
    this->e_retracted.clear();
    this->e_restart_extra.clear();
}

void GCode::PlaceholderParserIntegration::init(const GCodeWriter &writer)
{
    this->reset();
    const std::vector<Extruder> &extruders = writer.extruders();
    if (! extruders.empty()) {
        this->num_extruders = extruders.back().id() + 1;
        this->e_retracted.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->e_restart_extra.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->opt_e_retracted = new ConfigOptionFloats(e_retracted);
        this->opt_e_restart_extra = new ConfigOptionFloats(e_restart_extra);
        this->output_config.set_key_value("e_retracted", this->opt_e_retracted);
        this->output_config.set_key_value("e_restart_extra", this->opt_e_restart_extra);
        if (! writer.config.use_relative_e_distances) {
            e_position.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
            opt_e_position = new ConfigOptionFloats(e_position);
            this->output_config.set_key_value("e_position", opt_e_position);
        }
    }
    this->opt_extruded_volume = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_weight = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_volume_total = new ConfigOptionFloat(0.f);
    this->opt_extruded_weight_total = new ConfigOptionFloat(0.f);
    this->parser.set("extruded_volume", this->opt_extruded_volume);
    this->parser.set("extruded_weight", this->opt_extruded_weight);
    this->parser.set("extruded_volume_total", this->opt_extruded_volume_total);
    this->parser.set("extruded_weight_total", this->opt_extruded_weight_total);

    this->position.assign(3, 0);
    this->opt_position = new ConfigOptionFloats(this->position);
    this->output_config.set_key_value("position", this->opt_position);
    this->opt_zhop = new ConfigOptionFloat(writer.get_zhop());
    this->parser.set("zhop", this->opt_zhop);
}

void GCode::PlaceholderParserIntegration::update_from_gcodewriter(const GCodeWriter &writer)
{
    memcpy(this->position.data(), writer.get_position().data(), sizeof(double) * 3);
    this->opt_position->values = this->position;
    this->opt_zhop->value = writer.get_zhop();

    if (this->num_extruders > 0) {
        const std::vector<Extruder> &extruders = writer.extruders();
        assert(! extruders.empty() && num_extruders == extruders.back().id() + 1);
        this->e_retracted.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->e_restart_extra.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->opt_extruded_volume->values.assign(num_extruders, 0);
        this->opt_extruded_weight->values.assign(num_extruders, 0);
        double total_volume = 0.;
        double total_weight = 0.;
        for (const Extruder &e : extruders) {
            this->e_retracted[e.id()]     = e.retracted();
            this->e_restart_extra[e.id()] = e.restart_extra();
            double v = e.extruded_volume();
            double w = v * e.filament_density() * 0.001;
            this->opt_extruded_volume->values[e.id()] = v;
            this->opt_extruded_weight->values[e.id()] = w;
            total_volume += v;
            total_weight += w;
        }
        opt_extruded_volume_total->value = total_volume;
        opt_extruded_weight_total->value = total_weight;
        opt_e_retracted->values = this->e_retracted;
        opt_e_restart_extra->values = this->e_restart_extra;
        if (! writer.config.use_relative_e_distances) {
            this->e_position.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
            for (const Extruder &e : extruders)
                this->e_position[e.id()] = e.position();
            this->opt_e_position->values = this->e_position;
        }
    }
}

void GCode::PlaceholderParserIntegration::validate_output_vector_variables()
{
    if (this->opt_position->values.size() != 3)
        throw Slic3r::RuntimeError("\"position\" output variable must not be resized by the script.");
    if (this->num_extruders > 0) {
        if (this->opt_e_position && this->opt_e_position->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_position\" output variable must not be resized by the script.");
        if (this->opt_e_retracted->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_retracted\" output variable must not be resized by the script.");
        if (this->opt_e_restart_extra->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_restart_extra\" output variable must not be resized by the script.");
    }
}

static bool belt_object_layer_prints_something(const PrintObject &object, const Layer &layer)
{
    if (layer.has_extrusions())
        return true;
    if (object.has_belt_brim()) {
        const auto  &by_layer = object.belt_brim_by_layer();
        const size_t id       = layer.id();
        if (id < by_layer.size() && ! by_layer[id].empty())
            return true;
    }
    return false;
}

std::vector<GCode::LayerToPrint> GCode::collect_layers_to_print(const PrintObject& object, bool skip_empty_first_layer)
{
    std::vector<GCode::LayerToPrint> layers_to_print;
    layers_to_print.reserve(object.layers().size() + object.support_layers().size());

    std::vector<std::pair<double, double>> warning_ranges;

    size_t idx_object_layer = 0;
    size_t idx_support_layer = 0;
    size_t idx_brim_band = 0;
    const auto &brim_bands = object.belt_brim_prologue();
    const LayerToPrint* last_extrusion_layer = nullptr;
    while (idx_object_layer < object.layers().size() || idx_support_layer < object.support_layers().size()
        || idx_brim_band < brim_bands.size()) {
        LayerToPrint layer_to_print;
        double print_z_min = std::numeric_limits<double>::max();
        if (idx_object_layer < object.layers().size()) {
            layer_to_print.object_layer = object.layers()[idx_object_layer++];
            print_z_min = std::min(print_z_min, layer_to_print.object_layer->print_z);
        }

        if (idx_support_layer < object.support_layers().size()) {
            layer_to_print.support_layer = object.support_layers()[idx_support_layer++];
            print_z_min = std::min(print_z_min, layer_to_print.support_layer->print_z);
        }

        if (idx_brim_band < brim_bands.size()) {
            layer_to_print.belt_brim_band = &brim_bands[idx_brim_band++];
            print_z_min = std::min(print_z_min, layer_to_print.belt_brim_band->print_z);
        }

        if (layer_to_print.object_layer && layer_to_print.object_layer->print_z > print_z_min + EPSILON) {
            layer_to_print.object_layer = nullptr;
            --idx_object_layer;
        }

        if (layer_to_print.support_layer && layer_to_print.support_layer->print_z > print_z_min + EPSILON) {
            layer_to_print.support_layer = nullptr;
            --idx_support_layer;
        }

        if (layer_to_print.belt_brim_band && layer_to_print.belt_brim_band->print_z > print_z_min + EPSILON) {
            layer_to_print.belt_brim_band = nullptr;
            --idx_brim_band;
        }

        layer_to_print.original_object = &object;
        layers_to_print.push_back(layer_to_print);

        bool has_extrusions = (layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            || (layer_to_print.object_layer && layer_to_print.object_layer->conformal_covered)
            || (layer_to_print.support_layer && layer_to_print.support_layer->has_extrusions())
            || (layer_to_print.belt_brim_band && ! layer_to_print.belt_brim_band->fills.empty());

        if (layers_to_print.size() == 1u) {
            if (!has_extrusions && !skip_empty_first_layer)
                throw Slic3r::SlicingError(_(L("An object is floating above the bed with an empty first layer. Drop it flat, cut the bottom, or turn on supports.")), object.id().id);
        }

        if ((layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            || (layer_to_print.support_layer )) {
            double top_cd = object.config().support_top_z_distance;
            double bottom_cd = object.config().support_bottom_z_distance == 0. ? top_cd : object.config().support_bottom_z_distance;
            {
                top_cd    = std::ceil(top_cd / object.config().layer_height) * object.config().layer_height;
                bottom_cd = std::ceil(bottom_cd / object.config().layer_height) * object.config().layer_height;
            }
            double extra_gap = (layer_to_print.support_layer ? bottom_cd : top_cd);

            if (last_extrusion_layer && last_extrusion_layer->support_layer) {
                double raft_gap = object.config().raft_contact_distance.value;
                {
                    raft_gap = std::ceil(raft_gap / object.config().layer_height) * object.config().layer_height;
                }
                extra_gap = std::max(extra_gap, object.config().raft_contact_distance.value);
            }
            double maximal_print_z = (last_extrusion_layer ? last_extrusion_layer->print_z() : 0.)
                + layer_to_print.layer()->height
                + std::max(0., extra_gap);

            if (has_extrusions && layer_to_print.print_z() > maximal_print_z + 2. * EPSILON) {
                const bool belt_leading_gap = object.print()->config().belt_printer.value
                                           && last_extrusion_layer == nullptr;
                if (!belt_leading_gap)
                    warning_ranges.emplace_back(std::make_pair((last_extrusion_layer ? last_extrusion_layer->print_z() : 0.), layers_to_print.back().print_z()));
            }
        }
        if (has_extrusions)
            last_extrusion_layer = &layers_to_print.back();
    }

    if (skip_empty_first_layer && !warning_ranges.empty() && warning_ranges.front().first == 0.)
        warning_ranges.erase(warning_ranges.begin());

    if (! warning_ranges.empty()) {
        std::string warning;
        size_t i = 0;
        for (i = 0; i < std::min(warning_ranges.size(), size_t(5)); ++i)
            warning += Slic3r::format(_(L("The object has empty layers between %1% and %2% and can\u2019t be printed.")),
                                      warning_ranges[i].first, warning_ranges[i].second) + "\n";
        warning += Slic3r::format(_(L("Object: %1%")), object.model_object()->name) + "\n"
            + _(L("The model might be paper-thin at these heights or have a broken mesh."));

        const_cast<Print*>(object.print())->active_step_add_warning(
            PrintStateBase::WarningLevel::CRITICAL, WarningCode{ "empty_layers", warning_object(object.model_object()->name) }, warning, PrintStateBase::SlicingEmptyGcodeLayers);
    }

    if (object.print()->config().belt_printer.value)
        layers_to_print.erase(
            std::remove_if(layers_to_print.begin(), layers_to_print.end(), [&object](const LayerToPrint &ltp) {
                return ! ((ltp.object_layer != nullptr && belt_object_layer_prints_something(object, *ltp.object_layer)) ||
                          (ltp.support_layer != nullptr && ltp.support_layer->has_extrusions()) ||
                          (ltp.belt_brim_band != nullptr && ! ltp.belt_brim_band->fills.empty()));
            }),
            layers_to_print.end());

    return layers_to_print;
}

std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> GCode::collect_layers_to_print(const Print& print)
{
    struct OrderingItem {
        coordf_t    print_z;
        size_t      object_idx;
        size_t      layer_idx;
    };

    std::vector<std::vector<LayerToPrint>>  per_object(print.objects().size(), std::vector<LayerToPrint>());
    std::vector<OrderingItem>               ordering;

    std::vector<Slic3r::SlicingError> errors;

    for (size_t i = 0; i < print.objects().size(); ++i) {
        try {
            per_object[i] = collect_layers_to_print(*print.objects()[i], print.config().belt_printer.value || Nonplanar::nested_field_slice());
        } catch (const Slic3r::SlicingError &e) {
            errors.push_back(e);
            continue;
        }
        if (per_object[i].empty())
            continue;
        OrderingItem ordering_item;
        ordering_item.object_idx = i;
        ordering.reserve(ordering.size() + per_object[i].size());
        const LayerToPrint& front = per_object[i].front();
        for (const LayerToPrint& ltp : per_object[i]) {
            ordering_item.print_z = ltp.print_z();
            ordering_item.layer_idx = &ltp - &front;
            ordering.emplace_back(ordering_item);
        }
    }

    if (!errors.empty()) { throw Slic3r::SlicingErrors(errors); }

    std::sort(ordering.begin(), ordering.end(), [](const OrderingItem& oi1, const OrderingItem& oi2) { return oi1.print_z < oi2.print_z; });

    std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print;

    for (size_t i = 0; i < ordering.size();) {
        size_t j = i + 1;
        coordf_t zmax = ordering[i].print_z + EPSILON;
        for (; j < ordering.size() && ordering[j].print_z <= zmax; ++j);
        std::pair<coordf_t, std::vector<LayerToPrint>> merged;
        merged.first = 0.5 * (ordering[i].print_z + ordering[j - 1].print_z);
        merged.second.assign(print.objects().size(), LayerToPrint());
        for (; i < j; ++i) {
            const OrderingItem& oi = ordering[i];
            assert(merged.second[oi.object_idx].layer() == nullptr);
            merged.second[oi.object_idx] = std::move(per_object[oi.object_idx][oi.layer_idx]);
        }
        layers_to_print.emplace_back(std::move(merged));
    }

    return layers_to_print;
}

static bool pressure_advance_pattern_supported(const CustomGCode::Item &item, const GCodeWriter &writer, const PrintConfig &config)
{
    return !config.belt_printer.value && !writer.has_axis_remap() &&
           item.extra.find("; start pressure advance pattern for layer\n") == 0;
}

static double pressure_advance_pattern_max_z(const CustomGCode::Item &item)
{
    double z = item.print_z, top = 0.;
    GCodeReader reader;
    reader.parse_buffer(item.extra, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (!line.cmd_is("G0") && !line.cmd_is("G1"))
            return;
        const double next_z = line.has_z() ? line.z() : z;
        if (line.has_e() && line.e() > 0 && (line.has_x() || line.has_y()))
            top = std::max(top, std::max(z, next_z));
        z = next_z;
    });
    return top;
}

namespace DoExport {

    static void update_print_estimated_stats(const GCodeProcessor& processor, const std::vector<Extruder>& extruders, PrintStatistics& print_statistics, const PrintConfig& config)
    {
        const GCodeProcessorResult& result = processor.get_result();
        double normal_print_time = result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        print_statistics.estimated_normal_print_time = get_time_dhms(normal_print_time);
        print_statistics.estimated_silent_print_time = processor.is_stealth_time_estimator_enabled() ?
            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) : "N/A";

        double total_extruded_volume = 0.0;
        double total_used_filament   = 0.0;
        double total_weight          = 0.0;
        double total_cost            = 0.0;

        for (auto volume : result.print_statistics.total_volumes_per_extruder) {
            total_extruded_volume += volume.second;

            size_t extruder_id = volume.first;
            auto extruder = std::find_if(extruders.begin(), extruders.end(), [extruder_id](const Extruder& extr) {return extr.id() == extruder_id; });
            if (extruder == extruders.end())
                continue;

            double s = PI * sqr(0.5* extruder->filament_diameter());
            double weight = volume.second * extruder->filament_density() * 0.001;
            total_used_filament += volume.second/s;
            total_weight        += weight;
            total_cost          += weight * extruder->filament_cost() * 0.001;
        }

        total_cost += config.time_cost.getFloat() * (normal_print_time/3600.0);

        print_statistics.total_extruded_volume = total_extruded_volume;
        print_statistics.total_used_filament   = total_used_filament;
        print_statistics.total_weight          = total_weight;
        print_statistics.total_cost            = total_cost;

        print_statistics.filament_stats = result.print_statistics.model_volumes_per_extruder;
    }

    static std::vector<std::pair<std::string, std::string>> validate_custom_gcode(const Print& print) {
        static const unsigned int MAX_TAGS_COUNT = 5;
        std::vector<std::pair<std::string, std::string>> ret;

        auto check = [&ret, is_bbl_printer = print.is_BBL_printer()](const std::string& source, const std::string& gcode) {
            std::vector<std::string> tags;
            if (GCodeProcessor::contains_reserved_tags(gcode, MAX_TAGS_COUNT, tags, is_bbl_printer)) {
                if (!tags.empty()) {
                    size_t i = 0;
                    while (ret.size() < MAX_TAGS_COUNT && i < tags.size()) {
                        ret.push_back({ source, tags[i] });
                        ++i;
                    }
                }
            }
        };

        const GCodeConfig& config = print.config();
        check(_(L("Machine start G-code")), config.machine_start_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Machine end G-code")), config.machine_end_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Before layer change G-code")), config.before_layer_change_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Layer change G-code")), config.layer_change_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Timelapse G-code")), config.time_lapse_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Timelapse G-code")), config.wrapping_detection_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Change filament G-code")), config.change_filament_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Printing by object G-code")), config.printing_by_object_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Change extrusion role G-code")), config.change_extrusion_role_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Process change extrusion role G-code")), config.process_change_extrusion_role_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Pause G-code")), config.machine_pause_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_(L("Template Custom G-code")), config.template_custom_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.filament_start_gcode.values) {
                check(_(L("Filament start G-code")), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.filament_end_gcode.values) {
                check(_(L("Filament end G-code")), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.filament_change_extrusion_role_gcode.values) {
                check(_(L("Filament change extrusion role G-code")), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }

        return ret;
    }
}

bool GCode::is_BBL_Printer()
{
    if (m_curr_print)
        return m_curr_print->is_BBL_printer();
    return false;
}

WipeTowerType GCode::wipe_tower_type()
{
    if (m_curr_print)
        return m_curr_print->wipe_tower_type();
    return WipeTowerType::Type2;
}

static void set_mass_locator(GCodeProcessor &processor, const Print &print)
{
    struct Object
    {
        const PrintObject    *object;
        int                   first_instance;
        size_t                bodies_count;
        int                   first_body;
        std::vector<coordf_t> print_zs;
        std::vector<std::vector<size_t>> bodies;
        std::vector<IslandLocator>       islands;
        std::vector<bool> crowded;
    };
    std::vector<Object>                           objects;
    std::vector<GCodeProcessorResult::ObjectMass> object_masses;
    int                                           bodies_total = 0;
    for (const PrintObject *object : print.objects()) {
        const auto layers = object->layers();
        if (layers.empty())
            continue;
        const ModelVolumePtrs &volumes  = object->model_object()->volumes;
        const bool             assembly = std::count_if(volumes.begin(), volumes.end(), [](const ModelVolume *v) { return v->is_model_part(); }) > 1 ||
                              std::any_of(volumes.begin(), volumes.end(), [](const ModelVolume *v) { return v->is_negative_volume(); });
        size_t                           count = 0;
        std::vector<std::vector<size_t>> bodies;
        if (assembly) {
            count = object->separated_body_bboxes().size();
            if (count > 0 && std::all_of(layers.begin(), layers.end(), [](const Layer *l) { return l->lslices_separated_component_ids.size() == l->lslices.size(); }))
                for (const Layer *layer : layers)
                    bodies.emplace_back(layer->lslices_separated_component_ids);
            else {
                std::vector<const ExPolygons *> islands;
                for (const Layer *layer : layers)
                    islands.emplace_back(&layer->lslices);
                bodies = connected_bodies(islands, count);
            }
        }
        if (count < 2) {
            count = 0;
            bodies.assign(layers.size(), {});
        }
        Object &o = objects.emplace_back(Object{ object, int(object_masses.size()), count, bodies_total, {}, std::move(bodies), {}, {} });
        object_masses.resize(object_masses.size() + object->instances().size());
        for (size_t instance = 0; instance < object->instances().size(); ++instance)
            object_masses[o.first_instance + instance].assembly = assembly;
        bodies_total += int(count * object->instances().size());
        for (const Layer *layer : layers) {
            o.print_zs.emplace_back(layer->print_z);
            o.islands.emplace_back(layer->lslices, scaled<coord_t>(1.));
        }
    }
    if (objects.empty())
        return;
    std::vector<BoundingBox> boxes;
    for (const Object &o : objects) {
        BoundingBox box;
        for (const IslandLocator &islands : o.islands)
            for (const BoundingBox &island : islands.boxes())
                box.merge(island);
        for (const PrintInstance &instance : o.object->instances()) {
            BoundingBox &moved = boxes.emplace_back(box);
            moved.translate(instance.shift);
        }
    }
    for (Object &o : objects)
        for (size_t instance = 0; instance < o.object->instances().size(); ++instance) {
            const size_t i = o.first_instance + instance;
            o.crowded.emplace_back(false);
            for (size_t j = 0; j < boxes.size() && !o.crowded.back(); ++j)
                o.crowded.back() = j != i && boxes[i].overlap(boxes[j]);
        }

    struct Hit
    {
        size_t object{ 0 }, instance{ 0 }, layer{ 0 }, island{ 0 };
    };
    auto locate = [objects = std::move(objects), footprints = std::move(boxes),
                   last = std::optional<Hit>()](const Vec3d &point, bool support) mutable -> GCodeProcessor::MassLocation {
        if (support) {
            const Point p(scaled(point.x()), scaled(point.y()));
            int         found  = -1;
            bool        inside = false;
            double      best   = std::numeric_limits<double>::max();
            for (size_t i = 0; i < footprints.size(); ++i) {
                const BoundingBox &box = footprints[i];
                const double       gap = Point((box.min - p).cwiseMax(p - box.max).cwiseMax(0)).cast<double>().squaredNorm();
                const bool         in  = gap == 0.;
                const double       d   = in ? (box.center() - p).cast<double>().squaredNorm() : gap;
                if ((in && !inside) || (in == inside && d < best)) {
                    found  = int(i);
                    inside = in;
                    best   = d;
                }
            }
            return { found, -1 };
        }
        constexpr double z_tolerance = 0.002;
        const auto       local       = [&point, &objects](size_t object, size_t instance) {
            return Point(Point(scaled(point.x()), scaled(point.y())) - objects[object].object->instances()[instance].shift);
        };
        const auto location = [&objects, &last](const Hit &hit) {
            last            = hit;
            const Object &o = objects[hit.object];
            return GCodeProcessor::MassLocation{ o.first_instance + int(hit.instance),
                                                 o.bodies_count == 0 ? -1 : o.first_body + int(hit.instance * o.bodies_count + o.bodies[hit.layer][hit.island]) };
        };
        if (last) {
            const Object &o = objects[last->object];
            if (point.z() <= o.print_zs[last->layer] + z_tolerance &&
                (last->layer == 0 || point.z() > o.print_zs[last->layer - 1] + z_tolerance) &&
                o.islands[last->layer].holds(last->island, local(last->object, last->instance), o.crowded[last->instance]))
                return location(*last);
        }
        std::optional<Hit> nearest;
        double             distance = std::numeric_limits<double>::max();
        for (size_t object = 0; object < objects.size(); ++object) {
            const Object &o = objects[object];
            const auto    z = std::lower_bound(o.print_zs.begin(), o.print_zs.end(), point.z() - z_tolerance);
            if (z == o.print_zs.end())
                continue;
            const size_t layer = size_t(z - o.print_zs.begin());
            for (size_t instance = 0; instance < o.object->instances().size(); ++instance) {
                const auto [island, d] = o.islands[layer].find(local(object, instance), o.crowded[instance]);
                if (island < 0)
                    continue;
                const Hit hit{ object, instance, layer, size_t(island) };
                if (d == 0. || !o.crowded[instance])
                    return location(hit);
                if (d < distance) {
                    distance = d;
                    nearest  = hit;
                }
            }
        }
        return nearest ? location(*nearest) : GCodeProcessor::MassLocation{};
    };
    processor.set_mass_locator(std::move(locate), std::move(object_masses));
}

void GCode::do_export(Print* print, const char* path, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb, bool retain_raw_positions)
{
    PROFILE_CLEAR();

    m_curr_print = print;
    m_skirt_group_done.clear();

    GCodeWriter::full_gcode_comment = print->config().gcode_comments;
    CNumericLocalesSetter locales_setter;
    if (GCodeWriter::planned_moves != nullptr)
        GCodeWriter::planned_moves->clear();

    if (print->is_step_done(psGCodeExport) && boost::filesystem::exists(boost::filesystem::path(path)))
        return;

    BOOST_LOG_TRIVIAL(info) << boost::format("Will export G-code to %1% soon")%path;

    if (const StringObjectException err = Nonplanar::validate_layer_gcodes(*print, nullptr); ! err.string.empty())
        throw Slic3r::SlicingError(err.string, err.object != nullptr ? err.object->id().id : 0);

    GCodeProcessor::s_IsBBLPrinter = print->is_BBL_printer();
    m_writer.set_is_bbl_machine(print->is_BBL_printer());
    print->set_started(psGCodeExport);

    {
        LifecycleEventContext ctx;
        ctx.id = std::to_string(print->model().id().id);
        ctx.name = print->get_model_name();
        ctx.code = LifecycleEvtCode::Ok;
        ctx.msg  = path;
        ctx.cancellation_check = [print]() { return print->canceled(); };
        fire_lifecycle_event(LifecycleEvent::GCodeExportStarted, ctx);
    }

    std::vector<std::pair<std::string, std::string>> validation_res = DoExport::validate_custom_gcode(*print);
    if (!validation_res.empty()) {
        std::string reports;
        for (const auto& [source, keyword] : validation_res) {
            reports += source + ": \"" + keyword + "\"\n";
        }
        std::string temp = "Dangerous keywords in custom Gcode: " + reports + "\nThis may cause problems in g-code visualization and printing time estimation.";
        BOOST_LOG_TRIVIAL(warning) << temp;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code..." << log_memory_info();

    boost::nowide::remove(path);

    fs::path file_path(path);
    fs::path folder = file_path.parent_path();
    if (!fs::exists(folder)) {
        fs::create_directory(folder);
        BOOST_LOG_TRIVIAL(error) << "[WARNING]: the parent path " + folder.string() +" is not there, create it!" << std::endl;
    }

    static std::atomic<unsigned> tmp_counter{0};
    const std::string path_tmp = std::string(path) + "." + std::to_string(get_current_pid()) + "." + std::to_string(tmp_counter++) + ".tmp";

    m_processor.initialize(path_tmp);
    m_processor.set_print(print);
    if (retain_raw_positions)
        m_processor.set_keep_raw_positions(true);
    GCodeOutputStream file(boost::nowide::fopen(path_tmp.c_str(), "wb"), m_processor);
    if (! file.is_open()) {
        std::string err_msg = std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n";
        BOOST_LOG_TRIVIAL(error) << err_msg << std::endl;
        if (!fs::exists(folder)) {
            std::string add_err_msg = "the parent path " + folder.string() +" is not there!!!";
            BOOST_LOG_TRIVIAL(error) << add_err_msg << std::endl;
            err_msg += add_err_msg;
        }
        {
            LifecycleEventContext ctx;
            ctx.id = std::to_string(print->model().id().id);
            ctx.name = print->get_model_name();
            ctx.code = LifecycleEvtCode::Error;
            ctx.msg  = std::string(path) + "\n" + err_msg;
            ctx.cancellation_check = [print]() { return print->canceled(); };
            fire_lifecycle_event(LifecycleEvent::GCodeExportFinished, ctx);
        }
        throw Slic3r::RuntimeError(err_msg);
    }

    try {
        this->_do_export(*print, file, thumbnail_cb);
        file.flush();
        if (file.is_error()) {
            file.close();
            boost::nowide::remove(path_tmp.c_str());
            throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed\nIs the disk full?\n");
        }
    } catch (std::exception &ex) {
        file.close();
        boost::nowide::remove(path_tmp.c_str());
        {
            LifecycleEventContext ctx;
            ctx.id = std::to_string(print->model().id().id);
            ctx.name = print->get_model_name();
            ctx.code = LifecycleEvtCode::Error;
            ctx.msg  = std::string(path) + "\n" + ex.what();
            ctx.cancellation_check = [print]() { return print->canceled(); };
            fire_lifecycle_event(LifecycleEvent::GCodeExportFinished, ctx);
        }
        throw;
    }
    file.close();
    struct RemoveUnlessRenamed
    {
        const std::string &path;
        bool               renamed = false;
        ~RemoveUnlessRenamed()
        {
            if (!renamed)
                boost::nowide::remove(path.c_str());
        }
    } partial{ path_tmp };

    check_placeholder_parser_failed();

#if ORCA_CHECK_GCODE_PLACEHOLDERS
    if (!m_placeholder_error_messages.empty()){
        std::ostringstream message;
        message << "Some EditGcodeDialog defs were not specified properly. Do so in PrintConfig under SlicingStatesConfigDef:" << std::endl;
        for (const auto& error : m_placeholder_error_messages) {
            message << std::endl << error.first << ": " << std::endl;
            for (const auto& str : error.second)
                message << str << ", ";
            message.seekp(-2, std::ios_base::end);
            message << std::endl;
        }
        throw Slic3r::PlaceholderParserError(message.str());
    }
#endif

    BOOST_LOG_TRIVIAL(debug) << "Start processing gcode, " << log_memory_info();

    m_timelapse_warning_code = 0;
    if (m_config.printer_structure.value == PrinterStructure::psI3 && m_spiral_vase) {
        m_timelapse_warning_code += 1;
    }
    if (m_config.printer_structure.value == PrinterStructure::psI3 && print->config().print_sequence == PrintSequence::ByObject) {
        m_timelapse_warning_code += (1 << 1);
    }
    if (m_config.timelapse_type.value == TimelapseType::tlSmooth && !m_config.enable_prime_tower.value) {
        m_timelapse_warning_code += (1 << 2);
    }
    m_processor.result().timelapse_warning_code = m_timelapse_warning_code;
    m_processor.result().support_traditional_timelapse = m_support_traditional_timelapse;

    bool activate_long_retraction_when_cut = false;
    for (const auto& filament : m_writer.extruders()) {
        size_t fi = get_filament_config_index((int)filament.id());
        activate_long_retraction_when_cut |= (
            m_config.long_retractions_when_cut.get_at(fi)
         && m_config.retraction_distances_when_cut.get_at(fi) > 0
            );
    }

    m_processor.result().long_retraction_when_cut = activate_long_retraction_when_cut;
   
    {
        const ConfigOptionInts *bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key(m_config.curr_bed_type));
        std::vector<int> conflict_filament;
        for(auto extruder_id : m_initial_layer_extruders){
            int cur_bed_temp = bed_temp_opt->get_at(extruder_id);
            if (cur_bed_temp == 0) {
                conflict_filament.push_back(extruder_id);
            }
        }

        m_processor.result().filament_printable_reuslt = FilamentPrintableResult(conflict_filament, bed_type_to_gcode_string(m_config.curr_bed_type));
    }
    int extruder_size = m_print->config().nozzle_diameter.values.size();
    std::vector<Polygons> extruder_unprintable_polys   = m_print->get_extruder_unprintable_polygons();
    Pointfs               plate_printable_area         = m_print->config().printable_area.values;
    Pointfs               wrapping_exclude_area_points = m_print->config().wrapping_exclude_area.values;
    m_processor.check_multi_extruder_gcode_valid(extruder_size, plate_printable_area, m_print->config().printable_height.value, wrapping_exclude_area_points,
                                                 extruder_unprintable_polys, m_print->get_extruder_printable_height(),  m_print->get_filament_maps(),
                                                 m_print->get_physical_unprintable_filaments(m_print->get_slice_used_filaments(false)));

    m_processor.result().nozzle_group_result = m_print->get_layered_nozzle_group_result();

    trim_free_memory();
    m_processor.finalize(true);
    try {
        Nonplanar::ClearanceRepair::check_export(*print, m_processor.result());
    } catch (...) {
        boost::nowide::remove(path_tmp.c_str());
        throw;
    }
    if (std::any_of(print->objects().begin(), print->objects().end(), [](const PrintObject *object) {
            return curved_tops_mode(object->config().nonplanar_mode.value);
        })) {
        std::fstream header(path_tmp, std::ios::in | std::ios::out | std::ios::binary);
        std::string prefix(8192, '\0');
        header.read(prefix.data(), std::streamsize(prefix.size()));
        prefix.resize(size_t(header.gcount()));
        auto counter = [&](const std::string &key, size_t value) {
            const size_t at = prefix.find(key);
            if (at == std::string::npos) throw Slic3r::RuntimeError("Missing curved tops clearance header.");
            char digits[32];
            std::snprintf(digits, sizeof(digits), "%-20zu", value);
            header.clear();
            header.seekp(std::streamoff(at + key.size()));
            header.write(digits, 20);
        };
        counter("; nonplanar_grazes = ", m_processor.result().clearance_report.cone_graze_moves);
        counter("; nonplanar_hits = ", m_processor.result().clearance_report.hit_moves);
    }
    if (Nonplanar::wave_enabled(*print)) {
        const auto &processed = m_processor.result();
        bool printing = false;
        const double ceiling = print->config().printable_height.value;
        for (const auto &move : processed.moves) {
            if (move.timing_only)
                continue;
            printing |= move.type == EMoveType::Extrude && move.extrusion_role != erCustom;
            if (!printing || (move.type != EMoveType::Extrude && move.type != EMoveType::Travel))
                continue;
            if (move.position.z() < -EPSILON || (ceiling > 0. && move.position.z() > ceiling + EPSILON))
                throw Slic3r::SlicingError("Non-planar wave deposition or travel exceeds the printer's Z envelope.");
        }
    }
    std::vector<Nonplanar::WaveResult> wave_results;
    for (size_t i = 0; i < print->objects().size(); ++i) {
        const PrintObject *object = print->objects()[i];
        if (object->config().nonplanar_wave.value == NonplanarWave::Off)
            continue;
        Nonplanar::WaveResult out;
        out.object = i;
        out.mode = object->config().nonplanar_wave.value;
        const auto field = m_wave_fields.find(object);
        out.amplitude = field == m_wave_fields.end() ? 0. : field->second.amplitude();
        out.fallback_reason = print->config().nonplanar_kinematics.value != RotaryLayout::None ? "rotary printer" :
                              out.amplitude <= 0. ? "zero amplitude" :
                              object->layer_count() <= 3 ? "flat base covers object" : "no displaced extrusion roads";
        wave_results.push_back(std::move(out));
    }
    if (!wave_results.empty()) {
        const auto &processed = m_processor.result();
        for (size_t i = 1; i < processed.moves.size(); ++i) {
            const auto &move = processed.moves[i];
            const auto &state = processed.state(move);
            if (move.timing_only || move.type != EMoveType::Extrude || move.delta_extruder <= 0. ||
                state.nonplanar_patch_shell == 0xFF || state.wave_object < 0)
                continue;
            const size_t object_id = size_t(state.wave_object);
            const auto found = std::find_if(wave_results.begin(), wave_results.end(),
                                            [object_id](const auto &out) { return out.object == object_id; });
            if (found == wave_results.end() || !Nonplanar::wave_enabled(*print->objects()[object_id]))
                continue;
            const Vec3f &a = processed.moves[i - 1].position, &b = move.position;
            found->max_bead_height = std::max(found->max_bead_height, double(move.height));
            if (std::abs(b.z() - state.print_z) <= 0.0005 && std::abs(a.z() - state.print_z) <= 0.0005)
                continue;
            ++found->displaced_points;
            found->displaced_length += (b - a).cast<double>().norm();
        }
        Nonplanar::write_wave_verification(path_tmp, processed.clearance_report.cone_graze_moves,
                                           processed.clearance_report.hit_moves, wave_results);
    }
    if (!retain_raw_positions && print->config().nonplanar_kinematics.value == RotaryLayout::None)
        m_processor.result().joints.clear();
    DoExport::update_print_estimated_stats(m_processor, m_writer.extruders(), print->m_print_statistics, print->config());
    if (m_print->config().machine_max_printed_mass.value > EPSILON) {
        double mass_on_bed_total = print->m_print_statistics.total_weight;
        for (auto volume : m_processor.get_result().print_statistics.flush_per_filament) {
            size_t extruder_id = volume.first;
            auto   extruder    = std::find_if(m_writer.extruders().begin(), m_writer.extruders().end(), [extruder_id](const Extruder &extr) { return extr.id() == extruder_id; });
            if (extruder == m_writer.extruders().end()) continue;
            mass_on_bed_total -= (volume.second * extruder->filament_density() * 0.001);
        }
        if (mass_on_bed_total > m_print->config().machine_max_printed_mass.value) {
            m_processor.result().gcode_check_result.error_code |= (1 << 11);
        }
    }
    const bool curved_retry = m_processor.result().clearance_report.hit_moves > 0 &&
        std::any_of(print->objects().begin(), print->objects().end(), [](const PrintObject *object) {
            return curved_tops_mode(object->config().nonplanar_mode.value);
        });
    if (result != nullptr) {
        *result = std::move(m_processor.extract_result());
        result->filename = path;
    }

    BOOST_LOG_TRIVIAL(debug) << boost::format("Finished processing gcode to %1% ") % path_tmp;

    if (curved_retry) {
        boost::nowide::remove(path_tmp.c_str());
        return;
    }
    std::error_code ret = rename_file(path_tmp, path);
    partial.renamed     = !ret;
    if (ret) {
        {
            LifecycleEventContext ctx;
            ctx.id = std::to_string(print->model().id().id);
            ctx.name = print->get_model_name();
            ctx.code = LifecycleEvtCode::Error;
            ctx.msg  = std::string(path) + "\nFailed to rename the output G-code file: " + ret.message();
            ctx.cancellation_check = [print]() { return print->canceled(); };
            fire_lifecycle_event(LifecycleEvent::GCodeExportFinished, ctx);
        }
        throw Slic3r::RuntimeError(
            std::string("Failed to rename the output G-code file from ") + path_tmp + " to " + path + '\n' + "error code " + ret.message() + '\n' +
            "Is " + path_tmp + " locked?" + '\n');
    }
    else {
        BOOST_LOG_TRIVIAL(info) << boost::format("rename_file from %1% to %2% successfully")% path_tmp % path;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code finished" << log_memory_info();
    print->set_done(psGCodeExport);

    {
        LifecycleEventContext ctx;
        ctx.id = std::to_string(print->model().id().id);
        ctx.name = print->get_model_name();
        ctx.code = LifecycleEvtCode::Ok;
        ctx.msg  = path;
        ctx.cancellation_check = [print]() { return print->canceled(); };
        fire_lifecycle_event(LifecycleEvent::GCodeExportFinished, ctx);
    }

    if(result != nullptr)
        result->label_object_enabled = m_enable_exclude_object;
    PROFILE_UPDATE();
    PROFILE_OUTPUT(debug_out_path("gcode-export-profile.txt").c_str());
}

namespace DoExport {
    static void init_gcode_processor(const PrintConfig& config, GCodeProcessor& processor, bool& silent_time_estimator_enabled,
                                     const std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase>& nozzle_group_result = nullptr)
    {
        silent_time_estimator_enabled = (config.gcode_flavor == gcfMarlinLegacy || config.gcode_flavor == gcfMarlinFirmware)
                                        && config.silent_mode;
        processor.reset();
        processor.initialize_from_context(nozzle_group_result);
        processor.initialize_result_moves();
        processor.apply_config(config);
        processor.enable_stealth_time_estimator(silent_time_estimator_enabled);
    }

#if 0
	static double autospeed_volumetric_limit(const Print &print)
	{
	    std::vector<double> mm3_per_mm;
	    for (auto object : print.objects()) {
	        for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
	            const PrintRegion &region = object->printing_region(region_id);
	            for (auto layer : object->layers()) {
	                const LayerRegion* layerm = layer->regions()[region_id];
	                if (region.config().get_abs_value("inner_wall_speed") == 0 ||
	                    region.config().outer_wall_speed.value == 0 ||
	                    region.config().get_abs_value("bridge_speed") == 0)
	                    mm3_per_mm.push_back(layerm->perimeters.min_mm3_per_mm());
	                if (region.config().get_abs_value("sparse_infill_speed") == 0 ||
	                    region.config().get_abs_value("internal_solid_infill_speed") == 0 ||
	                    region.config().get_abs_value("top_surface_speed") == 0 ||
                        region.config().get_abs_value("bridge_speed") == 0)
                    {
                        auto min_mm3_per_mm_no_ironing = [](const ExtrusionEntityCollection& eec) -> double {
                            double min = std::numeric_limits<double>::max();
                            for (const ExtrusionEntity* ee : eec.entities)
                                if (ee->role() != erIroning)
                                    min = std::min(min, ee->min_mm3_per_mm());
                            return min;
                        };

                        mm3_per_mm.push_back(min_mm3_per_mm_no_ironing(layerm->fills));
                    }
	            }
	        }
	        if (object->config().get_abs_value("support_speed") == 0 ||
	            object->config().get_abs_value("support_interface_speed") == 0)
	            for (auto layer : object->support_layers())
	                mm3_per_mm.push_back(layer->support_fills.min_mm3_per_mm());
	    }
	    mm3_per_mm.erase(std::remove_if(mm3_per_mm.begin(), mm3_per_mm.end(), [](double v) { return v < 0.000001; }), mm3_per_mm.end());
	    double volumetric_speed = 0.;
	    if (! mm3_per_mm.empty()) {
	        volumetric_speed = *std::min_element(mm3_per_mm.begin(), mm3_per_mm.end()) * print.config().max_print_speed.value;
	    }
	    return volumetric_speed;
	}
#endif

    static void init_ooze_prevention(const Print &print, OozePrevention &ooze_prevention)
	{
	    ooze_prevention.enable = print.config().ooze_prevention.value && ! print.config().single_extruder_multi_material;
	}

    static int total_toolchanges_from_ordering(const ToolOrdering &tool_ordering)
    {
        int changes = 0;
        int last    = -1;
        for (const LayerTools &lt : tool_ordering)
            for (unsigned int extruder : lt.extruders) {
                if (last >= 0 && int(extruder) != last)
                    ++ changes;
                last = int(extruder);
            }
        return changes;
    }

    static int resolve_total_toolchanges(const WipeTowerData &wipe_tower_data, const ToolOrdering &tool_ordering)
    {
        int changes = wipe_tower_data.number_of_toolchanges;
        if (changes < 0)
            changes = total_toolchanges_from_ordering(tool_ordering);
        return std::max(0, changes);
    }

    static std::string update_print_stats_and_format_filament_stats(
        const bool                   has_wipe_tower,
	    const WipeTowerData         &wipe_tower_data,
	    const std::vector<Extruder> &extruders,
		PrintStatistics 		    &print_statistics,
        const ToolOrdering          &tool_ordering)
    {
		std::string filament_stats_string_out;

	    print_statistics.clear();
        print_statistics.total_toolchanges = resolve_total_toolchanges(wipe_tower_data, tool_ordering);
	    if (! extruders.empty()) {
	        std::pair<std::string, unsigned int> out_filament_used_mm ("; filament used [mm] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_cm3("; filament used [cm3] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_used_g  ("; filament used [g] = ", 0);
	        std::pair<std::string, unsigned int> out_filament_cost    ("; filament cost = ", 0);
	        for (const Extruder &extruder : extruders) {
	            double used_filament   = extruder.used_filament() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] : 0.f);
	            double extruded_volume = extruder.extruded_volume() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] * 2.4052f : 0.f);
	            double filament_weight = extruded_volume * extruder.filament_density() * 0.001;
	            double filament_cost   = filament_weight * extruder.filament_cost()    * 0.001;
                auto append = [&extruder](std::pair<std::string, unsigned int> &dst, const char *tmpl, double value) {
                    assert(is_decimal_separator_point());
	                while (dst.second < extruder.id()) {
	                    dst.first += (dst.second > 0) ? ", 0" : "0";
	                    ++ dst.second;
	                }
	                if (dst.second > 0)
	                    dst.first += ", ";
	                char buf[64];
					sprintf(buf, tmpl, value);
	                dst.first += buf;
	                ++ dst.second;
	            };
	            append(out_filament_used_mm,  "%.2lf", used_filament);
	            append(out_filament_used_cm3, "%.2lf", extruded_volume * 0.001);
	            if (filament_weight > 0.) {
	                print_statistics.total_weight = print_statistics.total_weight + filament_weight;
	                append(out_filament_used_g, "%.2lf", filament_weight);
	                if (filament_cost > 0.) {
	                    print_statistics.total_cost = print_statistics.total_cost + filament_cost;
	                    append(out_filament_cost, "%.2lf", filament_cost);
	                }
	            }
	            print_statistics.total_used_filament += used_filament;
	            print_statistics.total_extruded_volume += extruded_volume;
	            print_statistics.total_wipe_tower_filament += has_wipe_tower ? used_filament - extruder.used_filament() : 0.;
	            print_statistics.total_wipe_tower_cost += has_wipe_tower ? (extruded_volume - extruder.extruded_volume())* extruder.filament_density() * 0.001 * extruder.filament_cost() * 0.001 : 0.;
	        }
	        filament_stats_string_out += out_filament_used_mm.first;
            filament_stats_string_out += "\n" + out_filament_used_cm3.first;
            if (out_filament_used_g.second)
                filament_stats_string_out += "\n" + out_filament_used_g.first;
            if (out_filament_cost.second)
               filament_stats_string_out += "\n" + out_filament_cost.first;
            filament_stats_string_out += "\n";
        }
        return filament_stats_string_out;
    }
}

#if 0
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_z(const Print &print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject *po1, const PrintObject *po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    for (const PrintObject *object : objects)
        for (size_t i = 0; i < object->instances().size(); ++ i)
            instances.emplace_back(&object->instances()[i]);
    return instances;
}
#endif

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order)
{
    auto find_object_index = [](const Model& model, const ModelObject* obj) {
        for (int index = 0; index < model.objects.size(); index++)
        {
            if (model.objects[index] == obj)
                return index;
        }
        return -1;
    };

    std::vector<std::pair<const ModelInstance*, const PrintInstance*>> model_instance_to_print_instance;
    model_instance_to_print_instance.reserve(print.num_object_instances());
    for (const PrintObject *print_object : print.objects())
        for (const PrintInstance &print_instance : print_object->instances())
        {
            if (init_order)
                const_cast<ModelInstance*>(print_instance.model_instance)->arrange_order = find_object_index(print.model(), print_object->model_object());
            model_instance_to_print_instance.emplace_back(print_instance.model_instance, &print_instance);
        }
    std::sort(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), [](auto &l, auto &r) { return l.first->arrange_order < r.first->arrange_order; });
    if (init_order) {
        for (int k = 0; k < model_instance_to_print_instance.size(); k++) {
            const_cast<ModelInstance*>(model_instance_to_print_instance[k].first)->arrange_order = k + 1;
        }
    }

    std::vector<const PrintInstance*> instances;
    instances.reserve(model_instance_to_print_instance.size());
    for (const ModelObject *model_object : print.model().objects)
        for (const ModelInstance *model_instance : model_object->instances) {
            auto it = std::lower_bound(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), std::make_pair(model_instance, nullptr), [](auto &l, auto &r) { return l.first->arrange_order < r.first->arrange_order; });
            if (it != model_instance_to_print_instance.end() && it->first == model_instance)
                instances.emplace_back(it->second);
        }
    std::sort(instances.begin(), instances.end(), [](auto& l, auto& r) { return l->model_instance->arrange_order < r->model_instance->arrange_order; });
    return instances;
}

enum BambuBedType {
    bbtUnknown = 0,
    bbtCoolPlate = 1,
    bbtEngineeringPlate = 2,
    bbtHighTemperaturePlate = 3,
    bbtTexturedPEIPlate         = 4,
    bbtSuperTackPlate = 5,
};

static BambuBedType to_bambu_bed_type(BedType type)
{
    BambuBedType bambu_bed_type = bbtUnknown;
    if (type == btPC)
        bambu_bed_type = bbtCoolPlate;
    else if (type == btEP)
        bambu_bed_type = bbtEngineeringPlate;
    else if (type == btPEI)
        bambu_bed_type = bbtHighTemperaturePlate;
    else if (type == btPTE)
        bambu_bed_type = bbtTexturedPEIPlate;
    else if (type == btPCT)
        bambu_bed_type = bbtCoolPlate;
    else if (type == btSuperTack)
        bambu_bed_type = bbtSuperTackPlate;

    return bambu_bed_type;
}

static std::vector<int> get_imex_active_tools(const Print& print)
{
    std::vector<int> active_tools;

    if (!print.config().is_imex.value || print.objects().empty())
        return active_tools;

    const std::string& raw_mode = print.objects().front()->config().imex_parallel_mode.value;
    const std::string  active_mode = raw_mode.empty() ? kImexPrimaryMode : raw_mode;

    if (active_mode == kImexPrimaryMode)
        return active_tools;

    for (const auto& [phys, role] : parse_imex_active_tools(find_imex_mode(print.config(), active_mode).active_tools))
        active_tools.push_back(phys);
    return active_tools;
}

static bool whole_part_patch(const Nonplanar::ConformalPatch &patch)
{
    return patch.kind == Nonplanar::PatchKind::Volume || patch.kind == Nonplanar::PatchKind::Field;
}

static double level_top(const ExtrusionEntityCollection &level)
{
    double top = -std::numeric_limits<double>::infinity();
    for (const ExtrusionEntity *ee : level.entities)
        if (const auto *path = dynamic_cast<const ExtrusionPathConformal *>(ee))
            for (const Point3 &p : path->polyline.points)
                top = std::max(top, unscale<double>(p.z()));
    return std::isfinite(top) ? top : 0.;
}

static float level_height(const ExtrusionEntityCollection &level)
{
    for (const ExtrusionEntity *ee : level.entities)
        if (const auto *path = dynamic_cast<const ExtrusionPathConformal *>(ee))
            return path->height;
    return 0.f;
}

void GCode::_do_export(Print& print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_FUNC();

    m_print = &print;
    m_timelapse_pos_picker.init(&print,m_writer.get_xy_offset().cast<coord_t>());
    update_layer_related_config(0);

    DoExport::init_gcode_processor(print.config(), m_processor, m_silent_time_estimator_enabled,
                                   print.get_layered_nozzle_group_result());
    set_mass_locator(m_processor, print);
    const bool is_bbl_printers = print.is_BBL_printer();
    const bool skip_config_block = print.config().gcode_skip_config_block;
    const WipeTowerType wipe_tower_type = print.wipe_tower_type();
    m_calib_config.clear();
    switch (print.calib_mode()) {
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_Auto_PA_Line:
    case CalibMode::Calib_Retraction_tower:
        m_calib_config.set_key_value("wipe_inward", new ConfigOptionBool(false));
        break;
    default:
        break;
    }
    m_last_height  = 0.f;
    m_last_layer_z = 0.f;
    m_max_layer_z  = 0.f;
    m_path_task_ids.clear();
    m_deferred_extrusions.clear();
    m_nonplanar_deferral.clear();
    m_nonplanar_heights.clear();
    for (const PrintObject *object : print.objects())
        if (curved_tops_mode(object->config().nonplanar_mode.value)) {
            m_nonplanar_clearance = NonplanarClearanceModel::for_object(*object, object->config().layer_height.value);
            size_t planned_paths = 0;
            const size_t paths = nonplanar_plan_fingerprint(*object);
            const NonplanarDeferral *planned = object->wide_tops_plan(planned_paths);
            if (planned != nullptr && planned_paths != paths) {
                BOOST_LOG_TRIVIAL(warning) << "Nonplanar deposition order: the paths changed after the wide-cap plan; planning again";
                planned = nullptr;
            }
            m_nonplanar_deferral.emplace(object, planned != nullptr ? *planned : plan_nonplanar_deferral_reusing(*object, m_nonplanar_clearance, paths));
        }
    this->queue_advanced_conformal(print);
    m_last_width = 0.f;
    m_last_layer_accumulated_mass = 0.0;
    m_is_role_based_fan_on.fill(false);
    m_role_based_fan_marker_layer.fill(-1);

    m_fan_mover.release();
    m_ordering_cache.clear();

    this->init_belt_writer(print);
    m_writer.set_is_bbl_machine(is_bbl_printers);

    {
        const bool remap = BeltTransformPipeline::axis_remap_enabled(print.config());
        int rx = remap ? int(print.config().gcode_remap_x.value) : int(RemapAxis::PosX);
        int ry = remap ? int(print.config().gcode_remap_y.value) : int(RemapAxis::PosY);
        int rz = remap ? int(print.config().gcode_remap_z.value) : int(RemapAxis::PosZ);
        m_writer.set_axis_remap(rx, ry, rz);
        BoundingBoxf bbox_bed(print.config().printable_area.values);
        m_writer.set_build_volume_max(Vec3d(bbox_bed.max.x(), bbox_bed.max.y(),
                                              print.config().printable_height.value));
    }

    m_nonplanar_ceiling = 0.;
    m_last_native_curved = false;
    m_writer.set_z_travel_cap(0.);
    m_material_columns.reset();
    m_track_nonplanar_material = Nonplanar::wave_enabled(print);
    m_wave_fields.clear();
    for (const PrintObject *object : print.objects())
        if (Nonplanar::wave_enabled(*object))
            m_wave_fields.emplace(object, *object);
    for (const PrintObject *object : print.objects())
        for (size_t i = 0; i < object->num_printing_regions(); ++i) {
            const PrintRegionConfig &config = object->printing_region(i).config();
            m_track_nonplanar_material |= config.zaa_enabled || object->curved_fallback_zaa() || Nonplanar::top_shells_enabled(config, object->config()) ||
                                          Nonplanar::bottom_shells_enabled(config, object->config());
        }
    for (const PrintObject *object : print.objects())
        m_track_nonplanar_material |= !object->conformal().patches.empty();
    if (print.config().nonplanar_kinematics.value != RotaryLayout::None) {
        m_writer.set_tool_kinematics(make_tool_kinematics(print.config()));
        m_writer.set_min_joint_move_time(Nonplanar::full_orientation(print.config()) ? 0. :
            1. / Nonplanar::planner_segment_rate(print.config().nonplanar_segment_rate.value, print.config().gcode_flavor.value, print.is_BBL_printer()));
        if (std::unique_ptr<MachineKinematics> upright = make_upright_machine_kinematics(print.config())) {
            m_writer.set_kinematics(std::move(upright));
            m_writer.set_force_normal_lift(true);
        }
    }

    if (print.config().belt_printer.value) {
        m_writer.set_first_layer_point_test([this](const Vec3d &point_logical) {
            const Vec2d extruder_offset = m_writer.filament() != nullptr ? EXTRUDER_CONFIG(extruder_offset) : Vec2d::Zero();
            const Vec2d plate_offset = m_writer.get_xy_offset().cast<double>();
            return this->on_first_layer(Vec3d(point_logical.x() - (m_origin.x() - plate_offset.x()) + extruder_offset.x(),
                                              point_logical.y() - (m_origin.y() - plate_offset.y()) + extruder_offset.y(),
                                              point_logical.z()));
        });
    }

    m_layer_count = 0;
    const bool belt = print.config().belt_printer.value;
    auto object_layer_counts = [belt](const PrintObject &object, const Layer &layer) {
        if (belt)
            return belt_object_layer_prints_something(object, layer);
        if (object.conformal().patches.empty() || layer.has_extrusions())
            return true;
        for (const std::vector<int> *ids : { &layer.conformal_patches_before, &layer.conformal_patches_after })
            for (int id : *ids)
                if (Nonplanar::patch_has_extrusions(object.conformal().patches[size_t(id)]))
                    return true;
        return false;
    };
    auto curved_levels = [](const PrintObject &object) {
        size_t levels = 0;
        for (const Nonplanar::ConformalPatch &patch : object.conformal().patches)
            if (whole_part_patch(patch))
                for (const ExtrusionEntityCollection &shell : patch.shell_paths)
                    levels += Nonplanar::pass_prints(shell);
        return (unsigned int)(levels * object.instances().size());
    };
    if (print.config().print_sequence == PrintSequence::ByObject) {
        for (auto object : print.objects()) {
            std::vector<coordf_t> zs;
            zs.reserve(object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                if (object_layer_counts(*object, *layer))
                    zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                if (! belt || layer->has_extrusions())
                    zs.push_back(layer->print_z);
            for (const BeltBrimBand &band : object->belt_brim_prologue())
                zs.push_back(band.print_z);
            if (zs.empty())
                continue;
            std::sort(zs.begin(), zs.end());
            auto end_it = std::unique(zs.begin(), zs.end());
            unsigned int temp_layer_count = (unsigned int)(end_it - zs.begin());
            for (auto it = zs.begin(); it != end_it - 1; it++) {
                if (abs(*it - *(it + 1)) < EPSILON)
                    temp_layer_count--;
            }
            m_layer_count += (unsigned int)(object->instances().size() * temp_layer_count) + curved_levels(*object);
        }
    } else {
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                if (object_layer_counts(*object, *layer))
                    zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                if (! belt || layer->has_extrusions())
                    zs.push_back(layer->print_z);
            for (const BeltBrimBand &band : object->belt_brim_prologue())
                zs.push_back(band.print_z);
        }
        if (!zs.empty())
        {
            std::sort(zs.begin(), zs.end());
            auto end_it = std::unique(zs.begin(), zs.end());
            m_layer_count = (unsigned int)(end_it - zs.begin());
            for (auto it = zs.begin(); it != end_it - 1; it++) {
                if (abs(*it - *(it + 1)) < EPSILON)
                    m_layer_count--;
            }
        }
        for (const PrintObject *object : print.objects())
            m_layer_count += curved_levels(*object);
    }
    print.throw_if_canceled();

    m_enable_cooling_markers = true;
    this->apply_print_config(print.config());
    m_config.apply(print.default_object_config());
    m_config.apply(print.default_region_config());

    print.throw_if_canceled();

    if (print.config().spiral_mode.value)
        m_spiral_vase = make_unique<SpiralVase>(print.config());

    if (print.config().max_volumetric_extrusion_rate_slope.value > 0){
    		m_pressure_equalizer = make_unique<PressureEqualizer>(print.config());
    		m_enable_extrusion_role_markers = (bool)m_pressure_equalizer;
    } else
	    m_enable_extrusion_role_markers = false;

    if (m_config.small_area_infill_flow_compensation.value && !m_config.small_area_infill_flow_compensation_model.empty())
        m_small_area_infill_flow_compensator = make_unique<SmallAreaInfillFlowCompensator>(print.config());
    
    m_placeholder_parser_integration.parser = print.placeholder_parser();
    m_placeholder_parser_integration.parser.update_timestamp();
    m_placeholder_parser_integration.parser.update_user_name();
    m_placeholder_parser_integration.context.rng = std::mt19937(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    m_placeholder_parser_integration.context.global_config = std::make_unique<DynamicConfig>();
    print.update_object_placeholders(m_placeholder_parser_integration.parser.config_writable(), ".gcode");

    ToolOrdering tool_ordering;
    unsigned int initial_extruder_id = (unsigned int)-1;
    unsigned int initial_non_support_extruder_id = (unsigned int) -1;
    unsigned int final_extruder_id   = (unsigned int)-1;
    bool         has_wipe_tower      = false;
    print.m_statistics_by_extruder_count.clear();
    std::vector<int>                                    first_filaments;
    std::vector<int>                                    first_non_support_filaments;
    std::vector<const PrintInstance*> 					print_object_instances_ordering;
    std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
    std::vector<const PrintInstance *>::const_iterator  first_has_extrude_print_object;
    first_non_support_filaments.resize(print.config().nozzle_diameter.size(), -1);
    first_filaments.resize(print.config().nozzle_diameter.size(), -1);
    float max_additional_fan = 0.f;
    const auto &seq_dynamic_orderings   = print.sequential_dynamic_orderings();
    const bool  use_seq_dynamic_cache   = print.is_dynamic_group_reorder() && !seq_dynamic_orderings.empty();
    if (print.config().print_sequence == PrintSequence::ByObject) {
        print_object_instances_ordering = sort_object_instances_by_model_order(print);
        print_object_instance_sequential_active = print_object_instances_ordering.begin();
        first_has_extrude_print_object          = print_object_instance_sequential_active;
        bool find_fist_non_support_filament = false;
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
            auto cached_ordering = use_seq_dynamic_cache ? seq_dynamic_orderings.find((*print_object_instance_sequential_active)->print_object) : seq_dynamic_orderings.end();
            if (cached_ordering != seq_dynamic_orderings.end()) {
                tool_ordering = cached_ordering->second;
            } else {
                tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
                tool_ordering.sort_and_build_data(*(*print_object_instance_sequential_active)->print_object,initial_extruder_id);
            }
            float temp_max_additional_fan = tool_ordering.cal_max_additional_fan(print.config());
            if(temp_max_additional_fan > max_additional_fan )
                        max_additional_fan = temp_max_additional_fan;
            if (!find_fist_non_support_filament && tool_ordering.first_extruder() != (unsigned int) -1) {
                if (initial_extruder_id == (unsigned int) -1) {
                    initial_extruder_id = tool_ordering.first_extruder();
                    first_has_extrude_print_object = print_object_instance_sequential_active;
                }

                find_fist_non_support_filament = tool_ordering.cal_non_support_filaments(print.config(), initial_non_support_extruder_id, first_non_support_filaments, first_filaments);
            }
        }
        if (initial_extruder_id == static_cast<unsigned int>(-1))
            throw Slic3r::SlicingError(_(L("The slice is empty. The model may be thinner than one nozzle line.")));
        this->set_extruders(print.extruders());

        has_wipe_tower = print.has_wipe_tower() &&
            (Nonplanar::sequential_shell_tower(print) ? print.wipe_tower_data().tool_ordering.has_wipe_tower() : tool_ordering.has_wipe_tower());
    } else {
        tool_ordering = print.tool_ordering();
        tool_ordering.assign_custom_gcodes(print);
        float temp_max_additional_fan = tool_ordering.cal_max_additional_fan(print.config());
        if(temp_max_additional_fan > max_additional_fan )
                        max_additional_fan = temp_max_additional_fan;
        if (tool_ordering.all_extruders().empty())
            throw Slic3r::SlicingError(_(L("The slice is empty. The model may be thinner than one nozzle line.")));
        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
        initial_extruder_id = (wipe_tower_type == WipeTowerType::Type2 && has_wipe_tower && !print.config().single_extruder_multi_material_priming) ?
            tool_ordering.all_extruders().back() :
            tool_ordering.first_extruder();

        if (initial_extruder_id != static_cast<unsigned int>(-1)) {
            tool_ordering.cal_non_support_filaments(print.config(), initial_non_support_extruder_id, first_non_support_filaments, first_filaments);
        }

        this->set_extruders(tool_ordering.all_extruders());
        print_object_instances_ordering =
            (print.config().print_order == PrintOrder::Default ? chain_print_object_instances(print)
            : (print.config().print_order == PrintOrder::Snake ? chain_print_object_instances_snake(print)
            : (print.config().print_order == PrintOrder::BestOfStrategies ? chain_print_object_instances_best_of(print)
            : sort_object_instances_by_model_order(print))));




    }
    if (initial_extruder_id == (unsigned int)-1) {
        initial_extruder_id = 0;
        initial_non_support_extruder_id = 0;
    }

    if (initial_non_support_extruder_id == (unsigned int) -1)
        initial_non_support_extruder_id = initial_extruder_id;

    print.throw_if_canceled();

    int extruder_id = get_extruder_id(initial_extruder_id);

    m_cooling_buffer = make_unique<CoolingBuffer>(*this);
    m_cooling_buffer->set_current_extruder(initial_extruder_id, extruder_id);

    m_pa_processor = std::make_unique<AdaptivePAProcessor>(*this);

    m_placeholder_parser_integration.init(m_writer);

    auto match_physical_extruder_for_each_filament = [](std::vector<int> &filaments, const FullPrintConfig &config) {
        std::vector<int> physicial_first_filaments;
        physicial_first_filaments.resize(filaments.size());
        for (int extruder_id = 0; extruder_id < filaments.size(); extruder_id++) {
            physicial_first_filaments[config.physical_extruder_map.get_at(extruder_id)] = filaments[extruder_id];
        }
        filaments = physicial_first_filaments;
    };
    match_physical_extruder_for_each_filament(first_filaments, m_config);
    this->placeholder_parser().set("first_tools", new ConfigOptionInts(first_filaments));
    this->placeholder_parser().set("first_filaments", new ConfigOptionInts(first_filaments));
    this->placeholder_parser().set("initial_tool", initial_extruder_id);
    this->placeholder_parser().set("initial_extruder", initial_extruder_id);
    match_physical_extruder_for_each_filament(first_non_support_filaments, m_config);

    auto group_result = m_print->get_layered_nozzle_group_result();
    std::vector<int> first_non_support_hotends;
    first_non_support_hotends.reserve(first_non_support_filaments.size());
    for (int filament_id : first_non_support_filaments)
        first_non_support_hotends.push_back(filament_id < 0 ? -1 :
            first_hotend_id_for_gcode_placeholder(group_result, is_bbl_printers, filament_id, (int) get_extruder_id(filament_id)));

    this->placeholder_parser().set("first_non_support_tools", new ConfigOptionInts(first_non_support_filaments));
    this->placeholder_parser().set("first_non_support_filaments", new ConfigOptionInts(first_non_support_filaments));
    this->placeholder_parser().set("first_non_support_hotend", new ConfigOptionInts(first_non_support_hotends));
    this->placeholder_parser().set("initial_no_support_tool", initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_no_support_extruder", initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_no_support_hotend",
        first_hotend_id_for_gcode_placeholder(group_result, is_bbl_printers, (int) initial_non_support_extruder_id,
                                              (int) get_extruder_id(initial_non_support_extruder_id)));
    this->placeholder_parser().set("current_extruder", initial_extruder_id);
    this->placeholder_parser().set("current_hotend",
        first_hotend_id_for_gcode_placeholder(group_result, is_bbl_printers, (int) initial_extruder_id, extruder_id));
    this->placeholder_parser().set("current_filament_id", (int) initial_extruder_id);
    this->placeholder_parser().set("current_extruder_id", extruder_id);
    this->placeholder_parser().set("current_nozzle_id",
        first_nozzle_id_for_gcode_placeholder(group_result, (int) initial_extruder_id, extruder_id));
    this->placeholder_parser().set("initial_filament_id", (int) initial_extruder_id);
    this->placeholder_parser().set("initial_no_support_filament_id", (int) initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_nozzle_id", first_nozzle_id_for_gcode_placeholder(group_result, (int) initial_extruder_id, extruder_id));
    this->placeholder_parser().set("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
    this->placeholder_parser().set("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
    {
        size_t fi = get_filament_config_index(initial_extruder_id);
        this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(fi));
        this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(fi));
        this->placeholder_parser().set("retraction_distance_when_ec", m_config.retraction_distances_when_ec.get_at(fi));
        this->placeholder_parser().set("long_retraction_when_ec", m_config.long_retractions_when_ec.get_at(fi));
    }
    this->placeholder_parser().set("temperature", new ConfigOptionInts(print.config().nozzle_temperature));

    this->placeholder_parser().set("long_retractions_when_cut",new ConfigOptionBools(m_config.long_retractions_when_cut));
    this->placeholder_parser().set("retraction_distances_when_ec", new ConfigOptionFloatsNullable(m_config.retraction_distances_when_ec));
    this->placeholder_parser().set("long_retractions_when_ec",new ConfigOptionBoolsNullable(m_config.long_retractions_when_ec));

    this->placeholder_parser().set("max_additional_fan", max_additional_fan);
    this->placeholder_parser().set("first_x_layer_fan_speed", new ConfigOptionFloats(m_config.first_x_layer_fan_speed));
    this->placeholder_parser().set("close_additional_fan_first_x_layers", new ConfigOptionInts(m_config.close_additional_fan_first_x_layers));
    this->placeholder_parser().set("additional_fan_full_speed_layer", new ConfigOptionInts(m_config.additional_fan_full_speed_layer));

    this->placeholder_parser().set("total_layer_count", m_layer_count);
    this->placeholder_parser().set("current_object_idx", 0);
    this->placeholder_parser().set("has_wipe_tower", has_wipe_tower);

    Vec2f wipe_tower_center       = Vec2f::Zero();
    bool  wipe_tower_center_valid = false;
    if (has_wipe_tower) {
        BoundingBoxf bbx = print.wipe_tower_data().bbx;
        bbx.translate(print.get_fake_wipe_tower().pos.cast<double>());
        BoundingBoxf printer_bed_bbx(m_config.printable_area.values);
        if (bbx.center().x() < printer_bed_bbx.center().x())
            wipe_tower_center = Vec2f(float(bbx.max.x() + 2.f), float(bbx.center().y()));
        else
            wipe_tower_center = Vec2f(float(bbx.min.x() - 2.f), float(bbx.center().y()));
        auto printer_bbx = unscaled(get_extents(print.get_extruder_shared_printable_polygon()));
        if (wipe_tower_center.x() < printer_bbx.min[0]) wipe_tower_center.x() = float(printer_bbx.min[0]);
        if (wipe_tower_center.x() > printer_bbx.max[0]) wipe_tower_center.x() = float(printer_bbx.max[0]);
        wipe_tower_center_valid = true;
    }
    this->placeholder_parser().set("wipe_tower_center_pos_x", new ConfigOptionFloat(wipe_tower_center.x()));
    this->placeholder_parser().set("wipe_tower_center_pos_y", new ConfigOptionFloat(wipe_tower_center.y()));
    this->placeholder_parser().set("wipe_tower_center_pos_valid", new ConfigOptionBool(wipe_tower_center_valid));
    this->placeholder_parser().set("has_single_extruder_multi_material_priming", wipe_tower_type == WipeTowerType::Type2 && has_wipe_tower && print.config().single_extruder_multi_material_priming);
    this->placeholder_parser().set("total_toolchanges", DoExport::resolve_total_toolchanges(print.wipe_tower_data(), print.tool_ordering()));
    this->placeholder_parser().set("num_extruders", int(print.config().nozzle_diameter.values.size()));
    this->placeholder_parser().set("retract_length", new ConfigOptionFloats(print.config().retraction_length));

    std::vector<unsigned char> is_extruder_used(std::max(size_t(MAXIMUM_EXTRUDER_NUMBER), print.config().filament_diameter.size()), 0);
    for (unsigned int extruder : tool_ordering.all_extruders())
        is_extruder_used[extruder] = true;

    if (print.config().is_imex.value && !print.objects().empty()
        && initial_extruder_id != (unsigned int)-1) {
        const auto plate_head_map = parse_imex_head_filament_map(
            print.objects().front()->config().imex_head_filament_map.value);
        const ConfigOptionInts& pem = print.config().physical_extruder_map;
        const int primary_physical =
            ((int) initial_extruder_id >= 0 &&
             (int) initial_extruder_id < (int) pem.values.size())
                ? pem.values[(int) initial_extruder_id]
                : -1;
        for (int logical : imex_secondary_logical_slots(
                get_imex_active_tools(print), primary_physical, plate_head_map, pem))
            if (logical >= 0 && logical < (int) is_extruder_used.size())
                is_extruder_used[logical] = true;
    }

    this->placeholder_parser().set("is_extruder_used", new ConfigOptionBools(is_extruder_used));

    {
        BoundingBoxf bbox_bed(print.config().printable_area.values);
        Vec2f plate_offset = m_writer.get_xy_offset();
        this->placeholder_parser().set("print_bed_min", new ConfigOptionFloats({ bbox_bed.min.x(), bbox_bed.min.y()}));
        this->placeholder_parser().set("print_bed_max", new ConfigOptionFloats({ bbox_bed.max.x(), bbox_bed.max.y()}));
        this->placeholder_parser().set("print_bed_size", new ConfigOptionFloats({ bbox_bed.size().x(), bbox_bed.size().y() }));

        BoundingBoxf bbox;
        auto pts = std::make_unique<ConfigOptionPoints>();
        if (print.calib_mode() == CalibMode::Calib_PA_Pattern) {
            bbox = bbox_bed;
            bbox.offset(-25.0);
            pts->values.reserve(4);
            pts->values.emplace_back(bbox.min.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.max.y());
            pts->values.emplace_back(bbox.min.x(), bbox.max.y());

        } else if (print.calib_mode() == CalibMode::Calib_PA_Line) {
            CalibPressureAdvanceLine temp_pa_line_forsize(this);
            BoundingBoxf pattern_extents = temp_pa_line_forsize.print_extents(bbox_bed);

            bbox = bbox_bed;
            bbox.offset(-25.0);
            bbox.min.x() = std::max(pattern_extents.min.x(), bbox.min.x());
            bbox.max.x() = std::min(pattern_extents.max.x(), bbox.max.x());
            
            pts->values.reserve(4);
            pts->values.emplace_back(bbox.min.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.max.y());
            pts->values.emplace_back(bbox.min.x(), bbox.max.y());

        } else {
            pts->values.reserve(print.first_layer_convex_hull().size());
            for (const Point &pt : print.first_layer_convex_hull().points)
                pts->values.emplace_back(print.translate_to_print_space(pt));
            bbox = BoundingBoxf((pts->values));
        }
        this->placeholder_parser().set("first_layer_print_convex_hull", pts.release());
        this->placeholder_parser().set("first_layer_print_min", new ConfigOptionFloats({bbox.min.x(), bbox.min.y()}));
        this->placeholder_parser().set("first_layer_print_max", new ConfigOptionFloats({bbox.max.x(), bbox.max.y()}));
        this->placeholder_parser().set("first_layer_print_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));

        {  
            Polygons object_projections;
            for (auto& obj : print.objects()) {
                for (auto& instance : obj->instances()) {
                    const auto& bbox = instance.get_bounding_box();
                    Point min_p{ coord_t(scale_(bbox.min.x())),coord_t(scale_(bbox.min.y())) };
                    Point max_p{ coord_t(scale_(bbox.max.x())),coord_t(scale_(bbox.max.y())) };
                    Polygon instance_projection = {
                        {min_p.x(),min_p.y()},
                        {max_p.x(),min_p.y()},
                        {max_p.x(),max_p.y()},
                        {min_p.x(),max_p.y()}
                    };
                    object_projections.emplace_back(std::move(instance_projection));
                }
            }
            object_projections.emplace_back(print.first_layer_convex_hull());

            Polygons project_polys = union_(object_projections);
            Polygon  head_wrap_detect_zone;
            for (auto& point : print.config().head_wrap_detect_zone.values)
                head_wrap_detect_zone.append(scale_(point).cast<coord_t>() + scale_(plate_offset).cast<coord_t>());

            this->placeholder_parser().set("in_head_wrap_detect_zone", !intersection_pl(project_polys, {head_wrap_detect_zone}).empty());
        }

        {
            coordf_t max_print_z = 0;
            for (auto& obj : print.objects()) {
                max_print_z = std::max(max_print_z, (*std::max_element(obj->layers().begin(), obj->layers().end(), [](Layer* a, Layer* b) { return a->print_z < b->print_z; }))->print_z);
            }
            this->placeholder_parser().set("max_print_z", new ConfigOptionInt(std::ceil(max_print_z)));
        }

        BoundingBoxf mesh_bbox(m_config.bed_mesh_min, m_config.bed_mesh_max);
        auto         mesh_margin = m_config.adaptive_bed_mesh_margin.value;
        mesh_bbox.min            = mesh_bbox.min.cwiseMax((bbox.min.array() - mesh_margin).matrix());
        mesh_bbox.max            = mesh_bbox.max.cwiseMin((bbox.max.array() + mesh_margin).matrix());
        this->placeholder_parser().set("adaptive_bed_mesh_min", new ConfigOptionFloats({mesh_bbox.min.x(), mesh_bbox.min.y()}));
        this->placeholder_parser().set("adaptive_bed_mesh_max", new ConfigOptionFloats({mesh_bbox.max.x(), mesh_bbox.max.y()}));

        auto probe_dist_x  = std::max(1., m_config.bed_mesh_probe_distance.value.x());
        auto probe_dist_y  = std::max(1., m_config.bed_mesh_probe_distance.value.y());
        int  probe_count_x = std::max(3, (int) std::ceil(mesh_bbox.size().x() / probe_dist_x) + 1);
        int  probe_count_y = std::max(3, (int) std::ceil(mesh_bbox.size().y() / probe_dist_y) + 1);
        auto bed_mesh_algo = "bicubic";
        if (probe_count_x * probe_count_y <= 6) {
            bed_mesh_algo = "lagrange";
        }
        else
            if(print.config().gcode_flavor == gcfKlipper){
              probe_count_x = std::max(probe_count_x,4);
              probe_count_y = std::max(probe_count_y,4);
            }
        this->placeholder_parser().set("bed_mesh_probe_count", new ConfigOptionInts({probe_count_x, probe_count_y}));
        this->placeholder_parser().set("bed_mesh_algo", bed_mesh_algo);
        BoundingBoxf bbox_wo_wt;
        for (auto &objPtr : print.objects()) {
            BBoxData data;
            bbox_wo_wt.merge(unscaled(objPtr->get_first_layer_bbox(data.area, data.layer_height, data.name)));
        }
        auto center = bbox_wo_wt.center();
        this->placeholder_parser().set("first_layer_center_no_wipe_tower", new ConfigOptionFloats{ {center.x(),center.y()}});
    }
    bool activate_chamber_temp_control = false;
    auto max_chamber_temp              = 0;
    for (const auto &extruder : m_writer.extruders()) {
        activate_chamber_temp_control |= m_config.activate_chamber_temp_control.get_at(extruder.id());
        max_chamber_temp = std::max(max_chamber_temp, m_config.chamber_temperature.get_at(extruder.id()));
    }
    {
        BedType curr_bed_type = m_config.curr_bed_type;

        int min_temperature_vitrification = std::numeric_limits<int>::max();
        for (const auto& extruder : m_writer.extruders())
            min_temperature_vitrification = std::min(min_temperature_vitrification, m_config.temperature_vitrification.get_at(extruder.id()));


        std::string first_layer_bed_temp_str;
        const ConfigOptionInts* first_bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key((BedType)curr_bed_type));
        const ConfigOptionInts* bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_key((BedType)curr_bed_type));
        int target_bed_temp = 0;
        if (m_config.bed_temperature_formula == BedTempFormula::btfHighestTemp)
            target_bed_temp = get_highest_bed_temperature(true, print);
        else
            target_bed_temp = get_bed_temperature(initial_extruder_id, true, curr_bed_type);

        this->placeholder_parser().set("bbl_bed_temperature_gcode", new ConfigOptionBool(false));
        this->placeholder_parser().set("bed_temperature_initial_layer", new ConfigOptionInts(*first_bed_temp_opt));
        this->placeholder_parser().set("bed_temperature", new ConfigOptionInts(*bed_temp_opt));
        this->placeholder_parser().set("bed_temperature_initial_layer_single", new ConfigOptionInt(target_bed_temp));
        this->placeholder_parser().set("bed_temperature_initial_layer_vector", new ConfigOptionString());
        this->placeholder_parser().set("chamber_temperature", new ConfigOptionInts(m_config.chamber_temperature));
        this->placeholder_parser().set("overall_chamber_temperature", new ConfigOptionInt(max_chamber_temp));
        this->placeholder_parser().set("chamber_minimal_temperature", new ConfigOptionInts(m_config.chamber_minimal_temperature));
        this->placeholder_parser().set("enable_high_low_temp_mix", new ConfigOptionBool(!print.need_check_multi_filaments_compatibility()));
        this->placeholder_parser().set("min_vitrification_temperature", new ConfigOptionInt(min_temperature_vitrification));

        this->placeholder_parser().set("first_layer_bed_temperature", new ConfigOptionInts(*first_bed_temp_opt));
        this->placeholder_parser().set("first_layer_temperature", new ConfigOptionInts(m_config.nozzle_temperature_initial_layer));
        this->placeholder_parser().set("max_print_height",new ConfigOptionInt(m_config.printable_height));
        this->placeholder_parser().set("z_offset", new ConfigOptionFloat(m_config.z_offset));
        this->placeholder_parser().set("model_name", new ConfigOptionString(print.get_model_name()));
        this->placeholder_parser().set("plate_number", new ConfigOptionString(print.get_plate_number_formatted()));
        this->placeholder_parser().set("plate_name", new ConfigOptionString(print.get_plate_name()));
        this->placeholder_parser().set("first_layer_height", new ConfigOptionFloat(m_config.initial_layer_print_height.value));

        auto used_filaments = print.get_slice_used_filaments(false);
        this->placeholder_parser().set("is_all_bbl_filament", std::all_of(used_filaments.begin(), used_filaments.end(), [&](auto idx) {
            return m_config.filament_vendor.get_at(idx) == "Bambu Lab";
            }));

        std::vector<int> during_print_exhaust_fan_speed_num;
        during_print_exhaust_fan_speed_num.reserve(m_config.during_print_exhaust_fan_speed.size());
        for (const auto& item : m_config.during_print_exhaust_fan_speed.values)
            during_print_exhaust_fan_speed_num.emplace_back((int)(item / 100.0 * 255));
        this->placeholder_parser().set("during_print_exhaust_fan_speed_num", new ConfigOptionInts(during_print_exhaust_fan_speed_num));

        float outer_wall_volumetric_speed = get_outer_wall_volumetric_speed(m_config, print, initial_non_support_extruder_id,
                                                                            (int) get_filament_config_index((int) initial_non_support_extruder_id),
                                                                            get_extruder_id(initial_non_support_extruder_id));
        this->placeholder_parser().set("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_volumetric_speed));

        auto first_layer_filaments = print.get_slice_used_filaments(true);
        bool has_tpu_in_first_layer = std::any_of(first_layer_filaments.begin(), first_layer_filaments.end(), [&](unsigned int idx) { return m_config.filament_type.get_at(idx) == "TPU"; });
        this->placeholder_parser().set("has_tpu_in_first_layer", new ConfigOptionBool(has_tpu_in_first_layer));

        if (print.calib_params().mode == CalibMode::Calib_PA_Line) {
            this->placeholder_parser().set("scan_first_layer", new ConfigOptionBool(false));
        }
    }
    {
        double print_area_sum_threshold = 40000.0, pring_hight_threshold = 0.3;

        double   area_sum_temp  = 0.0;
        coordf_t max_hight_temp = -1.0;
        for (ObjectID print_object_ID_t : print.print_object_ids()) {
            const PrintObject *print_object = print.get_object(print_object_ID_t);
            if (!print_object->layers().empty() && print_object->layers().back()->print_z > max_hight_temp) max_hight_temp = print_object->layers().back()->print_z;
            if (!print_object->layers().empty() && print_object->layers().front()->print_z < print.config().initial_layer_print_height + EPSILON &&
                !print_object->layers().front()->lslices.empty()) {
                ExPolygons temp_Expolys = print_object->layers().front()->lslices;
                for (ExPolygon &temp_Expoly : temp_Expolys) { area_sum_temp += temp_Expoly.area(); }
            }
            if (!print_object->support_layers().empty() && print_object->support_layers().front()->print_z < print.config().initial_layer_print_height + EPSILON &&
                !print_object->support_layers().front()->support_islands.empty()) {
                ExPolygons temp_Expolys = print_object->support_layers().front()->support_islands;
                for (ExPolygon &temp_Expoly : temp_Expolys) { area_sum_temp += temp_Expoly.area(); }
            }
            if (print.m_brimMap.find(print_object_ID_t) != print.m_brimMap.end() && !print.m_brimMap.at(print_object_ID_t).entities.empty()) {
                for (const ExtrusionEntity *entities_temp : print.m_brimMap.at(print_object_ID_t).entities) {
                    Polygons temp_Expolys;
                    entities_temp->polygons_covered_by_spacing(temp_Expolys, 0.0f);
                    for (Polygon &temp_Expoly : temp_Expolys) { area_sum_temp += temp_Expoly.area(); }
                }
            }
        }
        if (has_wipe_tower && print.wipe_tower_data().wipe_tower_mesh_data) {
            Polygon temp_Expoly = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
            area_sum_temp += temp_Expoly.area();
        }
        bool hold_chamber_temp_for_flat_print = max_hight_temp > 0 && max_hight_temp < pring_hight_threshold && area_sum_temp > print_area_sum_threshold * 1.0e10;
        this->placeholder_parser().set("hold_chamber_temp_for_flat_print", new ConfigOptionBool(hold_chamber_temp_for_flat_print));
    }

    this->placeholder_parser().set("print_time_total_sec", new ConfigOptionString(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Print_Time_Total_Sec_Placeholder)));
    this->placeholder_parser().set("print_time_day", new ConfigOptionString(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Print_Time_Day_Placeholder)));
    this->placeholder_parser().set("print_time_hour", new ConfigOptionString(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Print_Time_Hour_Placeholder)));
    this->placeholder_parser().set("print_time_minute", new ConfigOptionString(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Print_Time_Minute_Placeholder)));
    this->placeholder_parser().set("print_time_sec", new ConfigOptionString(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Print_Time_Sec_Placeholder)));
    this->placeholder_parser().set("used_filament_length", new ConfigOptionString(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Used_Filament_Length_Placeholder)));

    update_placeholder_parser_with_variant_params();

    std::string imex_active_mode;
    int         imex_active_mode_index = 0;
    std::string imex_active_mode_gcode;
    m_imex_parallel_mode.clear();
    m_imex_head_filament_map.clear();
    if (print.config().is_imex.value && !print.objects().empty()) {
        const std::string& raw = print.objects().front()->config().imex_parallel_mode.value;
        imex_active_mode = raw.empty() ? kImexPrimaryMode : raw;
        m_imex_head_filament_map = parse_imex_head_filament_map(
            print.objects().front()->config().imex_head_filament_map.value);
        std::string imex_active_mode_tools;
        auto resolve_mode = [&](const std::string& name) {
            const ImexMode mode = find_imex_mode(print.config(), name);
            if (!mode.found())
                return false;
            imex_active_mode_index = mode.index;
            imex_active_mode_gcode = mode.gcode;
            imex_active_mode_tools = mode.active_tools;
            return true;
        };
        const bool imex_mode_resolved = resolve_mode(imex_active_mode);
        if (imex_active_mode != kImexPrimaryMode && (!imex_mode_resolved || imex_active_mode_tools.empty())) {
            print.active_step_add_warning(
                PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "imex_mode_fallback" },
                imex_mode_resolved
                    ? Slic3r::format(_(L("The IDEX/IQEX mode \"%1%\" has no tools assigned on the selected printer. "
                                         "Printing in Primary mode instead. Assign the mode's tools in Printer "
                                         "Settings, or pick another mode from the plate's IDEX/IQEX button.")),
                                     imex_active_mode)
                    : Slic3r::format(_(L("This plate is set to the IDEX/IQEX mode \"%1%\", which the selected printer "
                                         "does not define. Printing in Primary mode instead. Pick a mode from the "
                                         "plate's IDEX/IQEX button, or restore the mode in Printer Settings.")),
                                     imex_active_mode));
            imex_active_mode       = kImexPrimaryMode;
            imex_active_mode_index = 0;
            imex_active_mode_gcode.clear();
            imex_active_mode_tools.clear();
            resolve_mode(imex_active_mode);
        }
        m_imex_parallel_mode = imex_active_mode;
    }
    this->placeholder_parser().set("imex_mode",       imex_active_mode);
    this->placeholder_parser().set("imex_mode_index", imex_active_mode_index);
    this->placeholder_parser().set("imex_mode_gcode", imex_active_mode_gcode);

    std::string imex_processed_gcode;
    if (!imex_active_mode_gcode.empty())
        imex_processed_gcode = this->placeholder_parser_process(
            "imex_mode_gcode", imex_active_mode_gcode, initial_extruder_id);
    const bool imported_rotary = m_writer.tool_kinematics().rotary_count() > 0 &&
        std::any_of(print.objects().begin(), print.objects().end(), [](const PrintObject *object) {
            return !object->config().nonplanar_atom_toolpath.value.empty() || object->conformal().ordered_plan;
        });
    m_writer.set_inverse_time_retractions(imported_rotary);
    if (imported_rotary)
        file.write("G90\nG21\nG94\n");
    if (!print.config().file_start_gcode.value.empty())
        file.writeln(this->placeholder_parser_process("file_start_gcode", print.config().file_start_gcode.value, initial_extruder_id));

    std::string thumbnails_value = print.config().option<ConfigOptionString>("thumbnails")->value;
    bool has_BTT_thumbnail = (thumbnails_value.find("BTT_TFT") != std::string::npos);

    if(!has_BTT_thumbnail){
        file.write_format("; HEADER_BLOCK_START\n");
        file.write_format("; generated by OrcaSlicer %s on %s\n", SoftFever_VERSION,
                          Slic3r::Utils::local_timestamp().c_str());
        file.write_format("; pancakeslicer_build = %s\n", Slic3r::nonplanar_build_label().c_str());
        file.write_format("; nonplanar_clearance_mode = %s\n", ConfigOptionEnum<NonplanarMode>(print_clearance_mode(print)).serialize().c_str());
        if (Nonplanar::wave_enabled(print)) {
            for (const auto &[object, field] : m_wave_fields)
                file.write_format("; nonplanar_wave = %s A=%g λ=%g\n", object->config().nonplanar_wave.serialize().c_str(),
                                  field.amplitude(), object->config().nonplanar_wave_wavelength.value);
            file.write_format("; nonplanar_grazes = %-20d\n; nonplanar_hits = %-20d\n", 0, 0);
        }
        for (size_t i = 0; i < print.objects().size(); ++i)
            if (print.objects()[i]->config().nonplanar_wave.value != NonplanarWave::Off) {
                const std::string pending = "{\"object\":" + std::to_string(i) + "}";
                file.write_format("; nonplanar_wave_result = %-768s\n", pending.c_str());
            }
        for (const PrintObject *object : print.objects())
            if (const Nonplanar::OneShotState *oneshot = object->oneshot_state(); oneshot != nullptr && oneshot->solved) {
                file.write_format("; oneshot_student = %s, polish_k = %d\n", oneshot->student.c_str(), oneshot->polish_k);
                if (const std::string offer = "lean offer " + Nonplanar::FieldSolverOffer; oneshot->solved->solver.rfind(offer, 0) == 0)
                    file.write_format("; oneshot_field_solver = %s\n", oneshot->solved->solver.substr(offer.size()).c_str());
                if (oneshot->max_time_ratio_override > 0.)
                    file.write_format("; max_time_ratio_override = %g\n", oneshot->max_time_ratio_override);
            }
        for (const PrintObject *object : print.objects())
            if (!object->native_summary().empty())
                file.write_format("; nonplanar_native = %s: %s\n", object->model_object()->name.c_str(), object->native_summary().c_str());
        if (std::any_of(print.objects().begin(), print.objects().end(), [](const PrintObject *object) {
                return curved_tops_mode(object->config().nonplanar_mode.value);
            })) {
            size_t kept = 0, candidates = 0;
            double footprint = 0., curved_length = 0., upward_area = 0.;
            for (const PrintObject *object : print.objects()) {
                const CurvedTopsStats stats = curved_tops_stats(*object);
                kept += stats.kept;
                candidates += stats.candidates;
                footprint += stats.footprint_mm2;
                curved_length += stats.extrusion_mm;
                upward_area += stats.upward_area_mm2;
            }
            file.write_format("; nonplanar_curved_surfaces = %zu/%zu\n", kept, candidates);
            file.write_format("; nonplanar_curved_footprint_mm2 = %.3f\n", footprint);
            file.write_format("; curved_extrusion_length_mm = %.3f\n", curved_length);
            file.write_format("; curved_top_area_fraction = %.6f\n", upward_area > 0. ? std::clamp(footprint / upward_area, 0., 1.) : 0.);
            file.write_format("; curved_top_area_denominator_mm2 = %.3f\n", upward_area);
            file.write("; curved_top_area_denominator = sum of XY-projected areas of all upward-facing mesh facets\n");
            if (kept == 0)
                file.write("; planar_fallback_reason = no selected surface has curved roads after head clearance and geometric limits\n");
            if (!Nonplanar::wave_enabled(print))
                file.write_format("; nonplanar_grazes = %-20d\n; nonplanar_hits = %-20d\n", 0, 0);
        }
        if (is_bbl_printers)
            file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder).c_str());
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Total_Layer_Number_Placeholder).c_str());
        if (is_bbl_printers) {
            if (print.calib_params().mode == CalibMode::Calib_None) {
                m_enable_exclude_object = true;
                m_label_objects_ids.clear();
                m_label_objects_ids.reserve(print.num_object_instances());
                for (const PrintObject *print_object : print.objects())
                    for (const PrintInstance &print_instance : print_object->instances())
                        m_label_objects_ids.push_back(print_instance.model_instance->get_labeled_id());

                std::sort(m_label_objects_ids.begin(), m_label_objects_ids.end());

                std::string objects_id_list = "; model label id: ";
                for (auto it = m_label_objects_ids.begin(); it != m_label_objects_ids.end(); it++)
                    objects_id_list += (std::to_string(*it) + (it != m_label_objects_ids.end() - 1 ? "," : "\n"));
                file.writeln(objects_id_list);
            } else {
                m_enable_exclude_object = false;
                m_label_objects_ids.clear();
            }
    }

    {
        std::string filament_density_list = "; filament_density: ";
        (filament_density_list+=m_config.filament_density.serialize()) +='\n';
        file.writeln(filament_density_list);

        std::string filament_diameter_list = "; filament_diameter: ";
        (filament_diameter_list += m_config.filament_diameter.serialize()) += '\n';
        file.writeln(filament_diameter_list);

        coordf_t max_height_z = -1;
        for (const auto& object : print.objects()) {
            const Layer *top = object->layers().back();
            max_height_z = std::max(top->print_z, max_height_z);
            if (curved_tops_mode(object->config().nonplanar_mode.value)) {
                const CurvedTopsStats stats = curved_tops_stats(*object);
                if (stats.max_z > 0.) {
                    const double compensation = 0.25 * m_nonplanar_clearance.tip_radius_mm *
                        std::tan(object->config().nonplanar_curved_max_angle.value * M_PI / 180.);
                    max_height_z = std::max(max_height_z, std::ceil((stats.max_z + compensation) * 100. - EPSILON) / 100.);
                }
            }
            if (const auto field = m_wave_fields.find(object); field != m_wave_fields.end())
                max_height_z = std::max(max_height_z, std::ceil((top->print_z + field->second.amplitude()) * 100. - EPSILON) / 100.);
            const bool planar_faces = std::any_of(object->conformal().diagnostics.begin(), object->conformal().diagnostics.end(),
                [](const auto &diag) { return diag.fallback_reason == "planar_patch"; });
            for (const LayerRegion *layerm : top->regions()) {
                const PrintRegionConfig &rcfg = layerm->region().config();
                if ((rcfg.zaa_enabled && rcfg.zaa_mid_layer_slice) || object->curved_fallback_zaa())
                    max_height_z = std::max(max_height_z, std::ceil((top->print_z + 0.5 * top->height + 0.03) * 100. - EPSILON) / 100.);
                if (rcfg.top_texture.value != TopTexture::None)
                    max_height_z = std::max(max_height_z, std::ceil((top->print_z + rcfg.top_texture_depth.value) * 100. - EPSILON) / 100.);
                if (Nonplanar::zaa_fallback(rcfg, object->config()) ||
                    (Nonplanar::top_shells_enabled(rcfg, object->config()) &&
                     Nonplanar::effective_fallback(rcfg, object->config()) == NonplanarFallback::None))
                {
                    const double reach = std::max(rcfg.zaa_min_z.value,
                        planar_faces && !rcfg.zaa_enabled && !object->curved_fallback_zaa() ? 0.5 * top->height : 0.);
                    max_height_z = std::max(max_height_z,
                                            std::ceil((top->print_z + reach + 0.03) * 100. - EPSILON) / 100.);
                }
            }
            for (const Nonplanar::ConformalPatch &patch : object->conformal().patches)
                if (patch.z_max + Nonplanar::GlideLift > max_height_z)
                    max_height_z = std::ceil((patch.z_max + Nonplanar::GlideLift) * 100. - EPSILON) / 100.;
        }
        for (const auto &item : print.model().get_curr_plate_custom_gcodes().gcodes)
            if (pressure_advance_pattern_supported(item, m_writer, print.config()))
                max_height_z = std::max(max_height_z, std::ceil((pressure_advance_pattern_max_z(item) -
                    m_config.z_offset.value) * 100. - EPSILON) / 100.);
        if (has_wipe_tower && Nonplanar::sequential_shell_tower(print))
            for (const std::vector<WipeTower::ToolChangeResult> &layer : print.wipe_tower_data().tool_changes)
                for (const WipeTower::ToolChangeResult &tcr : layer)
                    max_height_z = std::max(max_height_z, coordf_t(tcr.print_z));

        std::ostringstream max_height_z_tip;
        max_height_z_tip<<"; max_z_height: " << std::fixed << std::setprecision(2) << max_height_z << '\n';
        file.writeln(max_height_z_tip.str());
    }

    {
        auto used_filaments = print.get_slice_used_filaments(false);
        std::ostringstream out;
        out << "; filament: ";
        for (size_t idx = 0; idx < used_filaments.size(); ++idx) {
            if (idx != 0)
                out << ',';
            out << used_filaments[idx] + 1;
        }
        file.writeln(out.str());
    }

    file.write_format("; HEADER_BLOCK_END\n\n");
    }

    {
        if (is_bbl_printers && !skip_config_block) {
            file.write("; CONFIG_BLOCK_START\n");
            std::string full_config;
            append_full_config(print, full_config);
            if (!full_config.empty())
                file.write(full_config);

            int first_layer_bed_temperature = get_bed_temperature(0, true, print.config().curr_bed_type);
            file.write_format("; first_layer_bed_temperature = %d\n",
                                first_layer_bed_temperature);
            file.write_format(
                "; first_layer_temperature = %d\n",
                print.config().nozzle_temperature_initial_layer.get_at(0));
            file.write("; CONFIG_BLOCK_END\n\n");
        } else if (thumbnail_cb != nullptr) {
            auto [thumbnails, errors] = GCodeThumbnails::make_and_check_thumbnail_list(print.full_print_config());

            if (errors != enum_bitmask<ThumbnailError>()) {
                std::string error_str = format("Invalid thumbnails value:");
                error_str += GCodeThumbnails::get_error_string(errors);
                throw Slic3r::ExportError(error_str);
            }

            if (!thumbnails.empty())
                GCodeThumbnails::export_thumbnails_to_file(
                    thumbnail_cb, print.get_plate_index(), thumbnails, [&file](const char* sz) { file.write(sz); }, [&print]() { print.throw_if_canceled(); });
        }
    }

    this->write_belt_header(file, print);

    const PrintObject *first_object         = print.objects().front();
    const double       layer_height         = first_object->config().layer_height.value;
    const double       initial_layer_print_height   = print.config().initial_layer_print_height.value;
    for (size_t region_id = 0; region_id < print.num_print_regions(); ++ region_id) {
        const PrintRegion &region = print.get_print_region(region_id);
        file.write_format("; external perimeters extrusion width = %.2fmm\n", region.flow(*first_object, frExternalPerimeter, layer_height).width());
        file.write_format("; perimeters extrusion width = %.2fmm\n",          region.flow(*first_object, frPerimeter,         layer_height).width());
        file.write_format("; infill extrusion width = %.2fmm\n",              region.flow(*first_object, frInfill,            layer_height).width());
        file.write_format("; solid infill extrusion width = %.2fmm\n",        region.flow(*first_object, frSolidInfill,       layer_height).width());
        file.write_format("; top infill extrusion width = %.2fmm\n",          region.flow(*first_object, frTopSolidInfill,    layer_height).width());
        if (print.has_support_material())
            file.write_format("; support material extrusion width = %.2fmm\n", support_material_flow(first_object).width());
        if (print.config().initial_layer_line_width.value > 0)
            file.write_format("; first layer extrusion width = %.2fmm\n",   region.flow(*first_object, frPerimeter, initial_layer_print_height, true).width());
        file.write_format("\n");
    }

    file.write_format("; EXECUTABLE_BLOCK_START\n");

    size_t object_label_id = 0;
    for (PrintObject *object : print.objects())
        object->set_id(object_label_id++);
    if( m_enable_exclude_object)
        file.write(set_object_info(&print));

    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::First_Line_M73_Placeholder).c_str());

    this->print_machine_envelope(file, print);

    if (m_config.auxiliary_fan.value && print.config().close_fan_the_first_x_layers.get_at(initial_extruder_id)) {
        file.write(m_writer.set_fan(0));
        file.write(m_writer.set_additional_fan(0));
    }

    std::string machine_start_gcode = this->placeholder_parser_process("machine_start_gcode", print.config().machine_start_gcode.value, initial_extruder_id);
    m_machine_start_pa = Nonplanar::parse_pressure_advance(machine_start_gcode);
    if (print.config().gcode_flavor != gcfKlipper) {
        this->_print_first_layer_bed_temperature(file, print, machine_start_gcode, initial_extruder_id, true);
        this->_print_first_layer_extruder_temperatures(file, print, machine_start_gcode, initial_extruder_id, false);
    }

    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    if (activate_chamber_temp_control && max_chamber_temp > 0){
        int temp_out =0;
        if(!custom_gcode_sets_temperature(machine_start_gcode,141,191,false,temp_out))
            file.write(m_writer.set_chamber_temperature(max_chamber_temp, true));
    }

    if (!imex_processed_gcode.empty())
        file.writeln(imex_processed_gcode);

    file.writeln(machine_start_gcode);
    if (m_config.enable_pre_heating.value)
        file.write_format(";%s\n", GCodeProcessor::Machine_Start_GCode_End_Tag.c_str());

    m_writer.set_current_position_clear(false);
    m_start_gcode_filament = GCodeProcessor::get_gcode_last_filament(machine_start_gcode);

    if (is_bbl_printers) {
        m_writer.init_extruder(initial_non_support_extruder_id);
        {
            DynamicConfig config;
            config.set_key_value("filament_extruder_id", new ConfigOptionInt((int)(initial_non_support_extruder_id)));
            config.set_key_value("current_filament_id", new ConfigOptionInt((int)(initial_non_support_extruder_id)));
            config.set_key_value("current_extruder_id", new ConfigOptionInt((int) get_extruder_id(initial_non_support_extruder_id)));
            config.set_key_value("current_nozzle_id", new ConfigOptionInt(first_nozzle_id_for_gcode_placeholder(group_result, (int) initial_non_support_extruder_id, (int) get_extruder_id(initial_non_support_extruder_id))));
            config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
            config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            std::string filament_start_gcode = this->placeholder_parser_process("filament_start_gcode", print.config().filament_start_gcode.values.at(initial_non_support_extruder_id), initial_non_support_extruder_id,&config);
            m_start_gcode_pa[initial_non_support_extruder_id] = Nonplanar::parse_pressure_advance(filament_start_gcode);
            file.writeln(filament_start_gcode);
            if (is_multi_nozzle_printer(m_config)) {
                int initial_nozzle_id = -1;
                if (group_result && group_result->is_support_dynamic_nozzle_map()) {
                    auto initial_nozzle = group_result->get_first_nozzle_for_filament(initial_extruder_id);
                    initial_nozzle_id = initial_nozzle ? initial_nozzle->group_id : -1;
                }
                file.write_format(";VT%d H%d\n", initial_extruder_id, initial_nozzle_id);
            } else {
                file.write_format(";VT%d\n", initial_extruder_id);
            }
        }
        file.write(set_filament_pressure_advance(initial_non_support_extruder_id));
    }

    if (!machine_start_gcode.empty() && this->m_fan_mover.get() != nullptr)
        file.write(this->m_fan_mover.get()->process_gcode("", true));

    if (is_bbl_printers) {
        this->_print_first_layer_extruder_temperatures(file, print, machine_start_gcode, initial_extruder_id, true);
    }

    if (m_config.support_air_filtration.value) {
        bool activate_air_filtration_during_print = false;
        int  during_print_exhaust_fan_speed = 0;

        for (const auto &extruder : m_writer.extruders()) {
            size_t fi = get_filament_config_index((int)extruder.id());
            if (m_config.activate_air_filtration.get_at(fi) && m_config.activate_air_filtration_during_print.get_at(fi)) {
                activate_air_filtration_during_print = true;
                during_print_exhaust_fan_speed = std::max(during_print_exhaust_fan_speed,
                                                        m_config.during_print_exhaust_fan_speed.get_at(fi));
            }
        }

        if (activate_air_filtration_during_print)
            file.write(m_writer.set_exhaust_fan(during_print_exhaust_fan_speed));
    }

    print.throw_if_canceled();

    file.write(this->preamble());

    if (imported_rotary) {
        JointPose startup;
        for (size_t i = 0; i < 2; ++i)
            startup.rot[i] = print.config().rotary_axis_home.get_at(i);
        bool relative = false;
        double units = 1.;
        GCodeReader reader;
        reader.parse_buffer(machine_start_gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
            if (ascii_iequals(line.cmd(), "G90")) relative = false;
            else if (ascii_iequals(line.cmd(), "G91")) relative = true;
            else if (ascii_iequals(line.cmd(), "G20")) units = 25.4;
            else if (ascii_iequals(line.cmd(), "G21")) units = 1.;
            else if (ascii_iequals(line.cmd(), "G28")) {
                std::array<bool, 2> rotary_home{};
                for (size_t i = 0; i < 2; ++i) {
                    float v;
                    rotary_home[i] = line.has_value(m_writer.tool_kinematics().rotary_axes()[i].letter, v);
                }
                const bool all = !line.has_x() && !line.has_y() && !line.has_z() && !rotary_home[0] && !rotary_home[1];
                for (int i = 0; i < 3; ++i)
                    if (all || line.has(Axis(i))) startup.xyz[i] = 0.;
                for (size_t i = 0; i < 2; ++i)
                    if (all || rotary_home[i]) startup.rot[i] = print.config().rotary_axis_home.get_at(i);
            } else if (ascii_iequals(line.cmd(), "G92") && (line.has_x() || line.has_y() || line.has_z())) {
                throw SlicingError("Atomizer rotary import: startup must preserve the machine joint datum; G92 may reset E, not XYZ.");
            } else if (ascii_iequals(line.cmd(), "G0") || ascii_iequals(line.cmd(), "G1")) {
                for (int i = 0; i < 3; ++i)
                    if (line.has(Axis(i)))
                        startup.xyz[i] = (relative ? startup.xyz[i] : 0.) + units * line.value(Axis(i));
                for (size_t i = 0; i < 2; ++i)
                    if (float v; line.has_value(m_writer.tool_kinematics().rotary_axes()[i].letter, v))
                        startup.rot[i] = (relative ? startup.rot[i] : 0.) + v;
            }
        });
        m_writer.set_joint_position(startup);
        ToolPose safe{ m_writer.get_position(), m_writer.get_tool_axis() };
        safe.tip.z() = std::max(safe.tip.z(), 5. + m_config.z_offset.value);
        if (safe.axis.dot(Vec3d::UnitZ()) > 1. - EPSILON)
            file.write(m_writer.travel_to_z(safe.tip.z(), "clear the plate after rotary startup", true,
                                           this->conformal_max_speed_z()));
        else
            file.write(m_writer.travel_to_pose(safe, "clear the plate after rotary startup"));
        file.write(m_writer.end_inverse_time());
    }

    DoExport::init_ooze_prevention(print, m_ooze_prevention);
    print.throw_if_canceled();

    std::function<void(void)> throw_if_canceled_func = [&print]() { print.throw_if_canceled(); };
    m_seam_placer.init(print, throw_if_canceled_func);
    if (!m_seam_placer.precise_seam_warning().empty())
        print.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, WarningCode{ "precise_seam" },
                                      m_seam_placer.precise_seam_warning(),
                                      PrintStateBase::SlicingPreciseSeamWarning);

    if (m_writer.multiple_extruders) {
        std::vector<Vec2d> points = get_path_of_change_filament(print);
        if (points.size() == 3) {
            travel_point_1 = points[0];
            travel_point_2 = points[1];
            travel_point_3 = points[2];
        }
    }

    if (wipe_tower_type != WipeTowerType::Type2 || ! (has_wipe_tower && print.config().single_extruder_multi_material_priming))
    {
        file.write(this->set_extruder(initial_extruder_id, 0.));

        if (!m_imex_parallel_mode.empty() && m_imex_parallel_mode != kImexPrimaryMode
            && !m_config.physical_extruder_map.values.empty()) {
            const int initial_physical =
                ((int) initial_extruder_id >= 0 &&
                 (int) initial_extruder_id < (int) m_config.physical_extruder_map.values.size())
                    ? m_config.physical_extruder_map.values[(int) initial_extruder_id]
                    : -1;
            const int num_pa_filament_slots = (int) print.config().filament_diameter.values.size();
            for (int tool_idx : get_imex_active_tools(print)) {
                if (tool_idx == initial_physical) continue;
                const int logical = resolve_filament_for_head(
                    m_imex_head_filament_map, m_config.physical_extruder_map, tool_idx);
                if (logical < 0 || logical >= num_pa_filament_slots) continue;
                const size_t pa_fi = get_filament_config_index(logical);
                if (!print.config().enable_pressure_advance.get_at(pa_fi)) continue;
                file.write(m_writer.set_pressure_advance(
                    print.config().pressure_advance.get_at(pa_fi),
                    tool_idx));
            }
        }
    }

    this->m_objsWithBrim.clear();
    m_brim_done = false;

    for (const Print::SkirtBrimGroup& group : print.skirt_brim_groups()) {
        for (const Print::SkirtBrimGroup::Brim& brim : group.brims) {
            if (brim.brim.empty())
                continue;
            for (const ObjectInstanceID& instance : brim.instances)
                this->m_objsWithBrim.insert(instance);
        }
    }
    if (this->m_objsWithBrim.empty()) m_brim_done = true;

    if (print.calib_params().mode == CalibMode::Calib_PA_Line) {
        std::string gcode;
        gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
        if ((NOZZLE_CONFIG(outer_wall_acceleration) > 0 && NOZZLE_CONFIG(outer_wall_acceleration) > 0)) {
            gcode += m_writer.set_print_acceleration((unsigned int)floor(NOZZLE_CONFIG(outer_wall_acceleration) + 0.5));
        }

        if (NOZZLE_CONFIG(outer_wall_jerk) > 0) {
            double jerk = NOZZLE_CONFIG(outer_wall_jerk);
            gcode += m_writer.set_jerk_xy(jerk);
        }

        auto params = print.calib_params();

        CalibPressureAdvanceLine pa_test(this);

        auto fast_speed = CalibPressureAdvance::find_optimal_PA_speed(print.full_print_config(), pa_test.line_width(), pa_test.height_layer());
        auto slow_speed = std::max(10.0, fast_speed / 10.0);
        if (fast_speed < slow_speed + 5)
            fast_speed = slow_speed + 5;

        pa_test.set_speed(fast_speed, slow_speed);
        pa_test.draw_numbers() = print.calib_params().print_numbers;

        const bool belt_world_coords = print.config().belt_printer.value;
        if (belt_world_coords)
            install_belt_kinematics(m_writer, print.config(), true);
        gcode += pa_test.generate_test(params.start, params.step, std::llround(std::ceil((params.end - params.start) / params.step)) + 1);
        if (belt_world_coords)
            install_belt_kinematics(m_writer, print.config(), false);

        file.write(gcode);
    } else {
        if (is_bbl_printers) {
            file.write("M981 S1 P20000 ;open spaghetti detector\n");
        }

        const auto initialize_wipe_tower = [&]() {
            static const std::vector<WipeTower::ToolChangeResult> empty_priming;
            const auto &data = print.wipe_tower_data();
            m_wipe_tower.reset(new WipeTowerIntegration(print.config(), print.get_plate_index(), print.get_plate_origin(),
                data.priming ? *data.priming : empty_priming, data.tool_changes, *data.final_purge, print.get_slice_used_filaments(false)));
            m_wipe_tower->set_wipe_tower_depth(print.get_wipe_tower_depth());
            m_wipe_tower->set_wipe_tower_bbx(print.get_wipe_tower_bbx());
            m_wipe_tower->set_rib_offset(print.get_rib_offset());
        };
        if (has_wipe_tower && Nonplanar::sequential_shell_tower(print)) {
            initialize_wipe_tower();
            file.write(m_writer.travel_to_z(initial_layer_print_height + m_config.z_offset.value, "Move to the first layer height"));
            if (wipe_tower_type == WipeTowerType::Type2 && print.config().single_extruder_multi_material_priming)
                file.write(m_wipe_tower->prime(*this));
        }
        if (print.config().print_sequence == PrintSequence::ByObject && (!has_wipe_tower || Nonplanar::sequential_shell_tower(print))) {
            size_t finished_objects = 0;
            print_object_instance_sequential_active = first_has_extrude_print_object;
            const PrintObject *prev_object = (*print_object_instance_sequential_active)->print_object;
            for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
                const PrintObject &object = *(*print_object_instance_sequential_active)->print_object;
                if (Nonplanar::sequential_shell_tower(print) || &object != prev_object || tool_ordering.first_extruder() != final_extruder_id) {
                    auto cached_ordering = use_seq_dynamic_cache ? seq_dynamic_orderings.find(&object) : seq_dynamic_orderings.end();
                    if (cached_ordering != seq_dynamic_orderings.end()) {
                        tool_ordering = cached_ordering->second;
                    } else {
                        tool_ordering = ToolOrdering(object, final_extruder_id);
                        tool_ordering.sort_and_build_data(object, final_extruder_id);
                    }
                    unsigned int new_extruder_id = tool_ordering.first_extruder();
                    if (new_extruder_id == (unsigned int)-1)
                        continue;
                    initial_extruder_id = new_extruder_id;
                    final_extruder_id   = tool_ordering.last_extruder();
                    assert(final_extruder_id != (unsigned int)-1);
                }
                print.throw_if_canceled();
                this->set_origin(unscale((*print_object_instance_sequential_active)->shift));
                this->on_set_origin((*print_object_instance_sequential_active)->print_object,
                                    (*print_object_instance_sequential_active)->shift);

                bool prime_extruder = false;
                if (finished_objects > 0) {
                    m_enable_cooling_markers = false;
                    m_avoid_crossing_perimeters.use_external_mp_once();
                    if (m_writer.need_toolchange(initial_extruder_id) && !m_wipe_tower) {
                        const PrintObjectConfig& object_config = object.config();
                        coordf_t initial_layer_print_height = print.config().initial_layer_print_height.value;

                        if (m_enable_exclude_object && print.config().support_object_skip_flush.value) {
                            m_filament_instances_code = _encode_label_ids_to_base64({(*print_object_instance_sequential_active)->model_instance->get_labeled_id()});
                        }

                        file.write(this->set_extruder(initial_extruder_id, initial_layer_print_height, true));
                        prime_extruder = true;
                    } else {
                        file.write(this->retract());
                    }
                    if (m_nonplanar_ceiling <= 0.)
                        file.write(m_writer.travel_to_z(m_max_layer_z + m_writer.config.z_hop.get_at(get_filament_config_index((int)initial_extruder_id))));
                    file.write(this->travel_to(Point(0, 0), erNone, "move to origin position for next object"));
                    m_enable_cooling_markers = true;
                    m_avoid_crossing_perimeters.disable_once();
                    this->placeholder_parser().set("current_object_idx", int(finished_objects));
                    std::string printing_by_object_gcode = this->placeholder_parser_process("printing_by_object_gcode", print.config().printing_by_object_gcode.value, initial_extruder_id);
                    this->_print_first_layer_bed_temperature(file, print, printing_by_object_gcode, initial_extruder_id, false);
                    this->_print_first_layer_extruder_temperatures(file, print, printing_by_object_gcode, initial_extruder_id, false);
                    file.writeln(printing_by_object_gcode);
                }
                m_cooling_buffer->set_current_extruder(initial_extruder_id, get_extruder_id(initial_extruder_id));
                m_cooling_buffer->reset(this->writer().get_position());
                m_belt_in_band = false;
                tool_ordering.cal_most_used_extruder(print.config());
                m_printed_objects.emplace_back(&object);
                this->process_layers(print, tool_ordering, collect_layers_to_print(object, print.config().belt_printer.value), *print_object_instance_sequential_active - object.instances().data(), file,
                                     prime_extruder);
                {
                    print.m_statistics_by_extruder_count.stats_by_single_extruder += tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::SingleExt);
                    print.m_statistics_by_extruder_count.stats_by_multi_extruder_best += tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtBest);
                    print.m_statistics_by_extruder_count.stats_by_multi_extruder_curr += tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtCurr);
                    const auto& layer_tools = tool_ordering.layer_tools();
                    for (const auto& lt : layer_tools)
                        m_sorted_layer_filaments.emplace_back(lt.extruders);
                }

                {
                    const auto plr_mode = print.config().enable_power_loss_recovery.value;
                    if (m_second_layer_things_done && plr_mode == PowerLossRecoveryMode::Enable) {
                        file.write(m_writer.enable_power_loss_recovery(PowerLossRecoveryMode::Disable));
                    }
                }
                ++ finished_objects;
                m_second_layer_things_done = false;
                prev_object = &object;
            }
        } else {
            std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print = collect_layers_to_print(print);
            if (has_wipe_tower && ! layers_to_print.empty()) {
                initialize_wipe_tower();
                file.write(m_writer.travel_to_z(initial_layer_print_height + m_config.z_offset.value, "Move to the first layer height"));

                if (wipe_tower_type == WipeTowerType::Type2 && print.config().single_extruder_multi_material_priming) {
                    file.write(m_wipe_tower->prime(*this));
                    BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                    coordf_t twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first + EPSILON;
                    for (const PrintObject *print_object : print.objects())
                        bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                    bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                    BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                    bbox_prime.offset(0.5f);
                    bool overlap = bbox_prime.overlap(bbox_print);

                    if (print.config().gcode_flavor == gcfMarlinLegacy || print.config().gcode_flavor == gcfMarlinFirmware) {
                        file.write(this->retract());
                        file.write("M300 S800 P500\n");
                        if (overlap) {
                            file.write("M1 Remove priming towers and click button.\n");
                        } else {
                            file.write("M1 S10\n");
                        }
                    }
                    else {
                        if (overlap) {
                           print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, WarningCode{ "priming_region_close" },
                               _(L("Your print is very close to the priming regions. "
                                 "Make sure there is no collision.")));
                        } else {
                        }
                    }
                }
                print.throw_if_canceled();
            }

            tool_ordering.cal_most_used_extruder(print.config());

            this->process_layers(print, tool_ordering, print_object_instances_ordering, layers_to_print, file);
            {
                print.m_statistics_by_extruder_count.stats_by_single_extruder = tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::SingleExt);
                print.m_statistics_by_extruder_count.stats_by_multi_extruder_best = tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtBest);
                print.m_statistics_by_extruder_count.stats_by_multi_extruder_curr = tool_ordering.get_filament_change_stats(ToolOrdering::FilamentChangeMode::MultiExtCurr);
                const auto& layer_tools = tool_ordering.layer_tools();
                for (const auto& lt : layer_tools)
                    m_sorted_layer_filaments.emplace_back(lt.extruders);
            }

            if (m_second_layer_things_done && print.config().enable_power_loss_recovery.value == PowerLossRecoveryMode::Enable) {
                file.write(m_writer.enable_power_loss_recovery(PowerLossRecoveryMode::Disable));
            }
            if (m_wipe_tower)
                file.write(m_wipe_tower->finalize(*this));
        }
    }
    for (const auto &[key, queue] : m_deferred_extrusions)
        if (! queue.empty())
            throw_nonplanar_internal("", std::to_string(queue.size()) + " moved extrusions were never written",
                key.first != nullptr ? key.first->id().id : 0);

    file.write(this->retract(false, true));

    {
        std::string gcode;
        m_writer.add_object_change_labels(gcode);
        file.write(gcode);
    }

    file.write(m_writer.set_fan(0));
    if(m_config.auxiliary_fan.value)
        file.write(m_writer.set_additional_fan(0));
    if (is_bbl_printers) {
        file.write("M981 S0 P20000 ; close spaghetti detector\n");
    }

    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(erCustom).c_str());

    if (m_config.enable_pre_heating.value)
        file.write_format(";%s\n", GCodeProcessor::Machine_End_GCode_Start_Tag.c_str());

    {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",   new ConfigOptionFloat(m_writer.get_position()(2) - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
        config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));

        if (print.config().single_extruder_multi_material) {
            int extruder_id = m_writer.filament()->id();
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
            config.set_key_value("current_filament_id", new ConfigOptionInt(extruder_id));
            config.set_key_value("current_extruder_id", new ConfigOptionInt((int) get_extruder_id(extruder_id)));
            config.set_key_value("current_nozzle_id", new ConfigOptionInt(nozzle_id_for_gcode_placeholder(group_result, extruder_id, (int) get_extruder_id(extruder_id), m_layer_index)));
            file.writeln(this->placeholder_parser_process("filament_end_gcode", print.config().filament_end_gcode.get_at(extruder_id), extruder_id, &config));
        } else {
            for (const std::string &end_gcode : print.config().filament_end_gcode.values) {
                int extruder_id = (unsigned int)(&end_gcode - &print.config().filament_end_gcode.values.front());
                config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                config.set_key_value("current_filament_id", new ConfigOptionInt(extruder_id));
                config.set_key_value("current_extruder_id", new ConfigOptionInt((int) get_extruder_id(extruder_id)));
                config.set_key_value("current_nozzle_id", new ConfigOptionInt(nozzle_id_for_gcode_placeholder(group_result, extruder_id, (int) get_extruder_id(extruder_id), m_layer_index)));
                file.writeln(this->placeholder_parser_process("filament_end_gcode", end_gcode, extruder_id, &config));
            }
        }
        file.writeln(this->placeholder_parser_process("machine_end_gcode", print.config().machine_end_gcode, m_writer.filament()->id(), &config));
    }
    file.write(m_writer.update_progress(m_layer_count, m_layer_count, true));
    file.write(m_writer.postamble());

    if (activate_chamber_temp_control && max_chamber_temp > 0)
        file.write(m_writer.set_chamber_temperature(0, false));
    if (m_config.support_air_filtration.value) {
        bool activate_air_filtration_on_completion = false;
        int complete_print_exhaust_fan_speed = 0;

        for (const auto& extruder : m_writer.extruders()) {
            size_t fi = get_filament_config_index((int)extruder.id());
            if (m_config.activate_air_filtration.get_at(fi) && m_config.activate_air_filtration_on_completion.get_at(fi)) {
                activate_air_filtration_on_completion = true;
                complete_print_exhaust_fan_speed = std::max(complete_print_exhaust_fan_speed, m_config.complete_print_exhaust_fan_speed.get_at(fi));
            }
        }

        if (activate_air_filtration_on_completion)
            file.write(m_writer.set_exhaust_fan(complete_print_exhaust_fan_speed));
    }

    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Last_Line_M73_Placeholder).c_str());
    file.write_format("; EXECUTABLE_BLOCK_END\n\n");

    print.throw_if_canceled();

    if (!is_bbl_printers && !skip_config_block) {
        file.write("; CONFIG_BLOCK_START\n");
        std::string full_config;
        append_full_config(print, full_config);
        if (!full_config.empty())
          file.write(full_config);

        int first_layer_bed_temperature = get_bed_temperature(0, true, print.config().curr_bed_type);
        file.write_format("; first_layer_bed_temperature = %d\n", first_layer_bed_temperature);
        file.write_format("; bed_shape = %s\n", print.full_print_config().opt_serialize("printable_area").c_str());
        file.write_format("; first_layer_temperature = %d\n", print.config().nozzle_temperature_initial_layer.get_at(0));
        file.write_format("; first_layer_height = %.3f\n", print.config().initial_layer_print_height.value);

        file.write("; CONFIG_BLOCK_END\n\n");
    }

    file.write(DoExport::update_print_stats_and_format_filament_stats(
        has_wipe_tower, print.wipe_tower_data(),
        m_writer.extruders(),
        print.m_print_statistics,
        print.tool_ordering()));
    print.m_print_statistics.initial_tool = initial_extruder_id;
    print.m_print_statistics.initial_no_support_tool = initial_non_support_extruder_id;
    if (!is_bbl_printers) {
        file.write_format("; total filament used [g] = %.2lf\n",
            print.m_print_statistics.total_weight);
        file.write_format("; total filament cost = %.2lf\n",
            print.m_print_statistics.total_cost);
        if (print.m_print_statistics.total_toolchanges > 0)
            file.write_format("; total filament change = %i\n",
                print.m_print_statistics.total_toolchanges);
        file.write_format("; total layers count = %i\n", m_layer_count);
        file.write_format(
            ";%s\n",
            GCodeProcessor::reserved_tag(
                GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder)
            .c_str());
      file.write("\n");
    }
    file.write("\n");

    print.throw_if_canceled();
}

void GCode::export_layer_filaments(GCodeProcessorResult* result)
{
    if (result == nullptr)
        return;

    const std::vector<int>filament_map = m_config.filament_map.values;
    std::vector<int>prev_filament(m_config.nozzle_diameter.size(), -1);
    for (size_t idx = 0; idx < m_sorted_layer_filaments.size(); ++idx) {
        for (auto f : m_sorted_layer_filaments[idx]) {
            if (f >= filament_map.size() || filament_map[f] <= 0)
                continue;
            int extruder_idx = filament_map[f] - 1;
            if (extruder_idx >= (int) prev_filament.size())
                continue;
            if (prev_filament[extruder_idx] != -1 && f != prev_filament[extruder_idx]) {
                std::pair<int, int> from_to_pair = { prev_filament[extruder_idx],f };
                auto iter = result->filament_change_count_map.find(from_to_pair);
                if (iter == result->filament_change_count_map.end())
                    result->filament_change_count_map.emplace(from_to_pair, 1);
                else
                    iter->second += 1;
            }
            prev_filament[extruder_idx] = f;
        }

        auto layer_filaments = m_sorted_layer_filaments[idx];
        std::sort(layer_filaments.begin(), layer_filaments.end());
        auto iter = result->layer_filaments.find(layer_filaments);
        if (iter == result->layer_filaments.end()) {
            result->layer_filaments[layer_filaments].emplace_back(idx, idx);
        }
        else {
            if (iter->second.back().second == idx - 1)
                iter->second.back().second = idx;
            else
                iter->second.emplace_back(idx, idx);
        }
    }

    result->filament_change_sequence.clear();
    result->nozzle_change_sequence.clear();

    int prev_sequence_filament = -1;
    int prev_sequence_nozzle = -1;
    for (size_t layer_idx = 0; layer_idx < m_sorted_layer_filaments.size(); ++layer_idx) {
        for (unsigned int filament_id : m_sorted_layer_filaments[layer_idx]) {
            int nozzle_id = 0;
            if (filament_id < filament_map.size() && filament_map[filament_id] > 0)
                nozzle_id = filament_map[filament_id] - 1;
            if (prev_sequence_nozzle != nozzle_id || prev_sequence_filament != static_cast<int>(filament_id)) {
                result->nozzle_change_sequence.emplace_back(static_cast<unsigned int>(nozzle_id));
                result->filament_change_sequence.emplace_back(filament_id);
                prev_sequence_nozzle = nozzle_id;
                prev_sequence_filament = static_cast<int>(filament_id);
            }
        }
    }

    result->used_mixed_filaments = m_print->get_slice_used_mixed_filaments();

    result->optimal_assignment.clear();
    result->optimal_assignment.reserve(filament_map.size());
    for (int nozzle_id : filament_map)
        result->optimal_assignment.emplace_back(nozzle_id > 0 ? nozzle_id - 1 : 0);
}

void GCode::check_placeholder_parser_failed()
{
    if (! m_placeholder_parser_integration.failed_templates.empty()) {
        std::string msg = Slic3r::format(_(L("Failed to generate G-code for invalid custom G-code.\n\n")));
        for (const auto &name_and_error : m_placeholder_parser_integration.failed_templates)
            msg += name_and_error.first + " " + name_and_error.second + "\n";
        msg += Slic3r::format(_(L("Please check the custom G-code or use the default custom G-code.")));
        throw Slic3r::PlaceholderParserError(msg);
    }
}

size_t GCode::cur_extruder_index() const
{
    return get_extruder_id(m_writer.filament()->id());
}

size_t GCode::get_extruder_id(unsigned int filament_id) const
{
    if (m_print) {
        return m_print->get_extruder_id(filament_id);
    }
    return 0;
}

size_t GCode::get_filament_config_index(int filament_id) const
{
    if (m_print)
        return m_filament_index_cache.get(filament_id, m_cur_layer_idx, m_print->config_index_generation(),
                                          [&] { return m_print->get_filament_config_indx(filament_id, m_cur_layer_idx); });
    return filament_id;
}

size_t GCode::get_filament_config_index(int filament_id, size_t layer_id) const
{
    return m_print ? m_print->get_filament_config_indx(filament_id, (int) layer_id, false) : filament_id;
}

size_t GCode::get_nozzle_config_index(int filament_id) const
{
    if (m_print)
        return m_nozzle_index_cache.get(filament_id, m_cur_layer_idx, m_print->config_index_generation(),
                                        [&] { return m_print->get_nozzle_config_index(filament_id, m_cur_layer_idx); });
    return get_extruder_id(filament_id);
}

namespace {
struct PrecomputedLayer
{
    size_t                                    index{size_t(-1)};
    std::vector<PrecomputedOverhangLayer>     overhang_layers;
};
}
template<typename OverhangSpeed>
static bool prepares_overhang_estimator(const Layer &layer, bool overhang_fan, OverhangSpeed overhang_speed)
{
    const LayerRegionPtrs &regions = layer.regions();
    return std::any_of(regions.begin(), regions.end(), [overhang_fan, &overhang_speed](const LayerRegion *region) {
        return region->has_extrusions() && (overhang_fan || overhang_speed(*region));
    });
}

std::vector<PrecomputedOverhangLayer> precompute_overhang_layers(const std::vector<GCode::LayerToPrint> &layers, bool overhang_fan)
{
    auto overhang_speed = [](const LayerRegion &region) { return any_enabled(region.region().config().enable_overhang_speed); };
    std::vector<PrecomputedOverhangLayer> out;
    for (const GCode::LayerToPrint &layer : layers)
        if (layer.object_layer != nullptr && layer.object_layer->lower_layer != nullptr &&
            prepares_overhang_estimator(*layer.object_layer, overhang_fan, overhang_speed)) {
            const LayerRegionPtrs &regions = layer.object_layer->regions();
            const bool curled_lines = std::any_of(regions.begin(), regions.end(), [](const LayerRegion *region) {
                return any_enabled(region->region().config().slowdown_for_curled_perimeters);
            });
            out.push_back(precompute_overhang_layer(layer.original_object, *layer.object_layer, curled_lines));
        }
    return out;
}

template<typename LayersAt>
static auto precomputed_layers_source(size_t &next_index, size_t layer_count, bool nop_layer, bool overhang_fan, LayersAt layers_at)
{
    return tbb::make_filter<void, PrecomputedLayer>(slic3r_tbb_filtermode::serial_in_order,
               [&next_index, layer_count, nop_layer](tbb::flow_control &fc) -> PrecomputedLayer {
                   if (next_index < layer_count)
                       return {next_index++};
                   if (next_index == layer_count + (nop_layer ? 1 : 0))
                       fc.stop();
                   else
                       ++next_index;
                   return {};
               }) &
           tbb::make_filter<PrecomputedLayer, PrecomputedLayer>(slic3r_tbb_filtermode::parallel,
               [layers_at, overhang_fan](PrecomputedLayer layer) -> PrecomputedLayer {
                   if (layer.index != size_t(-1))
                       layer.overhang_layers = precompute_overhang_layers(layers_at(layer.index), overhang_fan);
                   return layer;
               });
}

static bool overhang_fan_enabled(const PrintConfig &config, bool cooling_markers)
{
    return cooling_markers && any_enabled(config.enable_overhang_bridge_fan);
}

void GCode::process_layers(
    const Print                                                         &print,
    const ToolOrdering                                                  &tool_ordering,
    const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
    const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
    GCodeOutputStream                                                   &output_stream)
{
    size_t layer_to_print_idx = 0;
    const auto source = precomputed_layers_source(layer_to_print_idx, layers_to_print.size(), m_pressure_equalizer != nullptr,
        overhang_fan_enabled(print.config(), m_enable_cooling_markers),
        [&layers_to_print](size_t index) -> const std::vector<LayerToPrint> & { return layers_to_print[index].second; });
    const auto generator = tbb::make_filter<PrecomputedLayer, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &print_object_instances_ordering, &layers_to_print](PrecomputedLayer precomputed) -> LayerResult {
            if (precomputed.index == size_t(-1))
                return LayerResult::make_nop_layer_result();
            const std::pair<coordf_t, std::vector<LayerToPrint>>& layer = layers_to_print[precomputed.index];
            const LayerTools& layer_tools = tool_ordering.tools_for_layer(layer.first);
            print.set_status(80, Slic3r::format(_(L("Generating G-code: layer %1%")), std::to_string(precomputed.index + 1)));
            if (m_wipe_tower && layer_tools.has_wipe_tower)
                m_wipe_tower->next_layer();
            check_placeholder_parser_failed();
            print.throw_if_canceled();
            m_extrusion_quality_estimator.set_precomputed_layers(std::move(precomputed.overhang_layers));
            return this->process_layer(print, layer.second, layer_tools, &layer == &layers_to_print.back(), &print_object_instances_ordering, tool_ordering.get_most_used_extruder(), size_t(-1));
        });
    if (m_spiral_vase) {
        float nozzle_diameter  = EXTRUDER_CONFIG(nozzle_diameter);
        float max_xy_smoothing = m_config.get_abs_value("spiral_mode_max_xy_smoothing", nozzle_diameter);
        this->m_spiral_vase->set_max_xy_smoothing(max_xy_smoothing);
    }
    const auto spiral_mode = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [&spiral_mode = *this->m_spiral_vase.get(), &layers_to_print](LayerResult in) -> LayerResult {
        	if (in.nop_layer_result)
                return in;
                
            spiral_mode.enable(in.spiral_vase_enable);
            bool last_layer = in.layer_id == layers_to_print.size() - 1;
            return { spiral_mode.process_layer(std::move(in.gcode), last_layer), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush};
        });
    const auto pressure_equalizer = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [pressure_equalizer = this->m_pressure_equalizer.get()](LayerResult in) -> LayerResult {
            return pressure_equalizer->process_layer(std::move(in));
        });
    const auto cooling = tbb::make_filter<LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&cooling_buffer = *this->m_cooling_buffer.get()](LayerResult in) -> std::string {
        	if (in.nop_layer_result)
                return in.gcode;
            return cooling_buffer.process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto pa_processor_filter = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
            [&pa_processor = *this->m_pa_processor](std::string in) -> std::string {
                return pa_processor.process_layer(std::move(in));
            }
        );
    
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [&output_stream](std::string s) { output_stream.write(s); }
    );

    const auto fan_mover = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
            [&fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in)->std::string {

        CNumericLocalesSetter locales_setter;

        if (config.fan_speedup_time.value != 0 || config.fan_kickstart.value > 0) {
            if (fan_mover.get() == nullptr)
                fan_mover.reset(new Slic3r::FanMover(
                    writer,
                    std::abs((float)config.fan_speedup_time.value),
                    config.fan_speedup_time.value > 0,
                    config.use_relative_e_distances.value,
                    config.fan_speedup_overhangs.value,
                    (float)config.fan_kickstart.value));
            return fan_mover->process_gcode(in, true);
        }
        return in;
    });

    if (m_spiral_vase && m_pressure_equalizer)
        tbb::parallel_pipeline(12, source & generator & spiral_mode & pressure_equalizer & cooling & fan_mover & output);
    else if (m_spiral_vase)
    	tbb::parallel_pipeline(12, source & generator & spiral_mode & cooling & fan_mover & output);
    else if	(m_pressure_equalizer)
        tbb::parallel_pipeline(12, source & generator & pressure_equalizer & cooling & fan_mover & pa_processor_filter & output);
    else
    	tbb::parallel_pipeline(12, source & generator & cooling & fan_mover & pa_processor_filter & output);
    m_extrusion_quality_estimator.set_precomputed_layers({});

}

void GCode::process_layers(
    const Print                             &print,
    const ToolOrdering                      &tool_ordering,
    std::vector<LayerToPrint>                layers_to_print,
    const size_t                             single_object_idx,
    GCodeOutputStream                       &output_stream,
    const bool                               prime_extruder)
{
    size_t layer_to_print_idx = 0;
    const auto source = precomputed_layers_source(layer_to_print_idx, layers_to_print.size(), m_pressure_equalizer != nullptr,
        overhang_fan_enabled(print.config(), m_enable_cooling_markers),
        [&layers_to_print](size_t index) { return std::vector<LayerToPrint>{layers_to_print[index]}; });
    const auto generator = tbb::make_filter<PrecomputedLayer, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &layers_to_print, single_object_idx, prime_extruder](PrecomputedLayer precomputed) -> LayerResult {
            if (precomputed.index == size_t(-1))
                return LayerResult::make_nop_layer_result();
            LayerToPrint &layer = layers_to_print[precomputed.index];
            print.set_status(80, Slic3r::format(_(L("Generating G-code: layer %1%")), std::to_string(precomputed.index + 1)));
            check_placeholder_parser_failed();
            print.throw_if_canceled();
            m_extrusion_quality_estimator.set_precomputed_layers(std::move(precomputed.overhang_layers));
            LayerTools tools = tool_ordering.tools_for_layer(layer.print_z());
            if (m_wipe_tower && Nonplanar::sequential_shell_tower(print)) {
                tools.has_wipe_tower = true;
                tools.wipe_tower_partitions = std::max(size_t(1), tools.wipe_tower_partitions);
                m_wipe_tower->next_layer();
            }
            return this->process_layer(print, { std::move(layer) }, tools, &layer == &layers_to_print.back(), nullptr, tool_ordering.get_most_used_extruder(), single_object_idx, prime_extruder);
        });
    if (m_spiral_vase) {
        float nozzle_diameter  = EXTRUDER_CONFIG(nozzle_diameter);
        float max_xy_smoothing = m_config.get_abs_value("spiral_mode_max_xy_smoothing", nozzle_diameter);
        this->m_spiral_vase->set_max_xy_smoothing(max_xy_smoothing);
    }
    const auto spiral_mode = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [&spiral_mode = *this->m_spiral_vase.get(), &layers_to_print](LayerResult in)->LayerResult {
            if (in.nop_layer_result)
                return in;
            spiral_mode.enable(in.spiral_vase_enable);
            bool last_layer = in.layer_id == layers_to_print.size() - 1;
            return { spiral_mode.process_layer(std::move(in.gcode), last_layer), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush };
        });
    const auto pressure_equalizer = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [pressure_equalizer = this->m_pressure_equalizer.get()](LayerResult in) -> LayerResult {
             return pressure_equalizer->process_layer(std::move(in));
        });
    const auto cooling = tbb::make_filter<LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&cooling_buffer = *this->m_cooling_buffer.get()](LayerResult in)->std::string {
            if (in.nop_layer_result)
                return in.gcode;
            return cooling_buffer.process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto pa_processor_filter = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&pa_processor = *this->m_pa_processor](std::string in) -> std::string {
            return pa_processor.process_layer(std::move(in));
        }
    );
    
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [&output_stream](std::string s) { output_stream.write(s); }
    );

    const auto fan_mover = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [&fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in)->std::string {

        if (config.fan_speedup_time.value != 0 || config.fan_kickstart.value > 0) {
            if (fan_mover.get() == nullptr)
                fan_mover.reset(new Slic3r::FanMover(
                    writer,
                    std::abs((float)config.fan_speedup_time.value),
                    config.fan_speedup_time.value > 0,
                    config.use_relative_e_distances.value,
                    config.fan_speedup_overhangs.value,
                    (float)config.fan_kickstart.value));
            return fan_mover->process_gcode(in, true);
        }
        return in;
    });

    if (m_spiral_vase && m_pressure_equalizer)
        tbb::parallel_pipeline(12, source & generator & spiral_mode & pressure_equalizer & cooling & fan_mover & output);
    else if (m_spiral_vase)
    	tbb::parallel_pipeline(12, source & generator & spiral_mode & cooling & fan_mover & output);
    else if	(m_pressure_equalizer)
        tbb::parallel_pipeline(12, source & generator & pressure_equalizer & cooling & fan_mover & pa_processor_filter & output);
    else
    	tbb::parallel_pipeline(12, source & generator & cooling & fan_mover & pa_processor_filter & output);
    m_extrusion_quality_estimator.set_precomputed_layers({});
}

std::string GCode::placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_filament_id, const DynamicConfig *config_override)
{
#if ORCA_CHECK_GCODE_PLACEHOLDERS
    if (config_override) {
        const auto& custom_gcode_placeholders = custom_gcode_specific_placeholders();

        if (auto it = custom_gcode_placeholders.find(name); it != custom_gcode_placeholders.end()) {
            const auto& placeholders = it->second;

            for (const std::string& key : config_override->keys()) {
                if (std::find(placeholders.begin(), placeholders.end(), key) == placeholders.end()) {
                    auto& vector = m_placeholder_error_messages[name + " - option not specified for custom gcode type (s_CustomGcodeSpecificPlaceholders)"];
                    if (std::find(vector.begin(), vector.end(), key) == vector.end())
                        vector.emplace_back(key);
                }
                if (!custom_gcode_specific_config_def.has(key)) {
                    auto& vector = m_placeholder_error_messages[name + " - option has no definition (CustomGcodeSpecificConfigDef)"];
                    if (std::find(vector.begin(), vector.end(), key) == vector.end())
                        vector.emplace_back(key);
                }
            }
        }
        else {
            auto& vector = m_placeholder_error_messages[name + " - gcode type not found in s_CustomGcodeSpecificPlaceholders"];
            if (vector.empty())
                vector.emplace_back("");
        }
    }
#endif

PlaceholderParserIntegration &ppi = m_placeholder_parser_integration;
    try {
        ppi.update_from_gcodewriter(m_writer);
        std::string output = ppi.parser.process(templ, current_filament_id, config_override, &ppi.output_config, &ppi.context);
        ppi.validate_output_vector_variables();
        const CustomGCodeMotionStateChanges motion_state_changes = custom_gcode_motion_state_changes(output);
        if (motion_state_changes.acceleration)
            m_writer.invalidate_acceleration();
        if (motion_state_changes.jerk)
            m_writer.invalidate_jerk();

        if (const std::vector<double> &pos = ppi.opt_position->values; ppi.position != pos) {
            m_writer.set_position({ pos[0], pos[1], pos[2] });
            this->set_last_pos(this->gcode_to_point({ pos[0], pos[1] }));
        }

        for (const Extruder &e : m_writer.extruders()) {
            unsigned int eid = e.id();
            assert(eid < ppi.num_extruders);
            if ( eid < ppi.num_extruders) {
                if (! m_writer.config.use_relative_e_distances && ! is_approx(ppi.e_position[eid], ppi.opt_e_position->values[eid]))
                    const_cast<Extruder&>(e).set_position(ppi.opt_e_position->values[eid]);
                if (! is_approx(ppi.e_retracted[eid], ppi.opt_e_retracted->values[eid]) || 
                    ! is_approx(ppi.e_restart_extra[eid], ppi.opt_e_restart_extra->values[eid]))
                    const_cast<Extruder&>(e).set_retracted(ppi.opt_e_retracted->values[eid], ppi.opt_e_restart_extra->values[eid]);
            }
        }

        return output;
    } 
    catch (std::runtime_error &err) 
    {
        auto it = ppi.failed_templates.find(name);
        if (it == ppi.failed_templates.end())
            ppi.failed_templates.insert(it, std::make_pair(name, std::string(err.what())));
        return
            std::string("\n!!!!! Failed to process the custom G-code template ") + name + "\n" +
            err.what() +
            "!!!!! End of an error report for the custom G-code template " + name + "\n\n";
    }
}


void GCode::print_machine_envelope(GCodeOutputStream &file, Print &print)
{
    const auto flavor = print.config().gcode_flavor.value;
    if ((flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware || flavor == gcfRepRapFirmware) &&
        print.config().emit_machine_limits_to_gcode.value == true) {

        std::unordered_set<unsigned int> used_extruders;
        for (const auto& extruder : m_writer.extruders()) {
            used_extruders.insert(extruder.extruder_id());
        }

        auto get_max_value = [&used_extruders](const std::string key, const ConfigOptionFloats& v) { 
            unsigned int stride = 1;
            if (printer_options_with_variant_2.count(key) > 0) {
                stride = 2;
            }

            double value = std::numeric_limits<double>::lowest();
            for (unsigned int extruder : used_extruders) {
                value = std::max(value, v.values[extruder * stride]);
            }

            assert(value > std::numeric_limits<double>::lowest());
            return value;
        };
#define MAX_LIMIT(OPT) get_max_value(#OPT, print.config().OPT)

        int factor = flavor == gcfRepRapFirmware ? 60 : 1;
        file.write_format("M201 X%d Y%d Z%d E%d\n",
            int(MAX_LIMIT(machine_max_acceleration_x) + 0.5),
            int(MAX_LIMIT(machine_max_acceleration_y) + 0.5),
            int(MAX_LIMIT(machine_max_acceleration_z) + 0.5),
            int(MAX_LIMIT(machine_max_acceleration_e) + 0.5));
        file.write_format("M203 X%d Y%d Z%d E%d\n",
            int(MAX_LIMIT(machine_max_speed_x) * factor + 0.5),
            int(MAX_LIMIT(machine_max_speed_y) * factor + 0.5),
            int(MAX_LIMIT(machine_max_speed_z) * factor + 0.5),
            int(MAX_LIMIT(machine_max_speed_e) * factor + 0.5));

        int travel_acc = flavor == gcfMarlinLegacy
                       ? int(MAX_LIMIT(machine_max_acceleration_extruding) + 0.5)
                       : int(MAX_LIMIT(machine_max_acceleration_travel) + 0.5);
        if (flavor == gcfRepRapFirmware)
            file.write_format("M204 P%d T%d ; sets acceleration (P, T), mm/sec^2\n",
                int(MAX_LIMIT(machine_max_acceleration_extruding) + 0.5),
                travel_acc);
        else if (flavor == gcfMarlinFirmware)
            file.write_format("M204 P%d R%d T%d ; sets acceleration (P, T) and retract acceleration (R), mm/sec^2\n",
                int(MAX_LIMIT(machine_max_acceleration_extruding) + 0.5),
                int(MAX_LIMIT(machine_max_acceleration_retracting) + 0.5),
                int(MAX_LIMIT(machine_max_acceleration_travel) + 0.5));
        else
            file.write_format("M204 P%d R%d T%d\n",
                int(MAX_LIMIT(machine_max_acceleration_extruding) + 0.5),
                int(MAX_LIMIT(machine_max_acceleration_retracting) + 0.5),
                travel_acc);

        assert(is_decimal_separator_point());
        file.write_format(flavor == gcfRepRapFirmware
            ? "M566 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/min\n"
            : "M205 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/sec\n",
            MAX_LIMIT(machine_max_jerk_x) * factor,
            MAX_LIMIT(machine_max_jerk_y) * factor,
            MAX_LIMIT(machine_max_jerk_z) * factor,
            MAX_LIMIT(machine_max_jerk_e) * factor);

        file.write_format(writer().set_junction_deviation(MAX_LIMIT(machine_max_junction_deviation)).c_str());

        if (print.config().input_shaping_emit.value && flavor != gcfMarlinLegacy) {
            const bool input_shaping_disable = print.config().input_shaping_type.value == InputShaperType::Disable;
            file.write_format(writer().set_input_shaping('X', print.config().input_shaping_damp_x.value,
                print.config().input_shaping_freq_x.value, print.config().opt_serialize("input_shaping_type")).c_str());
            if (flavor != gcfRepRapFirmware && !input_shaping_disable) {
                file.write_format(writer().set_input_shaping('Y', print.config().input_shaping_damp_y.value,
                    print.config().input_shaping_freq_y.value, "").c_str());
            }
        }
    }
#undef MAX_LIMIT
}

int GCode::get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const
{
    std::string bed_temp_key = is_first_layer ? get_bed_temp_1st_layer_key(bed_type) : get_bed_temp_key(bed_type);
    const ConfigOptionInts* bed_temp_opt = m_config.option<ConfigOptionInts>(bed_temp_key);
    return bed_temp_opt->get_at(extruder_id);
}

int GCode::get_highest_bed_temperature(const bool is_first_layer, const Print& print) const
{
    auto bed_type = m_config.curr_bed_type;
    int bed_temp = 0;
    for (auto fidx : print.get_slice_used_filaments(is_first_layer)) {
        bed_temp = std::max(bed_temp, get_bed_temperature(fidx, is_first_layer, bed_type));
    }
    return bed_temp;
}

void GCode::_print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    std::vector<int> temps_per_bed;
    int bed_temp = 0;
    if (m_config.bed_temperature_formula.value == BedTempFormula::btfHighestTemp) {
        bed_temp = get_highest_bed_temperature(true, print);
    }
    else {
        bed_temp = get_bed_temperature(first_printing_extruder_id, true, print.config().curr_bed_type);
    }
    int  temp_by_gcode     = -1;
    bool temp_set_by_gcode = custom_gcode_sets_temperature(gcode, 140, 190, false, temp_by_gcode);
#if 0
    if (temp_set_by_gcode && temp_by_gcode >= 0 && temp_by_gcode < 1000)
        temp = temp_by_gcode;
#endif

    std::string set_temp_gcode = m_writer.set_bed_temperature(bed_temp, wait);
    if (! temp_set_by_gcode)
        file.write(set_temp_gcode);
}

void GCode::_print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait)
{
    int  temp_by_gcode = -1;
    bool include_g10   = print.config().gcode_flavor == gcfRepRapFirmware;
    if (custom_gcode_sets_temperature(gcode, 104, 109, include_g10, temp_by_gcode)) {
        int temp = print.config().nozzle_temperature_initial_layer.get_at(get_filament_config_index((int)first_printing_extruder_id));
        if (temp_by_gcode >= 0 && temp_by_gcode < 1000)
            temp = temp_by_gcode;
        m_writer.set_temperature(temp, wait, first_printing_extruder_id);
    } else {
        if (print.config().single_extruder_multi_material.value) {
            int temp = print.config().nozzle_temperature_initial_layer.get_at(get_filament_config_index((int)first_printing_extruder_id));
            if (temp > 0)
                file.write(m_writer.set_temperature(temp, wait, first_printing_extruder_id));
        } else {
            for (unsigned int tool_id : print.extruders()) {
                int temp = print.config().nozzle_temperature_initial_layer.get_at(get_filament_config_index((int)tool_id));
                if (m_ooze_prevention.enable && tool_id != first_printing_extruder_id) {
                    if (print.config().idle_temperature.get_at(tool_id) == 0)
                        temp += print.config().standby_temperature_delta.value;
                    else
                        temp = print.config().idle_temperature.get_at(tool_id);
                }
                if (temp > 0)
                    file.write(m_writer.set_temperature(temp, wait, tool_id));
            }
        }
    }
}

inline GCode::ObjectByExtruder& object_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>> &by_extruder,
    unsigned int                                                  extruder_id,
    size_t                                                        object_idx,
    size_t                                                        num_objects)
{
    std::vector<GCode::ObjectByExtruder> &objects_by_extruder = by_extruder[extruder_id];
    if (objects_by_extruder.empty())
        objects_by_extruder.assign(num_objects, GCode::ObjectByExtruder());
    return objects_by_extruder[object_idx];
}

inline std::vector<GCode::ObjectByExtruder::Island>& object_islands_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>>  &by_extruder,
    unsigned int                                                   extruder_id,
    size_t                                                         object_idx,
    size_t                                                         num_objects,
    size_t                                                         num_islands)
{
    std::vector<GCode::ObjectByExtruder::Island> &islands = object_by_extruder(by_extruder, extruder_id, object_idx, num_objects).islands;
    if (islands.empty())
        islands.assign(num_islands, GCode::ObjectByExtruder::Island());
    return islands;
}

std::vector<GCode::InstanceToPrint> GCode::sort_print_object_instances(
    std::vector<GCode::ObjectByExtruder> 		&objects_by_extruder,
    const std::vector<LayerToPrint> 			&layers,
    const std::vector<const PrintInstance*> 	*ordering,
    const size_t                     		 	 single_object_instance_idx)
{
    std::vector<InstanceToPrint> out;

    if (ordering == nullptr) {
        for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
            const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
            const PrintObject *print_object = layers[layer_id].original_object;
            if (print_object)
                out.emplace_back(object_by_extruder, layer_id, *print_object, single_object_instance_idx, print_object->instances()[single_object_instance_idx].model_instance->get_labeled_id());
        }
    } else {
        std::vector<std::pair<const PrintObject*, ObjectByExtruder*>> sorted;
        sorted.reserve(objects_by_extruder.size());
        for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
            const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
            const PrintObject *print_object = layers[layer_id].original_object;
            if (print_object)
                sorted.emplace_back(print_object, &object_by_extruder);
        }
        std::sort(sorted.begin(), sorted.end());

        if (! sorted.empty()) {
            out.reserve(sorted.size());
            for (const PrintInstance *instance : *ordering) {
                const PrintObject &print_object = *instance->print_object;
                std::pair<const PrintObject*, ObjectByExtruder*> key(&print_object, nullptr);
                auto it = std::lower_bound(sorted.begin(), sorted.end(), key);
                if (it != sorted.end() && it->first == &print_object)
                    out.emplace_back(*it->second, it->second - objects_by_extruder.data(), print_object, instance - print_object.instances().data(), instance->model_instance->get_labeled_id());
            }
        }
    }
    return out;
}

namespace ProcessLayer
{

    static std::string pressure_advance_pattern(GCodeWriter &writer, const std::string &source)
    {
        Vec3d position = writer.get_position();
        std::string output;
        GCodeReader reader;
        reader.apply_config(writer.config);
        reader.parse_buffer(source, [&](GCodeReader &parser, const GCodeReader::GCodeLine &line) {
            const bool move = line.cmd_is("G0") || line.cmd_is("G1");
            const double de = line.dist_E(parser);
            if (line.cmd_is("G10") || line.cmd_is("G11") ||
                (move && line.has_e() && !line.has_x() && !line.has_y() && !line.has_z())) {
                output += line.cmd_is("G10") || (move && de < 0) ? writer.retract() : writer.unretract();
                return;
            }
            if (move) {
                if (line.has_x()) position.x() = line.x();
                if (line.has_y()) position.y() = line.y();
                if (line.has_z()) position.z() = line.z();
            }
            if (line.cmd_is("G92") && line.has_e()) {
                output += writer.reset_e(true);
                return;
            }
            auto emitted = line;
            if (move && line.has_e()) {
                writer.filament()->extrude(de);
                if (!writer.config.use_relative_e_distances)
                    emitted.set(E, float(writer.filament()->E()), 5);
            }
            output += emitted.raw() + "\n";
        });
        writer.set_position(position, 0.);
        writer.set_current_position_clear(true);
        output += writer.retract();
        output += writer.eager_lift(LiftType::NormalLift);
        writer.set_position(writer.get_position(), std::max(0., writer.get_position().z() - position.z()));
        return output;
    }

    static std::string emit_custom_gcode_per_print_z(
        GCode                                                   &gcodegen,
        const CustomGCode::Item 								*custom_gcode,
        unsigned int                                             current_extruder_id,
        unsigned int                                             first_extruder_id,
        bool                                                     is_bbl_printer,
        const PrintConfig                                       &config)
    {
        std::string gcode;
        if (custom_gcode != nullptr) {
            gcodegen.writer().limit_next_travel_z();
            assert(custom_gcode->type != CustomGCode::ToolChange);
            const CustomGCode::Type gcode_type = custom_gcode->type;
            if (gcode_type == CustomGCode::ColorChange) {
                const unsigned int extruder = config.filament_diameter.size() == 1 || custom_gcode->extruder <= 0 ?
                    current_extruder_id : unsigned(custom_gcode->extruder - 1);
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Color_Change) + ",T" + std::to_string(extruder) + "," + custom_gcode->color + "\n";
                if (config.filament_diameter.size() > 1 && first_extruder_id != extruder) {
                    DynamicConfig cfg;
                    cfg.set_key_value("color_change_extruder", new ConfigOptionInt(int(extruder)));
                    gcode += gcodegen.placeholder_parser_process("machine_pause_gcode", config.machine_pause_gcode, current_extruder_id, &cfg);
                    gcode += "\nM117 Change filament for Extruder " + std::to_string(extruder) + "\n";
                } else if (is_bbl_printer) {
                    gcode += gcodegen.placeholder_parser_process("machine_pause_gcode", config.machine_pause_gcode, current_extruder_id) + "\n";
                } else {
                    gcode += "M600\n";
                    gcodegen.writer().unretract();
                }
            } else {
                if (gcode_type == CustomGCode::PausePrint)
                {
                    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) + "\n";
                    gcode += gcodegen.placeholder_parser_process("machine_pause_gcode", config.machine_pause_gcode, current_extruder_id) + "\n";
                }
                else {
                    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Custom_Code) + "\n";
                    if (gcode_type == CustomGCode::Template)
                        gcode += gcodegen.placeholder_parser_process("template_custom_gcode", config.template_custom_gcode, current_extruder_id);
                    else if (pressure_advance_pattern_supported(*custom_gcode, gcodegen.writer(), config))
                        gcode += pressure_advance_pattern(gcodegen.writer(), custom_gcode->extra);
                    else
                        gcode += custom_gcode->extra;

                }
                gcode += "\n";
            }
        }

        return gcode;
    }
}

namespace Skirt {
    static void skirt_loops_per_extruder_all_printing(const Print &print, const ExtrusionEntityCollection &skirt, const LayerTools &layer_tools, std::map<unsigned int, std::pair<size_t, size_t>> &skirt_loops_per_extruder_out)
    {
        size_t n_loops = skirt.entities.size();
        size_t n_tools = layer_tools.extruders.size();
        size_t lines_per_extruder = (n_loops + n_tools - 1) / n_tools;

            skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, n_loops);
    }

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_1st_layer(
        const Print             				&print,
        const ExtrusionEntityCollection &skirt,
        const LayerTools                		&layer_tools,
        std::vector<coordf_t>  			    	&skirt_done)
    {
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        const bool has_skirt_or_draft_shield = print.has_skirt() || print.has_infinite_skirt();
        if (skirt_done.empty() && has_skirt_or_draft_shield && ! skirt.entities.empty() && layer_tools.has_skirt) {
            skirt_loops_per_extruder_all_printing(print, skirt, layer_tools, skirt_loops_per_extruder_out);
            skirt_done.emplace_back(layer_tools.print_z);
        }
        return skirt_loops_per_extruder_out;
    }

    static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_other_layers(
        const Print 							&print,
        const ExtrusionEntityCollection     &skirt,
        const LayerTools                		&layer_tools,
        std::vector<coordf_t>			    	&skirt_done)
    {
        std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        const bool has_skirt_or_draft_shield = print.has_skirt() || print.has_infinite_skirt();
        if (has_skirt_or_draft_shield && ! skirt.entities.empty() && layer_tools.has_skirt &&
            (skirt_done.size() < (size_t)print.config().skirt_height.value || print.has_infinite_skirt())) {
            assert(!skirt_done.empty());
            if (skirt_done.empty())
                return skirt_loops_per_extruder_out;
            if (skirt_done.back() < layer_tools.print_z - EPSILON) {
#if 0
                skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, print.config().skirt_loops.value);
#else
                skirt_loops_per_extruder_all_printing(print, skirt, layer_tools, skirt_loops_per_extruder_out);
#endif
                skirt_done.emplace_back(layer_tools.print_z);
            }
        }
        return skirt_loops_per_extruder_out;
    }

    static Point find_start_point(ExtrusionLoop& loop, float start_angle) {
        coord_t min_x = std::numeric_limits<coord_t>::max();
        coord_t max_x = std::numeric_limits<coord_t>::min();
        coord_t min_y = min_x;
        coord_t max_y = max_x;

        Points pts;
        loop.collect_points(pts);
        for (Point pt: pts) {
            if (pt.x() < min_x)
                min_x = pt.x();
            else if (pt.x() > max_x)
                max_x = pt.x();
            if (pt.y() < min_y)
                min_y = pt.y();
            else if (pt.y() > max_y)
                max_y = pt.y();
        }

        Point center((min_x + max_x)/2., (min_y + max_y)/2.);
        double r       = center.distance_to(Point(min_x, min_y));
        double deg     = start_angle * PI / 180;
        double shift_x = r * std::cos(deg);
        double shift_y = r * std::sin(deg);
        return Point(center.x()+shift_x, center.y() + shift_y);
    }

}
std::string sanitize_instance_name(const std::string& name) {
    static const std::regex non_word_characters("[ !@#$%^&*()=+\\[\\]{};:\",']+");
    std::string result = std::regex_replace(name, non_word_characters, "_");
    if (!result.empty() && result.front() == '_') {
        result.erase(result.begin());
    }
    if (!result.empty() && result.back() == '_') {
        result.erase(result.end() - 1);
    }

    return result;
}

inline std::string get_instance_name(const PrintObject *object, size_t inst_id) {
    auto obj_name = sanitize_instance_name(object->model_object()->name);
    auto name = obj_name + "_id_" + std::to_string(object->get_id()) + "_copy_" + std::to_string(inst_id);
    return sanitize_instance_name(name);
}

const std::string& GCode::instance_name(const PrintInstance &instance)
{
    auto [it, inserted] = m_instance_names.try_emplace(&instance);
    if (inserted)
        it->second = get_instance_name(instance.print_object, instance.id);
    return it->second;
}

std::string GCode::generate_skirt(const Print &print,
        const ExtrusionEntityCollection &skirt,
        const Point& offset,
        const float skirt_start_angle,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id,
        std::vector<coordf_t> &skirt_done)
{
    
    bool first_layer = (layer.id() == 0 && abs(layer.bottom_z()) < EPSILON);
    std::string gcode;
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder;
    skirt_loops_per_extruder = first_layer ?
        Skirt::make_skirt_loops_per_extruder_1st_layer(print, skirt, layer_tools, skirt_done) :
        Skirt::make_skirt_loops_per_extruder_other_layers(print, skirt, layer_tools, skirt_done);

    if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
        const std::pair<size_t, size_t> loops = loops_it->second;
       
        set_origin(unscaled(offset));

        m_avoid_crossing_perimeters.use_external_mp();
        Flow layer_skirt_flow = print.skirt_flow().with_height(float(skirt_done.back() - (skirt_done.size() == 1 ? 0. : skirt_done[skirt_done.size() - 2])));
        double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
        const size_t start_idx = (first_layer || !print.m_config.single_loop_draft_shield)
                                 ? loops.first
                                 : (loops.second - 1);

        for (size_t i = start_idx; i < loops.second; ++i) {
            ExtrusionLoop loop = *dynamic_cast<const ExtrusionLoop*>(skirt.entities[i]);
            for (ExtrusionPath &path : loop.paths) {
                path.height = layer_skirt_flow.height();
                path.mm3_per_mm = mm3_per_mm;
            }

            if (first_layer && i==loops.first) {
                const Point desired_start_point = Skirt::find_start_point(loop, skirt_start_angle);
                gcode += this->extrude_loop(loop, "skirt", NOZZLE_CONFIG(support_speed), {}, &desired_start_point);
            }
            else
                gcode += this->extrude_loop(loop, "skirt", NOZZLE_CONFIG(support_speed));

            if (!first_layer && print.m_config.single_loop_draft_shield) {
                break;
            }
        }
        m_avoid_crossing_perimeters.use_external_mp(false);
        if (first_layer && loops.first == 0)
            m_avoid_crossing_perimeters.disable_once();
    }
    return gcode;
}

static size_t find_skirt_brim_group_idx(const Print& print, ObjectID object_id, size_t instance_id)
{
    const std::vector<Print::SkirtBrimGroup>& groups = print.skirt_brim_groups();
    for (size_t idx = 0; idx < groups.size(); ++idx) {
        const std::vector<ObjectInstanceID>& instances = groups[idx].instances;
        if (std::any_of(instances.begin(), instances.end(), [object_id, instance_id](const ObjectInstanceID& instance) {
                return instance.object_id == object_id && instance.instance_id == instance_id;
            }))
            return idx;
    }
    return size_t(-1);
}

std::string GCode::generate_object_skirt_group(const Print &print,
        const PrintObject &object,
        size_t instance_id,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id)
{
    if (print.config().skirt_type != stPerObject || !layer_tools.has_skirt || print.skirt_brim_groups().empty())
        return {};

    const size_t group_idx = find_skirt_brim_group_idx(print, object.id(), instance_id);
    if (group_idx == size_t(-1) || print.skirt_brim_groups()[group_idx].skirt.empty())
        return {};

    LayerTools object_skirt_tools = layer_tools;
    object_skirt_tools.extruders  = { extruder_id };
    object_skirt_tools.has_skirt  = true;

    return generate_skirt(print, print.skirt_brim_groups()[group_idx].skirt, Point(0, 0), object.config().skirt_start_angle,
                          object_skirt_tools, layer, extruder_id, m_skirt_group_done[group_idx]);
}

std::string GCode::generate_object_brim(const Print &print, const PrintObject &object, size_t instance_id, bool first_layer,
                                        const Layer *object_layer)
{
    if (object.has_belt_brim()) {
        if (object_layer == nullptr)
            return {};
        const std::vector<ExtrusionEntityCollection> &by_layer = object.belt_brim_by_layer();
        const size_t layer_idx = object_layer->id();
        if (layer_idx >= by_layer.size() || by_layer[layer_idx].empty())
            return {};
        std::string gcode;
        m_config.apply(print.default_region_config());
        m_config.apply(object.config(), true);
        BeltFloorObjectGuard floor_owner{ m_belt_floor_object, &object };
        const Point &offset = object.instances()[instance_id].shift;
        this->set_origin(unscale(offset));
        this->on_set_origin(&object, offset);
        m_avoid_crossing_perimeters.use_external_mp();
        for (const ExtrusionEntity *ee : by_layer[layer_idx].entities)
            if (ee != nullptr)
                gcode += this->extrude_entity(*ee, "brim", NOZZLE_CONFIG(support_speed));
        m_avoid_crossing_perimeters.use_external_mp(false);
        m_avoid_crossing_perimeters.disable_once();
        return gcode;
    }

    if (!first_layer)
        return {};

    auto emit_brim = [this](const ExtrusionEntityCollection& brim, const std::vector<ObjectInstanceID>& instances) {
        std::string gcode;
        const bool already_emitted = std::none_of(instances.begin(), instances.end(), [this](const ObjectInstanceID& instance) {
            return m_objsWithBrim.find(instance) != m_objsWithBrim.end();
        });
        if (already_emitted || brim.empty())
            return gcode;

        this->set_origin(0., 0.);
        m_avoid_crossing_perimeters.use_external_mp();
        for (const ExtrusionEntity* ee : brim.entities)
            if (ee != nullptr)
                gcode += this->extrude_entity(*ee, "brim", NOZZLE_CONFIG(support_speed));
        m_avoid_crossing_perimeters.use_external_mp(false);
        m_avoid_crossing_perimeters.disable_once();
        for (const ObjectInstanceID& instance : instances)
            m_objsWithBrim.erase(instance);
        return gcode;
    };

    const ObjectInstanceID object_instance_id{ object.id(), instance_id };
    const size_t group_idx = find_skirt_brim_group_idx(print, object.id(), instance_id);
    if (group_idx != size_t(-1)) {
        std::string gcode;
        for (const Print::SkirtBrimGroup::Brim& brim : print.skirt_brim_groups()[group_idx].brims)
            if (std::find(brim.instances.begin(), brim.instances.end(), object_instance_id) != brim.instances.end())
                gcode += emit_brim(brim.brim, brim.instances);
        return gcode;
    }

    return {};
}

LayerResult GCode::process_belt_brim_layer(
    const Print                     &print,
    const std::vector<LayerToPrint> &layers,
    const LayerTools                &layer_tools,
    const bool                       last_layer,
    const size_t                     single_object_instance_idx)
{
    LayerResult result { {}, 0, false, true };
    if (layer_tools.extruders.empty())
        return result;

    coordf_t print_z = 0.;
    coordf_t height  = 0.;
    for (const LayerToPrint &ltp : layers)
        if (ltp.belt_brim_band != nullptr) {
            print_z = ltp.belt_brim_band->print_z;
            height  = ltp.belt_brim_band->height;
            break;
        }

    m_cur_layer_idx = 0;

    struct BeltBrimZGuard {
        std::optional<coordf_t> &slot;
        ~BeltBrimZGuard() { slot.reset(); }
    } z_guard { m_belt_brim_z };
    m_belt_brim_z = print_z;
    m_layer = nullptr;

    std::string gcode;
    const unsigned int extruder_id = layer_tools.extruders.front();
    if (m_writer.filament() == nullptr || m_writer.filament()->id() != extruder_id)
        gcode += this->set_extruder(extruder_id, print_z);

    {
        char buf[64];
        gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
        sprintf(buf, ";Z:%g\n", print_z);
        gcode += buf;
        const float band_height = float(height);
        sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), band_height);
        gcode += buf;
        m_last_layer_z = float(print_z);
        m_max_layer_z  = std::max(m_max_layer_z, m_last_layer_z);
        m_last_height  = band_height;
    }

    if (! m_config.before_layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num",   new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z",     new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("before_layer_change_gcode",
            print.config().before_layer_change_gcode.value, m_writer.filament()->id(), &config) + "\n";
    }

    gcode += this->change_layer(print_z);

    if (! m_config.layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num",   new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",     new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("layer_change_gcode",
            print.config().layer_change_gcode.value, m_writer.filament()->id(), &config) + "\n";
    }

    for (const unsigned int brim_extruder : layer_tools.extruders) {
        if (m_writer.filament() == nullptr || m_writer.filament()->id() != brim_extruder)
            gcode += this->set_extruder(brim_extruder, print_z);
        gcode += this->emit_belt_brim_bands(print, layers, single_object_instance_idx, brim_extruder);
    }

    result.gcode = std::move(gcode);
    return result;
}

std::string GCode::emit_belt_brim_bands(const Print                     &print,
                                        const std::vector<LayerToPrint> &layers,
                                        const size_t                     single_object_instance_idx,
                                        const unsigned int               extruder_id)
{
    std::string gcode;
    for (const LayerToPrint &ltp : layers) {
        const BeltBrimBand *band = ltp.belt_brim_band;
        if (band == nullptr || band->fills.empty() || ltp.original_object == nullptr)
            continue;
        const PrintObject &object = *ltp.original_object;
        if (! object.has_belt_brim() || static_cast<unsigned int>(object.belt_brim_filament() - 1) != extruder_id)
            continue;
        m_config.apply(print.default_region_config());
        m_config.apply(object.config(), true);
        BeltFloorObjectGuard floor_owner{ m_belt_floor_object, &object };
        const size_t i_begin = single_object_instance_idx == size_t(-1) ? 0 : single_object_instance_idx;
        const size_t i_end   = single_object_instance_idx == size_t(-1) ? object.instances().size()
                                                                       : single_object_instance_idx + 1;
        for (size_t i = i_begin; i < i_end && i < object.instances().size(); ++ i) {
            const Point &offset = object.instances()[i].shift;
            this->set_origin(unscale(offset));
            this->on_set_origin(&object, offset);
            m_avoid_crossing_perimeters.use_external_mp();
            for (const ExtrusionEntity *ee : band->fills.entities)
                if (ee != nullptr)
                    gcode += this->extrude_entity(*ee, "brim", NOZZLE_CONFIG(support_speed));
            m_avoid_crossing_perimeters.use_external_mp(false);
            m_avoid_crossing_perimeters.disable_once();
        }
    }
    return gcode;
}

void GCode::mass_load_limited_machine_acceleration(
    const PrintStatistics &curr_print_statistics,
    const Print           &print,
    double                &y_acceleration_limit_res,
    double                &accumulated_mass_res)
{
    double curr_acceleration_y_config = 1e10;
    auto  &machine_max_acceleration_y = print.config().machine_max_acceleration_y.values;
    for (auto &temp : machine_max_acceleration_y)
        if (curr_acceleration_y_config > temp) curr_acceleration_y_config = temp;
    accumulated_mass_res = curr_print_statistics.total_weight;
    double machine_max_force_Y = print.config().machine_max_force_Y.getFloat(),
        machine_bed_mass_Y = print.config().machine_bed_mass_Y.getFloat();
    if (machine_max_force_Y > EPSILON && machine_bed_mass_Y > EPSILON) {
        double virtual_force_g_mms2 = machine_max_force_Y * 1e6,
            curr_acceleration_temp  = curr_acceleration_y_config;
        if (accumulated_mass_res > EPSILON) {
            curr_acceleration_temp = virtual_force_g_mms2 / (machine_bed_mass_Y + accumulated_mass_res);
            y_acceleration_limit_res = std::min(curr_acceleration_temp, curr_acceleration_y_config);
        } else {
            y_acceleration_limit_res = curr_acceleration_y_config;
            BOOST_LOG_TRIVIAL(info) << "mass_load_limited_machine_acceleration: Printed mass not detected";
        }
    } else {
        y_acceleration_limit_res = curr_acceleration_y_config;
    }
}

void GCode::compute_farthest_point(const std::vector<LayerToPrint> &layers, int most_used_extruder,
                                   const std::map<std::pair<const SupportLayer *, ExtrusionRole>, unsigned int> &support_filaments)
{
    m_farthest_point_timelapse.farthest_point = Point(0, 0);
    m_farthest_point_timelapse.farthest_gcode_pos = Vec2d(0, 0);
    m_farthest_point_timelapse.farthest_extruder_id = 0;
    m_farthest_point_timelapse.farthest_is_photo_head = false;

    int64_t max_dist_sq_ext = -1;
    Point   farthest_point_ext;
    int     farthest_extruder_ext = 0;

    int64_t max_dist_sq_fallback = -1;
    Point   farthest_point_fallback;
    int     farthest_extruder_fallback = 0;

    auto for_each_path = [](const ExtrusionEntity *entity, const auto &fn, const auto &self) -> void {
        if (entity->is_collection()) {
            for (const auto *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
                self(child, fn, self);
        } else if (entity->is_loop()) {
            for (const ExtrusionPath &p : static_cast<const ExtrusionLoop *>(entity)->paths)
                fn(p);
        } else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
            for (const ExtrusionPath &p : mp->paths)
                fn(p);
        } else {
            fn(static_cast<const ExtrusionPath &>(*entity));
        }
    };

    auto is_ext_perimeter_role = [](ExtrusionRole role) -> bool {
        return role == erExternalPerimeter;
    };
    auto is_fallback_role = [](ExtrusionRole role) -> bool {
        return role == erInternalInfill || role == erSolidInfill || role == erTopSolidInfill;
    };
    auto is_candidate_support_role = [](ExtrusionRole role) -> bool {
        return role == erSupportMaterial || role == erSupportMaterialInterface || role == erSupportTransition;
    };

    auto update_max = [](int64_t &max_dsq, Point &out_point, int &out_ext,
                         const Point &p, const Point &shift, int extruder_id) {
        Point global = p + shift;
        int64_t dsq = (int64_t)global.x() * global.x() + (int64_t)global.y() * global.y();
        if (dsq > max_dsq) {
            max_dsq = dsq;
            out_point = global;
            out_ext = extruder_id;
        }
    };

    auto collect_from_path = [&update_max](int64_t &max_dsq, Point &out_point, int &out_ext,
                                           const ExtrusionPath &path, const Point &shift, int extruder_id) {
        const Polyline3 &poly = path.polyline;
        if (poly.points.empty()) return;

        if (!poly.fitting_result.empty()) {
            if (poly.fitting_result.front().start_point_index < poly.points.size())
                update_max(max_dsq, out_point, out_ext,
                           poly.points[poly.fitting_result.front().start_point_index].to_point(), shift, extruder_id);

            for (const PathFittingData &seg : poly.fitting_result) {
                if (seg.path_type == EMovePathType::Linear_move) {
                    for (size_t i = seg.start_point_index; i <= seg.end_point_index && i < poly.points.size(); ++i)
                        update_max(max_dsq, out_point, out_ext, poly.points[i].to_point(), shift, extruder_id);
                } else if (seg.path_type == EMovePathType::Arc_move_cw || seg.path_type == EMovePathType::Arc_move_ccw) {
                    update_max(max_dsq, out_point, out_ext, seg.arc_data.end_point, shift, extruder_id);
                }
            }
        } else {
            for (const Point3 &pt : poly.points)
                update_max(max_dsq, out_point, out_ext, pt.to_point(), shift, extruder_id);
        }
    };

    for (const LayerToPrint &ltp : layers) {
        const PrintObject *print_obj = ltp.original_object;
        if (!print_obj) continue;

        for (const PrintInstance &inst : print_obj->instances()) {
            const Point &shift = inst.shift;

            if (ltp.object_layer) {
                for (const LayerRegion *region : ltp.object_layer->regions()) {
                    const PrintRegionConfig &rcfg = region->region().config();

                    for (const ExtrusionEntity *entity : region->perimeters.entities) {
                        for_each_path(entity, [&](const ExtrusionPath &path) {
                            if (is_ext_perimeter_role(path.role()))
                                collect_from_path(max_dist_sq_ext, farthest_point_ext, farthest_extruder_ext,
                                                  path, shift, (int)get_extruder_id(rcfg.outer_wall_filament_id.value - 1));
                        }, for_each_path);
                    }

                    for (const ExtrusionEntity *entity : region->fills.entities) {
                        for_each_path(entity, [&](const ExtrusionPath &path) {
                            if (!is_fallback_role(path.role())) return;
                            int eid = (path.role() == erInternalInfill)
                                ? (int)get_extruder_id(rcfg.sparse_infill_filament_id.value - 1)
                                : (int)get_extruder_id(rcfg.internal_solid_filament_id.value - 1);
                            collect_from_path(max_dist_sq_fallback, farthest_point_fallback, farthest_extruder_fallback,
                                              path, shift, eid);
                        }, for_each_path);
                    }
                }
            }

            if (ltp.support_layer) {
                for (const ExtrusionEntity *entity : ltp.support_layer->support_fills.entities) {
                    for_each_path(entity, [&](const ExtrusionPath &path) {
                        if (!is_candidate_support_role(path.role())) return;
                        auto support_filament = support_filaments.find({ ltp.support_layer, path.role() });
                        if (support_filament == support_filaments.end()) return;
                        int eid = (int)get_extruder_id(support_filament->second);
                        collect_from_path(max_dist_sq_fallback, farthest_point_fallback, farthest_extruder_fallback,
                                          path, shift, eid);
                    }, for_each_path);
                }
            }
        }
    }

    int64_t max_dist_sq;
    if (max_dist_sq_ext > 0) {
        max_dist_sq = max_dist_sq_ext;
        m_farthest_point_timelapse.farthest_point = farthest_point_ext;
        m_farthest_point_timelapse.farthest_extruder_id = farthest_extruder_ext;
    } else {
        max_dist_sq = max_dist_sq_fallback;
        m_farthest_point_timelapse.farthest_point = farthest_point_fallback;
        m_farthest_point_timelapse.farthest_extruder_id = farthest_extruder_fallback;
    }

    if (max_dist_sq > 0) {
        m_farthest_point_timelapse.farthest_gcode_pos = unscale(m_farthest_point_timelapse.farthest_point);
        bool single_nozzle = (m_config.nozzle_diameter.size() <= 1);
        m_farthest_point_timelapse.farthest_is_photo_head = single_nozzle || (m_farthest_point_timelapse.farthest_extruder_id == most_used_extruder);
    }
}

std::string GCode::generate_timelapse_gcode(const Print &print, coordf_t print_z, int most_used_extruder,
                                            const std::set<size_t> *layer_object_label_ids,
                                            const std::vector<const PrintObject*> *printed_objects,
                                            bool skip_pos_pick)
{
    if (!m_writer.filament())
        return {};

    PosPickCtx ctx;
    ctx.curr_pos = { (coord_t)(scale_(m_writer.get_position().x())),(coord_t)(scale_(m_writer.get_position().y())) };
    ctx.curr_layer = this->layer();
    ctx.curr_extruder_id = m_writer.filament()->extruder_id();
    ctx.picture_extruder_id = most_used_extruder;
    if (m_farthest_point_timelapse.enabled) {
        Vec3d po = print.get_plate_origin();
        ctx.farthest_point = m_farthest_point_timelapse.farthest_point - Point(scale_(po.x()), scale_(po.y()));
    }
    if (m_config.print_sequence == PrintSequence::ByObject && printed_objects)
        ctx.printed_objects = *printed_objects;

    auto timelapse_pos = skip_pos_pick ? DefaultTimelapsePos : m_timelapse_pos_picker.pick_pos(ctx);

    std::string timelapse_gcode;
    if (!print.config().time_lapse_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        config.set_key_value("most_used_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(most_used_extruder)));
        config.set_key_value("curr_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(ctx.curr_extruder_id)));
        config.set_key_value("timelapse_pos_x", new ConfigOptionInt((int)timelapse_pos.x()));
        config.set_key_value("timelapse_pos_y", new ConfigOptionInt((int)timelapse_pos.y()));
        config.set_key_value("has_timelapse_safe_pos", new ConfigOptionBool(timelapse_pos != DefaultTimelapsePos));
        config.set_key_value("timelapse_inline_photo", new ConfigOptionBool(skip_pos_pick));
        config.set_key_value("farthest_point_timelapse_enabled", new ConfigOptionBool(m_farthest_point_timelapse.enabled));
        config.set_key_value("clear_to_x0", new ConfigOptionBool(m_timelapse_pos_picker.get_is_clear_to_x0(ctx)));
        timelapse_gcode = this->placeholder_parser_process("timelapse_gcode", print.config().time_lapse_gcode.value, m_writer.filament()->id(), &config) + "\n";
    }

    if (!timelapse_gcode.empty()) {
        m_writer.set_current_position_clear(false);

        double temp_z_after_tool_change;
        if (GCodeProcessor::get_last_z_from_gcode(timelapse_gcode, temp_z_after_tool_change)) {
            Vec3d pos = m_writer.get_position();
            pos(2)    = temp_z_after_tool_change;
            m_writer.set_position(pos);
        }
    }

    if (layer_object_label_ids &&
        is_BBL_Printer() &&
        (print.num_object_instances() <= g_max_label_object) &&
        (print.num_object_instances() > 1) &&
        (!layer_object_label_ids->empty()) &&
        (print.calib_params().mode == CalibMode::Calib_None)) {
        std::ostringstream oss;
        for (auto it = layer_object_label_ids->begin(); it != layer_object_label_ids->end(); ++it) {
            if (it != layer_object_label_ids->begin()) oss << ",";
            oss << *it;
        }

        std::string start_str = std::string("; object ids of layer ") + std::to_string(m_layer_index + 1) + (" start: ") + oss.str() + "\n";
        start_str += "M624 " + _encode_label_ids_to_base64(std::vector<size_t>(layer_object_label_ids->begin(), layer_object_label_ids->end())) + "\n";

        std::string end_str = std::string("; object ids of this layer") + std::to_string(m_layer_index + 1) + (" end: ") + oss.str() + "\n";
        end_str   += "M625\n";

        timelapse_gcode = start_str + timelapse_gcode + end_str;
    }

    return timelapse_gcode;
}

LayerResult GCode::process_layer(
    const Print                    			&print,
    const std::vector<LayerToPrint> 		&layers,
    const LayerTools        		        &layer_tools,
    const bool                               last_layer,
    const std::vector<const PrintInstance*> *ordering,
    const int                               most_used_extruder,
    const size_t                     		 single_object_instance_idx,
    const bool                               prime_extruder)
{
    assert(! layers.empty());
    assert(single_object_instance_idx == size_t(-1) || layers.size() == 1);

    const Layer         *object_layer  = nullptr;
    const SupportLayer  *support_layer = nullptr;
    const SupportLayer  *raft_layer    = nullptr;
    for (const LayerToPrint &l : layers) {
        if (l.object_layer && ! object_layer)
            object_layer = l.object_layer;
        if (l.support_layer) {
            if (! support_layer)
                support_layer = l.support_layer;
            if (! raft_layer && support_layer->id() < support_layer->object()->slicing_parameters().raft_layers())
                raft_layer = support_layer;
        }
    }

    if (object_layer == nullptr && support_layer == nullptr &&
        std::any_of(layers.begin(), layers.end(),
                    [](const LayerToPrint &l) { return l.belt_brim_band != nullptr; }))
        return this->process_belt_brim_layer(print, layers, layer_tools, last_layer, single_object_instance_idx);

    const Layer* layer_ptr = nullptr;
    if (object_layer != nullptr)
        layer_ptr = object_layer;
    else if (support_layer != nullptr)
        layer_ptr = support_layer;
    const Layer& layer = *layer_ptr;
    m_cur_layer_idx = layer.id();
    if (Extruder *cur_filament = m_writer.filament())
        cur_filament->set_config_index((int)get_filament_config_index((int)cur_filament->id()));
    LayerResult   result { {}, layer.id(), false, last_layer };
    if (layer_tools.extruders.empty())
        return result;

    coordf_t             print_z       = layer.print_z;
    bool                 first_layer   = (layer.id() == 0 && abs(layer.bottom_z()) < EPSILON);
    m_writer.set_is_first_layer(first_layer);
    unsigned int         first_extruder_id = layer_tools.extruders.front();

    m_config.apply(layer.object()->config(), true);

    m_enable_loop_clipping = true;
    if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
        bool enable = layer_tools.extruders.size() == 1 && (layer.id() > 0 || !print.has_brim()) && (layer.id() >= (size_t)print.config().skirt_height.value && ! print.has_infinite_skirt());
        if (enable) {
            for (const LayerRegion *layer_region : layer.regions())
                if (size_t(layer_region->region().config().bottom_shell_layers.value) > layer.id() ||
                    layer_region->perimeters.items_count() > 1u ||
                    layer_region->fills.items_count() > 0) {
                    enable = false;
                    break;
                }
        }
        if (Point lid; enable && object_layer != nullptr && !Nonplanar::lid_start(*object_layer->object(), lid)) {
            const PrintObject &object = *object_layer->object();
            if (!object_layer->conformal_patches_before.empty() || !object_layer->conformal_patches_after.empty())
                enable = false;
            for (const auto &patch : object.conformal().patches)
                if (patch.kind == Nonplanar::PatchKind::Top && object.layers()[patch.host_layer]->print_z <= print_z + EPSILON) {
                    enable = false;
                    break;
                }
        }
        result.spiral_vase_enable = enable;
        m_enable_loop_clipping = !enable;
    }

    std::string gcode;
    assert(is_decimal_separator_point());
    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    char buf[80];
    sprintf(buf, print.is_BBL_printer() ? "; Z_HEIGHT: %g\n" : ";Z:%g\n", print_z);
    gcode += buf;
    float height = first_layer ? static_cast<float>(print_z) : static_cast<float>(print_z) - m_last_layer_z;
    sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), height);
    gcode += buf;
    m_last_layer_z = static_cast<float>(print_z);
    m_max_layer_z  = std::max(m_max_layer_z, m_last_layer_z);
    m_last_height = height;

    if (! m_config.before_layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num",   new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z",     new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("before_layer_change_gcode",
            print.config().before_layer_change_gcode.value, m_writer.filament()->id(), &config)
            + "\n";
    }

    PrinterStructure printer_structure           = m_config.printer_structure.value;
    bool is_i3_printer = printer_structure == PrinterStructure::psI3;
    bool is_multi_extruder = m_config.nozzle_diameter.size() > 1;

    m_farthest_point_timelapse.enabled = m_config.farthest_point_timelapse.value
        && m_config.timelapse_type.value == TimelapseType::tlTraditional
        && printer_structure != PrinterStructure::psI3;
    m_farthest_point_timelapse.most_used_extruder = most_used_extruder;
    m_farthest_point_timelapse.inserted_this_layer = false;

    bool need_insert_timelapse_gcode_for_traditional = false;
    if ((!m_wipe_tower || !m_wipe_tower->enable_timelapse_print()) && (is_BBL_Printer() || !m_config.time_lapse_gcode.value.empty())) {
        need_insert_timelapse_gcode_for_traditional = ((is_i3_printer && !m_spiral_vase) || is_multi_extruder);
    }

    bool has_insert_timelapse_gcode = false;
    bool has_wipe_tower             = (layer_tools.has_wipe_tower && m_wipe_tower);


    ZHopType z_hope_type = ZHopType(FILAMENT_CONFIG(z_hop_types));
    LiftType auto_lift_type = LiftType::NormalLift;
    if (z_hope_type == ZHopType::zhtAuto || z_hope_type == ZHopType::zhtSpiral || z_hope_type == ZHopType::zhtSlope)
        auto_lift_type = LiftType::SpiralLift;

    m_ordered_plan_layer = layer.object() != nullptr && layer.object()->conformal().ordered_plan;
    gcode += this->change_layer(print_z);
    update_layer_related_config(m_layer_index);
    update_placeholder_parser_with_variant_params();
    m_layer = &layer;
    m_object_layer_over_raft = false;

    if (!need_insert_timelapse_gcode_for_traditional && !m_config.time_lapse_gcode.value.empty() && !is_BBL_Printer()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("timelapse_gcode",
             print.config().time_lapse_gcode.value, m_writer.filament()->id(), &config)
             + "\n";
    }

    if (!m_config.layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("most_used_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(most_used_extruder)));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",   new ConfigOptionFloat(print_z));
        {
            const int  cur_filament_id = (int) m_writer.filament()->id();
            const auto group_result    = m_print->get_layered_nozzle_group_result();
            config.set_key_value("current_filament_id", new ConfigOptionInt(cur_filament_id));
            config.set_key_value("current_nozzle_id", new ConfigOptionInt(
                nozzle_id_for_gcode_placeholder(group_result, cur_filament_id, (int) m_writer.filament()->extruder_id(), m_layer_index)));
        }

        PrintStatistics curr_print_statistics;
        for (const Extruder &extruder : m_writer.extruders()) {
            double extruded_volume = extruder.extruded_volume() + (has_wipe_tower ? print.wipe_tower_data().used_filament[extruder.id()] * 2.4052f : 0.f);
            double filament_weight = extruded_volume * extruder.filament_density() * 0.001;
            if (filament_weight > 0.)
                curr_print_statistics.total_weight += filament_weight;
        }
        double curr_y_acceleration_limit = -1, curr_accumulated_mass = -1;
        mass_load_limited_machine_acceleration(curr_print_statistics, print, curr_y_acceleration_limit, curr_accumulated_mass);
        double curr_layer_mass = curr_print_statistics.total_weight - m_last_layer_accumulated_mass;
        if (curr_layer_mass <= EPSILON) curr_layer_mass = 0.0;
        m_last_layer_accumulated_mass = curr_print_statistics.total_weight;
        config.set_key_value("curr_y_acceleration_limit", new ConfigOptionFloat(curr_y_acceleration_limit));
        config.set_key_value("curr_accumulated_mass", new ConfigOptionFloat(curr_accumulated_mass));
        config.set_key_value("curr_layer_mass", new ConfigOptionFloat(curr_layer_mass));

        gcode += this->placeholder_parser_process("layer_change_gcode",
            print.config().layer_change_gcode.value, m_writer.filament()->id(), &config)
            + "\n";
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
    }
    
    gcode += ";_SET_FAN_SPEED_CHANGING_LAYER\n";

    double calib_z = print_z;
    if (m_config.belt_printer.value && print.calib_mode() != CalibMode::Calib_None) {
        for (const Layer* l : layer.object()->layers())
            if (!l->lslices.empty()) {
                calib_z = print_z - (l->print_z - l->height);
                break;
            }
    }
    switch (print.calib_mode()) {
        case CalibMode::Calib_PA_Tower: {
            gcode += writer().set_pressure_advance(this->interpolate_value_across_layers(static_cast<float>(print.calib_params().start),
                                                                                         static_cast<float>(print.calib_params().end),
                                                                                         static_cast<float>(print.calib_params().step)));
            break;
        }
        case CalibMode::Calib_Temp_Tower: {
            int sectioned_temp = 0;
            if (m_config.belt_printer.value &&
                sscanf(layer.object()->model_object()->name.c_str(), "temp_%d", &sectioned_temp) == 1 &&
                sectioned_temp > 0) {
                gcode += writer().set_temperature(static_cast<unsigned int>(sectioned_temp));
            } else {
                gcode += writer().set_temperature(this->interpolate_value_across_layers(static_cast<float>(print.calib_params().start), static_cast<float>(print.calib_params().end), 5.0f));
            }
            break;
        }
        case CalibMode::Calib_VFA_Tower: {
            float _speed = this->interpolate_value_across_layers(static_cast<float>(print.calib_params().start),
                                                                 static_cast<float>(print.calib_params().end),
                                                                 static_cast<float>(print.calib_params().step));
            m_calib_config.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable({std::round(_speed)}));
            break;
        }
        case CalibMode::Calib_Vol_speed_Tower: {
            auto _speed = print.calib_params().start + std::max(0.0, calib_z) * print.calib_params().step;
            m_calib_config.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable({std::round(_speed)}));
            break;
        }
        case CalibMode::Calib_Retraction_tower: {
            auto _length = print.calib_params().start + std::floor(std::max(0.0,calib_z-0.4)) * print.calib_params().step;
            DynamicConfig _cfg;
            _cfg.set_key_value("retraction_length", new ConfigOptionFloats{_length});
            writer().config.apply(_cfg);
            sprintf(buf, "; Calib_Retraction_tower: Z_HEIGHT: %g, length:%g\n", calib_z, _length);
            gcode += buf;
            break;
        }
        case CalibMode::Calib_Input_shaping_freq: {
            if (m_layer_index == 1){
                gcode += writer().set_input_shaping('A', print.calib_params().start, 0.f, print.calib_params().shaper_type);
                if (m_writer.get_gcode_flavor() == gcfKlipper) {
                    gcode += "SET_VELOCITY_LIMIT MINIMUM_CRUISE_RATIO=0\n";
                }
            } else {
                if (print.calib_params().freqStartX == print.calib_params().freqStartY && print.calib_params().freqEndX == print.calib_params().freqEndY) {
                    gcode += writer().set_input_shaping('A', 0.f, this->interpolate_value_across_layers(print.calib_params().freqStartX, print.calib_params().freqEndX), "");
                } else {
                    gcode += writer().set_input_shaping('X', 0.f, this->interpolate_value_across_layers(print.calib_params().freqStartX, print.calib_params().freqEndX), "");
                    gcode += writer().set_input_shaping('Y', 0.f, this->interpolate_value_across_layers(print.calib_params().freqStartY, print.calib_params().freqEndY), "");
                }
            }
            break;
        }
        case CalibMode::Calib_Input_shaping_damp: {
            if (m_layer_index == 1){
                if (m_writer.get_gcode_flavor() == gcfKlipper) {
                    gcode += "SET_VELOCITY_LIMIT MINIMUM_CRUISE_RATIO=0\n";
                }
                gcode += writer().set_input_shaping('X', 0.f, print.calib_params().freqStartX, print.calib_params().shaper_type);
                gcode += writer().set_input_shaping('Y', 0.f, print.calib_params().freqStartY, print.calib_params().shaper_type);
            } else {
                gcode += writer().set_input_shaping('A', this->interpolate_value_across_layers(print.calib_params().start, print.calib_params().end), 0.f, "");
            }
            break;
        }
        case CalibMode::Calib_Cornering: {
            if (m_writer.get_gcode_flavor() == gcfMarlinFirmware &&
                !m_config.machine_max_junction_deviation.values.empty() &&
                m_config.machine_max_junction_deviation.values.front() > 0) {
                gcode += writer().set_junction_deviation(this->interpolate_value_across_layers(print.calib_params().start, print.calib_params().end));
            } else {
                gcode += writer().set_jerk_xy(this->interpolate_value_across_layers(print.calib_params().start, print.calib_params().end));
            }
            break;
        }
    }

    if (first_layer) {
        if (NOZZLE_CONFIG(default_acceleration) > 0 && NOZZLE_CONFIG(initial_layer_acceleration) > 0) {
            gcode += m_writer.set_print_acceleration((unsigned int)floor(NOZZLE_CONFIG(initial_layer_acceleration) + 0.5));
        }

        if (NOZZLE_CONFIG(default_jerk) > 0 && NOZZLE_CONFIG(initial_layer_jerk) > 0) {
            gcode += m_writer.set_jerk_xy(NOZZLE_CONFIG(initial_layer_jerk));
        }

        if (m_writer.get_gcode_flavor() == gcfMarlinFirmware && NOZZLE_CONFIG(default_junction_deviation) > 0) {
            gcode += m_writer.set_junction_deviation(NOZZLE_CONFIG(default_junction_deviation));
        }
    }

    bool past_first_layer_band = !first_layer;
    if (int past = this->belt_layer_past_first_layer_band(object_layer); past >= 0)
        past_first_layer_band = past > 0;

    if (past_first_layer_band && !m_second_layer_things_done) {
        const auto plr_mode = print.config().enable_power_loss_recovery.value;
        gcode += m_writer.enable_power_loss_recovery(plr_mode);

        if (print.is_BBL_printer()) {
            if (print.config().scan_first_layer.value) {
                gcode += this->retract();
                gcode += "M976 S1 P1 ; scan model before printing 2nd layer\n";
                gcode += "M400 P100\n";
                gcode += this->unretract();
            }
        }
      if (NOZZLE_CONFIG(default_acceleration) > 0 && NOZZLE_CONFIG(initial_layer_acceleration) > 0) {
        gcode += m_writer.set_print_acceleration((unsigned int) floor(NOZZLE_CONFIG(default_acceleration) + 0.5));
      }

      if (NOZZLE_CONFIG(default_jerk) > 0 && NOZZLE_CONFIG(initial_layer_jerk) > 0) {
        gcode += m_writer.set_jerk_xy(NOZZLE_CONFIG(default_jerk));
      }

        if (!m_imex_parallel_mode.empty() && m_imex_parallel_mode != kImexPrimaryMode) {
            const int num_filament_slots = (int) print.config().filament_diameter.values.size();
            const int initial_physical =
                ((int) first_extruder_id >= 0 &&
                 (int) first_extruder_id < (int) m_config.physical_extruder_map.values.size())
                    ? m_config.physical_extruder_map.values[(int) first_extruder_id]
                    : -1;
            for (int tool_idx : get_imex_active_tools(print)) {
                const int logical = (tool_idx == initial_physical)
                    ? (int)first_extruder_id
                    : resolve_filament_for_head(
                          m_imex_head_filament_map, m_config.physical_extruder_map, tool_idx);
                if (logical < 0 || logical >= num_filament_slots) continue;
                int temperature = print.config().nozzle_temperature.get_at(get_filament_config_index(logical));
                if (temperature > 0)
                    gcode += GCodeWriter::set_temperature(temperature, m_writer.config.gcode_flavor, false,
                        tool_idx, "set IMEX tool temperature");
            }
        } else {
            for (const Extruder& extruder : m_writer.extruders()) {
                if ((print.config().single_extruder_multi_material.value || m_ooze_prevention.enable) &&
                    extruder.id() != m_writer.filament()->id())
                    continue;
                size_t fi = get_filament_config_index((int)extruder.id());
                int temperature = print.config().nozzle_temperature.get_at(fi);
                if (temperature > 0 && temperature != print.config().nozzle_temperature_initial_layer.get_at(fi))
                    gcode += m_writer.set_temperature(temperature, false, extruder.id());
            }
        }

        int bed_temp = 0;
        if (m_config.bed_temperature_formula == BedTempFormula::btfHighestTemp)
            bed_temp = get_highest_bed_temperature(false,print);
        else
            bed_temp = get_bed_temperature(first_extruder_id, false, m_config.curr_bed_type);
        gcode += m_writer.set_bed_temperature(bed_temp);
        m_second_layer_things_done = true;
    }

    if (single_object_instance_idx == size_t(-1)) {
        gcode += ProcessLayer::emit_custom_gcode_per_print_z(*this, layer_tools.custom_gcode, m_writer.filament()->id(), first_extruder_id, print.is_BBL_printer(), print.config());
        if (layer_tools.custom_gcode && pressure_advance_pattern_supported(*layer_tools.custom_gcode, m_writer, print.config())) {
            const Vec3d position = m_writer.get_position();
            m_nominal_z = position.z() - m_writer.get_zhop();
            set_last_pos(gcode_to_point({position.x(), position.y()}));
            m_wipe.reset_path();
            m_max_layer_z = std::max(m_max_layer_z, float(pressure_advance_pattern_max_z(*layer_tools.custom_gcode) - m_config.z_offset.value));
        }
    }

    auto get_next_extruder = [&](int current_extruder,const std::vector<unsigned int>&extruders) {
        std::vector<float> flush_matrix(cast<float>(get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, 0, m_config.nozzle_diameter.values.size())));
        const unsigned int number_of_extruders = (unsigned int)(sqrt(flush_matrix.size()) + EPSILON);
        std::vector<std::vector<float>> wipe_volumes;
        for (unsigned int i = 0; i < number_of_extruders; ++i)
            wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * number_of_extruders, flush_matrix.begin() + (i + 1) * number_of_extruders));
        unsigned int next_extruder = current_extruder;
        float min_flush = std::numeric_limits<float>::max();
        for (auto extruder_id : extruders) {
            if (print.config().filament_soluble.get_at(extruder_id) || extruder_id == current_extruder)
                continue;
            if (wipe_volumes[current_extruder][extruder_id] < min_flush) {
                next_extruder = extruder_id;
                min_flush = wipe_volumes[current_extruder][extruder_id];
            }
        }
        return next_extruder;
    };
    
    const bool overhang_fan   = overhang_fan_enabled(m_config, m_enable_cooling_markers);
    auto       overhang_speed = [this](const LayerRegion &region) {
        return bool(region.region().config().enable_overhang_speed.get_at(get_nozzle_config_index(m_writer.filament()->id())));
    };
    for (const auto &layer_to_print : layers)
        if (layer_to_print.object_layer && prepares_overhang_estimator(*layer_to_print.object_layer, overhang_fan, overhang_speed))
            m_extrusion_quality_estimator.prepare_for_new_layer(layer_to_print.original_object, layer_to_print.object_layer);

    std::map<unsigned int, std::vector<ObjectByExtruder>> by_extruder;
    std::map<std::pair<const SupportLayer *, ExtrusionRole>, unsigned int> support_filaments;
    std::vector<std::unique_ptr<ExtrusionEntityCollection>> split_perimeter_storage;
    bool is_anything_overridden = const_cast<LayerTools&>(layer_tools).wiping_extrusions().is_anything_overridden();
    for (const LayerToPrint &layer_to_print : layers) {
        if (layer_to_print.support_layer != nullptr) {
            const SupportLayer &support_layer = *layer_to_print.support_layer;
            const PrintObject& object = *layer_to_print.original_object;
            if (! support_layer.support_fills.entities.empty()) {
                ExtrusionRole   role               = support_layer.support_fills.role();
                bool            has_support        = role == erMixed || role == erSupportMaterial || role == erSupportTransition;
                bool            has_interface      = role == erMixed || role == erSupportMaterialInterface;
                unsigned int    support_extruder   = object.config().support_filament.value - 1;
                bool            support_dontcare   = object.config().support_filament.value == 0;
                unsigned int    interface_extruder = object.config().support_interface_filament.value - 1;
                bool            interface_dontcare = object.config().support_interface_filament.value == 0;

                WipingExtrusions& wiping_extrusions = const_cast<LayerTools&>(layer_tools).wiping_extrusions();
                if (support_dontcare) {
                    int extruder_override = wiping_extrusions.get_support_extruder_overrides(&object);
                    if (extruder_override >= 0) {
                        support_extruder = extruder_override;
                        support_dontcare = false;
                    }
                }

                if (interface_dontcare) {
                    int extruder_override = wiping_extrusions.get_support_interface_extruder_overrides(&object);
                    if (extruder_override >= 0) {
                        interface_extruder = extruder_override;
                        interface_dontcare = false;
                    }
                }

                if (support_dontcare && !interface_dontcare) {
                    unsigned int dontcare_extruder = first_extruder_id;
                    for (unsigned int extruder_id : layer_tools.extruders) {
                        if (print.config().filament_soluble.get_at(extruder_id))
                            continue;

                        if (extruder_id == interface_extruder)
                            continue;

                        dontcare_extruder = extruder_id;
                        break;
                    }
                #if 0
                    if (dontcare_extruder == interface_extruder && (object.config().support_interface_not_for_body && object.config().support_interface_filament.value!=0)) {
                        auto all_extruders = print.extruders();
                        dontcare_extruder = get_next_extruder(dontcare_extruder, all_extruders);
                    }
                #endif

                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                }
                else if (support_dontcare || interface_dontcare) {
                    unsigned int dontcare_extruder = first_extruder_id;
                    if (print.config().filament_soluble.get_at(dontcare_extruder)) {
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (! print.config().filament_soluble.get_at(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (print.config().filament_is_support.get_at(dontcare_extruder)) {
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (!print.config().filament_is_support.get_at(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                    if (interface_dontcare)
                        interface_extruder = dontcare_extruder;
                }
                bool single_extruder = ! has_support || support_extruder == interface_extruder;
                if (has_support) {
                    support_filaments[{ &support_layer, erSupportMaterial }] = support_extruder;
                    support_filaments[{ &support_layer, erSupportTransition }] = support_extruder;
                }
                if (has_interface) {
                    support_filaments[{ &support_layer, erSupportMaterialInterface }] =
                        single_extruder ? (has_support ? support_extruder : interface_extruder) : interface_extruder;
                }
                ObjectByExtruder &obj = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder, &layer_to_print - layers.data(), layers.size());
                obj.support = &support_layer.support_fills;
                obj.support_extrusion_role = single_extruder ? erMixed : erSupportMaterial;
                if (! single_extruder && has_interface) {
                    ObjectByExtruder &obj_interface = object_by_extruder(by_extruder, interface_extruder, &layer_to_print - layers.data(), layers.size());
                    obj_interface.support = &support_layer.support_fills;
                    obj_interface.support_extrusion_role = erSupportMaterialInterface;
                }
            }
        }

        if (layer_to_print.object_layer != nullptr) {
            const Layer &layer = *layer_to_print.object_layer;
            size_t n_slices = layer.lslices.size();
            const std::vector<BoundingBox> &layer_surface_bboxes = layer.lslices_bboxes;
            std::vector<size_t> slices_test_order;
            slices_test_order.reserve(n_slices);
            for (size_t i = 0; i < n_slices; ++ i)
                slices_test_order.emplace_back(i);
            std::sort(slices_test_order.begin(), slices_test_order.end(), [&layer_surface_bboxes](size_t i, size_t j) {
                const Vec2d s1 = layer_surface_bboxes[i].size().cast<double>();
                const Vec2d s2 = layer_surface_bboxes[j].size().cast<double>();
                return s1.x() * s1.y() < s2.x() * s2.y();
            });
            auto point_inside_surface = [&layer, &layer_surface_bboxes](const size_t i, const Point &point) {
                const BoundingBox &bbox = layer_surface_bboxes[i];
                return point(0) >= bbox.min(0) && point(0) < bbox.max(0) &&
                       point(1) >= bbox.min(1) && point(1) < bbox.max(1) &&
                       layer.lslices[i].contour.contains(point);
            };

            for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
                const LayerRegion *layerm = layer.regions()[region_id];
                if (layerm == nullptr)
                    continue;
                const PrintRegion &region = print.get_print_region(layerm->region().print_region_id());

                std::vector<unsigned int> printing_extruders;
                for (const ObjectByExtruder::Island::Region::Type entity_type : { ObjectByExtruder::Island::Region::INFILL, ObjectByExtruder::Island::Region::PERIMETERS }) {
                    for (const ExtrusionEntity *ee : (entity_type == ObjectByExtruder::Island::Region::INFILL) ? layerm->fills.entities : layerm->perimeters.entities) {
                        assert(dynamic_cast<const ExtrusionEntityCollection*>(ee) != nullptr);
                        const auto *extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                        if (extrusions->entities.empty())
                            continue;

                        auto process_extrusions = [&](const ExtrusionEntityCollection *current_extrusions,
                                                       const ExtrusionEntityCollection *overrides_key,
                                                       bool                             use_overrides) {
                            int correct_extruder_id = layer_tools.extruder(*current_extrusions, region);

                            const WipingExtrusions::ExtruderPerCopy *entity_overrides = nullptr;
                            if (! layer_tools.has_extruder(correct_extruder_id)) {
                                if (!layer_tools.is_mixed_slot(correct_extruder_id)) {
                                    correct_extruder_id = layer_tools.extruders.back();
                                }
                            }
                            printing_extruders.clear();
                            if (is_anything_overridden && use_overrides) {
                                entity_overrides = const_cast<LayerTools&>(layer_tools).wiping_extrusions().get_extruder_overrides(overrides_key, layer_to_print.original_object, correct_extruder_id, layer_to_print.object()->instances().size());
                                if (entity_overrides == nullptr) {
                                    printing_extruders.emplace_back(correct_extruder_id);
                                } else {
                                    printing_extruders.reserve(entity_overrides->size());
                                    for (int extruder : *entity_overrides)
                                        printing_extruders.emplace_back(extruder >= 0 ?
                                            extruder :
                                            static_cast<unsigned int>(- extruder - 1));
                                    Slic3r::sort_remove_duplicates(printing_extruders);
                                }
                            } else {
                                printing_extruders.emplace_back(correct_extruder_id);
                            }

                            for (unsigned int extruder : printing_extruders) {
                                std::vector<ObjectByExtruder::Island> &islands = object_islands_by_extruder(
                                    by_extruder,
                                    extruder,
                                    &layer_to_print - layers.data(),
                                    layers.size(), n_slices + 1);
                                for (size_t i = 0; i <= n_slices; ++i) {
                                    bool   last       = i == n_slices;
                                    size_t island_idx = last ? n_slices : slices_test_order[i];
                                    if (last || point_inside_surface(island_idx, current_extrusions->first_point())) {
                                        if (islands[island_idx].by_region.empty())
                                            islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                        islands[island_idx].by_region[region.print_region_id()].append(entity_type, current_extrusions, entity_overrides);
                                        break;
                                    }
                                }
                            }
                        };

                        bool split_mixed_perimeters =
                            entity_type == ObjectByExtruder::Island::Region::PERIMETERS &&
                            region.config().outer_wall_filament_id.value != region.config().inner_wall_filament_id.value &&
                            extrusions->role() == erMixed;

                        if (split_mixed_perimeters) {
                            auto outer_perimeters = std::make_unique<ExtrusionEntityCollection>();
                            auto inner_perimeters = std::make_unique<ExtrusionEntityCollection>();
                            for (const ExtrusionEntity *entity : extrusions->entities) {
                                const ExtrusionRole role = entity->role();
                                if (role == erExternalPerimeter || role == erOverhangPerimeter)
                                    outer_perimeters->append(*entity);
                                else if (role == erPerimeter)
                                    inner_perimeters->append(*entity);
                            }

                            if (!outer_perimeters->entities.empty()) {
                                split_perimeter_storage.emplace_back(std::move(outer_perimeters));
                                process_extrusions(split_perimeter_storage.back().get(), nullptr, false);
                            }
                            if (!inner_perimeters->entities.empty()) {
                                split_perimeter_storage.emplace_back(std::move(inner_perimeters));
                                process_extrusions(split_perimeter_storage.back().get(), nullptr, false);
                            }
                        } else {
                            process_extrusions(extrusions, extrusions, true);
                        }
                    }
                }
            }
        }
    }
    if (m_farthest_point_timelapse.enabled)
        compute_farthest_point(layers, most_used_extruder, support_filaments);

    std::map<unsigned int, std::pair<std::vector<InstanceToPrint>, std::vector<InstanceVisit>>> filament_to_print_instances;
    {
        const bool island_level_ordering = print.config().print_sequence != PrintSequence::ByObject &&
            single_object_instance_idx == size_t(-1) &&
            print.config().print_order != PrintOrder::AsObjectList;
        std::vector<unsigned int> plan_filaments = layer_tools.extruders;
        for (const auto &grp : layer_tools.mixed_sub_layer_groups)
            if (std::find(plan_filaments.begin(), plan_filaments.end(), grp.mixed_slot_0based) == plan_filaments.end())
                plan_filaments.push_back(grp.mixed_slot_0based);

        for (unsigned int filament_id : plan_filaments) {
            auto objects_by_extruder_it = by_extruder.find(filament_id);
            if (objects_by_extruder_it == by_extruder.end()) continue;

            auto &filament_plan = filament_to_print_instances[filament_id];

            if (!island_level_ordering) {
                filament_plan.first = sort_print_object_instances(objects_by_extruder_it->second, layers, ordering, single_object_instance_idx);
                filament_plan.second.reserve(filament_plan.first.size());
                for (size_t i = 0; i < filament_plan.first.size(); ++i)
                    filament_plan.second.push_back({i, {}, true});
                continue;
            }

            int   plate_idx = print.get_plate_index();
            Point wt_pos(print.config().wipe_tower_x.get_at(plate_idx), print.config().wipe_tower_y.get_at(plate_idx));

            std::vector<GCode::ObjectByExtruder> &objects_by_extruder = objects_by_extruder_it->second;
            std::vector<InstanceToPrint> &instances = filament_plan.first;
            std::vector<IslandOrderNode> nodes;
            std::vector<std::pair<size_t, bool>> layout;
            std::vector<size_t>          node_instances;
            auto quantize_to_mm = [](const Point &pt) -> Point {
                const coord_t grid = coord_t(scale_(1.));
                auto q = [grid](coord_t v) -> coord_t {
                    return ((v >= 0 ? v + grid / 2 : v - grid / 2) / grid) * grid;
                };
                return Point(q(pt.x()), q(pt.y()));
            };
            for (ObjectByExtruder &object_by_extruder : objects_by_extruder) {
                if (object_by_extruder.islands.empty() && (object_by_extruder.support == nullptr || object_by_extruder.support->empty())) continue;

                const size_t       layer_id     = &object_by_extruder - objects_by_extruder.data();
                const PrintObject *print_object = layers[layer_id].original_object;
                if (print_object == nullptr)
                    continue;
                const Layer *obj_layer = layers[layer_id].object_layer;
                std::vector<ObjectByExtruder::Island> &islands = object_by_extruder.islands;
                const bool islands_chainable = obj_layer != nullptr && islands.size() == obj_layer->lslices.size() + 1;
                for (size_t instance_id = 0; instance_id < print_object->instances().size(); ++instance_id) {
                    const size_t instance_idx = instances.size();
                    instances.emplace_back(object_by_extruder, layer_id, *print_object, instance_id,
                                           print_object->instances()[instance_id].model_instance->get_labeled_id());
                    layout.emplace_back(islands.size(), ! islands.empty() && ! islands.back().by_region.empty());
                    const Point &shift = print_object->instances()[instance_id].shift;
                    const size_t first_node = nodes.size();
                    if (islands_chainable)
                        for (size_t i = 0; i + 1 < islands.size(); ++i)
                            if (!islands[i].by_region.empty()) {
                                nodes.push_back({print_object->id(), instance_id, i,
                                                 quantize_to_mm(obj_layer->lslices[i].contour.centroid() + shift)});
                                node_instances.emplace_back(instance_idx);
                            }
                    if (nodes.size() == first_node) {
                        nodes.push_back({print_object->id(), instance_id, size_t(-1), quantize_to_mm(shift)});
                        node_instances.emplace_back(instance_idx);
                    }
                }
            }

            auto &cache_entry = m_ordering_cache[filament_id];
            if (! (cache_entry.nodes == nodes && cache_entry.layout == layout)) {
                cache_entry.nodes  = nodes;
                cache_entry.layout = layout;
                Points node_points;
                node_points.reserve(nodes.size());
                for (const IslandOrderNode &node : nodes)
                    node_points.emplace_back(node.pos);
                std::vector<size_t> tour = order_points_with_strategy(node_points, print.config().print_order, &wt_pos);
                std::reverse(tour.begin(), tour.end());

                std::vector<InstanceVisit> visits;
                std::vector<bool> instance_seen(instances.size(), false);
                std::vector<int>  last_visit_of_instance(instances.size(), -1);
                for (size_t node_idx : tour) {
                    const size_t instance_idx = node_instances[node_idx];
                    if (visits.empty() || visits.back().instance_idx != instance_idx) {
                        visits.push_back({instance_idx, {}, !instance_seen[instance_idx]});
                        instance_seen[instance_idx] = true;
                    }
                    if (nodes[node_idx].island_idx != size_t(-1))
                        visits.back().islands.emplace_back(nodes[node_idx].island_idx);
                    last_visit_of_instance[instance_idx] = int(visits.size()) - 1;
                }
                for (size_t i = 0; i < instances.size(); ++i) {
                    if (last_visit_of_instance[i] < 0)
                        continue;
                    InstanceVisit &last_visit = visits[size_t(last_visit_of_instance[i])];
                    if (last_visit.islands.empty())
                        continue;
                    std::vector<ObjectByExtruder::Island> &islands = instances[i].object_by_extruder.islands;
                    if (! islands.empty() && ! islands.back().by_region.empty())
                        last_visit.islands.emplace_back(islands.size() - 1);
                }
                cache_entry.visits = std::move(visits);
            }
            filament_plan.second = cache_entry.visits;
        }
    }

    std::set<size_t> layer_object_label_ids;
    for (auto iter = filament_to_print_instances.begin(); iter != filament_to_print_instances.end(); ++iter) {
        for (const InstanceToPrint &instance : iter->second.first) {
            layer_object_label_ids.insert(instance.label_object_id);
        }
    }

    m_farthest_point_timelapse.layer_object_label_ids = layer_object_label_ids;

    auto insert_timelapse_gcode = [this, print_z, &print, &most_used_extruder, &layer_object_label_ids,&printed_objects = std::as_const(m_printed_objects)](bool skip_pos_pick = false) -> std::string {
        return generate_timelapse_gcode(print, print_z, most_used_extruder, &layer_object_label_ids, &printed_objects, skip_pos_pick);
    };

    if (!need_insert_timelapse_gcode_for_traditional  && is_BBL_Printer()
        && !(m_farthest_point_timelapse.enabled && m_farthest_point_timelapse.farthest_is_photo_head)) {
        if (FILAMENT_CONFIG(retract_when_changing_layer)) {
            gcode += this->retract(false, false, auto_lift_type, true);
        }
        gcode += insert_timelapse_gcode();
    }

    if (m_wipe_tower)
        m_wipe_tower->set_is_first_print(true);

    auto insert_wrapping_detection_gcode = [this, &print, &print_z, &most_used_extruder]() -> std::string {
        std::string wrapping_gcode;
        if (print.config().enable_wrapping_detection && !print.config().wrapping_detection_gcode.value.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("most_used_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(most_used_extruder)));
            config.set_key_value("curr_physical_extruder_id", new ConfigOptionInt(m_config.physical_extruder_map.get_at(m_writer.filament()->extruder_id())));
            wrapping_gcode = this->placeholder_parser_process("wrapping_detection_gcode", print.config().wrapping_detection_gcode.value, m_writer.filament()->id(), &config) +"\n";
        }
        m_writer.set_current_position_clear(false);

        double temp_z_after_tool_change;
        if (GCodeProcessor::get_last_z_from_gcode(wrapping_gcode, temp_z_after_tool_change)) {
            Vec3d pos = m_writer.get_position();
            pos(2)    = temp_z_after_tool_change;
            m_writer.set_position(pos);
        }
        return wrapping_gcode;
    };

    bool has_insert_wrapping_detection_gcode = false;

    m_skirt_group_done.resize(print.skirt_brim_groups().size());

    std::set<std::pair<size_t, size_t>> belt_brim_emitted;

    auto emit_belt_brim_for_extruder = [this, &print, &layers, single_object_instance_idx](unsigned int extruder_id) -> std::string {
        return this->emit_belt_brim_bands(print, layers, single_object_instance_idx, extruder_id);
    };

    for (unsigned int extruder_id : layer_tools.extruders)
    {
        if (print.config().skirt_type == stCombined && !print.skirt_brim_groups().empty()) {
            for (size_t group_idx = 0; group_idx < print.skirt_brim_groups().size(); ++group_idx) {
                const Print::SkirtBrimGroup& group = print.skirt_brim_groups()[group_idx];
                if (group.skirt.empty())
                    continue;

                std::string skirt_gcode = generate_skirt(print, group.skirt, Point(0, 0), layer.object()->config().skirt_start_angle,
                                                          layer_tools, layer, extruder_id, m_skirt_group_done[group_idx]);
                gcode += std::move(skirt_gcode);
            }
        }

        if (print.config().print_sequence == PrintSequence::ByLayer && m_enable_exclude_object && print.config().support_object_skip_flush.value) {
            std::set<size_t> all_label_ids;
            for (InstanceToPrint &instance : filament_to_print_instances[extruder_id].first)
                all_label_ids.insert(instance.label_object_id);
            for (const auto &grp : layer_tools.mixed_sub_layer_groups)
                for (unsigned int comp : grp.components_0based)
                    if (comp == extruder_id) {
                        auto mit = filament_to_print_instances.find(grp.mixed_slot_0based);
                        if (mit != filament_to_print_instances.end())
                            for (const InstanceToPrint &inst : mit->second.first)
                                all_label_ids.insert(inst.label_object_id);
                        break;
                    }
            m_filament_instances_code.clear();
            if (!all_label_ids.empty()) {
                std::vector<size_t> filament_instances_id(all_label_ids.begin(), all_label_ids.end());
                m_filament_instances_code = _encode_label_ids_to_base64(filament_instances_id);
            }
        }

        has_insert_timelapse_gcode |= m_farthest_point_timelapse.inserted_this_layer;

        std::string gcode_toolchange;
        if (has_wipe_tower) {
            if (!m_wipe_tower->is_empty_wipe_tower_gcode(*this, extruder_id, extruder_id == layer_tools.extruders.back())) {
                if (need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode
                    && !(m_farthest_point_timelapse.enabled && m_farthest_point_timelapse.farthest_is_photo_head)) {
                    bool should_insert = true;
                    if (m_config.nozzle_diameter.values.size() == 2){
                        bool curr_is_photo_head = writer().filament() &&
                            get_extruder_id(writer().filament()->id()) == most_used_extruder;
                        bool is_case_b = m_farthest_point_timelapse.enabled && !m_farthest_point_timelapse.farthest_is_photo_head;
                        should_insert = is_case_b ? !curr_is_photo_head : curr_is_photo_head;
                    }

                    if (should_insert) {
                        gcode += this->retract(false, false, auto_lift_type, true);
                        m_writer.add_object_change_labels(gcode);

                        gcode += insert_timelapse_gcode();
                        has_insert_timelapse_gcode = true;
                    }
                }

                if (print.config().enable_wrapping_detection && !has_insert_wrapping_detection_gcode) {
                    gcode += this->retract(false, false, auto_lift_type, true);
                    gcode += insert_wrapping_detection_gcode();
                    has_insert_wrapping_detection_gcode = true;
                }
                gcode_toolchange = m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_tools.extruders.back());
            }
        } else {
            if (need_insert_timelapse_gcode_for_traditional &&
                !has_insert_timelapse_gcode &&
                !(m_farthest_point_timelapse.enabled && m_farthest_point_timelapse.farthest_is_photo_head) &&
                m_writer.need_toolchange(extruder_id) &&
                m_config.nozzle_diameter.values.size() == 2 &&
                writer().filament()) {
                bool curr_is_photo_head = get_extruder_id(writer().filament()->id()) == most_used_extruder;
                bool is_case_b = m_farthest_point_timelapse.enabled && !m_farthest_point_timelapse.farthest_is_photo_head;
                bool should_insert = is_case_b ? !curr_is_photo_head : curr_is_photo_head;
                if (should_insert) {
                    gcode += this->retract(false, false, auto_lift_type, true);
                    m_writer.add_object_change_labels(gcode);

                    gcode += insert_timelapse_gcode();
                    has_insert_timelapse_gcode = true;
                }
            }

            if (print.config().enable_wrapping_detection && !has_insert_wrapping_detection_gcode) {
                gcode += this->retract(false, false, auto_lift_type, true);
                gcode += insert_wrapping_detection_gcode();
                has_insert_wrapping_detection_gcode = true;
            }

            gcode_toolchange = this->set_extruder(extruder_id, print_z);
        }

        if (!gcode_toolchange.empty()) {
            result.spiral_vase_enable = false;
        }
        
        gcode += std::move(gcode_toolchange);

        if (layer_tools.has_wipe_tower && m_wipe_tower)
            m_last_processor_extrusion_role = erWipeTower;

        if (print.has_belt_brim()) {
            const Vec2d saved_origin = m_origin;
            gcode += emit_belt_brim_for_extruder(extruder_id);
            this->set_origin(saved_origin);
        }

        gcode += this->extrude_conformal_patches(print, layers, extruder_id, single_object_instance_idx, true);

        auto &filament_plan = filament_to_print_instances[extruder_id];
        std::vector<InstanceToPrint>     &instances_to_print = filament_plan.first;
        const std::vector<InstanceVisit> &instance_visits    = filament_plan.second;

        std::vector<ObjectByExtruder::Island::Region> by_region_per_copy_cache;
        for (int print_wipe_extrusions = is_anything_overridden; print_wipe_extrusions>=0; --print_wipe_extrusions) {
            if (is_anything_overridden && print_wipe_extrusions == 0)
                gcode+="; PURGING FINISHED\n";

            for (const InstanceVisit &visit : instance_visits) {
                InstanceToPrint &instance_to_print = instances_to_print[visit.instance_idx];
                const auto& inst = instance_to_print.print_object.instances()[instance_to_print.instance_id];
                const LayerToPrint &layer_to_print = layers[instance_to_print.layer_id];
                if (visit.first_visit && print_wipe_extrusions == (is_anything_overridden ? 1 : 0)) {
                    gcode += generate_object_skirt_group(print, instance_to_print.print_object, instance_to_print.instance_id, layer_tools, layer, extruder_id);
                    const PrintObject &vobj = instance_to_print.print_object;
                    if (vobj.has_belt_brim()) {
                        if (extruder_id == (unsigned int)(vobj.belt_brim_filament() - 1) &&
                            belt_brim_emitted.insert({ instance_to_print.layer_id, instance_to_print.instance_id }).second)
                            gcode += generate_object_brim(print, vobj, instance_to_print.instance_id, first_layer,
                                                          layer_to_print.object_layer);
                    } else {
                        gcode += generate_object_brim(print, vobj, instance_to_print.instance_id, first_layer,
                                                      layer_to_print.object_layer);
                    }
                }

                bool object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 &&
                    instance_to_print.print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();
                m_config.apply(print.default_region_config());
                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer = layer_to_print.layer();
                m_object_layer_over_raft = object_layer_over_raft;
                if (m_config.reduce_crossing_wall)
                    m_avoid_crossing_perimeters.init_layer(*m_layer);

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; printing object ") + instance_to_print.print_object.model_object()->name +
                             " id:" + std::to_string(instance_to_print.print_object.get_id()) + " copy " +
                             std::to_string(inst.id) + "\n";
                }
                if (m_enable_exclude_object)
                    m_writer.set_object_start_str(this->object_exclude_start(inst));

                m_extrusion_quality_estimator.set_current_object(&instance_to_print.print_object);

                m_deferral_plan = nullptr;
                if (const Layer *object_layer = layer_to_print.object_layer; object_layer && print_wipe_extrusions == 0)
                    if (auto it = m_nonplanar_deferral.find(&instance_to_print.print_object);
                        it != m_nonplanar_deferral.end() && ! it->second.empty()) {
                        m_deferral_plan     = &it->second;
                        m_deferral_key      = { &instance_to_print.print_object, size_t(instance_to_print.instance_id) };
                        m_deferral_layer_id = object_layer->id();
                    }

                const Point &offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                this->on_set_origin(&instance_to_print.print_object, offset);
                if (visit.first_visit && instance_to_print.object_by_extruder.support != nullptr) {
                    m_layer = layers[instance_to_print.layer_id].support_layer;
                    m_object_layer_over_raft = false;

                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                    if (m_last_obj_copy != this_object_copy)
                        m_avoid_crossing_perimeters.use_external_mp_once();
                    m_last_obj_copy = this_object_copy;
                    this->set_origin(unscale(offset));
                    this->on_set_origin(&instance_to_print.print_object, offset);
                    ExtrusionEntityCollection support_eec;

                    WipingExtrusions& wiping_extrusions = const_cast<LayerTools&>(layer_tools).wiping_extrusions();
                    bool support_overridden = wiping_extrusions.is_support_overridden(layer_to_print.original_object);
                    bool support_intf_overridden = wiping_extrusions.is_support_interface_overridden(layer_to_print.original_object);

                    ExtrusionRole support_extrusion_role = instance_to_print.object_by_extruder.support_extrusion_role;
                    bool is_overridden = support_extrusion_role == erSupportMaterialInterface ? support_intf_overridden : support_overridden;
                    if (is_overridden == (print_wipe_extrusions != 0)) {
                        gcode += this->extrude_support(
                            *instance_to_print.object_by_extruder.support, support_extrusion_role);

                        if (support_extrusion_role == erMixed || support_extrusion_role == erSupportMaterialInterface) {
                            gcode += this->extrude_support(*instance_to_print.object_by_extruder.support, erIroning);
                        }
                    }

                    m_layer = layer_to_print.layer();
                    m_object_layer_over_raft = object_layer_over_raft;
                }
                gcode += this->emit_deferred_before_layer(extruder_id);
                std::vector<ObjectByExtruder::Island> &islands = instance_to_print.object_by_extruder.islands;
                std::vector<size_t> island_order;
                island_order.reserve(visit.islands.size());
                for (size_t idx : visit.islands)
                    if (idx < islands.size())
                        island_order.emplace_back(idx);
                    else
                        BOOST_LOG_TRIVIAL(error) << "island tour refers to island " << idx << " of " << islands.size() << ", skipped";
                if (island_order.empty()) {
                    island_order.reserve(islands.size());
                    if (layer_to_print.object_layer != nullptr && islands.size() == layer_to_print.object_layer->lslices.size() + 1) {
                        for (size_t i = 0; i + 1 < islands.size(); ++i)
                            if (!islands[i].by_region.empty())
                                island_order.emplace_back(i);
                        if (island_order.size() > 1) {
                            Points island_centroids;
                            island_centroids.reserve(island_order.size());
                            for (size_t i : island_order)
                                island_centroids.emplace_back(layer_to_print.object_layer->lslices[i].contour.centroid());
                            const Point start_near = this->last_pos();
                            std::vector<size_t> chain = chain_points(island_centroids, this->last_pos_defined() ? &start_near : nullptr);
                            std::vector<size_t> ordered;
                            ordered.reserve(island_order.size());
                            for (size_t k : chain)
                                ordered.emplace_back(island_order[k]);
                            island_order = std::move(ordered);
                        }
                        if (!islands.back().by_region.empty())
                            island_order.emplace_back(islands.size() - 1);
                    } else {
                        for (size_t i = 0; i < islands.size(); ++i)
                            island_order.emplace_back(i);
                    }
                }
                if (curved_tops_mode(m_config.nonplanar_mode.value)) {
                    auto key = [](const ObjectByExtruder::Island &island) {
                        NonplanarIslandKey out;
                        for (const auto &region : island.by_region) {
                            for (const auto *ee : region.perimeters) collect_nonplanar_island_key(*ee, out);
                            for (const auto *ee : region.infills) collect_nonplanar_island_key(*ee, out);
                        }
                        return out;
                    };
                    std::stable_sort(island_order.begin(), island_order.end(), [&](size_t a, size_t b) {
                        return nonplanar_island_before(key(islands[a]), key(islands[b]));
                    });
                }
                for (size_t island_idx : island_order) {
                    ObjectByExtruder::Island &island = islands[island_idx];
                    const auto& by_region_specific = is_anything_overridden ? island.by_region_per_copy(by_region_per_copy_cache, static_cast<unsigned int>(instance_to_print.instance_id), extruder_id, print_wipe_extrusions != 0) : island.by_region;
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                    if (m_last_obj_copy != this_object_copy)
                        m_avoid_crossing_perimeters.use_external_mp_once();
                    m_last_obj_copy = this_object_copy;
                    this->set_origin(unscale(offset));
                    this->on_set_origin(&instance_to_print.print_object, offset);

                    auto has_infill = [](const std::vector<ObjectByExtruder::Island::Region> &by_region) {
                        for (auto region : by_region) {
                            if (!region.infills.empty())
                                return true;
                        }
                        return false;
                    };
                    {
                        if (curved_tops_mode(m_config.nonplanar_mode.value))
                            gcode += this->extrude_curved_tops(print, by_region_specific);
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, false);
                        if (!has_wipe_tower && need_insert_timelapse_gcode_for_traditional && printer_structure == PrinterStructure::psI3
                            && !has_insert_timelapse_gcode && has_infill(by_region_specific)) {
                            gcode += this->retract(false, false, auto_lift_type, true);

                            gcode += insert_timelapse_gcode();
                            has_insert_timelapse_gcode = true;
                        }
                        gcode += this->extrude_infill(print, by_region_specific, false);
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, false, true);
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, true);
                    }
                    gcode += this->extrude_infill(print,by_region_specific, true);
                }
                gcode += this->emit_deferred_after_layer(instance_to_print.print_object, extruder_id);

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; stop printing object ") +
                             instance_to_print.print_object.model_object()->name +
                             " id:" + std::to_string(instance_to_print.print_object.get_id()) + " copy " +
                             std::to_string(inst.id) + "\n";
                }
                if (!m_writer.is_object_start_str_empty()) {
                    m_writer.set_object_start_str("");
                } else if (m_enable_exclude_object) {
                    m_writer.set_object_end_str(this->object_exclude_end(inst));
                }
            }
        }

        gcode += this->extrude_conformal_patches(print, layers, extruder_id, single_object_instance_idx);
        gcode += this->extrude_raised_walls(print, layers, extruder_id, single_object_instance_idx);

        for (const auto &grp : layer_tools.mixed_sub_layer_groups) {
            int sub_idx = -1;
            for (size_t k = 0; k < grp.components_0based.size(); ++k) {
                if (grp.components_0based[k] == extruder_id) {
                    sub_idx = static_cast<int>(k);
                    break;
                }
            }
            if (sub_idx < 0)
                continue;

            auto mixed_instances_it = filament_to_print_instances.find(grp.mixed_slot_0based);
            if (mixed_instances_it == filament_to_print_instances.end() || mixed_instances_it->second.first.empty())
                continue;

            double lh = grp.layer_height > 0. ? grp.layer_height : static_cast<double>(height);
            double cumulative_h = 0.0;
            for (int i = 0; i < sub_idx; ++i)
                cumulative_h += grp.sub_heights[i];
            double default_sub_h = grp.sub_heights[sub_idx];
            double default_sub_z = print_z - lh + cumulative_h + default_sub_h;

            m_sub_layer_flow_ratio = default_sub_h / lh;
            m_sub_layer_height     = default_sub_h;
            m_nominal_z            = default_sub_z;

            gcode += this->set_extruder(extruder_id, default_sub_z);

            for (InstanceToPrint &instance_to_print : mixed_instances_it->second.first) {
                const bool use_per_volume = grp.is_gradient
                    && !grp.per_volume_gradient.empty()
                    && std::any_of(grp.per_volume_gradient.begin(), grp.per_volume_gradient.end(),
                                   [&](const auto &kv) { return kv.first.obj == &instance_to_print.print_object; });

                const LayerToPrint &layer_to_print = layers[instance_to_print.layer_id];
                const auto &inst = instance_to_print.print_object.instances()[instance_to_print.instance_id];

                bool object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 &&
                    instance_to_print.print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();
                m_config.apply(print.default_region_config());
                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer = layer_to_print.layer();
                m_object_layer_over_raft = object_layer_over_raft;
                if (m_config.reduce_crossing_wall)
                    m_avoid_crossing_perimeters.init_layer(*m_layer);

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; printing object ") + instance_to_print.print_object.model_object()->name +
                             " id:" + std::to_string(instance_to_print.print_object.get_id()) + " copy " +
                             std::to_string(inst.id) + "\n";
                }
                if (m_enable_exclude_object)
                    m_writer.set_object_start_str(this->object_exclude_start(inst));

                m_extrusion_quality_estimator.set_current_object(&instance_to_print.print_object);

                const Point &offset = inst.shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                this->on_set_origin(&instance_to_print.print_object, offset);

                struct SubLayerEmitEntry {
                    double sub_h;
                    double sub_z;
                    std::function<bool(size_t region_idx)> region_filter;
                    bool skip = false;
                };
                std::vector<SubLayerEmitEntry> emit_plan;

                auto compute_sub_zh = [&](double r1, double r2, double &out_sub_h, double &out_sub_z) {
                    std::vector<double> sub_heights_local(grp.components_0based.size());
                    for (size_t ci = 0; ci < grp.components_0based.size(); ++ci)
                        sub_heights_local[ci] = (static_cast<int>(ci) == grp.gradient_first_sorted_idx) ? r1 * lh : r2 * lh;
                    double cum = 0.0;
                    for (int ci = 0; ci < sub_idx; ++ci)
                        cum += sub_heights_local[ci];
                    out_sub_h = sub_heights_local[sub_idx];
                    out_sub_z = print_z - lh + cum + out_sub_h;
                };

                auto gradient_ratios = [](const auto &g) -> std::pair<double, double> {
                    double t  = (g.total_layers > 0) ? (2.0 * g.current_idx + 1.0) / (2.0 * g.total_layers) : 0.5;
                    double r1 = g.curve.empty()
                                ? (g.gradient_start + (g.gradient_end - g.gradient_start) * t)
                                : sample_gradient_curve(g.curve, t);
                    return {r1, 1.0 - r1};
                };

                auto region_uses_slot = [](const PrintRegionConfig &rcfg, unsigned int slot_1b) {
                    return (unsigned int)rcfg.outer_wall_filament_id.value     == slot_1b
                        || (unsigned int)rcfg.inner_wall_filament_id.value     == slot_1b
                        || (unsigned int)rcfg.sparse_infill_filament_id.value  == slot_1b
                        || (unsigned int)rcfg.internal_solid_filament_id.value == slot_1b
                        || (unsigned int)rcfg.top_surface_filament_id.value    == slot_1b
                        || (unsigned int)rcfg.bottom_surface_filament_id.value == slot_1b;
                };

                double obj_sub_z = default_sub_z;

                if (use_per_volume) {
                    const PrintObject *po = &instance_to_print.print_object;
                    const unsigned int slot_1b = grp.mixed_slot_0based + 1;

                    std::set<ObjectID> tagged_volumes_present;
                    bool has_untagged_for_slot = false;
                    for (ObjectByExtruder::Island &island : instance_to_print.object_by_extruder.islands) {
                        for (size_t r = 0; r < island.by_region.size(); ++r) {
                            const auto &region = island.by_region[r];
                            if (region.perimeters.empty() && region.infills.empty())
                                continue;
                            const PrintRegion &pr = print.get_print_region(r);
                            if (!region_uses_slot(pr.config(), slot_1b))
                                continue;
                            ObjectID vid = pr.gradient_volume_id();
                            if (vid.valid())
                                tagged_volumes_present.insert(vid);
                            else
                                has_untagged_for_slot = true;
                        }
                    }

                    for (const ObjectID &target_vid : tagged_volumes_present) {
                        auto vg_it = grp.per_volume_gradient.find({po, target_vid});
                        if (vg_it == grp.per_volume_gradient.end())
                            continue;
                        const auto &vg = vg_it->second;
                        auto [r1, r2] = gradient_ratios(vg);

                        bool vol_no_split = false;
                        bool skip_entry   = false;
                        const size_t n = grp.components_0based.size();
                        if (n == 2 && vg.current_idx + 1 == vg.total_layers) {
                            const size_t dom_idx = (r1 >= r2) ? 0 : 1;
                            const unsigned int first_sorted_comp = grp.components_0based[grp.gradient_first_sorted_idx];
                            const unsigned int other_comp = grp.components_0based[1 - grp.gradient_first_sorted_idx];
                            const unsigned int dom_0b = (dom_idx == 0) ? first_sorted_comp : other_comp;
                            const unsigned int oth_0b = (dom_idx == 0) ? other_comp : first_sorted_comp;
                            if (dom_0b < oth_0b) {
                                vol_no_split = true;
                                if (extruder_id != dom_0b)
                                    skip_entry = true;
                            }
                        }

                        double vol_sub_h = default_sub_h;
                        double vol_sub_z = default_sub_z;
                        if (vol_no_split) {
                            vol_sub_h = lh;
                            vol_sub_z = print_z;
                        } else {
                            compute_sub_zh(r1, r2, vol_sub_h, vol_sub_z);
                        }

                        emit_plan.push_back({vol_sub_h, vol_sub_z,
                            [target_vid, &print](size_t r) {
                                return print.get_print_region(r).gradient_volume_id() == target_vid;
                            },
                            skip_entry});
                    }

                    if (has_untagged_for_slot) {
                        double obj_sub_h = default_sub_h;
                        auto og_it = grp.per_object_gradient.find(po);
                        if (og_it != grp.per_object_gradient.end()) {
                            auto [r1, r2] = gradient_ratios(og_it->second);
                            compute_sub_zh(r1, r2, obj_sub_h, obj_sub_z);
                        }
                        emit_plan.push_back({obj_sub_h, obj_sub_z,
                            [&print](size_t r) {
                                return !print.get_print_region(r).gradient_volume_id().valid();
                            },
                            false});
                    }
                } else {
                    double legacy_sub_h = default_sub_h;
                    obj_sub_z = default_sub_z;
                    if (grp.is_gradient) {
                        auto og_it = grp.per_object_gradient.find(&instance_to_print.print_object);
                        if (og_it != grp.per_object_gradient.end()) {
                            auto [r1, r2] = gradient_ratios(og_it->second);
                            compute_sub_zh(r1, r2, legacy_sub_h, obj_sub_z);
                        }
                    }
                    emit_plan.push_back({legacy_sub_h, obj_sub_z, nullptr, false});
                }

                auto plan_has_infill = [](const std::vector<ObjectByExtruder::Island::Region> &by_region) {
                    for (const auto &r : by_region)
                        if (!r.infills.empty())
                            return true;
                    return false;
                };

                for (auto &entry : emit_plan) {
                    if (entry.skip)
                        continue;
                    m_sub_layer_flow_ratio = entry.sub_h / lh;
                    m_sub_layer_height     = entry.sub_h;
                    m_nominal_z            = entry.sub_z;
                    m_need_change_layer_lift_z = true;

                    for (ObjectByExtruder::Island &island : instance_to_print.object_by_extruder.islands) {
                        const auto &src = island.by_region;
                        std::vector<ObjectByExtruder::Island::Region> subset_storage;
                        if (entry.region_filter) {
                            subset_storage.resize(src.size());
                            for (size_t r = 0; r < src.size(); ++r)
                                if (entry.region_filter(r))
                                    subset_storage[r] = src[r];
                        }
                        const auto &by_region_specific = entry.region_filter ? subset_storage : src;

                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, false);
                        if (!has_wipe_tower && need_insert_timelapse_gcode_for_traditional
                            && printer_structure == PrinterStructure::psI3
                            && !has_insert_timelapse_gcode && plan_has_infill(by_region_specific)) {
                            gcode += this->retract(false, false, auto_lift_type, true);
                            gcode += insert_timelapse_gcode();
                            has_insert_timelapse_gcode = true;
                        }
                        gcode += this->extrude_infill(print, by_region_specific, false);
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, false, true);
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, true);
                        gcode += this->extrude_infill(print, by_region_specific, true);
                    }
                }

                if (instance_to_print.object_by_extruder.support && !instance_to_print.object_by_extruder.support->empty()) {
                    if (use_per_volume) {
                        m_nominal_z = obj_sub_z;
                        m_need_change_layer_lift_z = true;
                    }
                    ExtrusionRole support_role = instance_to_print.object_by_extruder.support_extrusion_role;
                    gcode += this->extrude_support(*instance_to_print.object_by_extruder.support, support_role);
                    if (support_role == erMixed || support_role == erSupportMaterialInterface)
                        gcode += this->extrude_support(*instance_to_print.object_by_extruder.support, erIroning);
                }

                if (!m_writer.is_object_start_str_empty()) {
                    m_writer.set_object_start_str("");
                } else if (m_enable_exclude_object) {
                    m_writer.set_object_end_str(this->object_exclude_end(inst));
                }
            }

            m_sub_layer_flow_ratio = 0.0;
            m_sub_layer_height     = 0.0;
        }
        if (!layer_tools.mixed_sub_layer_groups.empty()) {
            m_writer.add_object_end_labels(gcode);
            m_nominal_z = print_z;
            m_need_change_layer_lift_z = true;
        }

    }

    if (print.has_belt_brim()) {
        const Vec2d saved_origin = m_origin;
        for (const LayerToPrint &ltp : layers) {
            const PrintObject *obj = ltp.original_object;
            if (obj == nullptr || ! obj->has_belt_brim() || ltp.object_layer == nullptr)
                continue;
            const size_t       ltp_idx = size_t(&ltp - layers.data());
            const unsigned int brim0   = (unsigned int)(obj->belt_brim_filament() - 1);
            const size_t       i_begin = single_object_instance_idx == size_t(-1) ? 0 : single_object_instance_idx;
            const size_t       i_end   = single_object_instance_idx == size_t(-1) ? obj->instances().size()
                                                                                 : single_object_instance_idx + 1;
            for (size_t instance_id = i_begin; instance_id < i_end && instance_id < obj->instances().size(); ++ instance_id) {
                if (! belt_brim_emitted.insert({ ltp_idx, instance_id }).second)
                    continue;
                if (m_writer.filament() == nullptr || m_writer.filament()->id() != brim0)
                    gcode += this->set_extruder(brim0, print_z);
                gcode += generate_object_brim(print, *obj, instance_id, first_layer, ltp.object_layer);
            }
        }
        this->set_origin(saved_origin);
    }

    if (first_layer) {
        for (auto iter = by_extruder.begin(); iter != by_extruder.end(); ++iter) {
            if (!iter->second.empty())
                m_initial_layer_extruders.insert(iter->first);
        }
    }

#if 0
    if (m_spiral_vase)
        gcode = m_spiral_vase->process_layer(std::move(gcode));

    if (m_cooling_buffer)
        gcode = m_cooling_buffer->process_layer(std::move(gcode), layer.id(),
            object_layer || last_layer);

    file.write(gcode);
#endif

    BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z <<
    log_memory_info();

    has_insert_timelapse_gcode |= m_farthest_point_timelapse.inserted_this_layer;

    if (m_farthest_point_timelapse.enabled && m_farthest_point_timelapse.farthest_is_photo_head && !has_insert_timelapse_gcode) {
        if (FILAMENT_CONFIG(retract_when_changing_layer)) {
            gcode += this->retract(false, false, auto_lift_type, true);
        }
        m_writer.add_object_change_labels(gcode);
        gcode += insert_timelapse_gcode();
        has_insert_timelapse_gcode = true;
    }

    if (need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode) {
        if (m_support_traditional_timelapse
            && printer_structure == PrinterStructure::psI3
            && m_config.timelapse_type.value == TimelapseType::tlTraditional)
            m_support_traditional_timelapse = false;

        if (m_config.nozzle_diameter.values.size() == 2
            && m_support_traditional_timelapse
            && m_config.timelapse_type.value == TimelapseType::tlTraditional
            && (writer().filament() && get_extruder_id(writer().filament()->id()) != most_used_extruder)) {
            m_support_traditional_timelapse = false;
        }
        if (FILAMENT_CONFIG(retract_when_changing_layer)) {
            gcode += this->retract(false, false, auto_lift_type, true);
        }
        m_writer.add_object_change_labels(gcode);

        gcode += insert_timelapse_gcode();
    }

    result.gcode = std::move(gcode);
    result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
    return result;
}

void GCode::apply_print_config(const PrintConfig &print_config)
{
    m_writer.apply_print_config(print_config);
    m_config.apply(print_config);
    m_scaled_resolution = scaled<double>(print_config.resolution.value);
    m_enable_exclude_object = m_config.exclude_object;

#if ORCA_CHECK_GCODE_PLACEHOLDERS
    for (auto opt : std::initializer_list<ConfigOptionString*>{
             &m_config.machine_start_gcode,
             &m_config.machine_end_gcode,
             &m_config.before_layer_change_gcode,
             &m_config.layer_change_gcode,
             &m_config.time_lapse_gcode,
             &m_config.change_filament_gcode,
             &m_config.change_extrusion_role_gcode,
             &m_config.process_change_extrusion_role_gcode,
             &m_config.printing_by_object_gcode,
             &m_config.machine_pause_gcode,
             &m_config.template_custom_gcode,
         }) {
        if (opt->empty())
            opt->set(new ConfigOptionString(";VALUE FOR TESTING"));
    }
    for (auto opt : std::initializer_list<ConfigOptionStrings*>{
             &m_config.filament_start_gcode,
             &m_config.filament_end_gcode,
             &m_config.filament_change_extrusion_role_gcode
         }) {
        if (opt->empty())
            for (int i = 0; i < opt->size(); ++i)
                opt->set_at(new ConfigOptionString(";VALUE FOR TESTING"), i, 0);
    }
#endif
}

void GCode::append_full_config(const Print &print, std::string &str)
{
    DynamicPrintConfig cfg = print.full_print_config();
    {
        std::vector<double> temp_cfg_flush_multiplier = (print.config().prime_volume_mode == PrimeVolumeMode::pvmFast)
                                                            ? cfg.option<ConfigOptionFloats>("flush_multiplier_fast")->values
                                                            : cfg.option<ConfigOptionFloats>("flush_multiplier")->values;
        std::vector<double> temp_flush_volumes_matrix = cfg.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
        auto                temp_filament_color       = cfg.option<ConfigOptionStrings>("filament_colour")->values;
        size_t              heads_count_tmp           = temp_cfg_flush_multiplier.size(),
               matrix_value_count                     = temp_flush_volumes_matrix.size() / temp_cfg_flush_multiplier.size(),
               filament_count_tmp                     = temp_filament_color.size();
        if (filament_count_tmp * filament_count_tmp * heads_count_tmp == temp_flush_volumes_matrix.size()) {
            for (size_t idx = 0; idx < heads_count_tmp; ++idx) {
                double temp_cfg_flush_multiplier_idx = temp_cfg_flush_multiplier[idx];
                size_t temp_begin_t = idx * matrix_value_count, temp_end_t = (idx + 1) * matrix_value_count;
                std::transform(temp_flush_volumes_matrix.begin() + temp_begin_t, temp_flush_volumes_matrix.begin() + temp_end_t,
                               temp_flush_volumes_matrix.begin() + temp_begin_t,
                               [temp_cfg_flush_multiplier_idx](double inputx) { return std::round(inputx * temp_cfg_flush_multiplier_idx); });
            }
            cfg.option<ConfigOptionFloats>("flush_volumes_matrix")->values = temp_flush_volumes_matrix;
        } else if (filament_count_tmp == 1) {
        }
        else {
            throw Slic3r::SlicingError(_(L("Flush volumes matrix do not match to the correct size!")));
        }
    }
    cfg.option<ConfigOptionInts>("filament_map_2", true)->values = print.config().filament_map_2.values;
    if (!print.objects().empty() &&
        std::all_of(print.objects().begin(), print.objects().end(), [&print](const PrintObject *object) {
            return object->config().nonplanar_mode.value == print.objects().front()->config().nonplanar_mode.value;
        }))
        cfg.option<ConfigOptionEnum<NonplanarMode>>("nonplanar_mode", true)->value = print.objects().front()->config().nonplanar_mode.value;
    static const std::set<std::string_view> banned_keys( {
        "filament_extruder_compatibility"sv,
        "nonplanar_wave"sv, "nonplanar_wave_amplitude"sv, "nonplanar_wave_wavelength"sv,
        "nonplanar_wave_pattern"sv, "nonplanar_wave_direction"sv,
        "nonplanar_head_snapshot"sv,
        "nonplanar_region_source"sv,
        "prime_volume_mode"sv,
        "flush_multiplier_fast"sv,
        "filament_flush_temp_fast"sv,
        "support_fast_purge_mode"sv,
        "deretract_speed_extruder_change"sv,
        "farthest_point_timelapse"sv,
        "is_imex"sv,
        "imex_firmware_managed_zones"sv,
        "imex_gantry_count"sv,
        "imex_tools_per_gantry"sv,
        "imex_tool_layout"sv,
        "imex_nozzle_clearance_x"sv,
        "imex_nozzle_clearance_y"sv,
        "imex_carriage_margin"sv,
        "imex_viz_theme"sv,
        "imex_mode_names"sv,
        "imex_mode_active_tools"sv,
        "imex_mode_gcodes"sv,
        "imex_parallel_mode"sv,
        "imex_head_filament_map"sv,
        "compatible_printers"sv,
        "compatible_prints"sv,
        "filament_colour_type"sv,
        "print_host"sv,
        "print_host_webui"sv,
        "printhost_apikey"sv,
        "printhost_cafile"sv,
        "printhost_user"sv,
        "printhost_password"sv,
        "printhost_port"sv
    });
    const bool curved_tops = std::any_of(print.objects().begin(), print.objects().end(), [](const PrintObject *object) {
        return curved_tops_mode(object->config().nonplanar_mode.value);
    });
    auto is_banned = [&cfg, curved_tops](const std::string &key) {
        if (key.compare(0, 17, "nonplanar_curved_") == 0 && !curved_tops) return true;
        if (key == "nonplanar_adaptive_bands")
            if (const auto *opt = cfg.option<ConfigOptionBool>(key); opt != nullptr && !opt->value)
                return true;
        if (key == "nonplanar_field_file")
            if (const auto *opt = cfg.option<ConfigOptionString>(key); opt != nullptr && opt->value.empty())
                return true;
        if (key == "native_support_base_pad" || key == "native_support_base_pad_min_ratio")
            if (const auto *opt = cfg.option<ConfigOptionEnum<SupportMaterialStyle>>("support_style"); opt == nullptr || opt->value != smsNative)
                return true;
        return banned_keys.find(key) != banned_keys.end();
    };
    std::ostringstream ss;
    for (const std::string& key : cfg.keys()) {
        if (!is_banned(key) && !cfg.option(key)->is_nil()) {
            if (key == "wipe_tower_x" || key == "wipe_tower_y") {
                ss << std::fixed << std::setprecision(3) << "; " << key << " = " << dynamic_cast<const ConfigOptionFloats*>(cfg.option(key))->get_at(print.get_plate_index()) << "\n";
            }
            if(key == "extruder_colour")
                ss << "; " << key << " = " << cfg.opt_serialize("filament_colour") << "\n";
            else
                ss << "; " << key << " = " << cfg.opt_serialize(key) << "\n";
        }
    }
    str += ss.str();
}

void GCode::set_extruders(const std::vector<unsigned int> &extruder_ids)
{
    m_writer.set_extruders(extruder_ids);

    m_wipe.enable = false;
    for (auto id : extruder_ids)
        if (m_config.wipe.get_at(id)) {
            m_wipe.enable = true;
            break;
        }
}

void GCode::set_origin(const Vec2d &pointf)
{
    const Point3 translate(
        scale_(m_origin(0) - pointf(0)),
        scale_(m_origin(1) - pointf(1))
    );
    m_last_pos += translate;
    m_wipe.path.translate(translate.to_point());
    m_origin = pointf;
}

std::string GCode::preamble()
{
    std::string gcode = m_writer.preamble();

    m_writer.travel_to_z(m_config.z_offset.value);

    return gcode;
}

std::string GCode::change_layer(coordf_t print_z)
{
    std::string gcode;
    m_nonplanar_printed_this_layer = false;
    m_last_native_curved = false;
    if (m_layer_count > 0)
        gcode += m_writer.update_progress(++ m_layer_index, m_layer_count);
    coordf_t z = print_z + m_config.z_offset.value;
    if (FILAMENT_CONFIG(retract_when_changing_layer) && m_writer.will_move_z(z)) {
        LiftType lift_type = this->to_lift_type(ZHopType(FILAMENT_CONFIG(z_hop_types)));
        gcode += this->retract(false, false, ZHopType(FILAMENT_CONFIG(z_hop_types)) == ZHopType::zhtAuto ? LiftType::SpiralLift : lift_type);
    }

    m_writer.add_object_change_labels(gcode);

    if (m_spiral_vase) {
        std::ostringstream comment;
        comment << "move to next layer (" << m_layer_index << ")";
        gcode += m_writer.travel_to_z(z, comment.str());
    }

    m_need_change_layer_lift_z = true;

    if (m_writer.tool_kinematics().rotary_count() > 0 && (!m_config.nonplanar_atom_toolpath.value.empty() || m_ordered_plan_layer)) {
        m_nominal_z = z;
        m_conformal_lift = false;
        return gcode;
    }

    double tip_z = z;
    if (this->below_conformal_material()) {
        m_wipe.reset_path();
        tip_z = std::max(z, m_nonplanar_ceiling);
        gcode += m_writer.travel_to_z(std::max(tip_z, m_writer.get_position().z()), "lift over conformal material", true,
                                      this->conformal_max_speed_z());
        tip_z = m_writer.get_position().z();
    }

    m_nominal_z = z;
    if (m_writer.tool_kinematics().rotary_count() == 0)
        m_writer.get_position().z() = tip_z;
    m_conformal_lift = false;

    return gcode;
}



static std::unique_ptr<EdgeGrid::Grid> calculate_layer_edge_grid(const Layer& layer)
{
    auto out = make_unique<EdgeGrid::Grid>();

    const coord_t distance_field_resolution = coord_t(scale_(1.) + 0.5);
    out->create(layer.lslices, distance_field_resolution);
    out->calculate_sdf();
#if 0
        {
            static int iRun = 0;
            BoundingBox bbox = (*lower_layer_edge_grid)->bbox();
            bbox.min(0) -= scale_(5.f);
            bbox.min(1) -= scale_(5.f);
            bbox.max(0) += scale_(5.f);
            bbox.max(1) += scale_(5.f);
            EdgeGrid::save_png(*(*lower_layer_edge_grid), bbox, scale_(0.1f), debug_out_path("GCode_extrude_loop_edge_grid-%d.png", iRun++));
        }
#endif
    return out;
}

std::string GCode::extrude_loop(const ExtrusionLoop&                       loop_ref,
                                const std::string&                         description,
                                double                                     speed,
                                const std::vector<const ExtrusionEntity*>& region_perimeters,
                                const Point*                               start_point,
                                const WipeInwardSupport*                   wipe_support)
{
    ExtrusionLoop loop = loop_ref;

    bool is_hole = (loop.loop_role() & elrHole) == elrHole;

    if (m_config.spiral_mode && !is_hole) {
        if (m_config.wall_direction == WallDirection::CounterClockwise)
            loop.make_counter_clockwise();
        else
            loop.make_clockwise();
    }
    Point last_pos = start_point ? *start_point : this->last_pos();
    if (Point lid; m_config.spiral_mode && !is_hole && m_layer != nullptr && Nonplanar::lid_start(*m_layer->object(), lid))
        last_pos = lid;
    float seam_overhang = std::numeric_limits<float>::lowest();
    const bool conformal_loop = has_nonplanar_source(loop);
    if (m_loop_seam_placed) {
    } else if (!m_config.spiral_mode && description == "perimeter") {
        assert(m_layer != nullptr);
        m_seam_placer.place_seam(m_layer, loop, last_pos, seam_overhang);
    } else
        loop.split_at(last_pos, false);

    const auto seam_scarf_type = m_config.seam_slope_type.value;
    bool enable_seam_slope = ! conformal_loop && ! m_loop_seam_placed &&
        ((seam_scarf_type == SeamScarfType::External && !is_hole) || seam_scarf_type == SeamScarfType::All) &&
        !m_config.spiral_mode && !m_config.belt_printer.value &&
        !(m_layer != nullptr && Nonplanar::wave_enabled(*m_layer->object())) &&
        (loop.role() == erExternalPerimeter || (loop.role() == erPerimeter && m_config.seam_slope_inner_walls)) &&
        layer_id() > 0;
    const auto nozzle_diameter = EXTRUDER_CONFIG(nozzle_diameter);
    if (enable_seam_slope && m_config.seam_slope_conditional.value) {
        enable_seam_slope = loop.is_smooth(m_config.scarf_angle_threshold.value * M_PI / 180., nozzle_diameter);
    }

    if (enable_seam_slope && m_config.seam_slope_conditional.value && m_config.scarf_overhang_threshold.value > 0.0f) {
        const auto _line_width = loop.role() == erExternalPerimeter ? m_config.outer_wall_line_width.get_abs_value(nozzle_diameter) :
                                                                      m_config.inner_wall_line_width.get_abs_value(nozzle_diameter);
        enable_seam_slope      = seam_overhang < m_config.scarf_overhang_threshold.value * 0.01f * _line_width;
    }

    const double seam_gap = scale_(m_config.seam_gap.get_abs_value(nozzle_diameter));
    const bool seam_gap_applied = enable_seam_slope || m_enable_loop_clipping;
    const double seam_gap_distance_mm = seam_gap_applied ? unscale_(seam_gap) : 0.0;
    double seam_scarf_distance_mm = 0.0;
    const double clip_length = m_enable_loop_clipping && !enable_seam_slope ? seam_gap : 0;

    ExtrusionPaths paths;
    loop.clip_end(clip_length, &paths);
    if (paths.empty()) return "";

    double small_peri_speed = -1;
    if (speed == -1 && loop.length() <= SMALL_PERIMETER_LENGTH(NOZZLE_CONFIG(small_perimeter_threshold))) {
        if(NOZZLE_CONFIG(small_perimeter_speed).value == 0)
            small_peri_speed = NOZZLE_CONFIG(outer_wall_speed) * 0.5;
        else
            small_peri_speed = NOZZLE_CONFIG(small_perimeter_speed).get_abs_value(NOZZLE_CONFIG(outer_wall_speed));
    }

    std::string gcode;
    
    if (! conformal_loop && m_config.wipe_before_external_loop.value && !paths.empty() && paths.front().size() > 1 && paths.back().size() > 1 && paths.front().role() == erExternalPerimeter && region_perimeters.size() > 1) {
        const bool is_full_loop_ccw = loop.polygon().is_counter_clockwise();
        bool is_hole_loop = (loop.loop_role() & ExtrusionLoopRole::elrHole) != 0;
        const double nozzle_diam = nozzle_diameter;

        Point previous_point = Point(paths.front().polyline.points[1].x(), paths.front().polyline.points[1].y());
        Point current_point = Point(paths.front().polyline.points.front().x(), paths.front().polyline.points.front().y());
        Point next_point = Point(paths.back().polyline.points.back().x(), paths.back().polyline.points.back().y());

        if (next_point == current_point) {
            const Point3 &p3 = paths.back().polyline.points[paths.back().polyline.points.size() - 2];
            next_point = Point(p3.x(), p3.y());
        }

        Point a = next_point;
        Point b = previous_point;
        if ((is_hole_loop ? !is_full_loop_ccw : is_full_loop_ccw)) {
            std::swap(a, b);
        }

        double angle = current_point.ccw_angle(a, b) / 3;

        if (is_hole_loop ? !is_full_loop_ccw : is_full_loop_ccw) angle *= -1;

        Vec2d current_pos = current_point.cast<double>();
        Vec2d next_pos = next_point.cast<double>();
        Vec2d vec_dist = next_pos - current_pos;
        double vec_norm = vec_dist.norm();
        coordf_t dist = std::min(scaled(nozzle_diam) * 0.5, scaled(paths.front().width) * 0.5);

        Point pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
        pt.rotate(angle, current_point);
        pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
        pt.rotate(angle, current_point);
        
        int discoveredTouchingLines = 0;
        for (const ExtrusionEntity* ee : region_perimeters){
            auto potential_touching_line = ee->as_polyline();
            AABBTreeLines::LinesDistancer<Line> potential_touching_line_distancer{potential_touching_line.lines()};
            auto touching_line = potential_touching_line_distancer.all_lines_in_radius(pt, scale_(nozzle_diam));
            if(touching_line.size()){
                discoveredTouchingLines ++;
                if(discoveredTouchingLines > 1) break;
            }
        }
        if(discoveredTouchingLines > 1){
            ExtrusionPath fake_path_wipe(Polyline3(Points3{Point3(pt), Point3(current_point)}), paths.front());
            fake_path_wipe.set_force_no_extrusion(true);
            fake_path_wipe.mm3_per_mm = 0;
            gcode += extrude_path(fake_path_wipe, "move inwards before retraction/seam", speed);
        }
    }


    const auto speed_for_path = [&speed, &small_peri_speed](const ExtrusionPath& path) {
        const bool is_small_small_perimeter = small_peri_speed > 0 && !is_bridge(path.role()) && is_perimeter(path.role());
        return is_small_small_perimeter ? small_peri_speed : speed;
    };

    
    m_multi_flow_segment_path_pa_set = false;
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    double weighted_sum_mm3_per_mm = 0.0;
    double total_multipath_length = 0.0;
    for (const ExtrusionPath& path : paths) {
        if(!path.is_force_no_extrusion()){
            double path_length = unscale<double>(path.length());
            weighted_sum_mm3_per_mm += path.mm3_per_mm * path_length;
            total_multipath_length += path_length;
        }
    }
    if (total_multipath_length > 0.0)
        m_multi_flow_segment_path_average_mm3_per_mm = weighted_sum_mm3_per_mm / total_multipath_length;
    
    if (!enable_seam_slope) {
        for (const ExtrusionPath& path : paths) {
            gcode += this->_extrude(path, description, speed_for_path(path));
            m_multi_flow_segment_path_pa_set = true;
        }
    } else {
        double start_slope_ratio;
        if (m_config.seam_slope_start_height.percent) {
            start_slope_ratio = m_config.seam_slope_start_height.value / 100.;
        } else {
            double h = paths.front().height;
            start_slope_ratio = m_config.seam_slope_start_height.value / h;
        }
        if (start_slope_ratio >= 1)
            start_slope_ratio = 0.99;

        double loop_length = 0.;
        for (const auto & path : paths) {
            loop_length += unscale_(path.length());
        }

        const bool   slope_entire_loop        = m_config.seam_slope_entire_loop;
        const double slope_min_length         = slope_entire_loop ? loop_length : std::min(m_config.seam_slope_min_length.value, loop_length);
        const int    slope_steps              = m_config.seam_slope_steps;
        const double slope_max_segment_length = scale_(slope_min_length / slope_steps);
        seam_scarf_distance_mm = slope_min_length;

        ExtrusionLoopSloped new_loop(paths, seam_gap, slope_min_length, slope_max_segment_length, start_slope_ratio, loop.loop_role());
        new_loop.clip_slope(seam_gap);

        for (const ExtrusionPath* path : new_loop.get_all_paths()) {
            gcode += this->_extrude(*path, description, speed_for_path(*path));
            m_multi_flow_segment_path_pa_set = true;
        }

        if (!new_loop.ends.empty()) {
            paths.clear();
            paths.reserve(new_loop.paths.size() + new_loop.ends.size());
            paths.insert(paths.end(), new_loop.paths.begin(), new_loop.paths.end());
            paths.insert(paths.end(), new_loop.ends.begin(), new_loop.ends.end());
        }
    }

    if (description == "perimeter") {
        m_processor.result().print_statistics.total_seam_gap_distance += static_cast<float>(seam_gap_distance_mm);
        m_processor.result().print_statistics.total_seam_scarf_distance += static_cast<float>(seam_scarf_distance_mm);
    }

    if (m_layer != nullptr && Nonplanar::wave_enabled(*m_layer->object())) {
        m_wipe.reset_path();
        return gcode;
    }
    const bool is_ccw = loop.is_counter_clockwise();

    std::optional<Point> wipe_on_loops_dest;
    if (!conformal_loop && m_config.wipe_on_loops.value && paths.back().role() == erExternalPerimeter &&
        m_layer != nullptr && m_config.wall_loops.value > 1 && paths.front().size() >= 2 &&
        paths.back().polyline.points.size() >= 2)
        wipe_on_loops_dest = wipe_on_loops_destination(paths, scale_(nozzle_diameter), is_ccw, is_hole);

    bool wipe_inward_applied = false;
    if (conformal_loop || m_emitting_deferred) {
        m_wipe.reset_path();
    } else if (m_wipe.enable && FILAMENT_CONFIG(wipe)) {
        m_wipe.update_path(paths);

        if (m_config.wipe_inward && m_config.wipe_inward_distance.value > 0. &&
            wipe_support != nullptr && !wipe_support->inner_lines.empty() &&
            std::any_of(paths.begin(), paths.end(),
                [](const ExtrusionPath &path) { return is_external_perimeter(path.role()); }) &&
            m_wipe.path.points.size() >= 2) {
            const double outer_wall_line_width = paths.front().width;
            const double requested_offset = m_config.wipe_inward_distance.get_abs_value(outer_wall_line_width);
            const double offset_dist = scale_(std::min(requested_offset, outer_wall_line_width));
            if (offset_dist > SCALED_EPSILON) {
                const Point seam_start = paths.front().first_point();
                const Point seam_end   = paths.back().last_point();
                const Point wipe_start = wipe_on_loops_dest.value_or(seam_end);
                const double max_wipe_length = scale_(FILAMENT_CONFIG(wipe_distance));
                const double support_distance = scale_(std::max(nozzle_diameter, outer_wall_line_width));
                Polyline inward_path = m_wipe.path;
                if (offset_wipe_path_toward_support(
                        inward_path, seam_start, seam_end, wipe_start,
                        wipe_offset_direction(is_ccw, is_hole), offset_dist, max_wipe_length,
                        wipe_support->inner_lines, wipe_support->printed_lines,
                        m_wipe.path.lines(), support_distance)) {
                    m_wipe.path = std::move(inward_path);
                    wipe_inward_applied = true;
                }
            }
        }
    }

    if (!conformal_loop && !m_emitting_deferred && wipe_on_loops_dest) {
        gcode += m_writer.extrude_to_xy(
            this->point_to_gcode(*wipe_on_loops_dest), 0, "move inwards before travel", true);
        this->set_last_pos(*wipe_on_loops_dest);
    }

    if (wipe_inward_applied)
        gcode += m_wipe.wipe(*this, 0.);

    return gcode;
}

void GCode::start_at_upper_seam(const Layer &layer, ExtrusionPath &run) const
{
    Points3 &pts = run.polyline.points;
    if (layer.upper_layer == nullptr || pts.size() < 4 ||
        (pts.front().to_point() - pts.back().to_point()).cast<double>().norm() > scale_(2. * run.width))
        return;
    const Point            start = pts.front().to_point();
    const ExtrusionLoop   *nearest = nullptr;
    double                 nearest_d = std::numeric_limits<double>::max();
    std::vector<const ExtrusionEntity *> stack;
    for (const LayerRegion *layerm : layer.upper_layer->regions())
        stack.insert(stack.end(), layerm->perimeters.entities.begin(), layerm->perimeters.entities.end());
    while (!stack.empty()) {
        const ExtrusionEntity *ee = stack.back();
        stack.pop_back();
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(ee))
            stack.insert(stack.end(), c->entities.begin(), c->entities.end());
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(ee);
                 loop != nullptr && loop->role() == erExternalPerimeter)
            for (const Line &l : loop->polygon().lines())
                if (const double d = l.distance_to(start); d < nearest_d)
                    nearest_d = d, nearest = loop;
    }
    if (nearest == nullptr || nearest_d > scale_(2. * run.width))
        return;
    ExtrusionLoop upper = *nearest;
    float         overhang = 0.f;
    m_seam_placer.place_seam(layer.upper_layer, upper, start, overhang);
    const Point seam = upper.first_point();
    size_t      at   = 0;
    for (size_t i = 1; i < pts.size(); ++i)
        if ((pts[i].to_point() - seam).cast<double>().squaredNorm() < (pts[at].to_point() - seam).cast<double>().squaredNorm())
            at = i;
    if (at == 0 || at + 1 == pts.size())
        return;
    Points3 rotated(pts.begin() + at, pts.end());
    rotated.insert(rotated.end(), pts.begin(), pts.begin() + at + 1);
    pts = std::move(rotated);
}

std::string GCode::extrude_raised_walls(const Print &print, const std::vector<LayerToPrint> &layers, unsigned int extruder_id,
                                        size_t single_object_instance_idx)
{
    std::string gcode;
    for (const LayerToPrint &layer_to_print : layers) {
        const Layer *layer = layer_to_print.object_layer;
        if (layer == nullptr)
            continue;
        const PrintObject &object = layer_to_print.original_object != nullptr ? *layer_to_print.original_object : *layer->object();
        for (const LayerRegion *layerm : layer->regions()) {
            const PrintRegion &region = layerm->region();
            if (!region.config().nonplanar_sublayer_walls.value || region.extruder(frExternalPerimeter) - 1 != extruder_id)
                continue;
            std::vector<const ExtrusionPath *> raised;
            std::vector<const ExtrusionEntity *> stack(layerm->perimeters.entities.begin(), layerm->perimeters.entities.end());
            while (!stack.empty()) {
                const ExtrusionEntity *ee = stack.back();
                stack.pop_back();
                if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(ee))
                    stack.insert(stack.end(), c->entities.begin(), c->entities.end());
                else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(ee))
                    for (const ExtrusionPath &path : loop->raised)
                        raised.emplace_back(&path);
            }
            if (raised.empty())
                continue;
            const size_t i_begin = single_object_instance_idx == size_t(-1) ? 0 : single_object_instance_idx;
            const size_t i_end   = single_object_instance_idx == size_t(-1) ? object.instances().size() : single_object_instance_idx + 1;
            for (size_t instance_id = i_begin; instance_id < i_end; ++instance_id) {
                const PrintInstance &instance = object.instances()[instance_id];
                if (m_enable_exclude_object)
                    m_writer.set_object_start_str(this->object_exclude_start(instance));
                const Point &offset = instance.shift;
                m_last_obj_copy     = std::make_pair(&object, offset);
                this->set_origin(unscale(offset));
                this->on_set_origin(&object, offset);
                m_layer = layer;
                m_config.apply(print.default_region_config());
                m_config.apply(object.config(), true);
                m_config.apply(region.config());
                std::vector<bool> done(raised.size(), false);
                for (size_t n = 0; n < raised.size(); ++n) {
                    const Point from = this->last_pos();
                    size_t      best = 0;
                    bool        flip = false;
                    double      best_d = std::numeric_limits<double>::max();
                    for (size_t i = 0; i < raised.size(); ++i)
                        if (!done[i])
                            for (bool rev : { false, true }) {
                                const Point p = rev ? raised[i]->last_point() : raised[i]->first_point();
                                if (const double d = (p - from).cast<double>().squaredNorm(); d < best_d)
                                    best_d = d, best = i, flip = rev;
                            }
                    done[best] = true;
                    ExtrusionPath run = *raised[best];
                    if (flip)
                        run.reverse();
                    this->start_at_upper_seam(*layer, run);
                    gcode += this->extrude_path(run, "sub-layer wall", -1.);
                }
                if (!m_writer.is_object_start_str_empty())
                    m_writer.set_object_start_str("");
                else if (m_enable_exclude_object)
                    m_writer.set_object_end_str(this->object_exclude_end(instance));
            }
        }
    }
    return gcode;
}

std::string GCode::extrude_multi_path(const ExtrusionMultiPath& multipath, const std::string& description, double speed)
{
    std::string gcode;

    m_multi_flow_segment_path_pa_set = false;
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    double weighted_sum_mm3_per_mm = 0.0;
    double total_multipath_length = 0.0;
    for (const ExtrusionPath& path : multipath.paths) {
        if(!path.is_force_no_extrusion()){
            double path_length = unscale<double>(path.length());
            weighted_sum_mm3_per_mm += path.mm3_per_mm * path_length;
            total_multipath_length += path_length;
        }
    }
    if (total_multipath_length > 0.0)
        m_multi_flow_segment_path_average_mm3_per_mm = weighted_sum_mm3_per_mm / total_multipath_length;

    for (const ExtrusionPath &path : multipath.paths){
        gcode += this->_extrude(path, description, speed);
        m_multi_flow_segment_path_pa_set = true;
    }

    if (has_nonplanar_source(multipath) || m_emitting_deferred ||
        (m_layer != nullptr && Nonplanar::wave_enabled(*m_layer->object()))) {
        m_wipe.reset_path();
    } else if (m_wipe.enable && FILAMENT_CONFIG(wipe))
        m_wipe.update_path(multipath.paths, true);

    return gcode;
}

std::string GCode::extrude_entity(const ExtrusionEntity&                     entity,
                                  const std::string&                         description,
                                  double                                     speed,
                                  const std::vector<const ExtrusionEntity*>& region_perimeters,
                                  const WipeInwardSupport*                   wipe_support)
{
    if (m_deferral_plan != nullptr) {
        std::string gcode;
        if (this->defer_planar_material(entity, description, speed, gcode))
            return gcode;
    }
    if (const ExtrusionPath* path = dynamic_cast<const ExtrusionPath*>(&entity))
        return this->extrude_path(*path, description, speed);
    else if (const ExtrusionMultiPath* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity))
        return this->extrude_multi_path(*multipath, description, speed);
    else if (const ExtrusionLoop* loop = dynamic_cast<const ExtrusionLoop*>(&entity))
        return this->extrude_loop(*loop, description, speed, region_perimeters, nullptr, wipe_support);
    else
        throw Slic3r::InvalidArgument("Invalid argument supplied to extrude()");
    return "";
}

static void collect_curved_tops_paths(const ExtrusionEntity &entity, std::vector<ExtrusionPath> &paths)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        if (path->nonplanar_source) paths.push_back(*path);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const auto &p : loop->paths) if (p.nonplanar_source) paths.push_back(p);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const auto &p : multi->paths) if (p.nonplanar_source) paths.push_back(p);
    } else if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const auto *child : collection->entities) collect_curved_tops_paths(*child, paths);
    }
}

static std::vector<ExtrusionPath> curved_roads(const Layer *layer, const std::vector<ExtrusionPath> &paths, const NonplanarClearanceModel &head,
                                               const Point &start)
{
    const auto *hints = layer != nullptr ? layer->object()->nonplanar_repair() : nullptr;
    if (layer == nullptr || paths.empty() || !curved_tops_mode(layer->object()->config().nonplanar_mode.value) ||
        (hints != nullptr && hints->ordered_surfaces.count(paths.front().source_surface_id) != 0))
        return order_curved_tops_paths(paths, head, start);
    if (hints == nullptr || hints->ordered_roads.empty())
        return paths;
    std::vector<ExtrusionPath> struck, whole;
    for (const ExtrusionPath &path : paths) {
        bool hit = false;
        for (size_t i = 0; i + 1 < path.polyline.points.size() && !hit; ++i) {
            const Vec2d a = unscale(path.polyline.points[i].to_point()), b = unscale(path.polyline.points[i + 1].to_point());
            for (const Nonplanar::RepairHints::Disc &disc : hints->ordered_roads)
                if (const Vec2d d = b - a; (disc.p - (a + std::clamp(d.squaredNorm() > 0. ? (disc.p - a).dot(d) / d.squaredNorm() : 0., 0., 1.) * d))
                                                .squaredNorm() < sqr(disc.radius)) {
                    hit = true;
                    break;
                }
        }
        (hit ? struck : whole).push_back(path);
    }
    if (struck.empty())
        return paths;
    std::vector<ExtrusionPath> out = order_curved_tops_paths(struck, head, start);
    append(out, std::move(whole));
    return out;
}

std::string GCode::extrude_curved_tops(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region)
{
    std::string gcode;
    for (const auto &region : by_region) {
        m_current_region = &print.get_print_region(&region - &by_region.front());
        m_config.apply(m_current_region->config());
        std::vector<ExtrusionPath> current;
        std::function<void(const ExtrusionEntity *, const std::string &)> collect = [&](const ExtrusionEntity *ee, const std::string &description) {
            if (!has_nonplanar_source(*ee)) return;
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
                for (const auto *child : collection->entities) collect(child, description);
            } else if (m_deferral_plan == nullptr || !this->defer_planar_material(*ee, description, -1., gcode, &current))
                collect_curved_tops_paths(*ee, current);
        };
        for (const auto *ee : region.perimeters) collect(ee, "perimeter");
        for (const auto *ee : region.infills) collect(ee, "infill");
        std::map<std::pair<float,int>, std::vector<ExtrusionPath>> shells;
        for (auto &path : current) shells[{-path.distance_to_top, path.source_surface_id}].push_back(std::move(path));
        for (auto &[key, paths] : shells)
            for (const auto &path : curved_roads(m_layer, paths, m_nonplanar_clearance, this->last_pos()))
                gcode += this->extrude_path(path, is_perimeter(path.role()) ? "perimeter" : "infill");
    }
    return gcode;
}

void GCode::queue_advanced_conformal(const Print &print)
{
    for (const auto &entry : m_nonplanar_deferral) {
        const auto *object = entry.first;
        const auto &plan = entry.second;
        if (plan.advanced_paths == 0)
            continue;
        for (const Layer *layer : object->layers()) {
            const int base = 2 * int(layer->id());
            for (const LayerRegion *layerm : layer->regions()) {
                const PrintRegion &region = print.get_print_region(layerm->region().print_region_id());
                auto queue_paths = [&](const ExtrusionEntitiesPtr &entities, const std::string &description, FlowRole role) {
                    std::vector<const ExtrusionPath*> paths;
                    std::function<void(const ExtrusionEntity*)> collect = [&](const ExtrusionEntity *e) {
                        if (auto *path = dynamic_cast<const ExtrusionPath*>(e)) paths.push_back(path);
                        else if (auto *loop = dynamic_cast<const ExtrusionLoop*>(e)) for (const ExtrusionPath &q : loop->paths) paths.push_back(&q);
                        else if (auto *multi = dynamic_cast<const ExtrusionMultiPath*>(e)) for (const ExtrusionPath &q : multi->paths) paths.push_back(&q);
                        else if (auto *c = dynamic_cast<const ExtrusionEntityCollection*>(e)) for (const ExtrusionEntity *child : c->entities) collect(child);
                    };
                    for (const ExtrusionEntity *e : entities)
                        collect(e);
                    for (const ExtrusionPath *path : paths) {
                        if (! path->nonplanar_source)
                            continue;
                        bool reversed = false;
                        const auto *path_plan = plan.find(layer->id(), path->source_path_id, path->polyline, reversed);
                        if (path_plan == nullptr)
                            continue;
                        for (const auto &[slot, a, b] : plan_pieces(*path_plan, reversed, polyline_length_scaled(path->polyline))) {
                            if (slot >= base)
                                continue;
                            Polyline3 piece = sub_polyline(path->polyline, a, b);
                            if (piece.points.size() < 2)
                                continue;
                            double zmin = std::numeric_limits<double>::max();
                            for (const Point3 &q : piece.points)
                                zmin = std::min(zmin, unscale<double>(q.z()));
                            for (size_t instance = 0; instance < object->instances().size(); ++ instance)
                                m_deferred_extrusions[{ object, instance }].push_back({ slot, base, zmin, layer->id(), layer->print_z,
                                    std::make_shared<ExtrusionPath>(piece, *path), description, -1., &region,
                                    unsigned(std::max(0, int(region.extruder(role)) - 1)) });
                        }
                    }
                };
                queue_paths(layerm->perimeters.entities, "perimeter", frPerimeter);
                queue_paths(layerm->fills.entities, "infill", frSolidInfill);
            }
        }
    }
}

static bool           scales_contoured_flow(const ExtrusionPath &path, const Layer *layer);
static ExtrusionPaths absolute_contoured(const ExtrusionPath &path, double layer_z, bool scale_flow);

bool GCode::defer_planar_material(const ExtrusionEntity &entity, const std::string &description, double speed, std::string &gcode,
                                  std::vector<ExtrusionPath> *curved_current)
{
    std::vector<const ExtrusionPath*> paths;
    if (auto *path = dynamic_cast<const ExtrusionPath*>(&entity))
        paths.push_back(path);
    else if (auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity))
        for (const ExtrusionPath &p : loop->paths) paths.push_back(&p);
    else if (auto *multi = dynamic_cast<const ExtrusionMultiPath*>(&entity))
        for (const ExtrusionPath &p : multi->paths) paths.push_back(&p);

    struct Planned { const ExtrusionPath *path; const NonplanarDeferral::PathPlan *plan; bool reversed; };
    std::vector<Planned> planned;
    bool any = false, cut = false;
    int  slot = -1;
    for (const ExtrusionPath *path : paths) {
        bool reversed = false;
        const auto *plan = m_deferral_plan->find(m_deferral_layer_id, path->source_path_id, path->polyline, reversed);
        planned.push_back({ path, plan, reversed });
        if (plan != nullptr) {
            any = true;
            cut |= plan->breaks.size() > 1;
            slot = std::max(slot, plan->breaks.front().second);
        }
    }
    const int base = 2 * int(m_deferral_layer_id);
    if (! any || (! cut && slot == base))
        return false;
    if (! cut && slot < base)
        return true;

    const double layer_z = this->layer()->print_z;
    auto queue = [&](std::shared_ptr<ExtrusionEntity> unit, int unit_slot) {
        double zmin = std::numeric_limits<double>::max();
        auto at_height = [&](ExtrusionPaths &paths) {
            ExtrusionPaths out;
            for (ExtrusionPath &path : paths) {
                if (path.z_contoured)
                    append(out, absolute_contoured(path, layer_z, scales_contoured_flow(path, m_layer)));
                else {
                    if (! path.nonplanar_source)
                        for (Point3 &p : path.polyline.points)
                            p.z() = scaled<coord_t>(layer_z);
                    out.emplace_back(std::move(path));
                }
            }
            for (const ExtrusionPath &path : out)
                for (const Point3 &p : path.polyline.points)
                    zmin = std::min(zmin, unscale<double>(p.z()));
            paths = std::move(out);
        };
        if (auto *path = dynamic_cast<ExtrusionPath*>(unit.get()); path != nullptr && path->z_contoured)
            unit = std::make_shared<ExtrusionMultiPath>(*path);
        if (auto *loop = dynamic_cast<ExtrusionLoop*>(unit.get())) {
            float seam_overhang = std::numeric_limits<float>::lowest();
            if (! m_config.spiral_mode && description == "perimeter")
                m_seam_placer.place_seam(m_layer, *loop, this->last_pos(), seam_overhang);
            else
                loop->split_at(this->last_pos(), false);
            at_height(loop->paths);
        } else if (auto *multi = dynamic_cast<ExtrusionMultiPath*>(unit.get())) {
            at_height(multi->paths);
        } else if (auto *path = dynamic_cast<ExtrusionPath*>(unit.get())) {
            ExtrusionPaths one{ std::move(*path) };
            at_height(one);
            *path = std::move(one.front());
        }
        m_deferred_extrusions[m_deferral_key].push_back({ unit_slot, base, zmin, m_deferral_layer_id, layer_z, std::move(unit),
            description, speed, m_current_region, m_writer.filament() ? m_writer.filament()->id() : 0u });
    };

    if (! cut) {
        queue(std::shared_ptr<ExtrusionEntity>(entity.clone()), slot);
        return true;
    }

    std::vector<ExtrusionPath> keep;
    for (const Planned &item : planned) {
        const ExtrusionPath &path = *item.path;
        if (item.plan == nullptr) {
            keep.push_back(path);
            continue;
        }
        for (const auto &[piece_slot, a, b] : plan_pieces(*item.plan, item.reversed, polyline_length_scaled(path.polyline))) {
            Polyline3 piece = sub_polyline(path.polyline, a, b);
            if (piece.points.size() < 2)
                continue;
            if (piece_slot < base)
                continue;
            if (piece_slot == base)
                keep.emplace_back(std::move(piece), path);
            else
                queue(std::make_shared<ExtrusionPath>(std::move(piece), path), piece_slot);
        }
    }
    for (ExtrusionPath &path : keep)
        if (curved_current != nullptr) curved_current->push_back(std::move(path));
        else gcode += this->extrude_path(path, description, speed);
    return true;
}

std::string GCode::emit_deferred_material(int slot, unsigned int extruder_id)
{
    std::string gcode;
    auto &queue = m_deferred_extrusions[m_deferral_key];
    std::vector<DeferredExtrusion> now;
    std::vector<DeferredExtrusion> later;
    for (DeferredExtrusion &d : queue)
        (d.slot == slot && d.extruder == extruder_id ? now : later).push_back(std::move(d));
    queue = std::move(later);
    std::stable_sort(now.begin(), now.end(), [](const DeferredExtrusion &a, const DeferredExtrusion &b) { return a.base < b.base; });
    m_force_safe_travel = true;
    m_force_safe_travel_once = false;
    std::vector<bool> emitted(now.size(), false);
    for (size_t index = 0; index < now.size(); ++index) {
        if (emitted[index]) continue;
        DeferredExtrusion &d = now[index];
        if (d.region != nullptr)
            m_config.apply(d.region->config());
        char buf[160];
        sprintf(buf, ";PATH_DEFERRED from_layer=%zu from_layer_z=%.3f min_z=%.3f emitted_layer=%zu slot=%d\n", d.source_layer, d.source_layer_z, d.z, m_deferral_layer_id, slot);
        gcode += buf;
        const NonplanarDeferral *plan = m_deferral_plan;
        m_deferral_plan = nullptr;
        m_loop_seam_placed = true;
        m_emitting_deferred = true;
        std::vector<ExtrusionPath> paths;
        collect_curved_tops_paths(*d.entity, paths);
        if (!paths.empty()) {
            const auto surface = paths.front().source_surface_id;
            const auto shell = paths.front().distance_to_top;
            for (size_t end = index + 1; end < now.size() && now[end].base == d.base; ++end) {
                if (emitted[end] || now[end].region != d.region) continue;
                std::vector<ExtrusionPath> next;
                collect_curved_tops_paths(*now[end].entity, next);
                if (next.empty() || next.front().source_surface_id != surface || next.front().distance_to_top != shell) continue;
                paths.insert(paths.end(), next.begin(), next.end());
                emitted[end] = true;
            }
            for (const auto &path : curved_roads(m_layer, paths, m_nonplanar_clearance, this->last_pos()))
                gcode += this->extrude_path(path, is_perimeter(path.role()) ? "perimeter" : "infill");
        } else gcode += this->extrude_entity(*d.entity, d.description, d.speed);
        m_emitting_deferred = false;
        gcode += ";PATH_DEFERRED_END\n";
        m_loop_seam_placed = false;
        m_deferral_plan = plan;
    }
    m_force_safe_travel_once = true;
    return gcode;
}

std::string GCode::emit_deferred_before_layer(unsigned int extruder_id)
{
    std::string gcode;
    if (m_deferral_plan == nullptr)
        return gcode;
    const int before = 2 * int(m_deferral_layer_id);
    auto it = m_deferred_extrusions.find(m_deferral_key);
    while (it != m_deferred_extrusions.end()) {
        int next = std::numeric_limits<int>::max();
        for (const DeferredExtrusion &d : it->second)
            if (d.extruder == extruder_id && d.slot < before)
                next = std::min(next, d.slot);
        if (next == std::numeric_limits<int>::max())
            break;
        gcode += this->emit_deferred_material(next, extruder_id);
    }
    return gcode;
}

std::string GCode::emit_deferred_after_layer(const PrintObject &print_object, unsigned int extruder_id)
{
    std::string gcode;
    if (m_deferral_plan == nullptr)
        return gcode;
    const int  slot = 2 * int(m_deferral_layer_id) + 1;
    const bool last = m_deferral_layer_id == print_object.layers().back()->id();
    auto it = m_deferred_extrusions.find(m_deferral_key);
    while (it != m_deferred_extrusions.end()) {
        int next = std::numeric_limits<int>::max();
        for (const DeferredExtrusion &d : it->second)
            if (d.extruder == extruder_id && (d.slot == slot || (last && d.slot > slot)))
                next = std::min(next, d.slot);
        if (next == std::numeric_limits<int>::max())
            break;
        gcode += this->emit_deferred_material(next, extruder_id);
    }
    m_deferral_plan = nullptr;
    return gcode;
}

std::string GCode::extrude_path(const ExtrusionPath& path, const std::string& description, double speed)
{
    m_multi_flow_segment_path_pa_set = false;
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    std::string gcode = this->_extrude(path, description, speed);
    if (gcode.empty())
        return gcode;
    if (path.nonplanar_source || m_emitting_deferred ||
        (m_layer != nullptr && Nonplanar::wave_enabled(*m_layer->object()))) {
        m_wipe.reset_path();
    } else if (m_wipe.enable && FILAMENT_CONFIG(wipe)) {
        m_wipe.reset_path();
        m_wipe.path = path.polyline.to_polyline();
        if (is_tree(this->config().support_type) && is_support(path.role())) {
            if ((m_wipe.path.first_point() - m_wipe.path.last_point()).cast<double>().norm() > scale_(0.2)) {
                double min_dist = scale_(0.2);
                int    i        = 0;
                for (; i < path.polyline.points.size(); i++) {
                    double dist = (path.polyline.points[i] - path.last_point3()).cast<double>().norm();
                    if (dist < min_dist) min_dist = dist;
                    if (min_dist < scale_(0.2) && dist > min_dist) break;
                }
                m_wipe.path = Polyline3(Points3(path.polyline.points.begin() + i - 1, path.polyline.points.end())).to_polyline();
            }
        } else
            m_wipe.path.reverse();
        if (const Nonplanar::RepairHints *hints = m_layer != nullptr ? m_layer->object()->nonplanar_repair() : nullptr;
            (path.z_contoured || path.role() == erTopSolidInfill) && hints != nullptr && hints->travel_lift > 0.)
            m_wipe.reset_path();
        if (path.z_contoured && m_config.top_texture.value != TopTexture::None)
            m_wipe.reset_path();
    }

    return gcode;
}

std::string GCode::extrude_perimeters(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, bool is_first_layer, bool is_infill_first, bool unsupported_loops_only)
{
    std::string gcode;
    for (const ObjectByExtruder::Island::Region &region : by_region)
        if (! region.perimeters.empty()) {
            m_config.apply(print.get_print_region(&region - &by_region.front()).config());
            m_current_region = &print.get_print_region(&region - &by_region.front());
            std::vector<const ExtrusionEntity *> curved_order;
            if (curved_tops_mode(m_config.nonplanar_mode.value)) {
                curved_order = region.perimeters;
                std::stable_sort(curved_order.begin(), curved_order.end(), [](const auto *a, const auto *b) {
                    return nonplanar_before_planar(*a, *b);
                });
            }
            const auto &perimeters = curved_order.empty() ? region.perimeters : curved_order;

            const bool should_print = is_first_layer ? !is_infill_first
                : (m_config.is_infill_first == is_infill_first);
            if (!should_print) continue;

            std::optional<WipeInwardSupport> wipe_support;
            if (m_wipe.enable && FILAMENT_CONFIG(wipe) && m_config.wipe_inward &&
                m_config.wipe_inward_distance.value > 0. &&
                scale_(FILAMENT_CONFIG(wipe_distance)) > SCALED_EPSILON)
                wipe_support.emplace();

            const bool defer_unsupported = !is_infill_first;
            auto waits_for_infill = [](const ExtrusionEntity *ee) {
                return ee->is_loop() && static_cast<const ExtrusionLoop *>(ee)->print_after_infill;
            };

            if (wipe_support && defer_unsupported && unsupported_loops_only)
                for (const ExtrusionEntity* ee : perimeters)
                    if (!waits_for_infill(ee))
                        wipe_support->append(*ee);

            for (const ExtrusionEntity* ee : perimeters) {
                if (curved_tops_mode(m_config.nonplanar_mode.value) && has_nonplanar_source(*ee)) continue;
                if (defer_unsupported && waits_for_infill(ee) != unsupported_loops_only)
                    continue;
                gcode += this->extrude_entity(*ee, "perimeter", -1., region.perimeters,
                                             wipe_support ? &*wipe_support : nullptr);
                if (wipe_support)
                    wipe_support->append(*ee);
            }
        }
    return gcode;
}

std::string GCode::extrude_infill(const Print &print, const std::vector<ObjectByExtruder::Island::Region> &by_region, bool ironing)
{
    std::string                                   gcode;
    std::vector<const ExtrusionEntity*>           extrusions;
    std::vector<std::unique_ptr<ExtrusionEntity>> reversed;
    const char*                                   extrusion_name = ironing ? "ironing" : "infill";
    for (const ObjectByExtruder::Island::Region &region : by_region)
        if (! region.infills.empty()) {
            extrusions.clear();
            extrusions.reserve(region.infills.size());
            for (const ExtrusionEntity *ee : region.infills)
                if ((ee->role() == erIroning) == ironing &&
                    !(curved_tops_mode(m_config.nonplanar_mode.value) && has_nonplanar_source(*ee)))
                    extrusions.emplace_back(ee);
            if (! extrusions.empty()) {
                m_config.apply(print.get_print_region(&region - &by_region.front()).config());
                const bool fallback = m_layer != nullptr && m_layer->object()->curved_fallback_zaa();
                const bool textured = m_config.top_texture.value != TopTexture::None;
                if (curved_tops_mode(m_config.nonplanar_mode.value) && !fallback && !textured) {
                m_current_region = &print.get_print_region(&region - &by_region.front());
                std::stable_sort(extrusions.begin(), extrusions.end(), [](const auto *a, const auto *b) {
                    return nonplanar_before_planar(*a, *b);
                });
                for (size_t begin = 0; begin < extrusions.size();) {
                    size_t end = begin + 1;
                    while (end < extrusions.size() && same_nonplanar_emission_group(*extrusions[begin], *extrusions[end])) ++end;
                    std::vector<const ExtrusionEntity *> group(extrusions.begin() + begin, extrusions.begin() + end);
                    std::vector<std::unique_ptr<ExtrusionEntity>> owned;
                    chain_and_reorder_extrusion_entities(group, m_last_pos.to_point(), owned);
                    for (const ExtrusionEntity *fill : group) {
                        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(fill)) {
                            ExtrusionEntityCollection chained(*collection);
                            if (!chained.no_sort) chain_and_reorder_extrusion_entities(chained.entities, m_last_pos.to_point());
                            for (const auto *ee : chained.entities) gcode += this->extrude_entity(*ee, extrusion_name);
                        } else gcode += this->extrude_entity(*fill, extrusion_name);
                    }
                    begin = end;
                }
                } else {
                std::vector<const ExtrusionEntity *> tops;
                if (curved_tops_mode(m_config.nonplanar_mode.value) && textured)
                    m_current_region = &print.get_print_region(&region - &by_region.front());
                if ((m_config.zaa_enabled && m_config.zaa_mid_layer_slice) || fallback || textured) {
                    std::function<bool(const ExtrusionEntity &)> has_top = [&has_top, textured](const ExtrusionEntity &e) {
                        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(&e))
                            return std::any_of(c->entities.begin(), c->entities.end(), [&has_top](const ExtrusionEntity *x) { return has_top(*x); });
                        if (const auto *path = dynamic_cast<const ExtrusionPath *>(&e); textured && path != nullptr)
                            return path->z_contoured;
                        return !textured && e.role() == erTopSolidInfill;
                    };
                    const auto split = std::stable_partition(extrusions.begin(), extrusions.end(), [&has_top](const ExtrusionEntity *e) { return !has_top(*e); });
                    tops.assign(split, extrusions.end());
                    extrusions.erase(split, extrusions.end());
                }
                for (int pass = 0; pass < 2; ++pass) {
                reversed.clear();
                if (pass == 1)
                    extrusions = std::move(tops);
                else
                    chain_and_reorder_extrusion_entities(extrusions, m_last_pos.to_point(), reversed);
                auto next_reversed = reversed.begin();
                for (const ExtrusionEntity *fill : extrusions) {
                    ExtrusionEntity *own_copy = next_reversed != reversed.end() && next_reversed->get() == fill ? (next_reversed++)->get() : nullptr;
                    auto *eec = dynamic_cast<const ExtrusionEntityCollection*>(fill);
                    if (eec) {
                        ExtrusionEntityCollection chained = own_copy ? std::move(static_cast<ExtrusionEntityCollection&>(*own_copy)) : ExtrusionEntityCollection(*eec);
                        if (!chained.no_sort)
                            chain_and_reorder_extrusion_entities(chained.entities, m_last_pos.to_point());
                        for (ExtrusionEntity *ee : chained.entities)
                            gcode += this->extrude_entity(*ee, extrusion_name);
                    } else
                        gcode += this->extrude_entity(*fill, extrusion_name);
                }
                }
                }
            }
        }
    return gcode;
}

std::string GCode::extrude_conformal_patches(const Print &print, const std::vector<LayerToPrint> &layers, unsigned int extruder_id,
                                             size_t single_object_instance_idx, bool before)
{
    std::string gcode;
    m_writer.set_z_travel_cap(m_writer.max_speed_z());
    struct ResetCap { GCodeWriter &w; double after; ~ResetCap() { w.set_z_travel_cap(after); } }
        reset_cap{ m_writer, m_wave_fields.empty() ? 0. : m_writer.max_speed_z() };
    for (const LayerToPrint &layer_to_print : layers) {
        const Layer *layer = layer_to_print.object_layer;
        if (layer == nullptr)
            continue;
        const std::vector<int> &patch_ids = before ? layer->conformal_patches_before : layer->conformal_patches_after;
        if (patch_ids.empty())
            continue;
        const PrintObject                &object    = layer_to_print.original_object != nullptr ? *layer_to_print.original_object : *layer->object();
        const Nonplanar::ConformalResult &conformal = object.conformal();
        const size_t i_begin = single_object_instance_idx == size_t(-1) ? 0 : single_object_instance_idx;
        const size_t i_end   = single_object_instance_idx == size_t(-1) ? object.instances().size() : single_object_instance_idx + 1;
        for (size_t instance_id = i_begin; instance_id < i_end; ++instance_id) {
            const PrintInstance &instance = object.instances()[instance_id];
            if (m_enable_exclude_object)
                m_writer.set_object_start_str(this->object_exclude_start(instance));
            for (int patch_id : patch_ids) {
                const Nonplanar::ConformalPatch &patch  = conformal.patches[size_t(patch_id)];
                const PrintRegion               &region = object.printing_region(size_t(patch.region_id));
                if (unsigned(Nonplanar::patch_filament_id(object, patch) - 1) != extruder_id || !Nonplanar::patch_has_extrusions(patch))
                    continue;
                const Point &offset = instance.shift;
                m_last_obj_copy     = std::make_pair(&object, offset);
                this->set_origin(unscale(offset));
                this->on_set_origin(&object, offset);
                m_layer = layer;
                m_config.apply(print.default_region_config());
                m_config.apply(object.config(), true);
                m_config.apply(region.config());
                if (patch.kind == Nonplanar::PatchKind::Field) {
                    gcode += this->extrude_field_patch(patch);
                    m_max_layer_z = std::max(m_max_layer_z, float(patch.z_max));
                    continue;
                }
                if (patch.kind == Nonplanar::PatchKind::Chunk) {
                    gcode += this->extrude_multiplanar_chunk(patch);
                    m_max_layer_z = std::max(m_max_layer_z, float(patch.z_max));
                    continue;
                }
                const std::function<Vec2d(const Vec2d &)> slope_gradient = [&patch](const Vec2d &p) {
                    const Vec2d g = patch.surface.gradient(p);
                    return std::isnan(g.x()) || std::isnan(g.y()) ? Vec2d(Vec2d::Zero()) : g;
                };
                std::function<Vec2d(const Vec2d &)> surface_gradient;
                if (patch.band < 0 || patch.kind == Nonplanar::PatchKind::Top)
                    surface_gradient = slope_gradient;
                gcode += Nonplanar::auto_comment(patch);
                const size_t passes = patch.shell_paths.size() + (patch.ironing.empty() ? 0 : 1);
                auto         pass   = [&patch](size_t k) -> const ExtrusionEntityCollection & {
                    return k < patch.shell_paths.size() ? patch.shell_paths[k] : patch.ironing;
                };
                auto hold_before = [&patch, &pass](size_t k) {
                    return k > 0 && k < patch.shell_paths.size() && Nonplanar::pass_is_solid(pass(k - 1)) &&
                           Nonplanar::pass_is_top(pass(k));
                };
                const bool imported = !object.config().nonplanar_atom_toolpath.value.empty() || object.conformal().ordered_plan;
                auto printed = [imported](const ExtrusionEntity *ee) -> const ExtrusionPathConformal * {
                    const auto *path = dynamic_cast<const ExtrusionPathConformal *>(ee);
                    return path != nullptr && (imported || unscale<double>(path->polyline.length()) >= Nonplanar::ConformalMinPath) ? path : nullptr;
                };
                std::vector<const ExtrusionPathConformal *> order;
                std::vector<size_t>                         pass_end;
                for (size_t k = 0; k < passes; ++k) {
                    for (const ExtrusionEntity *ee : pass(k).entities)
                        if (const auto *path = printed(ee))
                            order.emplace_back(path);
                    pass_end.emplace_back(order.size());
                }
                std::deque<ExtrusionPathConformal> reordered;
                if (!imported && m_writer.tool_kinematics().rotary_count() == 0)
                    for (size_t k = 0; k < patch.shell_paths.size(); ++k) {
                        const size_t b = k == 0 ? 0 : pass_end[k - 1];
                        const bool   visible = std::any_of(order.begin() + b, order.begin() + pass_end[k], [](const ExtrusionPathConformal *p) {
                            return p->role() == erTopSolidInfill || p->role() == erBottomSurface || p->role() == erIroning;
                        });
                        coord_t lo = std::numeric_limits<coord_t>::max(), hi = std::numeric_limits<coord_t>::lowest();
                        if (patch.kind == Nonplanar::PatchKind::Overhang)
                            for (auto it = order.begin() + b; it != order.begin() + pass_end[k]; ++it)
                                for (const Point3 &q : (*it)->polyline.points)
                                    lo = std::min(lo, q.z()), hi = std::max(hi, q.z());
                        const bool flat = patch.kind == Nonplanar::PatchKind::Overhang && hi - lo < scaled<coord_t>(0.02);
                        if (!visible && !flat)
                            Nonplanar::order_pass_roads(order, b, pass_end[k], reordered);
                    }
                std::vector<ExtrusionPathConformal> oriented;
                Nonplanar::orient_free_roads(order, oriented, 0.1, patch.kind == Nonplanar::PatchKind::Volume && conformal.morph_strips);
                std::deque<ExtrusionPathConformal> fuzzed;
                if (patch.kind == Nonplanar::PatchKind::Overhang || patch.kind == Nonplanar::PatchKind::Volume) {
                    const FuzzySkinConfig fuzzy = Feature::FuzzySkin::fuzzy_skin_config(region.config(), int(layer->id()));
                    for (const ExtrusionPathConformal *&path : order)
                        if (const bool outer = is_external_perimeter(path->role()) || path->role() == erOverhangPerimeter;
                            (outer || path->role() == erPerimeter) && !path->preserve_points &&
                            Feature::FuzzySkin::should_fuzzify(fuzzy, int(layer->id()), outer ? 0 : 1, true)) {
                            fuzzed.emplace_back(*path);
                            Feature::FuzzySkin::fuzzy_conformal_path(fuzzed.back(), m_nominal_z, fuzzy);
                            path = &fuzzed.back();
                        }
                }
                size_t next = 0;
                m_conformal_band_patch = patch.band >= 0 ? patch.id : -1;
                m_conformal_may_hang   = patch.kind == Nonplanar::PatchKind::Overhang || patch.kind == Nonplanar::PatchKind::Bridge ||
                                       patch.kind == Nonplanar::PatchKind::Volume;
                m_conformal_prev       = nullptr;
                m_conformal_primed     = false;
                m_conformal_surface    = patch.kind == Nonplanar::PatchKind::Top && !patch.surface.empty() ? &patch.surface.get() : nullptr;
                m_conformal_tolerance  = patch.automatic ? patch.automatic->max_error : m_config.nonplanar_max_surface_error.value;
                if (! order.empty()) {
                    if (Point lid; m_config.spiral_mode && Nonplanar::lid_start(object, lid))
                        ;
                    else if (m_writer.tool_kinematics().rotary_count() == 0 && m_last_pos_defined && !imported &&
                             this->conformal_keep_primed(m_last_processor_extrusion_role, order.front()))
                        m_conformal_primed = true;
                    else
                        gcode += this->retract();
                    m_wipe.reset_path();
                    m_conformal_pa_tracker = std::make_unique<Nonplanar::PressureAdvanceTracker>(m_writer, this->conformal_pressure_advance(),
                                                                                                 order.front()->height);
                    m_conformal_pa_tracker->set_along_road(m_config.gcode_flavor.value == gcfKlipper ||
                                                           m_config.gcode_flavor.value == gcfRepRapFirmware);
                }
                for (size_t k = 0; k < passes; ++k) {
                    const bool  ironing = k == patch.shell_paths.size();
                    m_conformal_scored  = k + 1 == patch.shell_paths.size();
                    const char *what    = ironing ? "conformal ironing" :
                                          patch.kind == Nonplanar::PatchKind::Overhang ? "conformal overhang" :
                                          patch.kind == Nonplanar::PatchKind::Bridge ? "conformal bridge" :
                                          patch.kind == Nonplanar::PatchKind::SupportInterface ? "conformal support interface" : "conformal shell";
                    if (! Nonplanar::pass_prints(pass(k))) {
                        next = pass_end[k];
                        continue;
                    }
                    if (hold_before(k))
                        gcode += this->nonplanar_pass_hold(next < order.size() ? order[next] : nullptr);
                    this->nonplanar_pass_start(patch.footprint, ironing);
                    if (! ironing && whole_part_patch(patch)) {
                        m_last_height = level_height(pass(k));
                        gcode += this->curved_level_marker(print, m_nominal_z + level_top(pass(k)));
                    }
                    gcode += ";_NONPLANAR_BEGIN patch=" + std::to_string(patch.id) + " shell=" + std::to_string(k) + " band=" + std::to_string(patch.band) +
                             " rate=" + std::to_string(std::lround(this->conformal_segment_rate())) + "\n";
                    while (next < pass_end[k]) {
                        const ExtrusionPathConformal &path = *order[next++];
                        const size_t next_pass = size_t(std::upper_bound(pass_end.begin(), pass_end.end(), next) - pass_end.begin());
                        m_conformal_next_hold = next_pass > k && hold_before(next_pass);
                        gcode += this->extrude_conformal_path(path, what,
                                                              patch.surface.empty() ? std::function<Vec2d(const Vec2d &)>() : surface_gradient,
                                                              next < order.size() ? order[next] : nullptr,
                                                              !ironing && patch.kind == Nonplanar::PatchKind::Top && !patch.surface.empty() ?
                                                                  slope_gradient : nullptr);
                    }
                    gcode += ";_NONPLANAR_END\n";
                }
                if (m_conformal_pa_tracker)
                    m_conformal_pa_tracker->restore(gcode);
                m_conformal_pa_tracker.reset();
                gcode += this->nonplanar_patch_end();
                if (const std::string leave = this->rotary_hop(nullptr); !leave.empty())
                    gcode += ";_NONPLANAR_BEGIN patch=" + std::to_string(patch.id) + " shell=" + std::to_string(patch.shell_paths.size() - 1) +
                             " band=" + std::to_string(patch.band) + " rate=" + std::to_string(std::lround(this->conformal_segment_rate())) + "\n" +
                             leave + ";_NONPLANAR_END\n";
                m_conformal_scored = false;
                m_conformal_band_patch = -1;
                m_conformal_may_hang   = false;
                m_conformal_prev       = nullptr;
                m_conformal_surface    = nullptr;
                m_max_layer_z = std::max({ m_max_layer_z, float(patch.z_max + Nonplanar::GlideLift),
                                           float(m_nonplanar_ceiling - m_config.z_offset.value) });
            }
            if (! m_writer.is_object_start_str_empty())
                m_writer.set_object_start_str("");
            else if (m_enable_exclude_object)
                m_writer.set_object_end_str(this->object_exclude_end(instance));
        }
    }
    if (!gcode.empty()) {
        gcode += this->conformal_retract();
        const Vec2d  xy   = m_writer.get_position().head<2>();
        const double safe = m_print->has_wipe_tower() ? this->nonplanar_travel_z() : this->nonplanar_travel_z(&xy, &xy);
        gcode += m_writer.travel_to_z(std::max({ m_writer.get_position().z(), m_nominal_z + this->nonplanar_hop(), safe }),
                                      "lift over conformal shells", true, this->conformal_max_speed_z());
        m_conformal_lift = true;
    }
    return gcode;
}

static bool planar_beside_curved_tops(const Layer *layer, bool curved_objects, NonplanarMode mode, bool material_columns,
                                      const Print *print)
{
    return layer != nullptr && curved_objects && !curved_tops_mode(mode) && !material_columns &&
           !(print != nullptr && print->has_wipe_tower());
}

std::string GCode::travel_above_nonplanar(const Polyline &travel, const std::string &comment, double z)
{
    const double target_z = z == DBL_MAX ? m_nominal_z : z;
    double ceiling = this->nonplanar_travel_z();
    if (planar_beside_curved_tops(m_layer, !m_nonplanar_deferral.empty(), m_config.nonplanar_mode.value, bool(m_material_columns), m_print)) {
        std::vector<Vec2d> xy;
        for (const Point &p : travel.points) xy.emplace_back(this->point_to_gcode(p));
        ceiling = m_nonplanar_heights.travel_height(xy, m_nonplanar_clearance, std::max(m_writer.get_position().z(), target_z));
    }
    const double safe = std::max({ m_writer.get_position().z(), target_z, ceiling });
    std::string gcode = m_writer.travel_to_z(safe, "lift over deposited material", true, this->conformal_max_speed_z());
    for (size_t i = 1; i < travel.size(); ++i)
        gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
    gcode += m_writer.travel_to_z(target_z, "lower after non-planar travel", true, this->conformal_max_speed_z());
    m_need_change_layer_lift_z = false;
    return gcode;
}

static void snap_upright_roads(const ToolKinematics &kinematics, std::vector<Nonplanar::ConformalPoint> &points)
{
    if (kinematics.rotary_count() == 0 || !layout_is_table(kinematics.layout()))
        return;
    const double cos_hold = std::cos(Geometry::deg2rad(Nonplanar::UprightRoadDeg));
    if (std::all_of(points.begin(), points.end(),
                    [cos_hold](const Nonplanar::ConformalPoint &p) { return p.pose.axis.normalized().z() >= cos_hold; }))
        for (Nonplanar::ConformalPoint &p : points)
            p.pose.axis = Vec3d::UnitZ();
}

std::string GCode::extrude_conformal_path(const ExtrusionPathConformal &path, const std::string &description,
                                          const std::function<Vec2d(const Vec2d &)> &surface_gradient,
                                          const ExtrusionPathConformal *next,
                                          const std::function<Vec2d(const Vec2d &)> &slope_gradient)
{
    using namespace Nonplanar;
    std::string gcode;
    if (path.polyline.points.size() < 2)
        return gcode;

    const ToolKinematics &kinematics = m_writer.tool_kinematics();
    const bool            rotary     = kinematics.rotary_count() > 0;
    if (rotary && surface_gradient)
        if (ExtrusionPathConformal dense = path; densify_conformal_path(path, 0.5, dense)) {
            gcode = this->extrude_conformal_path(dense, description, surface_gradient, next, slope_gradient);
            m_conformal_prev = &path;
            return gcode;
        }
    const bool scarf_role = (path.role() == erTopSolidInfill && m_conformal_scored) ||
                            (m_config.seam_slope_type.value == SeamScarfType::All &&
                             (path.role() == erTopSolidInfill || path.role() == erSolidInfill || path.role() == erInternalInfill));
    if (!m_conformal_scarf && !rotary && !path.preserve_points && m_config.seam_slope_type.value != SeamScarfType::None &&
        !m_config.belt_printer.value && !m_config.spiral_mode.value && scarf_role &&
        (m_conformal_prev == nullptr || m_conformal_prev->polyline.points.back() != path.polyline.points.front()) &&
        (next == nullptr || next->polyline.points.front() != path.polyline.points.back())) {
        CurvedScarf scarf;
        const double h      = path.height > 0.f ? double(path.height) : 0.2;
        scarf.length        = m_config.seam_slope_entire_loop.value ? std::numeric_limits<double>::max() : m_config.seam_slope_min_length.value;
        scarf.start_share   = m_config.seam_slope_start_height.percent ? 0.01 * m_config.seam_slope_start_height.value :
                                                                         m_config.seam_slope_start_height.value / h;
        scarf.nozzle        = m_config.nozzle_diameter.get_at(m_writer.filament()->id());
        if (m_config.seam_slope_conditional.value)
            scarf.max_turn = PI - Geometry::deg2rad(double(m_config.scarf_angle_threshold.value));
        if (ExtrusionPathConformal scarfed = path; scarf_curved_loop(scarfed, scarf, slope_gradient)) {
            m_conformal_scarf = true;
            gcode = this->extrude_conformal_path(scarfed, description, surface_gradient, next, slope_gradient);
            m_conformal_scarf = false;
            m_conformal_prev  = &path;
            return gcode;
        }
    }
    m_conformal_points = conformal_points(path, [this](const Point3 &p) {
        const Vec2d xy = this->point_to_gcode(p.to_point());
        return Vec3d(xy.x(), xy.y(), m_nominal_z + unscale<double>(p.z()));
    });
    if (!rotary && !path.preserve_points && m_last_pos_defined && m_conformal_prev != nullptr &&
        m_conformal_prev->polyline.points.back() == path.polyline.points.front() &&
        std::all_of(m_conformal_points.begin(), m_conformal_points.end(), [](const ConformalPoint &p) { return p.bead_height <= 0.f; })) {
        Vec3d &start = m_conformal_points.front().pose.tip;
        const Vec3d &here = m_writer.get_position();
        if ((here - start).head<2>().norm() <= EPSILON && here.z() > start.z())
            start = here;
    }
    this->ensure_material_columns(path);
    const bool finish = path.role() == erTopSolidInfill || path.role() == erIroning || path.role() == erExternalPerimeter;
    if (rotary && surface_gradient) {
        MinimalTiltParams tilt;
        tilt.slope_limit = finish ? this->nonplanar_head().slope_map(72, path.height) : std::vector<double>(72, 0.5 * M_PI);
        tilt.max_rate      = 10. * M_PI / 180.;
        tilt.max_residual = 10. * M_PI / 180.;
        if (finish) {
            const double tolerance = m_conformal_tolerance > 0. ? m_conformal_tolerance : 0.05;
            tilt.residual_from = std::asin(std::min(1., 2. * tolerance / std::max(double(path.width), EPSILON)));
        } else
            tilt.residual_from = std::atan(2. * path.height / std::max(double(path.width), EPSILON));
        tilt.clearance_margin = this->nonplanar_head().margin;
        const Vec2d               origin  = this->point_to_gcode(Point(0, 0));
        const Nonplanar::RepairHints *repair = m_last_obj_copy.first != nullptr ? m_last_obj_copy.first->nonplanar_repair() : nullptr;
        if (m_material_columns)
            tilt.clearance = [this, &kinematics, origin, repair](const Vec3d &tip, const Vec3d &axis) {
                if (repair != nullptr)
                    for (const Nonplanar::RepairHints::Disc &disc : repair->upright)
                        if ((tip.head<2>() - origin - disc.p).squaredNorm() <= disc.radius * disc.radius)
                            return -std::numeric_limits<double>::infinity();
                const JointPose from = m_writer.get_joints();
                JointPose       joints;
                const Matrix3d  frame = kinematics.inverse({ tip, axis }, &from, joints) ?
                                            kinematics.head_frame(joints) :
                                            Matrix3d(Eigen::Quaterniond::FromTwoVectors(axis, Vec3d::UnitZ()).toRotationMatrix());
                return m_material_columns->tilted_clearance(tip, frame, this->nonplanar_head());
            };
        apply_minimal_tilt(m_conformal_points, [&surface_gradient, &origin](const Vec2d &p) { return surface_gradient(p - origin); },
                           kinematics, tilt);
    }
    snap_upright_roads(kinematics, m_conformal_points);
    this->clamp_table_tilt(m_conformal_points);

    m_wipe.reset_path();

    if (!path.entry_travel.points.empty()) {
        auto pose_at = [this, &path](size_t i) {
            const Point3 &p = path.entry_travel.points[i];
            const Vec2d xy = this->point_to_gcode(p.to_point());
            const Vec3d axis = path.entry_tool_axis.empty() ? Vec3d::UnitZ() :
                              Vec3d(path.entry_tool_axis[i].cast<double>().normalized());
            return ToolPose{ Vec3d(xy.x(), xy.y(), m_nominal_z + unscale<double>(p.z())), axis };
        };
        gcode += this->conformal_retract();
        m_writer.add_object_change_labels(gcode);
        const ToolPose first = pose_at(0);
        if ((m_writer.get_position() - first.tip).norm() > EPSILON ||
            (m_writer.get_tool_axis() - first.axis).norm() > EPSILON) {
            if (rotary)
                gcode += this->rotary_hop(&first);
            else
                gcode += this->surface_travel(first.tip, m_conformal_on_bead, true, false, "join Atomizer travel");
        }
        for (size_t i = 1; i < path.entry_travel.points.size(); ++i) {
            const ToolPose to = pose_at(i);
            const Vec3d d = Vec3d((to.tip * 1000.).array().round() / 1000.) -
                            Vec3d((m_writer.get_position() * 1000.).array().round() / 1000.);
            const double length = d.norm();
            if (length < EPSILON && (m_writer.get_tool_axis() - to.axis).norm() < EPSILON)
                continue;
            double speed = m_config.travel_speed.get_at(get_nozzle_config_index(m_writer.filament()->id()));
            if (!rotary && length > EPSILON) {
                const Vec3d limits(m_config.machine_max_speed_x.get_at(0), m_config.machine_max_speed_y.get_at(0),
                                   this->conformal_max_speed_z());
                for (int k = 0; k < 3; ++k)
                    if (limits[k] > 0. && std::abs(d[k]) > EPSILON)
                        speed = std::min(speed, 0.99 * limits[k] * length / std::abs(d[k]));
                speed = std::min(speed, length * this->conformal_segment_rate());
            }
            gcode += m_writer.travel_to_pose(to, "Atomizer travel", speed);
        }
        gcode += m_writer.end_inverse_time();
        m_conformal_on_bead = false;
    }

    const bool reseed = rotary && !m_conformal_primed &&
        (!m_last_pos_defined || (m_writer.get_position() - m_conformal_points.front().pose.tip).norm() > EPSILON);
    std::vector<RotaryRun> runs = rotary ? plan_rotary_runs(kinematics, m_conformal_points, m_writer.get_joints(), 90., reseed) :
                                           std::vector<RotaryRun>(1, RotaryRun{ 0, m_conformal_points.size() - 1, JointPose() });
    const std::vector<ConformalPoint> points = runs.size() > 1 ? m_conformal_points : std::vector<ConformalPoint>();
    Vec3d      hop_to;
    bool       glide_on = next != nullptr && next->polyline.points.front() == path.polyline.points.back();
    if (runs.size() > 1 && ! path.preserve_points && (runs.size() % 2 == 1 || ! glide_on) &&
        alternate_rotary_runs(kinematics, m_conformal_points, m_writer.get_joints(), runs))
        glide_on = glide_on && ! runs.back().reversed;
    const bool open_end = glide_on && next->polyline.points.size() >= 2 &&
                          (next->bead_height.size() < 2 || (next->bead_height[0] > 0.f && next->bead_height[1] > 0.f));
    for (size_t r = 0; r < runs.size(); ++r) {
        const RotaryRun &run = runs[r];
        std::optional<ExtrusionPathConformal> part;
        if (runs.size() > 1) {
            part.emplace(path);
            auto slice = [&run](auto &v) {
                if (v.size() > run.end)
                    v = std::decay_t<decltype(v)>(v.begin() + run.begin, v.begin() + run.end + 1);
            };
            slice(part->polyline.points);
            slice(part->bead_height);
            slice(part->tool_axis);
            slice(part->bead_width);
            m_conformal_points.assign(points.begin() + run.begin, points.begin() + run.end + 1);
            if (run.reversed) {
                part->reverse();
                std::reverse(m_conformal_points.begin(), m_conformal_points.end());
            }
        }
        const ExtrusionPathConformal &sub = part ? *part : path;
        const ToolPose &start = m_conformal_points.front().pose;
        const Vec3d    &here  = m_writer.get_position();
        const bool      primed = r == 0 && m_conformal_primed;
        m_conformal_primed     = false;
        if (rotary && r == 0 && m_last_pos_defined && (here - start.tip).norm() <= EPSILON &&
            m_writer.get_tool_axis().dot(start.axis) >= std::cos(5. * M_PI / 180.) && (m_writer.get_tool_axis() - start.axis).norm() > EPSILON)
            gcode += this->rotary_turn_on_spot(start);
        const bool lid_join = r == 0 && m_config.spiral_mode && path.role() == erExternalPerimeter && m_last_pos_defined &&
                              (here - start.tip).norm() <= 0.5 && (m_writer.get_tool_axis() - start.axis).norm() <= EPSILON;
        if (lid_join && (here - start.tip).norm() > EPSILON) {
            gcode += m_writer.travel_to_xyz(start.tip, "join the vase wall to its lid");
        } else if (r > 0 || ! m_last_pos_defined || (here - start.tip).norm() > EPSILON || (m_writer.get_tool_axis() - start.axis).norm() > EPSILON) {
            m_wipe.reset_path();
            if (!primed)
                gcode += m_conformal_on_bead || m_conformal_prev != nullptr ? this->conformal_retract() : this->retract();
            m_writer.add_object_change_labels(gcode);
            if (rotary && (r > 0 || start.axis.z() < 1. - EPSILON || ! this->rotary_at_home())) {
                JointPose nearest, selected;
                const JointPose current = m_writer.get_joints();
                const bool same_branch = kinematics.inverse(start, &current, nearest) &&
                    kinematics.inverse(start, &run.seed, selected) &&
                    std::abs(nearest.rot[0] - selected.rot[0]) <= 1e-4 &&
                    std::abs(nearest.rot[1] - selected.rot[1]) <= 1e-4;
                const std::string local = same_branch && (r == 0 || runs[r].reversed || runs[r - 1].reversed) ?
                    this->rotary_local_hop(start) : std::string();
                gcode += local.empty() ? this->rotary_hop(&start, &run.seed) : local;
            } else if (!m_wave_fields.empty() && m_layer != nullptr && m_wave_fields.count(m_layer->object()) > 0) {
                const Vec2d from = here.head<2>(), to = start.tip.head<2>();
                const double safe = std::max({ here.z(), start.tip.z(), this->nonplanar_travel_z(&from, &to) });
                gcode += m_writer.travel_to_z(safe, "lift over wave crests", true, this->conformal_max_speed_z());
                gcode += m_writer.travel_to_xy(start.tip.head<2>(), "move to wave path");
                gcode += m_writer.travel_to_z(start.tip.z(), "lower onto wave path", true, this->conformal_max_speed_z());
            } else if (m_material_columns && !rotary) {
                const bool lift = !primed && (m_conformal_prev == nullptr || this->conformal_lift_enforced(m_conformal_prev->role()));
                Vec2d via;
                Nonplanar::material_route(*m_material_columns, m_writer.get_position(), start.tip, 1., via);
                gcode += this->surface_travel(start.tip, m_conformal_on_bead, true, this->conformal_in_band(m_conformal_prev, &path),
                                              "move to conformal path", this->conformal_clearance(lift), via);
            } else {
                const bool   in_band = this->conformal_in_band(m_conformal_prev, &path);
                const Vec2d  from = here.head<2>(), to = start.tip.head<2>();
                double safe = std::max({ here.z(), start.tip.z(), this->nonplanar_travel_z(&from, &to, in_band) });
                if (!in_band && m_print->has_wipe_tower())
                    safe = std::max(safe, this->nonplanar_travel_z());
                gcode += m_writer.travel_to_z(safe, "lift over conformal shells", true, this->conformal_max_speed_z());
                gcode += m_writer.travel_to_xy(start.tip.head<2>(), "move to conformal path");
                gcode += m_writer.travel_to_z(start.tip.z(), "lower onto conformal path", true, this->conformal_max_speed_z());
            }
            m_conformal_ramp = !primed && !m_conformal_scarf;
            if (m_conformal_pa_tracker && path.role() == erIroning)
                m_conformal_pa_tracker->restore(gcode);
            else if (m_conformal_pa_tracker && !m_conformal_pa_tracker->along_road())
                m_conformal_pa_tracker->update(mean_bead_height(m_conformal_points), gcode);
        }
        if (!m_conformal_ramp && !m_conformal_scarf && m_writer.filament() != nullptr &&
            m_writer.get_extruder_retracted_length(int(m_writer.filament()->id())) > EPSILON)
            m_conformal_ramp = true;
        this->set_last_pos(sub.first_point());
        m_need_change_layer_lift_z = false;
        if (slope_gradient && m_config.nonplanar_flow_mod.value) {
            const Vec2d origin   = this->point_to_gcode(Point(0, 0));
            m_conformal_gradient = [&slope_gradient, origin](const Vec2d &p) { return slope_gradient(p - origin); };
        }
        m_conformal_open_end = (r + 1 == runs.size() && open_end) || m_conformal_scarf;
        gcode += ";_NONPLANAR_PATH\n";
        gcode += this->_extrude(sub, description);
        m_conformal_gradient = nullptr;
        m_conformal_ramp     = false;
        m_conformal_open_end = false;
        if (r + 1 < runs.size()) {
            gcode += path.preserve_points ? this->conformal_retract() : this->wipe_conformal_path();
            m_conformal_on_bead = true;
        }
    }
    if (path.preserve_points) {
        gcode += m_writer.end_inverse_time();
        m_conformal_on_bead = true;
        m_conformal_points.clear();
        m_conformal_prev = &path;
        return gcode;
    }
    const bool hop      = !glide_on && this->conformal_short_hop(path, next, hop_to);
    m_conformal_primed  = !glide_on && !hop && !rotary && !path.preserve_points && this->conformal_keep_primed(path.role(), next, &path);
    if (!glide_on && !hop) {
        if (!m_conformal_primed)
            gcode += this->wipe_conformal_path();
        m_conformal_on_bead = true;
    }
    gcode += m_writer.end_inverse_time();
    if (rotary) {
        if (next == nullptr) {
            gcode += this->rotary_hop(nullptr);
            m_conformal_on_bead = false;
        }
    } else if (next == nullptr && m_conformal_on_bead && m_material_columns) {
        gcode += this->conformal_retract();
        const Vec3d off = Nonplanar::normal_departure(*m_material_columns, Nonplanar::nozzle_land(this->nonplanar_head()),
                                                      m_writer.get_position(), this->conformal_clearance());
        const Vec3d d = off - m_writer.get_position();
        if (d.norm() > EPSILON) {
            const double vz_max = this->conformal_max_speed_z();
            double       v      = m_config.travel_speed.get_at(get_nozzle_config_index(m_writer.filament()->id()));
            if (vz_max > 0. && std::abs(d.z()) > EPSILON)
                v = std::min(v, 0.99 * vz_max * d.norm() / std::abs(d.z()));
            gcode += m_writer.travel_to_pose({ off, Vec3d::UnitZ() }, "leave the patch along the surface normal", v);
        }
        m_conformal_on_bead = false;
    }
    if (hop) {
        m_writer.add_object_change_labels(gcode);
        auto        printed = [](const Vec3d &p) { return Vec3d((p * 1000.).array().round() / 1000.); };
        const Vec3d d       = printed(hop_to) - printed(m_writer.get_position());
        if (d.norm() > EPSILON) {
            const double vz_max = this->conformal_max_speed_z();
            double       v      = m_config.travel_speed.get_at(get_nozzle_config_index(m_writer.filament()->id()));
            if (vz_max > 0. && std::abs(d.z()) > EPSILON)
                v = std::min(v, 0.99 * vz_max * d.norm() / std::abs(d.z()));
            v = std::min(v, d.norm() * Nonplanar::planner_segment_rate(m_config.nonplanar_segment_rate.value, m_config.gcode_flavor.value,
                                                                        m_print != nullptr && m_print->is_BBL_printer()));
            m_writer.set_speed(gcode, v * 60.);
            m_writer.extrude_to_pose(gcode, ToolPose{ hop_to, Vec3d::UnitZ() }, 0., "hop to conformal path");
        }
        this->set_last_pos(next->first_point());
        m_wipe.reset_path();
        m_conformal_on_bead = false;
    }
    m_conformal_points.clear();
    m_conformal_prev = &path;
    return gcode;
}

bool GCode::conformal_in_band(const ExtrusionPathConformal *prev, const ExtrusionPathConformal *path) const
{
    if (prev == nullptr || path == nullptr || m_conformal_band_patch < 0 || prev->patch_id != m_conformal_band_patch ||
        path->patch_id != m_conformal_band_patch)
        return false;
    for (const PrintObject *object : m_print->objects())
        if (const Nonplanar::RepairHints *hints = object->nonplanar_repair(); hints != nullptr && hints->travel_lift > 0.)
            return false;
    return true;
}

bool GCode::conformal_short_hop(const ExtrusionPathConformal &path, const ExtrusionPathConformal *next, Vec3d &to)
{
    using namespace Nonplanar;
    const bool in_band = this->conformal_in_band(&path, next);
    if (next == nullptr || (!in_band && next->patch_id != path.patch_id) || !m_material_columns || m_writer.filament() == nullptr ||
        !next->tool_axis.empty() || !this->rotary_at_home() || next->polyline.points.size() < 2)
        return false;
    const Vec2d  xy   = this->point_to_gcode(next->first_point());
    const Vec3d &here = m_writer.get_position();
    to                = Vec3d(xy.x(), xy.y(), m_nominal_z + unscale<double>(next->polyline.points.front().z()));
    const bool   flexible    = road_material(m_config.filament_type.get_at(m_writer.filament()->id())) == RoadMaterial::Flexible;
    const bool   into_top    = next->shell != path.shell && next->role() == erTopSolidInfill && path.role() != erInternalInfill;
    const double short_glide = flexible && !into_top ? 10. : 5.;
    const double minimum     = m_config.retraction_minimum_travel.get_at(m_writer.filament()->id());
    if ((to - here).head<2>().norm() >= (in_band ? std::max(minimum, short_glide) : std::min(minimum, 1.)))
        return false;
    const HeadEnvelope &head = this->nonplanar_head();
    const HeadEnvelope  land = nozzle_land(head);
    if (!in_band) {
        HeadEnvelope body = head;
        body.boxes.erase(std::remove_if(body.boxes.begin(), body.boxes.end(), [](const HeadBox &b) { return b.nozzle; }), body.boxes.end());
        if (!body.boxes.empty() && m_material_columns->swept_clearance(here, to, body) < 0.)
            return false;
    }
    const double length = (to - here).head<2>().norm();
    for (double s = 0.25; s < length - 0.25; s += 0.1) {
        const Vec3d p = here + (to - here) * (s / length);
        if (m_material_columns->top_in_rect(BoundingBoxf(p.head<2>() - Vec2d(0.1, 0.1), p.head<2>() + Vec2d(0.1, 0.1))) > p.z() + 0.1)
            return false;
    }
    auto depth = [&](const Vec3d &a, const Vec3d &b) { return -m_material_columns->swept_clearance(a, b, land); };
    const double accepted = std::max({ depth(here, here), depth(to, to), 0. }) + 0.01;
    const int    steps    = std::max(1, int(std::ceil(length / 0.1)));
    for (int k = 0; k < steps; ++k)
        if (depth(here + (to - here) * (double(k) / steps), here + (to - here) * (double(k + 1) / steps)) > accepted)
            return false;
    return true;
}

std::string GCode::extrude_multiplanar_chunk(const Nonplanar::ConformalPatch &patch)
{
    using namespace Nonplanar;
    std::string  gcode;
    const bool   rotary  = m_writer.tool_kinematics().rotary_count() > 0;
    const double hop     = std::max(this->nonplanar_head().margin,
                                    m_writer.filament() != nullptr ? m_config.z_hop.get_at(m_writer.filament()->id()) : 0.);
    bool         entered = false;
    double       top     = -std::numeric_limits<double>::infinity();
    m_wipe.reset_path();
    double       ceiling = -std::numeric_limits<double>::infinity();
    const auto   to_writer = [this](const Point3 &p) {
        const Vec2d xy = this->point_to_gcode(p.to_point());
        return Vec3d(xy.x(), xy.y(), m_nominal_z + unscale<double>(p.z()));
    };
    const double in_layer_travel = (full_orientation(m_config) || patch.free_travel) && m_writer.filament() != nullptr ?
        m_config.retraction_minimum_travel.get_at(m_writer.filament()->id()) : 0.;
    const double plane_travel = !rotary && patch.layer_normal.z() > EPSILON && m_writer.filament() != nullptr ?
        m_config.retraction_minimum_travel.get_at(m_writer.filament()->id()) : 0.;
    auto in_layer = [&](const Vec3d &from, const ExtrusionPathConformal *next) {
        if (next != nullptr && plane_travel > 0.) {
            const Vec3d to = to_writer(next->polyline.points.front());
            return (to - from).norm() < plane_travel && std::abs(patch.layer_normal.dot(to - from)) < 1e-3;
        }
        if (next == nullptr || in_layer_travel <= 0.)
            return false;
        const Vec3d to   = to_writer(next->polyline.points.front());
        const Vec3d axis = next->tool_axis.empty() ? Vec3d(Vec3d::UnitZ()) : Vec3d(next->tool_axis.front().cast<double>().normalized());
        return (to - from).norm() < in_layer_travel && std::abs(axis.dot(to - from)) < 1e-3 && axis.dot(from) > ceiling - 1e-3;
    };
    for (size_t k = 0; k < patch.shell_paths.size(); ++k) {
        gcode += ";_NONPLANAR_BEGIN patch=" + std::to_string(patch.id) + " shell=" + std::to_string(k) + " band=" + std::to_string(patch.band) +
                 " rate=" + std::to_string(std::lround(this->conformal_segment_rate())) + "\n";
        std::vector<const ExtrusionPathConformal *> paths;
        for (const ExtrusionEntity *ee : patch.shell_paths[k].entities)
            if (const auto *path = dynamic_cast<const ExtrusionPathConformal *>(ee); path != nullptr && path->polyline.points.size() >= 2)
                paths.emplace_back(path);
        enum class Reach { Lift, Retracted, Primed } reach = Reach::Lift;
        auto outer = [](const ExtrusionPathConformal &p) { return p.role() == erExternalPerimeter || p.role() == erOverhangPerimeter; };
        for (size_t i = 0; i < paths.size(); ++i) {
            const ExtrusionPathConformal *path = paths[i];
            m_conformal_points = conformal_points(*path, to_writer);
            const ToolPose &start = m_conformal_points.front().pose;
            const Vec3d    &axis  = start.axis;
            const Vec3d     here  = m_writer.get_position();
            if (reach != Reach::Lift) {
                if (reach == Reach::Retracted)
                    gcode += this->retract();
                if ((here - start.tip).norm() > EPSILON)
                    gcode += m_writer.travel_to_pose(start, "move in the layer");
            } else if (! entered || (here - start.tip).norm() > EPSILON) {
                gcode += this->retract();
                if (axis.z() > 1. - EPSILON && patch.layer_normal.z() > EPSILON) {
                    const Vec3d &n     = patch.layer_normal;
                    const double level = n.dot(start.tip);
                    auto over = [&](const Vec3d &p) { return (level - n.x() * p.x() - n.y() * p.y()) / n.z() + hop; };
                    gcode += m_writer.travel_to_z(std::max(here.z(), over(here)), "lift over the layer", true, this->conformal_max_speed_z());
                    gcode += m_writer.travel_to_xyz(Vec3d(start.tip.x(), start.tip.y(), std::max(start.tip.z(), over(start.tip))), "move over the layer",
                                                    true, this->conformal_max_speed_z());
                    gcode += m_writer.travel_to_z(start.tip.z(), "lower onto the layer", true, this->conformal_max_speed_z());
                } else if (axis.z() > 1. - EPSILON) {
                    const double travel_z = m_material_columns ?
                        safe_travel_z(*m_material_columns, this->nonplanar_head(), here.head<2>(), start.tip.head<2>(), hop) :
                        std::max(m_nonplanar_ceiling, double(m_max_layer_z)) + hop;
                    gcode += m_writer.travel_to_z(std::max({ here.z(), start.tip.z(), travel_z }), "lift over the chunks", true, this->conformal_max_speed_z());
                    gcode += m_writer.travel_to_xy(start.tip.head<2>(), "move in the chunk");
                    gcode += m_writer.travel_to_z(start.tip.z(), "lower onto the chunk", true, this->conformal_max_speed_z());
                } else if (! entered) {
                    gcode += this->rotary_move_safely(&start);
                } else {
                    const std::string local = full_orientation(m_config) || patch.free_travel ? std::string() : this->rotary_local_hop(start);
                    if (!local.empty())
                        gcode += local;
                    else {
                        const double level = std::max({ axis.dot(here), axis.dot(start.tip), ceiling + hop });
                        gcode += m_writer.travel_to_pose({ here + (level - axis.dot(here)) * axis, axis }, "lift along the tool");
                        gcode += m_writer.travel_to_pose({ start.tip + (level - axis.dot(start.tip)) * axis, axis }, "move in the chunk");
                        gcode += m_writer.travel_to_pose(start, "lower along the tool");
                    }
                }
            }
            if (! entered) {
                ceiling = axis.dot(start.tip);
                entered = true;
            }
            for (const ConformalPoint &p : m_conformal_points) {
                ceiling = std::max(ceiling, axis.dot(p.pose.tip));
                top     = std::max(top, p.pose.tip.z());
            }
            this->set_last_pos(path->first_point());
            m_need_change_layer_lift_z = false;
            m_conformal_open_end = patch.free_travel;
            gcode += this->_extrude(*path, "multiplanar chunk");
            m_conformal_open_end = false;
            const ExtrusionPathConformal *next = i + 1 < paths.size() ? paths[i + 1] : nullptr;
            const bool flows_on = patch.free_travel && next != nullptr &&
                                  (to_writer(next->polyline.points.front()) - m_writer.get_position()).norm() < 0.01;
            static const double keep_primed = [] { const char *v = research_switch("NP_NATIVE_KEEP_PRIMED"); return v != nullptr ? std::max(0., std::atof(v)) : 3.; }();
            const bool keep_short = keep_primed > 0. && next != nullptr &&
                                    (to_writer(next->polyline.points.front()) - m_writer.get_position()).norm() < keep_primed;
            reach = flows_on ? Reach::Primed :
                    ! in_layer(m_writer.get_position(), next) ? Reach::Lift :
                    (outer(*path) || outer(*next)) && ! keep_short ? Reach::Retracted : Reach::Primed;
            if (reach != Reach::Primed)
                gcode += this->wipe_conformal_path();
            gcode += m_writer.end_inverse_time();
            m_conformal_points.clear();
        }
        if (rotary && k + 1 == patch.shell_paths.size())
            gcode += this->rotary_move_safely(nullptr);
        gcode += ";_NONPLANAR_END\n";
    }
    if (! rotary && entered) {
        gcode += this->retract();
        gcode += m_writer.travel_to_z(std::max(m_writer.get_position().z(), top + hop), "lift off the chunk", true, this->conformal_max_speed_z());
    }
    gcode += m_writer.end_inverse_time();
    return gcode;
}

static bool macro_moves_head(const std::string &gcode)
{
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || (line[b] != 'G' && line[b] != 'g'))
            continue;
        const size_t e    = line.find_first_not_of("0123456789", b + 1);
        const int    code = e == b + 1 ? -1 : std::atoi(line.substr(b + 1, e - b - 1).c_str());
        if ((code >= 0 && code <= 3) || (code >= 28 && code <= 30))
            return true;
    }
    return false;
}

std::string GCode::curved_level_marker(const Print &print, double z)
{
    std::string gcode;
    char        buf[80];
    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    sprintf(buf, print.is_BBL_printer() ? "; Z_HEIGHT: %g\n" : ";Z:%g\n", z);
    gcode += buf;
    sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), m_last_height);
    gcode += buf;
    auto macro = [&](const char *key, const std::string &templ, int layer_num) {
        if (templ.empty())
            return std::string();
        DynamicConfig config;
        config.set_key_value("layer_num",   new ConfigOptionInt(layer_num));
        config.set_key_value("layer_z",     new ConfigOptionFloat(z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        std::string out = this->placeholder_parser_process(key, templ, m_writer.filament()->id(), &config) + "\n";
        return macro_moves_head(out) ? std::string() : out;
    };
    gcode += macro("before_layer_change_gcode", print.config().before_layer_change_gcode.value, m_layer_index + 1);
    gcode += m_writer.update_progress(++ m_layer_index, m_layer_count);
    gcode += macro("layer_change_gcode", print.config().layer_change_gcode.value, m_layer_index);
    return gcode;
}

std::string GCode::extrude_field_patch(const Nonplanar::ConformalPatch &patch)
{
    using namespace Nonplanar;
    std::string             gcode;
    const FieldChart       &field = *patch.field;
    const double            hop   = std::max(this->nonplanar_head().margin,
                                             m_writer.filament() != nullptr ? m_config.z_hop.get_at(m_writer.filament()->id()) : 0.);
    const double z_base = m_layer->slice_z + 0.5 * m_layer->height;
    const double host_z = m_layer->print_z;
    auto to_writer = [&](const Vec3d &p) {
        const Vec2d xy = this->point_to_gcode(Point::new_scale(p.x(), p.y()));
        return Vec3d(xy.x(), xy.y(), m_nominal_z + p.z() - z_base);
    };
    auto to_field = [&](const Point3 &p) { return Vec3d(unscale<double>(p.x()), unscale<double>(p.y()), unscale<double>(p.z()) + z_base); };
    m_wipe.reset_path();
    bool  entered = false;
    Vec3d last    = Vec3d::Zero();
    const auto *rotary = dynamic_cast<const RotaryKinematics *>(&m_writer.tool_kinematics());
    const bool  settle = rotary != nullptr && rotary->layout() != RotaryLayout::HeadAB;
    double      plane  = 0.;
    if (settle) {
        JointPose tilted = m_writer.get_joints();
        tilted.rot[0]    = rotary->machine_angle(0, Geometry::deg2rad(10.));
        const Vec3d a    = rotary->forward(tilted).axis;
        plane            = std::atan2(a.y(), a.x());
    }
    auto settled = [&](const Vec3d &axis) { return settle ? settle_axis(axis, plane) : axis; };
    auto pin_crossings = [](auto &poses, auto axis_of) {
        for (size_t i = 1; i < poses.size(); ++i) {
            Vec3d &a = axis_of(poses[i - 1]), &b = axis_of(poses[i]);
            if (a.head<2>().dot(b.head<2>()) < 0.)
                (a.z() > b.z() ? a : b) = Vec3d::UnitZ();
        }
    };
    const Vec3d xy_offset(m_writer.get_xy_offset().x(), m_writer.get_xy_offset().y(), 0.);
    auto chains = [&](JointPose from, const std::vector<ToolPose> &poses, size_t mark) {
        const RotaryAxis &c = rotary->rotary_axes()[1];
        for (size_t i = 0; i < poses.size(); ++i) {
            JointPose next;
            if (! rotary->inverse(ToolPose{ poses[i].tip - xy_offset, poses[i].axis }, &from, next) || std::abs(next.rot[1] - from.rot[1]) > 90.)
                return false;
            if (! c.continuous && (next.rot[1] < c.min_deg + (i == mark ? 45. : 1.) || next.rot[1] > c.max_deg - (i == mark ? 45. : 1.)))
                return false;
            from = next;
        }
        return true;
    };
    auto road_seed = [&](const std::vector<ToolPose> &road, JointPose &seed) {
        const RotaryAxis &c = rotary->rotary_axes()[1];
        for (double guess : { rotary->machine_angle(1, 0.), 0.5 * (c.min_deg + c.max_deg), c.min_deg + 1., c.max_deg - 1. }) {
            JointPose g = m_writer.get_joints(), s;
            g.rot[0]    = rotary->machine_angle(0, 0.);
            g.rot[1]    = guess;
            if (rotary->inverse(ToolPose{ road.front().tip - xy_offset, road.front().axis }, &g, s) && chains(s, road, size_t(-1))) {
                seed = s;
                return true;
            }
        }
        return false;
    };
    BoundingBoxf top_box;
    for (const ExtrusionEntityCollection &shell : patch.shell_paths)
        for (const ExtrusionEntity *ee : shell.entities)
            if (const auto *path = dynamic_cast<const ExtrusionPath *>(ee))
                for (const Point3 &p : path->polyline.points)
                    top_box.merge(Vec2d(field.forward(to_field(p)).head<2>()));
    const Vec2d        top_lo = top_box.defined ? Vec2d(top_box.min - Vec2d::Constant(2.)) : Vec2d::Zero();
    const int          top_nx = top_box.defined ? int(std::ceil(top_box.size().x() + 4.)) + 1 : 1;
    const int          top_ny = top_box.defined ? int(std::ceil(top_box.size().y() + 4.)) + 1 : 1;
    std::vector<float> top(size_t(top_nx) * size_t(top_ny), -std::numeric_limits<float>::infinity());
    auto top_cell = [&](const Vec3d &q, int &x, int &y) {
        x = std::clamp(int(q.x() - top_lo.x()), 0, top_nx - 1);
        y = std::clamp(int(q.y() - top_lo.y()), 0, top_ny - 1);
    };
    auto printed = [&](const Vec3d &q) {
        int x, y;
        top_cell(q, x, y);
        for (int b = std::max(0, y - 1); b <= std::min(top_ny - 1, y + 1); ++b)
            for (int a = std::max(0, x - 1); a <= std::min(top_nx - 1, x + 1); ++a)
                top[size_t(b) * top_nx + a] = std::max(top[size_t(b) * top_nx + a], float(q.z()));
    };
    auto top_along = [&](const Vec3d &qa, const Vec3d &qb) {
        double     hi = -std::numeric_limits<double>::infinity();
        const int  n  = std::max(1, int(std::ceil((qb - qa).head<2>().norm() / 0.5)));
        for (int s = 0; s <= n; ++s) {
            int x, y;
            top_cell(qa + (qb - qa) * (double(s) / n), x, y);
            hi = std::max(hi, double(top[size_t(y) * top_nx + x]));
        }
        return hi;
    };
    const bool fixed_conformal = rotary == nullptr && patch.native && m_layer != nullptr &&
                                 (m_layer->object()->config().nonplanar_mode.value == NonplanarMode::ConformalLayers ||
                                  m_layer->object()->config().nonplanar_mode.value == NonplanarMode::Slant);
    auto field_travel = [&](const Vec3d &a, const Vec3d &b, const ToolPose &target, bool in_layer, bool upright, std::vector<ToolPose> &poses) -> bool {
        const Vec3d  qa = field.forward(a), qb = field.forward(b);
        const double ends  = std::max(qa.z(), qb.z());
        const double above = top_along(qa, qb);
        const bool   banded = fixed_conformal && patch.banded;
        bool planar_stretch = std::abs(qa.z() - a.z()) < 1e-3 && std::abs(qb.z() - b.z()) < 1e-3 && std::abs(a.z() - b.z()) < 1e-3;
        for (int s = 1; s < 8 && planar_stretch; ++s) {
            const Vec3d m = a + (b - a) * (double(s) / 8.);
            planar_stretch = std::abs(field.forward(m).z() - m.z()) < 1e-3;
        }
        if (fixed_conformal && !planar_stretch && (b - a).head<2>().norm() >= 1.) {
            if (!in_layer || (b - a).head<2>().norm() > 15. || above > ends + 0.1)
                return false;
            const double level = 0.5 * (qa.z() + qb.z());
            double       sag   = 0.;
            const int    n     = std::max(2, int(std::ceil((b - a).head<2>().norm() / 0.5)));
            for (int s = 1; s < n; ++s) {
                const Vec3d c = a + (b - a) * (double(s) / n);
                Vec3d       on;
                if (!field.inverse(Vec3d(field.forward(c).x(), field.forward(c).y(), level), on))
                    return false;
                sag = std::max(sag, on.z() - c.z());
            }
            const double hz = std::clamp(sag + 0.15, 0.15, 1.);
            poses.clear();
            poses.push_back({ to_writer(a + Vec3d(0., 0., hz)), Vec3d::UnitZ() });
            poses.push_back({ to_writer(b + Vec3d(0., 0., hz)), Vec3d::UnitZ() });
            poses.push_back(target);
            return true;
        }
        const bool   flat  = (in_layer || (qb - qa).head<2>().norm() < 3.) && above <= ends + 0.1;
        if (banded && !flat)
            return false;
        if (flat) {
            const double lo = std::min(qa.z(), qb.z());
            const int    n  = std::max(1, int(std::ceil((b - a).norm() / 0.5)));
            bool         chord = true;
            for (int s = 1; s < n && chord; ++s) {
                const double level_s = field.forward(a + (b - a) * (double(s) / n)).z();
                chord = level_s >= lo - 0.02 && level_s <= ends + 0.3;
            }
            if (chord) {
                poses.assign(1, target);
                return true;
            }
            if (banded)
                return false;
        }
        const double level = flat ? ends : std::max(ends, above) + hop;
        std::vector<Vec3d> legs { qa, Vec3d(qa.x(), qa.y(), level), Vec3d(qb.x(), qb.y(), level), qb };
        poses.clear();
        const bool travel_slerp = Nonplanar::native_axis_key("NP_NATIVE_TRAVEL_SLERP", patch.native ? 1. : 0., rotary != nullptr) != 0.;
        const Vec3d       axis_from    = m_writer.get_tool_axis().normalized();
        double            total = 0., done = 0.;
        for (size_t l = 1; l < legs.size(); ++l)
            total += (legs[l] - legs[l - 1]).norm();
        for (size_t l = 1; l < legs.size(); ++l) {
            const double len = (legs[l] - legs[l - 1]).norm();
            if (len < EPSILON)
                continue;
            const int    n   = std::max(1, int(std::ceil(len / 2.)));
            for (int s = 1; s <= n; ++s) {
                Vec3d p;
                if (! field.inverse(legs[l - 1] + (legs[l] - legs[l - 1]) * (double(s) / n), p))
                    return false;
                Vec3d axis = upright ? Vec3d::UnitZ() : settled(field.orientation(p));
                if (travel_slerp && ! upright && total > EPSILON)
                    axis = Eigen::Quaterniond::Identity().slerp((done + len * s / n) / total, Eigen::Quaterniond::FromTwoVectors(axis_from, target.axis.normalized())) * axis_from;
                poses.push_back({ to_writer(p), axis });
            }
            done += len;
        }
        pin_crossings(poses, [](ToolPose &pose) -> Vec3d & { return pose.axis; });
        if (poses.empty())
            poses.push_back(target);
        poses.back() = target;
        return true;
    };
    auto printable = [](const ExtrusionEntity *ee) {
        const auto *path = dynamic_cast<const ExtrusionPathConformal *>(ee);
        return path != nullptr && path->polyline.points.size() >= 2 && unscale<double>(path->polyline.length()) >= ConformalMinPath ? path : nullptr;
    };
    const double glide_length = std::clamp(m_writer.filament() != nullptr ? m_config.retraction_minimum_travel.get_at(m_writer.filament()->id()) : 0., 2., 3.);
    bool         glide        = false, glide_inside = false;
    const bool   infill_glide = field.given() && m_config.reduce_infill_retraction && m_config.sparse_infill_density.value > 0;
    for (size_t k = 0; k < patch.shell_paths.size(); ++k) {
        if (Nonplanar::pass_prints(patch.shell_paths[k])) {
            m_last_height = level_height(patch.shell_paths[k]);
            gcode += this->curved_level_marker(*m_print, m_nominal_z + level_top(patch.shell_paths[k]));
        }
        gcode += ";_NONPLANAR_BEGIN patch=" + std::to_string(patch.id) + " shell=" + std::to_string(k) + " band=" + std::to_string(patch.band) +
                 " rate=" + std::to_string(std::lround(this->conformal_segment_rate())) + "\n";
        const ExtrusionEntitiesPtr &entities = patch.shell_paths[k].entities;
        for (size_t e = 0; e < entities.size(); ++e) {
            const auto *path = printable(entities[e]);
            if (path == nullptr)
                continue;
            if (path->native_road_id != UINT32_MAX)
                gcode += ";_NATIVE_ROAD_ID=" + std::to_string(path->native_road_id) + "\n";
            m_conformal_points = conformal_points(*path, [this](const Point3 &p) {
                const Vec2d xy = this->point_to_gcode(p.to_point());
                return Vec3d(xy.x(), xy.y(), m_nominal_z + unscale<double>(p.z()));
            });
            const Vec3d first = to_field(path->polyline.points.front());
            double plane_at_start = plane;
            for (ConformalPoint &p : m_conformal_points)
                p.pose.axis = settled(p.pose.axis);
            pin_crossings(m_conformal_points, [](ConformalPoint &p) -> Vec3d & { return p.pose.axis; });
            const double plane_at_end = plane;
            plane = plane_at_start;
            const ToolPose start = m_conformal_points.front().pose;
            std::vector<ToolPose> road;
            for (const ConformalPoint &p : m_conformal_points)
                road.push_back(p.pose);
            JointPose seed;
            if (! entered) {
                const bool reseed = rotary != nullptr && ! chains(m_writer.get_joints(), road, 0) && road_seed(road, seed);
                gcode += this->retract();
                gcode += this->rotary_move_safely(&start, reseed ? &seed : nullptr);
                if (rotary == nullptr && (m_writer.get_position() - start.tip).norm() > EPSILON) {
                    const Vec3d  here = m_writer.get_position();
                    const double z    = std::max(here.z(), start.tip.z()) + hop;
                    gcode += m_writer.travel_to_pose({ Vec3d(here.x(), here.y(), z), Vec3d::UnitZ() }, "lift to enter the curved layers");
                    gcode += m_writer.travel_to_pose({ Vec3d(start.tip.x(), start.tip.y(), z), Vec3d::UnitZ() }, "travel to the curved layers");
                    gcode += m_writer.travel_to_pose(start, "lower onto the curved layer");
                }
                entered = true;
            } else if ((m_writer.get_position() - start.tip).norm() > EPSILON || rotary != nullptr) {
                std::vector<ToolPose> travel;
                const bool moves  = (m_writer.get_position() - start.tip).norm() > EPSILON;
                const bool keep_upright = patch.native && is_support(path->role()) && m_writer.get_tool_axis().z() > 1. - EPSILON &&
                                          start.axis.z() > 1. - EPSILON;
                const bool along  = moves && field_travel(last, first, start, glide_inside, keep_upright, travel);
                std::vector<ToolPose> chain = travel;
                chain.insert(chain.end(), road.begin(), road.end());
                const bool upright = m_material_columns && m_writer.get_tool_axis().z() > 1. - EPSILON && start.axis.z() > 1. - EPSILON;
                if (rotary == nullptr || chains(m_writer.get_joints(), chain, travel.size())) {
                    if (moves) {
                        const bool        gliding = glide && along;
                        const bool        chord   = field.given() && along && travel.size() == 1;
                        if (!gliding)
                            gcode += this->retract();
                        const std::string local = gliding || chord ? std::string() : this->rotary_local_hop(start, field.given());
                        if (!local.empty())
                            gcode += local;
                        else if (along)
                            for (size_t i = 0; i < travel.size(); ++i)
                                gcode += m_writer.travel_to_pose(travel[i], i + 1 == travel.size() ? "lower onto the curved layer" : "travel along the curved layers");
                        else if (upright) {
                            const Vec3d  here = m_writer.get_position();
                            const double z    = std::max({ here.z(), start.tip.z(),
                                                           safe_travel_z(*m_material_columns, this->nonplanar_head(), here.head<2>(), start.tip.head<2>(), hop) });
                            gcode += m_writer.travel_to_pose({ Vec3d(here.x(), here.y(), z), Vec3d::UnitZ() }, "lift over the print");
                            gcode += m_writer.travel_to_pose({ Vec3d(start.tip.x(), start.tip.y(), z), Vec3d::UnitZ() }, "travel over the print");
                            gcode += m_writer.travel_to_pose(start, "lower onto the curved layer");
                        } else
                            gcode += this->rotary_hop(&start);
                    }
                } else {
                    gcode += this->retract();
                    gcode += road_seed(road, seed) ? this->rotary_hop(&start, &seed) : this->rotary_hop(&start);
                }
            }
            plane = plane_at_end;
            this->set_last_pos(path->first_point());
            m_need_change_layer_lift_z = false;
            const ExtrusionPathConformal *next = nullptr;
            for (size_t kk = k, ee = e + 1; kk < patch.shell_paths.size() && next == nullptr; ++kk, ee = 0)
                for (; ee < patch.shell_paths[kk].entities.size() && next == nullptr; ++ee)
                    next = printable(patch.shell_paths[kk].entities[ee]);
            const bool flows_on = (field.given() || fixed_conformal) && next != nullptr &&
                                  (next->polyline.points.front() - path->polyline.points.back()).cast<double>().norm() < scaled<double>(0.01);
            m_conformal_open_end = flows_on || field.given();
            gcode += this->_extrude(*path, "orientation field");
            if (fixed_conformal)
                m_wipe.reset_path();
            m_conformal_open_end = false;
            for (const Point3 &p : path->polyline.points) {
                m_max_layer_z = std::max(m_max_layer_z, float(unscale<double>(p.z()) + host_z));
                printed(field.forward(to_field(p)));
            }
            last = to_field(path->polyline.points.back());
            glide = glide_inside = false;
            if (next != nullptr && path->role() != erExternalPerimeter) {
                const Vec3d  qa = field.forward(last), qb = field.forward(to_field(next->polyline.points.front()));
                const double ends = std::max(qa.z(), qb.z());
                const bool   same = std::abs(qb.z() - qa.z()) < 0.5 * m_layer->height + 0.3;
                glide_inside = same && infill_glide && !is_perimeter(next->role()) && k < patch.shell_internal.size() &&
                               std::any_of(patch.shell_internal[k].begin(), patch.shell_internal[k].end(), [&](const ExPolygon &region) {
                                   return region.contains(Polyline(path->last_point(), next->first_point()));
                               });
                glide = glide_inside || (same && (qb - qa).head<2>().norm() < glide_length && top_along(qa, qb) <= ends + 0.1);
            }
            glide = glide || flows_on;
            if (!glide && !fixed_conformal && (!field.given() || FILAMENT_CONFIG(wipe)))
                gcode += this->wipe_conformal_path();
            gcode += m_writer.end_inverse_time();
            m_conformal_points.clear();
            if (path->native_road_id != UINT32_MAX)
                gcode += ";_NATIVE_ROAD_ID=4294967295\n";
        }
        if (k + 1 == patch.shell_paths.size()) {
            gcode += this->retract();
            gcode += this->rotary_move_safely(nullptr);
        }
        gcode += ";_NONPLANAR_END\n";
    }
    gcode += m_writer.end_inverse_time();
    return gcode;
}

static float overhang_fan_overlap_threshold(int overhang_fan_threshold);

void GCode::emit_conformal_moves(std::string &gcode, const ExtrusionPathConformal &path, double e_per_mm, double speed,
                                 const std::string &speed_marker, const std::string &description)
{
    using namespace Nonplanar;
    if (m_conformal_points.size() != path.polyline.points.size())
        m_conformal_points = conformal_points(path, [this](const Point3 &p) {
            const Vec2d xy = this->point_to_gcode(p.to_point());
            return Vec3d(xy.x(), xy.y(), m_nominal_z + unscale<double>(p.z()));
        });
    snap_upright_roads(m_writer.tool_kinematics(), m_conformal_points);
    this->clamp_table_tilt(m_conformal_points);

    const double first_layer_z = m_config.initial_layer_print_height.value + m_config.z_offset.value;
    for (const ConformalPoint &pt : m_conformal_points)
        if (pt.pose.tip.z() < first_layer_z - ConformalBelowFirstLayerTolerance)
            throw Slic3r::SlicingError(
                format("Non-planar: refusing to emit a conformal extrusion (%1%) at Z = %2$.3f mm, below the first layer at Z = %3$.3f mm. "
                       "This is a toolpath bug; slice without non-planar overhangs or shells and report it.",
                       description, pt.pose.tip.z(), first_layer_z),
                m_layer != nullptr && m_layer->object() != nullptr ? m_layer->object()->id().id : 0);

    const bool road_cooling = m_config.nonplanar_road_cooling.value && m_writer.filament() != nullptr;
    if (road_cooling) {
        if (path.role() == erTopSolidInfill && NOZZLE_CONFIG(top_surface_speed) > 0.)
            speed = std::min(speed, double(NOZZLE_CONFIG(top_surface_speed)));
        if (path.role() != erIroning && unscale<double>(path.polyline.length()) < SmallRoadLength) {
            const double outer = NOZZLE_CONFIG(outer_wall_speed);
            const double small = NOZZLE_CONFIG(small_perimeter_speed).value == 0 ? 0.5 * outer :
                                                                                     NOZZLE_CONFIG(small_perimeter_speed).get_abs_value(outer);
            if (small > 0.)
                speed = std::min(speed, small);
        }
    }

    if (is_perimeter(path.role()) && !is_bridge(path.role()) && path.polyline.points.front() == path.polyline.points.back() &&
        path.polyline.length() <= SMALL_PERIMETER_LENGTH(NOZZLE_CONFIG(small_perimeter_threshold)))
        speed = NOZZLE_CONFIG(small_perimeter_speed).value == 0 ? 0.5 * NOZZLE_CONFIG(outer_wall_speed) :
                                                                  NOZZLE_CONFIG(small_perimeter_speed).get_abs_value(NOZZLE_CONFIG(outer_wall_speed));

    const bool   curved_top  = path.role() == erTopSolidInfill && path.patch_id >= 0;
    const double speed_scale = curved_top ? 0.01 * m_config.nonplanar_top_speed_factor.value : 1.;
    if (speed_scale > 1.)
        speed *= speed_scale;
    if (const std::vector<double> &tilt_flow = m_config.nonplanar_tilt_flow.values;
        curved_top && m_conformal_surface != nullptr && m_conformal_points.size() == path.polyline.points.size() &&
        std::any_of(tilt_flow.begin(), tilt_flow.end(), [](double f) { return f != 1.; }))
        for (size_t i = 0; i < m_conformal_points.size(); ++i) {
            const Vec2d g = m_conformal_surface->gradient(unscale(path.polyline.points[i].to_point()));
            if (std::isnan(g.x()) || std::isnan(g.y()))
                continue;
            const double t = std::clamp(Geometry::rad2deg(std::atan(g.norm())) / 10., 0., double(tilt_flow.size() - 1));
            const size_t k = std::min(size_t(t), tilt_flow.size() - 1), k1 = std::min(k + 1, tilt_flow.size() - 1);
            const float  f = float(tilt_flow[k] + (t - double(k)) * (tilt_flow[k1] - tilt_flow[k]));
            m_conformal_points[i].bead_height *= f;
            m_conformal_points[i].vertical_height *= f;
        }

    if (m_material_columns && m_writer.tool_kinematics().rotary_count() == 0) {
        const auto top_at = [this](const Vec3d &p) {
            return m_material_columns->top_in_rect(BoundingBoxf(p.head<2>() - Vec2d(0.1, 0.1), p.head<2>() + Vec2d(0.1, 0.1)));
        };
        std::vector<ConformalPoint> lifted;
        bool                        any = false;
        for (size_t i = 0; i + 1 < m_conformal_points.size(); ++i) {
            const ConformalPoint &a = m_conformal_points[i], &b = m_conformal_points[i + 1];
            lifted.emplace_back(a);
            if (a.bead_height > 0.f && b.bead_height > 0.f)
                continue;
            const int                   n = std::max(1, int(std::ceil((b.pose.tip - a.pose.tip).head<2>().norm() / 0.25)));
            std::vector<ConformalPoint> run;
            bool                        rises = false;
            for (int k = 0; k <= n; ++k) {
                ConformalPoint p  = interpolate(a, b, double(k) / n);
                p.bead_height     = 0.f;
                p.vertical_height = 0.f;
                if (const double z = top_at(p.pose.tip) + GlideLift; z > p.pose.tip.z() + EPSILON)
                    p.pose.tip.z() = z, rises = true;
                run.emplace_back(p);
            }
            if (!rises)
                continue;
            any = true;
            ConformalPoint up = run.front(), down = run.back();
            up.pose.tip.z()   = std::max(a.pose.tip.z(), up.pose.tip.z());
            down.pose.tip.z() = std::max(b.pose.tip.z(), down.pose.tip.z());
            lifted.emplace_back(up);
            lifted.insert(lifted.end(), run.begin() + 1, run.end() - 1);
            lifted.emplace_back(down);
        }
        if (any) {
            lifted.emplace_back(m_conformal_points.back());
            m_conformal_points = std::move(lifted);
        }
    }

    ConformalEmitParams params;
    params.margin    = this->nonplanar_head().margin;
    params.clearance = this->nozzle_clearance();
    params.nominal_speed = speed;
    params.speed_scale   = std::min(1., speed_scale);
    params.ramp_length   = m_conformal_ramp ? StartRamp : 0.;
    params.taper_length  = EndTaper;
    params.coast_length  = EndCoast;
    params.open_end      = m_conformal_open_end;
    params.scored        = m_conformal_scored;
    if (m_conformal_pa_tracker && m_conformal_pa_tracker->along_road() && path.role() != erIroning)
        params.pressure_advance = m_conformal_pa_tracker.get();
    if (m_writer.filament() != nullptr)
        params.glide_speed = m_config.travel_speed.get_at(get_nozzle_config_index(m_writer.filament()->id()));
    params.e_per_mm3     = path.mm3_per_mm > 0. ? e_per_mm / path.mm3_per_mm : 0.;
    const NonplanarMode mode = m_config.nonplanar_mode.value;
    const bool patch_top = path.role() == erTopSolidInfill && (path.patch_id >= 0 ||
        (m_layer != nullptr && Nonplanar::wave_enabled(*m_layer->object())));
    if (patch_top || (path.role() == erTopSolidInfill &&
                      (mode == NonplanarMode::SmoothMorph || mode == NonplanarMode::ConformalLayers || mode == NonplanarMode::Slant)))
        params.e_per_mm3 *= m_config.nonplanar_top_finish_flow.value;
    if (patch_top) {
        if (m_conformal_surface != nullptr && !m_config.nonplanar_flow_mod.value) {
            double slope = 0.;
            size_t n     = 0;
            for (size_t i = 0; i < path.polyline.points.size(); ++i)
                if (path.bead_height[i] > 0.f)
                    if (const Vec2d g = m_conformal_surface->gradient(unscale(path.polyline.points[i].to_point()));
                        !std::isnan(g.x()) && !std::isnan(g.y())) {
                        slope += std::atan(g.norm());
                        ++n;
                    }
            if (n > 0)
                params.nominal_speed *= std::clamp(std::cos(slope / double(n)) / std::cos(Geometry::deg2rad(25.)), 0.7, 1.);
        }
    }
    const double diameter = FILAMENT_CONFIG(filament_diameter);
    const double volume_ratio = params.e_per_mm3 * PI * diameter * diameter * 0.25;
    if (volume_ratio > 0.)
        params.max_volume_per_second = FILAMENT_CONFIG(filament_max_volumetric_speed) / volume_ratio;
    params.comment       = description;
    params.speed_marker  = speed_marker;
    params.last_width    = &m_last_width;
    auto first_value     = [](const ConfigOptionFloats &opt) { return opt.values.empty() ? 0. : opt.values.front(); };
    auto lower_limit     = [](double a, double b) { return a > 0. && b > 0. ? std::min(a, b) : std::max(a, b); };
    const GCodeFlavor flavor  = m_config.gcode_flavor.value;
    const bool        emitted = flavor == gcfKlipper || (m_config.emit_machine_limits_to_gcode.value &&
                         (flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware || flavor == gcfRepRapFirmware));
    auto z_value         = [&lower_limit, &first_value, emitted](const ConfigOptionFloats &opt) {
        if (emitted)
            return first_value(opt);
        double v = 0.;
        for (double x : opt.values)
            v = lower_limit(v, x);
        return v;
    };
    params.limits.max_v_xy = lower_limit(first_value(m_config.machine_max_speed_x), first_value(m_config.machine_max_speed_y));
    params.limits.max_a_xy = lower_limit(first_value(m_config.machine_max_acceleration_x), first_value(m_config.machine_max_acceleration_y));
    params.limits.max_v_z  = (emitted ? 0.99 : 0.8) * z_value(m_config.machine_max_speed_z);
    params.limits.max_a_z  = z_value(m_config.machine_max_acceleration_z);
    params.limits.max_jerk_xy = lower_limit(first_value(m_config.machine_max_jerk_x), first_value(m_config.machine_max_jerk_y));
    params.limits.max_jerk_z  = z_value(m_config.machine_max_jerk_z);
    if (flavor == gcfKlipper) {
        if (params.limits.max_a_xy > 0.)
            params.limits.junction_deviation = sqr(params.limits.max_jerk_xy) * (std::sqrt(2.) - 1.) / params.limits.max_a_xy;
    } else if (flavor == gcfMarlinFirmware)
        params.limits.junction_deviation = first_value(m_config.machine_max_junction_deviation);
    if (m_writer.tool_kinematics().rotary_count() > 0)
        params.limits = MotionLimits();
    params.limits.segment_rate = this->conformal_segment_rate();
    params.condition.target_length = std::max(0.15, params.nominal_speed / params.limits.segment_rate);
    if (Nonplanar::full_orientation(m_config) && path.role() != erExternalPerimeter && path.role() != erOverhangPerimeter)
        params.limits.segment_rate = 0.;
    if (path.preserve_points) {
        params.condition.target_length = 0.;
        params.preserve_points = true;
        params.ramp_length = params.taper_length = params.coast_length = 0.;
    } else if ((flavor == gcfMarlinFirmware || (m_print != nullptr && m_print->is_BBL_printer())) && !this->should_disable_arc_fitting() &&
               !m_writer.has_axis_remap()) {
        const double resolution = m_config.resolution.value;
        params.arc_tolerance    = resolution > 0. ? std::min(resolution, params.condition.tolerance) : params.condition.tolerance;
    }
    if (m_config.nonplanar_flow_mod.value) {
        ConformalSpeedParams caps;
        caps.nominal_speed    = speed;
        caps.max_volumetric   = FILAMENT_CONFIG(filament_max_volumetric_speed);
        const bool hidden     = path.role() == erSolidInfill || path.role() == erInternalInfill;
        caps.reference_height = m_conformal_gradient && !hidden ? path.height : 0.;
        if (road_cooling)
            caps.min_speed = FILAMENT_CONFIG(slow_down_min_speed);
        caps.max_joint_speed  = Vec3d(first_value(m_config.machine_max_speed_x), first_value(m_config.machine_max_speed_y),
                                      first_value(m_config.machine_max_speed_z));
        const Vec2f  offset   = m_writer.get_xy_offset();
        const Vec3d  shift(offset.x(), offset.y(), 0.);
        params.speed_caps     = [this, &path, caps, shift](const std::vector<ConformalPoint> &points) {
            return conformal_speed_caps(points, path.width, m_conformal_gradient, m_writer.tool_kinematics(), m_writer.get_joints(), shift, caps);
        };
    }

    const ExtrusionRole role          = path.role();
    const bool          overhang_road = m_conformal_may_hang && (is_perimeter(role) || is_bridge(role)) && role != erInternalBridgeInfill &&
                                        !m_np_support.empty();
    if (overhang_road && NOZZLE_CONFIG(enable_overhang_speed) && role != erOverhangPerimeter && role != erBridgeInfill) {
        const double mm3_per_mm = e_per_mm * FILAMENT_CONFIG(filament_flow_ratio) / m_writer.filament()->e_per_mm3();
        double       ref_speed  = is_external_perimeter(role) ? NOZZLE_CONFIG(outer_wall_speed) : NOZZLE_CONFIG(inner_wall_speed);
        if (ref_speed == 0)
            ref_speed = FILAMENT_CONFIG(filament_max_volumetric_speed) / mm3_per_mm;
        if (FILAMENT_CONFIG(filament_max_volumetric_speed) > 0)
            ref_speed = std::min(ref_speed, FILAMENT_CONFIG(filament_max_volumetric_speed) / mm3_per_mm);
        const OverhangSpeedSections sections(OverhangSpeedSections::overlap_levels(), this->overhang_speed_table(ref_speed),
                                             float(ref_speed), path.width);
        params.speed_caps = [this, &path, sections, original = float(speed), caps = std::move(params.speed_caps)](
                                const std::vector<ConformalPoint> &points) {
            std::vector<double>      out  = caps ? caps(points) :
                                                   std::vector<double>(points.size() < 2 ? 0 : points.size() - 1, std::numeric_limits<double>::infinity());
            const std::vector<float> hang = road_hang(points, path.width, m_np_support, m_conformal_gradient, RoadFanParams().lateral_samples);
            for (size_t i = 0; i < out.size(); ++i)
                if (hang[i] > 0.f)
                    out[i] = std::min(out[i], double(sections.speed(hang[i] * path.width, original)));
            return out;
        };
    }
    double overhang_from = 2.;
    if (overhang_road && FILAMENT_CONFIG(enable_overhang_bridge_fan) && role != erBridgeInfill && role != erOverhangPerimeter)
        if (const float overlap = overhang_fan_overlap_threshold(FILAMENT_CONFIG(overhang_fan_threshold)); overlap >= 0.f)
            overhang_from = 1. - overlap;

    const bool role_fan = (role == erInternalBridgeInfill && FILAMENT_CONFIG(enable_overhang_bridge_fan) &&
                           FILAMENT_CONFIG(internal_bridge_fan_speed) >= 0) ||
                          (role == erSupportMaterialInterface && FILAMENT_CONFIG(support_material_interface_fan_speed) >= 0);
    if ((road_cooling || overhang_from <= 1.) && m_enable_cooling_markers && path.role() != erIroning)
        params.segment_tags = [this, &path, curved_top, road_cooling, overhang_from, role_fan](const std::vector<ConformalPoint> &points,
                                                                                               const std::vector<double>         &speeds) {
            RoadFanParams fan;
            fan.overhang_from = overhang_from;
            if (!road_cooling)
                fan.hang_weight = fan.downhill_weight = 0.;
            else if (road_material(m_config.filament_type.get_at(m_writer.filament()->id())) == RoadMaterial::Enclosure)
                fan.downhill_weight = 0.;
            std::vector<float> demand = road_fan_demand(points, speeds, path.width, m_np_support, m_conformal_gradient, fan);
            if (path.role() == erInternalInfill || role_fan)
                std::fill(demand.begin(), demand.end(), 0.f);
            if (road_cooling && curved_top && m_config.nonplanar_top_fan_boost.value > 0.) {
                const float floor = float(std::round(0.01 * m_config.nonplanar_top_fan_boost.value / fan.step) * fan.step);
                for (float &u : demand)
                    u = std::max(u, std::min(floor, 1.f));
            }
            return road_fan_tags(demand, m_np_fan_level);
        };
    params.prime         = m_conformal_prime;
    params.prime_length  = ConformalPrime;
    if (m_writer.filament() != nullptr) {
        const double max_e = first_value(m_config.machine_max_speed_e);
        params.prime_speed = m_writer.filament()->deretract_speed();
        if (max_e > 0. && params.prime_speed > max_e)
            params.prime_speed = max_e;
    }
    m_conformal_prime    = 0.;

    const double clock_speed = std::max(1., speed);
    emit_conformal_extrusion(m_writer, m_conformal_points, path.width, params, m_last_height, gcode,
                             [this, clock_speed](const Vec3d &a, const Vec3d &b, double width) {
                                 m_nonplanar_ceiling = std::max({ m_nonplanar_ceiling, a.z(), b.z() });
                                 if (m_material_columns) m_material_columns->deposit(a, b, width);
                                 m_print_clock += (b - a).norm() / clock_speed;
                                 m_fresh_beads.add(a, b, width, m_print_clock);
                             });
}

double GCode::tower_layer_z() const
{
    const double z = m_writer.get_position().z();
    return m_conformal_lift && z > m_nominal_z + EPSILON ? m_nominal_z : z;
}

void GCode::nonplanar_pass_start(const ExPolygons &footprint, bool ironing)
{
    using namespace Nonplanar;
    if (m_writer.filament() == nullptr || ironing)
        return;
    m_np_support = SupportSnapshot();
    const bool cooling  = m_config.nonplanar_road_cooling.value && m_enable_cooling_markers;
    const bool overhang = NOZZLE_CONFIG(enable_overhang_speed) || (FILAMENT_CONFIG(enable_overhang_bridge_fan) && m_enable_cooling_markers);
    if ((cooling || overhang) && m_material_columns && !footprint.empty()) {
        const BoundingBox bbox = get_extents(footprint);
        BoundingBoxf      area(this->point_to_gcode(bbox.min), this->point_to_gcode(bbox.max));
        area.min.array() -= 1.;
        area.max.array() += 1.;
        m_np_support = SupportSnapshot(*m_material_columns, area);
    }
}

std::string GCode::nonplanar_pass_hold(const ExtrusionPathConformal *next)
{
    using namespace Nonplanar;
    if (!m_config.nonplanar_road_cooling.value || !m_enable_cooling_markers || m_writer.filament() == nullptr ||
        m_writer.filament()->retracted() <= 0. || m_writer.tool_kinematics().rotary_count() > 0)
        return {};
    const Vec3d &here = m_writer.get_position();
    const Vec2d  xy   = here.head<2>();
    if (next != nullptr && !next->polyline.points.empty()) {
        const Point3 &p = next->polyline.points.front();
        const Vec2d   s = this->point_to_gcode(p.to_point());
        if ((s - xy).norm() < 0.01 && std::abs(m_nominal_z + unscale<double>(p.z()) - here.z()) < 0.01)
            return {};
    }
    double       z    = std::max(here.z(), this->nonplanar_travel_z(&xy, &xy)) + 1.;
    if (m_config.printable_height.value > 0.)
        z = std::min(z, m_config.printable_height.value + m_config.z_offset.value);
    if (z <= here.z() + EPSILON)
        return {};
    double speed = this->conformal_max_speed_z();
    if (speed <= 0.)
        speed = m_config.travel_speed_z.get_at(get_nozzle_config_index(m_writer.filament()->id()));
    if (speed <= 0.)
        speed = 10.;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s Z%.3f F%.0f\n", NpPassHoldTag, z, 60. * speed);
    m_np_hold_z = z;
    m_writer.get_position().z() = z;
    return buf;
}

std::string GCode::nonplanar_patch_end()
{
    std::string gcode;
    m_np_hold_z = 0.;
    m_np_support = Nonplanar::SupportSnapshot();
    if (m_np_fan_level > 0.f) {
        gcode += Nonplanar::NpFanEndTag;
        gcode += "\n";
    }
    m_np_fan_level = 0.f;
    return gcode;
}

double GCode::conformal_segment_rate() const
{
    return Nonplanar::planner_segment_rate(m_config.nonplanar_segment_rate.value, m_config.gcode_flavor.value,
                                           m_print != nullptr && m_print->is_BBL_printer());
}

void GCode::ensure_material_columns(const ExtrusionPath &path)
{
    if (!m_material_columns && (m_track_nonplanar_material ||
                               dynamic_cast<const ExtrusionPathConformal *>(&path) != nullptr)) {
        BoundingBoxf bounds;
        for (const Vec2d &p : m_config.printable_area.values) bounds.merge(p);
        for (const auto &p : path.polyline.points) bounds.merge(this->point_to_gcode(p.to_point()));
        bounds.min.array() -= this->nonplanar_head().reach() + 10.;
        bounds.max.array() += this->nonplanar_head().reach() + 10.;
        m_material_columns = std::make_unique<MaterialColumns>(bounds, 0.25);
    }
}

std::function<double(const Vec3d &, const Vec3d &)> GCode::nozzle_clearance()
{
    if (!m_material_columns || m_writer.tool_kinematics().rotary_count() > 0)
        return {};
    Nonplanar::HeadEnvelope nozzle = this->nonplanar_head();
    nozzle.boxes.erase(std::remove_if(nozzle.boxes.begin(), nozzle.boxes.end(), [](const Nonplanar::HeadBox &b) { return !b.nozzle; }),
                       nozzle.boxes.end());
    if (nozzle.boxes.empty())
        nozzle = Nonplanar::nozzle_land(this->nonplanar_head());
    return [this, prepared = MaterialColumns::prepare(nozzle), bound = this->nonplanar_head().margin + 1.](const Vec3d &tip, const Vec3d &) {
        return m_material_columns->swept_clearance(tip, tip, prepared, bound);
    };
}

double GCode::conformal_clearance(bool lift)
{
    return this->nonplanar_repair_lift() +
           std::max(0.1, lift && m_writer.filament() != nullptr ? m_config.z_hop.get_at(m_writer.filament()->id()) : 0.);
}

bool GCode::conformal_lift_enforced(ExtrusionRole role) const
{
    if (m_writer.filament() == nullptr)
        return true;
    const unsigned int extruder = m_writer.filament()->extruder_id();
    const double       z        = m_writer.get_position().z();
    const double       above = m_config.retract_lift_above.get_at(extruder), below = m_config.retract_lift_below.get_at(extruder);
    if (z < above || (below > 0. && z > below))
        return false;
    switch (RetractLiftEnforceType(m_config.retract_lift_enforce.get_at(extruder))) {
    case RetractLiftEnforceType::rletAllSurfaces: return true;
    case RetractLiftEnforceType::rletTopOnly:
    case RetractLiftEnforceType::rletTopAndBottom: return role == erTopSolidInfill || role == erIroning;
    default: return false;
    }
}

bool GCode::conformal_keep_primed(ExtrusionRole from, const ExtrusionPathConformal *next, const ExtrusionPathConformal *prev)
{
    if (next == nullptr || !m_material_columns || m_writer.filament() == nullptr || next->polyline.points.size() < 2 ||
        !next->tool_axis.empty())
        return false;
    const bool in_band = this->conformal_in_band(prev, next);
    const bool skipped_shell = in_band && next->shell > prev->shell + 1 && !m_conformal_next_hold;
    if (next->role() == erTopSolidInfill && from != erTopSolidInfill && !skipped_shell)
        return false;
    const Vec2d  xy   = this->point_to_gcode(next->first_point());
    const Vec3d &here = m_writer.get_position();
    const Vec3d  to(xy.x(), xy.y(), m_nominal_z + unscale<double>(next->polyline.points.front().z()));
    const double d = (to - here).head<2>().norm();
    if (d < m_config.retraction_minimum_travel.get_at(m_writer.filament()->id()))
        return true;
    const bool   flexible    = Nonplanar::road_material(m_config.filament_type.get_at(m_writer.filament()->id())) ==
                               Nonplanar::RoadMaterial::Flexible;
    const double short_glide = flexible ? 10. : 5.;
    if (in_band && d < short_glide && (next->shell != prev->shell || unscale<double>(next->polyline.length()) < short_glide))
        return true;
    auto buried = [](ExtrusionRole role) { return role == erSolidInfill || role == erInternalInfill; };
    const bool internal = m_config.reduce_infill_retraction.value && m_config.sparse_infill_density.value > 0 && buried(from) &&
                          buried(next->role());
    Vec2d via;
    return (internal || d < 2.) && Nonplanar::material_route(*m_material_columns, here, to, 1., via);
}

double GCode::nonplanar_repair_lift() const
{
    double lift = 0.;
    if (m_print != nullptr)
        for (const PrintObject *object : m_print->objects())
            if (const Nonplanar::RepairHints *hints = object->nonplanar_repair())
                lift = std::max(lift, hints->travel_lift);
    return lift;
}

std::string GCode::conformal_retract()
{
    if (m_writer.filament() == nullptr)
        return {};
    std::string gcode = m_writer.retract(false, m_conformal_retract);
    gcode += m_writer.reset_e();
    return gcode;
}

std::string GCode::surface_travel(const Vec3d &to, bool from_on_bead, bool to_on_bead, bool in_band, const std::string &comment,
                                  double clearance, const Vec2d &via)
{
    using namespace Nonplanar;
    const HeadEnvelope  &head = this->nonplanar_head();
    SurfaceTravelParams params;
    params.nozzle = params.body = head;
    auto keep = [](HeadEnvelope &env, bool nozzle) {
        env.boxes.erase(std::remove_if(env.boxes.begin(), env.boxes.end(), [nozzle](const HeadBox &b) { return b.nozzle != nozzle; }),
                        env.boxes.end());
    };
    keep(params.nozzle, true);
    keep(params.body, false);
    if (params.nozzle.boxes.empty())
        params.nozzle = nozzle_land(head);
    params.land           = nozzle_land(head);
    params.clearance      = clearance < 0. ? this->conformal_clearance() : clearance;
    params.body_clearance = in_band ? 0. : this->nonplanar_hop();
    double max_z = 0.;
    for (double v : m_config.machine_max_speed_z.values)
        max_z = max_z > 0. && v > 0. ? std::min(max_z, v) : std::max(max_z, v);
    const double travel_speed = m_config.travel_speed.get_at(m_writer.filament() != nullptr ? get_nozzle_config_index(m_writer.filament()->id()) : 0);
    if (max_z > 0. && travel_speed > 0.)
        params.max_grade = std::clamp(0.8 * max_z / travel_speed, 0.05, 0.12);
    params.ceiling        = m_config.printable_height.value > 0. ? m_config.printable_height.value + m_config.z_offset.value : 0.;
    params.bed            = BoundingBoxf(m_config.printable_area.values);
    params.floor          = m_nominal_z + params.clearance;
    if (!in_band && m_print != nullptr && m_print->has_wipe_tower())
        params.min_z = this->nonplanar_travel_z();
    m_fresh_beads.prune(m_print_clock, params.wet_age);
    params.fresh = &m_fresh_beads;
    params.via   = via;
    params.now   = m_print_clock;
    const Vec3d         from   = m_writer.get_position();
    const SurfaceTravel travel = plan_surface_travel(*m_material_columns, from, to, from_on_bead, to_on_bead, params);
    std::string  gcode;
    if (m_need_change_layer_lift_z) {
        gcode += m_writer.travel_to_z(from.z(), "layer change", true, this->conformal_max_speed_z());
        m_need_change_layer_lift_z = false;
    }
    Vec3d        last   = from;
    const double vz_max = this->conformal_max_speed_z();
    const double rate   = this->conformal_segment_rate();
    for (size_t i = 0; i < travel.points.size(); ++i) {
        const Vec3d &p = travel.points[i];
        if (i + 1 < travel.points.size()) {
            const Vec3d  c = travel.points[i + 1] - last;
            const double t = c.squaredNorm() > 0. ? std::clamp((p - last).dot(c) / c.squaredNorm(), 0., 1.) : 0.;
            if ((last + t * c - p).norm() < 0.01)
                continue;
        }
        auto        printed = [](const Vec3d &q) { return Vec3d((q * 1000.).array().round() / 1000.); };
        const Vec3d d = printed(p) - printed(last);
        double      v = travel_speed;
        if (vz_max > 0. && std::abs(d.z()) > EPSILON)
            v = std::min(v, 0.99 * vz_max * d.norm() / std::abs(d.z()));
        if (m_np_hold_z > 0. && vz_max > 0.) {
            const Vec3d dh = printed(p) - printed(Vec3d(last.x(), last.y(), m_np_hold_z));
            if (std::abs(dh.z()) > EPSILON)
                v = std::min(v, 0.99 * vz_max * dh.norm() / std::abs(dh.z()));
            m_np_hold_z = 0.;
        }
        if (rate > 0.)
            v = std::min(v, rate * d.norm());
        gcode += m_writer.travel_to_pose({ p, Vec3d::UnitZ() }, i + 1 == travel.points.size() ? comment : "travel over the surface", v);
        m_print_clock += d.norm() / std::max(1., v);
        last = p;
    }
    m_conformal_on_bead = false;
    return gcode;
}

static void extrude_contoured(GCodeWriter &writer, std::string &gcode, const Vec3d &to, double e, const std::string &comment, double vz_max,
                              double nominal, bool defer_cap)
{
    if (vz_max <= 0.) {
        writer.extrude_to_xyz(gcode, to, e, comment);
        return;
    }
    auto         printed = [](const Vec3d &q) { return Vec3d((q * 1000.).array().round() / 1000.); };
    const Vec3d  d       = printed(to) - printed(writer.get_position());
    const double dz      = std::abs(d.z());
    double       F       = nominal;
    if (dz > EPSILON) {
        const double cap = 0.95 * vz_max * 60. * d.norm() / dz;
        if (cap < nominal)
            F = nominal * std::pow(0.95, std::ceil(std::log(cap / nominal) / std::log(0.95)));
    }
    if (defer_cap) {
        if (F < nominal)
            gcode += std::string(CoolingBuffer::ZaaMaxFeedTag) + std::to_string(std::floor(F)) + "\n";
    } else if (std::abs(F - writer.get_current_speed()) > EPSILON)
        writer.set_speed(gcode, F);
    writer.extrude_to_xyz(gcode, to, e, comment);
}

static bool scales_contoured_flow(const ExtrusionPath &path, const Layer *layer)
{
    return path.role() != erIroning && (layer == nullptr || path.height >= layer->height - EPSILON);
}

static void extrude_contoured_road(GCodeWriter &writer, std::string &gcode, const Vec3d &from, const Vec3d &to, double nominal_z,
                                   double dE, double width, double height, double base, bool scale_flow, const std::string &comment,
                                   double vz_max, double nominal_feed, bool defer_cap)
{
    const double dz      = std::abs(to.z() - from.z());
    const double nominal = Nonplanar::bead_area(width, height);
    const int    n       = scale_flow && dz > 0.25 * height ?
                               std::clamp(int(std::ceil(dz / (0.25 * height) - EPSILON)), 1, std::max(1, int((to - from).head<2>().norm() / 0.1))) : 1;
    for (int i = 1; i <= n; ++i) {
        const Vec3d  a     = from + (to - from) * (double(i - 1) / n);
        const Vec3d  b     = i == n ? to : from + (to - from) * (double(i) / n);
        const double ratio = scale_flow && nominal > 0. ? std::max(0., Nonplanar::bead_area(width, height + (1. - base) * 0.5 * (a.z() + b.z()))) / nominal : 1.;
        const double z     = nominal_z + b.z();
        if (z < 0.1)
            throw RuntimeError("GCode: very low z");
        extrude_contoured(writer, gcode, Vec3d(b.x(), b.y(), z), dE / n * ratio, comment, vz_max, nominal_feed, defer_cap);
    }
}

static ExtrusionPaths absolute_contoured(const ExtrusionPath &path, double layer_z, bool scale_flow)
{
    ExtrusionPaths out;
    const Points3 &pts = path.polyline.points;
    const double   height = path.height, nominal = Nonplanar::bead_area(path.width, height);
    auto           at = [&](const Vec3d &q) {
        return Point3(coord_t(std::lround(q.x())), coord_t(std::lround(q.y())), scaled<coord_t>(layer_z) + coord_t(std::lround(q.z())));
    };
    for (size_t k = 0; k + 1 < pts.size(); ++k) {
        const Vec3d  from = pts[k].cast<double>(), to = pts[k + 1].cast<double>();
        const double dz   = std::abs(unscale<double>(to.z() - from.z()));
        const int    n    = scale_flow && dz > 0.25 * height ?
                                std::clamp(int(std::ceil(dz / (0.25 * height) - EPSILON)), 1, std::max(1, int(unscale<double>((to - from).head<2>().norm()) / 0.1))) : 1;
        for (int i = 1; i <= n; ++i) {
            const Vec3d  a     = from + (to - from) * (double(i - 1) / n);
            const Vec3d  b     = i == n ? to : from + (to - from) * (double(i) / n);
            const double ratio = scale_flow && nominal > 0. ?
                std::max(0., Nonplanar::bead_area(path.width, height + (1. - path.contour_base) * 0.5 * unscale<double>(a.z() + b.z()))) / nominal : 1.;
            ExtrusionPath piece(path);
            piece.polyline.points = { at(a), at(b) };
            piece.mm3_per_mm      = path.mm3_per_mm * ratio;
            piece.z_contoured     = false;
            out.emplace_back(std::move(piece));
        }
    }
    return out;
}

double GCode::conformal_max_speed_z() const
{
    return m_writer.max_speed_z() > 0. ? m_writer.max_speed_z() :
           m_config.machine_max_speed_z.values.empty() ? 0. : m_config.machine_max_speed_z.values.front();
}

double GCode::nonplanar_hop()
{
    return this->nonplanar_repair_lift() + std::max(this->nonplanar_head().margin,
                           m_writer.filament() != nullptr ? m_config.z_hop.get_at(m_writer.filament()->id()) : 0.);
}

double GCode::nonplanar_travel_z(const Vec2d *from, const Vec2d *to, bool nozzle_only)
{
    double z;
    if (m_material_columns && from != nullptr && to != nullptr && nozzle_only) {
        Nonplanar::HeadEnvelope nozzle = this->nonplanar_head();
        nozzle.boxes.erase(std::remove_if(nozzle.boxes.begin(), nozzle.boxes.end(), [](const Nonplanar::HeadBox &b) { return !b.nozzle; }),
                           nozzle.boxes.end());
        z = safe_travel_z(*m_material_columns, nozzle, *from, *to,
                          std::max(0.1, m_writer.filament() != nullptr ? m_config.z_hop.get_at(m_writer.filament()->id()) : 0.));
    } else {
        const double hop = this->nonplanar_hop();
        z = m_material_columns && from != nullptr && to != nullptr ? safe_travel_z(*m_material_columns, this->nonplanar_head(), *from, *to, hop) :
                                                                     std::max(m_nonplanar_ceiling, double(m_max_layer_z) + m_config.z_offset.value) + hop;
    }
    return m_config.printable_height.value > 0. ? std::min(z, m_config.printable_height.value + m_config.z_offset.value) : z;
}

bool GCode::nonplanar_travel_clear(const Polyline &travel, double z, bool short_travel)
{
    if (!m_track_nonplanar_material || !m_material_columns || travel.size() < 2)
        return false;
    constexpr double tolerance = 1e-6;
    if (z + tolerance >= std::max(m_nonplanar_ceiling, double(m_max_layer_z) + m_config.z_offset.value))
        return true;
    const Nonplanar::HeadEnvelope &head = this->nonplanar_head();
    BoundingBoxf rect;
    for (const Point &p : travel.points)
        rect.merge(this->point_to_gcode(p));
    const double reach = short_travel ? 1. : head.reach();
    rect.min.array() -= reach;
    rect.max.array() += reach;
    if (m_material_columns->top_in_rect(rect) <= z + tolerance)
        return true;
    if (short_travel)
        return false;
    for (size_t i = 1; i < travel.size(); ++i)
        if (safe_travel_z(*m_material_columns, head, this->point_to_gcode(travel.points[i - 1]), this->point_to_gcode(travel.points[i]), 0.) >
            z + tolerance)
            return false;
    return true;
}

std::string GCode::wipe_conformal_path()
{
    std::string gcode;
    if (m_conformal_points.size() < 2 || m_writer.filament() == nullptr)
        return gcode;
    const Extruder &filament = *m_writer.filament();
    const bool      wipe     = FILAMENT_CONFIG(wipe);
    double run = 0., area = 0.;
    for (size_t i = m_conformal_points.size() - 1; i > 0 && run < 2.; --i) {
        const Nonplanar::ConformalPoint &a = m_conformal_points[i - 1], &b = m_conformal_points[i];
        if (a.bead_height <= 0.f || b.bead_height <= 0.f)
            break;
        const double l = (b.pose.tip - a.pose.tip).norm();
        run += l;
        area += l * 0.5 * (a.bead_height + b.bead_height);
    }
    double scale = run > EPSILON ? Nonplanar::pressure_retract_scale(area / run, m_config.layer_height.value) : 1.;
    if (Nonplanar::road_material(m_config.filament_type.get_at(filament.id())) == Nonplanar::RoadMaterial::Flexible)
        scale = std::min(scale, 1.);
    const double length = m_config.use_firmware_retraction ? 0. : scale * filament.retraction_length();
    m_conformal_retract = length;
    const double    before   = wipe ? length * filament.retract_before_wipe() : 0.;
    const double    after    = wipe ? length * filament.retract_after_wipe() : 0.;
    Nonplanar::ConformalWipeParams params;
    params.distance        = wipe ? FILAMENT_CONFIG(wipe_distance) : Nonplanar::ConformalWipe;
    params.speed           = std::max(10., m_config.get_abs_value("wipe_speed", m_config.travel_speed.get_at(get_nozzle_config_index(filament.id()))));
    auto first_value       = [](const ConfigOptionFloats &opt) { return opt.values.empty() ? 0. : opt.values.front(); };
    params.limits.max_v_xy = std::min(first_value(m_config.machine_max_speed_x), first_value(m_config.machine_max_speed_y));
    params.limits.max_v_z  = 0.99 * first_value(m_config.machine_max_speed_z);
    params.max_v_e         = first_value(m_config.machine_max_speed_e);
    params.limits.segment_rate = Nonplanar::planner_segment_rate(m_config.nonplanar_segment_rate.value, m_config.gcode_flavor.value,
                                                      m_print != nullptr && m_print->is_BBL_printer());
    params.speed_marker    = this->enable_cooling_markers() ? ";_WIPE" : "";
    params.margin    = this->nonplanar_head().margin;
    params.clearance = this->nozzle_clearance();
    if (before > EPSILON)
        gcode += m_writer.retract(true, before);
    params.retract = std::max(0., length - after - std::max(before, m_writer.get_extruder_retracted_length(int(filament.id()))));
    Vec3d end;
    if (Nonplanar::emit_conformal_wipe(m_writer, m_conformal_points, params, gcode, end))
        this->set_last_pos(this->gcode_to_point(end.head<2>()));
    m_wipe.reset_path();
    return gcode;
}

Nonplanar::PressureAdvance GCode::conformal_pressure_advance() const
{
    if (m_writer.filament() == nullptr)
        return {};
    const unsigned int id = m_writer.filament()->id();
    const size_t       fi = get_filament_config_index(id);
    if (m_config.enable_pressure_advance.get_at(fi)) {
        if (m_config.adaptive_pressure_advance.get_at(fi))
            return {};
        return { m_config.pressure_advance.get_at(fi), {} };
    }
    if (m_print != nullptr && m_print->is_BBL_printer())
        return {};
    auto                       it = m_start_gcode_pa.find(id);
    Nonplanar::PressureAdvance pa = it != m_start_gcode_pa.end() && it->second.value >= 0. ? it->second : m_machine_start_pa;
    return pa.value > 0. ? pa : Nonplanar::PressureAdvance();
}

const Nonplanar::HeadEnvelope &GCode::nonplanar_head()
{
    if (m_nonplanar_head.empty())
        m_nonplanar_head = Nonplanar::resolve_head_envelope(m_config);
    return m_nonplanar_head;
}

static bool rotary_nearest_turn(const RotaryKinematics &rotary, const JointPose &current, JointPose &goal)
{
    static const bool enabled = [] { const char *v = research_switch("NP_ROTARY_NEAREST_TURN"); return v != nullptr && std::atoi(v) != 0; }();
    if (enabled)
        for (int i = 0; i < 2; ++i)
            if (rotary.geometry().axes[i].continuous)
                goal.rot[i] += 360. * std::round((current.rot[i] - goal.rot[i]) / 360.);
    return enabled;
}

bool GCode::rotary_at_home() const
{
    const auto *rotary = dynamic_cast<const RotaryKinematics *>(&m_writer.tool_kinematics());
    if (rotary == nullptr)
        return true;
    const JointPose joints = m_writer.get_joints();
    JointPose       home   = joints;
    for (int i = 0; i < 2; ++i)
        home.rot[i] = rotary->machine_angle(i, 0.);
    rotary_nearest_turn(*rotary, joints, home);
    for (int i = 0; i < 2; ++i)
        if (std::abs(joints.rot[i] - home.rot[i]) >= 1e-4)
            return false;
    return true;
}

std::string GCode::rotary_hop(const ToolPose *target, const JointPose *seed)
{
    const ToolKinematics &kinematics = m_writer.tool_kinematics();
    const auto           *rotary     = dynamic_cast<const RotaryKinematics *>(&kinematics);
    if (rotary == nullptr)
        return {};
    const Vec3d     here    = m_writer.get_position();
    const Vec3d     axis    = m_writer.get_tool_axis();
    const Vec3d     goal    = target != nullptr ? target->axis : Vec3d::UnitZ();
    const Vec3d     offset(m_writer.get_xy_offset().x(), m_writer.get_xy_offset().y(), 0.);
    const JointPose current = m_writer.get_joints();
    auto same_rot = [](const JointPose &a, const JointPose &b) { return std::abs(a.rot[0] - b.rot[0]) < 1e-4 && std::abs(a.rot[1] - b.rot[1]) < 1e-4; };
    std::optional<JointPose> spin;
    if (target == nullptr) {
        JointPose home = current;
        for (int i = 0; i < 2; ++i)
            home.rot[i] = rotary->machine_angle(i, 0.);
        if (! same_rot(current, home))
            spin = home;
    } else if (seed != nullptr) {
        const ToolPose pose{ target->tip - offset, target->axis };
        JointPose      by_seed, by_here;
        if (kinematics.inverse(pose, seed, by_seed) && kinematics.inverse(pose, &current, by_here) && ! same_rot(by_seed, by_here)) {
            spin         = by_seed;
            spin->rot[0] = rotary->machine_angle(0, 0.);
        }
    }
    if (spin && rotary_nearest_turn(*rotary, current, *spin) && same_rot(*spin, current))
        spin.reset();
    if (target == nullptr && ! spin)
        return {};
    const double tilt = std::acos(std::clamp(std::min(axis.z(), goal.z()), -1., 1.));
    const double c = std::cos(tilt), s = std::sin(tilt);
    const Nonplanar::HeadEnvelope &head = this->nonplanar_head();
    double drop = 0.;
    for (const Nonplanar::HeadBox &box : head.boxes) {
        const double grow = box.nozzle ? 0. : head.lateral_margin;
        const double r    = std::hypot(std::max(std::abs(box.min.x()), std::abs(box.max.x())) + grow,
                                       std::max(std::abs(box.min.y()), std::abs(box.max.y())) + grow);
        drop = std::max(drop, r * s - box.min.z() * c);
    }
    constexpr double Withdraw = 5.;
    const Vec3d away = axis.z() < 1. - EPSILON ? Vec3d(here + Withdraw * axis) : here;
    const Vec3d onto = target != nullptr && goal.z() < 1. - EPSILON ? Vec3d(target->tip + Withdraw * goal) :
                       target != nullptr                             ? target->tip : here;
    const double safe = std::max({ std::max(m_nonplanar_ceiling, double(m_max_layer_z)) + drop + this->nonplanar_hop(), away.z(),
                                   onto.z() });
    std::string gcode;
    if ((away - here).norm() > EPSILON)
        gcode += m_writer.travel_to_pose({ away, axis }, "withdraw along the tool");
    if (away.z() < safe - EPSILON)
        gcode += m_writer.travel_to_pose({ Vec3d(away.x(), away.y(), safe), axis }, "lift above the shells");
    if (spin) {
        gcode += m_writer.travel_to_pose({ Vec3d(away.x(), away.y(), safe), Vec3d::UnitZ() }, "turn upright above the shells");
        JointPose joints;
        if (! kinematics.inverse({ m_writer.get_position() - offset, Vec3d::UnitZ() }, &*spin, joints))
            throw Slic3r::SlicingError(_u8L("Non-planar: the head cannot reach a safe height to turn the rotary axes."));
        joints.rot = spin->rot;
        m_writer.move_to_joints(gcode, joints, "turn the rotary axes");
    }
    if (target != nullptr) {
        gcode += m_writer.travel_to_pose({ Vec3d(onto.x(), onto.y(), safe), goal }, "turn above the shells");
        if ((onto - target->tip).norm() > EPSILON && safe > onto.z() + EPSILON)
            gcode += m_writer.travel_to_pose({ onto, goal }, "lower beside the curved shell");
        gcode += m_writer.travel_to_pose(*target, "lower onto the curved shell");
    }
    gcode += m_writer.end_inverse_time();
    return gcode;
}

std::string GCode::rotary_turn_on_spot(const ToolPose &target)
{
    const auto *rotary = dynamic_cast<const RotaryKinematics *>(&m_writer.tool_kinematics());
    const Vec3d here = m_writer.get_position(), axis = m_writer.get_tool_axis();
    const Vec3d offset(m_writer.get_xy_offset().x(), m_writer.get_xy_offset().y(), 0.);
    JointPose   end;
    const JointPose joints = m_writer.get_joints();
    if (rotary == nullptr || ! m_material_columns || ! rotary->inverse({ target.tip - offset, target.axis }, &joints, end))
        return m_writer.travel_to_pose(target, "turn on the curved shell");
    const Nonplanar::HeadEnvelope &head = this->nonplanar_head();
    auto clearance = [&](const Vec3d &tip, const JointPose &rot) { return m_material_columns->tilted_clearance(tip, rotary->head_frame(rot), head); };
    const double floor = std::min({ head.margin, clearance(here, joints), clearance(target.tip, end) }) - 0.01;
    const double turn  = std::max(std::abs(end.rot[0] - joints.rot[0]), std::abs(end.rot[1] - joints.rot[1]));
    const int    steps = std::max(1, int(std::ceil(turn / 0.5)));
    double       worst = std::numeric_limits<double>::infinity();
    for (int k = 1; k < steps; ++k) {
        JointPose rot = joints;
        for (int i = 0; i < 2; ++i)
            rot.rot[i] = joints.rot[i] + (end.rot[i] - joints.rot[i]) * double(k) / steps;
        worst = std::min(worst, clearance(here, rot));
    }
    if (worst >= floor)
        return m_writer.travel_to_pose(target, "turn on the curved shell");
    const double lift = floor - worst + 0.05;
    std::string  gcode;
    gcode += m_writer.travel_to_pose({ here + lift * axis, axis }, "lift to turn on the curved shell");
    gcode += m_writer.travel_to_pose({ target.tip + lift * target.axis, target.axis }, "turn on the curved shell");
    gcode += m_writer.travel_to_pose(target, "lower onto the curved shell");
    return gcode;
}

GCode::~GCode()
{
    if (m_table_clamped > 0)
        BOOST_LOG_TRIVIAL(warning) << "Table clamp: " << m_table_clamped << " poses leaned back from the BC table or cradle";
}

void GCode::clamp_table_tilt(std::vector<Nonplanar::ConformalPoint> &points)
{
    const ToolKinematics &kinematics = m_writer.tool_kinematics();
    if (kinematics.rotary_count() == 0 || !layout_is_table(kinematics.layout()))
        return;
    if (!m_table_guard)
        m_table_guard = std::make_unique<TableGuard>(this->nonplanar_head(), kinematics);
    if (m_table_guard->empty())
        return;
    const double margin = this->nonplanar_head().margin;
    const Vec3d  offset(m_writer.get_xy_offset().x(), m_writer.get_xy_offset().y(), 0.);
    JointPose    seed = m_writer.get_joints();
    for (Nonplanar::ConformalPoint &p : points) {
        const Vec3d axis = p.pose.axis.normalized();
        if (axis.z() > 1. - EPSILON)
            continue;
        JointPose q;
        auto clear = [&](const Vec3d &a) {
            return kinematics.inverse({ p.pose.tip - offset, a }, &seed, q) && m_table_guard->clearance(q) >= margin;
        };
        if (clear(axis)) {
            seed = q;
            continue;
        }
        const double tilt = std::acos(std::clamp(axis.z(), -1., 1.));
        const Vec3d  across = (axis - axis.z() * Vec3d::UnitZ()).normalized();
        auto         leaned = [&](double share) { return Vec3d(std::cos(share * tilt) * Vec3d::UnitZ() + std::sin(share * tilt) * across); };
        double       lo = 0., hi = 1.;
        for (int i = 0; i < 12; ++i) {
            const double mid = 0.5 * (lo + hi);
            (clear(leaned(mid)) ? lo : hi) = mid;
        }
        p.pose.axis = leaned(lo);
        ++m_table_clamped;
        if (kinematics.inverse({ p.pose.tip - offset, p.pose.axis }, &seed, q))
            seed = q;
    }
}

std::string GCode::rotary_local_hop(const ToolPose &target, bool straighten)
{
    const auto *rotary = dynamic_cast<const RotaryKinematics *>(&m_writer.tool_kinematics());
    if (rotary == nullptr || !m_material_columns || !m_last_pos_defined)
        return {};
    const Vec3d here = m_writer.get_position(), axis = m_writer.get_tool_axis();
    const Vec3d offset(m_writer.get_xy_offset().x(), m_writer.get_xy_offset().y(), 0.);
    const JointPose joints = m_writer.get_joints();
    JointPose end;
    if (!rotary->inverse({ target.tip - offset, target.axis }, &joints, end))
        return {};
    if (std::abs(end.rot[0] - joints.rot[0]) > 90. || std::abs(end.rot[1] - joints.rot[1]) > 90.)
        return {};
    const Nonplanar::HeadEnvelope &head = this->nonplanar_head();
    Nonplanar::HeadEnvelope crossing = head;
    crossing.tip_allowance = {};
    if (!m_table_guard)
        m_table_guard = std::make_unique<TableGuard>(this->nonplanar_head(), *rotary);
    auto clearance = [&](const Vec3d &tip, const JointPose &rot, const Nonplanar::HeadEnvelope &envelope) {
        return std::min(m_material_columns->tilted_clearance(tip, rotary->head_frame(rot), envelope),
                        m_table_guard->clearance(rot) - head.margin);
    };
    const double at_here = std::min(0., clearance(here, joints, head)) - 0.01;
    const double at_end = std::min(0., clearance(target.tip, end, head)) - 0.01;
    const int count = std::max(1, int(std::ceil((target.tip - here).norm() / 2.)));
    std::vector<ToolPose> route;
    route.reserve(size_t(count + 1));
    for (int i = 0; i <= count; ++i) {
        const double u = double(i) / count;
        Vec3d tip = here + u * (target.tip - here);
        if (i > 0 && i < count) {
            const double top = m_material_columns->top_in_rect(BoundingBoxf(tip.head<2>() - Vec2d::Constant(0.125),
                                                                           tip.head<2>() + Vec2d::Constant(0.125)));
            if (std::isfinite(top))
                tip.z() = std::max(tip.z(), top);
        }
        route.push_back({ tip, rotary->forward(lerp(joints, end, u)).axis });
    }
    const double limit = std::max({m_nonplanar_ceiling, double(m_max_layer_z), here.z(), target.tip.z()}) + 20.;
    double lever = std::max(here.norm(), target.tip.norm()) + rotary->geometry().pivot.norm() +
                   rotary->geometry().tool_mount.norm() + rotary->geometry().tool_length + std::abs(rotary->geometry().table_height);
    double head_lever = 0.;
    for (const Nonplanar::HeadBox &box : head.boxes)
        if (!box.infinite_x && !box.infinite_y)
            head_lever = std::max(head_lever, box.min.norm() + box.max.norm());
    lever += head_lever;
    auto deficit = [&](const JointPose &from, const ToolPose &to, const Nonplanar::HeadEnvelope &envelope,
                       double floor, JointPose &last) {
        std::vector<JointPose> segments;
        last = from;
        try {
            rotary->linearize(from, rotary->forward(from), {to.tip - offset, to.axis}, GCodeWriter::max_tip_deviation, segments);
        } catch (const RuntimeError &) {
            return std::numeric_limits<double>::infinity();
        }
        double need = 0.;
        for (const JointPose &next : segments) {
            const double turn = std::abs(next.rot[0] - last.rot[0]) + std::abs(next.rot[1] - last.rot[1]);
            const double displacement = (next.xyz - last.xyz).norm() + turn * M_PI / 180. * lever;
            const int samples = std::max(1, int(std::ceil(displacement / 0.25)));
            for (int k = 0; k <= samples; ++k) {
                const JointPose q = lerp(last, next, double(k) / samples);
                need = std::max(need, floor - clearance(rotary->forward(q).tip + offset, q, envelope));
            }
            last = next;
        }
        return need;
    };
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::vector<double> lifts(route.size(), 0.);
        JointPose previous = joints;
        for (size_t i = 0; i < route.size(); ++i) {
            JointPose next;
            const double need = deficit(previous, route[i], i == 0 ? head : crossing, i == 0 ? at_here : 0.13, next);
            if (need > EPSILON) {
                const double lift = (need + 0.03) / std::max(0.25, route[i].axis.z());
                lifts[i] = std::max(lifts[i], lift);
                if (i > 0)
                    lifts[i - 1] = std::max(lifts[i - 1], lift);
            }
            previous = next;
        }
        if (std::any_of(lifts.begin(), lifts.end(), [](double lift) { return lift > 0.; })) {
            for (size_t i = 0; i < route.size(); ++i)
                route[i].tip += lifts[i] * route[i].axis;
            if (std::any_of(route.begin(), route.end(), [limit](const ToolPose &p) { return p.tip.z() > limit; }))
                return {};
            continue;
        }
        JointPose landed;
        if (deficit(previous, target, head, at_end, landed) > EPSILON)
            return {};
        if (straighten && route.size() > 2) {
            std::vector<ToolPose> kept { route.front() };
            JointPose at;
            deficit(joints, route.front(), head, at_here, at);
            for (size_t i = 0; i + 1 < route.size();) {
                size_t    j = route.size() - 1;
                JointPose reached;
                while (j > i + 1 && deficit(at, route[j], crossing, 0.13, reached) > EPSILON)
                    --j;
                if (j == i + 1)
                    deficit(at, route[j], crossing, 0.13, reached);
                kept.push_back(route[j]);
                at = reached;
                i  = j;
            }
            route = std::move(kept);
        }
        std::string gcode;
        for (size_t i = 0; i < route.size(); ++i)
            if ((m_writer.get_position() - route[i].tip).norm() > EPSILON ||
                (m_writer.get_tool_axis() - route[i].axis).norm() > EPSILON)
                gcode += m_writer.travel_to_pose(route[i], i == 0 ? "lift along the tool" : "travel over the curved shell");
        gcode += m_writer.travel_to_pose(target, "lower onto the curved shell");
        gcode += m_writer.end_inverse_time();
        return gcode;
    }
    return {};
}

std::string GCode::rotary_move_safely(const ToolPose *target, const JointPose *seed)
{
    const auto *kinematics = dynamic_cast<const RotaryKinematics *>(&m_writer.tool_kinematics());
    if (kinematics == nullptr)
        return {};
    const RotaryGeometry &geo     = kinematics->geometry();
    const JointPose       current = m_writer.get_joints();
    JointPose             goal;
    if (target == nullptr) {
        for (int i = 0; i < 2; ++i)
            goal.rot[i] = kinematics->machine_angle(i, 0.);
        rotary_nearest_turn(*kinematics, current, goal);
        if (std::abs(current.rot[0] - goal.rot[0]) < 1e-4 && std::abs(current.rot[1] - goal.rot[1]) < 1e-4)
            return {};
    } else if (! kinematics->inverse(*target, seed != nullptr ? seed : &current, goal))
        throw Slic3r::SlicingError(_u8L("Non-planar: a curved shell needs a tool tilt the rotary axes cannot reach."));

    const double ceiling = std::max(m_nonplanar_ceiling, double(m_max_layer_z));
    const double clear   = 5.;
    const bool   table   = layout_is_table(kinematics->layout()) && ! geo.firmware_rtcp;
    const bool   head    = ! layout_is_table(kinematics->layout());
    const Vec3d  o(0., 0., geo.table_height);
    double       high;
    if (table) {
        BoundingBoxf bed(m_config.printable_area.values);
        double       bed_radius = 0.;
        for (const Vec2d &c : { bed.min, bed.max, Vec2d(bed.min.x(), bed.max.y()), Vec2d(bed.max.x(), bed.min.y()) })
            bed_radius = std::max(bed_radius, c.norm());
        high = std::hypot(bed_radius, ceiling + geo.table_height) + clear;
    } else {
        double swing = geo.tool_length;
        if (geo.firmware_rtcp) {
            swing = 0.;
            for (const Nonplanar::HeadBox &box : this->nonplanar_head().boxes)
                if (! box.infinite_x && ! box.infinite_y)
                    swing = std::max(swing, Vec3d(std::max(-box.min.x(), box.max.x()), std::max(-box.min.y(), box.max.y()),
                                                  std::max(-box.min.z(), box.max.z())).norm());
        }
        high = ceiling + swing + clear;
    }
    auto joints_at = [&](const Vec3d &point, const JointPose &rot) {
        JointPose      out = rot;
        const ToolPose pose = kinematics->forward(rot);
        const Vec3d    tip  = table ? Vec3d(kinematics->head_frame(rot).transpose() * point - o) : point;
        if (! kinematics->inverse({ tip, pose.axis }, &rot, out))
            throw Slic3r::SlicingError(_u8L("Non-planar: the head cannot reach a safe height to turn the rotary axes."));
        return out;
    };
    auto lifted = [&](const JointPose &j) {
        const ToolPose pose = kinematics->forward(j);
        if (table) {
            Vec3d r = kinematics->head_frame(j) * (pose.tip + o);
            r.z()   = std::max(r.z(), high);
            return r;
        }
        const Vec3d departed = pose.tip + (head && pose.axis.z() < 1. - EPSILON ? clear : 0.) * pose.axis;
        return Vec3d(departed.x(), departed.y(), std::max(departed.z(), high));
    };

    std::string gcode;
    const ToolPose pose = kinematics->forward(current);
    if (head && pose.axis.z() < 1. - EPSILON)
        m_writer.move_to_joints(gcode, joints_at(pose.tip + clear * pose.axis, current), "retract along the tool before turning");
    const JointPose up = joints_at(lifted(current), current);
    m_writer.move_to_joints(gcode, up, "lift to turn the rotary axes");
    JointPose turned = up;
    turned.rot       = goal.rot;
    m_writer.move_to_joints(gcode, turned, "turn the rotary axes");
    if (target != nullptr) {
        Vec3d above;
        if (table) {
            above = kinematics->head_frame(goal) * (target->tip + o);
            above.z() = std::max(above.z(), high);
        } else {
            const double t = target->axis.z() > EPSILON ? std::max(0., (high - target->tip.z()) / target->axis.z()) : 0.;
            above          = target->tip + t * target->axis;
        }
        m_writer.move_to_joints(gcode, joints_at(above, goal), "move above the curved shell");
        m_writer.move_to_joints(gcode, goal, "lower onto the curved shell");
    }
    gcode += m_writer.end_inverse_time();
    return gcode;
}

std::string GCode::extrude_support(const ExtrusionEntityCollection &support_fills, const ExtrusionRole support_extrusion_role)
{
    static constexpr const char* support_label            = "support material";
    static constexpr const char* support_interface_label  = "support material interface";
    static constexpr const char* support_transition_label = "support transition";
    static constexpr const char* support_ironing_label    = "support ironing";

    const auto speed_for_path = [&](double length, ExtrusionRole role, double default_speed = -1.0) {
        if (!is_support(role) || length > SMALL_PERIMETER_LENGTH(NOZZLE_CONFIG(small_support_perimeter_threshold)))
            return default_speed;

        double small_perimeter_speed = -1.0;

        const auto base_speed = (role == erSupportMaterialInterface) 
            ? NOZZLE_CONFIG(support_interface_speed) : NOZZLE_CONFIG(support_speed);

        if (NOZZLE_CONFIG(small_support_perimeter_speed).value == 0)
            small_perimeter_speed = base_speed * 0.5;
        else
            small_perimeter_speed = NOZZLE_CONFIG(small_support_perimeter_speed).get_abs_value(base_speed);

        return small_perimeter_speed > 0 ? small_perimeter_speed : default_speed;
    };

    std::string gcode;
    if (!support_fills.entities.empty()) {

        std::vector<const ExtrusionEntity*> extrusions;
        extrusions.reserve(support_fills.entities.size());
        for (const ExtrusionEntity* ee : support_fills.entities) {
            const auto role = ee->role();
            if ((role == support_extrusion_role) || (support_extrusion_role == erMixed && role != erIroning)) {
                extrusions.emplace_back(ee);
            }
        }
        if (extrusions.empty())
            return gcode;

        std::vector<std::unique_ptr<ExtrusionEntity>> reversed;
        if (!support_fills.no_sort)
            chain_and_reorder_extrusion_entities(extrusions, m_last_pos.to_point(), reversed);

        for (const ExtrusionEntity *ee : extrusions) {
            ExtrusionRole role = ee->role();
            assert(is_support(role) || role == erIroning);

            const char* label = (role == erSupportMaterial) ? support_label :
                ((role == erSupportMaterialInterface) ? support_interface_label : 
                ((role == erIroning) ? support_ironing_label : support_transition_label));

            const ExtrusionPath* path = dynamic_cast<const ExtrusionPath*>(ee);
            const ExtrusionMultiPath* multipath = dynamic_cast<const ExtrusionMultiPath*>(ee);
            const ExtrusionLoop* loop = dynamic_cast<const ExtrusionLoop*>(ee);
            const ExtrusionEntityCollection* collection = dynamic_cast<const ExtrusionEntityCollection*>(ee);

            if (path) {
                gcode += extrude_path(*path, label, speed_for_path(path->length(), role));
            }
            else if (multipath) {
                gcode += extrude_multi_path(*multipath, label, speed_for_path(multipath->length(), role));
            }
            else if (loop) {
                gcode += extrude_loop(*loop, label, speed_for_path(loop->length(), role));
            }
            else if (collection) {
                gcode += extrude_support(*collection, support_extrusion_role);
            }
            else {
                throw Slic3r::InvalidArgument("Unknown extrusion type");
            }
        }
    }
    return gcode;
}

bool GCode::GCodeOutputStream::is_error() const
{
    return ::ferror(this->f);
}

void GCode::GCodeOutputStream::flush()
{
    ::fflush(this->f);
}

void GCode::GCodeOutputStream::close()
{
    if (this->f) {
        ::fclose(this->f);
        this->f = nullptr;
    }
}

void GCode::GCodeOutputStream::write(const char *what)
{
    if (what != nullptr) {
        const char* gcode = what;
        fwrite(gcode, 1, ::strlen(gcode), this->f);
        m_processor.process_buffer(std::string(gcode));
    }
}

void GCode::GCodeOutputStream::writeln(const std::string &what)
{
    if (! what.empty())
        this->write(what.back() == '\n' ? what : what + '\n');
}

void GCode::GCodeOutputStream::write_format(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    int buflen;
    {
        va_list args2;
        va_copy(args2, args);
        buflen =
    #ifdef _MSC_VER
            ::_vscprintf(format, args2)
    #else
            ::vsnprintf(nullptr, 0, format, args2)
    #endif
            + 1;
        va_end(args2);
    }

    char buffer[1024];
    bool buffer_dynamic = buflen > 1024;
    char *bufptr = buffer_dynamic ? (char*)malloc(buflen) : buffer;
    int res = ::vsnprintf(bufptr, buflen, format, args);
    if (res > 0)
        this->write(bufptr);

    if (buffer_dynamic)
        free(bufptr);

    va_end(args);
}

bool GCode::_needSAFC(const ExtrusionPath &path)
{
    if (path.nonplanar_source)
        return false;
    if (!m_small_area_infill_flow_compensator || !m_config.small_area_infill_flow_compensation.value)
        return false;

    static const InfillPattern supported_patterns[] = {
        InfillPattern::ipRectilinear,
        InfillPattern::ipAlignedRectilinear,
        InfillPattern::ipMonotonic,
        InfillPattern::ipMonotonicLine,
    };

    return std::any_of(std::begin(supported_patterns), std::end(supported_patterns), [&](const InfillPattern pattern) {
        return (this->on_first_layer() && this->config().bottom_surface_pattern == pattern) ||
               (path.role() == erSolidInfill && this->config().internal_solid_infill_pattern == pattern) ||
               (path.role() == erTopSolidInfill && this->config().top_surface_pattern == pattern);
    });
}

double GCode::calc_max_volumetric_speed(const double layer_height, const double line_width, const std::string co_str)
{
    std::vector<double> cs;
    std::stringstream   ss(co_str);
    std::string         token;

    while (std::getline(ss, token, ' ')) {
        try {
            cs.push_back(std::stod(token));
        } catch (...) {
            std::cerr << "Transformation failed: " << token << std::endl;
        }
    }
    if (cs.size() != 6 || std::all_of(cs.begin(), cs.end(), [](double v) { return v == 0; })) return std::numeric_limits<double>::max();

    const double x = layer_height;
    const double y = line_width;

    double res = cs[0] * x * x + cs[1] * y * y + cs[2] * x * y + cs[3] * x + cs[4] * y + cs[5];
    return res;
}

static float overhang_fan_overlap_threshold(int overhang_fan_threshold)
{
    switch (overhang_fan_threshold) {
    case (int) Overhang_threshold_1_4: return 0.9f;
    case (int) Overhang_threshold_2_4: return 0.75f;
    case (int) Overhang_threshold_3_4: return 0.5f;
    case (int) Overhang_threshold_4_4: return 0.25f;
    case (int) Overhang_threshold_bridge: return 0.05f;
    default: return -1.f;
    }
}

ConfigOptionFloatsOrPercents GCode::overhang_speed_table(double ref_speed) const
{
    auto level = [ref_speed](double speed) {
        return speed < 0.5 ? FloatOrPercent{100, true} : FloatOrPercent{speed * 100 / ref_speed, true};
    };
    const double speed_4_4 = NOZZLE_CONFIG(overhang_4_4_speed).get_abs_value(ref_speed);
    return ConfigOptionFloatsOrPercents({FloatOrPercent{100, true}, level(NOZZLE_CONFIG(overhang_1_4_speed).get_abs_value(ref_speed)),
                                         level(NOZZLE_CONFIG(overhang_2_4_speed).get_abs_value(ref_speed)),
                                         level(NOZZLE_CONFIG(overhang_3_4_speed).get_abs_value(ref_speed)), level(speed_4_4),
                                         NOZZLE_CONFIG(slowdown_for_curled_perimeters) ?
                                             level(speed_4_4) :
                                             FloatOrPercent{NOZZLE_CONFIG(bridge_speed) * 100 / ref_speed, true}});
}

std::string GCode::_extrude(const ExtrusionPath &path, const std::string &path_description, double speed)
{
    if (m_layer != nullptr && dynamic_cast<const ExtrusionPathConformal *>(&path) == nullptr) {
        const auto field = m_wave_fields.find(m_layer->object());
        if (field != m_wave_fields.end())
            if (auto wave = field->second.lift(path, *m_layer)) {
                for (const Point3 &point : wave->polyline.points)
                    m_max_layer_z = std::max(m_max_layer_z, float(m_nominal_z - m_config.z_offset.value + unscale<double>(point.z())));
                std::string curved = ";_NONPLANAR_BEGIN shell=0 wave=" + std::to_string(m_layer->object()->get_id()) +
                                     " rate=" + std::to_string(this->conformal_segment_rate()) + "\n";
                curved += this->extrude_conformal_path(*wave, path_description);
                curved += ";_NONPLANAR_END\n";
                m_conformal_prev = nullptr;
                return curved;
            }
    }
    const bool topmost_nonplanar = path.role() == erTopSolidInfill && path.nonplanar_source && !path.continuous_top &&
        path.distance_to_top >= 0.f && path.distance_to_top < 1e-4f;
    if (! m_slope_compensating && topmost_nonplanar) {
        constexpr double downhill_raise = 0.25;
        constexpr double uphill_lower   = 0.;
        ExtrusionPath compensated(path);
        compensate_nonplanar_slope(compensated.polyline, downhill_raise, uphill_lower, m_nonplanar_clearance.tip_radius_mm);
        m_slope_compensating = true;
        std::string out;
        try {
            out = this->_extrude(compensated, path_description, speed);
        } catch (...) {
            m_slope_compensating = false;
            throw;
        }
        m_slope_compensating = false;
        return out;
    }
    std::string gcode;
    if (!path.continuous_top && path.polyline.length() < scale_(0.05) &&
        (path.z_contoured || (path.role() == erTopSolidInfill && Nonplanar::requests_conformal_shells(*m_print))))
        return gcode;
    this->ensure_material_columns(path);

    if (path.z_contoured && m_material_columns &&
        (m_config.zaa_enabled || (m_layer != nullptr && (m_layer->object()->curved_fallback_zaa() || m_layer->object()->auto_z_contouring()))))
        m_nonplanar_ceiling = std::max(m_nonplanar_ceiling, m_material_columns->ceiling());

    const std::string  bridge_description = is_bridge(path.role()) ? path_description + " (bridge)" : std::string();
    const std::string &description        = bridge_description.empty() ? path_description : bridge_description;

    const Vec3d path_point_mm{
        unscale<double>(path.first_point().x()),
        unscale<double>(path.first_point().y()),
        m_layer ? m_layer->print_z : (m_belt_brim_z ? *m_belt_brim_z : 0.0)
    };
    const bool path_on_first_layer = this->on_first_layer(path_point_mm);

    const bool z_absolute = curved_tops_mode(m_config.nonplanar_mode.value) && path_has_absolute_z(path);
    auto absolute_point = [this](const Point3 &p) {
        Vec3d q = this->point_to_gcode(p);
        q.z() += m_config.z_offset.value;
        return Vec3d(GCodeFormatter::quantize_xyzf(q.x()), GCodeFormatter::quantize_xyzf(q.y()), GCodeFormatter::quantize_xyzf(q.z()));
    };
    auto check_nonplanar_step = [this, &path](const Vec3d &from, const Vec3d &to) {
        if (!path.nonplanar_source)
            return;
        const double dxy = std::hypot(to.x() - from.x(), to.y() - from.y());
        const double dz = std::abs(to.z() - from.z());
        const double max_gradient = m_config.nonplanar_curved_max_angle.value > 12. ?
            std::tan(m_config.nonplanar_curved_max_angle.value * M_PI / 180.) : 0.21255656167002213;
        constexpr double quantized_z_delta = 0.001;
        constexpr double quantized_xy_delta = 0.0014142135623730951;
        const double source_xy_upper_bound = dxy + quantized_xy_delta;
        constexpr double rounding_epsilon = 1e-9;
        if ((dxy < quantized_xy_delta && dz > quantized_z_delta + rounding_epsilon) ||
            (dxy >= quantized_xy_delta && dz > max_gradient * source_xy_upper_bound + quantized_z_delta + rounding_epsilon))
            throw_nonplanar_too_steep(to.z(), m_layer != nullptr ? m_layer->id() : 0, m_config.nonplanar_curved_max_angle.value,
                m_layer != nullptr ? m_layer->object()->id().id : 0,
                "height step from G-code X" + std::to_string(from.x()) + " Y" + std::to_string(from.y()) + " Z" + std::to_string(from.z()) +
                " to X" + std::to_string(to.x()) + " Y" + std::to_string(to.y()) + " Z" + std::to_string(to.z()) +
                " (task " + std::to_string(path.source_path_id) + ")");
    };
    if (curved_tops_mode(m_config.nonplanar_mode.value)) {
        const size_t task_id = m_path_task_ids.emplace(path.source_path_id, m_path_task_ids.size()).first->second;
        gcode += ";PATH_TASK id=" + std::to_string(task_id) +
                 " conformal=" + std::to_string(path.nonplanar_source ? 1 : 0) +
                 " surface=" + std::to_string(path.source_surface_id) +
                 " source_layer=" + (path.source_layer_id == size_t(-1) ? std::string("-1") : std::to_string(path.source_layer_id)) +
                 " shell_offset=" + std::to_string(path.distance_to_top) + "\n";
    }
    if (path.nonplanar_source)
        gcode += ";_NONPLANAR_BEGIN patch=" + std::to_string(path.source_surface_id) + " shell=" +
                 std::to_string(int(std::lround(path.distance_to_top / std::max(0.01f, path.height)))) +
                 " rate=" + std::to_string(std::lround(this->conformal_segment_rate())) +
                 (path.continuous_top ? " continuous=1" : "") + "\n";


    const ExtrusionPathSloped* sloped = dynamic_cast<const ExtrusionPathSloped*>(&path);

    const auto get_sloped_z = [&sloped, this](double z_ratio) {
        const auto height = sloped->height;
        return lerp(m_nominal_z - height, m_nominal_z, z_ratio);
    };

    bool slope_need_z_travel = false;
    if (sloped != nullptr && !sloped->is_flat()) {
        auto target_z = get_sloped_z(sloped->slope_begin.z_ratio);
        slope_need_z_travel = m_writer.will_move_z(target_z);
    }
    if (curved_tops_mode(m_config.nonplanar_mode.value) ||
        (m_layer != nullptr && Nonplanar::wave_enabled(*m_layer->object())))
        m_writer.set_z_travel_cap(this->conformal_max_speed_z());
    Point first_point = path.first_point();
    const double absolute_first_z = z_absolute ? absolute_point(path.polyline.points.front()).z() : 0.;
    const bool positioned = !m_conformal_points.empty() && dynamic_cast<const ExtrusionPathConformal *>(&path) != nullptr;
    if (!positioned && (!m_last_pos_defined || m_last_pos.to_point() != first_point || m_need_change_layer_lift_z ||
        slope_need_z_travel || (z_absolute && std::abs(m_writer.get_position().z() - absolute_first_z) > 0.0005))) {
        const bool _last_pos_undefined = !m_last_pos_defined;

        double z = DBL_MAX;
        if (sloped != nullptr) {
            z =  get_sloped_z(sloped->slope_begin.z_ratio);
        } else if (z_absolute) {
            z = absolute_first_z;
        } else if (path.z_contoured && !path.polyline.lines().empty()) {
            z = unscale_(path.polyline.lines().begin()->a.z()) + m_nominal_z;
        }

        const Nonplanar::RepairHints *hints  = m_layer != nullptr ? m_layer->object()->nonplanar_repair() : nullptr;
        const double                  target = z != DBL_MAX ? z : m_nominal_z;
        if (hints != nullptr && hints->travel_lift > 0. && sloped == nullptr &&
            (m_need_change_layer_lift_z || path.z_contoured || path.role() == erTopSolidInfill ||
             m_last_processor_extrusion_role == erTopSolidInfill ||
             std::binary_search(hints->travel_layers.begin(), hints->travel_layers.end(), m_layer->id()) ||
             std::abs(m_writer.get_position().z() - m_nominal_z) > EPSILON)) {
            const double over = std::max({ m_writer.get_position().z(), target, this->nonplanar_travel_z() });
            gcode += m_writer.travel_to_z(over, "lift over contoured surface", true, this->conformal_max_speed_z());
            gcode += this->travel_to(first_point, path.role(), "move to first " + description + " point", over);
        } else if (m_layer != nullptr && m_layer->top_textured && sloped == nullptr && !z_absolute) {
            double depth = 0.;
            for (const LayerRegion *layerm : m_layer->regions())
                if (layerm->region().config().top_texture.value != TopTexture::None)
                    depth = std::max(depth, layerm->region().config().top_texture_depth.value);
            const double over = std::max({ m_writer.get_position().z(), target, m_nominal_z + depth + 0.1 });
            gcode += m_writer.travel_to_z(over, "lift over textured top", true, this->conformal_max_speed_z());
            gcode += this->travel_to(first_point, path.role(), "move to first " + description + " point", over);
        } else
            gcode += this->travel_to(first_point, path.role(), "move to first " + description + " point", z, z_absolute && path.nonplanar_source ? &path : nullptr);

        if (!z_absolute && !slope_need_z_travel && (_last_pos_undefined || m_need_change_layer_lift_z)) {
            const std::string z_sync_comment = _last_pos_undefined ?
                "ensure Z matches planned layer height" : "";
            gcode += this->writer().travel_to_z(m_nominal_z, z_sync_comment, true);
        }
        m_need_change_layer_lift_z = false;
    }

    const auto contour_travel = [this](double z, const std::string &comment) {
        const double limit = this->conformal_max_speed_z();
        const bool defer_cap = (m_config.zaa_enabled || (m_layer != nullptr && m_layer->object()->curved_fallback_zaa())) &&
                               !Nonplanar::requests_conformal_shells(*m_print);
        std::string move = m_writer.travel_to_z(z, comment, true, defer_cap ? 0. : limit);
        if (defer_cap && limit > 0. && !move.empty())
            move.insert(0, std::string(CoolingBuffer::ZaaMaxFeedTag) + std::to_string(std::floor(limit * 60.)) + "\n");
        return move;
    };
    if (!positioned && path.z_contoured && !path.polyline.lines().empty()) {
        double current_z = m_writer.get_position().z();
        double first_z = unscale_(path.polyline.lines().begin()->a.z()) + m_nominal_z;
        if (GCodeFormatter::quantize_xyzf(first_z) != GCodeFormatter::quantize_xyzf(current_z)) {
            gcode += contour_travel(first_z, "set Z for contouring");
        }
    }
    if (z_absolute) {
        if (GCodeFormatter::quantize_xyzf(m_writer.get_position().z()) != GCodeFormatter::quantize_xyzf(absolute_first_z))
            gcode += m_writer.travel_to_z(absolute_first_z, "set Z for nonplanar extrusion", true);
    } else if (!path.z_contoured && sloped == nullptr) {
        double current_z = m_writer.get_position().z();
        if (GCodeFormatter::quantize_xyzf(current_z) != GCodeFormatter::quantize_xyzf(m_nominal_z)) {
            gcode += m_config.zaa_enabled || (m_layer != nullptr && m_layer->object()->curved_fallback_zaa()) || Nonplanar::requests_conformal_shells(*m_print) ?
                contour_travel(m_nominal_z, "reset Z after contouring") :
                m_writer.travel_to_z(m_nominal_z, "reset Z after contouring", true);
        }
    }

    m_writer.add_object_change_labels(gcode);

    Extruder *filament = m_writer.filament();
    if (const auto *conformal = dynamic_cast<const ExtrusionPathConformal *>(&path);
        conformal != nullptr && !conformal->preserve_points && filament != nullptr && !m_config.use_firmware_retraction &&
        m_config.gcode_flavor.value != gcfMakerWare) {
        gcode += m_writer.unlift();
        m_conformal_prime = filament->take_retraction();
    } else
        gcode += this->unretract();
    m_conformal_retract = 0.;
    m_config.apply(m_calib_config);

    unsigned int acceleration_i = 0;
    double jerk = 0;
    const size_t nozzle = get_nozzle_config_index(m_writer.filament()->id());
    if (m_config.default_acceleration.get_at(nozzle) > 0) {
        const ExtrusionRole role = path.role();
        const double bridge_acceleration = is_bridge(role) ?
            m_config.bridge_acceleration.get_at(nozzle).get_abs_value(m_config.outer_wall_acceleration.get_at(nozzle)) : 0.;
        const double sparse_infill_acceleration = role == erInternalInfill ?
            m_config.sparse_infill_acceleration.get_at(nozzle).get_abs_value(m_config.default_acceleration.get_at(nozzle)) : 0.;
        const double internal_solid_infill_acceleration = role == erSolidInfill ?
            m_config.internal_solid_infill_acceleration.get_at(nozzle).get_abs_value(m_config.default_acceleration.get_at(nozzle)) : 0.;
        double acceleration;
        if (path_on_first_layer && m_config.initial_layer_acceleration.get_at(nozzle) > 0) {
            acceleration = m_config.initial_layer_acceleration.get_at(nozzle);
#if 0
        } else if (this->object_layer_over_raft() && m_config.first_layer_acceleration_over_raft.value > 0) {
            acceleration = m_config.first_layer_acceleration_over_raft.value;
#endif
        } else if (bridge_acceleration > 0) {
            acceleration = bridge_acceleration;
        } else if (sparse_infill_acceleration > 0) {
            acceleration = sparse_infill_acceleration;
        } else if (internal_solid_infill_acceleration > 0) {
            acceleration = internal_solid_infill_acceleration;
        } else if (m_config.outer_wall_acceleration.get_at(nozzle) > 0 && is_external_perimeter(role)) {
            acceleration = m_config.outer_wall_acceleration.get_at(nozzle);
        } else if (m_config.inner_wall_acceleration.get_at(nozzle) > 0 && is_internal_perimeter(role)) {
            acceleration = m_config.inner_wall_acceleration.get_at(nozzle);
        } else if (m_config.top_surface_acceleration.get_at(nozzle) > 0 && is_top_surface(role)) {
            acceleration = m_config.top_surface_acceleration.get_at(nozzle);
        } else {
            acceleration = m_config.default_acceleration.get_at(nozzle);
        }
        acceleration_i = (unsigned int)floor(acceleration + 0.5);
    }

    if (NOZZLE_CONFIG(default_jerk) > 0) {
        if (path_on_first_layer && NOZZLE_CONFIG(initial_layer_jerk) > 0) {
            jerk = NOZZLE_CONFIG(initial_layer_jerk);
        } else if (NOZZLE_CONFIG(outer_wall_jerk) > 0 && is_external_perimeter(path.role())) {
             jerk = NOZZLE_CONFIG(outer_wall_jerk);
        } else if (NOZZLE_CONFIG(inner_wall_jerk) > 0 && is_internal_perimeter(path.role())) {
            jerk = NOZZLE_CONFIG(inner_wall_jerk);
        } else if (NOZZLE_CONFIG(top_surface_jerk) > 0 && is_top_surface(path.role())) {
            jerk = NOZZLE_CONFIG(top_surface_jerk);
        } else if (NOZZLE_CONFIG(infill_jerk) > 0 && is_infill(path.role())) {
            jerk = NOZZLE_CONFIG(infill_jerk);
        }
        else {
            jerk = NOZZLE_CONFIG(default_jerk);
        }
    }

    if (m_writer.get_gcode_flavor() == gcfKlipper) {
        gcode += m_writer.set_accel_and_jerk(acceleration_i, jerk);

    } else {
        gcode += m_writer.set_print_acceleration(acceleration_i);
        gcode += m_writer.set_jerk_xy(jerk);
    }

    double filament_flow_ratio = FILAMENT_CONFIG(filament_flow_ratio);
    auto _mm3_per_mm = path.mm3_per_mm * this->config().print_flow_ratio;
    _mm3_per_mm *= filament_flow_ratio;

    if (path.role() == erTopSolidInfill) {
        _mm3_per_mm *= NOZZLE_CONFIG(top_solid_infill_flow_ratio);
    } else if (path.role() == erBottomSurface) {
        _mm3_per_mm *= m_config.bottom_solid_infill_flow_ratio;
    } else if (path.role() == erInternalBridgeInfill) {
        _mm3_per_mm *= m_config.internal_bridge_flow;
    } else if (path.role() == erBrim) {
        _mm3_per_mm *= m_config.brim_flow_ratio;
    } else if (sloped) {
        _mm3_per_mm *= m_config.scarf_joint_flow_ratio;
    }

    if (m_config.set_other_flow_ratios) {
        if (path.role() == erExternalPerimeter) {
            _mm3_per_mm *= m_config.outer_wall_flow_ratio;
        } else if (path.role() == erPerimeter) {
            _mm3_per_mm *= m_config.inner_wall_flow_ratio;
        } else if (path.role() == erOverhangPerimeter) {
            _mm3_per_mm *= m_config.overhang_flow_ratio;
        } else if (path.role() == erInternalInfill) {
            _mm3_per_mm *= m_config.sparse_infill_flow_ratio;
        } else if (path.role() == erSolidInfill) {
            _mm3_per_mm *= m_config.internal_solid_infill_flow_ratio;
        } else if (path.role() == erGapFill) {
            _mm3_per_mm *= m_config.gap_fill_flow_ratio;
        } else if (path.role() == erSupportMaterial) {
            _mm3_per_mm *= m_config.support_flow_ratio;
        } else if (path.role() == erSupportMaterialInterface) {
            _mm3_per_mm *= m_config.support_interface_flow_ratio;
        }

        if (path_on_first_layer && (path.role() != erBrim && path.role() != erSkirt)) {
            _mm3_per_mm *= m_config.first_layer_flow_ratio;
        }
    }

    float effective_height = path.height;
    if (m_sub_layer_flow_ratio > 0.0) {
        _mm3_per_mm *= m_sub_layer_flow_ratio;
        effective_height = static_cast<float>(m_sub_layer_height);
    }

    double e_per_mm = m_writer.filament()->e_per_mm3() * _mm3_per_mm;
    if (z_absolute && path.nonplanar_source)
        e_per_mm *= path.role() == erTopSolidInfill ? m_config.nonplanar_top_finish_flow.value : m_config.nonplanar_curved_shell_flow.value;
    e_per_mm /= filament_flow_ratio;

    if (speed == -1) {
        if (path.role() == erPerimeter) {
            speed = NOZZLE_CONFIG(inner_wall_speed);
            if (sloped) {
                speed = std::min(speed, m_config.scarf_joint_speed.get_abs_value(speed));
            }
        } else if (path.role() == erExternalPerimeter) {
            speed = NOZZLE_CONFIG(outer_wall_speed);
            if (sloped) {
                speed = std::min(speed, m_config.scarf_joint_speed.get_abs_value(speed));
            }
        } else if(path.role() == erInternalBridgeInfill) {
            speed = m_config.internal_bridge_speed.get_at(nozzle).get_abs_value(m_config.bridge_speed.get_at(nozzle));
        } else if (path.role() == erOverhangPerimeter || path.role() == erSupportTransition || path.role() == erBridgeInfill) {
            speed = NOZZLE_CONFIG(bridge_speed);
        } else if (path.role() == erInternalInfill) {
            speed = NOZZLE_CONFIG(sparse_infill_speed);
        } else if (path.role() == erSolidInfill) {
            speed = NOZZLE_CONFIG(internal_solid_infill_speed);
        } else if (path.role() == erTopSolidInfill) {
            speed = NOZZLE_CONFIG(top_surface_speed);
        } else if (path.role() == erIroning) {
            const size_t filament_idx = get_filament_config_index(m_writer.filament()->id());
            speed = m_config.filament_ironing_speed.is_nil(filament_idx)
                ? m_config.ironing_speed.value
                : m_config.filament_ironing_speed.get_at(filament_idx);
        } else if (path.role() == erBottomSurface) {
            speed = NOZZLE_CONFIG(initial_layer_infill_speed);
        } else if (path.role() == erGapFill) {
            speed = NOZZLE_CONFIG(gap_infill_speed);
        } else if (path.role() == erSupportMaterial) {
            speed = NOZZLE_CONFIG(support_speed);
        } else if (path.role() == erSupportMaterialInterface) {
            speed = NOZZLE_CONFIG(support_interface_speed);
        } else {
            throw Slic3r::InvalidArgument("Invalid speed");
        }
    }
    double filament_max_volumetric_speed = FILAMENT_CONFIG(filament_max_volumetric_speed);
    if (FILAMENT_CONFIG(filament_adaptive_volumetric_speed)){
        double fitted_value = calc_max_volumetric_speed(path.height, path.width, FILAMENT_CONFIG(volumetric_speed_coefficients));
        filament_max_volumetric_speed = std::min(filament_max_volumetric_speed, fitted_value);
    }

    if (speed == 0)
        speed = filament_max_volumetric_speed / _mm3_per_mm;
    const double curved_shell_speed = z_absolute && path.nonplanar_source ? 0.01 * m_config.nonplanar_curved_shell_speed.value : 1.;
    if (curved_shell_speed > 1.)
        speed *= curved_shell_speed;

    const int _layer = this->effective_layer_index_for_point(path_point_mm);
    const bool belt_band_tags   = m_enable_cooling_markers && m_config.belt_printer.value;
    const int  belt_band_layers = belt_band_tags ? m_config.close_fan_the_first_x_layers.get_at(m_writer.filament()->id()) : 0;
    auto tag_belt_band = [this, &gcode, belt_band_tags, belt_band_layers, z = path_point_mm.z()](coord_t x, coord_t y) {
        if (! belt_band_tags)
            return;
        const bool in_band = this->effective_layer_index_for_point(Vec3d(unscale<double>(x), unscale<double>(y), z)) < belt_band_layers;
        if (in_band != m_belt_in_band) {
            gcode += in_band ? ";_BELT_BAND_START\n" : ";_BELT_BAND_END\n";
            m_belt_in_band = in_band;
        }
    };
    if (path_on_first_layer || object_layer_over_raft()) {
        if (path.role() != erBottomSurface) {
            const bool use_first_layer_speed = is_perimeter(path.role()) || path.role() == erBrim;
            speed = use_first_layer_speed ? NOZZLE_CONFIG(initial_layer_speed) :
                                            NOZZLE_CONFIG(initial_layer_infill_speed);
        }
    } else if (m_config.slow_down_layers > 1 && m_config.raft_layers == 0) {
        if (_layer > 0 && _layer < m_config.slow_down_layers) {
            const auto first_layer_speed =
                is_perimeter(path.role())
                    ? NOZZLE_CONFIG(initial_layer_speed)
                    : NOZZLE_CONFIG(initial_layer_infill_speed);
            if (first_layer_speed < speed) {
                speed = std::min(
                    speed,
                    Slic3r::lerp(first_layer_speed, speed,
                                (double) (_layer) / m_config.slow_down_layers));
            }
        }
    } else if (m_config.slow_down_layers > 1 && m_config.raft_layers > 0 ) {
        
        if (_layer > m_config.raft_layers && (_layer - m_config.raft_layers) < m_config.slow_down_layers) {
            const auto first_layer_speed 
                = is_perimeter(path.role()) ? NOZZLE_CONFIG(initial_layer_speed) :
                                                                       NOZZLE_CONFIG(initial_layer_infill_speed);
            if (first_layer_speed < speed) {
                speed = std::min(speed, Slic3r::lerp(first_layer_speed, speed,
                                                     (double) (_layer - m_config.raft_layers) / m_config.slow_down_layers));
            }
        }
    }
    if (path.role() == erSkirt) {
        const double skirt_speed = m_config.skirt_speed.value;
        if (skirt_speed > 0.0)
        speed = skirt_speed;
    }
    if (FILAMENT_CONFIG(filament_max_volumetric_speed) > 0) {
        speed = std::min(speed, FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
    }
{
    double ref_speed = speed;
    if (path.role() == erExternalPerimeter
        && m_config.resonance_avoidance.value) {

        if (ref_speed > m_config.max_resonance_avoidance_speed.value) {
            m_resonance_avoidance = false;
        }

        if (FILAMENT_CONFIG(filament_max_volumetric_speed) > 0) {
            speed = std::min(
                speed,
                FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm
            );
        }

        if (m_resonance_avoidance && speed < m_config.max_resonance_avoidance_speed.value) {
            if (speed < m_config.min_resonance_avoidance_speed.value +
                            ((m_config.max_resonance_avoidance_speed.value - m_config.min_resonance_avoidance_speed.value) / 2)) {
                speed = std::min(speed, m_config.min_resonance_avoidance_speed.value);
            } else {
                speed = m_config.max_resonance_avoidance_speed.value;
            }
        }

        m_resonance_avoidance = true;
    }
}
    
    bool variable_speed = false;
    std::vector<ProcessedPoint> new_points {};

    const bool need_overhang_detection = NOZZLE_CONFIG(enable_overhang_speed) ||
        (FILAMENT_CONFIG(enable_overhang_bridge_fan) && m_enable_cooling_markers);

    if (!path.nonplanar_source && need_overhang_detection && !path_on_first_layer && !object_layer_over_raft() &&
        (is_bridge(path.role()) || is_perimeter(path.role())) && dynamic_cast<const ExtrusionPathConformal *>(&path) == nullptr) {
            bool is_external = is_external_perimeter(path.role());
            double ref_speed   = is_external ? NOZZLE_CONFIG(outer_wall_speed) : NOZZLE_CONFIG(inner_wall_speed);
            if (ref_speed == 0)
                ref_speed = FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm;

            if (FILAMENT_CONFIG(filament_max_volumetric_speed) > 0) {
                ref_speed = std::min(ref_speed, FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
            }
            if (sloped) {
                ref_speed = std::min(ref_speed, m_config.scarf_joint_speed.get_abs_value(ref_speed));
            }
            
            const ConfigOptionPercents overhang_overlap_levels = OverhangSpeedSections::overlap_levels();

            float fan_overlap_threshold = -1.f;
            if (FILAMENT_CONFIG(enable_overhang_bridge_fan) && m_enable_cooling_markers && path.role() != erBridgeInfill &&
                path.role() != erOverhangPerimeter)
                fan_overlap_threshold = overhang_fan_overlap_threshold(FILAMENT_CONFIG(overhang_fan_threshold));

            new_points = m_extrusion_quality_estimator.estimate_extrusion_quality(path, overhang_overlap_levels,
                                                                                  this->overhang_speed_table(ref_speed), ref_speed, speed,
                                                                                  NOZZLE_CONFIG(slowdown_for_curled_perimeters),
                                                                                  fan_overlap_threshold);
            variable_speed = std::any_of(new_points.begin(), new_points.end(),
                                         [speed](const ProcessedPoint &p) { return fabs(double(p.speed) - speed) > 1; });
            if (FILAMENT_CONFIG(enable_overhang_bridge_fan) && m_enable_cooling_markers) {
                if (!NOZZLE_CONFIG(enable_overhang_speed))
                    for (ProcessedPoint &point : new_points)
                        point.speed = speed;
                variable_speed = new_points.size() > 1;
            }
    }

    // Wave overhangs (OrcaSlicer-WaveOverhangs, AGPL-3.0): wave lines and the walls and floors resting on
    const double wave_speed = path.wave_overhang                 ? m_config.wave_overhang_print_speed.value :
                              path.wave_overhang_floor           ? m_config.wave_overhang_floor_print_speed.value :
                              path.wave_overhang_perimeter       ? m_config.wave_overhang_perimeter_speed.value :
                              path.wave_overhang_floor_perimeter ? m_config.wave_overhang_floor_perimeter_speed.value : 0.;
    if (wave_speed > 0.) {
        const int ramp     = m_config.wave_overhang_floor_speed_ramp.value;
        const int distance = path.wave_overhang_floor_distance;
        const bool floor   = path.wave_overhang_floor || path.wave_overhang_floor_perimeter;
        const double t     = floor && ramp > 0 && distance > 1 ? std::min(1., double(distance - 1) / double(std::max(1, ramp - 1))) : 0.;
        speed = wave_speed + (speed - wave_speed) * t;
        if (FILAMENT_CONFIG(filament_max_volumetric_speed) > 0 && _mm3_per_mm > 0)
            speed = std::min(speed, FILAMENT_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
        variable_speed = false;
        new_points.clear();
    }

    double F = speed * 60;
    const unsigned int pa_config_index = (unsigned int) get_filament_config_index(m_writer.filament()->id());
    const bool adaptive_pa = m_config.adaptive_pressure_advance.get_at(pa_config_index) && m_config.enable_pressure_advance.get_at(pa_config_index);
    const bool adaptive_pa_overhangs = adaptive_pa && m_config.adaptive_pressure_advance_overhangs.get_at(pa_config_index);
    bool is_pa_calib = m_curr_print->calib_mode() == CalibMode::Calib_PA_Line ||
                       m_curr_print->calib_mode() == CalibMode::Calib_PA_Pattern ||
                       m_curr_print->calib_mode() == CalibMode::Calib_PA_Tower;
    bool evaluate_adaptive_pa = false;
    bool role_change = (m_last_extrusion_role != path.role());
    if (!is_pa_calib && adaptive_pa) {
        evaluate_adaptive_pa = true;
        if (m_multi_flow_segment_path_pa_set && evaluate_adaptive_pa)
            evaluate_adaptive_pa = false;
        if(role_change)
            evaluate_adaptive_pa = true;
    }
    if (path.role() != m_last_extrusion_role) {
        const auto current_filament_id = m_writer.filament()->id();
        const std::string& machine_role_change_gcode  = m_config.change_extrusion_role_gcode.value;
        const std::string& filament_role_change_gcode = m_config.filament_change_extrusion_role_gcode.get_at(current_filament_id);
        const std::string& process_role_change_gcode  = m_config.process_change_extrusion_role_gcode.value;

        if (!machine_role_change_gcode.empty() || !filament_role_change_gcode.empty() || !process_role_change_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("extrusion_role", new ConfigOptionString(extrusion_role_to_string_for_parser(path.role())));
            config.set_key_value("last_extrusion_role", new ConfigOptionString(extrusion_role_to_string_for_parser(m_last_extrusion_role)));
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
            config.set_key_value("layer_z", new ConfigOptionFloat(m_layer == nullptr ? m_last_height : m_layer->print_z));

            const auto append_role_gcode = [this, current_filament_id, &config, &gcode](const std::string& key, const std::string& templ) {
                if (templ.empty())
                    return;
                gcode += this->placeholder_parser_process(key, templ, current_filament_id, &config) + "\n";
            };

            append_role_gcode("change_extrusion_role_gcode", machine_role_change_gcode);
            append_role_gcode("filament_change_extrusion_role_gcode", filament_role_change_gcode);
            append_role_gcode("process_change_extrusion_role_gcode", process_role_change_gcode);
        }
    }

    if (m_enable_extrusion_role_markers) {
        if (path.role() != m_last_extrusion_role) {
            char buf[32];
            sprintf(buf, ";_EXTRUSION_ROLE:%d\n", int(path.role()));
            gcode += buf;
      }
    }

    m_last_extrusion_role = path.role();

    bool last_was_wipe_tower = (m_last_processor_extrusion_role == erWipeTower);
    char buf[64];
    assert(is_decimal_separator_point());

    if (path.role() != m_last_processor_extrusion_role) {
        m_last_processor_extrusion_role = path.role();
        sprintf(buf, ";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), ExtrusionEntity::role_to_string(m_last_processor_extrusion_role).c_str());
        gcode += buf;
    }

    if (last_was_wipe_tower || m_last_width != path.width) {
        m_last_width = path.width;
        sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width).c_str(), m_last_width);
        gcode += buf;
    }

    if (last_was_wipe_tower || std::abs(m_last_height - effective_height) > EPSILON) {
        m_last_height = effective_height;
        sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), m_last_height);
        gcode += buf;
    }
    
    if (evaluate_adaptive_pa) {
        bool isOverhangPerimeter = (path.role() == erOverhangPerimeter);
        if (m_multi_flow_segment_path_average_mm3_per_mm > 0) {
            sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                    GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                    pa_config_index,
                    m_multi_flow_segment_path_average_mm3_per_mm,
                    acceleration_i,
                    ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                    role_change,
                    isOverhangPerimeter);
            gcode += buf;
        } else if(_mm3_per_mm >0 ){
            sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                    GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                    pa_config_index,
                    _mm3_per_mm,
                    acceleration_i,
                    ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                    role_change,
                    isOverhangPerimeter);
            gcode += buf;
        }
    }


    auto overhang_fan_threshold = FILAMENT_CONFIG(overhang_fan_threshold);
    auto enable_overhang_bridge_fan = FILAMENT_CONFIG(enable_overhang_bridge_fan);

    auto check_overhang_fan = [&overhang_fan_threshold](float overlap, ExtrusionRole role) {
      if (role == erBridgeInfill || role == erOverhangPerimeter) {
        return true;
      }
      if (overhang_fan_threshold == Overhang_threshold_none)
        return is_external_perimeter(role);
      const float overlap_threshold = overhang_fan_overlap_threshold(overhang_fan_threshold);
      return overlap_threshold >= 0.f && overlap <= overlap_threshold;
    };

    std::string comment;
    if (m_enable_cooling_markers) {
        comment = ";_EXTRUDE_SET_SPEED";
        if (is_external_perimeter(path.role()))
            comment += ";_EXTERNAL_PERIMETER";
    }

    auto append_role_based_fan_marker = [this, &gcode](const ExtrusionRole role, const std::string_view& marker_prefix, const bool fan_on) {
        assert(m_enable_cooling_markers);

        if (fan_on) {
            if (!m_is_role_based_fan_on[role] || m_role_based_fan_marker_layer[role] != m_layer_index) {
                gcode += ";";
                gcode += marker_prefix;
                gcode += "_FAN_START\n";
                m_is_role_based_fan_on[role] = true;
                m_role_based_fan_marker_layer[role] = m_layer_index;
            }
        } else {
            if (m_is_role_based_fan_on[role]) {
                gcode += ";";
                gcode += marker_prefix;
                gcode += "_FAN_END\n";
                m_is_role_based_fan_on[role] = false;
                m_role_based_fan_marker_layer[role] = -1;
            }
        }
    };
    auto apply_role_based_fan_speed = [
        &path, &append_role_based_fan_marker,
        supp_interface_fan_speed = FILAMENT_CONFIG(support_material_interface_fan_speed),
        ironing_fan_speed        = FILAMENT_CONFIG(ironing_fan_speed)
    ] {
        append_role_based_fan_marker(erSupportMaterialInterface, "_SUPP_INTERFACE"sv,
                                     supp_interface_fan_speed >= 0 && path.role() == erSupportMaterialInterface);
        append_role_based_fan_marker(erIroning, "_IRONING"sv,
                                     ironing_fan_speed >= 0 && path.role() == erIroning);
    };

    auto check_and_insert_timelapse = [this, &gcode](const Point &endpoint_scaled) {
        if (!m_farthest_point_timelapse.enabled || !m_farthest_point_timelapse.farthest_is_photo_head || m_farthest_point_timelapse.inserted_this_layer)
            return;
        if (!m_writer.filament() || get_extruder_id(m_writer.filament()->id()) != m_farthest_point_timelapse.most_used_extruder)
            return;
        Vec2d endpoint_mm = unscale(endpoint_scaled) + m_origin;
        if ((endpoint_mm - m_farthest_point_timelapse.farthest_gcode_pos).norm() >= 0.5)
            return;
        if (!m_print || !m_layer)
            return;
        std::string timelapse_gcode = generate_timelapse_gcode(*m_print, m_layer->print_z, m_farthest_point_timelapse.most_used_extruder,
                                                               &m_farthest_point_timelapse.layer_object_label_ids, &m_printed_objects, true);
        if (!timelapse_gcode.empty()) {
            gcode += timelapse_gcode;
            m_farthest_point_timelapse.inserted_this_layer = true;
        }
    };

    if (!variable_speed) {
        if( (std::abs(writer().get_current_speed() - F) > EPSILON) || (std::abs(_mm3_per_mm - m_last_mm3_mm) > EPSILON) ){
            if(_mm3_per_mm >0 && adaptive_pa_overhangs && !evaluate_adaptive_pa){
                if(writer().get_current_speed() > F){
                    if(m_config.gcode_comments){
                        sprintf(buf, "; Ramp down-non-variable\n");
                        gcode += buf;
                    }
                    sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                            pa_config_index,
                            _mm3_per_mm,
                            acceleration_i,
                            ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                            1,
                            1);
                }else{
                    if(m_config.gcode_comments){ 
                        sprintf(buf, "; Ramp up-non-variable\n");
                        gcode += buf;
                    }
                    sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                            pa_config_index,
                            _mm3_per_mm,
                            acceleration_i,
                            ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                            1,
                            0);
                }
                gcode += buf;
                m_last_mm3_mm = _mm3_per_mm;
            }
        }
        
        m_writer.set_speed(gcode, F, "", comment);
        {
            if (m_enable_cooling_markers) {
                if (enable_overhang_bridge_fan) {
                    append_role_based_fan_marker(erOverhangPerimeter, "_OVERHANG"sv,
                                                 (overhang_fan_threshold == Overhang_threshold_none && is_external_perimeter(path.role())) ||
                                                 (path.role() == erBridgeInfill || path.role() == erOverhangPerimeter));
                    append_role_based_fan_marker(erInternalBridgeInfill, "_INTERNAL_BRIDGE"sv, path.role() == erInternalBridgeInfill);
                }

                apply_role_based_fan_speed();
            }
            if (const auto *conformal = dynamic_cast<const ExtrusionPathConformal *>(&path)) {
                this->emit_conformal_moves(gcode, *conformal, e_per_mm, speed, comment, description);
            } else if (z_absolute) {
                std::vector<Vec3d> points;
                points.reserve(path.polyline.points.size());
                for (const Point3 &p : path.polyline.points)
                    points.emplace_back(absolute_point(p));
                auto lowest = [](const ConfigOptionFloats &opt) {
                    double limit = 0.;
                    for (double value : opt.values)
                        if (value > 0.) limit = limit > 0. ? std::min(limit, value) : value;
                    return limit;
                };
                auto min_positive = [](double a, double b) { return a > 0. && b > 0. ? std::min(a, b) : std::max(a, b); };
                const GCodeFlavor flavor = m_config.gcode_flavor.value;
                const bool emitted = flavor == gcfKlipper || (m_config.emit_machine_limits_to_gcode.value &&
                    (flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware || flavor == gcfRepRapFirmware));
                Nonplanar::MotionLimits limits;
                limits.max_v_xy = min_positive(lowest(m_config.machine_max_speed_x), lowest(m_config.machine_max_speed_y));
                limits.max_v_z = min_positive(this->conformal_max_speed_z(), (emitted ? 0.99 : 0.8) * lowest(m_config.machine_max_speed_z));
                limits.max_a_z = lowest(m_config.machine_max_acceleration_z);
                limits.max_a_xy = min_positive(lowest(m_config.machine_max_acceleration_x), lowest(m_config.machine_max_acceleration_y));
                limits.max_jerk_xy = min_positive(lowest(m_config.machine_max_jerk_x), lowest(m_config.machine_max_jerk_y));
                limits.max_jerk_z = lowest(m_config.machine_max_jerk_z);
                if (flavor == gcfKlipper && limits.max_a_xy > 0.)
                    limits.junction_deviation = sqr(limits.max_jerk_xy) * (std::sqrt(2.) - 1.) / limits.max_a_xy;
                else if (flavor == gcfMarlinFirmware && !m_config.machine_max_junction_deviation.values.empty())
                    limits.junction_deviation = m_config.machine_max_junction_deviation.values.front();
                limits.segment_rate = 0.99 * this->conformal_segment_rate();
                const std::vector<double> feeds = Nonplanar::limit_feedrates(points, F / 60., limits);
                Vec3d prev3 = points.front();
                for (size_t i = 1; i < points.size(); ++i) {
                    const Vec3d &p3 = points[i];
                    check_nonplanar_step(prev3, p3);
                    const double line_length = (p3 - prev3).norm();
                    if (line_length < EPSILON)
                        continue;
                    double feed = 60. * feeds[i - 1];
                    if (limits.segment_rate > 0.)
                        feed = std::min(feed, 60. * limits.segment_rate * line_length);
                    if (curved_shell_speed < 1.)
                        feed *= curved_shell_speed;
                    m_writer.set_speed(gcode, feed);
                    const double charged_length = (p3 - prev3).head<2>().norm();
                    gcode += m_writer.extrude_to_xyz(p3, e_per_mm * charged_length,
                                                     GCodeWriter::full_gcode_comment ? description : "", path.is_force_no_extrusion());
                    prev3 = p3;
                }
            } else if (!m_config.enable_arc_fitting || path.polyline.fitting_result.empty() || m_config.spiral_mode || sloped != nullptr || path.z_contoured || this->should_disable_arc_fitting()) {
                double path_length = 0.;
                double total_length = sloped == nullptr ? 0. : path.polyline.length() * SCALING_FACTOR;
                double saved_z      = m_writer.get_position().z();

                const double contoured_feed = m_writer.get_current_speed();
                for (const Line3& line : path.polyline.lines()) {
                    std::string flow_description;
                    const double line_length = path.z_contoured
                        ? Nonplanar::charge_length(line.a.cast<double>(), line.b.cast<double>()) * SCALING_FACTOR
                        : line.length() * SCALING_FACTOR;
                    if (line_length < EPSILON)
                        continue;
                    path_length += line_length;
                    auto dE = e_per_mm * line_length;
                    if (_needSAFC(path)) {
                        auto oldE = dE;
                        dE = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                        if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                            flow_description = description + Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, line_length);
                        }
                    }
                    tag_belt_band((line.a.x() + line.b.x()) / 2, (line.a.y() + line.b.y()) / 2);
                    if (path.z_contoured) {
                        const Vec2d from = this->point_to_gcode(line.a.to_point()), to = this->point_to_gcode(line.b.to_point());
                        extrude_contoured_road(m_writer, gcode, Vec3d(from.x(), from.y(), unscale_(line.a.z())),
                                               Vec3d(to.x(), to.y(), unscale_(line.b.z())), m_nominal_z, dE, path.width, path.height, path.contour_base,
                                               scales_contoured_flow(path, m_layer), flow_description.empty() ? description : flow_description,
                                               this->conformal_max_speed_z(), contoured_feed, !Nonplanar::requests_conformal_shells(*m_print));
                    } else if (sloped == nullptr) {
                        m_writer.extrude_to_xy(gcode,
                            this->point_to_gcode(line.b.to_point()),
                            dE,
                            flow_description.empty() ? description : flow_description, path.is_force_no_extrusion());
                    } else {
                        const auto [z_ratio, e_ratio] = sloped->interpolate(path_length / total_length);
                        Vec2d dest2d = this->point_to_gcode(line.b.to_point());
                        Vec3d dest3d(dest2d(0), dest2d(1), get_sloped_z(z_ratio));
                        m_writer.extrude_to_xyz(gcode,
                            dest3d,
                            dE * e_ratio,
                            flow_description.empty() ? description : flow_description, path.is_force_no_extrusion(), false);
                    }
                    check_and_insert_timelapse(line.b.to_point());
                }
                if (path.z_contoured && std::abs(m_writer.get_current_speed() - contoured_feed) > EPSILON)
                    m_writer.set_speed(gcode, contoured_feed);
            } else {
                const std::vector<PathFittingData>& fitting_result = path.polyline.fitting_result;
                for (size_t fitting_index = 0; fitting_index < fitting_result.size(); fitting_index++) {
                    std::string flow_description;
                    switch (fitting_result[fitting_index].path_type) {
                    case EMovePathType::Linear_move: {
                        size_t start_index = fitting_result[fitting_index].start_point_index;
                        size_t end_index = fitting_result[fitting_index].end_point_index;
                        for (size_t point_index = start_index + 1; point_index < end_index + 1; point_index++) {
                            flow_description.clear();
                            const Line line = Line(path.polyline.points[point_index - 1].to_point(), path.polyline.points[point_index].to_point());
                            const double line_length = line.length() * SCALING_FACTOR;
                            if (line_length < EPSILON)
                                continue;
                            auto dE = e_per_mm * line_length;
                            if (_needSAFC(path)) {
                                auto oldE = dE;
                                dE = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                                if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                                    flow_description = description + Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, line_length);
                                }
                            }
                            m_writer.extrude_to_xy(gcode,
                                this->point_to_gcode(line.b),
                                dE,
                                flow_description.empty() ? description : flow_description, path.is_force_no_extrusion());
                            check_and_insert_timelapse(line.b);
                        }
                        break;
                    }
                    case EMovePathType::Arc_move_cw:
                    case EMovePathType::Arc_move_ccw: {
                        const ArcSegment& arc = fitting_result[fitting_index].arc_data;
                        const double arc_length = fitting_result[fitting_index].arc_data.length * SCALING_FACTOR;
                        if (arc_length < EPSILON)
                            continue;
                        const Vec2d center_offset = this->point_to_gcode(arc.center) - this->point_to_gcode(arc.start_point);
                        auto dE = e_per_mm * arc_length;
                        if (_needSAFC(path)) {
                            auto oldE = dE;
                            dE = m_small_area_infill_flow_compensator->modify_flow(arc_length, dE, path.role());

                            if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                                flow_description = description + Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, arc_length);
                            }
                        }
                        m_writer.extrude_arc_to_xy(gcode,
                            this->point_to_gcode(arc.end_point),
                            center_offset,
                            dE,
                            arc.direction == ArcDirection::Arc_Dir_CCW,
                            flow_description.empty() ? description : flow_description, path.is_force_no_extrusion());
                        check_and_insert_timelapse(arc.end_point);
                        break;
                    }
                    default:
                        assert(0);
                        break;
                    }
                }
            }
        }
    } else {
        double last_set_speed = new_points[0].speed * 60.0;

        double total_length = 0;
        if (sloped != nullptr) {
            Points3 p;
            p.reserve(new_points.size());
            std::transform(new_points.begin(), new_points.end(), std::back_inserter(p), [](const ProcessedPoint& pp) { return pp.p; });
            Polyline3 l(p);
            total_length = l.length() * SCALING_FACTOR;
        }
        m_writer.set_speed(gcode, last_set_speed, "", comment);
        Vec3d prev            = this->point_to_gcode_quantized(new_points[0].p);
        bool pre_fan_enabled = false;
        bool cur_fan_enabled = false;
        if( m_enable_cooling_markers && enable_overhang_bridge_fan)
            pre_fan_enabled = check_overhang_fan(new_points[0].overlap, path.role());
        
        if(path.role() == erInternalBridgeInfill)
            pre_fan_enabled = true;

        double path_length = 0.;
        for (size_t i = 1; i < new_points.size(); i++) {
            std::string flow_description;
            const ProcessedPoint &processed_point = new_points[i];
            const ProcessedPoint &pre_processed_point = new_points[i-1];
            Vec3d                 p                   = this->point_to_gcode_quantized(processed_point.p);
            tag_belt_band((pre_processed_point.p.x() + processed_point.p.x()) / 2, (pre_processed_point.p.y() + processed_point.p.y()) / 2);
            if (m_enable_cooling_markers) {
                if (enable_overhang_bridge_fan) {
                    cur_fan_enabled = check_overhang_fan(processed_point.overlap, path.role());
                    append_role_based_fan_marker(erOverhangPerimeter, "_OVERHANG"sv, pre_fan_enabled && cur_fan_enabled);
                    pre_fan_enabled = cur_fan_enabled;

                    append_role_based_fan_marker(erInternalBridgeInfill, "_INTERNAL_BRIDGE"sv, path.role() == erInternalBridgeInfill);
                }

                apply_role_based_fan_speed();
            }

            const double line_length = path.z_contoured ? (p - prev).head<2>().norm() : (p - prev).norm();
            if(line_length < EPSILON)
                continue;
            path_length += line_length;
            double new_speed = pre_processed_point.speed * 60.0;
            
            if ((std::abs(last_set_speed - new_speed) > EPSILON) || (std::abs(_mm3_per_mm - m_last_mm3_mm) > EPSILON)) {
                if(_mm3_per_mm >0 && adaptive_pa_overhangs){
                    if(last_set_speed > new_speed){
                        if(m_config.gcode_comments) {
                            sprintf(buf, "; Ramp up-variable\n");
                            gcode += buf;
                        }
                        sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                                pa_config_index,
                                _mm3_per_mm,
                                acceleration_i,
                                ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                                1,
                                1);
                    }else{
                        if(m_config.gcode_comments) {
                            sprintf(buf, "; Ramp down-variable\n");
                            gcode += buf;
                        }
                        sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(),
                                pa_config_index,
                                _mm3_per_mm,
                                acceleration_i,
                                ((path.role() == erBridgeInfill) ||(path.role() == erOverhangPerimeter)),
                                1,
                                0);
                    }
                    gcode += buf;
                    m_last_mm3_mm = _mm3_per_mm;
                }
            }
            if ((std::abs(last_set_speed - new_speed) > 60)) {
                m_writer.set_speed(gcode, new_speed, "", comment);
                last_set_speed = new_speed;
            } else if ((std::abs(F - new_speed) <= 60)) {
                m_writer.set_speed(gcode, F, "", comment);
                last_set_speed = F;
            }
            auto dE = e_per_mm * line_length;
            if (_needSAFC(path)) {
                auto oldE = dE;
                dE = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                    flow_description = description + Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f",oldE, line_length);
                }
            }
            if (path.z_contoured) {
                extrude_contoured_road(m_writer, gcode, Vec3d(prev.x(), prev.y(), unscale_(pre_processed_point.p.z())),
                                       Vec3d(p.x(), p.y(), unscale_(processed_point.p.z())), m_nominal_z, dE, path.width, path.height, path.contour_base,
                                       scales_contoured_flow(path, m_layer), flow_description.empty() ? description : flow_description,
                                       this->conformal_max_speed_z(), last_set_speed, !Nonplanar::requests_conformal_shells(*m_print));
            } else if (sloped == nullptr) {
                m_writer.extrude_to_xy(gcode, p.head<2>(), dE, flow_description.empty() ? description : flow_description);
            } else {
                const auto [z_ratio, e_ratio] = sloped->interpolate(path_length / total_length);
                Vec3d dest3d(p(0), p(1), get_sloped_z(z_ratio));
                m_writer.extrude_to_xyz(gcode, dest3d, dE * e_ratio, flow_description.empty() ? description : flow_description,
                                        false, false);
            }

            check_and_insert_timelapse(processed_point.p.to_point());

            prev = p;

        }
    }
    if (m_enable_cooling_markers) {
            gcode += ";_EXTRUDE_END\n";
    }

    if (path.role() != ExtrusionRole::erGapFill) {
      m_last_notgapfill_extrusion_role = path.role();
    }

    auto deposited_z = [&](const Point3 &point) {
        return z_absolute ? unscale<double>(point.z()) + m_config.z_offset.value :
            path.z_contoured ? m_nominal_z + unscale<double>(point.z()) : m_nominal_z;
    };
    if ((curved_tops_mode(m_config.nonplanar_mode.value)) && ! path.is_force_no_extrusion()) {
        const double half_width = 0.5 * path.width + m_nonplanar_clearance.bead_margin;
        const Points3 &points = path.polyline.points;
        for (size_t i = 1; i < points.size(); ++ i)
            m_nonplanar_heights.deposit(this->point_to_gcode(points[i - 1].to_point()), this->point_to_gcode(points[i].to_point()),
                std::max(deposited_z(points[i - 1]), deposited_z(points[i])), half_width);
        if (z_absolute)
            for (const Point3 &point : points) {
                const double z = deposited_z(point);
                m_max_layer_z = std::max(m_max_layer_z, float(z - m_config.z_offset.value));
                m_nonplanar_ceiling = std::max(m_nonplanar_ceiling, z);
            }
        else if (path.z_contoured)
            for (const Point3 &point : points)
                m_max_layer_z = std::max(m_max_layer_z, float(deposited_z(point) - m_config.z_offset.value));
    }
    if (path.nonplanar_source) gcode += ";_NONPLANAR_END\n";
    m_nonplanar_printed_this_layer |= path.nonplanar_source;
    m_last_native_curved = z_absolute && path.nonplanar_source && !path.is_force_no_extrusion();
    if (m_material_columns && dynamic_cast<const ExtrusionPathConformal *>(&path) == nullptr) {
        const auto to_writer = [this, &path, z_absolute](const Point3 &p) {
            const Vec2d xy = this->point_to_gcode(p.to_point());
            const double z = z_absolute ? unscale<double>(p.z()) + m_config.z_offset.value :
                path.z_contoured ? m_nominal_z + unscale<double>(p.z()) : m_writer.get_position().z();
            return Vec3d(xy.x(), xy.y(), z);
        };
        std::function<double(double)> slope_z;
        if (sloped) slope_z = [sloped, &get_sloped_z](double t) { return get_sloped_z(sloped->interpolate(t).z_ratio); };
        Nonplanar::deposit_material_path(*m_material_columns, path, to_writer, slope_z);
        m_print_clock += unscale<double>(path.length()) / std::max(1., m_writer.get_current_speed() / 60.);
        if (path.z_contoured)
            for (const Point3 &p : path.polyline.points)
                m_nonplanar_ceiling = std::max(m_nonplanar_ceiling, to_writer(p).z());
    }
    this->set_last_pos(path.last_point());
    return gcode;
}

std::string GCode::extrusion_role_to_string_for_parser(const ExtrusionRole & role)
{
    switch (role) {
        case erPerimeter: return "Perimeter";
        case erExternalPerimeter: return "ExternalPerimeter";
        case erOverhangPerimeter: return "OverhangPerimeter";
        case erInternalInfill: return "InternalInfill";
        case erSolidInfill: return "SolidInfill";
        case erTopSolidInfill: return "TopSolidInfill";
        case erBottomSurface: return "BottomSurface";
        case erBridgeInfill:
        case erInternalBridgeInfill: return "BridgeInfill";
        case erGapFill: return "GapFill";
        case erIroning: return "Ironing";
        case erSkirt: return "Skirt";
        case erBrim: return "Brim";
        case erSupportMaterial: return "SupportMaterial";
        case erSupportMaterialInterface: return "SupportMaterialInterface";
        case erSupportTransition: return "SupportTransition";
        case erWipeTower: return "WipeTower";
        case erCustom:
        case erMixed:
        case erCount:
        case erNone:
        default: return "Mixed";
    }
}

float GCode::interpolate_value_across_layers(float start_value, float end_value, float step) const
{
    float ratio;
    if (m_config.belt_printer.value && m_layer != nullptr && !m_layer->object()->layers().empty()) {
        const auto& layers = m_layer->object()->layers();
        double z_min = layers.front()->print_z;
        for (const Layer* l : layers)
            if (!l->lslices.empty()) { z_min = l->print_z; break; }
        const double z_max = layers.back()->print_z;
        if (m_layer->print_z <= z_min + EPSILON || z_max - z_min <= EPSILON)
            return start_value;
        ratio = float(std::min(1.0, (m_layer->print_z - z_min) / (z_max - z_min)));
    } else if (m_layer_index <= 1) {
        return start_value;
    } else {
        ratio = m_layer_index / (m_layer_count - 1.f);
    }
    if (step > 0.f) {
        const int n_bands = std::lround(std::abs(end_value - start_value) / step) + 1;
        const int band    = std::min(n_bands - 1, static_cast<int>(ratio * n_bands));
        return start_value + (end_value >= start_value ? 1.f : -1.f) * band * step;
    }
    return start_value + ratio * (end_value - start_value);
}

std::string encodeBase64(uint64_t value)
{
    uint8_t src[8];
    for (size_t i = 0; i < 8; i++)
        src[i] = (value >> (8 * i)) & 0xff;

    std::string dest;
    dest.resize(boost::beast::detail::base64::encoded_size(sizeof(src)));
    dest.resize(boost::beast::detail::base64::encode(&dest[0], src, sizeof(src)));
    return dest;
}

std::string GCode::object_exclude_start(const PrintInstance &inst)
{
    if (is_BBL_Printer()) {
        const size_t label_id = inst.model_instance->get_labeled_id();
        return "; start printing object, unique label id: " + std::to_string(label_id) + "\n" + "M624 " +
               _encode_label_ids_to_base64({ label_id }) + "\n";
    }
    const auto gflavor = m_config.gcode_flavor.value;
    if (gflavor == gcfKlipper)
        return "EXCLUDE_OBJECT_START NAME=" + instance_name(inst) + "\n";
    if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware)
        return "M486 S" + std::to_string(inst.unique_id) + "\n";
    return {};
}

std::string GCode::object_exclude_end(const PrintInstance &inst)
{
    if (is_BBL_Printer())
        return "; stop printing object, unique label id: " + std::to_string(inst.model_instance->get_labeled_id()) + "\nM625\n";
    const auto gflavor = m_config.gcode_flavor.value;
    if (gflavor == gcfKlipper)
        return "EXCLUDE_OBJECT_END NAME=" + instance_name(inst) + "\n";
    if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware)
        return "M486 S-1\n";
    return {};
}

std::string GCode::_encode_label_ids_to_base64(std::vector<size_t> ids)
{
    assert(m_label_objects_ids.size() < 64);

    uint64_t bitset = 0;
    for (size_t id : ids) {
        auto index = std::lower_bound(m_label_objects_ids.begin(), m_label_objects_ids.end(), id);
        if (index != m_label_objects_ids.end() && *index == id)
            bitset |= (1ull << (index - m_label_objects_ids.begin()));
        else
            throw Slic3r::LogicError("Unknown label object id!");
    }
    if (bitset == 0)
        throw Slic3r::LogicError("Label object id error!");

    return encodeBase64(bitset);
}

std::string GCode::travel_to(const Point& point, ExtrusionRole role, std::string comment, double z , const ExtrusionPath *native_target )
{
    const bool native_curved = curved_tops_mode(m_config.nonplanar_mode.value) && this->layer() != nullptr;
    const auto printed = [](const Vec3d &p) {
        return Vec3d(GCodeFormatter::quantize_xyzf(p.x()), GCodeFormatter::quantize_xyzf(p.y()), GCodeFormatter::quantize_xyzf(p.z()));
    };
    auto native_travel_speed = [this, &printed](const Vec3d &to, double limit) {
        const double length = (printed(to) - printed(m_writer.get_position())).norm();
        const double rate = this->conformal_segment_rate();
        return length > EPSILON && rate > 0. ? std::min(limit, 0.99 * rate * length) : limit;
    };
    bool nonplanar_travel = m_nonplanar_ceiling > 0.;
    Polyline travel { this->last_pos(), point };

    LiftType lift_type = LiftType::SpiralLift;
    bool needs_retraction = this->needs_retraction(travel, role, lift_type);

    const double target_z      = z == DBL_MAX ? m_nominal_z : z;
    bool         nonplanar_lift = false;
    bool         nonplanar_continuation = false;
    if (native_curved) {
        const double layer_z      = m_nominal_z;
        const bool   below_layer  = (m_writer.get_position().z() < layer_z - 0.0005 || target_z < layer_z - 0.0005) &&
            travel.length() > scale_(0.05);
        nonplanar_continuation = travel.length() <= scale_(0.05) &&
            std::abs(target_z - m_writer.get_position().z()) <= 0.0005;
        nonplanar_lift = (this->needs_zmove(travel, target_z) || m_force_safe_travel || below_layer || nonplanar_travel) &&
            !nonplanar_continuation;
    }
    if (native_curved)
        nonplanar_travel = false;
    bool could_be_wipe_disabled       = false;
    const bool used_external_mp_once  = m_avoid_crossing_perimeters.used_external_mp_once();
    std::string gcode;

    double jerk_to_set = 0.0;
    unsigned int acceleration_to_set = 0;
    
    if (this->on_first_layer()) {
        unsigned int initial_layer_travel_acceleration = m_config.get_abs_value_at("initial_layer_travel_acceleration", get_nozzle_config_index(m_writer.filament()->id()));
        double initial_layer_travel_jerk = m_config.get_abs_value_at("initial_layer_travel_jerk", get_nozzle_config_index(m_writer.filament()->id()));
    
        if (NOZZLE_CONFIG(default_acceleration) > 0 && initial_layer_travel_acceleration > 0) {
            acceleration_to_set = (unsigned int) floor(initial_layer_travel_acceleration + 0.5);
        }
        if (NOZZLE_CONFIG(default_jerk)> 0 && initial_layer_travel_jerk > 0) {
            jerk_to_set = initial_layer_travel_jerk;
        }
    } else {
        const bool is_short_travel = travel.length() < scale_(EXTRUDER_CONFIG(retraction_minimum_travel));

        if (NOZZLE_CONFIG(default_acceleration) > 0) {
            if (role == erOverhangPerimeter && is_short_travel) {
                const double bridge_acceleration  = m_config.get_abs_value_at("bridge_acceleration", get_nozzle_config_index(m_writer.filament()->id()));

                if (bridge_acceleration > 0)
                    acceleration_to_set = (unsigned int) floor(bridge_acceleration + 0.5);
            } else if (role == erExternalPerimeter && is_short_travel) {
                if (NOZZLE_CONFIG(outer_wall_acceleration) > 0)
                    acceleration_to_set = (unsigned int) floor(NOZZLE_CONFIG(outer_wall_acceleration) + 0.5);
            } else {
                if (NOZZLE_CONFIG(travel_acceleration) > 0)
                    acceleration_to_set = (unsigned int) floor(NOZZLE_CONFIG(travel_acceleration) + 0.5);
            }
        }

        if (NOZZLE_CONFIG(default_jerk) > 0) {
            if ((role == erExternalPerimeter || role == erOverhangPerimeter) && is_short_travel) {
                if (NOZZLE_CONFIG(outer_wall_jerk) > 0)
                    jerk_to_set = NOZZLE_CONFIG(outer_wall_jerk);
            } else {
                if (NOZZLE_CONFIG(travel_jerk) > 0)
                    jerk_to_set = NOZZLE_CONFIG(travel_jerk);
            }
        }
    }
    
    if (m_writer.get_gcode_flavor() == gcfKlipper) {
        gcode += m_writer.set_accel_and_jerk(acceleration_to_set, jerk_to_set);
    } else {
        gcode += m_writer.set_travel_acceleration(acceleration_to_set);
        gcode += m_writer.set_jerk_xy(jerk_to_set);
    }

    if (m_config.reduce_crossing_wall && ! nonplanar_continuation
        && !m_avoid_crossing_perimeters.disabled_once()
        && m_layer != nullptr
        && m_writer.is_current_position_clear())
    {
        travel = m_avoid_crossing_perimeters.travel_to(*this, point, &could_be_wipe_disabled);
        needs_retraction = this->needs_retraction(travel, role, lift_type);
    }

    if (native_curved && m_nonplanar_ceiling > 0. &&
        travel.length() >= scale_(FILAMENT_CONFIG(retraction_minimum_travel)))
        needs_retraction = true;

    const bool half_height_walls = native_curved && m_layer->object()->sublayer_walls_active();
    bool contoured_walls = half_height_walls && m_layer->object()->curved_fallback_zaa();
    if (half_height_walls)
        for (const PrintRegion &region : m_layer->object()->all_regions())
            contoured_walls |= region.config().zaa_enabled.value;
    const bool native_wall_link = half_height_walls && m_layer->object()->curved_surface_candidates() == 0 &&
        !contoured_walls &&
        std::any_of(m_layer->lslices.begin(), m_layer->lslices.end(),
            [&travel](const ExPolygon &slice) { return slice.contains(travel); });
    if (native_curved && !needs_retraction && !native_wall_link &&
        (half_height_walls || (!nonplanar_continuation &&
        travel.length() >= scale_(FILAMENT_CONFIG(retraction_minimum_travel)) &&
        std::any_of(m_layer->object()->nonplanar_surfaces().begin(), m_layer->object()->nonplanar_surfaces().end(),
            [this](const NonplanarSurface &surface) {
                return surface.stats.min.z - m_layer->height <= m_layer->slice_z && m_layer->slice_z <= surface.stats.max.z;
            })))) {
        const Vec3d from = m_writer.get_position();
        const double z = std::max(from.z(), target_z);
        Vec3d previous(from.x(), from.y(), z);
        for (size_t i = 1; i < travel.points.size(); ++i) {
            const Vec2d xy = this->point_to_gcode(travel.points[i]);
            const Vec3d next(xy.x(), xy.y(), z);
            if (!m_nonplanar_heights.supports_travel(previous, next, 0.4)) {
                needs_retraction = true;
                break;
            }
            previous = next;
        }
    }

    bool native_keep_primed = false;
    double primed_slope_lift = -1.;
    constexpr double primed_support_gap = 0.4;
    if (native_curved && native_target && m_last_native_curved && needs_retraction && !m_force_safe_travel &&
        !m_need_change_layer_lift_z && travel.size() == 2 &&
        (travel.length() < scale_(2.) || (travel.length() < scale_(5.) && native_target->polyline.length() < scale_(5.))) &&
        role == erTopSolidInfill && m_last_processor_extrusion_role == erTopSolidInfill) {
        const Vec3d from = m_writer.get_position();
        const Vec2d xy = this->point_to_gcode(travel.points.back());
        const double safe = m_nonplanar_heights.travel_height({from.head<2>(), xy}, m_nonplanar_clearance, std::max(from.z(), target_z));
        native_keep_primed = m_nonplanar_heights.supports_travel(Vec3d(from.x(), from.y(), safe), Vec3d(xy.x(), xy.y(), safe), primed_support_gap);
        if (!native_keep_primed && nonplanar_lift && m_writer.is_current_position_clear() && std::abs(from.z() - target_z) > 0.03) {
            const Vec3d a = printed(from), b = printed(Vec3d(xy.x(), xy.y(), target_z));
            const double lift = std::ceil((m_nonplanar_heights.travel_lift(a, b, m_nonplanar_clearance) + 0.000001) * 1000.) / 1000.;
            if (2. * lift < 2. * safe - a.z() - b.z() &&
                m_nonplanar_heights.supports_travel(a + Vec3d(0., 0., lift), b + Vec3d(0., 0., lift), primed_support_gap)) {
                native_keep_primed = true;
                primed_slope_lift = lift;
            }
        }
        if (native_keep_primed)
            needs_retraction = false;
    }

    m_avoid_crossing_perimeters.reset_once_modifiers();

    const bool travel_clear = nonplanar_travel &&
                              this->nonplanar_travel_clear(travel, std::min(z == DBL_MAX ? m_nominal_z : z, m_writer.get_position().z()),
                                                           !needs_retraction);
    if (travel_clear)
        nonplanar_travel = false;
    if (nonplanar_travel && planar_beside_curved_tops(m_layer, !m_nonplanar_deferral.empty(), m_config.nonplanar_mode.value, bool(m_material_columns), m_print)) {
        std::vector<Vec2d> xy;
        for (const Point &p : travel.points) xy.emplace_back(this->point_to_gcode(p));
        const double from_z = std::max(m_writer.get_position().z(), z == DBL_MAX ? m_nominal_z : z);
        if (m_nonplanar_heights.travel_height(xy, m_nonplanar_clearance, from_z) <= from_z + m_nonplanar_clearance.safety_mm + EPSILON)
            nonplanar_travel = false;
    }

    if (needs_retraction || nonplanar_travel) {

        Point last_post_before_retract = this->last_pos();
        gcode += this->retract(false, false, lift_type, false, role);
        if (last_post_before_retract != this->last_pos() && m_config.reduce_crossing_wall
            && m_layer != nullptr) {
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.use_external_mp_once();
            travel = m_avoid_crossing_perimeters.travel_to(*this, point);
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.reset_once_modifiers();
        }
    } else {
        m_wipe.reset_path();
    }


    m_writer.add_object_change_labels(gcode);

    if (travel.size() >= 2 && nonplanar_lift) {
        double hop = 0.;
        for (size_t i = 0; i < m_config.z_hop.size(); ++i) hop = std::max(hop, m_config.z_hop.get_at(i));
        std::vector<Vec2d> xy;
        for (const Point &p : travel.points) xy.emplace_back(this->point_to_gcode(p));
        const double base = std::max(m_writer.get_position().z(), target_z);
        double safe = m_nonplanar_heights.travel_height(xy, m_nonplanar_clearance, base);
        if (m_force_safe_travel)
            safe = m_nonplanar_heights.travel_height(xy, m_nonplanar_clearance,
                base + hop);
        bool sloped = false;
        double lift = 0.;
        const bool short_hop = m_layer != nullptr && curved_tops_mode(m_layer->object()->config().nonplanar_mode.value) &&
                               travel.size() == 2 && travel.length() < scale_(2.);
        if (travel.size() == 2 && !m_need_change_layer_lift_z &&
            (short_hop || (!m_force_safe_travel && m_writer.is_current_position_clear() && std::abs(target_z - m_writer.get_position().z()) > 0.03))) {
            const Vec3d from = printed(m_writer.get_position());
            const Vec3d to = printed(Vec3d(xy.back().x(), xy.back().y(), target_z));
            lift = primed_slope_lift >= 0. ? primed_slope_lift :
                std::ceil((m_nonplanar_heights.travel_lift(from, to, m_nonplanar_clearance) + 0.000001) * 1000.) / 1000.;
            if (short_hop)
                lift = 0.;
            sloped = primed_slope_lift >= 0. || 2. * lift < 2. * safe - from.z() - to.z();
            if (sloped && native_keep_primed)
                sloped = m_nonplanar_heights.supports_travel(from + Vec3d(0., 0., lift), to + Vec3d(0., 0., lift), primed_support_gap);
            if (sloped)
                safe = from.z() + lift;
        }
        m_writer.cancel_lazy_lift();
        if (safe > m_writer.get_position().z() + EPSILON || m_need_change_layer_lift_z || !m_writer.is_current_position_clear())
            gcode += m_writer.travel_to_z(safe, "lift over curved tops", true,
                native_travel_speed(Vec3d(m_writer.get_position().x(), m_writer.get_position().y(), safe), this->conformal_max_speed_z()));
        for (size_t i = 1; i < travel.size(); ++i) {
            const Vec2d xy_to = this->point_to_gcode(travel.points[i]);
            const Vec3d to(xy_to.x(), xy_to.y(), sloped ? target_z + lift : safe);
            gcode += m_writer.travel_to_xyz(to, comment, true, this->conformal_max_speed_z(),
                native_travel_speed(to, NOZZLE_CONFIG(travel_speed)));
        }
        if (std::abs(target_z - m_writer.get_position().z()) > EPSILON)
            gcode += m_writer.travel_to_z(target_z, "descend onto curved tops", true,
                native_travel_speed(Vec3d(m_writer.get_position().x(), m_writer.get_position().y(), target_z), this->conformal_max_speed_z()));
        m_need_change_layer_lift_z = false;
        this->set_last_pos(travel.points.back());
        if (m_force_safe_travel_once) m_force_safe_travel = m_force_safe_travel_once = false;
        return gcode;
    }

    if (!native_curved && travel.size() >= 2 && !travel_clear && this->below_conformal_material()) {
        const Vec2d from = this->point_to_gcode(travel.points.front()), to = this->point_to_gcode(travel.points.back());
        const double z_to = z == DBL_MAX ? m_nominal_z : z;
        if (const double safe = this->nonplanar_travel_z(&from, &to); safe > z_to + EPSILON) {
            if (!needs_retraction)
                gcode += this->retract(false, false, LiftType::NormalLift, false, role);
            gcode += m_writer.travel_to_z(std::max(safe, m_writer.get_position().z()), "lift over conformal shells", true,
                                          this->conformal_max_speed_z());
            gcode += m_writer.travel_to_xy(to, comment);
            gcode += m_writer.travel_to_z(z_to, comment, true, this->conformal_max_speed_z());
            m_need_change_layer_lift_z = false;
            this->set_last_pos(travel.points.back());
            return gcode;
        }
    }

    if (travel.size() >= 2) {
        if (nonplanar_travel) {
            gcode += this->travel_above_nonplanar(travel, comment, z);
        } else if (false ) {
            for (size_t i = 1; i < travel.size(); ++i) {
                gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
            }
        } else {
            if (travel.size() == 2) {
                const auto& dest2d = this->point_to_gcode(travel.points.back());
                Vec3d dest3d(dest2d(0), dest2d(1), z == DBL_MAX ? m_nominal_z : z);
                if (!needs_retraction && z != DBL_MAX && z < m_writer.get_position().z() - EPSILON &&
                    Nonplanar::requests_conformal_shells(*m_print))
                    gcode += m_writer.travel_to_xy(dest2d, comment);
                gcode += native_curved ? m_writer.travel_to_xyz(dest3d, comment, true, this->conformal_max_speed_z(),
                    native_travel_speed(dest3d, NOZZLE_CONFIG(travel_speed))) :
                    m_writer.travel_to_xyz(dest3d, comment, m_need_change_layer_lift_z);
                m_need_change_layer_lift_z = false;
            } else {
                for (size_t i = 1; i < travel.size(); ++i) {
                    if (i == 1) {
                        Vec2d dest2d = this->point_to_gcode(travel.points[i]);
                        Vec3d dest3d(dest2d(0), dest2d(1), m_nominal_z);
                        gcode += m_writer.travel_to_xyz(dest3d, comment, m_need_change_layer_lift_z);
                        m_need_change_layer_lift_z = false;
                    } else if (z != DBL_MAX && i == travel.size() - 1) {
                        Vec2d dest2d = this->point_to_gcode(travel.points[i]);
                        Vec3d dest3d(dest2d(0), dest2d(1), z);
                        gcode += m_writer.travel_to_xyz(dest3d, comment);
                    } else {
                        gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
                    }
                }
            }
        }
        this->set_last_pos(travel.points.back());
    }
    if (m_force_safe_travel && m_force_safe_travel_once)
        m_force_safe_travel = m_force_safe_travel_once = false;

    return gcode;
}

bool GCode::needs_zmove(const Polyline &travel, double target_z)
{
    if (std::abs(target_z - m_writer.get_position().z()) > 0.0005)
        return true;
    if (m_nonplanar_printed_this_layer && travel.length() >= scale_(1.0))
        return true;
    return false;
}

LiftType GCode::to_lift_type(ZHopType z_hop_types) {
    switch (z_hop_types)
    {
    case ZHopType::zhtNormal:
        return LiftType::NormalLift;
    case ZHopType::zhtSlope:
        return LiftType::SlopeLift;
    case ZHopType::zhtSpiral:
        return LiftType::SpiralLift;
    default:
        return LiftType::NormalLift;
    }
};

bool GCode::needs_retraction(const Polyline &travel, ExtrusionRole role, LiftType& lift_type)
{
    if (travel.length() < scale_(FILAMENT_CONFIG(retraction_minimum_travel))) {
        return false;
    }

    auto is_through_overhang = [this](const Polyline& travel) {
        BoundingBox travel_bbox = get_extents(travel);
        travel_bbox.inflated(1);
        travel_bbox.defined = true;

        const float protect_z = 0.4;
        std::pair<float, float> z_range;
        z_range.second = m_layer ? m_layer->print_z : 0.f;
        z_range.first = std::max(0.f, z_range.second - protect_z);
        std::vector<LayerPtrs> layers_of_objects;
        std::vector<BoundingBox> boundingBox_for_objects;
        VecOfPoints objects_instances_shift;
        std::vector<size_t> idx_of_object_sorted = m_curr_print->layers_sorted_for_object(z_range.first, z_range.second, layers_of_objects, boundingBox_for_objects, objects_instances_shift);

        std::vector<bool> is_layers_of_objects_sorted(layers_of_objects.size(), false);

        for (size_t idx : idx_of_object_sorted) {
            for (const Point & instance_shift : objects_instances_shift[idx]) {
                BoundingBox instance_bbox = boundingBox_for_objects[idx];
                if (!instance_bbox.defined)
                    continue;

                instance_bbox.offset(scale_(EPSILON));
                instance_bbox.translate(instance_shift.x(), instance_shift.y());
                if (!instance_bbox.overlap(travel_bbox))
                    continue;

                Polygons temp;
                temp.emplace_back(instance_bbox.polygon());
                if (intersection_pl(travel, temp).empty())
                    continue;

                if (!is_layers_of_objects_sorted[idx]) {
                    std::sort(layers_of_objects[idx].begin(), layers_of_objects[idx].end(), [](auto left, auto right) { return left->loverhangs_bbox.area() > right->loverhangs_bbox.area();});
                    is_layers_of_objects_sorted[idx] = true;
                }

                for (const auto& layer : layers_of_objects[idx]) {
                    for (ExPolygon overhang : layer->loverhangs) {
                        overhang.translate(instance_shift);
                        BoundingBox bbox1 = get_extents(overhang);

                        if (!bbox1.overlap(travel_bbox))
                            continue;

                        if (intersection_pl(travel, overhang).empty())
                            continue;

                        return true;
                    }
                }
            }
        }
        return false;
    };

    float max_z_hop = 0.f;
    for (int i = 0; i < m_config.z_hop.size(); i++)
        max_z_hop = std::max(max_z_hop, (float)m_config.z_hop.get_at(i));
    float travel_len_thresh = scale_(max_z_hop / tan(this->writer().filament()->travel_slope()));
    float accum_len = 0.f;
    Polyline clipped_travel;

    clipped_travel.append(Polyline(travel.points[0], travel.points[1]));
    if (clipped_travel.length() > travel_len_thresh)
        clipped_travel.points.back() = clipped_travel.points.front()+(clipped_travel.points.back() - clipped_travel.points.front()) * (travel_len_thresh / clipped_travel.length());
    clipped_travel.translate(Point::new_scale(double(m_origin.x() - m_writer.get_xy_offset().x()), double(m_origin.y() - m_writer.get_xy_offset().y())));

    if (is_perimeter(m_last_processor_extrusion_role) && m_last_processor_extrusion_role != erPerimeter) {
        if (ZHopType(FILAMENT_CONFIG(z_hop_types)) == ZHopType::zhtAuto) {
            lift_type = is_through_overhang(clipped_travel) ? LiftType::SpiralLift : LiftType::SlopeLift;
        }
        else {
            lift_type = to_lift_type(ZHopType(FILAMENT_CONFIG(z_hop_types)));
        }
        return true;
    }

    if (role == erSupportMaterial || role == erSupportTransition) {
        const SupportLayer* support_layer = dynamic_cast<const SupportLayer*>(m_layer);
        if (support_layer != NULL)
            for (const ExPolygon& support_island : support_layer->support_islands)
                if (support_island.contains(travel))
                    return false;
        if (support_layer != NULL && support_layer->support_type==stInnerTree)
            for (auto &area : support_layer->base_areas)
                if (area.contains(travel))
                    return false;
    }
    if (!is_perimeter(role) && m_config.reduce_infill_retraction && m_layer != nullptr &&
        m_config.sparse_infill_density.value > 0 && m_retract_when_crossing_perimeters.travel_inside_internal_regions(*m_layer, travel))
        return false;

    if (ZHopType(FILAMENT_CONFIG(z_hop_types)) == ZHopType::zhtAuto) {
        lift_type = is_through_overhang(clipped_travel) ? LiftType::SpiralLift : LiftType::SlopeLift;
    }
    else {
        lift_type = to_lift_type(ZHopType(FILAMENT_CONFIG(z_hop_types)));
    }
    return true;
}

std::string GCode::retract(bool toolchange, bool is_last_retraction, LiftType lift_type, bool apply_instantly, ExtrusionRole role)
{
    std::string gcode;

    const bool conformal_layers = m_layer != nullptr && m_layer->object() != nullptr &&
                                  (m_layer->object()->config().nonplanar_mode.value == NonplanarMode::ConformalLayers ||
                                   m_layer->object()->config().nonplanar_mode.value == NonplanarMode::Slant) &&
                                  std::any_of(m_layer->object()->conformal().patches.begin(), m_layer->object()->conformal().patches.end(),
                                              [](const Nonplanar::ConformalPatch &patch) { return patch.native; });
    const bool curved_wipes = curved_tops_mode(m_config.nonplanar_mode.value) || conformal_layers;
    if (curved_wipes)
        lift_type = LiftType::NormalLift;

    if (m_writer.filament() == nullptr)
        return gcode;

    if (curved_wipes && m_wipe.has_path()) {
        Polyline wipe_path;
        wipe_path.append(this->last_pos());
        wipe_path.append(m_wipe.path.points.begin() + 1, m_wipe.path.points.end());
        wipe_path.clip_end(wipe_path.length() - scale_(FILAMENT_CONFIG(wipe_distance)));
        std::vector<Vec2d> wipe_xy;
        for (const Point &p : wipe_path.points)
            wipe_xy.emplace_back(this->point_to_gcode(p));
        const double z = m_writer.get_position().z();
        if (! wipe_xy.empty() &&
            m_nonplanar_heights.travel_height(wipe_xy, m_nonplanar_clearance, z) > z + m_nonplanar_clearance.safety_mm + 1e-6)
            m_wipe.reset_path();
    }

    if (m_nonplanar_ceiling > 0.)
        m_wipe.reset_path();
    if (conformal_layers)
        m_wipe.reset_path();

    if (FILAMENT_CONFIG(wipe) && m_wipe.has_path() && scale_(FILAMENT_CONFIG(wipe_distance)) > SCALED_EPSILON &&
        !this->below_conformal_material()) {
        Wipe::RetractionValues wipeRetractions = m_wipe.calculateWipeRetractionLengths(*this, toolchange);
        gcode += toolchange ? m_writer.retract_for_toolchange(true, wipeRetractions.retraction_length_before_wipe) :
                              m_writer.retract(true, wipeRetractions.retraction_length_before_wipe);
        gcode += m_wipe.wipe(*this, wipeRetractions.retraction_length_during_wipe, toolchange, is_last_retraction);

    } else if (this->below_conformal_material())
        m_wipe.reset_path();

    if ((!this->on_first_layer()  || this->config().bottom_surface_pattern != InfillPattern::ipHilbertCurve) &&
	    (role != erTopSolidInfill || this->config().top_surface_pattern    != InfillPattern::ipHilbertCurve))
        gcode += toolchange ? m_writer.retract_for_toolchange() : m_writer.retract();

    gcode += m_writer.reset_e();
    RetractLiftEnforceType retract_lift_type = RetractLiftEnforceType(EXTRUDER_CONFIG(retract_lift_enforce));

    bool needs_lift = toolchange
        || m_writer.filament()->retraction_length() > 0
        || m_config.use_firmware_retraction;

    bool last_fill_extrusion_role_top_infill = (this->m_last_notgapfill_extrusion_role == ExtrusionRole::erTopSolidInfill || this->m_last_notgapfill_extrusion_role == ExtrusionRole::erIroning);

    bool can_lift = true;

    if (retract_lift_type == RetractLiftEnforceType::rletAllSurfaces) {
        can_lift = true;
    }
    else if (this->m_layer_index == 0 && (retract_lift_type == RetractLiftEnforceType::rletBottomOnly || retract_lift_type == RetractLiftEnforceType::rletTopAndBottom)) {
        can_lift = true;
    }
    else if (retract_lift_type == RetractLiftEnforceType::rletTopOnly || retract_lift_type == RetractLiftEnforceType::rletTopAndBottom) {
        can_lift = last_fill_extrusion_role_top_infill;
    }
    else {
        can_lift = false;
    }

    if (needs_lift && can_lift && this->rotary_at_home()) {
        if (m_writer.get_position().z() < m_nominal_z - EPSILON && Nonplanar::requests_conformal_shells(*m_print))
            gcode += m_writer.travel_to_z(m_nominal_z, "rise to layer Z before lift", true, this->conformal_max_speed_z());
        if (this->below_conformal_material())
            lift_type = LiftType::NormalLift;
        if (apply_instantly)
            gcode += m_writer.eager_lift(lift_type);
        else
            gcode += m_writer.lazy_lift(lift_type, m_spiral_vase != nullptr);
    }

    return gcode;
}

void GCode::update_layer_related_config(int layer_id){
    auto group_result = m_print->get_layered_nozzle_group_result();
    if(!group_result)
        return;

    auto extruder_map = group_result->get_extruder_map(false,layer_id);
    auto volume_map = group_result->get_volume_map(layer_id);
    auto nozzle_map = group_result->get_nozzle_map(layer_id);

    m_config.filament_map.values = extruder_map;
    m_config.filament_volume_map.values = volume_map;
    m_config.filament_nozzle_map.values = nozzle_map;

    m_writer.config.filament_map.values = extruder_map;
    m_writer.config.filament_volume_map.values = volume_map;
    m_writer.config.filament_nozzle_map.values = nozzle_map;

}

void GCode::update_placeholder_parser_with_variant_params()
{
    if (!m_print)
        return;

    size_t num_filaments = m_config.filament_type.values.size();
    if (num_filaments == 0)
        return;

    std::vector<size_t> config_index(num_filaments);
    for (size_t i = 0; i < num_filaments; ++i)
        config_index[i] = get_filament_config_index(i);
    auto remap_by_filament = [&](const auto &src) {
        std::decay_t<decltype(src.values)> dst(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i)
            dst[i] = src.get_at(config_index[i]);
        return dst;
    };

    this->placeholder_parser().set("filament_max_volumetric_speed",       new ConfigOptionFloats(remap_by_filament(m_config.filament_max_volumetric_speed)));
    this->placeholder_parser().set("filament_pre_cooling_temperature",    new ConfigOptionInts(remap_by_filament(m_config.filament_pre_cooling_temperature)));
    this->placeholder_parser().set("filament_pre_cooling_temperature_nc", new ConfigOptionInts(remap_by_filament(m_config.filament_pre_cooling_temperature_nc)));
    this->placeholder_parser().set("filament_cooling_before_tower",       new ConfigOptionFloats(remap_by_filament(m_config.filament_cooling_before_tower)));
    this->placeholder_parser().set("nozzle_temperature_initial_layer",    new ConfigOptionInts(remap_by_filament(m_config.nozzle_temperature_initial_layer)));
    this->placeholder_parser().set("nozzle_temperature",                  new ConfigOptionInts(remap_by_filament(m_config.nozzle_temperature)));
    this->placeholder_parser().set("first_layer_temperature",             new ConfigOptionInts(remap_by_filament(m_config.nozzle_temperature_initial_layer)));
    this->placeholder_parser().set("pressure_advance",                    new ConfigOptionFloats(remap_by_filament(m_config.pressure_advance)));
    this->placeholder_parser().set("enable_pressure_advance",             new ConfigOptionBools(remap_by_filament(m_config.enable_pressure_advance)));
    this->placeholder_parser().set("fan_min_speed",                       new ConfigOptionFloats(remap_by_filament(m_config.fan_min_speed)));
    this->placeholder_parser().set("fan_max_speed",                       new ConfigOptionFloats(remap_by_filament(m_config.fan_max_speed)));
    this->placeholder_parser().set("additional_cooling_fan_speed",        new ConfigOptionInts(remap_by_filament(m_config.additional_cooling_fan_speed)));
    this->placeholder_parser().set("filament_minimal_purge_on_wipe_tower", new ConfigOptionFloats(remap_by_filament(m_config.filament_minimal_purge_on_wipe_tower)));
    this->placeholder_parser().set("filament_multitool_ramming",          new ConfigOptionBools(remap_by_filament(m_config.filament_multitool_ramming)));
    this->placeholder_parser().set("filament_multitool_ramming_volume",   new ConfigOptionFloats(remap_by_filament(m_config.filament_multitool_ramming_volume)));
    this->placeholder_parser().set("filament_multitool_ramming_flow",     new ConfigOptionFloats(remap_by_filament(m_config.filament_multitool_ramming_flow)));
    this->placeholder_parser().set("nozzle_temperature_range_low",        new ConfigOptionInts(remap_by_filament(m_config.nozzle_temperature_range_low)));
    const auto nozzle_temperature_range_high = remap_by_filament(m_config.nozzle_temperature_range_high);
    this->placeholder_parser().set("nozzle_temperature_range_high",       new ConfigOptionInts(nozzle_temperature_range_high));

    this->placeholder_parser().set("retraction_distances_when_cut",       new ConfigOptionFloats(remap_by_filament(m_config.retraction_distances_when_cut)));
    this->placeholder_parser().set("filament_map", new ConfigOptionInts(m_config.filament_map));

    {
        bool use_fast_flush = m_config.prime_volume_mode == PrimeVolumeMode::pvmFast;
        auto flush_v_speed  = remap_by_filament(m_config.filament_flush_volumetric_speed);
        auto filament_max_v = remap_by_filament(m_config.filament_max_volumetric_speed);
        auto flush_temps    = remap_by_filament(use_fast_flush ? m_config.filament_flush_temp_fast
                                                               : m_config.filament_flush_temp);
        for (size_t i = 0; i < num_filaments; ++i) {
            if (flush_v_speed[i] == 0)
                flush_v_speed[i] = filament_max_v[i];
            if (flush_temps[i] == 0)
                flush_temps[i] = nozzle_temperature_range_high[i];
        }
        this->placeholder_parser().set("flush_volumetric_speeds", new ConfigOptionFloats(flush_v_speed));
        this->placeholder_parser().set("flush_temperatures",      new ConfigOptionInts(flush_temps));
    }
}

std::string GCode::set_extruder(unsigned int new_filament_id, double print_z, bool by_object, int toolchange_temp_override, bool defer_temp_wait)
{
    int new_extruder_id = get_extruder_id(new_filament_id);
    if (!m_writer.need_toolchange(new_filament_id))
        return "";

    if (!m_writer.multiple_extruders) {
        this->placeholder_parser().set("current_extruder", new_filament_id);
        this->placeholder_parser().set("current_filament_id", (int) new_filament_id);
        this->placeholder_parser().set("current_extruder_id", new_extruder_id);
        this->placeholder_parser().set("current_nozzle_id",
            nozzle_id_for_gcode_placeholder(m_print->get_layered_nozzle_group_result(), (int) new_filament_id, new_extruder_id, m_layer_index));
        this->placeholder_parser().set("current_hotend",
            hotend_id_for_gcode_placeholder(m_print->get_layered_nozzle_group_result(), m_print->is_BBL_printer(), (int) new_filament_id,
                                            new_extruder_id, m_layer_index));
        {
            size_t fi = get_filament_config_index(new_filament_id);
            this->placeholder_parser().set("retraction_distance_when_ec", m_config.retraction_distances_when_ec.get_at(fi));
            this->placeholder_parser().set("long_retraction_when_ec", m_config.long_retractions_when_ec.get_at(fi));
        }

        std::string gcode;
        const std::string &filament_start_gcode = m_config.filament_start_gcode.get_at(new_filament_id);
        if (! filament_start_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(this->writer().get_position().z() - m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(new_filament_id)));
            {
                size_t fi = get_filament_config_index(new_filament_id);
                config.set_key_value("retraction_distance_when_cut",
                                     new ConfigOptionFloat(m_config.retraction_distances_when_cut.get_at(fi)));
                config.set_key_value("long_retraction_when_cut", new ConfigOptionBool(m_config.long_retractions_when_cut.get_at(fi)));
            }

            const std::string start = this->placeholder_parser_process("filament_start_gcode", filament_start_gcode, new_filament_id, &config);
            m_start_gcode_pa[new_filament_id] = Nonplanar::parse_pressure_advance(start);
            gcode += start;
            check_add_eol(gcode);
        }
        gcode += set_filament_pressure_advance(new_filament_id,
            imex_pem_tool_for((int) new_filament_id, m_imex_parallel_mode, m_config.physical_extruder_map));

        const std::string toolchange_command = m_writer.toolchange(new_filament_id, new_extruder_id);
        if (!imex_suppresses_bare_toolchange(m_imex_parallel_mode, m_toolchange_count))
            gcode += toolchange_command;
        if (Extruder *fil = m_writer.filament())
            fil->set_config_index((int)get_filament_config_index((int)fil->id()));
        return gcode;
    }

    m_toolchange_count++;

    std::string gcode = this->retract(true, false);

    m_wipe.reset_path();

    if (by_object)
        m_writer.add_object_change_labels(gcode);

    bool add_change_filament_624 = false;
    if (m_writer.filament() != nullptr) {
        unsigned int        old_filament_id = m_writer.filament()->id();
        const std::string  &filament_end_gcode  = m_config.filament_end_gcode.get_at(old_filament_id);
        if (! filament_end_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z",   new ConfigOptionFloat(m_writer.get_position().z() - m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(get_extruder_id(old_filament_id))));
            if (!m_filament_instances_code.empty()) {
                gcode += ("M624 " + m_filament_instances_code + "\n");
                m_filament_instances_code = "";
                add_change_filament_624   = true;
            }
            gcode += placeholder_parser_process("filament_end_gcode", filament_end_gcode, old_filament_id, &config);
            check_add_eol(gcode);
        }
    }


    auto same_physical_extruder = [&](int a, int b) {
        if (!m_config.is_imex.value)
            return false;
        const auto& pem = m_config.physical_extruder_map.values;
        return !pem.empty() && a >= 0 && b >= 0
            && a < (int)pem.size() && b < (int)pem.size()
            && pem[a] == pem[b];
    };
    if (m_ooze_prevention.enable && m_writer.filament() != nullptr
        && !same_physical_extruder(m_writer.filament()->id(), (int)new_filament_id))
        gcode += m_ooze_prevention.pre_toolchange(*this);

    size_t new_fi = get_filament_config_index((int)new_filament_id);
    float new_retract_length = m_config.retraction_length.get_at(new_fi);
    float new_retract_length_toolchange = m_config.retract_length_toolchange.get_at(new_fi);
    int new_filament_temp = this->on_first_layer() ? m_config.nozzle_temperature_initial_layer.get_at(new_fi) : m_config.nozzle_temperature.get_at(new_fi);
    if (abs(print_z) < EPSILON)
        new_filament_temp = m_config.nozzle_temperature_initial_layer.get_at(new_fi);
    if (toolchange_temp_override > 0)
        new_filament_temp = toolchange_temp_override;

    if (defer_temp_wait) {
        int preheat_temp = new_filament_temp;
        if (toolchange_temp_override <= 0 && (m_layer == nullptr || preheat_temp <= 0))
            preheat_temp = m_config.nozzle_temperature_initial_layer.get_at(new_fi);
        if (preheat_temp > 0)
            gcode += m_writer.set_temperature(preheat_temp, false, new_filament_id);
    }

    Vec3d nozzle_pos = m_writer.get_position();
    float old_retract_length, old_retract_length_toolchange, wipe_volume;
    int old_filament_temp, old_filament_e_feedrate;

    float filament_area = float((M_PI / 4.f) * pow(m_config.filament_diameter.get_at(new_filament_id), 2));
    int old_filament_id = -1;
    int old_extruder_id = -1;
    if (m_writer.filament() != nullptr || m_start_gcode_filament != -1) {
        std::vector<float> flush_matrix(cast<float>(get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, new_extruder_id, m_config.nozzle_diameter.values.size())));
        const unsigned int number_of_extruders = (unsigned int) (m_config.filament_colour.values.size());
        if (m_writer.filament() != nullptr)
            assert(m_writer.filament()->id() < number_of_extruders);
        else
            assert(m_start_gcode_filament < number_of_extruders);

        old_filament_id = m_writer.filament() != nullptr ? m_writer.filament()->id() : m_start_gcode_filament;
        old_extruder_id = m_writer.filament() != nullptr ? m_writer.filament()->extruder_id() : get_extruder_id(m_start_gcode_filament);

        size_t old_fi = get_filament_config_index(old_filament_id);
        old_retract_length = m_config.retraction_length.get_at(old_fi);
        old_retract_length_toolchange = m_config.retract_length_toolchange.get_at(old_fi);
        old_filament_temp = this->on_first_layer()? m_config.nozzle_temperature_initial_layer.get_at(old_fi) : m_config.nozzle_temperature.get_at(old_fi);

        float grab_purge_volume = m_config.grab_length.get_at(new_extruder_id) * 2.4;
        if (old_extruder_id != new_extruder_id) {
            int old_filament_id_in_new_extruder = m_writer.filament(new_extruder_id) != nullptr ? m_writer.filament(new_extruder_id)->id() : -1;
            if (old_filament_id_in_new_extruder == -1)
                wipe_volume = 0;
            else {
                size_t flush_idx = size_t(old_filament_id_in_new_extruder) * number_of_extruders + new_filament_id;
                wipe_volume = flush_idx < flush_matrix.size() ? flush_matrix[flush_idx] : 0.f;
                wipe_volume *= m_config.flush_multiplier.get_at(new_extruder_id);
            }
        }
        else {
            size_t flush_idx = size_t(old_filament_id) * number_of_extruders + new_filament_id;
            wipe_volume = flush_idx < flush_matrix.size() ? flush_matrix[flush_idx] : 0.f;
            wipe_volume *= m_config.flush_multiplier.get_at(new_extruder_id);
        }
        wipe_volume = std::max(0.f, wipe_volume-grab_purge_volume);

        old_filament_e_feedrate = (int) (60.0 * m_config.filament_max_volumetric_speed.get_at(old_fi) / filament_area);
        old_filament_e_feedrate = old_filament_e_feedrate == 0 ? 100 : old_filament_e_feedrate;
        m_start_gcode_filament = -1;
    } else {
        old_retract_length = 0.f;
        old_retract_length_toolchange = 0.f;
        old_filament_temp = 0;
        wipe_volume = 0.f;
        old_filament_e_feedrate = 200;
    }
    float wipe_length = wipe_volume / filament_area;
    int new_filament_e_feedrate = (int)(60.0 * m_config.filament_max_volumetric_speed.get_at(new_fi) / filament_area);
    new_filament_e_feedrate = new_filament_e_feedrate == 0 ? 100 : new_filament_e_feedrate;

    float outer_wall_volumetric_speed = get_outer_wall_volumetric_speed(m_config, *m_print, new_filament_id, (int)new_fi, get_extruder_id(new_filament_id));
    float         wipe_avoid_pos_x            = 110.f;
    auto group_result   = m_print->get_layered_nozzle_group_result();
    int  old_nozzle_id  = nozzle_id_for_gcode_placeholder(group_result, old_filament_id, old_extruder_id, m_layer_index);
    int  next_nozzle_id = nozzle_id_for_gcode_placeholder(group_result, (int) new_filament_id, new_extruder_id, m_layer_index);
    DynamicConfig dyn_config;
    dyn_config.set_key_value("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_volumetric_speed));
    dyn_config.set_key_value("previous_extruder", new ConfigOptionInt(old_filament_id));
    dyn_config.set_key_value("next_extruder", new ConfigOptionInt((int)new_filament_id));
    dyn_config.set_key_value("current_hotend", new ConfigOptionInt(
        hotend_id_for_gcode_placeholder(group_result, m_print->is_BBL_printer(), old_filament_id, old_extruder_id, m_layer_index)));
    dyn_config.set_key_value("next_hotend", new ConfigOptionInt(
        hotend_id_for_gcode_placeholder(group_result, m_print->is_BBL_printer(), (int) new_filament_id, new_extruder_id, m_layer_index)));
    dyn_config.set_key_value("current_nozzle_id", new ConfigOptionInt(old_nozzle_id));
    dyn_config.set_key_value("next_nozzle_id", new ConfigOptionInt(next_nozzle_id));
    dyn_config.set_key_value("current_filament_id", new ConfigOptionInt(old_filament_id));
    dyn_config.set_key_value("next_filament_id", new ConfigOptionInt((int)new_filament_id));
    {
        const auto &extruder_variants = m_config.printer_extruder_variant.values;
        dyn_config.set_key_value("old_extruder_variant", new ConfigOptionString(
            (old_extruder_id >= 0 && old_extruder_id < (int) extruder_variants.size()) ? extruder_variants[old_extruder_id] : std::string()));
        dyn_config.set_key_value("new_extruder_variant", new ConfigOptionString(
            (new_extruder_id >= 0 && new_extruder_id < (int) extruder_variants.size()) ? extruder_variants[new_extruder_id] : std::string()));
    }
    dyn_config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
    dyn_config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
    dyn_config.set_key_value("filament_retract_length_nc", new ConfigOptionFloat(
        (old_filament_id != -1) ? (float) m_config.filament_retract_length_nc.get_at(get_filament_config_index(old_filament_id)) : 0.f));
    dyn_config.set_key_value("new_extruder_retracted_length",
        new ConfigOptionFloat(m_writer.get_extruder_retracted_length((int) new_filament_id)));
    dyn_config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
    dyn_config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
    dyn_config.set_key_value("max_layer_z", new ConfigOptionFloat(m_nonplanar_ceiling > 0. ?
        std::max(m_nonplanar_ceiling, double(m_max_layer_z) + m_config.z_offset.value) : m_max_layer_z));
    dyn_config.set_key_value("relative_e_axis", new ConfigOptionBool(m_config.use_relative_e_distances));
    dyn_config.set_key_value("toolchange_count", new ConfigOptionInt((int)m_toolchange_count));
    dyn_config.set_key_value("fan_speed", new ConfigOptionInt((int)0));
    dyn_config.set_key_value("old_retract_length", new ConfigOptionFloat(old_retract_length));
    dyn_config.set_key_value("new_retract_length", new ConfigOptionFloat(new_retract_length));
    dyn_config.set_key_value("old_retract_length_toolchange", new ConfigOptionFloat(old_retract_length_toolchange));
    dyn_config.set_key_value("new_retract_length_toolchange", new ConfigOptionFloat(new_retract_length_toolchange));
    dyn_config.set_key_value("old_filament_temp", new ConfigOptionInt(old_filament_temp));
    dyn_config.set_key_value("new_filament_temp", new ConfigOptionInt(new_filament_temp));
    dyn_config.set_key_value("x_after_toolchange", new ConfigOptionFloat(nozzle_pos(0)));
    dyn_config.set_key_value("y_after_toolchange", new ConfigOptionFloat(nozzle_pos(1)));
    const double return_z = m_nonplanar_ceiling > 0. ?
        std::max({ nozzle_pos(2), m_nonplanar_ceiling, double(m_max_layer_z) + m_config.z_offset.value }) + nonplanar_head().margin : nozzle_pos(2);
    dyn_config.set_key_value("z_after_toolchange", new ConfigOptionFloat(return_z));
    dyn_config.set_key_value("first_flush_volume", new ConfigOptionFloat(wipe_length / 2.f));
    dyn_config.set_key_value("second_flush_volume", new ConfigOptionFloat(wipe_length / 2.f));
    dyn_config.set_key_value("old_filament_e_feedrate", new ConfigOptionInt(old_filament_e_feedrate));
    dyn_config.set_key_value("new_filament_e_feedrate", new ConfigOptionInt(new_filament_e_feedrate));
    dyn_config.set_key_value("travel_point_1_x", new ConfigOptionFloat(float(travel_point_1.x())));
    dyn_config.set_key_value("travel_point_1_y", new ConfigOptionFloat(float(travel_point_1.y())));
    dyn_config.set_key_value("travel_point_2_x", new ConfigOptionFloat(float(travel_point_2.x())));
    dyn_config.set_key_value("travel_point_2_y", new ConfigOptionFloat(float(travel_point_2.y())));
    dyn_config.set_key_value("travel_point_3_x", new ConfigOptionFloat(float(travel_point_3.x())));
    dyn_config.set_key_value("travel_point_3_y", new ConfigOptionFloat(float(travel_point_3.y())));
    dyn_config.set_key_value("wipe_avoid_perimeter", new ConfigOptionBool(false));
    dyn_config.set_key_value("wipe_avoid_pos_x", new ConfigOptionFloat(wipe_avoid_pos_x));
    dyn_config.set_key_value("is_prime_tower_interface", new ConfigOptionBool(false));
    dyn_config.set_key_value("filament_tower_interface_purge_volume", new ConfigOptionFloat(m_config.filament_tower_interface_purge_volume.get_at(new_filament_id)));
    {
        int interface_temp = m_config.filament_tower_interface_print_temp.get_at(new_filament_id);
        if (interface_temp == -1)
            interface_temp = m_config.nozzle_temperature_range_high.get_at(new_fi);
        dyn_config.set_key_value("filament_tower_interface_print_temp", new ConfigOptionInt(interface_temp));
    }
    if (toolchange_temp_override > 0) {
        size_t num_filaments = m_print->config().filament_type.values.size();
        std::vector<int> temps(num_filaments);
        std::vector<int> first_layer_temps(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            size_t fi_i = get_filament_config_index((int)i);
            temps[i]             = m_config.nozzle_temperature.get_at(fi_i);
            first_layer_temps[i] = m_config.nozzle_temperature_initial_layer.get_at(fi_i);
        }
        if (new_filament_id < temps.size())
            temps[new_filament_id] = toolchange_temp_override;
        dyn_config.set_key_value("temperature", new ConfigOptionInts(temps));
        dyn_config.set_key_value("nozzle_temperature", new ConfigOptionInts(temps));
        if (new_filament_id < first_layer_temps.size())
            first_layer_temps[new_filament_id] = toolchange_temp_override;
        dyn_config.set_key_value("first_layer_temperature", new ConfigOptionInts(first_layer_temps));
        dyn_config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts(first_layer_temps));
    }

    {
        size_t num_filaments = m_print->config().filament_type.values.size();
        bool   use_fast_flush = m_print->config().prime_volume_mode == PrimeVolumeMode::pvmFast;
        std::vector<double> flush_v_speed(num_filaments);
        std::vector<int>    flush_temps(num_filaments);
        std::vector<double> filament_cooling_before_tower(num_filaments);
        for (size_t idx = 0; idx < num_filaments; ++idx) {
            size_t fi = get_filament_config_index(idx);
            flush_v_speed[idx] = m_print->config().filament_flush_volumetric_speed.get_at(fi);
            if (flush_v_speed[idx] == 0)
                flush_v_speed[idx] = m_print->config().filament_max_volumetric_speed.get_at(fi);
            flush_temps[idx] = use_fast_flush ? m_print->config().filament_flush_temp_fast.get_at(fi)
                                              : m_print->config().filament_flush_temp.get_at(fi);
            if (flush_temps[idx] == 0)
                flush_temps[idx] = m_print->config().nozzle_temperature_range_high.get_at(fi);
            filament_cooling_before_tower[idx] = m_print->config().filament_cooling_before_tower.get_at(fi);
        }
        std::fill(filament_cooling_before_tower.begin(), filament_cooling_before_tower.end(), 0);
        dyn_config.set_key_value("flush_volumetric_speeds", new ConfigOptionFloats(flush_v_speed));
        dyn_config.set_key_value("flush_temperatures", new ConfigOptionInts(flush_temps));
        dyn_config.set_key_value("filament_cooling_before_tower", new ConfigOptionFloats(filament_cooling_before_tower));
    }
    dyn_config.set_key_value("flush_length", new ConfigOptionFloat(wipe_length));

    int flush_count = std::min(g_max_flush_count, (int)std::round(wipe_volume / g_purge_volume_one_time));
    float flush_unit = wipe_length / flush_count;
    int flush_idx = 0;
    for (; flush_idx < flush_count; flush_idx++) {
        char key_value[64] = { 0 };
        snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
        dyn_config.set_key_value(key_value, new ConfigOptionFloat(flush_unit));
    }

    for (; flush_idx < g_max_flush_count; flush_idx++) {
        char key_value[64] = { 0 };
        snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
        dyn_config.set_key_value(key_value, new ConfigOptionFloat(0.f));
    }

    std::string change_filament_gcode = m_config.change_filament_gcode.value;

    change_filament_gcode = this->retract(false, false, LiftType::SpiralLift, true) + change_filament_gcode;

    std::string toolchange_gcode_parsed;
    if (!change_filament_gcode.empty() && !(m_config.manual_filament_change.value && m_toolchange_count == 1)) {
        dyn_config.set_key_value("toolchange_z", new ConfigOptionFloat(print_z));

        toolchange_gcode_parsed = placeholder_parser_process("change_filament_gcode", change_filament_gcode, new_filament_id, &dyn_config);
        check_add_eol(toolchange_gcode_parsed);
        gcode += toolchange_gcode_parsed;

        {
            gcode += ";_FORCE_RESUME_FAN_SPEED\n";
            m_writer.set_current_position_clear(false);
            double temp_z_after_tool_change;
            if (GCodeProcessor::get_last_z_from_gcode(toolchange_gcode_parsed, temp_z_after_tool_change)) {
                Vec3d pos = m_writer.get_position();
                pos(2) = temp_z_after_tool_change;
                m_writer.set_position(pos);
            }
        }
    }

    if (m_config.single_extruder_multi_material) {
        m_writer.reset_e();
    }

    std::string toolchange_command = m_writer.toolchange(new_filament_id, next_nozzle_id);
    if (Extruder *fil = m_writer.filament())
        fil->set_config_index((int)get_filament_config_index((int)fil->id()));
    const bool suppress_imex_bare = imex_suppresses_bare_toolchange(m_imex_parallel_mode, m_toolchange_count);
    if (!custom_gcode_changes_tool(toolchange_gcode_parsed, m_writer.toolchange_prefix(), new_filament_id)
        && !suppress_imex_bare)
        gcode += toolchange_command;
    else {
    }

    if (m_config.single_extruder_multi_material && !m_config.enable_prime_tower) {
        size_t new_fi = get_filament_config_index(new_filament_id);
        int temp = (m_layer_index <= 0 ? m_config.nozzle_temperature_initial_layer.get_at(new_fi) :
                                         m_config.nozzle_temperature.get_at(new_fi));

        gcode += m_writer.set_temperature(temp, false);
    }

    this->placeholder_parser().set("current_extruder", new_filament_id);
    this->placeholder_parser().set("current_hotend",
        hotend_id_for_gcode_placeholder(group_result, m_print->is_BBL_printer(), (int) new_filament_id, new_extruder_id, m_layer_index));
    this->placeholder_parser().set("current_filament_id", (int) new_filament_id);
    this->placeholder_parser().set("current_extruder_id", new_extruder_id);
    this->placeholder_parser().set("current_nozzle_id", next_nozzle_id);
    {
        size_t fi = get_filament_config_index(new_filament_id);
        this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(fi));
        this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(fi));
        this->placeholder_parser().set("retraction_distance_when_ec", m_config.retraction_distances_when_ec.get_at(fi));
        this->placeholder_parser().set("long_retraction_when_ec", m_config.long_retractions_when_ec.get_at(fi));
    }


    const std::string &filament_start_gcode = m_config.filament_start_gcode.get_at(new_filament_id);
    if (! filament_start_gcode.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(this->writer().get_position().z() - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(new_filament_id)));
        if (toolchange_temp_override > 0) {
            size_t num_filaments = m_print->config().filament_type.values.size();
            std::vector<int> temps(num_filaments);
            std::vector<int> first_layer_temps(num_filaments);
            for (size_t i = 0; i < num_filaments; ++i) {
                size_t fi_i = get_filament_config_index((int)i);
                temps[i]             = m_config.nozzle_temperature.get_at(fi_i);
                first_layer_temps[i] = m_config.nozzle_temperature_initial_layer.get_at(fi_i);
            }
            if (new_filament_id < temps.size())
                temps[new_filament_id] = toolchange_temp_override;
            config.set_key_value("temperature", new ConfigOptionInts(temps));
            config.set_key_value("nozzle_temperature", new ConfigOptionInts(temps));
            if (new_filament_id < first_layer_temps.size())
                first_layer_temps[new_filament_id] = toolchange_temp_override;
            config.set_key_value("first_layer_temperature", new ConfigOptionInts(first_layer_temps));
            config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts(first_layer_temps));
        }
        const std::string start = this->placeholder_parser_process("filament_start_gcode", filament_start_gcode, new_filament_id, &config);
        m_start_gcode_pa[new_filament_id] = Nonplanar::parse_pressure_advance(start);
        gcode += start;
        if (add_change_filament_624) {
            gcode += "M625\n";
            add_change_filament_624 = false;
        }
        check_add_eol(gcode);
    }
    if (m_ooze_prevention.enable && !defer_temp_wait)
        gcode += m_ooze_prevention.post_toolchange(*this);

    gcode += set_filament_pressure_advance(new_filament_id,
        imex_pem_tool_for((int) new_filament_id, m_imex_parallel_mode, m_config.physical_extruder_map));
    m_last_pos_defined = false;

    return gcode;
}

std::string GCode::set_filament_pressure_advance(unsigned int filament_id, int tool)
{
    const size_t fi = get_filament_config_index(filament_id);
    if (!m_config.enable_pressure_advance.get_at(fi))
        return {};
    m_pa_processor->resetPreviousPA(m_config.pressure_advance.get_at(fi));
    return m_writer.set_pressure_advance(m_config.pressure_advance.get_at(fi), tool);
}

inline std::string polygon_to_string(const Polygon &polygon, Print *print, bool is_print_space = false) {
    std::ostringstream gcode;
    gcode << "[";
    for (const Point &p : polygon.points) {
        const auto v = is_print_space ? Vec2d(p.x(), p.y()) : print->translate_to_print_space(p);
        gcode << "[" << v.x() << "," << v.y() << "],";
    }
    const auto first_v = is_print_space ? Vec2d(polygon.points.front().x(), polygon.points.front().y())
                                        : print->translate_to_print_space(polygon.points.front());
    gcode << "[" << first_v.x() << "," << first_v.y() << "]";
    gcode << "]";
    return gcode.str();
}
std::string GCode::set_object_info(Print *print) {
    const auto gflavor = print->config().gcode_flavor.value;
    if (print->is_BBL_printer() ||
        (gflavor != gcfKlipper && gflavor != gcfMarlinLegacy && gflavor != gcfMarlinFirmware && gflavor != gcfRepRapFirmware))
        return "";
    std::ostringstream gcode;
    size_t object_id = 0;
    if (print->calib_mode() == CalibMode::Calib_PA_Pattern) {
        BoundingBoxf bbox_bed(print->config().printable_area.values);
        bbox_bed.offset(-25.0);
        Polygon polygon_bed;
        polygon_bed.append(Point(bbox_bed.min.x(), bbox_bed.min.y()));
        polygon_bed.append(Point(bbox_bed.max.x(), bbox_bed.min.y()));
        polygon_bed.append(Point(bbox_bed.max.x(), bbox_bed.max.y()));
        polygon_bed.append(Point(bbox_bed.min.x(), bbox_bed.max.y()));
        gcode << "EXCLUDE_OBJECT_DEFINE NAME="
              << "Orca-PA-Calibration-Test"
              << " CENTER=" << 0 << "," << 0 << " POLYGON=" << polygon_to_string(polygon_bed, print, true) << "\n";
    } else if (print->calib_mode() == CalibMode::Calib_PA_Line) {
    } else {
        size_t unique_id = 0;
        m_instance_names.clear();
        for (PrintObject* object : print->objects()) {
            object->set_id(object_id++);
            size_t inst_id = 0;
            for (PrintInstance& inst : object->instances()) {
                inst.unique_id = unique_id++;
                inst.id        = inst_id++;
                auto bbox      = inst.get_bounding_box();
                auto center    = print->translate_to_print_space(Vec2d(bbox.center().x(), bbox.center().y()));
                const std::string &inst_name = instance_name(inst);
                if (gflavor == gcfKlipper) {
                    gcode << "EXCLUDE_OBJECT_DEFINE NAME=" << inst_name << " CENTER=" << center.x() << "," << center.y()
                          << " POLYGON=" << polygon_to_string(inst.get_convex_hull_2d(), print) << "\n";
                } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                    gcode << "M486 S" << std::to_string(inst.unique_id);
                    if (gflavor == gcfRepRapFirmware)
                        gcode << " A"
                              << "\"" << inst_name << "\"";
                    else
                        gcode << "\nM486 A" << inst_name;
                    gcode << "\nM486 S-1\n";
                }
            }
        }
    }

    return gcode.str();
}

int GCode::belt_layer_past_first_layer_band(const Layer *object_layer) const
{
    if (object_layer == nullptr)
        return -1;
    double min_height = std::numeric_limits<double>::max();
    bool   known      = false;
    for (const BoundingBox &bb : object_layer->lslices_bboxes) {
        const double xs[2] = { unscale<double>(bb.min.x()), unscale<double>(bb.max.x()) };
        const double ys[2] = { unscale<double>(bb.min.y()), unscale<double>(bb.max.y()) };
        for (double x : xs)
            for (double y : ys) {
                double h;
                if (! this->belt_height_above_floor(Vec3d(x, y, object_layer->print_z), h))
                    return -1;
                known      = true;
                min_height = std::min(min_height, h);
            }
    }
    if (! known)
        return -1;
    return min_height >= this->first_layer_band_mm() - EPSILON ? 1 : 0;
}

bool GCode::belt_height_above_floor(const Vec3d &point_slicing_mm, double &height_mm) const
{
    const PrintObject *object = m_belt_floor_object != nullptr ? m_belt_floor_object
                              : (m_layer != nullptr ? m_layer->object() : nullptr);
    if (object == nullptr)
        return false;
    const SlicingParameters &sp = object->slicing_parameters();
    if (std::abs(sp.belt_floor_shear_factor) < EPSILON)
        return false;
    const double u = sp.belt_floor_from_axis == 0 ? point_slicing_mm.x() : point_slicing_mm.y();
    const double floor_z = sp.belt_floor_shear_factor * u + sp.belt_floor_z_shift;
    height_mm = point_slicing_mm.z() - floor_z;
    return true;
}

Vec2d GCode::point_to_gcode(const Point &point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG(extruder_offset);
    return unscale(point) + m_origin - extruder_offset;
}

Vec3d GCode::point_to_gcode(const Point3& point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG(extruder_offset);
    Vec2d xy              = unscale(point.to_point()) + m_origin - extruder_offset;
    return Vec3d(xy.x(), xy.y(), unscale_(point.z()));
}

Point GCode::gcode_to_point(const Vec2d &point) const
{
    Vec2d pt = point - m_origin;
    if (const Extruder *extruder = m_writer.filament(); extruder)
        pt += m_config.extruder_offset.get_at(extruder->extruder_id());
    return scaled<coord_t>(pt);
        
}

Vec2d GCode::point_to_gcode_quantized(const Point& point) const
{
    Vec2d p = this->point_to_gcode(point);
    return { GCodeFormatter::quantize_xyzf(p.x()), GCodeFormatter::quantize_xyzf(p.y()) };
}

Vec3d GCode::point_to_gcode_quantized(const Point3& point) const
{
    Vec3d p = this->point_to_gcode(point);
    return {GCodeFormatter::quantize_xyzf(p.x()), GCodeFormatter::quantize_xyzf(p.y()), GCodeFormatter::quantize_xyzf(p.z())};
}

const std::vector<GCode::ObjectByExtruder::Island::Region>& GCode::ObjectByExtruder::Island::by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities) const
{
    bool has_overrides = false;
    for (const auto& reg : by_region)
        if (! reg.infills_overrides.empty() || ! reg.perimeters_overrides.empty()) {
            has_overrides = true;
            break;
        }

    by_region_per_copy_cache.clear();

    if (! has_overrides)
        return wiping_entities ? by_region_per_copy_cache : this->by_region;

    for (const auto& reg : by_region) {
        by_region_per_copy_cache.emplace_back();
        for (int iter = 0; iter < 2; ++iter) {
            const std::vector<const ExtrusionEntity*>&						entities    = (iter ? reg.infills : reg.perimeters);
            std::vector<const ExtrusionEntity*>&							target_eec  = (iter ? by_region_per_copy_cache.back().infills : by_region_per_copy_cache.back().perimeters);
            const std::vector<const WipingExtrusions::ExtruderPerCopy*>& 	overrides   = (iter ? reg.infills_overrides : reg.perimeters_overrides);

            if (wiping_entities) {
                for (unsigned int i = 0; i < overrides.size(); ++ i) {
                    const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
                    if (this_override != nullptr && (*this_override)[copy] == int(extruder))
                        target_eec.emplace_back(entities[i]);
                }
            } else {
                unsigned int i = 0;
                for (; i < overrides.size(); ++ i) {
                    const WipingExtrusions::ExtruderPerCopy *this_override = overrides[i];
                    if (this_override == nullptr || (*this_override)[copy] == -int(extruder)-1)
                        target_eec.emplace_back(entities[i]);
                }
                for (; i < entities.size(); ++ i)
                    target_eec.emplace_back(entities[i]);
            }
        }
    }
    return by_region_per_copy_cache;
}

void GCode::ObjectByExtruder::Island::Region::append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copies_extruder)
{
    std::vector<const ExtrusionEntity*>*					perimeters_or_infills;
    std::vector<const WipingExtrusions::ExtruderPerCopy*>* 	perimeters_or_infills_overrides;

    switch (type) {
    case PERIMETERS:
        perimeters_or_infills 			= &perimeters;
        perimeters_or_infills_overrides = &perimeters_overrides;
        break;
    case INFILL:
        perimeters_or_infills 			= &infills;
        perimeters_or_infills_overrides = &infills_overrides;
        break;
    default:
    	throw Slic3r::InvalidArgument("Unknown parameter!");
    }

    size_t old_size = perimeters_or_infills->size();
    size_t new_size = old_size + (eec->can_sort() ? eec->entities.size() : 1);
    perimeters_or_infills->reserve(new_size);
    if (eec->can_sort()) {
        for (auto* ee : eec->entities)
            perimeters_or_infills->emplace_back(ee);
    } else
        perimeters_or_infills->emplace_back(eec);

    if (copies_extruder != nullptr) {
        perimeters_or_infills_overrides->reserve(new_size);
        perimeters_or_infills_overrides->resize(old_size, nullptr);
        perimeters_or_infills_overrides->resize(new_size, copies_extruder);
    }
}

}
