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
├── CMakeLists.txt                 # Build config (FetchContent for JUCE)
├── LICENSE                        # MIT license
├── README.md                      # User documentation
├── docs/
│   └── Technical.md               # This file
├── .github/
│   └── workflows/
│       └── build.yml              # CI/CD pipeline
└── Source/
    ├── PluginProcessor.h          # DSP class header (~290 lines)
    ├── PluginProcessor.cpp        # DSP implementation (~1500 lines)
    ├── PluginEditor.h             # Editor header (~75 lines)
    ├── PluginEditor.cpp           # WebView setup + timer (~440 lines)
    ├── ui/
    │   ├── OrbitLooperWebBrowser.h    # WebView sub-component header
    │   ├── OrbitLooperWebBrowser.cpp  # Resource provider & JS event relay
    │   └── public/
    │       ├── index.html             # Main HTML structure + modal shells
    │       ├── style.css              # Custom CSS styling
    │       ├── juce-bridge.js         # SliderState class, JUCE event helpers
    │       ├── state.js               # Constants, DOM refs, shared mutable state
    │       ├── ui-controls.js         # Gain/pan sliders, text editing, confirm dialog
    │       ├── transport.js           # State machine, transport buttons, playback ring
    │       ├── metronome.js           # Metronome engine, rhythm matrix, beat feedback
    │       ├── keymapping.js          # MIDI CC panel, key bindings, keyboard dispatch
    │       └── main.js                # Logging, zoom, modals, settings, init & uiReady
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

Undo removes the top layer: `numLayers--`, recalculate `masterLoopLength = max(all layer lengths)`. No separate undo buffer — layers are independent.

### Memory

All 8 layers pre-allocated in `prepareToPlay()`. At default 60s / 48kHz:

$$8 \times 2 \times 60 \times 48000 \times 4 \text{ bytes} \approx 184 \text{ MB}$$

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
     │    ├── PLAYING: Sum all layers at modulo positions → output
     │    ├── OVERDUBBING: Sum existing layers + write input to active layer
     │    └── STOPPED: Silence output
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
| `max_loop_length` | Int | 0–1800 | 60 | seconds |
| `input_gain` | Float | -60–12 | 0 | dB |
| `output_gain` | Float | -60–12 | 0 | dB |
| `input_pan` | Float | -1.0–1.0 | 0 | - |
| `output_pan` | Float | -1.0–1.0 | 0 | - |

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

6. **Resource provider** — Serves `index.html`, `style.css`, and 7 JS modules from JUCE BinaryData. Each file maps to a `BinaryData::` symbol (hyphens removed, dots → underscores). No external file loading.

7. **Linux startup fallback chain** — If `uiReady` is not received, the editor retries navigation through multiple URL strategies in order: resource-provider URL, temp-file `file://` URL, then `data:text/html` URL built from embedded binary data.

8. **Linux WebKit safety env** — Before creating the WebView, Linux sets `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`, `WEBKIT_DISABLE_DMABUF_RENDERER=1`, and `WEBKIT_DISABLE_COMPOSITING_MODE=1` to reduce white-surface failures in some host/driver combinations.

### C++ → JS (Timer, 30Hz)

The editor timer calls `evaluateJavascript()` to push state:

```cpp
// State: looperState, position, inputPeak, outputPeak, loopLength, hasUndo, flashType, armed
webView->evaluateJavascript("window.updateLooperState(2, 0.45, 0.8, 0.6, 5.2, true, 0, false)");

// MIDI CCs: 10 values + learning flag + learn target
webView->evaluateJavascript("window.updateMidiState(-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, false, -1)");

// Key bindings (when changed)
webView->evaluateJavascript("window.updateKeyBindings('','','','','','','','','','')");

// Metronome beat
webView->evaluateJavascript("window.updateMetroBeat(2, 14)");
```

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
| Footswitch (2) | `looperFootswitchDown`, `looperFootswitchUp` |
| MIDI Learn (20) | `midiLearnRecord`, `midiLearnStop`, `midiLearnClear`, `midiLearnUndo`, `midiLearnFootswitch`, `midiLearnOverdub`, `midiLearnBarMode`, `midiLearnClick`, `midiLearnPreCount`, `midiLearnArmOverdub`, `midiLearnPlayClick`, `midiLearnPlay`, `midiLearnMonitor`, `midiLearnLoopModeCycle`, `midiLearnClassicMode`, `midiLearnBeatsMode`, `midiLearnDynamicMode`, `midiLearnPanInputLeft`, `midiLearnPanInputCenter`, `midiLearnPanInputRight` |
| MIDI Clear (20) | `midiClearRecord`, `midiClearStop`, etc. (one per action) |
| MIDI Cancel (1) | `midiLearnCancel` |
| Key Bindings (2) | `keyBindSet`, `keyBindClear` |
| Metronome (5) | `metroStart`, `metroStop`, `metroSetBPM`, `metroSetBeatsPerBar`, `metroSetPatterns` |
| Resize (1) | `contentHeightChanged` |

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
- **Rising edge**: CC value ≥ 64 AND previous value was < 64 → trigger action
- Each action tracks its own `ccWasHigh[action]` state
- Threshold constant: `CC_TRIGGER_THRESHOLD = 64`

