#pragma once

// The animation seam. juce_animation (juce::Animator / ValueAnimatorBuilder /
// Easings) is JUCE 8.0.4+, so the module treats it as an optional dependency and
// falls back to an equivalent implementation on JUCE 6 and 7.
//
// The fallback is not an approximation: it uses the same cubic-bezier control
// points as juce::Easings, the same unit-bezier solve, and the same progress
// semantics (progress = elapsed / duration, easing applied to raw progress,
// infinite animations climb past 1.0 without wrapping). Either way the tick
// source is the owner's juce::Timer, which is how ActivationComponent already
// drove the JUCE 8 animators.

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "../JuceCompat.h"

#if MOONBASE_JUCE_HAS_ANIMATION
  #include <juce_animation/juce_animation.h>
#else
  #include <cmath>
#endif

namespace moonbase::juce_integration::anim {

using Easing = std::function<float(float)>;

//==============================================================================
namespace easings {

#if MOONBASE_JUCE_HAS_ANIMATION

inline Easing createLinear()         { return juce::Easings::createLinear(); }
inline Easing createEase()           { return juce::Easings::createEase(); }
inline Easing createEaseOut()        { return juce::Easings::createEaseOut(); }
inline Easing createEaseOutBack()    { return juce::Easings::createEaseOutBack(); }
inline Easing createEaseInOutCubic() { return juce::Easings::createEaseInOutCubic(); }

#else

// juce::Easings::createCubicBezier is Chromium's CubicBezier. This mirrors it,
// including the linear extrapolation by end-point gradient that it uses outside
// [0, 1] (which is where an infinitely running animation ends up), so the curves
// are identical whichever backend a build picked.
inline Easing createCubicBezier(float p1x, float p1y, float p2x, float p2y)
{
    const double cx = 3.0 * p1x;
    const double bx = 3.0 * (p2x - p1x) - cx;
    const double ax = 1.0 - cx - bx;

    const double cy = 3.0 * p1y;
    const double by = 3.0 * (p2y - p1y) - cy;
    const double ay = 1.0 - cy - by;

    const double startGradient = p1x > 0.0                      ? p1y / p1x
                               : (p1y == 0.0f && p2x > 0.0f)    ? p2y / p2x
                               : (p1y == 0.0f && p2y == 0.0f)   ? 1.0
                                                                : 0.0;

    const double endGradient = p2x < 1.0                        ? (p2y - 1.0) / (p2x - 1.0)
                             : (p2y == 1.0f && p1x < 1.0f)      ? (p1y - 1.0) / (p1x - 1.0)
                             : (p2y == 1.0f && p1y == 1.0f)     ? 1.0
                                                                : 0.0;

    return [ax, bx, cx, ay, by, cy, startGradient, endGradient](float v)
    {
        const double x = (double) v;

        if (x < 0.0)
            return (float) (startGradient * x);

        if (x > 1.0)
            return (float) (1.0 + endGradient * (x - 1.0));

        const auto sampleX = [&](double t) { return ((ax * t + bx) * t + cx) * t; };
        const auto sampleY = [&](double t) { return ((ay * t + by) * t + cy) * t; };
        const auto derivativeX = [&](double t) { return (3.0 * ax * t + 2.0 * bx) * t + cx; };

        constexpr double epsilon = 1.0e-7;
        double t = x;

        // Newton's method first, bisection as the reliable fallback.
        for (int i = 0; i < 8; ++i)
        {
            const double error = sampleX(t) - x;

            if (std::abs(error) < epsilon)
                return (float) sampleY(t);

            const double derivative = derivativeX(t);

            if (std::abs(derivative) < epsilon)
                break;

            t -= error / derivative;
        }

        double low = 0.0, high = 1.0;
        t = x;

        while (low < high)
        {
            const double sampled = sampleX(t);

            if (std::abs(sampled - x) < epsilon)
                break;

            if (x > sampled)
                low = t;
            else
                high = t;

            const double next = (high + low) * 0.5;

            if (std::abs(next - t) < 1.0e-12)
                break;

            t = next;
        }

        return (float) sampleY(t);
    };
}

// Control points lifted from juce_Easings.cpp so the curves match on every JUCE.
inline Easing createLinear()         { return [](float v) { return v; }; }
inline Easing createEase()           { return createCubicBezier(0.25f, 0.1f,  0.25f, 1.0f); }
inline Easing createEaseOut()        { return createCubicBezier(0.0f,  0.0f,  0.58f, 1.0f); }
inline Easing createEaseOutBack()    { return createCubicBezier(0.34f, 1.56f, 0.64f, 1.0f); }
inline Easing createEaseInOutCubic() { return createCubicBezier(0.65f, 0.0f,  0.35f, 1.0f); }

#endif

} // namespace easings

//==============================================================================
#if !MOONBASE_JUCE_HAS_ANIMATION
namespace detail {

// Mirrors juce::ValueAnimator: progress is elapsed/duration, the easing is
// applied to raw progress, and the value callback sees exactly 1.0 on the frame
// that finishes, just before onComplete.
struct AnimationState
{
    double durationMs = 300.0;
    Easing easing = easings::createEase();
    std::function<void(float)> onValueChanged;
    std::function<void()> onComplete;
    bool infinite = false;

    bool running = false;
    bool pendingStart = false;
    double startedAtMs = 0.0;

