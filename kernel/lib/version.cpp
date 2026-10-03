// The only translation unit compiled with the version macros, so a version
// or date change rebuilds one object instead of the whole kernel.
#include <lib/version.h>

const char* lumen_version() { return LUMEN_VERSION; }
const char* lumen_build_date() { return LUMEN_BUILD_DATE; }
