/**
 * @file StudioDialogs.cpp
 * @brief Out-of-line implementations for `StudioDialogs.h` modal and browser components.
 *
 * Implements list rendering, keyboard selection/navigation, accessibility
 * announcements, and action dispatch for `Studio` dialogs. All components
 * execute exclusively on the JUCE message thread.
 */

#include "Studio/StudioDialogs.h"
#include "Studio/StudioTheme.h"

#include <array>

namespace {

bool isDarkTheme() {
    return juce::Desktop::getInstance().isDarkModeActive();
}

juce::Colour dialogBackgroundColour(bool dark) {
    return carta::theme::palette(dark).dialogBg;
}

juce::Colour listForegroundColour(bool dark) {
    return carta::theme::palette(dark).textPrimary;
}

void paintSelectedRowBackground(juce::Graphics& g, int width, int height, bool dark,
                                bool selected) {
    if (selected) {
        g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.15f));
        g.fillRect(0, 0, width, height);
    }
}

void paintKeyboardCursorOutline(juce::Graphics& g, int width, int height, bool dark, float alpha) {
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(alpha));
    g.drawRect(1, 1, std::max(0, width - 2), std::max(0, height - 2), 1);
}

void closeParentDialog(juce::Component& component) {
    if (auto* window = component.findParentComponentOfClass<juce::DialogWindow>()) {
        window->exitModalState(0);
    }
}

} // namespace

// ============================================================================
// TrackClipNavigator
// ============================================================================

TrackClipNavigator::NavigationList::NavigationList(juce::ListBoxModel* model,
                                                   TrackClipNavigator& owner)
    : juce::ListBox("Layers and clips", model), navigator(owner) {
}

bool TrackClipNavigator::NavigationList::keyPressed(const juce::KeyPress& key) {
    return navigator.handleListKey(key) || juce::ListBox::keyPressed(key);
}

TrackClipNavigator::TrackClipNavigator(std::vector<carta::NavigationRow> entries, int selected,
                                       std::function<void(std::vector<int>, int)> activate)
    : rows(std::move(entries)), onActivate(std::move(activate)), list(this, *this) {
    addAndMakeVisible(list);
    list.setRowHeight(25);
    list.setMultipleSelectionEnabled(true);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    addAndMakeVisible(selectTime);
    selectTime.onClick = [this] { this->activate(list.getSelectedRow(), 1); };
    selectTime.setEnabled(false);

    addAndMakeVisible(moveClips);
    moveClips.onClick = [this] { this->activate(list.getSelectedRow(), 2); };
    moveClips.setEnabled(false);

    addAndMakeVisible(duplicateClips);
    duplicateClips.onClick = [this] { this->activate(list.getSelectedRow(), 3); };
    duplicateClips.setEnabled(false);

    addAndMakeVisible(deleteClips);
    deleteClips.onClick = [this] { this->activate(list.getSelectedRow(), 4); };
    deleteClips.setEnabled(false);

    addAndMakeVisible(splitClips);
    splitClips.onClick = [this] { this->activate(list.getSelectedRow(), 5); };
    splitClips.setEnabled(false);

    addAndMakeVisible(copyClips);
    copyClips.onClick = [this] { this->activate(list.getSelectedRow(), 6); };
    copyClips.setEnabled(false);

    addAndMakeVisible(cutClips);
    cutClips.onClick = [this] { this->activate(list.getSelectedRow(), 7); };
    cutClips.setEnabled(false);

    list.setTitle("Layers and clips");
    list.setHelpText("Press M for keyboard clip selection, arrows to browse, Space to toggle, "
                     "A for all clips, T for clips on the current layer, N for none.");
    setSize(650, 400);
    list.updateContent();
    if (selected >= 0 && selected < getNumRows()) {
        list.selectRow(selected);
        list.scrollToEnsureRowIsOnscreen(selected);
    }
}

void TrackClipNavigator::focusList() {
    if (list.getSelectedRow() >= 0) {
        list.scrollToEnsureRowIsOnscreen(list.getSelectedRow());
    }
    list.grabKeyboardFocus();
}

void TrackClipNavigator::resized() {
    list.setBounds(getLocalBounds().withTrimmedBottom(88));
    const int quarter = std::max(0, (getWidth() - 40) / 4);
    const int third = std::max(0, (getWidth() - 32) / 3);
    selectTime.setBounds(8, getHeight() - 86, quarter, 28);
    copyClips.setBounds(16 + quarter, getHeight() - 86, quarter, 28);
    cutClips.setBounds(24 + 2 * quarter, getHeight() - 86, quarter, 28);
    moveClips.setBounds(32 + 3 * quarter, getHeight() - 86, quarter, 28);
    duplicateClips.setBounds(8, getHeight() - 54, third, 28);
    splitClips.setBounds(16 + third, getHeight() - 54, third, 28);
    deleteClips.setBounds(24 + 2 * third, getHeight() - 54, third, 28);
}

void TrackClipNavigator::paint(juce::Graphics& g) {
    const bool dark = isDarkTheme();
    g.fillAll(dialogBackgroundColour(dark));
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.7f));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(keyboardSelection ? "Arrows browse; Space toggles; A all, T layer, N none, M exits."
                                 : "Return focuses a row; M enables keyboard clip selection.",
               8, getHeight() - 22, getWidth() - 16, 20, juce::Justification::centredLeft, true);
}

int TrackClipNavigator::getNumRows() {
    return static_cast<int>(rows.size());
}

juce::String TrackClipNavigator::getNameForRow(int row) {
    return row >= 0 && row < getNumRows() ? rows[static_cast<size_t>(row)].description
                                          : juce::String();
}

void TrackClipNavigator::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                          bool selected) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    paintSelectedRowBackground(g, width, height, dark, selected);
    if (keyboardSelection && row == keyboardCursor) {
        paintKeyboardCursorOutline(g, width, height, dark, 0.75f);
    }
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(rows[static_cast<size_t>(row)].label, 8, 0, std::max(0, width - 16), height,
               juce::Justification::centredLeft, true);
}

void TrackClipNavigator::selectedRowsChanged(int last) {
    if (!updatingKeyboardSelection && last >= 0) {
        keyboardCursor = last;
    }
    const auto selected = list.getSelectedRows();
    selectTime.setEnabled(selected.size() == 1 && selected[0] >= 0 && selected[0] < getNumRows() &&
                          rows[static_cast<size_t>(selected[0])].hasSampleRange);
    bool clipsOnly = selected.size() > 0;
    for (int i = 0; i < selected.size(); ++i) {
        if (selected[i] < 0 || selected[i] >= getNumRows() ||
            rows[static_cast<size_t>(selected[i])].clip < 0) {
            clipsOnly = false;
            break;
        }
    }
    moveClips.setEnabled(clipsOnly);
    duplicateClips.setEnabled(clipsOnly);
    deleteClips.setEnabled(clipsOnly);
    splitClips.setEnabled(clipsOnly);
    copyClips.setEnabled(clipsOnly);
    cutClips.setEnabled(clipsOnly);
}

void TrackClipNavigator::listBoxItemClicked(int row, const juce::MouseEvent&) {
    if (keyboardSelection && row >= 0 && row < getNumRows()) {
        const int old = keyboardCursor;
        keyboardCursor = row;
        list.repaintRow(old);
        list.repaintRow(row);
    }
}

