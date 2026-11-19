/**
 * Timeline editing operations, selection geometry, undo history, and marker tools.
 *
 * Coordinates & invariants:
 * - Discrete selection and sample-edit bounds (`TimeSelection`, `QuietRegion`,
 *   `OverlapRegion`) use half-open `[start, end)` intervals in 44.1 kHz
 *   (`sessionRate`) project samples below 2^53 (`9007199254740991LL`).
 * - Floating-point timeline positions preserve intentional sub-sample offsets
 *   while normalizing near-integer project-sample boundaries within `1e-7` frames.
 * - Mutating edits validate geometry and marker invariants (`preservesMarkerBounds`)
 *   on candidate copies before committing; failures leave `Session`, `Clipboard`,
 *   and optional output arguments unchanged.
 *
 * Ownership & threading: value-model transformations run on the message thread;
 * long-running analysis/verification routines (`snapSelectionToZeroCrossings`,
 * `verifyClipConsolidation`, `findClipSourceSilence`, `findQuietRegions`) accept
 * `LoadProgress` callbacks for cooperative off-thread cancellation.
 */
#pragma once
#include "Core/Session.h"
#include <initializer_list>
#include <unordered_set>

namespace carta {

// =========================================================================
// Numeric Input & Viewport Navigation
// =========================================================================

/** Result of parsing an edited numeric field while preserving untouched stored floats. */
struct EditedFloat {
    float value = 0;
    bool edited = false;
};

/**
 * Parse a displayed numeric field without quantizing an untouched stored value.
 *
 * `scale` converts display units to the stored float; failed parsing leaves `output` unchanged.
 */
bool parseEditedFloat(const juce::String& input, const juce::String& displayed, float original,
                      double minimum, double maximum, double scale, EditedFloat& output);

/** Visible horizontal timeline window in seconds. */
struct TimelineViewport {
    double start = 0;
    double span = 0;
};

/** A navigation-only pair of views; changes to project duration are clamped. */
class ZoomMemory {
  public:
    bool remember(TimelineViewport) noexcept;
    bool toggle(TimelineViewport& current, double fullLength) noexcept;
    bool available() const noexcept { return present; }
    void clear() noexcept { present = false; }

  private:
    TimelineViewport previous{};
    bool present = false;
};

// =========================================================================
// Time Selection, Boundaries & Bookmarks
// =========================================================================

/** Timeline ranges are half-open `[start, end)`, in 44.1 kHz project samples. */
struct TimeSelection {
    juce::int64 start = 0;
    juce::int64 end = 0;
    int firstTrack = 0;
    int lastTrack = -1;

    bool hasRange() const noexcept {
        return start >= 0 && end > start && firstTrack >= 0 && lastTrack >= firstTrack;
    }

    bool hasPoint() const noexcept {
        return start >= 0 && end == start && firstTrack >= 0 && lastTrack == firstTrack;
    }
};

/**
 * A navigation-only selection bookmark.
 *
 * Geometry-stable edits may rebase it; other edits invalidate it because track
 * indices have no persistent identity.
 */
struct SelectionMemory {
    TimeSelection selection;
    juce::uint64 generation = 0;
    bool present = false;

