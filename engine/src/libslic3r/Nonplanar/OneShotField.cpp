#include "OneShotField.hpp"
#include "libslic3r/Nonplanar/Research.hpp"
#include "Sha256.hpp"

#include "Config.hpp"
#include "HeadEnvelope.hpp"
#include "Multiplanar.hpp"
#include "MultiplanarConfirm.hpp"
#include "Nonplanar/FieldObjective.hpp"
#include "Nonplanar/MultiplanarConfirm.hpp"
#include "Nonplanar/NpField.hpp"
#include "Nonplanar/Rests.hpp"
#include "Npy.hpp"
#include "PatchTools.hpp"
#include "Point.hpp"
#include "StudentNet.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/GCode/ToolKinematics.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process/child.hpp>
#include <boost/process/io.hpp>
#include <math.h>
#include <ios>
#include <functional>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <tbb/task_arena.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>
#include <memory>
#include <mutex>
#include <sstream>
#include <random>
#include <utility>
#include <string>
#include <vector>

namespace Slic3r::Nonplanar {

namespace {

constexpr int    Samples = 8192;
const double     Cos70   = std::cos(70. * M_PI / 180.);
constexpr double Inf     = std::numeric_limits<double>::infinity();
constexpr int    StudentPolish = 3;
constexpr double ClearanceDilation = 0.3;
constexpr double BodyPathCap = 0.;
double body_lean_cap()
{
    const char *v = research_switch("NP_ONESHOT_BODY_LEAN");
    return v != nullptr && *v != 0 ? std::max(0., std::atof(v)) * M_PI / 180. : BodyPathCap;
}
constexpr double OneShotCpuCap    = 900.;
constexpr int    OneShotCpuThreads = 4;
constexpr double OneShotWallGuard = 3600.;
constexpr size_t MaxNodes = 300000;

std::vector<int> columns(const std::vector<std::array<Vec3d, 3>> &tris, const ObjectiveGeometry &g, int axis)
{
    const int u = axis == 0 ? 1 : 0, w = axis == 2 ? 1 : 2;
    const double cyc = axis == 1 ? -1. : 1.;
    const size_t nd[3] = { g.dims[0], g.dims[1], g.dims[2] };
    std::vector<int> acc((nd[axis] + 1) * nd[u] * nd[w], 0);
    auto at = [&](size_t ia, size_t iu, size_t iw) -> int & { return acc[(iu * nd[w] + iw) * (nd[axis] + 1) + ia]; };
    const Vec2d o(g.origin[u] + 1e-5 * g.spacing[u], g.origin[w] + 1e-5 * g.spacing[w] * M_PI / 3.);
    const Vec2d s(g.spacing[u], g.spacing[w]);
    for (const auto &t : tris) {
        const Vec2d  P0(t[0][u], t[0][w]), P1(t[1][u], t[1][w]), P2(t[2][u], t[2][w]);
        const Vec2d  A = P1 - P0, B = P2 - P0;
        const double det = A.x() * B.y() - A.y() * B.x();
        if (std::abs(det) <= 1e-12)
            continue;
        const double sign = (det > 0. ? 1. : -1.) * cyc;
        const Vec2d  lo = P0.cwiseMin(P1).cwiseMin(P2), hi = P0.cwiseMax(P1).cwiseMax(P2);
        const long   u0 = std::max(0L, long(std::ceil((lo.x() - o.x()) / s.x()))), u1 = std::min(long(nd[u]) - 1, long(std::floor((hi.x() - o.x()) / s.x())));
        const long   w0 = std::max(0L, long(std::ceil((lo.y() - o.y()) / s.y()))), w1 = std::min(long(nd[w]) - 1, long(std::floor((hi.y() - o.y()) / s.y())));
        for (long iu = u0; iu <= u1; ++iu)
            for (long iw = w0; iw <= w1; ++iw) {
                const Vec2d  q  = o + Vec2d(double(iu) * s.x(), double(iw) * s.y()) - P0;
                const double bs = (q.x() * B.y() - q.y() * B.x()) / det, bt = (A.x() * q.y() - A.y() * q.x()) / det;
                if (bs < 0. || bt < 0. || bs + bt > 1.)
                    continue;
                const double z  = t[0][axis] + bs * (t[1][axis] - t[0][axis]) + bt * (t[2][axis] - t[0][axis]);
                const long   ia = std::clamp(long(std::ceil((z - g.origin[axis]) / g.spacing[axis])), 0L, long(nd[axis]));
                at(size_t(ia), size_t(iu), size_t(iw)) += int(sign);
            }
    }
    std::vector<int> wind(g.nodes(), 0);
    for (size_t iu = 0; iu < nd[u]; ++iu)
        for (size_t iw = 0; iw < nd[w]; ++iw) {
            int run = 0;
            for (size_t ia = 0; ia < nd[axis]; ++ia) {
                run += at(ia, iu, iw);
                size_t ijk[3];
                ijk[axis] = ia, ijk[u] = iu, ijk[w] = iw;
                wind[g.index(ijk[0], ijk[1], ijk[2])] = run;
            }
        }
    return wind;
}

void edt_line(std::vector<double> &f, double s)
{
    const size_t        n = f.size();
    std::vector<double> d(n), z(n + 1);
    std::vector<size_t> v(n);
    size_t              k = 0;
    size_t              first = n;
    for (size_t q = 0; q < n; ++q)
        if (std::isfinite(f[q])) {
            first = q;
            break;
        }
    if (first == n)
        return;
    v[0] = first, z[0] = -Inf, z[1] = Inf;
    for (size_t q = first + 1; q < n; ++q) {
        if (!std::isfinite(f[q]))
            continue;
        double x;
        while ((x = ((f[q] + s * s * double(q) * double(q)) - (f[v[k]] + s * s * double(v[k]) * double(v[k]))) /
                    (2. * s * s * (double(q) - double(v[k])))) <= z[k])
            --k;
        ++k;
        v[k] = q, z[k] = x, z[k + 1] = Inf;
    }
    k = 0;
    for (size_t q = 0; q < n; ++q) {
        while (z[k + 1] < double(q))
            ++k;
        const double dq = s * (double(q) - double(v[k]));
        d[q]            = dq * dq + f[v[k]];
    }
    f = std::move(d);
}

std::vector<float> shell_fraction(const ObjectiveGeometry &g, double want, double shell_mm)
{
    const size_t        n[3] = { g.dims[0] + 2, g.dims[1] + 2, g.dims[2] + 2 };
    auto                pid  = [&](size_t i, size_t j, size_t k) { return (i * n[1] + j) * n[2] + k; };
    std::vector<double> d(n[0] * n[1] * n[2], 0.);
    for (size_t i = 0; i < g.dims[0]; ++i)
        for (size_t j = 0; j < g.dims[1]; ++j)
            for (size_t k = 0; k < g.dims[2]; ++k)
                if (g.occ[g.index(i, j, k)])
                    d[pid(i + 1, j + 1, k + 1)] = Inf;
    for (int a = 0; a < 3; ++a) {
        const int b = (a + 1) % 3, c = (a + 2) % 3;
        std::vector<double> line(n[a]);
        size_t              ijk[3];
        for (size_t x = 0; x < n[b]; ++x)
            for (size_t y = 0; y < n[c]; ++y) {
                ijk[b] = x, ijk[c] = y;
                for (size_t q = 0; q < n[a]; ++q)
                    ijk[a] = q, line[q] = d[pid(ijk[0], ijk[1], ijk[2])];
                edt_line(line, g.spacing[a]);
                for (size_t q = 0; q < n[a]; ++q)
                    ijk[a] = q, d[pid(ijk[0], ijk[1], ijk[2])] = line[q];
            }
    }
    const double V     = g.spacing.prod();
    size_t       total = 0;
    for (uint8_t o : g.occ)
        total += o;
    want         = std::min(want, V * double(total));
    double reach = std::max(shell_mm, g.spacing.maxCoeff());
    std::vector<uint8_t> layer(g.nodes(), 0);
    for (;;) {
        size_t count = 0;
        for (size_t i = 0; i < g.dims[0]; ++i)
            for (size_t j = 0; j < g.dims[1]; ++j)
                for (size_t k = 0; k < g.dims[2]; ++k) {
                    const size_t id = g.index(i, j, k);
                    layer[id]       = g.occ[id] && std::sqrt(d[pid(i + 1, j + 1, k + 1)]) <= reach + 1e-9;
                    count += layer[id];
                }
        if (V * double(count) >= want || count == total) {
            const float f = float(std::min(1., want / std::max(V * double(count), 1e-12)));
            std::vector<float> out(g.nodes(), 0.f);
            for (size_t id = 0; id < g.nodes(); ++id)
                if (layer[id])
                    out[id] = f;
            return out;
        }
        reach += g.spacing.maxCoeff();
    }
}

std::string file_sha256(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return sha256_hex(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
}

std::vector<std::pair<std::string, std::string>> arm_dir_files(const PrintObject &object, const std::string &ext)
{
    std::vector<std::pair<std::string, std::string>> out;
    const char *dirs = research_switch("NP_ONESHOT_ARM_OFFER");
    if (dirs == nullptr || *dirs == 0)
        return out;
    const std::string stem = boost::filesystem::path(object.model_object()->name).stem().string();
    std::stringstream list(dirs);
    for (std::string dir; std::getline(list, dir, ',');)
        if (const boost::filesystem::path file = boost::filesystem::path(dir) / (stem + ext); boost::filesystem::exists(file))
            out.emplace_back(boost::filesystem::path(dir).filename().string(), file.string());
    return out;
}

bool arm_forced(const std::string &arm)
{
    const char *force = research_switch("NP_ONESHOT_ROUTE_FORCE");
    return force != nullptr && std::string(force) == "{A:" + arm + "}";
}

bool pick_meters()
{
    static const bool on = [] { const char *v = research_switch("NP_ONESHOT_PICK_METERS"); return v != nullptr && *v == '1'; }();
    return on;
}
constexpr double PickPartTie   = 0.5;
constexpr double PickPartShare = 0.05;
constexpr double PickNetTie    = 0.02;
constexpr double PickNetFloor  = 50.;
constexpr double PickTimeTie  = 0.98;
constexpr double PickTimeCap  = 1.5;
struct Meters
{
    double part = std::numeric_limits<double>::quiet_NaN(), net = part, time = part;
};
Meters meters_of(const MeasuredPrint &m, double time) { return { std::max(0., m.floating.part), std::max(0., m.support_mm3), time }; }
double part_band(const Meters &b) { return std::max(PickPartTie, PickPartShare * b.part); }
double net_band(const Meters &b) { return std::max(PickNetTie * b.net, PickNetFloor); }
bool meters_better(const Meters &a, const Meters &b)
{
    if (std::abs(a.part - b.part) > part_band(b))
        return a.part < b.part;
    if (std::abs(a.net - b.net) > net_band(b))
        return a.net < b.net;
    return a.time < PickTimeTie * b.time;
}
std::string meters_text(const Meters &m) { return Slic3r::format("PART %1$.1f mm, NET %2$.0f mm3, %3$.0f s", m.part, m.net, m.time); }

std::string external_field(const std::string &object_name, const std::string &command)
{
    static std::mutex                         mutex;
    static std::map<std::string, std::string> done;
    std::lock_guard<std::mutex>               lock(mutex);
    const std::string                         key = object_name + '\n' + command;
    if (auto it = done.find(key); it != done.end())
        return it->second;
    std::string &result = done[key];
    try {
        namespace fs = boost::filesystem;
        const std::string stem = (fs::temp_directory_path() / fs::unique_path("oneshot-field-%%%%%%%%")).string();
        std::string       cmd  = command;
        auto              put  = [&cmd](const std::string &what, const std::string &with) {
            for (size_t at = cmd.find(what); at != std::string::npos; at = cmd.find(what, at + with.size()))
                cmd.replace(at, what.size(), with);
        };
        put("{name}", fs::path(object_name).stem().string());
        put("{out}", stem + ".npz");
        const char  *limit   = research_switch("NP_ONESHOT_FIELD_SOLVER_TIMEOUT");
        const double seconds = limit != nullptr && std::atof(limit) > 0. ? std::atof(limit) : 300.;
        const auto   t0      = std::chrono::steady_clock::now();
        boost::process::child solver(cmd, (boost::process::std_out & boost::process::std_err) > stem + ".log");
        if (!solver.wait_for(std::chrono::milliseconds(int64_t(1000. * seconds)))) {
            solver.terminate();
            BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object_name << ": NP_ONESHOT_FIELD_SOLVER killed after " << seconds << " s, no offer (log " << stem << ".log)";
        } else if (solver.exit_code() != 0 || !fs::exists(stem + ".npf.npz")) {
            BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object_name << ": NP_ONESHOT_FIELD_SOLVER exit " << solver.exit_code() << ", no offer (log " << stem << ".log)";
        } else {
            result = stem + ".npf.npz";
            boost::system::error_code ec;
            fs::remove(stem + ".npz", ec);
            fs::remove(stem + ".stl", ec);
            BOOST_LOG_TRIVIAL(info) << "One-shot field " << object_name << ": NP_ONESHOT_FIELD_SOLVER wrote " << result << " in "
                                    << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s";
        }
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object_name << ": NP_ONESHOT_FIELD_SOLVER not run, no offer: " << ex.what();
    }
    return result;
}

std::string student_model()
{
    if (const char *env = research_switch("NP_ONESHOT_MODEL"); env != nullptr && *env != 0)
        return std::string(env) == "none" ? std::string() : std::string(env);
    const boost::filesystem::path bundled = boost::filesystem::path(resources_dir()) / "nonplanar" / "student" / "oneshot.nsw";
    return boost::filesystem::exists(bundled) ? bundled.string() : std::string();
}

void save_objective_geometry(const std::string &path, const ObjectiveGeometry &g)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_writer(&zip, path))
        throw RuntimeError(path + ": cannot write it");
    bool ok  = true;
    auto add = [&](const std::string &name, const std::string &bytes) {
        ok = ok && mz_zip_writer_add_mem(&zip, (name + ".npy").c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION);
    };
    const size_t        n = g.p.size();
    std::vector<double> p(3 * n), nn(3 * n), cls(n);
    for (size_t s = 0; s < n; ++s)
        for (int a = 0; a < 3; ++a)
            p[3 * s + a] = g.p[s][a], nn[3 * s + a] = g.n[s][a], cls[s] = g.cls[s];
    add("p", npy_f8({ n, 3 }, p.data()));
    add("n", npy_f8({ n, 3 }, nn.data()));
    add("a", npy_f8({ n }, g.a.data()));
    add("cls", npy_f8({ n }, cls.data()));
    add("origin", npy_f8({ 3 }, g.origin.data()));
    add("spacing", npy_f8({ 3 }, g.spacing.data()));
    const double dims[3] = { double(g.dims[0]), double(g.dims[1]), double(g.dims[2]) };
    add("dims", npy_f8({ 3 }, dims));
    const std::vector<float> occ(g.occ.begin(), g.occ.end());
    add("occ", npy_f4({ g.dims[0], g.dims[1], g.dims[2] }, occ.data()));
    add("shell", npy_f4({ g.dims[0], g.dims[1], g.dims[2] }, g.shell.data()));
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    close_zip_writer(&zip);
    if (!ok)
        throw RuntimeError(path + ": writing it failed");
}

}

double max_time_ratio_override()
{
    const char *env = research_switch("NP_ONESHOT_MAX_TIME_RATIO");
    if (env == nullptr || *env == 0)
        return -1.;
    char        *end   = nullptr;
    const double ratio = std::strtod(env, &end);
    return end != env && *end == 0 && std::isfinite(ratio) && ratio > 0. ? ratio : -1.;
}

