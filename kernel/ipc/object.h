// Kernel objects behind file descriptors that are not files: ports, shared
// memory, event queues (SPEC phase 11). Each is a File whose vnode has type
// VType::Object and carries the object; descriptors to it can be duplicated,
// inherited across fork and passed through ports like any other, and the
// object is destroyed when the last one closes.
//
// Readiness ("can this be read or written without waiting?") is asked
// through file_poll for every kind of descriptor; event queues are built on
// it. Whoever changes readiness calls poll_wake so sleeping event_wait
// callers look again.
#pragma once

#include <fs/file.h>
#include <lib/result.h>
#include <lib/types.h>

enum class ObjectKind : u8 { PortListener = 1, PortEndpoint, Shm, EventQueue };

struct KObject {
    ObjectKind kind;
    // Called once, when the last descriptor is closed. VFS lock held.
    void (*destroy)(KObject* self);
    // Readiness bits (poll::*), or null if the object is never waited on.
    u32 (*poll)(KObject* self);
};

namespace poll {
constexpr u32 READ = 1, WRITE = 2, HUP = 4, TIMER = 8, CHILD = 16;
}

// Wraps an object in a File (one reference). On failure the object has not
// been destroyed. Error: NoMemory.
Result<File*> object_file(KObject* obj);
// The object behind `f` if it is of `kind`, else null.
KObject* file_object(File* f, ObjectKind kind);
// Readiness of any descriptor.
u32 file_poll(File* f);

// The descriptor table and working directory of the calling process. Its
// threads share them, so every access goes through these functions (one
// short lock), and a lookup hands out a reference of its own: a file stays
// alive for the whole of a call that uses it even if another thread closes
// the descriptor meanwhile.
//
// Installs `f` (taking the reference) at the lowest free descriptor. On
// failure the reference is dropped. Error: TooManyFiles.
Result<int> fd_install(File* f, bool cloexec);
// The File behind a descriptor, with a new reference (file_unref it), or null.
File* fd_get(u64 fd);
// Empties a descriptor and returns the reference the table held, or null.
File* fd_take(u64 fd);
// Makes descriptor `fd` refer to `f` (taking the reference), closing what
// was there. Error: BadFd (out of range; the reference is dropped).
Result<void> fd_replace(u64 fd, File* f);
// Closes every descriptor marked close-on-exec.
void fd_close_on_exec();
// Gives `child` (not running yet) a copy of the calling process's table and
// working directory.
void fd_fork(Process* child);
// The working directory with a new reference (null = the root), and
// changing it (takes the reference `dir`).
Vnode* cwd_get();
void cwd_set(Vnode* dir);

// Readiness changed somewhere: wake every event_wait. Interrupt-safe.
void poll_wake();
// The same with the scheduler lock already held.
void poll_wake_locked();
// For event_wait: the generation counter poll_wake advances, and a wait
// that ends when it moves on from `seen` (scheduler lock taken inside).
u64 poll_generation();
WaitResult poll_wait(u64 seen, u64 ticks);
