#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Print.hpp"
#include <libslic3r/PrintBase.hpp>
#include <libslic3r/CustomGCode.hpp>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

static std::set<int> tools_for_role(const std::string& gcode, const std::string& role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

static std::vector<double> wait_park_xs(const std::string& gcode)
{
    std::vector<std::string> lines;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);)
        lines.emplace_back(std::move(line));
    std::vector<double> xs;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("M109", 0) != 0 || lines[i].find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos)
            continue;
        for (size_t j = i; j-- > 0;) {
            if (lines[j].rfind("G1 ", 0) != 0)
                continue;
            const size_t x_pos = lines[j].find('X');
            if (x_pos == std::string::npos)
                continue;
            xs.push_back(std::stod(lines[j].substr(x_pos + 1)));
            break;
        }
    }
    return xs;
}

static std::vector<double> elapsed_time_by_line(const std::string& gcode)
{
    ScopedTemporaryFile temp_gcode(".gcode");
    {
        std::ofstream os(temp_gcode.string());
        os << gcode;
    }
    GCodeProcessor processor;
    processor.process_file(temp_gcode.string());

    constexpr size_t    NORMAL  = size_t(PrintEstimatedStatistics::ETimeMode::Normal);
    const size_t        n_lines = size_t(std::count(gcode.begin(), gcode.end(), '\n')) + 2;
    std::vector<double> elapsed(n_lines, 0.);
    double              running = 0.;
    size_t              next    = 0;
    for (const auto& move : processor.get_result().moves) {
        const size_t id = std::min<size_t>(move.gcode_id, n_lines - 1);
        while (next <= id)
            elapsed[next++] = running;
        running += move.time[NORMAL];
    }
    while (next < n_lines)
        elapsed[next++] = running;
    return elapsed;
}

static std::vector<std::string> temperature_trace(const std::string& gcode)
{
    std::vector<std::string> lines;
    std::istringstream       stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        line.erase(0, line.find_first_not_of(" \t"));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        lines.emplace_back(std::move(line));
    }
    const std::vector<double> elapsed = elapsed_time_by_line(gcode);

    const auto is_tool = [](const std::string& l) { return l.size() >= 2 && l[0] == 'T' && std::isdigit((unsigned char) l[1]); };
    const auto is_temp = [](const std::string& l) { return l.rfind("M104", 0) == 0 || l.rfind("M109", 0) == 0; };
    const auto marker  = [](const std::string& l) -> const char* {
        for (const char* m : { "; CP TOOLCHANGE START", "; CP TOOLCHANGE END", "; CP PRIMING START", "; CP PRIMING END" })
            if (l.find(m) != std::string::npos)
                return m;
        return nullptr;
    };

    const auto tool_of = [&is_tool](const std::string& l) -> int {
        size_t t = std::string::npos;
        if (is_tool(l))
            t = 0;
        else if (l.rfind("M104", 0) == 0 && l.find(" T") != std::string::npos)
            t = l.find(" T") + 1;
        if (t == std::string::npos || t + 1 >= l.size() || !std::isdigit((unsigned char) l[t + 1]))
            return -1;
        return std::stoi(l.substr(t + 1));
    };

    std::vector<std::string> trace;
    bool                     in_block     = false;
    int                      current_tool = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (const char* m = marker(lines[i])) {
            in_block = std::string(m).find("START") != std::string::npos;
            trace.emplace_back(m);
        } else if (is_tool(lines[i]) || is_temp(lines[i])) {
            std::string entry = lines[i];
            const int   named = tool_of(lines[i]);
            if (!in_block && lines[i].rfind("M104", 0) == 0 && current_tool != -1 && named != -1 && named != current_tool) {
                size_t tn = i;
                while (tn < lines.size() && !is_tool(lines[tn]))
                    ++tn;
                if (tn < lines.size()) {
                    char lead[32];
                    std::snprintf(lead, sizeof(lead), "\tlead %.1fs", elapsed[tn + 1] - elapsed[i + 1]);
                    entry += lead;
                }
            }
            if (is_tool(lines[i]))
                current_tool = named;
            trace.emplace_back(std::move(entry));
        }
    }
    return trace;
}

static constexpr double TRACE_TIME_TOLERANCE_S = 1.5;
static constexpr double TRACE_ROUNDING_SLACK_S = 0.05;

struct TraceEntry
{
    std::string           text;
    std::optional<double> time_s;
    std::optional<double> lead_s;
};

static TraceEntry parse_trace_entry(const std::string& entry)
{
    TraceEntry out;
    std::string text = entry;

    const size_t tab = text.find('\t');
    if (tab != std::string::npos) {
        const std::string tail = text.substr(tab + 1);
        const size_t      sp   = tail.find(' ');
        if (sp != std::string::npos && sp + 1 < tail.size()
            && std::isdigit(static_cast<unsigned char>(tail[sp + 1]))) {
            out.lead_s = std::stod(tail.substr(sp + 1));
            text.erase(tab);
        }
    }

    static constexpr std::string_view k_time = "time: ";
    const size_t                      at     = text.find(k_time);
    if (at != std::string::npos && at + k_time.size() < text.size()
        && std::isdigit(static_cast<unsigned char>(text[at + k_time.size()]))) {
        const size_t first = at + k_time.size();
        size_t       last  = first;
        while (last < text.size() && (std::isdigit(static_cast<unsigned char>(text[last])) || text[last] == '.'))
            ++last;
        out.time_s = std::stod(text.substr(first, last - first));
        text.replace(first, last - first, "<n>");
    }

    out.text = std::move(text);
    return out;
}

static bool timings_match(const std::optional<double>& a, const std::optional<double>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a.has_value() || std::abs(*a - *b) <= TRACE_TIME_TOLERANCE_S;
}

static bool time_is_rounded_lead(const TraceEntry& e)
{
    if (!e.time_s.has_value() || !e.lead_s.has_value())
        return true;
    return std::abs(*e.time_s - *e.lead_s) <= 0.5 + TRACE_ROUNDING_SLACK_S;
}

static bool trace_entries_match(const std::string& a, const std::string& b)
{
    const auto x = parse_trace_entry(a);
    const auto y = parse_trace_entry(b);
    if (x.text != y.text)
        return false;
    if (x.time_s.has_value() != y.time_s.has_value())
        return false;
    return timings_match(x.lead_s, y.lead_s) && time_is_rounded_lead(x);
}