ObjectiveGeometry objective_geometry(const indexed_triangle_set &mesh, int wall_loops, int top_shell_layers, double line_width, double layer_height,
                                     const std::function<bool(const std::array<Vec3d, 3> &)> &unsampled)
{
    ObjectiveGeometry                 g;
    std::vector<std::array<Vec3d, 3>> tris;
    std::vector<double>               cdf;
    BoundingBoxf3                     box;
    double                            area = 0.;
    for (const stl_triangle_vertex_indices &f : mesh.indices) {
        const std::array<Vec3d, 3> t { mesh.vertices[f[0]].cast<double>(), mesh.vertices[f[1]].cast<double>(), mesh.vertices[f[2]].cast<double>() };
        tris.push_back(t);
        for (const Vec3d &v : t)
            box.merge(v);
        if (!unsampled || !unsampled(t))
            area += 0.5 * (t[1] - t[0]).cross(t[2] - t[0]).norm();
        cdf.push_back(area);
    }
    if (tris.empty() || area <= 0.)
        return g;
    std::mt19937_64                        rng(0x6e733273ULL ^ uint64_t(tris.size()));
    std::uniform_real_distribution<double> uni(0., 1.);
    const double                           a = area / Samples;
    double                                 area_wall = 0.;
    for (int k = 0; k < Samples; ++k) {
        const size_t f  = std::min<size_t>(size_t(std::upper_bound(cdf.begin(), cdf.end(), uni(rng) * area) - cdf.begin()), tris.size() - 1);
        const double r1 = uni(rng), r2 = uni(rng), s1 = std::sqrt(r1);
        const auto  &t  = tris[f];
        const Vec3d  p  = (1. - s1) * t[0] + s1 * (1. - r2) * t[1] + s1 * r2 * t[2];
        const Vec3d  c  = (t[1] - t[0]).cross(t[2] - t[0]);
        const Vec3d  n  = c / std::max(c.norm(), 1e-30);
        if (n.z() < -std::cos(M_PI / 180.) && p.z() < 0.05) {
            g.bed_area += a;
            continue;
        }
        g.p.push_back(p), g.n.push_back(n), g.a.push_back(a);
        g.cls.push_back(n.z() >= Cos70 ? 0 : n.z() <= -Cos70 ? 2 : 1);
        if (g.cls.back() == 1)
            area_wall += a;
    }
    double pitch = std::clamp(box.size().maxCoeff() / 96., 0.5, 2.);
    auto   nodes = [&box](double p) {
        size_t n = 1;
        for (int ax = 0; ax < 3; ++ax)
            n *= size_t(std::ceil((box.max[ax] + 2. * p - (ax < 2 ? box.min[ax] - 2. * p : 0.)) / p)) + 1;
        return n;
    };
    while (nodes(pitch) > MaxNodes)
        pitch *= 1.02;
    g.origin           = Vec3d(box.min.x() - 2. * pitch, box.min.y() - 2. * pitch, 0.);
    g.spacing          = Vec3d::Constant(pitch);
    g.mesh_min         = box.min;
    g.mesh_max         = box.max;
    for (int ax = 0; ax < 3; ++ax)
        g.dims[size_t(ax)] = size_t(std::ceil((box.max[ax] + 2. * pitch - g.origin[ax]) / pitch)) + 1;
    std::vector<uint8_t> votes(g.nodes(), 0);
    for (int ax = 0; ax < 3; ++ax) {
        const std::vector<int> wind = columns(tris, g, ax);
        for (size_t id = 0; id < g.nodes(); ++id)
            votes[id] += wind[id] != 0;
    }
    g.occ.resize(g.nodes());
    for (size_t id = 0; id < g.nodes(); ++id)
        g.occ[id] = votes[id] >= 2;
    const double shell_mm = std::max(wall_loops * line_width, top_shell_layers * layer_height);
    g.shell = shell_fraction(g, area_wall * wall_loops * line_width + (area - area_wall) * top_shell_layers * layer_height, shell_mm);
    return g;
}

namespace {

bool leans(const ObjectiveMachine &m)
{
    return m.rotary ? m.tilt_max > 0. : *std::max_element(m.slope_cap.begin(), m.slope_cap.end()) > 0.;
}

}

namespace {

bool organic_support(const PrintObjectConfig &oc)
{
    return oc.enable_support.value && is_tree(oc.support_type.value) &&
           (oc.support_style.value == smsTreeOrganic || oc.support_style.value == smsDefault);
}

std::vector<double> mode_off_tops(const PrintObject &object, double layer_height = 0.)
{
    const PrintConfig       &pc = object.print()->config();
    const PrintObjectConfig &oc = object.config();
    SlicingParameters        sp = object.slicing_parameters();
    if (layer_height > 0.)
        sp.layer_height = layer_height;
    const bool prime_tower = pc.enable_prime_tower.value && pc.filament_diameter.values.size() > 1;
    sp.surface_error = 0.;
    sp.overhang_reach = 0.;
    if (!pc.spiral_mode.value && !prime_tower && !organic_support(oc)) {
        sp.road_width    = 1.05 * pc.nozzle_diameter.get_at(0);
        sp.surface_error = oc.adaptive_layer_height_error.value;
        if (oc.overhang_layer_contact.value > 0.)
            sp.overhang_reach = (1. - 0.01 * oc.overhang_layer_contact.value) * sp.road_width;
    }
    const ModelObject         &mo      = *object.model_object();
    const std::vector<coordf_t> profile = (sp.surface_error > 0. || sp.overhang_reach > 0.) && mo.layer_config_ranges.empty() ?
                                              layer_height_profile_surface_error(sp, mo) :
                                              layer_height_profile_from_ranges(sp, mo.layer_config_ranges);
    const std::vector<coordf_t> layers = generate_object_layers(sp, profile, oc.precise_z_height.value);
    std::vector<double>         tops;
    for (size_t i = 1; i < layers.size(); i += 2)
        tops.push_back(layers[i]);
    return tops;
}

std::vector<float> anchor_field(const ObjectiveGeometry &g, const std::vector<double> &tops, double first_layer, double h)
{
    if (tops.empty())
        return {};
    std::vector<double> zk { 0. }, pk { 0. };
    for (size_t j = 0; j < tops.size(); ++j)
        zk.push_back(tops[j]), pk.push_back(first_layer + double(j) * h);
    auto phi = [&](double z) {
        if (z <= 0.)
            return z;
        if (z >= zk.back())
            return pk.back() + (z - zk.back());
        const size_t i = size_t(std::upper_bound(zk.begin(), zk.end(), z) - zk.begin());
        return pk[i - 1] + (pk[i] - pk[i - 1]) * (z - zk[i - 1]) / (zk[i] - zk[i - 1]);
    };
    std::vector<float> f(g.nodes());
    for (size_t i = 0; i < g.dims[0]; ++i)
        for (size_t j = 0; j < g.dims[1]; ++j)
            for (size_t k = 0; k < g.dims[2]; ++k)
                f[g.index(i, j, k)] = float(phi(g.origin.z() + double(k) * g.spacing.z()));
    return f;
}

ObjectiveWeights machine_weights(const PrintObject &object)
{
    ObjectiveWeights   w;
    const std::string &model = object.print()->config().printer_model.value;
    if (object.print()->config().nonplanar_kinematics.value != RotaryLayout::None)
        return w;
    if (model == "Bambu Lab A1 mini")
        w.up *= 1.23, w.wall *= 1.09;
    else if (model == "Prusa MINI" || model == "Prusa MINI IS")
        w.up *= 1.25, w.wall *= 0.98;
    else if (model == "Bambu Lab P1S")
        w.up *= 1.33, w.wall *= 0.94;
    return w;
}

}

ObjectiveMachine objective_machine(const PrintObject &object, const ObjectiveGeometry &geom, double tilt_max)
{
    const PrintConfig       &pc = object.print()->config();
    const PrintObjectConfig &oc = object.config();
    const PrintRegionConfig &rc = object.printing_region(0).config();
    const auto               rad = [](double deg) { return deg * M_PI / 180.; };
    ObjectiveMachine         m;
    m.support_on = oc.enable_support.value;
    m.h                  = oc.layer_height.value;
    m.first_layer        = pc.initial_layer_print_height.value;
    const SlicingParameters &sp = object.slicing_parameters();
    m.thickness_min = sp.min_layer_height > 0. ? sp.min_layer_height : 0.5 * m.h;
    m.thickness_max = sp.max_layer_height > 0. ? sp.max_layer_height : 1.6 * m.h;
    if (organic_support(oc))
        m.thickness_min = m.thickness_max = m.h;
    m.thickness_min *= 0.999;
    m.thickness_max *= 1.001;
    m.printable_overhang = rad(oc.multiplanar_overhang_threshold.value);
    const int    threshold = oc.support_threshold_angle.value > 0 ? oc.support_threshold_angle.value : 30;
    const double cos_s = std::cos(double(threshold) * M_PI / 180.), band = 2. * m.h + geom.spacing.z();
    m.support_cos      = cos_s;
    std::vector<double> faces_z;
    for (size_t s = 0; s < geom.p.size(); ++s)
        if (-geom.n[s].z() >= cos_s)
            faces_z.push_back(geom.p[s].z());
    std::sort(faces_z.begin(), faces_z.end());
    const std::vector<double> tops = mode_off_tops(object);
    m.anchor = anchor_field(geom, tops, m.first_layer, m.h);
    for (size_t j = 1; j < tops.size(); ++j)
        m.thickness_min = std::min(m.thickness_min, tops[j] - tops[j - 1] - 1e-4);
    m.pinned_v.assign(geom.dims[2], std::numeric_limits<float>::quiet_NaN());
    const bool no_pins = research_switch("NP_ONESHOT_NO_PINS") != nullptr && body_lean_cap() > 0.;
    for (const double z : no_pins ? std::vector<double>() : faces_z) {
        const double lo = (z - band - geom.origin.z()) / geom.spacing.z(), hi = (z + band - geom.origin.z()) / geom.spacing.z();
        for (long k = std::max(0L, long(std::floor(lo))); k <= std::min(long(geom.dims[2]) - 1, long(std::ceil(hi))); ++k)
            m.pinned_v[size_t(k)] = m.anchor.empty() ? float(geom.origin.z() + double(k) * geom.spacing.z()) :
                                                       m.anchor[geom.index(0, 0, size_t(k))];
    }
    m.rotary             = pc.nonplanar_kinematics.value != RotaryLayout::None;
    m.tilt_max           = std::min(m.rotary ? tilt_max : 0., m.rotary ? body_lean_cap() : BodyPathCap);
    m.slope_cap.fill(BodyPathCap);
    const double nozzle = pc.nozzle_diameter.get_at(0);
    m.line_width     = oc.line_width.get_abs_value(nozzle);
    m.infill_density = rc.sparse_infill_density.value / 100.;
    m.shell_speed    = 0.5 * (rc.outer_wall_speed.get_at(0) + rc.inner_wall_speed.get_at(0));
    m.sparse_speed   = rc.sparse_infill_speed.get_at(0);
    m.max_volumetric = pc.filament_max_volumetric_speed.get_at(0);
    {
        const bool   bbl     = object.print()->is_BBL_printer();
        const double d_comb  = std::min(nozzle_diameter_for_filament(pc, rc.sparse_infill_filament_id.value, bbl),
                                        nozzle_diameter_for_filament(pc, rc.internal_solid_filament_id.value, bbl));
        const double max_comb = rc.infill_combination_max_layer_height.get_abs_value(d_comb);
        m.infill_combine      = max_comb > 0. ? std::min(max_comb, d_comb) : d_comb;
    }
    m.travel_speed   = pc.travel_speed.get_at(0);
    m.min_layer_time = pc.slow_down_layer_time.get_at(0);
    m.first_layer_speed = pc.initial_layer_speed.get_at(0);
    m.top_speed         = rc.top_surface_speed.get_at(0);
    m.top_shell_layers  = rc.top_shell_layers.value;
    m.shells = false;
    if (m.rotary) {
        TimeModel &z = m.time_zonly;
        z.T = 0., z.c0 = 347.92, z.F_wallp = 0.10143, z.F_upp = 0.0093545, z.Ti = 0.28462, z.F_first = 0.99191, z.cap = 1.069;
        TimeModel &c = m.time_curved;
        c.T = 0., c.c0 = 435.33, c.F_flatx = 0.031288, c.F_upp = 0.046025, c.F_shells_top = 1.3976, c.Ti = 10.461, c.layers = 4.3382,
        c.T_minlayer = 4.0079, c.T_unwind = 0.23456, c.cap = 1.05;
    } else {
        TimeModel &z = m.time_zonly;
        const double a_default = oc.default_acceleration.get_at(0) > 0. ? oc.default_acceleration.get_at(0) : 5000.;
        const double a_travel  = oc.travel_acceleration.get_at(0) > 0. ? oc.travel_acceleration.get_at(0) : a_default;
        z.T = 0., z.c0 = 446.76, z.F_wallt = 1.0471, z.F_upt = 0.98145, z.Ti = 1.1251, z.Li = 0.3468 / m.sparse_speed,
        z.F_seg = 4.5281 * m.sparse_speed / a_default, z.layers = 116.41 * m.travel_speed / a_travel, z.F_first = 0.846;
        struct FixedHeadCap
        {
            const char *model;
            double      cap, support_cA, support_cH;
        };
        static constexpr FixedHeadCap caps[] = {
            { "Bambu Lab A1 mini", 1.080, 0.1696, 0.0155 },
            { "Prusa MINI", 1.095, 0.2132, 0.0260 },
            { "Prusa MINI IS", 1.095, 0.2132, 0.0260 },
            { "Bambu Lab P1S", 1.090, 0.0923, 0.0134 },
        };
        z.cap = 1.080;
        for (const FixedHeadCap &c : caps)
            if (pc.printer_model.value == c.model)
                z.cap = c.cap, m.support_cA = c.support_cA, m.support_cH = c.support_cH;
    }
    if (const double limit = max_time_ratio_override(); limit > 0.) {
        m.time_zonly.cap *= limit / OneShotState::MaxTimeRatio;
        m.time_curved.cap *= limit / OneShotState::MaxTimeRatio;
    }
    if (const OneShotState *state = object.oneshot_state(); state != nullptr && state->cap_scale < 1.)
        m.time_zonly.cap = std::max(1., m.time_zonly.cap * state->cap_scale);
    if (m.rotary)
        m.support_cV = 0.0754, m.support_cS = 0.1563;
    m.road_factor = m.rotary ? 1.30 : 2.42;
    if (m.rotary) {
        m.b_rate       = rad(pc.rotary_max_speed.get_at(0));
        m.c_rate       = rad(pc.rotary_max_speed.get_at(1));
        m.c_continuous = pc.rotary_axis_continuous.get_at(1);
    }
    double rmin = Inf, rmax = 0.;
    for (const Vec2d &p : pc.printable_area.values)
        rmin = std::min(rmin, p.norm()), rmax = std::max(rmax, p.norm());
    if (rmax > 0. && rmax - rmin < 0.05 * rmax)
        m.plate_radius = rmax;
    const Vec3d span = Vec3d(double(geom.dims[0]), double(geom.dims[1]), double(geom.dims[2])).cwiseProduct(geom.spacing);
    if (leans(m))
        m.head = head_points(resolve_head_envelope(pc), geom.spacing.minCoeff(), span.norm(), ClearanceDilation);
    return m;
}

std::chrono::steady_clock::time_point measure_deadline(OneShotState &state)
{
    if (state.measure_deadline == std::chrono::steady_clock::time_point {}) {
        state.measure_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(MeasureSafetyNet));
    }
    return state.measure_deadline;
}

double *measure_work(const PrintObject &object, OneShotState &state)
{
    if (std::isnan(state.measure_work_left)) {
        const double facets = double(std::max<size_t>(object.model_object()->facets_count(), 1));
        double       budget = MeasureWorkBudget * std::clamp(MeasureWorkFacets / facets, 1., MeasureWorkMaxScale);
        if (const char *env = research_switch("NP_ONESHOT_MEASURE_WORK"); env != nullptr && *env != 0)
            budget = std::max(0., std::atof(env));
        state.measure_work_left = budget > 0. ? budget : std::numeric_limits<double>::infinity();
    }
    return &state.measure_work_left;
}

bool oneshot_requested(const PrintObject &object)
{
    const auto *mode = object.model_object()->config.get().option<ConfigOptionEnum<NonplanarMode>>("nonplanar_mode");
    const OneShotState *state = object.oneshot_state();
    return (mode ? mode->value : object.print()->default_object_config().nonplanar_mode.value) == NonplanarMode::OneShot &&
           object.config().nonplanar_atom_toolpath.value.empty() &&
           (state == nullptr || (state->offer != OneShotState::Offer::Auto && state->offer != OneShotState::Offer::Planar));
}

namespace {

thread_local const OneShotState *injected = nullptr;
struct Injection
{
    const OneShotState *previous = injected;
    explicit Injection(const OneShotState &state) { injected = &state; }
    ~Injection() { injected = previous; }
};

}

bool take_injected_oneshot(OneShotState &state)
{
    if (injected == nullptr)
        return false;
    state = *injected;
    state.plinth_applied = false;
    state.injected       = true;
    return true;
}

bool admit_time_cut(const PrintObject &object, OneShotState &state, std::vector<NpField> &cuts)
{
    const MeasureDeadline deadline(measure_deadline(state));
    const MeasureWork     work(measure_work(object, state));
    if (!state.z_only || cuts.empty())
        return false;
    const auto   t0   = std::chrono::steady_clock::now();
    const double t_ps = measured_time(object, NonplanarMode::Off,
                                      {}, {}, false);
    bool admitted = false;
    for (NpField &c : cuts) {
        OneShotState trial = state;
        trial.solved       = std::make_shared<const NpField>(std::move(c));
        trial.z_only       = in_plane_spread(*trial.solved) < OneShotState::ZOnlySpread;
        if (!trial.z_only)
            continue;
        double        t_native;
        MeasuredPrint native;
        {
            const Injection native_only(trial);
            t_native = measured_time(object, NonplanarMode::OneShot, {}, {}, false, &native);
        }
        const double ratio = t_native / t_ps;
        BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": " << trial.solved->solver << " measured " << ratio
                                << " x planar + support" << (ratio <= state.max_time_ratio() ? ", adopted" : ", over the limit");
        const std::string &solver = trial.solved->solver;
        const size_t       at     = solver.rfind(" native ") + 8;
        state.time_cut_note += Slic3r::format(", time-cut native %1% measured %2$.3f%3%", solver.substr(at, solver.size() - at - 1), ratio,
                                              ratio <= state.max_time_ratio() ? " (adopted)" : "");
        if (std::isfinite(ratio) && ratio <= state.max_time_ratio()) {
            state.solved = trial.solved;
            admitted     = true;
            state.measured_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (object.print()->objects().size() == 1)
                auto_offer(object, state, t_ps, native.rms, t_native);
            return admitted;
        }
    }
    state.measured_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return admitted;
}

