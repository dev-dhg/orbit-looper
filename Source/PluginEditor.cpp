#include "PluginEditor.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_audio_utils/juce_audio_utils.h>

#if JUCE_LINUX
#include <cstdlib>
#endif

// Temporary debug logging to file (Windows standalone has no console)

//==============================================================================
OrbitLooperAudioProcessorEditor::OrbitLooperAudioProcessorEditor(
    OrbitLooperAudioProcessor &p)
    : AudioProcessorEditor(&p), audioProcessor(p) {
  DBG("OrbitLooper: Editor constructor started");

#if JUCE_LINUX
  // WebKitGTK can render a white surface in some plugin-host/driver setups
  // unless sandbox/compositing and dmabuf renderer are relaxed.
  ::setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);
  ::setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 1);
  ::setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
  DBG("OrbitLooper: Linux WebKit env set (sandbox/compositing/dmabuf "
      "workarounds)");
#endif

  //==========================================================================
  // CRITICAL: CREATION ORDER
  // 1. Relays already created (member initialization)
  // 2. Create attachments FIRST (before WebView)
  // 3. Create WebBrowserComponent with proper JUCE 8 API
  // 4. addAndMakeVisible LAST
  //==========================================================================

  // Create parameter attachments BEFORE creating WebView
  DBG("OrbitLooper: Creating parameter attachments");

  loopLevelAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
      *audioProcessor.apvts.getParameter("loop_level"), loopLevelRelay);

  maxLoopLengthAttachment =
      std::make_unique<juce::WebSliderParameterAttachment>(
          *audioProcessor.apvts.getParameter("max_loop_length"),
          maxLoopLengthRelay);

  inputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
      *audioProcessor.apvts.getParameter("input_gain"), inputGainRelay);

  outputGainAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
      *audioProcessor.apvts.getParameter("output_gain"), outputGainRelay);

  inputPanAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
      *audioProcessor.apvts.getParameter("input_pan"), inputPanRelay);

  outputPanAttachment = std::make_unique<juce::WebSliderParameterAttachment>(
      *audioProcessor.apvts.getParameter("output_pan"), outputPanRelay);

  // Create WebBrowserComponent with JUCE 8 API
  DBG("OrbitLooper: Creating WebView");
  webView = std::make_unique<OrbitLooperWebBrowser>(
      audioProcessor, this, loopLevelRelay, maxLoopLengthRelay, inputGainRelay,
      outputGainRelay, inputPanRelay, outputPanRelay);

  // addAndMakeVisible AFTER attachments are created
  DBG("OrbitLooper: Adding WebView to component");
  addAndMakeVisible(*webView);

  // Load web content via resource provider
  DBG("OrbitLooper: Loading web content");
  const auto resourceRoot =
      juce::WebBrowserComponent::getResourceProviderRoot();
  const auto initialUrl = resourceRoot.endsWithChar('/')
                              ? (resourceRoot + "index.html")
                              : (resourceRoot + "/index.html");
  DBG("OrbitLooper: Resource root = " + resourceRoot);
  DBG("OrbitLooper: Initial URL = " + initialUrl);

  initialWebUrl = initialUrl;
  DBG("OrbitLooper: Deferring initial navigation until editor is sized");

  // Set editor size and enable resizing
  constrainer.setMinimumSize(240, 350);
  constrainer.setMaximumSize(1440, 2880);
  constrainer.setFixedAspectRatio(static_cast<double>(designWidth) /
                                  static_cast<double>(designHeight));
  setConstrainer(&constrainer);
  setResizable(true, true);
  setSize(580, 720);

  // Start timer for state/meter updates (30 Hz)
  startTimerHz(30);

  DBG("OrbitLooper: Editor constructor completed");
}