TEST_CASE("Each feature prints with its assigned filament", "[MultiFilament]")
{
    auto [infill_filament, wall_filament] = GENERATE(table<int, int>({ {1, 1}, {1, 2}, {2, 1}, {2, 2} }));
    DYNAMIC_SECTION("infill filament " << infill_filament << ", wall filament " << wall_filament) {
        const std::string gcode = slice({ cube(20) },
            multifilament_config(2, {
                { "sparse_infill_filament_id",  infill_filament },
                { "internal_solid_filament_id", infill_filament },
                { "top_surface_filament_id",    infill_filament },
                { "bottom_surface_filament_id", infill_filament },
                { "outer_wall_filament_id",     wall_filament },
                { "inner_wall_filament_id",     wall_filament },
                { "skirt_loops",                1 },
                { "brim_type",                  "outer_only" },
                { "brim_width",                 5 },
            }));
        const std::set<int> wall_tool{ wall_filament - 1 };
        const std::set<int> infill_tool{ infill_filament - 1 };
        CHECK(tools_for_role(gcode, "perimeter") == wall_tool);
        CHECK(tools_for_role(gcode, "infill")    == infill_tool);
        CHECK(tools_for_role(gcode, "brim")      == wall_tool);
        CHECK(tools_for_role(gcode, "skirt")     == wall_tool);
    }
}

TEST_CASE("Each feature prints with its assigned filament (three filaments)", "[MultiFilament]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(3, {
            { "sparse_infill_filament_id",  2 },
            { "internal_solid_filament_id", 2 },
            { "top_surface_filament_id",    2 },
            { "bottom_surface_filament_id", 2 },
            { "outer_wall_filament_id",     3 },
            { "inner_wall_filament_id",     3 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 2 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 1 });
}

TEST_CASE("Per-object wall filament override is honored", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "skirt_loops",    0 },
            { "brim_type",      "no_brim" },
            { "print_sequence", "by object" },
        }),
        { {}, { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 } } });
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0, 1 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 0 });
}

TEST_CASE("Toolchange temperature wait moves to the wipe tower when enabled", "[MultiFilament]")
{
    const bool wait_on_tower = GENERATE(false, true);
    DYNAMIC_SECTION("wait_for_temp_on_wipe_tower " << (wait_on_tower ? 1 : 0)) {
        const std::string gcode = slice_with_object_overrides(
            { cube(20), cube(20) },
            multifilament_config(2, {
                { "nozzle_diameter",                "0.4,0.4" },
                { "printer_extruder_id",            "1,2" },
                { "printer_extruder_variant",       "Direct Drive Standard,Direct Drive Standard" },
                { "extruder_printable_height",      "0,0" },
                { "single_extruder_multi_material", 0 },
                { "enable_prime_tower",             1 },
                { "prime_tower_width",              35 },
                { "wipe_tower_x",                   "50" },
                { "wipe_tower_y",                   "50" },
                { "ooze_prevention",                1 },
                { "standby_temperature_delta",      -40 },
                { "preheat_time",                   0 },
                { "wait_for_temp_on_wipe_tower",    wait_on_tower ? 1 : 0 },
            }),
            { { { "extruder", 1 } }, { { "extruder", 2 } } });

        std::vector<std::string> lines;
        std::istringstream gcode_stream(gcode);
        for (std::string line; std::getline(gcode_stream, line);)
            lines.emplace_back(std::move(line));
        const auto is_tool_line   = [](const std::string& l) { return l.size() >= 2 && l[0] == 'T' && std::isdigit((unsigned char)l[1]); };
        const auto is_m109_line   = [](const std::string& l) { return l.rfind("M109", 0) == 0; };
        const auto is_m104_for_tool = [](const std::string& l, int tool) {
            if (l.rfind("M104", 0) != 0)
                return false;
            const std::string token = " T" + std::to_string(tool);
            const size_t      at    = l.find(token);
            return at != std::string::npos && !std::isdigit((unsigned char)l[at + token.size()]);
        };
        const auto is_tagged_wait = [](const std::string& l) { return l.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") != std::string::npos; };
        const auto is_extruding   = [](const std::string& l) {
            if (l.rfind("G1 ", 0) != 0)
                return false;
            const size_t e = l.find(" E");
            return e != std::string::npos && l.find_first_of("XY") != std::string::npos && l[e + 2] != '-';
        };

        int checked_blocks = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("; CP TOOLCHANGE START") == std::string::npos)
                continue;
            size_t block_end = i;
            while (block_end < lines.size() && lines[block_end].find("; CP TOOLCHANGE END") == std::string::npos)
                ++block_end;
            size_t tool_line = block_end;
            for (size_t j = i; j < block_end; ++j)
                if (is_tool_line(lines[j])) { tool_line = j; break; }
            if (tool_line == block_end)
                continue;
            ++checked_blocks;

            const int new_tool = std::stoi(lines[tool_line].substr(1));
            size_t    preheat = tool_line, restore = block_end;
            for (size_t j = i; j < tool_line; ++j)
                if (is_m104_for_tool(lines[j], new_tool)) { preheat = j; break; }
            for (size_t j = tool_line + 1; j < block_end; ++j)
                if (is_m104_for_tool(lines[j], new_tool)) { restore = j; break; }

            size_t tagged_wait = block_end, untagged_m109 = block_end, first_extrusion = block_end;
            for (size_t j = tool_line + 1; j < block_end; ++j) {
                if (is_m109_line(lines[j]) && tagged_wait == block_end && is_tagged_wait(lines[j]))
                    tagged_wait = j;
                if (is_m109_line(lines[j]) && untagged_m109 == block_end && !is_tagged_wait(lines[j]))
                    untagged_m109 = j;
                if (first_extrusion == block_end && is_extruding(lines[j]))
                    first_extrusion = j;
            }
            INFO("toolchange block at line " << i + 1);
            if (wait_on_tower) {
                REQUIRE(tagged_wait < block_end);
                CHECK(untagged_m109 == block_end);
                CHECK(preheat < tool_line);
                CHECK(restore == block_end);
                REQUIRE(first_extrusion < block_end);
                CHECK(tagged_wait < first_extrusion);
                size_t stop_line = block_end;
                for (size_t j = tagged_wait; j-- > tool_line;)
                    if (lines[j].rfind("G1 ", 0) == 0 && lines[j].find('X') != std::string::npos) { stop_line = j; break; }
                REQUIRE(stop_line < block_end);
                const double stop_x = std::stod(lines[stop_line].substr(lines[stop_line].find('X') + 1));
                double purge_min_x = std::numeric_limits<double>::max(), purge_max_x = std::numeric_limits<double>::lowest();
                for (size_t j = tagged_wait; j < block_end; ++j) {
                    const size_t x_pos = lines[j].find('X');
                    if (!is_extruding(lines[j]) || x_pos == std::string::npos)
                        continue;
                    const double x = std::stod(lines[j].substr(x_pos + 1));
                    purge_min_x = std::min(purge_min_x, x);
                    purge_max_x = std::max(purge_max_x, x);
                }
                REQUIRE(purge_min_x <= purge_max_x);
                INFO("stop travel: " << lines[stop_line] << " purge x range: " << purge_min_x << ".." << purge_max_x);
                const bool beside_tower = stop_x < purge_min_x - 0.5 || stop_x > purge_max_x + 0.5;
                CHECK(beside_tower);
            } else {
                REQUIRE(untagged_m109 < block_end);
                CHECK(tagged_wait == block_end);
                CHECK(preheat == tool_line);
                if (first_extrusion < block_end)
                    CHECK(untagged_m109 < first_extrusion);
            }
            i = block_end;
        }
        REQUIRE(checked_blocks > 0);
        if (!wait_on_tower)
            CHECK(gcode.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos);
    }
}

