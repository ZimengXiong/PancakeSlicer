#include "WipeTower2.hpp"

#include <algorithm>
#include <Eigen/Geometry>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <math.h>
#include <limits>
#include <utility>
#include <vector>
#include <numeric>
#include <memory>
#include <sstream>
#include <iomanip>

#include "libslic3r/Circle.hpp"
#include "libslic3r/ArcFitter.hpp"
#include "ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "GCodeProcessor.hpp"
#include "BoundingBox.hpp"
#include "libslic3r/Line.hpp"
#include "LocalesUtils.hpp"
#include "Geometry.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "PrintConfig.hpp"
#include "Surface.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/algorithm/string/predicate.hpp>
#include "libslic3r/Config.hpp"


namespace Slic3r
{

constexpr float         flat_iron_speed                = 10.f * 60.f;
static const double     wipe_tower_wall_infill_overlap = 0.0;
static constexpr double WIPE_TOWER_RESOLUTION          = 0.1;
static constexpr double     WT_SIMPLIFY_TOLERANCE_SCALED   = 0.001f / SCALING_FACTOR_INTERNAL;
static constexpr int    arc_fit_size                   = 20;
#define SCALED_WIPE_TOWER_RESOLUTION (WIPE_TOWER_RESOLUTION / SCALING_FACTOR_INTERNAL)
enum class LimitFlow { None, LimitPrintFlow, LimitRammingFlow };
static const std::map<float, float> nozzle_diameter_to_nozzle_change_width{{0.2f, 0.5f}, {0.4f, 1.0f}, {0.6f, 1.2f}, {0.8f, 1.4f}};

inline float align_round(float value, float base) { return std::round(value / base) * base; }

inline float align_ceil(float value, float base) { return std::ceil(value / base) * base; }

inline float align_floor(float value, float base) { return std::floor((value) / base) * base; }

static bool is_valid_gcode(const std::string& gcode)
{
    int  str_size    = gcode.size();
    int  start_index = 0;
    int  end_index   = 0;
    bool is_valid    = false;
    while (end_index < str_size) {
        if (gcode[end_index] != '\n') {
            end_index++;
            continue;
        }

        if (end_index > start_index) {
            std::string line_str = gcode.substr(start_index, end_index - start_index);
            line_str.erase(0, line_str.find_first_not_of(" "));
            line_str.erase(line_str.find_last_not_of(" ") + 1);
            if (!line_str.empty() && line_str[0] != ';') {
                is_valid = true;
                break;
            }
        }

        start_index = end_index + 1;
        end_index   = start_index;
    }

    return is_valid;
}

static Polygon chamfer_polygon(Polygon& polygon, double chamfer_dis = 2., double angle_tol = 30. / 180. * PI)
{
    if (polygon.points.size() < 3)
        return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));

    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        if (abs(cosangle) < cos_angle_tol) {
            float real_chamfer_dis = std::min({chamfer_dis, ab_len / 2.1, bc_len / 2.1});
            Vec2d left             = b - ab * real_chamfer_dis;
            Vec2d right            = b + bc * real_chamfer_dis;
            res.points.push_back(scaled(left));
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.points.shrink_to_fit();
    return res;
}

static Polygon rounding_polygon(Polygon& polygon, double rounding = 2., double angle_tol  = 30. / 180. * PI)
{
    if (polygon.points.size() < 3)
        return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));

    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        cosangle       = std::clamp(cosangle, -1.f, 1.f);
        bool is_ccw    = cross2(ab, bc) > 0;
        if (abs(cosangle) < cos_angle_tol) {
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1});
            Vec2d left              = b - ab * real_rounding_dis;
            Vec2d right             = b + bc * real_rounding_dis;
            {
                float half_angle = std::acos(cosangle) / 2.f;

                Vec2d dir  = (right - left).normalized();
                dir        = Vec2d{-dir[1], dir[0]};
                dir        = is_ccw ? dir : -dir;
                double dis = real_rounding_dis / sin(half_angle);

                Vec2d      center = b + dir * dis;
                double     radius = (left - center).norm();
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right),
                               is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n = arc_fit_size;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    res.points.push_back(tmp);
                }
            }
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.remove_duplicate_points();
    res.points.shrink_to_fit();
    return res;
}

static Polygon rounding_rectangle(Polygon& polygon, double rounding = 2., double angle_tol = 30. / 180. * PI)
{
    if (polygon.points.size() < 3)
        return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));

    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        cosangle       = std::clamp(cosangle, -1.f, 1.f);
        bool is_ccw    = cross2(ab, bc) > 0;
        if (abs(cosangle) < cos_angle_tol) {
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1});
            Vec2d left              = b - ab * real_rounding_dis;
            Vec2d right             = b + bc * real_rounding_dis;
            {
                Vec2d      center = b;
                double     radius = real_rounding_dis;
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right),
                               is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n = arc_fit_size;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    res.points.push_back(tmp);
                }
            }
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.points.shrink_to_fit();
    return res;
}

static Polygon scale_polygon(const std::vector<Vec2f>& points)
{
    Polygon res;
    for (const auto& p : points)
        res.points.push_back(scaled(p));
    return res;
}
static std::vector<Vec2f> unscale_polygon(const Polygon& polygon)
{
    std::vector<Vec2f> res;
    for (const auto& p : polygon.points)
        res.push_back(unscaled<float>(p));
    return res;
}

static Polygon generate_rectange(const Line& line, coord_t offset)
{
    Point p1 = line.a;
    Point p2 = line.b;

    double dx = p2.x() - p1.x();
    double dy = p2.y() - p1.y();

    double length = std::sqrt(dx * dx + dy * dy);

    double ux = dx / length;
    double uy = dy / length;

    double vx = -uy;
    double vy = ux;

    double ox = vx * offset;
    double oy = vy * offset;

    Points rect;
    rect.resize(4);
    rect[0] = {p1.x() + ox, p1.y() + oy};
    rect[1] = {p1.x() - ox, p1.y() - oy};
    rect[2] = {p2.x() - ox, p2.y() - oy};
    rect[3] = {p2.x() + ox, p2.y() + oy};
    Polygon poly(rect);
    return poly;
};

struct Segment
{
    Vec2f      start;
    Vec2f      end;
    bool       is_arc = false;
    ArcSegment arcsegment;
    Segment(const Vec2f& s, const Vec2f& e) : start(s), end(e) {}
    bool is_valid() const { return start.y() < end.y(); }
};

static Polygon generate_rectange_polygon(const Vec2f& wt_box_min, const Vec2f& wt_box_max)
{
    Polygon res;
    res.points.push_back(scaled(wt_box_min));
    res.points.push_back(scaled(Vec2f{wt_box_max[0], wt_box_min[1]}));
    res.points.push_back(scaled(wt_box_max));
    res.points.push_back(scaled(Vec2f{wt_box_min[0], wt_box_max[1]}));
    return res;
}

static float volume_to_length(float volume, float line_width, float layer_height)
{
	return std::max(0.f, volume / (layer_height * (line_width - layer_height * (1.f - float(M_PI) / 4.f))));
}

static float length_to_volume(float length, float line_width, float layer_height)
{
	return std::max(0.f, length * layer_height * (line_width - layer_height * (1.f - float(M_PI) / 4.f)));
}