bool TrackClipNavigator::handleListKey(const juce::KeyPress& key) {
    const auto mods = key.getModifiers();
    const auto character = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
    const auto commandCharacter =
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    if (mods.isCommandDown() && !mods.isAltDown() && !mods.isShiftDown() &&
        (commandCharacter == 'c' || commandCharacter == 'x')) {
        if (copyClips.isEnabled()) {
            activate(list.getSelectedRow(), commandCharacter == 'c' ? 6 : 7);
        }
        return true;
    }
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown() || mods.isShiftDown()) {
        return false;
    }
    if (key.getTextCharacter() == 'm' || key.getTextCharacter() == 'M') {
        keyboardSelection = !keyboardSelection;
        if (keyboardSelection) {
            keyboardCursor = juce::jlimit(0, std::max(0, getNumRows() - 1),
                                          list.getLastRowSelected() >= 0 ? list.getLastRowSelected()
                                                                         : keyboardCursor);
            auto selected = list.getSelectedRows();
            for (int row = 0; row < getNumRows(); ++row) {
                if (rows[static_cast<size_t>(row)].clip < 0) {
                    selected.removeRange({row, row + 1});
                }
            }
            updatingKeyboardSelection = true;
            list.setSelectedRows(selected, juce::sendNotification);
            updatingKeyboardSelection = false;
            announceKeyboardCursor(
                "Keyboard clip selection on. Arrows browse; Space toggles clips. ");
        } else {
            juce::AccessibilityHandler::postAnnouncement(
                "Keyboard clip selection off.",
                juce::AccessibilityHandler::AnnouncementPriority::medium);
        }
        list.repaint();
        repaint();
        return true;
    }
    if (!keyboardSelection || getNumRows() == 0) {
        return false;
    }
    int next = keyboardCursor;
    if (character == 'a' || character == 't' || character == 'n') {
        auto selected = list.getSelectedRows();
        const auto action = character == 'a'   ? carta::BrowserSelectionAction::all
                            : character == 't' ? carta::BrowserSelectionAction::track
                                               : carta::BrowserSelectionAction::clear;
        carta::updateBrowserClipSelection(rows, keyboardCursor, action, selected);
        updatingKeyboardSelection = true;
        list.setSelectedRows(selected, juce::sendNotification);
        updatingKeyboardSelection = false;
        juce::AccessibilityHandler::postAnnouncement(
            juce::String(selected.size()) + " clips selected.",
            juce::AccessibilityHandler::AnnouncementPriority::medium);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::upKey)) {
        --next;
    } else if (key.isKeyCode(juce::KeyPress::downKey)) {
        ++next;
    } else if (key.isKeyCode(juce::KeyPress::homeKey)) {
        next = 0;
    } else if (key.isKeyCode(juce::KeyPress::endKey)) {
        next = getNumRows() - 1;
    } else if (key.isKeyCode(juce::KeyPress::spaceKey)) {
        auto selected = list.getSelectedRows();
        if (carta::updateBrowserClipSelection(rows, keyboardCursor,
                                              carta::BrowserSelectionAction::toggle, selected)) {
            updatingKeyboardSelection = true;
            list.setSelectedRows(selected, juce::sendNotification);
            updatingKeyboardSelection = false;
            announceKeyboardCursor({});
            list.repaintRow(keyboardCursor);
        } else {
            juce::AccessibilityHandler::postAnnouncement(
                "Layer header. Choose a clip row to toggle it.",
                juce::AccessibilityHandler::AnnouncementPriority::medium);
        }
        return true;
    } else if (key.isKeyCode(juce::KeyPress::returnKey)) {
        activate(keyboardCursor, 0);
        return true;
    } else {
        return false;
    }
    next = juce::jlimit(0, getNumRows() - 1, next);
    if (next != keyboardCursor) {
        const int old = keyboardCursor;
        keyboardCursor = next;
        list.repaintRow(old);
        list.repaintRow(next);
        list.scrollToEnsureRowIsOnscreen(next);
        announceKeyboardCursor({});
    }
    return true;
}

void TrackClipNavigator::announceKeyboardCursor(const juce::String& prefix) {
    if (keyboardCursor < 0 || keyboardCursor >= getNumRows()) {
        return;
    }
    const auto& row = rows[static_cast<size_t>(keyboardCursor)];
    const auto state = row.clip >= 0
                           ? (list.isRowSelected(keyboardCursor) ? ", selected" : ", not selected")
                           : ", layer header";
    juce::AccessibilityHandler::postAnnouncement(
        prefix + row.description + state, juce::AccessibilityHandler::AnnouncementPriority::medium);
}

void TrackClipNavigator::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    activate(row, 0);
}

void TrackClipNavigator::returnKeyPressed(int row) {
    activate(row, 0);
}

void TrackClipNavigator::activate(int row, int action) {
    if (row < 0 || row >= getNumRows() ||
        (action == 1 && !rows[static_cast<size_t>(row)].hasSampleRange)) {
        return;
    }
    std::vector<int> indices;
    if (action >= 2) {
        const auto selected = list.getSelectedRows();
        for (int i = 0; i < selected.size(); ++i) {
            indices.push_back(selected[i]);
        }
    } else {
        indices.push_back(row);
    }
    onActivate(std::move(indices), action);
    closeParentDialog(*this);
}

// ============================================================================
// TrackMixList
// ============================================================================

TrackMixList::Rows::Rows(juce::ListBoxModel* model, TrackMixList& owner)
    : juce::ListBox("Layers", model), listOwner(owner) {
}

bool TrackMixList::Rows::keyPressed(const juce::KeyPress& key) {
    return listOwner.handleKey(key) || juce::ListBox::keyPressed(key);
}

TrackMixList::TrackMixList(std::vector<Row> entries, std::vector<int> selected,
                           std::function<void(std::vector<int>, int)> action, bool hasTimeRange)
    : rows(std::move(entries)), onAction(std::move(action)), list(this, *this),
      rangeAvailable(hasTimeRange) {
    addAndMakeVisible(list);
    list.setRowHeight(25);
    list.setMultipleSelectionEnabled(true);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setTitle(
        "Layer mix selection. M toggles keyboard selection; arrows browse, Space toggles a layer, "
        "A selects all, N clears.");

    for (auto* button : {&mute, &unmute, &solo, &unsolo}) {
        addAndMakeVisible(*button);
    }
    for (auto* button :
         {&copyTime, &cut, &cutClose, &removeTime, &closeGap, &insertTime, &previewClose}) {
        addAndMakeVisible(*button);
    }
    mute.onClick = [this] { activate(0); };
    unmute.onClick = [this] { activate(1); };
    solo.onClick = [this] { activate(2); };
    unsolo.onClick = [this] { activate(3); };
    cut.onClick = [this] { activate(4); };
    cutClose.onClick = [this] { activate(5); };
    removeTime.onClick = [this] { activate(6); };
    closeGap.onClick = [this] { activate(7); };
    insertTime.onClick = [this] { activate(8); };
    copyTime.onClick = [this] { activate(9); };
    previewClose.onClick = [this] { activate(10); };

    for (auto* button :
         {&copyTime, &cut, &cutClose, &removeTime, &closeGap, &insertTime, &previewClose}) {
        button->setTooltip(
            "Uses the current time selection on selected layers; other layers and markers stay in "
            "place.");
    }

    list.updateContent();
    juce::SparseSet<int> chosen;
    for (int index : selected) {
        if (index >= 0 && index < getNumRows()) {
            chosen.addRange({index, index + 1});
        }
    }
    if (chosen.size() > 0) {
        list.setSelectedRows(chosen, juce::sendNotification);
    }
    cursor = list.getSelectedRow() >= 0 ? list.getSelectedRow() : 0;
    selectedRowsChanged(cursor);
    setSize(700, 385);
}

void TrackMixList::focusList() {
    list.grabKeyboardFocus();
}

void TrackMixList::resized() {
    list.setBounds(getLocalBounds().withTrimmedBottom(100));
    auto bottom = getLocalBounds().reduced(8).removeFromBottom(90);
    auto buttons = bottom.removeFromTop(28);
    const int width = buttons.getWidth() / 4;
    mute.setBounds(buttons.removeFromLeft(width).reduced(2, 0));
    unmute.setBounds(buttons.removeFromLeft(width).reduced(2, 0));
    solo.setBounds(buttons.removeFromLeft(width).reduced(2, 0));
    unsolo.setBounds(buttons.reduced(2, 0));

    bottom.removeFromTop(4);
    auto edits = bottom.removeFromTop(28);
    const int editWidth = edits.getWidth() / 7;
    copyTime.setBounds(edits.removeFromLeft(editWidth).reduced(2, 0));
    cut.setBounds(edits.removeFromLeft(editWidth).reduced(2, 0));
    cutClose.setBounds(edits.removeFromLeft(editWidth).reduced(2, 0));
    removeTime.setBounds(edits.removeFromLeft(editWidth).reduced(2, 0));
    closeGap.setBounds(edits.removeFromLeft(editWidth).reduced(2, 0));
    insertTime.setBounds(edits.removeFromLeft(editWidth).reduced(2, 0));
    previewClose.setBounds(edits.reduced(2, 0));
}

void TrackMixList::paint(juce::Graphics& g) {
    const bool dark = isDarkTheme();
    g.fillAll(dialogBackgroundColour(dark));
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.7f));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(keyboardSelection
                   ? utf8Str("Arrows browse · Space toggles · A all · N none · M exits")
                   : juce::String("Select layers; M enables keyboard selection."),
               8, getHeight() - 24, getWidth() - 16, 20, juce::Justification::centredLeft, true);
}

int TrackMixList::getNumRows() {
    return static_cast<int>(rows.size());
}

