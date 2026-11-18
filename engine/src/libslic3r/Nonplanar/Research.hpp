#pragma once

#include <string>

namespace Slic3r {

void set_research_switch(const std::string &name, const std::string &value);
const char *research_switch(const char *name);

}
