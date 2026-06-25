#pragma once

// Click-free audio gate for license enforcement. Hold one in your processor and
// call process() from processBlock, passing the licensed flag (e.g.
// controller.licensedFlag().load()). It ramps the buffer to silence when
// unlicensed (and back up when licensed) over a short fade, so activating or
// deactivating mid-stream doesn't pop. Pure float math: needs no audio modules,
// and keeps gating in your hands (the module never silences audio itself).
//
//   gate.prepare (sampleRate);
//   gate.reset (controller.licensedFlag().load()); // optional: avoid a first-block fade
//   ...
//   gate.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
//                 buffer.getNumSamples(), controller.licensedFlag().load());

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration {

class LicenseGate
{
public:
    // Call from prepareToPlay. fadeMs is the activate/deactivate ramp length.
    void prepare(double sampleRate, double fadeMs = 8.0) noexcept
    {
        const auto fadeSamples = juce::jmax(1.0, sampleRate * (fadeMs / 1000.0));
        step_ = (float) (1.0 / fadeSamples);
    }

    // Snap fully open/closed with no ramp (e.g. from prepareToPlay, once the
    // initial license state is known, to skip a fade-in on the first block).
    void reset(bool licensed) noexcept { gain_ = licensed ? 1.0f : 0.0f; }

    // Apply gating in place on the audio thread. Fully licensed: a no-op. Fully
    // unlicensed: the buffer is cleared. In between: a per-sample ramp.
    void process(float* const* channels, int numChannels, int numSamples, bool licensed) noexcept
    {
        const float target = licensed ? 1.0f : 0.0f;

        if (juce::exactlyEqual(gain_, target))
        {
            if (juce::exactlyEqual(target, 0.0f))
                for (int ch = 0; ch < numChannels; ++ch)
                    juce::zeromem(channels[ch], sizeof(float) * (size_t) numSamples);
            return; // target == 1: pass through untouched
        }

        for (int n = 0; n < numSamples; ++n)
        {
            gain_ = gain_ < target ? juce::jmin(target, gain_ + step_)
                                   : juce::jmax(target, gain_ - step_);
            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][n] *= gain_;
        }
    }

    [[nodiscard]] float currentGain() const noexcept { return gain_; }

private:
    float gain_ = 0.0f; // starts closed: fail safe until the license is confirmed
    float step_ = 0.01f;
};

} // namespace moonbase::juce_integration
