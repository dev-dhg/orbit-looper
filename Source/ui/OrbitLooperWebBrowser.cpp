#include "OrbitLooperWebBrowser.h"
#include "../PluginEditor.h"
#include "BinaryData.h"
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#if JUCE_ANDROID
#include "../BluetoothClassicMidi.h"
#endif

namespace {
const char *linuxWebDiagUserScript = R"JS(
        (function () {
            function emitDiag(name, payload)
            {
                try
                {
                    if (window.__JUCE__ && window.__JUCE__.backend)
                        window.__JUCE__.backend.emitEvent(name, payload || {});
                }
                catch (e) {}
            }

            emitDiag('jsDiag', { msg: 'userscript-start', href: String(location.href) });

            window.addEventListener('error', function (event) {
                emitDiag('jsError', {
                    msg: String(event && event.message ? event.message : 'window-error'),
                    src: String(event && event.filename ? event.filename : ''),
                    line: Number(event && event.lineno ? event.lineno : 0),
                    col: Number(event && event.colno ? event.colno : 0)
                });
            });

            window.addEventListener('unhandledrejection', function (event) {
                var reason = '';
                try
                {
                    var value = event ? event.reason : undefined;
                    reason = String(value && value.stack ? value.stack : value);
                }
                catch (e)
                {
                    reason = 'unhandledrejection-unknown';
                }
                emitDiag('jsError', { msg: 'unhandledrejection', detail: reason });
            });

            document.addEventListener('DOMContentLoaded', function () {
                emitDiag('jsDiag', { msg: 'dom-content-loaded', ready: String(document.readyState) });
            });

            window.addEventListener('load', function () {
                emitDiag('jsDiag', { msg: 'window-load', ready: String(document.readyState) });
            });

            setTimeout(function () {
                emitDiag('jsDiag', {
                    msg: 'userscript-500ms',
                    ready: String(document.readyState),
                    hasBackend: !!(window.__JUCE__ && window.__JUCE__.backend)
                });
            }, 500);
        })();
    )JS";
}

juce::String OrbitLooperWebBrowser::sanitiseResourcePath(juce::String path) {
  if (path.isEmpty() || path == "/")
    return "/index.html";

  auto queryPos = path.indexOfChar('?');
  if (queryPos >= 0)
    path = path.substring(0, queryPos);

  auto fragmentPos = path.indexOfChar('#');
  if (fragmentPos >= 0)
    path = path.substring(0, fragmentPos);

  if (path.isEmpty() || path == "/")
    return "/index.html";

  if (!path.startsWithChar('/'))
    path = "/" + path;

  return path;
}

const char *
OrbitLooperWebBrowser::getMimeForExtension(const juce::String &extension) {
  static const std::unordered_map<juce::String, const char *> mimeMap = {
      {"html", "text/html"},        {"css", "text/css"},
      {"js", "text/javascript"},    {"mjs", "text/javascript"},
      {"json", "application/json"}, {"png", "image/png"},
      {"svg", "image/svg+xml"}};

  auto it = mimeMap.find(extension.toLowerCase());
  if (it != mimeMap.end())
    return it->second;

  return "text/plain";
}

juce::String OrbitLooperWebBrowser::getExtension(juce::String filename) {
  return filename.fromLastOccurrenceOf(".", false, false);
}

std::optional<juce::WebBrowserComponent::Resource>
OrbitLooperWebBrowser::getResource(const juce::String &url) {
  auto root = juce::WebBrowserComponent::getResourceProviderRoot();
  juce::String resourcePath = url;

  if (url.startsWithIgnoreCase(root)) {
    resourcePath = url.substring(root.length());
  }

  resourcePath = sanitiseResourcePath(resourcePath);

#if JUCE_DEBUG
  DBG("OrbitLooper resource requested. URL=" + url + " path=" + resourcePath);
#endif

  auto path = resourcePath.substring(1);

  if (path.isEmpty())
    path = "index.html";

  // Look the file up in the embedded BinaryData by its original filename —
  // no hardcoded per-file mapping needed.
  auto findResource = [](const juce::String &fileName)
      -> std::optional<juce::WebBrowserComponent::Resource> {
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i) {
      if (fileName == BinaryData::originalFilenames[i]) {
        int dataSize = 0;
        if (const char *data = BinaryData::getNamedResource(
                BinaryData::namedResourceList[i], dataSize)) {
          auto bytes = std::vector<std::byte>(
              reinterpret_cast<const std::byte *>(data),
              reinterpret_cast<const std::byte *>(data) + dataSize);
          return juce::WebBrowserComponent::Resource{
              std::move(bytes), getMimeForExtension(getExtension(fileName))};
        }
      }
    }
    return std::nullopt;
  };

  if (auto resource = findResource(path))
    return resource;

  // Deep-link fallback: serve unknown .html requests as the main page
  if (path.endsWithIgnoreCase(".html"))
    return findResource("index.html");

  return std::nullopt; // 404
}