class WipeTowerWriter2
{
public:
    WipeTowerWriter2(float layer_height,
                     float line_width,
                     GCodeFlavor flavor,
                     const std::vector<WipeTower2::FilamentParameters>& filament_parameters,
                     bool  enable_arc_fitting)
        :
		m_current_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
		m_current_z(0.f),
		m_current_feedrate(0.f),
		m_layer_height(layer_height),
		m_extrusion_flow(0.f),
		m_preview_suppressed(false),
		m_elapsed_time(0.f),
        m_gcode_flavor(flavor), m_filpar(filament_parameters)
    {
            GCodeProcessor::s_IsBBLPrinter = false;
            std::ostringstream str;
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << m_layer_height << "\n";
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) << ExtrusionEntity::role_to_string(erWipeTower) << "\n";
            m_gcode += str.str();
            change_analyzer_line_width(line_width);
    }

    WipeTowerWriter2& change_analyzer_line_width(float line_width) {
        std::stringstream str;
        str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << line_width << "\n";
        m_gcode += str.str();
        return *this;
    }

	WipeTowerWriter2& 			 set_initial_position(const Vec2f &pos, float width = 0.f, float depth = 0.f, float internal_angle = 0.f) {
        m_wipe_tower_width = width;
        m_wipe_tower_depth = depth;
        m_internal_angle = internal_angle;
		m_start_pos = this->rotate(pos);
		m_current_pos = pos;
		return *this;
	}

    WipeTowerWriter2& 			 set_position(const Vec2f &pos) { m_current_pos = pos; return *this; }

    WipeTowerWriter2&				 set_initial_tool(size_t tool) { m_current_tool = tool; return *this; }

	WipeTowerWriter2&				 set_z(float z) 
		{ m_current_z = z; return *this; }

	WipeTowerWriter2& 			 set_extrusion_flow(float flow)
		{ m_extrusion_flow = flow; return *this; }

	WipeTowerWriter2&				 set_y_shift(float shift) {
        m_current_pos.y() -= shift-m_y_shift;
        m_y_shift = shift;
        return (*this);
    }

    WipeTowerWriter2&            disable_linear_advance() {
        if (m_gcode_flavor == gcfRepRapSprinter || m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += (std::string("M572 D") + std::to_string(m_current_tool) + " S0\n");
        else if (m_gcode_flavor == gcfKlipper)
            m_gcode += "SET_PRESSURE_ADVANCE ADVANCE=0\n";
        else
            m_gcode += "M900 K0\n";
        return *this;
    }

    WipeTowerWriter2& switch_filament_monitoring(bool enable) {
        flush_planner_queue();
        m_gcode += enable ? "M591 R\n" : "M591 S0\n";
        return *this;
    }

    WipeTowerWriter2& 			 suppress_preview() { m_preview_suppressed = true; return *this; }
  	WipeTowerWriter2& 			 resume_preview()   { m_preview_suppressed = false; return *this; }

	WipeTowerWriter2& 			 feedrate(float f)
	{
        if (f != m_current_feedrate) {
			m_gcode += "G1" + set_format_F(f) + "\n";
            m_current_feedrate = f;
        }
		return *this;
	}

	const std::string&   gcode() const { return m_gcode; }
	const std::vector<WipeTower::Extrusion>& extrusions() const { return m_extrusions; }
	float                x()     const { return m_current_pos.x(); }
	float                y()     const { return m_current_pos.y(); }
	const Vec2f& 		 pos()   const { return m_current_pos; }
	const Vec2f	 		 start_pos_rotated() const { return m_start_pos; }
	const Vec2f  		 pos_rotated() const { return this->rotate(m_current_pos); }
	const Vec2f  		 rotated(const Vec2f &pt) const { return this->rotate(pt); }
	float 				 elapsed_time() const { return m_elapsed_time; }
    float                get_and_reset_used_filament_length() { float temp = m_used_filament_length; m_used_filament_length = 0.f; return temp; }

	WipeTowerWriter2& extrude_explicit(float x, float y, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
	{
		if (x == m_current_pos.x() && y == m_current_pos.y() && e == 0.f && (f == 0.f || f == m_current_feedrate))
			return *this;

		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        float len = std::sqrt(dx*dx+dy*dy);
        if (record_length)
            m_used_filament_length += e;

		Vec2f rotated_current_pos(this->pos_rotated());
		Vec2f rot(this->rotate(Vec2f(x,y)));

        if (! m_preview_suppressed && e > 0.f && len > 0.f) {
      float width = e * m_filpar[0].filament_area / (len * m_layer_height);
			width += m_layer_height * float(1. - M_PI / 4.);
			if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
				m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
			m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
		}

		m_gcode += "G1";
        if (std::abs(rot.x() - rotated_current_pos.x()) > (float)EPSILON)
			m_gcode += set_format_X(rot.x());

        if (std::abs(rot.y() - rotated_current_pos.y()) > (float)EPSILON)
			m_gcode += set_format_Y(rot.y());


		if (e != 0.f)
			m_gcode += set_format_E(e);

		if (f != 0.f && f != m_current_feedrate) {
            if (limit_volumetric_flow) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
            }
			m_gcode += set_format_F(f);
        }

        if (! boost::ends_with(m_gcode, "G1"))
            m_gcode += "\n";
        else
            m_gcode.erase(m_gcode.end()-2, m_gcode.end());

        m_current_pos.x() = x;
        m_current_pos.y() = y;

        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
		return *this;
	}

	WipeTowerWriter2& extrude_explicit(const Vec2f &dest, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
		{ return extrude_explicit(dest.x(), dest.y(), e, f, record_length); }

	WipeTowerWriter2& travel(float x, float y, float f = 0.f)
		{ return extrude_explicit(x, y, 0.f, f); }

	WipeTowerWriter2& travel(const Vec2f &dest, float f = 0.f) 
		{ return extrude_explicit(dest.x(), dest.y(), 0.f, f); }

	WipeTowerWriter2& extrude(float x, float y, float f = 0.f)
	{
		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        return extrude_explicit(x, y, std::sqrt(dx*dx+dy*dy) * m_extrusion_flow, f, true);
	}

	WipeTowerWriter2& extrude(const Vec2f &dest, const float f = 0.f) 
		{ return extrude(dest.x(), dest.y(), f); }

    WipeTowerWriter2& rectangle(const Vec2f& ld,float width,float height,const float f = 0.f)
    {
        Vec2f corners[4];
        corners[0] = ld;
        corners[1] = ld + Vec2f(width,0.f);
        corners[2] = ld + Vec2f(width,height);
        corners[3] = ld + Vec2f(0.f,height);
        int index_of_closest = 0;
        if (x()-ld.x() > ld.x()+width-x())
            index_of_closest = 1;
        if (y()-ld.y() > ld.y()+height-y())
            index_of_closest = (index_of_closest==0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y());
        travel(x(),corners[index_of_closest].y());

        int i = index_of_closest;
        do {
            ++i;
            if (i==4) i=0;
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }

    WipeTowerWriter2& rectangle(const WipeTower::box_coordinates& box, const float f = 0.f)
    {
        rectangle(Vec2f(box.ld.x(), box.ld.y()),
                  box.ru.x() - box.lu.x(),
                  box.ru.y() - box.rd.y(), f);
        return (*this);
    }

	WipeTowerWriter2& load(float e, float f = 0.f)
	{
		if (e == 0.f && (f == 0.f || f == m_current_feedrate))
			return *this;
		m_gcode += "G1";
		if (e != 0.f)
			m_gcode += set_format_E(e);
		if (f != 0.f && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter2& retract(float e, float f = 0.f)
		{ return load(-e, f); }

    WipeTowerWriter2& load_move_x_advanced(float farthest_x, float loading_dist, float loading_speed, float max_x_speed = 50.f)
    {
        float time = std::abs(loading_dist / loading_speed);
        float x_distance = std::abs(farthest_x - x());
        float x_speed = x_distance / time;

        if (x_speed > max_x_speed) {
            x_distance = max_x_speed * time;
            x_speed = max_x_speed;
        }

        float end_point = x() + (farthest_x > x() ? 1.f : -1.f) * x_distance;
        return extrude_explicit(end_point, y(), loading_dist, x_speed * 60.f, false, false);
    }

    WipeTowerWriter2& load_move_x_advanced_there_and_back(float farthest_x, float e_dist, float e_speed, float x_speed)
    {
        float old_x = x();
        float time = std::abs(e_dist / e_speed);
        float x_max_dist = std::abs(farthest_x - x());
        float x_dist = x_speed * time;
        int n = int(x_dist / (2*x_max_dist) + 1.f);
        float r = 2*n*x_max_dist / x_dist;

        float end_point = x() + (farthest_x > x() ? 1.f : -1.f) * x_max_dist / r;
        for (int i=0; i<n; ++i) {
            extrude_explicit(end_point, y(), e_dist/(2.f*n), x_speed * 60.f, false, false);
            extrude_explicit(old_x, y(), e_dist/(2.f*n), x_speed * 60.f, false, false);
        }
        return *this;
    }

	WipeTowerWriter2& z_hop(float hop, float f = 0.f)
	{ 
		m_gcode += std::string("G1") + set_format_Z(m_current_z + hop);
		if (f != 0 && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter2& z_hop_reset(float f = 0.f) 
		{ return z_hop(0, f); }

	WipeTowerWriter2& ram(float x1, float x2, float dy, float e0, float e, float f)
	{
		extrude_explicit(x1, m_current_pos.y() + dy, e0, f, true, false);
		extrude_explicit(x2, m_current_pos.y(), e, 0.f, true, false);
		return *this;
	}

	WipeTowerWriter2& cool(float x1, float x2, float e1, float e2, float f)
	{
		extrude_explicit(x1, m_current_pos.y(), e1, f, false, false);
		extrude_explicit(x2, m_current_pos.y(), e2, false, false);
		return *this;
	}

    WipeTowerWriter2& set_tool(size_t tool)
	{
		m_current_tool = tool;
		return *this;
	}

	WipeTowerWriter2& set_extruder_temp(int temperature, bool wait = false, const std::string& comment = std::string())
	{
        flush_planner_queue();
        m_gcode += "M" + std::to_string(wait ? 109 : 104) + " S" + std::to_string(temperature);
        if (!comment.empty())
            m_gcode += " " + comment;
        m_gcode += "\n";
        return *this;
    }

	WipeTowerWriter2& wait(float time)
	{
        if (time==0.f)
            return *this;
        m_gcode += wait_command(m_gcode_flavor, time);
		return *this;
    }

	WipeTowerWriter2& speed_override(int speed)
	{
        m_gcode += "M220 S" + std::to_string(speed) + "\n";
		return *this;
    }

	WipeTowerWriter2& speed_override_backup()
    {
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 B\n";
		return *this;
    }

	WipeTowerWriter2& speed_override_restore()
	{
        if (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware)
            m_gcode += "M220 R\n";
		return *this;
    }

	WipeTowerWriter2& set_extruder_trimpot(int current)
	{
        if (m_gcode_flavor == gcfKlipper)
            return *this;
        if (m_gcode_flavor == gcfRepRapSprinter || m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += "M906 E";
        else
            m_gcode += "M907 E";
        m_gcode += std::to_string(current) + "\n";
		return *this;
    }

	WipeTowerWriter2& flush_planner_queue()
	{
		m_gcode += flush_planner_queue_command(m_gcode_flavor);
		return *this;
	}

	WipeTowerWriter2& reset_extruder()
	{ 
		m_gcode += "G92 E0\n";
		return *this;
	}

	WipeTowerWriter2& comment_with_value(const char *comment, int value)
    {
        m_gcode += std::string(";") + comment + std::to_string(value) + "\n";
		return *this;
    }


    WipeTowerWriter2& set_fan(unsigned speed)
	{
		if (speed == m_last_fan_speed)
			return *this;
		if (speed == 0)
			m_gcode += "M107\n";
        else
            m_gcode += "M106 S" + std::to_string(unsigned(255.0 * speed / 100.0)) + "\n";
		m_last_fan_speed = speed;
		return *this;
	}

	WipeTowerWriter2& append(const std::string& text) { m_gcode += text; return *this; }

    const std::vector<Vec2f>& wipe_path() const
    {
        return m_wipe_path;
    }

    WipeTowerWriter2& add_wipe_point(const Vec2f& pt)
    {
        m_wipe_path.push_back(rotate(pt));
        return *this;
    }

    WipeTowerWriter2& add_wipe_point(float x, float y)
    {
        return add_wipe_point(Vec2f(x, y));
    }

    void spiral_flat_ironing(const Vec2f &center, float area, float step_length, float feedrate)
    {
        float edge_length = std::sqrt(area);
        Vec2f box_max     = center + Vec2f{step_length, step_length};
        Vec2f box_min     = center - Vec2f{step_length, step_length};
        int   n           = std::ceil(edge_length / step_length / 2.f);
        if (n <= 0)
            return;
        while (n--) {
            travel(box_max.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_max.y(), feedrate);
            travel(box_min.x(), m_current_pos.y(), feedrate);
            travel(m_current_pos.x(), box_min.y(), feedrate);

            box_max += Vec2f{step_length, step_length};
            box_min -= Vec2f{step_length, step_length};
        }
    }

    WipeTowerWriter2& extrude_arc_explicit(ArcSegment& arc,
                                          float       f             = 0.f,
                                          bool        record_length = false,
                                          LimitFlow   limit_flow    = LimitFlow::LimitPrintFlow)
    {
        float x   = (float) unscale(arc.end_point).x();
        float y   = (float) unscale(arc.end_point).y();
        float len = unscaled<float>(arc.length);
        float e   = len * m_extrusion_flow;
        if (len < (float) EPSILON && e == 0.f && (f == 0.f || f == m_current_feedrate))
            return *this;
        if (record_length)
            m_used_filament_length += e;

        Vec2f rotated_current_pos(this->pos_rotated());
        Vec2f rot(this->rotate(Vec2f(x, y)));

        if (!m_preview_suppressed && e > 0.f && len > 0.f) {
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
            change_analyzer_mm3_per_mm(len, e);
#endif
            float width = e * m_filpar[0].filament_area / (len * m_layer_height);
            width += m_layer_height * float(1. - M_PI / 4.);
            if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
                m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
            {
                int n = arc_fit_size;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    m_extrusions.emplace_back(WipeTower::Extrusion(this->rotate(unscaled<float>(tmp)), width, m_current_tool));
                }
                m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
            }
        }

        m_gcode += arc.direction == ArcDirection::Arc_Dir_CCW ? "G3" : "G2";
        const Vec2f center_offset = this->rotate(unscaled<float>(arc.center)) - rotated_current_pos;
        m_gcode += set_format_X(rot.x());
        m_gcode += set_format_Y(rot.y());
        m_gcode += set_format_I(center_offset.x());
        m_gcode += set_format_J(center_offset.y());

        if (e != 0.f)
            m_gcode += set_format_E(e);

        if (f != 0.f && f != m_current_feedrate) {
            if (limit_flow != LimitFlow::None) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                float tmp     = m_filpar[m_current_tool].max_e_speed;
                f /= std::max(1.f, e_speed / tmp);
            }
            m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
        m_gcode += "\n";
        return *this;
    }

    WipeTowerWriter2& extrude_arc(ArcSegment& arc, float f = 0.f, LimitFlow limit_flow = LimitFlow::LimitPrintFlow)
    {
        return extrude_arc_explicit(arc, f, false, limit_flow);
    }

    void              generate_path(Polylines& pls, float feedrate, float retract_length, float retract_speed, bool used_fillet)
    {
        auto get_closet_idx = [this](std::vector<Segment>& corners) -> int {
            Vec2f anchor{this->m_current_pos.x(), this->m_current_pos.y()};
            int   closestIndex = -1;
            float minDistance  = std::numeric_limits<float>::max();
            for (int i = 0; i < corners.size(); ++i) {
                float distance = (corners[i].start - anchor).squaredNorm();
                if (distance < minDistance) {
                    minDistance  = distance;
                    closestIndex = i;
                }
            }
            return closestIndex;
        };
        std::vector<Segment> segments;
        if (m_enable_arc_fitting) {
            for (auto& pl : pls)
                pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);

            for (const auto& pl : pls) {
                if (pl.points.size() < 2)
                    continue;
                for (int i = 0; i < pl.fitting_result.size(); i++) {
                    if (pl.fitting_result[i].path_type == EMovePathType::Linear_move) {
                        for (int j = pl.fitting_result[i].start_point_index; j < pl.fitting_result[i].end_point_index; j++)
                            segments.push_back({unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1])});
                    } else {
                        int beg = pl.fitting_result[i].start_point_index;
                        int end = pl.fitting_result[i].end_point_index;
                        segments.push_back({unscaled<float>(pl.points[beg]), unscaled<float>(pl.points[end])});
                        segments.back().is_arc     = true;
                        segments.back().arcsegment = pl.fitting_result[i].arc_data;
                    }
                }
            }
            for (auto& pl : pls)
                pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);

        } else {
            for (const auto& pl : pls) {
                if (pl.points.size() < 2)
                    continue;
                for (int i = 0; i < pl.size() - 1; i++) {
                    segments.push_back({unscaled<float>(pl.points[i]), unscaled<float>(pl.points[i + 1])});
                }
            }
        }

        int index_of_closest = get_closet_idx(segments);
        int i                = index_of_closest;
        travel(segments[i].start);
        segments[i].is_arc ? extrude_arc(segments[i].arcsegment, feedrate) : extrude(segments[i].end, feedrate);
        do {
            i = (i + 1) % segments.size();
            if (i == index_of_closest)
                break;
            float dx  = segments[i].start.x() - m_current_pos.x();
            float dy  = segments[i].start.y() - m_current_pos.y();
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > EPSILON) {
                retract(retract_length, retract_speed);
                travel(segments[i].start, 600.);
                retract(-retract_length, retract_speed);
            }
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, feedrate) : extrude(segments[i].end, feedrate);
        } while (1);
    }

private:
	Vec2f         m_start_pos;
	Vec2f         m_current_pos;
    std::vector<Vec2f>  m_wipe_path;
	float    	  m_current_z;
	float 	  	  m_current_feedrate;
    size_t        m_current_tool;
	float 		  m_layer_height;
	float 	  	  m_extrusion_flow;
	bool		  m_preview_suppressed;
	std::string   m_gcode;
	std::vector<WipeTower::Extrusion> m_extrusions;
	float         m_elapsed_time;
	float   	  m_internal_angle = 0.f;
	float		  m_y_shift = 0.f;
	float 		  m_wipe_tower_width = 0.f;
	float		  m_wipe_tower_depth = 0.f;
    unsigned      m_last_fan_speed = 0;
    int           current_temp = -1;
    float         m_used_filament_length = 0.f;
    GCodeFlavor   m_gcode_flavor;
    bool          m_enable_arc_fitting = false;
    const std::vector<WipeTower2::FilamentParameters>& m_filpar;

	std::string   set_format_X(float x)
    {
        m_current_pos.x() = x;
        return " X" + Slic3r::float_to_string_decimal_point(x, 3);
	}

	std::string   set_format_Y(float y) {
        m_current_pos.y() = y;
        return " Y" + Slic3r::float_to_string_decimal_point(y, 3);
	}

	std::string   set_format_Z(float z) {
        return " Z" + Slic3r::float_to_string_decimal_point(z, 3);
	}

	std::string   set_format_E(float e) {
        return " E" + Slic3r::float_to_string_decimal_point(e, 4);
	}

	std::string   set_format_F(float f) {
        char buf[64];
        sprintf(buf, " F%d", int(floor(f + 0.5f)));
        m_current_feedrate = f;
        return buf;
	}
    std::string       set_format_I(float i) { return " I" + Slic3r::float_to_string_decimal_point(i, 3); }
    std::string       set_format_J(float j) { return " J" + Slic3r::float_to_string_decimal_point(j, 3); }

	WipeTowerWriter2& operator=(const WipeTowerWriter2 &rhs);

	Vec2f rotate(Vec2f pt) const
	{
		pt.x() -= m_wipe_tower_width / 2.f;
		pt.y() += m_y_shift - m_wipe_tower_depth / 2.f;
	    double angle = m_internal_angle * float(M_PI/180.);
	    double c = cos(angle);
	    double s = sin(angle);
	    return Vec2f(float(pt.x() * c - pt.y() * s) + m_wipe_tower_width / 2.f, float(pt.x() * s + pt.y() * c) + m_wipe_tower_depth / 2.f);
	}

};



