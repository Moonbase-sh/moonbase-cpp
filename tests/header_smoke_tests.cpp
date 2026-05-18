#include <doctest/doctest.h>

#define linux 1
#define unix 1
#include "moonbase/moonbase.hpp"

static_assert(linux == 1, "moonbase headers must not undefine linux");
static_assert(unix == 1, "moonbase headers must not undefine unix");
static_assert(
    static_cast<int>(moonbase::platform::linux_os) >= 0,
    "linux_os platform enumerator must remain usable while linux is a macro");

#undef linux
#undef unix

TEST_CASE("public headers tolerate legacy linux and unix macros")
{
    CHECK(moonbase::to_string(moonbase::platform::linux_os) == "Linux");
}
