#include "libslic3r/ArcFitter.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CommonDefs.hpp"
#include "ExtrusionEntity.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "GCodeWriter.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Polygon.hpp"
#include "PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Nonplanar/PatchTools.hpp"
#include "libslic3r/Nonplanar/MultiplanarConfirm.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r_version.h"
#include "GCodeProcessor.hpp"

#include <algorithm>
#include <array>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/constants.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <cmath>
#include <exception>
#include <deque>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <fast_float/fast_float.h>

#include <float.h>
#include <assert.h>
#include <memory>
#include <optional>
#include <iterator>
#include <functional>
#include <math.h>
#include <map>
#include <limits>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#if __has_include(<charconv>)
    #include <charconv>
    #include <utility>
#endif

#include <chrono>

#include "Geometry/ArcWelder.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"

static const float DEFAULT_TOOLPATH_WIDTH = 0.4f;
static const float DEFAULT_TOOLPATH_HEIGHT = 0.2f;

static const float INCHES_TO_MM = 25.4f;
static const float MMMIN_TO_MMSEC = 1.0f / 60.0f;
static const float DRAW_ARC_TOLERANCE = 0.0125f;

static const float DEFAULT_ACCELERATION = 1500.0f;
static const float DEFAULT_RETRACT_ACCELERATION = 1500.0f;
static const float DEFAULT_TRAVEL_ACCELERATION = 1250.0f;

static const size_t MIN_EXTRUDERS_COUNT = 5;
static const float DEFAULT_FILAMENT_DIAMETER = 1.75f;
static const int   DEFAULT_FILAMENT_HRC = 0;
static const float DEFAULT_FILAMENT_COST = 29.99f;
static const int   DEFAULT_FILAMENT_VITRIFICATION_TEMPERATURE = 0;
static const Slic3r::Vec3f DEFAULT_EXTRUDER_OFFSET = Slic3r::Vec3f::Zero();

namespace Slic3r {

const std::vector<std::string> GCodeProcessor::Reserved_Tags = {
    " FEATURE: ",
    " WIPE_START",
    " WIPE_END",
    " LAYER_HEIGHT: ",
    " LINE_WIDTH: ",
    " CHANGE_LAYER",
    " COLOR_CHANGE",
    " PAUSE_PRINTING",
    " CUSTOM_GCODE",
    "_GP_FIRST_LINE_M73_PLACEHOLDER",
    "_GP_LAST_LINE_M73_PLACEHOLDER",
    "_GP_ESTIMATED_PRINTING_TIME_PLACEHOLDER",
    "_GP_TOTAL_LAYER_NUMBER_PLACEHOLDER",
    " MANUAL_TOOL_CHANGE ",
    "_DURING_PRINT_EXHAUST_FAN",
    " WIPE_TOWER_START",
    " WIPE_TOWER_END",
    " PA_CHANGE:",
    "@PRINT_TIME_TOTAL_SEC@",
    "@PRINT_TIME_DAY@",
    "@PRINT_TIME_HOUR@",
    "@PRINT_TIME_MINUTE@",
    "@PRINT_TIME_SEC@",
    "@USED_FILAMENT_LENGTH@"
};

const std::vector<std::string> GCodeProcessor::Reserved_Tags_compatible = {
    "TYPE:",
    "WIPE_START",
    "WIPE_END",
    "HEIGHT:",
    "WIDTH:",
    "LAYER_CHANGE",
    "COLOR_CHANGE",
    "PAUSE_PRINT",
    "CUSTOM_GCODE",
    "_GP_FIRST_LINE_M73_PLACEHOLDER",
    "_GP_LAST_LINE_M73_PLACEHOLDER",
    "_GP_ESTIMATED_PRINTING_TIME_PLACEHOLDER",
    "_GP_TOTAL_LAYER_NUMBER_PLACEHOLDER",
    " MANUAL_TOOL_CHANGE ",
    "_DURING_PRINT_EXHAUST_FAN",
    " WIPE_TOWER_START",
    " WIPE_TOWER_END",
    " PA_CHANGE:",
    "@PRINT_TIME_TOTAL_SEC@",
    "@PRINT_TIME_DAY@",
    "@PRINT_TIME_HOUR@",
    "@PRINT_TIME_MINUTE@",
    "@PRINT_TIME_SEC@",
    "@USED_FILAMENT_LENGTH@"
};


const std::string GCodeProcessor::Flush_Start_Tag = " FLUSH_START";
const std::string GCodeProcessor::Flush_End_Tag = " FLUSH_END";
const std::string GCodeProcessor::VFlush_Start_Tag = " VFLUSH_START";
const std::string GCodeProcessor::VFlush_End_Tag  = " VFLUSH_END";

const std::string GCodeProcessor::External_Purge_Tag = " EXTERNAL_PURGE";

const std::string GCodeProcessor::Skippable_Start_Tag = " SKIPPABLE_START";
const std::string GCodeProcessor::Skippable_End_Tag   = " SKIPPABLE_END";
const std::string GCodeProcessor::Skippable_Type_Tag  = " SKIPTYPE: ";

const std::string GCodeProcessor::Machine_Start_GCode_End_Tag = " MACHINE_START_GCODE_END";
const std::string GCodeProcessor::Machine_End_GCode_Start_Tag = " MACHINE_END_GCODE_START";
const std::string GCodeProcessor::Nozzle_Change_Start_Tag      = " NOZZLE_CHANGE_START";
const std::string GCodeProcessor::Nozzle_Change_End_Tag        = " NOZZLE_CHANGE_END";
const std::string GCodeProcessor::Toolchange_Wipe_Tag          = " CP_TOOLCHANGE_WIPE";

const float GCodeProcessor::Wipe_Width = 0.05f;
const float GCodeProcessor::Wipe_Height = 0.05f;

bool GCodeProcessor::s_IsBBLPrinter = true;

static void set_option_value(ConfigOptionFloats& option, size_t id, float value)
{
    if (id < option.values.size())
        option.values[id] = static_cast<double>(value);
};

static float get_option_value(const ConfigOptionFloats& option, size_t id)
{
    return option.values.empty() ? 0.0f :
        ((id < option.values.size()) ? static_cast<float>(option.values[id]) : static_cast<float>(option.values.front()));
}

static float estimated_acceleration_distance(float initial_rate, float target_rate, float acceleration)
{
    return (acceleration == 0.0f) ? 0.0f : (sqr(target_rate) - sqr(initial_rate)) / (2.0f * acceleration);
}

static float intersection_distance(float initial_rate, float final_rate, float acceleration, float distance)
{
    return (acceleration == 0.0f) ? 0.0f : (2.0f * acceleration * distance - sqr(initial_rate) + sqr(final_rate)) / (4.0f * acceleration);
}

static float speed_from_distance(float initial_feedrate, float distance, float acceleration)
{
    const float value = std::max(0.0f, sqr(initial_feedrate) + 2.0f * acceleration * distance);
    return ::sqrt(value);
}

static float max_allowable_speed(float acceleration, float target_velocity, float distance)
{
    const float value = std::max(0.0f, sqr(target_velocity) - 2.0f * acceleration * distance);
    return std::sqrt(value);
}

static float acceleration_time_from_distance(float initial_feedrate, float distance, float acceleration)
{
    return (acceleration != 0.0f) ? (speed_from_distance(initial_feedrate, distance, acceleration) - initial_feedrate) / acceleration : 0.0f;
}

static int get_object_label_id(const std::string_view comment_1)
{
    std::string      comment(comment_1);
    auto pos = comment.find(":");
    std::string num_str = comment.substr(pos + 1);
    int id = -1;
    try {
        id = stoi(num_str);
    }
    catch (const std::exception &) {}
    return id;
}

static float get_z_height(const std::string_view comment_1)
{
    std::string comment(comment_1);
    auto pos = comment.find(":");
    std::string num_str = comment.substr(pos + 1);
    float print_z = 0.0f;
    try {
        print_z = stof(num_str);
    } catch (const std::exception &) {}
    return print_z;
}

CommandProcessor::CommandProcessor()
{
    root = std::make_unique<TrieNode>();
}

void CommandProcessor::register_command(const std::string& str, command_handler_t handler, bool early_quit)
{
    TrieNode* node = root.get();
    for (char ch : str) {
        auto iter = node->children.find(ch);
        if (iter == node->children.end()) {
            std::unique_ptr<TrieNode> new_node = std::make_unique<TrieNode>();
            auto raw_ptr = new_node.get();
            node->children[ch] = std::move(new_node);
            node = raw_ptr;
        }
        else {
            node = iter->second.get();
        }
    }
    if (node->handler != nullptr) {
        assert(false);
    }
    node->handler = handler;
    node->early_quit = early_quit;
}

bool CommandProcessor::process_comand(std::string_view cmd, const GCodeReader::GCodeLine& line)
{
    TrieNode* node = root.get();
    for (char ch : cmd) {
        if (node->early_quit && node->handler) {
            node->handler(line);
            return true;
        }
        auto iter = node->children.find(ch);
        if (iter == node->children.end()) {
            return false;
        }
        node = iter->second.get();
    }
    if (!node || !node->handler)
        return false;
    node->handler(line);
    return true;
}

void GCodeProcessor::CachedPosition::reset()
{
    std::fill(position.begin(), position.end(), FLT_MAX);
    feedrate = FLT_MAX;
}

void GCodeProcessor::CpColor::reset()
{
    counter = 0;
    current = 0;
}

float GCodeProcessor::Trapezoid::acceleration_time(float entry_feedrate, float acceleration) const
{
    return acceleration_time_from_distance(entry_feedrate, acceleration_distance(), acceleration);
}

float GCodeProcessor::Trapezoid::deceleration_time(float distance, float acceleration) const
{
    return acceleration_time_from_distance(cruise_feedrate, deceleration_distance(distance), -acceleration);
}

void GCodeProcessor::TimeBlock::calculate_trapezoid()
{
    const float cruise = smooth_cruise > 0.0f ? std::max(std::min(feedrate_profile.cruise, smooth_cruise),
                                                         std::max(feedrate_profile.entry, feedrate_profile.exit)) :
                                                feedrate_profile.cruise;
    float accelerate_distance = std::max(0.0f, estimated_acceleration_distance(feedrate_profile.entry, cruise, acceleration));
    const float decelerate_distance = std::max(0.0f, estimated_acceleration_distance(cruise, feedrate_profile.exit, -acceleration));
    float cruise_distance = distance - accelerate_distance - decelerate_distance;

    if (cruise_distance < 0.0f) {
        accelerate_distance = std::clamp(intersection_distance(feedrate_profile.entry, feedrate_profile.exit, acceleration, distance), 0.0f, distance);
        cruise_distance = 0.0f;
        trapezoid.cruise_feedrate = speed_from_distance(feedrate_profile.entry, accelerate_distance, acceleration);
    }
    else
        trapezoid.cruise_feedrate = cruise;

    trapezoid.accelerate_until = accelerate_distance;
    trapezoid.decelerate_after = accelerate_distance + cruise_distance;
}

void GCodeProcessor::TimeMachine::State::reset()
{
    feedrate = 0.0f;
    safe_feedrate = 0.0f;
    rotary_feedrate = {};
    axis_feedrate = { 0.0f, 0.0f, 0.0f, 0.0f };
    abs_axis_feedrate = { 0.0f, 0.0f, 0.0f, 0.0f };
    enter_direction = { 0.0f, 0.0f, 0.0f };
    exit_direction = { 0.0f, 0.0f, 0.0f };
    jd_unit_vec = { 0.0f, 0.0f, 0.0f, 0.0f };
}

void GCodeProcessor::TimeMachine::CustomGCodeTime::reset()
{
    needed = false;
    cache = 0.0f;
    times = std::vector<std::pair<CustomGCode::Type, float>>();
}

void GCodeProcessor::TimeMachine::reset()
{
    enabled = false;
    acceleration = 0.0f;
    max_acceleration = 0.0f;
    retract_acceleration = 0.0f;
    max_retract_acceleration = 0.0f;
    travel_acceleration = 0.0f;
    max_travel_acceleration = 0.0f;
    extrude_factor_override_percentage = 1.0f;
    time = 0.0f;
    stop_times = std::vector<StopTime>();
    curr.reset();
    prev.reset();
    gcode_time.reset();
    blocks = std::vector<TimeBlock>();
    g1_times_cache = std::vector<G1LinesCacheItem>();
    first_layer_time = 0.0f;
    prepare_time = 0.0f;
    m_additional_time_buffer.clear();
    host_feed = HostFeed();
}

float GCodeProcessor::TimeMachine::HostFeed::stall(const TimeBlock& block, double now)
{
    if (block.feed_interval <= 0.0f)
        return 0.0f;
    double arrival = last_arrival + (block.flags.arc_continuation ? 0. : block.feed_interval);
    if (starts.size() == PlannerBlocks)
        arrival = std::max(arrival, starts.front());
    float  stall = 0.0f;
    double start = now;
    if (block.flags.nonplanar && arrival > now) {
        stall = float(arrival - now);
        start = arrival;
    } else
        arrival = std::min(arrival, now);
    last_arrival = arrival;
    starts.push_back(start);
    if (starts.size() > PlannerBlocks)
        starts.pop_front();
    return stall;
}

static void planner_forward_pass_kernel(const GCodeProcessor::TimeBlock& prev, GCodeProcessor::TimeBlock& curr)
{
    if (!prev.flags.nominal_length && prev.feedrate_profile.entry < curr.feedrate_profile.entry) {
        const float new_entry_speed = max_allowable_speed(-prev.acceleration, prev.feedrate_profile.entry, prev.distance);
        if (new_entry_speed < curr.feedrate_profile.entry) {
            curr.feedrate_profile.entry = new_entry_speed;
            curr.flags.recalculate = true;
        }
    }
}

static void planner_reverse_pass_kernel(GCodeProcessor::TimeBlock& curr, const GCodeProcessor::TimeBlock& next)
{
    const float max_entry_speed = curr.max_entry_speed;
    if (curr.feedrate_profile.entry != max_entry_speed || next.flags.recalculate) {
        const float new_entry_speed = curr.flags.nominal_length ? max_entry_speed :
            std::min(max_entry_speed, max_allowable_speed(-curr.acceleration, next.feedrate_profile.entry, curr.distance));
        if (curr.feedrate_profile.entry != new_entry_speed) {
            curr.feedrate_profile.entry = new_entry_speed;
            curr.flags.recalculate = true;
        }
    }
}

static void klipper_smooth_cruise(std::vector<GCodeProcessor::TimeBlock>& blocks)
{
    if (blocks.empty() || blocks.front().smooth_acceleration <= 0.0f)
        return;
    std::vector<float> s2(blocks.size() + 1);
    s2[0] = sqr(blocks.front().feedrate_profile.entry);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const float limit = sqr(blocks[i].max_entry_speed);
        if (i > 0)
            s2[i] = std::min(limit, s2[i - 1] + 2.0f * blocks[i - 1].distance * blocks[i - 1].smooth_acceleration);
    }
    s2.back() = sqr(blocks.back().safe_feedrate);
    for (size_t i = blocks.size(); i-- > 0;)
        s2[i] = std::min(s2[i], s2[i + 1] + 2.0f * blocks[i].distance * blocks[i].smooth_acceleration);
    for (size_t i = 0; i < blocks.size(); ++i) {
        GCodeProcessor::TimeBlock &b = blocks[i];
        if (b.smooth_acceleration <= 0.0f)
            continue;
        const float cap = std::sqrt(0.5f * (s2[i] + s2[i + 1]) + b.distance * b.smooth_acceleration);
        if (cap != b.smooth_cruise) {
            b.smooth_cruise          = cap;
            b.flags.recalculate      = true;
        }
    }
}

static void recalculate_trapezoids(std::vector<GCodeProcessor::TimeBlock>& blocks)
{
    GCodeProcessor::TimeBlock* curr = nullptr;
    GCodeProcessor::TimeBlock* next = nullptr;

    for (size_t i = 0; i < blocks.size(); ++i) {
      GCodeProcessor::TimeBlock& b = blocks[i];

        curr = next;
        next = &b;

        if (curr != nullptr) {
            if (curr->flags.recalculate || next->flags.recalculate) {
                curr->feedrate_profile.exit = next->feedrate_profile.entry;
                curr->calculate_trapezoid();
                curr->flags.recalculate = false;
            }
        }
    }

    if (next != nullptr) {
        next->feedrate_profile.exit = next->safe_feedrate;
        next->calculate_trapezoid();
        next->flags.recalculate = false;
    }
}

GCodeProcessor::TimeMachine::AdditionalBuffer GCodeProcessor::TimeMachine::merge_adjacent_additional_time_blocks(const AdditionalBuffer& buffer)
{
    AdditionalBuffer merged;
    if (buffer.empty())
        return merged;

    AdditionalBufferBlock current_block = buffer.front();
    for (size_t idx = 1; idx < buffer.size(); ++idx) {
        const AdditionalBufferBlock& next_block = buffer[idx];
        if (current_block.first == next_block.first)
            current_block.second += next_block.second;
        else {
            merged.push_back(current_block);
            current_block = next_block;
        }
    }
    merged.push_back(current_block);
    return merged;
}

void GCodeProcessor::TimeMachine::calculate_time(GCodeProcessorResult& result, PrintEstimatedStatistics::ETimeMode mode, size_t keep_last_n_blocks, float additional_time, EMoveType target_move_type, bool is_final)
{
    if (!enabled)
        return;
    const bool drain_final = is_final && !m_additional_time_buffer.empty();
    if (blocks.size() < 2 && !drain_final) {
        if (additional_time > 0.0f)
            m_additional_time_buffer.emplace_back(target_move_type, additional_time);
        return;
    }

    assert(keep_last_n_blocks <= blocks.size());

    const bool synchronize_at_end = result.nonplanar && additional_time > 0.0f &&
                                    target_move_type == EMoveType::Noop && blocks.size() > keep_last_n_blocks;
    AdditionalBuffer additional_buffer = m_additional_time_buffer;
    if (additional_time > 0.0f && !synchronize_at_end)
        additional_buffer.emplace_back(target_move_type, additional_time);
    additional_buffer = merge_adjacent_additional_time_blocks(additional_buffer);

    for (int i = static_cast<int>(blocks.size()) - 1; i > 0; --i) {
        planner_reverse_pass_kernel(blocks[i - 1], blocks[i]);
    }

    for (size_t i = 0; i + 1 < blocks.size(); ++i) {
        planner_forward_pass_kernel(blocks[i], blocks[i + 1]);
    }

    klipper_smooth_cruise(blocks);
    recalculate_trapezoids(blocks);

    const size_t n_blocks_process = blocks.size() - keep_last_n_blocks;
    size_t additional_buffer_idx = 0;
    for (size_t i = 0; i < n_blocks_process; ++i) {
        const TimeBlock& block = blocks[i];
        float block_time = block.time() + host_feed.stall(block, time);
        if (synchronize_at_end && i + 1 == n_blocks_process)
            block_time += additional_time;
        if (additional_buffer_idx < additional_buffer.size()) {
            const EMoveType buf_move_type = additional_buffer[additional_buffer_idx].first;
            if (buf_move_type == EMoveType::Noop || buf_move_type == block.move_type) {
                block_time += additional_buffer[additional_buffer_idx].second;
                ++additional_buffer_idx;
            }
        }

        time += double(block_time);
        if (block.skippable_type != SkipType::stNone)
            result.skippable_part_time[block.skippable_type] += block.time();
        result.moves[block.move_id].time[static_cast<size_t>(mode)] = block_time;
        gcode_time.cache += block_time;
        if (block.flags.prepare_stage)
            prepare_time += block_time;

        if (block.layer_id == 1)
            first_layer_time += block_time;

        if (mode == PrintEstimatedStatistics::ETimeMode::Normal) {
            GCodeProcessorResult::MoveVertex& curr_move = result.moves[block.move_id];
            if (curr_move.type != EMoveType::Extrude &&
                curr_move.type != EMoveType::Travel &&
                curr_move.type != EMoveType::Wipe)
              continue;

            assert(curr_move.actual_feedrate == 0.0f);

            GCodeProcessorResult::MoveVertex& prev_move = result.moves[block.move_id - 1];
            const bool interpolate = (prev_move.type == curr_move.type);
            if (!interpolate &&
                prev_move.type != EMoveType::Extrude &&
                prev_move.type != EMoveType::Travel &&
                prev_move.type != EMoveType::Wipe)
                prev_move.actual_feedrate = block.feedrate_profile.entry;

            if (EPSILON < block.trapezoid.accelerate_until && block.trapezoid.accelerate_until < block.distance - EPSILON) {
                const float t = block.trapezoid.accelerate_until / block.distance;
                const Vec3f position = prev_move.position + t * (curr_move.position - prev_move.position);
                if ((position - prev_move.position).norm() > EPSILON &&
                    (position - curr_move.position).norm() > EPSILON) {
                    const float delta_extruder = interpolate ? lerp(prev_move.delta_extruder, curr_move.delta_extruder, t) : curr_move.delta_extruder;
                    const float feedrate = curr_move.feedrate;
                    const float width = interpolate ? lerp(prev_move.width, curr_move.width, t) : curr_move.width;
                    const float height = interpolate ? lerp(prev_move.height, curr_move.height, t) : curr_move.height;
                    const float mm3_per_mm = curr_move.mm3_per_mm;
                    const float fan_speed = result.state(curr_move).fan_speed;
                    const float temperature = interpolate ? lerp(result.state(prev_move).temperature, result.state(curr_move).temperature, t) :
                                                            result.state(curr_move).temperature;
                    actual_speed_moves.push_back({
                        block.move_id,
                        position,
                        block.trapezoid.cruise_feedrate,
                        delta_extruder,
                        feedrate,
                        width,
                        height,
                        mm3_per_mm,
                        fan_speed,
                        temperature
                    });
                }
            }

            const bool has_deceleration = block.trapezoid.deceleration_distance(block.distance) > EPSILON;
            if (has_deceleration && block.trapezoid.decelerate_after > block.trapezoid.accelerate_until + EPSILON) {
                const float t = block.trapezoid.decelerate_after / block.distance;
                const Vec3f position = prev_move.position + t * (curr_move.position - prev_move.position);
                if ((position - prev_move.position).norm() > EPSILON &&
                    (position - curr_move.position).norm() > EPSILON) {
                    const float delta_extruder = interpolate ? lerp(prev_move.delta_extruder, curr_move.delta_extruder, t) : curr_move.delta_extruder;
                    const float feedrate = curr_move.feedrate;
                    const float width = interpolate ? lerp(prev_move.width, curr_move.width, t) : curr_move.width;
                    const float height = interpolate ? lerp(prev_move.height, curr_move.height, t) : curr_move.height;
                    const float mm3_per_mm = curr_move.mm3_per_mm;
                    const float fan_speed = result.state(curr_move).fan_speed;
                    const float temperature = interpolate ? lerp(result.state(prev_move).temperature, result.state(curr_move).temperature, t) :
                                                            result.state(curr_move).temperature;
                    actual_speed_moves.push_back({
                        block.move_id,
                        position,
                        block.trapezoid.cruise_feedrate,
                        delta_extruder,
                        feedrate,
                        width,
                        height,
                        mm3_per_mm,
                        fan_speed,
                        temperature
                    });
                }
            }

            const bool is_cruise_only = block.trapezoid.is_cruise_only(block.distance);
            actual_speed_moves.push_back({
                block.move_id,
                std::nullopt,
                (is_cruise_only || !has_deceleration) ? block.trapezoid.cruise_feedrate : block.feedrate_profile.exit,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt
            });
        }
        g1_times_cache.push_back({ block.g1_line_id, block.remaining_internal_g1_lines, float(time) });
        auto it_stop_time = std::lower_bound(stop_times.begin(), stop_times.end(), block.g1_line_id,
            [](const StopTime& t, unsigned int value) { return t.g1_line_id < value; });
        if (it_stop_time != stop_times.end() && it_stop_time->g1_line_id == block.g1_line_id)
            it_stop_time->elapsed_time = float(time);
    }

    m_additional_time_buffer.clear();
    if (additional_buffer_idx < additional_buffer.size()) {
        if (is_final) {
            float leftover = 0.0f;
            for (size_t i = additional_buffer_idx; i < additional_buffer.size(); ++i)
                leftover += additional_buffer[i].second;
            BOOST_LOG_TRIVIAL(debug) << "calculate_time(is_final): leftover=" << leftover
                << "s from " << (additional_buffer.size() - additional_buffer_idx) << " items";
            time += double(leftover);
            gcode_time.cache += leftover;
        } else {
            m_additional_time_buffer.insert(m_additional_time_buffer.end(),
                                            additional_buffer.begin() + additional_buffer_idx,
                                            additional_buffer.end());
        }
    }

    if (keep_last_n_blocks) {
        blocks.erase(blocks.begin(), blocks.begin() + n_blocks_process);

        TimeBlock &first_block = blocks.front();
        first_block.max_entry_speed = first_block.feedrate_profile.entry;
    } else {
        blocks.clear();
    }
}

void GCodeProcessor::TimeProcessor::reset()
{
    extruder_unloaded = true;
    machine_envelope_processing_enabled = false;
    machine_limits = MachineEnvelopeConfig();
    filament_load_times = 0.0f;
    filament_unload_times = 0.0f;
    machine_tool_change_time = 0.0f;


    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        machines[i].reset();
    }
    machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].enabled = true;
}

#if __has_include(<charconv>)
    template <typename T, typename = void>
    struct is_from_chars_convertible : std::false_type {};
    template <typename T>
    struct is_from_chars_convertible<T, std::void_t<decltype(std::from_chars(std::declval<const char*>(), std::declval<const char*>(), std::declval<T&>()))>> : std::true_type {};
#endif

template<typename T>
[[nodiscard]] static inline bool parse_number(const std::string_view sv, T &out)
{
#if __has_include(<charconv>)
    if constexpr (is_from_chars_convertible<T>::value) {
        auto str_end = sv.data() + sv.size();
        auto [end_ptr, error_code] = std::from_chars(sv.data(), str_end, out);
        return error_code == std::errc() && end_ptr == str_end;
    }
    else
#endif
    {
        try {
            assert(sv.size() < 1024);
            assert(sv.data() != nullptr);
            std::string str { sv };
            size_t read = 0;
            if constexpr (std::is_same_v<T, int>)
                out = std::stoi(str, &read);
            else if constexpr (std::is_same_v<T, long>)
                out = std::stol(str, &read);
            else if constexpr (std::is_same_v<T, float>)
                out = string_to_double_decimal_point(str, &read);
            else if constexpr (std::is_same_v<T, double>)
                out = string_to_double_decimal_point(str, &read);
            return str.size() == read;
        } catch (...) {
            return false;
        }
    }
}

namespace {
class GCodeFileWriter
{
public:
    GCodeFileWriter(FilePtr& out, const std::string& out_path, std::vector<size_t>& lines_ends, const char* error_message)
        : m_out(out), m_out_path(out_path), m_lines_ends(lines_ends), m_error_message(error_message)
    {}
    ~GCodeFileWriter() { assert(m_buffer.empty() || std::uncaught_exceptions() > 0); }

    void append(std::string_view text)
    {
        const size_t text_pos = m_file_pos + m_buffer.size();
        for (size_t i = text.find('\n'); i != std::string_view::npos; i = text.find('\n', i + 1))
            m_lines_ends.emplace_back(text_pos + i + 1);
        m_buffer += text;
        if (m_buffer.size() >= GCodeProcessor::Output_Block_Size)
            flush();
    }

    void flush()
    {
        if (m_buffer.empty())
            return;
        fwrite(m_buffer.data(), 1, m_buffer.size(), m_out.f);
        if (ferror(m_out.f)) {
            m_out.close();
            boost::nowide::remove(m_out_path.c_str());
            throw Slic3r::RuntimeError(m_error_message);
        }
        m_file_pos += m_buffer.size();
        m_buffer.clear();
    }

private:
    FilePtr&             m_out;
    const std::string&   m_out_path;
    std::vector<size_t>& m_lines_ends;
    const char*          m_error_message;
    std::string          m_buffer;
    size_t               m_file_pos{0};
};
}
class ExportLines
{
public:
    struct Backtrace
    {
        float time{60.0f};
        int   steps{10};
        float time_step() const { return time / float(steps); }
    };

    enum class EWriteType { BySize, ByTime };

private:
    struct LineData
    {
        std::string                                                                        line;
        std::array<float, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> times{0.0f, 0.0f};
    };

    enum ETimeMode {
        Normal  = static_cast<int>(PrintEstimatedStatistics::ETimeMode::Normal),
        Stealth = static_cast<int>(PrintEstimatedStatistics::ETimeMode::Stealth)
    };

#ifndef NDEBUG
    class Statistics
    {
        ExportLines& m_parent;
        size_t       m_max_size{0};
        size_t       m_lines_count{0};
        size_t       m_max_lines_count{0};

    public:
        explicit Statistics(ExportLines& parent) : m_parent(parent) {}

        void add_line(size_t line_size)
        {
            ++m_lines_count;
            m_max_size        = std::max(m_max_size, m_parent.get_size() + line_size);
            m_max_lines_count = std::max(m_max_lines_count, m_lines_count);
        }

        void remove_line() { --m_lines_count; }
        void remove_all_lines() { m_lines_count = 0; }
    };

    Statistics m_statistics;
#endif

    EWriteType m_write_type{EWriteType::BySize};
    const std::array<GCodeProcessor::TimeMachine, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)>& m_machines;
    GCodeFileWriter& m_writer;
    std::array<float, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> m_times{0.0f, 0.0f};
    size_t m_size{0};

    std::deque<LineData> m_lines;
    size_t               m_added_lines_counter{0};
    std::vector<std::pair<size_t, size_t>> m_gcode_lines_map;

    size_t m_times_cache_id{0};

public:
    ExportLines(EWriteType type, const std::array<GCodeProcessor::TimeMachine, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)>& machines, GCodeFileWriter& writer)
#ifndef NDEBUG
        : m_statistics(*this)
        , m_write_type(type)
        , m_machines(machines)
        , m_writer(writer){}
#else
        : m_write_type(type), m_machines(machines), m_writer(writer)
    {}
#endif
        unsigned int update(const std::string& line, size_t lines_counter, size_t g1_lines_counter)
    {
        unsigned int ret = 0;
        m_gcode_lines_map.push_back({lines_counter, 0});

        if (GCodeReader::GCodeLine::cmd_is(line, "G0") || GCodeReader::GCodeLine::cmd_is(line, "G1") ||
            GCodeReader::GCodeLine::cmd_is(line, "G2") || GCodeReader::GCodeLine::cmd_is(line, "G3") ||
            GCodeReader::GCodeLine::cmd_is(line, "G28"))
            ++g1_lines_counter;
        else
            return ret;

        auto init_it = m_machines[Normal].g1_times_cache.begin() + m_times_cache_id;
        auto it      = init_it;
        while (it != m_machines[Normal].g1_times_cache.end() && it->id < g1_lines_counter) {
            ++it;
            ++m_times_cache_id;
        }

        if (it == m_machines[Normal].g1_times_cache.end() || it->id > g1_lines_counter)
            return ret;

        if (GCodeReader::GCodeLine::cmd_is(line, "G2") || GCodeReader::GCodeLine::cmd_is(line, "G3")) {
            while (it != m_machines[Normal].g1_times_cache.end() && it->remaining_internal_g1_lines > 0) {
                ++it;
                ++m_times_cache_id;
                ++g1_lines_counter;
                ++ret;
            }
        }

        if (it != m_machines[Normal].g1_times_cache.end() && it->id == g1_lines_counter) {
            m_times[Normal] = it->elapsed_time;
            if (!m_machines[Stealth].g1_times_cache.empty())
                m_times[Stealth] = (m_machines[Stealth].g1_times_cache.begin() +
                                    std::distance(m_machines[Normal].g1_times_cache.begin(), it))
                                       ->elapsed_time;
        }

        return ret;
    }

    void append_line(const std::string& line, const bool ignore_from_move = false)
    {
        if (line.empty())
            return;

        if (m_write_type == EWriteType::ByTime) {
            m_lines.push_back({line, m_times});
#ifndef NDEBUG
            m_statistics.add_line(line.length());
#endif
            m_size += line.length();
        } else
            m_writer.append(line);
        ++m_added_lines_counter;
        if (!ignore_from_move) {
            assert(!m_gcode_lines_map.empty());
            m_gcode_lines_map.back().second = m_added_lines_counter;
        }
    }

    void insert_lines(const Backtrace&                                                    backtrace,
                      const std::string&                                                  cmd,
                      std::function<std::string(unsigned int, const std::vector<float>&)> line_inserter,
                      std::function<std::string(const std::string&)>                      line_replacer)
    {
        auto is_start_pos = [](const std::string& curr_cmd) {
            return ascii_iequals(curr_cmd, "G28") || ascii_iequals(curr_cmd, "G29") || ascii_iequals(curr_cmd, "PRINT_START") ||
                   ascii_iequals(curr_cmd, "START_PRINT");
        };
        assert(!m_lines.empty());
        const float time_step           = backtrace.time_step();
        size_t      rev_it_dist         = 0;
        float       last_time_insertion = 0.0f;
        for (int i = 0; i < backtrace.steps; ++i) {
            const float backtrace_time_i = (i + 1) * time_step;
            const float time_threshold_i = m_times[Normal] - backtrace_time_i;
            auto        rev_it           = m_lines.rbegin() + rev_it_dist;
            auto        start_rev_it     = rev_it;

            std::string curr_cmd = GCodeReader::GCodeLine::extract_cmd(rev_it->line);
            while (rev_it != m_lines.rend() && rev_it->times[Normal] > time_threshold_i && curr_cmd != cmd && !is_start_pos(curr_cmd)) {
                rev_it->line = line_replacer(rev_it->line);
                ++rev_it;
                if (rev_it != m_lines.rend())
                    curr_cmd = GCodeReader::GCodeLine::extract_cmd(rev_it->line);
            }

            if (rev_it != m_lines.rend() && (curr_cmd == cmd || is_start_pos(curr_cmd)))
                break;

            if (rev_it != m_lines.rend() && rev_it != start_rev_it && rev_it->times[Normal] != last_time_insertion) {
                last_time_insertion = rev_it->times[Normal];
                std::vector<float> time_diffs;
                time_diffs.push_back(m_times[Normal] - last_time_insertion);
                if (!m_machines[Stealth].g1_times_cache.empty())
                    time_diffs.push_back(m_times[Stealth] - rev_it->times[Stealth]);
                const std::string out_line = line_inserter(i + 1, time_diffs);
                rev_it_dist                = std::distance(m_lines.rbegin(), rev_it) + 1;
                m_lines.insert(rev_it.base(), {out_line, rev_it->times});
#ifndef NDEBUG
                m_statistics.add_line(out_line.length());
#endif
                m_size += out_line.length();
                for (auto map_it = m_gcode_lines_map.rbegin(); map_it != m_gcode_lines_map.rbegin() + rev_it_dist - 1; ++map_it) {
                    ++map_it->second;
                }

                ++m_added_lines_counter;
            }
        }
    }

    void write(float backtrace_time)
    {
        if (m_write_type != EWriteType::ByTime)
            return;

        while (!m_lines.empty() && m_lines.front().times[Normal] < m_times[Normal] - backtrace_time) {
            const LineData& data = m_lines.front();
            m_writer.append(data.line);
            m_size -= data.line.length();
            m_lines.pop_front();
#ifndef NDEBUG
            m_statistics.remove_line();
#endif
        }
    }

    void flush()
    {
        while (!m_lines.empty()) {
            m_writer.append(m_lines.front().line);
            m_lines.pop_front();
        }
        m_size = 0;
#ifndef NDEBUG
        m_statistics.remove_all_lines();
#endif
        m_writer.flush();
    }

    void synchronize_moves(GCodeProcessorResult& result) const
    {
        auto it = m_gcode_lines_map.begin();
        for (GCodeProcessorResult::MoveVertex& move : result.moves) {
            while (it != m_gcode_lines_map.end() && it->first < move.gcode_id) {
                ++it;
            }
            if (it != m_gcode_lines_map.end() && it->first == move.gcode_id)
                move.gcode_id = it->second;
        }
    }

    unsigned int remap_gcode_id(unsigned int gcode_id) const
    {
        if (gcode_id == static_cast<unsigned int>(-1))
            return gcode_id;
        auto it = std::lower_bound(m_gcode_lines_map.begin(), m_gcode_lines_map.end(), static_cast<size_t>(gcode_id),
            [](const std::pair<size_t, size_t>& e, size_t v) { return e.first < v; });
        if (it != m_gcode_lines_map.end() && it->first == static_cast<size_t>(gcode_id))
            return static_cast<unsigned int>(it->second);
        return gcode_id;
    }

    size_t get_size() const { return m_size; }

    void reserve(size_t lines_count) { m_gcode_lines_map.reserve(lines_count); }
};

