package com.orbitlooper;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.os.ParcelUuid;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.io.InputStream;
import java.util.Set;
import java.util.UUID;

/**
 * Bluetooth Classic MIDI bridge service (plain Java class, not an Android Service).
 *
 * Manages SPP/RFCOMM connections to paired Bluetooth Classic MIDI controllers,
 * parses raw MIDI bytes via the inner MidiByteParser, and forwards complete
 * MIDI messages to the C++ audio processor through JNI.
 *
 * Instantiated from C++ via JNI in BluetoothClassicMidi::initialize().
 */
public class BluetoothClassicMidiService {

    private static final String TAG = "BtClassicMidi";

    /** Standard Bluetooth Serial Port Profile UUID. */
    private static final UUID SPP_UUID =
            UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");

    // Connection state constants
    private static final String STATUS_DISCONNECTED = "disconnected";
    private static final String STATUS_CONNECTING   = "connecting";
    private static final String STATUS_CONNECTED    = "connected";

    // Current connection state
    private volatile String connectionStatus = STATUS_DISCONNECTED;
    private volatile String connectedAddress = "";

    // Active socket and read thread
    private BluetoothSocket socket;
    private Thread readThread;
    private final MidiByteParser parser = new MidiByteParser();

    // Lock for connection/disconnection serialisation (not on audio thread)
    private final Object connectionLock = new Object();

    // =========================================================================
    // JNI native methods — implemented in BluetoothClassicMidi.cpp
    // =========================================================================
    private native void nativeOnClassicMidiMessage(byte[] data);
    private native void nativeOnBtClassicStatusChanged(String status, String macAddress);

    // =========================================================================
    // Public API (called from C++ via JNI)
    // =========================================================================

    /**
     * Enumerate paired Bluetooth devices that may support SPP/MIDI.
     * Returns a JSON array: [{"name":"...","address":"AA:BB:CC:DD:EE:FF"}, ...]
     *
     * Lists ALL paired BT Classic devices rather than filtering by SPP UUID,
     * because many BT MIDI controllers (e.g., Mvave Chocolate Plus / FootCtrlPlus)
     * don't advertise SPP in their cached SDP records. The RFCOMM connection
     * attempt with SPP UUID will fail gracefully if the device doesn't support it.
     */
    public String getPairedSppDevices() {
        JSONArray result = new JSONArray();

        try {
            BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
            if (adapter == null || !adapter.isEnabled()) {
                return result.toString();
            }

            Set<BluetoothDevice> bondedDevices = adapter.getBondedDevices();
            if (bondedDevices == null) {
                return result.toString();
            }

            for (BluetoothDevice device : bondedDevices) {
                // Skip LE-only devices (type == DEVICE_TYPE_LE).
                // We want CLASSIC or DUAL devices.
                try {
                    int type = device.getType();
                    if (type == BluetoothDevice.DEVICE_TYPE_LE) {
                        continue;
                    }
                } catch (SecurityException e) {
                    // If we can't read the type, include it anyway.
                }

                try {
                    JSONObject obj = new JSONObject();
                    obj.put("name", device.getName() != null ? device.getName() : "Unknown");
                    obj.put("address", device.getAddress());
                    result.put(obj);
                } catch (Exception e) {
                    Log.w(TAG, "Error building device JSON", e);
                }
            }
        } catch (SecurityException e) {
            Log.w(TAG, "SecurityException enumerating paired devices", e);
        }

        return result.toString();
    }

    /**
     * Connect to a Bluetooth Classic SPP device by MAC address.
     * Spawns a background thread for the RFCOMM connection attempt.
     * Enforces single-device-at-a-time (Requirement 2.5).
     */
    public void connect(final String macAddress) {
        if (macAddress == null || macAddress.isEmpty()) {
            return;
        }

        // Spawn connection on a background thread to avoid blocking the caller.
        new Thread(() -> connectInternal(macAddress), "BtClassicConnect").start();
    }

    /**
     * Disconnect the currently connected device (if any).
     */
    public void disconnect() {
        synchronized (connectionLock) {
            disconnectInternal();
        }
    }