juce::WebBrowserComponent::Options OrbitLooperWebBrowser::createOptions(
    OrbitLooperAudioProcessor &p, OrbitLooperAudioProcessorEditor *e,
    juce::WebSliderRelay &loopLevel, juce::WebSliderRelay &inputGain,
    juce::WebSliderRelay &outputGain, juce::WebSliderRelay &inputPan,
    juce::WebSliderRelay &outputPan, OrbitLooperWebBrowser *browserInstance) {
  auto options =
      juce::WebBrowserComponent::Options{}
#if JUCE_WINDOWS
          .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
          .withWinWebView2Options(
              juce::WebBrowserComponent::Options::WinWebView2{}
                  .withUserDataFolder(juce::File::getSpecialLocation(
                      juce::File::SpecialLocationType::tempDirectory))
                  .withBackgroundColour(juce::Colour::fromString("ff0a0a0f")))
#endif
          .withNativeIntegrationEnabled()
          .withUserScript(linuxWebDiagUserScript)
          .withResourceProvider([browserInstance](const auto &url) {
            return browserInstance->getResource(url);
          })
          .withOptionsFrom(loopLevel)
          .withOptionsFrom(inputGain)
          .withOptionsFrom(outputGain)
          .withOptionsFrom(inputPan)
          .withOptionsFrom(outputPan)
          .withEventListener("looperRecord",
                             [&p](const juce::var &) { p.triggerRecord(); })
          .withEventListener("looperStop",
                             [&p](const juce::var &) { p.triggerStop(); })
          .withEventListener("looperClear",
                             [&p](const juce::var &) { p.triggerClear(); })
          .withEventListener("looperUndo",
                             [&p](const juce::var &) { p.triggerUndo(); })
          .withEventListener("looperFootswitch",
                             [&p](const juce::var &) { p.triggerFootswitch(); })
          .withEventListener(
              "looperFootswitchDown",
              [&p](const juce::var &) { p.triggerFootswitchDown(); })
          .withEventListener(
              "looperFootswitchUp",
              [&p](const juce::var &) { p.triggerFootswitchUp(); })
          .withEventListener("looperOverdub",
                             [&p](const juce::var &) { p.triggerOverdub(); })
          .withEventListener("looperPlay",
                             [&p](const juce::var &) { p.triggerPlay(); })
          .withEventListener(
              "looperMonitor",
              [&p](const juce::var &) { p.triggerMonitorToggle(); })
          .withEventListener("looperExport",
                             [browserInstance](const juce::var &) {
                               browserInstance->triggerExportDialog();
                             })

          .withEventListener("midiLearnCancel",
                             [&p](const juce::var &) { p.cancelMidiLearn(); })

          .withEventListener("keyBindSet",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 2) {
                                 int actionIdx = static_cast<int>(args[0]);
                                 juce::String keyCode = args[1].toString();
                                 if (actionIdx >= 0 &&
                                     actionIdx < OrbitLooperAudioProcessor::
                                                     NUM_KEY_ACTIONS)
                                   p.setKeyBinding(
                                       static_cast<OrbitLooperAudioProcessor::
                                                       MidiAction>(actionIdx),
                                       keyCode);
                               }
                             })
          .withEventListener("keyBindClear",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1) {
                                 int actionIdx = static_cast<int>(args[0]);
                                 if (actionIdx >= 0 &&
                                     actionIdx < OrbitLooperAudioProcessor::
                                                     NUM_KEY_ACTIONS)
                                   p.clearKeyBinding(
                                       static_cast<OrbitLooperAudioProcessor::
                                                       MidiAction>(actionIdx));
                               }
                             })
          .withEventListener("metroStart",
                             [&p](const juce::var &) { p.startMetronomeDSP(); })
          .withEventListener("metroStop",
                             [&p](const juce::var &) { p.stopMetronomeDSP(); })
          .withEventListener("metroSetBPM",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.setMetronomeBPM(static_cast<float>(args[0]));
                             })
          .withEventListener("metroSetBeatsPerBar",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.setMetronomeBeatsPerBar(
                                     static_cast<int>(args[0]));
                             })
          .withEventListener("metroSetNumBars",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.setMetronomeNumBars(
                                     static_cast<int>(args[0]));
                             })
          .withEventListener("metroSetPatterns",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 2)
                                 p.setMetronomePatterns(
                                     static_cast<uint16_t>(
                                         static_cast<int>(args[0])),
                                     static_cast<uint16_t>(
                                         static_cast<int>(args[1])));
                             })
          .withEventListener("metroSetAudible",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.setMetronomeAudible(
                                     static_cast<int>(args[0]) != 0);
                             })
          .withEventListener("setOverdubArm",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.overdubArmEnabled.store(
                                     static_cast<int>(args[0]) != 0);
                             })
          .withEventListener(
              "setLoopMode",
              [&p](const juce::var &args) {
                if (args.isArray() && args.size() >= 1) {
                  int modeIdx = static_cast<int>(args[0]);
                  p.loopMode.store(modeIdx);
                  if (auto *param = dynamic_cast<juce::AudioParameterChoice *>(
                          p.apvts.getParameter("loop_mode")))
                    param->setValueNotifyingHost(
                        param->getNormalisableRange().convertTo0to1(
                            static_cast<float>(modeIdx)));
                }
              })
          .withEventListener("setGlobalMaxLength",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.setMaxLoopLength(
                                     static_cast<float>(args[0]));
                             })
          .withEventListener("setMaxLayers",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.setMaxLayerCount(static_cast<int>(args[0]));
                             })
          .withEventListener("jsDiag",
                             [](const juce::var &args) {
                               DBG("OrbitLooper: JS DIAG: " +
                                   juce::JSON::toString(args));
                             })
          .withEventListener("jsError",
                             [](const juce::var &args) {
                               DBG("OrbitLooper: JS ERROR: " +
                                   juce::JSON::toString(args));
                             })
          .withEventListener("openAudioSettings",
                             [e](const juce::var &) {
                               if (auto *holder = juce::StandalonePluginHolder::
                                       getInstance()) {
                                 auto *deviceSelector =
                                     new juce::AudioDeviceSelectorComponent(
                                         holder->deviceManager, 0, 256, 0, 256,
                                         true, true, false, false);
                                 deviceSelector->setSize(500, 600);
                                 deviceSelector->setOpaque(false);

                                 // Use LookAndFeel to apply a consistent dark
                                 // theme to all components inside the selector
                                 auto &lf = deviceSelector->getLookAndFeel();
                                 lf.setColour(
                                     juce::ResizableWindow::backgroundColourId,
                                     juce::Colour::fromString("ff0a0a0f"));
                                 lf.setColour(juce::Label::textColourId,
                                              juce::Colours::white);
                                 lf.setColour(juce::Label::backgroundColourId,
                                              juce::Colours::transparentBlack);
                                 lf.setColour(
                                     juce::ComboBox::backgroundColourId,
                                     juce::Colour::fromString("ff1a1a28"));
                                 lf.setColour(juce::ComboBox::textColourId,
                                              juce::Colours::white);
                                 lf.setColour(
                                     juce::ComboBox::outlineColourId,
                                     juce::Colour::fromString("ff2a2a3a"));
                                 lf.setColour(
                                     juce::ListBox::backgroundColourId,
                                     juce::Colour::fromString("ff12121a"));
                                 lf.setColour(
                                     juce::ListBox::outlineColourId,
                                     juce::Colour::fromString("ff2a2a3a"));
                                 lf.setColour(
                                     juce::TextButton::buttonColourId,
                                     juce::Colour::fromString("ff1a1a28"));
                                 lf.setColour(juce::TextButton::textColourOffId,
                                              juce::Colours::white);
                                 lf.setColour(
                                     juce::GroupComponent::textColourId,
                                     juce::Colours::white);
                                 lf.setColour(
                                     juce::GroupComponent::outlineColourId,
                                     juce::Colour::fromString("ff2a2a3a"));

                                 juce::DialogWindow::LaunchOptions options;
                                 options.content.setOwned(deviceSelector);
                                 options.dialogTitle = "Audio Settings";
                                 options.dialogBackgroundColour =
                                     juce::Colour::fromString("ff0a0a0f");
                                 options.componentToCentreAround = e;
                                 options.escapeKeyTriggersCloseButton = true;
                                 // On Android the native title bar has no close
                                 // button; use JUCE's own chrome instead.
#if JUCE_ANDROID
                                 options.useNativeTitleBar = false;
#else
                                 options.useNativeTitleBar = true;
#endif
                                 options.resizable = false;
                                 options.launchAsync();
                               }
                             })
          .withEventListener("toggleFullscreen",
                             [e](const juce::var &) {
                               if (e) {
                                 if (auto *top = e->getTopLevelComponent()) {
                                   if (auto *window = dynamic_cast<
                                           juce::ResizableWindow *>(top)) {
                                     window->setFullScreen(
                                         !window->isFullScreen());
                                   }
                                 }
                               }
                             })
          .withEventListener("resizeWindow",
                             [e](const juce::var &args) {
                               if (e && args.isArray() && args.size() >= 2) {
                                 int newWidth = static_cast<int>(args[0]);
                                 int newHeight = static_cast<int>(args[1]);
                                 // Enforce aspect ratio (setBounds bypasses
                                 // the ComponentBoundsConstrainer)
                                 constexpr double aspect = 580.0 / 720.0;
                                 newHeight = static_cast<int>(
                                     std::round(newWidth / aspect));
                                 if (newWidth < 240) {
                                   newWidth = 240;
                                   newHeight = static_cast<int>(
                                       std::round(240.0 / aspect));
                                 }
                                 if (newHeight < 350) {
                                   newHeight = 350;
                                   newWidth = static_cast<int>(
                                       std::round(350.0 * aspect));
                                 }
                                 e->setSize(newWidth, newHeight);
                               }
                             })
          .withEventListener("standaloneInputUnmute",
                             [&p](const juce::var &) {
                               p.feedbackMuted.store(false);
                               // Also unmute JUCE's device-level mute
                               if (auto *h = juce::StandalonePluginHolder::getInstance())
                                 h->getMuteInputValue().setValue(false);
                             })
          .withEventListener("standaloneInputMute",
                             [&p](const juce::var &) {
                               p.feedbackMuted.store(true);
                               // Also mute JUCE's device-level mute
                               if (auto *h = juce::StandalonePluginHolder::getInstance())
                                 h->getMuteInputValue().setValue(true);
                             })
          .withEventListener("setMuteOnStartup",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() > 0) {
                                 bool val = (bool)args[0];
                                 p.muteOnStartup.store(val);
                                 DBG("OrbitLooper: setMuteOnStartup = " + juce::String(val ? "true" : "false"));
                                 // Persist to PropertiesFile
                                 juce::PropertiesFile props(
                                     OrbitLooperAudioProcessorEditor::
                                         makeSettingsOptions());
                                 props.setValue("muteOnStartup", val);
                                 bool saved = props.save();
                                 DBG("OrbitLooper: PropertiesFile save() = " + juce::String(saved ? "OK" : "FAILED")
                                     + " path=" + props.getFile().getFullPathName());
                               }
                             })
