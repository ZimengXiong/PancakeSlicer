#pragma once

#include <string>

namespace Slic3r {

struct WarningCode
{
    std::string code;
    std::string object;
    std::string params = "{}";
};

struct CodedWarning
{
    WarningCode code;
    std::string message;
};

std::string warning_object(const std::string &name);

}
