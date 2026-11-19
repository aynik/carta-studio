/**
 * Session persistence, WAV/PNG export, level/clipping preflight, and marker
 * exchange for Carta Studio.
 *
 * Timeline coordinates use half-open `[start, end)` sample ranges at the
 * 44.1 kHz project rate. Project serialization writes `.carta` JSON (versions
 * 1..25) with IEEE-754 hex double precision, SHA-256 source identity verification,
 * and atomic replace/no-replace commits, as well as portable media folders and
 * single-file `CARTAP01` archives. WAV exports enforce classic 32-bit RIFF
 * chunk and 8 MiB cue metadata limits.
 */

#pragma once

#include "Core/Session.h"
#include "IO/CheckedText.h"
#include <functional>
#include <map>
#include <juce_graphics/juce_graphics.h>

namespace carta {

/** Write a complete PNG capture to a new path; never replaces an existing file. */
bool writeNewPng(const juce::Image&, const juce::File&, juce::String& error);

/** Sample format for WAV rendering and export. */
enum class WavEncoding { pcm16, pcm24, float32 };

/**
 * Configuration options for WAV export and export preflight.
 *
 * `sourceChannels` is reserved for validated multichannel consolidation rather
 * than master bus export.
 */
struct WavOptions {
    WavEncoding encoding = WavEncoding::pcm24;
    bool dither = false;
    bool embedMarkers = false;
    bool sourceChannels = false;
};

/** Half-open `[start, end)` sample range at the 44.1 kHz project rate. */
struct WavRange {
    juce::int64 start = 0;
    juce::int64 end = 0;
};

/** Return the half-open `[0, projectEndSample)` range for `session`. */
WavRange fullWavRange(const Session&);

/**
 * Validate output channels, time range, and audible routing without applying
 * a file-format size limit; used by read-only mix analysis.
 *
 * On a track-specific failure, `affectedTrack` receives its zero-based index.
 */
bool validateMixRange(const Session&, int masterChannels, WavRange, juce::String& error,
                      int* affectedTrack = nullptr);

/** Validate mix routing plus RIFF size, dither, and optional cue limits. */
bool validateWavRange(const Session&, int masterChannels, WavRange, juce::String& error,
                      WavOptions options = {});

/** Progress callback receiving a fraction in `[0, 1]`; return false to cancel. */
using ExportProgress = std::function<bool(double)>;

/** Read-only per-channel sample peak, RMS, and full-scale overshoot summary. */
struct MixLevels {
    int channels = 0;
    float peak[2]{};
    double rms[2]{};
    /** First absolute project sample of a nonzero maximum per channel. */
    juce::int64 peakSample[2]{-1, -1};
    /** Count of samples strictly greater than 1.0 in magnitude. */
    juce::int64 aboveFullScale[2]{};
    /** First absolute project sample strictly greater than 1.0 in magnitude. */
    juce::int64 firstAboveFullScale[2]{-1, -1};
};

/**
 * Measure 44.1 kHz sample peak and RMS per output channel.
 *
 * A failed or cancelled scan leaves `result` unchanged. Reported peaks are
 * sample peaks rather than oversampled true peak or LUFS.
 */
bool scanMixLevels(const Session&, int masterChannels, WavRange, MixLevels& result,
                   juce::String& error, ExportProgress progress = {});

/** Pre-export peak and integer-PCM clipping statistics under selected dither. */
struct ExportPreflight {
    int channels = 0;
    float peak[2]{};
    juce::int64 clippedSamples[2]{};
    /** First absolute project sample that would clip when quantized. */
    juce::int64 firstClipped[2]{-1, -1};
};

/**
 * Measure the rendered peak and exact PCM clipping under the selected dither.
 *
 * A failed or cancelled scan leaves `result` unchanged; float32 WAV never clips.
 */
bool scanExportPreflight(const Session&, int masterChannels, WavRange, ExportPreflight&,
                         juce::String& error, ExportProgress progress = {},
                         WavOptions options = {});

/** Scan the maximum absolute sample peak across all output channels. */
bool scanPeak(const Session&, int masterChannels, WavRange, float& peak, juce::String& error,
              ExportProgress progress = {}, WavOptions options = {});

/** Convenience wrapper returning the peak for `WavRange`, or 0 on failure. */
float measurePeak(const Session&, int masterChannels, WavRange);

/** Convenience wrapper returning the peak for the full session range. */
float measurePeak(const Session&, int masterChannels);

/** Return true if both paths are non-empty and refer to the same file. */
bool pathsReferToSameFile(const juce::File&, const juce::File&);

/**
 * Verify that `target` is non-empty and differs from `protectedProject` and
 * every audio source file currently referenced by `session`.
 */
bool validateOutputDestination(const Session&, const juce::File& target,
                               const juce::File& protectedProject, juce::String& error);

/** Render `WavRange` atomically to `target`, optionally honoring `noReplace`. */
bool exportWavRange(const Session&, int masterChannels, WavRange, const juce::File&, bool& clipped,
                    juce::String& error, const juce::File& protectedProject = {},
                    ExportProgress progress = {}, WavOptions options = {}, bool noReplace = false,
                    DirectorySyncStatus* directorySync = nullptr);

/** Render the full master range to `target` using default `WavOptions`. */
bool exportWav(const Session&, int masterChannels, const juce::File&, bool& clipped,
               juce::String& error);

/**
 * Validate authored MiniDisc track program rendering as float32 WAV.
 *
 * Preflight leaves `range` unchanged on failure and enforces RIFF and cue limits.
 */
bool validateDiscProgramWav(const Session&, int masterChannels, WavRange& range,
                            juce::String& error);

/** Render the authored MiniDisc tracks in playback order as 32-bit float WAV. */
bool exportDiscProgramWav(const Session&, int masterChannels, const juce::File&,
                          juce::String& error, const juce::File& protectedProject = {},
                          ExportProgress progress = {}, bool noReplace = false,
                          DirectorySyncStatus* directorySync = nullptr);

/** Named master track region derived from project track markers. */
struct MasterTrack {
    juce::String name;
    WavRange range;
};

/**
 * Build ordered, contiguous master tracks; inserts an implicit first track at 0.
 *
 * Invalid, fractional, unnamed, or duplicate marker boundaries leave the
 * output vector unchanged.
 */
bool getMasterTracks(const Session&, std::vector<MasterTrack>&, juce::String& error);

/** Locate the marker-defined master track containing `sample`. */
bool masterTrackAtSample(const Session&, juce::int64 sample, MasterTrack&, int& index,
                         juce::String& error);

/** Summary returned by batch track WAV export. */
struct TrackBatchResult {
    juce::File directory;
    int completed = 0;
    bool clipped = false;
    DirectorySyncStatus directorySync = DirectorySyncStatus::unsupported;
};

/**
 * Create a new numbered folder under `parent` and write one numbered WAV per
 * marker-defined track, followed by a JSON manifest.
 *
 * Never intentionally replaces existing files. On failure or cancellation,
 * already-completed files remain in `result.directory`.
 */
bool exportTrackWavs(const Session&, int masterChannels, const juce::File& parent,
                     TrackBatchResult&, juce::String& error,
                     const juce::File& protectedProject = {}, ExportProgress progress = {},
                     WavOptions options = {});

/** Export a JSON sidecar describing contiguous, half-open 44.1 kHz master regions. */
bool exportTrackManifest(const Session&, int masterChannels, const juce::File&, juce::String& error,
                         const juce::File& protectedProject = {}, bool noReplace = false);

/** Cooperative cancellation callback for marker and label file exchange. */
using MarkerExchangeContinue = std::function<bool()>;

/**
 * Import portable JSON markers (`carta-markers`) with 44.1 kHz sample positions.
 *
 * Leaves `result` unchanged on any parse, duplicate, or bounds error.
 */
bool readMarkerFile(const Session&, const juce::File&, std::vector<Marker>& result,
                    juce::String& error, MarkerExchangeContinue keepGoing = {});

/** Export sample-grid-aligned markers to a `carta-markers` JSON file. */
bool writeMarkerFile(const Session&, const juce::File&, juce::String& error,
                     const juce::File& protectedProject = {}, bool noReplace = false,
                     MarkerExchangeContinue keepGoing = {});

/**
 * Import Audacity tab-separated point labels.
 *
 * Region and extended/frequency labels are rejected rather than silently
 * dropping information; decimal seconds round to the nearest 44.1 kHz sample.
 * Label names may be empty but must be single-line.
 */
bool readAudacityLabels(const Session&, const juce::File&, std::vector<Marker>& result,
                        juce::String& error, MarkerExchangeContinue keepGoing = {});

/** Export sample-grid-aligned markers as Audacity tab-separated point labels. */
bool writeAudacityLabels(const Session&, const juce::File&, juce::String& error,
                         const juce::File& protectedProject = {}, bool noReplace = false,
                         MarkerExchangeContinue keepGoing = {});

/** Save the session to a `.carta` JSON project file, atomically replacing any existing file. */
bool saveSession(const Session&, int masterChannels, const juce::File&, juce::String& error,
                 LoadProgress progress = {}, DirectorySyncStatus* directorySync = nullptr,
                 WavRange savedLoop = {});

/** Save a `.carta` project copy without replacing an existing destination. */
bool saveSessionCopy(const Session&, int masterChannels, const juce::File&, juce::String& error,
                     LoadProgress progress = {}, DirectorySyncStatus* directorySync = nullptr,
                     WavRange savedLoop = {});

/**
 * Create a new folder containing a `.carta` project and verified,
 * content-deduplicated media copies.
 *
 * The current session is untouched. On failure, `resultDirectory` identifies
 * a partial folder if one was created; existing files are never replaced.
 */
bool exportPortableSession(const Session&, int masterChannels, const juce::File& parent,
                           juce::File& resultDirectory, juce::String& error,
                           LoadProgress progress = {}, WavRange savedLoop = {});

/** Return true if `file` starts with the `CARTAP01` native package magic header. */
bool isNativePackage(const juce::File&);

/** Write a self-contained single-file `CARTAP01` package (`Project.carta` + `Media/`). */
bool saveNativePackage(const Session&, int masterChannels, const juce::File&, juce::String& error,
                       LoadProgress progress = {}, DirectorySyncStatus* directorySync = nullptr,
                       WavRange savedLoop = {}, bool noReplace = false, bool recovery = false);

/** Extract, verify checksums, and load a `CARTAP01` project package. */
bool loadNativePackage(Session&, int& masterChannels, juce::AudioFormatManager&, const juce::File&,
                       juce::String& error, LoadProgress progress = {},
                       WavRange* savedLoop = nullptr, bool* isRecovery = nullptr);

/**
 * Write a recovery snapshot `.carta` project.
 *
 * Recovery snapshots retain imported source hashes without rehashing source
 * files at snapshot time; loading still verifies every referenced source.
 */
bool saveRecoverySession(const Session&, int masterChannels, const juce::File&, juce::String& error,
                         LoadProgress progress = {}, DirectorySyncStatus* directorySync = nullptr,
                         WavRange savedLoop = {});

/** Missing or mismatched source details returned when a relinkable source fails to load. */
struct SourceRelinkRequest {
    juce::File original;
    juce::String expectedHash;
};

/** Map from original absolute source path to replacement file for relinking. */
using SourceRelinks = std::map<juce::String, juce::File>;

/**
 * Load a `.carta` project file and verify all referenced audio sources.
 *
 * Relinks are accepted only when a saved checksum matches the loaded source.
 * On failure, `result`, `masterChannels`, and success flags stay unchanged;
 * `request` is populated only when the failing source has a valid saved SHA-256.
 */
bool loadSession(Session&, int& masterChannels, juce::AudioFormatManager&, const juce::File&,
                 juce::String& error, LoadProgress progress = {}, bool* isRecovery = nullptr,
                 const SourceRelinks* relinks = nullptr, SourceRelinkRequest* request = nullptr,
                 bool* didRelink = nullptr, bool* didAssignMarkerIds = nullptr,
                 WavRange* savedLoop = nullptr);

} // namespace carta
