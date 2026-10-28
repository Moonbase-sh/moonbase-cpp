// Single compilation unit for the moonbase_licensing JUCE module.
//
// On Apple platforms JUCE compiles moonbase_licensing.mm instead, which simply
// #includes this file so the same body builds as Objective-C++ (the Apple
// crypto backend uses Security.framework). Everything the module defines is
// funneled through here so there is exactly one translation unit.

#ifdef MOONBASE_LICENSING_H_INCLUDED
 /* When you add this cpp file to your project, you mustn't include it in a file
    where you've already included any other headers - just put it inside a file
    on its own, possibly with your config flags preceding it, but don't include
    anything else. That also includes avoiding any automatic prefix header files
    that the compiler may be using. */
 #error "Incorrect use of JUCE cpp file"
#endif

#include "moonbase_licensing.h"

// On Linux, juce::WebInputStream makes HTTPS requests through libcurl. With
// JUCE_USE_CURL=0 it falls back to a plain-socket client that only speaks
// http://, so every call to the Moonbase API would fail at runtime and the
// product could never activate. Stop the build instead. Other platforms use the
// OS's own HTTP stack and ignore JUCE_USE_CURL.
#if (JUCE_LINUX || JUCE_BSD) && ! JUCE_USE_CURL
 #error "moonbase_licensing needs JUCE_USE_CURL on Linux: without libcurl, JUCE cannot make HTTPS requests to the Moonbase API. Remove JUCE_USE_CURL=0 and, with CMake, pass NEEDS_CURL TRUE to juce_add_plugin() or juce_add_gui_app()."
#endif

//==============================================================================
#include "juce/ActivationController.cpp"
#include "juce/ui/ActivationLookAndFeel.cpp"
#include "juce/ui/ActivationComponent.cpp"
#include "juce/ui/ActivationDialog.cpp"
