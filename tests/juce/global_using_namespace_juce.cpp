// The module header must be includable from a TU with a file-scope
// `using namespace juce;`, the way HISE is written. No other TU in this repo has
// one, so without this the system-header name collisions (MacTypes.h's `Point`,
// wingdi.h's `Rectangle()`) are invisible to CI. The compile is the assertion.

#include <juce_gui_basics/juce_gui_basics.h>

using namespace juce;

#include <moonbase_licensing/moonbase_licensing.h>

#include <doctest/doctest.h>

namespace
{
// Unqualified on purpose: anything the system headers leak shows up here.
Point<float> makePoint() { return { 3.0f, 4.0f }; }
Rectangle<int> makeRectangle() { return { 0, 0, 640, 480 }; }

[[maybe_unused]] Component* component = nullptr;
[[maybe_unused]] Timer* timer = nullptr;
} // namespace

TEST_CASE("the module header survives a global using namespace juce")
{
    CHECK(makePoint().getDistanceFromOrigin() == doctest::Approx(5.0f));
    CHECK(makeRectangle().getWidth() == 640);

    String text { "moonbase" };
    MemoryBlock block { text.toRawUTF8(), text.getNumBytesAsUTF8() };
    CHECK(block.getSize() == 8);

    Range<int> range { 2, 7 };
    CHECK(range.getLength() == 5);

    Colour colour = Colours::white;
    CHECK(colour.isOpaque());

    moonbase::juce_integration::ActivationConfig config;
    CHECK(config.resolvedDeviceIdResolver() != nullptr);
}
