#include "PluginEditor.h"
#include "BinaryData.h"
#include "PluginProcessor.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_audio_utils/juce_audio_utils.h>

#if JUCE_LINUX
#include <cstdlib>
#endif

#if JUCE_ANDROID
#include "BluetoothClassicMidi.h"
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
      audioProcessor, this, loopLevelRelay, inputGainRelay, outputGainRelay,
      inputPanRelay, outputPanRelay);

  // addAndMakeVisible AFTER attachments are created
  DBG("OrbitLooper: Adding WebView to component");
  addAndMakeVisible(*webView);

  // Load web content via resource provider
  DBG("OrbitLooper: Loading web content");
  const auto resourceRoot =
      juce::WebBrowserComponent::getResourceProviderRoot();

#if JUCE_ANDROID
  // Use the mobile-optimised HTML (bottom nav, no scaling, native layout).
  const auto htmlFile = juce::String("mobile-index.html");
#else
  const auto htmlFile = juce::String("index.html");
#endif

  const auto initialUrl = resourceRoot.endsWithChar('/')
                              ? (resourceRoot + htmlFile)
                              : (resourceRoot + "/" + htmlFile);
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

#if JUCE_ANDROID
  // On Android, fill the full screen. The JS auto-zoom in android-index.html
  // will scale the 580x720 design to fit whatever area JUCE gives us.
  {
    auto& displays = juce::Desktop::getInstance().getDisplays();
    if (const auto* display = displays.getPrimaryDisplay()) {
      const auto area = display->userArea;
      DBG("OrbitLooper: Android display userArea = " +
          juce::String(area.getWidth()) + "x" + juce::String(area.getHeight()));
      setSize(area.getWidth(), area.getHeight());
    } else {
      setSize(580, 720);
    }
  }
  // No resizing on Android — it's a fixed fullscreen standalone app.
  setResizable(false, false);

  // Load muteOnStartup from persistent storage (PropertiesFile)
  {
    juce::PropertiesFile props(makeSettingsOptions());
    bool savedMuteOnStartup = props.getBoolValue("muteOnStartup", true);
    audioProcessor.muteOnStartup.store(savedMuteOnStartup);
    DBG("OrbitLooper: muteOnStartup loaded = " + juce::String(savedMuteOnStartup ? "true" : "false"));
  }

  // Load last connected BT Classic device MAC from persistent storage
  {
    juce::PropertiesFile props(makeSettingsOptions());
    btClassicLastDeviceMac = props.getValue("btClassicLastDevice", "");
    DBG("OrbitLooper: btClassicLastDevice loaded = " + btClassicLastDeviceMac);
  }

  // Initialize the Bluetooth Classic MIDI JNI bridge
  BluetoothClassicMidi::initialize();

  // NOTE: We do NOT touch JUCE's native standalone mute during construction —
  // doing so interferes with audio device enumeration on Android.
  // Instead, we unmute JUCE's native mute after a short delay (once audio
  // devices are fully initialized), and only if muteOnStartup is OFF.
  // When muteOnStartup is ON, JUCE stays muted AND our feedbackMuted is true,
  // so both the device-level mute and our DSP-level mute are active.
  // The user unmutes both via the red banner or Settings toggle.
  if (!audioProcessor.muteOnStartup.load()) {
    juce::Timer::callAfterDelay(1000, []() {
      if (auto* h = juce::StandalonePluginHolder::getInstance())
        h->getMuteInputValue().setValue(false);
    });
  }

  // Apply our own feedback mute based on the saved preference.
  // When muteOnStartup is ON (default), mute input pass-through to prevent
  // feedback from internal speakers. This is separate from the Monitor
  // (speaker icon) toggle — feedbackMuted is for startup/settings only.
  audioProcessor.feedbackMuted.store(audioProcessor.muteOnStartup.load());
#else
  setResizable(true, true);
  setSize(580, 720);
