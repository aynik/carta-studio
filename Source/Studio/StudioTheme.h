/**
 * @file StudioTheme.h
 * @brief Formal light/dark color system and UI control role tokens for Carta Studio.
 *
 * Design principles:
 * - Pure neutral monochrome chrome (zero blue/teal cast) so UI surfaces,
 *   highlights, active tabs, and selection indicators never clash with
 *   MiniDisc codec colors (SP Stereo, SP Mono, LP2, LP4, Hi-SP, Hi-LP, PCM).
 * - Semantic separation: codec hues represent audio encoding formats only;
 *   on-disc/locked clips use a neutral slate surface + badge rather than teal;
 *   warnings and destructive actions use dedicated amber/crimson tokens.
 * - Explicit control roles distinguish push buttons, primary CTAs, toolbar
 *   controls, tabs, segmented controls, and read-only status chips.
 */

#pragma once

#include "MiniDisc/MdAuthoring.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace carta::theme {

/** Property key on `juce::Button::getProperties()` selecting its visual role. */
inline constexpr const char* kButtonRoleProp = "cartaButtonRole";
inline constexpr const char* kButtonActiveProp = "cartaButtonActive";

/** Recognised values for `kButtonRoleProp`. */
inline constexpr const char* kRoleDefault = "default";
inline constexpr const char* kRolePrimary = "primary";
inline constexpr const char* kRoleDestructive = "destructive";
inline constexpr const char* kRoleToolbar = "toolbar";
inline constexpr const char* kRoleTab = "tab";
inline constexpr const char* kRoleTabBarAction = "tabBarAction";
inline constexpr const char* kRoleSegment = "segment";

inline void setButtonRole(juce::Button& button, const char* role) {
    button.getProperties().set(kButtonRoleProp, juce::String(role));
    button.repaint();
}

inline void setButtonActive(juce::Button& button, bool active) {
    button.getProperties().set(kButtonActiveProp, active);
    button.repaint();
}

inline bool isButtonActive(const juce::Button& button) {
    const auto& props = button.getProperties();
    if (props.contains(kButtonActiveProp)) {
        return static_cast<bool>(props[kButtonActiveProp]);
    }
    return button.getToggleState();
}

/** Complete light/dark semantic palette (pure neutral chrome + semantic status). */
struct Palette {
    // Surfaces (strictly neutral grayscale)
    juce::Colour windowBg;
    juce::Colour panelBg;
    juce::Colour dialogBg;
    juce::Colour menuBg;
    juce::Colour inputBg;

    // Controls (buttons, segmented tracks, combo boxes)
    juce::Colour controlBg;
    juce::Colour controlHoverBg;
    juce::Colour controlDownBg;
    juce::Colour controlBorder;
    juce::Colour controlHoverBorder;
    juce::Colour focusOutline;
    juce::Colour hairline;

    // Neutral monochrome primary accent (zero hue clash with codec colors)
    juce::Colour accentBg;
    juce::Colour accentHoverBg;
    juce::Colour accentText;
    juce::Colour selectionBg;
    juce::Colour selectionText;

    // Typography
    juce::Colour textPrimary;
    juce::Colour textSecondary;
    juce::Colour textMuted;

    // Scrollbar
    juce::Colour scrollThumb;

    // Progress bars with overlaid text (WCAG AA+ contrast in both themes)
    juce::Colour progressTrackBg;
    juce::Colour progressFill;
    juce::Colour meterFill;

    // On-disc / locked timeline clips (neutral charcoal-slate, distinct from SP teal)
    juce::Colour onDiscClipFill;
    juce::Colour onDiscClipSelectedFill;
    juce::Colour onDiscClipBorder;
    juce::Colour onDiscPillBg;
    juce::Colour onDiscPillHoverBg;
    juce::Colour onDiscPillBorder;
    juce::Colour onDiscProgressFill;

    // Timeline MD guide lines (neutral monochrome)
    juce::Colour mdGuideLine;

    // Semantic status colors
    juce::Colour danger;
    juce::Colour dangerBg;
    juce::Colour warning;
    juce::Colour success;
};

