#pragma once
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/WarningCode.hpp"
#include "CurvedTopsSurface.hpp"
#include "SlantChoice.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class Print;
struct GCodeProcessorResult;
}

namespace Slic3r::Nonplanar {

class HeightField;
struct PatchDiagnostics;

struct ReportPatch
{
    size_t      object  = 0;
    int         id      = -1;
    int         kind    = 0;
    bool        top     = false;
    int         region  = -1;
    Vec2d       center  = Vec2d::Zero();
    int         band    = -1;
    int         shells  = 0;
    std::string pattern;
    bool        ironing = false;
    double      area    = 0.;
    std::vector<BoundingBox> boxes;
    std::string explain;
    std::string summary;
};

struct ReportFallback
{
    size_t      object = 0;
    int         patch  = -1;
    std::string reason;
    std::string cls;
    double      area   = 0.;
    bool        top    = true;
    ExPolygons  shape;
    std::vector<std::vector<Vec3f>> hatch;
};

struct ReportObject
{
    std::string name;
    size_t      model_object_id = 0;
    std::string mode;
    CurvedTopsStats curved_tops;
    std::pair<double, double> curved_band { 0., 0. };
    double curved_cone_tolerance = 0.05;
    std::string layer_source, layer_source_refused;
    size_t      top_patches   = 0;
    size_t      other_patches = 0;
    size_t      bands         = 0;
    double      curved_top    = 0.;
    double      planar_top    = 0.;
    double      stepped_top   = 0.;
    double      flat_top      = 0.;
    double      support_free  = 0.;
    std::string orientation;
    std::string oneshot;
    std::string oneshot_student;
    int         oneshot_polish_k = 0;
    double      oneshot_max_time_ratio_override = -1.;
    std::vector<std::string> regions;
    std::vector<CodedWarning> warnings;
    std::optional<SlantChoice> slant;
    int         conformal_rung = -1;
    std::string conformal_variant;
    std::string planar_limit, planar_reason;
    size_t      morph_hit_moves = 0;
    double      cap_requested = 0.;

    double curved_share() const {
        if (mode == "nonplanar_top") return curved_tops.area_fraction;
        if (!planar_limit.empty()) return 0.;
        return curved_top + planar_top > 0. ? curved_top / (curved_top + planar_top) : 1.;
    }
};

struct NonplanarReport
{
    std::vector<ReportObject>   objects;
    std::vector<ReportPatch>    patches;
    std::vector<ReportFallback> fallbacks;
    std::string head;
    bool        verified     = false;
    size_t      cone_grazes  = 0;
    size_t      body_hits    = 0;
    size_t      hits         = 0;
    size_t      machine_hits = 0;
    bool        clearance_override = false;
    size_t      duct_grazes  = 0;
    double      max_contact  = 0.;
    double      time_s        = 0.;
    double      curved_time_s = 0.;
    bool empty() const {
        if (!patches.empty() || !fallbacks.empty()) return false;
        for (const auto &object : objects)
            if (object.morph_hit_moves > 0 || ((object.mode == "nonplanar_top") && (
                !object.curved_tops.planar_limit.empty()))) return false;
        return true;
    }
    const ReportPatch *find_patch(int id, const Point &pos) const;
    std::string to_json(const std::map<size_t, std::string> &orientation = {}) const;
};

NonplanarReport make_report(const Print &print, const GCodeProcessorResult &result, bool planar_too = false);

std::string patch_kind_name(int kind);

std::string fallback_reason_text(const std::string &reason);
std::string fallback_class(const std::string &reason);
bool fallback_is_exact(const std::string &reason);
bool fallback_hatched(const std::string &reason);

void record_fallback_shape(PatchDiagnostics &diag, ExPolygons shape, const HeightField &surface);

}
