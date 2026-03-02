// ============================================================================
// STATE — Constants, DOM element references, and shared mutable state
// ============================================================================

// Constants
var STATES = { EMPTY: 0, RECORDING: 1, PLAYING: 2, OVERDUBBING: 3, STOPPED: 4 };
var STATE_NAMES = ['READY', 'RECORDING', 'PLAYING', 'OVERDUBBING', 'STOPPED'];
var STATE_CLASSES = ['empty', 'recording', 'playing', 'overdubbing', 'stopped'];
var KNOB_MIN_ANGLE = -135;
var KNOB_MAX_ANGLE = 135;
var RING_CIRCUMFERENCE = 2 * Math.PI * 145;
var MAX_LENGTH_MIN = 0;
var MAX_LENGTH_MAX = 1800; // 30 minutes
var GAIN_MIN = -60;
var GAIN_MAX = 12;

var MIDI_LEARN_EVENTS = ['midiLearnRecord', 'midiLearnStop', 'midiLearnClear', 'midiLearnUndo', 'midiLearnFootswitch', 'midiLearnOverdub', 'midiLearnBarMode', 'midiLearnClick', 'midiLearnPreCount', 'midiLearnArmOverdub', 'midiLearnPlayClick', 'midiLearnPlay', 'midiLearnMonitor', 'midiLearnLoopModeCycle', 'midiLearnClassicMode', 'midiLearnBeatsMode', 'midiLearnDynamicMode', 'midiLearnPanInputLeft', 'midiLearnPanInputCenter', 'midiLearnPanInputRight'];
var MIDI_CLEAR_EVENTS = ['midiClearRecord', 'midiClearStop', 'midiClearClear', 'midiClearUndo', 'midiClearFootswitch', 'midiClearOverdub', 'midiClearBarMode', 'midiClearClick', 'midiClearPreCount', 'midiClearArmOverdub', 'midiClearPlayClick', 'midiClearPlay', 'midiClearMonitor', 'midiClearLoopModeCycle', 'midiClearClassicMode', 'midiClearBeatsMode', 'midiClearDynamicMode', 'midiClearPanInputLeft', 'midiClearPanInputCenter', 'midiClearPanInputRight'];
var ACTION_EVENTS = ['looperRecord', 'looperStop', 'looperClear', 'looperUndo', 'looperFootswitch', 'looperOverdub', null, null, null, null, null, 'looperPlay', 'looperMonitor', 'toggleLoopModeCycle', null, null, null, null, null, null];

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
var btnAutoLength = document.getElementById('btnAutoLength');
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

// DOM Elements — Max Length Slider
var maxLengthSlider = document.getElementById('maxLengthSlider');
var maxLengthFill = document.getElementById('maxLengthFill');
var maxLengthThumb = document.getElementById('maxLengthThumb');
var maxLengthValue = document.getElementById('maxLengthValue');

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

