/**
 * @file Main.cpp
 * @brief Desktop entry point, IPC client fast-path, and main window lifecycle.
 *
 * When invoked as an IPC/CLI client (`carta::runIpcClient`), `main()` forwards
 * the command to a running daemon and exits without creating a JUCE application.
 * Otherwise it initializes `CartaStudioApplication` on the JUCE message thread,
 * binds the IPC server to a `SafePointer<Studio>`, and optionally imports
 * startup audio/project arguments.
 */

#include "Studio/Studio.h"

#if !CARTA_STUDIO_COMPONENT_TEST
/**
 * @brief Top-level JUCE application managing the main `Studio` window and IPC server.
 */
class CartaStudioApplication final : public juce::JUCEApplication {
  public:
    const juce::String getApplicationName() override { return "Carta Studio"; }
    const juce::String getApplicationVersion() override { return "0.6.2"; }

    /**
     * @brief Creates the main window (hidden when `--headless`), starts IPC, and queues CLI files.
     */
    void initialise(const juce::String&) override {
        const auto params = getCommandLineParameterArray();
        const bool headless = params.contains("--headless");
#if JUCE_MAC
        if (headless) {
            juce::Process::setDockIconVisible(false);
        }
#endif
        window = std::make_unique<Window>(headless);
        ipc = std::make_unique<carta::IpcServer>(
            [safe = juce::Component::SafePointer<Studio>(window->studio())](
                const juce::StringArray& args) -> juce::var {
                if (safe != nullptr) {
                    return safe->ipcCommand(args);
                }
                auto* o = new juce::DynamicObject();
                o->setProperty("ok", false);
                o->setProperty("error", "Editor is closing");
                return juce::var(o);
            });
        if (ipc->error().isNotEmpty()) {
            std::fprintf(stderr, "Carta Studio IPC: %s\n", ipc->error().toRawUTF8());
        }
        for (const auto& argument : params) {
            if (argument != "--headless") {
                window->studio()->importFile(
                    juce::File::getCurrentWorkingDirectory().getChildFile(argument));
            }
        }
    }

    /**
     * @brief Tears down IPC before destroying the `Studio` window and audio callbacks.
     */
    void shutdown() override {
        ipc.reset();
        window.reset();
    }

    /**
     * @brief Handles OS quit requests, prompting to save unsaved GUI edits when interactive.
     */
    void systemRequestedQuit() override {
        if (window == nullptr) {
            quit();
            return;
        }
        if (window->isHeadless()) {
            window->studio()->approveCleanExit();
            quit();
            return;
        }
        window->studio()->requestClose([] { juce::JUCEApplication::quit(); });
    }

  private:
    /**
     * @brief Top-level document window hosting the `Studio` component.
     */
    class Window final : public juce::DocumentWindow {
      public:
        explicit Window(bool headless = false)
            : DocumentWindow("Carta Studio", juce::Colours::transparentBlack, allButtons),
              headlessWindow(headless) {
            setUsingNativeTitleBar(!headless);
            if (headless) {
                setTitleBarHeight(0);
            }
            setContentOwned(new Studio(headless), true);
            setResizable(true, true);
            setResizeLimits(600, 350, 4000, 2500);
            if (headless) {
                setBounds(0, 0, 1100, 650);
                setVisible(false);
            } else {
                centreWithSize(getWidth(), getHeight());
                setVisible(true);
                toFront(true);
                juce::MessageManager::callAsync(
                    [safe = juce::Component::SafePointer<Studio>(studio())] {
                        if (safe != nullptr &&
                            juce::Component::getCurrentlyModalComponent() == nullptr) {
                            safe->grabKeyboardFocus();
                        }
                    });
            }
        }

        bool isHeadless() const noexcept { return headlessWindow; }
        Studio* studio() const { return static_cast<Studio*>(getContentComponent()); }
        void closeButtonPressed() override {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

      private:
        bool headlessWindow = false;
    };

    std::unique_ptr<Window> window;
    std::unique_ptr<carta::IpcServer> ipc;
};

#if JUCE_MAC || JUCE_LINUX
JUCE_CREATE_APPLICATION_DEFINE(CartaStudioApplication)

int main(int argc, char* argv[]) {
    const auto result = carta::runIpcClient(argc, argv);
    if (result >= 0) {
        return result;
    }
    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main(argc, const_cast<const char**>(argv));
}
#else
START_JUCE_APPLICATION(CartaStudioApplication)
#endif
#endif
