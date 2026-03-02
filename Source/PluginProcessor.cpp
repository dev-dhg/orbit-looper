#include "PluginProcessor.h"
#include "PluginEditor.h"

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
}

OrbitLooperAudioProcessor::~OrbitLooperAudioProcessor() {}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
OrbitLooperAudioProcessor::createParameterLayout() {
  juce::AudioProcessorValueTreeState::ParameterLayout layout;

  // Max Loop Length - how many seconds the buffer can hold (0-1800)
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"max_loop_length", 1}, "Max Loop Length",
      juce::NormalisableRange<float>(0.0f, 1800.0f, 0.001f),
      60.0f, // default: 1 minute
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          [](float value, int) {
            if (value >= 60.0f) {
              int m = static_cast<int>(value) / 60;
              float s = std::fmod(value, 60.0f);
              return juce::String(m) + "m " + juce::String(s, 3) + "s";
            }
            return juce::String(value, 3) + "s";
          })));

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
  currentSampleRate = sampleRate;
  const float limitSec = std::min(maxLoopSeconds.load(), SAFETY_LIMIT_SECONDS);
  maxLoopSamples = static_cast<int>(sampleRate * limitSec);

  // Allocate all layer buffers
  for (auto &layer : layers) {
    layer.bufferL.resize(static_cast<size_t>(maxLoopSamples), 0.0f);
    layer.bufferR.resize(static_cast<size_t>(maxLoopSamples), 0.0f);
    layer.length = 0;
  }

  // Reset state
  numLayers = 0;
  masterLoopLength = 0;
  activeLayerIdx = -1;
  footswitchAutoOverdub = false;
  writePosition = 0;
  readPosition = 0;
  currentState = LooperState::EMPTY;
  looperState.store(static_cast<int>(LooperState::EMPTY));
  hasUndoLayer.store(false);

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
    layer.bufferL.clear();
    layer.bufferR.clear();
    layer.length = 0;
  }
  numLayers = 0;
  masterLoopLength = 0;
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
}

void OrbitLooperAudioProcessor::cancelMidiLearn() {
  midiLearnActive.store(false);
  midiLearnTarget.store(CC_UNASSIGNED);
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

    if (!msg.isController())
      continue;

    const int cc = msg.getControllerNumber();
    const int value = msg.getControllerValue();

    // ── MIDI Learn mode ──
    if (midiLearnActive.load()) {
      // Any CC with value >= threshold completes the learn
      if (value >= CC_TRIGGER_THRESHOLD) {
        const int target = midiLearnTarget.load();
        if (target >= 0 && target < NUM_MIDI_ACTIONS) {
          setMidiCC(static_cast<MidiAction>(target), cc);
          lastLearnedCC.store(cc);
          lastLearnedAction.store(target);
        }
        midiLearnActive.store(false);
        midiLearnTarget.store(CC_UNASSIGNED);
      }
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

void OrbitLooperAudioProcessor::processStateChanges() {
  // Sync Loop Mode parameter (set via UI)
  auto *loopModeParam = dynamic_cast<juce::AudioParameterChoice *>(
      apvts.getParameter("loop_mode"));
  if (loopModeParam) {
    loopMode.store(loopModeParam->getIndex());
  }

  // Handle Loop Mode Cycle (from MIDI trigger)
  if (pendingLoopModeCycle.exchange(false) ||
      pendingClassicModeToggle.exchange(false)) {
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
    for (auto &layer : layers) {
      std::fill(layer.bufferL.begin(), layer.bufferL.end(), 0.0f);
      std::fill(layer.bufferR.begin(), layer.bufferR.end(), 0.0f);
      layer.length = 0;
    }
    numLayers = 0;
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
    hasUndoLayer.store(false);
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
        std::fill(removed.bufferL.begin(), removed.bufferL.end(), 0.0f);
        std::fill(removed.bufferR.begin(), removed.bufferR.end(), 0.0f);
        removed.length = 0;
      }

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
      // Finish recording base layer, go to stopped
      layers[0].length = writePosition;
      numLayers = 1;
      masterLoopLength = writePosition;
      float recordedSec = static_cast<float>(masterLoopLength) /
                          static_cast<float>(currentSampleRate);
      loopLengthSeconds.store(recordedSec);

      // Auto Length: Automatically set the new loop value on the max length
      // slider
      if (isAutoLength.load()) {
        auto *param = apvts.getParameter("max_loop_length");
        if (param) {
          float normalized =
              param->getNormalisableRange().convertTo0to1(recordedSec);
          param->setValueNotifyingHost(normalized);
        }
      }

      readPosition = 0;
      activeLayerIdx = -1;
      currentState = LooperState::STOPPED;
    } else if (currentState == LooperState::OVERDUBBING) {
      // Finalize current overdub layer
      if (activeLayerIdx >= 0 && activeLayerIdx < MAX_LAYERS) {
        auto &layer = layers[static_cast<size_t>(activeLayerIdx)];
        // If extended past master loop, use readPosition as length
        // Otherwise pad to masterLoopLength so modulo works correctly
        layer.length =
            (readPosition > masterLoopLength) ? readPosition : masterLoopLength;

        // Recalculate master loop length
        masterLoopLength = 0;
        for (int i = 0; i < numLayers; ++i)
          masterLoopLength =
              std::max(masterLoopLength, layers[static_cast<size_t>(i)].length);

        loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                                static_cast<float>(currentSampleRate));
        readPosition = readPosition % masterLoopLength;
      }
      activeLayerIdx = -1;
      footswitchAutoOverdub = false;
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
      // Finish recording and play
      layers[0].length = writePosition;
      numLayers = 1;
      masterLoopLength = writePosition;
      float recordedSec = static_cast<float>(masterLoopLength) /
                          static_cast<float>(currentSampleRate);
      loopLengthSeconds.store(recordedSec);

      // Auto Length: Automatically set the new loop value on the max length
      // slider
      if (isAutoLength.load()) {
        auto *param = apvts.getParameter("max_loop_length");
        if (param) {
          float normalized =
              param->getNormalisableRange().convertTo0to1(recordedSec);
          param->setValueNotifyingHost(normalized);
        }
      }

      readPosition = 0;
      activeLayerIdx = -1;
      hasUndoLayer.store(false);
      currentState = LooperState::PLAYING;
      looperState.store(static_cast<int>(currentState));
    } else if (currentState == LooperState::OVERDUBBING) {
      // Finish overdub and play
      if (activeLayerIdx >= 0 && activeLayerIdx < MAX_LAYERS) {
        auto &layer = layers[static_cast<size_t>(activeLayerIdx)];
        layer.length =
            (readPosition > masterLoopLength) ? readPosition : masterLoopLength;
        masterLoopLength = 0;
        for (int i = 0; i < numLayers; ++i)
          masterLoopLength =
              std::max(masterLoopLength, layers[static_cast<size_t>(i)].length);
        loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                                static_cast<float>(currentSampleRate));
        readPosition = readPosition % masterLoopLength;
      }
      activeLayerIdx = -1;
      footswitchAutoOverdub = false;
      hasUndoLayer.store(numLayers > 1);
      currentState = LooperState::PLAYING;
      looperState.store(static_cast<int>(currentState));
    }
  }
}

