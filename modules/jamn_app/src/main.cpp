#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "jamn_core/file_audio_device.h"
#include "jamn_dsp/jam_audio.h"
#include "jamn_platform/juce_audio_device.h"
#include "jamn_ui/jam_window_content.h"

namespace {

const char* FindOption(int argc, char* argv[], const char* name) {
    for (int index = 1; index < argc - 1; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return nullptr;
}

// Two deterministic checks, neither timing-dependent:
//  (a) can the platform backend be opened and closed without crashing?
//      Passes whether or not this machine has a sound card - which outcome
//      is not the assertion.
//  (b) does the shipped signal path run? A fixed block count through the
//      JUCE-free fake device, so it's deterministic regardless of (a).
//      Never run a fixed block count on the real device - that would be
//      timing-dependent and flaky.
int RunHeadless(int argc, char* argv[]) {
    // AudioDeviceManager reaches into MIDI device enumeration and
    // AsyncUpdater internally, both of which assert that a MessageManager
    // exists and that the calling thread is the message thread - true even
    // though this path never opens a window. This is JUCE's own documented
    // fix for exactly this: "particularly handy... at the beginning of a
    // console app's main()".
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    // Declared in this order so the device is destroyed before audio, same
    // as JamnApplication below - the audio thread must never touch a dead
    // JamAudio.
    jamn::dsp::JamAudio audio;
    jamn::platform::JuceAudioDevice device;

    const std::string error =
        device.Open(2, [&audio](double sampleRate, int) { audio.Prepare(sampleRate); },
                    [&audio](float* const* out, int numChannels, int numFrames) {
                        audio.Process(out, numChannels, numFrames);
                    });
    std::printf("jamn_app --headless: audio device: %s\n",
                error.empty() ? device.deviceName().c_str() : error.c_str());
    device.Close();

    audio.Prepare(48000.0);
    audio.SetGain(0.5f);
    audio.Trigger();

    jamn::core::FileAudioDevice fake(2, 128);
    if (const char* path = FindOption(argc, argv, "--out")) {
        fake.SetOutputPath(path);
    }
    fake.Process(256, [&audio](float* const* out, int numChannels, int numFrames) {
        audio.Process(out, numChannels, numFrames);
    });

    std::printf("jamn_app --headless: rendered 256 blocks of 128 frames\n");
    return 0;
}

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(const juce::String& name, std::unique_ptr<juce::Component> content)
        : DocumentWindow(name,
                          juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                              juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(content.release(), true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class JamnApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "JamN"; }
    const juce::String getApplicationVersion() override { return "0.0.1"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override {
        // Both lambdas below run on the message thread and nowhere else -
        // that is the single-producer half of JamAudio's trigger ring's
        // SpscRing contract.
        const std::string error = device_.Open(
            2, [this](double sampleRate, int) { audio_.Prepare(sampleRate); },
            [this](float* const* out, int numChannels, int numFrames) {
                audio_.Process(out, numChannels, numFrames);
            });

        if (!error.empty()) {
            // No sound card is not a fatal condition - the window still
            // opens and the controls still work, they just make no noise.
            // This dev box is exactly that machine.
            DBG("JamN: audio device unavailable: " << error);
            audio_.Prepare(48000.0);
        }

        auto content = std::make_unique<jamn::ui::JamWindowContent>();
        content->onButtonClicked = [this] { audio_.Trigger(); };
        content->onGainChanged = [this](float gain) { audio_.SetGain(gain); };
        // The slider is constructed with dontSendNotification (see
        // jam_window_content.cpp), so onGainChanged never fires on its
        // own for the initial value - without this, MasterBus would sit
        // at its own default (unity) while the slider displays kDefaultGain.
        audio_.SetGain(jamn::ui::JamWindowContent::kDefaultGain);
        window_ = std::make_unique<MainWindow>(getApplicationName(), std::move(content));
    }

    void shutdown() override {
        device_.Close();
        window_.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    // Declaration order is load-bearing: device_ is destroyed before
    // audio_, so the audio thread can never touch a dead JamAudio.
    jamn::dsp::JamAudio audio_;
    jamn::platform::JuceAudioDevice device_;
    std::unique_ptr<MainWindow> window_;
};

juce::JUCEApplicationBase* CreateJamnApplication() {
    return new JamnApplication();
}

}  // namespace

// What START_JUCE_APPLICATION expands to on this platform (see
// juce_Initialisation.h), spelled out so the --headless check below can run
// before any JUCE GUI machinery starts - a contributor's machine may have
// no display, and --headless must still work there.
int main(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--headless") == 0) {
            return RunHeadless(argc, argv);
        }
    }

    juce::JUCEApplicationBase::createInstance = &CreateJamnApplication;
    return juce::JUCEApplicationBase::main(argc, const_cast<const char**>(argv));
}
