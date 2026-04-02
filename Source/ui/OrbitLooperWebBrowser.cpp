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

  const char *resourceData = nullptr;
  int resourceSize = 0;
  juce::String mimeType;

  auto path = resourcePath.substring(1);

  if (path.isEmpty())
    path = "index.html";

#if JUCE_ANDROID
  if (path == "mobile-index.html") {
    resourceData = BinaryData::mobileindex_html;
    resourceSize = BinaryData::mobileindex_htmlSize;
    mimeType = "text/html";
  } else
#endif
  if (path == "index.html" || path.endsWithIgnoreCase(".html")) {
    path = "index.html";
    resourceData = BinaryData::index_html;
    resourceSize = BinaryData::index_htmlSize;
    mimeType = "text/html";
  } else if (path == "style.css") {
    resourceData = BinaryData::style_css;
    resourceSize = BinaryData::style_cssSize;
    mimeType = "text/css";
  } else if (path == "juce-bridge.js") {
    resourceData = BinaryData::jucebridge_js;
    resourceSize = BinaryData::jucebridge_jsSize;
    mimeType = "text/javascript";
  } else if (path == "state.js") {
    resourceData = BinaryData::state_js;
    resourceSize = BinaryData::state_jsSize;
    mimeType = "text/javascript";
  } else if (path == "ui-controls.js") {
    resourceData = BinaryData::uicontrols_js;
    resourceSize = BinaryData::uicontrols_jsSize;
    mimeType = "text/javascript";
  } else if (path == "transport.js") {
    resourceData = BinaryData::transport_js;
    resourceSize = BinaryData::transport_jsSize;
    mimeType = "text/javascript";
  } else if (path == "metronome.js") {
    resourceData = BinaryData::metronome_js;
    resourceSize = BinaryData::metronome_jsSize;
    mimeType = "text/javascript";
  } else if (path == "keymapping.js") {
    resourceData = BinaryData::keymapping_js;
    resourceSize = BinaryData::keymapping_jsSize;
    mimeType = "text/javascript";
  } else if (path == "main.js") {
    resourceData = BinaryData::main_js;
    resourceSize = BinaryData::main_jsSize;
    mimeType = "text/javascript";
  }

  if (resourceData != nullptr) {
    auto data = std::vector<std::byte>(
        reinterpret_cast<const std::byte *>(resourceData),
        reinterpret_cast<const std::byte *>(resourceData) + resourceSize);
    return juce::WebBrowserComponent::Resource{std::move(data),
                                               mimeType.toStdString()};
  }

  juce::String errorHtml =
      "<html><body><h1>404 - Not Found</h1><p>Resource: " + path +
      "</p></body></html>";
  auto errorData = std::vector<std::byte>(
      reinterpret_cast<const std::byte *>(errorHtml.toRawUTF8()),
      reinterpret_cast<const std::byte *>(errorHtml.toRawUTF8()) +
          errorHtml.getNumBytesAsUTF8());
  return juce::WebBrowserComponent::Resource{std::move(errorData), "text/html"};
}

