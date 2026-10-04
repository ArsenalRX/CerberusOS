// See dev.h.
#include <fs/block.h>
#include <fs/dev.h>
#include <fs/vfs.h>
#include <lib/console.h>
#include <lib/csprng.h>
#include <lib/string.h>

namespace {

constexpr u32 MAX_MAJOR = 64;
const CharDeviceOps* g_char[MAX_MAJOR];

const CharDeviceOps* char_ops(const Vnode* v) {
    u32 major = dev::major_of(v->rdev);
    return major < MAX_MAJOR ? g_char[major] : nullptr;
}

// ------------------------------------------------------- vnode operations --
Result<usize> dv_read(Vnode* v, u64 off, void* buf, usize n) {
    if (v->type == VType::BlockDev) return block_read_bytes(dev::minor_of(v->rdev), v, off, buf, n);
    const CharDeviceOps* ops = char_ops(v);
    if (!ops || !ops->read) return Error::NoDevice;
    return ops->read(dev::minor_of(v->rdev), off, buf, n);
}

Result<usize> dv_write(Vnode* v, u64 off, const void* buf, usize n) {
    if (v->type == VType::BlockDev) return block_write_bytes(dev::minor_of(v->rdev), v, off, buf, n);
    const CharDeviceOps* ops = char_ops(v);
    if (!ops || !ops->write) return Error::NoDevice;
    return ops->write(dev::minor_of(v->rdev), off, buf, n);
}

Result<i64> dv_ioctl(Vnode* v, u32 request, u64 arg) {
    if (v->type == VType::BlockDev) return block_ioctl(dev::minor_of(v->rdev), request, arg);
    const CharDeviceOps* ops = char_ops(v);
    if (!ops || !ops->ioctl) return Error::NotSupported;
    return ops->ioctl(dev::minor_of(v->rdev), request, arg);
}

Result<void> dv_fsync(Vnode* v) {
    if (v->type == VType::BlockDev) return block_sync(dev::minor_of(v->rdev), v);
    return {};
}

// Device nodes keep their place in the directory tree through the file
// system that holds them (tmpfs); its release hook frees them.
VnodeOps g_dev_vnode_ops = {
    nullptr, nullptr, nullptr, nullptr, dv_read, dv_write, nullptr, nullptr, nullptr, nullptr, dv_fsync, dv_ioctl,
    nullptr,
};

// ---------------------------------------------------------- memory devices --
Result<usize> mem_read(u32 minor, u64, void* buf, usize n) {
    switch (minor) {
    case dev::NULL_: return (usize)0;
    case dev::ZERO: memset(buf, 0, n); return n;
    case dev::RANDOM:
    case dev::URANDOM: csprng_bytes(buf, n); return n;
    }
    return Error::NoDevice;
}

Result<usize> mem_write(u32 minor, u64, const void*, usize n) {
    switch (minor) {
    case dev::NULL_:
    case dev::ZERO:
    case dev::RANDOM:
    case dev::URANDOM: return n;        // discarded (random: no entropy credit from users)
    }
    return Error::NoDevice;
}

const CharDeviceOps g_mem = {nullptr, mem_read, mem_write, nullptr};

// ---------------------------------------------------------------- console --
// Output goes to the console like kprintf. There is no input for programs
// yet: the kernel shell owns the keyboard until terminals arrive with job
// control (phase 17), so reads return 0 (end of file).
Result<usize> tty_read(u32, u64, void*, usize) { return (usize)0; }

Result<usize> tty_write(u32, u64, const void* buf, usize n) {
    console_lock();
    console_write((const char*)buf, n);
    console_unlock();
    return n;
}

const CharDeviceOps g_tty = {nullptr, tty_read, tty_write, nullptr};

} // namespace

void dev_register_char(u32 major, const CharDeviceOps* ops) {
    if (major < MAX_MAJOR) g_char[major] = ops;
}

const VnodeOps* dev_vnode_ops() { return &g_dev_vnode_ops; }

Result<void> dev_open(Vnode* v) {
    if (v->type == VType::BlockDev) return block_present(dev::minor_of(v->rdev)) ? Result<void>() : Error::NoDevice;
    const CharDeviceOps* ops = char_ops(v);
    if (!ops) return Error::NoDevice;
    return ops->open ? ops->open(dev::minor_of(v->rdev)) : Result<void>();
}

void dev_init() {
    dev_register_char(dev::MEM, &g_mem);
    dev_register_char(dev::TTY, &g_tty);
}