#endif

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
    // A (re)loaded page starts with no state — re-arm the one-shot pushes
    audioProcessor.midiStateChanged.store(true);
    audioProcessor.keyBindingsChanged.store(true);

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

  try {
    pushLooperState();
    pushMidiState();
    consumeUiToggleFlags();
    pushKeyBindings();
    pushMetronomeBeat();
    pushMidiActivity();
#if JUCE_ANDROID
    androidTimerTasks();
#endif
  } catch (...) {
    // Silently ignore WebView errors during timer callback
  }
}

juce::PropertiesFile::Options
OrbitLooperAudioProcessorEditor::makeSettingsOptions() {
  juce::PropertiesFile::Options opts;
  opts.applicationName = "OrbitLooper";
  opts.folderName = "OrbitLooper";
  opts.filenameSuffix = ".settings";
  return opts;
}

juce::String OrbitLooperAudioProcessorEditor::jsQuoted(const juce::String &s) {
  // JSON string escaping produces a valid double-quoted JS string literal
  return juce::JSON::toString(juce::var(s), true);
}

void OrbitLooperAudioProcessorEditor::pushLooperState() {
  // Get looper state from processor (thread-safe atomics)
  int state = audioProcessor.looperState.load();
  float position = audioProcessor.playbackPosition.load();
  float inputLevel = audioProcessor.inputPeakLevel.load();
  float outputLevel = audioProcessor.outputPeakLevel.load();
  float loopLength = audioProcessor.loopLengthSeconds.load();
  bool canUndo = audioProcessor.hasUndoLayer.load();
  bool isMuted = audioProcessor.inputMuted.load();

  // Seconds the recording ring/time display is scaled against (locked
  // length or Global Max while RECORDING; Global Max otherwise).
  float recordBasisSec =
      state == static_cast<int>(OrbitLooperAudioProcessor::LooperState::
                                    RECORDING)
          ? audioProcessor.recordBasisSeconds.load()
          : audioProcessor.getMaxLoopLength();

  // Read and consume visual feedback flags
  bool undoFlash = audioProcessor.undoTriggered.exchange(false);
  bool clearFlash = audioProcessor.clearTriggered.exchange(false);
  int flashType =
      clearFlash ? 2
                 : (undoFlash ? 1 : 0); // 0=none, 1=undo(orange), 2=clear(red)
  bool isArmed = audioProcessor.overdubArmedForUI.load();

  juce::String stateJS =
      "if (window.updateLooperState) { window.updateLooperState(" +
      juce::String(state) + ", " + juce::String(position, 4) + ", " +
      juce::String(inputLevel, 3) + ", " + juce::String(outputLevel, 3) +
      ", " + juce::String(loopLength, 2) + ", " +
      juce::String(canUndo ? "true" : "false") + ", " +
      juce::String(recordBasisSec, 1) + ", " + juce::String(flashType) +
      ", " + juce::String(isArmed ? "true" : "false") + ", " +
      juce::String(isMuted ? "true" : "false") + ", " +
      juce::String(audioProcessor.loopMode.load()) + "); }";
  webView->evaluateJavascript(stateJS);
}

void OrbitLooperAudioProcessorEditor::pushMidiState() {
  using MA = OrbitLooperAudioProcessor::MidiAction;

  // Only push when mappings or learn state actually changed (30 Hz → rare)
  if (!audioProcessor.midiStateChanged.exchange(false))
    return;

  bool learning = audioProcessor.isMidiLearning();
  int learnTarget =
      learning ? static_cast<int>(audioProcessor.getMidiLearnTarget()) : -1;

  juce::String midiJS =
      "if (window.updateMidiState) { window.updateMidiState(";
  for (int i = 0; i < OrbitLooperAudioProcessor::NUM_MIDI_ACTIONS; ++i)
    midiJS += juce::String(audioProcessor.getMidiCC(static_cast<MA>(i))) +
              ", ";
  midiJS += juce::String(learning ? "true" : "false") + ", " +
            juce::String(learnTarget) + "); }";
  webView->evaluateJavascript(midiJS);
}