juce::WebBrowserComponent::Options OrbitLooperWebBrowser::createOptions(
    OrbitLooperAudioProcessor &p, OrbitLooperAudioProcessorEditor *e,
    juce::WebSliderRelay &loopLevel, juce::WebSliderRelay &maxLoopLength,
    juce::WebSliderRelay &inputGain, juce::WebSliderRelay &outputGain,
    juce::WebSliderRelay &inputPan, juce::WebSliderRelay &outputPan,
    OrbitLooperWebBrowser *browserInstance) {
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
          .withOptionsFrom(maxLoopLength)
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

          .withEventListener(
              "midiLearnRecord",
              [&p](const juce::var &) {
                p.startMidiLearn(OrbitLooperAudioProcessor::MidiAction::Record);
              })
          .withEventListener("midiLearnStop",
                             [&p](const juce::var &) {
                               p.startMidiLearn(
                                   OrbitLooperAudioProcessor::MidiAction::Stop);
                             })
          .withEventListener(
              "midiLearnClear",
              [&p](const juce::var &) {
                p.startMidiLearn(OrbitLooperAudioProcessor::MidiAction::Clear);
              })
          .withEventListener("midiLearnUndo",
                             [&p](const juce::var &) {
                               p.startMidiLearn(
                                   OrbitLooperAudioProcessor::MidiAction::Undo);
                             })
          .withEventListener(
              "midiLearnFootswitch",
              [&p](const juce::var &) {
                p.startMidiLearn(
                    OrbitLooperAudioProcessor::MidiAction::Footswitch);
              })
          .withEventListener(
              "midiLearnOverdub",
              [&p](const juce::var &) {
                p.startMidiLearn(
                    OrbitLooperAudioProcessor::MidiAction::Overdub);
              })
          .withEventListener(
              "midiLearnBarMode",
              [&p](const juce::var &) {
                p.startMidiLearn(
                    OrbitLooperAudioProcessor::MidiAction::BarMode);
              })
          .withEventListener(
              "midiLearnClick",
              [&p](const juce::var &) {
                p.startMidiLearn(OrbitLooperAudioProcessor::MidiAction::Click);
              })
          .withEventListener(
              "midiLearnPreCount",
              [&p](const juce::var &) {
                p.startMidiLearn(
                    OrbitLooperAudioProcessor::MidiAction::PreCount);
              })
          .withEventListener(
              "midiLearnArmOverdub",
              [&p](const juce::var &) {
                p.startMidiLearn(
                    OrbitLooperAudioProcessor::MidiAction::ArmOverdub);
              })
          .withEventListener(
              "midiLearnPlayClick",
              [&p](const juce::var &) {
                p.startMidiLearn(
                    OrbitLooperAudioProcessor::MidiAction::PlayClick);
              })
          .withEventListener("midiLearnPlay",
                             [&p](const juce::var &) {
                               p.startMidiLearn(
                                   OrbitLooperAudioProcessor::MidiAction::Play);
                             })
          .withEventListener("midiLearnMonitor",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::Monitor);
                             })
          .withEventListener("midiLearnLoopModeCycle",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::LoopModeCycle);
                             })
          .withEventListener("midiLearnClassicMode",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::ClassicMode);
                             })
          .withEventListener("midiLearnBeatsMode",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::BeatsMode);
                             })
          .withEventListener("midiLearnDynamicMode",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::DynamicMode);
                             })
          .withEventListener("midiLearnPanInputLeft",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::PanInputLeft);
                             })
          .withEventListener("midiLearnPanInputCenter",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::PanInputCenter);
                             })
          .withEventListener("midiLearnPanInputRight",
                             [&p](const juce::var &) {
                               p.startMidiLearn(OrbitLooperAudioProcessor::
                                                    MidiAction::PanInputRight);
                             })
          .withEventListener("midiLearnCancel",
                             [&p](const juce::var &) { p.cancelMidiLearn(); })

          .withEventListener("midiClearRecord",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::Record);
                             })
          .withEventListener("midiClearStop",
                             [&p](const juce::var &) {
                               p.clearMidiCC(
                                   OrbitLooperAudioProcessor::MidiAction::Stop);
                             })
          .withEventListener("midiClearClear",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::Clear);
                             })
          .withEventListener("midiClearUndo",
                             [&p](const juce::var &) {
                               p.clearMidiCC(
                                   OrbitLooperAudioProcessor::MidiAction::Undo);
                             })
          .withEventListener("midiClearFootswitch",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::Footswitch);
                             })
          .withEventListener("midiClearOverdub",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::Overdub);
                             })
          .withEventListener("midiClearBarMode",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::BarMode);
                             })
          .withEventListener("midiClearClick",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::Click);
                             })
          .withEventListener("midiClearPreCount",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::PreCount);
                             })
          .withEventListener("midiClearArmOverdub",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::ArmOverdub);
                             })
          .withEventListener("midiClearPlayClick",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::PlayClick);
                             })
          .withEventListener("midiClearPlay",
                             [&p](const juce::var &) {
                               p.clearMidiCC(
                                   OrbitLooperAudioProcessor::MidiAction::Play);
                             })
          .withEventListener("midiClearMonitor",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::Monitor);
                             })
          .withEventListener("midiClearLoopModeCycle",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::LoopModeCycle);
                             })
          .withEventListener("midiClearClassicMode",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::ClassicMode);
                             })
          .withEventListener("midiClearBeatsMode",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::BeatsMode);
                             })
          .withEventListener("midiClearDynamicMode",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::DynamicMode);
                             })
          .withEventListener("midiClearPanInputLeft",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::PanInputLeft);
                             })
          .withEventListener("midiClearPanInputCenter",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::PanInputCenter);
                             })
          .withEventListener("midiClearPanInputRight",
                             [&p](const juce::var &) {
                               p.clearMidiCC(OrbitLooperAudioProcessor::
                                                 MidiAction::PanInputRight);
                             })

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
          .withEventListener("setAutoLength",
                             [&p](const juce::var &args) {
                               if (args.isArray() && args.size() >= 1)
                                 p.isAutoLength.store(
                                     static_cast<int>(args[0]) != 0);
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
                                 juce::PropertiesFile::Options opts;
                                 opts.applicationName = "OrbitLooper";
                                 opts.folderName = "OrbitLooper";
                                 opts.filenameSuffix = ".settings";
                                 juce::PropertiesFile props(opts);
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
                               browserInstance->evaluateJavascript(
                                   "window.updateBtClassicDevices(" + json + ")");
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

  return options;
}

OrbitLooperWebBrowser::OrbitLooperWebBrowser(
    OrbitLooperAudioProcessor &p, OrbitLooperAudioProcessorEditor *e,
    juce::WebSliderRelay &loopLevel, juce::WebSliderRelay &maxLoopLength,
    juce::WebSliderRelay &inputGain, juce::WebSliderRelay &outputGain,
    juce::WebSliderRelay &inputPan, juce::WebSliderRelay &outputPan)
    : juce::WebBrowserComponent(createOptions(p, e, loopLevel, maxLoopLength,
                                              inputGain, outputGain, inputPan,
                                              outputPan, this)),
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
