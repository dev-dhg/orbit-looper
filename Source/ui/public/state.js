// ============================================================================
// STATE — Constants, DOM element references, and shared mutable state
// ============================================================================

// Constants
var STATES = { EMPTY: 0, RECORDING: 1, PLAYING: 2, OVERDUBBING: 3, STOPPED: 4 };
var STATE_NAMES = ['READY', 'RECORDING', 'PLAYING', 'OVERDUBBING', 'STOPPED'];
var STATE_CLASSES = ['empty', 'recording', 'playing', 'overdubbing', 'stopped'];
var RING_CIRCUMFERENCE = 2 * Math.PI * 145;
var MAX_LENGTH_MAX = 1800; // 30 minutes (Bars-mode duration clamp)
var GAIN_MIN = -60;
var GAIN_MAX = 12;

var MIDI_LEARN_EVENTS = ['midiLearnRecord', 'midiLearnStop', 'midiLearnClear', 'midiLearnUndo', 'midiLearnFootswitch', 'midiLearnOverdub', 'midiLearnBarMode', 'midiLearnClick', 'midiLearnPreCount', 'midiLearnArmOverdub', 'midiLearnPlayClick', 'midiLearnPlay', 'midiLearnMonitor', 'midiLearnLoopModeCycle', 'midiLearnClassicMode', 'midiLearnBeatsMode', 'midiLearnDynamicMode', 'midiLearnPanInputLeft', 'midiLearnPanInputCenter', 'midiLearnPanInputRight'];
var MIDI_CLEAR_EVENTS = ['midiClearRecord', 'midiClearStop', 'midiClearClear', 'midiClearUndo', 'midiClearFootswitch', 'midiClearOverdub', 'midiClearBarMode', 'midiClearClick', 'midiClearPreCount', 'midiClearArmOverdub', 'midiClearPlayClick', 'midiClearPlay', 'midiClearMonitor', 'midiClearLoopModeCycle', 'midiClearClassicMode', 'midiClearBeatsMode', 'midiClearDynamicMode', 'midiClearPanInputLeft', 'midiClearPanInputCenter', 'midiClearPanInputRight'];
// Default keyboard-dispatch events per action index. null = handled by a
// dedicated function in keymapping.js (KEY_ACTION_HANDLERS) or unmapped.
var ACTION_EVENTS = ['looperRecord', 'looperStop', 'looperClear', 'looperUndo', 'looperFootswitch', 'looperOverdub', null, null, null, null, null, 'looperPlay', 'looperMonitor', null, null, null, null, null, null, null];
var NUM_ACTIONS = 20;

// DOM Elements — Transport
var knob = document.getElementById('knob');
var mainDisplay = document.getElementById('mainDisplay');
var subDisplay = document.getElementById('subDisplay');
var stateText = document.getElementById('stateText');
var loopTime = document.getElementById('loopTime');
var btnRecord = document.getElementById('btnRecord');
var btnStop = document.getElementById('btnStop');
var btnClear = document.getElementById('btnClear');
var btnUndo = document.getElementById('btnUndo');
var btnArmOverdub = document.getElementById('btnArmOverdub');
var btnExport = document.getElementById('btnExport');
var recordIcon = document.getElementById('recordIcon');
var stopIcon = document.getElementById('stopIcon');
var playbackArc = document.getElementById('playbackArc');
var playheadDot = document.getElementById('playheadDot');

// DOM Elements — Meters
var inputMeterFill = document.getElementById('inputMeterFill');
var outputMeterFill = document.getElementById('outputMeterFill');
var inputPeakValue = document.getElementById('inputPeakValue');
var outputPeakValue = document.getElementById('outputPeakValue');

// DOM Elements — Gain Sliders
var inputGainSlider = document.getElementById('inputGainSlider');
var inputGainFill = document.getElementById('inputGainFill');
var inputGainThumb = document.getElementById('inputGainThumb');
var inputGainValueEl = document.getElementById('inputGainValue');
var loopLevelSlider = document.getElementById('loopLevelSlider');
var loopLevelFill = document.getElementById('loopLevelFill');
var loopLevelThumb = document.getElementById('loopLevelThumb');
var loopLevelValueEl = document.getElementById('loopLevelValue');
var outputGainSlider = document.getElementById('outputGainSlider');
var outputGainFill = document.getElementById('outputGainFill');
var outputGainThumb = document.getElementById('outputGainThumb');
var outputGainValueEl = document.getElementById('outputGainValue');

