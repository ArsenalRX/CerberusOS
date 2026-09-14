// CMOS real-time clock: reads the date and time once at boot; the kernel
// then keeps wall time by adding the reference clock. Century and BCD/binary
// formats are handled. Time is reported as UTC as the firmware provides it.
#pragma once

#include <lib/types.h>

struct DateTime {
    u16 year;
    u8 month, day, hour, minute, second;
};

void rtc_init();
// Boot date/time as read from CMOS.
DateTime rtc_boot_time();
// Current wall time: boot time plus elapsed reference-clock seconds.
DateTime rtc_now();
// Seconds since the Unix epoch for a DateTime (proleptic Gregorian).
u64 datetime_to_unix(const DateTime& dt);
DateTime unix_to_datetime(u64 seconds);
