#pragma once

#include <array>
#include <atomic>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>

//==============================================================================
/**
 * Orbit Looper - A guitar looper plugin with WebView UI
 *
 * States:
 *   EMPTY       - No loop recorded, pass-through only
 *   RECORDING   - Recording the first loop layer
 *   PLAYING     - Playing back the loop
 *   OVERDUBBING - Recording a new layer on top of the loop
 *   STOPPED     - Loop exists but playback is stopped
 *
 * Features multi-layer non-destructive overdub, MIDI/key mapping,
 * metronome with bars mode, overdub arm, and resizable WebView UI.
 */
class OrbitLooperAudioProcessor : public juce::AudioProcessor {
public:
  //==============================================================================
  // Looper states
  enum class LooperState {
    EMPTY = 0,   // No loop - waiting for first record
    RECORDING,   // Recording first loop layer
    PLAYING,     // Playing back the loop
    OVERDUBBING, // Adding a new layer on top
    STOPPED      // Loop exists but stopped
  };

  //==============================================================================
  enum class LoopMode { Classic = 0, Bars, Dynamic };

  //==============================================================================
  OrbitLooperAudioProcessor();
  ~OrbitLooperAudioProcessor() override;

  //==============================================================================
  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;

  bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
  using AudioProcessor::processBlock;

  //==============================================================================
  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override;

  //==============================================================================
  const juce::String getName() const override;

  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  double getTailLengthSeconds() const override;