OrbitLooperAudioProcessorEditor::~OrbitLooperAudioProcessorEditor() {
  stopTimer();
  webUiReady.store(false);

  // Navigate away from the resource-provider URL before destruction.
  // On Linux/WebKitGTK, destroying the WebView while the resource provider
  // is still servicing requests can deadlock. Loading about:blank stops
  // all pending resource fetches and script execution.
#if JUCE_LINUX
  if (webView) {
    webView->setVisible(false);
    removeChildComponent(webView.get());
    webView.reset();

    // Allow GTK message loop to process the destruction events before
    // the plugin dynamics library unloads, preventing a host hang.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
  }
#else
  if (webView)
    webView->goToURL("about:blank");
#endif

  // Destruction in reverse declaration order:
  // 1. Attachments destroyed first
  // 2. WebView destroyed next
  // 3. Relays destroyed last
}

//==============================================================================
void OrbitLooperAudioProcessorEditor::paint(juce::Graphics &g) {
  g.fillAll(juce::Colour::fromString("ff0a0a0f")); // Match --bg-dark
}

void OrbitLooperAudioProcessorEditor::setWebUiReady(bool ready) {
  webUiReady.store(ready);
  if (ready) {
    // Force a zoom update as soon as the UI is ready
    webView->evaluateJavascript("if (window.setZoom) window.setZoom(" +
                                juce::String(currentZoom, 4) + ");");

    // Set standalone mode for UI (shows/hides audio settings icon)
    bool isStandalone = juce::PluginHostType::getPluginLoadedAs() ==
                        juce::AudioProcessor::wrapperType_Standalone;
    webView->evaluateJavascript(
        "if (window.setStandaloneMode) window.setStandaloneMode(" +
        juce::String(isStandalone ? "true" : "false") + ");");
  }
}

void OrbitLooperAudioProcessorEditor::resized() {
  if (webView) {
    webView->setBounds(getLocalBounds());

    if (!initialNavigationDone && getWidth() > 0 && getHeight() > 0 &&
        initialWebUrl.isNotEmpty()) {
      initialNavigationDone = true;
      DBG("OrbitLooper: Performing deferred navigation to " + initialWebUrl);
      webView->goToURL(initialWebUrl);
    }
  }

  // Calculate zoom based on the smaller of width or height scale to maintain
  // aspect ratio and fit within the window.
  double zoomW =
      static_cast<double>(getWidth()) / static_cast<double>(designWidth);
  double zoomH =
      static_cast<double>(getHeight()) / static_cast<double>(designHeight);
  double newZoom = std::min(zoomW, zoomH);

  // Only update if there's a significant change to reduce flashing/flicker
  if (std::abs(newZoom - currentZoom) > 0.0001) {
    currentZoom = newZoom;
    if (webView && webUiReady.load())
      webView->evaluateJavascript("if (window.setZoom) window.setZoom(" +
                                  juce::String(currentZoom, 4) + ");");
  }
}