juce::String TrackMixList::getNameForRow(int row) {
    if (row < 0 || row >= getNumRows()) {
        return {};
    }
    const auto& item = rows[static_cast<size_t>(row)];
    return "Layer " + juce::String(row + 1) + ", " + item.name +
           (item.muted ? ", muted" : ", unmuted") + (item.solo ? ", soloed" : ", not soloed");
}

void TrackMixList::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                    bool selected) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    paintSelectedRowBackground(g, width, height, dark, selected);
    if (keyboardSelection && row == cursor) {
        paintKeyboardCursorOutline(g, width, height, dark, 0.8f);
    }
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(getNameForRow(row), 8, 0, width - 16, height, juce::Justification::centredLeft,
               true);
}

void TrackMixList::selectedRowsChanged(int row) {
    if (!updating && row >= 0) {
        cursor = row;
    }
    const bool enabled = list.getNumSelectedRows() > 0;
    for (auto* button : {&mute, &unmute, &solo, &unsolo}) {
        button->setEnabled(enabled);
    }
    for (auto* button :
         {&copyTime, &cut, &cutClose, &removeTime, &closeGap, &insertTime, &previewClose}) {
        button->setEnabled(enabled && rangeAvailable);
    }
}

void TrackMixList::announce() {
    if (cursor >= 0 && cursor < getNumRows()) {
        juce::AccessibilityHandler::postAnnouncement(
            getNameForRow(cursor) +
                (list.isRowSelected(cursor) ? ", selected." : ", not selected."),
            juce::AccessibilityHandler::AnnouncementPriority::medium);
    }
}

bool TrackMixList::handleKey(const juce::KeyPress& key) {
    if (key.getModifiers().isAnyModifierKeyDown()) {
        return false;
    }
    const auto c =
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    if (c == 'm') {
        keyboardSelection = !keyboardSelection;
        juce::AccessibilityHandler::postAnnouncement(
            keyboardSelection ? "Keyboard layer selection on." : "Keyboard layer selection off.",
            juce::AccessibilityHandler::AnnouncementPriority::medium);
        announce();
        list.repaint();
        repaint();
        return true;
    }
    if (!keyboardSelection || getNumRows() == 0) {
        return false;
    }
    if (c == 'a' || c == 'n' || key.isKeyCode(juce::KeyPress::spaceKey)) {
        auto selected = list.getSelectedRows();
        if (c == 'a') {
            selected.addRange({0, getNumRows()});
        } else if (c == 'n') {
            selected.clear();
        } else if (selected.contains(cursor)) {
            selected.removeRange({cursor, cursor + 1});
        } else {
            selected.addRange({cursor, cursor + 1});
        }
        updating = true;
        list.setSelectedRows(selected, juce::sendNotification);
        updating = false;
        announce();
        list.repaint();
        return true;
    }
    int next = cursor;
    if (key.isKeyCode(juce::KeyPress::upKey)) {
        --next;
    } else if (key.isKeyCode(juce::KeyPress::downKey)) {
        ++next;
    } else if (key.isKeyCode(juce::KeyPress::homeKey)) {
        next = 0;
    } else if (key.isKeyCode(juce::KeyPress::endKey)) {
        next = getNumRows() - 1;
    } else {
        return false;
    }
    cursor = juce::jlimit(0, getNumRows() - 1, next);
    list.scrollToEnsureRowIsOnscreen(cursor);
    list.repaint();
    announce();
    return true;
}

void TrackMixList::activate(int action) {
    std::vector<int> indices;
    const auto selected = list.getSelectedRows();
    for (int i = 0; i < selected.size(); ++i) {
        indices.push_back(selected[i]);
    }
    if (indices.empty()) {
        return;
    }
    onAction(std::move(indices), action);
    closeParentDialog(*this);
}

// ============================================================================
// MasterTrackList
// ============================================================================

MasterTrackList::TrackListBox::TrackListBox(MasterTrackList& parent)
    : juce::ListBox("Master tracks", &parent), owner(parent) {
}

bool MasterTrackList::TrackListBox::keyPressed(const juce::KeyPress& key) {
    if (key.isKeyCode(juce::KeyPress::F2Key) && !key.getModifiers().isAnyModifierKeyDown()) {
        owner.activate(getSelectedRow(), 4);
        return true;
    }
    const auto mods = key.getModifiers();
    if (!mods.isCommandDown() && !mods.isAltDown() && !mods.isCtrlDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            's') {
        owner.activate(getSelectedRow(), mods.isShiftDown() ? 6 : 5);
        return true;
    }
    return juce::ListBox::keyPressed(key);
}

MasterTrackList::MasterTrackList(std::vector<carta::MasterTrack> entries, int selected,
                                 std::function<void(int, int)> activate)
    : tracks(std::move(entries)), onActivate(std::move(activate)), list(*this) {
    addAndMakeVisible(list);
    list.setRowHeight(26);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    addAndMakeVisible(previewBoundary);
    addAndMakeVisible(playTrack);
    addAndMakeVisible(zoomTrack);
    addAndMakeVisible(renameTrack);
    addAndMakeVisible(selectTrackTime);

    previewBoundary.onClick = [this] { this->activate(list.getSelectedRow(), 1); };
    playTrack.onClick = [this] { this->activate(list.getSelectedRow(), 2); };
    zoomTrack.onClick = [this] { this->activate(list.getSelectedRow(), 3); };
    renameTrack.onClick = [this] { this->activate(list.getSelectedRow(), 4); };
    selectTrackTime.onClick = [this] { this->activate(list.getSelectedRow(), 5); };

    list.setTitle(
        "Master tracks; Return moves the playhead, F2 renames, S selects time on all tracks, "
        "Shift S selects time on selected tracks");
    setSize(650, 330);
    list.updateContent();
    if (selected >= 0 && selected < getNumRows()) {
        list.selectRow(selected);
    }
}

void MasterTrackList::focusList() {
    list.grabKeyboardFocus();
}

void MasterTrackList::resized() {
    list.setBounds(getLocalBounds().withTrimmedTop(27).withTrimmedBottom(62));
    auto controls = getLocalBounds().reduced(8).withY(getHeight() - 59).withHeight(28);
    std::array<juce::Button*, 5> buttons{&previewBoundary, &playTrack, &zoomTrack, &renameTrack,
                                         &selectTrackTime};
    const std::array<int, 5> weights{155, 110, 130, 92, 110};
    const int available = std::max(0, controls.getWidth() - 32);
    for (size_t i = 0; i < buttons.size(); ++i) {
        const int width = i + 1 == buttons.size() ? controls.getWidth()
                                                  : std::max(0, (available * weights[i]) / 597);
        buttons[i]->setBounds(controls.removeFromLeft(width));
        if (i + 1 < buttons.size()) {
            controls.removeFromLeft(std::min(8, controls.getWidth()));
        }
    }
}

void MasterTrackList::paint(juce::Graphics& g) {
    const bool dark = isDarkTheme();
    g.fillAll(dialogBackgroundColour(dark));
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("#", 8, 3, 26, 20, juce::Justification::centredLeft);
    g.drawText("Name", 36, 3, std::max(0, getWidth() - 370), 20, juce::Justification::centredLeft);
    g.drawText("Start sample", getWidth() - 330, 3, 105, 20, juce::Justification::centredRight);
    g.drawText("End sample", getWidth() - 220, 3, 105, 20, juce::Justification::centredRight);
    g.drawText("Duration", getWidth() - 110, 3, 100, 20, juce::Justification::centredRight);
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.65f));
    g.drawText(
        utf8Str(
            "Return moves playhead · F2 renames · S selects time · Shift-S uses selected tracks"),
        8, getHeight() - 22, getWidth() - 16, 20, juce::Justification::centredLeft);
}

int MasterTrackList::getNumRows() {
    return static_cast<int>(tracks.size());
}

juce::String MasterTrackList::getNameForRow(int row) {
    if (row < 0 || row >= getNumRows()) {
        return {};
    }
    const auto& track = tracks[static_cast<size_t>(row)];
    return "Track " + juce::String(row + 1) + ", " + track.name + ", start sample " +
           juce::String(track.range.start) + ", end sample " + juce::String(track.range.end) +
           " exclusive, duration " + juce::String(track.range.end - track.range.start) + " samples";
}