TEST_CASE("Wipe tower priming pre-heats to the first layer temperature", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "nozzle_diameter",                        "0.4,0.4" },
            { "printer_extruder_id",                    "1,2" },
            { "printer_extruder_variant",               "Direct Drive Standard,Direct Drive Standard" },
            { "extruder_printable_height",              "0,0" },
            { "single_extruder_multi_material",         0 },
            { "single_extruder_multi_material_priming", 1 },
            { "enable_prime_tower",                     1 },
            { "prime_tower_width",                      35 },
            { "wipe_tower_x",                           "50" },
            { "wipe_tower_y",                           "50" },
            { "preheat_time",                           0 },
            { "nozzle_temperature_initial_layer",       "215,215" },
            { "nozzle_temperature",                     "240,240" },
            { "wait_for_temp_on_wipe_tower",            1 },
        }),
        { { { "extruder", 1 } }, { { "extruder", 2 } } });

    std::vector<std::string> lines;
    std::istringstream gcode_stream(gcode);
    for (std::string line; std::getline(gcode_stream, line);)
        lines.emplace_back(std::move(line));
    const auto temp_of = [](const std::string& l) {
        if (l.rfind("M104", 0) != 0 && l.rfind("M109", 0) != 0)
            return -1;
        const size_t s = l.find('S');
        return s == std::string::npos ? -1 : std::stoi(l.substr(s + 1));
    };

    size_t start = lines.size(), end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (start == lines.size() && lines[i].find("; CP PRIMING START") != std::string::npos)
            start = i;
        else if (start < lines.size() && lines[i].find("; CP PRIMING END") != std::string::npos) {
            end = i;
            break;
        }
    }
    REQUIRE(start < end);

    int checked_waits = 0;
    for (size_t i = start; i < end; ++i) {
        if (lines[i].find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos)
            continue;
        ++checked_waits;
        INFO("priming wait at line " << i + 1 << ": " << lines[i]);
        CHECK(temp_of(lines[i]) == 215);
        int preheat = -1;
        for (size_t j = i; j-- > start;)
            if ((preheat = temp_of(lines[j])) != -1)
                break;
        CHECK(preheat == 215);
    }
    REQUIRE(checked_waits > 0);
}

TEST_CASE("Wipe tower temperature-wait park is regenerated when the tower moves", "[MultiFilament]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "single_extruder_multi_material", 0 },
        { "enable_prime_tower",             1 },
        { "prime_tower_width",              35 },
        { "wipe_tower_wall_type",           "rectangle" },
        { "prime_tower_brim_width",         0 },
        { "printable_area",                 "0x0,200x0,200x200,0x200" },
        { "wipe_tower_x",                   "0" },
        { "wipe_tower_y",                   "50" },
        { "ooze_prevention",                1 },
        { "standby_temperature_delta",      -40 },
        { "wait_for_temp_on_wipe_tower",    1 },
    });
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        { { "extruder", 1 } }, { { "extruder", 2 } } };
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, config, &overrides);

    const std::string         at_edge       = gcode(print);
    const std::vector<double> at_edge_parks = wait_park_xs(at_edge);
    REQUIRE(!at_edge_parks.empty());
    for (double x : at_edge_parks) {
        INFO("wait park X " << x << " with the tower at x=0 on a 200mm bed");
        CHECK(x >= -0.05);
        CHECK(x <= 200.05);
    }
    REQUIRE(print.is_step_done(psWipeTower));

    config.set_deserialize_strict({ { "wipe_tower_x", "164" } });
    DynamicPrintConfig moved_config = DynamicPrintConfig::full_print_config();
    moved_config.apply(config);
    moved_config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    print.apply(model, moved_config);
    CHECK_FALSE(print.is_step_done(psWipeTower));

    const std::string         moved       = gcode(print);
    const std::vector<double> moved_parks = wait_park_xs(moved);
    REQUIRE(!moved_parks.empty());
    for (double x : moved_parks) {
        INFO("wait park X " << x << " with the tower at x=164 on a 200mm bed");
        CHECK(x >= -0.05);
        CHECK(x <= 200.05);
    }
}

TEST_CASE("Toolchange temperature commands are unchanged when the wipe tower wait is off", "[MultiFilament][Regression]")
{
    const std::string gcode = slice_with_object_overrides(
        { make_cube(20., 20., 5.), make_cube(20., 20., 5.) },
        multifilament_config(2, {
            { "nozzle_diameter",                        "0.4,0.4" },
            { "printer_extruder_id",                    "1,2" },
            { "printer_extruder_variant",               "Direct Drive Standard,Direct Drive Standard" },
            { "extruder_printable_height",              "0,0" },
            { "single_extruder_multi_material",         0 },
            { "single_extruder_multi_material_priming", 1 },
            { "enable_prime_tower",                     1 },
            { "prime_tower_width",                      35 },
            { "wipe_tower_x",                           "50" },
            { "wipe_tower_y",                           "50" },
            { "ooze_prevention",                        1 },
            { "standby_temperature_delta",              -40 },
            { "preheat_time",                           30 },
            { "preheat_steps",                          1 },
            { "nozzle_temperature_initial_layer",       "215,215" },
            { "nozzle_temperature",                     "240,240" },
            { "wait_for_temp_on_wipe_tower",            0 },
        }),
        { { { "extruder", 1 } }, { { "extruder", 2 } } });

    const std::vector<std::string> trace = temperature_trace(gcode);
    REQUIRE(trace.size() > 1);
    CHECK(gcode.find("_WAIT_FOR_TEMP_ON_WIPE_TOWER") == std::string::npos);

    const std::string golden_path = std::string(TEST_DATA_DIR PATH_SEPARATOR "wipe_tower_temperature_trace_main.txt");

    if (std::getenv("ORCA_UPDATE_WIPE_TOWER_TEMP_TRACE") != nullptr) {
        std::ofstream out(golden_path);
        REQUIRE(out.good());
        out << "# Temperature and tool-change commands of a wait_for_temp_on_wipe_tower-off slice,\n"
               "# captured from the main branch at <fill in the commit>. Regeneration is described\n"
               "# at the test that reads this file: \"Toolchange temperature commands are unchanged\n"
               "# when the wipe tower wait is off\" in tests/fff_print/test_multifilament.cpp.\n";
        for (const std::string& entry : trace)
            out << entry << "\n";
        WARN("Rewrote " << golden_path << " from this run; it no longer reflects main.");
        return;
    }

    std::vector<std::string> golden;
    {
        std::ifstream in(golden_path);
        INFO("reading " << golden_path);
        REQUIRE(in.good());
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty() && line[0] != '#')
                golden.push_back(std::move(line));
        }
    }
    REQUIRE(!golden.empty());

    for (size_t i = 0; i < trace.size(); ++i) {
        const auto entry = parse_trace_entry(trace[i]);
        if (time_is_rounded_lead(entry))
            continue;
        INFO("at trace entry " << i + 1);
        INFO("  " << trace[i]);
        FAIL("\"time:\" is not its entry's \"lead\" rounded to a whole second");
    }

    const size_t common = std::min(trace.size(), golden.size());
    for (size_t i = 0; i < common; ++i) {
        if (trace_entries_match(trace[i], golden[i]))
            continue;
        INFO("first difference at trace entry " << i + 1);
        INFO("  main:   " << golden[i]);
        INFO("  branch: " << trace[i]);
        FAIL("temperature emission differs from main with wait_for_temp_on_wipe_tower off");
    }
    CHECK(trace.size() == golden.size());
}

