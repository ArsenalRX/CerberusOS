// Names for Error values, used in logs and test output.
#include <lib/result.h>

const char* error_name(Error e) {
    switch (e) {
    case Error::None: return "none";
    case Error::NoMemory: return "no-memory";
    case Error::NotFound: return "not-found";
    case Error::Exists: return "exists";
    case Error::Invalid: return "invalid";
    case Error::Perm: return "permission";
    case Error::Busy: return "busy";
    case Error::Fault: return "fault";
    case Error::NoSpace: return "no-space";
    case Error::IsDir: return "is-directory";
    case Error::NotDir: return "not-directory";
    case Error::Again: return "again";
    case Error::Interrupted: return "interrupted";
    case Error::IO: return "io";
    case Error::NotSupported: return "not-supported";
    case Error::TooBig: return "too-big";
    case Error::Deadlock: return "deadlock";
    case Error::Timeout: return "timeout";
    }
    return "unknown";
}
