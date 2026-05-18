#include <moonbase/moonbase.hpp>

int main()
{
    moonbase::licensing_options options;
    options.endpoint = "https://example.invalid";
    options.product_id = "product";
    options.public_key = "invalid";
    (void)options;

    return moonbase::to_string(moonbase::platform::linux_os).empty() ? 1 : 0;
}
