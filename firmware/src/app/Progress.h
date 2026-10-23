#pragma once
// Progress operations (docs/srs.md section 2.1): a deck deleted from the portal
// takes the progress only it had, a restore from the portal replaces the SRS
// state, and Settings > Study > Delete progress.
#include <cstdint>

#include "ui/ListView.h"

namespace tgc {
namespace app {

bool portalDeleteDeck(const char* path, uint32_t* forgotten);
bool portalRestoreProgress(const char* tmpPath);

extern tgc::ListView gResetList;
extern char gResetNote[32];       // the list's note ("Nothing to delete")
extern uint32_t gResetConfirmAt;  // millis() the confirmation opened (keyAction()'s Enter guard)

void openReset(bool fresh);
void drawReset();
void drawResetConfirm();
void resetLoop(int a, int b);
void resetConfirmLoop(int a, int b);

}  // namespace app
}  // namespace tgc