WipeTower::ToolChangeResult WipeTower2::construct_tcr(WipeTowerWriter2& writer,
                                                     bool priming,
                                                     size_t old_tool,
                                                     bool is_finish,
                                                     bool is_contact) const
{
    WipeTower::ToolChangeResult result;
    result.priming      = priming;
    result.initial_tool = int(old_tool);
    result.new_tool     = int(m_current_tool);
    result.print_z      = m_z_pos;
    result.layer_height = m_layer_height;
    result.elapsed_time = writer.elapsed_time();
    result.start_pos    = writer.start_pos_rotated();
    result.end_pos      = priming ? writer.pos() : writer.pos_rotated();
    result.gcode        = std::move(writer.gcode());
    result.extrusions   = std::move(writer.extrusions());
    result.wipe_path    = std::move(writer.wipe_path());
    result.is_finish_first = is_finish;
    result.is_contact = is_contact;
    result.tool_change_start_pos = result.start_pos;

    return result;
}



bool WipeTower2::use_gap_wall(const PrintConfig& config)
{
    return config.prime_tower_skip_points.value && config.wipe_tower_wall_type.value != wtwCone;
}

bool WipeTower2::wait_for_temp_enabled(const PrintConfig& config)
{
    return config.wait_for_temp_on_wipe_tower.value && !config.single_extruder_multi_material.value;
}

WipeTower2::WipeTower2(const PrintConfig& config, const PrintRegionConfig& default_region_config,int plate_idx, Vec3d plate_origin, const std::vector<std::vector<float>>& wiping_matrix, size_t initial_tool) :
    m_semm(config.single_extruder_multi_material.value),
    m_enable_filament_ramming(config.enable_filament_ramming.value),
    m_wipe_tower_filament(config.wipe_tower_filament.value),
    m_wipe_tower_pos(config.wipe_tower_x.get_at(plate_idx), config.wipe_tower_y.get_at(plate_idx)),
    m_wipe_tower_width(float(config.prime_tower_width)),
    m_wipe_tower_rotation_angle(float(config.wipe_tower_rotation_angle)),
    m_wipe_tower_brim_width(float(config.prime_tower_brim_width)),
    m_wipe_tower_cone_angle(float(config.wipe_tower_cone_angle)),
    m_extra_flow(float(config.wipe_tower_extra_flow/100.)),
    m_extra_spacing_wipe(float(config.wipe_tower_extra_spacing/100. * config.wipe_tower_extra_flow/100.)),
    m_extra_spacing_ramming(float(config.wipe_tower_extra_spacing/100.)),
    m_y_shift(0.f),
    m_z_pos(0.f),
    m_bridging(float(config.wipe_tower_bridging)),
    m_sparse_layers_skipped(wipe_tower_sparse_layers_skipped(config)),
    m_sparse_layers_combined(wipe_tower_sparse_layers_combined(config)),
    m_gcode_flavor(config.gcode_flavor),
    m_travel_speed(config.travel_speed.get_at(get_extruder_index(config, (unsigned int)initial_tool))),
    m_infill_speed(default_region_config.sparse_infill_speed.get_at(get_extruder_index(config, (unsigned int)initial_tool))),
    m_perimeter_speed(default_region_config.inner_wall_speed.get_at(get_extruder_index(config, (unsigned int)initial_tool))),
    m_current_tool(initial_tool),
    wipe_volumes(wiping_matrix), m_wipe_tower_max_purge_speed(float(config.wipe_tower_max_purge_speed)),
    m_enable_arc_fitting(config.enable_arc_fitting), 
    m_used_fillet(config.wipe_tower_fillet_wall), 
    m_rib_width(config.wipe_tower_rib_width), 
    m_extra_rib_length(config.wipe_tower_extra_rib_length),
    m_wall_type((int)config.wipe_tower_wall_type),
    m_use_gap_wall(use_gap_wall(config)),
    m_enable_tower_interface_features(config.enable_tower_interface_features.value),
    m_enable_tower_interface_cooldown_during_tower(config.enable_tower_interface_cooldown_during_tower.value),
    m_wait_for_temp_on_wipe_tower(wait_for_temp_enabled(config))
{
    const float default_speed = 60.f;
    m_first_layer_speed = config.initial_layer_speed.get_at(get_extruder_index(config, (unsigned int)initial_tool));
    if (m_first_layer_speed == 0.f)
        m_first_layer_speed = default_speed / 2.f;

    if (m_infill_speed == 0.f)
        m_infill_speed = 80.f;
    if (m_perimeter_speed == 0.f)
        m_perimeter_speed = 80.f;


    if (m_semm) {
        m_cooling_tube_retraction = float(config.cooling_tube_retraction);
        m_cooling_tube_length     = float(config.cooling_tube_length);
        m_parking_pos_retraction  = float(config.parking_pos_retraction);
        m_extra_loading_move      = float(config.extra_loading_move);
        m_set_extruder_trimpot    = config.high_current_on_filament_swap;
    }

    m_is_mk4mmu3 = boost::icontains(config.printer_notes.value, "PRINTER_MODEL_MK4") && boost::icontains(config.printer_notes.value, "MMU");

    const std::vector<Vec2d>& bed_points = config.printable_area.values;
    BoundingBoxf bb(bed_points);
    m_bed_width = float(bb.size().x());
    m_bed_shape = (bed_points.size() == 4 ? RectangularBed : CircularBed);

    if (m_bed_shape == CircularBed) {
        double r2 = std::pow(m_bed_width/2., 2.);
        double lim2 = std::pow(m_bed_width/10., 2.);
        Vec2d center = bb.center();
        for (const Vec2d& pt : bed_points)
            if (std::abs(std::pow(pt.x()-center.x(), 2.) + std::pow(pt.y()-center.y(), 2.) - r2) > lim2) {
                m_bed_shape = CustomBed;
                break;
            }
    }

    m_bed_bottom_left = m_bed_shape == RectangularBed
                  ? Vec2f(bed_points.front().x(), bed_points.front().y())
                  : Vec2f::Zero();
    m_bed_polygon = Polygon::new_scale(bed_points);
}



void WipeTower2::set_extruder(size_t idx, const PrintConfig& config)
{
    m_filpar.push_back(FilamentParameters());

    m_filpar[idx].material = config.filament_type.get_at(idx);
    if (m_wipe_tower_filament > 0)
        m_filpar[idx].is_soluble = (idx != size_t(m_wipe_tower_filament - 1));
    else
        m_filpar[idx].is_soluble = config.filament_soluble.get_at(idx);
    m_filpar[idx].is_support = config.filament_is_support.get_at(idx);
    m_filpar[idx].temperature = config.nozzle_temperature.get_at(idx);
    m_filpar[idx].first_layer_temperature = config.nozzle_temperature_initial_layer.get_at(idx);
    m_filpar[idx].filament_minimal_purge_on_wipe_tower = config.filament_minimal_purge_on_wipe_tower.get_at(idx);
    {
        int interface_temp = config.filament_tower_interface_print_temp.get_at(idx);
        if (interface_temp == -1)
            interface_temp = config.nozzle_temperature_range_high.get_at(idx);
        m_filpar[idx].interface_print_temperature = interface_temp;
    }
    m_filpar[idx].tower_interface_pre_extrusion_dist = config.filament_tower_interface_pre_extrusion_dist.get_at(idx);
    m_filpar[idx].tower_interface_pre_extrusion_length = config.filament_tower_interface_pre_extrusion_length.get_at(idx);
    m_filpar[idx].tower_ironing_area = config.filament_tower_ironing_area.get_at(idx);
    m_filpar[idx].tower_interface_purge_length = config.filament_tower_interface_purge_volume.get_at(idx);

    if (m_semm) {
        m_filpar[idx].loading_speed           = float(config.filament_loading_speed.get_at(idx));
        m_filpar[idx].loading_speed_start     = float(config.filament_loading_speed_start.get_at(idx));
        m_filpar[idx].unloading_speed         = float(config.filament_unloading_speed.get_at(idx));
        m_filpar[idx].unloading_speed_start   = float(config.filament_unloading_speed_start.get_at(idx));
        m_filpar[idx].delay                   = float(config.filament_toolchange_delay.get_at(idx));
        m_filpar[idx].cooling_moves           = config.filament_cooling_moves.get_at(idx);
        m_filpar[idx].cooling_initial_speed   = float(config.filament_cooling_initial_speed.get_at(idx));
        m_filpar[idx].cooling_final_speed     = float(config.filament_cooling_final_speed.get_at(idx));
        m_filpar[idx].filament_stamping_loading_speed     = float(config.filament_stamping_loading_speed.get_at(idx));
        m_filpar[idx].filament_stamping_distance          = float(config.filament_stamping_distance.get_at(idx));
    }

    m_filpar[idx].filament_area = float((M_PI/4.f) * pow(config.filament_diameter.get_at(idx), 2));
    float nozzle_diameter = float(config.nozzle_diameter.get_at(idx));
    m_filpar[idx].nozzle_diameter = nozzle_diameter;
    {
        const std::vector<int> &filament_map = config.filament_map.values;
        const size_t nozzle_idx = idx < filament_map.size() && filament_map[idx] > 0 ? size_t(filament_map[idx] - 1) : 0;
        const float  max_layer_height = float(config.max_layer_height.get_at(nozzle_idx));
        m_filpar[idx].max_layer_height = max_layer_height > 0.f ? max_layer_height
                                                                : 0.75f * float(config.nozzle_diameter.get_at(nozzle_idx));
    }

    float max_vol_speed = float(config.filament_max_volumetric_speed.get_at(idx));
    if (max_vol_speed!= 0.f)
        m_filpar[idx].max_e_speed = (max_vol_speed / filament_area());

    m_perimeter_width = nozzle_diameter * Width_To_Nozzle_Ratio;

    if (m_semm) {
        std::istringstream stream{config.filament_ramming_parameters.get_at(idx)};
        float speed = 0.f;
        stream >> m_filpar[idx].ramming_line_width_multiplicator >> m_filpar[idx].ramming_step_multiplicator;
        m_filpar[idx].ramming_line_width_multiplicator /= 100;
        m_filpar[idx].ramming_step_multiplicator /= 100;
        while (stream >> speed)
            m_filpar[idx].ramming_speed.push_back(speed);
    } else {
        float vol  = config.filament_multitool_ramming_volume.get_at(idx);
        float flow = config.filament_multitool_ramming_flow.get_at(idx);
        m_filpar[idx].multitool_ramming = config.filament_multitool_ramming.get_at(idx) && vol > 0.f && flow > 0.f;
        m_filpar[idx].ramming_line_width_multiplicator = 2.;
        m_filpar[idx].ramming_step_multiplicator = 1.;

        m_filpar[idx].ramming_speed.push_back(flow);
        m_filpar[idx].multitool_ramming_time = flow > 0.f ? vol/flow : 0.f;
    }

    m_used_filament_length.resize(std::max(m_used_filament_length.size(), idx + 1));

    m_filpar[idx].retract_length = config.retraction_length.get_at(idx);
    m_filpar[idx].retract_speed  = config.retraction_speed.get_at(idx);
}