  //==============================================================================
  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int index, const juce::String &newName) override;

  //==============================================================================
  void getStateInformation(juce::MemoryBlock &destData) override;
  void setStateInformation(const void *data, int sizeInBytes) override;

  //==============================================================================
  // MIDI CC Mapping
  //==============================================================================
  enum class MidiAction {
    Record = 0,
    Stop, // Also acts as Play/Stop toggle
    Clear,
    Undo,
    Footswitch,     // Ditto-style single footswitch (single/double/long press)
    Overdub,        // Dedicated overdub trigger (only from PLAYING/STOPPED)
    BarMode,        // Toggle Bar Mode (UI-only)
    Click,          // Toggle Click/Metronome (UI-only)
    PreCount,       // Toggle Pre-count (UI-only)
    ArmOverdub,     // Toggle Overdub Arm (UI-only)
    PlayClick,      // Play/Stop Click — manual metronome audition (UI-only)
    Play,           // Dedicated Play action (starts playback if stopped)
    Monitor,        // Toggle Input Monitoring (UI-only)
    LoopModeCycle,  // Cycle Loop Mode: Dynamic -> Classic -> Bars (UI-only)
    ClassicMode,    // Set Loop Mode to Classic (UI-only)
    BeatsMode,      // Set Loop Mode to Beats/Bars (UI-only)
    DynamicMode,    // Set Loop Mode to Dynamic (UI-only)
    PanInputLeft,   // Set Input Pan to full Left (UI-only)
    PanInputCenter, // Set Input Pan to Center (UI-only)
    PanInputRight,  // Set Input Pan to full Right (UI-only)
    NUM_ACTIONS
  };

  static constexpr int NUM_MIDI_ACTIONS =
      static_cast<int>(MidiAction::NUM_ACTIONS);
  static constexpr int CC_UNASSIGNED = -1;
  static constexpr int CC_TRIGGER_THRESHOLD =
      64; // CC >= 64 = triggered (momentary)

  // Get/set CC assignments
  int getMidiCC(MidiAction action) const;
  void setMidiCC(MidiAction action, int ccNumber); // -1 to unassign
  void clearMidiCC(MidiAction action);

  // MIDI Learn
  void startMidiLearn(MidiAction action);
  void cancelMidiLearn();
  bool isMidiLearning() const;
  MidiAction getMidiLearnTarget() const;

  // Thread-safe MIDI state for UI
  std::atomic<int> lastLearnedCC{CC_UNASSIGNED};     // Set when learn completes
  std::atomic<int> lastLearnedAction{CC_UNASSIGNED}; // Which action was learned

  //==============================================================================
  // Keyboard Mapping (stored as key code strings, e.g. "Space", "KeyR")
  //==============================================================================
  static constexpr int NUM_KEY_ACTIONS =
      NUM_MIDI_ACTIONS; // Same actions as MIDI
  juce::String getKeyBinding(MidiAction action) const;
  void setKeyBinding(MidiAction action, const juce::String &keyCode);
  void clearKeyBinding(MidiAction action);

  // Thread-safe key binding state for UI (read by timer, written by UI thread)
  juce::String keyBindings[NUM_MIDI_ACTIONS]; // One per action, protected by
                                              // message thread access
  std::atomic<bool> keyBindingsChanged{true}; // Flag to resend to UI

  //==============================================================================
  // Looper transport controls (called from UI thread, apply on audio thread)
  void triggerRecord();     // Start recording / toggle overdub
  void triggerStop();       // Stop playback (keep loop) / Play if stopped
  void triggerClear();      // Clear the loop buffer
  void triggerUndo();       // Undo last overdub layer
  void triggerFootswitch(); // Ditto-style single footswitch (immediate record,
                            // no gesture)
  void triggerFootswitchDown(); // Footswitch pressed (for gesture detection
                                // from keyboard/UI)
  void triggerFootswitchUp(); // Footswitch released (for gesture detection from
                              // keyboard/UI)
  void triggerOverdub();      // Dedicated overdub (only from PLAYING/STOPPED)
  void triggerPlay();         // Dedicated Play action
  void triggerMonitorToggle();          // Toggle input monitor muting
  void setMaxLoopLength(float seconds); // Set max loop buffer size
                                        // (re-allocates on next prepareToPlay)
  float getMaxLoopLength() const; // Get current max loop length in seconds
  void
  exportLoopToWav(const juce::File &file); // Export loop buffer to WAV file

  //==============================================================================
  // Thread-safe state for UI
  std::atomic<int> looperState{static_cast<int>(LooperState::EMPTY)};
  std::atomic<float> playbackPosition{0.0f}; // 0.0 to 1.0
  std::atomic<float> inputPeakLevel{0.0f};
  std::atomic<float> outputPeakLevel{0.0f};
  std::atomic<float> loopLengthSeconds{0.0f};
  std::atomic<bool> hasUndoLayer{false};

  // Visual feedback flags (set by audio thread, consumed by UI timer)
  std::atomic<bool> undoTriggered{false};  // Flash ring orange
  std::atomic<bool> clearTriggered{false}; // Flash ring red

  // Input Monitor State
  std::atomic<bool> inputMuted{false}; // True if input is muted from output

  // Overdub Arm ("queue overdub at next loop boundary")
  std::atomic<bool> overdubArmEnabled{true}; // UI toggle state (message thread)
  std::atomic<bool> overdubArmedForUI{
      false}; // True while waiting for loop restart (for UI display)

  // Loop Mode: Classic, Bars, Dynamic
  std::atomic<int> loopMode{static_cast<int>(LoopMode::Classic)}; // UI state
  std::atomic<bool> isAutoLength{false}; // UI toggle state (message thread)

  // Max Layers: configurable cap on the compile-time MAX_LAYERS array (1–8)
  std::atomic<int> maxLayerCount{8}; // Runtime cap on overdub layers
  void setMaxLayerCount(int count);
  int getMaxLayerCount() const;

  //==============================================================================
  // Metronome / Click Track (C++ DSP — plays through plugin audio output)
  //==============================================================================
  // UI thread → audio thread settings
  std::atomic<bool> pendingMetroStart{false};
  std::atomic<bool> pendingMetroStop{false};
  std::atomic<float> metroBPMSetting{120.0f};
  std::atomic<int> metroBeatsPerBarSetting{4};
  std::atomic<int> metroNumBarsSetting{4};
  std::atomic<uint16_t> metroAccentBits{
      0x0001}; // bitmask: bit N = beat N is accent
  std::atomic<uint16_t> metroRegularBits{
      0x000E}; // bitmask: bit N = beat N is regular

  // Audio thread → UI thread feedback
  std::atomic<int> metroCurrentBeatForUI{
      -1}; // Beat index within bar (-1 = inactive)
  std::atomic<int> metroTotalBeatCount{
      0}; // Total beats since metronome started

  // Public methods (called from editor/message thread)
  void startMetronomeDSP();
  void stopMetronomeDSP();
  void setMetronomeBPM(float bpm);
  void setMetronomeBeatsPerBar(int bpb);
  void setMetronomeNumBars(int bars);
  void setMetronomePatterns(uint16_t accentBits, uint16_t regularBits);
  void setMetronomeAudible(bool audible);

  // Audio thread → UI thread feedback
  std::atomic<bool> metroAudible{true};

  //==============================================================================
  juce::AudioProcessorValueTreeState apvts;

  // UI toggle flags (set by MIDI CC on audio thread, consumed by editor timer
  // on message thread) Declared public so editor can access them
  std::atomic<bool> pendingBarModeToggle{false};
  std::atomic<bool> pendingClickToggle{false};
  std::atomic<bool> pendingPreCountToggle{false};
  std::atomic<bool> pendingOverdubArmToggle{false};
  std::atomic<bool> pendingPlayClickToggle{false};
  std::atomic<bool> pendingMonitorToggle{false};
  std::atomic<bool> pendingClassicModeToggle{
      false}; // Kept for MIDI mapping compatibility, maps to LoopModeCycle
  std::atomic<bool> pendingLoopModeCycle{false};
  std::atomic<bool> pendingSetClassicMode{false};
  std::atomic<bool> pendingSetBeatsMode{false};
  std::atomic<bool> pendingSetDynamicMode{false};
  std::atomic<bool> pendingPanInputLeft{false};
  std::atomic<bool> pendingPanInputCenter{false};
  std::atomic<bool> pendingPanInputRight{false};