bool admit_lean_offer(const PrintObject &object, OneShotState &state, std::vector<NpField> &cuts)
{
    state.forced_arm = false;
    std::vector<NpField> leans;
    for (auto it = cuts.begin(); it != cuts.end();)
        if (it->solver.rfind("lean offer ", 0) == 0) {
            leans.push_back(std::move(*it));
            it = cuts.erase(it);
        } else
            ++it;
    if (leans.empty() || !state.solved)
        return false;
    const std::string      name = object.model_object()->name;
    const ObjectiveWeights W    = machine_weights(object);
    auto term = [](const NpField &f, const char *key) {
        for (const auto &[k, v] : f.terms)
            if (k == key)
                return v;
        return std::numeric_limits<double>::quiet_NaN();
    };
    const char *keep_dir = research_switch("NP_ONESHOT_LEAN_KEEP");
    auto        keep_as  = [&](const std::string &what) {
        return keep_dir != nullptr && *keep_dir != 0 ? (boost::filesystem::path(keep_dir) / (name + "." + what + ".gcode")).string() : std::string();
    };
    const double  t_ps = measured_time(object, NonplanarMode::Off, {}, keep_as("ps"), false);
    MeasuredPrint mb;
    double        t_body;
    {
        const Injection body_only(state);
        t_body = measured_time(object, NonplanarMode::OneShot, {}, keep_as("body"), false, &mb);
    }
    Meters best_m = meters_of(mb, t_body);
    const bool     user_support = object.config().enable_support.value && !object.auto_support_applied();
    const Floating control      = planar_control_floating(object, user_support);
    static const double F = [] { const char *v = research_switch("NP_ONESHOT_FLOAT_W"); return v != nullptr && *v != 0 ? std::max(0., std::atof(v)) : 0.; }();
    const double   J_body = term(*state.solved, "J") + F * std::max(0., mb.floating.part), S_body = term(*state.solved, "J_S"),
                 T_body = term(*state.solved, "J_T");
    std::shared_ptr<const NpField> best;
    double                         best_J = J_body;
    for (NpField &lf : leans) {
        OneShotState trial = state;
        trial.solved       = std::make_shared<const NpField>(std::move(lf));
        trial.z_only       = false;
        MeasuredPrint mo;
        double        t_off;
        {
            const Injection lean_only(trial);
            t_off = measured_time(object, NonplanarMode::OneShot, {}, keep_as("lean." + boost::filesystem::path(trial.solved->solver).filename().string()),
                                  pick_meters(), &mo);
        }
        const NpField &o     = *trial.solved;
        const std::string arm_prefix = "lean offer arm ";
        const bool        forced     = o.solver.rfind(arm_prefix, 0) == 0 &&
                             arm_forced(o.solver.substr(arm_prefix.size(), o.solver.find(',') - arm_prefix.size()));
        const double   S_off = term(o, "J_S"), T_off = term(o, "J_T"), J_off = term(o, "J");
        const double   sup_ratio = mb.support_mm3 > 0. && std::isfinite(mo.support_mm3) ? mo.support_mm3 / mb.support_mm3 : 1.;
        const double   S_meas = S_body * sup_ratio;
        const double   J_meas = J_off - W.sup * S_off + W.sup * S_meas - W.time * T_off + W.time * T_body * t_off / t_body + F * std::max(0., mo.floating.part);
        std::string    why;
        if (!std::isfinite(t_off))
            why = "its measured slice failed";
        else if (!forced && !(t_off <= 1.5 * t_ps))
            why = Slic3r::format("%1$.2f x planar + support", t_off / t_ps);
        else if (!forced && control.part >= 0. && mo.floating.part > control.part + 1e-6)
            why = Slic3r::format("part floating %1$.1f mm > the control's %2$.1f", mo.floating.part, control.part);
        else if (pick_meters() && !forced && !meters_better(meters_of(mo, t_off), best_m))
            why = "not better on the meters (" + meters_text(meters_of(mo, t_off)) + " vs " + meters_text(best_m) + ")";
        else if (!pick_meters() && !(J_meas < best_J) && !forced)
            why = Slic3r::format("measured J %1$.0f >= %2$.0f", J_meas, best_J);
        const std::string line = Slic3r::format("%1%: J %2$.0f (S %3$.1f -> measured %4$.1f, support %5$.0f vs the body's %6$.0f mm3; T %7$.0f -> measured "
                                                "%8$.0f s vs the body's %9$.0f, p+s %10$.0f), measured J %11$.0f vs the body's %12$.0f, floating %13$.1f "
                                                "(body %14$.1f, control %15$.1f), F %17$.1f%16%",
                                                o.solver, J_off, S_off, S_meas, mo.support_mm3, mb.support_mm3, T_off, t_off, t_body, t_ps, J_meas, J_body,
                                                mo.floating.part, mb.floating.part, control.part, why.empty() ? ", candidate" : ", out: " + why, F);
        BOOST_LOG_TRIVIAL(warning) << "One-shot field " << name << ": " << line;
        state.time_cut_note += ", " + line;
        if (why.empty()) {
            double t_verified;
            {
                const Injection lean_only(trial);
                t_verified = measured_time(object, NonplanarMode::OneShot, {}, {}, true);
            }
            if (std::isfinite(t_verified)) {
                best   = trial.solved;
                best_J = forced ? -std::numeric_limits<double>::infinity() : J_meas;
                best_m = forced ? Meters { -1., -1., -1. } : meters_of(mo, t_off);
                state.forced_arm = forced;
            } else
                state.time_cut_note += last_measured_failure().empty() ? std::string(" (its verified export hits)") :
                                                                         " (its verified export failed: " + last_measured_failure() + ")";
        }
    }
    if (!best)
        return false;
    state.solved = best;
    state.z_only = in_plane_spread(*best) < OneShotState::ZOnlySpread;
    state.time_cut_note += ", the lean adopted";
    return true;
}

bool past_floating_bar(const PrintObject &object, OneShotState &state, const Floating &floating)
{
    const MeasureDeadline deadline(measure_deadline(state));
    const MeasureWork     work(measure_work(object, state));
    bool float_over = false;
    if (floating.part >= 0. && floats_past_bar(floating, {})) {
        const bool     user_support = object.config().enable_support.value && !object.auto_support_applied();
        const Floating control      = planar_control_floating(object, user_support);
        state.control_floating      = control.part;
        if (control.part >= 0. && !user_support)
            state.planar_air = control.air;
        float_over = control.part >= 0. && floats_past_bar(floating, control);
    }
    return float_over;
}

bool measured_repick(const PrintObject &object, OneShotState &state, double ratio, const Floating &floating, bool support)
{
    using Offer              = OneShotState::Offer;
    state.measured_ratio     = ratio;
    state.measured_floating  = floating.part;
    if (state.offer == Offer::Control)
        return false;
    const bool time_over = std::isfinite(ratio) && ratio > state.max_time_ratio() && !state.measured_anchor;
    const bool float_over = past_floating_bar(object, state, floating);
    if (!time_over && !float_over)
        return false;
    const bool budget = state.measured_repicks < MaxRepicks;
    if (float_over)
        state.offer = budget && support && state.offer == Offer::Body ? Offer::NormalSupport : Offer::Control;
    else if (budget)
        state.cap_scale *= state.max_time_ratio() / ratio;
    else
        state.measured_anchor = true;
    ++state.measured_repicks;
    BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": measured " << ratio << " x planar + support, floating "
                            << floating.part << " mm against the control's " << state.control_floating << " (" << state.planar_air << " into the air): "
                            << (float_over ? (state.offer == Offer::Control ? "the control offer" : "the normal(auto) support offer") :
                                state.measured_anchor ? "taking the anchor" : "solving again at a tighter cap");
    return true;
}

bool band_control(const PrintObject &object, OneShotState &state)
{
    if (pick_meters() || state.offer == OneShotState::Offer::Control || state.offer == OneShotState::Offer::Auto ||
        state.offer == OneShotState::Offer::Planar || state.measured_anchor || state.route != nullptr)
        return false;
    const double band = object.wall_band(LayerChangeMax), total = object.wall_band();
    state.band_note = Slic3r::format(", wall band %1$.4f over one step (total %2$.4f)", band, total);
    if (band < PrintObject::OneShotWallBand)
        return false;
    state.offer = OneShotState::Offer::Control;
    ++state.measured_repicks;
    state.band_note += " -> control";
    BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": wall band " << band << " past " << PrintObject::OneShotWallBand
                            << ": the control offer";
    return true;
}

bool control_past_cap(const PrintObject &object, OneShotState &state)
{
    using Offer = OneShotState::Offer;
    if (state.offer == Offer::Control || state.offer == Offer::Auto || state.offer == Offer::Planar || !state.measured_anchor || !(state.measured_ratio > state.max_time_ratio()))
        return false;
    state.offer = Offer::Control;
    ++state.measured_repicks;
    state.band_note += Slic3r::format(", the anchor %1$.3f x past the cap -> control", state.measured_ratio);
    BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": the anchor measured " << state.measured_ratio
                            << " x planar + support, past the cap: the control offer";
    return true;
}

constexpr double OneShotBodyBand   = 0.01;
constexpr double OneShotBodySlower = 1.02;

bool planar_keeps_pick(const PrintObject &object, OneShotState &state, const Floating &floating, double net)
{
    using Offer = OneShotState::Offer;
    const char *force = research_switch("NP_ONESHOT_ROUTE_FORCE");
    if (!pick_meters() || state.injected || state.offer == Offer::Control || state.offer == Offer::Auto || state.offer == Offer::Planar ||
        (force != nullptr && *force != 0))
        return false;
    const MeasureDeadline deadline(measure_deadline(state));
    const MeasureWork     work(measure_work(object, state));
    MeasuredPrint         control;
    const double          t_control = measured_control(object, control);
    if (!std::isfinite(t_control))
        return false;
    const Meters pick { std::max(0., floating.part), std::max(0., net), 0. }, planar = meters_of(control, t_control);
    const bool   wins = planar.part - pick.part > part_band(planar) || planar.net - pick.net > net_band(planar);
    state.band_note += ", against planar (" + meters_text(planar) + "): PART " + Slic3r::format("%1$.1f", pick.part) + " mm, NET " +
                       Slic3r::format("%1$.0f", pick.net) + " mm3" + (wins ? " -> the pick" : " -> planar (no PART or NET win)");
    if (wins)
        return false;
    state.offer = Offer::Planar;
    ++state.measured_repicks;
    BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": no PART or NET win over planar: it prints planar";
    return true;
}

bool body_earns_control(const PrintObject &object, OneShotState &state, const Floating &floating)
{
    const MeasureDeadline deadline(measure_deadline(state));
    const MeasureWork     work(measure_work(object, state));
    using Offer = OneShotState::Offer;
    if (pick_meters() || state.offer == Offer::Control || state.offer == Offer::Auto || state.offer == Offer::Planar || state.route != nullptr)
        return false;
    std::string why;
    if (const double band = object.wall_band(); band > OneShotBodyBand)
        why = Slic3r::format(", wall band %1$.4f past %2$.2f -> control", band, OneShotBodyBand);
    else if (state.measured_ratio > OneShotBodySlower) {
        const bool     user_support = object.config().enable_support.value && !object.auto_support_applied();
        const Floating control      = planar_control_floating(object, user_support);
        if (!(control.part >= 0. && floating.part >= 0. && floating.part <= 0.5 * control.part && control.part - floating.part >= 1.))
            why = Slic3r::format(", %1$.3f x the control, floating %2$.1f mm against its %3$.1f -> control", state.measured_ratio,
                                 floating.part, control.part);
    }
    if (why.empty())
        return false;
    state.offer = Offer::Control;
    ++state.measured_repicks;
    state.band_note += why;
    BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": the body against its control" << why;
    return true;
}

bool auto_offer(const PrintObject &object, OneShotState &state, double t_ps, double pick_rms, double pick_time)
{
    const MeasureDeadline deadline(measure_deadline(state));
    const MeasureWork     work(measure_work(object, state));
    if (state.offer == OneShotState::Offer::Auto || state.auto_weighed || !std::isfinite(t_ps) || t_ps <= 0.)
        return false;
    state.auto_weighed = true;
    const auto    t0 = std::chrono::steady_clock::now();
    if (!state.auto_measured) {
        MeasuredPrint automatic;
        automatic.as_printed     = make_tool_kinematics(object.print()->config())->rotary_count() == 0;
        automatic.count_contacts = !automatic.as_printed;
        std::vector<std::pair<std::string, std::string>> user;
        for (const char *key : { "enable_support", "support_type", "support_style", "support_critical_regions_only" }) {
            const ConfigOption *option = object.model_object()->config.get().option(key);
            if (option == nullptr)
                option = object.print()->default_object_config().option(key);
            if (option != nullptr)
                user.emplace_back(key, option->serialize());
        }
        measured_time(object, NonplanarMode::Auto, user, {}, automatic.as_printed, &automatic);
        if (automatic.contacts > 0) {
            MeasuredPrint planar;
            measured_time(object, NonplanarMode::Off, user, {}, false, &planar);
            planar.floating   = Floating { -1., -1. };
            planar.contacts   = automatic.contacts;
            automatic         = planar;
            state.auto_planar = true;
        }
        state.auto_measured = std::make_shared<const MeasuredPrint>(std::move(automatic));
        state.auto_floats = past_floating_bar(object, state, state.auto_measured->floating);
    }
    const MeasuredPrint &automatic = *state.auto_measured;
    state.measured_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const bool   floats = state.auto_floats;
    const double ratio  = automatic.time / t_ps;
    const bool   pick_over = pick_time / t_ps > state.max_time_ratio();
    const bool   smoother  = !state.auto_planar && (!std::isfinite(pick_rms) || automatic.rms < pick_rms);
    const bool   wins      = !floats && std::isfinite(automatic.rms) && std::isfinite(automatic.time) &&
                      (smoother || (pick_over && automatic.time < pick_time));
    state.auto_note = Slic3r::format(", Auto %1$.4f mm %2$.3f x against the pick's %3$.4f mm %4$.3f x%5%", automatic.rms, ratio, pick_rms,
                                     pick_time / t_ps,
                                     floats                                ? Slic3r::format(", Auto floats %1$.1f mm past the bar", automatic.floating.part) :
                                     !wins                                 ? std::string() :
                                     state.auto_planar                     ? Slic3r::format(" -> Auto taken: its planar control (%1% curved moves the contact bar strikes)", automatic.contacts) :
                                     std::isfinite(pick_rms) && !(automatic.rms < pick_rms) ? std::string(" -> Auto taken: faster than the pick past the cap") :
                                     ratio > state.max_time_ratio()    ? Slic3r::format(" -> Auto taken: x%1$.2f planar, past the cap", ratio) :
                                                                             std::string(" -> Auto"));
    BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": Auto's own print measured RMS along the normal "
                            << automatic.rms << " mm, " << ratio << " x planar + support, against the pick's " << pick_rms << " mm, "
                            << pick_time / t_ps << " x, Auto floating " << automatic.floating.part << " mm (support " << automatic.support
                            << (floats ? ", past the bar" : "") << "): " << (wins ? "Auto's print" : "the pick");
    if (wins)
        state.offer = OneShotState::Offer::Auto;
    return wins;
}

