// See file.h.
#include <arch/x86_64/cpu.h>
#include <boot/bootinfo.h>
#include <fs/file.h>
#include <fs/ustar.h>
#include <lib/console.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>

namespace {

const u8* g_archive = nullptr;
usize g_archive_size = 0;

bool count_entry(const char*, const UstarEntry&, void*) { return true; }

} // namespace

void files_init() {
    const BootInfo& bi = g_boot_info;
    for (usize i = 0; i < bi.module_count; i++) {
        usize len = strlen(bi.modules[i].path);
        if (len >= 4 && strcmp(bi.modules[i].path + len - 4, ".tar") == 0) {
            g_archive = (const u8*)bi.modules[i].address;
            g_archive_size = bi.modules[i].size;
            break;
        }
    }
    if (!g_archive) {
        kprintf("files: no boot archive among %lu module(s); user programs are unavailable\n",
                (unsigned long)bi.module_count);
        return;
    }
    usize entries = ustar_each(g_archive, g_archive_size, count_entry, nullptr);
    kprintf("files: boot archive %lu KiB, %lu entries\n", (unsigned long)(g_archive_size / KIB),
            (unsigned long)entries);
}

bool files_archive_present() { return g_archive != nullptr; }

Result<void> file_archive_lookup(const char* path, const u8** data, usize* size, u32* mode) {
    UstarEntry e;
    if (!g_archive || !ustar_find(g_archive, g_archive_size, path, &e)) return Error::NotFound;
    if (e.is_dir) return Error::IsDir;
    *data = e.data;
    *size = e.size;
    *mode = e.mode;
    return {};
}

Result<File*> file_open_console() {
    File* f = (File*)kzalloc(sizeof(File));
    if (!f) return Error::NoMemory;
    f->kind = FileKind::Console;
    f->refs = 1;
    f->readable = f->writable = true;
    return f;
}

Result<File*> file_open_archive(const char* path) {
    const u8* data;
    usize size;
    u32 mode;
    Result<void> found = file_archive_lookup(path, &data, &size, &mode);
    if (!found.ok()) return found.error();
    File* f = (File*)kzalloc(sizeof(File));
    if (!f) return Error::NoMemory;
    f->kind = FileKind::Archive;
    f->refs = 1;
    f->readable = true;
    f->data = data;
    f->size = size;
    return f;
}

File* file_ref(File* f) {
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
    return f;
}

void file_unref(File* f) {
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(f);
}

Result<usize> file_read(File* f, void* buf, usize n) {
    if (!f->readable) return Error::BadFd;
    if (f->kind == FileKind::Console) return (usize)0;
    // The offset is shared after fork: claim a range with compare-and-swap
    // so two readers on different CPUs never get the same bytes.
    usize at = __atomic_load_n(&f->offset, __ATOMIC_RELAXED), take;
    do {
        usize left = at < f->size ? f->size - at : 0;
        take = n < left ? n : left;
    } while (take && !__atomic_compare_exchange_n(&f->offset, &at, at + take, false, __ATOMIC_ACQ_REL,
                                                    __ATOMIC_RELAXED));
    memcpy(buf, f->data + at, take);
    return take;
}

Result<usize> file_write(File* f, const void* buf, usize n) {
    if (!f->writable) return Error::BadFd;
    // The console is the only writable kind. Like one kprintf, one write is
    // one uninterrupted piece of output (the caller hands over at most a
    // small chunk), so a program's line is not split by another thread's.
    console_lock();
    console_write((const char*)buf, n);
    console_unlock();
    return n;
}
