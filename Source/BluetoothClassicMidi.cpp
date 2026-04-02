#if JUCE_ANDROID

#define JUCE_CORE_INCLUDE_JNI_HELPERS 1
#include "BluetoothClassicMidi.h"

//==============================================================================
// Global SPSC queue instance (producer: JNI thread, consumer: audio thread).
//==============================================================================
MidiSpscQueue gBtClassicMidiQueue;

//==============================================================================
// Module-level state for the JNI bridge.
//==============================================================================
namespace
{
    // Java service object (global ref so it survives across JNI calls).
    juce::GlobalRef gServiceObject;

    // Cached method IDs — looked up once in initialize().
    jmethodID gGetPairedSppDevicesMethod = nullptr;
    jmethodID gConnectMethod             = nullptr;
    jmethodID gDisconnectMethod          = nullptr;
    jmethodID gGetConnectionStatusMethod = nullptr;
    jmethodID gGetConnectedDeviceAddressMethod = nullptr;

    // Thread-safe status written by nativeOnBtClassicStatusChanged,
    // read by the 30 Hz editor timer.
    std::mutex gStatusMutex;
    std::string gLastCallbackStatus { "disconnected" };
    std::string gLastCallbackMac;

    bool gInitialised = false;

    //--------------------------------------------------------------------------
    // Helper: convert a jstring to juce::String, handling nullptr.
    //--------------------------------------------------------------------------
    juce::String jstringToJuceString (JNIEnv* env, jstring js)
    {
        if (js == nullptr)
            return {};

        const char* utf = env->GetStringUTFChars (js, nullptr);
        if (utf == nullptr)
            return {};

        juce::String result (utf);
        env->ReleaseStringUTFChars (js, utf);
        return result;
    }
} // anonymous namespace

//==============================================================================
// BluetoothClassicMidi namespace implementation
//==============================================================================

void BluetoothClassicMidi::initialize()
{
    if (gInitialised)
        return;

    auto* env = juce::getEnv();
    if (env == nullptr)
    {
        DBG ("BluetoothClassicMidi: JNIEnv is null");
        return;
    }

    // Find the Java class.
    jclass localClass = env->FindClass ("com/orbitlooper/BluetoothClassicMidiService");

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        DBG ("BluetoothClassicMidi: FindClass failed for BluetoothClassicMidiService");
        return;
    }

    if (localClass == nullptr)
    {
        DBG ("BluetoothClassicMidi: BluetoothClassicMidiService class not found");
        return;
    }

    // Cache method IDs.
    jmethodID ctor = env->GetMethodID (localClass, "<init>", "()V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    gGetPairedSppDevicesMethod = env->GetMethodID (localClass, "getPairedSppDevices",
                                                    "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    gConnectMethod = env->GetMethodID (localClass, "connect",
                                        "(Ljava/lang/String;)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    gDisconnectMethod = env->GetMethodID (localClass, "disconnect", "()V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    gGetConnectionStatusMethod = env->GetMethodID (localClass, "getConnectionStatus",
                                                    "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    gGetConnectedDeviceAddressMethod = env->GetMethodID (localClass, "getConnectedDeviceAddress",
                                                          "()Ljava/lang/String;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    // Create the Java service instance and hold a global ref.
    juce::LocalRef<jobject> localObj (env->NewObject (localClass, ctor));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    if (localObj.get() == nullptr)
    {
        DBG ("BluetoothClassicMidi: failed to create BluetoothClassicMidiService instance");
        return;
    }

    gServiceObject = juce::GlobalRef (localObj);
    gInitialised = true;

    DBG ("BluetoothClassicMidi: initialised successfully");
}

//==============================================================================
juce::String BluetoothClassicMidi::getPairedDevicesJson()
{
    if (! gInitialised)
        return "[]";

    auto* env = juce::getEnv();
    juce::LocalRef<jstring> result (
        (jstring) env->CallObjectMethod (gServiceObject.get(), gGetPairedSppDevicesMethod));

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return "[]";
    }

    return jstringToJuceString (env, result.get());
}