void MasterTrackList::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                       bool selected) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    paintSelectedRowBackground(g, width, height, dark, selected);
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    const auto& track = tracks[static_cast<size_t>(row)];
    g.drawText(juce::String(row + 1), 8, 0, 26, height, juce::Justification::centredLeft);
    g.drawText(track.name, 36, 0, std::max(0, width - 370), height,
               juce::Justification::centredLeft, true);
    g.drawText(juce::String(track.range.start), width - 330, 0, 105, height,
               juce::Justification::centredRight);
    g.drawText(juce::String(track.range.end), width - 220, 0, 105, height,
               juce::Justification::centredRight);
    g.drawText(
        juce::String(static_cast<double>(track.range.end - track.range.start) / carta::sessionRate,
                     3) +
            " s",
        width - 110, 0, 100, height, juce::Justification::centredRight);
}

void MasterTrackList::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    activate(row, 0);
}

void MasterTrackList::returnKeyPressed(int row) {
    activate(row, 0);
}

void MasterTrackList::activate(int row, int action) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    onActivate(row, action);
    closeParentDialog(*this);
}

// ============================================================================
// OverlapList
// ============================================================================

OverlapList::OverlapList(std::vector<Row> entries, int selected, std::function<void(int)> activate)
    : rows(std::move(entries)), onActivate(std::move(activate)), list("Clip overlaps", this) {
    addAndMakeVisible(list);
    list.setRowHeight(26);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setTitle("Clip overlaps. Press Return to select the overlap time on its layer.");
    setSize(650, 300);
    list.updateContent();
    if (selected >= 0 && selected < getNumRows()) {
        list.selectRow(selected);
    }
}

void OverlapList::focusList() {
    list.grabKeyboardFocus();
}

void OverlapList::resized() {
    list.setBounds(getLocalBounds().withTrimmedTop(27).withTrimmedBottom(22));
}

void OverlapList::paint(juce::Graphics& g) {
    const bool dark = isDarkTheme();
    g.fillAll(dialogBackgroundColour(dark));
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("Layer", 8, 3, std::max(0, getWidth() - 280), 20, juce::Justification::centredLeft);
    g.drawText("Start sample", getWidth() - 265, 3, 125, 20, juce::Justification::centredRight);
    g.drawText("End sample", getWidth() - 135, 3, 125, 20, juce::Justification::centredRight);
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.65f));
    g.drawText("Return or double-click selects the overlap time; end is exclusive.", 8,
               getHeight() - 22, getWidth() - 16, 20, juce::Justification::centredLeft, true);
}

int OverlapList::getNumRows() {
    return static_cast<int>(rows.size());
}

juce::String OverlapList::getNameForRow(int index) {
    if (index < 0 || index >= getNumRows()) {
        return {};
    }
    const auto& row = rows[static_cast<size_t>(index)];
    return "Layer " + juce::String(row.range.track + 1) + ", " + row.trackName +
           ", overlapping clips, samples " + juce::String(row.range.start) + " to " +
           juce::String(row.range.end) + ", end exclusive. Signals sum when audible.";
}

void OverlapList::paintListBoxItem(int index, juce::Graphics& g, int width, int height,
                                   bool selected) {
    if (index < 0 || index >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    paintSelectedRowBackground(g, width, height, dark, selected);
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    const auto& row = rows[static_cast<size_t>(index)];
    g.drawText(juce::String(row.range.track + 1) + ". " + row.trackName, 8, 0,
               std::max(0, width - 280), height, juce::Justification::centredLeft, true);
    g.drawText(juce::String(row.range.start), width - 265, 0, 125, height,
               juce::Justification::centredRight);
    g.drawText(juce::String(row.range.end), width - 135, 0, 125, height,
               juce::Justification::centredRight);
}

void OverlapList::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    activate(row);
}

void OverlapList::returnKeyPressed(int row) {
    activate(row);
}

void OverlapList::activate(int row) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    onActivate(row);
    closeParentDialog(*this);
}

// ============================================================================
// QuietGapList
// ============================================================================

QuietGapList::GapRows::GapRows(juce::ListBoxModel* listModel, QuietGapList& owner)
    : juce::ListBox("Detected master-mix gaps", listModel), gapList(owner) {
}

bool QuietGapList::GapRows::keyPressed(const juce::KeyPress& key) {
    if (!key.getModifiers().isAnyModifierKeyDown()) {
        const auto character =
            juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
        if (character == 's' || character == 'm' || character == 'e' ||
            (character == 'd' && gapList.sourceScope)) {
            gapList.activate(getSelectedRow(), character == 's'   ? QuietGapAction::start
                                               : character == 'm' ? QuietGapAction::middle
                                               : character == 'e' ? QuietGapAction::end
                                                                  : QuietGapAction::detach);
            return true;
        }
    }
    return juce::ListBox::keyPressed(key);
}

QuietGapList::QuietGapList(std::vector<Row> entries,
                           std::function<juce::String(int, QuietGapAction)> activate,
                           std::function<void()> selected, bool sourceSilence)
    : rows(std::move(entries)), onActivate(std::move(activate)), onSelected(std::move(selected)),
      list(this, *this), sourceScope(sourceSilence) {
    addAndMakeVisible(list);
    list.setName(sourceScope ? "Detected source silence" : "Detected master-mix gaps");
    list.setTitle(
        sourceScope
            ? "Detected source silence. Return selects time on the clip's track, including "
              "overlapping clips; D detaches the safe interior from the scanned clip. S, M and E "
              "audition the full mix at the start, midpoint and end."
            : "Detected master-mix quiet gaps. Muted or cancelling sources may contain audio. "
              "Return selects a gap on all tracks; S, M and E audition its start, midpoint and "
              "end.");
    list.setRowHeight(26);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    addAndMakeVisible(error);
    error.setTitle("Quiet gap action status");
    addAndMakeVisible(select);
    addAndMakeVisible(audition);
    addAndMakeVisible(auditionStart);
    addAndMakeVisible(auditionEnd);
    addAndMakeVisible(cancel);

    if (sourceScope) {
        addAndMakeVisible(detach);
        detach.setTooltip(
            "Detach only the conservative interior of this silence from the scanned clip; other "
            "clips remain unchanged.");
        detach.onClick = [this] { activateRow(QuietGapAction::detach); };

        addAndMakeVisible(detachAll);
        detachAll.setTooltip(
            "Detach every available conservative interior from the scanned clip as one edit. Gaps "
            "without an interior are skipped.");
        detachAll.onClick = [this] { this->activate(-1, QuietGapAction::detachAll); };
        detachAll.setEnabled(std::any_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.detachable.end > row.detachable.start;
        }));
    }

    select.onClick = [this] { activateRow(QuietGapAction::select); };
    audition.onClick = [this] { activateRow(QuietGapAction::middle); };
    auditionStart.onClick = [this] { activateRow(QuietGapAction::start); };
    auditionEnd.onClick = [this] { activateRow(QuietGapAction::end); };
    cancel.onClick = [this] { close(); };

    setSize(sourceScope ? 860 : 620, 320);
    list.updateContent();
    if (!rows.empty()) {
        list.selectRow(0);
    }
    selectedRowsChanged(list.getSelectedRow());
}

void QuietGapList::focusList() {
    list.grabKeyboardFocus();
}

void QuietGapList::resized() {
    auto area = getLocalBounds().reduced(10);
    list.setBounds(area.removeFromTop(std::max(0, area.getHeight() - 66)));
    error.setBounds(area.removeFromTop(30));
    auto buttons = area.removeFromTop(32);
    select.setBounds(buttons.removeFromLeft(100).reduced(0, 2));
    buttons.removeFromLeft(5);
    if (sourceScope) {
        detach.setBounds(buttons.removeFromLeft(125).reduced(0, 2));
        buttons.removeFromLeft(5);
        detachAll.setBounds(buttons.removeFromLeft(105).reduced(0, 2));
        buttons.removeFromLeft(5);
    }
    auditionStart.setBounds(buttons.removeFromLeft(110).reduced(0, 2));
    buttons.removeFromLeft(5);
    audition.setBounds(buttons.removeFromLeft(130).reduced(0, 2));
    buttons.removeFromLeft(5);
    auditionEnd.setBounds(buttons.removeFromLeft(110).reduced(0, 2));
    buttons.removeFromLeft(5);
    cancel.setBounds(buttons.removeFromLeft(80).reduced(0, 2));
}

void QuietGapList::paint(juce::Graphics& g) {
    g.fillAll(dialogBackgroundColour(isDarkTheme()));
}

int QuietGapList::getNumRows() {
    return static_cast<int>(rows.size());
}

