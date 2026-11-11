#pragma once
#include "libslic3r/Nonplanar/Repair.hpp"
#include "libslic3r/Point.hpp"
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class Print;
class PrintObject;
class ModelObject;
struct GCodeProcessorResult;
struct ClearanceReport;
struct ClearanceParams;
class ClearanceMemo;
class ToolKinematics;

namespace Nonplanar {

struct HeadEnvelope;

class ClearanceRepair
{
public:
    explicit ClearanceRepair(Print &print);
    ~ClearanceRepair();
    ClearanceRepair(const ClearanceRepair &) = delete;
    ClearanceRepair &operator=(const ClearanceRepair &) = delete;

    static void check_export(Print &print, GCodeProcessorResult &result);

    static size_t curved_contacts(const Print &print, const GCodeProcessorResult &result);

    bool refuse_struck_morph(const GCodeProcessorResult &result);
    bool step_down_morph_direction(const GCodeProcessorResult &result);
    bool step_down_conformal(const GCodeProcessorResult &result);
    bool step_down_slant(const GCodeProcessorResult &result);

    bool restore_planar_remainders();

    bool apply();

    bool plan(const GCodeProcessorResult &result, const ClearanceReport &report, const HeadEnvelope &head, bool bar = false);
    bool plan_upright(const GCodeProcessorResult &result, const ClearanceReport &report);

private:
    static ClearanceReport verify(const Print &print, const GCodeProcessorResult &result, const ClearanceParams &params,
                                  const ToolKinematics &kinematics);
    bool oneshot_step(GCodeProcessorResult &result);
    bool curved_tops_step(GCodeProcessorResult &result);
    int m_curved_pass = 0;
    int m_curved_angle_passes = 0;
    struct CurvedHit
    {
        PrintObject *owner      = nullptr;
        int          surface_id = -1;
        Vec2d        shift;
        Vec3d        tip, from;
        float        depth = 0.f;
    };
    using SurfaceKey = std::pair<const PrintObject *, int>;
    std::map<SurfaceKey, std::map<int, std::vector<CurvedHit>>> m_curved_rung_hits;
    std::set<SurfaceKey>         m_curved_roads_last;
    std::map<SurfaceKey, size_t> m_curved_hits_last;
    std::set<SurfaceKey>         m_curved_roads_stalled;
    bool contact_guard(const GCodeProcessorResult &result, size_t contacts = 0);

    struct Planned
    {
        PrintObject *object = nullptr;
        RepairHints  hints;
    };
    std::string summary(size_t hits_now) const;

    Print                   &m_print;
    std::unique_ptr<ClearanceMemo> m_memo;
    int                      m_max_passes = 0;
    int                      m_pass       = 0;
    size_t                   m_first_hits = 0;
    size_t                   m_last_hits  = 0;
    double                   m_last_min   = 0.;
    std::map<const PrintObject *, double> m_first_floating;
    size_t                   m_best_hits    = 0;
    int                      m_stale_passes = 0;
    std::vector<Planned>     m_planned;
    std::vector<PrintObject *> m_repick;
    std::map<const PrintObject *, int> m_repicks;
    std::map<const PrintObject *, bool> m_band_tried;
    int                      m_guard_passes = 0;
    std::vector<std::string> m_guard_warnings;
    std::vector<std::string> m_actions;
};

bool auto_morph_refused(const ModelObject &object);
size_t morph_struck_hits(const PrintObject &object);
bool top_picks_morph();
int conformal_cap_step(const ModelObject &object);
void conformal_ladder_slice(const ModelObject &object);
constexpr int SlantSteps = 6;
int    slant_step(const ModelObject &object);
size_t slant_step_hits(const ModelObject &object);
void   slant_ladder_slice(const ModelObject &object);
std::vector<std::pair<Vec3d, Vec3d>> conformal_precedence(const ModelObject &object);
size_t conformal_flat_hits(const ModelObject &object);
std::vector<Vec3d> conformal_struck_points(const ModelObject &object);
struct CurvedSurfacesRetry : std::runtime_error
{
    CurvedSurfacesRetry(size_t id, std::vector<int> surfaces) :
        std::runtime_error("curved tops: retry with surfaces planar"), object_id(id), surfaces(std::move(surfaces)) {}
    size_t           object_id = 0;
    std::vector<int> surfaces;
};

}
}
