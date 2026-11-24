// mcla-x86: entry point.
//
// The SDK supplies wWinMain (src/ui/windowed_app_main_sdl.cpp, compiled into this executable by
// rexglue_configure_target): cvar parsing, the SDL app context, COM, OnInitialize and the message loop. All that is
// left for the game is the app object it creates, REX_DEFINE_APP.
//
// "nfsmw" is the identity of the generated code (nfsmw_init.h, PPCImageConfig). The app's own name, which names the
// config file, the user folder and the logs, is the string McLaApp passes to ReXApp: "mcla_x86".

#include "nfsmw_init.h"  // generated (MCLA_GENERATED_DIR): PPCImageConfig

#include <memory>

#include "mcla_app.h"

std::unique_ptr<rex::ui::WindowedApp> McLaApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::make_unique<McLaApp>(ctx, PPCImageConfig);
}

REX_DEFINE_APP(nfsmw, McLaApp::Create)

// Read by NVIDIA Optimus and AMD PowerXpress when the executable loads: on a laptop with switchable graphics they run
// the process on the high-performance GPU. Exported variables, so they cannot follow a setting.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
