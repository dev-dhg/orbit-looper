#pragma once

#include "../PluginProcessor.h"
#include <juce_gui_extra/juce_gui_extra.h>

class OrbitLooperAudioProcessorEditor; // Forward declaration

class OrbitLooperWebBrowser : public juce::WebBrowserComponent {
public:
  OrbitLooperWebBrowser(
      OrbitLooperAudioProcessor &p, OrbitLooperAudioProcessorEditor *e,
      juce::WebSliderRelay &loopLevel, juce::WebSliderRelay &inputGain,
      juce::WebSliderRelay &outputGain, juce::WebSliderRelay &inputPan,
      juce::WebSliderRelay &outputPan);

  ~OrbitLooperWebBrowser() override;

  // Overrides for debugging loading
  bool pageAboutToLoad(const juce::String &newURL) override;
  bool pageLoadHadNetworkError(const juce::String &errorInfo) override;
  void pageFinishedLoading(const juce::String &url) override;
  void newWindowAttemptingToLoad(const juce::String &newURL) override;

private:
  OrbitLooperAudioProcessor &audioProcessor;
  OrbitLooperAudioProcessorEditor *editor;
  std::shared_ptr<juce::FileChooser> exportFileChooser;

  // Resource provider callback
  std::optional<juce::WebBrowserComponent::Resource>
  getResource(const juce::String &url);

  static const char *getMimeForExtension(const juce::String &extension);
  static juce::String getExtension(juce::String filename);
  static juce::String sanitiseResourcePath(juce::String path);

  // Factory method for WebBrowserComponent::Options to pass to base constructor
  static juce::WebBrowserComponent::Options createOptions(
      OrbitLooperAudioProcessor &p, OrbitLooperAudioProcessorEditor *e,
      juce::WebSliderRelay &loopLevel, juce::WebSliderRelay &inputGain,
      juce::WebSliderRelay &outputGain, juce::WebSliderRelay &inputPan,
      juce::WebSliderRelay &outputPan,
      OrbitLooperWebBrowser
          *browserInstance); // Needed to bind the member resource provider

  void triggerExportDialog();

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OrbitLooperWebBrowser)
};
