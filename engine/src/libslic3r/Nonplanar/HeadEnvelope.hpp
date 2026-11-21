#pragma once
#include "libslic3r/Point.hpp"
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r { class BoundingBoxf; class PrintConfig; }

namespace Slic3r::Nonplanar {

struct HeadBox
{
    std::string name;
    Vec3d       min { Vec3d::Zero() };
    Vec3d       max { Vec3d::Zero() };
    bool        infinite_x = false;
    bool        infinite_y = false;
    bool        nozzle     = false;
    bool        block      = false;
    bool        duct       = false;
};

struct TipAllowance
{
    double radius = 0.;
    double height = 0.;
};

struct SlopeStarSample { double azimuth, max_clean_slope; };
struct SlopeStarReading
{
    std::string date;
    std::vector<SlopeStarSample> samples;
};

struct HeadEnvelope
{
    std::string          id;
    std::string          name;
    std::vector<HeadBox> boxes;
    double               tip_flat_radius = 0.5;
    TipAllowance         tip_allowance;
    double               margin = 0.3;
    double               lateral_margin = 0.3;
    bool                 calibrated = false;
    double               contact_tolerance = 0.;
    size_t               duct_graze_limit  = 0;
    std::string          provenance;
    std::string extensions;

    bool empty() const { return boxes.empty(); }

    double underside_height(const Vec2d &d) const;

    double max_slope(double phi, double gap, double reach = std::numeric_limits<double>::infinity()) const;

    std::vector<double> slope_map(int n, double gap) const;

    static HeadEnvelope from_slope_star(const HeadEnvelope &base, const SlopeStarReading &reading, float layer_height);

    double gantry_clearance() const;

    double reach() const;
};

struct LowBox
{
    Vec2d  min, max;
    double floor;
    bool   infinite_x, infinite_y;
};

std::vector<LowBox> low_boxes(const HeadEnvelope &head, double gap, double max_relief);
bool conflicts(const std::vector<LowBox> &boxes, const BoundingBoxf &a, double relief_a, const BoundingBoxf &b, double relief_b);

bool operator==(const HeadBox &lhs, const HeadBox &rhs);
bool operator==(const TipAllowance &lhs, const TipAllowance &rhs);
bool operator==(const HeadEnvelope &lhs, const HeadEnvelope &rhs);
inline bool operator!=(const HeadEnvelope &lhs, const HeadEnvelope &rhs) { return !(lhs == rhs); }

struct SideProfileRow { double height, xpos, xneg, ypos, yneg; };

HeadEnvelope envelope_from_side_profile(const std::vector<SideProfileRow> &rows, double top,
                                        double gantry_height, double gantry_ymin, double gantry_ymax);

struct NSideProfileRow { double height; std::vector<double> reach; };
std::vector<HeadBox> boxes_from_n_side_profile(const std::vector<NSideProfileRow> &rows, double top);

HeadEnvelope load_head_envelope(const std::string &json_text);
std::string  save_head_envelope(const HeadEnvelope &envelope);

class HeadLibrary
{
public:
    static HeadLibrary &instance();
    const HeadEnvelope      *find(const std::string &id) const;
    std::vector<std::string> ids() const;
    std::string              source_path(const std::string &id) const;
    void                     reload();
    void                     save_user_head(const HeadEnvelope &head);
    static const HeadEnvelope &generic_conservative();

private:
    HeadLibrary() = default;
    void load_locked() const;

    mutable std::mutex                          m_mutex;
    mutable bool                                m_loaded = false;
    mutable std::map<std::string, HeadEnvelope> m_heads;
    mutable std::map<std::string, std::string> m_sources;
};

HeadEnvelope resolve_head_envelope(const PrintConfig &config, std::string *warning = nullptr);
HeadEnvelope resolve_full_head_envelope(const PrintConfig &config, std::string *warning = nullptr);
bool clearance_override_active(const PrintConfig &config);

bool native_support_capable(const PrintConfig &config);

}