//==============================================================================
void OrbitLooperAudioProcessorEditor::timerCallback() {
  if (!webView || !webView->isVisible())
    return;

  if (!initialNavigationDone && initialWebUrl.isNotEmpty() && getWidth() > 0 &&
      getHeight() > 0) {
    initialNavigationDone = true;
    DBG("OrbitLooper: Performing timer fallback navigation to " +
        initialWebUrl);
    webView->goToURL(initialWebUrl);
    return;
  }

  if (!webUiReady.load())
    return;

  // Get looper state from processor (thread-safe atomics)
  int state = audioProcessor.looperState.load();
  float position = audioProcessor.playbackPosition.load();
  float inputLevel = audioProcessor.inputPeakLevel.load();
  float outputLevel = audioProcessor.outputPeakLevel.load();
  float loopLength = audioProcessor.loopLengthSeconds.load();
  bool canUndo = audioProcessor.hasUndoLayer.load();
  int maxLoopLen = audioProcessor.getMaxLoopLength();
  bool isMuted = audioProcessor.inputMuted.load();

  // Read and consume visual feedback flags
  bool undoFlash = audioProcessor.undoTriggered.exchange(false);
  bool clearFlash = audioProcessor.clearTriggered.exchange(false);
  int flashType =
      clearFlash ? 2
                 : (undoFlash ? 1 : 0); // 0=none, 1=undo(orange), 2=clear(red)
  bool isArmed = audioProcessor.overdubArmedForUI.load();

  try {
    // Send looper state to JavaScript
    juce::String stateJS =
        "if (window.updateLooperState) { window.updateLooperState(" +
        juce::String(state) + ", " + juce::String(position, 4) + ", " +
        juce::String(inputLevel, 3) + ", " + juce::String(outputLevel, 3) +
        ", " + juce::String(loopLength, 2) + ", " +
        juce::String(canUndo ? "true" : "false") + ", " +
        juce::String(maxLoopLen) + ", " + juce::String(flashType) + ", " +
        juce::String(isArmed ? "true" : "false") + ", " +
        juce::String(isMuted ? "true" : "false") + ", " +
        juce::String(audioProcessor.loopMode.load()) + ", " +
        juce::String(audioProcessor.isAutoLength.load() ? "true" : "false") +
        "); }";
    DBG("OrbitLooper state update: " << stateJS);
    webView->evaluateJavascript(stateJS);

    // Send MIDI CC mapping state
    using MA = OrbitLooperAudioProcessor::MidiAction;
    int ccRec = audioProcessor.getMidiCC(MA::Record);
    int ccStop = audioProcessor.getMidiCC(MA::Stop);
    int ccClr = audioProcessor.getMidiCC(MA::Clear);
    int ccUndo = audioProcessor.getMidiCC(MA::Undo);
    int ccFoot = audioProcessor.getMidiCC(MA::Footswitch);
    int ccOvr = audioProcessor.getMidiCC(MA::Overdub);
    int ccBar = audioProcessor.getMidiCC(MA::BarMode);
    int ccClk = audioProcessor.getMidiCC(MA::Click);
    int ccPre = audioProcessor.getMidiCC(MA::PreCount);
    int ccArm = audioProcessor.getMidiCC(MA::ArmOverdub);
    int ccPClk = audioProcessor.getMidiCC(MA::PlayClick);
    int ccPlay = audioProcessor.getMidiCC(MA::Play);
    int ccMon = audioProcessor.getMidiCC(MA::Monitor);
    int ccLMC = audioProcessor.getMidiCC(MA::LoopModeCycle);
    int ccClas = audioProcessor.getMidiCC(MA::ClassicMode);
    int ccBeats = audioProcessor.getMidiCC(MA::BeatsMode);
    int ccDyn = audioProcessor.getMidiCC(MA::DynamicMode);
    int ccPanL = audioProcessor.getMidiCC(MA::PanInputLeft);
    int ccPanC = audioProcessor.getMidiCC(MA::PanInputCenter);
    int ccPanR = audioProcessor.getMidiCC(MA::PanInputRight);
    bool learning = audioProcessor.isMidiLearning();
    int learnTarget =
        learning ? static_cast<int>(audioProcessor.getMidiLearnTarget()) : -1;

    juce::String midiJS =
        "if (window.updateMidiState) { window.updateMidiState(" +
        juce::String(ccRec) + ", " + juce::String(ccStop) + ", " +
        juce::String(ccClr) + ", " + juce::String(ccUndo) + ", " +
        juce::String(ccFoot) + ", " + juce::String(ccOvr) + ", " +
        juce::String(ccBar) + ", " + juce::String(ccClk) + ", " +
        juce::String(ccPre) + ", " + juce::String(ccArm) + ", " +
        juce::String(ccPClk) + ", " + juce::String(ccPlay) + ", " +
        juce::String(ccMon) + ", " + juce::String(ccLMC) + ", " +
        juce::String(ccClas) + ", " + juce::String(ccBeats) + ", " +
        juce::String(ccDyn) + ", " + juce::String(ccPanL) + ", " +
        juce::String(ccPanC) + ", " + juce::String(ccPanR) + ", " +
        juce::String(learning ? "true" : "false") + ", " +
        juce::String(learnTarget) + "); }";
    webView->evaluateJavascript(midiJS);

    // Consume UI toggle flags from MIDI CC triggers
    if (audioProcessor.pendingBarModeToggle.exchange(false))
      webView->evaluateJavascript(
          "if (window.toggleBarMode) window.toggleBarMode();");
    if (audioProcessor.pendingClickToggle.exchange(false))
      webView->evaluateJavascript(
          "if (window.toggleClick) window.toggleClick();");
    if (audioProcessor.pendingPreCountToggle.exchange(false))
      webView->evaluateJavascript(
          "if (window.togglePreCount) window.togglePreCount();");
    if (audioProcessor.pendingOverdubArmToggle.exchange(false))
      webView->evaluateJavascript(
          "if (window.toggleArmOverdub) window.toggleArmOverdub();");
    if (audioProcessor.pendingPlayClickToggle.exchange(false))
      webView->evaluateJavascript(
          "if (window.togglePlayClick) window.togglePlayClick();");
    if (audioProcessor.pendingMonitorToggle.exchange(false))
      webView->evaluateJavascript(
          "if (window.toggleMonitor) window.toggleMonitor();");
    if (audioProcessor.pendingLoopModeCycle.exchange(false))
      webView->evaluateJavascript(
          "if (window.toggleLoopModeCycle) window.toggleLoopModeCycle();");
    if (audioProcessor.pendingSetClassicMode.exchange(false))
      webView->evaluateJavascript(
          "if (window.setLoopMode) window.setLoopMode(0);");
    if (audioProcessor.pendingSetBeatsMode.exchange(false))
      webView->evaluateJavascript(
          "if (window.setLoopMode) window.setLoopMode(1);");
    if (audioProcessor.pendingSetDynamicMode.exchange(false))
      webView->evaluateJavascript(
          "if (window.setLoopMode) window.setLoopMode(2);");
    // Pan input shortcuts are handled via APVTS parameter directly in
    // processStateChanges The WebSliderParameterAttachment will sync the UI
    // automatically
    audioProcessor.pendingPanInputLeft.exchange(false);
    audioProcessor.pendingPanInputCenter.exchange(false);
    audioProcessor.pendingPanInputRight.exchange(false);

    // Send key binding state
    if (audioProcessor.keyBindingsChanged.exchange(false)) {
      juce::String keyJS =
          "if (window.updateKeyBindings) { window.updateKeyBindings(";
      for (int i = 0; i < OrbitLooperAudioProcessor::NUM_KEY_ACTIONS; ++i) {
        if (i > 0)
          keyJS += ", ";
        keyJS += "'" +
                 audioProcessor.getKeyBinding(static_cast<MA>(i))
                     .replace("'", "\\'") +
                 "'";
      }
      keyJS += "); }";
      webView->evaluateJavascript(keyJS);
    }

    // Send metronome beat feedback to JS (for visual highlighting + pre-count
    // tracking)
    {
      int beatInBar = audioProcessor.metroCurrentBeatForUI.load();
      int totalBeats = audioProcessor.metroTotalBeatCount.load();
      juce::String metroJS =
          "if (window.updateMetroBeat) { window.updateMetroBeat(" +
          juce::String(beatInBar) + ", " + juce::String(totalBeats) + "); }";
      webView->evaluateJavascript(metroJS);
    }
  } catch (...) {
    // Silently ignore WebView errors during timer callback
  }
}

void OrbitLooperAudioProcessorEditor::parentHierarchyChanged() {
  if (auto *top = getTopLevelComponent()) {
    // If running as standalone, hide the JUCE title bar and use native window
    if (juce::PluginHostType::getPluginLoadedAs() ==
        juce::AudioProcessor::wrapperType_Standalone) {
      if (auto *window = dynamic_cast<juce::DocumentWindow *>(top)) {
        window->setUsingNativeTitleBar(true);
      }
      // On some platforms (like Linux), the top level standalone window doesn't
      // automatically inherit the editor's constrainer min/max/aspect ratio
      // accurately.
      // if (auto *resizableWindow = dynamic_cast<juce::ResizableWindow *>(top))
      // {
      //   resizableWindow->setConstrainer(&constrainer);
      // }
    }
  }
}