// DOM Elements — MIDI Panel (rows are id-suffixed 0..19, built by loop)
function byIdRange(prefix, count) {
    var arr = [];
    for (var _i = 0; _i < count; _i++) arr.push(document.getElementById(prefix + _i));
    return arr;
}
var midiRows = document.getElementById('midiRows');
var midiCCDisplays = byIdRange('midiCC', NUM_ACTIONS);
var midiLearnBtns = byIdRange('midiLearn', NUM_ACTIONS);
var midiClearBtns = byIdRange('midiClear', NUM_ACTIONS);
var keyBindDisplays = byIdRange('keyBind', NUM_ACTIONS);
var keyLearnBtns = byIdRange('keyLearn', NUM_ACTIONS);
var keyClearBtns = byIdRange('keyClear', NUM_ACTIONS);

// DOM Elements — Metronome
var metroContent = document.getElementById('metroContent');
var modeToggle = document.getElementById('modeToggle');
var clickToggle = document.getElementById('clickToggle');
var bpmInput = document.getElementById('bpmInput');
var tapTempoButton = document.getElementById('tapTempoButton');
var barsInput = document.getElementById('barsInput');
var beatsInput = document.getElementById('beatsInput');
var metroInputs = document.getElementById('metroInputs');
var metroDuration = document.getElementById('metroDuration');
var accentRow = document.getElementById('accentRow');
var regularRow = document.getElementById('regularRow');
var preCountToggle = document.getElementById('preCountToggle');
var preCountBarsInput = document.getElementById('preCountBarsInput');
var preCountGroup = document.getElementById('preCountGroup');
var playClickBtn = document.getElementById('playClickBtn');

// Shared mutable state
var currentState = STATES.EMPTY;
var loopLevel = 95;
var midiPanelOpen = false;
var keyLearning = false;
var keyLearnTarget = -1;
var keyBindingsMap = new Array(NUM_ACTIONS).fill(''); // One binding per action

// Slider states (set by initJuceBridge)
var loopLevelSliderState = null;
var inputGainSliderState = null;
var outputGainSliderState = null;
var inputPanSliderState = null;
var outputPanSliderState = null;

// Drag flags
var isDraggingInputGain = false;
var isDraggingLoopLevel = false;
var isDraggingOutputGain = false;

// Gain values
var inputGainDB = 0;
var outputGainDB = 0;

// Footswitch
var footswitchKeyHeld = false;

// Metronome state
var metroPanelOpen = false;
var isBarMode = false;
var metronomeEnabled = false;
var playClickActive = false;
var bpm = 120;
var numBars = 4;
var beatsPerBar = 4;
var accentPattern = [true, false, false, false];
var regularPattern = [false, true, true, true];
var currentBeat = -1;
var totalBeats = 0;
var prevLooperState = STATES.EMPTY;
var isPreCounting = false;
var preCountJustCompleted = false;

// Peak Hold State
var maxInputPeakDb = -60.0;
var maxOutputPeakDb = -60.0;

// Pre-count State
var preCountEnabled = false;
var preCountBars = 2;
var preCountBeatsRemaining = 0;
var preCountTotalBeats = 0;
var preCountBeatsElapsed = 0;
var lastTotalBeats = -1;
var skipNextMetroAutoStart = false;

// Overdub Arm state
var overdubArmEnabled = true;
var overdubIsArmed = false;

// Loop mode state
var currentLoopMode = 0; // 0: Classic, 1: Bars, 2: Dynamic
var loopModeToggle = document.getElementById('loopModeToggle');

// Render cache — lets 30 Hz updateLooperState/updateLoopModeVisual skip DOM
// writes when nothing visual changed. MUST be defined before transport.js
// runs (updateLoopModeVisual(0) executes at load).
var _renderCache = { stateKey: '', muted: null, canUndo: null, loopMode: -1 };
var modePill = document.getElementById('modePill');
var modeSegments = [
    document.getElementById('segClassic'),
    document.getElementById('segBars'),
    document.getElementById('segDynamic')
];

// Metronome engine
var jsMetroEngineActive = false;
