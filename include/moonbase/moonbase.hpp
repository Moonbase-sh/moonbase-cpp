#pragma once

#include "moonbase/client.hpp"
// default_fingerprint.hpp and fingerprint.hpp are the deprecated-alias headers
// for the pre-4.0.0 names. Kept in the umbrella so that including
// <moonbase/moonbase.hpp> still compiles code written against 3.x; the aliases
// only warn where they are actually used.
#include "moonbase/default_fingerprint.hpp"
#include "moonbase/device_id_resolver.hpp"
#include "moonbase/errors.hpp"
#include "moonbase/fingerprint.hpp"
#include "moonbase/fingerprint_spec.hpp"
#include "moonbase/http.hpp"
#include "moonbase/legacy_fingerprint.hpp"
#include "moonbase/moonbase_device_id_resolver.hpp"
#ifndef MOONBASE_DISABLE_CURL_TRANSPORT
#include "moonbase/http_curl.hpp"
#endif
#include "moonbase/inventory.hpp"
#include "moonbase/licensing.hpp"
#include "moonbase/store.hpp"
#include "moonbase/types.hpp"
#include "moonbase/validator.hpp"
#include "moonbase/version.hpp"