void OrbitLooperAudioProcessor::handleRecordTrigger() {
  switch (currentState) {
  case LooperState::EMPTY:
    // Start recording the first loop layer
    activeLayerIdx = 0;
    writePosition = 0;
    currentState = LooperState::RECORDING;

    // Fixed Length Modes: Instantly lock duration upon starting recording
    if (loopMode.load() == static_cast<int>(LoopMode::Bars)) {
      float bpmVal = metroBPMSetting.load();
      int bpb = metroBeatsPerBarSetting.load();
      int bars = metroNumBarsSetting.load();
      masterLoopLength =
          static_cast<int>((60.0f / bpmVal) * bpb * bars * currentSampleRate);
    } else if (loopMode.load() == static_cast<int>(LoopMode::Classic)) {
      // Classic Mode follows the current "Max Length" slider as a fixed
      // duration
      masterLoopLength = maxLoopSamples;
    } else {
      masterLoopLength = 0; // Dynamic Mode: length determined by stop trigger
    }
    break;

  case LooperState::RECORDING: {
    // Finish recording base layer, start playback
    layers[0].length = writePosition;
    numLayers = 1;
    masterLoopLength = writePosition;
    float recordedSec = static_cast<float>(masterLoopLength) /
                        static_cast<float>(currentSampleRate);
    loopLengthSeconds.store(recordedSec);

    // Auto Length: Automatically set the new loop value on the max length
    // slider
    if (isAutoLength.load() &&
        loopMode.load() != static_cast<int>(LoopMode::Bars)) {
      auto *param = apvts.getParameter("max_loop_length");
      if (param) {
        float normalized =
            param->getNormalisableRange().convertTo0to1(recordedSec);
        param->setValueNotifyingHost(normalized);
      }
    }

    readPosition = 0;
    activeLayerIdx = -1;
    hasUndoLayer.store(false);
    currentState = LooperState::PLAYING;
    break;
  }

  case LooperState::PLAYING:
    // Start overdubbing on a new layer (or arm if arm-toggle enabled)
    if (overdubArmed) {
      // Already armed — toggle OFF (disarm)
      overdubArmed = false;
      overdubArmedForUI.store(false);
    } else if (overdubArmEnabled.load() && numLayers < maxLayerCount.load() &&
               masterLoopLength > 0) {
      // Arm: queue overdub for next loop boundary
      overdubArmed = true;
      overdubArmedForUI.store(true);
    } else if (numLayers < maxLayerCount.load() && masterLoopLength > 0) {
      activeLayerIdx = numLayers;
      numLayers++;

      // Clear the new layer prefix up to the start point (fast)
      auto &newLayer = layers[static_cast<size_t>(activeLayerIdx)];
      int prefixSamples = std::min(readPosition, maxLoopSamples);
      if (prefixSamples > 0) {
        juce::FloatVectorOperations::clear(newLayer.bufferL.data(),
                                           prefixSamples);
        juce::FloatVectorOperations::clear(newLayer.bufferR.data(),
                                           prefixSamples);
      }
      newLayer.length = 0;
      footswitchAutoOverdub = false;
      hasUndoLayer.store(true);
      writePosition = readPosition;
      currentState = LooperState::OVERDUBBING;
    }
    break;

  case LooperState::OVERDUBBING:
    // Stop overdubbing, finalize layer, continue playback
    if (activeLayerIdx >= 0 && activeLayerIdx < MAX_LAYERS) {
      auto &layer = layers[static_cast<size_t>(activeLayerIdx)];
      const int currentMode = loopMode.load();

      if (currentMode == static_cast<int>(LoopMode::Dynamic)) {
        // Dynamic Mode: If extended past master loop, use readPosition as
        // length
        layer.length =
            (readPosition > masterLoopLength) ? readPosition : masterLoopLength;

        // Recalculate master loop length
        masterLoopLength = 0;
        for (int i = 0; i < numLayers; ++i)
          masterLoopLength =
              std::max(masterLoopLength, layers[static_cast<size_t>(i)].length);

        loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                                static_cast<float>(currentSampleRate));
      } else {
        // Classic or Bars Mode: layer length = master loop length (locked)
        layer.length = masterLoopLength;
      }
      readPosition = readPosition % masterLoopLength;
    }
    activeLayerIdx = -1;
    footswitchAutoOverdub = false;
    hasUndoLayer.store(numLayers > 1);
    currentState = LooperState::PLAYING;
    break;

  case LooperState::STOPPED:
    // Start overdubbing from stopped state on a new layer
    if (numLayers < maxLayerCount.load() && masterLoopLength > 0) {
      activeLayerIdx = numLayers;
      numLayers++;
      auto &newLayer = layers[static_cast<size_t>(activeLayerIdx)];
      // Starting from stopped (pos 0), only clear a small head start
      juce::FloatVectorOperations::clear(newLayer.bufferL.data(),
                                         std::min(1024, maxLoopSamples));
      juce::FloatVectorOperations::clear(newLayer.bufferR.data(),
                                         std::min(1024, maxLoopSamples));
      newLayer.length = 0;
      footswitchAutoOverdub = false;
      hasUndoLayer.store(true);
      readPosition = 0;
      writePosition = 0;
      currentState = LooperState::OVERDUBBING;
    }
    break;
  }

  looperState.store(static_cast<int>(currentState));
}

