#include <moonbase_licensing/moonbase_licensing.h>

int main()
{
    moonbase::juce_integration::ActivationConfig config;
    config.endpoint = "https://example.invalid";
    config.productId = "product";
    config.publicKey = "invalid";

    return config.endpoint.isEmpty() ? 1 : 0;
}