TEST_CASE("Multi-extruder slice stays in bounds with a short max_layer_height", "[MultiFilament]")
{
    DynamicPrintConfig config = multifilament_config(2);
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4" },
        { "printer_extruder_id",       "1,2" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height", "0,0" },
        { "max_layer_height",          "0.3" },
    });
    Print print;
    init_and_process_print({ cube(20) }, print, config);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}


static void imex_7x4_printer(DynamicPrintConfig &config)
{
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4,0.4,0.4,0.4,0.4,0.4" },
        { "printer_extruder_id",       "1,2,3,4,5,6,7" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard,Direct Drive Standard,"
                                       "Direct Drive Standard,Direct Drive Standard,Direct Drive Standard,"
                                       "Direct Drive Standard" },
        { "extruder_printable_height", "0,0,0,0,0,0,0" },
        { "physical_extruder_map",     "0,0,0,0,1,2,3" },
        { "is_imex",                   "1" },
        { "imex_mode_names",           "primary;copy" },
        { "imex_mode_active_tools",    "0:P;0:P,1:C" },
        { "skirt_loops",               "0" },
        { "brim_type",                 "no_brim" },
        { "gcode_flavor",              "klipper" },
    });
    size_flush_to_nozzles(config);
}

static void all_regions_on_filament(DynamicPrintConfig &config, int filament_1based)
{
    for (const char *key : { "outer_wall_filament_id", "inner_wall_filament_id",
                             "sparse_infill_filament_id", "internal_solid_filament_id",
                             "top_surface_filament_id", "bottom_surface_filament_id" })
        config.set_deserialize_strict({ { key, std::to_string(filament_1based) } });
}

