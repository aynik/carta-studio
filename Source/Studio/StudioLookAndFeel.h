/**
 * @file StudioLookAndFeel.h
 * @brief Shared UI helpers, look-and-feel overrides, alert wrappers, and overview scrollbar.
 *
 * Provides message-thread UI primitives shared by `Studio` and its dialogs:
 * UTF-8 string conversion, half-open loop-range comparison (`sameLoop`),
 * focus-safe overview scrolling (`TimelineOverview`), platform-native alert
 * dispatch (`showStudioAlert`), neutral dark/light `LookAndFeel_V4` overrides
 * (`StudioNeutralLook`), and the audio device settings window.
 */

#pragma once

#include "Core/Session.h"
#include "IO/SessionIO.h"
#include "Platform/NativeSavePrompt.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <cstdint>
#include <functional>

/**
 * @brief Converts a UTF-8 C string literal to a `juce::String`.
 */
inline juce::String utf8Str(const char* text) {
    return juce::String::fromUTF8(text);
}

static_assert(std::atomic<bool>::is_always_lock_free, "Audio transport flags must be lock-free.");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Audio interruption counter must be lock-free.");

/**
 * @brief Returns true if two half-open `[start, end)` sample ranges match exactly.
 */
inline bool sameLoop(carta::WavRange a, carta::WavRange b) noexcept {
    return a.start == b.start && a.end == b.end;
}

/**
 * @brief Horizontal timeline overview scrollbar that swallows editor-bound keys.
 *
 * JUCE bubbles unhandled keys to parent components. At a scroll limit the
 * default scrollbar reports navigation keys as unhandled, which would seek
 * the timeline or change a selection even though the overview has focus.
 */
class TimelineOverview final : public juce::ScrollBar {
  public:
    TimelineOverview() : juce::ScrollBar(false) {}

    bool keyPressed(const juce::KeyPress& key) override {
        const auto code = key.getKeyCode();
        const auto mods = key.getModifiers();
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey ||
            code == juce::KeyPress::upKey || code == juce::KeyPress::downKey ||
            code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey ||
            code == juce::KeyPress::homeKey || code == juce::KeyPress::endKey) {
            if (!mods.isAnyModifierKeyDown()) {
                juce::ScrollBar::keyPressed(key);
            }
            return true;
        }
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey ||
            code == juce::KeyPress::F2Key ||
            (code >= juce::KeyPress::F5Key && code <= juce::KeyPress::F10Key)) {
            return true;
        }
        const auto c = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(code));
        if (!mods.isAnyModifierKeyDown() && c >= 32 && c < 127 &&
            code != juce::KeyPress::spaceKey && c != 'k') {
            return true;
        }
        return juce::ScrollBar::keyPressed(key);
    }
};

/**
 * @brief Displays an alert dialog using native sheets or a JUCE modal window in component tests.
 *
 * Native alerts need a real native parent window. Component tests deliberately
 * run without one, so retain a JUCE modal alert in that harness.
 */
inline void showStudioAlert(const juce::MessageBoxOptions& options,
                            std::function<void(int)> callback = {}) {
#if CARTA_STUDIO_COMPONENT_TEST
    auto* dialog = new juce::AlertWindow(options.getTitle(), options.getMessage(),
                                         options.getIconType(), options.getAssociatedComponent());
    for (int i = 0; i < options.getNumButtons(); ++i) {
        dialog->addButton(options.getButtonText(i), i + 1);
    }
    dialog->enterModalState(
        true, juce::ModalCallbackFunction::create([cb = std::move(callback)](int result) {
            if (cb) {
                cb(result > 0 ? result - 1 : -1);
            }
        }),
        true);
#elif JUCE_MAC
    carta::showNativeAlert(options, std::move(callback));
#elif JUCE_LINUX && CARTA_STUDIO_GTK_PROMPTS
    if (!carta::showGtkAlert(options, callback)) {
        juce::NativeMessageBox::showAsync(options, std::move(callback));
    }
#else
    juce::NativeMessageBox::showAsync(options, std::move(callback));
#endif
}

/**
 * @brief Builds `MessageBoxOptions` for mix-level analysis results with optional overload
 * navigation.
 */
