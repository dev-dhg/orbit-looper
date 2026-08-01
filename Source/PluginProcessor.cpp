#include "PluginProcessor.h"
#include "PluginEditor.h"

#if JUCE_ANDROID
#include "BluetoothClassicMidi.h"
#endif

//==============================================================================
OrbitLooperAudioProcessor::OrbitLooperAudioProcessor()
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, juce::Identifier("OrbitLooper"),
            createParameterLayout()) {
  // Initialize all MIDI CC mappings to unassigned
  for (auto &cc : midiCCMap)
    cc.store(CC_UNASSIGNED);

  // Loop-mode changes alter the spare-layer capacity target (Dynamic needs
  // the full Global Max) — re-run the message-thread allocator.
  apvts.addParameterListener("loop_mode", this);
}

OrbitLooperAudioProcessor::~OrbitLooperAudioProcessor() {
  apvts.removeParameterListener("loop_mode", this);
  cancelPendingUpdate();
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
OrbitLooperAudioProcessor::createParameterLayout() {
  juce::AudioProcessorValueTreeState::ParameterLayout layout;

  // NOTE: the former "max_loop_length" parameter was removed — it had no
  // rendered UI control. The Global Max Length (Settings modal →
  // maxLoopSeconds) is the single loop-length authority.

  // Loop Level - controls how much of the previous loop is kept during overdub
  // 100% = full overdub (additive), lower = loop decays over time
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"loop_level", 1}, "Loop Level",
      juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 95.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          [](float value, int) {
            return juce::String(static_cast<int>(value)) + "%";
          })));

  // Input Gain - boost input signal before recording (-60 to +12 dB)
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"input_gain", 1}, "Input Gain",
      juce::NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          [](float value, int) {
            return (value <= -59.9f) ? "-inf dB"
                                     : juce::String(value, 1) + " dB";
          })));

  // Input Pan - balance input signal (-1.0 to +1.0)
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"input_pan", 1}, "Input Pan",
      juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          [](float value, int) {
            if (std::abs(value) < 0.01f)
              return juce::String("C");
            return (value < 0.0f) ? "L" + juce::String(std::abs(value * 100), 0)
                                  : "R" + juce::String(value * 100, 0);
          })));

  // Output Gain - boost output signal after loop playback (-60 to +12 dB)
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"output_gain", 1}, "Output Gain",
      juce::NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          [](float value, int) {
            return (value <= -59.9f) ? "-inf dB"
                                     : juce::String(value, 1) + " dB";
          })));

  // Output Pan - balance output signal (-1.0 to +1.0)
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"output_pan", 1}, "Output Pan",
      juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          [](float value, int) {
            if (std::abs(value) < 0.01f)
              return juce::String("C");
            return (value < 0.0f) ? "L" + juce::String(std::abs(value * 100), 0)
                                  : "R" + juce::String(value * 100, 0);
          })));

  // Loop Mode - Dynamic, Classic, Bars
  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"loop_mode", 1}, "Loop Mode",
      juce::StringArray{"Classic", "Bars", "Dynamic"}, 0));

  return layout;
}

//==============================================================================
const std::array<const char *, OrbitLooperAudioProcessor::NUM_MIDI_ACTIONS> &
OrbitLooperAudioProcessor::getMidiActionNames() {
  static const std::array<const char *, NUM_MIDI_ACTIONS> names{
      "Record",        "Stop",        "Clear",       "Undo",
      "Footswitch",    "Overdub",     "BarMode",     "Click",
      "PreCount",      "ArmOverdub",  "PlayClick",   "Play",
      "Monitor",       "LoopModeCycle", "ClassicMode", "BeatsMode",
      "DynamicMode",   "PanInputLeft", "PanInputCenter", "PanInputRight"};
  return names;
}

//==============================================================================
const juce::String OrbitLooperAudioProcessor::getName() const {
  return JucePlugin_Name;
}

bool OrbitLooperAudioProcessor::acceptsMidi() const { return true; }
bool OrbitLooperAudioProcessor::producesMidi() const { return false; }
bool OrbitLooperAudioProcessor::isMidiEffect() const { return false; }
double OrbitLooperAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int OrbitLooperAudioProcessor::getNumPrograms() { return 1; }
int OrbitLooperAudioProcessor::getCurrentProgram() { return 0; }
void OrbitLooperAudioProcessor::setCurrentProgram(int) {}
const juce::String OrbitLooperAudioProcessor::getProgramName(int) { return {}; }
void OrbitLooperAudioProcessor::changeProgramName(int, const juce::String &) {}

//==============================================================================
void OrbitLooperAudioProcessor::prepareToPlay(double sampleRate,
                                              int /*samplesPerBlock*/) {
  const bool sameRate = std::abs(sampleRate - currentSampleRate) < 0.5;
  currentSampleRate = sampleRate;
  const int tapeSamples = globalMaxSamples();

  // Preserve the recorded loop across host re-prepares (DAW play/stop):
  // only when the sample rate and tape size are unchanged and the loop is
  // finalized. Mid-recording/overdubbing re-prepares still do a full reset.
  const bool loopFinalized = (currentState == LooperState::PLAYING ||
                              currentState == LooperState::STOPPED) &&
                             masterLoopLength > 0 && numLayers > 0;

  if (sameRate && loopFinalized &&
      layers[0].capacity.load() == tapeSamples) {
    readPosition = readPosition % masterLoopLength;
    activeLayerIdx = -1;
    footswitchAutoOverdub = false;
    overdubArmed = false;
    overdubArmedForUI.store(false);
  } else {
    // Lazy layer storage (D1): allocate ONLY the base layer at the Global Max
    // ("tape length"). Overdub layers are allocated on demand by the
    // message-thread allocator (handleAsyncUpdate), sized to the actual loop.
    layers[0].capacity.store(0);
    layers[0].bufferL.assign(static_cast<size_t>(tapeSamples), 0.0f);
    layers[0].bufferR.assign(static_cast<size_t>(tapeSamples), 0.0f);
    layers[0].length = 0;
    layers[0].needsCleanup.store(false);
    layers[0].capacity.store(tapeSamples);

    for (size_t i = 1; i < layers.size(); ++i) {
      auto &layer = layers[i];
      layer.capacity.store(0);
      std::vector<float>().swap(layer.bufferL);
      std::vector<float>().swap(layer.bufferR);
      layer.length = 0;
      layer.needsCleanup.store(false);
    }

    // Reset state
    numLayers = 0;
    layersInUseAtomic.store(0);
    masterLoopLength = 0;
    activeLayerIdx = -1;
    footswitchAutoOverdub = false;
    writePosition = 0;
    readPosition = 0;
    currentState = LooperState::EMPTY;
    looperState.store(static_cast<int>(LooperState::EMPTY));
    hasUndoLayer.store(false);
  }

  // Common resets (both preserve and full-reset paths):
  // Clear any pending commands (fixes multi-hit bug on standalone startup)
  pendingRecord.store(false);
  pendingStop.store(false);
  pendingClear.store(false);
  pendingUndo.store(false);
  pendingFootswitchDown.store(false);
  pendingFootswitchUp.store(false);
  pendingOverdub.store(false);
  pendingPlay.store(false);
  pendingBarModeToggle.store(false);

  pendingClickToggle.store(false);
  pendingPreCountToggle.store(false);
  uiFootswitchHeld = false;

  // Reset metronome DSP state
  pendingMetroStart.store(false);
  pendingMetroStop.store(false);
  clickActive = false;
  clickSamplesRemaining = 0;
  clickPhase = 0.0f;
  metroBeat = 0;
  metroTotalBeats = 0;
  metroTotalBeatCount.store(0);
  metroCurrentBeatForUI.store(-1);
  samplesSinceLastBeat = 0.0;
  samplesPerBeat = 0.0;

  // Reset footswitch timing
  footswitchHeld = false;
  uiFootswitchHeld = false;
  footswitchPressStart = 0;
  lastFootswitchPressTime = 0;
  totalSamplesProcessed = 0;
  longPressTriggered = false;
}

void OrbitLooperAudioProcessor::releaseResources() {
  // Free all layer buffers
  for (auto &layer : layers) {
    layer.capacity.store(0);
    std::vector<float>().swap(layer.bufferL);
    std::vector<float>().swap(layer.bufferR);
    layer.length = 0;
    layer.needsCleanup.store(false);
  }
  numLayers = 0;
  layersInUseAtomic.store(0);
  masterLoopLength = 0;
}

//==============================================================================
// Lazy layer storage — message-thread allocator (see PluginProcessor.h)
//==============================================================================

int OrbitLooperAudioProcessor::globalMaxSamples() const {
  const float limitSec = std::min(maxLoopSeconds.load(), SAFETY_LIMIT_SECONDS);
  return static_cast<int>(currentSampleRate * limitSec);
}

// Capacity target for the next overdub spare layer.
int OrbitLooperAudioProcessor::requiredLayerCapacitySamples() const {
  const int cap = globalMaxSamples();

  // Dynamic mode: overdubs may extend up to the Global Max.
  if (loopMode.load() == static_cast<int>(LoopMode::Dynamic))
    return cap;

  // Classic/Bars: overdubs are locked to the master loop length.
  // loopLengthSeconds is the audio thread's published master length; add a
  // small headroom for float rounding.
  const float masterSec = loopLengthSeconds.load();
  if (masterSec <= 0.0f)
    return cap; // No loop yet — size for the worst case

  const int needed =
      static_cast<int>(masterSec * currentSampleRate) + 256;
  return std::min(needed, cap);
}

