// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi live host: the app that plays a Live set's tracks through the suite's own plugins. For now it loads one
//  installed Encoder as a VST3, gives it the layout the host will give every Encoder -- a stereo track in, a stereo
//  sidechain, the field out at the host's order -- prepares it, and shows its own window. No audio yet.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Plugins.h"

namespace {

constexpr int kOrder = 3;  ///< one order for every plugin the host loads; VST3 carries up to 7th
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

class Content final : public juce::Component {
public:
    Content() {
        loaded_ =
            bambi::livehost::loadInstalled("bambi Encoder", bambi::livehost::Role::Encoder, kOrder, kRate, kBlock);
        status_.setText(loaded_.status, juce::dontSendNotification);
        status_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(status_);
        if (loaded_.plugin != nullptr) {
            editor_.reset(loaded_.plugin->createEditorIfNeeded());
            if (editor_ != nullptr) addAndMakeVisible(*editor_);
        }
        const int w = editor_ != nullptr ? editor_->getWidth() : 640;
        const int h = (editor_ != nullptr ? editor_->getHeight() : 120) + kStatus;
        setSize(w, h);
    }

    ~Content() override {
        //  the window before the instance it shows
        editor_.reset();
        if (loaded_.plugin != nullptr) loaded_.plugin->releaseResources();
    }

    void resized() override {
        auto area = getLocalBounds();
        status_.setBounds(area.removeFromTop(kStatus).reduced(8, 0));
        if (editor_ != nullptr) editor_->setBounds(area.withSize(editor_->getWidth(), editor_->getHeight()));
    }

private:
    static constexpr int kStatus = 24;
    bambi::livehost::Loaded loaded_;
    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    juce::Label status_;
};

class Window final : public juce::DocumentWindow {
public:
    Window()
        : DocumentWindow("bambi live host", juce::Colours::black,
                         DocumentWindow::closeButton | DocumentWindow::minimiseButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(new Content(), true);
        setResizable(false, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class App final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "bambi live host"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }
    void initialise(const juce::String&) override { window_ = std::make_unique<Window>(); }
    void shutdown() override { window_.reset(); }

private:
    std::unique_ptr<Window> window_;
};

}  // namespace

START_JUCE_APPLICATION(App)