void OrbitLooperAudioProcessor::handleOverdubTrigger() {
  // Dedicated overdub: only from PLAYING, STOPPED, or OVERDUBBING
  switch (currentState) {
  case LooperState::PLAYING:
    if (overdubArmed) {
      // Already armed — toggle OFF (disarm)
      overdubArmed = false;
      overdubArmedForUI.store(false);
    } else if (overdubArmEnabled.load() && numLayers < maxLayerCount.load() &&
               masterLoopLength > 0) {
      // Arm: queue overdub for next loop boundary
      overdubArmed = true;
      overdubArmedForUI.store(true);
    } else if (numLayers < maxLayerCount.load() && masterLoopLength > 0) {
      // Arm disabled — immediate overdub (original behavior)
      activeLayerIdx = numLayers;
      numLayers++;
      auto &newLayer = layers[static_cast<size_t>(activeLayerIdx)];

      // Clear prefix up to current position
      int prefixSamples = std::min(readPosition, maxLoopSamples);
      if (prefixSamples > 0) {
        juce::FloatVectorOperations::clear(newLayer.bufferL.data(),
                                           prefixSamples);
        juce::FloatVectorOperations::clear(newLayer.bufferR.data(),
                                           prefixSamples);
      }

      newLayer.length = 0;
      footswitchAutoOverdub = false;
      hasUndoLayer.store(true);
      writePosition = readPosition;
      currentState = LooperState::OVERDUBBING;
    }
    break;

  case LooperState::OVERDUBBING:
    // Stop overdubbing, finalize layer, continue playback
    if (activeLayerIdx >= 0 && activeLayerIdx < MAX_LAYERS) {
      auto &layer = layers[static_cast<size_t>(activeLayerIdx)];
      const int currentMode = loopMode.load();

      if (currentMode == static_cast<int>(LoopMode::Dynamic)) {
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
      readPosition = readPosition % masterLoopLength;
    }
    activeLayerIdx = -1;
    footswitchAutoOverdub = false;
    hasUndoLayer.store(numLayers > 1);
    currentState = LooperState::PLAYING;
    break;

  case LooperState::STOPPED:
    // Start overdubbing from stopped state
    if (numLayers < maxLayerCount.load() && masterLoopLength > 0) {
      activeLayerIdx = numLayers;
      numLayers++;
      auto &newLayer = layers[static_cast<size_t>(activeLayerIdx)];
      // Starting from stopped (pos 0), only clear a small head start
      juce::FloatVectorOperations::clear(newLayer.bufferL.data(),
                                         std::min(1024, maxLoopSamples));
      juce::FloatVectorOperations::clear(newLayer.bufferR.data(),
                                         std::min(1024, maxLoopSamples));
      newLayer.length = 0;
      footswitchAutoOverdub = false;
      hasUndoLayer.store(true);
      readPosition = 0;
      writePosition = 0;
      currentState = LooperState::OVERDUBBING;
    }
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
    // Footswitch into overdub: respect arm toggle
    if (overdubArmed) {
      overdubArmed = false;
      overdubArmedForUI.store(false);
    } else if (overdubArmEnabled.load() && numLayers < maxLayerCount.load() &&
               masterLoopLength > 0) {
      overdubArmed = true;
      overdubArmedForUI.store(true);
    } else if (numLayers < maxLayerCount.load() && masterLoopLength > 0) {
      activeLayerIdx = numLayers;
      numLayers++;
      auto &newLayer = layers[static_cast<size_t>(activeLayerIdx)];

      // Clear prefix up to current position
      int prefixSamples = std::min(readPosition, maxLoopSamples);
      if (prefixSamples > 0) {
        juce::FloatVectorOperations::clear(newLayer.bufferL.data(),
                                           prefixSamples);
        juce::FloatVectorOperations::clear(newLayer.bufferR.data(),
                                           prefixSamples);
      }

      newLayer.length = 0;
      // Mark as footswitch-initiated if there are previous overdubs to undo
      footswitchAutoOverdub =
          (numLayers > 2); // > 2 because we just incremented
      hasUndoLayer.store(true);
      writePosition = readPosition;
      currentState = LooperState::OVERDUBBING;
      looperState.store(static_cast<int>(currentState));
    }
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
    layers[0].length = writePosition;
    numLayers = 1;
    masterLoopLength = writePosition;
    loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                            static_cast<float>(currentSampleRate));
    readPosition = 0;
    activeLayerIdx = -1;
  } else if (currentState == LooperState::OVERDUBBING) {
    // Finalize current overdub layer
    if (activeLayerIdx >= 0 && activeLayerIdx < MAX_LAYERS) {
      auto &layer = layers[static_cast<size_t>(activeLayerIdx)];
      const int currentMode = loopMode.load();
      if (currentMode != static_cast<int>(LoopMode::Dynamic)) {
        layer.length = masterLoopLength;
      } else {
        layer.length =
            (readPosition > masterLoopLength) ? readPosition : masterLoopLength;

        masterLoopLength = 0;
        for (int i = 0; i < numLayers; ++i)
          masterLoopLength =
              std::max(masterLoopLength, layers[static_cast<size_t>(i)].length);

        loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                                static_cast<float>(currentSampleRate));
      }
      readPosition = readPosition % masterLoopLength;
    }
    activeLayerIdx = -1;
    footswitchAutoOverdub = false;
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

  // Sync max loop length -> buffer size
  {
    const float targetMaxSec = maxLoopSeconds.load();

    if (targetMaxSec != currentMaxAllocatedSeconds &&
        currentState == LooperState::EMPTY) {
      currentMaxAllocatedSeconds = targetMaxSec;
      maxLoopSamples = static_cast<int>(currentSampleRate * targetMaxSec);
      for (auto &layer : layers) {
        layer.bufferL.resize(static_cast<size_t>(maxLoopSamples), 0.0f);
        layer.bufferR.resize(static_cast<size_t>(maxLoopSamples), 0.0f);
      }
    }
  }

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
  bool isInputMuted = inputMuted.load();

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
    // Clear ahead to avoid stale data (block-level for efficiency)
    int clearStart = writePosition;
    int clearEnd = std::min(writePosition + numSamples + 1024, maxLoopSamples);
    if (clearStart < clearEnd) {
      juce::FloatVectorOperations::clear(baseLayer.bufferL.data() + clearStart,
                                         clearEnd - clearStart);
      juce::FloatVectorOperations::clear(baseLayer.bufferR.data() + clearStart,
                                         clearEnd - clearStart);
    }

    for (int i = 0; i < numSamples; ++i) {
      float inMono = (inputL[i] + inputR[i]) * 0.5f;
      if (writePosition < maxLoopSamples) {
        baseLayer.bufferL[static_cast<size_t>(writePosition)] = inMono * inPanL;
        baseLayer.bufferR[static_cast<size_t>(writePosition)] = inMono * inPanR;
        writePosition++;
      }

      // Pass-through while recording
      outputL[i] = isInputMuted ? 0.0f : inMono * inPanL;
      outputR[i] = isInputMuted ? 0.0f : inMono * inPanR;
      peakOut = std::max(peakOut,
                         std::max(std::abs(outputL[i]), std::abs(outputR[i])));

      // Fixed Length Modes (Classic/Bars): Auto-stop recording at calculated
      // length
      const int currentMode = loopMode.load();
      const bool nonDynamicMode =
          (currentMode != static_cast<int>(LoopMode::Dynamic));
      if (nonDynamicMode && masterLoopLength > 0 &&
          writePosition >= masterLoopLength) {
        baseLayer.length = masterLoopLength;
        numLayers = 1;
        loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                                static_cast<float>(currentSampleRate));

        readPosition = 0;
        activeLayerIdx = -1;
        currentState = LooperState::PLAYING;
        looperState.store(static_cast<int>(LooperState::PLAYING));
        break;
      }
    }

    // If we've hit max length, auto-stop recording
    if (writePosition >= maxLoopSamples) {
      baseLayer.length = maxLoopSamples;
      numLayers = 1;
      masterLoopLength = maxLoopSamples;
      float recordedSec = static_cast<float>(masterLoopLength) /
                          static_cast<float>(currentSampleRate);
      loopLengthSeconds.store(recordedSec);

      // Auto Length: Automatically set the new loop value on the max length
      // slider
      if (isAutoLength.load()) {
        auto *param = apvts.getParameter("max_loop_length");
        if (param) {
          float normalized =
              param->getNormalisableRange().convertTo0to1(recordedSec);
          param->setValueNotifyingHost(normalized);
        }
      }

      readPosition = 0;
      activeLayerIdx = -1;
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

    // Sum all layers with modulo: Position_layer = CurrentTime % Length_layer
    for (int i = 0; i < numSamples; ++i) {
      float sumL = 0.0f, sumR = 0.0f;
      for (int l = 0; l < numLayers; ++l) {
        const auto &layer = layers[static_cast<size_t>(l)];
        if (layer.length > 0) {
          int layerPos = readPosition % layer.length;
          sumL += layer.bufferL[static_cast<size_t>(layerPos)];
          sumR += layer.bufferR[static_cast<size_t>(layerPos)];
        }
      }

      float inMono = (inputL[i] + inputR[i]) * 0.5f;
      outputL[i] = (isInputMuted ? 0.0f : inMono * inPanL) + sumL * loopLevel;
      outputR[i] = (isInputMuted ? 0.0f : inMono * inPanR) + sumR * loopLevel;
      peakOut = std::max(peakOut,
                         std::max(std::abs(outputL[i]), std::abs(outputR[i])));

      int prevPos = readPosition;
      readPosition = (readPosition + 1) % masterLoopLength;

      // Overdub Arm: detect loop boundary (wrap from end to 0)
      if (overdubArmed && readPosition < prevPos) {
        // Loop just restarted — engage overdub
        overdubArmed = false;
        overdubArmedForUI.store(false);
        if (numLayers < maxLayerCount.load()) {
          activeLayerIdx = numLayers;
          numLayers++;
          auto &newLayer = layers[static_cast<size_t>(activeLayerIdx)];

          // Clear prefix up to current position (fast)
          int prefixSamples = std::min(readPosition, maxLoopSamples);
          if (prefixSamples > 0) {
            juce::FloatVectorOperations::clear(newLayer.bufferL.data(),
                                               prefixSamples);
            juce::FloatVectorOperations::clear(newLayer.bufferR.data(),
                                               prefixSamples);
          }

          newLayer.length = 0;
          footswitchAutoOverdub = false;
          hasUndoLayer.store(true);
          writePosition = readPosition;
          currentState = LooperState::OVERDUBBING;
          looperState.store(static_cast<int>(LooperState::OVERDUBBING));
          // Continue remaining samples in OVERDUBBING — but for simplicity
          // the next processBlock will handle it. The few missed samples
          // at the boundary are imperceptible.
          break;
        }
      }
    }

    break;
  }

  case LooperState::OVERDUBBING: {
    if (masterLoopLength <= 0 || activeLayerIdx < 0 ||
        activeLayerIdx >= MAX_LAYERS) {
      for (int i = 0; i < numSamples; ++i) {
        outputL[i] = isInputMuted ? 0.0f : inputL[i];
        outputR[i] = isInputMuted ? 0.0f : inputR[i];
      }
      break;
    }

    auto &activeLayer = layers[static_cast<size_t>(activeLayerIdx)];

    // Clear ahead to avoid stale data
    const int currentMode = loopMode.load();
    const bool nonDynamicMode =
        (currentMode != static_cast<int>(LoopMode::Dynamic));
    int clearStartIdx =
        nonDynamicMode ? (readPosition % masterLoopLength) : readPosition;
    int clearEndIdx =
        std::min(clearStartIdx + numSamples + 1024, maxLoopSamples);
    if (clearStartIdx < clearEndIdx) {
      juce::FloatVectorOperations::clear(activeLayer.bufferL.data() +
                                             clearStartIdx,
                                         clearEndIdx - clearStartIdx);
      juce::FloatVectorOperations::clear(activeLayer.bufferR.data() +
                                             clearStartIdx,
                                         clearEndIdx - clearStartIdx);
    }

    // Overdub: write input to new layer while summing all existing layers
    // Classic Mode: auto-stop overdub when readPosition reaches
    // masterLoopLength Dynamic Mode: readPosition advances linearly (can extend
    // past masterLoopLength) Existing layers read at (readPosition %
    // layerLength) — shorter layers tile
    for (int i = 0; i < numSamples; ++i) {
      // In non-dynamic modes, use modulo for buffer write position but track
      // linear readPosition
      int writePos =
          nonDynamicMode ? (readPosition % masterLoopLength) : readPosition;

      // Sum all complete layers (not the active one being recorded)
      float sumL = 0.0f, sumR = 0.0f;
      for (int l = 0; l < numLayers; ++l) {
        if (l == activeLayerIdx)
          continue; // Skip the layer being recorded
        const auto &layer = layers[static_cast<size_t>(l)];
        if (layer.length > 0) {
          int layerPos = writePos % layer.length;
          sumL += layer.bufferL[static_cast<size_t>(layerPos)];
          sumR += layer.bufferR[static_cast<size_t>(layerPos)];
        }
      }

      // Write input to active layer at current position
      float inMono = (inputL[i] + inputR[i]) * 0.5f;
      if (writePos < maxLoopSamples) {
        activeLayer.bufferL[static_cast<size_t>(writePos)] = inMono * inPanL;
        activeLayer.bufferR[static_cast<size_t>(writePos)] = inMono * inPanR;
      }

      // Output: dry input + sum of existing layers at loop level
      outputL[i] = (isInputMuted ? 0.0f : inMono * inPanL) + sumL * loopLevel;
      outputR[i] = (isInputMuted ? 0.0f : inMono * inPanR) + sumR * loopLevel;
      peakOut = std::max(peakOut,
                         std::max(std::abs(outputL[i]), std::abs(outputR[i])));

      readPosition++;

      // Non-dynamic modes: auto-stop overdub at loop boundary (OVERDUB → PLAY)
      if (nonDynamicMode && readPosition >= masterLoopLength) {
        activeLayer.length = masterLoopLength;
        readPosition = readPosition % masterLoopLength;
        activeLayerIdx = -1;
        footswitchAutoOverdub = false;
        hasUndoLayer.store(numLayers > 1);
        currentState = LooperState::PLAYING;
        looperState.store(static_cast<int>(LooperState::PLAYING));

        // Process remaining samples as PLAYING
        for (int j = i + 1; j < numSamples; ++j) {
          float pSumL = 0.0f, pSumR = 0.0f;
          for (int l = 0; l < numLayers; ++l) {
            const auto &layer = layers[static_cast<size_t>(l)];
            if (layer.length > 0) {
              int layerPos = readPosition % layer.length;
              pSumL += layer.bufferL[static_cast<size_t>(layerPos)];
              pSumR += layer.bufferR[static_cast<size_t>(layerPos)];
            }
          }
          float inMonoJ = (inputL[j] + inputR[j]) * 0.5f;
          outputL[j] =
              (isInputMuted ? 0.0f : inMonoJ * inPanL) + pSumL * loopLevel;
          outputR[j] =
              (isInputMuted ? 0.0f : inMonoJ * inPanR) + pSumR * loopLevel;
          peakOut = std::max(
              peakOut, std::max(std::abs(outputL[j]), std::abs(outputR[j])));
          readPosition = (readPosition + 1) % masterLoopLength;
        }
        break;
      }

      // Dynamic Mode: auto-stop overdub if we hit max buffer
      if (!nonDynamicMode && readPosition >= maxLoopSamples) {
        activeLayer.length = maxLoopSamples;
        masterLoopLength = 0;
        for (int l = 0; l < numLayers; ++l)
          masterLoopLength =
              std::max(masterLoopLength, layers[static_cast<size_t>(l)].length);

        loopLengthSeconds.store(static_cast<float>(masterLoopLength) /
                                static_cast<float>(currentSampleRate));
        readPosition = 0;
        activeLayerIdx = -1;
        footswitchAutoOverdub = false;
        hasUndoLayer.store(numLayers > 1);
        currentState = LooperState::PLAYING;
        looperState.store(static_cast<int>(LooperState::PLAYING));

        // Process remaining samples as PLAYING
        for (int j = i + 1; j < numSamples; ++j) {
          float pSumL = 0.0f, pSumR = 0.0f;
          for (int l = 0; l < numLayers; ++l) {
            const auto &layer = layers[static_cast<size_t>(l)];
            if (layer.length > 0) {
              int layerPos = readPosition % layer.length;
              pSumL += layer.bufferL[static_cast<size_t>(layerPos)];
              pSumR += layer.bufferR[static_cast<size_t>(layerPos)];
            }
          }
          float inMonoJ = (inputL[j] + inputR[j]) * 0.5f;
          outputL[j] =
              (isInputMuted ? 0.0f : inMonoJ * inPanL) + pSumL * loopLevel;
          outputR[j] =
              (isInputMuted ? 0.0f : inMonoJ * inPanR) + pSumR * loopLevel;
          peakOut = std::max(
              peakOut, std::max(std::abs(outputL[j]), std::abs(outputR[j])));
          readPosition = (readPosition + 1) % masterLoopLength;
        }
        break;
      }
    }

    // Keep write position in sync for state transitions
    writePosition = readPosition;

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
    // During recording, show progress relative to the "Max Length" slider for
    // UI sync. Even in Auto Length mode where the internal buffer is larger
    // (30m), the UI still uses the slider value to scale the normalized
    // position back to time.
    float sliderMaxSec = apvts.getRawParameterValue("max_loop_length")->load();
    float sliderMaxSamples =
        sliderMaxSec * static_cast<float>(currentSampleRate);

    if (sliderMaxSamples > 0)
      playbackPosition.store(static_cast<float>(writePosition) /
                             sliderMaxSamples);
    else
      playbackPosition.store(0.0f);
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
  if (masterLoopLength <= 0 || numLayers <= 0)
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
    juce::AudioBuffer<float> exportBuffer(numChannels, masterLoopLength);
    exportBuffer.clear();

    for (int l = 0; l < numLayers; ++l) {
      const auto &layer = layers[static_cast<size_t>(l)];
      if (layer.length > 0) {
        for (int s = 0; s < masterLoopLength; ++s) {
          int layerPos = s % layer.length;
          exportBuffer.addSample(0, s,
                                 layer.bufferL[static_cast<size_t>(layerPos)]);
          exportBuffer.addSample(1, s,
                                 layer.bufferR[static_cast<size_t>(layerPos)]);
        }
      }
    }

    writer->writeFromAudioSampleBuffer(exportBuffer, 0, masterLoopLength);
  }
}