// Resize/zero one layer's storage off the audio thread. Uses a
// publish/re-validate handshake with startOverdubLayer's claim:
// capacity=0 blocks new claims; if the layer got claimed first, back off.
void OrbitLooperAudioProcessor::ensureLayerStorage(int layerIdx,
                                                   int targetSamples) {
  auto &layer = layers[static_cast<size_t>(layerIdx)];
  const int oldCapacity = layer.capacity.load();
  const bool dirty = layer.needsCleanup.load();

  if (oldCapacity == targetSamples && !dirty)
    return;

  layer.capacity.store(0); // Block claims while we touch the storage

  if (layersInUseAtomic.load() > layerIdx) {
    // The audio thread claimed this layer in the meantime — hands off.
    layer.capacity.store(oldCapacity);
    return;
  }

  if (targetSamples <= 0) {
    std::vector<float>().swap(layer.bufferL);
    std::vector<float>().swap(layer.bufferR);
  } else {
    layer.bufferL.assign(static_cast<size_t>(targetSamples), 0.0f);
    layer.bufferR.assign(static_cast<size_t>(targetSamples), 0.0f);
    if (targetSamples < oldCapacity) {
      layer.bufferL.shrink_to_fit();
      layer.bufferR.shrink_to_fit();
    }
  }

  layer.length = 0; // Safe: not claimed
  layer.needsCleanup.store(false);
  layer.capacity.store(targetSamples);
}

void OrbitLooperAudioProcessor::handleAsyncUpdate() {
  const int state = looperState.load();
  const int inUse =
      std::max(layersInUseAtomic.load(),
               state == static_cast<int>(LooperState::RECORDING) ? 1 : 0);

  const int tapeSamples = globalMaxSamples();

  // Retired layers: zero the immediate spare slot for reuse, free the rest.
  for (int i = std::max(inUse, 1); i < MAX_LAYERS; ++i) {
    auto &layer = layers[static_cast<size_t>(i)];
    if (layer.needsCleanup.load())
      ensureLayerStorage(i, i == inUse ? requiredLayerCapacitySamples() : 0);
  }

  if (state == static_cast<int>(LooperState::EMPTY) && inUse == 0) {
    // Base layer at full tape length, ready for instant record
    ensureLayerStorage(0, tapeSamples);
    // Free leftover overdub storage from a previous loop
    for (int i = 1; i < MAX_LAYERS; ++i)
      ensureLayerStorage(i, 0);
  } else if (inUse >= 1 && inUse < maxLayerCount.load()) {
    // Keep exactly one spare overdub slot allocated ahead of use
    ensureLayerStorage(inUse, requiredLayerCapacitySamples());
  }
}

void OrbitLooperAudioProcessor::parameterChanged(const juce::String &id,
                                                 float /*newValue*/) {
  if (id == "loop_mode")
    triggerAsyncUpdate();
}

bool OrbitLooperAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  // Output must be Mono or Stereo
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

  // Input must be Mono, Stereo, or Disabled
  if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo() &&
      layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
    return false;

  return true;
}

//==============================================================================
// Transport controls (called from UI/message thread)
//==============================================================================

void OrbitLooperAudioProcessor::triggerRecord() { pendingRecord.store(true); }

void OrbitLooperAudioProcessor::triggerStop() { pendingStop.store(true); }

void OrbitLooperAudioProcessor::triggerClear() { pendingClear.store(true); }

void OrbitLooperAudioProcessor::triggerUndo() { pendingUndo.store(true); }

void OrbitLooperAudioProcessor::triggerFootswitch() {
  // Footswitch single press: same state cycling as Record
  // (This is called for UI button click - not MIDI CC or keyboard gesture)
  pendingRecord.store(true);
}

void OrbitLooperAudioProcessor::triggerFootswitchDown() {
  pendingFootswitchDown.store(true);
}

void OrbitLooperAudioProcessor::triggerFootswitchUp() {
  pendingFootswitchUp.store(true);
}

void OrbitLooperAudioProcessor::triggerOverdub() { pendingOverdub.store(true); }

void OrbitLooperAudioProcessor::triggerPlay() { pendingPlay.store(true); }

void OrbitLooperAudioProcessor::triggerMonitorToggle() {
  inputMuted.store(!inputMuted.load());
}

void OrbitLooperAudioProcessor::setMaxLoopLength(float seconds) {
  maxLoopSeconds.store(std::clamp(seconds, 1.0f, SAFETY_LIMIT_SECONDS));
  triggerAsyncUpdate(); // Re-size storage on the message thread
}

float OrbitLooperAudioProcessor::getMaxLoopLength() const {
  return maxLoopSeconds.load();
}

void OrbitLooperAudioProcessor::setMaxLayerCount(int count) {
  maxLayerCount.store(std::clamp(count, 1, MAX_LAYERS));
}

int OrbitLooperAudioProcessor::getMaxLayerCount() const {
  return maxLayerCount.load();
}

//==============================================================================
// Metronome DSP
//==============================================================================

void OrbitLooperAudioProcessor::startMetronomeDSP() {
  // Reset counters immediately on the UI thread so the timer callback
  // reads 0 instead of stale values from a previous metronome run.
  metroTotalBeatCount.store(0);
  metroCurrentBeatForUI.store(0);
  pendingMetroStart.store(true);
}

void OrbitLooperAudioProcessor::stopMetronomeDSP() {
  pendingMetroStop.store(true);
}

void OrbitLooperAudioProcessor::setMetronomeBPM(float bpmVal) {
  metroBPMSetting.store(std::clamp(bpmVal, 30.0f, 300.0f));
}

void OrbitLooperAudioProcessor::setMetronomeBeatsPerBar(int bpb) {
  metroBeatsPerBarSetting.store(std::clamp(bpb, 1, 16));
}

void OrbitLooperAudioProcessor::setMetronomeNumBars(int bars) {
  metroNumBarsSetting.store(std::clamp(bars, 1, 999));
}

void OrbitLooperAudioProcessor::setMetronomePatterns(uint16_t accentBits,
                                                     uint16_t regularBits) {
  metroAccentBits.store(accentBits);
  metroRegularBits.store(regularBits);
}

void OrbitLooperAudioProcessor::setMetronomeAudible(bool audible) {
  metroAudible.store(audible);
}

void OrbitLooperAudioProcessor::processMetronome(float *outL, float *outR,
                                                 int numChannels,
                                                 int numSamples) {
  // Handle start/stop requests from UI thread
  if (pendingMetroStart.exchange(false)) {
    clickActive = true;
    float bpmVal = metroBPMSetting.load();
    samplesPerBeat = currentSampleRate * 60.0 / static_cast<double>(bpmVal);
    samplesSinceLastBeat = samplesPerBeat; // Trigger first beat immediately
    metroBeat = 0;
    metroTotalBeats = 0;
    metroTotalBeatCount.store(0);
    clickSamplesRemaining = 0;
  }

  if (pendingMetroStop.exchange(false)) {
    clickActive = false;
    clickSamplesRemaining = 0;
    metroCurrentBeatForUI.store(-1);
  }

  if (!clickActive)
    return;

  // Read current settings (can change mid-block — that's fine)
  float bpmVal = metroBPMSetting.load();
  samplesPerBeat = currentSampleRate * 60.0 / static_cast<double>(bpmVal);
  int bpb = metroBeatsPerBarSetting.load();
  uint16_t accentBits = metroAccentBits.load();
  uint16_t regularBits = metroRegularBits.load();
  bool isAudible = metroAudible.load();

  for (int i = 0; i < numSamples; ++i) {
    samplesSinceLastBeat += 1.0;

    // Check if we've reached the next beat
    if (samplesSinceLastBeat >= samplesPerBeat) {
      samplesSinceLastBeat -= samplesPerBeat;

      int beatInBar = metroBeat % bpb;
      bool isAccent = (accentBits >> beatInBar) & 1;
      bool isRegular = (regularBits >> beatInBar) & 1;

      if (isAccent) {
        clickFreq = 1200.0f;
        // Louder accent
        clickStartGain = 0.6f;
        clickTotalSamples = static_cast<int>(currentSampleRate * 0.06); // 60ms
        clickSamplesRemaining = clickTotalSamples;
        clickPhase = 0.0f;
      } else if (isRegular) {
        clickFreq = 800.0f;
        // Softer regular beat
        clickStartGain = 0.4f;
        clickTotalSamples = static_cast<int>(currentSampleRate * 0.05); // 50ms
        clickSamplesRemaining = clickTotalSamples;
        clickPhase = 0.0f;
      }

      // Update UI feedback
      metroCurrentBeatForUI.store(beatInBar);
      metroTotalBeats++;
      metroTotalBeatCount.store(metroTotalBeats);
      metroBeat++;
    }

    // Generate click audio sample ONLY if audible
    // We still calculate envelope/phase to keep state consistent if toggled
    // mid-click, but strictly masking the output write is safer and cheaper.
    // Actually, if we want seamless toggling, we should probably process the
    // click envelope but just multiply result by 0 if !audible.

    if (clickSamplesRemaining > 0) {
      float t = 1.0f - (static_cast<float>(clickSamplesRemaining) /
                        static_cast<float>(clickTotalSamples));
      float envelope =
          clickStartGain * std::exp(-t * 6.0f); // Exponential decay

      float sample;
      if (clickFreq > 1000.0f) {
        // Triangle wave for accent click
        float phase01 = clickPhase - std::floor(clickPhase);
        sample = 2.0f * std::abs(2.0f * phase01 - 1.0f) - 1.0f;
      } else {
        // Sine wave for regular click
        sample = std::sin(clickPhase * juce::MathConstants<float>::twoPi);
      }

      sample *= envelope;

      if (isAudible) {
        outL[i] += sample;
        if (numChannels > 1 && outR != nullptr)
          outR[i] += sample;
      }

      clickPhase += clickFreq / static_cast<float>(currentSampleRate);
      if (clickPhase > 1.0f)
        clickPhase -= std::floor(clickPhase);

      clickSamplesRemaining--;
    }
  }
}

