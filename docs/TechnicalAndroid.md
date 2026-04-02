# Orbit Looper — Android Technical Documentation

Developer reference for the Android build, architecture, and platform-specific implementation details.

---

## Table of Contents

- [Overview](#overview)
- [Build System](#build-system)
- [Project Structure](#project-structure)
- [UI: mobile-index.html](#ui-mobile-indexhtml)
- [Mute on Startup](#mute-on-startup)
- [BLE MIDI](#ble-midi)
- [Bluetooth Classic MIDI Bridge](#bluetooth-classic-midi-bridge)
- [BLE MIDI Bonded Device Patch](#ble-midi-bonded-device-patch)
- [JUCE Standalone on Android](#juce-standalone-on-android)
- [Persistent Settings](#persistent-settings)
- [Known Limitations](#known-limitations)

---

## Overview

The Android version runs as a JUCE standalone app (not a plugin). It shares the same `PluginProcessor` and `PluginEditor` C++ code as the desktop version, with platform-specific behavior gated by `#if JUCE_ANDROID` preprocessor blocks.

Key differences from desktop:
- Uses `mobile-index.html` instead of `index.html` for the WebView UI
- Portrait-only, fullscreen, no window resizing
- Bottom navigation bar instead of left sidebar
- Custom mute-on-startup system (bypasses JUCE's native mute banner)
- BLE MIDI is the primary MIDI transport; a Bluetooth Classic MIDI bridge (SPP/RFCOMM) is also available

---

## Build System

### Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Android SDK | API 35 (compileSdk) | |
| Android NDK | 28.1.13356709 | Install via SDK Manager → SDK Tools → NDK (Side by side) |
| Gradle | 8.11.1 (wrapper) | Bundled in `android/gradle/` |
| Java | JDK 17+ | Required by AGP 8.10 |
| Desktop CMake build | Any | Must run `cmake -S . -B build` first to fetch JUCE via FetchContent |

### Build Commands

```bash
# 1. Fetch JUCE (desktop configure — only needed once)
cmake -S . -B build

# 2. Build APK
cd android
./gradlew assembleRelease_Release    # Linux/macOS
.\gradlew.bat assembleRelease_Release  # Windows
```

Output: `android/app/build/outputs/apk/release_/release/OrbitLooper.apk`

### Clean Build

When changing HTML/CSS/JS files (compiled into BinaryData), you must nuke the CMake and build caches:

```bash
# PowerShell (Windows)
Remove-Item -Recurse -Force android/app/.cxx
Remove-Item -Recurse -Force android/app/build
.\gradlew.bat assembleRelease_Release

# Bash (Linux/macOS)
rm -rf android/app/.cxx android/app/build
./gradlew assembleRelease_Release
```

C++ changes alone don't require a clean build — Gradle/CMake incremental builds handle them.

### ADB Install & Debug

```bash
adb install -r android/app/build/outputs/apk/release_/release/OrbitLooper.apk
adb logcat -s "JUCE" "OrbitLooper"
```


### Gradle Configuration

`android/app/build.gradle` key settings:

| Setting | Value | Reason |
|---------|-------|--------|
| `minSdkVersion` | 30 (Android 11) | AAudio exclusive-mode low-latency |
| `targetSdkVersion` | 35 (Android 15) | Latest target |
| `compileSdk` | 35 | Latest compile SDK |
| `abiFilters` | `arm64-v8a` only | Real device performance; no emulator x86 |
| `c++_static` STL | Static linking | Single .so, no STL dependency issues |
| `C++20` | Enabled | Matches desktop build |

### JUCE Java Sources

The Gradle build includes JUCE's Java bootstrap sources from the desktop `build/_deps/juce-src/` directory. This is why the desktop CMake configure must run first:

```groovy
main.java.srcDirs += [
    "../../build/_deps/juce-src/modules/juce_core/native/javacore/init",
    "../../build/_deps/juce-src/modules/juce_core/native/javacore/app",
    "../../build/_deps/juce-src/modules/juce_gui_basics/native/javaopt/app",
    "../../build/_deps/juce-src/modules/juce_audio_devices/native/javaopt/app",
    "../../build/_deps/juce-src/modules/juce_audio_devices/native/java/app",
    "../../build/_deps/juce-src/modules/juce_gui_extra/native/java/app"
]
```

---

## Project Structure

```
android/
├── CMakeLists.txt          # Android-specific CMake (wraps main CMakeLists.txt)
├── build.gradle            # Root Gradle config (AGP version, repositories)
├── settings.gradle         # Gradle project settings
├── gradle.properties       # JVM args, Android settings
├── gradlew.bat             # Gradle wrapper (Windows)
├── gradle/                 # Gradle wrapper JAR
└── app/
    ├── build.gradle        # App-level config (SDK versions, flavors, signing, APK naming)
    └── src/
        └── main/
            └── AndroidManifest.xml
```

The native C++ code is compiled via `externalNativeBuild.cmake` pointing to `android/CMakeLists.txt`, which includes the root `CMakeLists.txt`.

---

## UI: mobile-index.html

Android uses `mobile-index.html` instead of `index.html`. The selection happens in `PluginEditor.cpp`:

```cpp
#if JUCE_ANDROID
  const auto htmlFile = juce::String("mobile-index.html");
#else
  const auto htmlFile = juce::String("index.html");
#endif
```

### Key Differences from Desktop

- Sidebar becomes a fixed bottom navigation bar (60px)
- No window resizing — fullscreen, portrait-only
- `window.setZoom()` is a no-op (native pixel sizing)
- Modals are teleported to `<body>` to escape CSS transform clipping
- `vh` units resolve to 0px on Android WebView — all heights use concrete pixel values from JS
- `applyModalSize()` explicitly sets width/height on modals using `screen.width` and `window.innerHeight`
- The WebView viewport may be wider than the visible screen (e.g., 646px vs 385px visible) — all UI elements are constrained to `visW = Math.min(window.innerWidth, screen.width)`

### Standalone Mute Banner

A custom red banner at the top of the screen replaces JUCE's native yellow `NotificationArea`. See [Mute on Startup](#mute-on-startup).

---

## Mute on Startup

### Problem

JUCE's standalone wrapper starts with input muted to prevent feedback. On Android with internal speakers, this is essential. However, JUCE's native yellow `NotificationArea` banner fights with our WebView layout — it reserves 30px at the top and re-shows itself via `inputMutedChanged()` whenever the mute state changes.

### Solution — Three-Layer Mute System

There are three separate mute mechanisms that must be kept in sync:

1. **JUCE Standalone Mute** (`StandalonePluginHolder::getMuteInputValue()`) — Device-level. Replaces input buffer with silence before `processBlock`. Default ON on Android. **Must NOT be set to false during editor construction** — doing so breaks audio device enumeration.

2. **feedbackMuted** (`audioProcessor.feedbackMuted`) — DSP-level. Zeros input pass-through in `processBlock`. Controls the red WebView banner and Settings "MUTE INPUT" toggle. Independent from the Monitor button.

3. **inputMuted** (`audioProcessor.inputMuted`) — DSP-level. Zeros input pass-through in `processBlock`. Controls the Monitor/Speaker transport button. Purpose: prevent hearing USB input + pedal output simultaneously.

In `processBlock`:
```cpp
bool isInputMuted = inputMuted.load() || feedbackMuted.load();
```

### Startup Sequence

1. Editor constructor loads `muteOnStartup` from `PropertiesFile`
2. Sets `feedbackMuted = muteOnStartup`
3. Does NOT touch JUCE's mute (defaults to ON)
4. If `muteOnStartup` is OFF: after 1000ms delay, sets JUCE mute to false
5. Timer callback hides native yellow JUCE banner (finds sibling components with height ≤ 30)

### Unmute Flow (Red Banner)

When user clicks "Unmute":
1. `standaloneInputUnmute` event fires
2. Sets `feedbackMuted = false`
3. Sets `getMuteInputValue() = false` (unmutes JUCE device-level)
4. Banner hides (timer pushes `feedbackMuted` state to JS)

### Persistence

The "Mute Input on Startup" toggle is stored via `juce::PropertiesFile`:

```cpp
juce::PropertiesFile::Options opts;
opts.applicationName = "OrbitLooper";
opts.folderName = "OrbitLooper";
opts.filenameSuffix = ".settings";
```

On Android, this writes to the app's internal storage directory.

---

## BLE MIDI

Android's MIDI API only supports BLE (Bluetooth Low Energy) MIDI. Bluetooth Classic MIDI devices will not appear in the device list. This is an Android platform limitation, not an app limitation.

JUCE's `StandalonePluginHolder` auto-opens MIDI devices via a 500ms timer that scans for new devices. BLE MIDI devices must be paired at the OS level first, then they appear in JUCE's device list.

### MIDI Learn

The BLE MIDI latency fix removed the CC value threshold from MIDI Learn — any CC message (including val=0 on release) now completes the learn. This was necessary because some BLE controllers (e.g., AIRSTEP Lite) send val=0 on button release, and the original threshold of `CC >= 1` would miss these.

### MIDI Activity Indicator

A green dot in the Mapping modal shows BLE MIDI connection status. It's driven by `lastMidiActivityMs` — if no CC message is received within 5 seconds, the dot disappears.

---

## Bluetooth Classic MIDI Bridge

A Java-side bridge that opens SPP/RFCOMM sockets to paired Bluetooth Classic devices and injects parsed MIDI messages into the C++ audio processor via JNI.

### Architecture

```
┌─────────────────────────────────────────────┐
│  BluetoothClassicMidiService (Java)         │
│  ┌─────────────┐  ┌──────────────────────┐  │
│  │ SPP Socket  │→ │ MidiByteParser       │  │
│  │ (RFCOMM)    │  │ (running status,     │  │
│  │             │  │  SysEx filtering,    │  │
│  │             │  │  Real-Time pass-thru)│  │
│  └─────────────┘  └──────────┬───────────┘  │
│                              │ JNI call      │
└──────────────────────────────┼───────────────┘
                               ▼
┌─────────────────────────────────────────────┐
│  PluginProcessor (C++)                      │
│  Lock-free SPSC queue (256 slots,           │
│  cache-line padded) → drain in processBlock │
│  → MidiBuffer → processMidiMessages()       │
└─────────────────────────────────────────────┘
```

### Components

- **`BluetoothClassicMidiService`** (Java): Opens SPP/RFCOMM sockets to paired BT Classic devices. Contains a `MidiByteParser` inner class that parses raw MIDI bytes, handling running status, SysEx filtering, and System Real-Time message pass-through.
- **JNI bridge**: Pushes parsed MIDI messages to a lock-free single-producer single-consumer (SPSC) queue with 256 slots and cache-line padding.
- **`processBlock` integration**: Drains the queue into `MidiBuffer` before `processMidiMessages()` is called.
- **WebView UI**: Adds a "BLUETOOTH CLASSIC MIDI" section in the Settings modal (Android only). Users can tap REFRESH to scan paired devices and tap a device to connect.
- **Connection persistence**: Last connected device is persisted via `PropertiesFile` (`btClassicLastDevice` key).

### Files

| File | Purpose |
|------|---------|
| `Source/BluetoothClassicMidi.h/cpp` | C++ side: lock-free SPSC queue, JNI bridge |
| `android/app/src/main/java/com/orbitlooper/BluetoothClassicMidiService.java` | Java SPP/RFCOMM service, MidiByteParser |

All C++ code is guarded with `#if JUCE_ANDROID`.

### Note

This feature was built for BT Classic SPP devices. Most modern MIDI controllers (like Mvave Chocolate Plus) actually use BLE MIDI, not BT Classic SPP.

---

## BLE MIDI Bonded Device Patch

### Problem

Some BLE MIDI devices (e.g., Mvave Chocolate Plus / FootCtrlPlus) connect as BLE HID and stop advertising. JUCE's BLE scan (`BluetoothLeScanner.startScan()`) never finds them because they're not advertising the BLE MIDI UUID anymore.

### Solution

Patched `JuceMidiSupport.java` `BluetoothMidiManager.startStopScan()` to also check `getBondedDevices()` for devices with the BLE MIDI UUID (`03b80e5a-ede8-4b33-a751-6ce34ec4c700`). These bonded devices are added to the scan results even if not currently advertising.

### File

`build/_deps/juce-src/modules/juce_audio_devices/native/java/app/com/rmsl/juce/JuceMidiSupport.java` (patched in-place, compiled via Gradle source dirs).

### Important

This patch is applied to the JUCE source fetched by the desktop CMake build. It must be re-applied if JUCE is re-fetched.

---

## JUCE Standalone on Android

### Screen Setup

```cpp
// PluginEditor.cpp constructor
auto& displays = juce::Desktop::getInstance().getDisplays();
if (const auto* display = displays.getPrimaryDisplay()) {
    const auto area = display->userArea;
    setSize(area.getWidth(), area.getHeight());
}
setResizable(false, false);  // No resizing on Android
```

The editor fills the entire screen. Portrait orientation is enforced via the Android manifest.

### Timer Callback (30Hz)

The same 30Hz timer as desktop pushes state to the WebView. Android-specific additions:
- Pushes `inputMuted` state for the custom mute banner
- Pushes `muteOnStartup` toggle state (once, on first frame after `uiReady`)

---

## Persistent Settings

Two separate persistence mechanisms:

| What | Mechanism | Scope |
|------|-----------|-------|
| Plugin state (MIDI mappings, loop data, APVTS) | `getStateInformation()` / `setStateInformation()` via JUCE's `PropertySet` | Managed by JUCE standalone wrapper |
| Mute on Startup toggle | `juce::PropertiesFile` ("OrbitLooper.settings") | App-level, read in editor constructor |

---

## Known Limitations

- Portrait-only — landscape mode is not supported
- BLE MIDI is the primary MIDI transport. A Bluetooth Classic MIDI bridge (SPP/RFCOMM) is also available for devices that use the Serial Port Profile.
- BLE MIDI bonded device patch: Some BLE MIDI devices that connect as HID may not appear in JUCE's BLE scan. A patch to `JuceMidiSupport.java` adds bonded BLE MIDI devices to the scan results.
- No VST3/AU plugin format — Android runs as standalone only
- `vh` CSS units resolve to 0px in Android WebView — all modal sizing uses JS pixel calculations
- WebView viewport width may exceed visible screen width — UI constrains to `screen.width`
- No ASIO equivalent — uses Android's AAudio (Oboe) for low-latency audio
- Clean builds required for HTML/CSS/JS changes (BinaryData recompilation)