void GCodeProcessor::run_post_process()
{
    FilePtr in{ boost::nowide::fopen(m_result.filename.c_str(), "rb") };
    if (in.f == nullptr)
        throw Slic3r::RuntimeError(std::string("GCode processor post process export failed.\nCannot open file for reading.\n"));

    std::string out_path = m_result.filename + ".postprocess";
    FilePtr out{ boost::nowide::fopen(out_path.c_str(), "wb") };
    if (out.f == nullptr)
        throw Slic3r::RuntimeError(std::string("GCode processor post process export failed.\nCannot open file for writing.\n"));

    std::vector<double> filament_mm(m_result.filaments_count, 0.0);
    std::vector<double> filament_cm3(m_result.filaments_count, 0.0);
    std::vector<double> filament_g(m_result.filaments_count, 0.0);
    std::vector<double> filament_cost(m_result.filaments_count, 0.0);

    double filament_total_g = 0.0;
    double filament_total_cost = 0.0;

    for (const auto& [id, volume] : m_result.print_statistics.total_volumes_per_extruder) {
        filament_mm[id] = volume / (static_cast<double>(M_PI) * sqr(0.5 * m_result.filament_diameters[id]));
        filament_cm3[id] = volume * 0.001;
        filament_g[id] = filament_cm3[id] * double(m_result.filament_densities[id]);
        filament_cost[id] = filament_g[id] * double(m_result.filament_costs[id]) * 0.001;
        filament_total_g += filament_g[id];
        filament_total_cost += filament_cost[id];
    }

    double total_g_wipe_tower = m_print->print_statistics().total_wipe_tower_filament;


    auto time_in_minutes = [](float time_in_seconds) {
        assert(time_in_seconds >= 0.f);
        return int((time_in_seconds + 0.5f) / 60.0f);
    };

    auto time_in_last_minute = [](float time_in_seconds) {
        assert(time_in_seconds <= 60.0f);
        return time_in_seconds / 60.0f;
    };

    auto format_line_M73_main = [this](const std::string& mask, int percent, int time) {
        if(this->m_disable_m73)
            return std::string("");

        char line_M73[64];
        sprintf(line_M73, mask.c_str(),
            std::to_string(percent).c_str(),
            std::to_string(time).c_str());
        return std::string(line_M73);
    };

    auto format_line_M73_stop_int = [this](const std::string& mask, int time) {
        if (this->m_disable_m73)
            return std::string("");
        char line_M73[64];
        sprintf(line_M73, mask.c_str(), std::to_string(time).c_str());
        return std::string(line_M73);
    };

    auto format_line_exhaust_fan_control = [](const std::string& mask,int fan_index,int percent) {
        char line_fan[64] = { 0 };
        sprintf(line_fan,mask.c_str(),
            std::to_string(fan_index).c_str(),
            std::to_string(int((percent/100.0)*255)).c_str());
        return std::string(line_fan);
    };

    auto format_time_float = [](float time) {
        return Slic3r::float_to_string_decimal_point(time, 2);
    };

    auto format_line_M73_stop_float = [format_time_float](const std::string& mask, float time) {
        char line_M73[64];
        sprintf(line_M73, mask.c_str(), format_time_float(time).c_str());
        return std::string(line_M73);
    };

    std::string gcode_line;
    size_t g1_lines_counter = 0;
    std::array<std::pair<int, int>, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> last_exported_main;
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        last_exported_main[i] = { 0, time_in_minutes(m_time_processor.machines[i].time) };
    }

    std::array<int, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> last_exported_stop;
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        last_exported_stop[i] = time_in_minutes(m_time_processor.machines[i].time);
    }

    m_result.lines_ends.clear();
    GCodeFileWriter writer(out, out_path, m_result.lines_ends, "GCode processor post process export failed.\nIs the disk full?");
    ExportLines export_line(m_result.backtrace_enabled ? ExportLines::EWriteType::ByTime : ExportLines::EWriteType::BySize,
        m_time_processor.machines, writer);
    export_line.reserve(m_line_id);

    auto process_placeholders = [&](std::string& gcode_line) {
        bool processed = false;

        auto line = std::string_view(gcode_line).substr(0, gcode_line.length() - 1);

        if (line.length() > 1) {
            line = line.substr(1);
            if (line == reserved_tag(ETags::First_Line_M73_Placeholder) || line == reserved_tag(ETags::Last_Line_M73_Placeholder)) {
                for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
                    const TimeMachine& machine = m_time_processor.machines[i];
                    if (machine.enabled) {
                        export_line.append_line(format_line_M73_main(machine.line_m73_main_mask.c_str(),
                            (line == reserved_tag(ETags::First_Line_M73_Placeholder)) ? 0 : 100,
                            (line == reserved_tag(ETags::First_Line_M73_Placeholder)) ? time_in_minutes(machine.time) : 0));
                        processed = true;

                        if (line == reserved_tag(ETags::First_Line_M73_Placeholder) && !machine.stop_times.empty()) {
                            const int to_export_stop = time_in_minutes(machine.stop_times.front().elapsed_time);
                            export_line.append_line(format_line_M73_stop_int(machine.line_m73_stop_mask.c_str(), to_export_stop));
                            last_exported_stop[i] = to_export_stop;
                        }
                    }
                }
            } else if (line == reserved_tag(ETags::Estimated_Printing_Time_Placeholder)) {
                for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
                    const TimeMachine&                  machine = m_time_processor.machines[i];
                    PrintEstimatedStatistics::ETimeMode mode    = static_cast<PrintEstimatedStatistics::ETimeMode>(i);
                    if (mode == PrintEstimatedStatistics::ETimeMode::Normal || machine.enabled) {
                        char buf[128];
                        if (!s_IsBBLPrinter)
                            sprintf(buf, "; estimated printing time (%s mode) = %s\n",
                                    (mode == PrintEstimatedStatistics::ETimeMode::Normal) ? "normal" : "silent",
                                    get_time_dhms(machine.time).c_str());
                        else {
                            sprintf(buf, "; model printing time: %s; total estimated time: %s\n",
                                    get_time_dhms(machine.time - machine.prepare_time).c_str(), get_time_dhms(machine.time).c_str());
                        }
                        export_line.append_line(buf);
                    }
                }
                for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
                    const TimeMachine&                  machine = m_time_processor.machines[i];
                    PrintEstimatedStatistics::ETimeMode mode    = static_cast<PrintEstimatedStatistics::ETimeMode>(i);
                    if (mode == PrintEstimatedStatistics::ETimeMode::Normal || machine.enabled) {
                        char buf[128];
                        sprintf(buf, "; estimated first layer printing time (%s mode) = %s\n",
                                (mode == PrintEstimatedStatistics::ETimeMode::Normal) ? "normal" : "silent",
                                get_time_dhms(machine.prepare_time).c_str());
                        export_line.append_line(buf);
                        processed = true;
                    }
                }
            }
            else if (line == reserved_tag(ETags::Total_Layer_Number_Placeholder)) {
                char buf[128];
                sprintf(buf, "; total layer number: %u\n", m_layer_id);
                export_line.append_line(buf);
                processed = true;
            }
        }

        return processed;
    };

    auto process_used_filament = [&](std::string& gcode_line) {
        if (gcode_line.size() < 8 || gcode_line[0] != ';' || gcode_line[1] != ' ')
            return false;
        if (const char c = gcode_line[2]; c != 'f' && c != 't')
            return false;
        auto process_tag = [](std::string& gcode_line, const std::string_view tag, const std::vector<double>& values) {
            if (boost::algorithm::starts_with(gcode_line, tag)) {
                gcode_line = tag;
                char buf[1024];
                for (size_t i = 0; i < values.size(); ++i) {
                    sprintf(buf, i == values.size() - 1 ? " %.2lf\n" : " %.2lf,", values[i]);
                    gcode_line += buf;
                }
                return true;
            }
            return false;
        };

        bool ret = false;
        ret |= process_tag(gcode_line, PrintStatistics::FilamentUsedMmMask, filament_mm);
        ret |= process_tag(gcode_line, PrintStatistics::FilamentUsedGMask, filament_g);
        ret |= process_tag(gcode_line, PrintStatistics::TotalFilamentUsedGMask, { filament_total_g });
        ret |= process_tag(gcode_line, PrintStatistics::FilamentUsedCm3Mask, filament_cm3);
        ret |= process_tag(gcode_line, PrintStatistics::FilamentCostMask, filament_cost);
        ret |= process_tag(gcode_line, PrintStatistics::TotalFilamentCostMask, { filament_total_cost });
        return ret;
    };

    auto process_inline_placeholders = [&](std::string& gcode_line) {
        bool processed = false;
        if (gcode_line.find('@') == std::string::npos)
            return processed;

        const std::string& print_time_total_placeholder = reserved_tag(ETags::Print_Time_Total_Sec_Placeholder);
        const std::string& print_time_day_placeholder = reserved_tag(ETags::Print_Time_Day_Placeholder);
        const std::string& print_time_hour_placeholder = reserved_tag(ETags::Print_Time_Hour_Placeholder);
        const std::string& print_time_minute_placeholder = reserved_tag(ETags::Print_Time_Minute_Placeholder);
        const std::string& print_time_sec_placeholder = reserved_tag(ETags::Print_Time_Sec_Placeholder);
        const std::string& used_filament_placeholder = reserved_tag(ETags::Used_Filament_Length_Placeholder);

        double print_time_total_sec = m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        if (print_time_total_sec < 0.0)
            print_time_total_sec = 0.0;

        int total_seconds = static_cast<int>(print_time_total_sec);
        int print_time_day = total_seconds / 86400;
        int day_remainder_seconds = total_seconds % 86400;
        int print_time_hour = day_remainder_seconds / 3600;
        int print_time_minute = (day_remainder_seconds % 3600) / 60;
        int print_time_sec = day_remainder_seconds % 60;

        size_t pos = gcode_line.find(print_time_total_placeholder);
        while (pos != std::string::npos) {
            char buf[64];
            sprintf(buf, "%.2f", print_time_total_sec);
            gcode_line.replace(pos, print_time_total_placeholder.length(), buf);
            processed = true;
            pos = gcode_line.find(print_time_total_placeholder, pos + strlen(buf));
        }

        pos = gcode_line.find(print_time_day_placeholder);
        while (pos != std::string::npos) {
            char buf[64];
            sprintf(buf, "%d", print_time_day);
            gcode_line.replace(pos, print_time_day_placeholder.length(), buf);
            processed = true;
            pos = gcode_line.find(print_time_day_placeholder, pos + strlen(buf));
        }

        pos = gcode_line.find(print_time_hour_placeholder);
        while (pos != std::string::npos) {
            char buf[64];
            sprintf(buf, "%d", print_time_hour);
            gcode_line.replace(pos, print_time_hour_placeholder.length(), buf);
            processed = true;
            pos = gcode_line.find(print_time_hour_placeholder, pos + strlen(buf));
        }

        pos = gcode_line.find(print_time_minute_placeholder);
        while (pos != std::string::npos) {
            char buf[64];
            sprintf(buf, "%d", print_time_minute);
            gcode_line.replace(pos, print_time_minute_placeholder.length(), buf);
            processed = true;
            pos = gcode_line.find(print_time_minute_placeholder, pos + strlen(buf));
        }

        pos = gcode_line.find(print_time_sec_placeholder);
        while (pos != std::string::npos) {
            char buf[64];
            sprintf(buf, "%d", print_time_sec);
            gcode_line.replace(pos, print_time_sec_placeholder.length(), buf);
            processed = true;
            pos = gcode_line.find(print_time_sec_placeholder, pos + strlen(buf));
        }

        pos = gcode_line.find(used_filament_placeholder);
        while (pos != std::string::npos) {
            double total_filament_mm = 0.0;
            for (const auto& mm : filament_mm) {
                total_filament_mm += mm;
            }
            double used_filament_length = total_filament_mm / 1000.0;
            char buf[64];
            sprintf(buf, "%.2f", used_filament_length);
            gcode_line.replace(pos, used_filament_placeholder.length(), buf);
            processed = true;
            pos = gcode_line.find(used_filament_placeholder, pos + strlen(buf));
        }

        return processed;
    };

    auto is_temporary_decoration = [](const std::string_view gcode_line) {
        assert(! gcode_line.empty());
        assert(gcode_line.back() == '\n');

        return false;
    };

    auto g1_times_cache_it = Slic3r::reserve_vector<std::vector<TimeMachine::G1LinesCacheItem>::const_iterator>(m_time_processor.machines.size());
    for (const auto& machine : m_time_processor.machines)
        g1_times_cache_it.emplace_back(machine.g1_times_cache.begin());

    auto process_line_move = [
        time_in_minutes, format_time_float, format_line_M73_main, format_line_M73_stop_int, format_line_M73_stop_float, time_in_last_minute,
        &self = std::as_const(m_time_processor),
        &g1_times_cache_it, &last_exported_main, &last_exported_stop,
        &export_line]
        (const size_t g1_lines_counter) {
        for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
            const TimeMachine& machine = self.machines[i];
            if (machine.enabled) {
                auto& it = g1_times_cache_it[i];
                while (it != machine.g1_times_cache.end() && it->id < g1_lines_counter)
                    ++it;
                if (it != machine.g1_times_cache.end() && it->id == g1_lines_counter) {
                    std::pair<int, int> to_export_main = { int(100.0f * it->elapsed_time / machine.time),
                                                            time_in_minutes(machine.time - it->elapsed_time) };

                    if (last_exported_main[i] != to_export_main) {
                        export_line.append_line(format_line_M73_main(machine.line_m73_main_mask.c_str(),
                            to_export_main.first, to_export_main.second), true);
                        last_exported_main[i] = to_export_main;
                    }
                    auto it_stop = std::upper_bound(machine.stop_times.begin(), machine.stop_times.end(), it->elapsed_time,
                        [](float value, const TimeMachine::StopTime& t) { return value < t.elapsed_time; });
                    if (it_stop != machine.stop_times.end()) {
                        int to_export_stop = time_in_minutes(it_stop->elapsed_time - it->elapsed_time);
                        if (last_exported_stop[i] != to_export_stop) {
                            if (to_export_stop > 0) {
                                if (last_exported_stop[i] != to_export_stop) {
                                    export_line.append_line(format_line_M73_stop_int(machine.line_m73_stop_mask.c_str(), to_export_stop), true);
                                    last_exported_stop[i] = to_export_stop;
                                }
                            }
                            else {
                                bool is_last = false;
                                auto next_it = it + 1;
                                is_last |= (next_it == machine.g1_times_cache.end());

                                if (next_it != machine.g1_times_cache.end()) {
                                    auto next_it_stop = std::upper_bound(machine.stop_times.begin(), machine.stop_times.end(), next_it->elapsed_time,
                                        [](float value, const TimeMachine::StopTime& t) { return value < t.elapsed_time; });
                                    is_last |= (next_it_stop != it_stop);

                                    std::string time_float_str = format_time_float(time_in_last_minute(it_stop->elapsed_time - it->elapsed_time));
                                    std::string next_time_float_str = format_time_float(time_in_last_minute(it_stop->elapsed_time - next_it->elapsed_time));
                                    is_last |= (string_to_double_decimal_point(time_float_str) > 0. && string_to_double_decimal_point(next_time_float_str) == 0.);
                                }

                                if (is_last) {
                                    if (std::distance(machine.stop_times.begin(), it_stop) == static_cast<ptrdiff_t>(machine.stop_times.size() - 1))
                                        export_line.append_line(format_line_M73_stop_int(machine.line_m73_stop_mask.c_str(), to_export_stop), true);
                                    else
                                        export_line.append_line(format_line_M73_stop_float(machine.line_m73_stop_mask.c_str(), time_in_last_minute(it_stop->elapsed_time - it->elapsed_time)), true);

                                    last_exported_stop[i] = to_export_stop;
                                }
                            }
                        }
                    }
                }
            }
        }
    };

    unsigned int current_layer_id = 0;

    auto process_line_T = [this, &export_line, &current_layer_id](const std::string& gcode_line, const size_t g1_lines_counter, const ExportLines::Backtrace& backtrace) {
        const std::string cmd = GCodeReader::GCodeLine::extract_cmd(gcode_line);

        int tool_number = -1;
        if (!parse_number(std::string_view(cmd).substr(1), tool_number)){
            return;
        }
        if (cmd.size() >= 2) {
            if (tool_number != -1) {
                if (tool_number < 0 || (int)m_filament_nozzle_temp.size() <= tool_number) {
                    tool_number = std::clamp<int>(0, m_filament_nozzle_temp.size() - 1, tool_number);
                    std::string warning = "Found an invalid toolchange during G-code post-processing (check your custom G-code):";
                    warning += "\n> ";
                    warning += gcode_line;
                    warning += "Generated M104 lines may be incorrect.";
                    BOOST_LOG_TRIVIAL(error) << warning;
                    if (m_print != nullptr)
                        m_print->active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, WarningCode{ "temperature_lines_suspect" }, warning);
                }
                export_line.insert_lines(
                    backtrace, cmd,
                    [tool_number, this, &current_layer_id](unsigned int id, const std::vector<float>& time_diffs) {
                        const int temperature = int(current_layer_id > 1 ? m_filament_nozzle_temp[tool_number] :
                                                                         m_filament_nozzle_temp_first_layer[tool_number]);
                        if (this->m_is_XL_printer) {
                            std::string out = "M104.1 T" + std::to_string(tool_number);
                            if (time_diffs.size() > 0)
                                out += " P" + std::to_string(int(std::round(time_diffs[0])));
                            if (time_diffs.size() > 1)
                                out += " Q" + std::to_string(int(std::round(time_diffs[1])));
                            out += " S" + std::to_string(temperature) + "\n";
                            return out;
                        } else {
                            const int real_tool = tool_number < m_physical_extruder_map.size() ? m_physical_extruder_map[tool_number] : tool_number;
                            std::string comment = "preheat T" + std::to_string(real_tool) +
                                                " time: " + std::to_string((int) std::round(time_diffs[0])) + "s";
                            return GCodeWriter::set_temperature(temperature, this->m_flavor, false, real_tool, comment);
                        }
                    },
                    [this, tool_number](const std::string& line) {
                        if (GCodeReader::GCodeLine::cmd_is(line, "M104")) {
                            GCodeReader::GCodeLine gline;
                            GCodeReader reader;
                            reader.parse_line(line, [&gline](GCodeReader& reader, const GCodeReader::GCodeLine& l) { gline = l; });

                            float val;
                            if (gline.has_value('T', val) && gline.raw().find("cooldown") != std::string::npos) {
                                if (static_cast<int>(val) == (tool_number < m_physical_extruder_map.size() ? m_physical_extruder_map[tool_number] : tool_number))
                                    return std::string("; removed M104\n");
                            }
                        }
                        return line;
                    }
                );
            }
        }
    };

    m_skippable_blocks.clear();

    unsigned int line_id = 0;
    const ExportLines::Backtrace backtrace_T = { m_preheat_time, m_preheat_steps };
    float max_backtrace_time = 120.0f;

    m_filament_blocks.clear();
    m_extruder_blocks.clear();
    m_machine_start_gcode_end_line_id = (unsigned int) (-1);
    m_machine_end_gcode_start_line_id = (unsigned int) (-1);
    if (m_enable_pre_heating)
        m_extruder_blocks.push_back(ExtruderPreHeating::ExtruderUsageBlcok());
    auto layered_ngr = std::dynamic_pointer_cast<MultiNozzleUtils::LayeredNozzleGroupResult>(m_result.nozzle_group_result);
    ExtruderPreHeating::ExtruderUsageBlcok temp_construct_block;
    auto handle_filament_change = [&](int filament_id, int cur_line_id, int nozzle_id) {
        if ((m_machine_start_gcode_end_line_id == (unsigned int) (-1) && (unsigned int) (cur_line_id) < m_machine_start_gcode_end_line_id) ||
            (m_machine_end_gcode_start_line_id != (unsigned int) (-1) && (unsigned int) (cur_line_id) > m_machine_end_gcode_start_line_id))
            return;
        if (!m_filament_blocks.empty())
            m_filament_blocks.back().upper_gcode_id = cur_line_id;
        if (nozzle_id == -1 && layered_ngr)
            nozzle_id = layered_ngr->get_nozzle_id(filament_id, current_layer_id);
        int extruder_id = 0;
        if (layered_ngr) {
            if (auto nozzle_info = layered_ngr->get_nozzle_from_id(nozzle_id))
                extruder_id = nozzle_info->extruder_id;
        }
        m_filament_blocks.emplace_back(filament_id, extruder_id, nozzle_id, cur_line_id, (unsigned int) (-1));
    };

    auto handle_nozzle_change_line = [&](const std::string& line, int& old_filament, int& next_filament, int& extruder_id, int gcode_id, int& old_nozzle_id, int& new_nozzle_id) -> bool {
        std::regex re(R"(OF(\d+)\s+NF(\d+)\s+ON(\d+)\s+NN(\d+))");
        std::smatch match;
        if (!std::regex_search(line, match, re))
            return false;
        old_filament  = std::stoi(match[1]);
        next_filament = std::stoi(match[2]);
        old_nozzle_id = std::stoi(match[3]);
        new_nozzle_id = std::stoi(match[4]);
        std::optional<MultiNozzleUtils::NozzleInfo> nozzle_info;
        if (layered_ngr)
            nozzle_info = layered_ngr->get_nozzle_from_id(new_nozzle_id);
        extruder_id = nozzle_info ? nozzle_info->extruder_id : -1;
        return true;
    };

    auto handle_toolchange_wipe_line = [](const std::string& line, bool& is_contact, bool& is_first_layer) -> bool {
        std::regex re(R"(CT(\d)(?:\s+FL(\d))?)");
        std::smatch match;
        if (!std::regex_search(line, match, re))
            return false;
        is_contact     = std::stoi(match[1]) != 0;
        is_first_layer = match[2].matched ? std::stoi(match[2]) != 0 : false;
        return true;
    };

    auto build_usage_blocks = [&](const std::string& gcode_line, int line_id) {
        using GLine = GCodeReader::GCodeLine;
        size_t skips = 0;
        while (skips < gcode_line.size() && (gcode_line[skips] == ' ' || gcode_line[skips] == '\t'))
            ++skips;

        if (gcode_line.size() > 1 && gcode_line.front() == ';') {
            std::string_view tag(gcode_line);
            while (!tag.empty() && (tag.back() == '\n' || tag.back() == '\r'))
                tag.remove_suffix(1);
            tag.remove_prefix(1);
            if (tag == Machine_Start_GCode_End_Tag) { m_machine_start_gcode_end_line_id = line_id; return; }
            if (tag == Machine_End_GCode_Start_Tag) { m_machine_end_gcode_start_line_id = line_id; return; }
        }

        if (GLine::cmd_starts_with(gcode_line, "T")) {
            std::istringstream str(gcode_line.substr(skips + 1));
            int fid;
            str >> fid;
            if (!str.fail() && 0 <= fid && fid < 255) {
                int nozzle_id = -1; char param;
                while (str >> param) { if (param == 'H') { if (!(str >> nozzle_id)) BOOST_LOG_TRIVIAL(warning) << "Invalid nozzle id format in T command: " << gcode_line; break; } }
                handle_filament_change(fid, line_id, nozzle_id);
            }
            return;
        }
        if (GLine::cmd_starts_with(gcode_line, ";VT")) {
            std::istringstream str(gcode_line.substr(skips + 3));
            int fid;
            str >> fid;
            if (!str.fail() && 0 <= fid && fid < 255) {
                int nozzle_id = -1; char param;
                while (str >> param) { if (param == 'H') { if (!(str >> nozzle_id)) BOOST_LOG_TRIVIAL(warning) << "Invalid nozzle id format in VT command: " << gcode_line; break; } }
                handle_filament_change(fid, line_id, nozzle_id);
            }
            return;
        }
        if (GLine::cmd_starts_with(gcode_line, "M1020")) {
            size_t s_pos = gcode_line.find('S');
            if (s_pos != std::string::npos) {
                std::istringstream str(gcode_line.substr(s_pos + 1));
                int fid;
                str >> fid;
                if (!str.fail() && 0 <= fid && fid < 255) {
                    int nozzle_id = -1; char param;
                    while (str >> param) { if (param == 'H') { if (!(str >> nozzle_id)) BOOST_LOG_TRIVIAL(warning) << "Invalid nozzle id format in M1020 command: " << gcode_line; break; } }
                    handle_filament_change(fid, line_id, nozzle_id);
                }
            }
            return;
        }

        if (GLine::cmd_starts_with(gcode_line, (std::string(";") + Nozzle_Change_Start_Tag).c_str())) {
            int prev_filament{-1}, next_filament{-1}, extruder_id{-1}, prev_nozzle_id{-1}, next_nozzle_id{-1};
            handle_nozzle_change_line(gcode_line, prev_filament, next_filament, extruder_id, line_id, prev_nozzle_id, next_nozzle_id);
            if (!m_extruder_blocks.empty())
                m_extruder_blocks.back().initialize_step_2(line_id);
            return;
        }
        if (GLine::cmd_starts_with(gcode_line, (std::string(";") + Nozzle_Change_End_Tag).c_str())) {
            int prev_filament{-1}, next_filament{-1}, extruder_id{-1}, prev_nozzle_id{-1}, next_nozzle_id{-1};
            handle_nozzle_change_line(gcode_line, prev_filament, next_filament, extruder_id, line_id, prev_nozzle_id, next_nozzle_id);
            if (!m_extruder_blocks.empty())
                m_extruder_blocks.back().initialize_step_3(line_id, prev_filament, line_id, prev_nozzle_id);
            temp_construct_block.initialize_step_1(extruder_id, line_id, next_filament, next_nozzle_id);
            m_extruder_blocks.emplace_back(temp_construct_block);
            temp_construct_block.reset();
            return;
        }
        if (GLine::cmd_starts_with(gcode_line, (std::string(";") + Toolchange_Wipe_Tag).c_str())) {
            bool is_contact = false, is_first_layer = false;
            handle_toolchange_wipe_line(gcode_line, is_contact, is_first_layer);
            if (!m_extruder_blocks.empty())
                m_extruder_blocks.back().ignore_cooling_before_tower = is_contact || is_first_layer;
            return;
        }
    };

    {
        std::vector<char> buffer(65536 * 10, 0);
        assert(gcode_line.empty());
        for (;;) {
            size_t cnt_read = ::fread(buffer.data(), 1, buffer.size(), in.f);
            if (::ferror(in.f))
                throw Slic3r::RuntimeError(std::string("GCode processor post process export failed.\nError while reading from file.\n"));
            bool eof       = cnt_read == 0;
            auto it        = buffer.begin();
            auto it_bufend = buffer.begin() + cnt_read;
            while (it != it_bufend || (eof && ! gcode_line.empty())) {
                bool eol    = false;
                auto it_end = it;
                for (; it_end != it_bufend && ! (eol = *it_end == '\r' || *it_end == '\n'); ++ it_end) ;
                eol |= eof && it_end == it_bufend;
                gcode_line.insert(gcode_line.end(), it, it_end);

                it = it_end;
                if (it != it_bufend && *it == '\r') {
                    gcode_line += *it++;
                }
                if (it != it_bufend && *it == '\n') {
                    gcode_line += *it++;
                }

                if (eol) {
                    ++line_id;
                    const unsigned int internal_g1_lines_counter = export_line.update(gcode_line, line_id, g1_lines_counter);
                    if (gcode_line.size() > 1 && gcode_line.front() == ';') {
                        std::string_view tag_line(gcode_line);
                        while (!tag_line.empty() && (tag_line.back() == '\n' || tag_line.back() == '\r'))
                            tag_line.remove_suffix(1);
                        tag_line.remove_prefix(1);
                        if (tag_line == reserved_tag(ETags::Layer_Change))
                            ++current_layer_id;
                        else if (tag_line == Skippable_Start_Tag)
                            m_skippable_blocks.emplace_back(line_id, 0);
                        else if (tag_line == Skippable_End_Tag && !m_skippable_blocks.empty())
                            m_skippable_blocks.back().second = line_id;
                    }
                    if (m_enable_pre_heating)
                        build_usage_blocks(gcode_line, line_id);
                    bool processed = process_placeholders(gcode_line);
                    if (processed)
                        gcode_line.clear();
                    if (!processed)
                        processed = process_used_filament(gcode_line);
                    if (!gcode_line.empty())
                        process_inline_placeholders(gcode_line);
                    if (!processed && !is_temporary_decoration(gcode_line)) {
                        if (GCodeReader::GCodeLine::cmd_is(gcode_line, "G0") || GCodeReader::GCodeLine::cmd_is(gcode_line, "G1")) {
                            export_line.append_line(gcode_line);
                            process_line_move(g1_lines_counter++);
                            gcode_line.clear();
                        }
                        else if (GCodeReader::GCodeLine::cmd_is(gcode_line, "G2") || GCodeReader::GCodeLine::cmd_is(gcode_line, "G3")) {
                            export_line.append_line(gcode_line);
                            process_line_move(g1_lines_counter + internal_g1_lines_counter);
                            g1_lines_counter += (1 + internal_g1_lines_counter);
                            gcode_line.clear();
                        }
                        else if (GCodeReader::GCodeLine::cmd_is(gcode_line, "G28")) {
                            ++g1_lines_counter;
                        }
                        else if (m_result.backtrace_enabled && GCodeReader::GCodeLine::cmd_starts_with(gcode_line, "T")) {
                            process_line_T(gcode_line, g1_lines_counter, backtrace_T);
                            max_backtrace_time = std::max(max_backtrace_time, backtrace_T.time);
                        }
                    }

                    if (!gcode_line.empty())
                        export_line.append_line(gcode_line);
                    export_line.write(1.1f * max_backtrace_time);
                    gcode_line.clear();
                }
            }
            if (eof)
                break;
        }
    }

    if (m_enable_pre_heating) {
        if (!m_filament_blocks.empty())
            m_filament_blocks.back().upper_gcode_id = m_machine_end_gcode_start_line_id;

        if (!m_extruder_blocks.empty()) {
            int first_filament = 0;
            int last_filament  = 0;
            if (!m_filament_blocks.empty()) {
                first_filament = m_filament_blocks.front().filament_id;
                last_filament  = m_filament_blocks.back().filament_id;
            }
            {
                int extruder_id = -1;
                std::optional<MultiNozzleUtils::NozzleInfo> nozzle_info;
                if (layered_ngr)
                    nozzle_info = layered_ngr->get_first_nozzle_for_filament(first_filament);
                if (nozzle_info)
                    extruder_id = nozzle_info->extruder_id;
                int start_nozzle_id = nozzle_info ? nozzle_info->group_id : -1;
                m_extruder_blocks.front().initialize_step_1(extruder_id, m_machine_start_gcode_end_line_id, first_filament, start_nozzle_id);
            }
            m_extruder_blocks.back().initialize_step_2(m_machine_end_gcode_start_line_id);
            int last_nozzle_id = -1;
            if (!m_filament_blocks.empty())
                last_nozzle_id = m_filament_blocks.back().nozzle_id;
            m_extruder_blocks.back().initialize_step_3(m_machine_end_gcode_start_line_id, last_filament, m_machine_end_gcode_start_line_id, last_nozzle_id);
        }
    }

    export_line.flush();

    out.close();
    in.close();

    const std::string result_filename = m_result.filename;
    export_line.synchronize_moves(m_result);

    if (m_enable_pre_heating) {
        auto remap = [&export_line](unsigned int& id) { id = export_line.remap_gcode_id(id); };
        for (auto& b : m_filament_blocks) {
            remap(b.lower_gcode_id);
            remap(b.upper_gcode_id);
        }
        for (auto& b : m_extruder_blocks) {
            remap(b.start_id);
            remap(b.end_id);
            remap(b.post_extrusion_start_id);
            remap(b.post_extrusion_end_id);
        }
        for (auto& b : m_skippable_blocks) {
            remap(b.first);
            remap(b.second);
        }
        remap(m_machine_start_gcode_end_line_id);
        remap(m_machine_end_gcode_start_line_id);
    }

    if (rename_file(out_path, result_filename))
        throw Slic3r::RuntimeError(std::string("Failed to rename the output G-code file from ") + out_path + " to " + result_filename + '\n' +
            "Is " + out_path + " locked?" + '\n');
}

void GCodeProcessor::run_second_pass_injection()
{
    TimeProcessor::InsertedLinesMap inserted_operation_lines;

    {
        int valid_machine_id = 0;
        for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
            if (m_time_processor.machines[i].enabled) {
                valid_machine_id = (int) i;
                break;
            }
        }
        auto layered_ngr = std::dynamic_pointer_cast<MultiNozzleUtils::LayeredNozzleGroupResult>(m_result.nozzle_group_result);
        if (layered_ngr) {
            constexpr float inject_time_threshold = 0.f;
            std::vector<int> filament_nozzle_temps(m_filament_nozzle_temp.begin(), m_filament_nozzle_temp.end());
            std::vector<int> filament_nozzle_temps_initial(m_filament_nozzle_temp_first_layer.begin(), m_filament_nozzle_temp_first_layer.end());
            const std::vector<double> filament_max_temperature_drop_when_ec{ 0.f };

            std::vector<float> saved_time;
            saved_time.reserve(m_result.moves.size());
            {
                float acc = 0.f;
                for (auto& mv : m_result.moves) {
                    saved_time.push_back(mv.time[valid_machine_id]);
                    acc += mv.time[valid_machine_id];
                    mv.time[valid_machine_id] = acc;
                }
            }

            PreCoolingInjector injector(
                m_result.moves,
                m_filament_types,
                *layered_ngr,
                filament_nozzle_temps,
                filament_nozzle_temps_initial,
                m_physical_extruder_map,
                valid_machine_id,
                inject_time_threshold,
                m_handle_hotend_as_extruder,
                m_has_filament_switcher,
                m_filament_pre_cooling_temp,
                m_hotend_cooling_rate,
                m_hotend_heating_rate,
                m_skippable_blocks,
                m_extruder_max_nozzle_count,
                m_filament_preheat_temperature_delta,
                filament_max_temperature_drop_when_ec,
                m_machine_start_gcode_end_line_id,
                m_machine_end_gcode_start_line_id,
                m_result.extruder_types,
                m_nozzle_diameter);
            injector.build_extruder_free_blocks(m_filament_blocks, m_extruder_blocks);
            injector.process_pre_cooling_and_heating(inserted_operation_lines);

            for (size_t i = 0; i < m_result.moves.size(); ++i)
                m_result.moves[i].time[valid_machine_id] = saved_time[i];
        }
    }

    FilePtr in{ boost::nowide::fopen(m_result.filename.c_str(), "rb") };
    if (in.f == nullptr)
        throw Slic3r::RuntimeError(std::string("GCode processor pre-heat injection pass failed.\nCannot open file for reading.\n"));

    const std::string out_path = m_result.filename + ".preheat";
    FilePtr out{ boost::nowide::fopen(out_path.c_str(), "wb") };
    if (out.f == nullptr)
        throw Slic3r::RuntimeError(std::string("GCode processor pre-heat injection pass failed.\nCannot open file for writing.\n"));

    m_result.lines_ends.clear();
    GCodeFileWriter writer(out, out_path, m_result.lines_ends, "GCode processor pre-heat injection pass failed.\nIs the disk full?\n");

    std::string gcode_line;
    unsigned int line_id = 0;
    auto op_it = inserted_operation_lines.begin();
    std::vector<char> buffer(65536 * 10, 0);
    for (;;) {
        size_t cnt_read = ::fread(buffer.data(), 1, buffer.size(), in.f);
        if (::ferror(in.f))
            throw Slic3r::RuntimeError(std::string("GCode processor pre-heat injection pass failed.\nError while reading from file.\n"));
        bool eof       = cnt_read == 0;
        auto it        = buffer.begin();
        auto it_bufend = buffer.begin() + cnt_read;
        while (it != it_bufend || (eof && !gcode_line.empty())) {
            bool eol    = false;
            auto it_end = it;
            for (; it_end != it_bufend && !(eol = *it_end == '\r' || *it_end == '\n'); ++it_end);
            eol |= eof && it_end == it_bufend;
            gcode_line.insert(gcode_line.end(), it, it_end);
            it = it_end;
            if (it != it_bufend && *it == '\r')
                gcode_line += *it++;
            if (it != it_bufend && *it == '\n')
                gcode_line += *it++;
            if (eol) {
                ++line_id;
                if (op_it != inserted_operation_lines.end() && line_id == op_it->first) {
                    for (const auto& elem : op_it->second) {
                        switch (elem.second) {
                        case TimeProcessor::InsertLineType::PreCooling:
                        case TimeProcessor::InsertLineType::PreHeating:
                        case TimeProcessor::InsertLineType::FilamentChangePredict:
                            gcode_line += elem.first;
                            break;
                        default:
                            break;
                        }
                    }
                    ++op_it;
                }
                writer.append(gcode_line);
                gcode_line.clear();
            }
        }
        if (eof)
            break;
    }
    writer.flush();

    out.close();
    in.close();

    handle_offsets_of_second_process(inserted_operation_lines);

    if (rename_file(out_path, m_result.filename))
        throw Slic3r::RuntimeError(std::string("Failed to rename the output G-code file from ") + out_path + " to " + m_result.filename + '\n' +
            "Is " + out_path + " locked?" + '\n');
}

void GCodeProcessor::handle_offsets_of_second_process(const TimeProcessor::InsertedLinesMap& inserted_operation_lines)
{
    int total_offset = 0;
    auto iter = inserted_operation_lines.begin();
    for (GCodeProcessorResult::MoveVertex& move : m_result.moves) {
        while (iter != inserted_operation_lines.end() && iter->first < move.gcode_id) {
            total_offset += static_cast<int>(iter->second.size());
            ++iter;
        }
        move.gcode_id += total_offset;
    }
}

void GCodeProcessor::PreCoolingInjector::process_pre_cooling_and_heating(TimeProcessor::InsertedLinesMap& inserted_operation_lines)
{
    bool is_multiple_nozzle = std::any_of(extruder_max_nozzle_count.begin(), extruder_max_nozzle_count.end(), [](auto& elem) { return elem > 1; });
    (void) is_multiple_nozzle;
    auto get_nozzle_temp = [this](int filament_id, bool is_first_layer, bool from_or_to, bool consider_preheat_temperature_delta) {
        if (filament_id == -1)
            return from_or_to ? 140 : 0;
        double temp = (is_first_layer ? filament_nozzle_temps_initial_layer[filament_id] : filament_nozzle_temps[filament_id]);
        if (consider_preheat_temperature_delta)
            return (int) (temp - filament_preheat_temperature_delta[filament_id]);
        else
            return (int) (temp);
    };

    bool has_mixed_extruder_types = extruder_types.size() > 1 &&
        std::adjacent_find(extruder_types.begin(), extruder_types.end(), std::not_equal_to<>()) != extruder_types.end();
    float first_nozzle_dia = nozzle_diameter.empty() ? 0.4 : nozzle_diameter.front();
    float switcher_temp_offset = (first_nozzle_dia >= 0.6 - EPSILON) ? 40.f : 20.f;

    std::map<int, std::vector<ExtruderFreeBlock>> per_extruder_free_blocks;

    for (auto& block : m_extruder_free_blocks)
        per_extruder_free_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_free_blocks) {
        auto& extruder_free_blcoks = elem.second;
        for (auto iter = extruder_free_blcoks.begin(); iter != extruder_free_blcoks.end(); ++iter) {
            bool is_end = std::next(iter) == extruder_free_blcoks.end();
            bool apply_pre_cooling = true;
            bool apply_pre_heating = is_end ? false : true;
            float curr_temp = get_nozzle_temp(iter->last_filament_id, false, true, false);
            float target_temp = get_nozzle_temp(iter->next_filament_id, false, false, !iter->ignore_cooling_before_tower);
            if (has_filament_switcher && has_mixed_extruder_types && apply_pre_heating) {
                float print_temp = get_nozzle_temp(iter->next_filament_id, false, false, false);
                target_temp = std::min(target_temp, print_temp - switcher_temp_offset);
            }
            inject_cooling_heating_command(inserted_operation_lines, *iter, curr_temp, target_temp, apply_pre_cooling, apply_pre_heating);
        }
    }
}

void GCodeProcessor::PreCoolingInjector::build_extruder_free_blocks(const std::vector<ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks, const std::vector<ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks)
{
    if (extruder_usage_blocks.size() <= 1)
        build_by_filament_blocks(filament_usage_blocks);
    else
        build_by_extruder_blocks(extruder_usage_blocks);
}