juce::String QuietGapList::getNameForRow(int index) {
    if (index < 0 || index >= getNumRows()) {
        return {};
    }
    const auto& row = rows[static_cast<size_t>(index)];
    return (sourceScope ? "Source silence, samples " : "Master-mix quiet gap, samples ") +
           juce::String(row.range.start) + " to " + juce::String(row.range.end) +
           ", end exclusive. " +
           (sourceScope  ? (row.detachable.end > row.detachable.start
                                ? "Safe detach interior " + juce::String(row.detachable.start) +
                                      " to " + juce::String(row.detachable.end) + ". "
                                : juce::String("No safe detach interior. ")) +
                               "Selection includes overlapping clips."
            : row.marked ? "Contains a marker."
                         : "No marker in gap.");
}

void QuietGapList::paintListBoxItem(int index, juce::Graphics& g, int width, int height,
                                    bool selected) {
    if (index < 0 || index >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    paintSelectedRowBackground(g, width, height, dark, selected);
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    const auto& row = rows[static_cast<size_t>(index)];
    const auto suffix = sourceScope
                            ? (row.detachable.end > row.detachable.start
                                   ? utf8Str("  ·  detach ") + juce::String(row.detachable.start) +
                                         utf8Str(" – ") + juce::String(row.detachable.end)
                                   : juce::String(utf8Str("  ·  no safe detach interior")))
                        : row.marked ? juce::String(utf8Str("  ·  marker present"))
                                     : juce::String();
    g.drawText(juce::String(row.range.start) + utf8Str(" – ") + juce::String(row.range.end) +
                   " samples" + suffix,
               8, 0, std::max(0, width - 16), height, juce::Justification::centredLeft, true);
}

void QuietGapList::selectedRowsChanged(int row) {
    if (sourceScope) {
        detach.setEnabled(row >= 0 && row < getNumRows() &&
                          rows[static_cast<size_t>(row)].detachable.end >
                              rows[static_cast<size_t>(row)].detachable.start);
    }
}

void QuietGapList::returnKeyPressed(int row) {
    activate(row, QuietGapAction::select);
}

void QuietGapList::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    activate(row, QuietGapAction::select);
}

void QuietGapList::activateRow(QuietGapAction action) {
    activate(list.getSelectedRow(), action);
}

void QuietGapList::activate(int row, QuietGapAction action) {
    if (action != QuietGapAction::detachAll && (row < 0 || row >= getNumRows())) {
        return;
    }
    const auto problem = onActivate(row, action);
    if (problem.isNotEmpty()) {
        error.setText(problem, juce::dontSendNotification);
        return;
    }
    if (action != QuietGapAction::select && action != QuietGapAction::detach &&
        action != QuietGapAction::detachAll) {
        error.setText(action == QuietGapAction::start ? "Auditioning the gap start."
                      : action == QuietGapAction::end ? "Auditioning the gap end."
                                                      : "Auditioning the gap midpoint.",
                      juce::dontSendNotification);
    } else {
        auto finished = onSelected;
        close();
        if (finished) {
            juce::MessageManager::callAsync(std::move(finished));
        }
    }
}

void QuietGapList::close() {
    closeParentDialog(*this);
}

// ============================================================================
// QuietMarkerSuggestions
// ============================================================================

QuietMarkerSuggestions::QuietMarkerSuggestions(
    juce::String initial, std::function<juce::String(const juce::String&)> accept,
    std::function<juce::String(const juce::String&)> auditionLine,
    std::function<juce::String(const juce::String&)> selectLine,
    std::function<void(std::function<void()>)> browseGaps)
    : onAccept(std::move(accept)), onAudition(std::move(auditionLine)),
      onSelect(std::move(selectLine)), onBrowse(std::move(browseGaps)) {
    addAndMakeVisible(explanation);
    explanation.setText("Edit sample and name lines. Mix-gap selection covers all tracks.",
                        juce::dontSendNotification);

    addAndMakeVisible(editor);
    editor.setMultiLine(true, false);
    editor.setReturnKeyStartsNewLine(true);
    editor.setScrollbarsShown(true);
    editor.setTitle("Suggested marker samples and names");
    editor.setText(initial, juce::dontSendNotification);

    if (onBrowse) {
        addAndMakeVisible(browse);
        browse.onClick = [this] {
            auto* self = this;
            onBrowse([safe = juce::Component::SafePointer<QuietMarkerSuggestions>(self)] {
                if (safe != nullptr) {
                    safe->close();
                }
            });
        };
    }

    addAndMakeVisible(error);
    error.setTitle("Suggestion validation");
    addAndMakeVisible(apply);
    addAndMakeVisible(audition);
    if (onSelect) {
        addAndMakeVisible(select);
    }
    addAndMakeVisible(cancel);

    apply.onClick = [this] {
        const auto problem = onAccept(editor.getText());
        if (problem.isNotEmpty()) {
            error.setText(problem, juce::dontSendNotification);
            return;
        }
        close();
    };
    audition.onClick = [this] {
        const auto problem = onAudition(currentLine());
        error.setText(problem.isEmpty() ? "Auditioning the current line." : problem,
                      juce::dontSendNotification);
    };
    select.onClick = [this] {
        if (!onSelect) {
            return;
        }
        const auto problem = onSelect(currentLine());
        if (problem.isNotEmpty()) {
            error.setText(problem, juce::dontSendNotification);
            return;
        }
        close();
    };
    cancel.onClick = [this] { close(); };
    setSize(680, 400);
}

void QuietMarkerSuggestions::focusEditor() {
    editor.grabKeyboardFocus();
}

void QuietMarkerSuggestions::resized() {
    auto area = getLocalBounds().reduced(10);
    auto top = area.removeFromTop(28);
    if (onBrowse) {
        browse.setBounds(top.removeFromRight(145).reduced(0, 2));
    }
    explanation.setBounds(top);
    auto bottom = area.removeFromBottom(66);
    error.setBounds(bottom.removeFromTop(30));
    apply.setBounds(bottom.removeFromLeft(130).reduced(0, 2));
    bottom.removeFromLeft(8);
    audition.setBounds(bottom.removeFromLeft(170).reduced(0, 2));
    bottom.removeFromLeft(8);
    if (onSelect) {
        select.setBounds(bottom.removeFromLeft(110).reduced(0, 2));
        bottom.removeFromLeft(8);
    }
    cancel.setBounds(bottom.removeFromLeft(90).reduced(0, 2));
    editor.setBounds(area.reduced(0, 4));
}

void QuietMarkerSuggestions::paint(juce::Graphics& g) {
    g.fillAll(dialogBackgroundColour(isDarkTheme()));
}

void QuietMarkerSuggestions::close() {
    closeParentDialog(*this);
}

juce::String QuietMarkerSuggestions::currentLine() const {
    const auto text = editor.getText();
    const int caret = juce::jlimit(0, text.length(), editor.getCaretPosition());
    const int start = text.substring(0, caret).lastIndexOfChar('\n') + 1;
    const int end = text.indexOfChar(start, '\n');
    return text.substring(start, end < 0 ? text.length() : end);
}

// ============================================================================
// MarkerNamesEditor
// ============================================================================

MarkerNamesEditor::MarkerNamesEditor(juce::String initial, int count,
                                     std::function<juce::String(const juce::String&)> accept,
                                     std::function<void()> closed, bool positions)
    : onAccept(std::move(accept)), onClosed(std::move(closed)) {
    addAndMakeVisible(explanation);
    explanation.setText((positions ? "One position (samples or time)" : "One name") +
                            juce::String(" per line for ") + juce::String(count) +
                            " selected markers, in marker-list order.",
                        juce::dontSendNotification);

    addAndMakeVisible(editor);
    editor.setMultiLine(true, false);
    editor.setReturnKeyStartsNewLine(true);
    editor.setScrollbarsShown(true);
    editor.setTitle(positions ? "Selected marker positions in marker-list order"
                              : "Selected marker names in marker-list order");
    editor.setText(initial, juce::dontSendNotification);

    addAndMakeVisible(error);
    error.setTitle(positions ? "Marker position validation" : "Marker name validation");
    if (positions) {
        apply.setButtonText("Apply positions");
    }
    addAndMakeVisible(apply);
    addAndMakeVisible(cancel);

    apply.onClick = [this] {
        const auto problem = onAccept(editor.getText());
        if (problem.isNotEmpty()) {
            error.setText(problem, juce::dontSendNotification);
            return;
        }
        close();
    };
    cancel.onClick = [this] { close(); };
    setSize(560, 370);
}

void MarkerNamesEditor::focusEditor() {
    editor.grabKeyboardFocus();
}