private:
  //==============================================================================
  // Multi-layer loop system
  // Each layer is an independent audio recording with its own length.
  // During playback, each layer reads at (masterPosition % layerLength),
  // so shorter layers automatically tile (repeat) within the longer master
  // loop.
  //
  // Example: Layer 0 = 30s, Layer 1 = 40s (extended overdub).
  //   Master loop = 40s. At second 35:
  //     Layer 0 reads 35 % 30 = 5  (repeats)
  //     Layer 1 reads 35 % 40 = 35 (linear)
  //==============================================================================
  static constexpr float DEFAULT_MAX_LOOP_SECONDS = 60.0f;
  static constexpr float SAFETY_LIMIT_SECONDS =
      1800.0f;                         // 30 minutes hard ceiling
  static constexpr int MAX_LAYERS = 8; // Maximum number of loop layers
  std::atomic<float> maxLoopSeconds{DEFAULT_MAX_LOOP_SECONDS};

  struct LoopLayer {
    std::vector<float> bufferL;
    std::vector<float> bufferR;
    int length = 0; // 0 = unused/empty
  };

  std::array<LoopLayer, MAX_LAYERS> layers;
  int numLayers = 0;        // Number of active layers (base + overdubs)
  int masterLoopLength = 0; // Max of all layer lengths (in samples)
  int activeLayerIdx = -1;  // Layer currently being written to (-1 = none)
  bool footswitchAutoOverdub =
      false; // Footswitch single-press auto-started this overdub

  int writePosition = 0; // Current write position in the active layer
  int readPosition = 0;  // Current read/playback position

  bool overdubArmed =
      false; // Armed: waiting for loop boundary to start overdub

  double currentSampleRate = 44100.0;
  int maxLoopSamples = 0;

  // State machine
  LooperState currentState = LooperState::EMPTY;
  float currentMaxAllocatedSeconds = 0.0f; // Track actual allocated buffer size

  // Pending commands from UI thread
  std::atomic<bool> pendingRecord{false};
  std::atomic<bool> pendingStop{false};
  std::atomic<bool> pendingClear{false};
  std::atomic<bool> pendingUndo{false};
  std::atomic<bool> pendingFootswitchDown{
      false}; // Keyboard/UI footswitch press
  std::atomic<bool> pendingFootswitchUp{
      false}; // Keyboard/UI footswitch release
  std::atomic<bool> pendingOverdub{false};
  std::atomic<bool> pendingPlay{false};

  // Footswitch timing state (for single/double/long press detection)
  // Ditto-style: single press acts IMMEDIATELY on press-down, double-press on
  // 2nd press, long press detected WHILE held (triggers action + visual
  // feedback before release)
  static constexpr int LONG_PRESS_MS = 500;
  static constexpr int DOUBLE_PRESS_MS = 300;
  bool footswitchHeld = false;      // CC footswitch is currently held
  bool uiFootswitchHeld = false;    // Keyboard/UI footswitch currently held
  int64_t footswitchPressStart = 0; // Sample count when press started
  int64_t lastFootswitchPressTime =
      0; // Sample count of last rising edge (for double press)
  int64_t totalSamplesProcessed = 0; // Running sample counter
  bool longPressTriggered =
      false; // Prevent re-triggering long press while held

  // Crossfade for smooth transitions
  static constexpr int CROSSFADE_SAMPLES = 256;

  //==============================================================================
  // Metronome DSP internals (audio thread only — NOT atomic)
  bool clickActive = false;          // Metronome is currently running
  double samplesPerBeat = 0.0;       // Samples between beats at current BPM
  double samplesSinceLastBeat = 0.0; // Running counter within current beat
  int metroBeat = 0;                 // Current beat within pattern
  int metroTotalBeats = 0;           // Running total of beats since start

  // Click oscillator/envelope state
  float clickPhase = 0.0f;       // Oscillator phase (0-1)
  float clickFreq = 0.0f;        // Current click frequency (Hz)
  float clickStartGain = 0.0f;   // Click gain at onset
  int clickSamplesRemaining = 0; // Samples left in current click
  int clickTotalSamples = 0;     // Total samples for current click duration

  void processMetronome(float *outL, float *outR, int numChannels,
                        int numSamples);

  //==============================================================================
  // MIDI CC Mapping
  std::array<std::atomic<int>, NUM_MIDI_ACTIONS>
      midiCCMap; // CC number per action (-1 = none)
  std::atomic<bool> midiLearnActive{false};
  std::atomic<int> midiLearnTarget{
      CC_UNASSIGNED}; // Which MidiAction is learning
  std::array<bool, NUM_MIDI_ACTIONS> ccWasHigh{}; // Edge detection per action

  void processMidiMessages(const juce::MidiBuffer &midi);

  //==============================================================================
  void processStateChanges();
  void handleRecordTrigger();
  void handleOverdubTrigger();
  void handleFootswitchSinglePress();
  void handleFootswitchDoublePress();
  void handleFootswitchLongPress();

  juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OrbitLooperAudioProcessor)
};