//==============================================================================
// MIDI CC Mapping
//==============================================================================

int OrbitLooperAudioProcessor::getMidiCC(MidiAction action) const {
  const int idx = static_cast<int>(action);
  if (idx >= 0 && idx < NUM_MIDI_ACTIONS)
    return midiCCMap[static_cast<size_t>(idx)].load();
  return CC_UNASSIGNED;
}

void OrbitLooperAudioProcessor::setMidiCC(MidiAction action, int ccNumber) {
  const int idx = static_cast<int>(action);
  if (idx >= 0 && idx < NUM_MIDI_ACTIONS) {
    // Unassign this CC from any other action first
    if (ccNumber != CC_UNASSIGNED) {
      for (int i = 0; i < NUM_MIDI_ACTIONS; ++i)
        if (i != idx && midiCCMap[static_cast<size_t>(i)].load() == ccNumber)
          midiCCMap[static_cast<size_t>(i)].store(CC_UNASSIGNED);
    }
    midiCCMap[static_cast<size_t>(idx)].store(ccNumber);
    midiStateChanged.store(true);
  }
}

void OrbitLooperAudioProcessor::clearMidiCC(MidiAction action) {
  setMidiCC(action, CC_UNASSIGNED);
}

//==============================================================================
// Keyboard Mapping
//==============================================================================

juce::String OrbitLooperAudioProcessor::getKeyBinding(MidiAction action) const {
  const int idx = static_cast<int>(action);
  if (idx >= 0 && idx < NUM_KEY_ACTIONS)
    return keyBindings[idx];
  return {};
}

void OrbitLooperAudioProcessor::setKeyBinding(MidiAction action,
                                              const juce::String &keyCode) {
  const int idx = static_cast<int>(action);
  if (idx >= 0 && idx < NUM_KEY_ACTIONS) {
    // Unassign this key from any other action first
    if (keyCode.isNotEmpty()) {
      for (int i = 0; i < NUM_KEY_ACTIONS; ++i)
        if (i != idx && keyBindings[i] == keyCode)
          keyBindings[i] = {};
    }
    keyBindings[idx] = keyCode;
    keyBindingsChanged.store(true);
  }
}

void OrbitLooperAudioProcessor::clearKeyBinding(MidiAction action) {
  setKeyBinding(action, {});
}

//==============================================================================

void OrbitLooperAudioProcessor::startMidiLearn(MidiAction action) {
  midiLearnTarget.store(static_cast<int>(action));
  midiLearnActive.store(true);
  lastLearnedCC.store(CC_UNASSIGNED);
  lastLearnedAction.store(CC_UNASSIGNED);
  midiStateChanged.store(true);
  juce::Logger::writeToLog("OrbitLooper: MIDI learn started for action " +
      juce::String(static_cast<int>(action)));
}

void OrbitLooperAudioProcessor::cancelMidiLearn() {
  juce::Logger::writeToLog("OrbitLooper: MIDI learn cancelled (was active=" +
      juce::String(midiLearnActive.load() ? "Y" : "N") + ")");
  midiLearnActive.store(false);
  midiLearnTarget.store(CC_UNASSIGNED);
  midiStateChanged.store(true);
}

bool OrbitLooperAudioProcessor::isMidiLearning() const {
  return midiLearnActive.load();
}

OrbitLooperAudioProcessor::MidiAction
OrbitLooperAudioProcessor::getMidiLearnTarget() const {
  return static_cast<MidiAction>(midiLearnTarget.load());
}

//==============================================================================
// MIDI message processing (called on audio thread)
//==============================================================================

void OrbitLooperAudioProcessor::processMidiMessages(
    const juce::MidiBuffer &midi) {
  for (const auto metadata : midi) {
    const auto msg = metadata.getMessage();

#if JUCE_ANDROID
    // Diagnostic: log every message type when MIDI learn is active.
    if (midiLearnActive.load()) {
      juce::Logger::writeToLog("OrbitLooper MIDI-LEARN: ch=" + juce::String(msg.getChannel()) +
          " isCC=" + juce::String(msg.isController() ? "Y" : "N") +
          " isNote=" + juce::String(msg.isNoteOnOrOff() ? "Y" : "N") +
          (msg.isController()
               ? (" cc=" + juce::String(msg.getControllerNumber()) +
                  " val=" + juce::String(msg.getControllerValue()) +
                  " threshold=" + juce::String(CC_TRIGGER_THRESHOLD))
               : juce::String("")));
    }
#endif

    if (!msg.isController())
      continue;

    // Track last CC activity for BLE connection staleness detection
    lastMidiActivityMs.store(juce::Time::currentTimeMillis());

    const int cc = msg.getControllerNumber();
    const int value = msg.getControllerValue();

    // ── MIDI Learn mode ──
    if (midiLearnActive.load()) {
      // Any CC message completes the learn — value doesn't matter,
      // we only need the CC number. This fixes BLE MIDI controllers
      // like AIRSTEP Lite that send val=0 on release.
      const int target = midiLearnTarget.load();
      if (target >= 0 && target < NUM_MIDI_ACTIONS) {
        setMidiCC(static_cast<MidiAction>(target), cc);
        lastLearnedCC.store(cc);
        lastLearnedAction.store(target);
      }
      midiLearnActive.store(false);
      midiLearnTarget.store(CC_UNASSIGNED);
      midiStateChanged.store(true);
      continue; // Don't trigger actions while learning
    }

    // ── Normal CC → action mapping (edge-triggered: rising edge only) ──
    for (int i = 0; i < NUM_MIDI_ACTIONS; ++i) {
      if (midiCCMap[static_cast<size_t>(i)].load() == cc) {
        const bool isHigh = (value >= CC_TRIGGER_THRESHOLD);

        // Footswitch: Ditto-style — act IMMEDIATELY on press, long press while
        // held
        if (static_cast<MidiAction>(i) == MidiAction::Footswitch) {
          if (isHigh && !ccWasHigh[static_cast<size_t>(i)]) {
            // Rising edge - CC pressed: act immediately
            juce::Logger::writeToLog("OrbitLooper TRIGGER: Footswitch cc=" +
                juce::String(cc) + " val=" + juce::String(value));
            int64_t doublePressWindow = static_cast<int64_t>(
                (DOUBLE_PRESS_MS / 1000.0) * currentSampleRate);
            if (lastFootswitchPressTime > 0 &&
                (totalSamplesProcessed - lastFootswitchPressTime) <
                    doublePressWindow) {
              // Second press within window → double press (stop)
              handleFootswitchDoublePress();
            } else {
              // Immediate single press action
              handleFootswitchSinglePress();
            }
            lastFootswitchPressTime = totalSamplesProcessed;
            footswitchHeld = true;
            footswitchPressStart = totalSamplesProcessed;
            longPressTriggered = false;
          } else if (!isHigh && ccWasHigh[static_cast<size_t>(i)]) {
            // Falling edge - just reset held state
            footswitchHeld = false;
          }
          ccWasHigh[static_cast<size_t>(i)] = isHigh;
          continue; // Don't fall through to normal edge trigger
        }

        // All other actions: simple rising-edge trigger
        if (isHigh && !ccWasHigh[static_cast<size_t>(i)]) {
          juce::Logger::writeToLog("OrbitLooper TRIGGER: action=" +
              juce::String(i) + " cc=" + juce::String(cc) + " val=" + juce::String(value));
          switch (static_cast<MidiAction>(i)) {
          case MidiAction::Record:
            pendingRecord.store(true);
            break;
          case MidiAction::Stop:
            pendingStop.store(true);
            break;
          case MidiAction::Clear:
            pendingClear.store(true);
            break;
          case MidiAction::Undo:
            pendingUndo.store(true);
            break;
          case MidiAction::Overdub:
            pendingOverdub.store(true);
            break;
          case MidiAction::BarMode:
            pendingBarModeToggle.store(true);
            break;
          case MidiAction::Click:
            pendingClickToggle.store(true);
            break;
          case MidiAction::PreCount:
            pendingPreCountToggle.store(true);
            break;
          case MidiAction::ArmOverdub:
            pendingOverdubArmToggle.store(true);
            break;
          case MidiAction::PlayClick:
            pendingPlayClickToggle.store(true);
            break;
          case MidiAction::Play:
            pendingPlay.store(true);
            break;
          case MidiAction::Monitor:
            pendingMonitorToggle.store(true);
            break;
          case MidiAction::LoopModeCycle:
            pendingLoopModeCycle.store(true);
            break;
          case MidiAction::ClassicMode:
            pendingSetClassicMode.store(true);
            break;
          case MidiAction::BeatsMode:
            pendingSetBeatsMode.store(true);
            break;
          case MidiAction::DynamicMode:
            pendingSetDynamicMode.store(true);
            break;
          case MidiAction::PanInputLeft:
            pendingPanInputLeft.store(true);
            break;
          case MidiAction::PanInputCenter:
            pendingPanInputCenter.store(true);
            break;
          case MidiAction::PanInputRight:
            pendingPanInputRight.store(true);
            break;
          case MidiAction::Footswitch:
          case MidiAction::NUM_ACTIONS:
            break;
          }
        }
        ccWasHigh[static_cast<size_t>(i)] = isHigh;
      }
    }
  }
}

