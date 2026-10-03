// The OS version and build date (docs/SPEC.md §23). The number comes from the
// VERSION file at the repository root; development builds carry a
// "-dev+<commit>" suffix. Both strings are static; safe in any context.
#pragma once

const char* lumen_version();
const char* lumen_build_date();
