# Orbit Looper — User Guide

Welcome to Orbit Looper, a guitar looper with a modern interface designed for musicians. This guide walks you through everything you need to get started and make the most of the app.

---

## Getting Started

Orbit Looper is a multi-layer looper that runs as a standalone app (desktop and Android) or as a plugin (VST3, AU, LV2) inside your DAW.

When you first launch, you'll see the main screen: a circular loop ring showing your loop position, transport and utility controls, gain controls, and a tempo strip. The navigation bar opens Rhythm & Bars, MIDI mapping, audio-device settings, Orbit settings, and help.

### Audio Setup

Tap the audio/gear icon in the bottom navigation bar to open audio settings. Here you can:
- Select your audio input and output devices
- Choose your sample rate and buffer size
- Select MIDI input devices (for BLE MIDI controllers)

On Android, the app uses your device's built-in mic and speakers by default. For best results, connect a USB audio interface.

---

## Recording Your First Loop

1. Tap **Record** — the ring turns red and recording begins
2. Play your instrument
3. Tap **Record** again — the loop plays back immediately. You're now overdubbing (the ring turns orange)
4. Tap **Record** again to add another layer on top
5. Double Tap **Stop** to pause playback. Tap again to resume
6. Double Tap and Hold **Clear** to clear all. Tap again to record

That's it. You've just recorded and layered your first loop.

---

## Transport Controls

The circular row contains the primary performance controls. Arm Overdub, Monitor, and Export use the labeled utility row directly beneath it.

| Button | What it does |
|--------|-------------|
| **Record** | Start recording. Tap again to finish and begin overdubbing. Tap again to add more layers. |
| **Stop / Play** | Stop playback (loop is kept in memory). Tap again to resume. |
| **Clear** | Delete the entire loop and start fresh. |
| **Undo** | Remove the last overdub layer. Works during playback or while stopped. |
| **Arm Overdub** | Queue the next overdub to start exactly at the loop boundary (enabled by default). |
| **Monitor** | Toggle live input pass-through. Useful with USB audio interfaces to avoid hearing double audio. |
| **Export** | Save your loop as a 24-bit stereo WAV file (available while the loop is playing or stopped; the file matches the Loop Level you hear). |

---

## Looping Modes

Orbit Looper has three looping modes. You can switch between them from the main screen.

### Classic Mode (Default)
The first layer you record sets the loop length. All overdubs automatically wrap at this boundary. This is how traditional stompbox loopers work.

### Bars Mode
Loop length is determined by BPM, Bars, and Beats. Set BPM on the main screen and configure Bars and Beats in **Rhythm & Bars**. This mode is useful for precise, tempo-synced loops.

### Dynamic Mode
Overdubs can extend the loop length beyond the original recording, up to the Global Max Length set in Settings. Good for experimental, free-form looping.

---

## Tempo And Metronome

The performance controls are in the tempo strip beneath Loop Level:

- **BPM**: Enter an exact tempo from 30 to 300 BPM.
- **TAP**: Tap repeatedly to detect the tempo. A new sequence starts after 2.5 seconds without a tap. TAP changes BPM only; it does not enable Click or start the metronome.
- **Click**: Turn the metronome click on or off. The click is synthesized in the audio thread and plays through your audio interface.
- **Pre-count**: Enable a count-in before recording starts. It works with button, key, MIDI, and footswitch triggers.

Open **Rhythm & Bars** from the navigation bar for the advanced controls:

- **Bars / Beats**: Set the number of bars and beats per bar. Together with BPM, these determine the loop length in Bars Mode.
- **Pre-count Bars**: Choose the length of the count-in.
- **Audition Click**: Hear the click at the current BPM without recording.
- **Rhythm pattern editor**: Set accent (A) and regular (B) beats, up to 16 beats per bar.

---

## MIDI & Keyboard Control

Open the Mapping panel from the navigation bar.

### MIDI Learn
1. Tap **Learn** next to any action
2. Press a CC on your MIDI controller
3. The binding is saved automatically

### Key Bindings
1. Tap **Learn** next to any action
2. Press a key on your keyboard
3. One key per action — assigning a key already in use will reassign it

### Mappable Actions
There are 20 actions you can map: Record, Overdub, Play, Stop, Clear, Undo, Footswitch, Monitor, Bar Mode, Click, Play Click, Pre-count, Arm Overdub, Loop Mode Cycle, Classic Mode, Beats Mode, Dynamic Mode, Pan Input Left/Center/Right.

### Footswitch Mode (Ditto-Style)
Assign a MIDI CC or key to the **Footswitch** action to enable gesture-based control:
- **1× Tap**: Record → Play → Overdub → Play (cycle)
- **2× Tap**: Stop playback
- **2× Tap + Hold**: Clear all
- **Hold**: Undo last layer

### MIDI Activity
The green dot in the Mapping panel shows when MIDI messages are being received. If it disappears, your controller may have disconnected.

---

## Bluetooth MIDI (Android)

### BLE MIDI
Most modern MIDI controllers connect via Bluetooth Low Energy (BLE). They appear automatically in the audio settings MIDI device list once paired at the OS level.

If your BLE MIDI device doesn't appear in the list, it may have connected as a HID device and stopped advertising. Orbit Looper includes a patch that detects bonded BLE MIDI devices even when they're not actively advertising — so try pairing the device in Android's Bluetooth settings first, then check the MIDI device list again.

### Bluetooth Classic MIDI
For devices that use the Serial Port Profile (SPP), open **Settings** and scroll to the "BLUETOOTH CLASSIC MIDI" section:
1. Tap **REFRESH** to scan for paired Bluetooth Classic devices
2. Tap a device to connect
3. The connection is remembered for next time

### Troubleshooting
- **Device not appearing**: Make sure the device is paired in Android's Bluetooth settings first.
- **MIDI Chief interference**: If you have the MIDI Chief app installed, force-stop it before using Orbit Looper. It auto-connects to BLE MIDI devices and can steal the connection.
- **Connection drops**: Some BLE MIDI devices disconnect after ~30 seconds of inactivity. This is typically a device firmware behavior, not an app issue.

---

## Settings

Open Settings from the navigation bar.

- **Global Max Length**: The maximum loop length ("tape length") and Classic-mode recording limit. Memory is allocated per layer as you actually record — a longer setting only increases RAM for the base layer (~23 MB per minute at 48 kHz stereo); each overdub layer adds memory proportional to your actual loop length.
- **Max Layers**: Number of independent overdub layers (2–8). More layers use more memory.
- **Mute Input**: Mute audio input pass-through to prevent feedback from speakers. Recording still works while muted.
- **Mute Input on Startup**: Enabled by default on Android to prevent feedback from internal speakers. You can disable it in Settings.
- **Bluetooth Classic MIDI** (Android only): Connect to BT Classic SPP MIDI controllers. See the Bluetooth MIDI section above.

---

## Tips & Tricks

- **Double-click** any parameter value (Gain, Loop Level) to reset it to its default.
- **Shift+Click** or **Alt+Click** any parameter value to type a precise number.
- The **green dot** in the Mapping panel shows live MIDI activity — useful for confirming your controller is connected.
- The **Monitor** button is especially useful with USB audio interfaces to prevent hearing the direct signal and the looped signal at the same time.
- **WAV Export** saves a 24-bit stereo mixdown of all your loop layers.
- On Android, the app is portrait-only and optimized for phone screens.
- **Arm Overdub** (enabled by default) ensures your overdubs start exactly at the loop boundary for tight, in-time layers.
