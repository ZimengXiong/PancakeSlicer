#include "StrengthTerm.hpp"
#include "libslic3r/Nonplanar/Research.hpp"

#include "FieldObjective.hpp"

#include <boost/log/trivial.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace Slic3r::Nonplanar {

StrengthTerm::StrengthTerm(const ObjectiveGeometry &g, const std::string &path)
{
    std::ifstream in(path);
    std::string   tag;
    Vec3d         file_min, file_size(Vec3d::Constant(std::numeric_limits<double>::quiet_NaN()));
    if (!(in >> tag >> file_min.x() >> file_min.y() >> file_min.z()) || tag != "mesh_min") {
        BOOST_LOG_TRIVIAL(warning) << "Strength term: no 'mesh_min' header in " << path;
        return;
    }
    if (const std::streampos at = in.tellg(); in >> tag && tag == "mesh_size")
        in >> file_size.x() >> file_size.y() >> file_size.z();
    else {
        in.clear();
        in.seekg(at);
    }
    if (std::isfinite(file_size.x()) && std::isfinite(g.mesh_max.x()) &&
        (g.mesh_max - g.mesh_min - file_size).cwiseAbs().maxCoeff() > 0.05) {
        BOOST_LOG_TRIVIAL(info) << "Strength term: the geometry's box " << (g.mesh_max - g.mesh_min).transpose() << " is not the stress mesh's "
                                << file_size.transpose() << ", no term";
        return;
    }
    const Vec3d shift = std::isfinite(g.mesh_min.x()) ? Vec3d(g.mesh_min - file_min) : Vec3d::Zero();
    std::unordered_map<size_t, size_t> slot;
    size_t                             points = 0;
    double                             w;
    Vec3d                              p, e;
    while (in >> p.x() >> p.y() >> p.z() >> w >> e.x() >> e.y() >> e.z()) {
        ++points;
        const Vec3d u = (p + shift - g.origin).cwiseQuotient(g.spacing);
        const long  i0 = long(std::floor(u.x())), j0 = long(std::floor(u.y())), k0 = long(std::floor(u.z()));
        const Vec3d f  = u - Vec3d(double(i0), double(j0), double(k0));
        const std::array<double, 6> ee { e.x() * e.x(), e.y() * e.y(), e.z() * e.z(), e.x() * e.y(), e.x() * e.z(), e.y() * e.z() };
        for (int c = 0; c < 8; ++c) {
            const long i = i0 + (c & 1), j = j0 + ((c >> 1) & 1), k = k0 + ((c >> 2) & 1);
            if (i < 0 || j < 0 || k < 0 || i >= long(g.dims[0]) || j >= long(g.dims[1]) || k >= long(g.dims[2]))
                continue;
            const size_t id = g.index(size_t(i), size_t(j), size_t(k));
            if (!g.occ[id])
                continue;
            const double t = w * ((c & 1) ? f.x() : 1. - f.x()) * ((c & 2) ? f.y() : 1. - f.y()) * ((c & 4) ? f.z() : 1. - f.z());
            if (t <= 0.)
                continue;
            auto [it, fresh] = slot.emplace(id, m_nodes.size());
            if (fresh) {
                m_nodes.push_back(id);
                m_M.push_back({ 0., 0., 0., 0., 0., 0. });
            }
            for (int q = 0; q < 6; ++q)
                m_M[it->second][q] += t * ee[q];
            m_W += t;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Strength term: " << points << " stress points onto " << m_nodes.size() << " nodes, weight " << m_W
                            << ", shift " << shift.transpose();
}

double StrengthTerm::cost(const ObjectiveGeometry &, const std::vector<Vec3d> &G, std::vector<Vec3d> *dG) const
{
    if (m_W <= 0.)
        return 0.;
    double sum = 0.;
    for (size_t s = 0; s < m_nodes.size(); ++s) {
        const Vec3d &gr = G[m_nodes[s]];
        const double n  = gr.norm();
        if (!(n > 1e-12))
            continue;
        const Vec3d                  d = gr / n;
        const std::array<double, 6> &M = m_M[s];
        const Vec3d Md(M[0] * d.x() + M[3] * d.y() + M[4] * d.z(), M[3] * d.x() + M[1] * d.y() + M[5] * d.z(),
                       M[4] * d.x() + M[5] * d.y() + M[2] * d.z());
        const double dMd = d.dot(Md);
        sum += dMd;
        if (dG)
            (*dG)[m_nodes[s]] += (2. / (m_W * n)) * (Md - dMd * d);
    }
    return sum / m_W;
}

std::shared_ptr<const FieldTerm> make_strength_term(const ObjectiveGeometry &g)
{
    const char *path = research_switch("NP_STRENGTH_FILE");
    if (path == nullptr || *path == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Strength term: NP_STRENGTH_FILE unset, the term is zero";
        return std::make_shared<const StrengthTerm>(g, std::string());
    }
    return std::make_shared<const StrengthTerm>(g, path);
}

}
