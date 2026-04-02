#pragma once

#include "PluginProcessor.h"
#include "ui/OrbitLooperWebBrowser.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

//==============================================================================
/**
 * OrbitLooper Plugin Editor - WebView UI Integration
 *
 * CRITICAL: Member declaration order MUST be:
 * 1. Parameter relays (destroyed last)
 * 2. WebBrowserComponent (destroyed middle)
 * 3. Parameter attachments (destroyed first)
 *
 * This order prevents DAW crashes on plugin unload.
 */
class OrbitLooperAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        public juce::Timer {
public:
  OrbitLooperAudioProcessorEditor(OrbitLooperAudioProcessor &);
  ~OrbitLooperAudioProcessorEditor() override;

  //==============================================================================
  void paint(juce::Graphics &) override;
  void resized() override;
  void parentHierarchyChanged() override;

  //==============================================================================
  void timerCallback() override;

  void setWebUiReady(bool ready);

private:
  //==============================================================================
  // CRITICAL: MEMBER DECLARATION ORDER
  // DO NOT REORDER - This prevents DAW crash on unload
  //==============================================================================

  // 1. PARAMETER RELAYS (Destroyed last)
  juce::WebSliderRelay loopLevelRelay{"loop_level"};
  juce::WebSliderRelay maxLoopLengthRelay{"max_loop_length"};
  juce::WebSliderRelay inputGainRelay{"input_gain"};
  juce::WebSliderRelay outputGainRelay{"output_gain"};
  juce::WebSliderRelay inputPanRelay{"input_pan"};
  juce::WebSliderRelay outputPanRelay{"output_pan"};

  // 2. WEBBROWSERCOMPONENT (Destroyed middle)
  std::unique_ptr<OrbitLooperWebBrowser> webView;

  // 3. PARAMETER ATTACHMENTS (Destroyed first)
  std::unique_ptr<juce::WebSliderParameterAttachment> loopLevelAttachment;
  std::unique_ptr<juce::WebSliderParameterAttachment> maxLoopLengthAttachment;
  std::unique_ptr<juce::WebSliderParameterAttachment> inputGainAttachment;
  std::unique_ptr<juce::WebSliderParameterAttachment> outputGainAttachment;
  std::unique_ptr<juce::WebSliderParameterAttachment> inputPanAttachment;
  std::unique_ptr<juce::WebSliderParameterAttachment> outputPanAttachment;

  // Reference to processor
  OrbitLooperAudioProcessor &audioProcessor;

  juce::String initialWebUrl;
  bool initialNavigationDone = false;

  // Resizable window support
  juce::ComponentBoundsConstrainer constrainer;
  static constexpr int designWidth = 580;
  static constexpr int designHeight = 720;
  double currentZoom = 1.0;
  std::atomic<bool> webUiReady{false};
  bool muteOnStartupPushed = false; // One-shot: push muteOnStartup state to JS
  bool settingsSyncPushed = false;  // One-shot: push maxLoopSeconds + maxLayerCount to JS
  bool btClassicLastDevicePushed = false; // One-shot: push stored BT Classic MAC to JS
  juce::String btClassicLastDeviceMac; // MAC loaded from PropertiesFile at startup
  juce::String btClassicLastPersistedMac; // Last MAC we wrote to PropertiesFile (avoid repeated writes)

#if JUCE_ANDROID
  // Android: periodic state save (every ~5s) because the OS can kill the app
  // without calling destructors when swiped away or backgrounded.
  int64_t lastStateSaveMs = 0;
  static constexpr int64_t STATE_SAVE_INTERVAL_MS = 5000;
  bool bufferOptimizeDone = false; // One-shot: set lowest buffer on first launch
#endif

  //==============================================================================
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OrbitLooperAudioProcessorEditor)
};
