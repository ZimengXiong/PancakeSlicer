// Curved tops port of fich-l / OrcaSlicer-Nonplanar v0.1 (AGPL-3.0).
#pragma once
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <admesh/stl.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "CurvedTopsFacet.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r {

typedef struct {
  float x;
  float y;
  float z;
} mesh_vertex;

typedef struct {
  mesh_vertex    max;
  mesh_vertex    min;
} mesh_stats;

class PrintObject;
class Print;
bool   curved_cone_allowance(NonplanarMode mode);
double curved_cone_tolerance(NonplanarMode mode);
struct CurvedTopsStats {
    size_t kept = 0, candidates = 0;
    double footprint_mm2 = 0., extrusion_mm = 0., upward_area_mm2 = 0., area_fraction = 0.;
    double max_z = 0.;
    std::string planar_fallback_reason;
    std::string planar_limit;
    double max_angle_deg = 0.;
    double reachable_slope_deg = 0.;
    std::string access_by;
};
CurvedTopsStats curved_tops_stats(const PrintObject &object);
int curved_top_layers(const PrintObject &object);
struct CurvedSurfaceRejections { size_t height = 0, area = 0, collision = 0; bool head_unknown = false; };
void note_curved_surface_rejections(const PrintObject &object, const CurvedSurfaceRejections &rejections);
struct CurvedSurfaceDrop { int volume = -1; std::string reason; std::vector<int> facets; };

class NonplanarSurface;

typedef std::vector<NonplanarSurface> NonplanarSurfaces;

class NonplanarSurface
{
    public:
    std::map<int, NonplanarFacet> mesh;
    mesh_stats stats;
    int stable_id = -1;
    int volume = -1;
    NonplanarSurface() {};
    ~NonplanarSurface() {};
    NonplanarSurface(std::map<int, NonplanarFacet> &_mesh);
    bool operator==(const NonplanarSurface& other) const;
    void calculate_stats();
    void translate(float x, float y, float z);
    void scale(float factor);
    void scale(float versor[3]);
    void rotate_z(float angle);
    void debug_output();
    NonplanarSurfaces group_surfaces();
    void mark_neighbor_surfaces(int id);
    bool check_max_printing_height(float height);
    void check_printable_surfaces(float max_angle);
    bool check_surface_area(float min_area);
    ExPolygons horizontal_projection() const;

};

constexpr int       AutoTopCandidates = 2;
std::vector<double> auto_top_angles(const Points &points, std::optional<double> level = std::nullopt);
Points              surface_outline_points(const NonplanarSurface &surface);
std::vector<double> auto_top_angles(const NonplanarSurface &surface, std::optional<double> level, double spacing);
std::optional<double> mesh_auto_top_direction(const indexed_triangle_set &mesh);

std::string nonplanar_location(double z, size_t layer_id);
double      nonplanar_suggested_angle(double angle);
[[noreturn]] void throw_nonplanar_too_steep(double z, size_t layer_id, double angle, size_t object_id, const std::string &detail);
[[noreturn]] void throw_nonplanar_internal(const std::string &where, const std::string &detail, size_t object_id);
};