std::vector<WipeTower::ToolChangeResult> WipeTower2::prime(
	float 						initial_layer_print_height, 
	const std::vector<unsigned int> &tools,
    bool 						)
{
	this->set_layer(initial_layer_print_height, initial_layer_print_height, tools.size(), true, false);
	m_current_tool 		= tools.front();
    

    float prime_section_width = std::min(0.9f * m_bed_width / tools.size(), 60.f);
    WipeTower::box_coordinates cleaning_box(Vec2f(0.02f * m_bed_width, 0.01f + m_perimeter_width/2.f), prime_section_width, 100.f);
    if (m_bed_shape == CircularBed) {
        cleaning_box = WipeTower::box_coordinates(Vec2f(0.f, 0.f), prime_section_width, 100.f);
        float total_width_half = tools.size() * prime_section_width / 2.f;
        cleaning_box.translate(-total_width_half, -std::sqrt(std::max(0.f, std::pow(m_bed_width/2, 2.f) - std::pow(1.05f * total_width_half, 2.f))));
    }
    else
        cleaning_box.translate(m_bed_bottom_left);

    std::vector<WipeTower::ToolChangeResult> results;

    for (size_t idx_tool = 0; idx_tool < tools.size(); ++ idx_tool) {
        size_t old_tool = m_current_tool;

        WipeTowerWriter2 writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_enable_arc_fitting);
        writer.set_extrusion_flow(m_extrusion_flow)
              .set_z(m_z_pos)
              .set_initial_tool(m_current_tool);

        if (idx_tool == 0) {
            writer.append(";--------------------\n"
                          "; CP PRIMING START\n")
                  .append(";--------------------\n")
                  .speed_override_backup()
                  .speed_override(100)
                  .set_initial_position(Vec2f::Zero())
                  .travel(cleaning_box.ld, 7200);
            if (m_set_extruder_trimpot)
                writer.set_extruder_trimpot(750);
        }
        else
            writer.set_initial_position(results.back().end_pos);


        unsigned int tool = tools[idx_tool];
        m_left_to_right = true;
        toolchange_Change(writer, tool, m_filpar[tool].material, m_filpar[tool].first_layer_temperature, false);
        toolchange_Load(writer, cleaning_box);
        if (idx_tool + 1 == tools.size()) {
            toolchange_Wipe(writer, cleaning_box, wipe_volumes[tools[idx_tool-1]][tool], false, true);
        } else {
            toolchange_Wipe(writer, cleaning_box , 20.f, false, true);
            WipeTower::box_coordinates box = cleaning_box;
            box.translate(0.f, writer.y() - cleaning_box.ld.y() + m_perimeter_width);
            toolchange_Unload(writer, box , m_filpar[m_current_tool].material, m_filpar[m_current_tool].first_layer_temperature, m_filpar[tools[idx_tool + 1]].first_layer_temperature);
            cleaning_box.translate(prime_section_width, 0.f);
            writer.travel(cleaning_box.ld, 7200);
        }
        ++ m_num_tool_changes;


        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

        if (idx_tool+1 == tools.size()) {
            if (m_set_extruder_trimpot)
                writer.set_extruder_trimpot(550);
            writer.speed_override_restore()
                  .feedrate(m_travel_speed * 60.f)
                  .flush_planner_queue()
                  .reset_extruder()
                  .append("; CP PRIMING END\n"
                          ";------------------\n"
                          "\n\n");
        }

        results.emplace_back(construct_tcr(writer, true, old_tool, true, false));
    }

    m_old_temperature = -1;

	return results;
}

WipeTower::ToolChangeResult WipeTower2::tool_change(size_t tool)
{
    size_t old_tool = m_current_tool;

    float wipe_area = 0.f;
	float wipe_volume = 0.f;
    float ramming_depth = 0.f;
    bool interface_layer = m_enable_tower_interface_features && m_current_layer_has_interface;

	if (tool != (unsigned int)(-1))
	{
		for (const auto &b : m_layer_info->tool_changes)
			if ( b.new_tool == tool ) {
                wipe_volume = b.wipe_volume;
				wipe_area = b.required_depth;
                ramming_depth = b.ramming_depth;
				break;
			}
	}
	else {
	}
    if (interface_layer && tool != (unsigned int)(-1) && tool < m_filpar.size()) {
        float extra_purge_length = m_filpar[tool].tower_interface_purge_length;
        if (extra_purge_length > 0.f) {
            wipe_volume += extra_purge_length * m_filpar[tool].filament_area;
        }
    }

    WipeTower::box_coordinates cleaning_box(
		Vec2f(m_perimeter_width / 2.f, m_perimeter_width / 2.f),
		m_wipe_tower_width - m_perimeter_width,
        (tool != (unsigned int)(-1) ? wipe_area+m_depth_traversed-0.5f*m_perimeter_width
                                    : m_wipe_tower_depth-m_perimeter_width));

	WipeTowerWriter2 writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_enable_arc_fitting);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift + (tool!=(unsigned int)(-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth(): 0.f))
		.append(";--------------------\n"
				"; CP TOOLCHANGE START\n");

    if (tool != (unsigned)(-1)){
        writer.comment_with_value(" toolchange #", m_num_tool_changes + 1);
        writer.append(std::string("; material : " + (m_current_tool < m_filpar.size() ? m_filpar[m_current_tool].material : "(NONE)") + " -> " + m_filpar[tool].material + "\n").c_str())
            .append(";--------------------\n");
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
    }

    writer.speed_override_backup();
	writer.speed_override(100);

    Vec2f initial_position = toolchange_entry_pos(m_depth_traversed, ramming_depth, is_first_layer());
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

	if (m_set_extruder_trimpot)
		writer.set_extruder_trimpot(750);

    if (tool != (unsigned int)-1){
        const bool fill_box = !tool_ramming_enabled(m_current_tool) || boundary_wipe_start_enabled(m_current_tool);
        auto new_tool_temp = is_first_layer() ? m_filpar[tool].first_layer_temperature : m_filpar[tool].temperature;
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material,
                          (is_first_layer() ? m_filpar[m_current_tool].first_layer_temperature : m_filpar[m_current_tool].temperature),
                          new_tool_temp);
        int wait_for_temp = interface_layer && m_filpar[tool].interface_print_temperature > 0 ?
                                m_filpar[tool].interface_print_temperature :
                                (is_first_layer() || m_filpar[tool].temperature == 0 ? m_filpar[tool].first_layer_temperature : m_filpar[tool].temperature);
        toolchange_Change(writer, tool, m_filpar[tool].material, wait_for_temp, true);
        toolchange_Load(writer, cleaning_box);
        writer.travel(writer.x(), writer.y()-m_perimeter_width);
        int base_temp = is_first_layer() ? m_filpar[tool].first_layer_temperature : m_filpar[tool].temperature;
        if (interface_layer) {
            int interface_temp = m_filpar[tool].interface_print_temperature;
            if (interface_temp > 0 && interface_temp != base_temp && !m_wait_for_temp_on_wipe_tower)
                writer.set_extruder_temp(interface_temp, true);
            if (m_enable_tower_interface_cooldown_during_tower && interface_temp > 0 && interface_temp != base_temp)
                writer.set_extruder_temp(base_temp, false);
            float pre_dist = m_filpar[tool].tower_interface_pre_extrusion_dist;
            float pre_len = m_filpar[tool].tower_interface_pre_extrusion_length;
            if (pre_dist > 0.f && pre_len > 0.f) {
                float target_x = writer.x() + pre_dist;
                target_x = std::max(cleaning_box.ld.x(), std::min(cleaning_box.rd.x(), target_x));
                writer.extrude_explicit(target_x, writer.y(), pre_len, 600.f);
            }
        }
        toolchange_Wipe(writer, cleaning_box, wipe_volume, interface_layer, false, fill_box);
        if (interface_layer) {
            int interface_temp = m_filpar[tool].interface_print_temperature;
            if (!m_enable_tower_interface_cooldown_during_tower && interface_temp > 0 && interface_temp != base_temp)
                writer.set_extruder_temp(base_temp, false);
        }
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");
        ++ m_num_tool_changes;
    } else
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material, m_filpar[m_current_tool].temperature, m_filpar[m_current_tool].temperature);

    m_depth_traversed += wipe_area;

	if (m_set_extruder_trimpot)
		writer.set_extruder_trimpot(550);
	writer.speed_override_restore();
    writer.feedrate(m_travel_speed * 60.f)
          .flush_planner_queue()
          .reset_extruder()
          .append("; CP TOOLCHANGE END\n"
                  ";------------------\n"
                  "\n\n");

    if (m_current_tool < m_used_filament_length.size())
        m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, false, interface_layer);
}


