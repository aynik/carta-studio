/**
 * macOS AppKit modal sheet implementations for Carta Studio prompts and alerts.
 *
 * Anchors `NSAlert` modal sheets to the host `NSWindow` of the associated
 * JUCE `Component`, applies `NativeAlertPolicy` Escape bindings, dynamically
 * measures label widths for multi-field sheets, recalculates the AppKit key
 * view loop, and supports both ARC and manual retain/release builds.
 */

#include "Platform/NativeSavePrompt.h"
#include "Platform/NativeAlertPolicy.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#import <AppKit/AppKit.h>

namespace carta {
namespace {

NSString* nativeString(const juce::String& value) {
    return [NSString stringWithUTF8String:value.toRawUTF8()];
}

NSWindow* hostWindowForComponent(juce::Component* component) {
    auto* peer = component != nullptr ? component->getPeer() : nullptr;
    auto* view = peer != nullptr ? static_cast<NSView*>(peer->getNativeHandle()) : nullptr;
    return view != nullptr ? [view window] : nil;
}

} // namespace

void showNativeAlert(const juce::MessageBoxOptions& options, std::function<void(int)> callback) {
    NSWindow* window = hostWindowForComponent(options.getAssociatedComponent());
    const int count = options.getNumButtons();
    if (window == nil || count < 1 || count > 3) {
        if (callback) {
            callback(-1);
        }
        return;
    }

    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:nativeString(options.getTitle())];
    [alert setInformativeText:nativeString(options.getMessage())];
    [alert setAlertStyle:options.getIconType() == juce::MessageBoxIconType::WarningIcon
                             ? NSAlertStyleWarning
                             : NSAlertStyleInformational];

    std::vector<std::string> labels;
    labels.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        labels.push_back(options.getButtonText(i).toStdString());
    }

    const int escapeButton = nativeAlertEscapeButton(labels);
    for (int i = 0; i < count; ++i) {
        const auto label = options.getButtonText(i);
        NSButton* button = [alert addButtonWithTitle:nativeString(label)];
        if (i == escapeButton) {
            [button setKeyEquivalent:@"\x1b"];
            [button setKeyEquivalentModifierMask:0];
        }
    }

    [alert beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse response) {
                    static_assert(NSAlertSecondButtonReturn == NSAlertFirstButtonReturn + 1 &&
                                  NSAlertThirdButtonReturn == NSAlertFirstButtonReturn + 2);
                    const int result = nativeAlertChoice(
                        static_cast<long>(response) - NSAlertFirstButtonReturn, count);
                    if (callback) {
                        callback(result);
                    }
#if !__has_feature(objc_arc)
                    [alert release];
#endif
                  }];
}