inline juce::MessageBoxOptions levelResultsOptions(juce::Component* parent,
                                                   const juce::String& message, bool hasOverload) {
    auto options = juce::MessageBoxOptions()
                       .withIconType(juce::MessageBoxIconType::InfoIcon)
                       .withTitle("Mix levels")
                       .withMessage(message)
                       .withButton("Close")
                       .withButton("Go to peak")
                       .withAssociatedComponent(parent);
    if (hasOverload) {
        options = options.withButton("Go to first over");
    }
    return options;
}

/**
 * @brief Normalized file-chooser result shared between native pickers and JUCE file dialogs.
 */
struct StudioFileSelection {
    juce::Array<juce::File> files;

    juce::File getResult() const { return files.isEmpty() ? juce::File{} : files.getReference(0); }

    const juce::Array<juce::File>& getResults() const { return files; }
};

/**
 * @brief Neutral monochrome `LookAndFeel_V4` customization supporting dark and light modes.
 */
struct StudioNeutralLook final : juce::LookAndFeel_V4 {
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool over, bool down) override {
        const bool dark = juce::Desktop::getInstance().isDarkModeActive();
        if (over || down || button.getToggleState()) {
            g.setColour(dark ? juce::Colour(0xff404040) : juce::Colour(0xffd2d2d2));
            g.fillRoundedRectangle(button.getLocalBounds().toFloat().reduced(1), 3);
        }
    }

    juce::Font getPopupMenuFont() override { return withDefaultMetrics(juce::FontOptions(12.5f)); }

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override {
        g.fillAll(findColour(juce::PopupMenu::backgroundColourId));
        g.setColour(findColour(juce::ComboBox::outlineColourId).withAlpha(0.55f));
        g.drawRect(0, 0, width, height, 1);
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override {
        return withDefaultMetrics(juce::FontOptions(
            std::min(12.2f, std::max(10.5f, static_cast<float>(box.getHeight()) * 0.56f))));
    }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override {
        label.setBounds(7, 1, std::max(1, box.getWidth() - 25), std::max(1, box.getHeight() - 2));
        label.setFont(getComboBoxFont(box));
        label.setColour(juce::Label::textColourId, box.findColour(juce::ComboBox::textColourId));
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int,
                      juce::ComboBox& box) override {
        const auto cornerSize =
            box.findParentComponentOfClass<juce::ChoicePropertyComponent>() != nullptr ? 0.0f
                                                                                       : 3.0f;
        const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                                   static_cast<float>(height));
        g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle(bounds, cornerSize);
        g.setColour(box.findColour(box.hasKeyboardFocus(true)
                                       ? juce::ComboBox::focusedOutlineColourId
                                       : juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
        const auto arrowZone = juce::Rectangle<float>(static_cast<float>(width - 19), 0.0f, 14.0f,
                                                      static_cast<float>(height));
        juce::Path path;
        path.startNewSubPath(arrowZone.getX() + 3.0f, arrowZone.getCentreY() - 2.0f);
        path.lineTo(arrowZone.getCentreX(), arrowZone.getCentreY() + 2.5f);
        path.lineTo(arrowZone.getRight() - 3.0f, arrowZone.getCentreY() - 2.0f);
        g.setColour(box.findColour(juce::ComboBox::arrowColourId)
                        .withAlpha(box.isEnabled() ? 0.9f : 0.38f));
        g.strokePath(path, juce::PathStrokeType(1.6f));
    }
};

/**
 * @brief Resizable dialog window hosting JUCE's audio output selector.
 */
struct StudioAudioSettingsWindow final : juce::DialogWindow {
    StudioAudioSettingsWindow(juce::AudioDeviceManager& manager, juce::LookAndFeel& look,
                              juce::Component* owner)
        : juce::DialogWindow("Audio output settings",
                             juce::Desktop::getInstance().isDarkModeActive()
                                 ? juce::Colour(0xff252525)
                                 : juce::Colour(0xffeeeeee),
                             true) {
        setLookAndFeel(&look);
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        auto* selector =
            new juce::AudioDeviceSelectorComponent(manager, 0, 0, 1, 2, false, false, true, false);
        selector->setSize(500, 360);
        setContentOwned(selector, true);
        centreAroundComponent(owner, 500, 360);
    }

    ~StudioAudioSettingsWindow() override { setLookAndFeel(nullptr); }

    void closeButtonPressed() override {
        setVisible(false);
        if (onClose) {
            onClose();
        }
    }

    std::function<void()> onClose;
};
