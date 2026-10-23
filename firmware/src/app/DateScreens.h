#pragma once
// The date screens (docs/srs.md section 5): the date stepper (the power-cut
// question at boot, Settings > Device > Date for corrections), the Date
// submenu and the time zone list.
#include <cstdint>

#include "app/App.h"
#include "ui/ListView.h"

namespace tgc {
namespace app {

extern Screen gAfterDate;   // where the date screen returns to
extern int32_t gDateSteps;  // days from the current day's date to the proposed "today"
extern tgc::ListView gDateMenuList;
extern char gDateMenuNote[48];
extern tgc::ListView gTzListView;

void openDate(Screen after);
void drawDate();
void dateLoop(int a, int b);
void leaveDate();

void drawDateMenu();
void openDateMenu(bool fresh);
void dateMenuLoop(int a, int b);
void drawTzList();
void tzListLoop(int a, int b);

}  // namespace app
}  // namespace tgc