#if JUCE_ANDROID
          .withEventListener("btClassicScan",
                             [browserInstance](const juce::var &) {
                               auto json = BluetoothClassicMidi::getPairedDevicesJson();
                               // Pass as an escaped string literal; JS side
                               // JSON.parses strings (injection-safe even if
                               // a device name contains quotes).
                               browserInstance->evaluateJavascript(
                                   "window.updateBtClassicDevices(" +
                                   OrbitLooperAudioProcessorEditor::jsQuoted(json) + ")");
                             })
          .withEventListener("btClassicConnect",
                             [](const juce::var &args) {
                               juce::String mac;
                               if (args.isArray() && args.size() >= 1)
                                 mac = args[0].toString();
                               else if (args.hasProperty("mac"))
                                 mac = args["mac"].toString();
                               else
                                 mac = args.toString();
                               if (mac.isNotEmpty())
                                 BluetoothClassicMidi::connectDevice(mac);
                             })
          .withEventListener("btClassicDisconnect",
                             [](const juce::var &) {
                               BluetoothClassicMidi::disconnectDevice();
                             })
#endif
          .withEventListener("uiReady", [e](const juce::var &) {
            if (e)
              e->setWebUiReady(true);
          });

  // Table-driven registration of the per-action MIDI Learn/Clear listeners
  // ("midiLearn<Action>" / "midiClear<Action>") — replaces 40 hand-written
  // listeners with one loop over the shared action-name table.
  {
    const auto &actionNames = OrbitLooperAudioProcessor::getMidiActionNames();
    for (int i = 0; i < OrbitLooperAudioProcessor::NUM_MIDI_ACTIONS; ++i) {
      const auto action =
          static_cast<OrbitLooperAudioProcessor::MidiAction>(i);
      const juce::String name(actionNames[static_cast<size_t>(i)]);
      options =
          options
              .withEventListener(
                  "midiLearn" + name,
                  [&p, action](const juce::var &) { p.startMidiLearn(action); })
              .withEventListener(
                  "midiClear" + name,
                  [&p, action](const juce::var &) { p.clearMidiCC(action); });
    }
  }

  return options;
}

