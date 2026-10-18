// The one file that sees TGC_VERSION (see Version.h and scripts/version.py), so
// a new version recompiles only this file.
#include "Version.h"

// A build without scripts/version.py (an IDE indexer, a hand-rolled build)
// still compiles.
#ifndef TGC_VERSION
#define TGC_VERSION "dev"
#endif

namespace tgc {

const char* firmwareVersion() { return TGC_VERSION; }

}  // namespace tgc
