/**
 * Linux GTK3/X11 modal dialog bridge for text prompts, alerts, and file choosers.
 *
 * Forces the X11 GDK backend so transient dialogs parent cleanly onto JUCE's
 * X11/XWayland window handle, pumps the GLib main context from a 10 ms JUCE
 * `Timer` on the message thread, flushes the X11 display on destroy, and
 * defers callback delivery until the timer callback stack unwinds.
 */

#include "Platform/NativeSavePrompt.h"
#include "Platform/NativeAlertPolicy.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <cstdint>

namespace carta {
namespace {

/**
 * Self-owning message-thread timer pumping a transient GTK modal dialog.
 *
 * GTK owns a separate X11 connection. Pumping it on JUCE's message thread
 * ensures widgets and callbacks never execute on the audio thread or a worker.
 */
class GtkTextPrompt final : private juce::Timer {
  public:
    GtkTextPrompt(juce::Component* owner, GtkWidget* window, std::vector<GtkWidget*> inputs,
                  std::function<void(int, juce::StringArray)> callback, int escapeChoice = 0,
                  std::function<juce::StringArray()> collect = {})
        : parent(owner), dialog(window), entries(std::move(inputs)), done(std::move(callback)),
          collector(std::move(collect)), escape(escapeChoice) {
        g_signal_connect(dialog, "response", G_CALLBACK(response), this);
        startTimer(10);
    }

    static void response(GtkDialog*, gint result, gpointer context) {
        auto* self = static_cast<GtkTextPrompt*>(context);
        const int choice =
            result == GTK_RESPONSE_ACCEPT
                ? 1
                : (result >= 1 && result <= 3 ? result
                                              : (result == GTK_RESPONSE_CANCEL ? self->escape : 0));
        self->finish(choice);
    }

  private:
    void timerCallback() override {
        if (parent == nullptr || parent->getPeer() == nullptr) {
            finish(0);
            return;
        }
        for (int i = 0; i < 32 && !finished && g_main_context_pending(nullptr); ++i) {
            g_main_context_iteration(nullptr, FALSE);
        }
    }

    void finish(int choice) {
        if (finished) {
            return;
        }
        finished = true;
        stopTimer();

        juce::StringArray values;
        if (choice != 0) {
            if (collector) {
                values = collector();
            } else {
                for (auto* entry : entries) {
                    values.add(juce::String::fromUTF8(gtk_entry_get_text(GTK_ENTRY(entry))));
                }
            }
        }

        // GTK uses its own X11 connection. Flush the destroy request before
        // stopping the pump, otherwise the window manager can keep a closed
        // modal dialog visible and blocking its parent.
        auto* display = gtk_widget_get_display(dialog);
        gtk_widget_destroy(dialog);
        dialog = nullptr;
        gdk_display_flush(display);

        auto completion = std::move(done);
        // The response can arrive inside the timer's GTK iteration; defer
        // destruction until that stack has unwound.
        juce::MessageManager::callAsync([this, deliver = std::move(completion), choice,
                                         collected = std::move(values)]() mutable {
            if (deliver) {
                deliver(choice, std::move(collected));
            }
            delete this;
        });
    }

    juce::Component::SafePointer<juce::Component> parent;
    GtkWidget* dialog = nullptr;
    std::vector<GtkWidget*> entries;
    std::function<void(int, juce::StringArray)> done;
    std::function<juce::StringArray()> collector;
    int escape = 0;
    bool finished = false;
};

/** Wrap JUCE's X11 peer handle in a foreign `GdkWindow`, or return nullptr. */
GdkWindow* gtkParent(juce::Component* parent) {
    auto* peer = parent ? parent->getPeer() : nullptr;
    if (!peer || !peer->getNativeHandle()) {
        return nullptr;
    }
    static const bool initialized = [] {
        gdk_set_allowed_backends("x11");
        return gtk_init_check(nullptr, nullptr) != FALSE;
    }();
    if (!initialized) {
        return nullptr;
    }
    auto* display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) {
        return nullptr;
    }
    const auto xid = static_cast<Window>(reinterpret_cast<std::uintptr_t>(peer->getNativeHandle()));
    return gdk_x11_window_foreign_new_for_display(display, xid);
}

/** Realize `dialog`, bind transient parent `foreign`, and release the foreign reference. */
void attachParent(GtkWidget* dialog, GdkWindow* foreign) {
    gtk_widget_realize(dialog);
    gdk_window_set_transient_for(gtk_widget_get_window(dialog), foreign);
    g_object_unref(foreign);
}

/** Expand simple `*.ext` ASCII extension globs into case-insensitive GTK character classes. */
juce::String expandCaseInsensitiveGlob(const juce::String& pattern) {
    bool simple = pattern.startsWith("*.") && pattern.length() > 2;
    for (int i = 2; simple && i < pattern.length(); ++i) {
        const auto c = pattern[i];
        simple = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    }
    if (!simple) {
        return pattern;
    }

    juce::String glob = "*.";
    for (int i = 2; i < pattern.length(); ++i) {
        const auto c = pattern[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            const auto letter = juce::String::charToString(c);
            glob += "[" + letter.toLowerCase() + letter.toUpperCase() + "]";
        } else {
            glob += juce::String::charToString(c);
        }
    }
    return glob;
}

} // namespace