void WipeTower2::toolchange_Unload(
	WipeTowerWriter2 &writer,
	const WipeTower::box_coordinates 	&cleaning_box,
	const std::string&		 current_material,
    const int                old_temperature,
	const int 				 new_temperature)
{
	float xl = cleaning_box.ld.x() + 1.f * m_perimeter_width;
	float xr = cleaning_box.rd.x() - 1.f * m_perimeter_width;

    const float line_width = m_perimeter_width * m_filpar[m_current_tool].ramming_line_width_multiplicator;
	const float y_step = line_width * m_filpar[m_current_tool].ramming_step_multiplicator * m_extra_spacing_ramming;

    const Vec2f ramming_start_pos = Vec2f(xl, cleaning_box.ld.y() + m_depth_traversed + y_step/2.f);

    writer.append("; CP TOOLCHANGE UNLOAD\n")
        .change_analyzer_line_width(line_width);

	unsigned i = 0;
	m_left_to_right = true;
	float remaining = xr - xl ;
	float e_done = 0;

    const bool do_ramming = tool_ramming_enabled(m_current_tool);
    const bool cold_ramming = m_is_mk4mmu3;
    const bool boundary_wipe_start = boundary_wipe_start_enabled(m_current_tool);
    float planned_ramming_depth = 0.f;
    if (boundary_wipe_start && m_layer_info != m_plan.end())
        for (const auto& tch : m_layer_info->tool_changes)
            if (tch.old_tool == m_current_tool) { planned_ramming_depth = tch.ramming_depth; break; }

    if (do_ramming) {
        if (boundary_wipe_start)
            writer.travel(Vec2f(ramming_start_pos.x(), writer.y()));
        writer.travel(ramming_start_pos);
        if (! m_is_mk4mmu3)
            writer.disable_linear_advance();
        if (cold_ramming)
            writer.set_extruder_temp(old_temperature - 20);
    }
    else
        writer.set_position(ramming_start_pos);

    if (do_ramming && !boundary_wipe_start && (m_layer_info > m_plan.begin() && m_layer_info < m_plan.end() && (m_layer_info-1!=m_plan.begin() || !m_adhesion ))) {

        float sparse_beginning_y = 0.f;
        if (m_current_shape == SHAPE_REVERSED)
            sparse_beginning_y += ((m_layer_info-1)->depth - (m_layer_info-1)->toolchanges_depth())
                                      - ((m_layer_info)->depth-(m_layer_info)->toolchanges_depth()) ;
        else
            sparse_beginning_y += (m_layer_info-1)->toolchanges_depth() + m_perimeter_width;

        float sum_of_depths = 0.f;
        for (const auto& tch : m_layer_info->tool_changes) {
            if (tch.old_tool == m_current_tool) {
                sum_of_depths += tch.ramming_depth;
                float ramming_end_y = sum_of_depths;
                ramming_end_y -= (y_step/m_extra_spacing_ramming-m_perimeter_width) / 2.f;

                if ( (m_current_shape == SHAPE_REVERSED   && ramming_end_y < sparse_beginning_y - 0.5f*m_perimeter_width  ) ||
                     (m_current_shape == SHAPE_NORMAL && ramming_end_y > sparse_beginning_y + 0.5f*m_perimeter_width  )  )
                {
                    writer.extrude(xl + tch.first_wipe_line-1.f*m_perimeter_width,writer.y());
                    remaining -= tch.first_wipe_line-1.f*m_perimeter_width;
                }
                break;
            }
            sum_of_depths += tch.required_depth;
        }
    }

    if (m_is_mk4mmu3) {
        writer.switch_filament_monitoring(false);
        writer.wait(1.5f);
    }
    

    while (do_ramming && i < m_filpar[m_current_tool].ramming_speed.size())
    {
        const float time_step = m_semm ? 0.25f : m_filpar[m_current_tool].multitool_ramming_time;

        const float x = volume_to_length(m_filpar[m_current_tool].ramming_speed[i] * time_step, line_width, m_layer_height);
        const float e = m_filpar[m_current_tool].ramming_speed[i] * time_step / filament_area();
        const float dist = std::min(x - e_done, remaining);
        const float actual_time = dist/x * time_step;
        writer.ram(writer.x(), writer.x() + (m_left_to_right ? 1.f : -1.f) * dist, 0.f, 0.f, e * (dist / x), dist / (actual_time / 60.f));
        remaining -= dist;

		if (remaining < WT_EPSILON)	{
			writer.travel(writer.x(), writer.y() + y_step, 7200);
			m_left_to_right = !m_left_to_right;
			remaining = xr - xl;
		}
		e_done += dist;
		if (e_done > x - WT_EPSILON) {
			++i;
			e_done = 0;
		}
	}

    if (planned_ramming_depth > 0.f) {
        const int   reserved_rows = std::max(1, int(std::round(planned_ramming_depth / y_step)));
        const float last_row_y    = ramming_start_pos.y() + (reserved_rows - 1) * y_step;
        const float e_per_mm      = 1.f / (volume_to_length(1.f, line_width, m_layer_height) * filament_area());
        const float fill_feed     = m_filpar[m_current_tool].ramming_speed.empty() ? 3000.f :
            60.f * volume_to_length(m_filpar[m_current_tool].ramming_speed.back(), line_width, m_layer_height);
        while (true) {
            const float target_x = m_left_to_right ? xr : xl;
            if (std::abs(target_x - writer.x()) > WT_EPSILON)
                writer.ram(writer.x(), target_x, 0.f, 0.f, e_per_mm * std::abs(target_x - writer.x()), fill_feed);
            if (writer.y() + 0.5f * y_step > last_row_y)
                break;
            writer.travel(writer.x(), writer.y() + y_step, 7200);
            m_left_to_right = !m_left_to_right;
        }
    }

	Vec2f end_of_ramming(writer.x(),writer.y());
    writer.change_analyzer_line_width(m_perimeter_width);
    if(m_enable_filament_ramming)
        writer.append("; Ramming start\n");

    float old_x = writer.x();
    float turning_point = (!m_left_to_right ? xl : xr );
    if (m_enable_filament_ramming && m_semm && (m_cooling_tube_retraction != 0 || m_cooling_tube_length != 0)) {
        writer.append("; Retract(unload)\n");
        float total_retraction_distance = m_cooling_tube_retraction + m_cooling_tube_length/2.f - 15.f;
        writer.suppress_preview()
              .retract(15.f, m_filpar[m_current_tool].unloading_speed_start * 60.f)
              .retract(0.70f * total_retraction_distance, 1.0f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.20f * total_retraction_distance, 0.5f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.10f * total_retraction_distance, 0.3f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .resume_preview();
    }

    const int& number_of_cooling_moves = m_filpar[m_current_tool].cooling_moves;
    const bool cooling_will_happen = m_enable_filament_ramming && m_semm && number_of_cooling_moves > 0 && m_cooling_tube_length != 0;
    bool change_temp_later = false;

    if (m_semm) {
        if (new_temperature != 0 && (new_temperature != m_old_temperature || is_first_layer() || cold_ramming) ) {
            if (cold_ramming && cooling_will_happen)
                change_temp_later = true;
            else
                writer.set_extruder_temp(new_temperature, false);
            m_old_temperature = new_temperature;
        }
    }

    if (cooling_will_happen) {
        writer.append("; Cooling\n");
        const float& initial_speed = m_filpar[m_current_tool].cooling_initial_speed;
        const float& final_speed   = m_filpar[m_current_tool].cooling_final_speed;

        float speed_inc = (final_speed - initial_speed) / (2.f * number_of_cooling_moves - 1.f);

        if (m_is_mk4mmu3)
            writer.disable_linear_advance();

        writer.suppress_preview()
              .travel(writer.x(), writer.y() + y_step);
        old_x = writer.x();
        turning_point = xr-old_x > old_x-xl ? xr : xl;
        float stamping_dist_e = m_filpar[m_current_tool].filament_stamping_distance + m_cooling_tube_length / 2.f;

        for (int i=0; i<number_of_cooling_moves; ++i) {

            if (i>0 && m_filpar[m_current_tool].filament_stamping_distance != 0) {

                float stamping_turning_point = std::clamp(old_x + 20.f * (turning_point - old_x > 0.f ? 1.f : -1.f), xl, xr);

                if (stamping_dist_e > 5) {
                    float cent = writer.x();
                    writer.load_move_x_advanced(stamping_turning_point, (stamping_dist_e - 5), m_filpar[m_current_tool].filament_stamping_loading_speed, 200);
                    writer.load_move_x_advanced(cent, 5, m_filpar[m_current_tool].filament_stamping_loading_speed, m_travel_speed);
                    writer.travel(cent, writer.y());
                } else
                    writer.load_move_x_advanced_there_and_back(stamping_turning_point, stamping_dist_e, m_filpar[m_current_tool].filament_stamping_loading_speed, m_travel_speed);

                writer.retract(stamping_dist_e, m_filpar[m_current_tool].unloading_speed * 60.f);
            }

            if (i == number_of_cooling_moves - 1 && change_temp_later) {
                    writer.set_extruder_temp(new_temperature, false);
            }

            float speed = initial_speed + speed_inc * 2*i;
            writer.load_move_x_advanced(turning_point, m_cooling_tube_length, speed);
            speed += speed_inc;
            writer.load_move_x_advanced(old_x, -m_cooling_tube_length, speed);
        }
    }

    if (m_enable_filament_ramming && m_semm) {
        writer.append("; Cooling park\n");
        writer.wait(m_filpar[m_current_tool].delay);
        const auto _e = -m_cooling_tube_length / 2.f + m_parking_pos_retraction - m_cooling_tube_retraction;
        if (_e != 0.f)
            writer.retract(_e, 2000);
    }

    if(m_enable_filament_ramming)
        writer.append("; Ramming end\n");


    Vec2f pos = Vec2f(end_of_ramming.x(), end_of_ramming.y() + (y_step/m_extra_spacing_ramming-m_perimeter_width) / 2.f + m_perimeter_width);
    if (planned_ramming_depth > 0.f) {
        writer.travel(Vec2f(ramming_start_pos.x(),
            cleaning_box.ld.y() + m_depth_traversed +
                wipe_start_offset_after_ram(planned_ramming_depth, is_first_layer()) + m_perimeter_width), 2400.f);
        m_left_to_right = true;
    }
    else if (do_ramming)
        writer.travel(pos, 2400.f);
    else {
        writer.set_position(Vec2f(end_of_ramming.x(),
            cleaning_box.ld.y() + m_depth_traversed + wipe_start_offset_after_ram(0.f, is_first_layer()) + m_perimeter_width));
    }

    writer.resume_preview()
          .flush_planner_queue();
}

void WipeTower2::toolchange_Change(
	WipeTowerWriter2 &writer,
    const size_t 	new_tool,
    const std::string&  new_material,
    const int       wait_for_temp,
    const bool      wait_beside_tower)
{
    if (m_current_tool < m_used_filament_length.size())
    	m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    writer.append("[change_filament_gcode]\n");

    if (m_is_mk4mmu3)
        writer.switch_filament_monitoring(true);

    const bool wait_for_temp_here = m_wait_for_temp_on_wipe_tower && wait_for_temp > 0;

    if (wait_for_temp_here && wait_beside_tower) {
        float min_x = 0.f, max_x = m_wipe_tower_width;
        if (m_wall_type == (int)wtwRib) {
            WipeTower::box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
            const BoundingBox rib_bbox = get_extents(generate_rib_polygon(wt_box));
            min_x = std::min(min_x, unscaled<float>(rib_bbox.min.x()));
            max_x = std::max(max_x, unscaled<float>(rib_bbox.max.x()));
        } else if (m_wall_type == (int)wtwCone) {
            const double support_scale = get_wipe_tower_cone_base(m_wipe_tower_width, m_wipe_tower_height, m_wipe_tower_depth,
                                                                  m_wipe_tower_cone_angle).second;
            const double z = m_sparse_layers_skipped ? (m_current_height + m_layer_info->height) : m_layer_info->z;
            const double r = std::tan(Geometry::deg2rad(m_wipe_tower_cone_angle / 2.f)) * (m_wipe_tower_height - z);
            const double w = m_layer_info->depth + m_perimeter_width;
            if (r > 0.5 * w + 0.01) {
                const float bulge = float(std::sqrt(r * r - 0.25 * w * w) / support_scale);
                min_x = std::min(min_x, m_wipe_tower_width / 2.f - bulge);
                max_x = std::max(max_x, m_wipe_tower_width / 2.f + bulge);
            }
        }
        if (is_first_layer()) {
            const float brim = m_wipe_tower_brim_width < 0.f ? WipeTower::get_auto_brim_by_height(m_wipe_tower_height) :
                                                               m_wipe_tower_brim_width;
            min_x -= brim;
            max_x += brim;
        }
        constexpr float gap     = 2.f;
        constexpr float min_gap = 0.5f;
        const bool      on_left = writer.x() < m_wipe_tower_width / 2.f;
        const float     near_x  = on_left ? min_x - gap : max_x + gap;
        const float     far_x   = on_left ? max_x + gap : min_x - gap;
        const Eigen::Rotation2Df to_bed(float(Geometry::deg2rad(m_wipe_tower_rotation_angle)));
        auto park_pt_on_bed = [this, &writer, to_bed](float side_x) {
            const Vec2f bed_pt = to_bed * (writer.rotated(Vec2f(side_x, writer.y())) + m_rib_offset) + m_wipe_tower_pos;
            return m_bed_polygon.contains(Point::new_scale(bed_pt.x(), bed_pt.y()));
        };
        float park_x    = near_x;
        bool  have_park = park_pt_on_bed(near_x);
        if (!have_park) {
            const float limit_x = on_left ? min_x - min_gap : max_x + min_gap;
            if (park_pt_on_bed(limit_x)) {
                float on = limit_x, off = near_x;
                for (int i = 0; i < 8; ++i) {
                    const float mid = 0.5f * (on + off);
                    if (park_pt_on_bed(mid))
                        on = mid;
                    else
                        off = mid;
                }
                park_x    = on;
                have_park = true;
            }
        }
        if (!have_park && park_pt_on_bed(far_x)) {
            park_x    = far_x;
            have_park = true;
        }
        if (have_park) {
            const Vec2f stop = writer.rotated(Vec2f(park_x, writer.y()));
            writer.feedrate(m_travel_speed * 60.f)
                  .append(std::string("G1 X") + Slic3r::float_to_string_decimal_point(stop.x())
                                     +  " Y"  + Slic3r::float_to_string_decimal_point(stop.y())
                                     + never_skip_tag() + "\n");
        }
        writer.set_extruder_temp(wait_for_temp, true, wait_for_temp_tag());
    }

    Vec2f current_pos = writer.pos_rotated();
    writer.feedrate(m_travel_speed * 60.f)
          .append(std::string("G1 X") + Slic3r::float_to_string_decimal_point(current_pos.x())
                             +  " Y"  + Slic3r::float_to_string_decimal_point(current_pos.y())
                             + never_skip_tag() + "\n"
    );

    if (wait_for_temp_here && !wait_beside_tower)
        writer.set_extruder_temp(wait_for_temp, true, wait_for_temp_tag());

    writer.append("[deretraction_from_wipe_tower_generator]");

	writer.set_tool(new_tool);


	writer.flush_planner_queue();
	m_current_tool = new_tool;
}

void WipeTower2::toolchange_Load(
	WipeTowerWriter2 &writer,
	const WipeTower::box_coordinates  &cleaning_box)
{
    if (m_semm && m_enable_filament_ramming && (m_parking_pos_retraction != 0 || m_extra_loading_move != 0)) {
        float xl = cleaning_box.ld.x() + m_perimeter_width * 0.75f;
        float xr = cleaning_box.rd.x() - m_perimeter_width * 0.75f;
        float oldx = writer.x();
        float turning_point = ( oldx-xl < xr-oldx ? xr : xl );
        float edist = m_parking_pos_retraction+m_extra_loading_move;

        writer.append("; CP TOOLCHANGE LOAD\n")
              .suppress_preview()
              .load(0.2f * edist, 60.f * m_filpar[m_current_tool].loading_speed_start)
              .load_move_x_advanced(turning_point, 0.7f * edist,        m_filpar[m_current_tool].loading_speed)
              .load_move_x_advanced(oldx,          0.1f * edist, 0.1f * m_filpar[m_current_tool].loading_speed)

              .travel(oldx, writer.y())
              .resume_preview();

        if (m_set_extruder_trimpot)
            writer.set_extruder_trimpot(550);
    }
}

void WipeTower2::toolchange_Wipe(
	WipeTowerWriter2 &writer,
	const WipeTower::box_coordinates  &cleaning_box,
	float wipe_volume,
    bool interface_layer,
    bool priming,
    bool fill_box)
{
    writer.set_extrusion_flow(m_extrusion_flow * (is_first_layer() ? 1.18f : 1.f))
		  .append("; CP TOOLCHANGE WIPE\n");
	const float& xl = cleaning_box.ld.x();
	const float& xr = cleaning_box.rd.x();

    writer.set_extrusion_flow(m_extrusion_flow * m_extra_flow);
    const float line_width = wipe_line_width();
    writer.change_analyzer_line_width(line_width);

	float x_to_wipe = volume_to_length(wipe_volume, m_perimeter_width, m_layer_height) / m_extra_flow;
	float dy = wipe_row_spacing(is_first_layer());
    const float target_speed = is_first_layer() || (m_num_tool_changes <= 1 && m_sparse_layers_skipped) ? m_first_layer_speed * 60.f : std::min(m_wipe_tower_max_purge_speed * 60.f, m_infill_speed * 60.f);
    float wipe_speed = 0.33f * target_speed;

    if ((m_left_to_right ? xr-writer.x() : writer.x()-xl) < 2.5f*line_width) {
        writer.travel((m_left_to_right ? xr-line_width : xl+line_width),writer.y()+dy);
        m_left_to_right = !m_left_to_right;
    }
    
	for (int i = 0; true; ++i)	{
		if (i!=0) {
            if      (wipe_speed < 0.34f * target_speed) wipe_speed = 0.375f * target_speed;
            else if (wipe_speed < 0.377 * target_speed) wipe_speed = 0.458f * target_speed;
            else if (wipe_speed < 0.46f * target_speed) wipe_speed = 0.875f * target_speed;
            else wipe_speed = std::min(target_speed, wipe_speed + 50.f);
		}

		float traversed_x = writer.x();

        if (i == 0 && m_use_gap_wall && !interface_layer && !priming && m_left_to_right &&
            writer.x() - xl < 2.5f * line_width) {
            float ironing_length = 3.f;
            if (xr - writer.x() < ironing_length)
                ironing_length = std::max(xr - writer.x(), 0.f);
            const float retract_length = m_filpar[m_current_tool].retract_length;
            const float retract_speed  = m_filpar[m_current_tool].retract_speed * 60.f;
            writer.extrude(writer.x() + ironing_length, writer.y(), wipe_speed);
            writer.retract(retract_length, retract_speed);
            writer.travel(writer.x() - 1.5f * ironing_length, writer.y(), 600.f);
            writer.travel(writer.x() + 0.5f * ironing_length, writer.y(), 240.f);
            const Vec2f iron_end(writer.x() + ironing_length, writer.y());
            writer.spiral_flat_ironing(writer.pos(), m_filpar[m_current_tool].tower_ironing_area, m_perimeter_width, flat_iron_speed);
            writer.travel(iron_end, wipe_speed);
            writer.retract(-retract_length, retract_speed);
        }

		if (m_left_to_right)
            writer.extrude(xr - (i % 4 == 0 ? 0 : 1.5f*line_width), writer.y(), wipe_speed);
		else
            writer.extrude(xl + (i % 4 == 1 ? 0 : 1.5f*line_width), writer.y(), wipe_speed);

        if (writer.y()+float(EPSILON) > cleaning_box.lu.y()-0.5f*line_width)
            break;

		traversed_x -= writer.x();
        x_to_wipe -= std::abs(traversed_x);
        if (!fill_box && x_to_wipe < WT_EPSILON) {
            writer.travel(m_left_to_right ? xl + 1.5f*line_width : xr - 1.5f*line_width, writer.y(), 7200);
			break;
		}
        writer.extrude(writer.x() + (i % 4 == 0 ? -1.f : (i % 4 == 1 ? 1.f : 0.f)) * 1.5f*line_width, writer.y() + dy);
		m_left_to_right = !m_left_to_right;
	}

    writer.add_wipe_point(writer.x(), writer.y())
          .add_wipe_point(writer.x(), writer.y() - dy)
          .add_wipe_point(! m_left_to_right ? m_wipe_tower_width : 0.f, writer.y() - dy);

    if (m_layer_info != m_plan.end() && m_current_tool != m_layer_info->tool_changes.back().new_tool)
        m_left_to_right = !m_left_to_right;

    writer.set_extrusion_flow(m_extrusion_flow);
    writer.change_analyzer_line_width(m_perimeter_width);
}




WipeTower::ToolChangeResult WipeTower2::finish_layer()
{
	assert(! this->layer_finished());
    m_current_layer_finished = true;

    size_t old_tool = m_current_tool;

	WipeTowerWriter2 writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_enable_arc_fitting);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));


    bool first_layer = is_first_layer() || (m_num_tool_changes <= 1 && m_sparse_layers_skipped);
    float                      feedrate      = first_layer ? m_first_layer_speed * 60.f : std::min(m_wipe_tower_max_purge_speed * 60.f, m_infill_speed * 60.f);
    if (m_enable_tower_interface_features && m_prev_layer_had_interface)
        feedrate = std::min(feedrate, 20.f * 60.f);
    float current_depth = m_layer_info->depth - m_layer_info->toolchanges_depth();
    WipeTower::box_coordinates fill_box(Vec2f(m_perimeter_width, m_layer_info->depth-(current_depth-m_perimeter_width)),
                             m_wipe_tower_width - 2 * m_perimeter_width, current_depth-m_perimeter_width);


    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu),
                                 m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    if (fill_box.ru.y() - fill_box.rd.y() > m_perimeter_width - WT_EPSILON)
        writer.rectangle(fill_box.ld, fill_box.rd.x()-fill_box.ld.x(), fill_box.ru.y()-fill_box.rd.y(), feedrate);

    if (writer.x() > fill_box.ld.x()+EPSILON) writer.travel(fill_box.ld.x(),writer.y());
    if (writer.y() > fill_box.ld.y()+EPSILON) writer.travel(writer.x(),fill_box.ld.y());

    const float dy = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
    float left = fill_box.lu.x() + 2*m_perimeter_width;
    float right = fill_box.ru.x() - 2 * m_perimeter_width;
    if (dy > m_perimeter_width)
    {
        writer.travel(fill_box.ld + Vec2f(m_perimeter_width * 2, 0.f))
            .append(";--------------------\n"
                    "; CP EMPTY GRID START\n")
            .comment_with_value(" layer #", m_num_layer_changes + 1);

        bool solid_infill = m_layer_info+1 == m_plan.end()
                          ? false
                          : std::any_of((m_layer_info+1)->tool_changes.begin(),
                                        (m_layer_info+1)->tool_changes.end(),
                                            [this](const WipeTowerInfo::ToolChange& tch) {
                                       return m_filpar[tch.new_tool].is_soluble
                                           || m_filpar[tch.old_tool].is_soluble;
                                            });
        solid_infill |= first_layer && m_adhesion;

        if (solid_infill) {
            float sparse_factor = 1.5f;
            if (first_layer) {
                left  -= m_perimeter_width;
                right += m_perimeter_width;
                sparse_factor = 1.f;
            }
            float y = fill_box.ld.y() + m_perimeter_width;
            int n = dy / (m_perimeter_width * sparse_factor);
            float spacing = (dy-m_perimeter_width)/(n-1);
            int i=0;
            for (i=0; i<n; ++i) {
                writer.extrude(writer.x(), y, feedrate)
                      .extrude(i%2 ? left : right, y);
                y = y + spacing;
            }
            writer.extrude(writer.x(), fill_box.lu.y());
        } else {
            writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);

            const int n = 1+int((right-left)/m_bridging);
            const float dx = (right-left)/n;
            for (int i=1;i<=n;++i) {
                float x=left+dx*i;
                writer.travel(x,writer.y());
                writer.extrude(x,i%2 ? fill_box.rd.y() : fill_box.ru.y());
            }
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    const float spacing = m_perimeter_width - m_layer_height*float(1.-M_PI_4);
    feedrate = first_layer ? m_first_layer_speed * 60.f : std::min(m_wipe_tower_max_purge_speed * 60.f, m_perimeter_speed * 60.f);

    Polygon poly;
    if (m_wall_type == (int)wtwCone) {
         WipeTower::box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f)),
                                           m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
        bool infill_cone = first_layer && m_wipe_tower_width > 2 * spacing && m_wipe_tower_depth > 2 * spacing;
        poly = generate_support_cone_wall(writer, wt_box, feedrate, infill_cone, spacing);
    } else {
        WipeTower::box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
        poly = generate_support_rib_wall(writer, wt_box, feedrate, first_layer, m_wall_type == (int)wtwRib, true);
    }

    if (first_layer) {
        writer.append("; WIPE_TOWER_BRIM_START\n");
            float brim_width = m_wipe_tower_brim_width;
        if (brim_width < 0.f)
            brim_width = WipeTower::get_auto_brim_by_height(m_wipe_tower_height);

        size_t loops_num = (brim_width + spacing / 2.f) / spacing;
        
        for (size_t i = 0; i < loops_num; ++ i) {
            poly = offset(poly, scale_(spacing)).front();
            int cp = poly.closest_point_index(Point::new_scale(writer.x(), writer.y()));
            writer.travel(unscale(poly.points[cp]).cast<float>());
            for (int i=cp+1; true; ++i ) {
                if (i==int(poly.points.size()))
                    i = 0;
                writer.extrude(unscale(poly.points[i]).cast<float>());
                if (i == cp)
                    break;
            }
        }
        writer.append("; WIPE_TOWER_BRIM_END\n");
        m_wipe_tower_brim_width_real = loops_num * spacing;

        BoundingBox first_layer_box = get_extents(poly);
        m_first_layer_bbx = BoundingBoxf(unscale(first_layer_box.min), unscale(first_layer_box.max));
    }

    int i = poly.closest_point_index(Point::new_scale(writer.x(), writer.y()));
    writer.add_wipe_point(writer.pos());
    writer.add_wipe_point(unscale(poly.points[i==0 ? int(poly.points.size())-1 : i-1]).cast<float>());

    const bool combined_away = m_layer_info != m_plan.end() && m_layer_info->combined_away;
    if ((! m_sparse_layers_skipped || toolchanges_on_layer || first_layer) && ! combined_away) {
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();
        m_current_height += m_layer_info->height;
    }

    return construct_tcr(writer, false, old_tool, true, false);
}