    bool remember(const TimeSelection&, int trackCount, juce::uint64 currentGeneration) noexcept;
    bool canRestore(int trackCount, juce::uint64 currentGeneration) const noexcept;
    bool restore(TimeSelection&, int trackCount, juce::uint64 currentGeneration) const noexcept;
    void retainAfterGeometryStableEdit(juce::uint64 previousGeneration,
                                       juce::uint64 nextGeneration) noexcept;
};

/**
 * Construct a half-open selection without overflowing `start + length`.
 *
 * Invalid sample bounds or track indices leave the result unchanged.
 */
bool selectionFromLength(juce::int64 start, juce::int64 length, int firstTrack, int lastTrack,
                         juce::int64 limit, int trackCount, TimeSelection& result) noexcept;

/** Target landmarks for directional boundary selection commands. */
enum class SelectionBoundary {
    masterStart,
    masterEnd,
    previousMarker,
    nextMarker,
    previousClip,
    nextClip
};

/**
 * Select between a project-sample playhead and a strictly previous/next boundary.
 *
 * The default scope selects all rows; clip edges use that scope.
 * Invalid positions, scopes and empty ranges leave `result` intact.
 */
bool selectToBoundary(const Session&, juce::int64 playhead, SelectionBoundary,
                      TimeSelection& result, TrackRange scope = {}) noexcept;

/** Variant of `selectToBoundary` restricted to explicit track indices. */
bool selectToBoundaryOnTracks(const Session&, juce::int64 playhead, SelectionBoundary,
                              const std::vector<int>& tracks, TimeSelection& result);

/**
 * Select the largest unoccupied interval containing a sample on one track.
 *
 * Clip occupancy uses project samples, regardless of mute or sample levels.
 */
bool selectClipGap(const Session&, int track, juce::int64 sample, TimeSelection& result) noexcept;

/**
 * Intersect clip-free gaps on distinct tracks.
 *
 * Rejects any occupied track; the returned row span encloses the supplied
 * indices, which remain the scope.
 */
bool selectClipGapOnTracks(const Session&, const std::vector<int>& tracks, juce::int64 sample,
                           TimeSelection& result);

/**
 * Select the half-open marker-defined master track containing the playhead.
 *
 * Default scope means all rows. Failures preserve the output selection.
 */
bool selectMasterTrackAtSample(const Session&, juce::int64 playhead, TimeSelection& result,
                               juce::String& error, TrackRange scope = {});

// =========================================================================
// Time Formatting, Parsing & Accessibility Summaries
// =========================================================================

/** Human-readable nonnegative project sample position, truncated to milliseconds. */
juce::String formatProjectTime(juce::int64 sample);

/** Display unit mode for timeline readouts and ruler ticks. */
enum class TimelineTimeFormat { automatic, samples, clock, seconds };

/** Selected readout format; time formats use microseconds to distinguish samples. */
juce::String formatTimelineTime(juce::int64 sample, TimelineTimeFormat format);

/**
 * Parse samples (optionally suffixed `"smp"`), decimal seconds suffixed `"s"`,
 * or `mm:ss[.fraction]` / `hh:mm:ss[.fraction]`.
 *
 * Time values round to the nearest 44.1 kHz sample, ties away from zero.
 */
bool parseSampleOrTime(const juce::String&, juce::int64& result);

/**
 * Describe one marker's position and repair issues against the known master end.
 */
juce::String describeMarkerPosition(const Session&, int marker, juce::int64 masterEnd);

/** Textual summary of the current timeline focus for assistive technologies. */
juce::String describeTimeline(const Session&, const TimeSelection&, int track, int clip, int marker,
                              juce::int64 playhead, bool playing,
                              const SourceVerificationCache* checks = nullptr,
                              bool independentTrackScope = false);

/** Concise keyboard-navigation description of a focused track and optional clip. */
juce::String describeNavigationTarget(const Session&, int track, int clip,
                                      const SourceVerificationCache* checks = nullptr);

/** Row model for accessible track/clip navigation lists (`clip == -1` denotes a track header). */
struct NavigationRow {
    int track = -1;
    int clip = -1;
    juce::String label;
    juce::String description;
    bool hasSampleRange = false;
};

/**
 * Track headers followed by valid clips in chronological order, including
 * coincident clips. Indices refer to the current, unmodified session.
 */
std::vector<NavigationRow> makeNavigationRows(const Session&,
                                              const SourceVerificationCache* checks = nullptr);

/** Multi-selection actions in the accessible track/clip browser. */
enum class BrowserSelectionAction { toggle, all, track, clear };

/** Selection contains clip rows only. Invalid cursor actions leave it unchanged. */
bool updateBrowserClipSelection(const std::vector<NavigationRow>&, int cursor,
                                BrowserSelectionAction, juce::SparseSet<int>&);

// =========================================================================
// Timeline Viewport, Ruler & Drag Geometry
// =========================================================================

/**
 * Track indices with rows intersecting a viewport below the ruler.
 *
 * The end is exclusive; the first visible row may be clipped at the viewport bottom.
 */
juce::Range<int> visibleTrackRows(const Session&, int firstTrack, int viewportHeight) noexcept;

/**
 * Map a time to a horizontal pixel, saturating remote geometry before any
 * integer conversion. Invalid viewports return the left edge.
 */
int timelineTimeToPixel(double time, double viewStart, double viewDuration, int left,
                        int width) noexcept;

/** Non-overlapping horizontal layout bounds for one marker label. */
struct MarkerLabelRegion {
    int marker = -1;
    juce::Range<int> bounds;
};

/**
 * Marker coordinates are in index order. Labels never overlap; the selected
 * marker is reserved first. Invalid or offscreen coordinates are ignored.
 */
std::vector<MarkerLabelRegion> layoutMarkerLabels(const std::vector<int>& positions,
                                                  juce::Range<int> viewport, int selectedMarker);

/** Sample-aligned ruler tick interval and tick sample positions for one viewport. */
struct RulerTicks {
    juce::int64 interval = 0;
    std::vector<juce::int64> samples;
};

/** Well-spaced project-sample-aligned ticks for a viewport; invalid inputs yield none. */
RulerTicks makeRulerTicks(double startSeconds, double spanSeconds, int widthPixels);

/** Format a single ruler tick at `sample` appropriate for the tick `interval`. */
juce::String formatRulerTick(juce::int64 sample, juce::int64 interval,
                             TimelineTimeFormat format = TimelineTimeFormat::automatic);

/**
 * Page the viewport only when the playhead moves outside its safe area.
 *
 * Invalid geometry leaves the original start unchanged; return is within the timeline.
 */
double followPlayheadViewport(double start, double span, double fullLength, double playhead,
                              bool pinned = false) noexcept;

/** Scroll horizontally while a selection drag is held near or outside the view edge. */
double selectionEdgeScroll(double start, double span, double fullLength, int left, int right,
                           int pointer, double elapsedSeconds) noexcept;

/**
 * Convert a drag to timeline movement, including viewport scrolling and
 * clamping a pointer outside the horizontal content area to its edge.
 */
double clipDragDelta(int pointer, int anchor, int left, int right, double viewStart,
                     double initialViewStart, double span) noexcept;

/** Direction (`-1`, `0`, `+1`) for row-wise selection scrolling at vertical viewport edges. */
int selectionTrackScrollDirection(int top, int bottom, int pointer, int firstTrack, int trackCount,
                                  int lastTrackBottom) noexcept;

// =========================================================================
// Selection Gestures, Zero Crossings & Overlap Analysis
// =========================================================================

/**
 * Resize either endpoint around its opposite edge.
 *
 * Crossing reverses the range; dragging into another track extends the original
 * contiguous track range.
 */
TimeSelection resizeTimeSelection(const TimeSelection&, bool leftEdge, juce::int64 sample,
                                  int track);

/** Target boundary or body for sample nudging. */
enum class SelectionPart { start, end, whole };

/**
 * Move one boundary or the whole range by an exact number of project samples.
 *
 * Reject moves outside `[0, limit]` or those that would collapse the range.
 */
bool nudgeTimeSelection(TimeSelection&, SelectionPart, juce::int64 delta,
                        juce::int64 limit) noexcept;

/**
 * Stateful keyboard extension retains an anchor across collapse and reversal.
 *
 * If the selection or playhead changes independently, the next step starts a
 * new gesture. The limit and all positions are in project samples.
 */
struct KeyboardRangeExtension {
    juce::int64 anchor = 0;
    juce::int64 focus = 0;
    int firstTrack = 0;
    int lastTrack = -1;
    bool active = false;
    bool independent = false;
};

/** Step-extend or contract a time selection with keyboard navigation. */
bool extendTimeSelection(TimeSelection&, double& playhead, int selectedTrack, int trackCount,
                         int direction, juce::int64 limit, KeyboardRangeExtension&,
                         bool independentPlayback = false) noexcept;

/**
 * Extend or contract a contiguous selection across track rows, preserving its
 * time range and a stable opposite-row anchor until the selection changes.
 */
struct KeyboardTrackExtension {
    int anchor = -1;
    int focus = -1;
    int first = -1;
    int last = -1;
    juce::int64 start = 0;
    juce::int64 end = 0;
    bool active = false;
};

/** Step-extend or contract a selection vertically across contiguous track rows. */
bool extendSelectionTracks(TimeSelection&, int selectedTrack, int trackCount, int direction,
                           KeyboardTrackExtension&) noexcept;

/**
 * The project-sample interval containing sample timestamps within this clip.
 *
 * End is exclusive. Returns false if it contains no project samples.
 */
bool clipSampleRange(const Clip&, juce::int64& start, juce::int64& end) noexcept;

/** Intersect a clip with a half-open timeline interval, selecting contained project sample
 * timestamps. */
bool clipSampleRangeWithin(const Clip&, double begin, double end, juce::int64& start,
                           juce::int64& finish) noexcept;

/**
 * Find low-amplitude zero crossings in a rendered selected-track mix around both endpoints.
 *
 * Search is bounded by `radius` project samples. The caller isolates selected
 * tracks before calling. Failure leaves `result` unchanged.
 */
bool snapSelectionToZeroCrossings(const Session&, int masterChannels, const TimeSelection&,
                                  juce::int64 radius, TimeSelection& result, juce::String& error,
                                  LoadProgress progress = {}, bool requireAllChannels = false);

/** Half-open same-track clip overlap span in project samples. */
struct OverlapRegion {
    int track = -1;
    juce::int64 start = 0;
    juce::int64 end = 0;
};

/** Regions containing project samples, ordered by time then track. */
std::vector<OverlapRegion> overlapRegions(const Session&);

/**
 * Find the strictly previous/next project-sample clip edge on one track, or all
 * tracks when `track` is `-1`. Position may be between project samples.
 */
bool adjacentClipBoundary(const Session&, int track, double position, bool next,
                          juce::int64& sample) noexcept;

/**
 * Traverse every valid clip on a track in start/end/index order, including
 * coincident overlaps. `current = -1` selects the first/last clip. No wrapping.
 */
bool adjacentClipIndex(const Track&, int current, bool next, int& result) noexcept;

/** Toggle flags controlling which landmarks are included by `timelineSnapTargets`. */
struct SnapTargetOptions {
    bool clipEdges = true;
    bool markers = true;
    bool selection = true;
    bool playhead = true;
    bool timelineStart = true;
};

/**
 * Collect timeline boundaries.
 *
 * Excluded indices identify the item being dragged; selection and marker targets
 * can be omitted to avoid self-snapping.
 */
std::vector<double> timelineSnapTargets(const Session&, const TimeSelection&, double playhead,
                                        int excludedTrack = -1, int excludedClip = -1,
                                        bool includeMarkers = true, bool includeSelection = true,
                                        bool projectSampleTargets = false,
                                        SnapTargetOptions options = {});

// =========================================================================
// Editor State, Undo History & Clipboard
// =========================================================================

/** Transient index reference to one clip in a `Session` snapshot. */
struct ClipReference {
    int track = -1;
    int clip = -1;
};

/** A history entry includes the editing context as well as the project data. */
struct EditorState {
    Session session;
    TimeSelection selection;
    int selectedTrack = -1;
    int selectedClip = -1;
    int selectedMarker = -1;
    int masterChannels = 2;
    double position = 0;
    std::vector<ClipReference> selectedClips;
    std::vector<int> selectedTracks;
};

struct Clipboard;

/** Undo/redo navigation target identifying a revision and human-readable label. */
struct HistoryTarget {
    juce::uint64 revision = 0;
    juce::String action;
};

/**
 * Bounded undo/redo history with metadata and retained-media memory budgets.
 */
class EditHistory {
  public:
    explicit EditHistory(size_t metadataLimitBytes = 64u * 1024u * 1024u)
        : metadataLimit(metadataLimitBytes) {}