void showNativeTextFieldsPrompt(juce::Component* component, const juce::String& title,
                                const juce::String& message,
                                const std::vector<TextPromptField>& fields,
                                const juce::String& accept, const juce::String& alternative,
                                std::function<void(int, juce::StringArray)> callback) {
    NSWindow* window = hostWindowForComponent(component);
    if (window == nil || fields.empty() || fields.size() > 8) {
        if (callback) {
            callback(0, {});
        }
        return;
    }

    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:nativeString(title)];
    [alert setInformativeText:nativeString(message)];
    [alert setAlertStyle:NSAlertStyleInformational];

    // Size the label column for the actual system font and text, while keeping
    // long labels from pushing the editor beyond a compact sheet.
    CGFloat labelWidth = 150;
    NSDictionary* attributes =
        @{NSFontAttributeName : [NSFont systemFontOfSize:[NSFont systemFontSize]]};
    for (const auto& spec : fields) {
        const CGFloat measuredWidth =
            std::ceil([nativeString(spec.label) sizeWithAttributes:attributes].width);
        labelWidth = std::max(labelWidth, std::min(CGFloat{260}, measuredWidth));
    }

    const auto count = static_cast<NSInteger>(fields.size());
    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, labelWidth + 270, count * 32 - 8)];
    NSMutableArray<NSTextField*>* inputs =
        [[NSMutableArray alloc] initWithCapacity:static_cast<NSUInteger>(count)];

    for (NSInteger i = 0; i < count; ++i) {
        const auto& spec = fields[static_cast<size_t>(i)];
        const CGFloat y = static_cast<CGFloat>((count - 1 - i) * 32);

        NSTextField* label = [NSTextField labelWithString:nativeString(spec.label)];
        [label setFrame:NSMakeRect(0, y + 2, labelWidth, 20)];
        [label setToolTip:nativeString(spec.label)];
        [accessory addSubview:label];

        NSTextField* field =
            [[NSTextField alloc] initWithFrame:NSMakeRect(labelWidth + 5, y, 265, 24)];
        [field setStringValue:nativeString(spec.initial)];
        [field setAccessibilityLabel:nativeString(spec.label)];
        [accessory addSubview:field];
        [inputs addObject:field];
#if !__has_feature(objc_arc)
        [field release];
#endif
    }

    [alert setAccessoryView:accessory];
    NSButton* submit = [alert addButtonWithTitle:nativeString(accept)];
    const bool hasAlternative = alternative.isNotEmpty();
    if (hasAlternative) {
        [alert addButtonWithTitle:nativeString(alternative)];
    }
    NSButton* cancel = [alert addButtonWithTitle:@"Cancel"];
    [submit setKeyEquivalent:@"\r"];
    [submit setKeyEquivalentModifierMask:0];
    [cancel setKeyEquivalent:@"\x1b"];
    [cancel setKeyEquivalentModifierMask:0];

    // The accessory is installed after NSAlert creates its window. Include its
    // text fields in AppKit's geometric Tab and Shift-Tab focus order.
    [[alert window] recalculateKeyViewLoop];
    [alert beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse response) {
                    const int choice =
                        response == NSAlertFirstButtonReturn
                            ? 1
                            : (hasAlternative && response == NSAlertSecondButtonReturn ? 2 : 0);
                    juce::StringArray values;
                    if (choice != 0) {
                        for (NSTextField* field in inputs) {
                            values.add(juce::String::fromUTF8([[field stringValue] UTF8String]));
                        }
                    }
                    if (callback) {
                        callback(choice, values);
                    }
#if !__has_feature(objc_arc)
                    [inputs release];
                    [accessory release];
                    [alert release];
#endif
                  }];

    [[alert window] makeFirstResponder:[inputs firstObject]];
    [[inputs firstObject] selectText:nil];
}

void showNativeTextPrompt(juce::Component* component, const juce::String& title,
                          const juce::String& message, const juce::String& label,
                          const juce::String& initial, const juce::String& accept,
                          std::function<void(bool, juce::String)> callback) {
    showNativeTextFieldsPrompt(
        component, title, message, {{{}, label, initial}}, accept, {},
        [completion = std::move(callback)](int choice, juce::StringArray values) {
            const bool accepted = choice == 1 && values.size() == 1;
            if (completion) {
                completion(accepted, accepted ? values[0] : juce::String{});
            }
        });
}

void showNativeSavePrompt(juce::Component* component, std::function<void(int)> callback) {
    NSWindow* window = hostWindowForComponent(component);
    if (window == nil) {
        if (callback) {
            callback(-1);
        }
        return;
    }

    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Unsaved project"];
    [alert setInformativeText:@"Save your changes before continuing?"];
    [alert setAlertStyle:NSAlertStyleWarning];

    NSButton* save = [alert addButtonWithTitle:@"Save"];
    NSButton* discard = [alert addButtonWithTitle:@"Discard"];
    NSButton* cancel = [alert addButtonWithTitle:@"Cancel"];
    [save setKeyEquivalent:@"\r"];
    [save setKeyEquivalentModifierMask:0];
    [discard setKeyEquivalent:@"d"];
    [discard setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [cancel setKeyEquivalent:@"\x1b"];
    [cancel setKeyEquivalentModifierMask:0];

    [alert beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse response) {
                    const int result = response == NSAlertFirstButtonReturn
                                           ? 0
                                           : (response == NSAlertSecondButtonReturn ? 1 : -1);
                    if (callback) {
                        callback(result);
                    }
#if !__has_feature(objc_arc)
                    [alert release];
#endif
                  }];
}

} // namespace carta
