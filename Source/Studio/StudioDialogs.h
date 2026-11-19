/**
 * @file StudioDialogs.h
 * @brief Modal and non-modal browser, editor, and help dialog components for `Studio`.
 *
 * Declares the list-based browsers, multi-line text editors, keyboard shortcut
 * reference, and command-search palette used by `Studio`. All coordinate fields
 * use 44.1 kHz project samples (`carta::sessionRate`) with half-open
 * `[start, end)` bounds. Dialog components run exclusively on the JUCE message
 * thread.
 */

#pragma once

#include "Studio/StudioLookAndFeel.h"
#include "Core/Session.h"
#include "Editing/Editing.h"
#include "IO/SessionIO.h"
#include <algorithm>
#include <functional>
#include <vector>

/**
 * @brief Browser and action palette for layers and clips in the current session.
 */
class TrackClipNavigator final : public juce::Component, private juce::ListBoxModel {
    class NavigationList final : public juce::ListBox {
      public:
        NavigationList(juce::ListBoxModel* model, TrackClipNavigator& owner);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        TrackClipNavigator& navigator;
    };

  public:
    TrackClipNavigator(std::vector<carta::NavigationRow> entries, int selected,
                       std::function<void(std::vector<int>, int)> activate);

    /** @brief Moves keyboard focus to the layer/clip list and scrolls the selected row into view.
     */
    void focusList();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void selectedRowsChanged(int last) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    bool handleListKey(const juce::KeyPress& key);
    void announceKeyboardCursor(const juce::String& prefix);
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void activate(int row, int action);

    std::vector<carta::NavigationRow> rows;
    std::function<void(std::vector<int>, int)> onActivate;
    NavigationList list;
    juce::TextButton selectTime{"Select clip time"};
    juce::TextButton moveClips{"Move selected clips..."};
    juce::TextButton duplicateClips{"Duplicate clips"};
    juce::TextButton deleteClips{"Delete clips"};
    juce::TextButton splitClips{"Split at playhead"};
    juce::TextButton copyClips{"Copy clips"};
    juce::TextButton cutClips{"Cut clips"};
    int keyboardCursor = 0;
    bool keyboardSelection = false;
    bool updatingKeyboardSelection = false;
};

/**
 * @brief Multi-select layer browser supporting mix state changes and independent range edits.
 */
class TrackMixList final : public juce::Component, private juce::ListBoxModel {
    class Rows final : public juce::ListBox {
      public:
        Rows(juce::ListBoxModel* model, TrackMixList& owner);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        TrackMixList& listOwner;
    };

#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif

  public:
    struct Row {
        juce::String name;
        bool muted = false;
        bool solo = false;
    };

    TrackMixList(std::vector<Row> entries, std::vector<int> selected,
                 std::function<void(std::vector<int>, int)> action, bool hasTimeRange = false);

    /** @brief Focuses the layer list. */
    void focusList();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void selectedRowsChanged(int row) override;
    void announce();
    bool handleKey(const juce::KeyPress& key);
    void activate(int action);

    std::vector<Row> rows;
    std::function<void(std::vector<int>, int)> onAction;
    Rows list;
    juce::TextButton mute{"Mute"};
    juce::TextButton unmute{"Unmute"};
    juce::TextButton solo{"Solo"};
    juce::TextButton unsolo{"Clear solo"};
    juce::TextButton cut{"Cut time"};
    juce::TextButton cutClose{"Cut + close"};
    juce::TextButton removeTime{"Delete time"};
    juce::TextButton closeGap{"Close gap"};
    juce::TextButton insertTime{"Insert gap"};
    juce::TextButton copyTime{"Copy time"};
    juce::TextButton previewClose{"Preview close"};
    int cursor = 0;
    bool keyboardSelection = false;
    bool updating = false;
    bool rangeAvailable = false;
};

/**
 * @brief Browser listing marker-derived master tracks with audition, zoom, rename, and select
 * actions.
 */
class MasterTrackList final : public juce::Component, private juce::ListBoxModel {
    class TrackListBox final : public juce::ListBox {
      public:
        explicit TrackListBox(MasterTrackList& parent);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        MasterTrackList& owner;
    };

  public:
    MasterTrackList(std::vector<carta::MasterTrack> entries, int selected,
                    std::function<void(int, int)> activate);

    /** @brief Focuses the master track list. */
    void focusList();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void activate(int row, int action);

    std::vector<carta::MasterTrack> tracks;
    std::function<void(int, int)> onActivate;
    TrackListBox list;
    juce::TextButton previewBoundary{"Audition boundary"};
    juce::TextButton playTrack{"Play track"};
    juce::TextButton zoomTrack{"Zoom to track"};
    juce::TextButton renameTrack{"Rename"};
    juce::TextButton selectTrackTime{"Select time"};
};

/**
 * @brief Browser displaying overlapping clip sample ranges on individual layers.
 */
class OverlapList final : public juce::Component, private juce::ListBoxModel {
  public:
    struct Row {
        carta::OverlapRegion range;
        juce::String trackName;
    };

