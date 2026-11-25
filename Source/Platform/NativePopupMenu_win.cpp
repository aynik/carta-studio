/**
 * @file NativePopupMenu_win.cpp
 * @brief Native Win32 popup menu bridge (`HMENU` / `TrackPopupMenuEx`) for Carta Studio.
 */

#include "Platform/NativeSavePrompt.h"
#include <juce_gui_basics/juce_gui_basics.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace carta {
namespace {

HMENU buildWin32MenuFromJuce(const juce::PopupMenu& source) {
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) {
        return nullptr;
    }

    juce::PopupMenu::MenuItemIterator it(source);
    bool lastWasSeparator = true;
    while (it.next()) {
        const auto& item = it.getItem();
        if (item.isSeparator) {
            if (!lastWasSeparator) {
                ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                lastWasSeparator = true;
            }
            continue;
        }
        const std::wstring label = item.text.toWideCharPointer();
        if (item.isSectionHeader) {
            ::AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, label.c_str());
            lastWasSeparator = false;
            continue;
        }
        if (item.subMenu != nullptr) {
            HMENU sub = buildWin32MenuFromJuce(*item.subMenu);
            if (sub) {
                UINT flags = MF_POPUP | (item.isEnabled ? MF_ENABLED : MF_GRAYED);
                ::AppendMenuW(hMenu, flags, reinterpret_cast<UINT_PTR>(sub), label.c_str());
                lastWasSeparator = false;
            }
            continue;
        }

        UINT flags = MF_STRING;
        if (!item.isEnabled || item.itemID == 0) {
            flags |= MF_GRAYED;
        }
        if (item.isTicked) {
            flags |= MF_CHECKED;
        }
        ::AppendMenuW(hMenu, flags, static_cast<UINT_PTR>(item.itemID), label.c_str());
        lastWasSeparator = false;
    }
    return hMenu;
}

} // namespace

bool showNativePopupMenu(const juce::PopupMenu& menu, juce::Component* target,
                         juce::Point<int> screenPos, std::function<void(int)> callback) {
    auto* top = target != nullptr ? target->getTopLevelComponent() : nullptr;
    auto* peer = top != nullptr ? top->getPeer() : nullptr;
    HWND hwnd = peer != nullptr ? static_cast<HWND>(peer->getNativeHandle()) : nullptr;
    if (!hwnd) {
        return false;
    }

    HMENU hMenu = buildWin32MenuFromJuce(menu);
    if (!hMenu) {
        return false;
    }

    // Convert logical JUCE screen coordinates to physical Win32 screen pixels.
    const double scale = peer->getPlatformScaleFactor();
    const int px = static_cast<int>(std::llround(static_cast<double>(screenPos.x) * scale));
    const int py = static_cast<int>(std::llround(static_cast<double>(screenPos.y) * scale));

    ::SetForegroundWindow(hwnd);
    const UINT cmd = ::TrackPopupMenuEx(
        hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, px, py, hwnd,
        nullptr);
    ::DestroyMenu(hMenu);

    const int chosen = static_cast<int>(cmd);
    if (callback) {
        juce::MessageManager::callAsync([cb = std::move(callback), chosen]() mutable { cb(chosen); });
    }
    return true;
}

} // namespace carta