void MarkerNamesEditor::resized() {
    auto area = getLocalBounds().reduced(10);
    explanation.setBounds(area.removeFromTop(28));
    auto bottom = area.removeFromBottom(66);
    error.setBounds(bottom.removeFromTop(30));
    apply.setBounds(bottom.removeFromLeft(120).reduced(0, 2));
    bottom.removeFromLeft(8);
    cancel.setBounds(bottom.removeFromLeft(90).reduced(0, 2));
    editor.setBounds(area.reduced(0, 4));
}

void MarkerNamesEditor::paint(juce::Graphics& g) {
    g.fillAll(dialogBackgroundColour(isDarkTheme()));
}

void MarkerNamesEditor::close() {
    if (onClosed) {
        onClosed();
    }
    closeParentDialog(*this);
}

// ============================================================================
// MarkerList
// ============================================================================

MarkerList::MarkerRows::MarkerRows(juce::ListBoxModel* model, MarkerList& owner)
    : juce::ListBox("Track markers", model), markerList(owner) {
}

bool MarkerList::MarkerRows::keyPressed(const juce::KeyPress& key) {
    if (markerList.handleListKey(key)) {
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown()) {
        if (key.getKeyCode() == juce::KeyPress::F2Key) {
            markerList.activate(getSelectedRow(), getNumSelectedRows() > 1 ? 5 : 1);
            return true;
        }
        if (juce::CharacterFunctions::toLowerCase(
                static_cast<juce::juce_wchar>(key.getKeyCode())) == 'p') {
            markerList.activate(getSelectedRow(), getNumSelectedRows() > 1 ? 7 : 2);
            return true;
        }
        if (juce::CharacterFunctions::toLowerCase(
                static_cast<juce::juce_wchar>(key.getKeyCode())) == 's') {
            markerList.activate(getSelectedRow(), 4);
            return true;
        }
        if (juce::CharacterFunctions::toLowerCase(
                static_cast<juce::juce_wchar>(key.getKeyCode())) == 'b') {
            markerList.activate(getSelectedRow(), 6);
            return true;
        }
    }
    return juce::ListBox::keyPressed(key);
}

MarkerList::MarkerList(std::vector<Row> entries, int selected,
                       std::function<void(std::vector<int>, int, int)> activate)
    : rows(std::move(entries)), onActivate(std::move(activate)), list(this, *this) {
    addAndMakeVisible(list);
    list.setRowHeight(26);
    list.setMultipleSelectionEnabled(true);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    addAndMakeVisible(rename);
    addAndMakeVisible(move);
    addAndMakeVisible(shift);
    addAndMakeVisible(remove);
    addAndMakeVisible(audition);

    rename.onClick = [this] { activateRow(list.getNumSelectedRows() > 1 ? 5 : 1); };
    move.onClick = [this] { activateRow(list.getNumSelectedRows() > 1 ? 7 : 2); };
    shift.onClick = [this] { activateRow(4); };
    remove.onClick = [this] { activateRow(3); };
    audition.onClick = [this] { activateRow(6); };

    list.setTitle(
        "Track markers; Return selects, B auditions boundary, F2 renames selected markers, P sets "
        "position, S shifts, Delete removes. Press M for keyboard multiple selection.");
    setSize(650, 330);
    list.updateContent();
    if (selected >= 0 && selected < getNumRows()) {
        list.selectRow(selected);
    }
    selectedRowsChanged(list.getSelectedRow());
}

void MarkerList::focusList() {
    list.grabKeyboardFocus();
}

void MarkerList::selectRows(const std::vector<int>& indices) {
    juce::SparseSet<int> selected;
    for (const auto row : indices) {
        if (row >= 0 && row < getNumRows()) {
            selected.addRange({row, row + 1});
        }
    }
    if (selected.size() > 0) {
        list.setSelectedRows(selected, juce::sendNotification);
    }
}

void MarkerList::resized() {
    list.setBounds(getLocalBounds().withTrimmedTop(27).withTrimmedBottom(62));
    rename.setBounds(8, getHeight() - 59, 110, 28);
    move.setBounds(126, getHeight() - 59, 125, 28);
    shift.setBounds(259, getHeight() - 59, 100, 28);
    remove.setBounds(367, getHeight() - 59, 90, 28);
    audition.setBounds(465, getHeight() - 59, 150, 28);
}

void MarkerList::paint(juce::Graphics& g) {
    const bool dark = isDarkTheme();
    g.fillAll(dialogBackgroundColour(dark));
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("#", 8, 3, 28, 20, juce::Justification::centredLeft);
    g.drawText("Name", 40, 3, std::max(0, getWidth() - 392), 20, juce::Justification::centredLeft);
    g.drawText("Sample", getWidth() - 345, 3, 110, 20, juce::Justification::centredRight);
    g.drawText("Status", getWidth() - 220, 3, 212, 20, juce::Justification::centredLeft);
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.65f));
    g.drawText(
        keyboardSelection
            ? utf8Str("Arrows browse · Space toggles · A all · N none · B auditions · M exits")
            : utf8Str("Return selects · B auditions · F2 renames · P sets position · S shifts · M "
                      "selects multiple"),
        8, getHeight() - 24, getWidth() - 16, 20, juce::Justification::centredLeft, true);
}

juce::String MarkerList::issues(const carta::MarkerInspection& info) {
    juce::StringArray result;
    if (!info.validPosition) {
        result.add("Invalid position");
    }
    if (info.outside) {
        result.add("Outside master");
    }
    if (info.duplicate) {
        result.add("Duplicate sample");
    }
    if (info.emptyName) {
        result.add("Empty name");
    }
    if (info.offGrid) {
        result.add("Rounded to sample");
    }
    return result.isEmpty() ? juce::String("OK") : result.joinIntoString(", ");
}

int MarkerList::getNumRows() {
    return static_cast<int>(rows.size());
}

juce::String MarkerList::getNameForRow(int row) {
    if (row < 0 || row >= getNumRows()) {
        return {};
    }
    const auto& item = rows[static_cast<size_t>(row)];
    return "Marker " + juce::String(item.info.index + 1) + ", " +
           (item.name.trim().isEmpty() ? juce::String("unnamed") : item.name) + ", " +
           (item.info.validPosition ? "sample " + juce::String(item.info.sample)
                                    : juce::String("invalid position")) +
           ", " + issues(item.info);
}

void MarkerList::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                  bool selected) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    paintSelectedRowBackground(g, width, height, dark, selected);
    if (keyboardSelection && row == keyboardCursor) {
        paintKeyboardCursorOutline(g, width, height, dark, 0.75f);
    }
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    const auto& item = rows[static_cast<size_t>(row)];
    g.drawText(juce::String(item.info.index + 1), 8, 0, 28, height,
               juce::Justification::centredLeft);
    g.drawText(item.name.trim().isEmpty() ? juce::String("(unnamed)") : item.name, 40, 0,
               std::max(0, width - 392), height, juce::Justification::centredLeft, true);
    g.drawText(item.info.validPosition ? juce::String(item.info.sample) : juce::String("Invalid"),
               width - 345, 0, 110, height, juce::Justification::centredRight);
    g.drawText(issues(item.info), width - 220, 0, 212, height, juce::Justification::centredLeft,
               true);
}

void MarkerList::selectedRowsChanged(int row) {
    if (!updatingKeyboardSelection && row >= 0) {
        keyboardCursor = row;
    }
    const int count = list.getNumSelectedRows();
    rename.setEnabled(count > 0);
    move.setEnabled(count > 0);
    shift.setEnabled(count > 0);
    remove.setEnabled(count > 0);
    const int selected = list.getSelectedRow();
    audition.setEnabled(count == 1 && selected >= 0 && selected < getNumRows() &&
                        rows[static_cast<size_t>(selected)].info.validPosition &&
                        !rows[static_cast<size_t>(selected)].info.outside);
}

void MarkerList::listBoxItemClicked(int row, const juce::MouseEvent&) {
    if (keyboardSelection && row >= 0 && row < getNumRows()) {
        const int old = keyboardCursor;
        keyboardCursor = row;
        list.repaintRow(old);
        list.repaintRow(row);
    }
}