    OverlapList(std::vector<Row> entries, int selected, std::function<void(int)> activate);

    /** @brief Focuses the overlap list. */
    void focusList();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    int getNumRows() override;
    juce::String getNameForRow(int index) override;
    void paintListBoxItem(int index, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void activate(int row);

    std::vector<Row> rows;
    std::function<void(int)> onActivate;
    juce::ListBox list;
};

/**
 * @brief Actions available for detected quiet gaps and source-silence regions.
 */
enum class QuietGapAction { select, start, middle, end, detach, detachAll };

/**
 * @brief Returns the 44.1 kHz project sample audition point for a half-open quiet region.
 */
inline juce::int64 quietGapAuditionPoint(carta::QuietRegion gap, QuietGapAction action) noexcept {
    if (action == QuietGapAction::start) {
        return gap.start;
    }
    if (action == QuietGapAction::end) {
        return gap.end;
    }
    return gap.start + (gap.end - gap.start) / 2;
}

/**
 * @brief Finds the largest conservative interior in `safe` overlapping half-open `gap`.
 */
inline carta::QuietRegion detachableSilence(carta::QuietRegion gap,
                                            const std::vector<carta::QuietRegion>& safe) {
    carta::QuietRegion best{};
    for (const auto& region : safe) {
        if (region.start >= gap.end) {
            break;
        }
        const auto start = std::max(gap.start, region.start);
        const auto end = std::min(gap.end, region.end);
        if (end > start && end - start > best.end - best.start) {
            best = {start, end};
        }
    }
    return best;
}

/**
 * @brief Browser for master-mix quiet gaps or source-silence regions with audition and detach
 * actions.
 */
class QuietGapList final : public juce::Component, private juce::ListBoxModel {
    class GapRows final : public juce::ListBox {
      public:
        GapRows(juce::ListBoxModel* listModel, QuietGapList& owner);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        QuietGapList& gapList;
    };

#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif

  public:
    struct Row {
        carta::QuietRegion range;
        bool marked = false;
        carta::QuietRegion detachable{};
    };

    QuietGapList(std::vector<Row> entries,
                 std::function<juce::String(int, QuietGapAction)> activate,
                 std::function<void()> selected = {}, bool sourceSilence = false);

    /** @brief Focuses the detected gap list. */
    void focusList();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    int getNumRows() override;
    juce::String getNameForRow(int index) override;
    void paintListBoxItem(int index, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void selectedRowsChanged(int row) override;
    void returnKeyPressed(int row) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void activateRow(QuietGapAction action);
    void activate(int row, QuietGapAction action);
    void close();

    std::vector<Row> rows;
    std::function<juce::String(int, QuietGapAction)> onActivate;
    std::function<void()> onSelected;
    GapRows list;
    bool sourceScope = false;
    juce::Label error;
    juce::TextButton select{"Select gap"};
    juce::TextButton detach{"Detach interior"};
    juce::TextButton detachAll{"Detach all"};
    juce::TextButton auditionStart{"Audition start"};
    juce::TextButton audition{"Audition midpoint"};
    juce::TextButton auditionEnd{"Audition end"};
    juce::TextButton cancel{"Close"};
};

/**
 * @brief Multi-line editor for reviewing, auditioning, and accepting suggested quiet-gap markers.
 */
class QuietMarkerSuggestions final : public juce::Component {
#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif

  public:
    QuietMarkerSuggestions(juce::String initial,
                           std::function<juce::String(const juce::String&)> accept,
                           std::function<juce::String(const juce::String&)> auditionLine,
                           std::function<juce::String(const juce::String&)> selectLine = {},
                           std::function<void(std::function<void()>)> browseGaps = {});

    /** @brief Focuses the suggestions text editor. */
    void focusEditor();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    void close();
    juce::String currentLine() const;

    std::function<juce::String(const juce::String&)> onAccept;
    std::function<juce::String(const juce::String&)> onAudition;
    std::function<juce::String(const juce::String&)> onSelect;
    std::function<void(std::function<void()>)> onBrowse;
    juce::Label explanation;
    juce::Label error;
    juce::TextEditor editor;
    juce::TextButton apply{"Add markers"};
    juce::TextButton audition{"Audition current line"};
    juce::TextButton select{"Select mix gap"};
    juce::TextButton cancel{"Cancel"};
    juce::TextButton browse{"Browse all gaps"};
};

/**
 * @brief Multi-line editor for batch-renaming or batch-repositioning selected markers.
 */
class MarkerNamesEditor final : public juce::Component {
#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif

  public:
    MarkerNamesEditor(juce::String initial, int count,
                      std::function<juce::String(const juce::String&)> accept,
                      std::function<void()> closed, bool positions = false);

    /** @brief Focuses the multi-line marker editor. */
    void focusEditor();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    void close();