void GCodeProcessor::PreCoolingInjector::inject_cooling_heating_command(TimeProcessor::InsertedLinesMap& inserted_operation_lines, const ExtruderFreeBlock& block, float curr_temp, float target_temp, bool pre_cooling, bool pre_heating)
{
    auto get_valid_extruder_id = [&](int last_nozzle_id) {
        auto nozzle_opt = nozzle_group_result.get_nozzle_from_id(last_nozzle_id);
        return nozzle_opt ? nozzle_opt->extruder_id : 0;
    };

    auto is_pre_cooling_valid = [&nozzle_temps = this->filament_nozzle_temps, &pre_cooling_temps = this->filament_pre_cooling_temps](int idx) -> bool {
        if (idx < 0)
            return false;
        return pre_cooling_temps[idx] > 0 && pre_cooling_temps[idx] < nozzle_temps[idx];
    };

    auto get_partial_free_cooling_thres = [&](int idx) -> float {
        if (idx < 0)
            return 30.f;
        float temp_in_tower = filament_nozzle_temps[idx];
        return temp_in_tower - (float) (filament_pre_cooling_temps[idx]);
    };

    auto gcode_move_comp = [](const GCodeProcessorResult::MoveVertex& a, unsigned int gcode_id) {
        return a.gcode_id < gcode_id;
    };

    auto find_skip_block_end = [&skippable_blocks = this->skippable_blocks](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            skippable_blocks.begin(), skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& block) { return id < block.first; });
        if (it != skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->second;
        }
        return 0;
    };

    auto find_skip_block_start = [&skippable_blocks = this->skippable_blocks](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            skippable_blocks.begin(), skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& block) { return id < block.first; });
        if (it != skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->first;
        }
        return 0;
    };

    auto adjust_iter = [&](GCodeProcessorResult::Moves::const_iterator iter,
                           const GCodeProcessorResult::Moves::const_iterator& begin,
                           const GCodeProcessorResult::Moves::const_iterator& end,
                           bool forward) -> GCodeProcessorResult::Moves::const_iterator {
        if (forward) {
            while (iter != end) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_end = find_skip_block_end(current_id);
                if (skip_block_end == 0)
                    break;
                iter = std::lower_bound(iter, end, skip_block_end + 1, gcode_move_comp);
            }
        } else {
            while (iter != begin) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_start = find_skip_block_start(current_id);
                if (skip_block_start == 0)
                    break;
                auto new_iter = std::lower_bound(begin, iter, skip_block_start, gcode_move_comp);
                if (new_iter == begin)
                    break;
                iter = std::prev(new_iter);
            }
        }
        return iter;
    };

    if (!pre_cooling && !pre_heating && block.free_upper_gcode_id <= block.free_lower_gcode_id)
        return;

    auto move_iter_lower = std::lower_bound(moves.begin(), moves.end(), block.free_lower_gcode_id, gcode_move_comp);
    auto move_iter_upper = std::lower_bound(moves.begin(), moves.end(), block.free_upper_gcode_id, gcode_move_comp);

    if (move_iter_lower == moves.end() || move_iter_upper == moves.begin())
        return;
    --move_iter_upper;
    float complete_free_time_gap = 0;
    if (move_iter_lower == moves.begin())
        complete_free_time_gap = move_iter_upper->time[valid_machine_id];
    else
        complete_free_time_gap = move_iter_upper->time[valid_machine_id] - std::prev(move_iter_lower)->time[valid_machine_id];

    auto partial_free_move_lower = std::lower_bound(moves.begin(), moves.end(), block.partial_free_lower_id, gcode_move_comp);
    auto partial_free_move_upper = std::lower_bound(moves.begin(), moves.end(), block.partial_free_upper_id, gcode_move_comp);
    if (partial_free_move_lower == moves.end() || partial_free_move_upper == moves.begin())
        return;
    --partial_free_move_upper;
    float partial_free_time_gap = 0;
    if (partial_free_move_lower == moves.begin())
        partial_free_time_gap = partial_free_move_upper->time[valid_machine_id];
    else
        partial_free_time_gap = partial_free_move_upper->time[valid_machine_id] - std::prev(partial_free_move_lower)->time[valid_machine_id];

    if (move_iter_lower >= move_iter_upper)
        return;

    bool apply_cooling_when_partial_free = is_pre_cooling_valid(block.last_filament_id) && pre_cooling;

    if (apply_cooling_when_partial_free && partial_free_time_gap + complete_free_time_gap < inject_time_threshold)
        return;

    if (!apply_cooling_when_partial_free && complete_free_time_gap < inject_time_threshold)
        return;

    int extruder_id = get_valid_extruder_id(block.last_nozzle_id);
    float ext_heating_rate = heating_rate[extruder_id];
    float ext_cooling_rate = cooling_rate[extruder_id];

    auto add_M104_lines = [&](int gcode_id, int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, TimeProcessor::InsertLineType type, const std::string& comment = std::string()) {
        auto format_line_M104 = [&](int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, const std::string& comment = std::string()) -> std::vector<std::string> {
            std::vector<std::string> buffer;
            if (skippable) {
                const bool support_dynamic_nozzle_map = this->nozzle_group_result.is_support_dynamic_nozzle_map();
                std::string m632_line = "M632 S" + std::to_string(next_filament_idx);
                if (support_dynamic_nozzle_map)
                    m632_line += " H" + std::to_string(next_nozzle_id);
                if (extruder_max_nozzle_count[target_extruder] > 1)
                    m632_line += " N R";
                m632_line += " W\n";
                buffer.emplace_back(std::move(m632_line));
            }
            buffer.emplace_back("M400\n");
            std::string M104_line = "M104";
            if (handle_hotend_as_extruder) {
                M104_line += (" I" + std::to_string(target_filament == -1 ? next_filament_idx : target_filament));
            } else if (target_extruder != -1) {
                M104_line += (" T" + std::to_string(physical_extruder_map[target_extruder]));
            }

            M104_line += " S" + std::to_string(target_temp);
            M104_line += " N0";

            if (!comment.empty())
                M104_line += " ;" + comment;
            M104_line += '\n';

            buffer.emplace_back(M104_line);

            if (skippable)
                buffer.emplace_back("M633\n");

            return buffer;
        };

        std::vector<std::string> line_buf = format_line_M104(target_extruder, target_temp, target_filament, skippable, next_filament_idx, next_nozzle_id, comment);
        for (auto& line : line_buf)
            inserted_operation_lines[gcode_id].emplace_back(line, type);
    };

    constexpr float room_temperature = 25.f;

    if (apply_cooling_when_partial_free) {
        float max_cooling_temp = std::min(curr_temp, std::min(get_partial_free_cooling_thres(block.last_filament_id), partial_free_time_gap * ext_cooling_rate));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": partial cooling for %1% %2%") % max_cooling_temp % curr_temp;
        curr_temp = std::max(room_temperature, curr_temp - max_cooling_temp);
        add_M104_lines(block.partial_free_lower_id, extruder_id, curr_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling in post extrusion");
    }

    if (pre_cooling && !pre_heating) {
        if (target_temp >= curr_temp)
            return;
        int clamped_target = std::max((int) room_temperature, (int) target_temp);
        add_M104_lines(block.free_lower_gcode_id, extruder_id, clamped_target, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling");
        return;
    }
    if (!pre_cooling && pre_heating) {
        if (target_temp <= curr_temp)
            return;
        float heating_start_time = move_iter_upper->time[valid_machine_id] - (target_temp - curr_temp) / ext_heating_rate;
        auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [valid_machine_id = this->valid_machine_id](float time, const GCodeProcessorResult::MoveVertex& a) { return time < a.time[valid_machine_id]; });
        if (heating_move_iter == move_iter_lower) {
            add_M104_lines(block.free_lower_gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
        } else {
            --heating_move_iter;
            heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);
            add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
        }
        return;
    }
    float mid_temp = std::max(room_temperature, (curr_temp * ext_heating_rate + target_temp * ext_cooling_rate - complete_free_time_gap * ext_cooling_rate * ext_heating_rate) / (ext_cooling_rate + ext_heating_rate));
    float heating_temp = target_temp - mid_temp;
    float heating_start_time = move_iter_upper->time[valid_machine_id] - heating_temp / ext_heating_rate;
    auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [valid_machine_id = this->valid_machine_id](float time, const GCodeProcessorResult::MoveVertex& a) { return time < a.time[valid_machine_id]; });
    if (heating_move_iter == move_iter_lower)
        return;
    --heating_move_iter;
    heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);

    float real_cooling_time = heating_move_iter->time[valid_machine_id] - move_iter_lower->time[valid_machine_id];
    int real_delta_temp = std::min((int) (real_cooling_time * ext_cooling_rate), (int) curr_temp);
    if (real_delta_temp == 0)
        return;
    int cooling_temp = std::max((int) room_temperature, (int) curr_temp - real_delta_temp);
    add_M104_lines(block.free_lower_gcode_id, extruder_id, cooling_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreCooling, "Multi extruder pre cooling");
    add_M104_lines(heating_move_iter->gcode_id, extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, TimeProcessor::InsertLineType::PreHeating, "Multi extruder pre heating");
}

void GCodeProcessor::PreCoolingInjector::build_by_filament_blocks(const std::vector<ExtruderPreHeating::FilamentUsageBlock>& filament_usage_blocks_)
{
    m_extruder_free_blocks.clear();

    std::map<int, std::vector<ExtruderPreHeating::FilamentUsageBlock>> per_extruder_usage_blocks;
    for (auto& block : filament_usage_blocks_) {
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);
    }
    ExtruderPreHeating::FilamentUsageBlock start_filament_block(-1, -1, -1, 0, machine_start_gcode_end_id);
    ExtruderPreHeating::FilamentUsageBlock end_filament_block(-1, -1, -1, machine_end_gcode_start_id, std::numeric_limits<unsigned int>::max());

    for (auto& elem : per_extruder_usage_blocks) {
        auto& blocks = elem.second;
        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        const auto& filament_blocks = elem.second;

        for (auto iter = filament_blocks.begin(); iter < filament_blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == filament_blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = iter->upper_gcode_id;
            block.last_filament_id = iter->filament_id;
            block.last_nozzle_id = iter->nozzle_id;
            block.free_upper_gcode_id = niter->lower_gcode_id;
            block.next_filament_id = niter->filament_id;
            block.next_nozzle_id = niter->nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id = extruder_id;
            block.partial_free_lower_id = block.free_lower_gcode_id;
            block.partial_free_upper_id = block.free_lower_gcode_id;
            m_extruder_free_blocks.emplace_back(block);
        }
    }
    std::for_each(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](ExtruderFreeBlock& block) { block.ignore_cooling_before_tower = true; });
    sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

void GCodeProcessor::PreCoolingInjector::build_by_extruder_blocks(const std::vector<ExtruderPreHeating::ExtruderUsageBlcok>& extruder_usage_blocks_)
{
    m_extruder_free_blocks.clear();
    std::map<int, std::vector<ExtruderPreHeating::ExtruderUsageBlcok>> per_extruder_usage_blocks;
    for (auto& block : extruder_usage_blocks_)
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        auto& blocks = elem.second;
        ExtruderPreHeating::ExtruderUsageBlcok start_filament_block;
        start_filament_block.initialize_step_1(extruder_id, 0, -1, -1);
        start_filament_block.initialize_step_2(machine_start_gcode_end_id);
        start_filament_block.initialize_step_3(machine_start_gcode_end_id, -1, machine_start_gcode_end_id, -1);

        ExtruderPreHeating::ExtruderUsageBlcok end_filament_block;
        end_filament_block.initialize_step_1(extruder_id, machine_end_gcode_start_id, -1, -1);
        end_filament_block.initialize_step_2(std::numeric_limits<int>::max());
        end_filament_block.initialize_step_3(std::numeric_limits<int>::max(), -1, std::numeric_limits<int>::max(), -1);

        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        const auto& extruder_usage_blocks = elem.second;
        for (auto iter = extruder_usage_blocks.begin(); iter != extruder_usage_blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == extruder_usage_blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = iter->end_id;
            block.last_filament_id = iter->end_filament;
            block.last_nozzle_id = iter->end_nozzle_id;
            block.free_upper_gcode_id = niter->start_id;
            block.next_filament_id = niter->start_filament;
            block.next_nozzle_id = niter->start_nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id = extruder_id;
            block.partial_free_lower_id = iter->post_extrusion_start_id;
            block.partial_free_upper_id = iter->post_extrusion_end_id;
            block.ignore_cooling_before_tower = niter->ignore_cooling_before_tower;
            m_extruder_free_blocks.emplace_back(block);
        }
    }

    sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

void GCodeProcessor::UsedFilaments::reset()
{
    color_change_cache = 0.0f;
    volumes_per_color_change = std::vector<double>();

    model_extrude_cache = 0.0f;
    model_volumes_per_filament.clear();

    flush_per_filament.clear();

    role_cache = 0.0f;
    filaments_per_role.clear();

    wipe_tower_cache = 0.0f;
    wipe_tower_volumes_per_filament.clear();

    support_volume_cache = 0.0f;
    support_volumes_per_filament.clear();

    total_volume_cache = 0.0f;
    total_volumes_per_filament.clear();
}

void GCodeProcessor::UsedFilaments::increase_support_caches(double extruded_volume)
{
    support_volume_cache += extruded_volume;
    role_cache += extruded_volume;
    total_volume_cache += extruded_volume;
}

void GCodeProcessor::UsedFilaments::increase_model_caches(double extruded_volume)
{
    color_change_cache += extruded_volume;
    model_extrude_cache += extruded_volume;
    role_cache += extruded_volume;
    total_volume_cache += extruded_volume;
}

void GCodeProcessor::UsedFilaments::increase_wipe_tower_caches(double extruded_volume)
{
    wipe_tower_cache += extruded_volume;
    role_cache += extruded_volume;
    total_volume_cache += extruded_volume;
}

void GCodeProcessor::UsedFilaments::process_color_change_cache()
{
    if (color_change_cache != 0.0f) {
        volumes_per_color_change.push_back(color_change_cache);
        color_change_cache = 0.0f;
    }
}


void GCodeProcessor::UsedFilaments::process_total_volume_cache(GCodeProcessor* processor)
{
    size_t active_filament_id = processor->get_filament_id();
    if (total_volume_cache!= 0.0f) {
        if (total_volumes_per_filament.find(active_filament_id) != total_volumes_per_filament.end())
            total_volumes_per_filament[active_filament_id] += total_volume_cache;
        else
            total_volumes_per_filament[active_filament_id] = total_volume_cache;
        total_volume_cache = 0.0f;
    }
}

void GCodeProcessor::UsedFilaments::process_model_cache(GCodeProcessor* processor)
{
    size_t active_filament_id = processor->get_filament_id();
    if (model_extrude_cache != 0.0f) {
        if (model_volumes_per_filament.find(active_filament_id) != model_volumes_per_filament.end())
            model_volumes_per_filament[active_filament_id] += model_extrude_cache;
        else
            model_volumes_per_filament[active_filament_id] = model_extrude_cache;
        model_extrude_cache = 0.0f;
    }
}

void GCodeProcessor::UsedFilaments::process_wipe_tower_cache(GCodeProcessor* processor)
{
    size_t active_filament_id = processor->get_filament_id();
    if (wipe_tower_cache != 0.0f) {
        if (wipe_tower_volumes_per_filament.find(active_filament_id) != wipe_tower_volumes_per_filament.end())
            wipe_tower_volumes_per_filament[active_filament_id] += wipe_tower_cache;
        else
            wipe_tower_volumes_per_filament[active_filament_id] = wipe_tower_cache;
        wipe_tower_cache = 0.0f;
    }
}

void GCodeProcessor::UsedFilaments::process_support_cache(GCodeProcessor* processor)
{
    size_t active_filament_id = processor->get_filament_id();
    if (support_volume_cache != 0.0f){
        if (support_volumes_per_filament.find(active_filament_id) != support_volumes_per_filament.end())
            support_volumes_per_filament[active_filament_id] += support_volume_cache;
        else
            support_volumes_per_filament[active_filament_id] = support_volume_cache;
        support_volume_cache = 0.0f;
    }
}

void GCodeProcessor::UsedFilaments::update_flush_per_filament(size_t filament_id, float flush_volume)
{
    if (flush_volume != 0.f) {
        if (flush_per_filament.find(filament_id) != flush_per_filament.end())
            flush_per_filament[filament_id] += flush_volume;
        else
            flush_per_filament[filament_id] = flush_volume;

        if (total_volumes_per_filament.find(filament_id) != total_volumes_per_filament.end())
            total_volumes_per_filament[filament_id] += flush_volume;
        else
            total_volumes_per_filament[filament_id] = flush_volume;
    }
}

void GCodeProcessor::UsedFilaments::process_role_cache(GCodeProcessor* processor)
{
    if (role_cache != 0.0f) {
        std::pair<double, double> filament = { 0.0f, 0.0f };

        double s = PI * sqr(0.5 * processor->m_result.filament_diameters[processor->get_filament_id()]);
        filament.first = role_cache / s * 0.001;
        filament.second = role_cache * processor->m_result.filament_densities[processor->get_filament_id()] * 0.001;

        ExtrusionRole active_role = processor->m_extrusion_role;
        if (filaments_per_role.find(active_role) != filaments_per_role.end()) {
            filaments_per_role[active_role].first += filament.first;
            filaments_per_role[active_role].second += filament.second;
        }
        else
            filaments_per_role[active_role] = filament;
        role_cache = 0.0f;
    }
}

void GCodeProcessor::UsedFilaments::process_caches(GCodeProcessor* processor)
{
    process_color_change_cache();
    process_model_cache(processor);
    process_role_cache(processor);
    process_wipe_tower_cache(processor);
    process_support_cache(processor);
    process_total_volume_cache(processor);
}

void GCodeProcessorResult::reset() {
    lock();

    moves.clear();
    move_states = { MoveState() };
    joints.clear();
    move_type_distances.fill(0.0f);
    clearance.clear();
    nonplanar = false;
    clearance_report = {};
    nonplanar_report = {};
    plate_mass = {};
    object_masses.clear();
    body_masses.clear();
    support_masses.clear();
    lines_ends.clear();
    printable_area = Pointfs();
    bed_exclude_area = Pointfs();
    wrapping_exclude_area = Pointfs();
    toolpath_outside = false;
    label_object_enabled = false;
    long_retraction_when_cut = false;
    timelapse_warning_code = 0;
    printable_height = 0.0f;
    machine_frame_transform_active = false;
    belt_tilt_angle = 0.f;
    belt_z_origin   = 0.f;
    settings_ids.reset();
    filaments_count = 0;
    backtrace_enabled = false;
    extruder_colors = std::vector<std::string>();
    filament_diameters = std::vector<float>(MIN_EXTRUDERS_COUNT, DEFAULT_FILAMENT_DIAMETER);
    required_nozzle_HRC = std::vector<int>(MIN_EXTRUDERS_COUNT, DEFAULT_FILAMENT_HRC);
    filament_densities = std::vector<float>(MIN_EXTRUDERS_COUNT, DEFAULT_FILAMENT_DENSITY);
    filament_costs = std::vector<float>(MIN_EXTRUDERS_COUNT, DEFAULT_FILAMENT_COST);
    custom_gcode_per_print_z = std::vector<CustomGCode::Item>();
    spiral_vase_mode = false;
    layer_filaments.clear();
    filament_change_sequence.clear();
    used_mixed_filaments.clear();
    nozzle_change_sequence.clear();
    optimal_assignment.clear();
    filament_change_count_map.clear();
    warnings.clear();
    nozzle_group_result.reset();
    extruder_types.clear();
    printer_extruder_variant.clear();
    printer_extruder_id.clear();
    skippable_part_time.clear();

    unlock();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: this=%2% reset finished")%__LINE__%this;
}

const std::vector<std::pair<GCodeProcessor::EProducer, std::string>> GCodeProcessor::Producers = {
    { EProducer::OrcaSlicer, SLIC3R_APP_NAME },
    { EProducer::OrcaSlicer, "generated by OrcaSlicer" },
    { EProducer::OrcaSlicer, "generated by BambuStudio" },
    { EProducer::OrcaSlicer, "BambuStudio" }
};

unsigned int GCodeProcessor::s_result_id = 0;

bool GCodeProcessor::contains_reserved_tag(const std::string& gcode, std::string& found_tag)
{
    bool ret = false;

    GCodeReader parser;
    auto& _tags = s_IsBBLPrinter ? Reserved_Tags : Reserved_Tags_compatible;
    parser.parse_buffer(gcode, [&ret, &found_tag, _tags](GCodeReader& parser, const GCodeReader::GCodeLine& line) {
        std::string comment = line.raw();
        if (comment.length() > 2 && comment.front() == ';') {
            comment = comment.substr(1);
            for (const std::string& s : _tags) {
                if (boost::starts_with(comment, s)) {
                    ret = true;
                    found_tag = comment;
                    parser.quit_parsing();
                    return;
                }
            }
        }
        });

    return ret;
}

bool GCodeProcessor::contains_reserved_tags(const std::string& gcode, unsigned int max_count, std::vector<std::string>& found_tag, bool is_bbl_printer)
{
    max_count = std::max(max_count, 1U);

    bool ret = false;

    CNumericLocalesSetter locales_setter;

    GCodeReader parser;
    auto& _tags = is_bbl_printer ? Reserved_Tags : Reserved_Tags_compatible;
    parser.parse_buffer(gcode, [&ret, &found_tag, max_count, _tags](GCodeReader& parser, const GCodeReader::GCodeLine& line) {
        std::string comment = line.raw();
        if (comment.length() > 2 && comment.front() == ';') {
            comment = comment.substr(1);
            for (const std::string& s : _tags) {
                if (boost::starts_with(comment, s)) {
                    ret = true;
                    found_tag.push_back(comment);
                    if (found_tag.size() == max_count) {
                        parser.quit_parsing();
                        return;
                    }
                }
            }
        }
        });

    return ret;
}

GCodeProcessor::GCodeProcessor()
: m_options_z_corrector(m_result)
{
    reset();
    m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].line_m73_main_mask = "M73 P%s R%s\n";
    m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].line_m73_stop_mask = "M73 C%s\n";
    m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].line_m73_main_mask = "M73 Q%s S%s\n";
    m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].line_m73_stop_mask = "M73 D%s\n";

    register_commands();
}

void GCodeProcessor::register_commands()
{
    std::unordered_map<std::string, CommandProcessor::command_handler_t> command_handler_list = {
        {"G0", [this](const GCodeReader::GCodeLine& line) { process_G0(line); }},
        {"G1", [this](const GCodeReader::GCodeLine& line) { process_G1(line); }},
        {"G2", [this](const GCodeReader::GCodeLine& line) { process_G2_G3(line, true); }},
        {"G3", [this](const GCodeReader::GCodeLine& line) { process_G2_G3(line, false); }},
        {"G4", [this](const GCodeReader::GCodeLine& line) { process_G4(line); }},

        {"G10", [this](const GCodeReader::GCodeLine& line) { process_G10(line); }},
        {"G11", [this](const GCodeReader::GCodeLine& line) { process_G11(line); }},

        {"G20", [this](const GCodeReader::GCodeLine& line) { process_G20(line); }},
        {"G21", [this](const GCodeReader::GCodeLine& line) { process_G21(line); }},
        {"G22", [this](const GCodeReader::GCodeLine& line) { process_G22(line); }},
        {"G23", [this](const GCodeReader::GCodeLine& line) { process_G23(line); }},
        {"G28", [this](const GCodeReader::GCodeLine& line) { process_G28(line); }},
        {"G29", [this](const GCodeReader::GCodeLine& line) { process_G29(line); }},

        {"G90", [this](const GCodeReader::GCodeLine& line) { process_G90(line); }},
        {"G91", [this](const GCodeReader::GCodeLine& line) { process_G91(line); }},
        {"G92", [this](const GCodeReader::GCodeLine& line) { process_G92(line); }},
        {"G93", [this](const GCodeReader::GCodeLine& line) { process_G93(line); }},
        {"G94", [this](const GCodeReader::GCodeLine& line) { process_G94(line); }},

        {"M1", [this](const GCodeReader::GCodeLine& line) { process_M1(line); }},

        {"M82", [this](const GCodeReader::GCodeLine& line) { process_M82(line); }},
        {"M83", [this](const GCodeReader::GCodeLine& line) { process_M83(line); }},

        {"M104", [this](const GCodeReader::GCodeLine& line) { process_M104(line); }},
        {"M106", [this](const GCodeReader::GCodeLine& line) { process_M106(line); }},
        {"M107", [this](const GCodeReader::GCodeLine& line) { process_M107(line); }},
        {"M108", [this](const GCodeReader::GCodeLine& line) { process_M108(line); }},
        {"M109", [this](const GCodeReader::GCodeLine& line) { process_M109(line); }},

        {"M132", [this](const GCodeReader::GCodeLine& line) { process_M132(line); }},
        {"M135", [this](const GCodeReader::GCodeLine& line) { process_M135(line); }},

        {"M140", [this](const GCodeReader::GCodeLine& line) { process_M140(line); }},
        {"M190", [this](const GCodeReader::GCodeLine& line) { process_M190(line); }},
        {"M191", [this](const GCodeReader::GCodeLine& line) { process_M191(line); }},

        {"M201", [this](const GCodeReader::GCodeLine& line) { process_M201(line); }},
        {"M203", [this](const GCodeReader::GCodeLine& line) { process_M203(line); }},
        {"M204", [this](const GCodeReader::GCodeLine& line) { process_M204(line); }},
        {"M205", [this](const GCodeReader::GCodeLine& line) { process_M205(line); }},
        {"M221", [this](const GCodeReader::GCodeLine& line) { process_M221(line); }},

        {"M400", [this](const GCodeReader::GCodeLine& line) { process_M400(line); }},
        {"M401", [this](const GCodeReader::GCodeLine& line) { process_M401(line); }},
        {"M402", [this](const GCodeReader::GCodeLine& line) { process_M402(line); }},
        {"M566", [this](const GCodeReader::GCodeLine& line) { process_M566(line); }},
        {"M702", [this](const GCodeReader::GCodeLine& line) { process_M702(line); }},
        {"M1020", [this](const GCodeReader::GCodeLine& line) { process_M1020(line); }},
        {"M900", [this](const GCodeReader::GCodeLine& line) { process_M900(line); }},
        {"M572", [this](const GCodeReader::GCodeLine& line) { process_M572(line); }},

        {"T", [this](const GCodeReader::GCodeLine& line) { process_T(line); }},
        {"SYNC", [this](const GCodeReader::GCodeLine& line) { process_SYNC(line); }},

        {"VG1", [this](const GCodeReader::GCodeLine& line) { process_VG1(line); }},
        {"VM104", [this](const GCodeReader::GCodeLine& line) { process_VM104(line); }},
        {"VM109", [this](const GCodeReader::GCodeLine& line) { process_VM109(line); }},
        {"M622", [this](const GCodeReader::GCodeLine& line) { process_M622(line);}},
        {"M623", [this](const GCodeReader::GCodeLine& line) { process_M623(line);}},
        {"M6211", [this](const GCodeReader::GCodeLine& line) { process_M6211(line); }}
    };

    std::unordered_set<std::string>early_quit_commands = {
        "T"
    };

    auto to_lowercase = [](std::string str)->std::string {
        std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
            return std::tolower(c);
            });
        return str;
        };

    for (auto elem : command_handler_list) {
        auto& uppercase_cmd = elem.first;
        auto& handler = elem.second;
        bool early_quit = early_quit_commands.count(uppercase_cmd) > 0;
        m_command_processor.register_command(uppercase_cmd, handler,early_quit);
        if (auto lowercase_cmd = to_lowercase(uppercase_cmd); lowercase_cmd != uppercase_cmd)
            m_command_processor.register_command(lowercase_cmd, handler,early_quit);
    }
}

void GCodeProcessor::process_M6211(const GCodeReader::GCodeLine& line)
{
    if (boost::algorithm::istarts_with(m_printer_model, "elegoo"))
        process_elegoo_M6211(line);
}

bool GCodeProcessor::check_multi_extruder_gcode_valid(const int                         extruder_size,
                                                      const Pointfs                     plate_printable_area,
                                                      const double                      plate_printable_height,
                                                      const Pointfs                     wrapping_exclude_area,
                                                      const std::vector<Polygons>      &unprintable_areas,
                                                      const std::vector<double>        &printable_heights,
                                                      const std::vector<int>           &filament_map,
                                                      const std::vector<std::set<int>> &unprintable_filament_types)
{
    m_result.limit_filament_maps.clear();
    m_result.gcode_check_result.reset();
    Polygon plate_printable_poly = Polygon::new_scale(plate_printable_area);
    Polygon wrapping_exclude_poly = Polygon::new_scale(wrapping_exclude_area);

    m_result.limit_filament_maps.resize(filament_map.size(), 0);

    auto to_2d = [](const Vec3d &pos) -> Point {
        Point ps(scale_(pos.x()), scale_(pos.y()));
        return ps;
    };

    const bool machine_frame_active = m_machine_frame_transform.is_active();
    auto compare_pos = [&](const GCodeProcessorResult::MoveVertex &move) -> Vec3d {
        Vec3d pos = move.position.cast<double>();
        if (!machine_frame_active)
            return pos;
        Vec3d extruder_off = Vec3d::Zero();
        if (size_t(move.extruder_id) < m_extruder_offsets.size())
            extruder_off = m_extruder_offsets[move.extruder_id].cast<double>();
        Vec3d machine(pos.x() - m_x_offset - extruder_off.x(),
                      pos.y() - m_y_offset - extruder_off.y(),
                      pos.z() - extruder_off.z() + m_z_offset);
        Vec3d build = m_machine_frame_transform.apply_inverse(machine);
        return Vec3d(build.x() + m_x_offset, build.y() + m_y_offset, build.z());
    };

    struct GCodePosInfo
    {
        Points pos;
        Points pos_custom;
        float  max_print_z;
        float  max_print_z_custom;
    };
    std::map<int, std::map<int, GCodePosInfo>> gcode_path_pos;
    for (const GCodeProcessorResult::MoveVertex &move : m_result.moves) {
        const int object_label_id = m_result.state(move).object_label_id;
        if (move.type == EMoveType::Extrude ) {
            const Vec3d cp = compare_pos(move);
            const float z_for_height = machine_frame_active ? float(cp.z()) : m_result.state(move).print_z;
            if (move.extrusion_role == ExtrusionRole::erCustom) {
                gcode_path_pos[object_label_id][int(move.extruder_id)].pos_custom.emplace_back(to_2d(cp));
                gcode_path_pos[object_label_id][int(move.extruder_id)].max_print_z_custom =
                    std::max(gcode_path_pos[object_label_id][int(move.extruder_id)].max_print_z_custom, z_for_height);
            } else {
                gcode_path_pos[object_label_id][int(move.extruder_id)].pos.emplace_back(to_2d(cp));
                gcode_path_pos[object_label_id][int(move.extruder_id)].max_print_z = std::max(gcode_path_pos[object_label_id][int(move.extruder_id)].max_print_z,
                                                                                                   z_for_height);
            }
        }
    }

    bool valid = true;
    Point plate_offset = Point(scale_(m_x_offset), scale_(m_y_offset));
    plate_printable_poly.translate(plate_offset);
    BoundingBox plate_printable_bbox = plate_printable_poly.bounding_box();
    if (plate_printable_poly.is_valid()) {
        plate_printable_bbox.offset(scale_(2.0));
    } else
        plate_printable_bbox.defined = false;

    for (auto obj_iter = gcode_path_pos.begin(); obj_iter != gcode_path_pos.end(); ++obj_iter) {
        int                                object_label_id = obj_iter->first;
        const std::map<int, GCodePosInfo> &path_pos        = obj_iter->second;
        for (auto iter = path_pos.begin(); iter != path_pos.end(); ++iter) {
            int extruder_id = filament_map[iter->first] - 1;
            Points iter_points;
            iter_points.insert(iter_points.end(), iter->second.pos.begin(), iter->second.pos.end());
            Polygon     path_poly(iter_points);
            if (path_poly.empty()) continue;
            BoundingBox bbox = path_poly.bounding_box();
            if (plate_printable_bbox.defined) {
                if (!plate_printable_bbox.contains(bbox)) {
                    m_result.gcode_check_result.error_code |= (1<<2);
                    std::pair<int, int> filament_to_object_id;
                    filament_to_object_id.first  = iter->first;
                    filament_to_object_id.second = object_label_id;
                    m_result.gcode_check_result.print_area_error_infos[extruder_id].push_back(filament_to_object_id);
                    valid = false;
                }
            }
            if ( !m_belt_printer && iter->second.max_print_z > plate_printable_height ) {
                m_result.gcode_check_result.error_code |= (1 << 3);
                std::pair<int, int> filament_to_object_id;
                filament_to_object_id.first  = iter->first;
                filament_to_object_id.second = object_label_id;
                m_result.gcode_check_result.print_height_error_infos[extruder_id].push_back(filament_to_object_id);
                valid = false;
            }

            if (extruder_size > 1) {
                if ((extruder_id < unprintable_areas.size()) && !unprintable_areas[extruder_id].empty())
                    for (Polygon poly : unprintable_areas[extruder_id]) {
                        poly.translate(plate_offset);
                        if (poly.bounding_box().overlap(bbox)) {
                            m_result.gcode_check_result.error_code |= 1;
                            std::pair<int, int>         filament_to_object_id;
                            filament_to_object_id.first = iter->first;
                            filament_to_object_id.second = object_label_id;
                            m_result.gcode_check_result.print_area_error_infos[extruder_id].push_back(filament_to_object_id);
                            valid = false;
                        }
                    }

                if (!m_belt_printer && (extruder_id < printable_heights.size()) && (iter->second.max_print_z > printable_heights[extruder_id])) {
                    m_result.gcode_check_result.error_code |= (1 << 1);
                    std::pair<int, int> filament_to_object_id;
                    filament_to_object_id.first  = iter->first;
                    filament_to_object_id.second = object_label_id;
                    m_result.gcode_check_result.print_height_error_infos[extruder_id].push_back(filament_to_object_id);
                    m_result.limit_filament_maps[iter->first] |= (1 << extruder_id);
                    valid = false;
                }

                for (int i = 0; i < unprintable_areas.size(); ++i) {
                    for (Polygon poly : unprintable_areas[i]) {
                        poly.translate(plate_offset);
                        if (!poly.bounding_box().overlap(bbox)) continue;

                        m_result.limit_filament_maps[iter->first] |= (1 << i);
                    }
                }
            }
        }
    }

    for (int extruder_id = 0; extruder_id < unprintable_filament_types.size(); ++extruder_id) {
        const std::set<int> &filament_ids = unprintable_filament_types[extruder_id];
        for (int filament_id : filament_ids) {
            m_result.limit_filament_maps[filament_id] |= (1 << extruder_id);
        }
    };

    return valid;
}

void GCodeProcessor::apply_config(const PrintConfig& config)
{
    m_parser.apply_config(config);

    m_flavor = config.gcode_flavor;
    m_printer_model = config.printer_model.value;

    m_single_extruder_multi_material = config.single_extruder_multi_material;

    size_t filament_count = config.filament_diameter.values.size();
    m_result.filaments_count = filament_count;

    m_is_XL_printer = is_XL_printer(config);
    m_preheat_time = config.preheat_time;
    m_preheat_steps = config.preheat_steps;
    if(m_preheat_steps < 1)
        m_preheat_steps = 1;
    m_result.backtrace_enabled = config.ooze_prevention && m_preheat_time > 0 && (m_is_XL_printer || (!m_single_extruder_multi_material && filament_count > 1));

    assert(config.nozzle_volume.size() == config.nozzle_diameter.size());
    m_nozzle_volume.resize(config.nozzle_volume.size());
    for (size_t idx = 0; idx < config.nozzle_volume.size(); ++idx)
        m_nozzle_volume[idx] = config.nozzle_volume.values[idx];

    m_physical_extruder_map = config.physical_extruder_map.values;
    m_extruder_max_nozzle_count = config.extruder_max_nozzle_count.values;

    m_filament_types.resize(filament_count);
    for (size_t idx = 0; idx < filament_count; ++idx)
        m_filament_types[idx] = config.filament_type.get_at(idx);
    m_nozzle_diameter                    = config.nozzle_diameter.values;
    m_hotend_cooling_rate                = config.hotend_cooling_rate.values;
    m_hotend_heating_rate                = config.hotend_heating_rate.values;
    m_filament_pre_cooling_temp          = config.filament_pre_cooling_temperature.values;
    m_filament_preheat_temperature_delta = config.filament_preheat_temperature_delta.values;
    m_enable_pre_heating                 = config.enable_pre_heating.value;
    if (const ConfigOptionBool* has_switcher = config.option<ConfigOptionBool>("has_filament_switcher"))
        m_has_filament_switcher = has_switcher->value;
    m_result.extruder_types.resize(config.extruder_type.values.size());
    for (size_t idx = 0; idx < config.extruder_type.values.size(); ++idx)
        m_result.extruder_types[idx] = static_cast<ExtruderType>(config.extruder_type.values[idx]);
    m_result.printer_extruder_variant = config.printer_extruder_variant.values;
    m_result.printer_extruder_id      = config.printer_extruder_id.values;

    m_extruder_offsets.resize(filament_count);
    m_extruder_colors.resize(filament_count);
    m_result.filament_diameters.resize(filament_count);
    m_result.required_nozzle_HRC.resize(filament_count);
    m_result.filament_densities.resize(filament_count);
    m_result.filament_vitrification_temperature.resize(filament_count);
    m_result.filament_costs.resize(filament_count);
    m_extruder_temps.resize(filament_count);
    m_filament_nozzle_temp.resize(filament_count);
    m_filament_nozzle_temp_first_layer.resize(filament_count);
    m_result.nozzle_hrc = static_cast<int>(config.nozzle_hrc.getInt());
    std::vector<NozzleType>(config.nozzle_type.size()).swap(m_result.nozzle_type);
    for (size_t idx = 0; idx < m_result.nozzle_type.size(); ++idx) {
        m_result.nozzle_type[idx] = NozzleType(config.nozzle_type.values[idx]);
    }

    std::vector<int> filament_map = config.filament_map.values;
    filament_map.resize(filament_count, config.master_extruder_id.value);

    for (size_t i = 0; i < filament_count; ++ i) {
        m_extruder_offsets[i] = to_3d(config.extruder_offset.get_at(filament_map[i] - 1).cast<float>().eval(), 0.f);
        m_extruder_colors[i]            = static_cast<unsigned char>(i);
        m_filament_nozzle_temp_first_layer[i] = static_cast<int>(config.nozzle_temperature_initial_layer.get_at(i));
        m_filament_nozzle_temp[i]      = static_cast<int>(config.nozzle_temperature.get_at(i));
        if (m_filament_nozzle_temp[i] == 0) {
            m_filament_nozzle_temp[i] = m_filament_nozzle_temp_first_layer[i];
        }
        m_result.filament_diameters[i]  = static_cast<float>(config.filament_diameter.get_at(i));
        m_result.required_nozzle_HRC[i] = static_cast<int>(config.required_nozzle_HRC.get_at(i));
        m_result.filament_densities[i]  = static_cast<float>(config.filament_density.get_at(i));
        m_result.filament_vitrification_temperature[i] = static_cast<float>(config.temperature_vitrification.get_at(i));
        m_result.filament_costs[i]      = static_cast<float>(config.filament_cost.get_at(i));
    }

    if (m_flavor == gcfMarlinLegacy || m_flavor == gcfMarlinFirmware || m_flavor == gcfKlipper || m_flavor == gcfRepRapFirmware) {
        m_time_processor.machine_limits = reinterpret_cast<const MachineEnvelopeConfig&>(config);
        if (m_flavor == gcfMarlinLegacy || m_flavor == gcfKlipper) {
            m_time_processor.machine_limits.machine_max_acceleration_travel = m_time_processor.machine_limits.machine_max_acceleration_extruding;
        }
        if (m_flavor == gcfRepRapFirmware) {
            m_time_processor.machine_limits.machine_min_travel_rate.values.assign(m_time_processor.machine_limits.machine_min_travel_rate.size(), 0.);
            m_time_processor.machine_limits.machine_min_extruding_rate.values.assign(m_time_processor.machine_limits.machine_min_extruding_rate.size(), 0.);
        }
    }

    m_time_processor.filament_load_times = static_cast<float>(config.machine_load_filament_time.value);
    m_time_processor.filament_unload_times = static_cast<float>(config.machine_unload_filament_time.value);
    m_time_processor.machine_tool_change_time = static_cast<float>(config.machine_tool_change_time.value);

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        float max_acceleration = get_option_value(m_time_processor.machine_limits.machine_max_acceleration_extruding, i);
        m_time_processor.machines[i].max_acceleration = max_acceleration;
        m_time_processor.machines[i].acceleration     = (max_acceleration > 0.0f) ? max_acceleration : DEFAULT_ACCELERATION;
        float max_retract_acceleration = get_option_value(m_time_processor.machine_limits.machine_max_acceleration_retracting, i);
        m_time_processor.machines[i].max_retract_acceleration = max_retract_acceleration;
        m_time_processor.machines[i].retract_acceleration     = (max_retract_acceleration > 0.0f) ? max_retract_acceleration :
                                                                                                    DEFAULT_RETRACT_ACCELERATION;
        float max_travel_acceleration = get_option_value(m_time_processor.machine_limits.machine_max_acceleration_travel, i);
        if (!GCodeWriter::supports_separate_travel_acceleration(config.gcode_flavor.value)){
            max_travel_acceleration = 0;
        }
        m_time_processor.machines[i].max_travel_acceleration = max_travel_acceleration;
        m_time_processor.machines[i].travel_acceleration     = (max_travel_acceleration > 0.0f) ? max_travel_acceleration :
                                                                                                  DEFAULT_TRAVEL_ACCELERATION;
    }

    m_disable_m73 = config.disable_m73;

    const ConfigOptionFloat* initial_layer_print_height = config.option<ConfigOptionFloat>("initial_layer_print_height");
    if (initial_layer_print_height != nullptr)
        m_first_layer_height = std::abs(initial_layer_print_height->value);

    m_result.printable_height = config.printable_height;

    m_machine_frame_transform.init_from_config(config);
    m_result.machine_frame_transform_active = m_machine_frame_transform.is_active();
    m_belt_printer = config.belt_printer.value;

    auto filament_maps = config.option<ConfigOptionInts>("filament_map");
    if (filament_maps != nullptr) {
        m_filament_maps = filament_maps->values;
        std::transform(m_filament_maps.begin(), m_filament_maps.end(), m_filament_maps.begin(), [](int value) {return value - 1; });
    }

    const ConfigOptionBool* spiral_vase = config.option<ConfigOptionBool>("spiral_mode");
    if (spiral_vase != nullptr) {
        m_detect_layer_based_on_tag = spiral_vase->value;
        m_result.spiral_vase_mode = spiral_vase->value;
    }

    const ConfigOptionBool* has_scarf_joint_seam = config.option<ConfigOptionBool>("has_scarf_joint_seam");
    if (has_scarf_joint_seam != nullptr)
        m_detect_layer_based_on_tag = m_detect_layer_based_on_tag || has_scarf_joint_seam->value;

    const ConfigOptionBool* manual_filament_change = config.option<ConfigOptionBool>("manual_filament_change");
    if (manual_filament_change != nullptr)
        m_manual_filament_change = manual_filament_change->value;

    const ConfigOptionFloat* z_offset = config.option<ConfigOptionFloat>("z_offset");
    if (z_offset != nullptr)
        m_z_offset = z_offset->value;

    if (config.nonplanar_kinematics.value != RotaryLayout::None) {
        m_tool_kinematics = make_tool_kinematics(config);
        m_tool_kinematics->inverse({ Vec3d::Zero(), Vec3d::UnitZ() }, nullptr, m_joints);
    }
}

