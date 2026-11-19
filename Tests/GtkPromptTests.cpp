#include "Platform/NativeSavePrompt.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <cstdlib>
#include <iostream>
#include <memory>

static void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
class Host final : public juce::DocumentWindow {
  public:
    Host()
        : DocumentWindow("GTK prompt test", juce::Colours::black,
                         juce::DocumentWindow::closeButton) {
        setSize(500, 300);
        setVisible(true);
    }
    void closeButtonPressed() override {}
};
static void pump() {
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
}
static GtkWidget* currentDialog() {
    GtkWidget* found = nullptr;
    GList* windows = gtk_window_list_toplevels();
    for (auto* item = windows; item; item = item->next)
        if (GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(GTK_WIDGET(item->data))) {
            check(!found, "only one GTK dialog is visible");
            found = GTK_WIDGET(item->data);
        }
    g_list_free(windows);
    return found;
}
static void findEntries(GtkWidget* widget, std::vector<GtkWidget*>& entries) {
    if (GTK_IS_ENTRY(widget))
        entries.push_back(widget);
    if (GTK_IS_CONTAINER(widget)) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (auto* item = children; item; item = item->next)
            findEntries(GTK_WIDGET(item->data), entries);
        g_list_free(children);
    }
}
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    auto host = std::make_unique<Host>();
    pump();
    int calls = 0, choice = -1;
    juce::StringArray result;
    const auto done = [&](int selected, juce::StringArray values) {
        ++calls;
        choice = selected;
        result = std::move(values);
    };
    if (argc == 2 && juce::String(argv[1]) == "--keyboard") {
        check(carta::showGtkTextFieldsPrompt(host.get(), "Carta GTK keyboard test", "Enter a name",
                                             {{"name", "Name", "initial"}}, "Apply", {}, done),
              "open keyboard test prompt");
        for (int i = 0; i < 400 && calls == 0; ++i)
            pump();
        check(calls == 1 && choice == 1 && result.size() == 1 && result[0] == "Typed title",
              "typing and Return accept the edited value");
        check(carta::showGtkTextFieldsPrompt(host.get(), "Carta GTK escape test",
                                             "Cancel this edit", {{"name", "Name", "original"}},
                                             "Apply", {}, done),
              "open Escape test prompt");
        for (int i = 0; i < 400 && calls == 1; ++i)
            pump();
        check(calls == 2 && choice == 0 && result.isEmpty(), "Escape cancels the edit");
        int alertCalls = 0, alertChoice = -99;
        const auto save = juce::MessageBoxOptions()
                              .withTitle("Carta GTK save test")
                              .withMessage("Save changes?")
                              .withButton("Save")
                              .withButton("Discard")
                              .withButton("Cancel")
                              .withAssociatedComponent(host.get());
        check(carta::showGtkAlert(save,
                                  [&](int value) {
                                      ++alertCalls;
                                      alertChoice = value;
                                  }),
              "open save confirmation");
        for (int i = 0; i < 400 && alertCalls == 0; ++i)
            pump();
        check(alertCalls == 1 && alertChoice != 0 && alertChoice != 1,
              "Escape cannot save or discard the project");
        const auto folder =
            juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("carta-overwrite-keyboard-" + juce::Uuid().toString());
        check(folder.createDirectory().wasOk(), "create overwrite keyboard fixture");
        const auto existing = folder.getChildFile("existing.wav");
        check(existing.replaceWithText("original"), "create existing overwrite fixture");
        int pickerCalls = 0;
        juce::Array<juce::File> picked;
        const auto pickerDone = [&](juce::Array<juce::File> files) {
            ++pickerCalls;
            picked = std::move(files);
        };
        const int saveFlags = juce::FileBrowserComponent::saveMode |
                              juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting;
        struct KeyboardOverwrite {
            int phase = 1;
            bool seen = false;
        } state;
        const auto timer = g_timeout_add(
            20,
            +[](gpointer data) -> gboolean {
                auto& context = *static_cast<KeyboardOverwrite*>(data);
                GList* windows = gtk_window_list_toplevels();
                for (auto* it = windows; it; it = it->next)
                    if (GTK_IS_MESSAGE_DIALOG(it->data) &&
                        gtk_widget_get_visible(GTK_WIDGET(it->data))) {
                        context.seen = true;
                        gtk_window_set_title(GTK_WINDOW(it->data),
                                             context.phase == 1 ? "Carta GTK overwrite cancel"
                                                                : "Carta GTK overwrite replace");
                    }
                g_list_free(windows);
                return G_SOURCE_CONTINUE;
            },
            &state);
        const auto beginOverwrite = [&] {
            check(carta::showGtkFileChooser(host.get(), "Carta GTK overwrite picker", existing,
                                            "*.wav", saveFlags, pickerDone),
                  "open overwrite keyboard picker");
            for (int i = 0; i < 10; ++i)
                pump();
            auto* picker = currentDialog();
            check(picker && GTK_IS_FILE_CHOOSER(picker), "overwrite keyboard picker is open");
            g_timeout_add(
                300,
                +[](gpointer data) -> gboolean {
                    gtk_dialog_response(GTK_DIALOG(data), GTK_RESPONSE_ACCEPT);
                    return G_SOURCE_REMOVE;
                },
                picker);
            return picker;
        };
        auto* picker = beginOverwrite();
        for (int i = 0; i < 400 && !state.seen; ++i)
            pump();
        check(state.seen && pickerCalls == 0 && currentDialog() == picker &&
                  existing.loadFileAsString() == "original",
              "Escape cancels overwrite and preserves the existing file");
        gtk_dialog_response(GTK_DIALOG(picker), GTK_RESPONSE_CANCEL);
        for (int i = 0; i < 20 && pickerCalls == 0; ++i)
            pump();
        check(pickerCalls == 1 && picked.isEmpty(), "cancel the first overwrite picker");
        state.phase = 2;
        state.seen = false;
        picker = beginOverwrite();
        for (int i = 0; i < 400 && pickerCalls == 1; ++i)
            pump();
        check(state.seen && pickerCalls == 2 && picked.size() == 1 && picked[0] == existing &&
                  existing.loadFileAsString() == "original",
              "keyboard replacement confirms the path without altering the file");
        g_source_remove(timer);
        check(folder.deleteRecursively(), "remove overwrite keyboard fixture");
        std::cout << "GTK keyboard tests passed.\n";
        return 0;
    }
    check(!carta::showGtkTextFieldsPrompt(nullptr, "No parent", "", {{"name", "Name", "x"}}, "OK",
                                          {}, done) &&
              calls == 0,
          "missing parent falls back without invoking callback");
    check(carta::showGtkTextFieldsPrompt(
              host.get(), "Edit title", "Set the title",
              {{"name", "Name", juce::String::fromUTF8("夜")}, {"count", "Count", "3"}}, "Apply",
              "Other", done),
          "show a GTK multi-field prompt");
    pump();
    auto* dialog = currentDialog();
    check(dialog && gtk_window_get_modal(GTK_WINDOW(dialog)), "native dialog is visible and modal");
    Window actualParent = 0;
    auto* display = gdk_x11_display_get_xdisplay(gtk_widget_get_display(dialog));
    XSync(display, False);
    check(XGetTransientForHint(display, GDK_WINDOW_XID(gtk_widget_get_window(dialog)),
                               &actualParent) &&
              actualParent == reinterpret_cast<std::uintptr_t>(host->getPeer()->getNativeHandle()),
          "GTK dialog is transient for the JUCE window");
    std::vector<GtkWidget*> entries;
    findEntries(dialog, entries);
    GtkWidget* name = nullptr;
    GtkWidget* count = nullptr;
    for (auto* entry : entries) {
        const auto label =
            juce::String::fromUTF8(atk_object_get_name(gtk_widget_get_accessible(entry)));
        if (label == "Name")
            name = entry;
        if (label == "Count")
            count = entry;
    }
    if (entries.size() != 2 || !name || !count) {
        std::cerr << "Found " << entries.size() << " entries:";
        for (auto* entry : entries) {
            const char* accessible = atk_object_get_name(gtk_widget_get_accessible(entry));
            std::cerr << " [" << (accessible ? accessible : "<unnamed>") << "]";
        }
        std::cerr << '\n';
    }
    check(entries.size() == 2 && name && count &&
              juce::String::fromUTF8(gtk_entry_get_text(GTK_ENTRY(name))) ==
                  juce::String::fromUTF8("夜"),
          "fields preserve their accessible labels and initial Unicode values");
    gtk_entry_set_text(GTK_ENTRY(name), "新しい名前");
    gtk_entry_set_text(GTK_ENTRY(count), "42");
    gtk_dialog_response(GTK_DIALOG(dialog), 1);
    pump();
    check(calls == 1 && choice == 1 && result.size() == 2 &&
              result[0] == juce::String::fromUTF8("新しい名前") && result[1] == "42" &&
              !currentDialog(),
          "accept returns all edited values once");
    check(carta::showGtkTextFieldsPrompt(host.get(), "Alternative", "", {{"x", "Field", "value"}},
                                         "Apply", "Other", done),
          "show alternative prompt");
    pump();
    dialog = currentDialog();
    check(dialog != nullptr, "alternative prompt opened");
    gtk_dialog_response(GTK_DIALOG(dialog), 2);
    pump();
    check(calls == 2 && choice == 2 && result.size() == 1 && result[0] == "value",
          "alternative returns the field values");
    check(carta::showGtkTextFieldsPrompt(host.get(), "Cancel", "", {{"x", "Field", "value"}},
                                         "Apply", {}, done),
          "show cancel prompt");
    pump();
    dialog = currentDialog();
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    pump();
    check(calls == 3 && choice == 0 && result.isEmpty(), "cancellation returns no values");
    int alertCalls = 0, alertChoice = -99;
    const auto save = juce::MessageBoxOptions()
                          .withTitle("Save test")
                          .withMessage("Save changes?")
                          .withButton("Save")
                          .withButton("Discard")
                          .withButton("Cancel")
                          .withAssociatedComponent(host.get());
    const auto alertDone = [&](int value) {
        ++alertCalls;
        alertChoice = value;
    };
    for (int button = 1; button <= 3; ++button) {
        check(carta::showGtkAlert(save, alertDone), "show save confirmation");
        pump();
        dialog = currentDialog();
        check(dialog && gtk_window_get_modal(GTK_WINDOW(dialog)), "save confirmation is modal");
        gtk_dialog_response(GTK_DIALOG(dialog), button);
        pump();
        check(alertCalls == button && alertChoice == button - 1,
              "save confirmation returns the selected button index");
    }
    check(carta::showGtkAlert(save, alertDone), "show confirmation for Escape mapping");
    pump();
    gtk_dialog_response(GTK_DIALOG(currentDialog()), GTK_RESPONSE_CANCEL);
    pump();
    check(alertCalls == 4 && alertChoice == 2, "Escape maps to Cancel and never Discard");
    check(carta::showGtkAlert(save, alertDone), "show confirmation for window dismissal");
    pump();
    gtk_dialog_response(GTK_DIALOG(currentDialog()), GTK_RESPONSE_DELETE_EVENT);
    pump();
    check(alertCalls == 5 && alertChoice == -1,
          "window dismissal cancels without choosing a button");
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("carta-picker-" + juce::Uuid().toString());
    check(folder.createDirectory().wasOk(), "create file picker fixtures");
    const auto first = folder.getChildFile(juce::String::fromUTF8("音: one.wav")),
               second = folder.getChildFile("two.WAV"), third = folder.getChildFile("three.Wav"),
               other = folder.getChildFile("not-a-wave.txt");
    check(first.replaceWithText("first") && second.replaceWithText("second") &&
              third.replaceWithText("third") && other.replaceWithText("other"),
          "create distinct source files");
    int pickerCalls = 0;
    juce::Array<juce::File> picked;
    const auto pickedDone = [&](juce::Array<juce::File> files) {
        ++pickerCalls;
        picked = std::move(files);
    };
    const int openFlags = juce::FileBrowserComponent::openMode |
                          juce::FileBrowserComponent::canSelectFiles |
                          juce::FileBrowserComponent::canSelectMultipleItems;
    check(!carta::showGtkFileChooser(nullptr, "Missing parent", folder, "*.wav", openFlags,
                                     pickedDone) &&
              pickerCalls == 0,
          "file picker falls back without a parent");
    check(
        carta::showGtkFileChooser(host.get(), "Open audio", folder, "*.wav", openFlags, pickedDone),
        "open native multi-file picker");
    for (int i = 0; i < 10; ++i)
        pump();
    dialog = currentDialog();
    check(dialog && GTK_IS_FILE_CHOOSER(dialog) &&
              gtk_file_chooser_get_select_multiple(GTK_FILE_CHOOSER(dialog)),
          "picker permits multiple local files");
    gtk_file_chooser_select_all(GTK_FILE_CHOOSER(dialog));
    for (int i = 0; i < 5; ++i)
        pump();
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    for (int i = 0; i < 50 && pickerCalls == 0; ++i)
        pump();
    if (pickerCalls != 1 || picked.size() != 3 || !picked.contains(first) ||
        !picked.contains(second) || !picked.contains(third)) {
        std::cerr << "Picker callbacks: " << pickerCalls << ", paths:";
        for (const auto& path : picked)
            std::cerr << " [" << path.getFullPathName() << "]";
        std::cerr << '\n';
    }
    check(pickerCalls == 1 && picked.size() == 3 && picked.contains(first) &&
              picked.contains(second) && picked.contains(third),
          "native picker returns distinct paths and matches mixed-case extensions");
    check(carta::showGtkFileChooser(
              host.get(), "Save as", folder.getChildFile("new.carta"), "*.carta",
              juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                  juce::FileBrowserComponent::warnAboutOverwriting,
              pickedDone),
          "open native save picker");
    for (int i = 0; i < 5; ++i)
        pump();
    dialog = currentDialog();
    check(dialog && gtk_file_chooser_get_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog)),
          "save picker requests overwrite confirmation");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    pump();
    check(pickerCalls == 2 && picked.isEmpty(), "cancelling the file picker returns no files");
    const int saveFlags = juce::FileBrowserComponent::saveMode |
                          juce::FileBrowserComponent::canSelectFiles |
                          juce::FileBrowserComponent::warnAboutOverwriting;
    check(carta::showGtkFileChooser(host.get(), "Replace existing", first, "*.wav", saveFlags,
                                    pickedDone),
          "open overwrite picker");
    for (int i = 0; i < 10; ++i)
        pump();
    dialog = currentDialog();
    check(dialog && gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT),
          "save button uses GTK's standard accept response");
    struct OverwriteDecision {
        int calls = 0;
        bool accept = false;
    } decision;
    g_signal_connect(dialog, "confirm-overwrite",
                     G_CALLBACK(+[](GtkFileChooser*, gpointer data) -> GtkFileChooserConfirmation {
                         auto& state = *static_cast<OverwriteDecision*>(data);
                         ++state.calls;
                         return state.accept ? GTK_FILE_CHOOSER_CONFIRMATION_ACCEPT_FILENAME
                                             : GTK_FILE_CHOOSER_CONFIRMATION_SELECT_AGAIN;
                     }),
                     &decision);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    for (int i = 0; i < 50 && decision.calls == 0; ++i)
        pump();
    check(decision.calls == 1 && pickerCalls == 2 && currentDialog() == dialog &&
              first.loadFileAsString() == "first",
          "rejecting overwrite keeps picker open and existing file unchanged");
    decision.accept = true;
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    for (int i = 0; i < 50 && pickerCalls == 2; ++i)
        pump();
    check(decision.calls == 2 && pickerCalls == 3 && picked.size() == 1 && picked[0] == first &&
              first.loadFileAsString() == "first",
          "accepting overwrite returns existing path without modifying it");
    check(carta::showGtkFileChooser(host.get(), "Real overwrite", first, "*.wav", saveFlags,
                                    pickedDone),
          "open picker with GTK's real overwrite dialog");
    for (int i = 0; i < 10; ++i)
        pump();
    dialog = currentDialog();
    struct NativeDecision {
        GtkWidget* picker;
        int response;
        int ticks = 0;
        bool seen = false, hasButtons = false;
    } native{dialog, GTK_RESPONSE_CANCEL};
    const auto scheduleDecision = [](NativeDecision& state) {
        g_timeout_add(
            20,
            +[](gpointer data) -> gboolean {
                auto& item = *static_cast<NativeDecision*>(data);
                GList* windows = gtk_window_list_toplevels();
                GtkWidget* confirmation = nullptr;
                for (auto* it = windows; it; it = it->next)
                    if (GTK_IS_MESSAGE_DIALOG(it->data) &&
                        gtk_widget_get_visible(GTK_WIDGET(it->data)))
                        confirmation = GTK_WIDGET(it->data);
                g_list_free(windows);
                if (confirmation) {
                    item.seen = true;
                    item.hasButtons = gtk_dialog_get_widget_for_response(GTK_DIALOG(confirmation),
                                                                         GTK_RESPONSE_CANCEL) &&
                                      gtk_dialog_get_widget_for_response(GTK_DIALOG(confirmation),
                                                                         GTK_RESPONSE_ACCEPT);
                    gtk_dialog_response(GTK_DIALOG(confirmation), item.response);
                    return G_SOURCE_REMOVE;
                }
                if (++item.ticks > 250) {
                    gtk_dialog_response(GTK_DIALOG(item.picker), GTK_RESPONSE_CANCEL);
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            },
            &state);
    };
    scheduleDecision(native);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    for (int i = 0; i < 120 && !native.seen && pickerCalls == 3; ++i)
        pump();
    check(native.seen && native.hasButtons && pickerCalls == 3 && currentDialog() == dialog &&
              first.loadFileAsString() == "first",
          "real GTK overwrite cancellation leaves the picker and destination intact");
    native.response = GTK_RESPONSE_ACCEPT;
    native.ticks = 0;
    native.seen = false;
    native.hasButtons = false;
    scheduleDecision(native);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    for (int i = 0; i < 120 && pickerCalls == 3; ++i)
        pump();
    check(native.seen && native.hasButtons && pickerCalls == 4 && picked.size() == 1 &&
              picked[0] == first && first.loadFileAsString() == "first",
          "real GTK overwrite confirmation returns the chosen path without writing it");
    check(folder.deleteRecursively(), "remove file picker fixtures");
    check(carta::showGtkTextFieldsPrompt(host.get(), "Parent loss", "", {{"x", "Field", "value"}},
                                         "Apply", {}, done),
          "show prompt before owner destruction");
    pump();
    host.reset();
    pump();
    pump();
    check(calls == 4 && choice == 0 && result.isEmpty() && !currentDialog(),
          "destroying the parent cancels and closes the prompt once");
    std::cout << "GTK prompt tests passed.\n";
}