    /** Returns "disconnected", "connecting", or "connected". */
    public String getConnectionStatus() {
        return connectionStatus;
    }

    /** Returns the MAC address of the currently connected device, or "". */
    public String getConnectedDeviceAddress() {
        return connectedAddress;
    }

    // =========================================================================
    // Internal connection logic
    // =========================================================================

    private void connectInternal(String macAddress) {
        synchronized (connectionLock) {
            // Enforce single-device-at-a-time (Requirement 2.5):
            // disconnect any existing connection before opening a new one.
            if (!STATUS_DISCONNECTED.equals(connectionStatus)) {
                disconnectInternal();
            }

            connectionStatus = STATUS_CONNECTING;
            connectedAddress = macAddress;
            nativeOnBtClassicStatusChanged(STATUS_CONNECTING, macAddress);

            BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
            if (adapter == null) {
                Log.e(TAG, "BluetoothAdapter is null");
                setDisconnected(macAddress);
                return;
            }

            BluetoothDevice device;
            try {
                device = adapter.getRemoteDevice(macAddress);
            } catch (IllegalArgumentException e) {
                Log.e(TAG, "Invalid MAC address: " + macAddress, e);
                setDisconnected(macAddress);
                return;
            }

            BluetoothSocket newSocket;
            try {
                newSocket = device.createRfcommSocketToServiceRecord(SPP_UUID);
            } catch (IOException e) {
                Log.e(TAG, "Failed to create RFCOMM socket", e);
                setDisconnected(macAddress);
                return;
            } catch (SecurityException e) {
                Log.e(TAG, "SecurityException creating RFCOMM socket", e);
                setDisconnected(macAddress);
                return;
            }

            try {
                // Cancel discovery to speed up connection (best practice).
                try {
                    adapter.cancelDiscovery();
                } catch (SecurityException ignored) {
                    // Not critical — just an optimisation.
                }

                newSocket.connect();
            } catch (IOException e) {
                Log.e(TAG, "RFCOMM connect failed", e);
                closeSocketQuietly(newSocket);
                setDisconnected(macAddress);
                return;
            } catch (SecurityException e) {
                Log.e(TAG, "SecurityException on RFCOMM connect", e);
                closeSocketQuietly(newSocket);
                setDisconnected(macAddress);
                return;
            }

            // Connection succeeded.
            socket = newSocket;
            connectionStatus = STATUS_CONNECTED;
            nativeOnBtClassicStatusChanged(STATUS_CONNECTED, macAddress);

            // Reset parser state for the new connection.
            parser.reset();

            // Start the RFCOMM read thread (Task 5.3).
            readThread = new Thread(() -> readLoop(macAddress), "BtClassicRead");
            readThread.start();
        }
    }