void OrbitLooperAudioProcessorEditor::consumeUiToggleFlags() {
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
  // processStateChanges. The WebSliderParameterAttachment will sync the UI
  // automatically.
  audioProcessor.pendingPanInputLeft.exchange(false);
  audioProcessor.pendingPanInputCenter.exchange(false);
  audioProcessor.pendingPanInputRight.exchange(false);
}

void OrbitLooperAudioProcessorEditor::pushKeyBindings() {
  using MA = OrbitLooperAudioProcessor::MidiAction;
  if (audioProcessor.keyBindingsChanged.exchange(false)) {
    juce::String keyJS =
        "if (window.updateKeyBindings) { window.updateKeyBindings(";
    for (int i = 0; i < OrbitLooperAudioProcessor::NUM_KEY_ACTIONS; ++i) {
      if (i > 0)
        keyJS += ", ";
      keyJS += jsQuoted(audioProcessor.getKeyBinding(static_cast<MA>(i)));
    }
    keyJS += "); }";
    webView->evaluateJavascript(keyJS);
  }
}

void OrbitLooperAudioProcessorEditor::pushMetronomeBeat() {
  // Metronome beat feedback (visual highlighting + pre-count tracking)
  int beatInBar = audioProcessor.metroCurrentBeatForUI.load();
  int totalBeats = audioProcessor.metroTotalBeatCount.load();
  webView->evaluateJavascript(
      "if (window.updateMetroBeat) { window.updateMetroBeat(" +
      juce::String(beatInBar) + ", " + juce::String(totalBeats) + "); }");
}

void OrbitLooperAudioProcessorEditor::pushMidiActivity() {
  // MIDI activity staleness (BLE connection indicator)
  int64_t lastActivity = audioProcessor.lastMidiActivityMs.load();
  int64_t now = juce::Time::currentTimeMillis();
  bool midiActive = (lastActivity > 0) && ((now - lastActivity) < 5000);
  webView->evaluateJavascript(
      "if(window.updateMidiActivity) window.updateMidiActivity(" +
      juce::String(midiActive ? "true" : "false") + ");");
}

