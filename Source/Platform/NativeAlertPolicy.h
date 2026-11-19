/**
 * Shared platform alert dismissal and button-mapping policy.
 *
 * Used by both AppKit (`NativeSavePrompt_mac.mm`) and GTK3
 * (`NativeTextPrompt_linux.cpp`) modal wrappers to keep Escape key binding and
 * response bounds checks consistent across platforms.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace carta {

/**
 * Determine which zero-based button index Escape should trigger, or `-1`.
 *
 * Prefers an explicit `"cancel"` or `"close"` button (case-insensitive). An
 * informational single-button alert can also be dismissed with Escape;
 * ambiguous multi-button choices stay unbound.
 */
inline int nativeAlertEscapeButton(const std::vector<std::string>& labels) {
    const auto findLabel = [&](const char* wanted) {
        for (size_t i = 0; i < labels.size(); ++i) {
            auto lower = labels[i];
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == wanted) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    if (const int cancel = findLabel("cancel"); cancel >= 0) {
        return cancel;
    }
    if (const int close = findLabel("close"); close >= 0) {
        return close;
    }
    return labels.size() == 1 ? 0 : -1;
}

/**
 * Validate a native alert response offset against `[0, count)` for 1..3 buttons.
 *
 * Returns the zero-based button index, or `-1` if out of range.
 */
inline int nativeAlertChoice(long offset, int count) {
    return count >= 1 && count <= 3 && offset >= 0 && offset < count ? static_cast<int>(offset)
                                                                     : -1;
}

} // namespace carta