std::pair<double, double> WipeTower2::get_wipe_tower_cone_base(double width, double height, double depth, double angle_deg)
{
    double R = std::tan(Geometry::deg2rad(angle_deg/2.)) * height;
    double fake_width = 0.66 * width;
    double diag = std::hypot(fake_width / 2., depth / 2.);
    double support_scale = 1.;
    if (R > diag) {
        double w = fake_width;
        double sin = 0.5 * depth / diag;
        double tan = depth / w;
        double t = (R - diag) * sin;
        support_scale = (w / 2. + t / tan + t * tan) / (w / 2.);
    }
    return std::make_pair(R, support_scale);
}

Polygon WipeTower2::cone_base_polygon(double width, double depth, double height, double angle_deg)
{
    Polygon box({Point::new_scale(Vec2d(0., 0.)), Point::new_scale(Vec2d(width, 0.)),
                 Point::new_scale(Vec2d(width, depth)), Point::new_scale(Vec2d(0., depth))});
    if (angle_deg <= EPSILON || height <= EPSILON || width <= EPSILON || depth <= EPSILON)
        return box;
    const auto [R, x_scale] = get_wipe_tower_cone_base(width, height, depth, angle_deg);
    if (R <= EPSILON)
        return box;
    const Vec2d center(width / 2., depth / 2.);
    Polygon ellipse;
    for (double alpha = 0.; alpha < 2. * M_PI; alpha += M_PI / 20.)
        ellipse.points.push_back(Point::new_scale(center + R * Vec2d(std::cos(alpha) / x_scale, std::sin(alpha))));
    Polygons u = union_({box, ellipse});
    return u.empty() ? box : u.front();
}

