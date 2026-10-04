// The only translation unit compiled with the version macros, so a version
// or date change rebuilds one object instead of the whole kernel.
#include <lib/version.h>

const char* cerberus_version() { return CERBERUS_VERSION; }
const char* cerberus_build_date() { return CERBERUS_BUILD_DATE; }