void OrbitLooperAudioProcessor::resetLooperToEmpty() {
  // Base layer needs no zeroing: recording clear-ahead + exact-length
  // finalize guarantee stale samples are never read. Overdub slots are
  // retired here (capacity 0 blocks reuse) and zeroed/freed by the
  // message-thread allocator before they can be claimed again.
  layers[0].length = 0;
  for (size_t i = 1; i < layers.size(); ++i) {
    auto &layer = layers[i];
    layer.length = 0;
    if (layer.capacity.load() > 0) {
      layer.capacity.store(0);
      layer.needsCleanup.store(true);
    }
  }
  numLayers = 0;
  layersInUseAtomic.store(0);
  masterLoopLength = 0;
  activeLayerIdx = -1;
  footswitchAutoOverdub = false;
  overdubArmed = false;
  overdubArmedForUI.store(false);
  writePosition = 0;
  readPosition = 0;
  currentState = LooperState::EMPTY;
  looperState.store(static_cast<int>(LooperState::EMPTY));
  loopLengthSeconds.store(0.0f);
  recordBasisSeconds.store(0.0f);
  hasUndoLayer.store(false);
  triggerAsyncUpdate();
}

// Finalize the base recording (layer 0) at the given length and make the
// loop playable.
void OrbitLooperAudioProcessor::finishBaseLayer(int lengthSamples) {
  layers[0].length = lengthSamples;
  numLayers = 1;
  layersInUseAtomic.store(1);
  masterLoopLength = lengthSamples;
  loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                          static_cast<float>(currentSampleRate));
  readPosition = 0;
  activeLayerIdx = -1;
  hasUndoLayer.store(false);
  triggerAsyncUpdate(); // Size the first overdub spare to the new master
}

// Finalize the layer currently being overdubbed and recalculate the master
// loop. In Dynamic mode an overdub may have extended past the master loop;
// in Classic/Bars the layer length is locked to the master loop.
void OrbitLooperAudioProcessor::finalizeOverdubLayer() {
  if (activeLayerIdx >= 0 && activeLayerIdx < MAX_LAYERS) {
    auto &layer = layers[static_cast<size_t>(activeLayerIdx)];

    if (loopMode.load() == static_cast<int>(LoopMode::Dynamic)) {
      layer.length =
          (readPosition > masterLoopLength) ? readPosition : masterLoopLength;

      masterLoopLength = 0;
      for (int i = 0; i < numLayers; ++i)
        masterLoopLength =
            std::max(masterLoopLength, layers[static_cast<size_t>(i)].length);

      loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                              static_cast<float>(currentSampleRate));
    } else {
      layer.length = masterLoopLength;
    }

    if (masterLoopLength > 0)
      readPosition = readPosition % masterLoopLength;
  }
  activeLayerIdx = -1;
  footswitchAutoOverdub = false;
  hasUndoLayer.store(numLayers > 1);
}

// Begin a new overdub layer writing from fromPosition. Returns false when
// the layer cap is reached, no loop exists, or the spare layer's storage is
// not ready yet (caller state is unchanged).
bool OrbitLooperAudioProcessor::startOverdubLayer(int fromPosition,
                                                  bool viaFootswitch) {
  if (numLayers >= maxLayerCount.load() || masterLoopLength <= 0)
    return false;

  auto &newLayer = layers[static_cast<size_t>(numLayers)];

  // Claim gate: the spare must have message-thread-allocated storage that
  // covers the master loop, and must not be awaiting cleanup.
  if (newLayer.capacity.load() < masterLoopLength ||
      newLayer.needsCleanup.load())
    return false;

  // Publish the claim, then re-validate: the allocator retires a layer by
  // zeroing its capacity BEFORE touching storage, and backs off when it
  // sees our claim — one side always yields (see ensureLayerStorage).
  activeLayerIdx = numLayers;
  numLayers++;
  layersInUseAtomic.store(numLayers);
  const int layerCap = newLayer.capacity.load();
  if (layerCap < masterLoopLength) {
    numLayers--;
    layersInUseAtomic.store(numLayers);
    activeLayerIdx = -1;
    return false;
  }

  // Clear the layer prefix up to the start point (at least a 1024-sample
  // head start) so playback modulo-reads never hit stale data.
  const int prefixSamples = std::min(std::max(fromPosition, 1024), layerCap);
  if (prefixSamples > 0) {
    juce::FloatVectorOperations::clear(newLayer.bufferL.data(), prefixSamples);
    juce::FloatVectorOperations::clear(newLayer.bufferR.data(), prefixSamples);
  }

  newLayer.length = 0;
  // Footswitch-initiated overdubs (with prior overdubs present) get the
  // 2-layer undo treatment — see pendingUndo.
  footswitchAutoOverdub = viaFootswitch && (numLayers > 2);
  hasUndoLayer.store(true);
  readPosition = fromPosition;
  writePosition = fromPosition;
  currentState = LooperState::OVERDUBBING;
  triggerAsyncUpdate(); // Allocate the next spare ahead of use
  return true;
}

// Render samples [fromSample, numSamples) in OVERDUBBING state. Used by the
// OVERDUBBING case (from 0) and by the arm-boundary engagement in PLAYING
// (mid-block continuation, so no dry input leaks through unprocessed).
void OrbitLooperAudioProcessor::renderOverdubSegment(
    const float *inL, const float *inR, float *outL, float *outR,
    int fromSample, int numSamples, bool isInputMuted, float inPanL,
    float inPanR, float loopLevel, float &peakOut) {
  if (fromSample >= numSamples)
    return;

  if (masterLoopLength <= 0 || activeLayerIdx < 0 ||
      activeLayerIdx >= MAX_LAYERS) {
    for (int i = fromSample; i < numSamples; ++i) {
      outL[i] = isInputMuted ? 0.0f : inL[i];
      outR[i] = isInputMuted ? 0.0f : inR[i];
    }
    return;
  }

  auto &activeLayer = layers[static_cast<size_t>(activeLayerIdx)];
  const int activeCap = activeLayer.capacity.load();
  const int segmentSamples = numSamples - fromSample;

  // Clear ahead to avoid stale data
  const bool nonDynamicMode =
      (loopMode.load() != static_cast<int>(LoopMode::Dynamic));
  int clearStartIdx =
      nonDynamicMode ? (readPosition % masterLoopLength) : readPosition;
  int clearEndIdx = std::min(clearStartIdx + segmentSamples + 1024, activeCap);
  if (clearStartIdx < clearEndIdx) {
    juce::FloatVectorOperations::clear(activeLayer.bufferL.data() +
                                           clearStartIdx,
                                       clearEndIdx - clearStartIdx);
    juce::FloatVectorOperations::clear(activeLayer.bufferR.data() +
                                           clearStartIdx,
                                       clearEndIdx - clearStartIdx);
  }

  // Overdub: write input to new layer while summing all existing layers.
  // Classic/Bars: auto-stop when readPosition reaches masterLoopLength.
  // Dynamic: readPosition advances linearly (can extend past the master).
  // Existing layers read at (writePos % layerLength) — shorter layers tile.
  // Positions are tracked incrementally; writePos never wraps within a pass
  // (non-dynamic auto-stops at the boundary), so the counters stay in sync
  // without per-sample modulo.
  int layerPos[MAX_LAYERS];
  {
    const int startPos =
        nonDynamicMode ? (readPosition % masterLoopLength) : readPosition;
    for (int l = 0; l < numLayers; ++l) {
      const int len = layers[static_cast<size_t>(l)].length;
      layerPos[l] = len > 0 ? startPos % len : 0;
    }
  }

  for (int i = fromSample; i < numSamples; ++i) {
    // In non-dynamic modes, use modulo for buffer write position but track
    // linear readPosition
    int writePos =
        nonDynamicMode ? (readPosition % masterLoopLength) : readPosition;

    // Sum all complete layers (not the active one being recorded)
    float sumL = 0.0f, sumR = 0.0f;
    for (int l = 0; l < numLayers; ++l) {
      const auto &layer = layers[static_cast<size_t>(l)];
      if (l != activeLayerIdx && layer.length > 0) {
        sumL += layer.bufferL[static_cast<size_t>(layerPos[l])];
        sumR += layer.bufferR[static_cast<size_t>(layerPos[l])];
      }
      if (layer.length > 0 && ++layerPos[l] >= layer.length)
        layerPos[l] = 0;
    }

    // Write input to active layer at current position
    float inMono = (inL[i] + inR[i]) * 0.5f;
    if (writePos < activeCap) {
      activeLayer.bufferL[static_cast<size_t>(writePos)] = inMono * inPanL;
      activeLayer.bufferR[static_cast<size_t>(writePos)] = inMono * inPanR;
    }

    // Output: dry input + sum of existing layers at loop level
    outL[i] = (isInputMuted ? 0.0f : inMono * inPanL) + sumL * loopLevel;
    outR[i] = (isInputMuted ? 0.0f : inMono * inPanR) + sumR * loopLevel;
    peakOut =
        std::max(peakOut, std::max(std::abs(outL[i]), std::abs(outR[i])));

    readPosition++;

    // Non-dynamic modes: auto-stop overdub at loop boundary (OVERDUB → PLAY)
    // Dynamic mode: auto-stop when this layer's storage is exhausted
    if ((nonDynamicMode && readPosition >= masterLoopLength) ||
        (!nonDynamicMode && readPosition >= activeCap)) {
      finalizeOverdubLayer();
      currentState = LooperState::PLAYING;
      looperState.store(static_cast<int>(LooperState::PLAYING));

      // Process remaining samples as PLAYING
      renderPlaybackTail(inL, inR, outL, outR, i + 1, numSamples, isInputMuted,
                         inPanL, inPanR, loopLevel, peakOut);
      break;
    }
  }

  // Keep write position in sync for state transitions
  if (currentState == LooperState::OVERDUBBING)
    writePosition = readPosition;
}

