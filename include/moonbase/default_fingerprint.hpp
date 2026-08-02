#pragma once

// Compatibility header.
//
// The default device fingerprint changed in 4.0.0: it now follows the cross-SDK
// Moonbase device fingerprint spec, so `default_fingerprint_provider` resolves to
// moonbase_device_id_resolver rather than to the old `moonbase-cpp:fingerprint:v1`
// algorithm. Device ids computed here therefore differ from those computed by
// 3.x, and existing licenses need either re-activation or a
// migrating_device_id_resolver. See "Migrating from 3.x" in the README.
//
// The previous algorithm is preserved verbatim as
// moonbase::legacy_cpp_device_id_resolver in <moonbase/legacy_fingerprint.hpp>,
// so it can keep validating licenses that were bound under it.
//
// Define MOONBASE_DISABLE_DEPRECATED_ALIASES to compile the alias out.

#include "moonbase/moonbase_device_id_resolver.hpp"

namespace moonbase {

#if !defined(MOONBASE_DISABLE_DEPRECATED_ALIASES)

using default_fingerprint_provider
    [[deprecated("renamed to moonbase::moonbase_device_id_resolver; note that 4.0.0 also changed "
                 "the algorithm to the cross-SDK spec, and the previous one is now "
                 "moonbase::legacy_cpp_device_id_resolver")]] = moonbase_device_id_resolver;

#endif

} // namespace moonbase