bool showGtkTextFieldsPrompt(juce::Component* parent, const juce::String& title,
                             const juce::String& message,
                             const std::vector<TextPromptField>& fields, const juce::String& accept,
                             const juce::String& alternative,
                             std::function<void(int, juce::StringArray)> callback) {
    if (fields.empty() || fields.size() > 8) {
        return false;
    }

    // JUCE's Linux peer is X11 (including XWayland), so use the same backend
    // for a real transient parent rather than opening an unparented dialog.
    auto* foreign = gtkParent(parent);
    if (!foreign) {
        return false;
    }

    auto* dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title.toRawUTF8());
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    if (alternative.isNotEmpty()) {
        gtk_dialog_add_button(GTK_DIALOG(dialog), alternative.toRawUTF8(), 2);
    }
    gtk_dialog_add_button(GTK_DIALOG(dialog), accept.toRawUTF8(), 1);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), 1);

    auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_set_spacing(GTK_BOX(area), 10);

    auto* explanation = gtk_label_new(message.toRawUTF8());
    gtk_label_set_line_wrap(GTK_LABEL(explanation), TRUE);
    gtk_label_set_xalign(GTK_LABEL(explanation), 0);
    gtk_widget_set_size_request(explanation, 360, -1);
    gtk_box_pack_start(GTK_BOX(area), explanation, FALSE, FALSE, 0);

    auto* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);

    std::vector<GtkWidget*> entries;
    entries.reserve(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        auto* label = gtk_label_new(field.label.toRawUTF8());
        gtk_label_set_xalign(GTK_LABEL(label), 0);

        auto* entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), field.initial.toRawUTF8());
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
        atk_object_set_name(gtk_widget_get_accessible(entry), field.label.toRawUTF8());

        gtk_grid_attach(GTK_GRID(grid), label, 0, static_cast<int>(i), 1, 1);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, static_cast<int>(i), 1, 1);
        entries.push_back(entry);
    }

    attachParent(dialog, foreign);
    new GtkTextPrompt(parent, dialog, entries, std::move(callback));
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entries.front());
    gtk_editable_select_region(GTK_EDITABLE(entries.front()), 0, -1);
    return true;
}

bool showGtkAlert(const juce::MessageBoxOptions& options, std::function<void(int)> callback) {
    auto* parent = options.getAssociatedComponent();
    const int count = options.getNumButtons();
    if (count < 1 || count > 3) {
        return false;
    }

    auto* foreign = gtkParent(parent);
    if (!foreign) {
        return false;
    }

    const auto type = options.getIconType() == juce::MessageBoxIconType::WarningIcon
                          ? GTK_MESSAGE_WARNING
                          : (options.getIconType() == juce::MessageBoxIconType::QuestionIcon
                                 ? GTK_MESSAGE_QUESTION
                                 : GTK_MESSAGE_INFO);
    auto* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, type, GTK_BUTTONS_NONE, "%s",
                                          options.getMessage().toRawUTF8());
    gtk_window_set_title(GTK_WINDOW(dialog), options.getTitle().toRawUTF8());
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    std::vector<std::string> labels;
    for (int i = 0; i < count; ++i) {
        const auto label = options.getButtonText(i);
        labels.push_back(label.toStdString());
        gtk_dialog_add_button(GTK_DIALOG(dialog), label.toRawUTF8(), i + 1);
    }
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), 1);
    attachParent(dialog, foreign);

    const int escapeButton = nativeAlertEscapeButton(labels);
    new GtkTextPrompt(
        parent, dialog, {},
        [deliver = std::move(callback), count](int choice, juce::StringArray) {
            if (deliver) {
                deliver(choice >= 1 && choice <= count ? choice - 1 : -1);
            }
        },
        escapeButton < 0 ? 0 : escapeButton + 1);
    gtk_widget_show_all(dialog);
    return true;
}

