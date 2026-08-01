# Orbit Looper — Technical Documentation

Detailed architecture and implementation reference for contributors and developers.

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [File Structure](#file-structure)
- [State Machine](#state-machine)
- [Multi-Layer Loop System](#multi-layer-loop-system)
- [DSP Pipeline](#dsp-pipeline)
- [Metronome DSP](#metronome-dsp)
- [Overdub Arm System](#overdub-arm-system)
- [WebView Integration](#webview-integration)
- [MIDI CC Mapping](#midi-cc-mapping)
- [Keyboard Bindings](#keyboard-bindings)
- [Footswitch Gesture Detection](#footswitch-gesture-detection)
- [Pre-Count System](#pre-count-system)
- [Resizable Window](#resizable-window)
- [State Persistence](#state-persistence)
- [Cross-Platform Build](#cross-platform-build)
- [Threading Model](#threading-model)
- [Known Constraints](#known-constraints)

---

## Architecture Overview

```
┌─────────────────────────────────────────────┐
│                  DAW Host                    │
│  (Reaper, Ableton, FL Studio, etc.)         │
└────────────┬────────────────────┬───────────┘
             │ Audio Buffers      │ MIDI
             ▼                    ▼
┌─────────────────────────────────────────────┐
│        OrbitLooperAudioProcessor            │
│  ┌──────────┐ ┌──────────┐ ┌────────────┐  │
│  │  State   │ │  Multi-  │ │ Metronome  │  │
│  │ Machine  │ │  Layer   │ │    DSP     │  │
│  │          │ │  Looper  │ │            │  │
│  └──────────┘ └──────────┘ └────────────┘  │
│  ┌──────────┐ ┌──────────┐ ┌────────────┐  │
│  │  MIDI CC │ │ Footsw.  │ │  Overdub   │  │
│  │ Mapping  │ │ Gestures │ │    Arm     │  │
│  └──────────┘ └──────────┘ └────────────┘  │
│          │  atomics  │                      │
└──────────┼───────────┼──────────────────────┘
           ▼           ▼
┌─────────────────────────────────────────────┐
│     OrbitLooperAudioProcessorEditor         │
│  ┌──────────────────────────────────────┐   │
│  │    juce::WebBrowserComponent         │   │
│  │  ┌──────────────────────────────┐    │   │
│  │  │  index.html, style.css,      │    │   │
│  │  │  7 JS modules (BinaryData)   │    │   │
│  │  │  Transport, Knob, Meters,   │    │   │
│  │  │  MIDI Panel, Metronome UI   │    │   │
│  │  └──────────────────────────────┘    │   │
│  └──────────────────────────────────────┘   │
│  Timer (30Hz) ←→ evaluateJavascript()       │
│  Event Listeners ←→ emitEvent()             │
└─────────────────────────────────────────────┘
```

**Framework**: JUCE 8.0.12, C++20, CMake 3.22+
**UI**: Modular HTML, CSS, and JS files served via JUCE's resource provider via BinaryData
**Bridge**: Bidirectional — C++ calls `evaluateJavascript()` at 30Hz, JS calls `window.__JUCE__.backend.emitEvent()` for actions

---

## File Structure

```
OrbitLooper/
├── CMakeLists.txt                 # Desktop build (FetchContent for JUCE, patches)
├── HANDOVER.md                    # Session handover / working notes
├── docs/
│   ├── Technical.md               # This file
│   ├── TechnicalAndroid.md        # Android specifics (build, audio, BT MIDI)
│   └── UserGuide.md               # End-user guide
├── android/                       # Gradle wrapper project (wraps CMake)
├── .github/workflows/build.yml    # CI/CD pipeline
└── Source/
    ├── PluginProcessor.h/.cpp     # DSP, state machine, MIDI, lazy layer storage
    ├── PluginEditor.h/.cpp        # WebView setup, 30Hz timer, Android housekeeping
    ├── BluetoothClassicMidi.h/.cpp# BT Classic MIDI: SPSC queue + JNI bridge (Android)
    ├── patches/                   # JUCE source patches (Linux WebView, juceaide)
    └── ui/
        ├── OrbitLooperWebBrowser.h/.cpp  # Resource provider & JS event listeners
        └── public/
            ├── index.html             # Desktop UI
            ├── mobile-index.html      # Android UI (bottom nav, mute banner)
            ├── style.css              # ALL CSS (shared + body.mobile overrides)
            ├── juce-bridge.js         # SliderState class, JUCE event helpers
            ├── state.js               # Constants, DOM refs, shared mutable state
            ├── ui-controls.js         # Gain/pan sliders, text editing, confirm dialog
            ├── transport.js           # State updates, transport buttons, playback ring
            ├── metronome.js           # Metronome engine, rhythm matrix, beat feedback
            ├── keymapping.js          # MIDI CC panel, key bindings, keyboard dispatch
            ├── main.js                # Logging, zoom, modals, settings, init & uiReady
            └── mobile.js              # Android-only layout/init (loads after main.js)
```

---

## State Machine

The looper operates as a finite state machine with 5 states:

```
                 ┌──────────┐
        Record   │  EMPTY   │
       ┌────────▶│          │◀──── Clear (from any state)
       │         └────┬─────┘
       │              │ Record
       │              ▼
       │         ┌──────────┐
       │         │RECORDING │
       │         │          │
       │         └────┬─────┘
       │              │ Record / Stop / Footswitch
       │              ▼
       │         ┌──────────┐
  ┌────┼────────▶│ PLAYING  │◀─────────┐
  │    │    ┌───▶│          │───┐      │
  │    │    │    └────┬─────┘   │      │
  │    │    │         │         │      │
  │    │    │ Record/ │   Stop  │      │
  │    │    │ Overdub │         │      │
  │    │    │         ▼         ▼      │
  │    │    │   ┌───────────┐ ┌──────┐ │
  │    │    │   │OVERDUBBING│ │STOPPED│ │
  │    │    └───│           │ │      │─┘
  │    │        └───────────┘ └──────┘
  │    │         Record/Stop   Stop(toggle)
  │    │
  │    └── Undo (removes layer, stays PLAYING)
  └─── Stop from OVERDUBBING → PLAYING
```

### State Enum

```cpp
enum class LooperState {
    EMPTY = 0,      // No loop — waiting for first record
    RECORDING,      // Recording first loop layer
    PLAYING,        // Playing back the loop
    OVERDUBBING,    // Adding new layer on top
    STOPPED         // Loop exists but paused
};
```

State is stored as `std::atomic<int> looperState` for thread-safe UI access.

---

## Multi-Layer Loop System

### Architecture

Instead of a single buffer with destructive overdub, the looper uses up to 8 independent layers:

```cpp
static constexpr int MAX_LAYERS = 8;

struct LoopLayer {
    std::vector<float> bufferL;
    std::vector<float> bufferR;
    int length = 0;  // 0 = unused
};

std::array<LoopLayer, MAX_LAYERS> layers;
int numLayers = 0;           // Active layer count
int masterLoopLength = 0;    // Max of all layer lengths (samples)
int activeLayerIdx = -1;     // Layer being written to
```

### Modulo Playback

Each layer plays back at its own modulo position:

$$\text{Position}_{\text{layer}} = \text{readPosition} \mod \text{layer.length}$$

This means shorter layers automatically repeat (tile) within the master loop cycle.

**Example**: Layer 0 = 30s, Layer 1 = 40s (extended overdub).
- Master loop = 40s
- At second 35: Layer 0 reads `35 % 30 = 5`, Layer 1 reads `35 % 40 = 35`

### Layer Length Finalization

When stopping an overdub:
- **Extended** (`writePosition > masterLoopLength`): `layer.length = writePosition` → new master length
- **Non-extended** (`writePosition ≤ masterLoopLength`): `layer.length = masterLoopLength` (zero-padded)

### Undo

Undo removes the top layer: `numLayers--`, recalculate `masterLoopLength = max(all layer lengths)`. No separate undo buffer — layers are independent. The retired slot is zeroed by the message-thread allocator before it can be reused (its published capacity is set to 0 so the audio thread cannot claim it early).

### Memory (lazy layer storage)

Layers are NOT pre-allocated. Storage rules:

- **Base layer** (layer 0): allocated in `prepareToPlay` at the Global Max
  Length ("tape length"). Default 300 s / 48 kHz stereo ≈ 115 MB.
- **Overdub layers**: allocated on the MESSAGE thread (`handleAsyncUpdate`),
  one spare ahead of use, sized to the actual master loop (Classic/Bars) or
  Global Max (Dynamic). A 30 s loop layer costs ~11.5 MB.
- The audio thread never allocates. It claims a spare via a capacity gate
  with a publish/re-validate handshake (`startOverdubLayer` ↔
  `ensureLayerStorage`); if the spare isn't ready (allocation in flight),
  the overdub trigger is dropped and can simply be pressed again.
- Undo/clear retire slots (capacity → 0, `needsCleanup`) instead of
  memsetting on the audio thread; the allocator zeroes/frees them async.

---

## DSP Pipeline

### processBlock Flow

```
Input Buffer
     │
     ├─── Input Gain & Pan (linear scaling)
     │
     ├─── Process MIDI Messages (CC mapping, learn, footswitch)
     │
     ├─── Process State Changes (pending commands from UI/MIDI)
     │
     ├─── State-Dependent Processing:
     │    ├── EMPTY: Pass-through
     │    ├── RECORDING: Write input to layer[0], pass-through
     │    ├── PLAYING: Sum all layers at tiled positions → output + dry input
     │    ├── OVERDUBBING: Sum existing layers + write input to active layer
     │    └── STOPPED: Pass-through (loop kept, not played)
     │
     ├─── Loop Level: Scale playback by loop_level parameter
     │
     ├─── Output Gain & Pan (linear scaling)
     │
     ├─── Metronome: Mix click into output (AFTER gain, never recorded)
     │
     ├─── Peak Level Meters (input post-gain, output post-all)
     │
     └─── Output Buffer
```

---

## Mono Input Routing

The looper includes an automatic **Mono-to-Stereo** detection system to handle single-input instruments (e.g., guitar) connected to one hardware channel of a stereo interface.

### RMS-Based Detection

Since some drivers (like WASAPI) always report both input channels as active, the looper uses per-channel RMS energy to detect mono signals:

1. **Measurement**: Calculate the RMS of both input channels.
2. **Comparison**:
    - If **Channel 0** < -96dB RMS AND **Channel 1** > -96dB RMS → **Duplicate Ch1 to Ch0**.
    - If **Channel 1** < -96dB RMS AND **Channel 0** > -96dB RMS → **Duplicate Ch0 to Ch1**.
3. **Threshold**: The `-96dB` threshold (≈ 0.0000158) is tuned to be above standard hardware noise floor but below the quietest musical signals.

This ensures that a single input is always recorded and monitored as a center-panned dual-mono signal, preventing the "one ear only" problem without requiring manual configuration.

---

### Parameters (APVTS)

| ID | Type | Range | Default | Unit |
|----|------|-------|---------|------|
| `loop_level` | Float | 0–100 | 95 | % |
| `input_gain` | Float | -60–12 | 0 | dB |
| `output_gain` | Float | -60–12 | 0 | dB |
| `input_pan` | Float | -1.0–1.0 | 0 | - |
| `output_pan` | Float | -1.0–1.0 | 0 | - |

> Loop length is not a host parameter: the **Global Max Length** (Settings
> modal, persisted in plugin state as `maxLoopSeconds`, default 300 s) is the
> single loop-length authority — it is the Classic-mode "tape length" and the
> per-layer allocation ceiling.

---

## Metronome DSP

The metronome runs entirely in C++ `processBlock`, synthesizing clicks and mixing them into the output buffer after all loop DSP and gain processing.

### Synthesis

```
Beat trigger (every samplesPerBeat samples)
     │
     ├── Check pattern bitmasks (metroAccentBits, metroRegularBits)
     │    ├── Accent hit: 1200Hz triangle, 60ms, gain 0.6
     │    └── Regular hit: 800Hz sine, 50ms, gain 0.4
     │
     └── Exponential decay envelope: gain × e^(-t × 6)
```

### Timing

$$\text{samplesPerBeat} = \frac{\text{sampleRate} \times 60}{\text{BPM}}$$

Beat scheduling is sample-accurate — no Web Audio lookahead complexity.

### Pattern Bitmasks

Two `uint16_t` atomics store the rhythm pattern:
- `metroAccentBits`: bit N = beat N is accent (high click)
- `metroRegularBits`: bit N = beat N is regular (low click)

JS packs boolean pattern arrays into these bitmasks via `sendPatternBits()`.

### UI ↔ C++ Bridge

| Direction | Mechanism |
|-----------|-----------|
| JS → C++ | Event listeners: `metroStart`, `metroStop`, `metroSetBPM`, `metroSetBeatsPerBar`, `metroSetPatterns` |
| C++ → JS | Timer sends `window.updateMetroBeat(beatInBar, totalBeats)` at 30Hz |

---

## Overdub Arm System

Overdub Arm queues an overdub to start at the next loop boundary instead of immediately.

### Design

**ARMED is NOT a new LooperState** — it's a visual overlay on the PLAYING state using boolean flags:
- `overdubArmed` (private, audio thread): actual arm state
- `overdubArmedForUI` (atomic): UI display flag
- `overdubArmEnabled` (atomic): toggle state from UI

### Boundary Detection

In `processBlock` during PLAYING state:

```cpp
int prevPos = readPosition;
readPosition = (readPosition + 1) % masterLoopLength;

if (overdubArmed && readPosition < prevPos) {
    // Loop boundary crossed — start overdub
    allocateNewLayer();
    currentState = LooperState::OVERDUBBING;
    overdubArmed = false;
}
```

### Trigger Logic (3-way)

When an overdub trigger arrives during PLAYING:
1. If already armed → **disarm** (cancel)
2. If arm toggle enabled → **arm** (wait for boundary)
3. Else → **immediate overdub** (original behavior)

---

## WebView Integration

### Critical Design Decisions

1. **No External ES6 Modules** — JUCE's resource provider doesn't support ES6 module CORS. Multiple `<script src="...">` tags load 7 JS files in dependency order (juce-bridge → state → ui-controls → transport → metronome → keymapping → main). All modules share a single global scope.

2. **WebView backend is platform-conditional** — `.withBackend(Backend::webview2)` and `.withWinWebView2Options(...)` are wrapped in `#if JUCE_WINDOWS` / `#endif`. On Linux (WebKitGTK) and macOS (WKWebView), JUCE uses the default backend. Hardcoding WebView2 on all platforms causes a silent white window on Linux.

3. **Load explicit entry URL** — The editor navigates to `${resourceRoot}/index.html` instead of only the root URL. This avoids backend-dependent root resolution edge cases that can return an empty/white page.

4. **Resource path sanitization** — Resource requests strip query strings/fragments. Pure file URLs must be correctly handled by checking if `url.startsWithIgnoreCase(root)` to strip the root, rather than falling back to `index.html` for valid sub-assets like `style.css`.

5. **Member destruction order** (prevents DAW crash):
   ```cpp
   // PluginEditor.h — order matters!
   juce::WebSliderRelay loopLevelRelay;        // 1. Destroyed LAST
   std::unique_ptr<juce::WebBrowserComponent> webView;  // 2. Destroyed MIDDLE
   std::unique_ptr<juce::WebSliderParameterAttachment> loopLevelAttachment;  // 3. Destroyed FIRST
   ```

6. **Resource provider** — Serves the HTML/CSS/JS from JUCE BinaryData via a
   generic lookup over `BinaryData::originalFilenames` (no per-file mapping).
   Unknown `.html` paths fall back to `index.html`; anything else 404s.

7. **Deferred initial navigation** — The editor defers `goToURL()` until it
   has a non-zero size (`resized()`), with a timer fallback, avoiding
   backend-dependent races on slow WebView startup.

8. **Linux WebKit safety env** — Before creating the WebView, Linux sets `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`, `WEBKIT_DISABLE_DMABUF_RENDERER=1`, and `WEBKIT_DISABLE_COMPOSITING_MODE=1` to reduce white-surface failures in some host/driver combinations.

9. **JS injection safety** — every dynamic string embedded in
   `evaluateJavascript()` goes through `jsQuoted()` (JSON escaping).

10. **JS load-order rule** — the UI files execute top-level code at load.
    Shared globals MUST be declared in `state.js` (first-loaded); a
    TypeError at load silently kills everything defined later in that file.

### C++ → JS (Timer, 30Hz)

The editor timer pushes state via `evaluateJavascript()`, split into named
sub-tasks (`pushLooperState`, `pushMidiState`, `pushKeyBindings`,
`pushMetronomeBeat`, `pushMidiActivity`, `androidTimerTasks`):

- `updateLooperState(state, position, inPeak, outPeak, loopLen, canUndo, recordBasisSec, flashType, isArmed, isMuted, loopMode)` — every tick; the JS side render-caches and skips unchanged DOM writes
- `updateMidiState(cc0..cc19, learning, learnTarget)` — only when the `midiStateChanged` dirty flag is set
- `updateKeyBindings(k0..k19)` — only when `keyBindingsChanged` is set
- `updateMetroBeat(beatInBar, totalBeats)` / `updateMidiActivity(active)` — every tick

### JS → C++ (Events)

User actions emit events consumed by C++ event listeners:

```javascript
window.__JUCE__.backend.emitEvent("looperRecord", {});
window.__JUCE__.backend.emitEvent("looperStop", {});
window.__JUCE__.backend.emitEvent("metroSetBPM", { value: 120.0 });
```

### Event Listener Catalog

| Category | Events |
|----------|--------|
| Transport (7) | `looperRecord`, `looperStop`, `looperClear`, `looperUndo`, `looperExport`, `looperOverdub`, `looperPlay` |
| Overdub Arm (1) | `setOverdubArm` |
| Loop Mode (1) | `setLoopMode` |
| Monitor (1) | `looperMonitor` |
| Footswitch (3) | `looperFootswitch`, `looperFootswitchDown`, `looperFootswitchUp` |
| MIDI Learn/Clear (40) | `midiLearn<Action>` / `midiClear<Action>` — registered in a loop over `getMidiActionNames()` (one pair per action) |
| MIDI Cancel (1) | `midiLearnCancel` |
| Key Bindings (2) | `keyBindSet`, `keyBindClear` |
| Metronome (7) | `metroStart`, `metroStop`, `metroSetBPM`, `metroSetBeatsPerBar`, `metroSetNumBars`, `metroSetPatterns`, `metroSetAudible` |
| Settings (3) | `setGlobalMaxLength`, `setMaxLayers`, `setMuteOnStartup` |
| Window/System (4) | `resizeWindow`, `toggleFullscreen`, `openAudioSettings`, `uiReady` |
| Standalone mute (2) | `standaloneInputMute`, `standaloneInputUnmute` |
| BT Classic, Android (3) | `btClassicScan`, `btClassicConnect`, `btClassicDisconnect` |
| Diagnostics (2) | `jsDiag`, `jsError` |

---

## MIDI CC Mapping

### MidiAction Enum

```cpp
enum class MidiAction {
    Record = 0,
    Stop,           // Play/Stop toggle
    Clear,
    Undo,
    Footswitch,     // Ditto-style (single/double/long press)
    Overdub,        // Dedicated overdub trigger
    BarMode,        // UI toggle
    Click,          // UI toggle
    PreCount,       // UI toggle
    ArmOverdub,     // UI toggle (default: ON)
    PlayClick,      // Play/Stop Click — manual metronome audition (UI-only)
    Play,           // Dedicated Play action (starts playback if stopped)
    Monitor,        // Toggle Input Monitoring (UI-only)
    LoopModeCycle,  // Cycle Loop Mode: Classic → Bars → Dynamic (UI-only)
    ClassicMode,    // Set Loop Mode to Classic (UI-only)
    BeatsMode,      // Set Loop Mode to Beats/Bars (UI-only)
    DynamicMode,    // Set Loop Mode to Dynamic (UI-only)
    PanInputLeft,   // Set Input Pan to full Left (UI-only)
    PanInputCenter, // Set Input Pan to Center (UI-only)
    PanInputRight,  // Set Input Pan to full Right (UI-only)
    NUM_ACTIONS     // = 20
};
```

### Edge Detection

CC values are edge-triggered to work with momentary footswitches:
- **Rising edge**: CC value ≥ 1 AND previous value was 0 → trigger action
- Any non-zero value counts as "pressed" (supports 0/1-style foot controllers)
- Each action tracks its own `ccWasHigh[action]` state
- Threshold constant: `CC_TRIGGER_THRESHOLD = 1`

### MIDI Learn

1. `startMidiLearn(action)` sets `midiLearnActive = true, midiLearnTarget = action`
2. `processMidiMessages()` on audio thread completes the learn on ANY CC
   message regardless of value (supports controllers that send val=0 on release)
3. First matching CC is assigned, learn mode exits
4. Auto-unassign: if CC was used by another action, that mapping is cleared

### UI Toggle Actions

BarMode, Click, PreCount, ArmOverdub, PlayClick, Monitor, LoopModeCycle, ClassicMode, BeatsMode, DynamicMode, and PanInput (Left/Center/Right) are UI-only toggles. When triggered via MIDI CC:
1. Audio thread sets `pendingBarModeToggle` (etc.) atomic to `true`
2. Editor timer (30Hz) reads and consumes the flag
3. Calls `evaluateJavascript("window.toggleBarMode()")` to toggle the UI element

---

## Keyboard Bindings

### Storage

```cpp
juce::String keyBindings[20];  // One per MidiAction, stores e.code values
```

Key codes are JavaScript `KeyboardEvent.code` strings: `"Space"`, `"KeyR"`, `"Digit1"`, etc.

### Dispatch (JS)

`keymapping.js` dispatches keydown through a table: `KEY_ACTION_HANDLERS`
maps action indices with dedicated behavior (record w/ pre-count, stop w/
pre-count cancel, UI toggles, loop-mode sets, pan shortcuts); anything not
in the table falls back to `emitEvent(ACTION_EVENTS[i])`. Index 4
(footswitch) is gesture-based and handled separately (below).

### Footswitch Special Handling

Action 4 (Footswitch) uses separate down/up events for gesture detection:
- `keydown` (no repeat) → `looperFootswitchDown`
- `keyup` → `looperFootswitchUp`

---

## Footswitch Gesture Detection

### Timing Constants

```cpp
static constexpr int LONG_PRESS_MS = 500;
static constexpr int DOUBLE_PRESS_MS = 300;
```

### Detection Flow (Audio Thread)

```
CC/Key rising edge
     │
     ├── Record footswitchPressStart timestamp
     ├── Execute single-press action IMMEDIATELY
     │   (Record → Overdub → Play → Stop cycle)
     │
     ├── While held: check if (now - pressStart) > 500ms
     │   └── Long press: Undo (playing/overdubbing)
     │
     └── On 2nd rising edge within 300ms of 1st:
         ├── Double press: Stop (with layer finalization if overdubbing)
         └── While 2nd press held: check if (now - 2ndPressStart) > 500ms
             └── 2x Tap and Hold: Stop & Clear (from any active state)
```

### Footswitch Auto-Overdub Tracking

When footswitch single-press starts an overdub from PLAYING, `footswitchAutoOverdub` is set. If long-press undo follows, it removes **two layers** (the incidental + the one the user wanted to undo).

---

## Pre-Count System

Pre-count is implemented entirely in JavaScript, using the C++ metronome for audio.

### Flow

```
Record trigger (any source)
     │
     ├── Is EMPTY + preCountEnabled?
     │   ├── YES: Start pre-count
     │   │   ├── Start metronome DSP (metroStart event)
     │   │   ├── Show "COUNT IN X" text (amber pulsing)
     │   │   ├── Track beats via updateMetroBeat()
     │   │   ├── After N bars: emit looperRecord, stop metronome (if click OFF)
     │   │   └── preCountJustCompleted flag allows next EMPTY→RECORDING
     │   │
     │   └── NO: Record immediately
     │
     └── MIDI/External bypass handling:
         If C++ starts recording before JS can intercept:
         1. JS detects EMPTY→RECORDING + loopLength=0 + no justCompleted flag
         2. Sends looperClear to abort recording
         3. Starts pre-count sequence
```

---

## Resizable Window

Desktop only (Android is fixed fullscreen). The editor uses a
`ComponentBoundsConstrainer` with a locked 580×720 aspect ratio; `resized()`
computes a zoom factor from the current size and pushes it to JS
(`window.setZoom`), which scales `.app-wrapper` via CSS layout `zoom`. Layout
zoom forces WebView2/WebKit to rerasterize text, SVGs, borders, and controls at
the current editor size; compositor `transform: scale()` must not be used here
because it can upscale the raster layer created at the startup resolution.
A custom JS drag handle (`#customResizer` in `main.js`) projects the mouse
delta onto the aspect diagonal and emits `resizeWindow(w, h)` back to C++
(native JUCE resizer handles are obscured by the WebView). On Android,
the shared scaler exits immediately and `mobile.js` replaces `setZoom` with a
no-op. It sizes the wrapper, fixed bottom navigation, mute banner, and modals to
the visible portrait area, using the screen's short edge so a landscape sensor
state during portrait-locked startup cannot widen the UI.

---

## State Persistence

`getStateInformation`/`setStateInformation` persist SETTINGS ONLY — loop
audio is NOT saved. Stored via `juce::ValueTree` (identifier `OrbitLooper`),
serialized to XML:

| Property | Type | Description |
|----------|------|-------------|
| APVTS parameters | — | loop_level, gains, pans, loop_mode |
| `maxLoopSeconds` | float | Global Max Length (single length authority) |
| `midiCC_<action>` | int | MIDI CC assignments — keys generated from `getMidiActionNames()` lowercased (20 actions) |
| `keyBind_0` … `keyBind_19` | String | Key bindings |
| `loopMode` | int | Classic / Bars / Dynamic |
| `maxLayerCount` | int | Runtime layer cap (1–8) |
| `muteOnStartup`, `inputMuted` | int | Mute states |

Additionally, Android persists app-level settings (muteOnStartup,
btClassicLastDevice, bufferOptimized) in a `PropertiesFile`
(`makeSettingsOptions()`), and the standalone holder saves plugin + audio
device state every ~5 s (the OS can kill the app without destructors).

---

## Cross-Platform Build

### CMake Configuration

JUCE is fetched via `FetchContent_Declare`:

```cmake
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.12
    GIT_SHALLOW    TRUE
)
```

**ASIO**: JUCE 8.0.11+ bundles the Steinberg ASIO headers, so `JUCE_ASIO=1` works out of the box on Windows with no external SDK download.

### Platform Detection

| Platform | Formats | WebView Flags | Extra Definitions |
|----------|---------|---------------|-------------------|
| Windows | VST3, Standalone | `NEEDS_WEBVIEW2=TRUE` | `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`, `JUCE_ASIO=1` (bundled headers) |
| macOS | VST3, AU, Standalone | (neither) | `JUCE_USE_CURL=0` |
| Linux | VST3, LV2, Standalone | `NEEDS_WEB_BROWSER=TRUE` | `JUCE_USE_CURL=0`, `JUCE_JACK=1` |

**Linux critical note**: `NEEDS_WEB_BROWSER=TRUE` is required for webkit2gtk + GTK include paths. Without it, the build fails with `gtk/gtk.h: No such file or directory`.

### CI/CD Pipeline

GitHub Actions workflow (`.github/workflows/build.yml`) triggers on `v*` tags or manual dispatch. **5 parallel jobs**:

| Job | Runner | Architecture | Formats |
|-----|--------|--------------|---------|
| `build-windows-x64` | `windows-latest` | x64 | VST3, Standalone |
| `build-windows-arm64` | `windows-latest` | ARM64 (cross-compile) | VST3, Standalone |
| `build-macos` | `macos-latest` | Universal (x64 + ARM64) | VST3, AU, Standalone |
| `build-linux-x64` | `ubuntu-latest` | x64 | VST3, LV2, Standalone |
| `build-linux-arm64` | `ubuntu-24.04-arm` | ARM64 (native) | VST3, LV2, Standalone |

**Windows CI note**: The `Microsoft.Web.WebView2` NuGet package (v1.0.3485.44) must be installed before CMake configure — GitHub Actions runners don't have it pre-installed.

**Windows ARM64 CI note**: JUCE's `FindWebView2.cmake` checks `CMAKE_SYSTEM_PROCESSOR` for lowercase `arm64`. If this value is `ARM64` (uppercase), JUCE may incorrectly pick the x64 static loader lib and fail linking with unresolved `CreateCoreWebView2EnvironmentWithOptions`. The project now normalizes ARM64 detection in `CMakeLists.txt` and sets the workflow configure arg to `-DCMAKE_SYSTEM_PROCESSOR=arm64`.

**Linux WebView troubleshooting note**: If the plugin opens to a white window, run a Debug build and check `DBG` output from `PluginEditor.cpp` for `Resource root`, `Initial URL`, and `resource requested` lines. These logs confirm whether the WebView is loading `index.html` and whether resource path normalization is working.

**Linux CI note**: LV2 and Standalone targets must be built under `xvfb-run` because JUCE's LV2 manifest generator loads the plugin (which inits GTK/WebKit), and CI runners are headless.

A `release` job collects all artifacts and creates a GitHub Release with zip archives.

---

## Threading Model

### Audio Thread (processBlock)
- All DSP: loop read/write, state machine, MIDI processing, metronome synthesis
- Reads pending command atomics from UI thread
- Writes state atomics for UI thread

### Message Thread (Editor)
- Timer callback (30Hz): reads state atomics, pushes to JS via `evaluateJavascript()`
- Event listeners: receives JS events, calls processor trigger methods
- WebView rendering and interaction

### Thread Safety
- All cross-thread communication via `std::atomic<>` (bool, int, float)
- No mutexes in the audio path (the BT Classic status strings use a mutex on
  the message/JNI side only; MIDI bytes flow through a lock-free SPSC queue)
- Layer storage handshake: allocator retires a layer by publishing capacity 0
  before touching it; the audio thread re-validates capacity after publishing
  a claim — one side always yields
- Key bindings array accessed only from message thread

---

## Technical Considerations & Architecture Notes

Information for developers looking to maintain or extend the plugin.

### Development Constraints & "Gotchas"
1. **No External ES6 Modules** — The UI uses standard CSS/JS served by JUCE 8's `BinaryData`. While JUCE 8 supports modern JS, we avoid external `import`/`export` to ensure zero-latency UI loading and unified compatibility across all OS-native WebView backends without CORS edge cases. All files share ONE global scope — declare shared globals in `state.js` (first loaded).
2. **Real-time Safety** — The audio thread (`processBlock`) is strictly no-allocation. Layer storage is managed by the message-thread allocator (`handleAsyncUpdate`); the audio thread only claims published capacity. Communication with the UI is handled via lock-free atomics.
3. **Memory Footprint** — Lazy: base layer ≈ 23 MB/min of Global Max (48 kHz stereo); each overdub layer costs memory proportional to the actual loop length (Dynamic mode layers reserve up to the Global Max).
4. **Undo Strategy** — Layers are independent. "Undo" simply decrements the active layer count and recalculates the master loop length; the retired slot is zeroed asynchronously before reuse.

### Architecture Highlights
- **State Push Model** — Instead of the UI "polling" the processor, the Editor Timer (30Hz) "pushes" state to JavaScript via `evaluateJavascript()`. This ensures the UI is always a reflection of the latest atomic values from the DSP.
- **Native ASIO (Windows)** — Developers do NOT need the external Steinberg SDK. JUCE 8.0.11+ bundles the compatible headers, making the Windows build "clone and compile" ready.
- **Cross-Platform WebView** — We avoid hardcoding the backend (e.g., WebView2) to allow JUCE to select the most efficient native engine: WKWebView on macOS, WebKitGTK on Linux.
- **Mono-to-Stereo Intelligence** — The looper automatically detects if an instrument is plugged into only one side of a stereo interface and duplicates the signal, solving the "one ear" monitoring problem for guitarists.
- **Custom JS Resizing** — Standard JUCE resizer handles are often obscured by the native WebView window. Orbit Looper uses a custom JS-based dragger that communicates window dimensions back to the C++ host to resize the native plugin window.

---

## Future Roadmap
- [ ] **CLAP Support** — Native CLAP format support.
- [ ] **Multi Track** — Support for multiple tracks.
