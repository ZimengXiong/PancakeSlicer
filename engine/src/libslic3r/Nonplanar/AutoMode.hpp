#pragma once
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/WarningCode.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
class Print;
class PrintObject;
class PrintRegion;
}

namespace Slic3r::Nonplanar {

class HeightField;
struct ConformalPatch;
struct MultiplanarPlan;
struct HeadEnvelope;
struct ReportObject;

std::vector<uint8_t> auto_planar_nodes(const HeightField &surface, const std::vector<uint8_t> &candidate,
                                      double layer_height, double line_width, double max_slope);

bool auto_z_contouring(const PrintObject &object, const std::vector<double> &layers);

constexpr int AutoMinShells = 2;

double auto_curved_underside_slope();

bool top_shells_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object);
bool bottom_shells_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object);
double full_conformal_thickness(const PrintRegionConfig &config, const PrintObject &object);
bool overhangs_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object);
bool support_free(const PrintObjectConfig &object);
bool bridges_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object);
double infill_depth(const PrintRegionConfig &config, const PrintObject &object);

int planar_top_layers(const PrintRegionConfig &config, double shell_height);
NonplanarFallback effective_fallback(const PrintRegionConfig &config, const PrintObjectConfig &object);
bool zaa_fallback(const PrintRegionConfig &config, const PrintObjectConfig &object);
bool spiral_top_enabled(const PrintRegionConfig &config, const PrintObjectConfig &object);
NonplanarCollisionCheck effective_collision_check(const Print &print);

bool auto_multiplanar_candidate(double steep_top_share, double thin_top_share);
bool auto_multiplanar_chunks(const MultiplanarPlan &plan);
bool auto_multiplanar_confirmed(double chunks_rms, double planar_rms, double chunks_time, double planar_time);
double multiplanar_time_limit(double planar_time);

struct AutoObjectSettings
{
    double max_error        = 0.;
    double min_area         = 0.;
    double max_patch_height = 0.;
    int    clip_shells      = AutoMinShells;
    int    bottom_shells    = 0;
};
AutoObjectSettings auto_object_settings(const PrintObject &object, const PrintRegion &region, double shell_height);

struct AutoPatchSettings
{
    int                 shells  = AutoMinShells;
    NonplanarTopPattern pattern = NonplanarTopPattern::Contour;
    bool                ironing = false;
    double              max_error = 0.;
    double shell_area = 0.;
    double slope90    = 0.;
    double curvature95 = 0.;
    double curvature80 = 0.;
    double coherence  = 0.;
    double aspect     = 1.;
};

void choose_patch_settings(const PrintObject &object, const HeadEnvelope &head, double max_error, double first_layer_top,
                           ConformalPatch &patch, const ExPolygons *steep_faces = nullptr);

NonplanarTopPattern top_pattern(const ConformalPatch &patch, const PrintRegionConfig &config);
bool                ironing_enabled(const ConformalPatch &patch, const PrintRegionConfig &config);

std::string auto_comment(const ConformalPatch &patch);

std::string explain_top_patch(const PrintObject &object, const ConformalPatch &patch);
std::string describe_top_patch(const PrintObject &object, const ConformalPatch &patch);

std::vector<CodedWarning> object_warnings(const PrintObject &object, const ReportObject &report);

}