//==============================================================================
void BluetoothClassicMidi::connectDevice (const juce::String& macAddress)
{
    if (! gInitialised)
        return;

    auto* env = juce::getEnv();
    juce::LocalRef<jstring> jMac (env->NewStringUTF (macAddress.toRawUTF8()));

    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    env->CallVoidMethod (gServiceObject.get(), gConnectMethod, jMac.get());

    if (env->ExceptionCheck())
        env->ExceptionClear();
}

//==============================================================================
void BluetoothClassicMidi::disconnectDevice()
{
    if (! gInitialised)
        return;

    auto* env = juce::getEnv();
    env->CallVoidMethod (gServiceObject.get(), gDisconnectMethod);

    if (env->ExceptionCheck())
        env->ExceptionClear();
}

//==============================================================================
juce::String BluetoothClassicMidi::getConnectionStatus()
{
    if (! gInitialised)
        return "disconnected";

    auto* env = juce::getEnv();
    juce::LocalRef<jstring> result (
        (jstring) env->CallObjectMethod (gServiceObject.get(), gGetConnectionStatusMethod));

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return "disconnected";
    }

    return jstringToJuceString (env, result.get());
}

//==============================================================================
juce::String BluetoothClassicMidi::getConnectedDeviceAddress()
{
    if (! gInitialised)
        return {};

    auto* env = juce::getEnv();
    juce::LocalRef<jstring> result (
        (jstring) env->CallObjectMethod (gServiceObject.get(), gGetConnectedDeviceAddressMethod));

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return {};
    }

    return jstringToJuceString (env, result.get());
}

//==============================================================================
juce::String BluetoothClassicMidi::getLastCallbackStatus()
{
    std::lock_guard<std::mutex> lock (gStatusMutex);
    return juce::String (gLastCallbackStatus);
}

juce::String BluetoothClassicMidi::getLastCallbackMac()
{
    std::lock_guard<std::mutex> lock (gStatusMutex);
    return juce::String (gLastCallbackMac);
}

//==============================================================================
// JNI native callbacks
//==============================================================================

extern "C" JNIEXPORT void JNICALL
Java_com_orbitlooper_BluetoothClassicMidiService_nativeOnClassicMidiMessage (
    JNIEnv* env, jobject /*thiz*/, jbyteArray data)
{
    if (data == nullptr)
        return;

    const jsize len = env->GetArrayLength (data);
    if (len <= 0 || len > 3)
        return;

    jbyte buf[3] = {};
    env->GetByteArrayRegion (data, 0, len, buf);

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return;
    }

    // Push into the lock-free queue (no mutex, no heap alloc — Req 9.2).
    gBtClassicMidiQueue.pushOrDropOldest (
        reinterpret_cast<const uint8_t*> (buf), static_cast<int> (len));
}

extern "C" JNIEXPORT void JNICALL
Java_com_orbitlooper_BluetoothClassicMidiService_nativeOnBtClassicStatusChanged (
    JNIEnv* env, jobject /*thiz*/, jstring status, jstring macAddress)
{
    const char* statusUtf = (status != nullptr)
                                ? env->GetStringUTFChars (status, nullptr)
                                : nullptr;
    const char* macUtf    = (macAddress != nullptr)
                                ? env->GetStringUTFChars (macAddress, nullptr)
                                : nullptr;

    {
        std::lock_guard<std::mutex> lock (gStatusMutex);
        gLastCallbackStatus = (statusUtf != nullptr) ? statusUtf : "disconnected";
        gLastCallbackMac    = (macUtf != nullptr)    ? macUtf    : "";
    }

    if (statusUtf != nullptr)
        env->ReleaseStringUTFChars (status, statusUtf);
    if (macUtf != nullptr)
        env->ReleaseStringUTFChars (macAddress, macUtf);

    if (env->ExceptionCheck())
        env->ExceptionClear();
}

#endif // JUCE_ANDROID