    void reset() noexcept;
    void record(EditorState before, juce::String action = "Edit",
                bool renderGeometryStable = false);

    /** A recovered project is unsaved, even though it has no undo history yet. */
    void markRecovered() noexcept { currentRevision = nextRevision++; }

    bool undo(EditorState& current, bool* renderGeometryStable = nullptr);
    bool redo(EditorState& current, bool* renderGeometryStable = nullptr);
    juce::uint64 revision() const noexcept { return currentRevision; }
    void markSaved() noexcept { savedRevision = currentRevision; }
    void markSaved(juce::uint64 revision) noexcept { savedRevision = revision; }
    bool isDirty() const noexcept { return currentRevision != savedRevision; }
    bool canUndo() const noexcept { return !undoStack.empty(); }
    bool canRedo() const noexcept { return !redoStack.empty(); }

    juce::String undoAction() const {
        return undoStack.empty() ? juce::String() : undoStack.back().action;
    }

    juce::String redoAction() const {
        return redoStack.empty() ? juce::String() : redoStack.back().action;
    }

    /** Targets are nearest first and identify the revision reached by each step. */
    std::vector<HistoryTarget> targets(bool redo) const;

    /** Snapshot metadata only; shared decoded audio and allocator overhead are excluded. */
    size_t estimatedMetadataBytes() const noexcept;

    /**
     * Estimate unique decoded media and waveform cache payload referenced by
     * history but not by the current session or clipboard. Mapped PCM storage
     * and allocator overhead are excluded.
     */
    size_t estimatedRetainedMediaBytes(const Session&, const Clipboard&) const;

    /** Estimated temporary PCM file bytes, deduplicated by backing storage. */
    juce::uint64 estimatedRetainedMappedBytes(const Session&, const Clipboard&) const;

    void enforceRetainedMediaBudget(const Session&, const Clipboard&,
                                    size_t limitBytes = 256u * 1024u * 1024u,
                                    juce::uint64 mappedLimitBytes = 4ULL * 1024ULL * 1024ULL *
                                                                    1024ULL);

    bool takeBudgetPruned() noexcept {
        const bool result = budgetPruned;
        budgetPruned = false;
        return result;
    }

    std::vector<Session> undoSessions() const;
    std::vector<Session> redoSessions() const;

  private:
    // Pointers are owned by clips in state and remain valid for this entry.
    struct Entry {
        EditorState state;
        juce::uint64 revision = 0;
        size_t bytes = 0;
        std::vector<const Audio*> sources;
        juce::String action;
        bool renderGeometryStable = false;
    };

    static size_t estimate(const EditorState&) noexcept;
    static Entry makeEntry(EditorState&&, juce::uint64 revision, juce::String action,
                           bool renderGeometryStable);
    void trimBudget();
    void discardDistantEntry();

    std::vector<Entry> undoStack;
    std::vector<Entry> redoStack;
    juce::uint64 currentRevision = 0;
    juce::uint64 savedRevision = 0;
    juce::uint64 nextRevision = 1;
    size_t metadataLimit;
    bool budgetPruned = false;
};

/** Row-preserving timeline clipboard holding either range slices or whole clips. */
struct Clipboard {
    juce::int64 lengthSamples = 0;
    std::vector<std::vector<Clip>> tracks;
    bool wholeClips = false; // Distinguishes clip-group copies from copied time ranges.

    bool empty() const noexcept { return lengthSamples <= 0 || tracks.empty(); }

