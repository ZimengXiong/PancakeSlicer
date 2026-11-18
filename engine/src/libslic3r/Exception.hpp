#ifndef _libslic3r_Exception_h_
#define _libslic3r_Exception_h_

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class Exception : public std::runtime_error { using std::runtime_error::runtime_error; };
#define SLIC3R_DERIVE_EXCEPTION(DERIVED_EXCEPTION, PARENT_EXCEPTION) \
    class DERIVED_EXCEPTION : public PARENT_EXCEPTION { using PARENT_EXCEPTION::PARENT_EXCEPTION; }
SLIC3R_DERIVE_EXCEPTION(CriticalException,  Exception);
SLIC3R_DERIVE_EXCEPTION(RuntimeError,       CriticalException);
SLIC3R_DERIVE_EXCEPTION(LogicError,         CriticalException);
SLIC3R_DERIVE_EXCEPTION(HardCrash,          CriticalException);
SLIC3R_DERIVE_EXCEPTION(InvalidArgument,    LogicError);
SLIC3R_DERIVE_EXCEPTION(OutOfRange,         LogicError);
SLIC3R_DERIVE_EXCEPTION(IOError,            CriticalException);
SLIC3R_DERIVE_EXCEPTION(FileIOError,        IOError);
SLIC3R_DERIVE_EXCEPTION(HostNetworkError,   IOError);
SLIC3R_DERIVE_EXCEPTION(ExportError,        CriticalException);
SLIC3R_DERIVE_EXCEPTION(PlaceholderParserError, RuntimeError);
class SlicingError : public Exception 
{
public:
    using Exception::Exception;
    SlicingError(std::string const &msg, size_t objectId) : Exception(msg), objectId_(objectId) {}
    SlicingError(std::string const &msg, std::string code, std::string object = {}, std::string params = "{}")
        : Exception(msg), code_(std::move(code)), object_(std::move(object)), params_(std::move(params)) {}
    size_t objectId() const { return objectId_; }
    const std::string &code() const { return code_; }
    const std::string &object() const { return object_; }
    const std::string &params() const { return params_; }

private:
    size_t objectId_ = 0;
    std::string code_, object_, params_ = "{}";
};

class SlicingErrors : public Exception
{
public:
    using Exception::Exception;
    SlicingErrors(const std::vector<SlicingError> &errors) : Exception("Errors"), errors_(errors) {}

    std::vector<SlicingError> errors_;
};

#undef SLIC3R_DERIVE_EXCEPTION

}

#endif