// Shared PLAYING-state record/overdub press behavior: disarm if armed, arm
// if arming is enabled, otherwise start an immediate overdub.
void OrbitLooperAudioProcessor::toggleArmOrStartOverdub(bool viaFootswitch) {
  if (overdubArmed) {
    // Already armed — toggle OFF (disarm)
    overdubArmed = false;
    overdubArmedForUI.store(false);
  } else if (overdubArmEnabled.load() && numLayers < maxLayerCount.load() &&
             masterLoopLength > 0) {
    // Arm: queue overdub for next loop boundary
    overdubArmed = true;
    overdubArmedForUI.store(true);
  } else {
    startOverdubLayer(readPosition, viaFootswitch);
  }
}

// Render samples [fromSample, numSamples) as PLAYING after an in-block
// state transition (overdub auto-stop). Advances readPosition.
void OrbitLooperAudioProcessor::renderPlaybackTail(
    const float *inL, const float *inR, float *outL, float *outR,
    int fromSample, int numSamples, bool isInputMuted, float inPanL,
    float inPanR, float loopLevel, float &peakOut) {
  // Per-layer positions tracked incrementally (no per-sample modulo)
  int layerPos[MAX_LAYERS];
  for (int l = 0; l < numLayers; ++l) {
    const int len = layers[static_cast<size_t>(l)].length;
    layerPos[l] = len > 0 ? readPosition % len : 0;
  }

  for (int j = fromSample; j < numSamples; ++j) {
    float pSumL = 0.0f, pSumR = 0.0f;
    for (int l = 0; l < numLayers; ++l) {
      const auto &layer = layers[static_cast<size_t>(l)];
      if (layer.length > 0) {
        pSumL += layer.bufferL[static_cast<size_t>(layerPos[l])];
        pSumR += layer.bufferR[static_cast<size_t>(layerPos[l])];
        if (++layerPos[l] >= layer.length)
          layerPos[l] = 0;
      }
    }
    float inMonoJ = (inL[j] + inR[j]) * 0.5f;
    outL[j] = (isInputMuted ? 0.0f : inMonoJ * inPanL) + pSumL * loopLevel;
    outR[j] = (isInputMuted ? 0.0f : inMonoJ * inPanR) + pSumR * loopLevel;
    peakOut =
        std::max(peakOut, std::max(std::abs(outL[j]), std::abs(outR[j])));
    readPosition = (readPosition + 1) % masterLoopLength;
    if (readPosition == 0) // Master wrapped — resync tiling positions
      for (int l = 0; l < numLayers; ++l)
        layerPos[l] = 0;
  }
}

void OrbitLooperAudioProcessor::processStateChanges() {
  // Sync Loop Mode parameter (set via UI)
  auto *loopModeParam = dynamic_cast<juce::AudioParameterChoice *>(
      apvts.getParameter("loop_mode"));
  if (loopModeParam) {
    loopMode.store(loopModeParam->getIndex());
  }

  // Handle Loop Mode Cycle (from MIDI trigger)
  if (pendingLoopModeCycle.exchange(false)) {
    if (loopModeParam) {
      int nextMode = (loopModeParam->getIndex() + 1) % 3;
      loopModeParam->setValueNotifyingHost(
          loopModeParam->getNormalisableRange().convertTo0to1(
              static_cast<float>(nextMode)));
    }
  }

  // Handle direct Loop Mode set (from MIDI trigger)
  if (pendingSetClassicMode.exchange(false)) {
    if (loopModeParam) {
      loopModeParam->setValueNotifyingHost(
          loopModeParam->getNormalisableRange().convertTo0to1(0.0f));
    }
  }
  if (pendingSetBeatsMode.exchange(false)) {
    if (loopModeParam) {
      loopModeParam->setValueNotifyingHost(
          loopModeParam->getNormalisableRange().convertTo0to1(1.0f));
    }
  }
  if (pendingSetDynamicMode.exchange(false)) {
    if (loopModeParam) {
      loopModeParam->setValueNotifyingHost(
          loopModeParam->getNormalisableRange().convertTo0to1(2.0f));
    }
  }

  // Handle Input Pan shortcuts (from MIDI trigger)
  {
    auto *panParam = apvts.getParameter("input_pan");
    if (pendingPanInputLeft.exchange(false) && panParam) {
      panParam->setValueNotifyingHost(
          panParam->getNormalisableRange().convertTo0to1(-1.0f));
    }
    if (pendingPanInputCenter.exchange(false) && panParam) {
      panParam->setValueNotifyingHost(
          panParam->getNormalisableRange().convertTo0to1(0.0f));
    }
    if (pendingPanInputRight.exchange(false) && panParam) {
      panParam->setValueNotifyingHost(
          panParam->getNormalisableRange().convertTo0to1(1.0f));
    }
  }

  // Handle clear first (highest priority)
  if (pendingClear.exchange(false)) {
    resetLooperToEmpty();
    clearTriggered.store(true); // Flash red ring
    return;
  }

  // Handle undo (remove last layer)
  if (pendingUndo.exchange(false)) {
    if (numLayers > 1) {
      int layersToRemove = 1;

      // If currently overdubbing via footswitch auto-overdub, remove 2 layers:
      // the incidental overdub that single-press started + the previous one
      if (currentState == LooperState::OVERDUBBING && footswitchAutoOverdub &&
          numLayers > 2)
        layersToRemove = 2;

      for (int r = 0; r < layersToRemove && numLayers > 1; ++r) {
        numLayers--;
        auto &removed = layers[static_cast<size_t>(numLayers)];
        // Retire the slot: capacity 0 blocks reuse until the message-thread
        // allocator has zeroed it (no giant memset on the audio thread).
        removed.length = 0;
        removed.capacity.store(0);
        removed.needsCleanup.store(true);
      }
      layersInUseAtomic.store(numLayers);
      triggerAsyncUpdate();

      // Recalculate master loop length
      masterLoopLength = 0;
      for (int i = 0; i < numLayers; ++i)
        masterLoopLength =
            std::max(masterLoopLength, layers[static_cast<size_t>(i)].length);

      loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                              static_cast<float>(currentSampleRate));
      if (masterLoopLength > 0)
        readPosition = readPosition % masterLoopLength;
      else
        readPosition = 0;

      activeLayerIdx = -1;
      footswitchAutoOverdub = false;
      hasUndoLayer.store(numLayers > 1);
      undoTriggered.store(true); // Flash orange ring

      // If we were overdubbing, go back to playing
      if (currentState == LooperState::OVERDUBBING) {
        currentState = LooperState::PLAYING;
        looperState.store(static_cast<int>(LooperState::PLAYING));
      }
    }
  }

  // Handle stop (also acts as Play/Stop toggle)
  if (pendingStop.exchange(false)) {
    if (currentState == LooperState::RECORDING) {
      if (writePosition == 0) {
        // Nothing recorded yet — avoid a zombie 0-length loop
        resetLooperToEmpty();
        return;
      }
      // Finish recording base layer, go to stopped
      finishBaseLayer(writePosition);
      currentState = LooperState::STOPPED;
    } else if (currentState == LooperState::OVERDUBBING) {
      finalizeOverdubLayer();
      currentState = LooperState::STOPPED;
    } else if (currentState == LooperState::PLAYING) {
      currentState = LooperState::STOPPED;
      overdubArmed = false;
      overdubArmedForUI.store(false);
    } else if (currentState == LooperState::STOPPED && masterLoopLength > 0) {
      // Toggle: if stopped and we have layers, start playing
      readPosition = 0;
      currentState = LooperState::PLAYING;
    }
    looperState.store(static_cast<int>(currentState));
    return;
  }

  // Handle record trigger (cycles states)
  if (pendingRecord.exchange(false)) {
    handleRecordTrigger();
  }

  // Handle dedicated overdub trigger
  if (pendingOverdub.exchange(false)) {
    handleOverdubTrigger();
  }

  // Handle dedicated Play trigger
  if (pendingPlay.exchange(false)) {
    if (currentState == LooperState::STOPPED && masterLoopLength > 0) {
      // Start playing
      readPosition = 0;
      currentState = LooperState::PLAYING;
      looperState.store(static_cast<int>(currentState));
    } else if (currentState == LooperState::RECORDING) {
      if (writePosition == 0) {
        // Nothing recorded yet — avoid a zombie 0-length loop
        resetLooperToEmpty();
        return;
      }
      // Finish recording and play
      finishBaseLayer(writePosition);
      currentState = LooperState::PLAYING;
      looperState.store(static_cast<int>(currentState));
    } else if (currentState == LooperState::OVERDUBBING) {
      // Finish overdub and play
      finalizeOverdubLayer();
      currentState = LooperState::PLAYING;
      looperState.store(static_cast<int>(currentState));
    }
  }
}

