// Result<T>: the return type of every fallible kernel function (SPEC §6.3).
// Holds either a value or an Error; no exceptions, no allocation. Errors map
// to negative errno values at the syscall boundary.
#pragma once

#include <lib/panic.h>
#include <lib/types.h>

enum class Error : u8 {
    None,
    NoMemory,
    NotFound,
    Exists,
    Invalid,
    Perm,
    Busy,
    Fault,
    NoSpace,
    IsDir,
    NotDir,
    Again,
    Interrupted,
    IO,
    NotSupported,
    TooBig,
    Deadlock,
    Timeout,
    BadFd,              // not an open file descriptor
    NoChild,            // no child process to wait for
    NoProcess,
    TooManyFiles,
    NotExecutable,
    Access,             // permission bits refuse the operation (EACCES)
    ReadOnly,           // read-only file system (EROFS)
    NotEmpty,           // directory not empty
    NameTooLong,
    Loop,               // too many symbolic links
    CrossDevice,        // rename across file systems
    NoDevice,           // no such device, or the device does not support it
    Pipe,               // the other end of a channel is closed
};

// Short lowercase name for logs ("no-memory", "invalid", ...).
const char* error_name(Error e);

template <typename T> class [[nodiscard]] Result {
public:
    Result(T v) : value_(v), error_(Error::None) {}
    Result(Error e) : value_(), error_(e) { ASSERT(e != Error::None); }

    bool ok() const { return error_ == Error::None; }
    // Panics if the result holds an error; check ok() first.
    T& value() {
        ASSERT_ALWAYS(ok());
        return value_;
    }
    Error error() const { return error_; }

private:
    T value_;
    Error error_;
};

template <> class [[nodiscard]] Result<void> {
public:
    Result() : error_(Error::None) {}
    Result(Error e) : error_(e) {}

    bool ok() const { return error_ == Error::None; }
    Error error() const { return error_; }

private:
    Error error_;
};