bool showGtkFileChooser(juce::Component* parent, const juce::String& title,
                        const juce::File& initial, const juce::String& filters, int flags,
                        std::function<void(juce::Array<juce::File>)> callback) {
    const bool save = (flags & juce::FileBrowserComponent::saveMode) != 0;
    const bool files = (flags & juce::FileBrowserComponent::canSelectFiles) != 0;
    const bool folders = (flags & juce::FileBrowserComponent::canSelectDirectories) != 0;
    if ((!files && !folders) || (save && !files) || (files && folders)) {
        return false;
    }

    auto* foreign = gtkParent(parent);
    if (!foreign) {
        return false;
    }

    const auto action =
        save ? GTK_FILE_CHOOSER_ACTION_SAVE
             : (folders ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN);
    auto* dialog = gtk_file_chooser_dialog_new(title.toRawUTF8(), nullptr, action, "_Cancel",
                                               GTK_RESPONSE_CANCEL, save ? "_Save" : "_Open",
                                               GTK_RESPONSE_ACCEPT, nullptr);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    auto* picker = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_local_only(picker, TRUE);
    gtk_file_chooser_set_select_multiple(
        picker, !save && (flags & juce::FileBrowserComponent::canSelectMultipleItems) != 0);
    gtk_file_chooser_set_do_overwrite_confirmation(
        picker, save && (flags & juce::FileBrowserComponent::warnAboutOverwriting) != 0);

    if (initial.isDirectory()) {
        gtk_file_chooser_set_current_folder(picker, initial.getFullPathName().toRawUTF8());
    } else if (initial != juce::File{}) {
        const auto folder = initial.getParentDirectory();
        if (folder.isDirectory()) {
            gtk_file_chooser_set_current_folder(picker, folder.getFullPathName().toRawUTF8());
        }
        if (save) {
            gtk_file_chooser_set_current_name(picker, initial.getFileName().toRawUTF8());
        } else if (initial.existsAsFile()) {
            gtk_file_chooser_set_filename(picker, initial.getFullPathName().toRawUTF8());
        }
    }

    if (files && filters.isNotEmpty() && filters != "*" && filters != "*.*") {
        juce::StringArray patterns;
        patterns.addTokens(filters, ";,|", "\"");
        patterns.trim();
        patterns.removeEmptyStrings();
        if (!patterns.isEmpty()) {
            auto* filter = gtk_file_filter_new();
            gtk_file_filter_set_name(filter, filters.toRawUTF8());
            for (const auto& pattern : patterns) {
                // GTK globs are case-sensitive. Expand simple ASCII extension
                // globs so mixed-case names are as usable as lowercase ones.
                gtk_file_filter_add_pattern(filter, expandCaseInsensitiveGlob(pattern).toRawUTF8());
            }
            gtk_file_chooser_add_filter(picker, filter);
        }
    }

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    attachParent(dialog, foreign);

    auto collect = [picker] {
        juce::StringArray paths;
        GSList* selected = gtk_file_chooser_get_filenames(picker);
        for (auto* item = selected; item; item = item->next) {
            paths.add(juce::String::fromUTF8(static_cast<const char*>(item->data)));
            g_free(item->data);
        }
        g_slist_free(selected);
        return paths;
    };

    new GtkTextPrompt(
        parent, dialog, {},
        [deliver = std::move(callback)](int choice, juce::StringArray paths) {
            juce::Array<juce::File> result;
            if (choice == 1) {
                for (const auto& path : paths) {
                    result.add(juce::File(path));
                }
            }
            if (deliver) {
                deliver(std::move(result));
            }
        },
        0, std::move(collect));
    gtk_widget_show_all(dialog);
    return true;
}

