#pragma once
#include "libslic3r/Nonplanar/HeadEnvelope.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
struct GCodeProcessorResult;
class ToolKinematics;
struct JointPose;
class Print;

class MaterialColumns
{
public:
    MaterialColumns(const BoundingBoxf &bbox, double pitch);
    void deposit(const Vec3d &a, const Vec3d &b, double width);
    double top_in_rect(const BoundingBoxf &rect) const;
    std::vector<double> tops(const BoundingBoxf &rect, Vec2d &origin, int &nx, double &pitch) const;
    double ceiling() const { return m_ceiling; }
    double clearance(const Vec3d &tip, const Nonplanar::HeadEnvelope &head, std::string *component = nullptr) const;
    double swept_clearance(const Vec3d &a, const Vec3d &b, const Nonplanar::HeadEnvelope &head,
                           std::string *component = nullptr) const;
    struct PreparedBox { Vec3d min, max; bool infinite_x, infinite_y; size_t index; };
    struct PreparedHead
    {
        std::vector<PreparedBox> boxes;
        Vec2d finite_min { Vec2d::Constant(std::numeric_limits<double>::infinity()) };
        Vec2d finite_max { Vec2d::Constant(-std::numeric_limits<double>::infinity()) };
    };
    static PreparedHead prepare(const Nonplanar::HeadEnvelope &head);
    double swept_clearance(const Vec3d &a, const Vec3d &b, const PreparedHead &head, double bound, int *box = nullptr,
                           int hint = -1) const;
    double tilted_clearance(const Vec3d &tip, const Matrix3d &frame, const Nonplanar::HeadEnvelope &head) const;
    void merge(const MaterialColumns &other);
    double tile_bound(const BoundingBoxf &rect) const;
private:
    void cells(const BoundingBoxf &rect, int &x0, int &y0, int &x1, int &y1) const;
    double cell_top(int x, int y) const
    {
        const auto &block = m_blocks[size_t(y / m_tile) * m_tx + x / m_tile];
        return !block ? -std::numeric_limits<double>::infinity() : (*block)[size_t(y % m_tile) * m_tile + x % m_tile];
    }
    double top_above(const BoundingBoxf &rect, double floor) const;
    std::vector<double> &writable(size_t t);
    BoundingBoxf m_bbox;
    double m_pitch;
    int m_nx, m_ny, m_tile, m_tx;
    std::vector<double> m_tiles;
    std::vector<std::shared_ptr<std::vector<double>>> m_blocks;
    double m_ceiling = -std::numeric_limits<double>::infinity();
};

double safe_travel_z(const MaterialColumns &columns, const Nonplanar::HeadEnvelope &head,
                     const Vec2d &a, const Vec2d &b, double hop);
enum class ClearanceHitClass { Body, Cone, Land };
struct ClearanceHit
{
    size_t move_id = 0;
    unsigned int gcode_line = 0;
    std::string component;
    ClearanceHitClass cls = ClearanceHitClass::Body;
    double penetration = 0.;
    Vec3d tip { Vec3d::Zero() };
    unsigned int layer = 0;
    uint32_t struck_move = uint32_t(-1);
    bool duct = false;
};
struct ClearanceParams
{
    Nonplanar::HeadEnvelope head;
    double pitch = 0.25;
    double sweep = 0.25;
    double bead_eps = 0.05;
    bool contact_bar = false;
    double cone_tolerance = 0.05;
};
struct ClearanceReport
{
    bool ran = false;
    size_t hit_moves = 0;
    size_t machine_hit_moves = 0;
    size_t tolerated_moves = 0;
    size_t cone_graze_moves = 0;
    bool   clearance_override = false;
    size_t duct_graze_moves   = 0;
    size_t duct_moves         = 0;
    double max_contact = 0.;
    size_t land_strict_moves = 0;
    size_t hit_land_moves = 0, hit_cone_moves = 0, hit_body_moves = 0, hit_travel_moves = 0, hit_extrusion_moves = 0;
    double min_clearance = std::numeric_limits<double>::infinity();
    std::vector<float> per_move;
    std::vector<ClearanceHit> worst;
    std::vector<std::string> omissions;
    bool passed() const { return ran && hit_moves == 0; }
};
ClearanceReport verify_clearance(const GCodeProcessorResult &result, const ClearanceParams &params,
                                 const ToolKinematics &kinematics,
                                 const std::function<void()> &throw_if_canceled = {});
// Answers a repeated verify_clearance() from the last sweep while the moves, joints and parameters are bit-identical.
// Rotary kinematics always sweep afresh.
class ClearanceMemo
{
public:
    ClearanceReport verify(const GCodeProcessorResult &result, const ClearanceParams &params, const ToolKinematics &kinematics,
                           const std::function<void()> &throw_if_canceled = {});
private:
    static void fingerprint(const GCodeProcessorResult &result, std::vector<std::array<uint32_t, 8>> &moves,
                            std::vector<std::array<uint64_t, 5>> &joints);
    bool                                 m_valid = false;
    ClearanceParams                      m_params;
    ClearanceReport                      m_report;
    std::vector<std::array<uint32_t, 8>> m_moves, m_scratch_moves;
    std::vector<std::array<uint64_t, 5>> m_joints, m_scratch_joints;
};
class TableGuard
{
public:
    TableGuard(const Nonplanar::HeadEnvelope &head, const ToolKinematics &kinematics);
    ~TableGuard();
    bool   empty() const;
    double clearance(const JointPose &joints) const;
private:
    struct Data;
    std::unique_ptr<Data>  m_data;
    const ToolKinematics  &m_kinematics;
    double                 m_margin;
};
void mark_clearance_override(const Print &print, GCodeProcessorResult &result);
void check_export_clearance(Print &print, GCodeProcessorResult &result);
}