// DOM Elements — MIDI Panel
var midiRows = document.getElementById('midiRows');
var midiCCDisplays = [document.getElementById('midiCC0'), document.getElementById('midiCC1'), document.getElementById('midiCC2'), document.getElementById('midiCC3'), document.getElementById('midiCC4'), document.getElementById('midiCC5'), document.getElementById('midiCC6'), document.getElementById('midiCC7'), document.getElementById('midiCC8'), document.getElementById('midiCC9'), document.getElementById('midiCC10'), document.getElementById('midiCC11'), document.getElementById('midiCC12'), document.getElementById('midiCC13'), document.getElementById('midiCC14'), document.getElementById('midiCC15'), document.getElementById('midiCC16'), document.getElementById('midiCC17'), document.getElementById('midiCC18'), document.getElementById('midiCC19')];
var midiLearnBtns = [document.getElementById('midiLearn0'), document.getElementById('midiLearn1'), document.getElementById('midiLearn2'), document.getElementById('midiLearn3'), document.getElementById('midiLearn4'), document.getElementById('midiLearn5'), document.getElementById('midiLearn6'), document.getElementById('midiLearn7'), document.getElementById('midiLearn8'), document.getElementById('midiLearn9'), document.getElementById('midiLearn10'), document.getElementById('midiLearn11'), document.getElementById('midiLearn12'), document.getElementById('midiLearn13'), document.getElementById('midiLearn14'), document.getElementById('midiLearn15'), document.getElementById('midiLearn16'), document.getElementById('midiLearn17'), document.getElementById('midiLearn18'), document.getElementById('midiLearn19')];
var midiClearBtns = [document.getElementById('midiClear0'), document.getElementById('midiClear1'), document.getElementById('midiClear2'), document.getElementById('midiClear3'), document.getElementById('midiClear4'), document.getElementById('midiClear5'), document.getElementById('midiClear6'), document.getElementById('midiClear7'), document.getElementById('midiClear8'), document.getElementById('midiClear9'), document.getElementById('midiClear10'), document.getElementById('midiClear11'), document.getElementById('midiClear12'), document.getElementById('midiClear13'), document.getElementById('midiClear14'), document.getElementById('midiClear15'), document.getElementById('midiClear16'), document.getElementById('midiClear17'), document.getElementById('midiClear18'), document.getElementById('midiClear19')];
var keyBindDisplays = [document.getElementById('keyBind0'), document.getElementById('keyBind1'), document.getElementById('keyBind2'), document.getElementById('keyBind3'), document.getElementById('keyBind4'), document.getElementById('keyBind5'), document.getElementById('keyBind6'), document.getElementById('keyBind7'), document.getElementById('keyBind8'), document.getElementById('keyBind9'), document.getElementById('keyBind10'), document.getElementById('keyBind11'), document.getElementById('keyBind12'), document.getElementById('keyBind13'), document.getElementById('keyBind14'), document.getElementById('keyBind15'), document.getElementById('keyBind16'), document.getElementById('keyBind17'), document.getElementById('keyBind18'), document.getElementById('keyBind19')];
var keyLearnBtns = [document.getElementById('keyLearn0'), document.getElementById('keyLearn1'), document.getElementById('keyLearn2'), document.getElementById('keyLearn3'), document.getElementById('keyLearn4'), document.getElementById('keyLearn5'), document.getElementById('keyLearn6'), document.getElementById('keyLearn7'), document.getElementById('keyLearn8'), document.getElementById('keyLearn9'), document.getElementById('keyLearn10'), document.getElementById('keyLearn11'), document.getElementById('keyLearn12'), document.getElementById('keyLearn13'), document.getElementById('keyLearn14'), document.getElementById('keyLearn15'), document.getElementById('keyLearn16'), document.getElementById('keyLearn17'), document.getElementById('keyLearn18'), document.getElementById('keyLearn19')];
var keyClearBtns = [document.getElementById('keyClear0'), document.getElementById('keyClear1'), document.getElementById('keyClear2'), document.getElementById('keyClear3'), document.getElementById('keyClear4'), document.getElementById('keyClear5'), document.getElementById('keyClear6'), document.getElementById('keyClear7'), document.getElementById('keyClear8'), document.getElementById('keyClear9'), document.getElementById('keyClear10'), document.getElementById('keyClear11'), document.getElementById('keyClear12'), document.getElementById('keyClear13'), document.getElementById('keyClear14'), document.getElementById('keyClear15'), document.getElementById('keyClear16'), document.getElementById('keyClear17'), document.getElementById('keyClear18'), document.getElementById('keyClear19')];

// DOM Elements — Metronome
var metroContent = document.getElementById('metroContent');
var modeToggle = document.getElementById('modeToggle');
var clickToggle = document.getElementById('clickToggle');
var bpmInput = document.getElementById('bpmInput');
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
var maxLengthSec = 60;
var midiPanelOpen = false;
var keyLearning = false;
var keyLearnTarget = -1;
var keyBindingsMap = ['', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', ''];  // 20 actions

// Slider states (set by initJuceBridge)
var loopLevelSliderState = null;
var maxLoopLengthSliderState = null;
var inputGainSliderState = null;
var outputGainSliderState = null;
var inputPanSliderState = null;
var outputPanSliderState = null;

// Drag flags
var isDraggingInputGain = false;
var isDraggingLoopLevel = false;
var isDraggingMaxLength = false;
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
var currentState = STATES.EMPTY;
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
var autoLengthActive = false;
var loopModeToggle = document.getElementById('loopModeToggle');
var modePill = document.getElementById('modePill');
var modeSegments = [
    document.getElementById('segClassic'),
    document.getElementById('segBars'),
    document.getElementById('segDynamic')
];

// Metronome engine
var jsMetroEngineActive = false;