OrbitLooperWebBrowser::OrbitLooperWebBrowser(
    OrbitLooperAudioProcessor &p, OrbitLooperAudioProcessorEditor *e,
    juce::WebSliderRelay &loopLevel, juce::WebSliderRelay &inputGain,
    juce::WebSliderRelay &outputGain, juce::WebSliderRelay &inputPan,
    juce::WebSliderRelay &outputPan)
    : juce::WebBrowserComponent(createOptions(p, e, loopLevel, inputGain,
                                              outputGain, inputPan, outputPan,
                                              this)),
      audioProcessor(p), editor(e) {}

OrbitLooperWebBrowser::~OrbitLooperWebBrowser() {}

void OrbitLooperWebBrowser::triggerExportDialog() {
  exportFileChooser = std::make_shared<juce::FileChooser>(
      "Export Loop as WAV",
      juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
          .getChildFile("loop.wav"),
      "*.wav");

  exportFileChooser->launchAsync(juce::FileBrowserComponent::saveMode |
                                     juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser &fc) {
                                   auto result = fc.getResult();
                                   if (result != juce::File())
                                     audioProcessor.exportLoopToWav(result);
                                 });
}

bool OrbitLooperWebBrowser::pageAboutToLoad(const juce::String &newURL) {
  DBG("OrbitLooper: pageAboutToLoad URL=" + newURL);
  return juce::WebBrowserComponent::pageAboutToLoad(newURL);
}

bool OrbitLooperWebBrowser::pageLoadHadNetworkError(
    const juce::String &errorInfo) {
  DBG("OrbitLooper: pageLoadHadNetworkError: " + errorInfo);
  return juce::WebBrowserComponent::pageLoadHadNetworkError(errorInfo);
}

void OrbitLooperWebBrowser::pageFinishedLoading(const juce::String &url) {
  DBG("OrbitLooper: pageFinishedLoading URL=" + url);
  juce::WebBrowserComponent::pageFinishedLoading(url);
}

void OrbitLooperWebBrowser::newWindowAttemptingToLoad(
    const juce::String &newURL) {
  DBG("OrbitLooper: newWindowAttemptingToLoad URL=" + newURL);
  juce::WebBrowserComponent::newWindowAttemptingToLoad(newURL);
}
