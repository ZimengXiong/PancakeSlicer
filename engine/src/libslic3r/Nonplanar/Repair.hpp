#pragma once
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <cstddef>
#include <cstdint>
#include "HeightField.hpp"
#include "libslic3r/Point.hpp"
#include <vector>


namespace Slic3r::Nonplanar {

struct CurvedTopsScaffold;

struct RepairHints
{
    struct SurfaceCap { Vec2d p; double radius = 0.; double z = 0.; };
    struct Disc { Vec2d p; double radius = 0.; double z = std::numeric_limits<double>::quiet_NaN(); };
    struct OrderBand { Vec2d p; double height = 0.; };

    std::shared_ptr<CurvedTopsScaffold> curved_scaffold;
    std::map<int, double> curved_top_areas;

    std::vector<SurfaceCap> surface_caps;
    std::vector<Disc>       ironing_drops;
    std::vector<OrderBand>  order_bands;
    std::vector<Disc>       uniform_beads;
    std::vector<Disc>       upright;
    std::vector<Disc>       planar;
    std::vector<Disc>       ordered_roads;
    std::map<int, int>      ordered_road_passes;
    std::set<int>           ordered_surfaces;
    std::map<int, int>      top_angle_rungs;
    std::map<int, std::vector<size_t>> top_angle_hits;
    std::set<int>           top_angle_settled;
    bool                    refill_tops = false;
    double                  travel_lift = 0.;
    std::vector<size_t>     travel_layers;
    enum class CurvedFinish : uint8_t { Blended, NativeFitted, NativeMesh };
    CurvedFinish curved_finish = CurvedFinish::Blended;
};

HeightField surface_cap_field(const RepairHints &hints, const HeightField &like);
std::vector<uint8_t> planar_disc_mask(const RepairHints &hints, const HeightField &like);

}
