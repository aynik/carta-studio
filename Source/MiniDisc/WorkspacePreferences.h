/**
 * @file WorkspacePreferences.h
 * Persistent XML preferences for the MiniDisc authoring workspace panel.
 *
 * Stores dock visibility, height (in logical UI pixels within [100, 10000]),
 * and active workspace tab ("tracks" or "record"). Missing files preserve
 * defaults; malformed or future-schema files fail without mutating caller
 * state. Writes use atomic temporary replacement with checked-text framing.
 */

#pragma once

#include "IO/CheckedText.h"

#include <utility>

namespace carta {

// ============================================================================
// Preferences Model & Validation
// ============================================================================

/** Persisted layout and view state for the docked MiniDisc workspace. */
struct WorkspacePreferences {
    int height = 260;
    bool visible = true;
    juce::String view = "tracks";

    bool operator==(const WorkspacePreferences& other) const {
        return height == other.height && visible == other.visible && view == other.view;
    }
};

/** Validates bounds and allowed tab identifiers for workspace preferences. */
inline bool validWorkspacePreferences(const WorkspacePreferences& prefs) {
    return prefs.height >= 100 && prefs.height <= 10000 &&
           (prefs.view == "tracks" || prefs.view == "record");
}

// ============================================================================
// XML Persistence
// ============================================================================

/**
 * Loads MiniDisc workspace preferences from XML.
 *
 * A missing file succeeds and leaves `prefs` at its current defaults. Invalid,
 * oversized (>4096 bytes), or unknown-version files return false and leave
 * `prefs` untouched. Legacy tab names ("segments", "order", "checks", "map")
 * normalize forward to "record".
 */
inline bool loadWorkspacePreferences(const juce::File& file, WorkspacePreferences& prefs) {
    if (!file.exists()) {
        return true;
    }
    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > 4096) {
        return false;
    }

    auto xml = juce::XmlDocument::parse(file);
    if (!xml || !xml->hasTagName("CARTA_DISC_WORKSPACE") ||
        xml->getStringAttribute("version") != "1") {
        return false;
    }

    const auto height = xml->getStringAttribute("height");
    const auto visible = xml->getStringAttribute("visible");
    if (height.isEmpty() || height.length() > 5 || !height.containsOnly("0123456789") ||
        (visible != "0" && visible != "1")) {
        return false;
    }

    WorkspacePreferences loaded;
    loaded.height = height.getIntValue();
    loaded.visible = visible == "1";
    loaded.view = xml->getStringAttribute("view");
    if (loaded.view == "segments" || loaded.view == "order" || loaded.view == "checks" ||
        loaded.view == "map") {
        loaded.view = "record";
    }
    if (!validWorkspacePreferences(loaded)) {
        return false;
    }

    prefs = std::move(loaded);
    return true;
}

/**
 * Atomically writes valid MiniDisc workspace preferences to XML.
 *
 * On validation failure or directory/write failure, leaves any existing file
 * untouched and returns false.
 */
inline bool saveWorkspacePreferences(const juce::File& file, const WorkspacePreferences& prefs) {
    if (!validWorkspacePreferences(prefs) || file.getParentDirectory().createDirectory().failed()) {
        return false;
    }

    juce::XmlElement xml("CARTA_DISC_WORKSPACE");
    xml.setAttribute("version", 1);
    xml.setAttribute("height", prefs.height);
    xml.setAttribute("visible", prefs.visible ? "1" : "0");
    xml.setAttribute("view", prefs.view);

    auto temp = prepareCheckedText(file, xml.toString());
    return temp && replaceTemporaryAtomically(*temp, file);
}

} // namespace carta