### MIDI Learn

1. `startMidiLearn(action)` sets `midiLearnActive = true, midiLearnTarget = action`
2. `processMidiMessages()` on audio thread watches for any CC ≥ 64
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

```javascript
document.addEventListener('keydown', (e) => {
    if (e.repeat) return;  // Prevent rapid-fire
    const keyCode = e.code;
    for (let i = 0; i < keyBindingsMap.length; i++) {
        if (keyBindingsMap[i] === keyCode) {
            ACTION_EVENTS[i]();  // Fire corresponding action
        }
    }
});
```

### Footswitch Special Handling

Action 4 (Footswitch) uses separate down/up events for gesture detection:
- `keydown` (no repeat) → `looperFootswitchDown`
- `keyup` → `looperFootswitchUp`

---

## Footswitch Gesture Detection

### Timing Constants

```cpp
static constexpr int LONG_PRESS_MS = 500;
static constexpr int DOUBLE_PRESS_MS = 200;
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
     └── On 2nd rising edge within 200ms of 1st:
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

### Implementation

```cpp
// Editor constructor
setResizable(true, true);
constrainer.setFixedAspectRatio((double)designWidth / designHeight);
setConstrainer(&constrainer);
setSize(480, 700);

// resized() callback
void resized() override {
    double zoom = (double)getWidth() / designWidth;
    currentZoom = zoom;
    webView->evaluateJavascript("window.setZoom(" + String(zoom) + ")");
}
```

### CSS Zoom

```javascript
window.setZoom = function(factor) {
    document.documentElement.style.zoom = factor;
};
```

### Dynamic Height

When collapsible panels toggle, JS reports content height:

```javascript
function reportContentHeight() {
    const h = document.body.scrollHeight;
    window.__JUCE__.backend.emitEvent("contentHeightChanged", { height: h });
}
```

C++ updates `designHeight`, recalculates aspect ratio, and resizes the window.

---

## State Persistence

### getStateInformation / setStateInformation

Saved via `juce::ValueTree` with identifier `"OrbitLooper"`:

| Property | Type | Description |
|----------|------|-------------|
| `loop_L_[layer]_[chunk]` | Base64 | Loop layer L channel data |
| `loop_R_[layer]_[chunk]` | Base64 | Loop layer R channel data |
| `layerLength_[n]` | int | Each layer's length in samples |
| `numLayers` | int | Active layer count |
| `masterLoopLength` | int | Master loop length |
| `looperState` | int | Current state enum value |
| `readPosition` | int | Current playback position |
| `maxLoopSeconds` | int | Max loop length setting |
| `midiCC_record` ... `midiCC_paninputright` | int | MIDI CC assignments (20 actions) |
| `keyBind_0` ... `keyBind_19` | String | Key bindings (20 actions) |
| `apvts` | XML | AudioProcessorValueTreeState |

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
- No mutexes in the audio path
- Key bindings array accessed only from message thread

---

## Technical Considerations & Architecture Notes

Information for developers looking to maintain or extend the plugin.

### Development Constraints & "Gotchas"
1. **No External ES6 Modules** — The UI uses standard CSS/JS served by JUCE 8's `BinaryData`. While JUCE 8 supports modern JS, we avoid external `import`/`export` to ensure zero-latency UI loading and unified compatibility across all OS-native WebView backends without CORS edge cases. 
2. **Real-time Safety** — The audio thread (`processBlock`) is strictly no-allocation. All memory for layers is pre-allocated in `prepareToPlay`. Communication with the UI is handled via Lock-Free atomics.
3. **Memory Footprint** — 8 layers × stereo × max duration is substantial (~184 MB at 60s). An 1800s (30m) loop allocates ~5.5 GB RAM.
4. **Undo Strategy** — Layers are independent. "Undo" simply decrements the active layer count and recalculates the master loop length. This allows for instant, multi-level undo without extra buffer copying.

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
