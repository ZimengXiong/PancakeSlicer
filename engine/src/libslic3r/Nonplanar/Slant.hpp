#pragma once
#include "LayerField.hpp"
#include "SlantChoice.hpp"
#include "libslic3r/Point.hpp"

#include <memory>
#include <optional>

namespace Slic3r {
class PrintObject;
}

namespace Slic3r::Nonplanar {

struct SlantShape
{
    Vec2d  u { Vec2d::UnitX() };
    double u_max = 0.;
    double width = 0.;
    double tilt  = 0.;
    double first = 0.2;
    double start = 0.2;
    double h     = 0.2;
    int    morph = 10;
    double gap_lo = 0.;
    double gap_hi = 0.;
};

class SlantField final : public LayerField
{
public:
    explicit SlantField(const SlantShape &shape);
    double phi(const Vec3d &p) const override;
    double z_at(const Vec2d &xy, double level) const override;
    double identity_below() const override { return m_s.start; }
    bool   cut() const { return m_s.morph <= 0; }
    std::optional<double> cut_limit(int k) const;
    const SlantShape &shape() const { return m_s; }
    double low_gap() const { return m_low; }
    double dt() const { return m_dt; }

private:
    double s_of(const Vec2d &xy) const;
    double plane(double s) const;
    double z_of(double k, double s) const;

    SlantShape m_s;
    double     m_k0  = 0.;
    double     m_dt  = 0.;
    double     m_low = 0.;
    int        m_kmax = 0;
};

std::shared_ptr<const LayerField> slant_field(const PrintObject &object, SlantChoice &choice);

}