TEST_CASE("Parallel-mode IMEX prints transition the printing head to its second-layer temperature",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    config.set_deserialize_strict({
        { "imex_parallel_mode",                "copy" },
        { "nozzle_temperature_initial_layer",  "200" },
        { "nozzle_temperature",                "240" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(gcode.find("M104 S240 T0") != std::string::npos);
    CHECK(gcode.find("M104 S240 T1") != std::string::npos);
}

TEST_CASE("Primary-mode IMEX prints mark only the filament slot they print with",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 6);
    config.set_deserialize_strict({
        { "imex_parallel_mode", "primary" },
        { "machine_start_gcode",
          ";USED0:{if is_extruder_used[0]}1{else}0{endif}\n"
          ";USED5:{if is_extruder_used[5]}1{else}0{endif}\n" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(gcode.find(";USED5:1") != std::string::npos);
    CHECK(gcode.find(";USED0:0") != std::string::npos);
}

TEST_CASE("Copy-mode IMEX prints still mark every secondary carriage's filament slot",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    config.set_deserialize_strict({
        { "imex_parallel_mode", "copy" },
        { "machine_start_gcode",
          ";USED0:{if is_extruder_used[0]}1{else}0{endif}\n"
          ";USED4:{if is_extruder_used[4]}1{else}0{endif}\n"
          ";USED5:{if is_extruder_used[5]}1{else}0{endif}\n" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(gcode.find(";USED0:1") != std::string::npos);
    CHECK(gcode.find(";USED4:1") != std::string::npos);
    CHECK(gcode.find(";USED5:0") != std::string::npos);
}

TEST_CASE("Primary-mode IMEX prints still transition to the second-layer temperature",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    config.set_deserialize_strict({
        { "imex_parallel_mode",                "primary" },
        { "nozzle_temperature_initial_layer",  "200" },
        { "nozzle_temperature",                "240" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(gcode.find("M104 S240") != std::string::npos);
}

TEST_CASE("An IMEX plate set to a mode the printer no longer defines slices as Primary",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    config.set_deserialize_strict({
        { "imex_parallel_mode",                "copy-renamed" },
        { "nozzle_temperature_initial_layer",  "200" },
        { "nozzle_temperature",                "240" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(gcode.find("M104 S240") != std::string::npos);
    CHECK(gcode.find("M104 S240 T") == std::string::npos);

    bool               selects_initial_tool = false;
    std::istringstream tool_lines(gcode);
    std::string        line;
    while (std::getline(tool_lines, line))
        if (line.rfind("T0", 0) == 0) {
            selects_initial_tool = true;
            break;
        }
    CHECK(selects_initial_tool);
}

TEST_CASE("IQEX modes emit first- and second-layer temperatures for every active carriage",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    config.set_deserialize_strict({
        { "imex_mode_names",                   "primary;copy;iq-copy" },
        { "imex_mode_active_tools",            "0:P;0:P,1:C;0:P,1:C,2:C,3:C" },
        { "imex_parallel_mode",                "iq-copy" },
        { "nozzle_temperature_initial_layer",  "200" },
        { "nozzle_temperature",                "240" },
        { "machine_start_gcode",
          ";USED0:{if is_extruder_used[0]}1{else}0{endif}\n"
          ";USED4:{if is_extruder_used[4]}1{else}0{endif}\n"
          ";USED5:{if is_extruder_used[5]}1{else}0{endif}\n"
          ";USED6:{if is_extruder_used[6]}1{else}0{endif}\n" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(gcode.find(";USED0:1") != std::string::npos);
    CHECK(gcode.find(";USED4:1") != std::string::npos);
    CHECK(gcode.find(";USED5:1") != std::string::npos);
    CHECK(gcode.find(";USED6:1") != std::string::npos);

    CHECK(gcode.find("M104 S240 T0") != std::string::npos);
    CHECK(gcode.find("M104 S240 T1") != std::string::npos);
    CHECK(gcode.find("M104 S240 T2") != std::string::npos);
    CHECK(gcode.find("M104 S240 T3") != std::string::npos);
}

TEST_CASE("IMEX heater commands name the physical head, not the logical filament",
          "[MultiFilament][IMEX][Regression]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    config.set_deserialize_strict({
        { "imex_parallel_mode",                "primary" },
        { "ooze_prevention",                   "1" },
        { "standby_temperature_delta",         "-50" },
        { "single_extruder_multi_material",    "0" },
        { "nozzle_temperature_initial_layer",  "200,200,200,200,200,200,200" },
        { "nozzle_temperature",                "240,240,240,240,240,240,240" },
    });

    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        { { "extruder", "1" } },
        { { "extruder", "5" } },
    };
    const std::string gcode = slice_with_object_overrides({ cube(20), cube(20) }, config, overrides);

    CHECK(gcode.find("\nT4") != std::string::npos);

    std::istringstream ss(gcode);
    std::string        line;
    std::vector<std::string> offenders;
    while (std::getline(ss, line)) {
        if (line.rfind("M104", 0) != 0 && line.rfind("M109", 0) != 0)
            continue;
        const size_t t = line.find(" T");
        if (t == std::string::npos || t + 2 >= line.size() || !std::isdigit((unsigned char) line[t + 2]))
            continue;
        if (std::stoi(line.substr(t + 2)) > 3)
            offenders.push_back(line);
    }
    INFO("heater commands naming a non-existent head: " << offenders.size()
         << (offenders.empty() ? "" : " e.g. " + offenders.front()));
    CHECK(offenders.empty());
}

TEST_CASE("Heater commands keep the logical filament id on a non-IMEX printer",
          "[MultiFilament][IMEX][Regression]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter",                  "0.4,0.4" },
        { "printer_extruder_id",              "1,2" },
        { "printer_extruder_variant",         "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height",        "0,0" },
        { "is_imex",                          "0" },
        { "physical_extruder_map",            "1,0" },
        { "single_extruder_multi_material",   "0" },
        { "ooze_prevention",                  "1" },
        { "standby_temperature_delta",        "-50" },
        { "idle_temperature",                 "151,173" },
        { "nozzle_temperature_initial_layer", "215,215" },
        { "nozzle_temperature",               "240,240" },
        { "preheat_time",                     "0" },
        { "enable_prime_tower",               "0" },
        { "gcode_flavor",                     "klipper" },
    });

    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        { { "extruder", "1" } },
        { { "extruder", "2" } },
    };
    const std::string gcode = slice_with_object_overrides({ cube(20), cube(20) }, config, overrides);

    std::map<int, std::set<int>> tools_by_temp;
    std::istringstream           ss(gcode);
    for (std::string line; std::getline(ss, line);) {
        if (line.rfind("M104", 0) != 0 && line.rfind("M109", 0) != 0)
            continue;
        const size_t s = line.find('S');
        const size_t t = line.find(" T");
        if (s == std::string::npos || t == std::string::npos)
            continue;
        if (s + 1 >= line.size() || !std::isdigit((unsigned char) line[s + 1]))
            continue;
        if (t + 2 >= line.size() || !std::isdigit((unsigned char) line[t + 2]))
            continue;
        tools_by_temp[std::stoi(line.substr(s + 1))].insert(std::stoi(line.substr(t + 2)));
    }

    std::string seen;
    for (const auto& [temperature, tools] : tools_by_temp) {
        seen += " S" + std::to_string(temperature) + "->";
        for (int tool : tools)
            seen += "T" + std::to_string(tool);
    }
    INFO("heater commands naming a tool:" << seen);

    REQUIRE(tools_by_temp.count(151) == 1);
    REQUIRE(tools_by_temp.count(173) == 1);
    CHECK(tools_by_temp[151] == std::set<int>{ 0 });
    CHECK(tools_by_temp[173] == std::set<int>{ 1 });
}

TEST_CASE("An IMEX plate whose filament never routes to the Primary tool is blocked",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 6);
    config.set_deserialize_strict({ { "imex_parallel_mode", "copy" } });

    std::vector<TriangleMesh> meshes;
    meshes.push_back(cube(20));
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { { "extruder", "6" } } };

    Slic3r::Model model;
    Slic3r::Print print;
    init_print(std::move(meshes), print, model, config, &overrides, false);

    std::vector<StringObjectException> warnings;
    const StringObjectException err = print.validate(&warnings);

    REQUIRE_FALSE(err.string.empty());
    CHECK(err.string.find("T0") != std::string::npos);
    CHECK(err.string.find("T2") != std::string::npos);
}

TEST_CASE("An IMEX plate using a mixed filament is blocked", "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(8);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 8);
    config.set_deserialize_strict({
        { "imex_parallel_mode",               "copy" },
        { "filament_is_mixed",                "0,0,0,0,0,0,0,1" },
        { "filament_mixed_components",        ";;;;;;;1,5" },
        { "filament_mixed_sublayer_ratios",   ";;;;;;;" },
        { "filament_mixed_gradient",          "0,0,0,0,0,0,0,0" },
        { "filament_mixed_gradient_range",    ";;;;;;;" },
        { "filament_mixed_gradient_curve",    ";;;;;;;" },
        { "filament_mixed_gradient_per_part", "0,0,0,0,0,0,0,0" },
    });

    std::vector<TriangleMesh> meshes;
    meshes.push_back(cube(20));
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { { "extruder", "8" } } };

    Slic3r::Model model;
    Slic3r::Print print;
    init_print(std::move(meshes), print, model, config, &overrides, false);

    std::vector<StringObjectException> warnings;
    const StringObjectException err = print.validate(&warnings);

    REQUIRE_FALSE(err.string.empty());
    CHECK(err.string.find("Mixed filaments") != std::string::npos);
}

TEST_CASE("A mixed filament outranks the multi-color rule on the same plate",
          "[MultiFilament][IMEX][Regression]")
{
    const auto build = [](bool blend_slot_8) {
        DynamicPrintConfig config = multifilament_config(8);
        imex_7x4_printer(config);
        all_regions_on_filament(config, 8);
        config.set_deserialize_strict({
            { "imex_parallel_mode",               "copy" },
        { "filament_is_mixed",                "0,0,0,0,0,0,0,1" },
        { "filament_mixed_components",        ";;;;;;;1,5" },
        { "filament_mixed_sublayer_ratios",   ";;;;;;;" },
        { "filament_mixed_gradient",          "0,0,0,0,0,0,0,0" },
        { "filament_mixed_gradient_range",    ";;;;;;;" },
        { "filament_mixed_gradient_curve",    ";;;;;;;" },
        { "filament_mixed_gradient_per_part", "0,0,0,0,0,0,0,0" },
        });
        if (!blend_slot_8)
            config.set_deserialize_strict({ { "filament_is_mixed", "0,0,0,0,0,0,0,0" } });
        return config;
    };
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        { { "extruder", "8" } }, { { "extruder", "1" } },
    };
    const auto validate_plate = [&](const DynamicPrintConfig& config, Slic3r::Print& print,
                                    Slic3r::Model& model) {
        std::vector<TriangleMesh> meshes;
        meshes.push_back(cube(20));
        TriangleMesh second = cube(20);
        second.translate(30.0, 0.0, 0.0);
        meshes.push_back(second);
        init_print(std::move(meshes), print, model, config, &overrides, false);
        std::vector<StringObjectException> warnings;
        return print.validate(&warnings);
    };

    {
        Slic3r::Model model;
        Slic3r::Print print;
        const StringObjectException err = validate_plate(build(false), print, model);
        REQUIRE_FALSE(err.string.empty());
        CHECK(err.string.find("Multi-color") != std::string::npos);
    }

    {
        Slic3r::Model model;
        Slic3r::Print print;
        const StringObjectException err = validate_plate(build(true), print, model);
        REQUIRE_FALSE(err.string.empty());
        CHECK(err.string.find("not supported in IDEX/IQEX parallel modes") != std::string::npos);
        CHECK(err.string.find("Multi-color") == std::string::npos);
        CHECK(err.object == print.objects().front());
    }
}

TEST_CASE("A layer slider tool change counts as a color in a parallel mode", "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    config.set_deserialize_strict({ { "imex_parallel_mode", "copy" } });

    Slic3r::Model model;
    Slic3r::Print print;
    init_print({ cube(20) }, print, model, config);
    std::vector<StringObjectException> warnings;
    REQUIRE(print.validate(&warnings).string.empty());

    model.plates_custom_gcodes[model.curr_plate_index].gcodes.push_back(
        { 5.0, CustomGCode::Type::ToolChange, 5, "", "" });
    print.apply(model, print.full_print_config());
    REQUIRE_FALSE(print.validate(&warnings).string.empty());
}

TEST_CASE("An IMEX plate whose filament routes to the Primary tool validates",
          "[MultiFilament][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    config.set_deserialize_strict({ { "imex_parallel_mode", "copy" } });

    Slic3r::Model model;
    Slic3r::Print print;
    init_print({ cube(20) }, print, model, config);

    std::vector<StringObjectException> warnings;
    const StringObjectException err = print.validate(&warnings);

    CHECK(err.string.empty());
}
TEST_CASE("Each filament prints with its variant of the extruder's variant string", "[MultiFilament]")
{
    auto [variant_list, nozzle_volume_type, filament, temperature, resolved] = GENERATE(table<std::string, NozzleVolumeType, int, int, std::string>({
        { "Direct Drive Standard",                        nvtStandard, 1, 211, "211,223" },
        { "Direct Drive Standard",                        nvtStandard, 2, 223, "211,223" },
        { "Direct Drive High Flow",                       nvtHighFlow, 1, 239, "239,223" },
        { "Direct Drive High Flow",                       nvtHighFlow, 2, 223, "239,223" },
        { "Direct Drive Standard,Direct Drive High Flow", nvtHighFlow, 1, 239, "239,223" },
        { "Direct Drive Standard,Direct Drive High Flow", nvtHighFlow, 2, 223, "239,223" },
    }));
    DYNAMIC_SECTION(variant_list << " printer, " << get_nozzle_volume_type_string(nozzle_volume_type) << " nozzle, filament " << filament) {
        DynamicPrintConfig config = multifilament_config(2, {
            { "extruder_variant_list",            variant_list },
            { "filament_extruder_variant",        "Direct Drive Standard;Direct Drive High Flow;Direct Drive Standard" },
            { "filament_self_index",              "1,1,2" },
            { "nozzle_temperature",               "211,239,223" },
            { "nozzle_temperature_initial_layer", "211,239,223" },
            { "sparse_infill_filament_id",        filament },
            { "internal_solid_filament_id",       filament },
            { "top_surface_filament_id",          filament },
            { "bottom_surface_filament_id",       filament },
            { "outer_wall_filament_id",           filament },
            { "inner_wall_filament_id",           filament },
            { "enable_prime_tower",               0 },
            { "skirt_loops",                      0 },
            { "brim_type",                        "no_brim" },
            { "machine_start_gcode",              "; start temperature {nozzle_temperature_initial_layer[initial_extruder]}" },
        });
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { nozzle_volume_type };
        const std::string gcode = slice({ cube(20) }, config);

        std::set<int> temperatures;
        std::istringstream stream(gcode);
        for (std::string line; std::getline(stream, line);) {
            if (line.rfind("M104 ", 0) != 0 && line.rfind("M109 ", 0) != 0)
                continue;
            const size_t s = line.find(" S");
            if (s != std::string::npos && std::stoi(line.substr(s + 2)) > 0)
                temperatures.insert(std::stoi(line.substr(s + 2)));
        }
        CHECK(temperatures == std::set<int>{ temperature });
        CHECK(gcode.find("; start temperature " + std::to_string(temperature) + "\n") != std::string::npos);
        CHECK(gcode.find("; nozzle_temperature = " + resolved + "\n") != std::string::npos);
    }
}

static std::string constant_pressure_advance_model(const std::string &pa)
{
    return pa + ",1,1000\n" + pa + ",500,1000\n" + pa + ",1,100000\n" + pa + ",500,100000";
}

static std::set<std::string> pressure_advance_values(const std::string &gcode)
{
    const std::string token = "SET_PRESSURE_ADVANCE ADVANCE=";
    std::set<std::string> values;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);)
        if (line.rfind(token, 0) == 0)
            values.insert(line.substr(token.size(), line.find(';') - token.size()));
    return values;
}

TEST_CASE("Each filament sets the pressure advance of its extruder variant", "[MultiFilament]")
{
    auto [nozzle_volume_type, filament, pressure_advance, adaptive_pressure_advance] = GENERATE(table<NozzleVolumeType, int, std::string, std::string>({
        { nvtStandard, 1, "0.021", "0.012" },
        { nvtHighFlow, 1, "0.037", "0.034" },
        { nvtHighFlow, 2, "0.043", "0.056" },
    }));
    const bool adaptive = GENERATE(false, true);
    DYNAMIC_SECTION(get_nozzle_volume_type_string(nozzle_volume_type) << " nozzle, filament " << filament << (adaptive ? ", adaptive" : "")) {
        DynamicPrintConfig config = multifilament_config(2, {
            { "gcode_flavor",               "klipper" },
            { "extruder_variant_list",      "Direct Drive Standard,Direct Drive High Flow" },
            { "filament_extruder_variant",  "Direct Drive Standard;Direct Drive High Flow;Direct Drive Standard" },
            { "filament_self_index",        "1,1,2" },
            { "enable_pressure_advance",    "1,1,1" },
            { "pressure_advance",           "0.021,0.037,0.043" },
            { "adaptive_pressure_advance",  adaptive ? "1,1,1" : "0,0,0" },
            { "sparse_infill_filament_id",  filament },
            { "internal_solid_filament_id", filament },
            { "top_surface_filament_id",    filament },
            { "bottom_surface_filament_id", filament },
            { "outer_wall_filament_id",     filament },
            { "inner_wall_filament_id",     filament },
            { "enable_prime_tower",         0 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
            { "machine_start_gcode",        "; start pressure advance {pressure_advance[initial_extruder]}" },
        });
        config.option<ConfigOptionStrings>("adaptive_pressure_advance_model")->values = {
            constant_pressure_advance_model("0.012"), constant_pressure_advance_model("0.034"), constant_pressure_advance_model("0.056") };
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { nozzle_volume_type };
        const std::string gcode = slice({ cube(20) }, config);

        std::set<std::string> expected{ pressure_advance };
        if (adaptive)
            expected.insert(adaptive_pressure_advance);
        CHECK(pressure_advance_values(gcode) == expected);
        CHECK(gcode.find("; start pressure advance " + pressure_advance + "\n") != std::string::npos);
    }
}

static DynamicPrintConfig two_extruder_pressure_advance_config(const std::string &filament_map, const std::string &adaptive_pressure_advance,
                                                               int wall_filament, int infill_filament)
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "gcode_flavor",                   "klipper" },
        { "single_extruder_multi_material", 0 },
        { "nozzle_diameter",                "0.4,0.4" },
        { "extruder_printable_height",      "0,0" },
        { "printer_extruder_id",            "1,2" },
        { "printer_extruder_variant",       "Direct Drive Standard;Direct Drive High Flow" },
        { "extruder_variant_list",          "Direct Drive Standard;Direct Drive High Flow" },
        { "filament_map",                   filament_map },
        { "filament_extruder_variant",      "Direct Drive Standard;Direct Drive High Flow;Direct Drive Standard;Direct Drive High Flow" },
        { "filament_self_index",            "1,1,2,2" },
        { "enable_pressure_advance",        "1,1,1,1" },
        { "pressure_advance",               "0.021,0.037,0.043,0.049" },
        { "adaptive_pressure_advance",      adaptive_pressure_advance },
        { "sparse_infill_filament_id",      infill_filament },
        { "internal_solid_filament_id",     infill_filament },
        { "top_surface_filament_id",        infill_filament },
        { "bottom_surface_filament_id",     infill_filament },
        { "outer_wall_filament_id",         wall_filament },
        { "inner_wall_filament_id",         wall_filament },
        { "enable_prime_tower",             0 },
        { "skirt_loops",                    0 },
        { "brim_type",                      "no_brim" },
        { "machine_start_gcode",            "; start pressure advance {pressure_advance[initial_extruder]}" },
    });
    config.option<ConfigOptionStrings>("adaptive_pressure_advance_model")->values = {
        constant_pressure_advance_model("0.012"), constant_pressure_advance_model("0.034"),
        constant_pressure_advance_model("0.056"), constant_pressure_advance_model("0.078") };
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = { etDirectDrive, etDirectDrive };
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { nvtStandard, nvtHighFlow };
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    return config;
}

TEST_CASE("Each filament sets the pressure advance of its extruder variant on a two-extruder printer", "[MultiFilament]")
{
    auto [filament, extruder, pressure_advance, adaptive_pressure_advance] = GENERATE(table<int, int, std::string, std::string>({
        { 1, 1, "0.021", "0.012" },
        { 1, 2, "0.037", "0.034" },
        { 2, 1, "0.043", "0.056" },
        { 2, 2, "0.049", "0.078" },
    }));
    const bool adaptive = GENERATE(false, true);
    DYNAMIC_SECTION("filament " << filament << " on extruder " << extruder << (adaptive ? ", adaptive" : "")) {
        const std::string filament_map = filament == 1 ? std::to_string(extruder) + "," + std::to_string(3 - extruder) :
                                                         std::to_string(3 - extruder) + "," + std::to_string(extruder);
        const std::string gcode = slice({ cube(20) },
            two_extruder_pressure_advance_config(filament_map, adaptive ? "1,1,1,1" : "0,0,0,0", filament, filament));

        std::set<std::string> expected{ pressure_advance };
        if (adaptive)
            expected.insert(adaptive_pressure_advance);
        CHECK(pressure_advance_values(gcode) == expected);
        CHECK(gcode.find("; start pressure advance " + pressure_advance + "\n") != std::string::npos);
    }
}

TEST_CASE("Hotend placeholders resolve to -1 on a BBL printer and to the extruder index elsewhere", "[MultiFilament]")
{
    auto [is_bbl, start, changes] = GENERATE(table<bool, std::string, std::set<std::string>>({
        { true,  "; hotend start filament 0: -1 -1 -1",
          { "; hotend change filament 0: -1 -1", "; hotend change filament 1: -1 -1" } },
        { false, "; hotend start filament 0: 1 1 1",
          { "; hotend change filament -1: -1 1", "; hotend change filament 0: 1 0", "; hotend change filament 1: 0 1" } },
    }));
    DynamicPrintConfig config = two_extruder_pressure_advance_config("2,1", "0,0,0,0", 1, 2);
    config.set_key_value("machine_start_gcode", new ConfigOptionString(
        "; hotend start filament [initial_no_support_extruder]: [initial_no_support_hotend] [current_hotend] {first_non_support_hotend[0]}"));
    config.set_key_value("change_filament_gcode", new ConfigOptionString(
        "; hotend change filament [current_filament_id]: [current_hotend] [next_hotend]"));
    Print print;
    print.is_BBL_printer() = is_bbl;
    Model model;
    init_print({ cube(20) }, print, model, config);
    const std::string gcode = Slic3r::Test::gcode(print);

    INFO("BBL printer: " << is_bbl);
    std::vector<std::string> starts;
    std::set<std::string>    found;
    std::istringstream       stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind("; hotend start ", 0) == 0)
            starts.push_back(line);
        else if (line.rfind("; hotend change ", 0) == 0)
            found.insert(line);
    }
    CHECK(starts == std::vector<std::string>{ start });
    CHECK(found == changes);
}