void GCodeProcessor::apply_config(const DynamicPrintConfig& config)
{
    m_parser.apply_config(config);

    m_belt_render_config.clear();
    {
        const auto *belt = config.option<ConfigOptionBool>("belt_printer");
        if (belt != nullptr) {
            static const char *belt_keys[] = {
                "belt_printer", "belt_slice_rotation", "belt_slice_rotation_angle",
                "gcode_remap_x", "gcode_remap_y", "gcode_remap_z",
                "belt_frame_tilt_decouple", "belt_frame_tilt_angle",
            };
            for (const char *key : belt_keys)
                if (const ConfigOption *opt = config.option(key); opt != nullptr)
                    m_belt_render_config.set_key_value(key, opt->clone());
            static const char *bed_keys[] = { "printable_area", "printable_height" };
            if (belt->value)
                for (const char *key : bed_keys)
                    if (const ConfigOption *opt = config.option(key); opt != nullptr)
                        m_belt_render_config.set_key_value(key, opt->clone());
        }
    }

    const ConfigOptionFloatsNullable* nozzle_volume = config.option<ConfigOptionFloatsNullable>("nozzle_volume");
    if (nozzle_volume != nullptr) {
        m_nozzle_volume.resize(nozzle_volume->size(), 0);
        for (size_t idx = 0; idx < nozzle_volume->size(); ++idx)
            m_nozzle_volume[idx] = nozzle_volume->values[idx];
    }

    const ConfigOptionInt *nozzle_HRC = config.option<ConfigOptionInt>("nozzle_hrc");
    if (nozzle_HRC != nullptr) m_result.nozzle_hrc = nozzle_HRC->value;

    const ConfigOptionInts* physical_extruder_map = config.option<ConfigOptionInts>("physical_extruder_map");
    if (physical_extruder_map != nullptr) {
        m_physical_extruder_map = physical_extruder_map->values;
    }

    const ConfigOptionIntsNullable* extruder_max_nozzle_count = config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    if (extruder_max_nozzle_count != nullptr) {
        m_extruder_max_nozzle_count = extruder_max_nozzle_count->values;
    }

    const ConfigOptionFloatsNullable* hotend_cooling_rate = config.option<ConfigOptionFloatsNullable>("hotend_cooling_rate");
    if (hotend_cooling_rate != nullptr)
        m_hotend_cooling_rate = hotend_cooling_rate->values;

    const ConfigOptionFloatsNullable* hotend_heating_rate = config.option<ConfigOptionFloatsNullable>("hotend_heating_rate");
    if (hotend_heating_rate != nullptr)
        m_hotend_heating_rate = hotend_heating_rate->values;

    const ConfigOptionIntsNullable* filament_pre_cooling_temperature = config.option<ConfigOptionIntsNullable>("filament_pre_cooling_temperature");
    if (filament_pre_cooling_temperature != nullptr)
        m_filament_pre_cooling_temp = filament_pre_cooling_temperature->values;

    const ConfigOptionFloatsNullable* filament_preheat_temperature_delta = config.option<ConfigOptionFloatsNullable>("filament_preheat_temperature_delta");
    if (filament_preheat_temperature_delta != nullptr)
        m_filament_preheat_temperature_delta = filament_preheat_temperature_delta->values;

    const ConfigOptionBool* enable_pre_heating = config.option<ConfigOptionBool>("enable_pre_heating");
    if (enable_pre_heating != nullptr)
        m_enable_pre_heating = enable_pre_heating->value;

    const ConfigOptionBool* handle_hotend_as_extruder = config.option<ConfigOptionBool>("handle_hotend_as_extruder");
    if (handle_hotend_as_extruder != nullptr)
        m_handle_hotend_as_extruder = handle_hotend_as_extruder->value;

    const ConfigOptionBool* has_filament_switcher = config.option<ConfigOptionBool>("has_filament_switcher");
    if (has_filament_switcher != nullptr)
        m_has_filament_switcher = has_filament_switcher->value;

    const ConfigOptionFloats* nozzle_diameter = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle_diameter != nullptr)
        m_nozzle_diameter = nozzle_diameter->values;

    const ConfigOptionStrings* filament_type = config.option<ConfigOptionStrings>("filament_type");
    if (filament_type != nullptr) {
        m_filament_types.resize(filament_type->size());
        for (size_t idx = 0; idx < filament_type->size(); ++idx)
            m_filament_types[idx] = filament_type->get_at(idx);
    }

    const ConfigOptionEnumsGeneric* extruder_type = config.option<ConfigOptionEnumsGeneric>("extruder_type");
    if (extruder_type != nullptr) {
        m_result.extruder_types.resize(extruder_type->values.size());
        for (size_t idx = 0; idx < extruder_type->values.size(); ++idx)
            m_result.extruder_types[idx] = static_cast<ExtruderType>(extruder_type->values[idx]);
    }

    if (const ConfigOptionStrings* pe_variant = config.option<ConfigOptionStrings>("printer_extruder_variant"))
        m_result.printer_extruder_variant = pe_variant->values;
    if (const ConfigOptionInts* pe_id = config.option<ConfigOptionInts>("printer_extruder_id"))
        m_result.printer_extruder_id = pe_id->values;

    const ConfigOptionEnumsGenericNullable* nozzle_type = config.option<ConfigOptionEnumsGenericNullable>("nozzle_type");
    if (nozzle_type != nullptr) {
        m_result.nozzle_type.resize(nozzle_type->size());
        for (size_t idx = 0; idx < nozzle_type->values.size(); ++idx) {
            m_result.nozzle_type[idx] = NozzleType(nozzle_type->values[idx]);
        }
    }

    const ConfigOptionEnum<GCodeFlavor>* gcode_flavor = config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    if (gcode_flavor != nullptr)
        m_flavor = gcode_flavor->value;

    const ConfigOptionPoints* printable_area = config.option<ConfigOptionPoints>("printable_area");
    if (printable_area != nullptr)
        m_result.printable_area = make_counter_clockwise(printable_area->values);

    const ConfigOptionPoints* bed_exclude_area = config.option<ConfigOptionPoints>("bed_exclude_area");
    if (bed_exclude_area != nullptr)
        m_result.bed_exclude_area = bed_exclude_area->values;

    const ConfigOptionPoints* wrapping_exclude_area = config.option<ConfigOptionPoints>("wrapping_exclude_area");
    if (wrapping_exclude_area != nullptr)
        m_result.wrapping_exclude_area = wrapping_exclude_area->values;

    const ConfigOptionString* print_settings_id = config.option<ConfigOptionString>("print_settings_id");
    if (print_settings_id != nullptr)
        m_result.settings_ids.print = print_settings_id->value;

    const ConfigOptionStrings* filament_settings_id = config.option<ConfigOptionStrings>("filament_settings_id");
    if (filament_settings_id != nullptr)
        m_result.settings_ids.filament = filament_settings_id->values;

    const ConfigOptionString* printer_settings_id = config.option<ConfigOptionString>("printer_settings_id");
    if (printer_settings_id != nullptr)
        m_result.settings_ids.printer = printer_settings_id->value;

    const ConfigOptionString* printer_model = config.option<ConfigOptionString>("printer_model");
    if (printer_model != nullptr)
        m_printer_model = printer_model->value;

    m_result.filaments_count = config.option<ConfigOptionFloats>("filament_diameter")->values.size();

    const ConfigOptionFloats* filament_diameters = config.option<ConfigOptionFloats>("filament_diameter");
    if (filament_diameters != nullptr) {
        m_result.filament_diameters.clear();
        m_result.filament_diameters.resize(filament_diameters->values.size());
        for (size_t i = 0; i < filament_diameters->values.size(); ++i) {
            m_result.filament_diameters[i] = static_cast<float>(filament_diameters->values[i]);
        }
    }

    if (m_result.filament_diameters.size() < m_result.filaments_count) {
        for (size_t i = m_result.filament_diameters.size(); i < m_result.filaments_count; ++i) {
            m_result.filament_diameters.emplace_back(DEFAULT_FILAMENT_DIAMETER);
        }
    }

    const ConfigOptionInts *filament_HRC = config.option<ConfigOptionInts>("required_nozzle_HRC");
    if (filament_HRC != nullptr) {
        m_result.required_nozzle_HRC.clear();
        m_result.required_nozzle_HRC.resize(filament_HRC->values.size());
        for (size_t i = 0; i < filament_HRC->values.size(); ++i) { m_result.required_nozzle_HRC[i] = static_cast<float>(filament_HRC->values[i]); }
    }

    if (m_result.required_nozzle_HRC.size() < m_result.filaments_count) {
        for (size_t i = m_result.required_nozzle_HRC.size(); i < m_result.filaments_count; ++i) { m_result.required_nozzle_HRC.emplace_back(DEFAULT_FILAMENT_HRC);
        }
    }

    const ConfigOptionFloats* filament_densities = config.option<ConfigOptionFloats>("filament_density");
    if (filament_densities != nullptr) {
        m_result.filament_densities.clear();
        m_result.filament_densities.resize(filament_densities->values.size());
        for (size_t i = 0; i < filament_densities->values.size(); ++i) {
            m_result.filament_densities[i] = static_cast<float>(filament_densities->values[i]);
        }
    }

    if (m_result.filament_densities.size() < m_result.filaments_count) {
        for (size_t i = m_result.filament_densities.size(); i < m_result.filaments_count; ++i) {
            m_result.filament_densities.emplace_back(DEFAULT_FILAMENT_DENSITY);
        }
    }

    auto filament_maps = config.option<ConfigOptionInts>("filament_map");
    if (filament_maps != nullptr) {
        m_filament_maps = filament_maps->values;
        std::transform(m_filament_maps.begin(), m_filament_maps.end(), m_filament_maps.begin(), [](int value) {return value - 1; });
    }

    const ConfigOptionFloats* filament_costs = config.option<ConfigOptionFloats>("filament_cost");
    if (filament_costs != nullptr) {
        m_result.filament_costs.clear();
        m_result.filament_costs.resize(filament_costs->values.size());
        for (size_t i = 0; i < filament_costs->values.size(); ++i)
            m_result.filament_costs[i]=static_cast<float>(filament_costs->values[i]);
    }
    for (size_t i = m_result.filament_costs.size(); i < m_result.filaments_count; ++i) {
        m_result.filament_costs.emplace_back(DEFAULT_FILAMENT_COST);
    }

    const ConfigOptionInts* filament_vitrification_temperature = config.option<ConfigOptionInts>("temperature_vitrification");
    if (filament_vitrification_temperature != nullptr) {
        m_result.filament_vitrification_temperature.clear();
        m_result.filament_vitrification_temperature.resize(filament_vitrification_temperature->values.size());
        for (size_t i = 0; i < filament_vitrification_temperature->values.size(); ++i) {
            m_result.filament_vitrification_temperature[i] = static_cast<int>(filament_vitrification_temperature->values[i]);
        }
    }
    if (m_result.filament_vitrification_temperature.size() < m_result.filaments_count) {
        for (size_t i = m_result.filament_vitrification_temperature.size(); i < m_result.filaments_count; ++i) {
            m_result.filament_vitrification_temperature.emplace_back(DEFAULT_FILAMENT_VITRIFICATION_TEMPERATURE);
        }
    }

    const ConfigOptionPoints* extruder_offset = config.option<ConfigOptionPoints>("extruder_offset");
    const ConfigOptionBool* single_extruder_multi_material = config.option<ConfigOptionBool>("single_extruder_multi_material");
    if (extruder_offset != nullptr) {
        if (single_extruder_multi_material != nullptr && single_extruder_multi_material->getBool()) {
            Vec2f offset = extruder_offset->values[0].cast<float>();
            m_extruder_offsets.resize(m_result.filaments_count);
            for (size_t i = 0; i < m_result.filaments_count; ++i) {
                m_extruder_offsets[i] = { offset(0), offset(1), 0.0f };
            }
        }
        else {
            m_extruder_offsets.resize(extruder_offset->values.size());
            for (size_t i = 0; i < extruder_offset->values.size(); ++i) {
                Vec2f offset = extruder_offset->values[i].cast<float>();
                m_extruder_offsets[i] = { offset(0), offset(1), 0.0f };
            }
        }
    }

    if (m_extruder_offsets.size() < m_result.filaments_count) {
        for (size_t i = m_extruder_offsets.size(); i < m_result.filaments_count; ++i) {
            m_extruder_offsets.emplace_back(DEFAULT_EXTRUDER_OFFSET);
        }
    }

    const ConfigOptionStrings* filament_colour = config.option<ConfigOptionStrings>("filament_colour");
    if (filament_colour != nullptr && filament_colour->values.size() == m_result.extruder_colors.size()) {
        for (size_t i = 0; i < m_result.extruder_colors.size(); ++i) {
            if (m_result.extruder_colors[i].empty())
                m_result.extruder_colors[i] = filament_colour->values[i];
        }
    }

    if (m_result.extruder_colors.size() < m_result.filaments_count) {
        for (size_t i = m_result.extruder_colors.size(); i < m_result.filaments_count; ++i) {
            m_result.extruder_colors.emplace_back(std::string());
        }
    }

    for (size_t i = 0; i < m_result.extruder_colors.size(); ++i) {
        if (m_result.extruder_colors[i].empty())
            m_result.extruder_colors[i] = "#FF8000";
    }

    m_extruder_colors.resize(m_result.extruder_colors.size());
    for (size_t i = 0; i < m_result.extruder_colors.size(); ++i) {
        m_extruder_colors[i] = static_cast<unsigned char>(i);
    }

    m_extruder_temps.resize(m_result.filaments_count);

    const ConfigOptionFloat* machine_load_filament_time = config.option<ConfigOptionFloat>("machine_load_filament_time");
    if (machine_load_filament_time != nullptr)
        m_time_processor.filament_load_times = static_cast<float>(machine_load_filament_time->value);

    const ConfigOptionFloat* machine_unload_filament_time = config.option<ConfigOptionFloat>("machine_unload_filament_time");
    if (machine_unload_filament_time != nullptr)
        m_time_processor.filament_unload_times = static_cast<float>(machine_unload_filament_time->value);

    const ConfigOptionFloat* machine_tool_change_time = config.option<ConfigOptionFloat>("machine_tool_change_time");
    if (machine_tool_change_time != nullptr)
        m_time_processor.machine_tool_change_time = static_cast<float>(machine_tool_change_time->value);

    if (m_flavor == gcfMarlinLegacy || m_flavor == gcfMarlinFirmware || m_flavor == gcfKlipper) {
        const ConfigOptionFloats* machine_max_acceleration_x = config.option<ConfigOptionFloats>("machine_max_acceleration_x");
        if (machine_max_acceleration_x != nullptr)
            m_time_processor.machine_limits.machine_max_acceleration_x.values = machine_max_acceleration_x->values;

        const ConfigOptionFloats* machine_max_acceleration_y = config.option<ConfigOptionFloats>("machine_max_acceleration_y");
        if (machine_max_acceleration_y != nullptr)
            m_time_processor.machine_limits.machine_max_acceleration_y.values = machine_max_acceleration_y->values;

        const ConfigOptionFloats* machine_max_acceleration_z = config.option<ConfigOptionFloats>("machine_max_acceleration_z");
        if (machine_max_acceleration_z != nullptr)
            m_time_processor.machine_limits.machine_max_acceleration_z.values = machine_max_acceleration_z->values;

        const ConfigOptionFloats* machine_max_acceleration_e = config.option<ConfigOptionFloats>("machine_max_acceleration_e");
        if (machine_max_acceleration_e != nullptr)
            m_time_processor.machine_limits.machine_max_acceleration_e.values = machine_max_acceleration_e->values;

        const ConfigOptionFloats* machine_max_speed_x = config.option<ConfigOptionFloats>("machine_max_speed_x");
        if (machine_max_speed_x != nullptr)
            m_time_processor.machine_limits.machine_max_speed_x.values = machine_max_speed_x->values;

        const ConfigOptionFloats* machine_max_speed_y = config.option<ConfigOptionFloats>("machine_max_speed_y");
        if (machine_max_speed_y != nullptr)
            m_time_processor.machine_limits.machine_max_speed_y.values = machine_max_speed_y->values;

        const ConfigOptionFloats* machine_max_speed_z = config.option<ConfigOptionFloats>("machine_max_speed_z");
        if (machine_max_speed_z != nullptr)
            m_time_processor.machine_limits.machine_max_speed_z.values = machine_max_speed_z->values;

        const ConfigOptionFloats* machine_max_speed_e = config.option<ConfigOptionFloats>("machine_max_speed_e");
        if (machine_max_speed_e != nullptr)
            m_time_processor.machine_limits.machine_max_speed_e.values = machine_max_speed_e->values;

        const ConfigOptionFloats* machine_max_jerk_x = config.option<ConfigOptionFloats>("machine_max_jerk_x");
        if (machine_max_jerk_x != nullptr)
            m_time_processor.machine_limits.machine_max_jerk_x.values = machine_max_jerk_x->values;

        const ConfigOptionFloats* machine_max_jerk_y = config.option<ConfigOptionFloats>("machine_max_jerk_y");
        if (machine_max_jerk_y != nullptr)
            m_time_processor.machine_limits.machine_max_jerk_y.values = machine_max_jerk_y->values;

        const ConfigOptionFloats* machine_max_jerk_z = config.option<ConfigOptionFloats>("machine_max_jerk_z");
        if (machine_max_jerk_z != nullptr)
            m_time_processor.machine_limits.machine_max_jerk_z.values = machine_max_jerk_z->values;

        const ConfigOptionFloats* machine_max_jerk_e = config.option<ConfigOptionFloats>("machine_max_jerk_e");
        if (machine_max_jerk_e != nullptr)
            m_time_processor.machine_limits.machine_max_jerk_e.values = machine_max_jerk_e->values;

          const ConfigOptionFloats* machine_max_junction_deviation = config.option<ConfigOptionFloats>("machine_max_junction_deviation");
        if (machine_max_junction_deviation != nullptr)
              m_time_processor.machine_limits.machine_max_junction_deviation.values = machine_max_junction_deviation->values;

        const ConfigOptionFloats* machine_max_acceleration_extruding = config.option<ConfigOptionFloats>("machine_max_acceleration_extruding");
        if (machine_max_acceleration_extruding != nullptr)
            m_time_processor.machine_limits.machine_max_acceleration_extruding.values = machine_max_acceleration_extruding->values;

        const ConfigOptionFloats* machine_max_acceleration_retracting = config.option<ConfigOptionFloats>("machine_max_acceleration_retracting");
        if (machine_max_acceleration_retracting != nullptr)
            m_time_processor.machine_limits.machine_max_acceleration_retracting.values = machine_max_acceleration_retracting->values;


        const ConfigOptionFloats* machine_max_acceleration_travel = config.option<ConfigOptionFloats>(m_flavor == gcfMarlinLegacy || m_flavor == gcfKlipper
                                                                                                    ? "machine_max_acceleration_extruding"
                                                                                                    : "machine_max_acceleration_travel");
        if (machine_max_acceleration_travel != nullptr)
            m_time_processor.machine_limits.machine_max_acceleration_travel.values = machine_max_acceleration_travel->values;


        const ConfigOptionFloats* machine_min_extruding_rate = config.option<ConfigOptionFloats>("machine_min_extruding_rate");
        if (machine_min_extruding_rate != nullptr)
            m_time_processor.machine_limits.machine_min_extruding_rate.values = machine_min_extruding_rate->values;

        const ConfigOptionFloats* machine_min_travel_rate = config.option<ConfigOptionFloats>("machine_min_travel_rate");
        if (machine_min_travel_rate != nullptr)
            m_time_processor.machine_limits.machine_min_travel_rate.values = machine_min_travel_rate->values;
    }

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        float max_acceleration = get_option_value(m_time_processor.machine_limits.machine_max_acceleration_extruding, i);
        m_time_processor.machines[i].max_acceleration = max_acceleration;
        m_time_processor.machines[i].acceleration     = (max_acceleration > 0.0f) ? max_acceleration : DEFAULT_ACCELERATION;
        float max_retract_acceleration = get_option_value(m_time_processor.machine_limits.machine_max_acceleration_retracting, i);
        m_time_processor.machines[i].max_retract_acceleration = max_retract_acceleration;
        m_time_processor.machines[i].retract_acceleration     = (max_retract_acceleration > 0.0f) ? max_retract_acceleration :
                                                                                                    DEFAULT_RETRACT_ACCELERATION;
        float max_travel_acceleration = get_option_value(m_time_processor.machine_limits.machine_max_acceleration_travel, i);
        m_time_processor.machines[i].max_travel_acceleration = max_travel_acceleration;
        m_time_processor.machines[i].travel_acceleration     = (max_travel_acceleration > 0.0f) ? max_travel_acceleration :
                                                                                                  DEFAULT_TRAVEL_ACCELERATION;
    }

    if (m_flavor == gcfMarlinLegacy || m_flavor == gcfMarlinFirmware) {
        const ConfigOptionBool* silent_mode = config.option<ConfigOptionBool>("silent_mode");
        if (silent_mode != nullptr) {
            if (silent_mode->value && m_time_processor.machine_limits.machine_max_acceleration_x.values.size() > 1)
                enable_stealth_time_estimator(true);
        }
    }

    const ConfigOptionFloat* initial_layer_print_height = config.option<ConfigOptionFloat>("initial_layer_print_height");
    if (initial_layer_print_height != nullptr)
        m_first_layer_height = std::abs(initial_layer_print_height->value);

    const ConfigOptionFloat* printable_height = config.option<ConfigOptionFloat>("printable_height");
    if (printable_height != nullptr)
        m_result.printable_height = printable_height->value;

    const ConfigOptionBool* spiral_vase = config.option<ConfigOptionBool>("spiral_mode");
    if (spiral_vase != nullptr) {
        m_detect_layer_based_on_tag = spiral_vase->value;
        m_result.spiral_vase_mode = spiral_vase->value;
    }

    const ConfigOptionBool* has_scarf_joint_seam = config.option<ConfigOptionBool>("has_scarf_joint_seam");
    if (has_scarf_joint_seam != nullptr)
        m_detect_layer_based_on_tag = m_detect_layer_based_on_tag || has_scarf_joint_seam->value;

    const ConfigOptionEnumGeneric *bed_type = config.option<ConfigOptionEnumGeneric>("curr_bed_type");
    if (bed_type != nullptr)
        m_result.bed_type = (BedType)bed_type->value;


    const ConfigOptionFloat* z_offset = config.option<ConfigOptionFloat>("z_offset");
    if (z_offset != nullptr)
        m_z_offset = z_offset->value;

    if (const auto *layout = config.option<ConfigOptionEnum<RotaryLayout>>("nonplanar_kinematics");
        layout != nullptr && layout->value != RotaryLayout::None) {
        PrintConfig print_config;
        print_config.apply(config, true);
        m_tool_kinematics = make_tool_kinematics(print_config);
        m_tool_kinematics->inverse({ Vec3d::Zero(), Vec3d::UnitZ() }, nullptr, m_joints);
    }

    ensure_nozzle_group_result(static_cast<int>(m_result.filaments_count));
}

void GCodeProcessor::enable_stealth_time_estimator(bool enabled)
{
    m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].enabled = enabled;
}

void GCodeProcessor::reset()
{
    m_tool_kinematics.reset();
    m_joints        = JointPose();
    m_rotary_position_known_axes = 0;
    m_inverse_time  = false;
    m_joint_delta = JointPose();
    m_joint_feed = 0.;
    m_rotary_acceleration = {};
    m_rotary_jerk = {};
    m_native_road_id = UINT32_MAX;
    m_wave_object = -1;
    m_nonplanar_patch_shell = 0xFF;
    m_nonplanar_patch = -1;
    m_nonplanar_band = -1;
    m_nonplanar_feed_interval = 0.f;
    m_klipper_min_cruise_ratio = 0.5f;
    m_units = EUnits::Millimeters;
    m_global_positioning_type = EPositioningType::Absolute;
    m_e_local_positioning_type = EPositioningType::Absolute;
    m_extruder_offsets = std::vector<Vec3f>(MIN_EXTRUDERS_COUNT, Vec3f::Zero());
    m_flavor = gcfRepRapSprinter;
    m_nozzle_volume = std::vector<float>(MAXIMUM_EXTRUDER_NUMBER, 0.f);

    m_start_position = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_end_position = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_origin = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_cached_position.reset();
    m_wiping = false;
    m_flushing = false;
    m_virtual_flushing = false;
    m_wipe_tower = false;
    m_skippable = false;
    m_skippable_type = SkipType::stNone;
    m_skippable_blocks.clear();
    m_filament_blocks.clear();
    m_extruder_blocks.clear();
    m_machine_start_gcode_end_line_id = (unsigned int) (-1);
    m_machine_end_gcode_start_line_id = (unsigned int) (-1);
    m_skip_end_gcode_delays = false;
    m_remaining_volume = std::vector<float>(MAXIMUM_EXTRUDER_NUMBER, 0.f);

    m_line_id = 0;
    m_last_line_id = 0;
    m_feedrate = 0.0f;
    m_width = 0.0f;
    m_height = 0.0f;
    m_forced_width = 0.0f;
    m_forced_height = 0.0f;
    m_mm3_per_mm = 0.0f;
    m_travel_dist = 0.0f;
    m_fan_speed = 0.0f;
    m_pressure_advance = 0.0f;
    m_z_offset = 0.0f;

    m_extrusion_role = erNone;

    m_filament_id = std::vector<unsigned char>(MAXIMUM_EXTRUDER_NUMBER, static_cast<unsigned char>(-1));
    m_last_filament_id = std::vector<unsigned char>(MAXIMUM_EXTRUDER_NUMBER, static_cast<unsigned char>(-1));
    m_extruder_id = static_cast<unsigned char>(-1);
    m_nozzle_status_recorder = MultiNozzleUtils::NozzleStatusRecorder{};
    m_nozzle_group_result.reset();
    m_machine_config_idx = 0;
    m_extruder_colors.resize(MIN_EXTRUDERS_COUNT);
    for (size_t i = 0; i < MIN_EXTRUDERS_COUNT; ++i) {
        m_extruder_colors[i] = static_cast<unsigned char>(i);
    }
    m_extruder_temps.resize(MIN_EXTRUDERS_COUNT);
    for (size_t i = 0; i < MIN_EXTRUDERS_COUNT; ++i) {
        m_extruder_temps[i] = 0.0f;
    }

    m_physical_extruder_map.clear();
    m_extruder_max_nozzle_count = { 1 };

    m_filament_types.clear();
    m_nozzle_diameter.clear();
    m_hotend_cooling_rate = m_hotend_heating_rate = { 2.f };
    m_filament_pre_cooling_temp = { 0 };
    m_filament_preheat_temperature_delta.clear();
    m_enable_pre_heating = false;
    m_handle_hotend_as_extruder = false;
    m_has_filament_switcher = false;

    m_highest_bed_temp = 0;

    m_extruded_last_z = 0.0f;
    m_zero_layer_height = 0.0f;
    m_first_layer_height = 0.0f;
    m_processing_start_custom_gcode = false;
    m_in_config_block = false;
    m_g1_line_id = 0;
    m_layer_id = 0;
    m_cp_color.reset();
    m_mass_locator = nullptr;

    m_producer = EProducer::Unknown;

    m_time_processor.reset();
    m_used_filaments.reset();

    m_result.reset();
    m_result.id = ++s_result_id;
    m_move_state_ids = { { m_result.move_states.front(), 0 } };
    m_recent_move_states = { 0, 0 };

    m_last_default_color_id = 0;

    m_options_z_corrector.reset();

    m_detect_layer_based_on_tag = false;

    m_seams_count = 0;
    m_preheat_time = 0.f;
    m_preheat_steps = 1;
}

static inline const char* skip_whitespaces(const char *begin, const char *end) {
    for (; begin != end && (*begin == ' ' || *begin == '\t'); ++ begin);
    return begin;
}

static inline const char* remove_eols(const char *begin, const char *end) {
    for (; begin != end && (*(end - 1) == '\r' || *(end - 1) == '\n'); -- end);
    return end;
}

DynamicConfig GCodeProcessor::export_config_for_render() const
{
    DynamicConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings(m_parser.get_config().filament_colour.values));
    config.set_key_value("filament_is_support", new ConfigOptionBools(m_parser.get_config().filament_is_support.values));
    config.set_key_value("filament_type", new ConfigOptionStrings(m_parser.get_config().filament_type.values));
    config.set_key_value("filament_map", new ConfigOptionInts(m_parser.get_config().filament_map.values));
    for (const std::string &key : m_belt_render_config.keys())
        config.set_key_value(key, m_belt_render_config.option(key)->clone());
    return config;
}

void GCodeProcessor::process_file(const std::string& filename, std::function<void()> cancel_callback)
{
    CNumericLocalesSetter locales_setter;

    {
        m_parser.parse_file_raw(filename, [this](GCodeReader& reader, const char *begin, const char *end) {
            begin = skip_whitespaces(begin, end);
            if (begin != end && *begin == ';') {
                begin = skip_whitespaces(++ begin, end);
                end   = remove_eols(begin, end);
                if (begin != end) {
                    if (m_producer == EProducer::Unknown) {
                        if (detect_producer(std::string_view(begin, end - begin))) {
                            m_parser.quit_parsing();
                        }
                    } else if (std::string(begin, end).find("CONFIG_BLOCK_END") != std::string::npos) {
                        m_parser.quit_parsing();
                    }
                }
            }
        });
        m_parser.reset();

        if (m_producer == EProducer::OrcaSlicer || m_producer == EProducer::Slic3rPE || m_producer == EProducer::Slic3r) {
            DynamicPrintConfig config;
            config.apply(FullPrintConfig::defaults());
            config.load_from_gcode_file(filename, ForwardCompatibilitySubstitutionRule::EnableSilent);

            auto printer_model_opt = config.opt<ConfigOptionString>("printer_model");
            if (printer_model_opt && !printer_model_opt->value.empty()) {
                GCodeProcessor::s_IsBBLPrinter = boost::starts_with(printer_model_opt->value, "Bambu Lab");
            }

            ConfigOptionStrings *filament_color = config.opt<ConfigOptionStrings>("filament_colour");
            ConfigOptionInts    *filament_map   = config.opt<ConfigOptionInts>("filament_map", true);
            if (filament_color && filament_color->size() != filament_map->size()) {
                filament_map->values.resize(filament_color->size(), 1);
            }

            apply_config(config);
        }
        else if (m_producer == EProducer::Simplify3D)
            apply_config_simplify3d(filename);
        else if (m_producer == EProducer::SuperSlicer)
            apply_config_superslicer(filename);
    }

    m_result.filename = filename;
    m_result.id = ++s_result_id;
    initialize_result_moves();
    size_t parse_line_callback_cntr = 10000;
    m_parser.parse_file(filename, [this, cancel_callback, &parse_line_callback_cntr](GCodeReader& reader, const GCodeReader::GCodeLine& line) {
        if (-- parse_line_callback_cntr == 0) {
            parse_line_callback_cntr = 10000;
            if (cancel_callback)
                cancel_callback();
        }
        this->process_gcode_line(line, true);
    }, m_result.lines_ends);

    this->finalize(false);
}

void GCodeProcessor::initialize(const std::string& filename)
{
    assert(is_decimal_separator_point());

    m_result.filename = filename;
    m_result.id = ++s_result_id;
}

void GCodeProcessor::process_buffer(const std::string &buffer)
{
    m_parser.parse_buffer(buffer, [this](GCodeReader&, const GCodeReader::GCodeLine& line) {
        this->process_gcode_line(line, false);
    });
}

void GCodeProcessor::finalize(bool post_process)
{
    m_result.z_offset = m_z_offset;
    finalize_object_masses();

    for (GCodeProcessorResult::MoveVertex& move : m_result.moves) {
        if (move.type == EMoveType::Wipe) {
            move.width = Wipe_Width;
            move.height = Wipe_Height;
        }
    }

    calculate_time(m_result, 0, 0.0f, EMoveType::Noop, true);

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        TimeMachine& machine = m_time_processor.machines[i];
        TimeMachine::CustomGCodeTime& gcode_time = machine.gcode_time;
        if (gcode_time.needed && gcode_time.cache != 0.0f)
            gcode_time.times.push_back({ CustomGCode::ColorChange, gcode_time.cache });
    }

    m_used_filaments.process_caches(this);

    update_estimated_times_stats();

    m_result.initial_layer_time = get_first_layer_time(PrintEstimatedStatistics::ETimeMode::Normal);

    if (post_process){
        run_post_process();
        if (m_enable_pre_heating)
            run_second_pass_injection();
    }
    update_slice_warnings();
}

float GCodeProcessor::get_time(PrintEstimatedStatistics::ETimeMode mode) const
{
    return (mode < PrintEstimatedStatistics::ETimeMode::Count) ? float(m_time_processor.machines[static_cast<size_t>(mode)].time) : 0.0f;
}

float GCodeProcessor::get_prepare_time(PrintEstimatedStatistics::ETimeMode mode) const
{
    return (mode < PrintEstimatedStatistics::ETimeMode::Count) ? m_time_processor.machines[static_cast<size_t>(mode)].prepare_time : 0.0f;
}

std::string GCodeProcessor::get_time_dhm(PrintEstimatedStatistics::ETimeMode mode) const
{
    return (mode < PrintEstimatedStatistics::ETimeMode::Count) ? short_time(get_time_dhms(float(m_time_processor.machines[static_cast<size_t>(mode)].time))) : std::string("N/A");
}

std::vector<std::pair<CustomGCode::Type, std::pair<float, float>>> GCodeProcessor::get_custom_gcode_times(PrintEstimatedStatistics::ETimeMode mode, bool include_remaining) const
{
    std::vector<std::pair<CustomGCode::Type, std::pair<float, float>>> ret;
    if (mode < PrintEstimatedStatistics::ETimeMode::Count) {
        const TimeMachine& machine = m_time_processor.machines[static_cast<size_t>(mode)];
        float total_time = 0.0f;
        for (const auto& [type, time] : machine.gcode_time.times) {
            float remaining = include_remaining ? machine.time - total_time : 0.0f;
            ret.push_back({ type, { time, remaining } });
            total_time += time;
        }
    }
    return ret;
}

ConfigSubstitutions load_from_superslicer_gcode_file(const std::string& filename, DynamicPrintConfig& config, ForwardCompatibilitySubstitutionRule compatibility_rule)
{

    boost::nowide::ifstream ifs(filename);

    auto                      header_end_pos = ifs.tellg();
    ConfigSubstitutionContext substitutions_ctxt(compatibility_rule);
    size_t                    key_value_pairs = 0;

    ifs.seekg(0, ifs.end);
    auto file_length = ifs.tellg();
    auto data_length = std::min<std::fstream::pos_type>(65535, file_length - header_end_pos);
    ifs.seekg(file_length - data_length, ifs.beg);
    std::vector<char> data(size_t(data_length) + 1, 0);
    ifs.read(data.data(), data_length);
    ifs.close();
    key_value_pairs = ConfigBase::load_from_gcode_string_legacy(config, data.data(), substitutions_ctxt);

    if (key_value_pairs < 80)
        throw Slic3r::RuntimeError(format("Suspiciously low number of configuration values extracted from %1%: %2%", filename, key_value_pairs));

    return std::move(substitutions_ctxt.substitutions);
}

void GCodeProcessor::apply_config_superslicer(const std::string& filename)
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    load_from_superslicer_gcode_file(filename, config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    apply_config(config);
}