    // Deferred, like juce::Animator: the start time is taken from the first
    // update() after this, so it is always on the updater's timeline rather than
    // whatever clock the caller happened to be holding.
    void start()
    {
        pendingStart = true;
        running = true;
    }

    void complete()
    {
        pendingStart = false;

        if (! std::exchange(running, false))
            return;

        if (onComplete)
            onComplete();
    }

    void update(double nowMs)
    {
        if (! running)
            return;

        if (std::exchange(pendingStart, false))
            startedAtMs = nowMs;

        auto progress = durationMs > 0.0 ? (float) ((nowMs - startedAtMs) / durationMs) : 1.0f;
        const bool finished = ! infinite && progress >= 1.0f;

        if (finished)
        {
            progress = 1.0f;
            running = false;
        }

        if (onValueChanged)
            onValueChanged(easing ? easing(progress) : progress);

        if (finished && onComplete)
            onComplete();
    }
};

} // namespace detail
#endif

//==============================================================================
// A started, updatable animation. Copyable handle semantics, like juce::Animator.
class Animation
{
public:
    void start()
    {
#if MOONBASE_JUCE_HAS_ANIMATION
        animator.start();
#else
        state->start();
#endif
    }

    // Halts the animation and fires the on-complete callback, exactly as
    // juce::Animator::complete() does. The only way to stop an infinite one.
    void complete()
    {
#if MOONBASE_JUCE_HAS_ANIMATION
        animator.complete();
#else
        state->complete();
#endif
    }

private:
    friend class AnimationBuilder;
    friend class Updater;

#if MOONBASE_JUCE_HAS_ANIMATION
    explicit Animation(juce::Animator a) : animator(std::move(a)) {}
    juce::Animator animator;
#else
    explicit Animation(std::shared_ptr<detail::AnimationState> s) : state(std::move(s)) {}
    std::shared_ptr<detail::AnimationState> state;
#endif
};

//==============================================================================
// Same immutable-builder shape as juce::ValueAnimatorBuilder, so call sites read
// identically on either path.
class AnimationBuilder
{
public:
    [[nodiscard]] AnimationBuilder withDurationMs(double ms) const
    {
        auto copy = *this;
#if MOONBASE_JUCE_HAS_ANIMATION
        copy.builder = copy.builder.withDurationMs(ms);
#else
        copy.state.durationMs = ms;
#endif
        return copy;
    }

    [[nodiscard]] AnimationBuilder withEasing(Easing easing) const
    {
        auto copy = *this;
#if MOONBASE_JUCE_HAS_ANIMATION
        copy.builder = copy.builder.withEasing(std::move(easing));
#else
        copy.state.easing = std::move(easing);
#endif
        return copy;
    }

    [[nodiscard]] AnimationBuilder withValueChangedCallback(std::function<void(float)> fn) const
    {
        auto copy = *this;
#if MOONBASE_JUCE_HAS_ANIMATION
        copy.builder = copy.builder.withValueChangedCallback(std::move(fn));
#else
        copy.state.onValueChanged = std::move(fn);
#endif
        return copy;
    }

    [[nodiscard]] AnimationBuilder withOnCompleteCallback(std::function<void()> fn) const
    {
        auto copy = *this;
#if MOONBASE_JUCE_HAS_ANIMATION
        copy.builder = copy.builder.withOnCompleteCallback(std::move(fn));
#else
        copy.state.onComplete = std::move(fn);
#endif
        return copy;
    }

    // Progress keeps climbing past 1.0 instead of wrapping; the caller decides
    // what a cycle means.
    [[nodiscard]] AnimationBuilder runningInfinitely() const
    {
        auto copy = *this;
#if MOONBASE_JUCE_HAS_ANIMATION
        copy.builder = copy.builder.runningInfinitely();
#else
        copy.state.infinite = true;
#endif
        return copy;
    }

    [[nodiscard]] Animation build() const
    {
#if MOONBASE_JUCE_HAS_ANIMATION
        return Animation(builder.build());
#else
        return Animation(std::make_shared<detail::AnimationState>(state));
#endif
    }

private:
#if MOONBASE_JUCE_HAS_ANIMATION
    juce::ValueAnimatorBuilder builder;
#else
    detail::AnimationState state;
#endif
};

//==============================================================================
// Ticks every registered animation. Drive it from a juce::Timer (or from a test
// with explicit timestamps).
class Updater
{
public:
    void addAnimation(const Animation& animation)
    {
#if MOONBASE_JUCE_HAS_ANIMATION
        updater.addAnimator(animation.animator);
#else
        animations.push_back(animation.state);
#endif
    }

    void update() { update(juce::Time::getMillisecondCounterHiRes()); }

    void update(double nowMs)
    {
#if MOONBASE_JUCE_HAS_ANIMATION
        updater.update(nowMs);
#else
        // By index, re-reading size(): this runs 60 times a second, so it must not
        // allocate, and a completion callback is allowed to add an animation (which
        // can reallocate the vector underneath us). Nothing ever removes one.
        for (std::size_t i = 0; i < animations.size(); ++i)
            animations[i]->update(nowMs);
#endif
    }

private:
#if MOONBASE_JUCE_HAS_ANIMATION
    juce::AnimatorUpdater updater;
#else
    std::vector<std::shared_ptr<detail::AnimationState>> animations;
#endif
};

} // namespace moonbase::juce_integration::anim
