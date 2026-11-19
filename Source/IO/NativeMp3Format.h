/**
 * Optional JUCE AudioFormat registration for gapless-verified MP3 decoding.
 *
 * Backs the "Carta MP3" format with `inspectMp3Gapless` and minimp3 floating
 * point decoding when `CARTA_STUDIO_MINIMP3` is enabled.
 */

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace carta {

/**
 * Register the optional streaming MP3 reader once per AudioFormatManager.
 *
 * No-ops if a format named "Carta MP3" is already registered on `formats`.
 */
void registerNativeMp3Format(juce::AudioFormatManager& formats);

} // namespace carta
