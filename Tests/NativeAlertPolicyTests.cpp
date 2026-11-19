/**
 * @file NativeAlertPolicyTests.cpp
 * @brief Unit tests for native alert Escape-key mapping and button-index bounds.
 */
#include "Platform/NativeAlertPolicy.h"
#include <cstdlib>
#include <iostream>

static void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
int main() {
    using carta::nativeAlertChoice;
    using carta::nativeAlertEscapeButton;
    check(nativeAlertEscapeButton({"Export anyway", "Cancel"}) == 1,
          "Escape cancels an export confirmation");
    check(nativeAlertEscapeButton({"Close", "Go to peak", "Go to first over"}) == 0,
          "Escape closes level results");
    check(nativeAlertEscapeButton({"OK"}) == 0, "Escape dismisses a single-button notice");
    check(nativeAlertEscapeButton({"Yes", "No"}) == -1,
          "Escape is not assigned to an ambiguous choice");
    check(nativeAlertEscapeButton({"Close", "CANCEL"}) == 1,
          "explicit Cancel takes priority over Close");
    check(nativeAlertChoice(0, 2) == 0 && nativeAlertChoice(1, 2) == 1 &&
              nativeAlertChoice(2, 3) == 2,
          "button order is preserved");
    check(nativeAlertChoice(-1, 2) == -1 && nativeAlertChoice(2, 2) == -1 &&
              nativeAlertChoice(0, 0) == -1 && nativeAlertChoice(0, 4) == -1,
          "unrecognized responses never select an action");
}
