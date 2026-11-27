#pragma once

#include <admesh/stl.h>
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Point.hpp"
#include "FieldOrder.hpp"
#include "HeadEnvelope.hpp"
#include "LayerField.hpp"

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class PrintObject;
class PrintObjectConfig;
}

namespace Slic3r::Nonplanar {

struct ConformalLayersParams
{
    double max_angle    = 30.;
    int    shells       = 5;
    double min_ramp     = 0.4;
    double layer_height = 0.2;
    double first_layer  = 0.2;
    double body_height  = 0.;
    double level_anchor = 0.;
    int    flat_layers  = 3;
    double pitch        = 0.25;
    double min_area     = 0.;
    double round_sigma  = 8.;
    double max_thickness = 0.;
    std::vector<Vec3d> struck;
};

struct ConformalTop
{
    double      area_mm2 = 0.;
    double      z_min = 0., z_max = 0.;
    double      level = 0.;
    double      ramp  = 0.;
    double      stepped_mm2 = 0.;
    std::string fallback;
};

class ConformalLayerField final : public LayerField
{
public:
    double phi(const Vec3d &p) const override;
    double z_at(const Vec2d &xy, double level) const override;
    double identity_below() const override { return m_identity_below; }
    double level_anchor() const { return m_level_anchor; }
    double band_height() const { return m_band_height; }
    double body_height() const { return m_body_height; }
    std::vector<double> levels(double first, double top) const;
    double lowest_ramp_floor() const;

    std::vector<ConformalTop> tops;

private:
    friend std::shared_ptr<const ConformalLayerField> build_conformal_layer_field(const indexed_triangle_set &, const ConformalLayersParams &);
    struct Column { double T = 0., c = 1., L = 0., zb = 0., d = 0., tf = 0., omega = 0., ease = 0.; };
    double column_phi(const Column &col, double z) const;
    double column_z(const Column &col, double level) const;
    Column        column(const Vec2d &xy) const;

    Vec2d               m_origin { Vec2d::Zero() };
    double              m_pitch = 0.25;
    size_t              m_nx = 0, m_ny = 0;
    double              m_shell_depth = 0.;
    double              m_level_anchor = 0., m_band_height = 0.2, m_body_height = 0.2;
    double              m_identity_below = 0.;
    std::vector<Column> m_cols;
};

std::shared_ptr<const ConformalLayerField> build_conformal_layer_field(const indexed_triangle_set &mesh, const ConformalLayersParams &params);

struct ConformalVariant { double round_sigma = 8.; double cap_deg = 0.; bool flat = false; };
ConformalVariant conformal_variant(const PrintObjectConfig &config, int step);
std::string conformal_variant_text(const ConformalVariant &variant);
constexpr int    ConformalSteps = 4;
std::string conformal_planar_limit(const PrintObject &object, std::string &reason);
bool conformal_requested(const PrintObject &object);

std::shared_ptr<const ConformalLayerField> conformal_layers_field(const PrintObject &object, std::string *summary = nullptr);

double conformal_band_height(const HeadEnvelope &head, double layer_height);

std::string contour_solid_angles(const LayerField &field, const std::vector<ExPolygons> &domains, const std::vector<double> &levels,
                                 double flat_angle, bool sparse = false, double *dominant = nullptr);

size_t layer_order_head_conflicts(const std::vector<FieldRoad> &roads, size_t layers, const HeadEnvelope &head);

struct BandOrderStats
{
    double band = 0., offset = 0.;
    size_t bands = 0, groups = 0, cut_roads = 0;
    std::vector<long> group_bands;
};

void link_roads(ExtrusionEntitiesPtr &roads, const ExPolygons *area, double fill_reach = 0.);

double band_offset(const std::vector<const std::vector<FieldRoad> *> &sets, double band);

FieldOrder band_order_roads(std::vector<FieldRoad> &roads, size_t layers, double band, BandOrderStats *stats = nullptr,
                            const std::vector<std::pair<Vec3d, Vec3d>> *precede = nullptr,
                            const std::vector<ExPolygons> *domains = nullptr,
                            double shared_offset = std::numeric_limits<double>::quiet_NaN());

}