float GCodeProcessor::get_first_layer_time(PrintEstimatedStatistics::ETimeMode mode) const
{
    return (mode < PrintEstimatedStatistics::ETimeMode::Count) ? m_time_processor.machines[static_cast<size_t>(mode)].first_layer_time : 0.0f;
}

void GCodeProcessor::apply_config_simplify3d(const std::string& filename)
{
    struct BedSize
    {
        double x{ 0.0 };
        double y{ 0.0 };

        bool is_defined() const { return x > 0.0 && y > 0.0; }
    };

    BedSize bed_size;
    bool    producer_detected = false;

    m_parser.parse_file_raw(filename, [this, &bed_size, &producer_detected](GCodeReader& reader, const char* begin, const char* end) {

        auto extract_double = [](const std::string_view cmt, const std::string& key, double& out) {
            size_t pos = cmt.find(key);
            if (pos != cmt.npos) {
                pos = cmt.find(',', pos);
                if (pos != cmt.npos) {
                    out = string_to_double_decimal_point(cmt.substr(pos+1));
                    return true;
                }
            }
            return false;
        };

        auto extract_floats = [](const std::string_view cmt, const std::string& key, std::vector<float>& out) {
            size_t pos = cmt.find(key);
            if (pos != cmt.npos) {
                pos = cmt.find(',', pos);
                if (pos != cmt.npos) {
                    const std::string_view data_str = cmt.substr(pos + 1);
                    std::vector<std::string> values_str;
                    boost::split(values_str, data_str, boost::is_any_of("|,"), boost::token_compress_on);
                    for (const std::string& s : values_str) {
                        out.emplace_back(static_cast<float>(string_to_double_decimal_point(s)));
                    }
                    return true;
                }
            }
            return false;
        };

        begin = skip_whitespaces(begin, end);
        end   = remove_eols(begin, end);
        if (begin != end) {
            if (*begin == ';') {
                begin = skip_whitespaces(++ begin, end);
                if (begin != end) {
                    std::string_view comment(begin, end - begin);
                    if (producer_detected) {
                        if (bed_size.x == 0.0 && comment.find("strokeXoverride") != comment.npos)
                            extract_double(comment, "strokeXoverride", bed_size.x);
                        else if (bed_size.y == 0.0 && comment.find("strokeYoverride") != comment.npos)
                            extract_double(comment, "strokeYoverride", bed_size.y);
                        else if (comment.find("filamentDiameters") != comment.npos) {
                            m_result.filament_diameters.clear();
                            extract_floats(comment, "filamentDiameters", m_result.filament_diameters);
                        } else if (comment.find("filamentDensities") != comment.npos) {
                            m_result.filament_densities.clear();
                            extract_floats(comment, "filamentDensities", m_result.filament_densities);
                        } else if (comment.find("extruderDiameter") != comment.npos) {
                            std::vector<float> extruder_diameters;
                            extract_floats(comment, "extruderDiameter", extruder_diameters);
                            m_result.filaments_count = extruder_diameters.size();
                        }
                    } else if (boost::starts_with(comment, "G-Code generated by Simplify3D(R)"))
                        producer_detected = true;
                }
            } else {
                reader.quit_parsing();
            }
        }
    });

    if (m_result.filaments_count == 0)
        m_result.filaments_count = std::max<size_t>(1, std::min(m_result.filament_diameters.size(), m_result.filament_densities.size()));

    if (bed_size.is_defined()) {
        m_result.printable_area = {
            { 0.0, 0.0 },
            { bed_size.x, 0.0 },
            { bed_size.x, bed_size.y },
            { 0.0, bed_size.y }
        };
    }
}

void GCodeProcessor::process_gcode_line(const GCodeReader::GCodeLine& line, bool producers_enabled)
{

    ++m_line_id;

    m_start_position = m_end_position;

    const std::string_view cmd = line.cmd();
    if (m_flavor == gcfKlipper)
    {
        if (ascii_iequals(cmd, "SET_VELOCITY_LIMIT"))
        {
            process_SET_VELOCITY_LIMIT(line);
            return;
        }
        if (ascii_iequals(cmd, "SET_PRESSURE_ADVANCE"))
        {
            process_SET_PRESSURE_ADVANCE(line);
            return;
        }
    }

    if (cmd.length() > 1) {
        m_command_processor.process_comand(cmd, line);
    }
    else {
        const std::string &comment = line.raw();
        if (comment.length() > 2 && comment.front() == ';')
        {
            std::string comment_content = comment.substr(1);
            if (comment_content[0] == 'V' || comment_content[0] == 'v') {
                GCodeReader reader;
                GCodeReader::GCodeLine new_line;
                reader.parse_line(comment_content, [&new_line](const auto& greader, const auto& gline) {
                    new_line = gline;
                    });
                m_command_processor.process_comand(new_line.cmd(), new_line);
            }
            else {
                process_tags(comment_content, producers_enabled);
            }
        }
    }
}

int GCodeProcessor::get_gcode_last_filament(const std::string& gcode_str)
{
    int str_size = gcode_str.size();
    int start_index = 0;
    int end_index = 0;
    int out_filament = -1;
    while (end_index < str_size) {
        if (gcode_str[end_index] != '\n') {
            end_index++;
            continue;
        }

        if (end_index > start_index) {
            std::string line_str = gcode_str.substr(start_index, end_index - start_index);
            line_str.erase(0, line_str.find_first_not_of(" "));
            line_str.erase(line_str.find_last_not_of(" ") + 1);
            if (line_str.empty() || line_str[0] != 'T') {
                start_index = end_index + 1;
                end_index = start_index;
                continue;
            }

            int out = -1;
            if (parse_number(line_str.substr(1), out) && out >= 0 && out < 255)
                out_filament = out;
        }

        start_index = end_index + 1;
        end_index = start_index;
    }

    return out_filament;
}

bool GCodeProcessor::get_last_z_from_gcode(const std::string& gcode_str, double& z)
{
    int str_size = gcode_str.size();
    int start_index = 0;
    int end_index = 0;
    bool is_z_changed = false;
    while (end_index < str_size) {
        if (gcode_str[end_index] != '\n') {
            end_index++;
            continue;
        }
        if (end_index > start_index) {
            std::string line_str = gcode_str.substr(start_index, end_index - start_index);
            line_str.erase(0, line_str.find_first_not_of(" "));
            line_str.erase(line_str.find_last_not_of(";") + 1);
            line_str.erase(line_str.find_last_not_of(" ") + 1);

            if (line_str.size() > 4 && (line_str.find("G0 ") == 0
                                       || line_str.find("G1 ") == 0
                                       || line_str.find("G2 ") == 0
                                       || line_str.find("G3 ") == 0))
            {
                auto z_pos = line_str.find(" Z");
                double temp_z = 0;
                if (z_pos != line_str.npos
                    && z_pos + 2 < line_str.size()) {
                    std::string z_sub = line_str.substr(z_pos + 2);
                    char* c = &z_sub[0];
                    char* end = c + sizeof(z_sub.c_str());

                    auto is_end_of_word = [](char c) {
                        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0 || c == ';';
                    };

                    auto [pend, ec] = fast_float::from_chars(c, end, temp_z);
                    if (pend != c && is_end_of_word(*pend)) {
                        z = temp_z;
                        is_z_changed = true;
                    }
                }
            }
        }
        start_index = end_index + 1;
        end_index = start_index;
    }
    return is_z_changed;
}

bool GCodeProcessor::get_last_position_from_gcode(const std::string &gcode_str, Vec3f &pos)
{
    int  str_size     = gcode_str.size();
    int  start_index  = 0;
    int  end_index    = 0;
    bool is_z_changed = false;
    while (end_index < str_size) {
        if (gcode_str[end_index] != '\n') {
            end_index++;
            continue;
        }
        if (end_index > start_index) {
            std::string line_str = gcode_str.substr(start_index, end_index - start_index);
            line_str.erase(0, line_str.find_first_not_of(" "));
            line_str.erase(line_str.find_last_not_of(";") + 1);
            line_str.erase(line_str.find_last_not_of(" ") + 1);

            if (line_str.size() > 5 && (line_str.find("G0 ") == 0 || line_str.find("G1 ") == 0 || line_str.find("G2 ") == 0 || line_str.find("G3 ") == 0)) {
                {
                    float &x      = pos.x();
                    auto   z_pos  = line_str.find(" X");
                    float  temp_z = 0;
                    if (z_pos != line_str.npos && z_pos + 2 < line_str.size()) {
                        std::string z_sub = line_str.substr(z_pos + 2);
                        char       *c     = &z_sub[0];
                        char       *end   = c + sizeof(z_sub.c_str());

                        auto is_end_of_word = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0 || c == ';'; };

                        auto [pend, ec] = fast_float::from_chars(c, end, temp_z);
                        if (pend != c && is_end_of_word(*pend)) {
                            x            = temp_z;
                            is_z_changed = true;
                        }
                    }
                }

                {
                    float &y      = pos.y();
                    auto   z_pos  = line_str.find(" Y");
                    float  temp_z = 0;
                    if (z_pos != line_str.npos && z_pos + 2 < line_str.size()) {
                        std::string z_sub = line_str.substr(z_pos + 2);
                        char       *c     = &z_sub[0];
                        char       *end   = c + sizeof(z_sub.c_str());

                        auto is_end_of_word = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0 || c == ';'; };

                        auto [pend, ec] = fast_float::from_chars(c, end, temp_z);
                        if (pend != c && is_end_of_word(*pend)) {
                            y            = temp_z;
                            is_z_changed = true;
                        }
                    }
                }

                {
                    float &z      = pos.z();
                    auto   z_pos  = line_str.find(" Z");
                    float  temp_z = 0;
                    if (z_pos != line_str.npos && z_pos + 2 < line_str.size()) {
                        std::string z_sub = line_str.substr(z_pos + 2);
                        char       *c     = &z_sub[0];
                        char       *end   = c + sizeof(z_sub.c_str());

                        auto is_end_of_word = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0 || c == ';'; };

                        auto [pend, ec] = fast_float::from_chars(c, end, temp_z);
                        if (pend != c && is_end_of_word(*pend)) {
                            z            = temp_z;
                            is_z_changed = true;
                        }
                    }
                }
            }
        }
        start_index = end_index + 1;
        end_index   = start_index;
    }
    return is_z_changed;
}

void GCodeProcessor::process_tags(const std::string_view comment, bool producers_enabled)
{
    if (producers_enabled && process_producers_tags(comment))
        return;

    constexpr std::string_view road_tag = "_NATIVE_ROAD_ID=";
    if (boost::starts_with(comment, road_tag)) {
        const auto value = comment.substr(road_tag.size());
        uint32_t id = UINT32_MAX;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
        m_native_road_id = parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() ? id : UINT32_MAX;
        return;
    }
    if (boost::starts_with(comment, reserved_tag(ETags::Role))) {
        set_extrusion_role(ExtrusionEntity::string_to_role(comment.substr(reserved_tag(ETags::Role).length())));
        if (m_extrusion_role == erExternalPerimeter)
            m_seams_detector.activate(true);
        m_processing_start_custom_gcode = (m_extrusion_role == erCustom && m_g1_line_id == 0);
        return;
    }

    if (boost::starts_with(comment, "_NONPLANAR_BEGIN")) {
        auto value = [comment](std::string_view key, int missing) {
            const size_t pos = comment.find(key);
            return pos == comment.npos ? missing : std::atoi(std::string(comment.substr(pos + key.size())).c_str());
        };
        m_result.nonplanar = true;
        m_wave_object = value("wave=", -1);
        m_nonplanar_patch_shell = static_cast<unsigned char>(std::clamp(value("shell=", 0), 0, 0xFE));
        m_nonplanar_patch       = static_cast<short>(std::clamp(value("patch=", -1), -1, 0x7FFF));
        m_nonplanar_band        = static_cast<signed char>(std::clamp(value("band=", -1), -1, 0x7F));
        const size_t rate_pos = comment.find("rate=");
        const double rate     = rate_pos == comment.npos ? 0. : std::atof(std::string(comment.substr(rate_pos + 5)).c_str());
        m_nonplanar_feed_interval = rate > 0. && m_flavor != gcfKlipper ? float(1. / rate) : 0.f;
        return;
    }
    if (boost::starts_with(comment, "_NONPLANAR_END")) {
        m_wave_object = -1;
        m_nonplanar_patch_shell = 0xFF;
        m_nonplanar_patch       = -1;
        m_nonplanar_band        = -1;
        return;
    }

    if (boost::starts_with(comment, " start printing object")) {
        m_object_label_id = get_object_label_id(comment);
        return;
    }

    if (boost::starts_with(comment, " stop printing object")) {
        m_object_label_id = -1;
        return;
    }

    if (boost::starts_with(comment, " Z_HEIGHT:")) {
        m_print_z = get_z_height(comment);
        return;
    }

    if (m_belt_printer && boost::starts_with(comment, "Z:")) {
        m_print_z = get_z_height(comment);
        return;
    }

    if (boost::starts_with(comment, " CONFIG_BLOCK_START")) {
        m_in_config_block = true;
        return;
    }
    if (boost::starts_with(comment, " CONFIG_BLOCK_END")) {
        m_in_config_block = false;
        return;
    }

    if (!m_in_config_block && boost::starts_with(comment, " belt_slice_rotation_angle = ")) {
        try {
            m_result.belt_tilt_angle = std::abs(std::stof(std::string(comment.substr(29))));
        } catch (...) {}
        return;
    }
    if (boost::starts_with(comment, reserved_tag(ETags::Wipe_Start))) {
        m_wiping = true;
        return;
    }

    if (boost::starts_with(comment, reserved_tag(ETags::Wipe_End))) {
        m_wiping = false;
        return;
    }

    if (boost::starts_with(comment, reserved_tag(ETags::Wipe_Tower_Start))) {
        m_wipe_tower = true;
        return;
    }

    if (boost::starts_with(comment, reserved_tag(ETags::Wipe_Tower_End))) {
        m_wipe_tower = false;
        m_used_filaments.process_wipe_tower_cache(this);
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::Skippable_Start_Tag)) {
        m_skippable = true;
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::Skippable_End_Tag)) {
        m_skippable = false;
        m_skippable_type = SkipType::stNone;
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::Skippable_Type_Tag)) {
        std::string_view type = comment.substr(GCodeProcessor::Skippable_Type_Tag.length());
        set_skippable_type(type);
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::Flush_Start_Tag)) {
        m_flushing = true;
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::Flush_End_Tag)) {
        m_flushing = false;
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::VFlush_Start_Tag)) {
        m_virtual_flushing = true;
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::VFlush_End_Tag)) {
        m_virtual_flushing = false;
        return;
    }

    if (comment == Machine_End_GCode_Start_Tag) {
        m_skip_end_gcode_delays = true;
        return;
    }

    if (boost::starts_with(comment, GCodeProcessor::External_Purge_Tag)) {
        static const std::regex numberRegex(R"(\d+\.\d+)");
        std::smatch match;
        std::string line(comment);
        if (std::regex_search(line, match, numberRegex)) {
            int filament_id = get_filament_id();
            float filament_diameter = (static_cast<size_t>(filament_id) < m_result.filament_diameters.size()) ?  m_result.filament_diameters[filament_id] : m_result.filament_diameters.back();
            float filament_radius = 0.5f * filament_diameter;
            float area_filament_cross_section = static_cast<float>(M_PI) * sqr(filament_radius);

            float dE = std::stof(match.str());
            float volume_extruded_filament = area_filament_cross_section * dE;
            m_used_filaments.update_flush_per_filament(filament_id, volume_extruded_filament);
        }
        return;
    }

    if (!producers_enabled || m_producer == EProducer::OrcaSlicer) {
        if (boost::starts_with(comment, reserved_tag(ETags::Height))) {
            if (!parse_number(comment.substr(reserved_tag(ETags::Height).size()), m_forced_height))
                BOOST_LOG_TRIVIAL(error) << "GCodeProcessor encountered an invalid value for Height (" << comment << ").";
            return;
        }
        if (boost::starts_with(comment, reserved_tag(ETags::Width))) {
            if (!parse_number(comment.substr(reserved_tag(ETags::Width).size()), m_forced_width))
                BOOST_LOG_TRIVIAL(error) << "GCodeProcessor encountered an invalid value for Width (" << comment << ").";
            return;
        }
        if (m_manual_filament_change && boost::starts_with(comment, reserved_tag(ETags::Manual_Tool_Change))) {
            std::string_view tool_change_cmd = comment.substr(reserved_tag(ETags::Manual_Tool_Change).length());
            if (boost::starts_with(tool_change_cmd, "T")) {
                process_T(tool_change_cmd);
            }
        }
    }

    if (boost::starts_with(comment, reserved_tag(ETags::Color_Change))) {
        unsigned char filament_id = 0;
        static std::vector<std::string> Default_Colors = {
            "#0B2C7A",
            "#1C8891",
            "#AAF200",
            "#F5CE0A",
            "#D16830",
            "#942616",
        };

        std::string color = Default_Colors[0];
        auto is_valid_color = [](const std::string& color) {
            auto is_hex_digit = [](char c) {
                return ((c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'F') ||
                        (c >= 'a' && c <= 'f'));
            };

            if (color[0] != '#' || color.length() != 7)
                return false;
            for (int i = 1; i <= 6; ++i) {
                if (!is_hex_digit(color[i]))
                    return false;
            }
            return true;
        };

        std::vector<std::string> tokens;
        boost::split(tokens, comment, boost::is_any_of(","), boost::token_compress_on);
        if (tokens.size() > 1) {
            if (tokens[1][0] == 'T') {
                int eid;
                if (!parse_number(tokens[1].substr(1), eid) || eid < 0 || eid > 255) {
                    BOOST_LOG_TRIVIAL(error) << "GCodeProcessor encountered an invalid value for Color_Change (" << comment << ").";
                    return;
                }
                filament_id = static_cast<unsigned char>(eid);
            }
        }
        if (tokens.size() > 2) {
            if (is_valid_color(tokens[2]))
                color = tokens[2];
        }
        else {
            color = Default_Colors[m_last_default_color_id];
            ++m_last_default_color_id;
            if (m_last_default_color_id == Default_Colors.size())
                m_last_default_color_id = 0;
        }

        if (filament_id < m_extruder_colors.size())
            m_extruder_colors[filament_id] = static_cast<unsigned char>(m_extruder_offsets.size()) + m_cp_color.counter;
        ++m_cp_color.counter;
        if (m_cp_color.counter == UCHAR_MAX)
            m_cp_color.counter = 0;

        if (get_filament_id() == filament_id) {
            m_cp_color.current = m_extruder_colors[filament_id];
            store_move_vertex(EMoveType::Color_change);
            CustomGCode::Item item = { static_cast<double>(m_end_position[2]), CustomGCode::ColorChange, filament_id + 1, color, "" };
            m_result.custom_gcode_per_print_z.emplace_back(item);
            m_options_z_corrector.set();
            process_custom_gcode_time(CustomGCode::ColorChange);
            process_filaments(CustomGCode::ColorChange);
        }

        return;
    }

    if (comment == reserved_tag(ETags::Pause_Print)) {
        store_move_vertex(EMoveType::Pause_Print);
        CustomGCode::Item item = { static_cast<double>(m_end_position[2]), CustomGCode::PausePrint, get_filament_id() + 1, "", ""};
        m_result.custom_gcode_per_print_z.emplace_back(item);
        m_options_z_corrector.set();
        process_custom_gcode_time(CustomGCode::PausePrint);
        return;
    }

    if (comment == reserved_tag(ETags::Custom_Code)) {
        store_move_vertex(EMoveType::Custom_GCode);
        CustomGCode::Item item = { static_cast<double>(m_end_position[2]), CustomGCode::Custom, get_filament_id() + 1, "", ""};
        m_result.custom_gcode_per_print_z.emplace_back(item);
        m_options_z_corrector.set();
        return;
    }

    if (comment == reserved_tag(ETags::Layer_Change)) {
        ++m_layer_id;
        return;
    }
}

bool GCodeProcessor::process_producers_tags(const std::string_view comment)
{
    switch (m_producer)
    {
    case EProducer::Slic3rPE:
    case EProducer::Slic3r:
    case EProducer::SuperSlicer:
    case EProducer::OrcaSlicer: { return process_bambuslicer_tags(comment); }
    case EProducer::Cura:        { return process_cura_tags(comment); }
    case EProducer::Simplify3D:  { return process_simplify3d_tags(comment); }
    case EProducer::CraftWare:   { return process_craftware_tags(comment); }
    case EProducer::ideaMaker:   { return process_ideamaker_tags(comment); }
    case EProducer::KissSlicer:  { return process_kissslicer_tags(comment); }
    default:                     { return false; }
    }
}

bool GCodeProcessor::process_bambuslicer_tags(const std::string_view comment)
{
    return false;
}

bool GCodeProcessor::process_cura_tags(const std::string_view comment)
{
    std::string tag = "TYPE:";
    size_t pos = comment.find(tag);
    if (pos != comment.npos) {
        const std::string_view type = comment.substr(pos + tag.length());
        if (type == "SKIRT")
            set_extrusion_role(erSkirt);
        else if (type == "WALL-OUTER")
            set_extrusion_role(erExternalPerimeter);
        else if (type == "WALL-INNER")
            set_extrusion_role(erPerimeter);
        else if (type == "SKIN")
            set_extrusion_role(erSolidInfill);
        else if (type == "FILL")
            set_extrusion_role(erInternalInfill);
        else if (type == "SUPPORT")
            set_extrusion_role(erSupportMaterial);
        else if (type == "SUPPORT-INTERFACE")
            set_extrusion_role(erSupportMaterialInterface);
        else if (type == "PRIME-TOWER")
            set_extrusion_role(erWipeTower);
        else {
            set_extrusion_role(erNone);
            BOOST_LOG_TRIVIAL(warning) << "GCodeProcessor found unknown extrusion role: " << type;
        }

        if (m_extrusion_role == erExternalPerimeter)
            m_seams_detector.activate(true);

        return true;
    }

    tag = "FLAVOR:";
    pos = comment.find(tag);
    if (pos != comment.npos) {
        const std::string_view flavor = comment.substr(pos + tag.length());
        if (flavor == "BFB")
            m_flavor = gcfMarlinLegacy;
        else if (flavor == "Mach3")
            m_flavor = gcfMach3;
        else if (flavor == "Makerbot")
            m_flavor = gcfMakerWare;
        else if (flavor == "UltiGCode")
            m_flavor = gcfMarlinLegacy;
        else if (flavor == "Marlin(Volumetric)")
            m_flavor = gcfMarlinLegacy;
        else if (flavor == "Griffin")
            m_flavor = gcfMarlinLegacy;
        else if (flavor == "Repetier")
            m_flavor = gcfRepetier;
        else if (flavor == "RepRap")
            m_flavor = gcfRepRapFirmware;
        else if (flavor == "Marlin")
            m_flavor = gcfMarlinLegacy;
        else
            BOOST_LOG_TRIVIAL(warning) << "GCodeProcessor found unknown flavor: " << flavor;

        return true;
    }

    tag = "LAYER:";
    pos = comment.find(tag);
    if (pos != comment.npos) {
        ++m_layer_id;
        return true;
    }

    return false;
}

bool GCodeProcessor::process_simplify3d_tags(const std::string_view comment)
{
    std::string_view cmt = comment;
    size_t pos = cmt.find(" feature");
    if (pos == 0)
        cmt.remove_prefix(8);

    pos = cmt.find(" skirt");
    if (pos == 0) {
        set_extrusion_role(erSkirt);
        return true;
    }

    pos = cmt.find(" outer perimeter");
    if (pos == 0) {
        set_extrusion_role(erExternalPerimeter);
        m_seams_detector.activate(true);
        return true;
    }

    pos = cmt.find(" inner perimeter");
    if (pos == 0) {
        set_extrusion_role(erPerimeter);
        return true;
    }

    pos = cmt.find(" gap fill");
    if (pos == 0) {
        set_extrusion_role(erGapFill);
        return true;
    }

    pos = cmt.find(" infill");
    if (pos == 0) {
        set_extrusion_role(erInternalInfill);
        return true;
    }

    pos = cmt.find(" solid layer");
    if (pos == 0) {
        set_extrusion_role(erSolidInfill);
        return true;
    }

    pos = cmt.find(" bridge");
    if (pos == 0) {
        set_extrusion_role(erBridgeInfill);
        return true;
    }

    pos = cmt.find(" internal bridge");
    if (pos == 0) {
        set_extrusion_role(erInternalBridgeInfill);
        return true;
    }

    pos = cmt.find(" support");
    if (pos == 0) {
        set_extrusion_role(erSupportMaterial);
        return true;
    }

    pos = cmt.find(" dense support");
    if (pos == 0) {
        set_extrusion_role(erSupportMaterialInterface);
        return true;
    }

    pos = cmt.find(" prime pillar");
    if (pos == 0) {
        set_extrusion_role(erWipeTower);
        return true;
    }

    pos = cmt.find(" ooze shield");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    pos = cmt.find(" raft");
    if (pos == 0) {
        set_extrusion_role(erSupportMaterial);
        return true;
    }

    pos = cmt.find(" internal single extrusion");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    std::string tag = " tool";
    pos = cmt.find(tag);
    if (pos == 0) {
        const std::string_view data = cmt.substr(pos + tag.length());
        std::string h_tag = "H";
        size_t h_start = data.find(h_tag);
        size_t h_end = data.find_first_of(' ', h_start);
        std::string w_tag = "W";
        size_t w_start = data.find(w_tag);
        size_t w_end = data.find_first_of(' ', w_start);
        if (h_start != data.npos) {
            if (!parse_number(data.substr(h_start + 1, (h_end != data.npos) ? h_end - h_start - 1 : h_end), m_forced_height))
                BOOST_LOG_TRIVIAL(error) << "GCodeProcessor encountered an invalid value for Height (" << comment << ").";
        }
        if (w_start != data.npos) {
            if (!parse_number(data.substr(w_start + 1, (w_end != data.npos) ? w_end - w_start - 1 : w_end), m_forced_width))
                BOOST_LOG_TRIVIAL(error) << "GCodeProcessor encountered an invalid value for Width (" << comment << ").";
        }

        return true;
    }

    tag = " layer";
    pos = cmt.find(tag);
    if (pos == 0) {
        const std::string_view data = cmt.substr(pos + tag.length());
        size_t end_start = data.find("end");
        if (end_start == data.npos)
            ++m_layer_id;

        return true;
    }

    return false;
}

bool GCodeProcessor::process_craftware_tags(const std::string_view comment)
{
    std::string tag = "segType:";
    size_t pos = comment.find(tag);
    if (pos != comment.npos) {
        const std::string_view type = comment.substr(pos + tag.length());
        if (type == "Skirt")
            set_extrusion_role(erSkirt);
        else if (type == "Perimeter")
            set_extrusion_role(erExternalPerimeter);
        else if (type == "HShell")
            set_extrusion_role(erNone);
        else if (type == "InnerHair")
            set_extrusion_role(erNone);
        else if (type == "Loop")
            set_extrusion_role(erNone);
        else if (type == "Infill")
            set_extrusion_role(erInternalInfill);
        else if (type == "Raft")
            set_extrusion_role(erSkirt);
        else if (type == "Support")
            set_extrusion_role(erSupportMaterial);
        else if (type == "SupportTouch")
            set_extrusion_role(erSupportMaterial);
        else if (type == "SoftSupport")
            set_extrusion_role(erSupportMaterialInterface);
        else if (type == "Pillar")
            set_extrusion_role(erWipeTower);
        else {
            set_extrusion_role(erNone);
            BOOST_LOG_TRIVIAL(warning) << "GCodeProcessor found unknown extrusion role: " << type;
        }

        if (m_extrusion_role == erExternalPerimeter)
            m_seams_detector.activate(true);

        return true;
    }

    pos = comment.find(" Layer #");
    if (pos == 0) {
        ++m_layer_id;
        return true;
    }

    return false;
}

bool GCodeProcessor::process_ideamaker_tags(const std::string_view comment)
{
    std::string tag = "TYPE:";
    size_t pos = comment.find(tag);
    if (pos != comment.npos) {
        const std::string_view type = comment.substr(pos + tag.length());
        if (type == "RAFT")
            set_extrusion_role(erSkirt);
        else if (type == "WALL-OUTER")
            set_extrusion_role(erExternalPerimeter);
        else if (type == "WALL-INNER")
            set_extrusion_role(erPerimeter);
        else if (type == "SOLID-FILL")
            set_extrusion_role(erSolidInfill);
        else if (type == "FILL")
            set_extrusion_role(erInternalInfill);
        else if (type == "BRIDGE")
            set_extrusion_role(erBridgeInfill);
        else if (type == "INTERNAL BRIDGE")
            set_extrusion_role(erInternalBridgeInfill);
        else if (type == "SUPPORT")
            set_extrusion_role(erSupportMaterial);
        else {
            set_extrusion_role(erNone);
            BOOST_LOG_TRIVIAL(warning) << "GCodeProcessor found unknown extrusion role: " << type;
        }

        if (m_extrusion_role == erExternalPerimeter)
            m_seams_detector.activate(true);

        return true;
    }

    tag = "WIDTH:";
    pos = comment.find(tag);
    if (pos != comment.npos) {
        if (!parse_number(comment.substr(pos + tag.length()), m_forced_width))
            BOOST_LOG_TRIVIAL(error) << "GCodeProcessor encountered an invalid value for Width (" << comment << ").";
        return true;
    }

    tag = "HEIGHT:";
    pos = comment.find(tag);
    if (pos != comment.npos) {
        if (!parse_number(comment.substr(pos + tag.length()), m_forced_height))
            BOOST_LOG_TRIVIAL(error) << "GCodeProcessor encountered an invalid value for Height (" << comment << ").";
        return true;
    }

    pos = comment.find("LAYER:");
    if (pos == 0) {
        ++m_layer_id;
        return true;
    }

    return false;
}

bool GCodeProcessor::process_kissslicer_tags(const std::string_view comment)
{
    size_t pos = comment.find(" 'Raft Path'");
    if (pos == 0) {
        set_extrusion_role(erSkirt);
        return true;
    }

    pos = comment.find(" 'Support Interface Path'");
    if (pos == 0) {
        set_extrusion_role(erSupportMaterialInterface);
        return true;
    }

    pos = comment.find(" 'Travel/Ironing Path'");
    if (pos == 0) {
        set_extrusion_role(erIroning);
        return true;
    }

    pos = comment.find(" 'Support (may Stack) Path'");
    if (pos == 0) {
        set_extrusion_role(erSupportMaterial);
        return true;
    }

    pos = comment.find(" 'Perimeter Path'");
    if (pos == 0) {
        set_extrusion_role(erExternalPerimeter);
        m_seams_detector.activate(true);
        return true;
    }

    pos = comment.find(" 'Pillar Path'");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    pos = comment.find(" 'Destring/Wipe/Jump Path'");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    pos = comment.find(" 'Prime Pillar Path'");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    pos = comment.find(" 'Loop Path'");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    pos = comment.find(" 'Crown Path'");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    pos = comment.find(" 'Solid Path'");
    if (pos == 0) {
        set_extrusion_role(erNone);
        return true;
    }

    pos = comment.find(" 'Stacked Sparse Infill Path'");
    if (pos == 0) {
        set_extrusion_role(erInternalInfill);
        return true;
    }

    pos = comment.find(" 'Sparse Infill Path'");
    if (pos == 0) {
        set_extrusion_role(erSolidInfill);
        return true;
    }

    pos = comment.find(" BEGIN_LAYER_");
    if (pos == 0) {
        ++m_layer_id;
        return true;
    }

    return false;
}

bool GCodeProcessor::detect_producer(const std::string_view comment)
{
    for (const auto& [id, search_string] : Producers) {
        size_t pos = comment.find(search_string);
        if (pos != comment.npos) {
            m_producer = id;
            return true;
        }
    }
    return false;
}

void GCodeProcessor::process_G0(const GCodeReader::GCodeLine& line)
{
    process_G1(line);
}

void GCodeProcessor::process_G1(const GCodeReader::GCodeLine& line, const std::optional<unsigned int>& remaining_internal_g1_lines)
{
    if (m_tool_kinematics) {
        process_rotary_G1(line);
        return;
    }
    std::array<std::optional<double>, 4> g1_axes = { std::nullopt, std::nullopt, std::nullopt, std::nullopt };
    if (line.has_x()) g1_axes[X] = (double)line.x();
    if (line.has_y()) g1_axes[Y] = (double)line.y();
    if (line.has_z()) g1_axes[Z] = (double)line.z();
    if (line.has_e()) g1_axes[E] = (double)line.e();
    std::optional<double> g1_feedrate = std::nullopt;
    if (line.has_f()) g1_feedrate = (double)line.f();
    process_G1(g1_axes, g1_feedrate);
}

void GCodeProcessor::process_rotary_G1(const GCodeReader::GCodeLine& line)
{
    const ToolKinematics &kin      = *m_tool_kinematics;
    const bool            relative = m_global_positioning_type == EPositioningType::Relative;
    const bool            from_unknown = m_rotary_position_known_axes != 7;
    if (!relative)
        for (int i = 0; i < 3; ++i)
            if (line.has(Axis(i))) m_rotary_position_known_axes |= 1 << i;
    const double          scale    = (m_units == EUnits::Inches) ? double(INCHES_TO_MM) : 1.0;
    JointPose             joints   = m_joints;
    for (int i = 0; i < 3; ++i)
        if (line.has(Axis(i)))
            joints.xyz[i] = (relative ? joints.xyz[i] : 0.) + scale * line.value(Axis(i));
    for (int i = 0; i < 2; ++i)
        if (float v; line.has_value(kin.rotary_axes()[i].letter, v))
            joints.rot[i] = (relative ? joints.rot[i] : 0.) + v;
    m_joint_delta.xyz = joints.xyz - m_joints.xyz;
    for (int i = 0; i < 2; ++i)
        m_joint_delta.rot[i] = joints.rot[i] - m_joints.rot[i];
    if (line.has_f())
        m_joint_feed = line.f();
    const Vec3d from = kin.forward(m_joints).tip;
    const Vec3d to   = kin.forward(joints).tip;
    m_joints         = joints;

    std::array<std::optional<double>, 4> g1_axes;
    for (int i = 0; i < 3; ++i)
        g1_axes[i] = (relative ? to[i] - from[i] : to[i] - m_origin[i]) / scale;
    if (line.has_e())
        g1_axes[E] = (double)line.e();
    std::optional<double> g1_feedrate;
    if (line.has_f()) {
        if (! m_inverse_time)
            g1_feedrate = (double)line.f();
        else if (const double length = (to - from).norm(); length > EPSILON)
            g1_feedrate = length * line.f();
    }
    const size_t previous_moves = m_result.moves.size();
    process_G1(g1_axes, g1_feedrate);
    if (m_joint_delta.rot[0] != 0. || m_joint_delta.rot[1] != 0.)
        m_result.nonplanar = true;
    for (size_t i = previous_moves; i < m_result.moves.size(); ++i) {
        m_result.moves[i].clearance_pose_known = m_rotary_position_known_axes == 7;
        m_result.moves[i].clearance_from_unknown = from_unknown;
    }
}

