// Ports: named message endpoints (SPEC phase 11).
//
// A server creates a named port (port_create) and receives on it; each
// client that connects (port_connect) gets one end of a new two-way
// channel, and the server is handed the other end by its next port_recv on
// the named port (as a descriptor, like accept). Messages then travel over
// the channel in both directions with port_send / port_recv: whole
// messages of up to 64 KiB, in order, optionally carrying open descriptors
// (files, shared memory, other ports).
//
// Access control (SPEC §5A): a named port has an owner and a mode like a
// file's; connecting needs write permission. The identity of the process at
// the other end of a channel is recorded by the kernel when the channel is
// made (port_peer) and cannot be forged.
//
// Queues are bounded: at most 64 messages or 1 MiB wait in one direction,
// and a sender that finds the queue full waits (it is never dropped, and
// never grows the queue).
#pragma once

#include <ipc/object.h>
#include <lib/result.h>
#include <lib/types.h>
#include <sched/sched.h>

namespace port {
constexpr usize NAME_MAX = 63;
constexpr usize MESSAGE_MAX = 64 * KIB;
constexpr u32 FDS_MAX = 8;              // descriptors per message
constexpr u32 QUEUE_MESSAGES = 64;
constexpr usize QUEUE_BYTES = 1 * MIB;
constexpr u32 BACKLOG = 32;             // connections waiting to be accepted
constexpr u64 FOREVER = ~0ull;
} // namespace port

struct PortPeer {
    u32 pid, uid, gid;
};

// A new named port owned by `cred` with permission bits `mode`. Errors:
// Invalid (bad name), Exists, NoMemory.
Result<File*> port_create(const char* name, u32 mode, const Credentials& cred, u32 pid);
// Connects to a named port: the client end of a new channel. Errors:
// NotFound, Access, Again (the server's backlog is full), NoMemory.
Result<File*> port_connect(const char* name, const Credentials& cred, u32 pid);

// Sends one message (a kernel buffer of `len` bytes, taken over by the
// call) with `nfds` descriptors (references taken over too). Waits while
// the peer's queue is full. Errors: Invalid (not a channel end), Pipe (the
// peer is gone), Interrupted.
// `nonblock`: Error::Again instead of waiting for room in the queue.
Result<void> port_send(File* end, u8* data, usize len, File** fds, u32 nfds, bool nonblock = false);

struct PortMessage {
    u8* data;                   // kmalloc'd; the receiver frees it
    usize len;
    File* fds[port::FDS_MAX];   // references the receiver now owns
    u32 nfds;
};
// Receives the next message, waiting up to `timeout_ms` (0 = do not wait,
// port::FOREVER = no limit). On a named port the "message" is a new
// connection: len 0 and one descriptor, the server end of the channel. A
// message larger than `max_len` or with more than `max_fds` descriptors
// stays queued (TooBig). Errors: Again (nothing yet), Timeout, Pipe (peer
// gone and nothing queued), Interrupted, Invalid.
Result<void> port_recv(File* f, usize max_len, u32 max_fds, u64 timeout_ms, PortMessage* out);
// Who is at the other end of a channel. Error: Invalid.
Result<PortPeer> port_peer(File* end);