void measured_export_check(PrintObject &object, OneShotState &state, const std::function<void()> &resolve)
{
    const MeasureDeadline deadline(measure_deadline(state));
    const MeasureWork     work(measure_work(object, state));
    if (!state.z_only || research_switch("NP_ONESHOT_NO_MEASURED_CHECK") != nullptr || object.print()->objects().size() == 1)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    const double t_ps = measured_time(object, NonplanarMode::Off,
                                      {}, {}, false);
    for (;;) {
        MeasuredPrint body;
        {
            const Injection body_only(state);
            measured_time(object, NonplanarMode::OneShot, {}, {}, false, &body);
        }
        if (!measured_repick(object, state, body.time / t_ps, body.floating, body.support))
            break;
        resolve();
    }
    state.measured_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

double in_plane_spread(const NpField &f)
{
    double spread = 0.;
    for (size_t k = 0; k < f.dims[2]; ++k) {
        double lo = Inf, hi = -Inf;
        for (size_t i = 0; i < f.dims[0]; ++i)
            for (size_t j = 0; j < f.dims[1]; ++j)
                lo = std::min(lo, double(f.phi[f.index(i, j, k)])), hi = std::max(hi, double(f.phi[f.index(i, j, k)]));
        spread = std::max(spread, 0.5 * (hi - lo));
    }
    return spread;
}

constexpr double TopSpreadLayers = 6.;

static std::vector<double> tops_profile(const std::vector<double> &tops)
{
    std::vector<double> out { 0., tops.front() };
    for (size_t j = 1; j < tops.size(); ++j) {
        const double below = tops[j - 1], h = tops[j] - below;
        out.insert(out.end(), { below, h, tops[j] - 1e-4, h });
    }
    out.insert(out.end(), { tops.back(), out.back() });
    return out;
}

static std::vector<double> capped_tops(const std::vector<double> &tops, double min_h, double max_h)
{
    if (tops.size() < 3)
        return tops;
    constexpr double    dz    = 0.01;
    const double        z0    = tops.front(), top = tops.back();
    const size_t        n     = size_t(std::ceil((top - z0) / dz)) + 1;
    std::vector<double> t(n);
    for (size_t i = 0, j = 1; i < n; ++i) {
        const double z = z0 + (double(i) + 0.5) * dz;
        while (j + 1 < tops.size() && tops[j] <= z)
            ++j;
        t[i] = std::clamp(tops[j] - tops[j - 1], min_h, max_h);
    }
    cap_layer_change(t, dz);
    auto at = [&](double z) { return t[std::min(n - 1, size_t(std::max(0., (z - z0) / dz)))]; };
    std::vector<double> out { z0 };
    for (double below = z0; below < top - 1e-6;) {
        double h = at(below);
        for (double z = below; z < below + h && z < top; z += dz)
            h = std::min(h, std::max(at(z), z - below));
        h = std::clamp(h, min_h, max_h);
        const double rest = top - below;
        size_t       n    = size_t(std::ceil(rest / h - 1e-9));
        while (n > 1 && rest / double(n) < min_h)
            --n;
        if (n <= 1 || rest / double(n) > max_h)
            h = std::min(rest, max_h);
        else if (rest < TopSpreadLayers * h)
            h = rest / double(n);
        below = std::min(below + h, top);
        out.push_back(below);
    }
    return out;
}

std::vector<double> layer_profile(const NpField &f, double first_layer, double top, double min_h, double max_h,
                                  const std::vector<std::pair<double, double>> &planar_bands, double planar_h)
{
    if (!f.layer_tops.empty())
        return tops_profile(f.layer_tops);
    std::vector<double> z(f.dims[2]), phi(f.dims[2]);
    for (size_t k = 0; k < f.dims[2]; ++k) {
        double sum = 0.;
        for (size_t i = 0; i < f.dims[0]; ++i)
            for (size_t j = 0; j < f.dims[1]; ++j)
                sum += f.phi[f.index(i, j, k)];
        z[k]   = f.node(0, 0, k).z();
        phi[k] = sum / double(f.dims[0] * f.dims[1]);
    }
    auto inverse = [&](double level) {
        for (size_t k = 1; k < z.size(); ++k)
            if (phi[k] >= level)
                return z[k - 1] + (z[k] - z[k - 1]) * (level - phi[k - 1]) / std::max(phi[k] - phi[k - 1], 1e-12);
        return z.back() + (level - phi.back());
    };
    std::vector<double> out { 0., first_layer };
    double              below = first_layer;
    for (int j = 1; below < top - 1e-6; ++j) {
        double h = std::clamp(inverse(first_layer + j * f.h) - below, min_h, max_h);
        if (planar_h > 0. && std::any_of(planar_bands.begin(), planar_bands.end(),
                                         [&](const auto &b) { return below + h > b.first && below < b.second; })) {
            const double next = first_layer + std::ceil((below + min_h - first_layer) / planar_h - 1e-6) * planar_h;
            h = std::min(next - below, max_h);
            while (inverse(first_layer + (j + 1) * f.h) < below + h + min_h)
                ++j;
        }
        if (const double rest = top - below; rest - h < std::max(min_h, 0.5 * f.h))
            h = rest <= max_h ? rest : 0.5 * rest;
        if (smooth_layers() && planar_h <= 0.) {
            const double rest = top - below, h0 = std::clamp(inverse(first_layer + j * f.h) - below, min_h, max_h);
            if (rest < TopSpreadLayers * h0 && rest > h0 + 1e-6) {
                size_t parts = size_t(std::ceil(rest / h0 - 1e-9));
                while (parts > 1 && rest / double(parts) < min_h)
                    --parts;
                h = std::min(rest / double(std::max<size_t>(parts, 1)), max_h);
            }
        }
        out.insert(out.end(), { below, h, std::min(below + h, top) - 1e-4, h });
        below += h;
    }
    out.insert(out.end(), { top, out.back() });
    return out;
}

std::vector<std::pair<double, double>> supported_underside_bands(const indexed_triangle_set &mesh, int threshold_deg, double band)
{
    const double cos_s = std::cos(double(threshold_deg) * M_PI / 180.);
    float        bed   = std::numeric_limits<float>::max();
    for (const stl_vertex &v : mesh.vertices)
        bed = std::min(bed, v.z());
    std::vector<std::pair<double, double>> bands;
    for (const stl_triangle_vertex_indices &t : mesh.indices) {
        const Vec3d a = mesh.vertices[t[0]].cast<double>(), b = mesh.vertices[t[1]].cast<double>(), c = mesh.vertices[t[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        const double lo = std::min({ a.z(), b.z(), c.z() }), hi = std::max({ a.z(), b.z(), c.z() });
        if (n.norm() > 1e-9 && -n.z() >= cos_s * n.norm() && lo > double(bed) + EPSILON)
            bands.emplace_back(lo - band, hi + band);
    }
    std::sort(bands.begin(), bands.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto &b : bands)
        if (!merged.empty() && b.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, b.second);
        else
            merged.push_back(b);
    return merged;
}

namespace {

double solved_term(const OneShotState &state, const char *name)
{
    if (state.solved)
        for (const auto &[key, value] : state.solved->terms)
            if (key == name)
                return value;
    return 0.;
}

}

double OneShotState::time_ratio() const { return solved_term(*this, "stat_T_ratio"); }
double OneShotState::time_cap() const { return solved_term(*this, "stat_T_cap"); }
bool OneShotState::combine_infill() const { return solved_term(*this, "stat_infill_separate") < 0.5; }

std::string OneShotState::offers() const
{
    std::string out;
    if (!solved)
        return out;
    out += time_cut_note;
    const std::string prefix = "stat_offer_J_";
    for (const auto &[key, value] : solved->terms)
        if (key.compare(0, prefix.size(), prefix) == 0) {
            const std::string name = key.substr(prefix.size());
            out += (out.empty() ? ", offers: " : ", ") + name + " " +
                   (std::isnan(value) ? std::string("none") :
                                        Slic3r::format("J %1$.0f %2%", value,
                                                       solved_term(*this, ("stat_offer_feasible_" + name).c_str()) > 0.5 ? "feasible" : "infeasible"));
        }
    return out;
}

std::string OneShotState::floating_decision() const
{
    if (std::isnan(measured_floating))
        return {};
    return Slic3r::format(", floating %1$.1f mm (control %2%) -> %3%", measured_floating,
                          std::isnan(control_floating) ? std::string("not sliced: under any bar") : Slic3r::format("%1$.1f", control_floating),
                          offer == Offer::Control ? "control" : offer == Offer::NormalSupport ? "normal(auto) support" : offer == Offer::Auto ? "Auto" :
                          offer == Offer::Planar ? "planar" : "body");
}

NpField one_shot_field(const PrintObject &object, const indexed_triangle_set &mesh, double tilt_max, const std::function<void()> &cancel,
                       std::string &student, int &polish_k, OneShotAbortReason &abort, std::vector<NpField> *time_cut,
                       const std::function<bool(const std::array<Vec3d, 3> &)> &unsampled)
{
    const auto               t0 = std::chrono::steady_clock::now();
    const PrintObjectConfig &oc = object.config();
    const PrintRegionConfig &rc = object.printing_region(0).config();
    const double             nozzle = object.print()->config().nozzle_diameter.get_at(0);
    const ObjectiveGeometry  g = objective_geometry(mesh, rc.wall_loops.value, rc.top_shell_layers.value, oc.line_width.get_abs_value(nozzle),
                                                    oc.layer_height.value, unsampled);
    if (g.nodes() == 0) {
        abort      = OneShotAbortReason();
        abort.kind = OneShotAbort::Infeasible;
        abort.what = "no grid node inside the mesh";
        return NpField();
    }
    const ObjectiveMachine   m = objective_machine(object, g, tilt_max);
    const ObjectiveWeights   W = machine_weights(object);
    ObjectiveParams          params;
    params.max_starts   = 3;
    params.lottery_keep = 1;
    params.level_rounds = 1;
    params.local_rounds = 0;
    params.budget       = 0.;
    if (const char *only = research_switch("NP_ONESHOT_ONLY_OFFER"); only != nullptr)
        params.only_offer = only;
    if (const char *search = research_switch("NP_ONESHOT_SEARCH"); search != nullptr && *search != 0) {
        int starts = params.max_starts, keep = params.lottery_keep, local = params.local_rounds;
        if (std::sscanf(search, "%d,%d,%d", &starts, &keep, &local) >= 1) {
            params.max_starts   = starts;
            params.lottery_keep = std::max(1, keep);
            params.local_rounds = std::max(0, local);
        }
    }
    if (const OneShotState *state = object.oneshot_state();
        state != nullptr && (state->measured_anchor || state->offer == OneShotState::Offer::Control))
        params.only_offer = "anchor";
    const bool level_only = !leans(m);
    if (level_only)
        params.planar_adaptive = true;
    std::vector<std::vector<double>> exact_tops;
    if (params.only_offer.empty()) {
        const SlicingParameters &sp = object.slicing_parameters();
        for (const float q : { 0.25f, 0.5f, 1.f }) {
            const std::vector<coordf_t> layers = generate_object_layers(sp, layer_height_profile_adaptive(sp, *object.model_object(), q),
                                                                        object.config().precise_z_height.value);
            std::vector<double> tops;
            for (size_t i = 1; i < layers.size(); i += 2)
                tops.push_back(layers[i]);
            if (smooth_layers())
                tops = capped_tops(tops, m.thickness_min, m.thickness_max);
            params.native_offers.emplace_back("orca_adaptive_q" + std::to_string(int(std::lround(100. * q))), anchor_field(g, tops, m.first_layer, m.h));
            params.exact_natives.emplace_back("native_orca_q" + std::to_string(int(std::lround(100. * q))), anchor_field(g, tops, m.first_layer, m.h));
            exact_tops.push_back(tops);
        }
        for (int i = 2; i <= 7; ++i) {
            const double t = 0.04 * i;
            if (t < m.thickness_min - 1e-9 || t > m.thickness_max + 1e-9)
                continue;
            std::vector<double> tops = mode_off_tops(object, t);
            if (smooth_layers())
                tops = capped_tops(tops, m.thickness_min, m.thickness_max);
            params.exact_natives.emplace_back("native_u" + float_to_string_decimal_point(t, 2), anchor_field(g, tops, m.first_layer, m.h));
            exact_tops.push_back(std::move(tops));
        }
    }
    const std::string model = level_only ? std::string() : student_model();
    std::vector<TimeCutOffer> cuts;
    if (time_cut && model.empty() && params.only_offer.empty() && research_switch("NP_ONESHOT_NO_MEASURED_CHECK") == nullptr)
        params.time_cut = &cuts;
    student  = model.empty() ? std::string("none") : file_sha256(model);
    polish_k = model.empty() ? 0 : StudentPolish;
    NpField f;
    f.origin      = g.origin;
    f.spacing     = g.spacing;
    f.dims        = g.dims;
    f.h           = m.h;
    f.first_layer = m.first_layer;
    f.phi.resize(g.nodes());
    for (size_t i = 0; i < g.dims[0]; ++i)
        for (size_t j = 0; j < g.dims[1]; ++j)
            for (size_t k = 0; k < g.dims[2]; ++k)
                f.phi[g.index(i, j, k)] = float(g.origin.z() + double(k) * g.spacing.z());
    if (m.anchor.size() == g.nodes())
        f.phi = m.anchor;
    if (!model.empty()) {
        f.phi                = student_phi(StudentNet(model), g, m);
        params.multi_start   = false;
        params.warm_adaptive = false;
        params.local_rounds  = 0;
        params.level_rounds  = 0;
        params.iterations    = StudentPolish;
    }
    const std::vector<float> anchor_phi  = f.phi;
    const ObjectiveParams    full_params = params;
    std::string              init_path;
    if (const char *init = research_switch("NP_ONESHOT_INIT"); init != nullptr && *init != 0 && model.empty()) {
        BoundingBoxf3 box;
        for (const stl_vertex &v : mesh.vertices)
            box.merge(v.cast<double>());
        try {
            const NpLayerField given_field(load_npfield_for(init, box, plate_offset(object)));
            for (size_t i = 0; i < g.dims[0]; ++i)
                for (size_t j = 0; j < g.dims[1]; ++j)
                    for (size_t k = 0; k < g.dims[2]; ++k)
                        f.phi[g.index(i, j, k)] = float(given_field.phi(g.origin + g.spacing.cwiseProduct(Vec3d(double(i), double(j), double(k)))));
            init_path            = init;
            params.multi_start   = false;
            params.warm_adaptive = false;
            params.local_rounds  = 0;
            params.level_rounds  = 0;
            const char *iters    = research_switch("NP_ONESHOT_INIT_ITERS");
            params.iterations    = iters != nullptr && *iters != 0 ? std::max(1, std::atoi(iters)) : 20;
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object.model_object()->name << ": NP_ONESHOT_INIT " << init << " unused: " << ex.what();
        }
    }
    struct Aborted
    {
        bool wall;
    };
    const std::vector<float> planar = f.phi;
    ObjectiveTerms           t;
    const double             cpu0 = process_cpu_seconds();
    const int                threads = std::clamp(tbb::this_task_arena::max_concurrency(), 1, OneShotCpuThreads);
    const double             cpu_cap = OneShotCpuCap * threads;
    ObjectiveTerms given;
    auto solve_once = [&]() {
    abort                         = OneShotAbortReason();
    try {
        t = solve_objective(g, m, W, params, f.phi,
                            [&cancel, t0, cpu0, cpu_cap]() {
                                if (cancel)
                                    cancel();
                                if (cpu0 >= 0. && process_cpu_seconds() - cpu0 > cpu_cap)
                                    throw Aborted { false };
                                if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > OneShotWallGuard)
                                    throw Aborted { true };
                            },
                            [&object](const std::string &line) {
                                BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": " << line;
                            },
                            &given);
        if (!t.feasible()) {
            abort.kind = OneShotAbort::Infeasible;
            abort.what = Slic3r::format("H thickness/slope/head/bed/time %1%/%2%/%3%/%4%/%5%, time ratio %6$.3f vs cap %7$.3f; the start: "
                                        "%8%/%9%/%10%/%11%/%12%, ratio %13$.3f",
                                        t.H_thickness, t.H_slope, t.H_head, t.H_bed, t.H_time, t.T_ratio, t.T_cap, given.H_thickness,
                                        given.H_slope, given.H_head, given.H_bed, given.H_time, given.T_ratio);
        }
    } catch (const Aborted &a) {
        abort.kind = a.wall ? OneShotAbort::WallTime : OneShotAbort::CpuTime;
        abort.what = a.wall ? Slic3r::format("%1% s wall", OneShotWallGuard) : Slic3r::format("%1% CPU-s", cpu_cap);
        f.phi      = planar;
        t          = evaluate_objective(g, m, W, f.phi, true);
        BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object.model_object()->name << ": solve aborted at " << abort.what;
    }
    };
    solve_once();
    if (!init_path.empty()) {
        BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": from " << init_path << ": J " << t.J
                                << (t.feasible() ? ", feasible" : ", outside the hard set: solved again from planar");
        if (abort.kind == OneShotAbort::Infeasible) {
            f.phi  = anchor_phi;
            params = full_params;
            solve_once();
        }
    }
    BoundingBoxf3 box;
    for (const stl_vertex &v : mesh.vertices)
        box.merge(v.cast<double>());
    f.mesh_min = box.min;
    if (const size_t raised = make_z_monotone(f); raised > 0)
        BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": " << raised << " grid nodes raised to keep phi increasing in z";
    f.solver   = model.empty() ? std::string("one-shot teacher ") + ObjectiveVersion :
                                 "one-shot student " + boost::filesystem::path(model).filename().string() + " " + ObjectiveVersion;
    f.terms    = t.named();
    f.terms.emplace_back("stat_j_version", ObjectiveVersionNumber);
    f.proxy_version = ObjectiveVersion;
    f.terms.emplace_back("stat_oneshot_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    f.terms.emplace_back("stat_oneshot_aborted", double(int(abort.kind)));
    f.terms.emplace_back("stat_anchor_ratio", given.T_ratio);
    f.terms.emplace_back("stat_anchor_feasible", given.feasible() ? 1. : 0.);
    f.terms.emplace_back("stat_anchor_H", double(given.H_thickness > 0) + 2. * (given.H_slope > 0) + 4. * (given.H_head > 0) +
                                                8. * (given.H_bed > 0) + 16. * (given.H_time > 0));
    f.terms.emplace_back("stat_oneshot_cpu_seconds", cpu0 >= 0. ? process_cpu_seconds() - cpu0 : -1.);
    f.terms.emplace_back("stat_oneshot_cpu_threads", double(threads));
    f.terms.emplace_back("stat_planar_support_s", support_time(g, m));
    std::vector<float> flat(g.nodes());
    for (size_t i = 0; i < g.dims[0]; ++i)
        for (size_t j = 0; j < g.dims[1]; ++j)
            for (size_t k = 0; k < g.dims[2]; ++k)
                flat[g.index(i, j, k)] = float(g.origin.z() + double(k) * g.spacing.z());
    if (m.anchor.size() == g.nodes())
        flat = m.anchor;
    ObjectiveMachine m_planar = m;
    m_planar.infill_combine   = 0.;
    f.terms.emplace_back("stat_planar_time_s", evaluate_objective(g, m_planar, W, flat, false).T);
    if (t.exact_native >= 0 && size_t(t.exact_native) < exact_tops.size())
        f.layer_tops = exact_tops[size_t(t.exact_native)];
    if (time_cut)
        for (TimeCutOffer &c : cuts) {
            NpField cf = f;
            cf.phi     = std::move(c.phi);
            make_z_monotone(cf);
            cf.layer_tops = c.terms.exact_native >= 0 && size_t(c.terms.exact_native) < exact_tops.size() ? exact_tops[size_t(c.terms.exact_native)] :
                                                                                                           std::vector<double>();
            cf.terms   = c.terms.named();
            cf.terms.emplace_back("stat_j_version", ObjectiveVersionNumber);
            cf.terms.emplace_back("stat_time_cut", 1.);
            cf.solver += " (time-cut native " + c.name + ")";
            time_cut->push_back(std::move(cf));
        }
    if (const char *local = research_switch("NP_ONESHOT_LEAN_LOCAL"); time_cut && local != nullptr && *local != 0 && std::string(local) != "0" && model.empty() &&
        params.only_offer.empty()) {
        const std::vector<std::vector<float>> leans = overhang_local_leans(g, m, f.phi, 6., { 1., 0.6 });
        for (size_t i = 0; i < leans.size(); ++i) {
            NpField lf = f;
            lf.phi     = leans[i];
            make_z_monotone(lf);
            const ObjectiveTerms lt = evaluate_objective(g, m, W, lf.phi, true);
            lf.layer_tops.clear();
            lf.terms  = lt.named();
            lf.terms.emplace_back("stat_lean_offer", 2.);
            lf.solver = "lean offer overhang-local " + std::string(i == 0 ? "1.0" : "0.6");
            BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": " << lf.solver << " J " << lt.J << " S " << lt.S
                                    << " E_up " << lt.E_up << " E_wall " << lt.E_wall << " E_down " << lt.E_down << " T " << lt.T << " H "
                                    << lt.H_thickness << "/" << lt.H_slope << "/" << lt.H_head << "/" << lt.H_bed << "/" << lt.H_time << ", spread " << lt.spread;
            time_cut->push_back(std::move(lf));
        }
    }
    auto offer_file = [&](const std::string &path, const std::string &label) {
        BoundingBoxf3 box;
        for (const stl_vertex &v : mesh.vertices)
            box.merge(v.cast<double>());
        NpField            original = load_npfield_for(path, box, plate_offset(object));
        const NpLayerField given_field(original);
        NpField            lf = f;
        for (size_t i = 0; i < g.dims[0]; ++i)
            for (size_t j = 0; j < g.dims[1]; ++j)
                for (size_t k = 0; k < g.dims[2]; ++k)
                    lf.phi[g.index(i, j, k)] = float(given_field.phi(g.origin + g.spacing.cwiseProduct(Vec3d(double(i), double(j), double(k)))));
        const ObjectiveTerms lt = evaluate_objective(g, m, W, lf.phi, true);
        lf.layer_tops.clear();
        lf.terms  = lt.named();
        lf.terms.emplace_back("stat_lean_offer", 1.);
        lf.solver = "lean offer " + label;
        BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": " << lf.solver << " J " << lt.J << " S " << lt.S
                                << " E_up " << lt.E_up << " E_wall " << lt.E_wall << " E_down " << lt.E_down << " T " << lt.T << " H "
                                << lt.H_thickness << "/" << lt.H_slope << "/" << lt.H_head << "/" << lt.H_bed << "/" << lt.H_time;
        original.terms  = std::move(lf.terms);
        original.solver = std::move(lf.solver);
        time_cut->push_back(std::move(original));
    };
    if (const char *lean = research_switch("NP_ONESHOT_LEAN_OFFER"); time_cut && lean != nullptr && *lean != 0 && model.empty() && params.only_offer.empty()) {
        std::stringstream paths(lean);
        for (std::string path; std::getline(paths, path, ',');) {
            try {
                offer_file(path, boost::filesystem::path(path).filename().string());
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object.model_object()->name << ": NP_ONESHOT_LEAN_OFFER " << path << " unused: " << ex.what();
            }
        }
    }
    if (time_cut && model.empty() && params.only_offer.empty())
        for (const auto &[arm, path] : arm_dir_files(object, ".npf.npz")) {
            if (boost::filesystem::exists(boost::filesystem::path(path).parent_path() /
                                          (boost::filesystem::path(object.model_object()->name).stem().string() + ".route.json")))
                continue;
            try {
                const std::string want = load_npfield(path).mesh_sha256, input = object.model_object()->input_file;
                if (!want.empty() && !input.empty() && want != file_sha256(input))
                    throw Slic3r::RuntimeError("its mesh_sha256 is not this object's input file's");
                offer_file(path, "arm " + arm + ", sha256 " + file_sha256(path));
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object.model_object()->name << ": arm " << arm << " unused: " << ex.what();
            }
        }
    if (const char *command = research_switch("NP_ONESHOT_FIELD_SOLVER"); time_cut && command != nullptr && *command != 0 && model.empty() &&
        params.only_offer.empty())
        if (const std::string path = external_field(object.model_object()->name, command); !path.empty()) {
            try {
                const std::string solver = load_npfield(path).solver;
                const size_t      open   = solver.rfind(" (");
                const std::string backend = open != std::string::npos && solver.back() == ')' ? solver.substr(open + 2, solver.size() - open - 3) : "cpu";
                offer_file(path, FieldSolverOffer + backend + ", sha256 " + file_sha256(path));
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object.model_object()->name << ": NP_ONESHOT_FIELD_SOLVER field unused: " << ex.what();
            }
        }
    if (const char *dir = research_switch("NP_ONESHOT_DUMP"); dir != nullptr && *dir != 0) {
        const OneShotState *st     = object.oneshot_state();
        const bool          anchor = st != nullptr && (st->measured_anchor || st->offer == OneShotState::Offer::Control);
        const std::string   stem   = (boost::filesystem::path(dir) / object.model_object()->name).string() + (anchor ? ".anchor" : "");
        save_objective_geometry(stem + ".geom.npz", g);
        save_npfield(stem + ".field.npz", f);
        if (const char *only = research_switch("NP_SOLVE_ONLY"); only != nullptr && *only != 0 && *only != '0') {
            BOOST_LOG_TRIVIAL(warning) << "One-shot field " << object.model_object()->name << ": solve only: J " << t.J << " S " << t.S << " E "
                                       << t.E << " T " << t.T << (t.feasible() ? " feasible" : " infeasible") << ", spread " << t.spread << ", "
                                       << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s, dumped "
                                       << stem << ".field.npz";
            throw Slic3r::SlicingError("NP_SOLVE_ONLY: the field is solved and dumped; no slice");
        }
    }
    BOOST_LOG_TRIVIAL(info) << "One-shot field " << object.model_object()->name << ": J " << t.J << ", " << g.nodes() << " nodes at "
                            << g.spacing.x() << " mm, " << (t.feasible() ? "feasible" : "infeasible") << ", "
                            << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s";
    return f;
}

namespace {

constexpr double RouteTimeCap = OneShotState::MaxTimeRatio / 1.02;
constexpr double RouteScreenBand = 1.05;
constexpr int    RouteMeasured   = 4;
constexpr std::array<int, 4> PlinthLadder { 8, 15, 25, 40 };
const std::vector<std::pair<std::string, std::string>> NoOverrides;
constexpr size_t RouteSubsets = 32;
const double     SeamCos = std::cos(20. * M_PI / 180.);
constexpr double SeamK   = 0.25;
constexpr double JoinRidgeMs  = 0.0108;
double join_line_k()
{
    static const double k = [] { const char *v = research_switch("NP_ONESHOT_JOIN_LINE"); return v != nullptr && *v != 0 ? std::atof(v) : 1.; }();
    return k;
}
bool arm_grade()
{
    static const bool on = [] { const char *v = research_switch("NP_ONESHOT_ARM_GRADE"); return v != nullptr && *v == '1'; }();
    return on;
}

bool force_joins(const std::string &cuts)
{
    static const char *force = research_switch("NP_ONESHOT_ROUTE_FORCE");
    static const bool  on    = [] { const char *v = research_switch("NP_ONESHOT_ROUTE_FORCE_JOINS"); return v != nullptr && std::atoi(v) != 0; }();
    return on && force != nullptr && cuts == force;
}
constexpr double SlabLayers   = 3.;
constexpr double TerminalFaceArea = 20.;
constexpr double JoinTurnMax = 20.;
constexpr double FanStepFallback = 10.;
constexpr double GradeAbove = 15.;
constexpr double FanReach = 23.;
constexpr double ChunkOverhangMax = 50.;
double chunk_overhang_max()
{
    static const double cap = [] {
        const char *v = research_switch("NP_ONESHOT_CHUNK_OVERHANG_MAX");
        return v != nullptr && *v != 0 ? std::max(0., std::atof(v)) : ChunkOverhangMax;
    }();
    return cap;
}
constexpr int FanMinLayers = 3;

bool on_clip(const std::array<Vec3d, 3> &t, const std::vector<HalfSpace> &clips)
{
    return std::any_of(clips.begin(), clips.end(), [&t](const HalfSpace &c) {
        return std::all_of(t.begin(), t.end(), [&c](const Vec3d &v) { return std::abs(c.normal.dot(v) - c.offset) < 1e-3; });
    });
}

double cut_perimeter(const indexed_triangle_set &mesh, const std::vector<HalfSpace> &clips, size_t base)
{
    const HalfSpace            &cut   = clips[base];
    const indexed_triangle_set  piece = clip_mesh(mesh, clips);
    const Vec3d                 n     = cut.normal.normalized();
    const Vec3d                 u     = (std::abs(n.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).cross(n).normalized(), v = n.cross(u);
    std::vector<Vec2d>          pts;
    for (const stl_vertex &p : piece.vertices)
        if (std::abs(cut.normal.dot(p.cast<double>()) - cut.offset) < 1e-3)
            pts.emplace_back(u.dot(p.cast<double>()), v.dot(p.cast<double>()));
    if (pts.size() < 3)
        return 0.;
    std::sort(pts.begin(), pts.end(), [](const Vec2d &a, const Vec2d &b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    std::vector<Vec2d> hull(2 * pts.size());
    size_t             k     = 0;
    auto               cross = [](const Vec2d &o, const Vec2d &a, const Vec2d &b) { return (a - o).x() * (b - o).y() - (a - o).y() * (b - o).x(); };
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0.)
            --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0.)
            --k;
        hull[k++] = pts[i - 1];
    }
    double length = 0.;
    for (size_t i = 1; i < k; ++i)
        length += (hull[i] - hull[i - 1]).norm();
    return length;
}

struct PieceTerms
{
    ObjectiveTerms t;
    double         seam = 0., overhang = 0.;
};
PieceTerms price_piece(const ObjectiveGeometry &g, const ObjectiveMachine &m, const ObjectiveWeights &W, const std::vector<float> &phi,
                       const Matrix3d &frame, double overhang_threshold)
{
    PieceTerms          out;
    std::vector<double> e;
    out.t = evaluate_objective(g, m, W, phi, false, nullptr, &e);
    const double er = m.h * std::sin(20. * M_PI / 180.) / std::sqrt(12.), cos_t = std::cos(overhang_threshold);
    for (size_t s = 0; s < g.p.size(); ++s) {
        const double nd = g.n[s].z();
        if (nd < -cos_t)
            out.overhang += g.a[s];
        if ((frame.transpose() * g.n[s]).z() >= SeamCos && nd < SeamCos)
            out.seam += W.up * g.a[s] * std::max(0., SeamK * SeamK * m.h * m.h - e[s] * e[s]) / (er * er);
    }
    return out;
}


bool face_slab(const indexed_triangle_set &mesh, const MultiplanarChunk &c, double h)
{
    double top = -std::numeric_limits<double>::max();
    for (const stl_vertex &v : clip_mesh(mesh, c.clips).vertices)
        top = std::max(top, c.build_dir.dot(v.cast<double>()));
    return top - c.base <= SlabLayers * h + 0.05;
}

bool slab_joins_exempt()
{
    const char *v = research_switch("NP_ONESHOT_SLAB_EXEMPT");
    return v != nullptr && *v == '1';
}

std::string grade_joins(MultiplanarPlan &plan, const indexed_triangle_set &mesh, const ToolKinematics &kinematics, double h, double fan_step)
{
    std::string log;
    for (size_t ci = 1; ci < plan.chunks.size(); ++ci) {
        const MultiplanarChunk &c    = plan.chunks[ci];
        const size_t            p    = size_t(c.parent);
        const Vec3d             bn   = -c.clips[size_t(c.base_clip)].normal;
        const double            bo   = -c.clips[size_t(c.base_clip)].offset;
        const Vec3d             dp   = plan.chunks[p].build_dir;
        const double            turn = std::acos(std::clamp(dp.dot(bn), -1., 1.));
        const int               steps = int(std::ceil(Geometry::rad2deg(turn) / fan_step - 1e-9));
        Vec3d                   axis  = dp.cross(bn);
        if (Geometry::rad2deg(turn) <= GradeAbove || steps < 2 || axis.norm() < 1e-9 || face_slab(mesh, c, h))
            continue;
        axis.normalize();
        const Vec3d                u     = axis.cross(bn);
        const double               sigma = dp.dot(u) >= 0. ? 1. : -1.;
        const indexed_triangle_set piece = clip_mesh(mesh, c.clips);
        Vec3d                      hinge = Vec3d::Zero(), centre = Vec3d::Zero();
        double                     lowest = std::numeric_limits<double>::max();
        size_t                     on_cut = 0;
        for (const stl_vertex &v : piece.vertices)
            if (const Vec3d x = v.cast<double>(); std::abs(bn.dot(x) - bo) < 1e-3) {
                centre += x, ++on_cut;
                if (sigma * u.dot(x) < lowest)
                    lowest = sigma * u.dot(x), hinge = x;
            }
        if (on_cut == 0)
            continue;
        centre /= double(on_cut);
        const Vec3d edge = hinge;
        auto make_fan = [&](int n) {
            std::vector<MultiplanarChunk> fan;
            for (int k = 1; k < n; ++k) {
                const double t = double(k) / double(n);
                const Vec3d  m = ((std::sin((1. - t) * turn) * dp + std::sin(t * turn) * bn) / std::sin(turn)).normalized();
                JointPose    joints;
                if (!kinematics.inverse(ToolPose { centre, m }, nullptr, joints))
                    return std::vector<MultiplanarChunk>();
                MultiplanarChunk w;
                w.build_dir = m;
                w.base      = m.dot(hinge);
                w.frame     = chunk_frame(kinematics, joints, m);
                fan.push_back(std::move(w));
            }
            for (size_t k = 0; k < fan.size(); ++k) {
                MultiplanarChunk &w = fan[k];
                w.clips             = plan.chunks[p].clips;
                w.base_clip         = int(w.clips.size());
                w.clips.push_back({ -w.build_dir, -w.base });
                if (k + 1 < fan.size())
                    w.clips.push_back({ fan[k + 1].build_dir, fan[k + 1].base });
                w.parent = k == 0 ? int(p) : int(ci + k - 1);
            }
            return fan;
        };
        std::string tried;
        auto prints = [&](const std::vector<MultiplanarChunk> &fan) {
            MultiplanarChunk rest = plan.chunks[p];
            rest.clips.push_back({ fan.front().build_dir, fan.front().base });
            const int rest_layers = printable_layers(chunk_piece(mesh, rest, h), rest, h);
            tried += Slic3r::format(" n=%1%: parent %2%, wedges", fan.size() + 1, rest_layers);
            bool all = rest_layers >= FanMinLayers;
            for (const MultiplanarChunk &w : fan) {
                const int layers = printable_layers(chunk_piece(mesh, w, h), w, h);
                tried += Slic3r::format(" %1%", layers);
                all = all && layers >= FanMinLayers;
            }
            return all;
        };
        std::vector<MultiplanarChunk> fan;
        int                           n  = steps;
        bool                          ok = false;
        double                        reach = FanReach;
        const std::vector<double> reaches = chunk_support_requested() ? std::vector<double>{ FanReach, 0.5 * FanReach, 0.25 * FanReach }
                                                                      : std::vector<double>{ FanReach };
        for (const double r : reaches) {
            reach = r;
            hinge = edge - sigma * r * u;
            tried += Slic3r::format(" reach %1$.0f:", r);
            n = steps;
            for (const int least = std::max(2, int(std::ceil(Geometry::rad2deg(turn) / JoinTurnMax - 1e-9))); n >= least && !ok; --n)
                if (fan = make_fan(n); fan.empty() || (ok = prints(fan)))
                    break;
            if (ok || fan.empty())
                break;
        }
        if (!ok) {
            log += Slic3r::format("join of chunk %1% (%2$.0f deg): %3%%4%; ", ci, Geometry::rad2deg(turn),
                                  fan.empty() ? "a wedge out of the axes' reach" : "no fan whose wedges all print", tried.empty() ? "" : " (" + tried.substr(1) + ")");
            continue;
        }
        plan.chunks[p].clips.push_back({ fan.front().build_dir, fan.front().base });
        const int shift = int(fan.size());
        for (MultiplanarChunk &other : plan.chunks)
            if (other.parent >= int(ci))
                other.parent += shift;
        plan.chunks[ci].parent = int(ci + fan.size() - 1);
        log += Slic3r::format("join of chunk %1% graded: %2$.0f deg in %3% steps, hinge %4$.2f,%5$.2f,%6$.2f (reach %7$.0f); ", ci, Geometry::rad2deg(turn), n,
                              hinge.x(), hinge.y(), hinge.z(), reach);
        plan.chunks.insert(plan.chunks.begin() + ptrdiff_t(ci), std::make_move_iterator(fan.begin()), std::make_move_iterator(fan.end()));
        ci += fan.size();
    }
    return log;
}

struct SpecCut
{
    int    parent = -1;
    Vec3d  n, dir;
    double o = 0.;
    double jvis = 0.;
};

MultiplanarPlan spec_cut_plan(const std::vector<SpecCut> &cuts, const indexed_triangle_set &mesh, const ToolKinematics &kinematics)
{
    MultiplanarPlan      plan;
    std::vector<int>     ids { -1 };
    std::vector<std::vector<HalfSpace>> clips { {} };
    for (const SpecCut &c : cuts) {
        const int    q   = c.parent;
        const Vec3d  n   = c.n.normalized();
        const double o   = c.o;
        const Vec3d  dir = c.dir.normalized();
        const auto   at  = std::find(ids.begin(), ids.end(), q);
        if (at == ids.end())
            throw Slic3r::RuntimeError("NP_ONESHOT_ROUTE_SPEC: a cut's parent is not an earlier cut");
        const size_t   p = size_t(at - ids.begin());
        MultiplanarCut cut;
        cut.parent          = int(p);
        cut.parent_clip     = { n, o };
        cut.chunk.parent    = int(p);
        cut.chunk.build_dir = dir;
        cut.chunk.clips     = { { -n, -o } };
        cut.chunk.base_clip = 0;
        std::vector<HalfSpace> own = clips[p];
        own.push_back({ -n, -o });
        BoundingBoxf3 box;
        cut.chunk.base = std::numeric_limits<double>::infinity();
        for (const stl_vertex &v : clip_mesh(mesh, own).vertices) {
            box.merge(v.cast<double>());
            cut.chunk.base = std::min(cut.chunk.base, dir.dot(v.cast<double>()));
        }
        if (!box.defined)
            throw Slic3r::RuntimeError("NP_ONESHOT_ROUTE_SPEC: a cut leaves its chunk empty");
        JointPose joints;
        if (kinematics.inverse(ToolPose { box.center(), dir }, nullptr, joints))
            cut.chunk.frame = chunk_frame(kinematics, joints, dir);
        clips[p].push_back({ n, o });
        clips.insert(clips.begin() + ptrdiff_t(p) + 1, std::move(own));
        ids.insert(ids.begin() + ptrdiff_t(p) + 1, int(plan.cuts.size()));
        plan.cuts.push_back(std::move(cut));
    }
    return multiplanar_subset(plan, std::vector<bool>(plan.cuts.size(), true));
}

MultiplanarPlan route_spec_plan(const std::string &path, const indexed_triangle_set &mesh, const ToolKinematics &kinematics)
{
    const nlohmann::json spec = nlohmann::json::parse(std::ifstream(path));
    auto                 vec  = [](const nlohmann::json &a) { return Vec3d(a.at(0).get<double>(), a.at(1).get<double>(), a.at(2).get<double>()); };
    std::vector<SpecCut> cuts;
    for (const nlohmann::json &c : spec.at("cuts")) {
        SpecCut cut;
        cut.parent = c.value("parent", -1);
        cut.n      = vec(c.at("n"));
        cut.o      = c.at("o").get<double>();
        cut.dir    = c.contains("dir") ? vec(c.at("dir")) : cut.n;
        cuts.push_back(cut);
    }
    return spec_cut_plan(cuts, mesh, kinematics);
}


constexpr double LipMinZ = 12., LipAccept = 0.7, LipMinPlanar = 50., LipStep = 0.25;
constexpr double LipTilts[] = { 30., 35., 40., 45., 55. };
constexpr size_t LipPerTilt = 2;
constexpr int LipMeasured = 4;
constexpr double LipRegionBend = 15., LipRegionMin = 50., LipJvisMax = 60., LipJvisWeight = 2.;
struct LipFaces
{
    std::vector<Vec3d>                c, nrm;
    std::vector<std::array<Vec3d, 3>> tri;
    std::vector<double>               area, region_area;
};
LipFaces lip_faces(const indexed_triangle_set &mesh)
{
    LipFaces          f;
    std::vector<int>  index(mesh.indices.size(), -1);
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        const stl_triangle_vertex_indices &t = mesh.indices[i];
        const Vec3d  t0 = mesh.vertices[t[0]].cast<double>(), t1 = mesh.vertices[t[1]].cast<double>(), t2 = mesh.vertices[t[2]].cast<double>();
        const Vec3d  cr = (t1 - t0).cross(t2 - t0);
        const double a  = 0.5 * cr.norm();
        if (a <= 0.)
            continue;
        index[i] = int(f.c.size());
        f.c.push_back((t0 + t1 + t2) / 3.), f.nrm.push_back(cr.normalized()), f.area.push_back(a), f.tri.push_back({ t0, t1, t2 });
    }
    std::vector<int> parent(f.c.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> root = [&](int x) { return parent[x] == x ? x : parent[x] = root(parent[x]); };
    std::map<std::pair<int, int>, int> edges;
    const double cos_bend = std::cos(LipRegionBend * M_PI / 180.);
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        if (index[i] < 0)
            continue;
        for (int e = 0; e < 3; ++e) {
            const int a = mesh.indices[i][e], b = mesh.indices[i][(e + 1) % 3];
            const auto key = std::minmax(a, b);
            if (auto it = edges.find(key); it == edges.end())
                edges.emplace(key, index[i]);
            else if (f.nrm[size_t(it->second)].dot(f.nrm[size_t(index[i])]) > cos_bend)
                parent[size_t(root(it->second))] = root(index[i]);
        }
    }
    std::map<int, double> sum;
    for (size_t i = 0; i < f.c.size(); ++i)
        sum[root(int(i))] += f.area[i];
    f.region_area.resize(f.c.size());
    for (size_t i = 0; i < f.c.size(); ++i) {
        const double A = sum[root(int(i))];
        const bool   bed = f.nrm[i].z() < -0.99 && f.c[i].z() < 0.3;
        f.region_area[i] = A >= LipRegionMin && !bed ? A : 0.;
    }
    return f;
}
double lip_join_weight(const Vec3d &m, const Vec3d &d, double region_area)
{
    const Vec3d  tz = m.cross(Vec3d::UnitZ()), td = m.cross(d);
    const double theta = tz.norm() < 0.1 ? 90. :
                         Geometry::rad2deg(std::acos(std::clamp(std::abs(tz.dot(td)) / std::max(tz.norm() * td.norm(), 1e-12), 0., 1.)));
    return std::min(1., theta / 10.) * std::min(1., region_area / 1000.);
}
double lip_jvis(const indexed_triangle_set &mesh, const LipFaces &f, const Vec3d &d, double o)
{
    double sum = 0.;
    size_t k   = 0;
    for (const stl_triangle_vertex_indices &t : mesh.indices) {
        const Vec3d p[3] = { mesh.vertices[t[0]].cast<double>(), mesh.vertices[t[1]].cast<double>(), mesh.vertices[t[2]].cast<double>() };
        if (0.5 * (p[1] - p[0]).cross(p[2] - p[0]).norm() <= 0.)
            continue;
        const size_t i = k++;
        if (f.region_area[i] <= 0.)
            continue;
        std::vector<Vec3d> hits;
        for (int e = 0; e < 3; ++e) {
            const double s0 = d.dot(p[e]) - o, s1 = d.dot(p[(e + 1) % 3]) - o;
            if ((s0 < 0.) != (s1 < 0.))
                hits.push_back(p[e] + (p[(e + 1) % 3] - p[e]) * (s0 / (s0 - s1)));
        }
        if (hits.size() == 2)
            sum += (hits[1] - hits[0]).norm() * lip_join_weight(f.nrm[i], d, f.region_area[i]);
    }
    return sum;
}

std::vector<std::pair<double, SpecCut>> lip_offer_cuts(const indexed_triangle_set &mesh)
{
    std::vector<Vec3d>  c, nrm, vtx;
    std::vector<double> area;
    for (const stl_triangle_vertex_indices &f : mesh.indices) {
        const Vec3d  t0 = mesh.vertices[f[0]].cast<double>(), t1 = mesh.vertices[f[1]].cast<double>(), t2 = mesh.vertices[f[2]].cast<double>();
        const Vec3d  cr = (t1 - t0).cross(t2 - t0);
        const double a  = 0.5 * cr.norm();
        if (a <= 0.)
            continue;
        c.push_back((t0 + t1 + t2) / 3.), nrm.push_back(cr.normalized()), area.push_back(a);
    }
    for (const stl_vertex &v : mesh.vertices)
        vtx.push_back(v.cast<double>());
    double planar = 0.;
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i].z() >= 0.3 && nrm[i].z() < -0.707)
            planar += area[i];
    std::vector<std::pair<double, SpecCut>> out;
    if (planar < LipMinPlanar)
        return out;
    constexpr double     LipJvisSample = 0.05;
    const LipFaces       faces = lip_faces(mesh);
    std::vector<Vec3d>   sp;
    std::vector<double>  sw;
    std::vector<size_t>  sf;
    for (size_t i = 0; i < faces.c.size(); ++i) {
        if (faces.region_area[i] <= 0.)
            continue;
        const int                   n = std::max(1, int(std::ceil(std::sqrt(faces.area[i] / LipJvisSample))));
        const std::array<Vec3d, 3> &T = faces.tri[i];
        const size_t                first = sp.size();
        for (int u = 0; u < n; ++u)
            for (int v = 0; v < n - u; ++v) {
                const double a = (u + 1. / 3.) / n, b = (v + 1. / 3.) / n;
                sp.push_back(T[0] + (T[1] - T[0]) * a + (T[2] - T[0]) * b);
                sf.push_back(i);
            }
        sw.insert(sw.end(), sp.size() - first, faces.area[i] / double(sp.size() - first));
    }
    for (const double t : LipTilts) {
        std::vector<std::tuple<double, double, SpecCut>> picks;
        for (int az = 0; az < 360; az += 15) {
            const double tr = t * M_PI / 180., ar = az * M_PI / 180.;
            const Vec3d  d(std::sin(tr) * std::cos(ar), std::sin(tr) * std::sin(ar), std::cos(tr));
            std::vector<size_t> fi(c.size()), vi(vtx.size());
            std::iota(fi.begin(), fi.end(), 0), std::iota(vi.begin(), vi.end(), 0);
            std::sort(fi.begin(), fi.end(), [&](size_t a, size_t b) { return c[a].dot(d) < c[b].dot(d); });
            std::sort(vi.begin(), vi.end(), [&](size_t a, size_t b) { return vtx[a].dot(d) > vtx[b].dot(d); });
            std::vector<double> chunk(fi.size() + 1, 0.), root(fi.size() + 1, 0.), chunk_area(fi.size() + 1, 0.);
            for (size_t k = fi.size(); k-- > 0;) {
                const size_t i = fi[k];
                chunk[k]      = chunk[k + 1] + (c[i].z() >= 0.3 && nrm[i].dot(d) < -0.707 ? area[i] : 0.);
                chunk_area[k] = chunk_area[k + 1] + area[i];
            }
            for (size_t k = 0; k < fi.size(); ++k) {
                const size_t i = fi[k];
                root[k + 1] = root[k] + (c[i].z() >= 0.3 && nrm[i].z() < -0.707 ? area[i] : 0.);
            }
            const double lo = std::floor(vtx[vi.back()].dot(d) / LipStep) * LipStep, hi = vtx[vi.front()].dot(d);
            double minz = std::numeric_limits<double>::infinity();
            size_t kv   = 0;
            std::vector<double> os;
            for (double o = lo; o < hi; o += LipStep)
                os.push_back(o);
            std::vector<double> sc(os.size(), std::numeric_limits<double>::infinity()), frac(os.size(), 1.);
            for (size_t j = os.size(); j-- > 0;) {
                while (kv < vi.size() && vtx[vi[kv]].dot(d) >= os[j])
                    minz = std::min(minz, vtx[vi[kv++]].z());
                if (kv == 0 || minz < LipMinZ)
                    continue;
                const size_t k = size_t(std::lower_bound(fi.begin(), fi.end(), os[j], [&](size_t i, double o) { return c[i].dot(d) < o; }) - fi.begin());
                if (k == fi.size())
                    continue;
                sc[j]   = chunk[k] + root[k];
                frac[j] = chunk_area[k] / chunk_area[0];
            }
            std::vector<double> jvis(os.size(), 0.);
            std::vector<double> face_w(faces.c.size(), -1.);
            for (size_t q = 0; q < sp.size(); ++q) {
                const size_t i = sf[q];
                if (face_w[i] < 0.) {
                    const double g = std::sqrt(std::max(0., 1. - std::pow(faces.nrm[i].dot(d), 2)));
                    face_w[i]      = g * lip_join_weight(faces.nrm[i], d, faces.region_area[i]) / LipStep;
                }
                const long b = std::lround((sp[q].dot(d) - lo) / LipStep);
                if (b >= 0 && b < long(os.size()))
                    jvis[size_t(b)] += sw[q] * face_w[i];
            }
            std::vector<double> obj(os.size(), std::numeric_limits<double>::infinity());
            for (size_t j = 0; j < os.size(); ++j)
                if (std::isfinite(sc[j]) && sc[j] <= LipAccept * planar)
                    obj[j] = sc[j] + LipJvisWeight * jvis[j];
            const double low = *std::min_element(obj.begin(), obj.end());
            if (!std::isfinite(low))
                continue;
            std::vector<size_t> tied;
            for (size_t j = 0; j < obj.size(); ++j)
                if (obj[j] <= low + 0.5)
                    tied.push_back(j);
            const size_t j = tied[tied.size() / 2];
            SpecCut      cut;
            cut.n = cut.dir = d, cut.o = os[j];
            picks.emplace_back(low, frac[j], cut);
        }
        std::sort(picks.begin(), picks.end(), [](const auto &a, const auto &b) {
            return std::get<0>(a) < std::get<0>(b) || (std::get<0>(a) == std::get<0>(b) && std::get<1>(a) < std::get<1>(b));
        });
        size_t taken = 0;
        for (size_t k = 0; k < picks.size() && taken < LipPerTilt; ++k) {
            SpecCut cut = std::get<2>(picks[k]);
            cut.jvis    = lip_jvis(mesh, faces, cut.dir, cut.o);
            if (cut.jvis <= LipJvisMax)
                out.emplace_back(t, cut), ++taken;
        }
    }
    return out;
}

bool lip_offers_requested()
{
    static const bool on = [] { const char *v = research_switch("NP_ONESHOT_LIP_OFFER"); return v != nullptr && std::atoi(v) != 0; }();
    return on;
}
}