TEST_CASE("Adaptive pressure advance on one extruder leaves the other extruder's pressure advance alone", "[MultiFilament]")
{
    auto [adaptive, expected] = GENERATE(table<std::string, std::set<std::string>>({
        { "1,0,0,0", { "0.021", "0.049", "0.012" } },
        { "0,0,0,1", { "0.021", "0.049", "0.078" } },
    }));
    DYNAMIC_SECTION("adaptive " << adaptive) {
        const std::string gcode = slice({ cube(20) }, two_extruder_pressure_advance_config("1,2", adaptive, 1, 2));
        CHECK(pressure_advance_values(gcode) == expected);
    }
}

static std::vector<std::string> pressure_advance_sequence(const std::string &gcode, int tool)
{
    const std::string token = "SET_PRESSURE_ADVANCE ADVANCE=";
    std::vector<std::string> values;
    int current_tool = 0;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.size() > 1 && line[0] == 'T' && std::isdigit((unsigned char) line[1]))
            current_tool = std::stoi(line.substr(1));
        else if (current_tool == tool && line.rfind(token, 0) == 0) {
            std::string value = line.substr(token.size(), line.find(';') - token.size());
            if (values.empty() || values.back() != value)
                values.push_back(std::move(value));
        }
    }
    return values;
}