void GCodeProcessor::process_G1(const std::array<std::optional<double>, 4>& axes, const std::optional<double>& feedrate,
    G1DiscretizationOrigin origin, const std::optional<unsigned int>& remaining_internal_g1_lines)
{
    int filament_id = get_filament_id();
    int last_filament_id = get_last_filament_id();
    float filament_diameter = (static_cast<size_t>(filament_id) < m_result.filament_diameters.size()) ? m_result.filament_diameters[filament_id] : m_result.filament_diameters.back();
    float filament_radius = 0.5f * filament_diameter;
    float area_filament_cross_section = static_cast<float>(M_PI) * sqr(filament_radius);

    auto move_type = [this](const AxisCoords& delta_pos) {
        if (m_wiping)
            return EMoveType::Wipe;
        else if (delta_pos[E] < 0.0f)
            return (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f || delta_pos[Z] != 0.0f) ? EMoveType::Travel : EMoveType::Retract;
        else if (delta_pos[E] > 0.0f) {
            if (delta_pos[X] == 0.0f && delta_pos[Y] == 0.0f)
                return (delta_pos[Z] == 0.0f) ? EMoveType::Unretract : EMoveType::Travel;
            else if (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f)
                return EMoveType::Extrude;
        }
        else if (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f || delta_pos[Z] != 0.0f)
            return EMoveType::Travel;

        return EMoveType::Noop;
    };

    auto extract_absolute_position_on_axis = [&](Axis axis, std::optional<double> value, double area_filament_cross_section)
    {
        if (value.has_value()) {
            bool is_relative = (m_global_positioning_type == EPositioningType::Relative);
            if (axis == E)
                is_relative |= (m_e_local_positioning_type == EPositioningType::Relative);

            const double lengthsScaleFactor = (m_units == EUnits::Inches) ? double(INCHES_TO_MM) : 1.0;
            double       ret                = *value * lengthsScaleFactor;
            return is_relative ? m_start_position[axis] + ret : m_origin[axis] + ret;
        }
        else
            return m_start_position[axis];
    };

    ++m_g1_line_id;

    m_time_processor.machine_envelope_processing_enabled = true;

    for (unsigned char a = X; a <= E; ++a) {
        m_end_position[a] = extract_absolute_position_on_axis((Axis)a, axes[a], double(area_filament_cross_section));
    }

    if (feedrate.has_value())
        m_feedrate = (*feedrate) * MMMIN_TO_MMSEC;

    float max_abs_delta = 0.0f;
    AxisCoords delta_pos;
    for (unsigned char a = X; a <= E; ++a) {
        delta_pos[a] = m_end_position[a] - m_start_position[a];
        max_abs_delta = std::max<float>(max_abs_delta, std::abs(delta_pos[a]));
    }

    if (max_abs_delta == 0.0f && (!m_tool_kinematics ||
        (m_joint_delta.xyz.isZero() && m_joint_delta.rot[0] == 0. && m_joint_delta.rot[1] == 0.)))
        return;

    EMoveType type = move_type(delta_pos);
    if (type == EMoveType::Noop && m_tool_kinematics)
        type = EMoveType::Travel;
    const float delta_xyz = std::sqrt(sqr(delta_pos[X]) + sqr(delta_pos[Y]) + sqr(delta_pos[Z]));
    m_travel_dist = delta_xyz;

    if (type == EMoveType::Extrude) {
        float volume_extruded_filament = area_filament_cross_section * delta_pos[E];
        float area_toolpath_cross_section = volume_extruded_filament / delta_xyz;

        if(m_extrusion_role == ExtrusionRole::erSupportMaterial || m_extrusion_role == ExtrusionRole::erSupportMaterialInterface || m_extrusion_role ==ExtrusionRole::erSupportTransition)
            m_used_filaments.increase_support_caches(volume_extruded_filament);
        else if (m_extrusion_role==ExtrusionRole::erWipeTower) {
            m_used_filaments.increase_wipe_tower_caches(volume_extruded_filament);
        }
        else {
            m_used_filaments.increase_model_caches(volume_extruded_filament);
        }
        m_mm3_per_mm = area_toolpath_cross_section;

        if (m_forced_height > 0.0f)
            m_height = m_forced_height;
        else if (origin == G1DiscretizationOrigin::G1) {
            if (m_end_position[Z] > m_extruded_last_z + EPSILON)
                m_height = m_end_position[Z] - m_extruded_last_z;
        }

        if (m_height == 0.0f)
            m_height = DEFAULT_TOOLPATH_HEIGHT;

        if (m_end_position[Z] == 0.0f)
            m_end_position[Z] = m_height;

        if (origin == G1DiscretizationOrigin::G1)
            m_extruded_last_z = m_end_position[Z];
        if (const auto removed = m_options_z_corrector.update(m_height)) {
            for (TimeMachine &machine : m_time_processor.machines)
                for (TimeBlock &block : machine.blocks)
                    if (block.move_id > *removed)
                        --block.move_id;
        }

        if (m_forced_width > 0.0f)
            m_width = m_forced_width;
        else if (m_extrusion_role == erExternalPerimeter)
            m_width = delta_pos[E] * static_cast<float>(M_PI * sqr(1.05f * filament_radius)) / (delta_xyz * m_height);
        else if (m_extrusion_role == erBridgeInfill || m_extrusion_role == erInternalBridgeInfill || m_extrusion_role == erNone)
            m_width = static_cast<float>(m_result.filament_diameters[filament_id]) * std::sqrt(delta_pos[E] / delta_xyz);
        else
            m_width = delta_pos[E] * static_cast<float>(M_PI * sqr(filament_radius)) / (delta_xyz * m_height) + static_cast<float>(1.0 - 0.25 * M_PI) * m_height;

        if (m_width == 0.0f)
            m_width = DEFAULT_TOOLPATH_WIDTH;

        m_width = std::min(m_width, std::max(2.0f, 4.0f * m_height));
    }
    else if (type == EMoveType::Unretract && m_flushing) {
        int extruder_id = get_extruder_id();
        float volume_flushed_filament = area_filament_cross_section * delta_pos[E];
        if (m_remaining_volume[extruder_id] > volume_flushed_filament)
        {
            m_used_filaments.update_flush_per_filament(last_filament_id, volume_flushed_filament);
            m_remaining_volume[extruder_id] -= volume_flushed_filament;
        }
        else {
            m_used_filaments.update_flush_per_filament(last_filament_id, m_remaining_volume[extruder_id]);
            m_used_filaments.update_flush_per_filament(filament_id, volume_flushed_filament - m_remaining_volume[extruder_id]);
            m_remaining_volume[extruder_id] = 0.f;
        }
    }

    AxisCoords time_delta = delta_pos;
    if (m_tool_kinematics)
        for (int axis = 0; axis < 3; ++axis)
            time_delta[axis] = float(m_joint_delta.xyz[axis]);
    auto move_length = [](const AxisCoords& time_delta) {
        float sq_xyz_length = sqr(time_delta[X]) + sqr(time_delta[Y]) + sqr(time_delta[Z]);
        return (sq_xyz_length > 0.0f) ? std::sqrt(sq_xyz_length) : std::abs(time_delta[E]);
    };

    auto is_extrusion_only_move = [](const AxisCoords& time_delta) {
        return time_delta[X] == 0.0f && time_delta[Y] == 0.0f && time_delta[Z] == 0.0f && time_delta[E] != 0.0f;
    };

    float distance = move_length(time_delta);
    if (m_tool_kinematics && m_joint_delta.xyz.norm() < EPSILON &&
        (m_joint_delta.rot[0] != 0. || m_joint_delta.rot[1] != 0.))
        distance = float(std::hypot(m_joint_delta.rot[0], m_joint_delta.rot[1]));
    assert(distance != 0.0f);
    float inv_distance = 1.0f / distance;
    const bool bambu = m_print != nullptr && m_print->is_BBL_printer();

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        TimeMachine& machine = m_time_processor.machines[i];
        if (!machine.enabled)
            continue;

        TimeMachine::State& curr = machine.curr;
        TimeMachine::State& prev = machine.prev;
        std::vector<TimeBlock>& blocks = machine.blocks;

        curr.feedrate = (time_delta[E] == 0.0f) ?
            minimum_travel_feedrate(static_cast<PrintEstimatedStatistics::ETimeMode>(i), m_feedrate) :
            minimum_feedrate(static_cast<PrintEstimatedStatistics::ETimeMode>(i), m_feedrate);

        if (m_tool_kinematics)
            curr.feedrate = float(m_inverse_time ? distance * m_joint_feed / 60. : m_joint_feed / 60.);

        curr.enter_direction = { static_cast<float>(time_delta[X]), static_cast<float>(time_delta[Y]), static_cast<float>(time_delta[Z]) };
        float norm = curr.enter_direction.norm();
        if (norm > 0.f)
            curr.enter_direction = curr.enter_direction / norm;
        curr.exit_direction = curr.enter_direction;
        curr.jd_unit_vec = Vec4f(static_cast<float>(time_delta[X]),
                                 static_cast<float>(time_delta[Y]),
                                 static_cast<float>(time_delta[Z]),
                                 static_cast<float>(time_delta[E])).normalized();

        TimeBlock block;
        block.move_type = type;
        block.skippable_type = m_skippable_type;
        block.role = (type != EMoveType::Travel || m_extrusion_role == erCustom) ? m_extrusion_role : erNone;
        block.distance = distance;
        block.g1_line_id = m_g1_line_id;
        block.move_id = static_cast<unsigned int>(m_result.moves.size());
        block.remaining_internal_g1_lines = remaining_internal_g1_lines.has_value() ? *remaining_internal_g1_lines : 0;
        block.layer_id = std::max<unsigned int>(1, m_layer_id);
        block.flags.prepare_stage = m_processing_start_custom_gcode;
        block.flags.nonplanar = m_nonplanar_patch_shell != 0xFF;
        block.feed_interval = m_nonplanar_feed_interval;
        block.flags.arc_continuation = origin == G1DiscretizationOrigin::G2G3 && !m_arc_first_block;
        if (origin == G1DiscretizationOrigin::G2G3)
            m_arc_first_block = false;

        if ((!m_marlin_junctions || bambu) &&
            (prev.exit_direction(0) != 0.0f || prev.exit_direction(1) != 0.0f) &&
            (curr.enter_direction(0) != 0.0f || curr.enter_direction(1) != 0.0f) &&
            std::hypot(time_delta[X], time_delta[Y]) > 1e-4) {
            Vec3f v1 = prev.exit_direction;
            v1(2, 0) = 0.0f;
            v1.normalize();
            Vec3f v2 = curr.enter_direction;
            v2(2, 0) = 0.0f;
            v2.normalize();
            float norm_diff = (v2 - v1).norm();
            if (norm_diff < 0.5f && norm_diff > 0.00001f) {
                float dot = v1(0) * v2(0) + v1(1) * v2(1);
                float cross = v1(0) * v2(1) - v1(1) * v2(0);
                float angle = float(atan2(double(cross), double(dot)));
                float sin_theta_2 = sqrt((1.0f - cos(angle)) * 0.5f);
                float r = sqrt(sqr(time_delta[X]) + sqr(time_delta[Y])) * 0.5 / sin_theta_2;
                float acc = get_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i));
                curr.feedrate = std::min(curr.feedrate, sqrt(acc * r));
            }
        }

        float min_feedrate_factor = 1.0f;
        for (unsigned char a = X; a <= E; ++a) {
            curr.axis_feedrate[a] = curr.feedrate * time_delta[a] * inv_distance;
            if (a == E)
                curr.axis_feedrate[a] *= machine.extrude_factor_override_percentage;

            curr.abs_axis_feedrate[a] = std::abs(curr.axis_feedrate[a]);
            if (curr.abs_axis_feedrate[a] != 0.0f) {
                float axis_max_feedrate = get_axis_max_feedrate(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a), m_machine_config_idx);
                if (axis_max_feedrate != 0.0f) min_feedrate_factor = std::min<float>(min_feedrate_factor, axis_max_feedrate / curr.abs_axis_feedrate[a]);
            }
        }
        if (m_tool_kinematics)
            for (int axis = 0; axis < 2; ++axis) {
                const double rate = std::abs(curr.feedrate * m_joint_delta.rot[axis] * inv_distance);
                const double limit = m_tool_kinematics->rotary_axes()[axis].max_speed_deg_s;
                if (rate > 0. && limit > 0.)
                    min_feedrate_factor = std::min(min_feedrate_factor, float(limit / rate));
            }
        curr.feedrate *= min_feedrate_factor;
        block.feedrate_profile.cruise = curr.feedrate;

        if (min_feedrate_factor < 1.0f) {
            for (unsigned char a = X; a <= E; ++a) {
                curr.axis_feedrate[a] *= min_feedrate_factor;
                curr.abs_axis_feedrate[a] *= min_feedrate_factor;
            }
        }

        float acceleration =
            (type == EMoveType::Travel) ? get_travel_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)) :
            (is_extrusion_only_move(time_delta) ?
                get_retract_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)) :
                get_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)));

        for (unsigned char a = X; a <= E; ++a) {
            float axis_max_acceleration = get_axis_max_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a), m_machine_config_idx);
            if (acceleration * std::abs(time_delta[a]) * inv_distance > axis_max_acceleration)
                acceleration = axis_max_acceleration / (std::abs(time_delta[a]) * inv_distance);
        }

        if (m_tool_kinematics)
            for (int axis = 0; axis < 2; ++axis) {
                const float component = float(std::abs(m_joint_delta.rot[axis]) * inv_distance);
                if (component > 0.f && m_rotary_acceleration[axis] > 0.f)
                    acceleration = std::min(acceleration, m_rotary_acceleration[axis] / component);
                curr.rotary_feedrate[axis] = float(curr.feedrate * m_joint_delta.rot[axis] * inv_distance);
            }
        block.acceleration = acceleration;
        if (m_flavor == gcfKlipper)
            block.smooth_acceleration = std::min(acceleration, get_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)) *
                                                                   (1.0f - m_klipper_min_cruise_ratio));

        static const float PREVIOUS_FEEDRATE_THRESHOLD = 0.0001f;
        const bool has_prev_move = !blocks.empty() && prev.feedrate > PREVIOUS_FEEDRATE_THRESHOLD;

        const float vmax_junction_jd = calc_vmax_junction_deviation(block, prev, curr, has_prev_move,
                                                                    static_cast<PrintEstimatedStatistics::ETimeMode>(i),
                                                                    blocks.empty() ? nullptr : &blocks.back());
        const bool use_junction_deviation = vmax_junction_jd >= 0.0f;

        curr.safe_feedrate = use_junction_deviation ? 0.0f : block.feedrate_profile.cruise;

        if (!use_junction_deviation) {
            for (unsigned char a = X; a <= E; ++a) {
                float axis_max_jerk = get_axis_max_jerk(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a));
                if (curr.abs_axis_feedrate[a] > axis_max_jerk)
                    curr.safe_feedrate = std::min(curr.safe_feedrate, m_marlin_junctions ?
                        float(block.feedrate_profile.cruise * axis_max_jerk / curr.abs_axis_feedrate[a]) : axis_max_jerk);
            }
        }

        if (m_tool_kinematics)
            for (int axis = 0; axis < 2; ++axis)
                if (m_rotary_jerk[axis] > 0.f && std::abs(curr.rotary_feedrate[axis]) > m_rotary_jerk[axis])
                    curr.safe_feedrate = std::min(curr.safe_feedrate,
                        curr.feedrate * m_rotary_jerk[axis] / std::abs(curr.rotary_feedrate[axis]));
        block.feedrate_profile.exit = curr.safe_feedrate;

        float vmax_junction = use_junction_deviation ? vmax_junction_jd : curr.safe_feedrate;
        if (!use_junction_deviation && has_prev_move) {
            bool prev_speed_larger = prev.feedrate > block.feedrate_profile.cruise;
            float smaller_speed_factor = prev_speed_larger ? (block.feedrate_profile.cruise / prev.feedrate) : (prev.feedrate / block.feedrate_profile.cruise);
            vmax_junction = prev_speed_larger ? block.feedrate_profile.cruise : prev.feedrate;

            float v_factor = 1.0f;
            bool limited = false;

            for (unsigned char a = X; a <= E; ++a) {
                if (a == X) {
                    Vec3f exit_v = prev.feedrate * (prev.exit_direction);
                    if (prev_speed_larger)
                        exit_v *= smaller_speed_factor;
                    Vec3f entry_v = block.feedrate_profile.cruise * (curr.enter_direction);
                    Vec3f max_xyz_jerk_v = get_xyz_max_jerk(static_cast<PrintEstimatedStatistics::ETimeMode>(i));
                    if (!m_marlin_junctions) {
                        Vec3f jerk_v = entry_v - exit_v;
                        jerk_v = Vec3f(abs(jerk_v.x()), abs(jerk_v.y()), abs(jerk_v.z()));
                        for (size_t k = 0; k < 3; k++)
                            if (jerk_v[k] > max_xyz_jerk_v[k]) {
                                v_factor *= max_xyz_jerk_v[k] / jerk_v[k];
                                jerk_v *= v_factor;
                                limited = true;
                            }
                    }
                    for (size_t k = 0; m_marlin_junctions && k < 3; k++) {
                        float v_exit = exit_v[k], v_entry = entry_v[k];
                        if (limited) {
                            v_exit *= v_factor;
                            v_entry *= v_factor;
                        }
                        const float jerk = bambu ? std::abs(v_entry - v_exit) :
                                           (v_exit > v_entry) ? ((v_entry > 0.0f || v_exit < 0.0f) ? v_exit - v_entry : std::max(v_exit, -v_entry)) :
                                                                ((v_entry < 0.0f || v_exit > 0.0f) ? v_entry - v_exit : std::max(-v_exit, v_entry));
                        if (jerk > max_xyz_jerk_v[k]) {
                            v_factor *= max_xyz_jerk_v[k] / jerk;
                            limited = true;
                        }
                    }
                }
                else if (a == Y || a == Z) {
                    continue;
                }
                else {
                    float v_exit = prev.axis_feedrate[a];
                    float v_entry = curr.axis_feedrate[a];

                    if (prev_speed_larger)
                        v_exit *= smaller_speed_factor;

                    if (limited) {
                        v_exit *= v_factor;
                        v_entry *= v_factor;
                    }

                    float jerk =
                        (v_exit > v_entry) ?
                        (((v_entry > 0.0f) || (v_exit < 0.0f)) ?
                            (v_exit - v_entry) :
                            std::max(v_exit, -v_entry)) :
                        (((v_entry < 0.0f) || (v_exit > 0.0f)) ?
                            (v_entry - v_exit) :
                            std::max(-v_exit, v_entry));


                    float axis_max_jerk = get_axis_max_jerk(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a));
                    if (jerk > axis_max_jerk) {
                        v_factor *= axis_max_jerk / jerk;
                        limited = true;
                    }
                }
            }

            if (m_tool_kinematics)
                for (int axis = 0; axis < 2; ++axis) {
                    float v_exit = prev.rotary_feedrate[axis] * (prev_speed_larger ? smaller_speed_factor : 1.f);
                    float v_entry = curr.rotary_feedrate[axis];
                    const float jerk = v_exit * v_entry >= 0.f ? std::abs(v_entry - v_exit) :
                                                                               std::max(std::abs(v_exit), std::abs(v_entry));
                    if (m_rotary_jerk[axis] > 0.f && jerk * v_factor > m_rotary_jerk[axis]) {
                        v_factor = m_rotary_jerk[axis] / jerk;
                        limited = true;
                    }
                }
            if (limited)
                vmax_junction *= v_factor;

            float vmax_junction_threshold = vmax_junction * 0.99f;

            if (prev.safe_feedrate > vmax_junction_threshold && curr.safe_feedrate > vmax_junction_threshold)
                vmax_junction = curr.safe_feedrate;
        }

        float v_allowable = max_allowable_speed(-acceleration, curr.safe_feedrate, block.distance);
        block.feedrate_profile.entry = std::min(vmax_junction, v_allowable);

        block.max_entry_speed = vmax_junction;
        block.flags.nominal_length = (block.feedrate_profile.cruise <= v_allowable);
        block.flags.recalculate = true;
        block.safe_feedrate = curr.safe_feedrate;

        block.calculate_trapezoid();

        prev = curr;

        blocks.push_back(block);
    }

    if (m_time_processor.machines[0].blocks.size() > TimeProcessor::Planner::refresh_threshold)
        calculate_time(m_result, TimeProcessor::Planner::queue_size);

    const Vec3f plate_offset = {(float) m_x_offset, (float) m_y_offset, 0.0f};

    if (m_seams_detector.is_active()) {
        if (type == EMoveType::Extrude && m_extrusion_role == erExternalPerimeter) {
            const Vec3f new_pos = m_result.moves.back().position - m_extruder_offsets[filament_id] - plate_offset;
            if (!m_seams_detector.has_first_vertex()) {
                m_seams_detector.set_first_vertex(new_pos);
            } else if (m_detect_layer_based_on_tag) {
            const std::optional<Vec3f> first_vertex = m_seams_detector.get_first_vertex();
            if (new_pos.z() > first_vertex->z()) {
                m_seams_detector.set_first_vertex(new_pos);
                }
            }
        }
        else if ((type != EMoveType::Extrude || (m_extrusion_role != erExternalPerimeter && m_extrusion_role != erOverhangPerimeter)) && m_seams_detector.has_first_vertex()) {
            auto set_end_position = [this](const Vec3f& pos) {
                m_end_position[X] = pos.x(); m_end_position[Y] = pos.y(); m_end_position[Z] = pos.z();
            };

            const Vec3f curr_pos(m_end_position[X], m_end_position[Y], m_end_position[Z]);
            const Vec3f new_pos = m_result.moves.back().position - m_extruder_offsets[filament_id] - plate_offset;
            const std::optional<Vec3f> first_vertex = m_seams_detector.get_first_vertex();

            if ((new_pos - *first_vertex).squaredNorm() < 0.0625f) {
                set_end_position(0.5f * (new_pos + *first_vertex) + m_z_offset * Vec3f::UnitZ());
                store_move_vertex(EMoveType::Seam);
                set_end_position(curr_pos);
            }

            m_seams_detector.activate(false);
        }
    }
    else if (type == EMoveType::Extrude && m_extrusion_role == erExternalPerimeter) {
        m_seams_detector.activate(true);
        m_seams_detector.set_first_vertex(m_result.moves.back().position - m_extruder_offsets[filament_id] - plate_offset);
    }

    if (type == EMoveType::Extrude)
        add_object_mass(filament_id, area_filament_cross_section * delta_pos[E]);

    store_move_vertex(type);
}

void GCodeProcessor::process_VG1(const GCodeReader::GCodeLine& line)
{
    int filament_id = get_filament_id();
    int last_filament_id = get_last_filament_id();
    float filament_diameter = (static_cast<size_t>(filament_id) < m_result.filament_diameters.size()) ? m_result.filament_diameters[filament_id] : m_result.filament_diameters.back();
    float filament_radius = 0.5f * filament_diameter;
    float area_filament_cross_section = static_cast<float>(M_PI) * sqr(filament_radius);

    auto absolute_position = [this](Axis axis, const GCodeReader::GCodeLine& lineG1) {
        bool is_relative = (m_global_positioning_type == EPositioningType::Relative);
        if (axis == E)
            is_relative |= (m_e_local_positioning_type == EPositioningType::Relative);

        if (lineG1.has(Slic3r::Axis(axis))) {
            float lengthsScaleFactor = (m_units == EUnits::Inches) ? INCHES_TO_MM : 1.0f;
            float ret = lineG1.value(Slic3r::Axis(axis)) * lengthsScaleFactor;
            return is_relative ? m_start_position[axis] + ret : m_origin[axis] + ret;
        }
        else
            return m_start_position[axis];
        };

    auto move_type = [this](const AxisCoords& delta_pos) {
        EMoveType type = EMoveType::Noop;

        if (m_wiping)
            type = EMoveType::Wipe;
        else if (delta_pos[E] < 0.0f)
            type = (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f || delta_pos[Z] != 0.0f) ? EMoveType::Travel : EMoveType::Retract;
        else if (delta_pos[E] > 0.0f) {
            if (delta_pos[X] == 0.0f && delta_pos[Y] == 0.0f)
                type = (delta_pos[Z] == 0.0f) ? EMoveType::Unretract : EMoveType::Travel;
            else if (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f)
                type = EMoveType::Extrude;
        }
        else if (delta_pos[X] != 0.0f || delta_pos[Y] != 0.0f || delta_pos[Z] != 0.0f)
            type = EMoveType::Travel;

        return type;
        };

    ++m_g1_line_id;

    m_time_processor.machine_envelope_processing_enabled = true;

    for (unsigned char a = X; a <= E; ++a) {
        m_end_position[a] = absolute_position((Axis)a, line);
    }

    if (line.has_f())
        m_feedrate = line.f() * MMMIN_TO_MMSEC;

    float max_abs_delta = 0.0f;
    AxisCoords delta_pos;
    for (unsigned char a = X; a <= E; ++a) {
        delta_pos[a] = m_end_position[a] - m_start_position[a];
        max_abs_delta = std::max<float>(max_abs_delta, std::abs(delta_pos[a]));
    }

    if (max_abs_delta == 0.0f)
        return;

    EMoveType type = move_type(delta_pos);
    if (EMoveType::Unretract == type && m_virtual_flushing) {
        int extruder_id = get_extruder_id();
        float volume_flushed_filament = area_filament_cross_section * delta_pos[E];
        if (m_remaining_volume[extruder_id] > volume_flushed_filament)
        {
            m_used_filaments.update_flush_per_filament(last_filament_id, volume_flushed_filament);
            m_remaining_volume[extruder_id] -= volume_flushed_filament;
        }
        else {
            m_used_filaments.update_flush_per_filament(last_filament_id, m_remaining_volume[extruder_id]);
            m_used_filaments.update_flush_per_filament(filament_id, volume_flushed_filament - m_remaining_volume[extruder_id]);
            m_remaining_volume[extruder_id] = 0.f;
        }
    }

    if (line.has_f())
        m_feedrate = line.f() * MMMIN_TO_MMSEC;

    auto move_length = [](const AxisCoords& delta_pos) {
        float sq_xyz_length = sqr(delta_pos[X]) + sqr(delta_pos[Y]) + sqr(delta_pos[Z]);
        return (sq_xyz_length > 0.0f) ? std::sqrt(sq_xyz_length) : std::abs(delta_pos[E]);
    };

    auto is_extrusion_only_move = [](const AxisCoords& delta_pos) {
        return delta_pos[X] == 0.0f && delta_pos[Y] == 0.0f && delta_pos[Z] == 0.0f && delta_pos[E] != 0.0f;
    };

    float distance = move_length(delta_pos);
    assert(distance != 0.0f);
    float inv_distance = 1.0f / distance;
    const bool bambu = m_print != nullptr && m_print->is_BBL_printer();

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        TimeMachine& machine = m_time_processor.machines[i];
        if (!machine.enabled)
            continue;

        TimeMachine::State& curr = machine.curr;
        TimeMachine::State& prev = machine.prev;
        std::vector<TimeBlock>& blocks = machine.blocks;

        curr.feedrate = (delta_pos[E] == 0.0f) ?
            minimum_travel_feedrate(static_cast<PrintEstimatedStatistics::ETimeMode>(i), m_feedrate) :
            minimum_feedrate(static_cast<PrintEstimatedStatistics::ETimeMode>(i), m_feedrate);

        curr.enter_direction = { static_cast<float>(delta_pos[X]), static_cast<float>(delta_pos[Y]), static_cast<float>(delta_pos[Z]) };
        float norm = curr.enter_direction.norm();
        if (!is_extrusion_only_move(delta_pos))
            curr.enter_direction = curr.enter_direction / norm;
        curr.exit_direction = curr.enter_direction;
        curr.jd_unit_vec = Vec4f(static_cast<float>(delta_pos[X]),
                                 static_cast<float>(delta_pos[Y]),
                                 static_cast<float>(delta_pos[Z]),
                                 static_cast<float>(delta_pos[E])).normalized();

        TimeBlock block;
        block.move_type = type;
        block.skippable_type = m_skippable_type;
        block.role = (type != EMoveType::Travel || m_extrusion_role == erCustom) ? m_extrusion_role : erNone;
        block.distance = distance;
        block.g1_line_id = m_g1_line_id;
        block.layer_id = std::max<unsigned int>(1, m_layer_id);
        block.flags.prepare_stage = m_processing_start_custom_gcode;
        block.flags.nonplanar = m_nonplanar_patch_shell != 0xFF;
        block.feed_interval = m_nonplanar_feed_interval;

        if ((!m_marlin_junctions || bambu) &&
            (prev.exit_direction(0) != 0.0f || prev.exit_direction(1) != 0.0f) &&
            (curr.enter_direction(0) != 0.0f || curr.enter_direction(1) != 0.0f) &&
            std::hypot(delta_pos[X], delta_pos[Y]) > 1e-4) {
            Vec3f v1 = prev.exit_direction;
            v1(2, 0) = 0.0f;
            v1.normalize();
            Vec3f v2 = curr.enter_direction;
            v2(2, 0) = 0.0f;
            v2.normalize();
            float norm_diff = (v2 - v1).norm();
            if (norm_diff < 0.5f && norm_diff > 0.00001f) {
                float dot = v1(0) * v2(0) + v1(1) * v2(1);
                float cross = v1(0) * v2(1) - v1(1) * v2(0);
                float angle = float(atan2(double(cross), double(dot)));
                float sin_theta_2 = sqrt((1.0f - cos(angle)) * 0.5f);
                float r = sqrt(sqr(delta_pos[X]) + sqr(delta_pos[Y])) * 0.5 / sin_theta_2;
                float acc = get_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i));
                curr.feedrate = std::min(curr.feedrate, sqrt(acc * r));
            }
        }

        float min_feedrate_factor = 1.0f;
        for (unsigned char a = X; a <= E; ++a) {
            curr.axis_feedrate[a] = curr.feedrate * delta_pos[a] * inv_distance;
            if (a == E)
                curr.axis_feedrate[a] *= machine.extrude_factor_override_percentage;

            curr.abs_axis_feedrate[a] = std::abs(curr.axis_feedrate[a]);
            if (curr.abs_axis_feedrate[a] != 0.0f) {
                float axis_max_feedrate = get_axis_max_feedrate(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a), m_machine_config_idx);
                if (axis_max_feedrate != 0.0f) min_feedrate_factor = std::min<float>(min_feedrate_factor, axis_max_feedrate / curr.abs_axis_feedrate[a]);
            }
        }
        curr.feedrate *= min_feedrate_factor;
        block.feedrate_profile.cruise = curr.feedrate;

        if (min_feedrate_factor < 1.0f) {
            for (unsigned char a = X; a <= E; ++a) {
                curr.axis_feedrate[a] *= min_feedrate_factor;
                curr.abs_axis_feedrate[a] *= min_feedrate_factor;
            }
        }

        float acceleration =
            (type == EMoveType::Travel) ? get_travel_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)) :
            (is_extrusion_only_move(delta_pos) ?
                get_retract_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)) :
                get_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)));

        for (unsigned char a = X; a <= E; ++a) {
            float axis_max_acceleration = get_axis_max_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a), m_machine_config_idx);
            if (acceleration * std::abs(delta_pos[a]) * inv_distance > axis_max_acceleration)
                acceleration = axis_max_acceleration / (std::abs(delta_pos[a]) * inv_distance);
        }

        block.acceleration = acceleration;
        if (m_flavor == gcfKlipper)
            block.smooth_acceleration = std::min(acceleration, get_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i)) *
                                                                   (1.0f - m_klipper_min_cruise_ratio));

        static const float PREVIOUS_FEEDRATE_THRESHOLD = 0.0001f;
        const bool has_prev_move = !blocks.empty() && prev.feedrate > PREVIOUS_FEEDRATE_THRESHOLD;

        const float vmax_junction_jd = calc_vmax_junction_deviation(block, prev, curr, has_prev_move,
                                                                    static_cast<PrintEstimatedStatistics::ETimeMode>(i),
                                                                    blocks.empty() ? nullptr : &blocks.back());
        const bool use_junction_deviation = vmax_junction_jd >= 0.0f;

        curr.safe_feedrate = use_junction_deviation ? 0.0f : block.feedrate_profile.cruise;

        if (!use_junction_deviation) {
            for (unsigned char a = X; a <= E; ++a) {
                float axis_max_jerk = get_axis_max_jerk(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a));
                if (curr.abs_axis_feedrate[a] > axis_max_jerk)
                    curr.safe_feedrate = std::min(curr.safe_feedrate, m_marlin_junctions ?
                        float(block.feedrate_profile.cruise * axis_max_jerk / curr.abs_axis_feedrate[a]) : axis_max_jerk);
            }
        }

        block.feedrate_profile.exit = curr.safe_feedrate;

        float vmax_junction = use_junction_deviation ? vmax_junction_jd : curr.safe_feedrate;
        if (!use_junction_deviation && has_prev_move) {
            bool prev_speed_larger = prev.feedrate > block.feedrate_profile.cruise;
            float smaller_speed_factor = prev_speed_larger ? (block.feedrate_profile.cruise / prev.feedrate) : (prev.feedrate / block.feedrate_profile.cruise);
            vmax_junction = prev_speed_larger ? block.feedrate_profile.cruise : prev.feedrate;

            float v_factor = 1.0f;
            bool limited = false;

            for (unsigned char a = X; a <= E; ++a) {
                if (a == X) {
                    Vec3f exit_v = prev.feedrate * (prev.exit_direction);
                    if (prev_speed_larger)
                        exit_v *= smaller_speed_factor;
                    Vec3f entry_v = block.feedrate_profile.cruise * (curr.enter_direction);
                    Vec3f max_xyz_jerk_v = get_xyz_max_jerk(static_cast<PrintEstimatedStatistics::ETimeMode>(i));
                    if (!m_marlin_junctions) {
                        Vec3f jerk_v = entry_v - exit_v;
                        jerk_v = Vec3f(abs(jerk_v.x()), abs(jerk_v.y()), abs(jerk_v.z()));
                        for (size_t k = 0; k < 3; k++)
                            if (jerk_v[k] > max_xyz_jerk_v[k]) {
                                v_factor *= max_xyz_jerk_v[k] / jerk_v[k];
                                jerk_v *= v_factor;
                                limited = true;
                            }
                    }
                    for (size_t k = 0; m_marlin_junctions && k < 3; k++) {
                        float v_exit = exit_v[k], v_entry = entry_v[k];
                        if (limited) {
                            v_exit *= v_factor;
                            v_entry *= v_factor;
                        }
                        const float jerk = bambu ? std::abs(v_entry - v_exit) :
                                           (v_exit > v_entry) ? ((v_entry > 0.0f || v_exit < 0.0f) ? v_exit - v_entry : std::max(v_exit, -v_entry)) :
                                                                ((v_entry < 0.0f || v_exit > 0.0f) ? v_entry - v_exit : std::max(-v_exit, v_entry));
                        if (jerk > max_xyz_jerk_v[k]) {
                            v_factor *= max_xyz_jerk_v[k] / jerk;
                            limited = true;
                        }
                    }
                }
                else if (a == Y || a == Z) {
                    continue;
                }
                else {
                    float v_exit = prev.axis_feedrate[a];
                    float v_entry = curr.axis_feedrate[a];

                    if (prev_speed_larger)
                        v_exit *= smaller_speed_factor;

                    if (limited) {
                        v_exit *= v_factor;
                        v_entry *= v_factor;
                    }

                    float jerk =
                        (v_exit > v_entry) ?
                        (((v_entry > 0.0f) || (v_exit < 0.0f)) ?
                            (v_exit - v_entry) :
                            std::max(v_exit, -v_entry)) :
                        (((v_entry < 0.0f) || (v_exit > 0.0f)) ?
                            (v_entry - v_exit) :
                            std::max(-v_exit, v_entry));


                    float axis_max_jerk = get_axis_max_jerk(static_cast<PrintEstimatedStatistics::ETimeMode>(i), static_cast<Axis>(a));
                    if (jerk > axis_max_jerk) {
                        v_factor *= axis_max_jerk / jerk;
                        limited = true;
                    }
                }
            }

            if (limited)
                vmax_junction *= v_factor;

            float vmax_junction_threshold = vmax_junction * 0.99f;

            if (prev.safe_feedrate > vmax_junction_threshold && curr.safe_feedrate > vmax_junction_threshold)
                vmax_junction = curr.safe_feedrate;
        }

        float v_allowable = max_allowable_speed(-acceleration, curr.safe_feedrate, block.distance);
        block.feedrate_profile.entry = std::min(vmax_junction, v_allowable);

        block.max_entry_speed = vmax_junction;
        block.flags.nominal_length = (block.feedrate_profile.cruise <= v_allowable);
        block.flags.recalculate = true;
        block.safe_feedrate = curr.safe_feedrate;

        block.calculate_trapezoid();

        prev = curr;

        blocks.push_back(block);
    }

}

void GCodeProcessor::process_G2_G3(const GCodeReader::GCodeLine& line, bool clockwise)
{
    enum class EFitting { None, IJ, R };
    std::string_view axis_pos_I;
    std::string_view axis_pos_J;
    EFitting fitting = EFitting::None;
    if (line.has('R')) {
        fitting = EFitting::R;
    } else {
        axis_pos_I = line.axis_pos('I');
        axis_pos_J = line.axis_pos('J');
        if (! axis_pos_I.empty() || ! axis_pos_J.empty())
            fitting = EFitting::IJ;
    }

    if (fitting == EFitting::None)
        return;

    int filament_id = get_filament_id();
    const float filament_diameter = (static_cast<size_t>(filament_id) < m_result.filament_diameters.size()) ? m_result.filament_diameters[filament_id] : m_result.filament_diameters.back();
    const float filament_radius = 0.5f * filament_diameter;
    const float area_filament_cross_section = static_cast<float>(M_PI) * sqr(filament_radius);

    AxisCoords end_position = m_start_position;
    for (unsigned char a = X; a <= E; ++a) {
        end_position[a] = extract_absolute_position_on_axis((Axis)a, line, double(area_filament_cross_section));
    }

    Vec3f rel_center = Vec3f::Zero();
#ifndef NDEBUG
    double radius = 0.0;
#endif
    if (fitting == EFitting::R) {
        float r;
        if (!line.has_value('R', r) || r == 0.0f)
            return;
#ifndef NDEBUG
        radius = (double)std::abs(r);
#endif
        const Vec2f start_pos((float)m_start_position[X], (float)m_start_position[Y]);
        const Vec2f end_pos((float)end_position[X], (float)end_position[Y]);
        const Vec2f c = Geometry::ArcWelder::arc_center(start_pos, end_pos, r, !clockwise);
        rel_center.x() = c.x() - m_start_position[X];
        rel_center.y() = c.y() - m_start_position[Y];
    }
    else {
        assert(fitting == EFitting::IJ);
        if (! axis_pos_I.empty() && ! line.has_value(axis_pos_I, rel_center.x()))
            return;
        if (! axis_pos_J.empty() && ! line.has_value(axis_pos_J, rel_center.y()))
            return;
    }

    if (m_units == EUnits::Inches)
        rel_center *= INCHES_TO_MM;

    struct Arc
    {
        Vec3d start{ Vec3d::Zero() };
        Vec3d end{ Vec3d::Zero() };
        Vec3d center{ Vec3d::Zero() };

        double angle{ 0.0 };
        double delta_x() const { return end.x() - start.x(); }
        double delta_y() const { return end.y() - start.y(); }
        double delta_z() const { return end.z() - start.z(); }

        double length() const { return angle * start_radius(); }
        double travel_length() const { return std::sqrt(sqr(length()) + sqr(delta_z())); }
        double start_radius() const { return (start - center).norm(); }
        double end_radius() const { return (end - center).norm(); }

        Vec3d relative_start() const { return start - center; }
        Vec3d relative_end() const { return end - center; }

        bool is_full_circle() const { return std::abs(delta_x()) < EPSILON && std::abs(delta_y()) < EPSILON; }
    };

    Arc arc;

    arc.start = Vec3d(m_start_position[X], m_start_position[Y], m_start_position[Z]);

    arc.center = arc.start + rel_center.cast<double>();

    arc.end = Vec3d(end_position[X], end_position[Y], end_position[Z]);

    if (std::abs(arc.end_radius() - arc.start_radius()) > 0.001) {
    }

    assert(fitting != EFitting::R || std::abs(radius - arc.start_radius()) < EPSILON);

    std::optional<float> feedrate;
    if (line.has_f()) {
        feedrate = 1.0f * line.f() * MMMIN_TO_MMSEC;
    }

    std::optional<float> extrusion;
    if (line.has_e())
        extrusion = end_position[E] - m_start_position[E];

    const Vec3d rel_arc_start = arc.relative_start();
    const Vec3d rel_arc_end   = arc.relative_end();

    if (arc.is_full_circle())
        arc.angle = 2.0 * PI;
    else {
        arc.angle = std::atan2(rel_arc_start.x() * rel_arc_end.y() - rel_arc_start.y() * rel_arc_end.x(),
            rel_arc_start.x() * rel_arc_end.x() + rel_arc_start.y() * rel_arc_end.y());
        if (arc.angle < 0.0)
            arc.angle += 2.0 * PI;
        if (clockwise)
            arc.angle -= 2.0 * PI;
    }

    const double travel_length = arc.travel_length();
    if (travel_length < 0.001)
        return;
    m_arc_first_block = true;

    auto adjust_target = [this](const AxisCoords& target, const AxisCoords& prev_position) {
        AxisCoords ret = target;
        if (m_global_positioning_type == EPositioningType::Relative) {
            for (unsigned char a = X; a <= E; ++a) {
                ret[a] -= prev_position[a];
            }
        }
        else if (m_e_local_positioning_type == EPositioningType::Relative)
            ret[E] -= prev_position[E];

        const double lengthsScaleFactor = (m_units == EUnits::Inches) ? double(INCHES_TO_MM) : 1.0;
        for (unsigned char a = X; a <= E; ++a) {
            ret[a] /= lengthsScaleFactor;
        }
        return ret;
    };

    auto internal_only_g1_line = [this](const AxisCoords& target, bool has_z, const std::optional<float>& feedrate,
        const std::optional<float>& extrusion, const std::optional<unsigned int>& remaining_internal_g1_lines = std::nullopt) {
          std::array<std::optional<double>, 4> g1_axes = { target[X], target[Y], std::nullopt, std::nullopt };
          std::optional<double> g1_feedrate = std::nullopt;
          if (has_z)
              g1_axes[Z] = target[Z];
          if (extrusion.has_value())
              g1_axes[E] = target[E];
          if (feedrate.has_value())
              g1_feedrate = (double)*feedrate / MMMIN_TO_MMSEC;
          process_G1(g1_axes, g1_feedrate, G1DiscretizationOrigin::G2G3, remaining_internal_g1_lines);
    };

    if (m_flavor == gcfMarlinFirmware) {

        static const float MAX_ARC_DEVIATION = 0.02f;
        static const float MIN_ARC_SEGMENTS_PER_SEC = 50;
        static const float MIN_ARC_SEGMENT_MM = 0.1f;
        static const float MAX_ARC_SEGMENT_MM = 2.0f;
        const float feedrate_mm_s = feedrate.has_value() ? *feedrate : m_feedrate;
        const float radius_mm = rel_center.norm();
        const float segment_mm = std::clamp(std::min(std::sqrt(8.0f * radius_mm * MAX_ARC_DEVIATION), feedrate_mm_s * (1.0f / MIN_ARC_SEGMENTS_PER_SEC)), MIN_ARC_SEGMENT_MM, MAX_ARC_SEGMENT_MM);
        const float flat_mm = radius_mm * std::abs(arc.angle);
        const size_t segments = std::max<size_t>(flat_mm / segment_mm + 0.8f, 1);

        AxisCoords prev_target = m_start_position;

        if (segments > 1) {
            const float inv_segments = 1.0f / static_cast<float>(segments);
            const float theta_per_segment = static_cast<float>(arc.angle) * inv_segments;
            const float cos_T = cos(theta_per_segment);
            const float sin_T = sin(theta_per_segment);
            const float z_per_segment = arc.delta_z() * inv_segments;
            const float extruder_per_segment = (extrusion.has_value()) ? *extrusion * inv_segments : 0.0f;

            static const size_t N_ARC_CORRECTION = 25;
            size_t arc_recalc_count = N_ARC_CORRECTION;

            Vec2f rvec(-rel_center.x(), -rel_center.y());
            AxisCoords arc_target = { 0.0f, 0.0f, m_start_position[Z], m_start_position[E] };
            for (size_t i = 1; i < segments; ++i) {
                if (--arc_recalc_count) {
                    const float r_new_Y = rvec.x() * sin_T + rvec.y() * cos_T;
                    rvec.x() = rvec.x() * cos_T - rvec.y() * sin_T;
                    rvec.y() = r_new_Y;
                }
                else {
                    arc_recalc_count = N_ARC_CORRECTION;
                    const float Ti = i * theta_per_segment;
                    const float cos_Ti = cos(Ti);
                    const float sin_Ti = sin(Ti);
                    rvec.x() = -rel_center.x() * cos_Ti + rel_center.y() * sin_Ti;
                    rvec.y() = -rel_center.x() * sin_Ti - rel_center.y() * cos_Ti;
                }

                arc_target[X] = arc.center.x() + rvec.x();
                arc_target[Y] = arc.center.y() + rvec.y();
                arc_target[Z] += z_per_segment;
                arc_target[E] += extruder_per_segment;

                m_start_position = m_end_position;
                internal_only_g1_line(adjust_target(arc_target, prev_target), z_per_segment != 0.0, (i == 1) ? feedrate : std::nullopt,
                    extrusion, segments - i);
                prev_target = arc_target;
            }
        }

        m_start_position = m_end_position;
        internal_only_g1_line(adjust_target(end_position, prev_target), arc.delta_z() != 0.0, (segments == 1) ? feedrate : std::nullopt, extrusion);
    }
    else {
#if 0
        static const double MM_PER_ARC_SEGMENT = 1.0;
        const size_t segments = std::max<size_t>(std::floor(travel_length / MM_PER_ARC_SEGMENT), 1);
#else
        static const double gcode_arc_tolerance = 0.0125;
        const size_t segments = Geometry::ArcWelder::arc_discretization_steps(arc.start_radius(), std::abs(arc.angle), gcode_arc_tolerance);
#endif

        const double inv_segment = 1.0 / double(segments);
        const double theta_per_segment = arc.angle * inv_segment;
        const double z_per_segment = arc.delta_z() * inv_segment;
        const double extruder_per_segment = (extrusion.has_value()) ? *extrusion * inv_segment : 0.0;
        const double sq_theta_per_segment = sqr(theta_per_segment);
        const double cos_T = 1.0 - 0.5 * sq_theta_per_segment;
        const double sin_T = theta_per_segment - sq_theta_per_segment * theta_per_segment / 6.0f;

        AxisCoords prev_target = m_start_position;
        AxisCoords arc_target;

        arc_target[Z] = m_start_position[Z];

        arc_target[E] = m_start_position[E];

        static const size_t N_ARC_CORRECTION = 25;
        Vec3d curr_rel_arc_start = arc.relative_start();
        size_t count = N_ARC_CORRECTION;

        for (size_t i = 1; i < segments; ++i) {
            if (count-- == 0) {
                const double cos_Ti = ::cos(i * theta_per_segment);
                const double sin_Ti = ::sin(i * theta_per_segment);
                curr_rel_arc_start.x() = -double(rel_center.x()) * cos_Ti + double(rel_center.y()) * sin_Ti;
                curr_rel_arc_start.y() = -double(rel_center.x()) * sin_Ti - double(rel_center.y()) * cos_Ti;
                count = N_ARC_CORRECTION;
            }
            else {
                const float r_axisi = curr_rel_arc_start.x() * sin_T + curr_rel_arc_start.y() * cos_T;
                curr_rel_arc_start.x() = curr_rel_arc_start.x() * cos_T - curr_rel_arc_start.y() * sin_T;
                curr_rel_arc_start.y() = r_axisi;
            }

            arc_target[X] = arc.center.x() + curr_rel_arc_start.x();
            arc_target[Y] = arc.center.y() + curr_rel_arc_start.y();
            arc_target[Z] += z_per_segment;
            arc_target[E] += extruder_per_segment;

            m_start_position = m_end_position;
            internal_only_g1_line(adjust_target(arc_target, prev_target), z_per_segment != 0.0, (i == 1) ? feedrate : std::nullopt,
                extrusion, segments - i);
            prev_target = arc_target;
        }

        m_start_position = m_end_position;
        internal_only_g1_line(adjust_target(end_position, prev_target), arc.delta_z() != 0.0, (segments == 1) ? feedrate : std::nullopt, extrusion);
    }
}