bool MarkerList::handleListKey(const juce::KeyPress& key) {
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown() || mods.isShiftDown()) {
        return false;
    }
    const auto character =
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    if (character == 'm') {
        keyboardSelection = !keyboardSelection;
        if (keyboardSelection) {
            keyboardCursor = juce::jlimit(0, std::max(0, getNumRows() - 1),
                                          list.getLastRowSelected() >= 0 ? list.getLastRowSelected()
                                                                         : keyboardCursor);
            announceCursor("Keyboard marker selection on. ");
        } else {
            juce::AccessibilityHandler::postAnnouncement(
                "Keyboard marker selection off.",
                juce::AccessibilityHandler::AnnouncementPriority::medium);
        }
        list.repaint();
        repaint();
        return true;
    }
    if (!keyboardSelection || getNumRows() == 0) {
        return false;
    }
    if (character == 'a' || character == 'n' || key.isKeyCode(juce::KeyPress::spaceKey)) {
        auto selected = list.getSelectedRows();
        if (character == 'a') {
            selected.addRange({0, getNumRows()});
        } else if (character == 'n') {
            selected.clear();
        } else if (selected.contains(keyboardCursor)) {
            selected.removeRange({keyboardCursor, keyboardCursor + 1});
        } else {
            selected.addRange({keyboardCursor, keyboardCursor + 1});
        }
        updatingKeyboardSelection = true;
        list.setSelectedRows(selected, juce::sendNotification);
        updatingKeyboardSelection = false;
        announceCursor({});
        list.repaintRow(keyboardCursor);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::deleteKey) || key.isKeyCode(juce::KeyPress::backspaceKey)) {
        activate(keyboardCursor, 3);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::F2Key)) {
        activate(list.getNumSelectedRows() == 1 ? list.getSelectedRow() : keyboardCursor,
                 list.getNumSelectedRows() > 1 ? 5 : 1);
        return true;
    }
    if (character == 'p') {
        activate(list.getNumSelectedRows() == 1 ? list.getSelectedRow() : keyboardCursor,
                 list.getNumSelectedRows() > 1 ? 7 : 2);
        return true;
    }
    if (character == 's') {
        activate(keyboardCursor, 4);
        return true;
    }
    if (character == 'b') {
        activate(list.getSelectedRow(), 6);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::returnKey)) {
        activate(keyboardCursor, 0);
        return true;
    }
    int next = keyboardCursor;
    if (key.isKeyCode(juce::KeyPress::upKey)) {
        --next;
    } else if (key.isKeyCode(juce::KeyPress::downKey)) {
        ++next;
    } else if (key.isKeyCode(juce::KeyPress::homeKey)) {
        next = 0;
    } else if (key.isKeyCode(juce::KeyPress::endKey)) {
        next = getNumRows() - 1;
    } else {
        return false;
    }
    next = juce::jlimit(0, getNumRows() - 1, next);
    if (next != keyboardCursor) {
        const int old = keyboardCursor;
        keyboardCursor = next;
        list.repaintRow(old);
        list.repaintRow(next);
        list.scrollToEnsureRowIsOnscreen(next);
        announceCursor({});
    }
    return true;
}

void MarkerList::announceCursor(const juce::String& prefix) {
    if (keyboardCursor < 0 || keyboardCursor >= getNumRows()) {
        return;
    }
    juce::AccessibilityHandler::postAnnouncement(
        prefix + getNameForRow(keyboardCursor) +
            (list.isRowSelected(keyboardCursor) ? ", selected. " : ", not selected. ") +
            juce::String(list.getNumSelectedRows()) + " markers selected.",
        juce::AccessibilityHandler::AnnouncementPriority::medium);
}

void MarkerList::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    activate(row, 0);
}

void MarkerList::returnKeyPressed(int row) {
    activate(row, 0);
}

void MarkerList::deleteKeyPressed(int row) {
    activate(row, 3);
}

void MarkerList::activateRow(int action) {
    activate(list.getSelectedRow(), action);
}

void MarkerList::activate(int row, int action) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    if ((action == 1 || action == 2 || action == 6) && list.getNumSelectedRows() != 1) {
        return;
    }
    if (action == 6 && (!rows[static_cast<size_t>(row)].info.validPosition ||
                        rows[static_cast<size_t>(row)].info.outside)) {
        return;
    }
    std::vector<int> targets;
    if (action == 3 || action == 4 || action == 5 || action == 7) {
        const auto selected = list.getSelectedRows();
        for (int i = 0; i < selected.size(); ++i) {
            targets.push_back(selected[i]);
        }
        if (targets.empty()) {
            return;
        }
    } else {
        targets.push_back(row);
    }
    onActivate(std::move(targets), action, row);
    closeParentDialog(*this);
}

// ============================================================================
// TrackSourceList
// ============================================================================

TrackSourceList::TrackSourceList(juce::String title, std::vector<carta::TrackSource> entries,
                                 std::vector<juce::String> results,
                                 std::function<void(std::shared_ptr<carta::Audio>)> relink)
    : trackName(std::move(title)), sources(std::move(entries)), checks(std::move(results)),
      list("Layer sources", this), onRelink(std::move(relink)) {
    addAndMakeVisible(list);
    list.setRowHeight(52);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(relinkButton);
    relinkButton.onClick = [this] {
        const int row = list.getSelectedRow();
        if (row < 0 || row >= getNumRows()) {
            return;
        }
        onRelink(sources[static_cast<size_t>(row)].audio);
        closeParentDialog(*this);
    };
    list.setTitle("Layer sources. Select a row, then choose Relink selected source to locate a "
                  "checksum-identical file.");
    setSize(700, 300);
    list.updateContent();
    if (!sources.empty()) {
        list.selectRow(0);
    }
}

void TrackSourceList::focusList() {
    list.grabKeyboardFocus();
}

void TrackSourceList::resized() {
    list.setBounds(getLocalBounds().withTrimmedTop(30).withTrimmedBottom(60));
    relinkButton.setBounds(8, getHeight() - 56, 180, 28);
}

void TrackSourceList::paint(juce::Graphics& g) {
    const bool dark = isDarkTheme();
    g.fillAll(dialogBackgroundColour(dark));
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(trackName + " - source files", 8, 3, getWidth() - 16, 24,
               juce::Justification::centredLeft, true);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("File status reflects the last check this session; files may change afterward.", 8,
               getHeight() - 23, getWidth() - 16, 20, juce::Justification::centredLeft, true);
}

int TrackSourceList::getNumRows() {
    return static_cast<int>(sources.size());
}

juce::String TrackSourceList::description(int row) const {
    const auto& entry = sources[static_cast<size_t>(row)];
    const auto& source = *entry.audio;
    const auto format =
        source.sourceFormat.isEmpty() ? juce::String("Unknown format") : source.sourceFormat;
    const auto bits = source.sourceBitsPerSample > 0
                          ? juce::String(source.sourceBitsPerSample) + "-bit reported"
                          : juce::String("bit depth unknown");
    const auto verified = static_cast<size_t>(row) < checks.size()
                              ? checks[static_cast<size_t>(row)] + "; "
                              : juce::String();
    return verified + format + ", " + juce::String(source.numChannels()) + " channels, " +
           juce::String(source.sampleRate, 0) + " Hz, " + bits + ", " +
           juce::String(source.numSamples()) + " frames (" + juce::String(source.length(), 3) +
           " s), " + juce::String(entry.clipCount) + (entry.clipCount == 1 ? " clip" : " clips");
}

juce::String TrackSourceList::getNameForRow(int row) {
    if (row < 0 || row >= getNumRows()) {
        return {};
    }
    const auto path = sources[static_cast<size_t>(row)].audio->file.getFullPathName();
    return (path.isEmpty() ? juce::String("Source without a path") : path) + ", " +
           description(row);
}

void TrackSourceList::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                       bool selected) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    paintSelectedRowBackground(g, width, height, dark, selected);
    g.setColour(listForegroundColour(dark));
    g.setFont(juce::FontOptions(12.0f));
    const auto path = sources[static_cast<size_t>(row)].audio->file.getFullPathName();
    g.drawText(path.isEmpty() ? juce::String("Source without a path") : path, 8, 2, width - 16, 22,
               juce::Justification::centredLeft, true);
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.7f));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(description(row), 8, 26, width - 16, 20, juce::Justification::centredLeft, true);
}

// ============================================================================
// KeyboardHelp & ImportIssueReport
// ============================================================================

