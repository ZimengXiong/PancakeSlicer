#include "FieldTerm.hpp"
#include "libslic3r/Nonplanar/Research.hpp"

#include "FieldObjective.hpp"
#include "ClearanceTerms.hpp"
#include "StrengthTerm.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <utility>

namespace Slic3r::Nonplanar {

FieldTermField::Sample FieldTermField::sample(const Vec3d &p) const
{
    Sample out;
    size_t cell[3];
    Vec3d t, derivative;
    for (int a = 0; a < 3; ++a) {
        const double u = (p[a] - geometry.origin[a]) / geometry.spacing[a];
        const double hi = double(geometry.dims[size_t(a)] - 1);
        const double clamped = std::clamp(u, 0., hi);
        cell[a] = std::min(size_t(clamped), geometry.dims[size_t(a)] - 2);
        t[a] = clamped - double(cell[a]);
        derivative[a] = u > 0. && u < hi ? 1. / geometry.spacing[a] : 0.;
    }
    for (int q = 0; q < 8; ++q) {
        const int bit[3] = { q & 1, (q >> 1) & 1, (q >> 2) & 1 };
        const Vec3d w(bit[0] ? t.x() : 1. - t.x(), bit[1] ? t.y() : 1. - t.y(), bit[2] ? t.z() : 1. - t.z());
        const size_t id = geometry.index(cell[0] + bit[0], cell[1] + bit[1], cell[2] + bit[2]);
        const double weight = w.x() * w.y() * w.z();
        out.nodes[q] = id;
        out.weights[q] = weight;
        out.value += weight * double(phi[id]);
        out.occupancy += weight * double(geometry.occ[id] != 0);
        for (int a = 0; a < 3; ++a) {
            const double dw = (bit[a] ? 1. : -1.) * derivative[a] * w[(a + 1) % 3] * w[(a + 2) % 3];
            out.gradient[a] += dw * double(phi[id]);
            out.occupancy_gradient[a] += dw * double(geometry.occ[id] != 0);
        }
    }
    return out;
}

namespace {

std::mutex                              g_mutex;
std::map<std::string, FieldTermFactory> g_factories;
std::once_flag                          g_builtins;
void register_builtins()
{
    register_field_term("strength", make_strength_term);
    register_field_term("head", make_head_term);
    register_field_term("axis", make_axis_term);
    register_field_term("axist", make_axist_term);
}

}

void register_field_term(const std::string &name, FieldTermFactory factory)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_factories[name] = std::move(factory);
}

const std::vector<ActiveFieldTerm> &active_field_terms(const ObjectiveGeometry &g)
{
    static const std::vector<ActiveFieldTerm> none;
    const char                               *spec = research_switch("NP_FIELD_TERMS");
    if (spec == nullptr || *spec == 0)
        return none;
    using Key = std::tuple<const void *, size_t, double, double, double>;
    static std::map<Key, std::vector<ActiveFieldTerm>> cache;
    const Key key { &g, g.nodes(), g.origin.x(), g.origin.y(), g.spacing.x() };
    std::call_once(g_builtins, register_builtins);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
    }
    std::vector<ActiveFieldTerm> terms;
    std::stringstream            ss(spec);
    for (std::string item; std::getline(ss, item, ',');) {
        const size_t      colon = item.find(':');
        const std::string name  = item.substr(0, colon);
        const double      w     = colon == std::string::npos ? 1. : std::atof(item.c_str() + colon + 1);
        FieldTermFactory  factory;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (auto it = g_factories.find(name); it != g_factories.end())
                factory = it->second;
        }
        if (!factory) {
            BOOST_LOG_TRIVIAL(warning) << "Field terms: no term named " << name;
            continue;
        }
        terms.push_back({ name, w, factory(g) });
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return cache.emplace(key, std::move(terms)).first->second;
}

}