void GCodeProcessor::process_G4(const GCodeReader::GCodeLine& line)
{
    float value_s = 0.0;
    float value_p = 0.0;
    if (line.has_value('S', value_s) || line.has_value('P', value_p)) {
        value_s += value_p * 0.001;
        simulate_st_synchronize(value_s);
    }
}

void GCodeProcessor::process_G29(const GCodeReader::GCodeLine& line)
{
    const float value_s = 260.0;
    if (s_IsBBLPrinter){
        if(m_measure_g29_time)
            simulate_st_synchronize(value_s);
    }
    else{
        simulate_st_synchronize(value_s);
    }
}

void GCodeProcessor::process_G10(const GCodeReader::GCodeLine& line)
{
    GCodeReader::GCodeLine g10;
    g10.set(Axis::E, -this->m_parser.config().retraction_length.get_at(m_extruder_id));
    g10.set(Axis::F,  this->m_parser.config().retraction_speed.get_at(m_extruder_id) * 60);
    const float feedrate = m_feedrate;
    --m_g1_line_id;
    process_G1(g10);
    m_feedrate = feedrate;
}

void GCodeProcessor::process_G11(const GCodeReader::GCodeLine& line)
{
    GCodeReader::GCodeLine g11;
    g11.set(Axis::E, this->m_parser.config().retraction_length.get_at(m_extruder_id) + this->m_parser.config().retract_restart_extra.get_at(m_extruder_id));
    g11.set(Axis::F, this->m_parser.config().deretraction_speed.get_at(m_extruder_id) * 60);
    const float feedrate = m_feedrate;
    --m_g1_line_id;
    process_G1(g11);
    m_feedrate = feedrate;
}

void GCodeProcessor::process_G20(const GCodeReader::GCodeLine& line)
{
    m_units = EUnits::Inches;
}

void GCodeProcessor::process_G21(const GCodeReader::GCodeLine& line)
{
    m_units = EUnits::Millimeters;
}

void GCodeProcessor::process_G22(const GCodeReader::GCodeLine& line)
{
    store_move_vertex(EMoveType::Retract);
}

void GCodeProcessor::process_G23(const GCodeReader::GCodeLine& line)
{
    store_move_vertex(EMoveType::Unretract);
}

void GCodeProcessor::process_G28(const GCodeReader::GCodeLine& line)
{
    const size_t previous_moves = m_result.moves.size();
    std::string_view cmd = line.cmd();
    std::string new_line_raw = { cmd.data(), cmd.size() };
    bool found = false;
    if (line.has('X')) {
        new_line_raw += " X0";
        found = true;
    }
    if (line.has('Y')) {
        new_line_raw += " Y0";
        found = true;
    }
    if (line.has('Z')) {
        new_line_raw += " Z0";
        found = true;
    }
    if (!found)
        new_line_raw += " X0  Y0  Z0";

    GCodeReader::GCodeLine new_gline;
    GCodeReader reader;
    reader.parse_line(new_line_raw, [&](GCodeReader& reader, const GCodeReader::GCodeLine& gline) { new_gline = gline; });
    process_G1(new_gline);
    if (m_tool_kinematics) {
        m_rotary_position_known_axes = 0;
        for (size_t i = previous_moves; i < m_result.moves.size(); ++i)
            m_result.moves[i].clearance_pose_known = false;
    }
}

void GCodeProcessor::process_G90(const GCodeReader::GCodeLine& line)
{
    m_global_positioning_type = EPositioningType::Absolute;
}

void GCodeProcessor::process_G91(const GCodeReader::GCodeLine& line)
{
    m_global_positioning_type = EPositioningType::Relative;
}

void GCodeProcessor::process_G92(const GCodeReader::GCodeLine& line)
{
    float lengths_scale_factor = (m_units == EUnits::Inches) ? INCHES_TO_MM : 1.0f;
    bool any_found = false;

    if (line.has_x()) {
        m_origin[X] = m_end_position[X] - line.x() * lengths_scale_factor;
        any_found = true;
    }

    if (line.has_y()) {
        m_origin[Y] = m_end_position[Y] - line.y() * lengths_scale_factor;
        any_found = true;
    }

    if (line.has_z()) {
        m_origin[Z] = m_end_position[Z] - line.z() * lengths_scale_factor;
        any_found = true;
        if (m_result.belt_tilt_angle != 0.f)
            m_result.belt_z_origin = m_origin[Z];
    }

    if (line.has_e()) {
        m_end_position[E] = line.e() * lengths_scale_factor;
        any_found = true;
    }
    else
        simulate_st_synchronize();

    if (!any_found && !line.has_unknown_axis()) {
        for (unsigned char a = X; a <= E; ++a) {
            m_origin[a] = m_end_position[a];
        }
    }
}

void GCodeProcessor::process_M1(const GCodeReader::GCodeLine& line)
{
    simulate_st_synchronize();
}

void GCodeProcessor::process_M82(const GCodeReader::GCodeLine& line)
{
    m_e_local_positioning_type = EPositioningType::Absolute;
}

void GCodeProcessor::process_M83(const GCodeReader::GCodeLine& line)
{
    m_e_local_positioning_type = EPositioningType::Relative;
}

void GCodeProcessor::process_M104(const GCodeReader::GCodeLine& line)
{
    int filament_id = get_filament_id();
    float new_temp;
    if (line.has_value('S', new_temp))
        m_extruder_temps[filament_id] = new_temp;
}

void GCodeProcessor::process_VM104(const GCodeReader::GCodeLine& line)
{
    process_M104(line);
}

void GCodeProcessor::process_M106(const GCodeReader::GCodeLine& line)
{
    if (!line.has('P') || (line.has('P') && line.p() == 1.0f)) {
        float new_fan_speed;
        if (line.has_value('S', new_fan_speed))
            m_fan_speed = (100.0f / 255.0f) * new_fan_speed;
        else
            m_fan_speed = 100.0f;
    }
}

void GCodeProcessor::process_M900(const GCodeReader::GCodeLine &line)
{
    float pa_value = m_pressure_advance;
    line.has_value('K', pa_value);
    m_pressure_advance = std::max(0.0f, pa_value);
}

void GCodeProcessor::process_M572(const GCodeReader::GCodeLine &line)
{
    float pa_value = m_pressure_advance;
    line.has_value('S', pa_value);
    m_pressure_advance = std::max(0.0f, pa_value);
}

void GCodeProcessor::process_SET_PRESSURE_ADVANCE(const GCodeReader::GCodeLine& line)
{
    static const std::regex regex(R"(SET_PRESSURE_ADVANCE\s+(?:.*\s+)?ADVANCE\s*=\s*([\d.]+))");
    std::smatch matches;

    if (std::regex_search(line.raw(), matches, regex) && matches.size() > 1) {
        float pa_value = 0;
        try {
            pa_value = std::stof(matches[1].str());
        } catch (...) {}
        m_pressure_advance = std::max(0.0f, pa_value);
    }
}

void GCodeProcessor::process_M107(const GCodeReader::GCodeLine& line)
{
    m_fan_speed = 0.0f;
}

void GCodeProcessor::process_M108(const GCodeReader::GCodeLine& line)
{

    if (m_flavor != gcfSailfish)
        return;

    std::string cmd = line.raw();
    size_t pos = cmd.find("T");
    if (pos != std::string::npos)
        process_T(cmd.substr(pos));
}

void GCodeProcessor::process_M109(const GCodeReader::GCodeLine& line)
{
    int filament_id = get_filament_id();
    float new_temp;
    if (line.has_value('R', new_temp)) {
        float val;
        if (line.has_value('T', val)) {
            size_t eid = static_cast<size_t>(val);
            if (eid < m_extruder_temps.size())
                m_extruder_temps[eid] = new_temp;
        }
        else
            m_extruder_temps[filament_id] = new_temp;
    }
    else if (line.has_value('S', new_temp))
        m_extruder_temps[filament_id] = new_temp;
}

void GCodeProcessor::process_VM109(const GCodeReader::GCodeLine& line)
{
    process_M109(line);
}

void GCodeProcessor::process_M132(const GCodeReader::GCodeLine& line)
{

    if (line.has('X'))
        m_origin[X] = 0.0f;

    if (line.has('Y'))
        m_origin[Y] = 0.0f;

    if (line.has('Z'))
        m_origin[Z] = 0.0f;

    if (line.has('E'))
        m_origin[E] = 0.0f;
}

void GCodeProcessor::process_M135(const GCodeReader::GCodeLine& line)
{

    if (m_flavor != gcfMakerWare)
        return;

    std::string cmd = line.raw();
    size_t pos = cmd.find("T");
    if (pos != std::string::npos)
        process_T(cmd.substr(pos));
}

void GCodeProcessor::process_M140(const GCodeReader::GCodeLine& line)
{
    float new_temp;
    if (line.has_value('S', new_temp))
        m_highest_bed_temp = m_highest_bed_temp < (int)new_temp ? (int)new_temp : m_highest_bed_temp;
}

void GCodeProcessor::process_M190(const GCodeReader::GCodeLine& line)
{
    float new_temp;
    if (line.has_value('S', new_temp))
        m_highest_bed_temp = m_highest_bed_temp < (int)new_temp ? (int)new_temp : m_highest_bed_temp;
}

void GCodeProcessor::process_M191(const GCodeReader::GCodeLine& line)
{
    float chamber_temp = 0;
    const float wait_chamber_temp_time = 720.0;
    if (line.has_value('S', chamber_temp) && chamber_temp > 40)
        simulate_st_synchronize(wait_chamber_temp_time);
}


void GCodeProcessor::process_M201(const GCodeReader::GCodeLine& line)
{
    if (m_tool_kinematics)
        for (int axis = 0; axis < 2; ++axis)
            if (float value; line.has_value(m_tool_kinematics->rotary_axes()[axis].letter, value))
                m_rotary_acceleration[axis] = std::max(0.f, value);

    float factor = ((m_flavor != gcfRepRapSprinter && m_flavor != gcfRepRapFirmware) && m_units == EUnits::Inches) ? INCHES_TO_MM : 1.0f;

    auto set_all_slots = [](ConfigOptionFloats &option, size_t mode, float value) {
        for (size_t slot_base = 0; slot_base < option.size(); slot_base += 2)
            set_option_value(option, slot_base + mode, value);
    };
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (static_cast<PrintEstimatedStatistics::ETimeMode>(i) == PrintEstimatedStatistics::ETimeMode::Normal || m_time_processor.machine_envelope_processing_enabled) {
            if (line.has_x()) set_all_slots(m_time_processor.machine_limits.machine_max_acceleration_x, i, line.x() * factor);

            if (line.has_y()) set_all_slots(m_time_processor.machine_limits.machine_max_acceleration_y, i, line.y() * factor);

            if (line.has_z()) set_all_slots(m_time_processor.machine_limits.machine_max_acceleration_z, i, line.z() * factor);

            if (line.has_e()) set_all_slots(m_time_processor.machine_limits.machine_max_acceleration_e, i, line.e() * factor);
        }
    }
}

void GCodeProcessor::process_M203(const GCodeReader::GCodeLine& line)
{
    if (m_flavor == gcfRepetier)
        return;

    float factor = (m_flavor == gcfMarlinLegacy || m_flavor == gcfMarlinFirmware || m_flavor == gcfSmoothie || m_flavor == gcfKlipper) ? 1.0f : MMMIN_TO_MMSEC;

    auto set_all_slots = [](ConfigOptionFloats &option, size_t mode, float value) {
        for (size_t slot_base = 0; slot_base < option.size(); slot_base += 2)
            set_option_value(option, slot_base + mode, value);
    };
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (static_cast<PrintEstimatedStatistics::ETimeMode>(i) == PrintEstimatedStatistics::ETimeMode::Normal || m_time_processor.machine_envelope_processing_enabled) {
            if (line.has_x())
                set_all_slots(m_time_processor.machine_limits.machine_max_speed_x, i, line.x() * factor);

            if (line.has_y())
                set_all_slots(m_time_processor.machine_limits.machine_max_speed_y, i, line.y() * factor);

            if (line.has_z())
                set_all_slots(m_time_processor.machine_limits.machine_max_speed_z, i, line.z() * factor);

            if (line.has_e())
                set_all_slots(m_time_processor.machine_limits.machine_max_speed_e, i, line.e() * factor);
        }
    }
}

void GCodeProcessor::process_M204(const GCodeReader::GCodeLine& line)
{
    float value;
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (static_cast<PrintEstimatedStatistics::ETimeMode>(i) == PrintEstimatedStatistics::ETimeMode::Normal ||
            m_time_processor.machine_envelope_processing_enabled) {
            if (line.has_value('S', value)) {
                set_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), value);
                set_travel_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), value);
                if (line.has_value('T', value))
                    set_retract_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), value);
            }
            else {
                if (line.has_value('P', value))
                    set_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), value);
                if (line.has_value('R', value))
                    set_retract_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), value);
                if (line.has_value('T', value))
                    set_travel_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), value);
            }
        }
    }
}

void GCodeProcessor::process_M205(const GCodeReader::GCodeLine& line)
{
    if (m_tool_kinematics)
        for (int axis = 0; axis < 2; ++axis)
            if (float value; line.has_value(m_tool_kinematics->rotary_axes()[axis].letter, value))
                m_rotary_jerk[axis] = std::max(0.f, value);

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (static_cast<PrintEstimatedStatistics::ETimeMode>(i) == PrintEstimatedStatistics::ETimeMode::Normal ||
            m_time_processor.machine_envelope_processing_enabled) {
            if (line.has_x()) {
                float max_jerk = line.x();
                set_option_value(m_time_processor.machine_limits.machine_max_jerk_x, i, max_jerk);
                set_option_value(m_time_processor.machine_limits.machine_max_jerk_y, i, max_jerk);
            }

            if (line.has_y())
                set_option_value(m_time_processor.machine_limits.machine_max_jerk_y, i, line.y());

            if (line.has_z())
                set_option_value(m_time_processor.machine_limits.machine_max_jerk_z, i, line.z());

            if (line.has_e())
                set_option_value(m_time_processor.machine_limits.machine_max_jerk_e, i, line.e());

            float value;
            if (line.has_value('S', value))
                set_option_value(m_time_processor.machine_limits.machine_min_extruding_rate, i, value);

            if (line.has_value('T', value))
                set_option_value(m_time_processor.machine_limits.machine_min_travel_rate, i, value);

            if (line.has_value('J', value))
                set_option_value(m_time_processor.machine_limits.machine_max_junction_deviation, i, value);
        }
    }
}

void GCodeProcessor::process_SET_VELOCITY_LIMIT(const GCodeReader::GCodeLine& line)
{
    static const std::regex square_corner_velocity_pattern("\\sSQUARE_CORNER_VELOCITY\\s*=\\s*([0-9]*\\.*[0-9]*)");
    std::smatch matches;
    if (std::regex_search(line.raw(), matches, square_corner_velocity_pattern) && matches.size() == 2) {
        float _jerk = 0;
        try
        {
            _jerk = std::stof(matches[1]);
        }
        catch (...){}
        for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
            set_option_value(m_time_processor.machine_limits.machine_max_jerk_x, i, _jerk);
            set_option_value(m_time_processor.machine_limits.machine_max_jerk_y, i, _jerk);
        }
    }

    static const std::regex accel_pattern("\\sACCEL\\s*=\\s*([0-9]*\\.*[0-9]*)");
    if (std::regex_search(line.raw(), matches, accel_pattern) && matches.size() == 2) {
        float _accl = 0;
        try
        {
            _accl = std::stof(matches[1]);
        }
        catch (...) {}
        for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
            set_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), _accl);
            set_travel_acceleration(static_cast<PrintEstimatedStatistics::ETimeMode>(i), _accl);
        }
    }

    static const std::regex velocity_pattern("\\sVELOCITY\\s*=\\s*([0-9]*\\.*[0-9]*)");
    if (std::regex_search(line.raw(), matches, velocity_pattern) && matches.size() == 2) {
        float _speed = 0;
        try
        {
            _speed = std::stof(matches[1]);
        }
        catch (...) {}
        for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
                set_option_value(m_time_processor.machine_limits.machine_max_speed_x, i, _speed);
                set_option_value(m_time_processor.machine_limits.machine_max_speed_y, i, _speed);
        }
    }

    static const std::regex cruise_ratio_pattern("\\sMINIMUM_CRUISE_RATIO\\s*=\\s*([0-9]*\\.*[0-9]*)");
    static const std::regex accel_to_decel_pattern("\\sACCEL_TO_DECEL\\s*=\\s*([0-9]*\\.*[0-9]*)");
    try {
        if (std::regex_search(line.raw(), matches, cruise_ratio_pattern) && matches.size() == 2)
            m_klipper_min_cruise_ratio = std::clamp(std::stof(matches[1]), 0.0f, 0.99f);
        else if (std::regex_search(line.raw(), matches, accel_to_decel_pattern) && matches.size() == 2) {
            const float accel = get_acceleration(PrintEstimatedStatistics::ETimeMode::Normal);
            if (accel > 0.0f)
                m_klipper_min_cruise_ratio = std::clamp(1.0f - std::stof(matches[1]) / accel, 0.0f, 0.99f);
        }
    } catch (...) {}
}

void GCodeProcessor::process_M221(const GCodeReader::GCodeLine& line)
{
    float value_s;
    float value_t;
    if (line.has_value('S', value_s) && !line.has_value('T', value_t)) {
        value_s *= 0.01f;
        for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
            m_time_processor.machines[i].extrude_factor_override_percentage = value_s;
        }
    }
}

void GCodeProcessor::process_M622(const GCodeReader::GCodeLine& line)
{
    float value_j;
    if(line.has_value('J',value_j)){
        int interger_j = (int)(std::round(value_j));
        if(interger_j == 1 && !m_measure_g29_time)
            m_measure_g29_time = true;
    }

}

void GCodeProcessor::process_M623(const GCodeReader::GCodeLine& line)
{
    if(m_measure_g29_time)
        m_measure_g29_time = false;
}


void GCodeProcessor::process_M400(const GCodeReader::GCodeLine& line)
{
    float value_s = 0.0;
    float value_p = 0.0;
    if (line.has_value('S', value_s) || line.has_value('P', value_p)) {
        value_s += value_p * 0.001;
        if (m_skip_end_gcode_delays)
            return;
        simulate_st_synchronize(value_s);
    }
}

void GCodeProcessor::process_M401(const GCodeReader::GCodeLine& line)
{
    if (m_flavor != gcfRepetier)
        return;

    for (unsigned char a = 0; a <= 3; ++a) {
        m_cached_position.position[a] = m_start_position[a];
    }
    m_cached_position.feedrate = m_feedrate;
}

void GCodeProcessor::process_M402(const GCodeReader::GCodeLine& line)
{
    if (m_flavor != gcfRepetier)
        return;

    bool has_xyz = !(line.has('X') || line.has('Y') || line.has('Z'));

    float p = FLT_MAX;
    for (unsigned char a = X; a <= Z; ++a) {
        if (has_xyz || line.has(a)) {
            p = m_cached_position.position[a];
            if (p != FLT_MAX)
                m_start_position[a] = p;
        }
    }

    p = m_cached_position.position[E];
    if (p != FLT_MAX)
        m_start_position[E] = p;

    p = FLT_MAX;
    if (!line.has_value(4, p))
        p = m_cached_position.feedrate;

    if (p != FLT_MAX)
        m_feedrate = p;
}

void GCodeProcessor::process_M566(const GCodeReader::GCodeLine& line)
{
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (line.has_x())
            set_option_value(m_time_processor.machine_limits.machine_max_jerk_x, i, line.x() * MMMIN_TO_MMSEC);

        if (line.has_y())
            set_option_value(m_time_processor.machine_limits.machine_max_jerk_y, i, line.y() * MMMIN_TO_MMSEC);

        if (line.has_z())
            set_option_value(m_time_processor.machine_limits.machine_max_jerk_z, i, line.z() * MMMIN_TO_MMSEC);

        if (line.has_e())
            set_option_value(m_time_processor.machine_limits.machine_max_jerk_e, i, line.e() * MMMIN_TO_MMSEC);
    }
}

void GCodeProcessor::process_M702(const GCodeReader::GCodeLine& line)
{
    int filament_id = get_filament_id();
    if (line.has('C')) {
        m_time_processor.extruder_unloaded = true;
        simulate_st_synchronize(get_filament_unload_time(filament_id));
    }
}


void GCodeProcessor::process_SYNC(const GCodeReader::GCodeLine& line)
{
    float time = 0;
    if (line.has_value('T', time) ) {
        simulate_st_synchronize(time);
    }
}


void GCodeProcessor::process_T(const GCodeReader::GCodeLine& line)
{
    int   nozzle_id = -1;
    float nozzle_val = 0.f;
    if (line.has_value('H', nozzle_val))
        nozzle_id = static_cast<int>(nozzle_val);
    process_T(line.cmd(), nozzle_id);
}

void GCodeProcessor::process_M1020(const GCodeReader::GCodeLine &line)
{
    int curr_filament_id = get_filament_id(false);
    int curr_extruder_id = get_extruder_id(false);
    if (line.raw().length() > 5) {
        std::string filament_id_str = line.raw().substr(7);
        if (filament_id_str.empty())
            return;

        int eid = 0;
        eid = std::stoi(filament_id_str);
        if (eid < 0 || eid > 254) {
            if ((m_flavor != gcfRepRapFirmware && m_flavor != gcfRepRapSprinter) || eid != -1)
                BOOST_LOG_TRIVIAL(error) << "Invalid M1020 command (" << line.raw() << ").";
        }
        else {
            if (eid >= m_result.filaments_count) {
                BOOST_LOG_TRIVIAL(error) << "Invalid M1020 command (" << line.raw() << ").";
                return;
            }
            int   nozzle_id = -1;
            float nozzle_val = 0.f;
            if (line.has_value('H', nozzle_val))
                nozzle_id = static_cast<int>(nozzle_val);
            process_filament_change(eid, nozzle_id);
        }
    }
}

void GCodeProcessor::process_T(const std::string_view command)
{
    process_T(command, -1);
}

void GCodeProcessor::process_T(const std::string_view command, int nozzle_id)
{
    unsigned int eid = 0;
    auto         ret          = std::from_chars(command.data() + 1, command.data()+command.size(), eid);
    if (std::errc::invalid_argument == ret.ec)
        return;

    int curr_filament_id = get_filament_id(false);
    int curr_extruder_id = get_extruder_id(false);
    if (command.length() > 1) {
        if (eid < 0 || eid > 254) {
            if ((m_flavor == gcfMarlinLegacy || m_flavor == gcfMarlinFirmware) && (command == "Tx" || command == "Tc" || command == "T?" ||
                 eid == 1000 || eid == 1100 || eid == 255 || eid == 1001 || eid == 65279 || eid == 65535))
                return;

            if ((m_flavor != gcfRepRapFirmware && m_flavor != gcfRepRapSprinter) || eid != -1)
                BOOST_LOG_TRIVIAL(error) << "Invalid T command (" << command << ").";
        }
        else {
            if (eid >= m_result.filaments_count) {
                BOOST_LOG_TRIVIAL(error) << "Invalid T command (" << command << ").";
                return;
            }
            process_filament_change(eid, nozzle_id);
        }
    }
}


void GCodeProcessor::init_filament_maps_and_nozzle_type_when_import_only_gcode()
{
    if (m_filament_maps.empty()) {
        m_filament_maps.assign((int) EnforcerBlockerType::ExtruderMax, 1);
    }
    if (m_result.nozzle_type.empty()) {
        m_result.nozzle_type.assign((int) EnforcerBlockerType::ExtruderMax, NozzleType::ntUndefine);
    }
}

void GCodeProcessor::ensure_nozzle_group_result(int min_filament_count)
{
    if (m_nozzle_group_result) {
        m_result.nozzle_group_result = m_nozzle_group_result;
        return;
    }

    int filament_count = std::max(1, min_filament_count);
    filament_count = std::max(filament_count, static_cast<int>(m_filament_maps.size()));

    std::vector<int> filament_map = m_filament_maps;
    if (filament_map.empty()) {
        filament_map.assign(filament_count, 0);
    } else if (static_cast<int>(filament_map.size()) < filament_count) {
        filament_map.resize(filament_count, filament_map.front());
    }

    int min_value = *std::min_element(filament_map.begin(), filament_map.end());
    if (min_value >= 1) {
        for (int &value : filament_map) {
            value -= 1;
        }
    }

    for (int &value : filament_map) {
        value = std::max(0, value);
    }

    int max_extruder_id = *std::max_element(filament_map.begin(), filament_map.end());
    max_extruder_id = std::max(0, max_extruder_id);

    std::string nozzle_diameter = format_diameter_to_str(DEFAULT_TOOLPATH_WIDTH);
    std::vector<MultiNozzleUtils::NozzleInfo> nozzle_list;
    nozzle_list.reserve(static_cast<size_t>(max_extruder_id + 1));
    for (int extruder_id = 0; extruder_id <= max_extruder_id; ++extruder_id) {
        MultiNozzleUtils::NozzleInfo info;
        info.diameter    = nozzle_diameter;
        info.volume_type = NozzleVolumeType::nvtStandard;
        info.extruder_id = extruder_id;
        info.group_id    = extruder_id;
        nozzle_list.emplace_back(std::move(info));
    }

    std::vector<int> filament_nozzle_map(filament_count, 0);
    for (int i = 0; i < filament_count; ++i) {
        filament_nozzle_map[i] = filament_map[i];
    }

    std::vector<unsigned int> used_filaments;
    used_filaments.reserve(filament_count);
    for (int i = 0; i < filament_count; ++i) {
        used_filaments.push_back(static_cast<unsigned int>(i));
    }

    auto result = MultiNozzleUtils::LayeredNozzleGroupResult::create(filament_nozzle_map, nozzle_list, used_filaments);
    if (result) {
        m_nozzle_group_result        = std::make_shared<MultiNozzleUtils::LayeredNozzleGroupResult>(*result);
        m_result.nozzle_group_result = m_nozzle_group_result;
    }
}

bool GCodeProcessor::use_multi_nozzle_change_time_model() const
{
    if (m_nozzle_diameter.size() > 1)
        return true;
    for (int count : m_extruder_max_nozzle_count)
        if (count > 1)
            return true;
    return false;
}

std::optional<MultiNozzleUtils::NozzleInfo> GCodeProcessor::resolve_target_nozzle(
    const MultiNozzleUtils::NozzleGroupResultBase &group, int id, int nozzle_id) const
{
    std::optional<MultiNozzleUtils::NozzleInfo> info;
    if (nozzle_id != -1)
        info = group.get_nozzle_from_id(nozzle_id);
    if (!info) {
        auto used_nozzles = group.get_nozzles_for_filament(id);
        if (!used_nozzles.empty())
            info = used_nozzles.front();
    }
    return info;
}

void GCodeProcessor::process_filament_change(int id, int nozzle_id)
{
    if (!use_multi_nozzle_change_time_model() || !m_result.nozzle_group_result) {
        if (m_nozzle_group_result) {
            if (auto info = resolve_target_nozzle(*m_nozzle_group_result, id, nozzle_id))
                m_nozzle_status_recorder.set_nozzle_status(info->group_id, id, info->extruder_id);
        }
        process_filament_change(id);
        return;
    }

    assert(id < m_result.filaments_count);
    const int prev_extruder_id = get_extruder_id(false);
    const int prev_filament_id = get_filament_id(false);
    float     extra_time       = 0.0f;

    if (prev_filament_id == id && nozzle_id == -1)
        return;

    if (prev_extruder_id != -1)
        m_last_filament_id[prev_extruder_id] = static_cast<unsigned char>(prev_filament_id);

    std::optional<MultiNozzleUtils::NozzleInfo> target_nozzle_info = resolve_target_nozzle(*m_result.nozzle_group_result, id, nozzle_id);
    if (!target_nozzle_info)
        return;

    const int new_extruder_id          = target_nozzle_info->extruder_id;
    const int old_extruder_id          = prev_extruder_id;
    const int new_nozzle_id_in_extruder = target_nozzle_info->group_id;
    const int old_nozzle_id_in_extruder = m_nozzle_status_recorder.get_nozzle_in_extruder(new_extruder_id);
    const int new_filament_id          = id;
    const int old_filament_in_nozzle   = m_nozzle_status_recorder.get_filament_in_nozzle(new_nozzle_id_in_extruder);
    const int old_filament_in_extruder = m_nozzle_status_recorder.get_filament_in_nozzle(old_nozzle_id_in_extruder);

    const bool extruder_change          = new_extruder_id != old_extruder_id;
    const bool nozzle_in_extruder_change = new_nozzle_id_in_extruder != old_nozzle_id_in_extruder;
    const bool filament_in_nozzle_change = new_filament_id != old_filament_in_nozzle;

    if (prev_extruder_id != -1)
        process_filaments(CustomGCode::ToolChange);

    m_result.lock();
    if (extruder_change && old_extruder_id != -1) {
        const float t = get_extruder_change_time(new_extruder_id);
        extra_time += t;
        m_result.print_statistics.total_tool_change_time += t;
        m_result.print_statistics.total_extruder_changes += 1;
    }
    if (nozzle_in_extruder_change || filament_in_nozzle_change) {
        if (old_filament_in_extruder >= 0) {
            const float t = get_filament_unload_time(static_cast<size_t>(old_filament_in_extruder));
            extra_time += t;
            m_result.print_statistics.total_filament_unload_time += t;
        }
        m_time_processor.extruder_unloaded = false;
        const float t = get_filament_load_time(static_cast<size_t>(new_filament_id));
        extra_time += t;
        m_result.print_statistics.total_filament_load_time += t;
        if (filament_in_nozzle_change && old_filament_in_nozzle != -1)
            m_result.print_statistics.total_flush_filament_changes += 1;
    }
    if (prev_filament_id != -1)
        m_result.print_statistics.total_filament_changes += 1;
    m_result.unlock();

    if (new_extruder_id != -1)
        m_filament_id[new_extruder_id] = static_cast<unsigned char>(new_filament_id);
    m_extruder_id = static_cast<unsigned char>(new_extruder_id);

    m_nozzle_status_recorder.set_nozzle_status(new_nozzle_id_in_extruder, new_filament_id, new_extruder_id);

    m_cp_color.current = m_extruder_colors[new_filament_id];

    store_move_vertex(EMoveType::Tool_change);

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        TimeMachine& machine = m_time_processor.machines[i];
        if (!machine.enabled)
            continue;
        TimeBlock block;
        block.move_id             = static_cast<unsigned int>(m_result.moves.size()) - 1;
        block.move_type           = EMoveType::Tool_change;
        block.skippable_type      = m_skippable_type;
        block.layer_id            = std::max<unsigned int>(1, m_layer_id);
        block.g1_line_id          = m_g1_line_id;
        block.flags.prepare_stage = m_processing_start_custom_gcode;
        block.flags.nonplanar = m_nonplanar_patch_shell != 0xFF;
        block.feed_interval = m_nonplanar_feed_interval;
        block.distance            = 0.0f;
        block.calculate_trapezoid();
        machine.blocks.push_back(block);
    }

    simulate_st_synchronize(extra_time, EMoveType::Tool_change);

    m_machine_config_idx = get_machine_config_idx();
}

void GCodeProcessor::process_filament_change(int id)
{
    assert(id < m_result.filaments_count);
    int prev_extruder_id = get_extruder_id(false);
    int prev_filament_id = get_filament_id(false);
    int next_extruder_id = m_filament_maps[id];
    int next_filament_id = id;
    float extra_time = 0;
    unsigned int filament_changes_delta = 0;
    unsigned int extruder_changes_delta = 0;
    float filament_load_time_delta = 0.0f;
    float filament_unload_time_delta = 0.0f;
    float tool_change_time_delta = 0.0f;

    if (prev_filament_id == next_filament_id)
        return;

    if (prev_extruder_id != -1)
        m_last_filament_id[prev_extruder_id] = prev_filament_id;

    if (prev_extruder_id == next_extruder_id) {
        assert(prev_extruder_id != -1);
        process_filaments(CustomGCode::ToolChange);
        m_filament_id[next_extruder_id] = next_filament_id;
        filament_changes_delta += 1;
        const float filament_unload_time = get_filament_unload_time(static_cast<size_t>(prev_filament_id));
        extra_time += filament_unload_time;
        filament_unload_time_delta += filament_unload_time;
        m_time_processor.extruder_unloaded = false;
        const float filament_load_time = get_filament_load_time(static_cast<size_t>(next_filament_id));
        extra_time += filament_load_time;
        filament_load_time_delta += filament_load_time;
    }
    else {
        if (prev_extruder_id == -1) {
            m_extruder_id = next_extruder_id;
            m_filament_id[next_extruder_id] = next_filament_id;
            m_time_processor.extruder_unloaded = false;
            const float filament_load_time = get_filament_load_time(static_cast<size_t>(next_filament_id));
            extra_time += filament_load_time;
            filament_load_time_delta += filament_load_time;
        }
        else {
            process_filaments(CustomGCode::ToolChange);
            m_extruder_id = next_extruder_id;
            if (m_last_filament_id[next_extruder_id] == (unsigned char)(-1)) {
                m_filament_id[next_extruder_id] = next_filament_id;
                m_time_processor.extruder_unloaded = false;
                const float filament_load_time = get_filament_load_time(static_cast<size_t>(next_filament_id));
                extra_time += filament_load_time;
                filament_load_time_delta += filament_load_time;
            }
            else if (m_last_filament_id[next_extruder_id] != next_filament_id) {
                m_filament_id[next_extruder_id] = next_filament_id;
                filament_changes_delta += 1;
                const float filament_unload_time = get_filament_unload_time(static_cast<size_t>(prev_filament_id));
                extra_time += filament_unload_time;
                filament_unload_time_delta += filament_unload_time;
                m_time_processor.extruder_unloaded = false;
                const float filament_load_time = get_filament_load_time(static_cast<size_t>(next_filament_id));
                extra_time += filament_load_time;
                filament_load_time_delta += filament_load_time;
            }
            extruder_changes_delta += 1;
            const float tool_change_time = get_extruder_change_time(next_extruder_id);
            extra_time += tool_change_time;
            tool_change_time_delta += tool_change_time;
        }
    }

    if (filament_changes_delta > 0 || extruder_changes_delta > 0 || filament_load_time_delta > 0.0f || filament_unload_time_delta > 0.0f || tool_change_time_delta > 0.0f) {
        m_result.lock();
        m_result.print_statistics.total_filament_changes += filament_changes_delta;
        m_result.print_statistics.total_extruder_changes += extruder_changes_delta;
        m_result.print_statistics.total_filament_load_time += filament_load_time_delta;
        m_result.print_statistics.total_filament_unload_time += filament_unload_time_delta;
        m_result.print_statistics.total_tool_change_time += tool_change_time_delta;
        m_result.unlock();
    }

    m_cp_color.current = m_extruder_colors[next_filament_id];

    store_move_vertex(EMoveType::Tool_change);

    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        TimeMachine& machine = m_time_processor.machines[i];
        if (!machine.enabled)
            continue;
        TimeBlock block;
        block.move_id             = static_cast<unsigned int>(m_result.moves.size()) - 1;
        block.move_type           = EMoveType::Tool_change;
        block.skippable_type      = m_skippable_type;
        block.layer_id            = std::max<unsigned int>(1, m_layer_id);
        block.g1_line_id          = m_g1_line_id;
        block.flags.prepare_stage = m_processing_start_custom_gcode;
        block.flags.nonplanar = m_nonplanar_patch_shell != 0xFF;
        block.feed_interval = m_nonplanar_feed_interval;
        block.distance            = 0.0f;
        block.calculate_trapezoid();
        machine.blocks.push_back(block);
    }

    simulate_st_synchronize(extra_time, EMoveType::Tool_change);

    m_machine_config_idx = get_machine_config_idx();
}