KeyboardHelp::KeyboardHelp() {
    addAndMakeVisible(text);
    text.setMultiLine(true, true);
    text.setReadOnly(true);
    text.setCaretVisible(false);
    text.setScrollbarsShown(true);
    text.setTitle("Keyboard shortcuts");
    text.setText(
        "Keyboard shortcuts\n\n"
        "These commands apply when the timeline has keyboard focus. Command means Cmd on macOS and "
        "Ctrl on other platforms.\n\n"
        "FILE AND HISTORY\n"
        "Command+I   Import audio\n"
        "Command+O   Open project\n"
        "Command+S   Save project\n"
        "Command+Shift+S   Save project as\n"
        "Command+Z / Command+Shift+Z   Undo / redo\n\n"
        "Command+Shift+P   Search menu commands\n\n"
        "Command+Shift+I / J   Insert an MD track / set MD track range from selection\n\n"
        "EDITING\n"
        "Command+X / C   Cut / copy selected clips, or time selection\n"
        "Command+V   Paste at playhead\n"
        "Command+A   Select all audio\n"
        "Command+B   Split selected clips at playhead, or selection boundaries\n"
        "Command+D   Duplicate selected clip or clip group\n"
        "Command+Shift+D   Duplicate time selection after its end (overlap)\n"
        "Command+M   Add marker at playhead\n"
        "F2   Rename focused marker or selected clip\n"
        "Delete / Backspace   Delete focused marker, or selected time or clip\n"
        "Shift+Delete   Close selection on selected layers\n"
        "Command+Delete   Close selection on all layers\n"
        "Command+Alt+Left / Right   Move selected clip one sample\n\n"
        "F5 / F6   Move selected clip start earlier / later one sample\n"
        "F7 / F8   Move selected clip end earlier / later one sample\n\n"
        "SELECTION\n"
        "Command+Shift-click a clip title   Toggle it in clip selection\n"
        "Command+Shift-drag across clip bodies   Select intersecting clips\n"
        "G   Toggle the keyboard-focused clip in the clip selection\n"
        "Alt+G   Select the clip gap at the edit point\n"
        "T   Toggle the focused layer in the independent layer selection\n"
        "M / S   Toggle mute / solo on the focused layer\n"
        "Command+Shift-click a layer header   Toggle it in layer selection\n"
        "Shift+Left / Right   Create or extend selection one sample\n"
        "Shift+Up / Down   Extend or contract selection across layers\n"
        "I / O   Mark selection in / out at the playhead, including during playback\n"
        "Shift+Alt+Left / Right   Move selection end one sample\n"
        "Command+Shift+Alt+Left / Right   Move selection start one sample\n"
        "Command+Shift+Left / Right   Move the whole selection one sample\n"
        "Escape   Cancel a drag or applicable audition; otherwise clear selection\n\n"
        "NAVIGATION AND PLAYBACK\n"
        "Command+0   Return keyboard focus to the timeline\n"
        "F3   Browse layers and clips\n"
        "F4   Announce timeline status\n"
        "Left / Right   Move playhead one sample\n"
        "E   Set an edit point at the playhead on the focused layer\n"
        ", / .   Move the edit point one sample\n"
        "Alt+[ / ]   Move the edit point to a clip boundary\n"
        "Shift+Alt+[ / ]   Move the edit point to a boundary on any layer\n"
        "Alt+Page Up / Down   Move the edit point to a track marker\n"
        "Home / End   Move playhead to master start / end\n"
        "Page Up / Page Down   Previous / next track marker\n"
        "Shift+Page Up / Down   Focus previous / next marker without seeking\n"
        "F9 / F10   Move the selected marker one sample left / right\n"
        "P / B   Set focused marker position / audition its boundary\n"
        "Alt+Up / Down   Select previous / next track\n"
        "Shift+Alt+Up / Down   Decrease / increase track height\n"
        "Alt+Left / Right   Select previous / next clip on track\n"
        "Shift+F5 / F6   Focus previous / next clip without seeking\n"
        "Shift+F7 / F8   Focus previous / next track without seeking\n"
        "[ / ]   Previous / next clip boundary on selected track\n"
        "Shift+[ / ]   Previous / next clip boundary on any track\n"
        "+ or = / -   Zoom in / out around playhead\n"
        "Z   Toggle the previous zoom view\n"
        "Shift+Z   Zoom to selected clips\n"
        "Space   Play / pause\nK   Stop at playhead\n\n"
        "Mouse: drag a clip title to move; Option/Alt-drag copies it (Control-drag also on Linux). "
        "Drag the body to select time. Option/Alt-click cycles overlapping clips. Shift while "
        "dragging overrides snapping.\n",
        false);
    setSize(620, 500);
}

void KeyboardHelp::resized() {
    text.setBounds(getLocalBounds().reduced(8));
}

void KeyboardHelp::focusText() {
    text.grabKeyboardFocus();
}

ImportIssueReport::ImportIssueReport(const juce::String& report) {
    addAndMakeVisible(text);
    text.setMultiLine(true, true);
    text.setReadOnly(true);
    text.setCaretVisible(false);
    text.setScrollbarsShown(true);
    text.setTitle("Import issues");
    text.setText(report, false);
    setSize(650, 400);
}

void ImportIssueReport::resized() {
    text.setBounds(getLocalBounds().reduced(8));
}

void ImportIssueReport::focusText() {
    text.grabKeyboardFocus();
}

// ============================================================================
// CommandSearch
// ============================================================================

CommandSearch::SearchField::SearchField(CommandSearch& owner) : search(owner) {
}

bool CommandSearch::SearchField::keyPressed(const juce::KeyPress& key) {
    return search.handleKey(key) || juce::TextEditor::keyPressed(key);
}

CommandSearch::CommandSearch(std::vector<CommandSearchEntry> commands,
                             std::function<void(CommandSearchEntry)> activate)
    : entries(std::move(commands)), onActivate(std::move(activate)), query(*this),
      list("Commands", this) {
    addAndMakeVisible(query);
    query.setTitle("Search commands");
    query.setTextToShowWhenEmpty("Search commands", juce::Colours::grey);
    query.onTextChange = [this] { filter(); };

    addAndMakeVisible(list);
    list.setTitle("Commands; use up and down arrows, then Return to run");
    list.setRowHeight(26);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    setSize(680, 420);
    filter();
}

void CommandSearch::focusSearch() {
    query.grabKeyboardFocus();
}

void CommandSearch::resized() {
    auto area = getLocalBounds().reduced(8);
    query.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    list.setBounds(area);
}

void CommandSearch::filter() {
    visible.clear();
    juce::StringArray terms;
    terms.addTokens(query.getText().trim(), " \t\r\n", "");
    terms.removeEmptyStrings();
    for (size_t i = 0; i < entries.size(); ++i) {
        bool matches = true;
        for (const auto& term : terms) {
            if (!entries[i].path.containsIgnoreCase(term)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            visible.push_back(i);
        }
    }
    list.updateContent();
    if (!visible.empty()) {
        list.selectRow(0);
    } else {
        list.deselectAllRows();
    }
    list.repaint();
}

int CommandSearch::getNumRows() {
    return static_cast<int>(visible.size());
}

juce::String CommandSearch::getNameForRow(int row) {
    if (row < 0 || row >= getNumRows()) {
        return {};
    }
    const auto& entry = entries[visible[static_cast<size_t>(row)]];
    return entry.path + (entry.enabled ? juce::String() : " (unavailable)");
}

void CommandSearch::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                     bool selected) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    const bool dark = isDarkTheme();
    const auto& entry = entries[visible[static_cast<size_t>(row)]];
    paintSelectedRowBackground(g, width, height, dark, selected);
    g.setColour((dark ? juce::Colours::white : juce::Colours::black)
                    .withAlpha(entry.enabled ? 0.9f : 0.45f));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(getNameForRow(row), 8, 0, std::max(0, width - 16), height,
               juce::Justification::centredLeft, true);
}

bool CommandSearch::handleKey(const juce::KeyPress& key) {
    if (key.getModifiers().isAnyModifierKeyDown()) {
        return false;
    }
    if (key.isKeyCode(juce::KeyPress::escapeKey)) {
        closeParentDialog(*this);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::returnKey)) {
        run(list.getSelectedRow());
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)) {
        if (getNumRows() > 0) {
            const int next = juce::jlimit(0, getNumRows() - 1,
                                          list.getSelectedRow() +
                                              (key.isKeyCode(juce::KeyPress::downKey) ? 1 : -1));
            list.selectRow(next);
            list.scrollToEnsureRowIsOnscreen(next);
        }
        return true;
    }
    return false;
}

void CommandSearch::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    run(row);
}

void CommandSearch::returnKeyPressed(int row) {
    run(row);
}

void CommandSearch::run(int row) {
    if (row < 0 || row >= getNumRows()) {
        return;
    }
    const auto command = entries[visible[static_cast<size_t>(row)]];
    if (!command.enabled) {
        juce::AccessibilityHandler::postAnnouncement(
            "Command unavailable in the current context.",
            juce::AccessibilityHandler::AnnouncementPriority::medium);
        return;
    }
    const auto activate = onActivate;
    closeParentDialog(*this);
    activate(command);
}