std::vector<std::vector<float>> WipeTower2::extract_wipe_volumes(const ConfigBase& config)
{
    const std::vector<double> &raw_matrix      = config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    const auto                *nozzle_diameter = config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto                *colours         = config.option<ConfigOptionStrings>("filament_colour");
    const size_t               filament_nums   = colours == nullptr ? 0 : colours->values.size();
    size_t       nozzle_nums         = (nozzle_diameter == nullptr || nozzle_diameter->values.empty()) ? 1 : nozzle_diameter->values.size();
    unsigned int number_of_extruders = (unsigned int)(sqrt(raw_matrix.size() / nozzle_nums) + EPSILON);
    if (size_t(number_of_extruders) * number_of_extruders * nozzle_nums != raw_matrix.size() ||
        (filament_nums > 0 && filament_nums * filament_nums == raw_matrix.size())) {
        nozzle_nums         = 1;
        number_of_extruders = (unsigned int)(sqrt(raw_matrix.size()) + EPSILON);
    }

    const bool purge = config.option<ConfigOptionBool>("purge_in_prime_tower")->value
                    && config.option<ConfigOptionBool>("single_extruder_multi_material")->value;

    std::vector<std::vector<float>> wipe_volumes(number_of_extruders, std::vector<float>(number_of_extruders, 0.f));
    if (purge) {
        const auto *multiplier = config.option<ConfigOptionFloats>("flush_multiplier");
        for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
            const std::vector<double> block = get_flush_volumes_matrix(raw_matrix, nozzle_id, nozzle_nums);
            const double              scale = multiplier->get_at(nozzle_id);
            for (unsigned int i = 0; i<number_of_extruders; ++i)
                for (unsigned int j = 0; j<number_of_extruders; ++j)
                    wipe_volumes[i][j] = std::max<float>(wipe_volumes[i][j], float(block[size_t(i) * number_of_extruders + j]) * scale);
        }
    }

    const auto *minimal_purge = config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower");
    for (unsigned int i = 0; i<number_of_extruders; ++i)
        for (unsigned int j = 0; j<number_of_extruders; ++j)
            wipe_volumes[i][j] = std::max<float>(wipe_volumes[i][j], minimal_purge->get_at(j));

    return wipe_volumes;
}

float WipeTower2::estimate_semm_flush_volume(const ConfigBase& config, size_t filaments_cnt)
{
    const std::vector<std::vector<float>> wipe_volumes = extract_wipe_volumes(config);
    if (wipe_volumes.empty())
        return 0.f;
    float maximum = 0.f;
    for (const std::vector<float> &v : wipe_volumes)
        maximum += *std::max_element(v.begin(), v.end());
    maximum = maximum * filaments_cnt / wipe_volumes.size();

    maximum *= 0.6;
    return maximum;
}

static float get_wipe_depth(float volume, float layer_height, float perimeter_width, float extra_flow, float extra_spacing, float width)
{
    float length_to_extrude = (volume_to_length(volume, perimeter_width, layer_height)) / extra_flow;
    length_to_extrude = std::max(length_to_extrude,0.f);

	return (int(length_to_extrude / width) + 1) * perimeter_width * extra_spacing;
}

void WipeTower2::plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool,
                                unsigned int new_tool, float wipe_volume)
{
	assert(m_plan.empty() || m_plan.back().z <= z_par + WT_EPSILON);

	if (m_plan.empty() || m_plan.back().z + WT_EPSILON < z_par)
		m_plan.push_back(WipeTowerInfo(z_par, layer_height_par));

    if (m_first_layer_idx == size_t(-1) && (! m_sparse_layers_skipped || old_tool != new_tool || m_plan.size() == 1))
        m_first_layer_idx = m_plan.size() - 1;

    if (old_tool == new_tool)
        return;

    const bool first_layer_plan = (m_plan.size() - 1) == m_first_layer_idx;
    m_plan.back().tool_changes.push_back(set_toolchange(old_tool, new_tool, layer_height_par, wipe_volume, first_layer_plan));
}

WipeTower2::WipeTowerInfo::ToolChange WipeTower2::set_toolchange(size_t old_tool, size_t new_tool, float layer_height, float wipe_volume, bool first_layer_plan)
{
    float width = m_wipe_tower_width - 3*m_perimeter_width;
	float length_to_extrude = volume_to_length((m_semm ? 0.25f : m_filpar[old_tool].multitool_ramming_time) * std::accumulate(m_filpar[old_tool].ramming_speed.begin(), m_filpar[old_tool].ramming_speed.end(), 0.f),
										m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator,
										layer_height);
    const bool do_ramming = tool_ramming_enabled(old_tool);
    const bool boundary_wipe_start = boundary_wipe_start_enabled(old_tool);
    float ramming_depth = do_ramming ? ((int(length_to_extrude / width) + 1) * (m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator * m_filpar[old_tool].ramming_step_multiplicator) * m_extra_spacing_ramming) : 0;
    float first_wipe_line = (do_ramming && !boundary_wipe_start) ? - (width*((length_to_extrude / width)-int(length_to_extrude / width)) - width) : 0.f;

    float first_wipe_volume = length_to_volume(first_wipe_line, m_perimeter_width * m_extra_flow, layer_height);

    const float planning_spacing = first_layer_plan ? m_extra_flow : m_extra_spacing_wipe;

    float wiping_depth = get_wipe_depth(wipe_volume - first_wipe_volume, layer_height, m_perimeter_width, m_extra_flow, planning_spacing, width);

	return WipeTowerInfo::ToolChange(old_tool, new_tool, ramming_depth + wiping_depth, ramming_depth, first_wipe_line, wipe_volume);
}



void WipeTower2::plan_tower()
{
	m_wipe_tower_depth = 0.f;
	for (auto& layer : m_plan)
		layer.depth = 0.f;
    m_wipe_tower_height = m_plan.empty() ? 0.f : m_plan.back().z;
    m_current_height = 0.f;
	
    for (int layer_index = int(m_plan.size()) - 1; layer_index >= 0; --layer_index)
	{
		float this_layer_depth = std::max(m_plan[layer_index].depth, m_plan[layer_index].toolchanges_depth());
		m_plan[layer_index].depth = this_layer_depth;
		
		if (this_layer_depth > m_wipe_tower_depth - m_perimeter_width)
			m_wipe_tower_depth = this_layer_depth + m_perimeter_width;

		for (int i = layer_index - 1; i >= 0 ; i--)
		{
			if (m_plan[i].depth - this_layer_depth < 2*m_perimeter_width )
				m_plan[i].depth = this_layer_depth;
		}
	}
}

void WipeTower2::save_on_last_wipe()
{
    for (m_layer_info=m_plan.begin();m_layer_info<m_plan.end();++m_layer_info) {
        set_layer(m_layer_info->z, m_layer_info->height, 0, m_layer_info->z == m_plan.front().z, m_layer_info->z == m_plan.back().z);
        if (m_layer_info->tool_changes.size()==0)
            continue;

        int idx = first_toolchange_to_nonsoluble_nonsupport(m_layer_info->tool_changes);

        if (idx == -1) {
            finish_layer().total_extrusion_length_in_plane();
        }

        const float width = m_wipe_tower_width - 3*m_perimeter_width;
        auto recompute_toolchange = [this, width](WipeTowerInfo::ToolChange& toolchange, float volume_to_save) {
            float volume_left_to_wipe = std::max(m_filpar[toolchange.new_tool].filament_minimal_purge_on_wipe_tower, toolchange.wipe_volume_total - volume_to_save);
            float volume_we_need_depth_for = std::max(0.f, volume_left_to_wipe - length_to_volume(toolchange.first_wipe_line, m_perimeter_width*m_extra_flow, m_layer_info->height));

            const bool first_layer_plan = size_t(m_layer_info - m_plan.begin()) == m_first_layer_idx;
            const float planning_spacing = first_layer_plan ? m_extra_flow : m_extra_spacing_wipe;

            float depth_to_wipe = get_wipe_depth(volume_we_need_depth_for, m_layer_info->height, m_perimeter_width, m_extra_flow, planning_spacing, width);

            toolchange.required_depth = toolchange.ramming_depth + depth_to_wipe;
            toolchange.wipe_volume = volume_left_to_wipe;
        };

        for (int i=0; i<int(m_layer_info->tool_changes.size()); ++i) {
            auto& toolchange = m_layer_info->tool_changes[i];
            tool_change(toolchange.new_tool);

            if (i == idx) {
                recompute_toolchange(toolchange, length_to_volume(finish_layer().total_extrusion_length_in_plane(), m_perimeter_width, m_layer_info->height));
            } else if (toolchange.wipe_volume < m_filpar[toolchange.new_tool].filament_minimal_purge_on_wipe_tower) {
                recompute_toolchange(toolchange, 0.f);
            }
        }
    }
}


int WipeTower2::first_toolchange_to_nonsoluble_nonsupport(
        const std::vector<WipeTowerInfo::ToolChange>& tool_changes) const
{
    if (tool_changes.empty())
        return -1;

    if (m_wipe_tower_filament > 0) {
        for (size_t idx = 0; idx < tool_changes.size(); ++idx) {
            if (!m_filpar[tool_changes[idx].new_tool].is_soluble)
                return idx;
        }
        return -1;
    }

    auto is_wall_filament = [this](size_t tool) {
        return !m_filpar[tool].is_soluble && !m_filpar[tool].is_support;
    };
    for (size_t idx = 0; idx < tool_changes.size(); ++idx)
        if (is_wall_filament(tool_changes[idx].new_tool))
            return idx;
    if (is_wall_filament(tool_changes.front().old_tool))
        return -1;
    return 0;
}

static WipeTower::ToolChangeResult merge_tcr(WipeTower::ToolChangeResult& first,
                                             WipeTower::ToolChangeResult& second)
{
    assert(first.new_tool == second.initial_tool);
    WipeTower::ToolChangeResult out = first;
    out.is_contact = first.is_contact || second.is_contact;
    if (first.end_pos != second.start_pos)
        out.gcode += "G1 X" + Slic3r::float_to_string_decimal_point(second.start_pos.x(), 3)
                     + " Y" + Slic3r::float_to_string_decimal_point(second.start_pos.y(), 3)
                     + " F7200\n";
    out.gcode += second.gcode;
    out.extrusions.insert(out.extrusions.end(), second.extrusions.begin(), second.extrusions.end());
    out.end_pos = second.end_pos;
    out.wipe_path = second.wipe_path;
    out.initial_tool = first.initial_tool;
    out.new_tool = second.new_tool;
    return out;
}


void WipeTower2::compute_wall_skip_points()
{
    m_wall_skip_points.assign(m_plan.size(), std::vector<Vec2f>());
    for (size_t layer_id = 0; layer_id < m_plan.size(); ++layer_id) {
        float depth_traversed = 0.f;
        for (const auto& toolchange : m_plan[layer_id].tool_changes) {
            m_wall_skip_points[layer_id].emplace_back(
                toolchange_entry_pos(depth_traversed, toolchange.ramming_depth, layer_id == m_first_layer_idx));
            depth_traversed += toolchange.required_depth;
        }
    }
}

