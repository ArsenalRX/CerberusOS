// Kernel shell over the serial console: a tiny line editor and a command
// table. Exists for bring-up and the self-tests (`test <name>`); it is not
// the user shell. Runs forever on the calling CPU with interrupts enabled.
#pragma once

#include <lib/types.h>

using ShellCommand = int (*)(int argc, char** argv);

struct ShellCommandEntry {
    const char* name;
    const char* args;           // its arguments, for `help`
    const char* help;           // one short line
    ShellCommand fn;            // null: a group heading in `help`
};

[[noreturn]] void shell_run();