    bool hasAudio() const noexcept {
        for (const auto& track : tracks) {
            if (!track.empty()) {
                return true;
            }
        }
        return false;
    }
};

/**
 * After a range cut, place the playhead and paste focus at the start of the
 * affected rows. For an all-track cut the clipboard begins at row zero.
 */
void finishRangeCut(TimeSelection&, int& selectedTrack, int& selectedClip, int& selectedMarker,
                    double& playhead, bool allTracks, int trackCount) noexcept;

/** Resolved position and optional magnetic guide line returned by `snapTimelinePosition`. */
struct SnapResult {
    double time = 0;
    double guide = 0;
    bool hasGuide = false;
};

/**
 * Resolve a dragged edge or clip start.
 *
 * Magnetic targets include either edge of a moving clip (`trailingOffset`),
 * provided the result remains in bounds.
 */
SnapResult snapTimelinePosition(double proposed, double minimum, double maximum, bool sampleGrid,
                                double tolerance, const std::vector<double>& targets,
                                double trailingOffset = 0, juce::int64 gridSamples = 1);

// =========================================================================
// Clip Grouping, Selection & Group-Level Edits
// =========================================================================

/**
 * Positions are absolute project samples; the end is exclusive.
 *
 * Moving keeps source offset and duration. Trimming/untrimming keeps source alignment.
 */
bool moveClipToSample(Clip&, juce::int64 start, juce::String& error);

/**
 * Move one clip to another existing track without changing its geometry.
 *
 * On success, `clip` becomes its new index in the destination.
 */
bool moveClipToTrack(Session&, int sourceTrack, int& clip, int destinationTrack);

/** Replace every occurrence of an already-verified source, without changing clip geometry. */
bool replaceVerifiedSource(Session&, const std::shared_ptr<Audio>& original,
                           const std::shared_ptr<Audio>& replacement, juce::String& error);

/**
 * Expand references to all members of any referenced persistent group, in track/clip order.
 *
 * Invalid or duplicate input leaves `output` unchanged.
 */
bool expandClipGroups(const Session&, const std::vector<ClipReference>&,
                      std::vector<ClipReference>& output, juce::String& error);

/**
 * Grouping merges complete existing groups.
 *
 * Ungrouping dissolves each touched group, including members outside the input.
 * Failures leave the session intact.
 */
bool groupClips(Session&, const std::vector<ClipReference>&, juce::String& error);

/** Dissolve every persistent clip group touched by the selection atomically. */
bool ungroupClips(Session&, const std::vector<ClipReference>&, juce::String& error);

/**
 * Give copied groups fresh identities.
 *
 * A copied group with only one member is ungrouped. The input clips must be
 * distinct copies; failure changes none.
 */
bool forkCopiedClipGroups(const Session&, const std::vector<Clip*>& copies, juce::String& error);

/** Transient timeline clip selection: indices expire on any project edit. */
class ClipSelection {
  public:
    bool toggle(const Session&, ClipReference, juce::uint64 generation);
    // User selection treats a persistent group as one unit; internal history
    // restoration and exact edit-result mappings continue to use raw refs.
    bool toggleGroup(const Session&, ClipReference, juce::uint64 generation);
    bool selectExpanded(const Session&, const std::vector<ClipReference>&, juce::uint64 generation);
    // Restore references against their matching session snapshot atomically.
    bool restore(const Session&, const std::vector<ClipReference>&, juce::uint64 generation);
    // Replace the group with clips on the given rows. If range is supplied,
    // include clips intersecting its half-open time interval. No matches or an
    // invalid scope leave the existing group unchanged.
    bool selectTracks(const Session&, TrackRange, const TimeSelection* range,
                      juce::uint64 generation);
    bool selectTrackIndices(const Session&, const std::vector<int>& tracks,
                            const TimeSelection* range, juce::uint64 generation);
    bool selectSourceOnTracks(const Session&, const std::shared_ptr<Audio>& source,
                              const std::vector<int>& tracks, const TimeSelection* range,
                              juce::uint64 generation);

    void clear() noexcept {
        refs.clear();
        keys.clear();
    }

    const std::vector<ClipReference>& get(juce::uint64 generation) const noexcept;
    bool contains(int track, int clip, juce::uint64 generation) const noexcept;
    // Only for an edit proven to preserve every selected clip index.
    bool retainAfterStableEdit(juce::uint64 previous, juce::uint64 current) noexcept;

  private:
    juce::uint64 revision = 0;
    std::vector<ClipReference> refs;
    std::unordered_set<juce::uint64> keys;
};

/**
 * A successful explicit range selection takes editing focus from any clip group.
 *
 * Invalid ranges preserve both selections.
 */
bool activateTimeSelection(TimeSelection& current, ClipSelection& clips, const TimeSelection& next,
                           int trackCount) noexcept;

/** Accessible distinction between the keyboard-focused clip and the group. */
juce::String describeClipSelection(const ClipSelection&, juce::uint64 generation, int focusedTrack,
                                   int focusedClip);

/** Keyboard shortcut actions routed to an active clip-group selection. */
enum class ClipGroupShortcut { none, copy, cut, remove, duplicate, split };

/**
 * Only unmodified Delete or unmodified Command/Control letter shortcuts act
 * on an active clip group; modified range commands remain independent.
 */
ClipGroupShortcut clipGroupShortcut(int keyCode, bool isDeleteKey, bool plain, bool plainCommand,
                                    bool hasSelection) noexcept;

/**
 * Copy whole selected clips, retaining intervening rows and fractional source timing.
 *
 * The clipboard origin is the sample at or before the earliest start.
 * Invalid selections leave the output unchanged.
 */
bool copyClipGroup(const Session&, const std::vector<ClipReference>&, Clipboard&,
                   juce::String& error);

/** Copy and remove whole clips atomically; failure preserves the clipboard too. */
bool cutClipGroup(Session&, const std::vector<ClipReference>&, Clipboard&, juce::String& error);

/**
 * Move independent whole clips together by signed project-sample and track offsets.
 *
 * Destination overlaps sum; failure leaves the session untouched. Optional
 * output maps each selected reference to its new location in input order;
 * rejected moves leave both session and output unchanged.
 */
bool moveClipGroup(Session&, const std::vector<ClipReference>&, juce::int64 sampleDelta,
                   juce::int64 trackDelta, juce::String& error,
                   std::vector<ClipReference>* movedRefs = nullptr);

/** Prepare a move without changing the source; on failure output and `movedRefs` are unchanged. */
bool prepareClipGroupMove(const Session&, Session& output, const std::vector<ClipReference>&,
                          juce::int64 sampleDelta, juce::int64 trackDelta, juce::String& error,
                          std::vector<ClipReference>* movedRefs = nullptr);

/** Copy selected clips to offset destinations, leaving the source and caller clipboard untouched.
 */
bool prepareClipGroupCopy(const Session&, Session& output, const std::vector<ClipReference>&,
                          juce::int64 sampleDelta, juce::int64 trackDelta, juce::String& error,
                          std::vector<ClipReference>* copiedRefs = nullptr);

/**
 * Validate destination rows and geometry without changing the session.
 *
 * Marker safety is checked separately by `moveClipGroup` when the edit is committed.
 */
bool validateClipGroupMove(const Session&, const std::vector<ClipReference>&,
                           juce::int64 sampleDelta, juce::int64 trackDelta, juce::String& error);

/**
 * Measure the peak linear magnitude of a clip's active source interval with active fade curves
 * (excluding `clip.gain`). Returns false if the clip has no readable audio.
 */
bool measureClipRawPeak(const Clip&, float& peakOut, juce::String& error);

/** Atomically scale clip gains by `deltaDb`, rejecting if any clip would exceed `maxLinearGain`. */
bool amplifyClips(Session&, const std::vector<ClipReference>&, double deltaDb, juce::String& error);

/** Atomically normalize clip gains to `targetPeakDb` (<= 0 dBFS), linked or per-clip independent.
 */
bool normalizeClips(Session&, const std::vector<ClipReference>&, double targetPeakDb,
                    bool independent, juce::String& error);

/** Shift source audio within fixed clip boundaries, expanding persistent groups. */
bool slipClipGroup(Session&, const std::vector<ClipReference>&, juce::int64 sampleDelta,
                   juce::String& error);

/**
 * Preflight once per drag; subsequent checks allocate nothing on success.
 *
 * The plan is valid only while the source session revision is unchanged.
 */
class ClipGroupMovePreview {
  public:
    bool prepare(const Session&, const std::vector<ClipReference>&, juce::String& error);
    bool validate(juce::int64 sampleDelta, juce::int64 trackDelta, juce::String& error) const;