void OrbitLooperAudioProcessor::handleRecordTrigger() {
  switch (currentState) {
  case LooperState::EMPTY: {
    // Start recording the first loop layer. The base layer holds the full
    // "tape" (Global Max); if its storage is mid-resize (capacity 0), the
    // trigger is dropped — press again once the allocator has finished.
    const int cap0 = layers[0].capacity.load();
    if (cap0 <= 0)
      break;

    activeLayerIdx = 0;
    writePosition = 0;
    layersInUseAtomic.store(1);
    currentState = LooperState::RECORDING;

    // Fixed Length Modes: Instantly lock duration upon starting recording
    if (loopMode.load() == static_cast<int>(LoopMode::Bars)) {
      float bpmVal = metroBPMSetting.load();
      int bpb = metroBeatsPerBarSetting.load();
      int bars = metroNumBarsSetting.load();
      masterLoopLength = std::min(
          static_cast<int>((60.0f / bpmVal) * bpb * bars * currentSampleRate),
          cap0);
    } else if (loopMode.load() == static_cast<int>(LoopMode::Classic)) {
      // Classic Mode: the Global Max Length IS the tape length
      masterLoopLength = cap0;
    } else {
      masterLoopLength = 0; // Dynamic Mode: length determined by stop trigger
    }

    // Publish the ring/time scale basis for the UI
    recordBasisSeconds.store(
        static_cast<float>(masterLoopLength > 0 ? masterLoopLength : cap0) /
        static_cast<float>(currentSampleRate));
    break;
  }

  case LooperState::RECORDING: {
    if (writePosition == 0) {
      // Nothing recorded yet — back to EMPTY instead of a 0-length loop
      resetLooperToEmpty();
      return;
    }
    // Finish recording base layer, start playback
    finishBaseLayer(writePosition);
    currentState = LooperState::PLAYING;
    break;
  }

  case LooperState::PLAYING:
    // Start overdubbing on a new layer (or arm if arm-toggle enabled)
    toggleArmOrStartOverdub(false);
    break;

  case LooperState::OVERDUBBING:
    // Stop overdubbing, finalize layer, continue playback
    finalizeOverdubLayer();
    currentState = LooperState::PLAYING;
    break;

  case LooperState::STOPPED:
    // Start overdubbing from stopped state on a new layer
    startOverdubLayer(0);
    break;
  }

  looperState.store(static_cast<int>(currentState));
}

void OrbitLooperAudioProcessor::handleOverdubTrigger() {
  // Dedicated overdub: only from PLAYING, STOPPED, or OVERDUBBING
  switch (currentState) {
  case LooperState::PLAYING:
    toggleArmOrStartOverdub(false);
    break;

  case LooperState::OVERDUBBING:
    // Stop overdubbing, finalize layer, continue playback
    finalizeOverdubLayer();
    currentState = LooperState::PLAYING;
    break;

  case LooperState::STOPPED:
    // Start overdubbing from stopped state
    startOverdubLayer(0);
    break;

  case LooperState::EMPTY:
  case LooperState::RECORDING:
    // Ignore dedicated overdub trigger in these states
    break;
  }

  looperState.store(static_cast<int>(currentState));
}

//==============================================================================
// Footswitch gesture handlers
//==============================================================================

void OrbitLooperAudioProcessor::handleFootswitchSinglePress() {
  // Same cycling as Record:
  // EMPTY→RECORDING, RECORDING→PLAYING, PLAYING→OVERDUBBING,
  // OVERDUBBING→PLAYING, STOPPED→PLAYING
  switch (currentState) {
  case LooperState::STOPPED:
    // Single press from stopped = PLAY (not overdub)
    readPosition = 0;
    currentState = LooperState::PLAYING;
    looperState.store(static_cast<int>(currentState));
    break;

  case LooperState::PLAYING:
    // Footswitch into overdub: respect arm toggle. Footswitch-initiated
    // overdubs get the 2-layer undo treatment (see pendingUndo).
    toggleArmOrStartOverdub(true);
    looperState.store(static_cast<int>(currentState));
    break;

  case LooperState::EMPTY:
  case LooperState::RECORDING:
  case LooperState::OVERDUBBING:
    handleRecordTrigger();
    break;
  }
}

void OrbitLooperAudioProcessor::handleFootswitchDoublePress() {
  // Double press = STOP playback (from any state except EMPTY)
  // If held, this state transition enables "Stop & Clear" in
  // handleFootswitchLongPress.
  if (currentState == LooperState::RECORDING) {
    if (writePosition == 0) {
      // Nothing recorded yet — avoid a zombie 0-length loop
      resetLooperToEmpty();
      return;
    }
    // Finalize base layer (no auto-length push on the double-press stop path)
    finishBaseLayer(writePosition);
  } else if (currentState == LooperState::OVERDUBBING) {
    finalizeOverdubLayer();
  }
  if (currentState != LooperState::EMPTY) {
    overdubArmed = false;
    overdubArmedForUI.store(false);
    currentState = LooperState::STOPPED;
    looperState.store(static_cast<int>(LooperState::STOPPED));
  }
}

void OrbitLooperAudioProcessor::handleFootswitchLongPress() {
  // Long Press behavior depends on state:
  // - PLAYING/OVERDUBBING: Undo last layer
  // - STOPPED: Clear all (Stop & Clear) - acts as "2x Tap + Hold" if preceded
  // by double tap stop
  if ((currentState == LooperState::PLAYING ||
       currentState == LooperState::OVERDUBBING) &&
      numLayers > 1) {
    // Long press while playing or overdubbing with layers = undo last overdub
    pendingUndo.store(true);
  } else if (currentState == LooperState::STOPPED) {
    // Long press while stopped = clear all
    pendingClear.store(true);
  }
}

