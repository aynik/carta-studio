/**
 * Native modal sheet and dialog interfaces for macOS (AppKit) and Linux (GTK3).
 *
 * All functions must be invoked on the JUCE message thread and deliver their
 * completion callbacks asynchronously on the message thread.
 */

#pragma once

#include <functional>
#include <vector>
#include <juce_core/juce_core.h>

namespace juce {
class Component;
class MessageBoxOptions;
} // namespace juce

namespace carta {

/**
 * Present a native unsaved-changes save confirmation sheet.
 *
 * Callback receives `0` for Save, `1` for Discard, or `-1` (Cancel / dismissal).
 */
void showNativeSavePrompt(juce::Component*, std::function<void(int)>);

/**
 * Present a native alert sheet with 1..3 buttons from `MessageBoxOptions`.
 *
 * Callback receives the zero-based button index, or `-1` if no button was chosen.
 */
void showNativeAlert(const juce::MessageBoxOptions&, std::function<void(int)>);

/**
 * Present a native single-line text input sheet.
 *
 * Cancel and parent-window loss invoke the callback with `accepted = false`.
 */
void showNativeTextPrompt(juce::Component*, const juce::String& title, const juce::String& message,
                          const juce::String& label, const juce::String& initial,
                          const juce::String& accept, std::function<void(bool, juce::String)>);

/** Field specification for multi-field native text prompt sheets. */
struct TextPromptField {
    juce::String key;
    juce::String label;
    juce::String initial;
};

/**
 * Present a native sheet with 1..8 labeled single-line fields in order.
 *
 * Callback choice is `1` for `accept`, `2` for non-empty `alternative`, or `0`
 * for cancel/dismissal.
 */
void showNativeTextFieldsPrompt(juce::Component*, const juce::String& title,
                                const juce::String& message,
                                const std::vector<TextPromptField>& fields,
                                const juce::String& accept, const juce::String& alternative,
                                std::function<void(int, juce::StringArray)>);

/**
 * Present a transient GTK3 multi-field text dialog on Linux/X11.
 *
 * Returns false without invoking the callback when GTK/X11 or field count is invalid.
 */
bool showGtkTextFieldsPrompt(juce::Component*, const juce::String& title,
                             const juce::String& message,
                             const std::vector<TextPromptField>& fields, const juce::String& accept,
                             const juce::String& alternative,
                             std::function<void(int, juce::StringArray)>);

/**
 * Present a transient GTK3 alert dialog on Linux/X11.
 *
 * Returns false without invoking the callback when GTK/X11 is unavailable.
 */
bool showGtkAlert(const juce::MessageBoxOptions&, std::function<void(int)>);

/**
 * Present a transient GTK3 file or folder chooser dialog on Linux/X11.
 *
 * Returns false without invoking the callback when GTK/X11 or flags are invalid.
 */
bool showGtkFileChooser(juce::Component*, const juce::String& title, const juce::File& initial,
                        const juce::String& filters, int flags,
                        std::function<void(juce::Array<juce::File>)>);

} // namespace carta