  private:
    struct Geometry {
        int track;
        double start;
        double offset;
        double duration;
    };

    std::vector<Geometry> clips;
    juce::int64 trackCount = 0;
};

/**
 * Snapshot unselected clip intervals once per gesture.
 *
 * Queries detect whether moved clips overlap stationary clips on their
 * destination rows; overlaps within the selected group are unchanged and are
 * deliberately excluded.
 */
class ClipOverlapPreview {
  public:
    bool prepare(const Session&, const std::vector<ClipReference>&, juce::String& error);
    bool overlaps(juce::int64 sampleDelta, juce::int64 trackDelta) const noexcept;
    bool overlapsSingle(double start, int destinationTrack) const noexcept;

  private:
    struct Moving {
        int track;
        double start;
        double duration;
    };

    struct Interval {
        double start;
        double end;
    };

    bool intersects(int track, double start, double duration) const noexcept;

    std::vector<Moving> moving;
    std::vector<std::vector<Interval>> stationary;
};

/** Sorted, finite targets for binary search during a group drag. */
std::vector<double> clipGroupSnapTargets(const Session&, const TimeSelection&, double playhead,
                                         const std::vector<ClipReference>&,
                                         SnapTargetOptions options = {});

/** Sample-delta snap result for dragging a group of clips. */
struct GroupSnapResult {
    juce::int64 delta = 0;
    double guide = 0;
    bool hasGuide = false;
};

/** `targets` must be finite and sorted as returned by `clipGroupSnapTargets`. */
GroupSnapResult snapClipGroupMove(double anchorStart, double anchorDuration,
                                  juce::int64 requestedDelta, double tolerance,
                                  const std::vector<double>& targets) noexcept;

/**
 * Snap the dragged anchor start to a project-sample grid using one shared
 * integer-sample delta, retaining fractional offsets between group clips.
 */
bool snapClipGroupToGrid(double anchorStart, juce::int64 requestedDelta, juce::int64 gridSamples,
                         juce::int64& result) noexcept;

/** Shift by project samples, retaining an existing fractional timeline position. */
double shiftTimelineBySamples(double time, juce::int64 delta) noexcept;

// =========================================================================
// Clip Splitting, Sequencing, Alignment & Trimming
// =========================================================================

/**
 * Split one clip at an exact project sample; requires at least one sample on either side.
 *
 * Invalid positions and clip references leave the session intact.
 */
bool canSplitClipAtSample(const Clip&, juce::int64 sample) noexcept;
bool splitClipAtSample(Session&, int track, int clip, juce::int64 sample);

/**
 * Split only selected clips that can be cut at this project sample.
 *
 * Invalid references reject the whole operation; clips not crossing it are
 * unchanged. Optional output maps each original clip to its resulting piece(s),
 * in input order. Unsplit clips map to one reference. Failures leave output unchanged.
 */
bool splitClipGroupAtSample(Session&, const std::vector<ClipReference>&, juce::int64 sample,
                            juce::String& error,
                            std::vector<std::vector<ClipReference>>* pieces = nullptr);

/**
 * Delete whole clips atomically and leave markers in place.
 */
bool deleteClipGroup(Session&, const std::vector<ClipReference>&, juce::String& error);

/**
 * Duplicate the selected clips' sample span immediately after itself atomically.
 *
 * Optional output identifies the duplicates in the input selection order.
 * Failure leaves both the session and output unchanged.
 */
bool duplicateClipGroup(Session&, const std::vector<ClipReference>&, juce::String& error,
                        std::vector<ClipReference>* duplicatedRefs = nullptr);

/**
 * Place selected clips and persistent groups consecutively in start-time order.
 *
 * Groups use their bounding spans and retain internal timing. The earliest start
 * is fixed; tracks, source geometry and stacking are retained.
 */
bool sequenceClipGroup(Session&, const std::vector<ClipReference>&, juce::String& error);
bool prepareSequenceClipGroup(const Session&, Session& output, const std::vector<ClipReference>&,
                              juce::String& error);

/**
 * Space selected clips and persistent groups with equal, nonnegative gaps
 * between their spans. The earliest and latest starts stay fixed.
 */
bool distributeClipGroup(Session&, const std::vector<ClipReference>&, juce::String& error);
bool prepareDistributeClipGroup(const Session&, Session& output, const std::vector<ClipReference>&,
                                juce::String& error);

/** Align the clip's start or exclusive end to a project sample without trimming. */
bool alignClipEdgeToSample(Clip&, bool endEdge, juce::int64 sample, juce::String& error);

/** Exact timeline-time alignment preserves fractional-sample target edges. */
bool alignClipEdgeToTime(Clip&, bool endEdge, double target, juce::String& error);

/**
 * Align one anchor while translating all selected clips by the same amount.
 *
 * Invalid geometry or marker bounds leave the session unchanged.
 */
bool alignClipGroupEdgeToTime(Session&, const std::vector<ClipReference>&, ClipReference anchor,
                              bool endEdge, double target, juce::String& error);

/**
 * Find the nearest strictly previous/next edge on any other clip, including
 * clips on other tracks; the source clip's own edges are excluded.
 */
bool adjacentClipEdge(const Session&, ClipReference, bool endEdge, bool next,
                      double& target) noexcept;
bool adjacentClipEdgeExcluding(const Session&, ClipReference, bool endEdge, bool next,
                               const std::vector<ClipReference>& excluded, double& target) noexcept;

/** Trim a clip's timeline bounds to `[start, end)` project samples while keeping source alignment.
 */
bool trimClipToSamples(Clip&, juce::int64 start, juce::int64 end, juce::String& error);

/**
 * Move one clip edge by one project sample without changing the opposite edge.
 *
 * Trimming and untrimming retain the source-anchored envelope.
 */
bool nudgeClipEdge(Clip&, bool endEdge, int direction, juce::String& error);

/**
 * Adjust the same edge of every selected clip by a signed sample offset, or leave all
 * clips unchanged if any adjustment cannot be made safely.
 */
bool nudgeClipGroupEdges(Session&, const std::vector<ClipReference>&, bool endEdge,
                         juce::int64 samples, juce::String& error);

/**
 * Trim or untrim each selected edge to one project sample.
 *
 * The opposite edge and source alignment are retained; invalid clips reject the
 * entire batch. The caller must validate marker bounds before committing the edit.
 */
bool trimClipGroupEdgesToSample(Session&, const std::vector<ClipReference>&, bool endEdge,
                                juce::int64 sample, juce::String& error);

/**
 * Set either or both edges of an anchor clip to project samples, applying the
 * same respective edge movements to all selected clips.
 *
 * Unchanged fields keep their exact original edges. Bounds or marker failure
 * leaves the session intact.
 */
bool trimClipGroupToAnchorRange(Session&, const std::vector<ClipReference>&, ClipReference anchor,
                                bool setStart, juce::int64 start, bool setEnd, juce::int64 end,
                                juce::String& error);

/**
 * Trim each standalone clip or persistent group using its own anchor.
 *
 * The focused member anchors its group; other groups use their outer edge. All
 * groups are validated as one atomic edit.
 */
bool trimClipUnitsToSample(Session&, const std::vector<ClipReference>&, ClipReference focus,
                           bool endEdge, juce::int64 sample, juce::String& error);

/** Produce adjusted copies for a drag preview without changing the session. */
bool previewClipGroupEdges(const Session&, const std::vector<ClipReference>&, bool endEdge,
                           juce::int64 samples, std::vector<Clip>&, juce::String& error);

/**
 * Prepare source geometry once per gesture; calculate only selected clips on each update.
 *
 * The plan expires when the project revision changes.
 */
class ClipGroupEdgePreview {
  public:
    bool prepare(const Session&, const std::vector<ClipReference>&, juce::String& error);
    bool calculate(bool endEdge, juce::int64 samples, std::vector<Clip>&, juce::String& error);

