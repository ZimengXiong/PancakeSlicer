#include "Research.hpp"

#include <map>
#include <string>

namespace Slic3r {

namespace {
std::map<std::string, std::string> &switches()
{
    static std::map<std::string, std::string> map;
    return map;
}
}

void set_research_switch(const std::string &name, const std::string &value) { switches()[name] = value; }

const char *research_switch(const char *name)
{
    const auto it = switches().find(name);
    return it == switches().end() ? nullptr : it->second.c_str();
}

}
