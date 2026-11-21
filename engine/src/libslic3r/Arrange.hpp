#ifndef ARRANGE_HPP
#define ARRANGE_HPP

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "Print.hpp"
#include <cmath>
#include "libslic3r.h"
#include <vector>
#include <string>
#include <functional>
#include <iostream>
#include <ostream>

#define BED_SHRINK_SEQ_PRINT 5

namespace Slic3r {

class BoundingBox;

namespace arrangement {

class CircleBed {
    Point center_;
    double radius_;
public:

    inline CircleBed(): center_(0, 0), radius_(std::nan("")) {}
    explicit inline CircleBed(const Point& c, double r): center_(c), radius_(r) {}

    inline double radius() const { return radius_; }
    inline const Point& center() const { return center_; }
};

struct InfiniteBed {
    Point center;
    explicit InfiniteBed(const Point &p = {0, 0}): center{p} {}
};

static const constexpr int UNARRANGED = -1;

struct ArrangePolygon {
    ExPolygon poly;
    Vec2crd   translation{0, 0};
    double    rotation{0.0};
    coord_t   inflation = 0;
    int       bed_idx{UNARRANGED};
    int       priority{0};
    int       locked_plate{ -1 };
    bool      is_virt_object{ false };
    bool      is_extrusion_cali_object{ false };
    bool      is_wipe_tower{ false };
    bool      has_tree_support{false};
    int       row{0};
    int       col{0};
    std::vector<int> extrude_ids{};
    int filament_temp_type{ -1 };
    int       bed_temp{0};
    int       print_temp{0};
    int       first_bed_temp{ 0 };
    int       first_print_temp{ 0 };
    int       vitrify_temp{ 0 };
    int       itemid{ 0 };
    int       is_applied{ 0 };
    double    height{ 0 };
    double    brim_width{ 0 };
    double    nonplanar_relief{ 0 };
    coord_t   nonplanar_inflation{ 0 };
    bool      nonplanar_inflation_capped{ false };
    std::string name;

    std::vector<double> allowed_rotations = {0.};

    std::function<void(const ArrangePolygon&)> setter = nullptr;

    void apply() {
        if (setter && !is_applied) {
            setter(*this);
            is_applied = 1;
        }
    }

    bool is_arranged() const { return bed_idx != UNARRANGED; }

    inline ExPolygon transformed_poly() const
    {
        ExPolygon ret = poly;
        ret.rotate(rotation);
        ret.translate(translation.x(), translation.y());

        return ret;
    }
};

using ArrangePolygons = std::vector<ArrangePolygon>;

struct ArrangeParams {

    coord_t min_obj_distance = 0;

    float accuracy = 1.f;

    bool parallel = true;

    bool allow_rotations = false;

    bool do_final_align = true;

    bool  allow_multi_materials_on_same_plate = true;
    bool  avoid_extrusion_cali_region         = true;
    bool  is_seq_print                        = false;
    bool  align_to_y_axis                     = false;
    float bed_shrink_x = 1;
    float bed_shrink_y = 1;
    float brim_skirt_distance = 0;
    float clearance_height_to_rod = 0;
    float clearance_height_to_lid = 0;
    float clearance_radius = 0;
    float object_skirt_offset = 0;
    float nozzle_height = 0;
    float printable_height = 256.0;
    Vec2d align_center{ 0.5,0.5 };
    bool  is_belt         = false;
    int   belt_axis       = 1;
    bool  belt_reversed   = false;
    float belt_tilt_slope = 1.f;

    ArrangePolygons excluded_regions;
    ArrangePolygons nonprefered_regions;
    std::function<void(unsigned, std::string)> progressind = [](unsigned st, std::string str = "") {
        std::cout << "st=" << st << ", " << str << std::endl;
    };

    std::function<void(const ArrangePolygon &)> on_packed;

    std::function<bool(void)>     stopcondition;

    ArrangeParams() = default;
    explicit ArrangeParams(coord_t md) : min_obj_distance(md) {}
    std::string to_json() const{
        std::string ret = "{";
        ret += "\"min_obj_distance\":" + std::to_string(min_obj_distance) + ",";
        ret += "\"accuracy\":" + std::to_string(accuracy) + ",";
        ret += "\"parallel\":" + std::to_string(parallel) + ",";
        ret += "\"allow_rotations\":" + std::to_string(allow_rotations) + ",";
        ret += "\"do_final_align\":" + std::to_string(do_final_align) + ",";
        ret += "\"allow_multi_materials_on_same_plate\":" + std::to_string(allow_multi_materials_on_same_plate) + ",";
        ret += "\"avoid_extrusion_cali_region\":" + std::to_string(avoid_extrusion_cali_region) + ",";
        ret += "\"is_seq_print\":" + std::to_string(is_seq_print) + ",";
        ret += "\"bed_shrink_x\":" + std::to_string(bed_shrink_x) + ",";
        ret += "\"bed_shrink_y\":" + std::to_string(bed_shrink_y) + ",";
        ret += "\"brim_skirt_distance\":" + std::to_string(brim_skirt_distance) + ",";
        ret += "\"clearance_height_to_rod\":" + std::to_string(clearance_height_to_rod) + ",";
        ret += "\"clearance_height_to_lid\":" + std::to_string(clearance_height_to_lid) + ",";
        ret += "\"clearance_radius\":" + std::to_string(clearance_radius) + ",";
        ret += "\"printable_height\":" + std::to_string(printable_height) + ",";
        return ret;
    }

};

void update_arrange_params(ArrangeParams& params, const DynamicPrintConfig* print_cfg, const ArrangePolygons& selected);

void update_selected_items_inflation(ArrangePolygons& selected, const DynamicPrintConfig* print_cfg, ArrangeParams& params);

void update_unselected_items_inflation(ArrangePolygons& unselected, const DynamicPrintConfig* print_cfg, const ArrangeParams& params);

void update_selected_items_axis_align(ArrangePolygons& selected, const DynamicPrintConfig* print_cfg, const ArrangeParams& params);

Points get_shrink_bedpts(const DynamicPrintConfig* print_cfg, const ArrangeParams& params);

template<class TBed> void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const TBed &bed, const ArrangeParams &params = {});

template<> void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const Points &bed, const ArrangeParams &params);

extern template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const BoundingBox &bed, const ArrangeParams &params);
extern template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const CircleBed &bed, const ArrangeParams &params);
extern template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const Polygon &bed, const ArrangeParams &params);
extern template void arrange(ArrangePolygons &items, const ArrangePolygons &excludes, const InfiniteBed &bed, const ArrangeParams &params);

inline void arrange(ArrangePolygons &items, const Points &bed, const ArrangeParams &params = {}) { arrange(items, {}, bed, params); }
inline void arrange(ArrangePolygons &items, const BoundingBox &bed, const ArrangeParams &params = {}) { arrange(items, {}, bed, params); }
inline void arrange(ArrangePolygons &items, const CircleBed &bed, const ArrangeParams &params = {}) { arrange(items, {}, bed, params); }
inline void arrange(ArrangePolygons &items, const Polygon &bed, const ArrangeParams &params = {}) { arrange(items, {}, bed, params); }
inline void arrange(ArrangePolygons &items, const InfiniteBed &bed, const ArrangeParams &params = {}) { arrange(items, {}, bed, params); }

}}

#endif