TEST_CASE("Adaptive pressure advance predicts the same values after layers only a non-adaptive extruder prints", "[MultiFilament]")
{
    auto slice_with = [](const std::string &adaptive) {
        DynamicPrintConfig config = two_extruder_pressure_advance_config("1,2", adaptive, 1, 1);
        config.set_deserialize_strict({
            { "print_sequence",                "by object" },
            { "filament_max_volumetric_speed", "100" },
            { "slow_down_for_layer_cooling",   "0" },
            { "travel_speed",                  "120,40" },
            { "retraction_speed",              "30,40" },
            { "deretraction_speed",            "30,40" },
            { "initial_layer_speed",           "30,40" },
            { "initial_layer_infill_speed",    "60,40" },
        });
        auto &models = config.option<ConfigOptionStrings>("adaptive_pressure_advance_model")->values;
        models[0] = constant_pressure_advance_model("0.021");
        models[3] = "0.01,1,1000\n0.09,40,1000\n0.01,1,100000\n0.09,40,100000";
        return slice_with_object_overrides({ cube(20), cube(20) }, config,
            { {}, { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 }, { "sparse_infill_filament_id", 2 },
                    { "internal_solid_filament_id", 2 }, { "top_surface_filament_id", 2 }, { "bottom_surface_filament_id", 2 } } });
    };
    const std::vector<std::string> expected = pressure_advance_sequence(slice_with("1,0,0,1"), 1);
    REQUIRE(expected.size() > 2);
    CHECK(pressure_advance_sequence(slice_with("0,0,0,1"), 1) == expected);
}

