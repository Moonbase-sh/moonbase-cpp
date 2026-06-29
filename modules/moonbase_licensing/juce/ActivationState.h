#pragma once

// A tiny JSON-backed store for client-side state that should survive restarts
// (currently: which app-update versions the user chose to ignore). It lives next
// to the license file and is intentionally generic so future state can be added
// without new files. Unknown keys are preserved on write, so a field written by a
// newer build is not clobbered by an older one.

#include <juce_core/juce_core.h>

namespace moonbase::juce_integration {

class ActivationState
{
public:
    explicit ActivationState(juce::File file) : file_(std::move(file)) { load(); }

    //== Ignored app updates ===================================================
    [[nodiscard]] juce::StringArray ignoredUpdates() const
    {
        juce::StringArray out;
        if (auto* arr = root_.getProperty(kIgnoredUpdates, {}).getArray())
            for (const auto& v : *arr)
                out.add(v.toString());
        return out;
    }

    [[nodiscard]] bool isUpdateIgnored(const juce::String& version) const
    {
        return version.isNotEmpty() && ignoredUpdates().contains(version);
    }

    void ignoreUpdate(const juce::String& version)
    {
        if (version.isEmpty())
            return;
        load(); // read-modify-write so a concurrent instance's entries survive
        if (isUpdateIgnored(version))
            return;
        juce::Array<juce::var> arr;
        if (auto* existing = root_.getProperty(kIgnoredUpdates, {}).getArray())
            arr = *existing;
        arr.add(version);
        setProperty(kIgnoredUpdates, arr);
    }

    //== Generic escape hatch for future state =================================
    [[nodiscard]] juce::var get(const juce::Identifier& key) const
    {
        return root_.getProperty(key, {});
    }

    void set(const juce::Identifier& key, const juce::var& value) { setProperty(key, value); }

private:
    static inline const juce::Identifier kIgnoredUpdates { "ignoredUpdates" };

    void load()
    {
        root_ = juce::var(new juce::DynamicObject());
        if (file_.existsAsFile())
            if (auto parsed = juce::JSON::parse(file_.loadFileAsString());
                parsed.getDynamicObject() != nullptr)
                root_ = parsed;
    }

    void setProperty(const juce::Identifier& key, const juce::var& value)
    {
        if (root_.getDynamicObject() == nullptr)
            root_ = juce::var(new juce::DynamicObject());
        root_.getDynamicObject()->setProperty(key, value);
        save();
    }

    void save() const
    {
        file_.getParentDirectory().createDirectory();
        file_.replaceWithText(juce::JSON::toString(root_)); // pretty, unknown keys kept
    }

    juce::File file_;
    juce::var root_;
};

} // namespace moonbase::juce_integration
