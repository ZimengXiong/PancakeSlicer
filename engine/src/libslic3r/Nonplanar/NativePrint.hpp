#pragma once
#include "LayerField.hpp"
#include "libslic3r/ExPolygon.hpp"

#include <admesh/stl.h>

#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
class PrintObject;
enum class EnforcerBlockerType : int8_t;
}

namespace Slic3r::Nonplanar {

struct ConformalResult;

bool native_requested();

double native_axis_key(const char *name, double rotary_default, bool rotary);

bool generate_planar_native_support(PrintObject &object);

std::shared_ptr<const LayerField> native_field(PrintObject &object);

void build_native_patch(PrintObject &object, const std::shared_ptr<const LayerField> &field, ConformalResult &result);

std::vector<int> part_regions(const PrintObject &object);

class NativeSliceOverride
{
public:
    NativeSliceOverride(std::string name, std::vector<ExPolygons> slices, indexed_triangle_set enforcers, indexed_triangle_set blockers);
    NativeSliceOverride(std::string name, std::vector<std::vector<ExPolygons>> by_volume, indexed_triangle_set enforcers,
                        indexed_triangle_set blockers);
    ~NativeSliceOverride();
    NativeSliceOverride(const NativeSliceOverride &) = delete;
    NativeSliceOverride &operator=(const NativeSliceOverride &) = delete;
    static const std::vector<std::vector<ExPolygons>> *find(const std::string &name);
    static const indexed_triangle_set *support_facets(const std::string &name, EnforcerBlockerType type);

private:
    std::string m_name;
};

}