void WipeTower2::generate(std::vector<std::vector<WipeTower::ToolChangeResult>> &result)
{
	if (m_plan.empty())
        return;

    if (m_sparse_layers_combined)
        combine_sparse_wipe_tower_plan(m_plan, m_filpar, m_first_layer_idx, m_current_tool);

	plan_tower();
#if 1
    for (int i=0;i<5;++i) {
        save_on_last_wipe();
        plan_tower();
    }
#endif

    if (m_wall_type == (int)wtwRib) {
        float max_depth = 0.f;
        for (const auto& current_plan : m_plan)
            max_depth = std::max(max_depth, current_plan.depth);
        if (max_depth > EPSILON) {
            m_wipe_tower_width = align_ceil(std::sqrt(max_depth * m_wipe_tower_width), m_perimeter_width);
            for (size_t idx = 0; idx < m_plan.size(); ++idx)
                for (auto& toolchange : m_plan[idx].tool_changes)
                    toolchange = set_toolchange(toolchange.old_tool, toolchange.new_tool,
                                                m_plan[idx].height, toolchange.wipe_volume,
                                                idx == m_first_layer_idx);
            plan_tower();
        }

        const float min_depth = WipeTower::get_limit_depth_by_height(m_wipe_tower_height);
        if (m_wipe_tower_depth + EPSILON < min_depth)
            m_rib_length = std::max(m_rib_length, min_depth * (float)std::sqrt(2.f));
    }

    const float diagonal = std::sqrt(m_wipe_tower_depth * m_wipe_tower_depth + m_wipe_tower_width * m_wipe_tower_width);
    m_rib_length = std::max(m_rib_length, diagonal);
    m_rib_length += m_extra_rib_length;
    m_rib_length = std::max(diagonal, m_rib_length);
    m_rib_width  = std::min(m_rib_width, std::min(m_wipe_tower_depth, m_wipe_tower_width) /
                                             2.f);

    if (m_use_gap_wall)
        compute_wall_skip_points();

    m_layer_info = m_plan.begin();
    m_current_height = 0.f;

    for (const auto& layer : m_plan) {
        if (!layer.tool_changes.empty()) {
            m_current_tool = layer.tool_changes.front().old_tool;
            break;
        }
    }

    m_used_filament_length.assign(m_used_filament_length.size(), 0.f);
    assert(m_used_filament_length_until_layer.empty());
    m_used_filament_length_until_layer.emplace_back(0.f, m_used_filament_length);

    m_old_temperature = -1;

	for (const WipeTower2::WipeTowerInfo& layer : m_plan)
	{
        std::vector<WipeTower::ToolChangeResult> layer_result;
        set_layer(layer.z, layer.height, 0, false , layer.z == m_plan.back().z);
        m_internal_rotation += 180.f;

        if (m_layer_info->depth < m_wipe_tower_depth - m_perimeter_width)
			m_y_shift = (m_wipe_tower_depth-m_layer_info->depth-m_perimeter_width)/2.f;

        int idx = first_toolchange_to_nonsoluble_nonsupport(layer.tool_changes);
        WipeTower::ToolChangeResult finish_layer_tcr;

        if (idx == -1) {
            finish_layer_tcr = finish_layer();
        }

        for (int i=0; i<int(layer.tool_changes.size()); ++i) {
            layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool));
            if (i == idx)
                finish_layer_tcr = finish_layer();
        }

        if (layer_result.empty()) {
            layer_result.emplace_back(std::move(finish_layer_tcr));
        }
        else {
            if (idx == -1) {
                layer_result[0] = merge_tcr(finish_layer_tcr, layer_result[0]);
                layer_result[0].force_travel = true;
            }
            else
                layer_result[idx] = merge_tcr(layer_result[idx], finish_layer_tcr);
        }

        if (layer.combined_away)
            for (WipeTower::ToolChangeResult &tcr : layer_result)
                tcr.combined_away = true;

		result.emplace_back(std::move(layer_result));

        if (m_used_filament_length_until_layer.empty() || m_used_filament_length_until_layer.back().first != layer.z)
            m_used_filament_length_until_layer.emplace_back();
        m_used_filament_length_until_layer.back() = std::make_pair(layer.z, m_used_filament_length);
	}
}



std::vector<std::pair<float, float>> WipeTower2::get_z_and_depth_pairs() const
{
    std::vector<std::pair<float, float>> out = {{0.f, m_wipe_tower_depth}};
    for (const WipeTowerInfo& wti : m_plan) {
        assert(wti.depth < wti.depth + WT_EPSILON);
        if (wti.depth < out.back().second - WT_EPSILON)
            out.emplace_back(wti.z, wti.depth);
    }
    if (out.back().first < m_wipe_tower_height - WT_EPSILON)
        out.emplace_back(m_wipe_tower_height, 0.f);
    return out;
}


Polygon WipeTower2::generate_rib_polygon(const WipeTower::box_coordinates& wt_box)
{

    auto get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) -> float {
        return std::abs(max_height - cur_height) / max_height * max_len;
    };
    coord_t diagonal_width = scaled(m_rib_width) / 2;
    float   a = this->m_wipe_tower_width, b = this->m_wipe_tower_depth;
    Line    line_1(Point::new_scale(Vec2f{0, 0}), Point::new_scale(Vec2f{a, b}));
    Line    line_2(Point::new_scale(Vec2f{a, 0}), Point::new_scale(Vec2f{0, b}));
    float   diagonal_extra_length = std::max(0.f, m_rib_length - (float) unscaled(line_1.length())) / 2.f;
    diagonal_extra_length         = scaled(get_current_layer_rib_len(this->m_z_pos, this->m_wipe_tower_height, diagonal_extra_length));
    Point y_shift{0, scaled(this->m_y_shift)};

    line_1.extend(double(diagonal_extra_length));
    line_2.extend(double(diagonal_extra_length));
    line_1.translate(-y_shift);
    line_2.translate(-y_shift);

    Polygon poly_1 = generate_rectange(line_1, diagonal_width);
    Polygon poly_2 = generate_rectange(line_2, diagonal_width);
    Polygon poly;
    poly.points.push_back(Point::new_scale(wt_box.ld));
    poly.points.push_back(Point::new_scale(wt_box.rd));
    poly.points.push_back(Point::new_scale(wt_box.ru));
    poly.points.push_back(Point::new_scale(wt_box.lu));

    Polygons p_1_2 = union_({poly_1, poly_2, poly});
    return p_1_2.front();
};

Polygon WipeTower2::generate_support_rib_wall(WipeTowerWriter2&                 writer,
                                              const WipeTower::box_coordinates& wt_box,
                                             double                 feedrate,
                                             bool                   first_layer,
                                             bool                   rib_wall,
                                             bool                   extrude_perimeter)
{

    float     retract_length = m_filpar[m_current_tool].retract_length;
    float     retract_speed  = m_filpar[m_current_tool].retract_speed * 60;
    Polygon   wall_polygon   = rib_wall ? generate_rib_polygon(wt_box) : generate_rectange_polygon(wt_box.ld, wt_box.ru);
    Polylines result_wall;
    Polygon   insert_skip_polygon;
    if (m_used_fillet) {
        if (!rib_wall && m_y_shift > EPSILON)
        {
        } else {
            wall_polygon           = rib_wall ? rounding_polygon(wall_polygon) : wall_polygon;
            Polygon wt_box_polygon = generate_rectange_polygon(wt_box.ld, wt_box.ru);
            wall_polygon           = union_({wall_polygon, wt_box_polygon}).front();
        }
    }
    if (!extrude_perimeter)
        return wall_polygon;

    if (m_use_gap_wall) {
        static const std::vector<Vec2f> no_skip_points;
        const size_t layer_id = size_t(m_layer_info - m_plan.begin());
        const std::vector<Vec2f>& layer_skip_points =
            layer_id < m_wall_skip_points.size() ? m_wall_skip_points[layer_id] : no_skip_points;
        result_wall = construct_gap_for_skip_points(wall_polygon, layer_skip_points, m_wipe_tower_width, 2.5 * m_perimeter_width,
                                                   insert_skip_polygon);
    } else {
        result_wall.push_back(to_polyline(wall_polygon));
        insert_skip_polygon = wall_polygon;
    }
    writer.generate_path(result_wall, feedrate, retract_length, retract_speed, m_used_fillet);
    if (rib_wall && is_first_layer()) {
        BoundingBox bbox = get_extents(insert_skip_polygon);
        m_rib_offset     = Vec2f(-unscaled<float>(bbox.min.x()), -unscaled<float>(bbox.min.y()));
    }

    return insert_skip_polygon;
}


Polygon WipeTower2::generate_support_cone_wall(
    WipeTowerWriter2& writer, const WipeTower::box_coordinates& wt_box, double feedrate, bool infill_cone, float spacing){

    const auto [R, support_scale] = get_wipe_tower_cone_base(m_wipe_tower_width, m_wipe_tower_height, m_wipe_tower_depth,
                                                             m_wipe_tower_cone_angle);

    double z = m_sparse_layers_skipped ?
                   (m_current_height + m_layer_info->height) :
                   m_layer_info->z;

    double r      = std::tan(Geometry::deg2rad(m_wipe_tower_cone_angle / 2.f)) * (m_wipe_tower_height - z);
    Vec2f  center = (wt_box.lu + wt_box.rd) / 2.;
    double w      = wt_box.lu.y() - wt_box.ld.y();
    enum Type { Arc, Corner, ArcStart, ArcEnd };

    std::vector<std::pair<Vec2f, Type>> pts = {{wt_box.ru, Corner}};
    if (double alpha_start = std::asin((0.5 * w) / r); !std::isnan(alpha_start) && r > 0.5 * w + 0.01) {
        for (double alpha = alpha_start; alpha < M_PI - alpha_start + 0.001; alpha += (M_PI - 2 * alpha_start) / 40.)
            pts.emplace_back(Vec2f(center.x() + r * std::cos(alpha) / support_scale, center.y() + r * std::sin(alpha)),
                             alpha == alpha_start ? ArcStart : Arc);
        pts.back().second = ArcEnd;
    }
    pts.emplace_back(wt_box.lu, Corner);
    pts.emplace_back(wt_box.ld, Corner);
    for (int i = int(pts.size()) - 3; i > 0; --i)
        pts.emplace_back(Vec2f(pts[i].first.x(), 2 * center.y() - pts[i].first.y()), i == int(pts.size()) - 3 ? ArcStart :
                                                                                     i == 1                   ? ArcEnd :
                                                                                                                Arc);
    pts.emplace_back(wt_box.rd, Corner);

    Polygon poly;
    for (const auto& [pt, tag] : pts)
        poly.points.push_back(Point::new_scale(pt));

    Polylines polylines;
    if (infill_cone && m_wipe_tower_width > 2 * spacing && m_wipe_tower_depth > 2 * spacing) {
        ExPolygons infill_areas;
        ExPolygon  wt_contour(poly);
        Polygon    wt_rectangle(
            Points{Point::new_scale(wt_box.ld), Point::new_scale(wt_box.rd), Point::new_scale(wt_box.ru), Point::new_scale(wt_box.lu)});
        wt_rectangle = offset(wt_rectangle, scale_(-spacing / 2.)).front();
        wt_contour   = offset_ex(wt_contour, scale_(-spacing / 2.)).front();
        infill_areas = diff_ex(wt_contour, wt_rectangle);
        if (infill_areas.size() == 2) {
            ExPolygon& bottom_expoly = infill_areas.front().contour.points.front().y() < infill_areas.back().contour.points.front().y() ?
                                           infill_areas[0] :
                                           infill_areas[1];
            std::unique_ptr<Fill> filler(Fill::new_from_type(ipMonotonicLine));
            filler->angle   = Geometry::deg2rad(45.f);
            filler->spacing = spacing;
            FillParams params;
            params.density = 1.f;
            Surface surface(stBottom, bottom_expoly);
            filler->bounding_box = get_extents(bottom_expoly);
            polylines            = filler->fill_surface(&surface, params);
            if (!polylines.empty()) {
                if (polylines.front().points.front().x() > polylines.back().points.back().x()) {
                    std::reverse(polylines.begin(), polylines.end());
                    for (Polyline& p : polylines)
                        p.reverse();
                }
            }
        }
    }

    int    start_i  = 0;
    double min_dist = std::numeric_limits<double>::max();
    for (int i = 0; i < int(pts.size()); ++i) {
        if (pts[i].second == Corner) {
            double dist = (pts[i].first - Vec2f(writer.x(), writer.y())).squaredNorm();
            if (dist < min_dist) {
                min_dist = dist;
                start_i  = i;
            }
        }
    }
    writer.travel(pts[start_i].first);

    int i = start_i + 1 == int(pts.size()) ? 0 : start_i + 1;
    while (i != start_i) {
        writer.extrude(pts[i].first, feedrate);
        if (pts[i].second == ArcEnd) {
            if (!polylines.empty()) {
                bool mirror = ((pts[i].first.y() - center.y()) * (unscale(polylines.front().points.front()).y() - center.y())) < 0.;
                for (const Polyline& line : polylines) {
                    writer.travel(center - (mirror ? 1.f : -1.f) * (unscale(line.points.front()).cast<float>() - center));
                    for (size_t i = 0; i < line.points.size(); ++i)
                        writer.extrude(center - (mirror ? 1.f : -1.f) * (unscale(line.points[i]).cast<float>() - center));
                }
                writer.travel(pts[i].first);
            }
        }
        if (++i == int(pts.size()))
            i = 0;
    }
    writer.extrude(pts[start_i].first, feedrate);
    return poly;
}
}