namespace {

bool choose_route_once(PrintObject &object, OneShotState &state, double fan_step)
{
    bool cap_refused = false;
    const MeasureDeadline deadline(measure_deadline(state));
    const MeasureWork     work(measure_work(object, state));
    const PrintConfig                    &pc         = object.print()->config();
    const std::unique_ptr<ToolKinematics> kinematics = make_tool_kinematics(pc);
    state.plinth_layers = 0;
    if (!state.z_only || !object.config().nonplanar_oneshot_routes.value || kinematics->rotary_count() == 0 || pc.spiral_mode.value)
        return cap_refused;
    const auto                 t_start = std::chrono::steady_clock::now();
    const std::string         &name    = object.model_object()->name;
    const indexed_triangle_set mesh    = slicing_frame_mesh(object);
    MultiplanarParams          params  = multiplanar_params(object, kinematics.get());
    params.work_budget = oneshot_planner_cuts();
    const bool                 chunk_support = chunk_support_requested() && object.config().enable_support.value;
    const char                *spec    = research_switch("NP_ONESHOT_ROUTE_SPEC");
    const MultiplanarPlan      plan    = spec != nullptr && *spec != 0 ? route_spec_plan(spec, mesh, *kinematics)
                                                                       : object_multiplanar_plan(object, mesh, params);
    if (plan.work_stopped)
        BOOST_LOG_TRIVIAL(warning) << "One-shot routes " << name << ": the chunk planner stopped at its work budget (" << params.work_budget
                                   << " validated cuts) with " << plan.cuts.size() << " cuts";
    std::ostringstream         log;
    const std::vector<std::pair<double, SpecCut>> lips = lip_offers_requested() && (spec == nullptr || *spec == 0) ? lip_offer_cuts(mesh) :
                                                                                                                      std::vector<std::pair<double, SpecCut>>();
    const std::vector<std::pair<std::string, std::string>> arms = arm_dir_files(object, ".route.json");
    for (size_t k = 0; k < lips.size(); ++k)
        log << Slic3r::format("lip cut L%1%: tilt %2$.0f, dir %3$.3f,%4$.3f,%5$.3f @ %6$.2f, J_vis %7$.1f; ", k, lips[k].first, lips[k].second.dir.x(),
                              lips[k].second.dir.y(), lips[k].second.dir.z(), lips[k].second.o, lips[k].second.jvis);
    if (plan.cuts.empty() && lips.empty() && arms.empty()) {
        state.route_log = plan.work_stopped ? "routes: no tilted chunk within the planner's work budget" : "routes: no tilted chunk";
        return cap_refused;
    }
    const PrintObjectConfig &oc = object.config();
    const PrintRegionConfig &rc = object.printing_region(0).config();
    const double             h  = oc.layer_height.value;
    const double             lw = oc.line_width.get_abs_value(pc.nozzle_diameter.get_at(0));
    const ObjectiveWeights   W  = machine_weights(object);
    const double             J0 = solved_term(state, "J"), T0 = solved_term(state, "J_T");
    const NpField      &body = *state.solved;
    std::vector<double> zk(body.dims[2]), pk(body.dims[2]);
    for (size_t k = 0; k < body.dims[2]; ++k) {
        double sum = 0.;
        for (size_t i = 0; i < body.dims[0]; ++i)
            for (size_t j = 0; j < body.dims[1]; ++j)
                sum += body.phi[body.index(i, j, k)];
        zk[k] = body.node(0, 0, k).z(), pk[k] = sum / double(body.dims[0] * body.dims[1]);
    }
    auto body_phi = [&](double z) {
        const size_t i = std::clamp(size_t(std::upper_bound(zk.begin(), zk.end(), z) - zk.begin()), size_t(1), zk.size() - 1);
        return pk[i - 1] + (pk[i] - pk[i - 1]) * (z - zk[i - 1]) / std::max(zk[i] - zk[i - 1], 1e-12);
    };
    auto clips_key = [](const std::vector<HalfSpace> &clips) {
        std::string key;
        for (const HalfSpace &c : clips)
            key.append(reinterpret_cast<const char *>(&c), sizeof(HalfSpace)).push_back('|');
        return key;
    };
    std::map<std::string, PieceTerms>  priced;
    std::map<std::string, std::string> head_hits;
    const bool plinth_allowed = plinth_requested() && oc.raft_layers.value == 0;
    std::map<std::string, MultiplanarParams> plinth_params;
    auto lifted = [&](int layers, const std::vector<HalfSpace> &root) -> const MultiplanarParams & {
        const std::string key = std::to_string(layers) + "#" + clips_key(root);
        auto it = plinth_params.find(key);
        if (it == plinth_params.end()) {
            Plinth plinth = plinth_for_raft(object, clip_mesh(mesh, root), layers);
            it = plinth_params.emplace(key, params).first;
            it->second.plate_z = plinth.plate_z;
            it->second.plinth  = std::move(plinth.outline);
        }
        return it->second;
    };
    auto price = [&](const MultiplanarChunk &c, bool root) -> const PieceTerms & {
        const std::string key = std::string(root ? "r" : "c") + clips_key(c.clips);
        if (auto it = priced.find(key); it != priced.end())
            return it->second;
        throw_if_canceled(object);
        const Matrix3d         frame = root ? Matrix3d::Identity() : c.frame;
        indexed_triangle_set   piece = clip_mesh(mesh, c.clips);
        std::vector<HalfSpace> clips = c.clips;
        if (!root) {
            for (stl_vertex &v : piece.vertices)
                v = (frame * v.cast<double>() - Vec3d(0., 0., c.base)).cast<float>();
            for (HalfSpace &cl : clips) {
                cl.normal = frame * cl.normal;
                cl.offset -= cl.normal.z() * c.base;
            }
        }
        const ObjectiveGeometry g = objective_geometry(piece, rc.wall_loops.value, rc.top_shell_layers.value, lw, h,
                                                       [&clips](const std::array<Vec3d, 3> &t) { return on_clip(t, clips); });
        if (g.p.empty())
            return priced[key] = PieceTerms();
        ObjectiveMachine   m = objective_machine(object, g, 0.);
        std::vector<float> phi(g.nodes());
        if (!root) {
            m.first_layer = h;
            m.anchor.clear();
            m.pinned_v.clear();
        }
        for (size_t i = 0; i < g.dims[0]; ++i)
            for (size_t j = 0; j < g.dims[1]; ++j)
                for (size_t k = 0; k < g.dims[2]; ++k) {
                    const double z = g.origin.z() + double(k) * g.spacing.z();
                    phi[g.index(i, j, k)] = float(root ? body_phi(z) : z);
                }
        return priced[key] = price_piece(g, m, W, phi, frame, params.overhang_threshold);
    };
    struct Offer
    {
        std::string     cuts;
        MultiplanarPlan plan;
        double          J = 0., T = 0., seam = 0., T_motion = 0.;
        double          J_ceil = 0.;
        std::string     why;
        std::string     pieces;
        std::shared_ptr<const NpField> root;
        double t_measured = std::numeric_limits<double>::quiet_NaN(), J_measured = std::numeric_limits<double>::quiet_NaN();
        int         plinth = 0;
        std::string bed_hit;
        bool        arm = false;
        std::string root_phi;
    };
    const size_t             K       = plan.cuts.size();
    const std::vector<int>   parents = multiplanar_cut_parents(plan);
    std::vector<std::vector<bool>> masks;
    auto closed = [&](const std::vector<bool> &keep) {
        for (size_t i = 0; i < K; ++i)
            if (keep[i] && parents[i] >= 0 && !keep[size_t(parents[i])])
                return false;
        return std::find(keep.begin(), keep.end(), true) != keep.end();
    };
    if (K < 16)
        for (uint32_t bits = 1; bits < (1u << K) && masks.size() <= RouteSubsets; ++bits) {
            std::vector<bool> keep(K);
            for (size_t i = 0; i < K; ++i)
                keep[i] = (bits >> i) & 1;
            if (closed(keep))
                masks.push_back(std::move(keep));
        }
    if (masks.empty() || masks.size() > RouteSubsets) {
        masks.clear();
        for (size_t k = 1; k <= K; ++k) {
            std::vector<bool> prefix(K, false);
            std::fill(prefix.begin(), prefix.begin() + k, true);
            for (size_t drop = 0; drop <= k; ++drop) {
                std::vector<bool> keep = prefix;
                if (drop < k)
                    keep[drop] = false;
                if (closed(keep) && std::find(masks.begin(), masks.end(), keep) == masks.end())
                    masks.push_back(std::move(keep));
            }
        }
    }
    struct Variant
    {
        size_t          cut;
        MultiplanarPlan plan;
        std::string     tag;
        std::vector<size_t> global;
    };
    std::vector<Variant> variants;
    {
        for (size_t i = 0; i < K; ++i) {
            const MultiplanarChunk &c     = plan.cuts[i].chunk;
            const HalfSpace         base  = c.clips[size_t(c.base_clip)];
            const indexed_triangle_set piece = clip_mesh(mesh, c.clips);
            std::map<std::array<long, 3>, std::pair<double, Vec3d>> clusters;
            std::map<std::array<long, 3>, Vec3d>                    centroids;
            Vec3d  centre = Vec3d::Zero();
            size_t on_cut = 0;
            for (const stl_vertex &v : piece.vertices)
                if (std::abs(base.normal.dot(v.cast<double>()) - base.offset) < 1e-3)
                    centre += v.cast<double>(), ++on_cut;
            for (const stl_triangle_vertex_indices &f : piece.indices) {
                const std::array<Vec3d, 3> t { piece.vertices[f[0]].cast<double>(), piece.vertices[f[1]].cast<double>(),
                                               piece.vertices[f[2]].cast<double>() };
                if (on_clip(t, c.clips))
                    continue;
                const Vec3d  cr = (t[1] - t[0]).cross(t[2] - t[0]);
                const double a  = 0.5 * cr.norm();
                if (a <= 0.)
                    continue;
                const Vec3d n = cr.normalized();
                const std::array<long, 3> key { std::lround(n.x() * 50.), std::lround(n.y() * 50.), std::lround(n.z() * 50.) };
                auto                     &e = clusters[key];
                e.first += a;
                e.second += a * n;
                auto &g = centroids.try_emplace(key, Vec3d::Zero()).first->second;
                g += a * (t[0] + t[1] + t[2]) / 3.;
            }
            double best = 0.;
            Vec3d  face = Vec3d::Zero();
            for (const auto &[key, value] : clusters)
                if (value.first > best)
                    best = value.first, face = value.second.normalized();
            {
                std::vector<int> ids { -1 };
                for (size_t j = 0; j < K; ++j)
                    ids.insert(ids.begin() + plan.cuts[j].parent + 1, int(j));
                const int    p     = int(std::find(ids.begin(), ids.end(), int(i)) - ids.begin());
                const double depth = SlabLayers * h;
                std::vector<std::pair<MultiplanarCut, bool>> slabs;
                for (const auto &[key, value] : clusters) {
                    const Vec3d  n  = value.second.normalized();
                    const double nd = std::abs(n.dot(c.build_dir));
                    if (value.first < TerminalFaceArea || nd < 0.1 || nd > std::cos(5. * M_PI / 180.))
                        continue;
                    const Vec3d g = centroids.at(key) / value.first;
                    JointPose   slab_joints;
                    if (!kinematics->inverse(ToolPose { g, n }, nullptr, slab_joints))
                        continue;
                    const double   o = n.dot(g) - depth;
                    double plane_area = 0.;
                    for (const stl_triangle_vertex_indices &f : mesh.indices) {
                        const Vec3d  t0 = mesh.vertices[f[0]].cast<double>(), t1 = mesh.vertices[f[1]].cast<double>(),
                                     t2 = mesh.vertices[f[2]].cast<double>();
                        const Vec3d  cr = (t1 - t0).cross(t2 - t0);
                        const double a  = 0.5 * cr.norm();
                        if (a > 0. && cr.normalized().dot(n) > std::cos(2. * M_PI / 180.) &&
                            std::abs(n.dot((t0 + t1 + t2) / 3.) - n.dot(g)) < 0.05)
                            plane_area += a;
                    }
                    MultiplanarCut slab;
                    slab.parent           = p;
                    slab.parent_clip      = { n, o };
                    slab.chunk.parent     = p;
                    slab.chunk.build_dir  = n;
                    slab.chunk.base       = o;
                    slab.chunk.clips      = { { -n, -o } };
                    slab.chunk.base_clip  = 0;
                    slab.chunk.frame      = chunk_frame(*kinematics, slab_joints, n);
                    slabs.emplace_back(std::move(slab), plane_area > value.first + 1.);
                    log << Slic3r::format("face slab on cut %1%: %2$.0f mm2 at %3$.3f,%4$.3f,%5$.3f%6%; ", i, value.first, n.x(), n.y(), n.z(),
                                          slabs.back().second ? " (whole plane)" : "");
                }
                const uint32_t all = (1u << slabs.size()) - 1;
                for (uint32_t bits = all; bits > 0 && (bits == all || slabs.size() <= 3); --bits) {
                    Variant v { i, plan, "s", {} };
                    for (size_t k = 0; k < slabs.size(); ++k) {
                        if (!((bits >> k) & 1))
                            continue;
                        if (bits != all)
                            v.tag += std::to_string(k);
                        if (slabs[k].second)
                            v.global.push_back(v.plan.cuts.size());
                        v.plan.cuts.push_back(slabs[k].first);
                    }
                    variants.push_back(std::move(v));
                }
            }
            if (on_cut == 0 || best < TerminalFaceArea || std::abs(face.dot(c.build_dir)) > std::cos(5. * M_PI / 180.))
                continue;
            centre /= double(on_cut);
            Vec3d          dir  = face;
            const double   az   = std::atan2(face.y(), face.x());
            const double   tilt = std::acos(std::clamp(face.z(), -1., 1.));
            const double   reach = kinematics->max_tilt(az) - M_PI / 180.;
            if (tilt > reach)
                dir = Vec3d(std::sin(reach) * std::cos(az), std::sin(reach) * std::sin(az), std::cos(reach));
            JointPose joints;
            if (std::abs(dir.dot(c.build_dir)) > std::cos(5. * M_PI / 180.) || !kinematics->inverse(ToolPose { centre, dir }, nullptr, joints))
                continue;
            Variant v { i, plan, "a", {} };
            MultiplanarCut &cut = v.plan.cuts[i];
            const double    o   = dir.dot(centre);
            cut.parent_clip     = { dir, o };
            cut.chunk.build_dir = dir;
            cut.chunk.base      = o;
            cut.chunk.clips[size_t(cut.chunk.base_clip)] = { -dir, -o };
            cut.chunk.frame = chunk_frame(*kinematics, joints, dir);
            log << Slic3r::format("terminal face of cut %1%: %2$.0f mm2 at %3$.3f,%4$.3f,%5$.3f, layers at %6$.0f deg off it; ", i, best, face.x(),
                                  face.y(), face.z(), Geometry::rad2deg(std::acos(std::clamp(dir.dot(face), -1., 1.))));
            variants.push_back(std::move(v));
        }
    }
    size_t made = 0;
    for (const Variant &v : variants)
        made += v.tag == "a" ? 1 : v.plan.cuts.size() - K;
    log << "terminal-face offers made: " << made << "; ";
    std::vector<Offer> offers;
    auto finish = [&](Offer o) {
        const bool chunk_supported = chunk_support || (o.plan.lip && object.config().enable_support.value);
        if (o.why.empty() && !force_joins(o.cuts) && (!o.arm || arm_grade()))
            if (const std::string graded = grade_joins(o.plan, mesh, *kinematics, h, fan_step); !graded.empty()) {
                if (!o.arm)
                    o.cuts += "g";
                o.pieces += " (" + graded + ")";
            }
        if (!o.why.empty()) {
            offers.push_back(std::move(o));
            return;
        }
        for (size_t ci = 0; ci < o.plan.chunks.size() && o.why.empty(); ++ci) {
            const MultiplanarChunk &c  = o.plan.chunks[ci];
            const PieceTerms       &pt = price(c, ci == 0);
            o.J += ci == 0 || chunk_supported ? pt.t.J + pt.seam : pt.t.J - W.sup * pt.t.S + pt.seam;
            o.J_ceil += pt.t.J + pt.seam;
            o.pieces += Slic3r::format(" [%1%: S %2$.0f over %3$.0f E_up %4$.0f E_wall %5$.0f E_down %6$.0f T %7$.0f]", ci, pt.t.S, pt.overhang,
                                       pt.t.E_up, pt.t.E_wall, pt.t.E_down, pt.t.T);
            o.T += pt.t.T;
            o.seam += pt.seam;
            if (ci == 0)
                continue;
            if (pt.overhang > chunk_overhang_max() && !chunk_supported && !o.arm) {
                o.why = Slic3r::format("chunk %1%: %2$.0f mm2 overhangs in its own layers (> %3$.0f), and a chunk prints no support", ci,
                                       pt.overhang, chunk_overhang_max());
                cap_refused = cap_refused || o.cuts.back() == 'g';
                break;
            }
            if (const double turn = Geometry::rad2deg(std::acos(std::clamp(o.plan.chunks[size_t(c.parent)].build_dir.dot(c.build_dir), -1., 1.)));
                turn > JoinTurnMax && !(slab_joins_exempt() && face_slab(mesh, c, h)) && !force_joins(o.cuts) && !o.arm) {
                o.why = Slic3r::format("chunk %1%: its layers turn %2$.0f deg from its parent's (> %3$.0f): a visible join line", ci, turn, JoinTurnMax);
                break;
            }
            {
                const MultiplanarChunk &parent = o.plan.chunks[size_t(c.parent)];
                const Vec3d             n      = c.clips[size_t(c.base_clip)].normal;
                auto                    join   = [&n](const Vec3d &d) {
                    return Geometry::rad2deg(std::acos(std::clamp(std::abs(d.normalized().dot(n.normalized())), 0., 1.)));
                };
                const double parent_join = join(c.parent == 0 ? Vec3d::UnitZ() : parent.build_dir), own_join = join(c.build_dir);
                o.pieces += Slic3r::format(" (chunk %1%: dir %2$.3f,%3$.3f,%4$.3f cut %5$.3f,%6$.3f,%7$.3f @ %8$.3f, parent %9%, join %10$.1f / %11$.1f deg)",
                                           ci, c.build_dir.x(), c.build_dir.y(), c.build_dir.z(), n.x(), n.y(), n.z(),
                                           c.clips[size_t(c.base_clip)].offset, c.parent, parent_join, own_join);
                const double er    = h * std::sin(20. * M_PI / 180.) / std::sqrt(12.);
                const double ridge = (1. + join_line_k()) * JoinRidgeMs / (er * er) * lw * cut_perimeter(mesh, c.clips, size_t(c.base_clip));
                o.J += ridge;
                o.J_ceil += ridge;
                o.pieces += Slic3r::format(" (join %1$.0f)", ridge);
            }
            BoundingBoxf3 box;
            for (const stl_vertex &v : clip_mesh(mesh, c.clips).vertices)
                box.merge(v.cast<double>());
            JointPose joints;
            if (!box.defined || !kinematics->inverse(ToolPose { box.center(), c.build_dir }, nullptr, joints)) {
                o.why = Slic3r::format("chunk %1%: the rotary axes cannot reach its build direction", ci);
                break;
            }
            double move = 0.;
            for (int a = 0; a < 2; ++a)
                if (const double rate = kinematics->rotary_axes()[size_t(a)].max_speed_deg_s; rate > 0.)
                    move = std::max(move, std::abs(joints.rot[size_t(a)]) / rate);
            o.T_motion += 2. * move;
            std::string key = clips_key(c.clips);
            for (size_t j = 0; j < ci; ++j)
                key += "/" + clips_key(o.plan.chunks[j].clips);
            auto hit = head_hits.find(key);
            if (hit == head_hits.end())
                hit = head_hits.emplace(key, chunk_head_hit(mesh, o.plan, ci, params)).first;
            if (hit->second.empty())
                continue;
            if (plinth_allowed && hit->second.find("would meet the bed") != std::string::npos) {
                if (o.bed_hit.empty())
                    o.bed_hit = Slic3r::format("chunk %1%: %2%", ci, hit->second);
            } else
                o.why = Slic3r::format("chunk %1%: %2%", ci, hit->second);
        }
        if (o.why.empty() && !o.bed_hit.empty()) {
            std::string blocked;
            for (const int layers : PlinthLadder) {
                const MultiplanarParams &raised = lifted(layers, o.plan.chunks.front().clips);
                blocked.clear();
                for (size_t ci = 1; ci < o.plan.chunks.size() && blocked.empty(); ++ci) {
                    std::string key = std::to_string(layers) + "#" + clips_key(o.plan.chunks[ci].clips);
                    for (size_t j = 0; j < ci; ++j)
                        key += "/" + clips_key(o.plan.chunks[j].clips);
                    auto hit = head_hits.find(key);
                    if (hit == head_hits.end())
                        hit = head_hits.emplace(key, chunk_head_hit(mesh, o.plan, ci, raised)).first;
                    if (!hit->second.empty())
                        blocked = Slic3r::format("chunk %1%: %2%", ci, hit->second);
                }
                if (blocked.empty()) {
                    o.plinth = layers;
                    o.pieces += Slic3r::format(" (plinth: %1% raft layers, %2$.2f mm)", layers, -raised.plate_z);
                    break;
                }
            }
            if (o.plinth == 0)
                o.why = o.bed_hit + Slic3r::format("; on a %1%-layer plinth, %2%", PlinthLadder.back(), blocked);
        }
        o.J += W.time * o.T_motion;
        o.J_ceil += W.time * o.T_motion;
        offers.push_back(std::move(o));
    };
    for (size_t pv = 0; pv <= variants.size(); ++pv)
    for (const std::vector<bool> &keep : masks) {
        if (pv > 0) {
            const size_t vc   = variants[pv - 1].cut;
            bool         leaf = keep[vc];
            for (size_t j = 0; j < K && leaf; ++j)
                leaf = !(keep[j] && parents[j] == int(vc));
            if (!leaf)
                continue;
        }
        Offer o;
        for (size_t i = 0; i < K; ++i)
            if (keep[i])
                o.cuts += (o.cuts.empty() ? "{" : ",") + std::to_string(i) + (pv > 0 && i == variants[pv - 1].cut ? variants[pv - 1].tag : "");
        o.cuts += "}";
        std::vector<bool> with = keep;
        if (pv > 0)
            with.resize(variants[pv - 1].plan.cuts.size(), true);
        o.plan = multiplanar_subset(pv > 0 ? variants[pv - 1].plan : plan, with);
        if (pv > 0)
            for (const size_t g : variants[pv - 1].global) {
                const HalfSpace base = variants[pv - 1].plan.cuts[g].chunk.clips.front();
                const auto      at   = std::find_if(o.plan.chunks.begin(), o.plan.chunks.end(), [&base](const MultiplanarChunk &x) {
                    return x.base_clip >= 0 && (x.clips[size_t(x.base_clip)].normal - base.normal).norm() < 1e-9 &&
                           std::abs(x.clips[size_t(x.base_clip)].offset - base.offset) < 1e-9;
                });
                if (at == o.plan.chunks.end())
                    continue;
                if (at + 1 != o.plan.chunks.end()) {
                    o.why = "a whole-plane face slab must print last";
                    break;
                }
                at->clips     = { base };
                at->base_clip = 0;
                for (auto other = o.plan.chunks.begin(); other != at; ++other)
                    other->clips.push_back({ -base.normal, -base.offset });
            }
        finish(std::move(o));
    }
    for (size_t k = 0; k < lips.size(); ++k) {
        Offer o;
        o.cuts = Slic3r::format("{L%1%}", k);
        try {
            o.plan     = spec_cut_plan({ lips[k].second }, mesh, *kinematics);
            o.plan.lip = true;
        } catch (const std::exception &ex) {
            log << "offer " << o.cuts << ": " << ex.what() << "; ";
            continue;
        }
        finish(std::move(o));
    }
    for (const auto &[arm, file] : arms) {
        Offer o;
        o.cuts = "{A:" + arm + "}";
        o.arm  = true;
        try {
            const nlohmann::json spec = nlohmann::json::parse(std::ifstream(file));
            const std::string    want = spec.value("mesh_sha256", std::string()), input = object.model_object()->input_file;
            if (!want.empty() && !input.empty() && want != file_sha256(input))
                throw Slic3r::RuntimeError("its mesh_sha256 is not this object's input file's");
            o.plan = route_spec_plan(file, mesh, *kinematics);
            if (o.plan.cuts.empty()) {
                log << "offer " << o.cuts << ": planar (" << spec.value("label", std::string("no cuts")) << "), the body; ";
                continue;
            }
            const std::string source = !spec.contains("source") ? std::string("no source") :
                                       spec.at("source").is_string() ? spec.at("source").get<std::string>() : spec.at("source").dump();
            o.pieces += " (arm " + arm + ", " + source + ", sha256 " + file_sha256(file) + ")";
            if (const char *v = research_switch("NP_ONESHOT_ARM_ROOT_PHI"); v != nullptr && *v == '1')
                if (const boost::filesystem::path phi = boost::filesystem::path(file).parent_path() /
                                                        (boost::filesystem::path(object.model_object()->name).stem().string() + ".npf.npz");
                    boost::filesystem::exists(phi))
                    o.root_phi = phi.string();
        } catch (const std::exception &ex) {
            log << "offer " << o.cuts << ": " << ex.what() << "; ";
            continue;
        }
        finish(std::move(o));
    }
    const double screen_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    auto                 forced = [](const Offer &o) { return route_forced(o.cuts); };
    std::vector<Offer *> order;
    for (Offer &o : offers)
        if (o.why.empty() && (o.J < RouteScreenBand * J0 || forced(o) || o.arm))
            order.push_back(&o);
    std::sort(order.begin(), order.end(), [&forced](const Offer *a, const Offer *b) {
        return forced(*a) != forced(*b) ? forced(*a) : a->J < b->J;
    });
    double       t_body = std::numeric_limits<double>::quiet_NaN(), t_ps = t_body;
    MeasuredPrint                          body_measured;
    std::vector<std::pair<Meters, Offer *>> metered;
    const char  *keep_dir = research_switch("NP_ONESHOT_ROUTE_KEEP");
    auto         keep_as  = [&](const std::string &what) {
        return keep_dir != nullptr && *keep_dir != 0 ? (boost::filesystem::path(keep_dir) / (name + "." + what + ".gcode")).string() : std::string();
    };
    const Offer *chosen = nullptr;
    double       J_chosen = J0;
    int          measured = 0;
    std::vector<std::pair<double, Offer *>> priced_offers;
    int lip_measured = 0, arm_measured = 0;
    for (Offer *o : order) {
        const bool lip = o->cuts.rfind("{L", 0) == 0;
        if (o->arm)
            ++arm_measured;
        else if (lip ? lip_measured == LipMeasured : measured == RouteMeasured) {
            o->why = "unmeasured (the measuring budget)";
            continue;
        } else
            ++(lip ? lip_measured : measured);
        if (!o->root_phi.empty()) {
            try {
                BoundingBoxf3 box;
                for (const stl_vertex &v : mesh.vertices)
                    box.merge(v.cast<double>());
                NpField given = load_npfield_for(o->root_phi, box, plate_offset(object));
                if (given.layer_tops.empty() && !(in_plane_spread(given) < OneShotState::ZOnlySpread))
                    throw Slic3r::RuntimeError("it is not z-only and carries no layer tops, and a route's root prints planar layers");
                o->root = std::make_shared<const NpField>(std::move(given));
                log << "offer " << o->cuts << ": its root prints the arm's field; ";
            } catch (const std::exception &ex) {
                log << "offer " << o->cuts << ": the arm's field for its root unused (" << ex.what() << "); ";
            }
        }
        if (!o->root) {
            const std::vector<HalfSpace> &rclips = o->plan.chunks.front().clips;
            const indexed_triangle_set    rmesh  = clip_mesh(mesh, rclips);
            std::string                   root_student;
            int                           root_polish = 0;
            OneShotAbortReason            root_abort;
            NpField rf = one_shot_field(object, rmesh, 0., [&object]() { throw_if_canceled(object); }, root_student,
                                        root_polish, root_abort, nullptr, [&rclips](const std::array<Vec3d, 3> &t) { return on_clip(t, rclips); });
            if (rf.dims[0] * rf.dims[1] * rf.dims[2] == 0) {
                o->why = "its root holds no grid node";
                continue;
            }
            if (root_abort.kind == OneShotAbort::None) {
                const PieceTerms &old_root = price(o->plan.chunks.front(), true);
                double            J_root = 0., T_root = 0.;
                for (const auto &[key, value] : rf.terms)
                    if (key == "J")
                        J_root = value;
                    else if (key == "J_T")
                        T_root = value;
                o->J += J_root - old_root.t.J;
                o->T += T_root - old_root.t.T;
                o->root = std::make_shared<const NpField>(std::move(rf));
            }
        }
        if (std::isnan(t_body)) {
            const Injection body_only(state);
            t_body = measured_time(object, NonplanarMode::OneShot, {}, keep_as("body"), false, &body_measured);
            t_ps   = measured_time(object, NonplanarMode::Off,
                                   {},
                                   keep_as("ps"), false);
        }
        OneShotState offered = state;
        offered.route        = std::make_shared<const MultiplanarPlan>(o->plan);
        if (o->root)
            offered.solved = o->root;
        double        t_route;
        MeasuredPrint route_measured;
        {
            const Injection route(offered);
            t_route = measured_time(object, NonplanarMode::OneShot, o->plinth > 0 ? plinth_overrides(o->plinth) : NoOverrides, keep_as("route" + o->cuts),
                                    pick_meters(), &route_measured);
        }
        if (!std::isfinite(t_route)) {
            o->why = pick_meters() ? "its verified slice failed or hits" : "its measured slice failed (a chunk floats, or no slice)";
            continue;
        }
        if (const double cap = pick_meters() ? PickTimeCap : RouteTimeCap; !(t_route <= cap * t_ps) && !forced(*o)) {
            cap_refused = true;
            o->why = Slic3r::format("measured %1$.0f s > %2$.3f x planar + support %3$.0f s", t_route, cap, t_ps);
            continue;
        }
        const double J = o->J - W.time * (o->T + o->T_motion) + W.time * T0 * t_route / t_body;
        o->t_measured  = t_route;
        o->J_measured  = J;
        log << "offer " << o->cuts << (o->plinth > 0 ? Slic3r::format(" on a %1%-layer plinth", o->plinth) : std::string()) << " measured " << std::lround(t_route) << " s (body " << std::lround(t_body) << ", p+s " << std::lround(t_ps)
            << "), J " << std::lround(J) << "; ";
        if (pick_meters()) {
            const Meters m = meters_of(route_measured, t_route);
            log << "(" << meters_text(m) << ") ";
            metered.emplace_back(m, o);
            continue;
        }
        if (J < J0 || forced(*o))
            priced_offers.emplace_back(forced(*o) ? -Inf : J, o);
        else
            o->why = Slic3r::format("measured J %1$.0f >= the body's", J);
    }
    if (pick_meters() && !metered.empty()) {
        Meters best = meters_of(body_measured, t_body);
        log << "body (" << meters_text(best) << "); ";
        for (const auto &[m, o] : metered)
            if (forced(*o) || ((chosen == nullptr || !forced(*chosen)) && meters_better(m, best))) {
                chosen   = o;
                best     = forced(*o) ? Meters { -1., -1., -1. } : m;
                J_chosen = o->J_measured;
            }
        for (const auto &[m, o] : metered)
            if (o != chosen)
                o->why = "not the best on the meters (" + meters_text(m) + ")";
    }
    std::sort(priced_offers.begin(), priced_offers.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[J, o] : priced_offers) {
        OneShotState offered = state;
        offered.route        = std::make_shared<const MultiplanarPlan>(o->plan);
        if (o->root)
            offered.solved = o->root;
        double t_verified;
        {
            const Injection route(offered);
            t_verified = measured_time(object, NonplanarMode::OneShot, o->plinth > 0 ? plinth_overrides(o->plinth) : NoOverrides,
                                       keep_as("route" + o->cuts + ".verified"), true);
        }
        if (std::isfinite(t_verified)) {
            chosen   = o;
            J_chosen = std::isfinite(J) ? J : o->J;
            break;
        }
        o->why = last_measured_failure().empty() ? std::string("its verified export hits") : "its verified export failed: " + last_measured_failure();
    }
    for (const auto &[J, o] : priced_offers)
        if (o != chosen && o->why.empty())
            o->why = Slic3r::format("measured J %1$.0f, not the lowest verified", J);
    for (const Offer &o : offers)
        log << "offer " << o.cuts << " (" << o.plan.chunks.size() - 1 << " chunks): screen J " << std::lround(o.J) << " (J's own S " << std::lround(o.J_ceil) << ") seam "
            << std::lround(o.seam) << o.pieces
            << (o.why.empty() ? "" : " out: " + o.why) << "; ";
    const double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    log << "body J " << std::lround(J0)
        << Slic3r::format(" [S %1$.0f E_up %2$.0f E_wall %3$.0f E_down %4$.0f T %5$.0f]", solved_term(state, "J_S"), solved_term(state, "J_E_up"),
                          solved_term(state, "J_E_wall"), solved_term(state, "J_E_down"), T0)
        << "; routes " << std::lround(screen_s) << " s screen, " << std::lround(total_s) << " s total";
    state.route_log = log.str();
    if (const char *dir = research_switch("NP_ONESHOT_DUMP"); dir != nullptr && *dir != 0) {
        std::ofstream out((boost::filesystem::path(dir) / (object.model_object()->name + ".route-offers.tsv")).string());
        out << "cuts\tchunks\tscreen_J\tscreen_J_own_S\tscreen_T\tscreen_T_motion\tseam\tmeasured_s\tmeasured_J\tchosen\twhy\tpieces\n";
        out << "{}\t0\t" << J0 << "\t" << J0 << "\t" << T0 << "\t0\t0\t" << t_body << "\t" << J0 << "\t" << (chosen == nullptr) << "\tbody\t\n";
        for (const Offer &o : offers)
            out << o.cuts << '\t' << o.plan.chunks.size() - 1 << '\t' << o.J << '\t' << o.J_ceil << '\t' << o.T << '\t' << o.T_motion << '\t'
                << o.seam << '\t' << o.t_measured << '\t' << o.J_measured << '\t' << (&o == chosen) << '\t' << o.why << '\t' << o.pieces << '\n';
    }
    for (const Offer &o : offers)
        if (forced(o) && &o != chosen)
            state.forced_why = o.cuts + ": " + (o.why.empty() ? std::string("not picked") : o.why);
    NpField f = chosen != nullptr && chosen->root ? *chosen->root : body;
    f.terms.emplace_back("stat_route_offers", double(offers.size()));
    f.terms.emplace_back("stat_route_chosen", chosen ? double(chosen->plan.chunks.size() - 1) : 0.);
    f.terms.emplace_back("stat_route_J", J_chosen);
    f.terms.emplace_back("stat_route_seconds", total_s);
    f.terms.emplace_back("stat_route_measured", double(measured + lip_measured + arm_measured));
    if (std::isfinite(t_ps))
        f.terms.emplace_back("stat_route_ps_time_s", t_ps);
    if (chosen) {
        auto route = std::make_shared<MultiplanarPlan>(chosen->plan);
        route->auto_reason = Slic3r::format("One-shot prints %1% using %2% tilted chunks%5%: J %3$.0f vs. %4$.0f with flat layers.", name,
                                            route->chunks.size() - 1, J_chosen, J0,
                                            chosen->plinth > 0 ? Slic3r::format(" on a %1%-layer raft that lifts it clear of the bed", chosen->plinth) : std::string());
        state.route        = std::move(route);
        state.plinth_layers = chosen->plinth;
        state.route_cuts    = chosen->cuts;
    }
    state.solved = std::make_shared<const NpField>(std::move(f));
    BOOST_LOG_TRIVIAL(warning) << "One-shot routes " << name << ": " << state.route_log;
    return cap_refused;
}

}

