// Apple compilation unit. JUCE compiles this instead of moonbase_licensing.cpp
// on macOS/iOS so the module body builds as Objective-C++; the Apple crypto
// backend (Security.framework / CommonCrypto) and any Cocoa-touching helpers
// compile here.
#include "moonbase_licensing.cpp"