//==============================================================================
void OrbitLooperAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                             juce::MidiBuffer &midiMessages) {
  juce::ScopedNoDenormals noDenormals;

  // Drain Bluetooth Classic MIDI queue into the MidiBuffer (Android only).
  // No mutex, no heap allocation — lock-free pop from the SPSC ring buffer.
#if JUCE_ANDROID
  {
    MidiMessageSlot slot;
    while (gBtClassicMidiQueue.pop (slot))
    {
      midiMessages.addEvent (slot.data, static_cast<int> (slot.size), 0);
      lastMidiActivityMs.store (
          static_cast<int64_t> (juce::Time::getMillisecondCounter()),
          std::memory_order_relaxed);
    }
  }
#endif

  // Process MIDI CC messages (learn + triggers)
  processMidiMessages(midiMessages);
  midiMessages.clear(); // Don't pass MIDI through

  // Process UI footswitch down/up (keyboard/button gesture detection)
  // Ditto-style: act immediately on press-down
  if (pendingFootswitchDown.exchange(false)) {
    if (!uiFootswitchHeld) {
      // Act immediately on press
      int64_t doublePressWindow =
          static_cast<int64_t>((DOUBLE_PRESS_MS / 1000.0) * currentSampleRate);
      if (lastFootswitchPressTime > 0 &&
          (totalSamplesProcessed - lastFootswitchPressTime) <
              doublePressWindow) {
        handleFootswitchDoublePress();
      } else {
        handleFootswitchSinglePress();
      }
      lastFootswitchPressTime = totalSamplesProcessed;
      uiFootswitchHeld = true;
      footswitchPressStart = totalSamplesProcessed;
      longPressTriggered = false;
    }
  }

  if (pendingFootswitchUp.exchange(false)) {
    uiFootswitchHeld = false;
  }

  // Detect long press WHILE held (triggers action + visual feedback before
  // release)
  if ((footswitchHeld || uiFootswitchHeld) && !longPressTriggered) {
    int64_t holdDuration = totalSamplesProcessed - footswitchPressStart;
    int64_t longPressThreshold =
        static_cast<int64_t>((LONG_PRESS_MS / 1000.0) * currentSampleRate);
    if (holdDuration >= longPressThreshold) {
      longPressTriggered = true;
      handleFootswitchLongPress();
    }
  }

  auto totalNumInputChannels = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  // Clear unused output channels
  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  if (buffer.getNumSamples() == 0)
    return;

  // Process pending state changes
  processStateChanges();

  // NOTE: layer storage is never (re)allocated here — all allocation happens
  // on the message thread (handleAsyncUpdate). The audio thread only claims
  // published capacity (see startOverdubLayer).

  // Get parameters
  const float loopLevelPercent =
      apvts.getRawParameterValue("loop_level")->load();
  const float loopLevel = loopLevelPercent / 100.0f;
  const float inputGainDB = apvts.getRawParameterValue("input_gain")->load();
  const float outputGainDB = apvts.getRawParameterValue("output_gain")->load();
  const float inputPan = apvts.getRawParameterValue("input_pan")->load();
  const float outputPan = apvts.getRawParameterValue("output_pan")->load();
  const float inputGainLin = juce::Decibels::decibelsToGain(inputGainDB);
  const float outputGainLin = juce::Decibels::decibelsToGain(outputGainDB);

  const float inPanL = std::min(1.0f, 1.0f - inputPan);
  const float inPanR = std::min(1.0f, 1.0f + inputPan);
  const float outPanL = std::min(1.0f, 1.0f - outputPan);
  const float outPanR = std::min(1.0f, 1.0f + outputPan);

  const int numSamples = buffer.getNumSamples();

  // Mono-to-Stereo Input Routing (RMS-based detection):
  // WASAPI (and some other drivers) always present both input channels to the
  // callback, even when the user only has one physical instrument connected. We
  // detect this by comparing per-channel RMS: if one channel is below -96dB
  // (pure hardware noise floor) while the other has signal, we duplicate the
  // active channel to both buffer slots so downstream DSP treats it as
  // dual-mono. This runs BEFORE input gain / pan.
  if (totalNumInputChannels >= 2 && numSamples > 0) {
    float sumSq0 = 0.0f, sumSq1 = 0.0f;
    auto *ch0 = buffer.getReadPointer(0);
    auto *ch1 = buffer.getReadPointer(1);
    for (int i = 0; i < numSamples; ++i) {
      sumSq0 += ch0[i] * ch0[i];
      sumSq1 += ch1[i] * ch1[i];
    }
    float rms0 = std::sqrt(sumSq0 / static_cast<float>(numSamples));
    float rms1 = std::sqrt(sumSq1 / static_cast<float>(numSamples));

    // Threshold: -96dB RMS ≈ 0.0000158. Hardware noise on unused channels
    // measures ~0.000004 (-108dB), well below this. Even the quietest guitar
    // signal measures ~0.0001 (-80dB), well above this.
    const float silenceThreshold = 0.0000158f;

    bool ch0Silent = (rms0 < silenceThreshold);
    bool ch1Silent = (rms1 < silenceThreshold);

    if (ch0Silent && !ch1Silent)
      buffer.copyFrom(0, 0, buffer, 1, 0, numSamples); // Duplicate R → L
    else if (!ch0Silent && ch1Silent)
      buffer.copyFrom(1, 0, buffer, 0, 0, numSamples); // Duplicate L → R
    // Both have signal (true stereo) or both silent: no conversion
  }

  // Apply input gain before processing
  if (std::abs(inputGainLin - 1.0f) > 1.0e-6f) {
    for (int ch = 0; ch < totalNumInputChannels; ++ch)
      buffer.applyGain(ch, 0, numSamples, inputGainLin);
  }

  auto *inputL = buffer.getReadPointer(0);
  auto *inputR = totalNumInputChannels > 1 ? buffer.getReadPointer(1) : inputL;
  auto *outputL = buffer.getWritePointer(0);
  auto *outputR =
      totalNumOutputChannels > 1 ? buffer.getWritePointer(1) : outputL;

  // Track peak input level
  float peakIn = 0.0f;
  for (int i = 0; i < numSamples; ++i) {
    float inMono = (inputL[i] + inputR[i]) * 0.5f;
    float s = std::max(std::abs(inMono * inPanL), std::abs(inMono * inPanR));
    peakIn = std::max(peakIn, s);
  }
  inputPeakLevel.store(peakIn);

  // Process based on current state
  float peakOut = 0.0f;
  bool isInputMuted = inputMuted.load() || feedbackMuted.load();

  switch (currentState) {
  case LooperState::EMPTY:
  case LooperState::STOPPED: {
    // Pass-through (100% dry)
    for (int i = 0; i < numSamples; ++i) {
      float inMono = (inputL[i] + inputR[i]) * 0.5f;
      outputL[i] = isInputMuted ? 0.0f : inMono * inPanL;
      outputR[i] = isInputMuted ? 0.0f : inMono * inPanR;
      peakOut = std::max(peakOut,
                         std::max(std::abs(outputL[i]), std::abs(outputR[i])));
    }
    break;
  }

  case LooperState::RECORDING: {
    // Record input into base layer (layer 0), pass-through dry signal
    auto &baseLayer = layers[0];
    const int cap0 = baseLayer.capacity.load();
    // Clear ahead to avoid stale data (block-level for efficiency)
    int clearStart = writePosition;
    int clearEnd = std::min(writePosition + numSamples + 1024, cap0);
    if (clearStart < clearEnd) {
      juce::FloatVectorOperations::clear(baseLayer.bufferL.data() + clearStart,
                                         clearEnd - clearStart);
      juce::FloatVectorOperations::clear(baseLayer.bufferR.data() + clearStart,
                                         clearEnd - clearStart);
    }

    // Fixed Length Modes (Classic/Bars): auto-stop at the locked length
    const bool recNonDynamicMode =
        (loopMode.load() != static_cast<int>(LoopMode::Dynamic));

    for (int i = 0; i < numSamples; ++i) {
      float inMono = (inputL[i] + inputR[i]) * 0.5f;
      if (writePosition < cap0) {
        baseLayer.bufferL[static_cast<size_t>(writePosition)] = inMono * inPanL;
        baseLayer.bufferR[static_cast<size_t>(writePosition)] = inMono * inPanR;
        writePosition++;
      }

      // Pass-through while recording
      outputL[i] = isInputMuted ? 0.0f : inMono * inPanL;
      outputR[i] = isInputMuted ? 0.0f : inMono * inPanR;
      peakOut = std::max(peakOut,
                         std::max(std::abs(outputL[i]), std::abs(outputR[i])));

      if (recNonDynamicMode && masterLoopLength > 0 &&
          writePosition >= masterLoopLength) {
        finishBaseLayer(masterLoopLength);
        currentState = LooperState::PLAYING;
        looperState.store(static_cast<int>(LooperState::PLAYING));
        break;
      }
    }

    // If we've hit the end of the tape (Global Max), auto-stop recording
    if (currentState == LooperState::RECORDING && writePosition >= cap0) {
      finishBaseLayer(cap0);
      currentState = LooperState::PLAYING;
      looperState.store(static_cast<int>(LooperState::PLAYING));
    }

    break;
  }

  case LooperState::PLAYING: {
    if (masterLoopLength <= 0 || numLayers <= 0) {
      // Shouldn't happen, but safety fallback
      for (int i = 0; i < numSamples; ++i) {
        outputL[i] = isInputMuted ? 0.0f : inputL[i];
        outputR[i] = isInputMuted ? 0.0f : inputR[i];
      }
      break;
    }

    // Sum all layers; per-layer positions tracked incrementally so the hot
    // loop has no per-sample integer division (shorter layers tile)
    int layerPos[MAX_LAYERS];
    for (int l = 0; l < numLayers; ++l) {
      const int len = layers[static_cast<size_t>(l)].length;
      layerPos[l] = len > 0 ? readPosition % len : 0;
    }

    for (int i = 0; i < numSamples; ++i) {
      float sumL = 0.0f, sumR = 0.0f;
      for (int l = 0; l < numLayers; ++l) {
        const auto &layer = layers[static_cast<size_t>(l)];
        if (layer.length > 0) {
          sumL += layer.bufferL[static_cast<size_t>(layerPos[l])];
          sumR += layer.bufferR[static_cast<size_t>(layerPos[l])];
          if (++layerPos[l] >= layer.length)
            layerPos[l] = 0;
        }
      }

      float inMono = (inputL[i] + inputR[i]) * 0.5f;
      outputL[i] = (isInputMuted ? 0.0f : inMono * inPanL) + sumL * loopLevel;
      outputR[i] = (isInputMuted ? 0.0f : inMono * inPanR) + sumR * loopLevel;
      peakOut = std::max(peakOut,
                         std::max(std::abs(outputL[i]), std::abs(outputR[i])));

      int prevPos = readPosition;
      readPosition = (readPosition + 1) % masterLoopLength;

      if (readPosition < prevPos) {
        // Master loop wrapped to 0 — resync tiling positions
        for (int l = 0; l < numLayers; ++l)
          layerPos[l] = 0;

        // Overdub Arm: engage at the loop boundary
        if (overdubArmed) {
          overdubArmed = false;
          overdubArmedForUI.store(false);
          if (startOverdubLayer(readPosition)) {
            looperState.store(static_cast<int>(LooperState::OVERDUBBING));
            // Continue the remainder of the block in OVERDUBBING so no dry
            // input leaks through unprocessed at the boundary.
            renderOverdubSegment(inputL, inputR, outputL, outputR, i + 1,
                                 numSamples, isInputMuted, inPanL, inPanR,
                                 loopLevel, peakOut);
            break;
          }
        }
      }
    }

    break;
  }

  case LooperState::OVERDUBBING: {
    renderOverdubSegment(inputL, inputR, outputL, outputR, 0, numSamples,
                         isInputMuted, inPanL, inPanR, loopLevel, peakOut);
    break;
  }
  }

  // Apply output gain and pan after processing
  if (std::abs(outputGainLin - 1.0f) > 1.0e-6f ||
      std::abs(outPanL - 1.0f) > 1.0e-6f ||
      std::abs(outPanR - 1.0f) > 1.0e-6f) {
    for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
      float panGain = (ch == 0) ? outPanL : outPanR;
      buffer.applyGain(ch, 0, numSamples, outputGainLin * panGain);
    }
    // Recalculate output peak after gain and pan
    peakOut = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      peakOut = std::max(peakOut, std::abs(outputL[i]));
      if (totalNumOutputChannels > 1)
        peakOut = std::max(peakOut, std::abs(outputR[i]));
    }
  }

  outputPeakLevel.store(peakOut);

  // Metronome click synthesis — mixed into output AFTER gain and peak metering
  // so the click is unaffected by output gain and doesn't influence level
  // meters. Click is output-only: never recorded into the loop (loop records
  // from inputL/inputR).
  processMetronome(outputL, outputR, totalNumOutputChannels, numSamples);

  // Increment total sample counter (for footswitch timing)
  totalSamplesProcessed += numSamples;

  // Update playback position (0.0 to 1.0) for UI
  if (currentState == LooperState::OVERDUBBING && numLayers > 0 &&
      layers[0].length > 0) {
    // During overdub (possibly extending), show position relative to base layer
    // so the ring keeps spinning at the original tempo
    playbackPosition.store(static_cast<float>(readPosition % layers[0].length) /
                           static_cast<float>(layers[0].length));
  } else if (masterLoopLength > 0 && currentState == LooperState::PLAYING) {
    playbackPosition.store(static_cast<float>(readPosition) /
                           static_cast<float>(masterLoopLength));
  } else if (currentState == LooperState::RECORDING && writePosition > 0) {
    // During recording, show progress toward the point recording will
    // auto-stop: the locked master length (Classic/Bars) or the full tape
    // (Dynamic). Matches recordBasisSeconds sent to the UI.
    const int basisSamples =
        masterLoopLength > 0 ? masterLoopLength : layers[0].capacity.load();
    playbackPosition.store(basisSamples > 0
                               ? static_cast<float>(writePosition) /
                                     static_cast<float>(basisSamples)
                               : 0.0f);
  } else {
    playbackPosition.store(0.0f);
  }
}