inline Palette palette(bool dark) noexcept {
    if (dark) {
        return Palette{
            /* windowBg */ juce::Colour(0xff1e1e1e),
            /* panelBg */ juce::Colour(0xff232323),
            /* dialogBg */ juce::Colour(0xff252525),
            /* menuBg */ juce::Colour(0xff262626),
            /* inputBg */ juce::Colour(0xff222222),
            /* controlBg */ juce::Colour(0xff2e2e2e),
            /* controlHoverBg */ juce::Colour(0xff3a3a3a),
            /* controlDownBg */ juce::Colour(0xff464646),
            /* controlBorder */ juce::Colour(0xff4c4c4c),
            /* controlHoverBorder */ juce::Colour(0xff6a6a6a),
            /* focusOutline */ juce::Colour(0xffa0a0a0),
            /* hairline */ juce::Colour(0xff363636),
            /* accentBg */ juce::Colour(0xffe4e4e4),
            /* accentHoverBg */ juce::Colour(0xfff5f5f5),
            /* accentText */ juce::Colour(0xff171717),
            /* selectionBg */ juce::Colour(0xff4a4a4a),
            /* selectionText */ juce::Colour(0xfff2f2f2),
            /* textPrimary */ juce::Colour(0xffe4e4e4),
            /* textSecondary */ juce::Colour(0xffa8a8a8),
            /* textMuted */ juce::Colour(0xff7c7c7c),
            /* scrollThumb */ juce::Colour(0xff585858),
            /* progressTrackBg */ juce::Colour(0xff1a1a1a),
            /* progressFill */ juce::Colour(0xff474b52),
            /* meterFill */ juce::Colour(0xff7d838d),
            /* onDiscClipFill */ juce::Colour(0xff2c2f34),
            /* onDiscClipSelectedFill */ juce::Colour(0xff3b3f46),
            /* onDiscClipBorder */ juce::Colour(0xff9ea4ae),
            /* onDiscPillBg */ juce::Colour(0xff23262a),
            /* onDiscPillHoverBg */ juce::Colour(0xff34383f),
            /* onDiscPillBorder */ juce::Colour(0xffa8aeb8),
            /* onDiscProgressFill */ juce::Colour(0xff525862),
            /* mdGuideLine */ juce::Colour(0xffb8b8b8),
            /* danger */ juce::Colour(0xffe05a5a),
            /* dangerBg */ juce::Colour(0xff3b2325),
            /* warning */ juce::Colour(0xffd99b26),
            /* success */ juce::Colour(0xff43a865),
        };
    }
    return Palette{
        /* windowBg */ juce::Colour(0xffeeeeee),
        /* panelBg */ juce::Colour(0xffe7e7e7),
        /* dialogBg */ juce::Colour(0xffeeeeee),
        /* menuBg */ juce::Colour(0xfff5f5f5),
        /* inputBg */ juce::Colour(0xffffffff),
        /* controlBg */ juce::Colour(0xfff7f7f7),
        /* controlHoverBg */ juce::Colour(0xffffffff),
        /* controlDownBg */ juce::Colour(0xffe0e0e0),
        /* controlBorder */ juce::Colour(0xffbcbcbc),
        /* controlHoverBorder */ juce::Colour(0xff949494),
        /* focusOutline */ juce::Colour(0xff555555),
        /* hairline */ juce::Colour(0xffd2d2d2),
        /* accentBg */ juce::Colour(0xff262626),
        /* accentHoverBg */ juce::Colour(0xff3c3c3c),
        /* accentText */ juce::Colour(0xffffffff),
        /* selectionBg */ juce::Colour(0xffd6d6d6),
        /* selectionText */ juce::Colour(0xff181818),
        /* textPrimary */ juce::Colour(0xff202020),
        /* textSecondary */ juce::Colour(0xff5c5c5c),
        /* textMuted */ juce::Colour(0xff848484),
        /* scrollThumb */ juce::Colour(0xffb8b8b8),
        /* progressTrackBg */ juce::Colour(0xffe4e4e4),
        /* progressFill */ juce::Colour(0xffc4c8ce),
        /* meterFill */ juce::Colour(0xff6b727c),
        /* onDiscClipFill */ juce::Colour(0xffd9dde2),
        /* onDiscClipSelectedFill */ juce::Colour(0xffc5cbd3),
        /* onDiscClipBorder */ juce::Colour(0xff525862),
        /* onDiscPillBg */ juce::Colour(0xfff2f4f7),
        /* onDiscPillHoverBg */ juce::Colour(0xffffffff),
        /* onDiscPillBorder */ juce::Colour(0xff525862),
        /* onDiscProgressFill */ juce::Colour(0xffb2b9c4),
        /* mdGuideLine */ juce::Colour(0xff5a5a5a),
        /* danger */ juce::Colour(0xffc4343c),
        /* dangerBg */ juce::Colour(0xfffbe7e8),
        /* warning */ juce::Colour(0xffb4690e),
        /* success */ juce::Colour(0xff15803d),
    };
}

/** Formal MiniDisc codec palette — mutually distinct hues, WCAG-AA contrast. */
inline juce::Colour codecColour(MdCodecMode mode, bool dark) noexcept {
    switch (mode) {
    case MdCodecMode::spStereo:
        return dark ? juce::Colour(0xff2d9b8f) : juce::Colour(0xff0f766e);
    case MdCodecMode::spMono:
        return dark ? juce::Colour(0xff5f78d8) : juce::Colour(0xff3b55c4);
    case MdCodecMode::lp2:
        return dark ? juce::Colour(0xffd48a29) : juce::Colour(0xffb45309);
    case MdCodecMode::lp4:
        return dark ? juce::Colour(0xffd45b68) : juce::Colour(0xffbe123c);
    case MdCodecMode::hiSp:
        return dark ? juce::Colour(0xff3889cc) : juce::Colour(0xff1d5bbf);
    case MdCodecMode::hiLp:
        return dark ? juce::Colour(0xff9768db) : juce::Colour(0xff6d28d9);
    case MdCodecMode::pcm:
        return dark ? juce::Colour(0xff43a865) : juce::Colour(0xff15803d);
    }
    return dark ? juce::Colour(0xff2d9b8f) : juce::Colour(0xff0f766e);
}

/** Rotating 8-colour group accent palette. */
inline juce::Colour groupColour(int groupIndex, bool dark) noexcept {
    static constexpr juce::uint32 darkPalette[] = {0xff4c85d6, 0xff2fa67a, 0xffd9932b, 0xffd46190,
                                                   0xff8f6ad6, 0xff2b9eb3, 0xffd9733b, 0xff7da832};
    static constexpr juce::uint32 lightPalette[] = {0xff2557a7, 0xff0d7a56, 0xffa8640b, 0xffad2560,
                                                    0xff6338b8, 0xff0e7085, 0xffb34b14, 0xff4b7312};
    const int slot = ((groupIndex % 8) + 8) % 8;
    return juce::Colour(dark ? darkPalette[slot] : lightPalette[slot]);
}

} // namespace carta::theme