    /**
     * Close the socket, stop the read thread, and set state to disconnected.
     * Must be called while holding connectionLock.
     */
    private void disconnectInternal() {
        String mac = connectedAddress;

        // Stop the read thread first.
        Thread rt = readThread;
        if (rt != null) {
            rt.interrupt();
            readThread = null;
        }

        // Close the socket (this will also cause the read thread's
        // InputStream.read() to throw IOException, unblocking it).
        if (socket != null) {
            closeSocketQuietly(socket);
            socket = null;
        }

        // Wait briefly for the read thread to finish.
        if (rt != null) {
            try {
                rt.join(500);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }

        setDisconnected(mac);
    }

    private void setDisconnected(String macAddress) {
        connectionStatus = STATUS_DISCONNECTED;
        connectedAddress = "";
        nativeOnBtClassicStatusChanged(STATUS_DISCONNECTED, macAddress);
    }

    // =========================================================================
    // RFCOMM Read Thread (Task 5.3)
    // =========================================================================

    /**
     * Background read loop: reads raw bytes from the RFCOMM InputStream,
     * feeds each byte to MidiByteParser, which calls nativeOnClassicMidiMessage
     * for each complete MIDI message.
     *
     * Catches IOException to detect unexpected disconnect (Requirement 3.3).
     * The thread is interruptible for clean disconnect.
     */
    private void readLoop(String macAddress) {
        InputStream inputStream;

        try {
            BluetoothSocket s = socket;
            if (s == null) return;
            inputStream = s.getInputStream();
        } catch (IOException e) {
            Log.e(TAG, "Failed to get InputStream", e);
            handleUnexpectedDisconnect(macAddress);
            return;
        }

        try {
            while (!Thread.currentThread().isInterrupted()) {
                int byteRead = inputStream.read();
                if (byteRead == -1) {
                    // End of stream — remote device closed connection.
                    break;
                }
                parser.processByte((byte) byteRead);
            }
        } catch (IOException e) {
            // Socket closed — either by us (clean disconnect) or by remote
            // device (unexpected disconnect / link loss).
            if (!Thread.currentThread().isInterrupted()) {
                Log.w(TAG, "RFCOMM read IOException (unexpected disconnect)", e);
            }
        }

        // If we exited the loop and weren't explicitly disconnected by the user,
        // treat it as an unexpected disconnect.
        if (!Thread.currentThread().isInterrupted()) {
            handleUnexpectedDisconnect(macAddress);
        }
    }

    /**
     * Handle unexpected disconnect detected by the read thread.
     * Cleans up resources and notifies the C++ layer.
     */
    private void handleUnexpectedDisconnect(String macAddress) {
        synchronized (connectionLock) {
            if (socket != null) {
                closeSocketQuietly(socket);
                socket = null;
            }
            readThread = null;
            connectionStatus = STATUS_DISCONNECTED;
            connectedAddress = "";
        }
        nativeOnBtClassicStatusChanged(STATUS_DISCONNECTED, macAddress);
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    /**
     * Check whether a paired device advertises the SPP UUID.
     */
    private static boolean hasSppUuid(BluetoothDevice device) {
        try {
            ParcelUuid[] uuids = device.getUuids();
            if (uuids == null) return false;
            for (ParcelUuid pu : uuids) {
                if (SPP_UUID.equals(pu.getUuid())) {
                    return true;
                }
            }
        } catch (SecurityException e) {
            Log.w(TAG, "SecurityException reading device UUIDs", e);
        }
        return false;
    }

    private static void closeSocketQuietly(BluetoothSocket s) {
        try {
            s.close();
        } catch (IOException ignored) {
        }
    }

    // =========================================================================
    // MidiByteParser — Inner class (Task 5.2)
    // =========================================================================

    /**
     * State-machine MIDI byte parser for raw SPP byte streams.
     *
     * State diagram (from design document):
     *   Idle → WaitingData1 → WaitingData2 → EmitMessage → WaitingData1 (running status)
     *   Idle → InSysEx → Idle (on 0xF7)
     *
     * Handles:
     *   - Running status (Requirement 4.2)
     *   - System Real-Time pass-through without disrupting state (Requirement 4.3)
     *   - SysEx filtering (Requirement 4.4)
     *   - Error recovery on unexpected bytes (Requirement 4.5)
     */
    class MidiByteParser {

        private static final int STATE_IDLE          = 0;
        private static final int STATE_WAITING_DATA1 = 1;
        private static final int STATE_WAITING_DATA2 = 2;
        private static final int STATE_IN_SYSEX      = 3;

        private int state = STATE_IDLE;

        /** The current (or running) status byte. */
        private byte runningStatus = 0;

        /** How many data bytes the current message type expects (1 or 2). */
        private int expectedDataBytes = 0;

        /** Buffer for the message being assembled. */
        private final byte[] msgBuf = new byte[3];
        private int msgBufIndex = 0;

        void reset() {
            state = STATE_IDLE;
            runningStatus = 0;
            expectedDataBytes = 0;
            msgBufIndex = 0;
        }

        /**
         * Feed a single byte from the RFCOMM stream into the parser.
         */
        void processByte(byte b) {
            int unsigned = b & 0xFF;

            // ---- System Real-Time (0xF8–0xFF): ignore, never disrupt state ----
            if (unsigned >= 0xF8) {
                return;
            }

            // ---- SysEx handling ----
            if (state == STATE_IN_SYSEX) {
                if (unsigned == 0xF7) {
                    // SysEx end — return to Idle.
                    state = STATE_IDLE;
                }
                // All other bytes inside SysEx are discarded.
                return;
            }

            if (unsigned == 0xF0) {
                // SysEx start — enter SysEx state, discard everything until 0xF7.
                state = STATE_IN_SYSEX;
                return;
            }

            // ---- System Common 0xF1–0xF6: discard (rare, not needed) ----
            if (unsigned >= 0xF1 && unsigned <= 0xF7) {
                // 0xF7 outside SysEx is also just discarded.
                // Reset parse state since these interrupt any in-progress message.
                state = STATE_IDLE;
                return;
            }

            // ---- Status byte (0x80–0xEF) ----
            boolean isStatusByte = (unsigned >= 0x80);

            if (isStatusByte) {
                // New status byte received.
                runningStatus = b;
                expectedDataBytes = dataByteCount(unsigned);
                msgBuf[0] = b;
                msgBufIndex = 1;
                state = STATE_WAITING_DATA1;
                return;
            }

            // ---- Data byte (0x00–0x7F) ----
            switch (state) {
                case STATE_IDLE:
                    // Data byte with no running status — check if we have a
                    // valid running status to apply (Requirement 4.2).
                    if (runningStatus != 0) {
                        // Reuse running status.
                        expectedDataBytes = dataByteCount(runningStatus & 0xFF);
                        msgBuf[0] = runningStatus;
                        msgBuf[1] = b;
                        msgBufIndex = 2;
                        if (expectedDataBytes == 1) {
                            emitMessage(2);
                            // Stay ready for more running-status data.
                            state = STATE_IDLE;
                        } else {
                            state = STATE_WAITING_DATA2;
                        }
                    }
                    // else: no running status — discard (Requirement 4.5).
                    break;

                case STATE_WAITING_DATA1:
                    msgBuf[msgBufIndex++] = b;
                    if (expectedDataBytes == 1) {
                        // 1-data-byte message complete (Program Change, Channel Pressure).
                        emitMessage(msgBufIndex);
                        // Running status: go back to waiting for next data byte(s).
                        msgBuf[0] = runningStatus;
                        msgBufIndex = 1;
                        // Stay in a state where the next data byte triggers running status.
                        // We go to IDLE so the next data byte hits the running-status path.
                        state = STATE_IDLE;
                    } else {
                        // 2-data-byte message — need one more.
                        state = STATE_WAITING_DATA2;
                    }
                    break;

                case STATE_WAITING_DATA2:
                    msgBuf[msgBufIndex++] = b;
                    // 2-data-byte message complete.
                    emitMessage(msgBufIndex);
                    // Running status: ready for next pair of data bytes.
                    msgBuf[0] = runningStatus;
                    msgBufIndex = 1;
                    // Go to IDLE so next data byte triggers running-status path.
                    state = STATE_IDLE;
                    break;

                default:
                    break;
            }
        }

        /**
         * Emit a complete MIDI message to the C++ layer via JNI.
         */
        private void emitMessage(int length) {
            byte[] msg = new byte[length];
            System.arraycopy(msgBuf, 0, msg, 0, length);
            nativeOnClassicMidiMessage(msg);
        }

        /**
         * Returns the number of data bytes expected for a given status byte.
         * Channel voice messages only (0x80–0xEF).
         */
        private int dataByteCount(int statusByte) {
            int highNibble = statusByte & 0xF0;
            switch (highNibble) {
                case 0xC0: // Program Change — 1 data byte
                case 0xD0: // Channel Pressure — 1 data byte
                    return 1;
                case 0x80: // Note Off — 2 data bytes
                case 0x90: // Note On — 2 data bytes
                case 0xA0: // Poly Aftertouch — 2 data bytes
                case 0xB0: // Control Change — 2 data bytes
                case 0xE0: // Pitch Bend — 2 data bytes
                    return 2;
                default:
                    return 0;
            }
        }
    }

    // Load the native library that contains the JNI implementations.
    static {
        try {
            System.loadLibrary("juce_jni");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load native library", e);
        }
    }
}
