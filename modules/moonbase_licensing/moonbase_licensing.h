/*******************************************************************************
 The block below describes the properties of this module, and is read by
 the Projucer to automatically generate project code that uses it.
 For details about the syntax and how to create or use a module, see the
 JUCE Module Format.md file.


 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 moonbase_licensing
  vendor:             Moonbase
  version:            3.3.0
  name:               Moonbase Licensing
  description:        Moonbase license activation for JUCE apps and plugins, with a built-in activation UI. Talks to the Moonbase API natively — no juce::OnlineUnlockStatus. Zero third-party dependencies: JUCE WebInputStream transport, bundled nlohmann/json, and OS-native RS256 verification (Security.framework / CNG / libcrypto).
  website:            https://moonbase.sh
  license:            MIT
  minimumCppStandard: 17

  dependencies:       juce_core juce_events juce_data_structures juce_graphics juce_gui_basics juce_animation
  OSXFrameworks:      Security IOKit
  iOSFrameworks:      Security
  windowsLibs:        bcrypt
  linuxLibs:          crypto
  searchpaths:        . vendor

 END_JUCE_MODULE_DECLARATION

*******************************************************************************/

#pragma once
#define MOONBASE_LICENSING_H_INCLUDED

//==============================================================================
// Module configuration. Both are forced on so the module pulls in no
// third-party crypto/HTTP — define them yourself before this header to override.

#ifndef MOONBASE_CRYPTO_NATIVE
 #define MOONBASE_CRYPTO_NATIVE 1   // Security.framework / CNG / system libcrypto
#endif

#ifndef MOONBASE_DISABLE_CURL_TRANSPORT
 #define MOONBASE_DISABLE_CURL_TRANSPORT 1   // use the JUCE WebInputStream transport instead
#endif

// Module version (keep in sync with the `version:` field above). Also used as
// the SDK version when it isn't otherwise defined for this build, so the base
// client's User-Agent reports a real version instead of 0.0.0.
//
// Both this and the `version:` field are rewritten by scripts/bump-version.sh
// and committed through .releaserc.json's git assets. The CI consistency job
// compares them against CMakeLists.txt VERSION, because drift here silently
// misreports SDK traffic to the API.
#ifndef MOONBASE_LICENSING_VERSION
 #define MOONBASE_LICENSING_VERSION "3.3.0"
#endif
#ifndef MOONBASE_CPP_VERSION
 #define MOONBASE_CPP_VERSION MOONBASE_LICENSING_VERSION
#endif

//==============================================================================
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_animation/juce_animation.h>

// The Moonbase C++ SDK (header-only). Resolved from this module's own copy via
// the `.` searchpath; nlohmann/json from the `vendor` searchpath.
#include <moonbase/moonbase.hpp>

//==============================================================================
// Native integration + built-in UI.
#include "juce/juce_http_transport.h"
#include "juce/legacy_juce_device_id_resolver.h"
#include "juce/JuceMetadata.h"
#include "juce/LicenseGate.h"
#include "juce/ActivationConfig.h"
#include "juce/ActivationController.h"
#include "juce/ui/ActivationLookAndFeel.h"
#include "juce/ui/ActivationComponent.h"
#include "juce/ui/ActivationDialog.h"
