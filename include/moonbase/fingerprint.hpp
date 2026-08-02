#pragma once

// Compatibility header.
//
// moonbase::fingerprint_provider was renamed to moonbase::device_id_resolver in
// 4.0.0, when this SDK adopted the cross-SDK device fingerprint spec. The old
// names still work, so a custom provider keeps compiling across the upgrade, but
// they are deprecated and will be removed in 5.0.0.
//
// New code should include <moonbase/device_id_resolver.hpp> for the interface,
// <moonbase/moonbase_device_id_resolver.hpp> for the spec implementation, and
// <moonbase/fingerprint_spec.hpp> for the algorithm primitives.
//
// Define MOONBASE_DISABLE_DEPRECATED_ALIASES to compile the aliases out now,
// which is the quickest way to find every remaining use in a codebase.

#include "moonbase/device_id_resolver.hpp"

namespace moonbase {

#if !defined(MOONBASE_DISABLE_DEPRECATED_ALIASES)

using fingerprint_provider [[deprecated("renamed to moonbase::device_id_resolver")]] = device_id_resolver;

using static_fingerprint_provider
    [[deprecated("renamed to moonbase::static_device_id_resolver")]] = static_device_id_resolver;

#endif

} // namespace moonbase