void GCodeProcessor::set_print(Print* print)
{
    m_print = print;
    m_keep_tips = print != nullptr && std::any_of(print->objects().begin(), print->objects().end(), [](const PrintObject* object) {
        return object->config().nonplanar_mode.value != NonplanarMode::Off;
    });
    m_marlin_junctions = print != nullptr && (Nonplanar::requests_conformal_shells(*print) ||
                                              Nonplanar::curved_planar_measurement() ||
                                              std::any_of(print->objects().begin(), print->objects().end(), [](const PrintObject *object) {
                                                  return curved_tops_mode(object->config().nonplanar_mode.value);
                                              }) ||
                                              print->config().nonplanar_kinematics.value != RotaryLayout::None);
}

unsigned int GCodeProcessor::intern_move_state(const GCodeProcessorResult::MoveState& state)
{
    const std::vector<GCodeProcessorResult::MoveState>& states = m_result.move_states;
    if (states[m_recent_move_states[0]] == state)
        return m_recent_move_states[0];
    std::swap(m_recent_move_states[0], m_recent_move_states[1]);
    if (states[m_recent_move_states[0]] == state)
        return m_recent_move_states[0];
    const auto [it, inserted] = m_move_state_ids.try_emplace(state, static_cast<unsigned int>(states.size()));
    if (inserted)
        m_result.move_states.push_back(state);
    return m_recent_move_states[0] = it->second;
}

void GCodeProcessor::store_move_vertex(EMoveType type, EMovePathType path_type, bool internal_only)
{
    int filament_id = get_filament_id();
    const auto normal_mode = PrintEstimatedStatistics::ETimeMode::Normal;
    const size_t normal_mode_id = static_cast<size_t>(normal_mode);
    const float delta_x = std::abs(m_end_position[X] - m_start_position[X]);
    const float delta_y = std::abs(m_end_position[Y] - m_start_position[Y]);
    const float delta_z = std::abs(m_end_position[Z] - m_start_position[Z]);
    const float delta_e = std::abs(m_end_position[E] - m_start_position[E]);
    const bool has_x = delta_x > 0.0f;
    const bool has_y = delta_y > 0.0f;
    const bool has_z = delta_z > 0.0f;
    const bool has_e = delta_e > 0.0f;
    const float move_acceleration =
        (type == EMoveType::Travel) ? get_travel_acceleration(normal_mode) :
        ((type == EMoveType::Retract || type == EMoveType::Unretract) ? get_retract_acceleration(normal_mode) :
                                                                    get_acceleration(normal_mode));
    const float junction_deviation = get_option_value(m_time_processor.machine_limits.machine_max_junction_deviation, normal_mode_id);
    const bool use_jd_jerk = (m_flavor == gcfMarlinFirmware && junction_deviation > 0.0f);
    const auto axis_jerk_for_preview = [this, use_jd_jerk, move_acceleration](Axis axis) {
        return use_jd_jerk ? get_axis_max_jerk_with_jd(normal_mode, axis, move_acceleration) : get_axis_max_jerk(normal_mode, axis);
    };
    const float jerk_x = axis_jerk_for_preview(X);
    const float jerk_y = axis_jerk_for_preview(Y);
    const float jerk_z = axis_jerk_for_preview(Z);
    const float jerk_e = axis_jerk_for_preview(E);
    const float move_jerk =
        (has_e && !has_x && !has_y && !has_z) ? jerk_e :
        (has_z && !has_x && !has_y) ? jerk_z :
        (has_x && has_y) ? std::min(jerk_x, jerk_y) :
        has_x ? jerk_x :
        has_y ? jerk_y :
        has_z ? jerk_z :
                std::min(jerk_x, jerk_y);
    m_last_line_id = (type == EMoveType::Color_change || type == EMoveType::Pause_Print || type == EMoveType::Custom_GCode) ?
        m_line_id + 1 :
        ((type == EMoveType::Seam) ? m_last_line_id : m_line_id);

    if (type == EMoveType::Travel) {
        m_result.print_statistics.total_travel_moves++;
        m_result.print_statistics.total_travel_distance += m_travel_dist;
    }

    const float store_z = (m_processing_start_custom_gcode && m_result.belt_tilt_angle == 0.f)
        ? m_first_layer_height
        : m_end_position[Z] - m_z_offset;

    GCodeProcessorResult::MoveState state;
    state.fan_speed = m_fan_speed;
    state.temperature = m_extruder_temps[filament_id];
    state.pressure_advance = m_pressure_advance;
    state.acceleration = move_acceleration;
    state.jerk = move_jerk;
    state.print_z = m_print_z;
    state.object_label_id = m_object_label_id;
    state.nonplanar_patch_shell = m_nonplanar_patch_shell;
    state.nonplanar_band = m_nonplanar_band;
    state.nonplanar_patch = m_nonplanar_patch;
    state.wave_object = m_wave_object;
    state.native_road_id = m_native_road_id;

    m_result.moves.push_back({
        m_last_line_id,
        type,
        m_extrusion_role,
        static_cast<unsigned char>(filament_id),
        m_cp_color.current,
        Vec3f(m_end_position[X] + m_x_offset, m_end_position[Y] + m_y_offset, store_z) + m_extruder_offsets[filament_id],
        static_cast<float>(m_end_position[E] - m_start_position[E]),
        m_feedrate,
        0.0f,
        m_width,
        m_height,
        m_mm3_per_mm,
        { 0.0f, 0.0f },
        std::max<unsigned int>(1, m_layer_id) - 1,
        intern_move_state(state),
        internal_only
    });
    m_result.moves.back().clearance_pose_known = !m_tool_kinematics || m_rotary_position_known_axes == 7;
    if (keep_joints()) {
        m_result.joints.resize(m_result.moves.size() - 1);
        m_result.joints.push_back(m_joints);
        if (!m_tool_kinematics)
            m_result.joints.back().xyz = Vec3d(m_end_position[X], m_end_position[Y], m_end_position[Z]);
    }
    if (!internal_only)
        m_result.move_type_distances[size_t(type)] += (type == EMoveType::Retract || type == EMoveType::Unretract) ?
            std::fabs(m_result.moves.back().delta_extruder) : m_travel_dist;

    if (type == EMoveType::Seam) {
        m_seams_count++;
    }

    if (type == EMoveType::Color_change || type == EMoveType::Pause_Print) {
        for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
            TimeMachine& machine = m_time_processor.machines[i];
            if (!machine.enabled)
                continue;

            machine.stop_times.push_back({ m_g1_line_id, 0.0f });
        }
    }
}

void GCodeProcessorResult::ObjectMass::add(const Sum &sum, const BoundingBoxf3 &extent, size_t layer)
{
    box.merge(extent);
    if (printed_up_to_layer.size() <= layer)
        printed_up_to_layer.resize(layer + 1);
    printed_up_to_layer[layer].add(sum);
}

void GCodeProcessor::add_object_mass(int filament_id, float volume)
{
    const ExtrusionRole role = m_extrusion_role;
    if (volume <= 0.f || role == erNone || role == erSkirt || role == erWipeTower || role == erCustom || role == erMixed)
        return;

    const bool   has_density = size_t(filament_id) < m_result.filament_densities.size() && m_result.filament_densities[filament_id] > 0.f;
    const double mass        = double(volume) * (has_density ? m_result.filament_densities[filament_id] : DEFAULT_FILAMENT_DENSITY);
    const Vec3d half_height = 0.5 * double(m_height) * Vec3d::UnitZ();
    const Vec3d offset      = Vec3d(m_x_offset, m_y_offset, -m_z_offset) - half_height + m_extruder_offsets[filament_id].cast<double>();
    const Vec3d start       = Vec3d(m_start_position[X], m_start_position[Y], m_start_position[Z]) + offset;
    const Vec3d end         = Vec3d(m_end_position[X], m_end_position[Y], m_end_position[Z]) + offset;
    const GCodeProcessorResult::ObjectMass::Sum sum{ mass, double(volume), 0.5 * mass * (start + end),
                                                     mass / 3. * (start.cwiseProduct(start) + start.cwiseProduct(end) + end.cwiseProduct(end)) };
    BoundingBoxf3 extent;
    extent.merge(start.cwiseMin(end) - half_height);
    extent.merge(start.cwiseMax(end) + half_height);
    const bool   part  = role != erBrim && !is_support(role);
    const size_t layer = std::max<unsigned int>(1, m_layer_id) - 1;

    m_result.plate_mass.add(sum, extent, layer);
    if (role == erBrim || !m_mass_locator)
        return;
    const auto add = [&sum, &extent, layer](std::vector<GCodeProcessorResult::ObjectMass> &masses, int index) {
        if (index < 0)
            return;
        if (masses.size() <= size_t(index))
            masses.resize(index + 1);
        masses[index].add(sum, extent, layer);
    };
    const MassLocation location = m_mass_locator(0.5 * (start + end) + half_height, !part);
    if (part) {
        add(m_result.object_masses, location.object);
        add(m_result.body_masses, location.body);
    } else
        add(m_result.support_masses, location.object);
}

void GCodeProcessor::finalize_object_masses()
{
    const auto accumulate = [](GCodeProcessorResult::ObjectMass &object) {
        for (size_t i = 1; i < object.printed_up_to_layer.size(); ++i)
            object.printed_up_to_layer[i].add(object.printed_up_to_layer[i - 1]);
    };
    accumulate(m_result.plate_mass);
    for (GCodeProcessorResult::ObjectMass &object : m_result.object_masses)
        accumulate(object);
    for (GCodeProcessorResult::ObjectMass &body : m_result.body_masses)
        accumulate(body);
    for (GCodeProcessorResult::ObjectMass &support : m_result.support_masses)
        accumulate(support);
}

void GCodeProcessor::set_extrusion_role(ExtrusionRole role)
{
    m_used_filaments.process_role_cache(this);
    m_extrusion_role = role;
}

void GCodeProcessor::set_skippable_type(const std::string_view type)
{
    if (!m_skippable) {
        m_skippable_type = SkipType::stNone;
        return;
    }
    auto iter = skip_type_map.find(type);
    if (iter != skip_type_map.end()) {
        m_skippable_type = iter->second;
    } else {
        m_skippable_type = SkipType::stOther;
    }
}

float GCodeProcessor::minimum_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const
{
    if (m_time_processor.machine_limits.machine_min_extruding_rate.empty())
        return feedrate;

    return std::max(feedrate, get_option_value(m_time_processor.machine_limits.machine_min_extruding_rate, static_cast<size_t>(mode)));
}

float GCodeProcessor::minimum_travel_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const
{
    if (m_time_processor.machine_limits.machine_min_travel_rate.empty())
        return feedrate;

    return std::max(feedrate, get_option_value(m_time_processor.machine_limits.machine_min_travel_rate, static_cast<size_t>(mode)));
}

int GCodeProcessor::get_machine_config_idx() const
{
    const int extruder_id = get_extruder_id(false);
    if (!m_nozzle_group_result || extruder_id < 0)
        return 0;
    const int nozzle_id = m_nozzle_status_recorder.get_nozzle_in_extruder(extruder_id);
    auto nozzle_info = m_nozzle_group_result->get_nozzle_from_id(nozzle_id);
    if (!nozzle_info || extruder_id >= (int) m_result.extruder_types.size())
        return 0;
    return std::max(0, get_config_index_base(nozzle_info->volume_type, m_result.extruder_types[extruder_id],
                                             extruder_id + 1, m_result.printer_extruder_variant,
                                             m_result.printer_extruder_id));
}

float GCodeProcessor::get_axis_max_feedrate(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const
{
    return get_axis_max_feedrate(mode, axis, 0);
}

float GCodeProcessor::get_axis_max_feedrate(PrintEstimatedStatistics::ETimeMode mode, Axis axis, int machine_idx) const
{
    const size_t pos = static_cast<size_t>(machine_idx) * 2 + static_cast<size_t>(mode);
    switch (axis)
    {
    case X: { return get_option_value(m_time_processor.machine_limits.machine_max_speed_x, pos); }
    case Y: { return get_option_value(m_time_processor.machine_limits.machine_max_speed_y, pos); }
    case Z: { return get_option_value(m_time_processor.machine_limits.machine_max_speed_z, pos); }
    case E: { return get_option_value(m_time_processor.machine_limits.machine_max_speed_e, pos); }
    default: { return 0.0f; }
    }
}

float GCodeProcessor::get_axis_max_acceleration(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const
{
    return get_axis_max_acceleration(mode, axis, 0);
}

float GCodeProcessor::get_axis_max_acceleration(PrintEstimatedStatistics::ETimeMode mode, Axis axis, int machine_idx) const
{
    const size_t pos = static_cast<size_t>(machine_idx) * 2 + static_cast<size_t>(mode);
    switch (axis)
    {
    case X: { return get_option_value(m_time_processor.machine_limits.machine_max_acceleration_x, pos); }
    case Y: { return get_option_value(m_time_processor.machine_limits.machine_max_acceleration_y, pos); }
    case Z: { return get_option_value(m_time_processor.machine_limits.machine_max_acceleration_z, pos); }
    case E: { return get_option_value(m_time_processor.machine_limits.machine_max_acceleration_e, pos); }
    default: { return 0.0f; }
    }
}

float GCodeProcessor::get_axis_max_jerk_with_jd(PrintEstimatedStatistics::ETimeMode mode, Axis axis, float acceleration) const
{
    if (axis != X && axis != Y && axis != Z && axis != E)
        return 0.0f;

    const size_t id = static_cast<size_t>(mode);
    const float jd = get_option_value(m_time_processor.machine_limits.machine_max_junction_deviation, id);
    if (jd <= 0.0f)
        return 0.0f;

    const float axis_max_acc = get_axis_max_acceleration(mode, axis);
    float effective_acc = acceleration;
    if (effective_acc <= 0.0f)
        effective_acc = get_acceleration(mode);
    if (axis_max_acc > 0.0f)
        effective_acc = effective_acc > 0.0f ? std::min(effective_acc, axis_max_acc) : axis_max_acc;
    if (effective_acc <= 0.0f)
        return 0.0f;

    return std::sqrt(jd * effective_acc * 2.5f);
}

float GCodeProcessor::get_axis_max_jerk_with_jd(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const
{
    return get_axis_max_jerk_with_jd(mode, axis, get_acceleration(mode));
}

float GCodeProcessor::get_junction_deviation(PrintEstimatedStatistics::ETimeMode mode, float acceleration) const
{
    const size_t id = static_cast<size_t>(mode);

    if (m_flavor == gcfKlipper) {
        const float scv = get_option_value(m_time_processor.machine_limits.machine_max_jerk_x, id);
        if (scv <= 0.0f || acceleration <= 0.0f)
            return 0.0f;
        return sqr(scv) * (std::sqrt(2.0f) - 1.0f) / acceleration;
    }

    if (m_flavor == gcfMarlinFirmware)
        return get_option_value(m_time_processor.machine_limits.machine_max_junction_deviation, id);

    return 0.0f;
}

float GCodeProcessor::calc_junction_acceleration(const TimeBlock& block, const Vec4f& junction_unit_vec,
                                                 PrintEstimatedStatistics::ETimeMode mode) const
{
    float junction_acceleration = block.acceleration;
    for (unsigned char a = X; a <= E; ++a) {
        if (junction_unit_vec[a] == 0.0f)
            continue;
        const float axis_max_acceleration = get_axis_max_acceleration(mode, static_cast<Axis>(a), m_machine_config_idx);
        if (axis_max_acceleration > 0.0f)
            junction_acceleration = std::min(junction_acceleration, std::abs(axis_max_acceleration / junction_unit_vec[a]));
    }
    return junction_acceleration;
}

static constexpr float KlipperExtruderCornerVelocity = 1.0f;

float GCodeProcessor::calc_vmax_junction_deviation(const TimeBlock& block, const TimeMachine::State& prev,
                                                   const TimeMachine::State& curr, bool has_prev_move,
                                                   PrintEstimatedStatistics::ETimeMode mode,
                                                   const TimeBlock* prev_block) const
{
    const float junction_deviation = get_junction_deviation(mode, block.acceleration);
    if (junction_deviation <= 0.0f)
        return -1.0f;
    if (!has_prev_move)
        return 0.0f;
    float junction_cos_theta = (-prev.jd_unit_vec).dot(curr.jd_unit_vec);
    if (junction_cos_theta > 0.999999f)
        return 0.0f;
    junction_cos_theta = std::max(junction_cos_theta, -0.999999f);

    const float sin_theta_d2 = std::sqrt(0.5f * (1.0f - junction_cos_theta));
    const Vec4f junction_vec = curr.jd_unit_vec - prev.jd_unit_vec;
    const float junction_vec_norm = junction_vec.norm();
    const Vec4f junction_unit_vec = (junction_vec_norm > 0.0f) ? Vec4f(junction_vec / junction_vec_norm)
                                                               : Vec4f(0.0f, 0.0f, 0.0f, 0.0f);
    const float junction_acceleration = calc_junction_acceleration(block, junction_unit_vec, mode);

    float vmax_junction_sqr = (junction_acceleration * junction_deviation * sin_theta_d2) / (1.0f - sin_theta_d2);

    if (m_flavor == gcfKlipper && prev_block != nullptr) {
        const float scv = get_option_value(m_time_processor.machine_limits.machine_max_jerk_x, static_cast<size_t>(mode));
        const float toolhead_accel = std::max(get_acceleration(mode), block.acceleration);
        const float cos_theta_d2 = std::sqrt(0.5f * (1.0f + junction_cos_theta));
        if (scv > 0.0f && toolhead_accel > 0.0f && cos_theta_d2 > 0.0f) {
            const float jd     = sqr(scv) * (std::sqrt(2.0f) - 1.0f) / toolhead_accel;
            const float r_jd   = sin_theta_d2 / (1.0f - sin_theta_d2);
            const float accel  = std::min(block.acceleration, prev_block->acceleration);
            const float q      = 0.25f * sin_theta_d2 / cos_theta_d2;
            vmax_junction_sqr  = std::min({ r_jd * jd * accel, 2.0f * block.distance * block.acceleration * q,
                                            2.0f * prev_block->distance * prev_block->acceleration * q });
        }
        if (curr.feedrate > 0.0f && prev.feedrate > 0.0f) {
            const float diff_r = std::abs(curr.axis_feedrate[E] / curr.feedrate - prev.axis_feedrate[E] / prev.feedrate);
            if (diff_r > 0.0f)
                vmax_junction_sqr = std::min(vmax_junction_sqr, sqr(KlipperExtruderCornerVelocity / diff_r));
        }
    }

    if (m_flavor != gcfKlipper && block.distance < 1.0f && junction_cos_theta < -0.7071067812f) {
        const float neg = junction_cos_theta < 0.0f ? -1.0f : 1.0f;
        const float t = neg * junction_cos_theta;
        const float asinx = 0.032843707f + t * (-1.451838349f + t * (29.66153956f + t * (-131.1123477f +
                            t * (262.8130562f + t * (-242.7199627f + t * (84.31466202f))))));
        const float junction_theta = float(0.5 * M_PI) + neg * asinx;
        vmax_junction_sqr = std::min(vmax_junction_sqr, (block.distance * junction_acceleration) / junction_theta);
    }

    vmax_junction_sqr = std::min(vmax_junction_sqr, std::min(sqr(block.feedrate_profile.cruise), sqr(prev.feedrate)));
    return std::sqrt(vmax_junction_sqr);
}

float GCodeProcessor::get_axis_max_jerk(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const
{
    const size_t id = static_cast<size_t>(mode);
    const float jd = get_option_value(m_time_processor.machine_limits.machine_max_junction_deviation, id);
    if (m_flavor == gcfMarlinFirmware && jd > 0.0f) {
        return get_axis_max_jerk_with_jd(mode, axis);
    }

    switch (axis)
    {
    case X: { return get_option_value(m_time_processor.machine_limits.machine_max_jerk_x, id); }
    case Y: { return get_option_value(m_time_processor.machine_limits.machine_max_jerk_y, id); }
    case Z: { return get_option_value(m_time_processor.machine_limits.machine_max_jerk_z, id); }
    case E: { return get_option_value(m_time_processor.machine_limits.machine_max_jerk_e, id); }
    default: { return 0.0f; }
    }
}

Vec3f GCodeProcessor::get_xyz_max_jerk(PrintEstimatedStatistics::ETimeMode mode) const
{
    const size_t id = static_cast<size_t>(mode);
    float jx = 0.0f;
    float jy = 0.0f;
    float jz = 0.0f;
    const float jd = get_option_value(m_time_processor.machine_limits.machine_max_junction_deviation, id);

    if (m_flavor != gcfMarlinFirmware || jd <= 0.0f)
    {
        jx = get_option_value(m_time_processor.machine_limits.machine_max_jerk_x, id);
        jy = get_option_value(m_time_processor.machine_limits.machine_max_jerk_y, id);
        jz = get_option_value(m_time_processor.machine_limits.machine_max_jerk_z, id);
    }
    else
    {
        jx = get_axis_max_jerk_with_jd(mode, X);
        jy = get_axis_max_jerk_with_jd(mode, Y);
        jz = get_axis_max_jerk_with_jd(mode, Z);
    }

    return Vec3f(jx, jy, jz);
}

float GCodeProcessor::get_retract_acceleration(PrintEstimatedStatistics::ETimeMode mode) const
{
    size_t id = static_cast<size_t>(mode);
    return (id < m_time_processor.machines.size()) ? m_time_processor.machines[id].retract_acceleration : DEFAULT_RETRACT_ACCELERATION;
}

void GCodeProcessor::set_retract_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value)
{
    size_t id = static_cast<size_t>(mode);
    if (id < m_time_processor.machines.size()) {
        m_time_processor.machines[id].retract_acceleration = (m_time_processor.machines[id].max_retract_acceleration == 0.0f) ? value :
            std::min(value, m_time_processor.machines[id].max_retract_acceleration);
    }
}

float GCodeProcessor::get_acceleration(PrintEstimatedStatistics::ETimeMode mode) const
{
    size_t id = static_cast<size_t>(mode);
    return (id < m_time_processor.machines.size()) ? m_time_processor.machines[id].acceleration : DEFAULT_ACCELERATION;
}

void GCodeProcessor::set_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value)
{
    size_t id = static_cast<size_t>(mode);
    if (id < m_time_processor.machines.size()) {
        m_time_processor.machines[id].acceleration = (m_time_processor.machines[id].max_acceleration == 0.0f) ? value :
            std::min(value, m_time_processor.machines[id].max_acceleration);
    }
}

float GCodeProcessor::get_travel_acceleration(PrintEstimatedStatistics::ETimeMode mode) const
{
    size_t id = static_cast<size_t>(mode);
    return (id < m_time_processor.machines.size()) ? m_time_processor.machines[id].travel_acceleration : DEFAULT_TRAVEL_ACCELERATION;
}

void GCodeProcessor::set_travel_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value)
{
    size_t id = static_cast<size_t>(mode);
    if (id < m_time_processor.machines.size()) {
        m_time_processor.machines[id].travel_acceleration = (m_time_processor.machines[id].max_travel_acceleration == 0.0f) ? value :
            std::min(value, m_time_processor.machines[id].max_travel_acceleration);
    }
}

float GCodeProcessor::get_filament_load_time(size_t extruder_id)
{
    return m_time_processor.extruder_unloaded ? 0.0f : m_time_processor.filament_load_times;
}

float GCodeProcessor::get_filament_unload_time(size_t extruder_id)
{
    return m_time_processor.extruder_unloaded ? 0.0f : m_time_processor.filament_unload_times;
}

float GCodeProcessor::get_extruder_change_time(size_t extruder_id)
{
    return m_time_processor.machine_tool_change_time;
}

int GCodeProcessor::get_filament_vitrification_temperature(size_t extrude_id)
{
    if (extrude_id < m_result.filament_vitrification_temperature.size())
        return m_result.filament_vitrification_temperature[extrude_id];
    else
        return 0;
}

void GCodeProcessor::process_custom_gcode_time(CustomGCode::Type code)
{
    simulate_st_synchronize();
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        TimeMachine& machine = m_time_processor.machines[i];
        if (!machine.enabled)
            continue;

        TimeMachine::CustomGCodeTime& gcode_time = machine.gcode_time;
        gcode_time.needed = true;
        if (gcode_time.cache != 0.0f) {
            gcode_time.times.push_back({ code, gcode_time.cache });
            gcode_time.cache = 0.0f;
        }
    }
}

void GCodeProcessor::process_filaments(CustomGCode::Type code)
{
    if (code == CustomGCode::ColorChange)
        m_used_filaments.process_color_change_cache();

    if (code == CustomGCode::ToolChange) {
        m_used_filaments.process_model_cache(this);
        m_used_filaments.process_support_cache(this);
        m_used_filaments.process_total_volume_cache(this);
        size_t last_extruder_id = get_extruder_id();
        m_remaining_volume[last_extruder_id] = m_nozzle_volume[last_extruder_id];
    }
}

void GCodeProcessor::calculate_time(GCodeProcessorResult& result, size_t keep_last_n_blocks, float additional_time, EMoveType target_move_type, bool is_final)
{
    std::vector<TimeMachine::ActualSpeedMove> actual_speed_moves;
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        TimeMachine& machine = m_time_processor.machines[i];
        machine.calculate_time(m_result, static_cast<PrintEstimatedStatistics::ETimeMode>(i), keep_last_n_blocks, additional_time, target_move_type, is_final);
        if (static_cast<PrintEstimatedStatistics::ETimeMode>(i) == PrintEstimatedStatistics::ETimeMode::Normal)
            actual_speed_moves = std::move(machine.actual_speed_moves);
    }

    assert(&result == &m_result);
    GCodeProcessorResult::Moves& moves = result.moves;
    ChunkedVector<JointPose>& joints = result.joints;
    const bool with_joints = !joints.empty();
    size_t inserted_actual_speed_moves_count = 0;
    size_t kept                              = 0;
    size_t group_start                       = 0;
    for (size_t i = 0; i < actual_speed_moves.size(); ++i) {
        if (actual_speed_moves[i].position.has_value())
            continue;
        const unsigned int move_id = actual_speed_moves[i].move_id;
        if (move_id < moves.size() && (kept == 0 || move_id > actual_speed_moves[kept - 1].move_id)) {
            inserted_actual_speed_moves_count += i - group_start;
            moves[move_id].actual_feedrate = actual_speed_moves[i].actual_feedrate;
            if (move_id + 1 < moves.size() && moves[move_id + 1].type == EMoveType::Seam)
                moves[move_id + 1].actual_feedrate = actual_speed_moves[i].actual_feedrate;
            for (size_t j = group_start; j <= i; ++j, ++kept)
                if (kept != j)
                    actual_speed_moves[kept] = std::move(actual_speed_moves[j]);
        }
        group_start = i + 1;
    }
    actual_speed_moves.erase(actual_speed_moves.begin() + kept, actual_speed_moves.end());

    size_t read  = moves.size();
    moves.resize(moves.size() + inserted_actual_speed_moves_count);
    if (with_joints)
        joints.resize(moves.size());
    size_t write = moves.size();
    m_actual_speed_id_map.clear();
    size_t entry = actual_speed_moves.size();
    while (entry > 0) {
        const unsigned int block_id = actual_speed_moves[--entry].move_id;
        assert(block_id < read);
        while (read > block_id + 1) {
            moves[--write] = moves[--read];
            if (with_joints)
                joints[write] = joints[read];
        }
        const GCodeProcessorResult::MoveVertex block_move = moves[--read];
        const JointPose block_joints = with_joints ? joints[read] : JointPose();
        moves[--write] = block_move;
        if (with_joints)
            joints[write] = block_joints;
        m_actual_speed_id_map.emplace_back(block_id, (unsigned int)write);
        for (; entry > 0 && actual_speed_moves[entry - 1].position.has_value(); --entry) {
            const TimeMachine::ActualSpeedMove& it = actual_speed_moves[entry - 1];
            GCodeProcessorResult::MoveVertex new_move = block_move;
            new_move.time = { 0.0f, 0.0f };
            new_move.position = *it.position;
            new_move.actual_feedrate = it.actual_feedrate;
            new_move.delta_extruder = *it.delta_extruder;
            new_move.feedrate = *it.feedrate;
            new_move.width = *it.width;
            new_move.height = *it.height;
            new_move.mm3_per_mm = *it.mm3_per_mm;
            GCodeProcessorResult::MoveState state = result.state(block_move);
            state.fan_speed = *it.fan_speed;
            state.temperature = *it.temperature;
            new_move.state = intern_move_state(state);
            new_move.internal_only = true;
            new_move.timing_only = true;
            moves[--write] = new_move;
            if (with_joints)
                joints[write] = block_joints;
        }
    }
    assert(read == write);

    std::reverse(m_actual_speed_id_map.begin(), m_actual_speed_id_map.end());
    const auto remap_move_id = [&](size_t id) {
        auto it = std::lower_bound(m_actual_speed_id_map.begin(), m_actual_speed_id_map.end(), id,
                                   [](const std::pair<unsigned int, unsigned int>& entry, size_t value) { return entry.first < value; });
        if (it != m_actual_speed_id_map.end() && it->first == id)
            return size_t(it->second);
        return it == m_actual_speed_id_map.begin() ? id : id + (it - 1)->second - (it - 1)->first;
    };
    if (const auto id = m_options_z_corrector.move_id())
        m_options_z_corrector.remap_move_id(remap_move_id(*id));
    for (size_t i = 0; i < static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        for (GCodeProcessor::TimeBlock& block : m_time_processor.machines[i].blocks) {
            auto it = std::lower_bound(m_actual_speed_id_map.begin(), m_actual_speed_id_map.end(), block.move_id,
                                       [](const std::pair<unsigned int, unsigned int>& entry, unsigned int id) { return entry.first < id; });
            block.move_id = (it != m_actual_speed_id_map.end() && it->first == block.move_id) ?
                                it->second : block.move_id + (unsigned int)inserted_actual_speed_moves_count;
        }
    }
}

void GCodeProcessor::simulate_st_synchronize(float additional_time, EMoveType target_move_type)
{
    calculate_time(m_result, 0, additional_time, target_move_type);
}

void GCodeProcessor::update_estimated_times_stats()
{
    auto update_mode = [this](PrintEstimatedStatistics::ETimeMode mode) {
        PrintEstimatedStatistics::Mode& data = m_result.print_statistics.modes[static_cast<size_t>(mode)];
        data.time = get_time(mode);
        data.prepare_time = get_prepare_time(mode);
        data.custom_gcode_times = get_custom_gcode_times(mode, true);
    };

    update_mode(PrintEstimatedStatistics::ETimeMode::Normal);
    if (m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].enabled)
        update_mode(PrintEstimatedStatistics::ETimeMode::Stealth);
    else
        m_result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].reset();

    m_result.print_statistics.volumes_per_color_change  = m_used_filaments.volumes_per_color_change;
    m_result.print_statistics.model_volumes_per_extruder      = m_used_filaments.model_volumes_per_filament;
    m_result.print_statistics.wipe_tower_volumes_per_extruder = m_used_filaments.wipe_tower_volumes_per_filament;
    m_result.print_statistics.support_volumes_per_extruder = m_used_filaments.support_volumes_per_filament;
    m_result.print_statistics.flush_per_filament      = m_used_filaments.flush_per_filament;
    m_result.print_statistics.used_filaments_per_role   = m_used_filaments.filaments_per_role;
    m_result.print_statistics.total_volumes_per_extruder = m_used_filaments.total_volumes_per_filament;
}

double GCodeProcessor::extract_absolute_position_on_axis(Axis axis, const GCodeReader::GCodeLine& line, double area_filament_cross_section)
{
    if (line.has(Slic3r::Axis(axis))) {
        bool is_relative = (m_global_positioning_type == EPositioningType::Relative);
        if (axis == E)
            is_relative |= (m_e_local_positioning_type == EPositioningType::Relative);

        const double lengthsScaleFactor = (m_units == EUnits::Inches) ? double(INCHES_TO_MM) : 1.0;
        double ret = line.value(Slic3r::Axis(axis)) * lengthsScaleFactor;
        return is_relative ? m_start_position[axis] + ret : m_origin[axis] + ret;
    }
    else
        return m_start_position[axis];
}

void GCodeProcessor::update_slice_warnings()
{
    m_result.warnings.clear();

    auto get_used_filaments = [this]() {
        std::vector<size_t> used_filaments;
        used_filaments.reserve(m_used_filaments.total_volumes_per_filament.size());
        for (auto item : m_used_filaments.total_volumes_per_filament) {
            used_filaments.push_back(item.first);
        }
        return used_filaments;
    };

    auto used_filaments = get_used_filaments();
    assert(!used_filaments.empty());
    GCodeProcessorResult::SliceWarning warning;
    warning.level = 1;
    if (m_highest_bed_temp != 0) {
        for (size_t i = 0; i < used_filaments.size(); i++) {
            int temperature = get_filament_vitrification_temperature(used_filaments[i]);
            if (temperature != 0 && m_highest_bed_temp >= temperature)
                warning.params.push_back(std::to_string(used_filaments[i]));
        }
    }

    if (!warning.params.empty()) {
        warning.level       = 3;
        warning.msg         = BED_TEMP_TOO_HIGH_THAN_FILAMENT;
        warning.error_code  = "1000C001";
        m_result.warnings.push_back(warning);
    }

    warning.params.clear();
    warning.level=1;

    std::vector<int> nozzle_hrc_lists(m_result.nozzle_type.size(), 0);
    for (size_t idx = 0; idx < m_result.nozzle_type.size(); ++idx) {
        nozzle_hrc_lists[idx] = m_result.nozzle_hrc;
        if(nozzle_hrc_lists[idx] <= 0)
            nozzle_hrc_lists[idx] = Print::get_hrc_by_nozzle_type(m_result.nozzle_type[idx]);
    }

    for (size_t idx = 0; idx < used_filaments.size(); ++idx) {
        int filament_hrc = 0;

        if (used_filaments[idx] < m_result.required_nozzle_HRC.size())
            filament_hrc = m_result.required_nozzle_HRC[used_filaments[idx]];

        int filament_extruder_id = used_filaments[idx] < m_filament_maps.size() ? m_filament_maps[used_filaments[idx]] : -1;
        int extruder_hrc = (filament_extruder_id >= 0 && (size_t) filament_extruder_id < nozzle_hrc_lists.size()) ? nozzle_hrc_lists[filament_extruder_id] : 0;

        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": Check HRC: filament:%1%, hrc=%2%, extruder:%3%, hrc:%4%") % used_filaments[idx] % filament_hrc % filament_extruder_id % extruder_hrc;

        if (extruder_hrc!=0 && extruder_hrc < filament_hrc)
            warning.params.push_back(std::to_string(used_filaments[idx]));
    }

    if (!warning.params.empty()) {
        warning.level      = 3;
        warning.msg = NOZZLE_HRC_CHECKER;
        warning.error_code = "1000C002";
        m_result.warnings.push_back(warning);
    }

    warning.params.clear();
    warning.level = 1;
    if (!m_result.support_traditional_timelapse) {
        warning.level      = 2;
        warning.msg        = NOT_SUPPORT_TRADITIONAL_TIMELAPSE;
        warning.error_code = "10018003";
        m_result.warnings.push_back(warning);

        warning.level      = 3;
        warning.error_code = "1000C003";
        m_result.warnings.push_back(warning);
    }

    if (m_result.timelapse_warning_code != 0) {
        if (m_result.timelapse_warning_code & 1) {
            warning.level      = 1;
            warning.msg        = NOT_GENERATE_TIMELAPSE;
            warning.error_code = "10014001";
            m_result.warnings.push_back(warning);
        }
        if ((m_result.timelapse_warning_code >> 1) & 1) {
            warning.level      = 1;
            warning.msg        = NOT_GENERATE_TIMELAPSE;
            warning.error_code = "10014002";
            m_result.warnings.push_back(warning);
        }
        if ((m_result.timelapse_warning_code >> 2) & 1) {
            warning.level      = 2;
            warning.msg        = SMOOTH_TIMELAPSE_WITHOUT_PRIME_TOWER;
            warning.error_code = "10018004";
            m_result.warnings.push_back(warning);
        }
    }

    m_result.warnings.shrink_to_fit();
}

int GCodeProcessor::get_filament_id(bool force_initialize)const
{
    int extruder_id = get_extruder_id(force_initialize);
    if (extruder_id == -1)
        return force_initialize ? 0 : -1;

    if (m_filament_id[extruder_id] == (unsigned char)(-1))
        return force_initialize ? 0 : -1;

    return static_cast<int>(m_filament_id[extruder_id]);
}

int GCodeProcessor::get_last_filament_id(bool force_initialize)const
{
    int extruder_id = get_extruder_id(force_initialize);
    if (extruder_id == -1)
        return force_initialize ? 0 : -1;

    if (m_last_filament_id[extruder_id] == (unsigned char)(-1))
        return force_initialize ? 0 : -1;

    return static_cast<int>(m_last_filament_id[extruder_id]);
}

int GCodeProcessor::get_extruder_id(bool force_initialize)const
{
    if (m_extruder_id == (unsigned char)(-1))
        return force_initialize ? 0 : -1;
    return static_cast<int>(m_extruder_id);
}

}