    void clear() noexcept {
        originals.clear();
        scratch.clear();
    }

  private:
    std::vector<Clip> originals;
    std::vector<Clip> scratch;
};

/** Prepared once per drag; rejects previews that would strand an existing marker. */
class ClipGroupEdgeMarkerPreview {
  public:
    bool prepare(const Session&, const std::vector<ClipReference>&, juce::String& error);
    bool validate(const std::vector<Clip>& adjusted, juce::String& error) const;

  private:
    double unselectedEnd = 0;
    juce::int64 minimumEnd = 0;
    juce::int64 requiredEnd = 0;
    size_t clipCount = 0;
};

/** Duplicate one clip immediately after itself on the same track, clearing group membership. */
bool duplicateClipAfter(Session&, int track, int& clip, juce::String& error);

// =========================================================================
// Range Edits, Ripple Operations & Crossfades
// =========================================================================

/**
 * Copy retains shared source media, with positions relative to the selection.
 *
 * Clipboard entries retain silent tracks and preserve overlaps.
 */
Clipboard copyRange(const Session&, const TimeSelection&);

/** Copy explicit rows, retaining empty clipboard rows between selected tracks. */
bool copyIndependentTracks(const Session&, const TimeSelection&, const std::vector<int>& tracks,
                           Clipboard&, juce::String& error);

/**
 * Duplicate selected audio immediately after the range, summing with any existing material.
 *
 * The clipboard, selection and markers are untouched.
 */
bool duplicateRange(Session&, const TimeSelection&, juce::String& error);

/**
 * Move all audio intersecting a contiguous range to existing destination rows.
 *
 * Overlaps at the destination sum; markers and clipboard remain unchanged.
 */
bool moveRange(Session&, const TimeSelection&, int targetTrack, juce::int64 atSample,
               juce::String& error);
bool moveIndependentRange(Session&, const TimeSelection&, const std::vector<int>& tracks,
                          int targetTrack, juce::int64 atSample, juce::String& error);

/** Count groups whose moved portions will remain linked to audio outside the range or selected
 * tracks. */
int rangeMoveLinkedGroups(const Session&, const TimeSelection&,
                          const std::vector<int>& independentTracks);

/** Non-ripple deletion: leave the selected time in place as a gap. */
bool removeRange(Session&, const TimeSelection&, juce::String* error = nullptr);

/**
 * Keep only portions inside the selection on the selected tracks.
 *
 * Other tracks stay unchanged; rejects newly orphaned markers without mutation.
 */
bool trimTracksToRange(Session&, const TimeSelection&, juce::String& error);

/** Preserve markers that were inside the master before a content-only edit. */
bool preservesMarkerBounds(const Session& before, const Session& after, juce::String& error);

// Preserve linked group identities across a prepared timeline edit. The
// candidate may be adjusted when an implicit first track gains a marker.
/** Split all clips crossing either range endpoint on the selected tracks. */
bool splitRange(Session&, const TimeSelection&);

/**
 * Split every clip crossing a project-sample position on the specified tracks.
 *
 * Existing edges are unchanged; markers and all other tracks remain in place.
 */
bool splitTracksAtSample(Session&, TrackRange, juce::int64 sample);

/**
 * Move intersecting clip pieces onto new tracks, retaining absolute time and
 * each source track's mix and routing. Returns the first new track index.
 *
 * Empty source rows produce no new track; failure leaves the session unchanged.
 */
bool splitRangeToNewTracks(Session&, const TimeSelection&, int& firstNewTrack);
bool splitIndependentRangeToNewTracks(Session&, const TimeSelection&,
                                      const std::vector<int>& tracks, int& firstNewTrack);

/**
 * Join the selected clip to its one immediately adjacent successor, only when
 * they are contiguous pieces of the same source with identical clip settings.
 *
 * Updates `selected` to the merged clip index; rejection changes nothing.
 */
bool joinNextClip(Session&, int track, int& selected, juce::String& error);

/**
 * Join each selected same-track chain without rendering.
 *
 * Gaps separate chains; singleton clips remain selected. Overlaps, ambiguity
 * or incompatible adjacent clips reject the entire operation. The optional
 * mapping gives each input reference's resulting clip.
 */
bool joinSelectedClipChains(Session&, const std::vector<ClipReference>&, juce::String& error,
                            std::vector<ClipReference>* mapping = nullptr);

/**
 * Close the selected duration on selected or all tracks.
 *
 * All-track ripple also moves markers: markers inside the removed range land
 * at its start. Reject atomically if the result would collide markers or put
 * them outside the master.
 */
bool rippleRemoveRange(Session&, const TimeSelection&, bool allTracks,
                       juce::String* error = nullptr);

/**
 * Cut and close a gap atomically.
 *
 * All-track scope copies every affected row; failure leaves both the session
 * and the previous clipboard unchanged.
 */
bool rippleCutRange(Session&, const TimeSelection&, bool allTracks, Clipboard&,
                    juce::String& error);

/**
 * Insert the selection's duration at its start on selected or all tracks.
 *
 * Crossing clips are split; all-track insertion shifts markers at/after start.
 * Failure leaves the session untouched.
 */
bool insertGap(Session&, const TimeSelection&, bool allTracks, juce::String& error);

/** Edit actions supported over non-contiguous selected track indices. */
enum class IndependentRangeEdit {
    deleteGap,
    closeGap,
    insertGap,
    cutGap,
    cutClose,
    splitBoundaries,
    trimOutside,
    duplicate
};

/**
 * Apply a time interval to explicit track rows.
 *
 * Markers never move; cuts copy a row-preserving clipboard, including silent
 * rows between selected tracks. Failure leaves both session and clipboard untouched.
 */
bool editIndependentTracks(Session&, const TimeSelection&, const std::vector<int>& tracks,
                           IndependentRangeEdit, Clipboard&, juce::String& error);

/**
 * Set complementary linear fades on two partially overlapping clips.
 *
 * Rejects nested/ambiguous overlaps without changing either clip.
 */
bool crossfadeClipPair(Session&, ClipReference first, ClipReference second, juce::String& error,
                       juce::Range<double>* overlap = nullptr);
bool crossfadeOverlap(Session&, int track, int clip, juce::String& error,
                      juce::Range<double>* overlap = nullptr);

/**
 * Create complementary linear fades with one unambiguous overlapping clip
 * on another track, leaving all unrelated clip envelopes intact.
 */
bool crossfadeAcrossTracks(Session&, int track, int clip, juce::String& error,
                           juce::Range<double>* overlap = nullptr);

// =========================================================================
// Track Flags, Channel Routing, Isolation & Consolidation
// =========================================================================

/** Boolean track mix toggles modified by `setTrackFlag` / `setTrackFlags`. */
enum class TrackFlag { mute, solo };

/** Set one flag on a contiguous track range; invalid ranges and no-ops return false. */
bool setTrackFlag(Session&, TrackRange, TrackFlag, bool value) noexcept;

/** Set a flag on explicitly selected tracks; invalid or duplicate indices reject atomically. */
bool setTrackFlags(Session&, const std::vector<int>& tracks, TrackFlag, bool value);

/**
 * Swap the two source-channel routing rows on a track containing stereo clips only.
 *
 * Custom coefficients are retained; mixed channel layouts are rejected.
 */
bool swapStereoSourceChannels(Session&, int track, juce::String& error);

/**
 * Fold a stereo-source track's existing routing to equal left/right output
 * before track balance. Rejects mixed source-channel counts atomically.
 */
bool foldStereoTrackToMono(Session&, int track, juce::String& error);

/**
 * Duplicate a stereo track into two independently editable source-channel tracks.
 *
 * Sources are shared; the original routing is partitioned across the two rows.
 */
bool splitStereoTrackChannels(Session&, int track, juce::String& error);

/** Recombine adjacent, still-matching source-channel tracks without rendering. */
bool combineStereoChannelTracks(Session&, int firstTrack, juce::String& error);

/**
 * Build an isolated stereo render from two mono-source tracks.
 *
 * Track gain and clip envelopes apply; track pan, routing, mute and solo do not.
 */
bool prepareMonoStereoPair(const Session&, int leftTrack, int rightTrack, Session& output,
                           juce::String& error, juce::int64* startSample = nullptr,
                           juce::int64* endSample = nullptr);

/**
 * Mute tracks outside the selected inclusive range and clear their solo states.
 *
 * Selected tracks keep their mute/solo states. Returns false and leaves the
 * session unchanged for an invalid track range.
 */
bool limitToTracks(Session&, int firstTrack, int lastTrack);
bool limitToTrackIndices(Session&, const std::vector<int>& tracks);

/**
 * Restrict a snapshot to one clip using the same track mute/solo scope as
 * isolated audition.
 *
 * Other tracks are muted; source references stay retained. Returns the clip's
 * half-open project-sample range, unchanged on failure.
 */
bool isolateClip(Session&, int track, int clip, juce::int64& start, juce::int64& end);

/**
 * Isolate nonadjacent clips for a mixdown.
 *
 * Selected tracks retain their mute/solo, routing and envelopes; solo on other
 * tracks is ignored. The span contains all selected project samples, including
 * intervening silence.
 */
bool isolateClipGroup(Session&, const std::vector<ClipReference>&, juce::int64& start,
                      juce::int64& end, juce::String& error);

/**
 * Prepare a same-track mono or stereo clip consolidation before track mixing.
 *
 * All touched persistent groups must be complete. Output is unchanged on failure.
 */
bool prepareClipConsolidation(const Session&, const std::vector<ClipReference>&, Session& render,
                              int& track, int& channels, juce::int64& start, juce::int64& end,
                              juce::String& error);

/** Replace the same clips with verified 44.1 kHz rendered media as one atomic edit. */
bool replaceClipsWithConsolidation(Session&, const std::vector<ClipReference>&,
                                   const std::shared_ptr<Audio>&, juce::int64 start,
                                   juce::int64 end, int& newClip, juce::String& error);

/**
 * Check the affected track and the master mix before publishing a rendered replacement.
 *
 * The callback is cancellable; comparison uses project-rate samples.
 */
bool verifyClipConsolidation(const Session&, const std::vector<ClipReference>&,
                             const std::shared_ptr<Audio>&, juce::int64 start, juce::int64 end,
                             juce::String& error, std::function<bool(double)> progress = {},
                             bool allowRoutingChange = false);

/**
 * Divide remaining vertical pixels among expanded tracks; collapsed rows retain their saved height.
 *
 * Returns true only if heights changed. `allVisible` reports whether the
 * minimum-height rows fit; empty sessions have no change and fit trivially.
 */
bool fitTrackHeights(Session&, int availablePixels, bool& allVisible);

/**
 * Reject destinations that alias audio retained in the session, undo/redo,
 * clipboard, background snapshots or files currently being read.
 */
bool validateRetainedMediaDestination(const Session&, const std::vector<Session>& undo,
                                      const std::vector<Session>& redo, const Clipboard&,
                                      const juce::File&, juce::String& error,
                                      std::initializer_list<const Session*> background = {},
                                      const std::vector<juce::File>& pendingSources = {});

// =========================================================================
// Markers, Quiet-Gap Analysis & Source Silence Detachment
// =========================================================================

/** Move one marker to a unique, in-bounds master sample; failure leaves it unchanged. */
bool setMarkerSample(Session&, int marker, juce::int64 sample, juce::String& error);

/** Validate against a caller-prepared master end without mutating the session. */
bool validateMarkerSample(const Session&, int marker, juce::int64 sample, juce::int64 masterEnd,
                          juce::String& error);

/**
 * Delete explicit markers by index as one atomic operation; the implicit first
 * master track is unaffected. Duplicate or invalid indices reject the edit.
 */
bool deleteMarkers(Session&, const std::vector<int>& indices, juce::String& error);

/** Half-open silent or quiet interval `[start, end)` in 44.1 kHz project samples. */
struct QuietRegion {
    juce::int64 start = 0;
    juce::int64 end = 0;
};

/**
 * Conservative all-source-channel digital silence at project sample positions.
 *
 * Resampled positions require every contributing source sample to be zero.
 */
bool findClipSourceSilence(const Clip&, juce::int64 minimumSamples, std::vector<QuietRegion>&,
                           juce::String& error, LoadProgress progress = {},
                           bool anyPlaybackRate = false);

bool detachClipRange(Session&, int track, int clip, juce::int64 start, juce::int64 end,
                     juce::String& error);
bool detachClipRanges(Session&, int track, int clip, const std::vector<QuietRegion>&,
                      juce::String& error);

juce::String quietMarkerName(juce::int64 sample);

/**
 * Scan the rendered full master in bounded blocks.
 *
 * Only internal quiet runs are returned; cancellation or invalid audio leaves
 * the result unchanged.
 */
bool findQuietRegions(const Session&, int masterChannels, double thresholdDb,
                      juce::int64 minimumSamples, std::vector<QuietRegion>& result,
                      juce::String& error, LoadProgress progress = {});

/**
 * Produce the same midpoint names for UI and IPC.
 *
 * A gap containing an existing marker is flagged and omitted from the editable suggestion lines.
 */
bool quietMarkerSuggestions(const Session&, const std::vector<QuietRegion>&,
                            juce::StringArray& lines, std::vector<bool>& occupied,
                            juce::String& error);

/** Parse one editable suggestion without changing the output arguments on error. */
bool parseQuietMarkerSuggestion(const juce::String& line, juce::int64 masterEnd,
                                juce::int64& sample, juce::String& name, juce::String& error);

/** Parse editable `"sample name"` lines and append all markers atomically. */
bool addSuggestedMarkers(Session&, const juce::String& lines, juce::String& error);

/**
 * Add markers named from selected clips at their first contained project samples.
 *
 * Existing markers are retained; collisions reject the whole edit.
 */
bool addMarkersAtClipStarts(Session&, const std::vector<ClipReference>&, juce::String& error);

/**
 * Add markers at the first project sample at or after each selected clip end.
 *
 * The master end, collisions and invalid clips reject the entire batch.
 */
bool addMarkersAtClipEnds(Session&, const std::vector<ClipReference>&, juce::String& error);

/**
 * Add the focused clip's captured WAV cues within its trim, mapped to project samples.
 *
 * Existing marker collisions reject the entire operation.
 */
bool addMarkersFromClipCues(Session&, ClipReference, juce::String& error);

/** Count of existing marker UUIDs preserved when reconciling imported markers. */
struct MarkerImportReconciliation {
    std::size_t retained = 0;
};

/**
 * Carry identity only across unambiguous, sample-aligned boundaries.
 *
 * Other imported markers retain their newly generated identities.
 */
MarkerImportReconciliation reconcileImportedMarkers(const std::vector<Marker>& existing,
                                                    std::vector<Marker>& imported);

/** Diagnostic inspection record for one explicit marker in a `Session`. */
struct MarkerInspection {
    int index = -1;
    juce::int64 sample = 0;
    bool validPosition = false;
    bool outside = false;
    bool duplicate = false;
    bool emptyName = false;
    bool offGrid = false;
};

/**
 * Inspect every explicit marker, including repairable invalid entries.
 *
 * Ordered by sample, with coincident entries retaining their original order.
 */
std::vector<MarkerInspection> inspectMarkers(const Session&);

/**
 * Replace explicit marker names with their chronological master-track numbers.
 *
 * An implicit track at zero occupies number 1; positions and order are unchanged.
 * Invalid, duplicate or out-of-bounds positions are rejected atomically.
 */
bool nameMarkersByTrackNumber(Session&, juce::String& error);

/** Rename selected markers atomically without changing their positions or order. */
bool renameMarkerIndices(Session&, const std::vector<int>& indices,
                         const std::vector<juce::String>& names, juce::String& error);

/** Replace selected marker positions atomically; names stay attached to their markers. */
bool setMarkerSamples(Session&, const std::vector<int>& indices,
                      const std::vector<juce::int64>& samples, juce::String& error);

/**
 * Shift every explicit marker by the same project-sample offset, atomically.
 *
 * Existing markers must have unique, in-bounds, sample-aligned positions.
 */
bool shiftMarkers(Session&, juce::int64 delta, juce::String& error);

/**
 * Shift explicit markers in `[first, last)`, preserving marker order and rejecting
 * collisions, crossings and out-of-bounds results without changing anything.
 */
bool shiftMarkersInRange(Session&, juce::int64 first, juce::int64 last, juce::int64 delta,
                         juce::String& error);

/**
 * Shift an arbitrary set of explicit markers by a shared sample offset.
 *
 * Every marker must be valid and grid-aligned; selected markers cannot cross
 * or collide with stationary ones. Failure leaves the session unchanged.
 */
bool shiftMarkerIndices(Session&, const std::vector<int>& indices, juce::int64 delta,
                        juce::String& error);

/**
 * Round fractional marker positions to the nearest sample after checking all
 * resulting positions for collisions and bounds. Failure changes nothing.
 */
bool snapMarkersToSamples(Session&, juce::String& error);

// =========================================================================
// Range Pasting
// =========================================================================

/**
 * Paste is additive: existing clips and markers stay in place.
 *
 * All target tracks must already exist; an invalid destination changes nothing.
 * Optional output identifies inserted clips; failures leave it unchanged.
 */
bool pasteRange(Session&, const Clipboard&, int targetTrack, juce::int64 atSample,
                juce::String* error = nullptr, std::vector<ClipReference>* pastedRefs = nullptr);

/**
 * Replace the clipboard-length span on each destination row, including silent rows.
 *
 * Audio outside the span and marker positions are preserved.
 */
bool pasteRangeReplacing(Session&, const Clipboard&, int targetTrack, juce::int64 atSample,
                         juce::String& error, std::vector<ClipReference>* pastedRefs = nullptr);

/**
 * Insert time on destination rows or all rows, then paste.
 *
 * All-row insertion moves later markers; destination-row insertion leaves markers in place.
 */
bool pasteRangeInserting(Session&, const Clipboard&, int targetTrack, juce::int64 atSample,
                         bool allTracks, juce::String& error,
                         std::vector<ClipReference>* pastedRefs = nullptr);

} // namespace carta
