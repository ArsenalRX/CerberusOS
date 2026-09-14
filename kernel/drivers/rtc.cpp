// Reads are repeated until two consecutive samples agree, which avoids
// tearing across a CMOS update cycle without waiting on the UIP flag.
#include <arch/x86_64/io.h>
#include <drivers/refclock.h>
#include <drivers/rtc.h>
#include <lib/kprintf.h>

namespace {

constexpr u16 CMOS_ADDR = 0x70;
constexpr u16 CMOS_DATA = 0x71;

DateTime g_boot;
u64 g_boot_unix = 0;

u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, (u8)(0x80 | reg));     // keep NMI disabled bit as firmware left it: set it
    return inb(CMOS_DATA);
}

u8 bcd(u8 v, bool is_bcd) { return is_bcd ? (u8)((v & 0x0F) + (v >> 4) * 10) : v; }

DateTime read_once() {
    u8 status_b = cmos_read(0x0B);
    bool is_bcd = !(status_b & 0x04);
    bool h12 = !(status_b & 0x02);
    DateTime dt;
    dt.second = bcd(cmos_read(0x00), is_bcd);
    dt.minute = bcd(cmos_read(0x02), is_bcd);
    u8 hour_raw = cmos_read(0x04);
    bool pm = h12 && (hour_raw & 0x80);
    dt.hour = bcd((u8)(hour_raw & 0x7F), is_bcd);
    if (h12) {
        if (dt.hour == 12) dt.hour = 0;
        if (pm) dt.hour = (u8)(dt.hour + 12);
    }
    dt.day = bcd(cmos_read(0x07), is_bcd);
    dt.month = bcd(cmos_read(0x08), is_bcd);
    u16 year = bcd(cmos_read(0x09), is_bcd);
    u8 century = bcd(cmos_read(0x32), is_bcd);
    dt.year = (u16)((century >= 19 && century <= 21) ? century * 100 + year : 2000 + year);
    return dt;
}

bool same(const DateTime& a, const DateTime& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour &&
           a.minute == b.minute && a.second == b.second;
}

} // namespace

u64 datetime_to_unix(const DateTime& dt) {
    // Days from civil (Howard Hinnant's algorithm).
    i64 y = dt.year, m = dt.month, d = dt.day;
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + doe - 719468;
    return (u64)(days * 86400 + dt.hour * 3600 + dt.minute * 60 + dt.second);
}

DateTime unix_to_datetime(u64 seconds) {
    i64 z = (i64)(seconds / 86400) + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = yoe + era * 400;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    i64 d = doy - (153 * mp + 2) / 5 + 1;
    i64 m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    DateTime dt;
    dt.year = (u16)y;
    dt.month = (u8)m;
    dt.day = (u8)d;
    u64 rem = seconds % 86400;
    dt.hour = (u8)(rem / 3600);
    dt.minute = (u8)(rem % 3600 / 60);
    dt.second = (u8)(rem % 60);
    return dt;
}

void rtc_init() {
    DateTime a = read_once(), b;
    for (int i = 0; i < 10; i++) {
        b = read_once();
        if (same(a, b)) break;
        a = b;
    }
    g_boot = a;
    g_boot_unix = datetime_to_unix(a);
    kprintf("rtc: %04u-%02u-%02u %02u:%02u:%02u (CMOS, treated as UTC)\n", a.year, a.month, a.day, a.hour,
            a.minute, a.second);
}

DateTime rtc_boot_time() { return g_boot; }

DateTime rtc_now() { return unix_to_datetime(g_boot_unix + refclock_now_us() / 1000000); }
