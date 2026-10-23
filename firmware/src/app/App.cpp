// The app's shared context; see App.h.
#include "app/App.h"

namespace tgc {
namespace app {

std::unique_ptr<tgc::PackReader> gPack = std::make_unique<tgc::PackReader>();
std::unique_ptr<tgc::FsPackSource> gSource;
tgc::Renderer gRender;
tgc::XorShiftRng gRng;

tgc::SrsStore gSrs;
tgc::FsSrsFile gSrsIo;
tgc::Queue gQueue;
int gDirty = 0;            // answers not yet persisted
bool gSrsLoaded = false;        // the state file loaded at boot

Screen gScreen = Screen::Quiz;
Screen gBeforeMenu = Screen::Quiz;
Preferences gPrefs;

}  // namespace app
}  // namespace tgc
