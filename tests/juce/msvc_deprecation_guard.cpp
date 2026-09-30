// MSVC marks CRT functions it considers unsafe (strerror, getenv, ...) with
// C4996. The module is header-only, so one such call warns in every plugin TU
// that includes the module header, and consumers reviewing their warnings have
// to either live with it or silence it project-wide. This TU makes C4996 an
// error for the module header, so the Windows job fails instead. JUCE's own
// headers come first so the pragma only covers code this repo owns. The compile
// is the assertion.

#include <juce_gui_basics/juce_gui_basics.h>

#if defined(_MSC_VER)
 #pragma warning(error : 4996)
#endif

#include <moonbase_licensing/moonbase_licensing.h>