//==============================================================================
bool OrbitLooperAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor *OrbitLooperAudioProcessor::createEditor() {
  return new OrbitLooperAudioProcessorEditor(*this);
}

//==============================================================================
void OrbitLooperAudioProcessor::exportLoopToWav(const juce::File &file) {
  // Message-thread mixdown. Only export while the loop is stable — in
  // RECORDING/OVERDUBBING the audio thread is writing the very buffers we
  // would read. Snapshot counts/lengths first and clamp by the published
  // capacities so a concurrent trigger can at worst glitch the file, never
  // read out of bounds.
  const int state = looperState.load();
  if (state != static_cast<int>(LooperState::PLAYING) &&
      state != static_cast<int>(LooperState::STOPPED))
    return;

  const int layerCount =
      std::clamp(layersInUseAtomic.load(), 0, static_cast<int>(MAX_LAYERS));
  int layerLengths[MAX_LAYERS] = {};
  int exportMaster = 0;
  for (int l = 0; l < layerCount; ++l) {
    const auto &layer = layers[static_cast<size_t>(l)];
    layerLengths[l] =
        std::clamp(layer.length, 0, layer.capacity.load());
    exportMaster = std::max(exportMaster, layerLengths[l]);
  }

  if (exportMaster <= 0 || layerCount <= 0)
    return;

  // Delete existing file if any
  if (file.existsAsFile())
    file.deleteFile();

  std::unique_ptr<juce::OutputStream> outputStream(
      file.createOutputStream().release());
  if (outputStream == nullptr)
    return;

  juce::WavAudioFormat wavFormat;
  const int numChannels = 2;
  const int bitsPerSample = 24;

  auto writer = wavFormat.createWriterFor(
      outputStream, juce::AudioFormatWriterOptions{}
                        .withSampleRate(currentSampleRate)
                        .withNumChannels(numChannels)
                        .withBitsPerSample(bitsPerSample));

  if (writer != nullptr) {
    // Mix down all layers into a single buffer using modulo per layer
    juce::AudioBuffer<float> exportBuffer(numChannels, exportMaster);
    exportBuffer.clear();

    for (int l = 0; l < layerCount; ++l) {
      const auto &layer = layers[static_cast<size_t>(l)];
      const int len = layerLengths[l];
      if (len > 0) {
        for (int s = 0; s < exportMaster; ++s) {
          int layerPos = s % len;
          exportBuffer.addSample(0, s,
                                 layer.bufferL[static_cast<size_t>(layerPos)]);
          exportBuffer.addSample(1, s,
                                 layer.bufferR[static_cast<size_t>(layerPos)]);
        }
      }
    }

    // Match playback: apply loop_level so the file sounds like what the user
    // hears, and so summing up to 8 layers can't trivially clip the 24-bit int.
    const float exportLoopLevel =
        apvts.getRawParameterValue("loop_level")->load() / 100.0f;
    exportBuffer.applyGain(exportLoopLevel);

    writer->writeFromAudioSampleBuffer(exportBuffer, 0, exportMaster);
  }
}

//==============================================================================
void OrbitLooperAudioProcessor::getStateInformation(
    juce::MemoryBlock &destData) {
  auto state = apvts.copyState();
  state.setProperty("maxLoopSeconds", maxLoopSeconds.load(), nullptr);

  // Save MIDI CC mappings (table-driven; key = "midiCC_" + lowercase name)
  {
    const auto &actionNames = getMidiActionNames();
    for (int i = 0; i < NUM_MIDI_ACTIONS; ++i)
      state.setProperty("midiCC_" + juce::String(
                                        actionNames[static_cast<size_t>(i)])
                                        .toLowerCase(),
                        getMidiCC(static_cast<MidiAction>(i)), nullptr);
  }

  // Save key bindings
  for (int i = 0; i < NUM_KEY_ACTIONS; ++i)
    state.setProperty("keyBind_" + juce::String(i), keyBindings[i], nullptr);

  // Save Loop Mode state
  state.setProperty("loopMode", loopMode.load(), nullptr);
  state.setProperty("maxLayerCount", maxLayerCount.load(), nullptr);
  state.setProperty("muteOnStartup", muteOnStartup.load() ? 1 : 0, nullptr);
  state.setProperty("inputMuted", inputMuted.load() ? 1 : 0, nullptr);

  std::unique_ptr<juce::XmlElement> xml(state.createXml());
  copyXmlToBinary(*xml, destData);
}

void OrbitLooperAudioProcessor::setStateInformation(const void *data,
                                                    int sizeInBytes) {
  std::unique_ptr<juce::XmlElement> xmlState(
      getXmlFromBinary(data, sizeInBytes));

  if (xmlState.get() != nullptr) {
    if (xmlState->hasTagName(apvts.state.getType())) {
      auto tree = juce::ValueTree::fromXml(*xmlState);
      if (tree.hasProperty("maxLoopSeconds"))
        setMaxLoopLength( // clamps + resizes storage on the message thread
            static_cast<float>(tree.getProperty("maxLoopSeconds")));

      // Restore MIDI CC mappings (table-driven; matches getStateInformation)
      {
        const auto &actionNames = getMidiActionNames();
        for (int i = 0; i < NUM_MIDI_ACTIONS; ++i) {
          const juce::String prop =
              "midiCC_" +
              juce::String(actionNames[static_cast<size_t>(i)]).toLowerCase();
          if (tree.hasProperty(prop))
            setMidiCC(static_cast<MidiAction>(i),
                      static_cast<int>(tree.getProperty(prop)));
        }
      }

      // Restore key bindings
      for (int i = 0; i < NUM_KEY_ACTIONS; ++i) {
        juce::String propName = "keyBind_" + juce::String(i);
        if (tree.hasProperty(propName))
          setKeyBinding(static_cast<MidiAction>(i),
                        tree.getProperty(propName).toString());
      }

      // Restore Loop Mode
      auto *loopModeParam = dynamic_cast<juce::AudioParameterChoice *>(
          apvts.getParameter("loop_mode"));
      if (tree.hasProperty("loopMode")) {
        int savedMode = static_cast<int>(tree.getProperty("loopMode"));
        loopMode.store(savedMode);
        if (loopModeParam)
          loopModeParam->setValueNotifyingHost(
              loopModeParam->getNormalisableRange().convertTo0to1(
                  static_cast<float>(savedMode)));
      }
      if (tree.hasProperty("maxLayerCount"))
        maxLayerCount.store(
            std::clamp(static_cast<int>(tree.getProperty("maxLayerCount")), 1,
                       MAX_LAYERS));

      if (tree.hasProperty("muteOnStartup"))
        muteOnStartup.store(static_cast<int>(tree.getProperty("muteOnStartup")) != 0);

      if (tree.hasProperty("inputMuted"))
        inputMuted.store(static_cast<int>(tree.getProperty("inputMuted")) != 0);

      apvts.replaceState(tree);
    }
  }
}

//==============================================================================
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new OrbitLooperAudioProcessor();
}