    std::function<juce::String(const juce::String&)> onAccept;
    std::function<void()> onClosed;
    juce::Label explanation;
    juce::Label error;
    juce::TextEditor editor;
    juce::TextButton apply{"Apply names"};
    juce::TextButton cancel{"Cancel"};
};

/**
 * @brief Multi-select track marker list supporting inspection, audition, rename, shift, and delete.
 */
class MarkerList final : public juce::Component, private juce::ListBoxModel {
    class MarkerRows final : public juce::ListBox {
      public:
        MarkerRows(juce::ListBoxModel* model, MarkerList& owner);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        MarkerList& markerList;
    };

#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif

  public:
    struct Row {
        carta::MarkerInspection info;
        juce::String name;
        juce::String id{};
        double position = 0;
    };

    MarkerList(std::vector<Row> entries, int selected,
               std::function<void(std::vector<int>, int, int)> activate);

    /** @brief Focuses the marker list. */
    void focusList();

    /** @brief Restores a set of selected marker row indices. */
    void selectRows(const std::vector<int>& indices);

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    static juce::String issues(const carta::MarkerInspection& info);
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;
    void selectedRowsChanged(int row) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    bool handleListKey(const juce::KeyPress& key);
    void announceCursor(const juce::String& prefix);
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void deleteKeyPressed(int row) override;
    void activateRow(int action);
    void activate(int row, int action);

    std::vector<Row> rows;
    std::function<void(std::vector<int>, int, int)> onActivate;
    MarkerRows list;
    juce::TextButton rename{utf8Str("Rename…")};
    juce::TextButton move{utf8Str("Set position…")};
    juce::TextButton shift{utf8Str("Shift…")};
    juce::TextButton remove{"Delete"};
    juce::TextButton audition{"Audition boundary"};
    int keyboardCursor = 0;
    bool keyboardSelection = false;
    bool updatingKeyboardSelection = false;
};

/**
 * @brief List showing audio source files referenced by a layer and their verification status.
 */
class TrackSourceList final : public juce::Component, private juce::ListBoxModel {
  public:
    TrackSourceList(juce::String title, std::vector<carta::TrackSource> entries,
                    std::vector<juce::String> results,
                    std::function<void(std::shared_ptr<carta::Audio>)> relink);

    /** @brief Focuses the source list. */
    void focusList();

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    int getNumRows() override;
    juce::String description(int row) const;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;

    juce::String trackName;
    std::vector<carta::TrackSource> sources;
    std::vector<juce::String> checks;
    juce::ListBox list;
    std::function<void(std::shared_ptr<carta::Audio>)> onRelink;
    juce::TextButton relinkButton{"Relink selected source..."};
};

/**
 * @brief Read-only modal window displaying timeline and editing keyboard shortcuts.
 */
class KeyboardHelp final : public juce::Component {
  public:
    KeyboardHelp();
    void resized() override;
    void focusText();

  private:
    juce::TextEditor text;
};

/**
 * @brief Read-only modal window displaying queued/failed audio import diagnostics.
 */
class ImportIssueReport final : public juce::Component {
  public:
    explicit ImportIssueReport(const juce::String& report);
    void resized() override;
    void focusText();

  private:
    juce::TextEditor text;
};

/**
 * @brief Menu command entry indexed by `CommandSearch`.
 */
struct CommandSearchEntry {
    int id = 0;
    int menu = 0;
    juce::String path;
    bool enabled = false;
};

/**
 * @brief Recursively flattens enabled/disabled leaf menu items into `out` for command search.
 */
inline void collectSearchCommands(const juce::PopupMenu& menu, int menuIndex,
                                  const juce::String& path, bool enabled,
                                  std::vector<CommandSearchEntry>& out) {
    juce::PopupMenu::MenuItemIterator iterator(menu);
    while (iterator.next()) {
        const auto& item = iterator.getItem();
        if (item.isSeparator || item.isSectionHeader || item.text.isEmpty()) {
            continue;
        }
        const auto name = path + " > " + item.text;
        const bool available = enabled && item.isEnabled;
        if (item.subMenu) {
            collectSearchCommands(*item.subMenu, menuIndex, name, available, out);
        } else if (item.itemID > 0 && item.itemID < 1000000 && item.itemID != 371) {
            out.push_back({item.itemID, menuIndex, name, available});
        }
    }
}

/**
 * @brief Filterable command palette searching menu bar commands by case-insensitive terms.
 */
class CommandSearch final : public juce::Component, private juce::ListBoxModel {
#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif

    class SearchField final : public juce::TextEditor {
      public:
        explicit SearchField(CommandSearch& owner);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        CommandSearch& search;
    };

  public:
    CommandSearch(std::vector<CommandSearchEntry> commands,
                  std::function<void(CommandSearchEntry)> activate);

    /** @brief Focuses the search text box. */
    void focusSearch();

    void resized() override;

  private:
    void filter();
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool selected) override;
    bool handleKey(const juce::KeyPress& key);
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void run(int row);

    std::vector<CommandSearchEntry> entries;
    std::vector<size_t> visible;
    std::function<void(CommandSearchEntry)> onActivate;
    SearchField query;
    juce::ListBox list;
};
