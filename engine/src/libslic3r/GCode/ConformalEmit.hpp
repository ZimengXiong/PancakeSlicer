#pragma once
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/Nonplanar/BeadModel.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

class ExtrusionPath;
class ExtrusionPathConformal;
class MaterialColumns;
class GCodeWriter;

namespace Nonplanar {

class PressureAdvanceTracker;

struct ConformalPoint
{
    ToolPose pose;
    float    bead_height = 0.f;
    float    vertical_height = 0.f;
    float    width       = 0.f;
};

ConformalPoint interpolate(const ConformalPoint &a, const ConformalPoint &b, double t);

using ToWriterFrame = std::function<Vec3d(const Point3 &)>;

constexpr double GlideLift = 0.1;
constexpr double UprightRoadDeg = 5.;

std::vector<ConformalPoint> conformal_points(const ExtrusionPathConformal &path, const ToWriterFrame &to_writer);

struct ConditionParams
{
    double target_length   = 0.15;
    double max_length      = 2.;
    double tolerance       = 0.01;
    double z_tolerance     = 0.005;
    double min_tolerance   = 0.03;
    double min_z_tolerance = 0.005;
    double axis_tolerance  = 0.01;
    double bead_tolerance  = 0.005;
    double glide_z_tolerance = 0.5 * GlideLift;
};
void condition_path(std::vector<ConformalPoint> &points, const ConditionParams &params);

double planner_segment_rate(double configured, GCodeFlavor flavor, bool bbl_printer);
constexpr double ConformalWipe  = 1.;
constexpr double ConformalMinSegment = 0.05;
constexpr double ConformalMinPath = 0.1;
constexpr double ConformalPrime = 0.5;
constexpr double ConformalBelowFirstLayerTolerance = 0.005;

struct ConformalEmitParams
{
    double       nominal_speed = 0.;
    double       max_volume_per_second = 0.;
    double       glide_speed   = 0.;
    double       e_per_mm3     = 0.;
    MotionLimits limits;
    ConditionParams condition;
    bool         preserve_points = false;
    double       arc_tolerance = 0.;
    std::string  comment;
    std::string  speed_marker;
    std::function<std::vector<double>(const std::vector<ConformalPoint> &)> speed_caps;
    double       speed_scale   = 1.;
    double       prime         = 0.;
    double       prime_length  = 0.;
    double       prime_speed   = 0.;
    float       *last_width = nullptr;
    double       ramp_length   = 0.;
    double       taper_length  = 0.;
    double       coast_length  = 0.;
    bool         open_end      = false;
    bool         scored        = false;
    PressureAdvanceTracker *pressure_advance = nullptr;
    std::function<double(const Vec3d &tip, const Vec3d &axis)> clearance;
    double       margin = 0.;
    std::function<std::vector<std::string>(const std::vector<ConformalPoint> &, const std::vector<double> &)> segment_tags;
};

struct ConformalSpeedParams
{
    double nominal_speed    = 0.;
    double max_volumetric   = 0.;
    double reference_height = 0.;
    double min_ratio        = 0.5;
    double min_speed        = 0.;
    Vec3d  max_joint_speed  { Vec3d::Zero() };
};
std::vector<double> conformal_speed_caps(const std::vector<ConformalPoint> &points, double width,
                                         const std::function<Vec2d(const Vec2d &)> &gradient, const ToolKinematics &kinematics,
                                         const JointPose &start, const Vec3d &writer_offset, const ConformalSpeedParams &params);

using DepositFn = std::function<void(const Vec3d &, const Vec3d &, double)>;
double emit_conformal_extrusion(GCodeWriter &writer, const std::vector<ConformalPoint> &points, double width,
                                const ConformalEmitParams &params, float &last_height, std::string &out,
                                const DepositFn &deposit = {});

struct ConformalWipeParams
{
    double       distance = 0.;
    double       retract  = 0.;
    double       speed    = 0.;
    MotionLimits limits;
    double       max_v_e  = 0.;
    std::string  speed_marker;
    std::function<double(const Vec3d &tip, const Vec3d &axis)> clearance;
    double       margin = 0.;
};
bool emit_conformal_wipe(GCodeWriter &writer, const std::vector<ConformalPoint> &points, const ConformalWipeParams &params,
                           std::string &out, Vec3d &end);

void deposit_material_path(MaterialColumns &columns, const ExtrusionPath &path, const ToWriterFrame &to_writer,
                           const std::function<double(double)> &z_at_fraction = {});

bool densify_conformal_path(const ExtrusionPathConformal &path, double max_segment, ExtrusionPathConformal &out);

struct RotaryRun
{
    size_t    begin = 0, end = 0;
    JointPose seed;
    bool      reversed = false;
};
std::vector<RotaryRun> plan_rotary_runs(const ToolKinematics &kinematics, const std::vector<ConformalPoint> &points,
                                        const JointPose &from, double max_jump_deg = 90., bool reseed = false);

bool alternate_rotary_runs(const ToolKinematics &kinematics, const std::vector<ConformalPoint> &points, const JointPose &from,
                           std::vector<RotaryRun> &runs, double max_jump_deg = 90.);

struct MinimalTiltParams
{
    std::vector<double> slope_limit;
    double              margin    = 3. * M_PI / 180.;
    double              smoothing = 2.;
    double              max_rate  = 0.;
    double              max_tilt  = 0.5 * M_PI;
    double              max_residual = 0.5 * M_PI;
    double              residual_from = 0.;
    std::function<double(const Vec3d &tip, const Vec3d &axis)> clearance;
    double              clearance_margin = 0.;
};
void apply_minimal_tilt(std::vector<ConformalPoint> &points, const std::function<Vec2d(const Vec2d &)> &gradient,
                        const ToolKinematics &kinematics, const MinimalTiltParams &params);

}
}