namespace {

class GtkPopupMenuHost final : private juce::Timer {
  public:
    GtkPopupMenuHost(GtkWidget* m, std::function<void(int)> cb)
        : menu(m), done(std::move(cb)) {
        g_signal_connect(menu, "deactivate", G_CALLBACK(onDeactivate), this);
        startTimer(10);
    }

    void itemActivated(int id) { chosenId = id; }

  private:
    static void onDeactivate(GtkMenuShell*, gpointer ctx) {
        static_cast<GtkPopupMenuHost*>(ctx)->finish();
    }

    void timerCallback() override {
        for (int i = 0; i < 32 && !finished && g_main_context_pending(nullptr); ++i) {
            g_main_context_iteration(nullptr, FALSE);
        }
    }

    void finish() {
        if (finished) {
            return;
        }
        finished = true;
        stopTimer();
        auto* display = gtk_widget_get_display(menu);
        gtk_widget_destroy(menu);
        menu = nullptr;
        if (display) {
            gdk_display_flush(display);
        }
        const int result = chosenId;
        auto cb = std::move(done);
        juce::MessageManager::callAsync([this, deliver = std::move(cb), result]() mutable {
            if (deliver) {
                deliver(result);
            }
            delete this;
        });
    }

    GtkWidget* menu = nullptr;
    std::function<void(int)> done;
    int chosenId = 0;
    bool finished = false;
};

void populateGtkMenu(GtkWidget* gtkMenu, const juce::PopupMenu& source, GtkPopupMenuHost* host) {
    juce::PopupMenu::MenuItemIterator it(source);
    bool lastWasSeparator = true;
    while (it.next()) {
        const auto& item = it.getItem();
        if (item.isSeparator) {
            if (!lastWasSeparator) {
                gtk_menu_shell_append(GTK_MENU_SHELL(gtkMenu), gtk_separator_menu_item_new());
                lastWasSeparator = true;
            }
            continue;
        }
        if (item.isSectionHeader) {
            GtkWidget* hdr = gtk_menu_item_new_with_label(item.text.toRawUTF8());
            gtk_widget_set_sensitive(hdr, FALSE);
            gtk_menu_shell_append(GTK_MENU_SHELL(gtkMenu), hdr);
            lastWasSeparator = false;
            continue;
        }
        if (item.subMenu != nullptr) {
            GtkWidget* subItem = gtk_menu_item_new_with_label(item.text.toRawUTF8());
            gtk_widget_set_sensitive(subItem, item.isEnabled ? TRUE : FALSE);
            GtkWidget* childMenu = gtk_menu_new();
            populateGtkMenu(childMenu, *item.subMenu, host);
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(subItem), childMenu);
            gtk_menu_shell_append(GTK_MENU_SHELL(gtkMenu), subItem);
            lastWasSeparator = false;
            continue;
        }

        GtkWidget* mi = item.isTicked ? gtk_check_menu_item_new_with_label(item.text.toRawUTF8())
                                      : gtk_menu_item_new_with_label(item.text.toRawUTF8());
        if (item.isTicked) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
        }
        gtk_widget_set_sensitive(mi, (item.isEnabled && item.itemID != 0) ? TRUE : FALSE);
        g_object_set_data(G_OBJECT(mi), "carta-item-id",
                          GINT_TO_POINTER(static_cast<gint>(item.itemID)));
        g_signal_connect(
            mi, "activate",
            G_CALLBACK(+[](GtkMenuItem* w, gpointer ctx) {
                const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "carta-item-id"));
                static_cast<GtkPopupMenuHost*>(ctx)->itemActivated(id);
            }),
            host);
        gtk_menu_shell_append(GTK_MENU_SHELL(gtkMenu), mi);
        lastWasSeparator = false;
    }
}

} // namespace

bool showNativePopupMenu(const juce::PopupMenu& menu, juce::Component* target,
                         juce::Point<int>, std::function<void(int)> callback) {
    GdkWindow* foreign = gtkParent(target);
    if (!foreign) {
        return false;
    }
    GtkWidget* gtkMenu = gtk_menu_new();
    auto* host = new GtkPopupMenuHost(gtkMenu, std::move(callback));
    populateGtkMenu(gtkMenu, menu, host);
    gtk_widget_show_all(gtkMenu);
    gtk_menu_popup_at_pointer(GTK_MENU(gtkMenu), nullptr);
    return true;
}

} // namespace carta
