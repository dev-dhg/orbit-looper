#pragma once

#if JUCE_ANDROID

#include <atomic>
#include <cstdint>
#include <cstring>
#include <jni.h>
#include <juce_core/juce_core.h>
#include <mutex>
#include <string>

//==============================================================================
/**
 * Bluetooth Classic MIDI — Lock-free SPSC queue and message types.
 *
 * MidiMessageSlot: fixed 4-byte slot for a single MIDI channel message.
 * MidiSpscQueue:   single-producer / single-consumer ring buffer (capacity 256).
 *
 * Producer = JNI callback thread (Java BT read thread → native).
 * Consumer = audio thread (processBlock drains into MidiBuffer).
 *
 * Zero heap allocations in push/pop. Cache-line padding between head and tail
 * to avoid false sharing between the two threads.
 */

//==============================================================================
struct MidiMessageSlot
{
    uint8_t data[3]; // Max 3 bytes for channel voice messages
    uint8_t size;    // 1, 2, or 3
};

//==============================================================================
class MidiSpscQueue
{
public:
    static constexpr uint32_t CAPACITY = 256;

    MidiSpscQueue() : head (0), tail (0) {}

    /** Producer: push a MIDI message into the queue.
     *  Returns false if the queue is full. The caller is responsible for
     *  advancing the tail (dropping the oldest message) and retrying.
     *  See pushOrDropOldest() for the convenience wrapper.
     */
    bool push (const uint8_t* msgData, int msgSize)
    {
        const auto currentHead = head.load (std::memory_order_relaxed);
        const auto currentTail = tail.load (std::memory_order_acquire);

        if (count (currentHead, currentTail) == CAPACITY)
            return false;

        auto& slot = slots[currentHead & MASK];
        const auto clampedSize = (msgSize > 3) ? 3 : msgSize;
        std::memcpy (slot.data, msgData, static_cast<size_t> (clampedSize));
        slot.size = static_cast<uint8_t> (clampedSize);

        head.store (currentHead + 1, std::memory_order_release);
        return true;
    }

    /** Producer convenience: push a message, dropping the oldest if full.
     *  Implements Requirement 5.5 — never blocks the JNI callback thread.
     */
    void pushOrDropOldest (const uint8_t* msgData, int msgSize)
    {
        if (! push (msgData, msgSize))
        {
            // Advance tail to discard oldest unread message
            tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release);
            push (msgData, msgSize);
        }
    }

    /** Consumer: pop the oldest message from the queue.
     *  Returns false if the queue is empty.
     */
    bool pop (MidiMessageSlot& out)
    {
        const auto currentTail = tail.load (std::memory_order_relaxed);
        const auto currentHead = head.load (std::memory_order_acquire);

        if (currentHead == currentTail)
            return false;

        const auto& slot = slots[currentTail & MASK];
        std::memcpy (out.data, slot.data, slot.size);
        out.size = slot.size;

        tail.store (currentTail + 1, std::memory_order_release);
        return true;
    }

    /** Returns the number of messages currently in the queue (approximate). */
    uint32_t size() const
    {
        return count (head.load (std::memory_order_acquire),
                      tail.load (std::memory_order_acquire));
    }

private:
    static constexpr uint32_t MASK = CAPACITY - 1;

    static uint32_t count (uint32_t h, uint32_t t)
    {
        return h - t; // Works correctly with unsigned wrap-around
    }

    // Cache-line padding to prevent false sharing between producer and consumer.
    // Typical cache line is 64 bytes; atomic<uint32_t> is 4 bytes.
    alignas (64) std::atomic<uint32_t> head;
    char padHead[64 - sizeof (std::atomic<uint32_t>)];

    alignas (64) std::atomic<uint32_t> tail;
    char padTail[64 - sizeof (std::atomic<uint32_t>)];

    MidiMessageSlot slots[CAPACITY];
};

//==============================================================================
/** Global SPSC queue instance — shared between JNI callback (producer) and
 *  audio thread processBlock (consumer). Defined in BluetoothClassicMidi.cpp.
 */
extern MidiSpscQueue gBtClassicMidiQueue;

//==============================================================================
/**
 * BluetoothClassicMidi namespace — JNI bridge to the Java
 * BluetoothClassicMidiService. All functions must be called from the message
 * thread (JUCE event listeners / editor timer) unless noted otherwise.
 */
namespace BluetoothClassicMidi
{
    /** Initialise the JNI bridge: create the Java service instance and cache
     *  method IDs. Call once from the editor constructor.
     */
    void initialize();

    /** Returns a JSON array of paired SPP devices:
     *  [{"name":"...","address":"AA:BB:CC:DD:EE:FF"}, ...]
     */
    juce::String getPairedDevicesJson();

    /** Start an asynchronous connection to the device at the given MAC address. */
    void connectDevice (const juce::String& macAddress);

    /** Disconnect the currently connected device (if any). */
    void disconnectDevice();

    /** Returns "disconnected", "connecting", or "connected". */
    juce::String getConnectionStatus();

    /** Returns the MAC address of the currently connected device, or empty. */
    juce::String getConnectedDeviceAddress();

    /** Thread-safe accessors for the latest status pushed by the Java callback.
     *  Read by the 30 Hz editor timer to push status to the WebView UI.
     */
    juce::String getLastCallbackStatus();
    juce::String getLastCallbackMac();
}

//==============================================================================
// JNI native callbacks — called from the Java BT read thread.
//==============================================================================
extern "C"
{
    JNIEXPORT void JNICALL
    Java_com_orbitlooper_BluetoothClassicMidiService_nativeOnClassicMidiMessage (
        JNIEnv* env, jobject thiz, jbyteArray data);

    JNIEXPORT void JNICALL
    Java_com_orbitlooper_BluetoothClassicMidiService_nativeOnBtClassicStatusChanged (
        JNIEnv* env, jobject thiz, jstring status, jstring macAddress);
}

#endif // JUCE_ANDROID