namespace {

void choose_route_any(PrintObject &object, OneShotState &state)
{
    const double step = std::clamp(object.config().nonplanar_oneshot_fan_step.value, 1., 45.);
    if (!choose_route_once(object, state, step) || state.route != nullptr || step >= FanStepFallback)
        return;
    const std::string fine_log = state.route_log;
    choose_route_once(object, state, FanStepFallback);
    state.route_log = Slic3r::format("joins in %1$.0f deg steps put the routes past the time cap or the overhang cap (%2%): planned again in %3$.0f deg steps; ",
                                     step, fine_log, FanStepFallback) + state.route_log;
}

}

bool route_forced(const std::string &cuts)
{
    const char *force = research_switch("NP_ONESHOT_ROUTE_FORCE");
    return force != nullptr && *force != 0 && (cuts == force || cuts == std::string(force) + "g");
}

void choose_route(PrintObject &object, OneShotState &state)
{
    if (state.forced_arm) {
        state.route_log = "routes: none, a forced arm prints as given";
        return;
    }
    state.route_cuts.clear();
    state.forced_why.clear();
    choose_route_any(object, state);
    if (const char *force = research_switch("NP_ONESHOT_ROUTE_FORCE"); force != nullptr && std::string(force).rfind("{A:", 0) == 0 && state.route_cuts != force) {
        const std::string arm  = std::string(force).substr(3, std::string(force).size() - 4);
        const size_t      lean = state.time_cut_note.find("lean offer arm " + arm + ",");
        const std::string why  = !state.forced_why.empty() ? state.forced_why :
                                 lean != std::string::npos ? state.time_cut_note.substr(lean, state.time_cut_note.find(", lean offer ", lean + 1) - lean) :
                                                             std::string("no such offer");
        throw SlicingError(Slic3r::format("FORCED ARM NOT ADOPTED: %1% did not print (%2%)", force, why));
    }
    if (const char *force = research_switch("NP_ONESHOT_ROUTE_FORCE"); force != nullptr && *force != 0 && !route_forced(state.route_cuts))
        throw SlicingError(Slic3r::format("FORCED ROUTE NOT ADOPTED: %1% did not print (%2%)", force,
                                          state.forced_why.empty() ? std::string("no such offer") : state.forced_why));
}

}
