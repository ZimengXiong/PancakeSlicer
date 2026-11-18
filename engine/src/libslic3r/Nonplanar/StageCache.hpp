#pragma once

#include "libslic3r/WarningCode.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class Print;
class PrintObject;

namespace Nonplanar {

void set_stage_cache(const std::string &dir, uint64_t max_bytes);

std::string stage_cache_key(const PrintObject &object);

bool stage_cache_read(const std::string &key, const char *kind, std::string &data);
void stage_cache_write(const std::string &key, const char *kind, const std::string &data);

void stage_cache_prime(Print &print);
void stage_cache_record(const Print &print);
void stage_cache_converged(const Print &print, bool converged, const std::map<size_t, std::vector<std::string>> &access_warnings);
std::vector<std::string> stage_cache_access_warnings(size_t object_id);
void stage_cache_note_guard_warnings(const Print &print, const std::vector<CodedWarning> &warnings);
std::vector<CodedWarning> stage_cache_guard_warnings(const Print &print);

}
}
