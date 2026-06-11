// NS3 boundary-enforcement harness -- design C (drop gameos_main).
// Spec: docs/superpowers/specs/2026-05-18-mclib-standalone-boundary-enforcement-spec.md
//
// This TU is NOT a game entry point and is built ONLY when
// MC2_BUILD_ENGINE_STANDALONE=ON. It exists so the engine libs
// (mclib gosfx mlr stuff gameos windows) link WITHOUT code/ under
// /WHOLEARCHIVE. The resulting linker unresolved-symbol set is the
// authoritative, compiler-proven NS3 metric: every mclib/engine -> code
// (and platform) boundary edge, with no static-archive pruning hiding any.
//
// gameos_main (GameOS/gameos/gameosmain.cpp -- the real game-entry TU that
// owns the GameOS Environment callback contract) is deliberately excluded
// from the link: it is the game entry, not the engine boundary under test.
// Nothing in the engine libs proper should reference GetGameOSEnvironment /
// Environment.* ; if the link surfaces those, that itself is a finding.
//
// Only the 3 grep-verified upward globals are NULL-stubbed, so the link
// surfaces the UNKNOWN surface rather than the 3 already-known leaks:
//   eye             extern mclib/camera.h:1125     (CameraPtr  = Camera*)
//   globalFloatHelp extern mclib/appear.h:43        (FloatHelpPtr = FloatHelp*)
//   MPlayer         bare ref mclib/abldbug.cpp:1283 (MultiPlayer*; class is
//                   code/multplyr.h:1294 -- forward-declared here only, never
//                   dereferenced, so the symbol resolves without code/).

#include <gameos.hpp>
#include "mclib.h"        // umbrella: camera.h (135), appear.h (139),
                          // floathelp.h (175) -- verified code/-clean

class MultiPlayer;        // code/multplyr.h:1294 -- fwd-decl only

CameraPtr    eye             = NULL;
FloatHelpPtr globalFloatHelp = NULL;
MultiPlayer* MPlayer         = NULL;

int main(void) { return 0; }
