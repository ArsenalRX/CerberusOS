// Shared memory (SPEC phase 11): a block of zero-filled memory that several
// processes can map at once. shm_create returns a descriptor, which can be
// passed to another process through a port; shm_map maps the block into the
// caller's address space. The memory lives until the last descriptor is
// closed and the last mapping is gone.
//
// The block is physically contiguous (it is mapped the way device memory
// is), so very large blocks can fail on a fragmented machine.
#pragma once

#include <ipc/object.h>
#include <lib/result.h>
#include <lib/types.h>
#include <sched/sched.h>

constexpr usize SHM_MAX_BYTES = 256 * MIB;

// A new block of `size` bytes (rounded up to pages). Errors: Invalid (zero
// or over SHM_MAX_BYTES), NoMemory.
Result<File*> shm_create(usize size);
// Maps the block into the calling process; writable if asked. Returns the
// address. Errors: Invalid (not a shared-memory descriptor), NoMemory.
Result<vaddr_t> shm_map(File* f, bool writable);
// Unmaps a block mapped at `addr` by shm_map. Error: Invalid.
Result<void> shm_unmap(vaddr_t addr);
// Size in bytes of the block behind a descriptor, 0 if it is not one.
usize shm_size(File* f);

// Process lifetime: the child of a fork shares the parent's mappings; a
// process that exits (or replaces its program) lets go of its mappings.
Result<void> shm_fork(Process* parent, Process* child);
void shm_release_process(Process* p);