TEST_CASE("Adaptive pressure advance on an unused extruder variant leaves the G-code unchanged", "[MultiFilament]")
{
    const std::string adaptive = GENERATE("0,1,0,0", "0,0,1,0");
    DYNAMIC_SECTION("adaptive " << adaptive) {
        auto masked = [](const std::string &gcode) {
            return std::regex_replace(gcode.substr(0, gcode.find("; CONFIG_BLOCK_START")), std::regex("; generated by .*|; pancakeslicer_build = .*\\n| id:\\d+"), "");
        };
        const std::string reference = masked(slice({ cube(20) }, two_extruder_pressure_advance_config("1,2", "0,0,0,0", 1, 2)));
        const std::string gcode     = masked(slice({ cube(20) }, two_extruder_pressure_advance_config("1,2", adaptive, 1, 2)));
        REQUIRE(reference.find("SET_PRESSURE_ADVANCE") != std::string::npos);
        CHECK(gcode == reference);
    }
}

static std::set<int> fan_speeds(const std::string &gcode, const std::string &command)
{
    std::set<int> speeds;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);)
        if (line.rfind(command, 0) == 0)
            if (const int pwm = std::stoi(line.substr(command.size())); pwm > 0)
                speeds.insert(int(std::lround(pwm * 100. / 255.)));
    return speeds;
}

TEST_CASE("Each filament cools with the fan speeds of its extruder variant", "[MultiFilament]")
{
    auto [nozzle_volume_type, filament, fan_min_speed, fan_max_speed, additional_fan_speed, range_high] = GENERATE(table<NozzleVolumeType, int, int, int, int, int>({
        { nvtStandard, 1, 15, 25, 10, 240 },
        { nvtHighFlow, 1, 35, 45, 20, 260 },
        { nvtHighFlow, 2, 55, 65, 40, 280 },
    }));
    const bool fast_layers = GENERATE(false, true);
    DYNAMIC_SECTION(get_nozzle_volume_type_string(nozzle_volume_type) << " nozzle, filament " << filament << (fast_layers ? ", fast layers" : ", slow layers")) {
        DynamicPrintConfig config = multifilament_config(2, {
            { "extruder_variant_list",          "Direct Drive Standard,Direct Drive High Flow" },
            { "filament_extruder_variant",      "Direct Drive Standard;Direct Drive High Flow;Direct Drive Standard" },
            { "filament_self_index",            "1,1,2" },
            { "fan_min_speed",                  "15,35,55" },
            { "fan_max_speed",                  "25,45,65" },
            { "additional_cooling_fan_speed",   "10,20,40" },
            { "nozzle_temperature_range_high",  "240,260,280" },
            { "auxiliary_fan",                  1 },
            { "reduce_fan_stop_start_freq",     "1,1" },
            { "slow_down_layer_time",           fast_layers ? "1000,1000" : "0,0" },
            { "fan_cooling_layer_time",         fast_layers ? "1000,1000" : "0,0" },
            { "slow_down_for_layer_cooling",    "0,0" },
            { "enable_overhang_bridge_fan",     "0,0" },
            { "sparse_infill_filament_id",      filament },
            { "internal_solid_filament_id",     filament },
            { "top_surface_filament_id",        filament },
            { "bottom_surface_filament_id",     filament },
            { "outer_wall_filament_id",         filament },
            { "inner_wall_filament_id",         filament },
            { "enable_prime_tower",             0 },
            { "skirt_loops",                    0 },
            { "brim_type",                      "no_brim" },
            { "machine_start_gcode",            "; start range high {nozzle_temperature_range_high[initial_extruder]}" },
        });
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { nozzle_volume_type };
        const std::string gcode = slice({ cube(20) }, config);

        CHECK(fan_speeds(gcode, "M106 S") == std::set<int>{ fast_layers ? fan_max_speed : fan_min_speed });
        CHECK(fan_speeds(gcode, "M106 P2 S") == std::set<int>{ additional_fan_speed });
        CHECK(gcode.find("; start range high " + std::to_string(range_high) + "\n") != std::string::npos);
    }
}