#if JUCE_ANDROID
void OrbitLooperAudioProcessorEditor::androidTimerTasks() {
  // Push feedback-muted state for the mobile custom mute banner.
  // This is separate from the Monitor (speaker icon) toggle.
  bool fbMuted = audioProcessor.feedbackMuted.load();
  webView->evaluateJavascript(
      "if(window.setStandaloneMuteState){window.setStandaloneMuteState(" +
      juce::String(fbMuted ? "true" : "false") + ");}");

  // Hide the native JUCE NotificationArea (yellow mute banner).
  // We use our own custom red WebView banner instead.
  // Only scan for a short window after startup or a mute change — the
  // banner can only (re)appear when the device-level mute state flips.
  if (fbMuted != lastFeedbackMuted) {
    lastFeedbackMuted = fbMuted;
    bannerScanTicksRemaining = 90; // re-arm ~3s scan window
  }
  if (bannerScanTicksRemaining > 0) {
    --bannerScanTicksRemaining;
    if (auto* parent = getParentComponent()) {
      for (int i = 0; i < parent->getNumChildComponents(); ++i) {
        auto* child = parent->getChildComponent(i);
        if (child != this && child != webView.get() && child->isVisible() &&
            child->getHeight() <= 30) {
          child->setVisible(false);
          child->setSize(0, 0);
        }
      }
    }
  }

  // Push muteOnStartup toggle state (once, on first frame after uiReady)
  if (!muteOnStartupPushed) {
    muteOnStartupPushed = true;
    webView->evaluateJavascript(
        "if(window.setMuteOnStartupState){window.setMuteOnStartupState(" +
        juce::String(audioProcessor.muteOnStartup.load() ? "true" : "false") +
        ");}");
  }

  // Push saved settings state (once, on first frame after uiReady)
  // Syncs maxLoopSeconds, maxLayerCount, and inputMuted with the UI buttons
  if (!settingsSyncPushed) {
    settingsSyncPushed = true;
    int maxLen = static_cast<int>(audioProcessor.getMaxLoopLength());
    int maxLayers = audioProcessor.getMaxLayerCount();
    bool monitorMuted = audioProcessor.inputMuted.load();
    webView->evaluateJavascript(
        "if(window.syncSettings){window.syncSettings(" +
        juce::String(maxLen) + "," + juce::String(maxLayers) + "," +
        juce::String(monitorMuted ? "true" : "false") + ");}");
  }

  // Push Bluetooth Classic MIDI connection status to JS
  {
    auto btStatus = BluetoothClassicMidi::getLastCallbackStatus();
    auto btMac = BluetoothClassicMidi::getLastCallbackMac();
    webView->evaluateJavascript(
        "if(window.updateBtClassicStatus){window.updateBtClassicStatus(" +
        jsQuoted(btStatus) + "," + jsQuoted(btMac) + ");}");

    // Persist last connected device MAC to PropertiesFile (Requirement 7.1)
    if (btStatus == "connected" && btMac.isNotEmpty() &&
        btMac != btClassicLastPersistedMac) {
      btClassicLastPersistedMac = btMac;
      btClassicLastDeviceMac = btMac;
      juce::PropertiesFile props(makeSettingsOptions());
      props.setValue("btClassicLastDevice", btMac);
      props.save();
      DBG("OrbitLooper: btClassicLastDevice persisted = " + btMac);
    }
  }

  // Push stored BT Classic last device MAC to JS (once, on first frame after
  // uiReady). Seeds localStorage so updateBtClassicDevices can highlight the
  // device (Requirement 7.2). Does NOT auto-connect (Requirement 7.3).
  if (!btClassicLastDevicePushed) {
    btClassicLastDevicePushed = true;
    if (btClassicLastDeviceMac.isNotEmpty()) {
      webView->evaluateJavascript(
          "try{localStorage.setItem('btClassicLastDevice'," +
          jsQuoted(btClassicLastDeviceMac) + ");}catch(e){}");
      DBG("OrbitLooper: btClassicLastDevice pushed to JS = " +
          btClassicLastDeviceMac);
    }
  }

  // One-shot: on first launch, set audio buffer to lowest available size.
  // Runs from timer because audio device isn't ready during constructor.
  if (!bufferOptimizeDone) {
    if (auto* holder = juce::StandalonePluginHolder::getInstance()) {
      auto& dm = holder->deviceManager;
      if (auto* device = dm.getCurrentAudioDevice()) {
        bufferOptimizeDone = true;
        juce::PropertiesFile bProps(makeSettingsOptions());
        if (!bProps.getBoolValue("bufferOptimized", false)) {
          auto bufferSizes = device->getAvailableBufferSizes();
          auto currentSetup = dm.getAudioDeviceSetup();
          if (bufferSizes.size() > 0) {
            int minBuffer = bufferSizes[0];
            if (currentSetup.bufferSize > minBuffer) {
              currentSetup.bufferSize = minBuffer;
              dm.setAudioDeviceSetup(currentSetup, true);
              DBG("OrbitLooper: first launch - set buffer to minimum: " +
                  juce::String(minBuffer));
            }
          }
          bProps.setValue("bufferOptimized", true);
          bProps.save();
        }
      }
    }
  }

  // Periodic state save: Android can kill the app without calling destructors
  // when swiped away or backgrounded. Save plugin + audio device state every ~5s.
  {
    auto now = static_cast<int64_t>(juce::Time::getMillisecondCounter());
    if (now - lastStateSaveMs >= STATE_SAVE_INTERVAL_MS) {
      lastStateSaveMs = now;
      if (auto* holder = juce::StandalonePluginHolder::getInstance()) {
        holder->savePluginState();
        holder->saveAudioDeviceState();
      }
    }
  }
}
#endif

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