//==============================================================================
void OrbitLooperAudioProcessor::getStateInformation(
    juce::MemoryBlock &destData) {
  auto state = apvts.copyState();
  state.setProperty("maxLoopSeconds", maxLoopSeconds.load(), nullptr);

  // Save MIDI CC mappings
  state.setProperty("midiCC_record", getMidiCC(MidiAction::Record), nullptr);
  state.setProperty("midiCC_stop", getMidiCC(MidiAction::Stop), nullptr);
  state.setProperty("midiCC_clear", getMidiCC(MidiAction::Clear), nullptr);
  state.setProperty("midiCC_undo", getMidiCC(MidiAction::Undo), nullptr);
  state.setProperty("midiCC_footswitch", getMidiCC(MidiAction::Footswitch),
                    nullptr);
  state.setProperty("midiCC_overdub", getMidiCC(MidiAction::Overdub), nullptr);
  state.setProperty("midiCC_barmode", getMidiCC(MidiAction::BarMode), nullptr);
  state.setProperty("midiCC_click", getMidiCC(MidiAction::Click), nullptr);
  state.setProperty("midiCC_precount", getMidiCC(MidiAction::PreCount),
                    nullptr);
  state.setProperty("midiCC_armoverdub", getMidiCC(MidiAction::ArmOverdub),
                    nullptr);
  state.setProperty("midiCC_playclick", getMidiCC(MidiAction::PlayClick),
                    nullptr);
  state.setProperty("midiCC_play", getMidiCC(MidiAction::Play), nullptr);
  state.setProperty("midiCC_monitor", getMidiCC(MidiAction::Monitor), nullptr);
  state.setProperty("midiCC_loopmodecycle",
                    getMidiCC(MidiAction::LoopModeCycle), nullptr);
  state.setProperty("midiCC_classicmode", getMidiCC(MidiAction::ClassicMode),
                    nullptr);
  state.setProperty("midiCC_beatsmode", getMidiCC(MidiAction::BeatsMode),
                    nullptr);
  state.setProperty("midiCC_dynamicmode", getMidiCC(MidiAction::DynamicMode),
                    nullptr);
  state.setProperty("midiCC_paninputleft", getMidiCC(MidiAction::PanInputLeft),
                    nullptr);
  state.setProperty("midiCC_paninputcenter",
                    getMidiCC(MidiAction::PanInputCenter), nullptr);
  state.setProperty("midiCC_paninputright",
                    getMidiCC(MidiAction::PanInputRight), nullptr);

  // Save key bindings
  for (int i = 0; i < NUM_KEY_ACTIONS; ++i)
    state.setProperty("keyBind_" + juce::String(i), keyBindings[i], nullptr);

  // Save Loop Mode and Auto Length state
  state.setProperty("loopMode", loopMode.load(), nullptr);
  state.setProperty("autoLength", isAutoLength.load() ? 1 : 0, nullptr);
  state.setProperty("maxLayerCount", maxLayerCount.load(), nullptr);

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
        maxLoopSeconds.store(
            static_cast<float>(tree.getProperty("maxLoopSeconds")));

      // Restore MIDI CC mappings
      if (tree.hasProperty("midiCC_record"))
        setMidiCC(MidiAction::Record,
                  static_cast<int>(tree.getProperty("midiCC_record")));
      if (tree.hasProperty("midiCC_stop"))
        setMidiCC(MidiAction::Stop,
                  static_cast<int>(tree.getProperty("midiCC_stop")));
      if (tree.hasProperty("midiCC_clear"))
        setMidiCC(MidiAction::Clear,
                  static_cast<int>(tree.getProperty("midiCC_clear")));
      if (tree.hasProperty("midiCC_undo"))
        setMidiCC(MidiAction::Undo,
                  static_cast<int>(tree.getProperty("midiCC_undo")));
      if (tree.hasProperty("midiCC_footswitch"))
        setMidiCC(MidiAction::Footswitch,
                  static_cast<int>(tree.getProperty("midiCC_footswitch")));
      if (tree.hasProperty("midiCC_overdub"))
        setMidiCC(MidiAction::Overdub,
                  static_cast<int>(tree.getProperty("midiCC_overdub")));
      if (tree.hasProperty("midiCC_barmode"))
        setMidiCC(MidiAction::BarMode,
                  static_cast<int>(tree.getProperty("midiCC_barmode")));
      if (tree.hasProperty("midiCC_click"))
        setMidiCC(MidiAction::Click,
                  static_cast<int>(tree.getProperty("midiCC_click")));
      if (tree.hasProperty("midiCC_precount"))
        setMidiCC(MidiAction::PreCount,
                  static_cast<int>(tree.getProperty("midiCC_precount")));
      if (tree.hasProperty("midiCC_armoverdub"))
        setMidiCC(MidiAction::ArmOverdub,
                  static_cast<int>(tree.getProperty("midiCC_armoverdub")));
      if (tree.hasProperty("midiCC_playclick"))
        setMidiCC(MidiAction::PlayClick,
                  static_cast<int>(tree.getProperty("midiCC_playclick")));
      if (tree.hasProperty("midiCC_play"))
        setMidiCC(MidiAction::Play,
                  static_cast<int>(tree.getProperty("midiCC_play")));
      if (tree.hasProperty("midiCC_monitor"))
        setMidiCC(MidiAction::Monitor,
                  static_cast<int>(tree.getProperty("midiCC_monitor")));
      if (tree.hasProperty("midiCC_loopmodecycle"))
        setMidiCC(MidiAction::LoopModeCycle,
                  static_cast<int>(tree.getProperty("midiCC_loopmodecycle")));
      if (tree.hasProperty("midiCC_classicmode"))
        setMidiCC(MidiAction::ClassicMode,
                  static_cast<int>(tree.getProperty("midiCC_classicmode")));
      if (tree.hasProperty("midiCC_beatsmode"))
        setMidiCC(MidiAction::BeatsMode,
                  static_cast<int>(tree.getProperty("midiCC_beatsmode")));
      if (tree.hasProperty("midiCC_dynamicmode"))
        setMidiCC(MidiAction::DynamicMode,
                  static_cast<int>(tree.getProperty("midiCC_dynamicmode")));
      if (tree.hasProperty("midiCC_paninputleft"))
        setMidiCC(MidiAction::PanInputLeft,
                  static_cast<int>(tree.getProperty("midiCC_paninputleft")));
      if (tree.hasProperty("midiCC_paninputcenter"))
        setMidiCC(MidiAction::PanInputCenter,
                  static_cast<int>(tree.getProperty("midiCC_paninputcenter")));
      if (tree.hasProperty("midiCC_paninputright"))
        setMidiCC(MidiAction::PanInputRight,
                  static_cast<int>(tree.getProperty("midiCC_paninputright")));

      // Restore key bindings
      for (int i = 0; i < NUM_KEY_ACTIONS; ++i) {
        juce::String propName = "keyBind_" + juce::String(i);
        if (tree.hasProperty(propName))
          setKeyBinding(static_cast<MidiAction>(i),
                        tree.getProperty(propName).toString());
      }

      // Restore Loop Mode and Auto Length state
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
      if (tree.hasProperty("autoLength"))
        isAutoLength.store(static_cast<int>(tree.getProperty("autoLength")) !=
                           0);
      if (tree.hasProperty("maxLayerCount"))
        maxLayerCount.store(
            std::clamp(static_cast<int>(tree.getProperty("maxLayerCount")), 1,
                       MAX_LAYERS));

      apvts.replaceState(tree);
    }
  }
}

//==============================================================================
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new OrbitLooperAudioProcessor();
}
