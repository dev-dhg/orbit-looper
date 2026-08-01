// ============================================================================
// KEY MAPPING — MIDI CC panel, key bindings, keyboard dispatch, footswitch
// ============================================================================

// ========== MIDI CC STATE UPDATE (called from C++) ==========
window.updateMidiState = function (ccRec, ccStop, ccClr, ccUndo, ccFoot, ccOvr, ccBar, ccClk, ccPre, ccArm, ccPClk, ccPlay, ccMon, ccLMC, ccClas, ccBeats, ccDyn, ccPanL, ccPanC, ccPanR, learning, learnTarget) {
    var ccValues = [ccRec, ccStop, ccClr, ccUndo, ccFoot, ccOvr, ccBar, ccClk, ccPre, ccArm, ccPClk, ccPlay, ccMon, ccLMC, ccClas, ccBeats, ccDyn, ccPanL, ccPanC, ccPanR];
    for (var i = 0; i < 20; i++) {
        var ccDisp = midiCCDisplays[i];
        var learnBtn = midiLearnBtns[i];
        var clearBtn = midiClearBtns[i];

        if (!ccDisp || !learnBtn || !clearBtn) continue;

        var cc = ccValues[i];
        var isLearning = learning && learnTarget === i;

        if (isLearning) {
            ccDisp.textContent = '...';
            ccDisp.className = 'midi-cc-display learning';
        } else if (cc >= 0) {
            ccDisp.textContent = 'CC ' + cc;
            ccDisp.className = 'midi-cc-display assigned';
        } else {
            ccDisp.textContent = '--';
            ccDisp.className = 'midi-cc-display';
        }

        if (isLearning) {
            learnBtn.textContent = 'Cancel';
            learnBtn.className = 'midi-learn-btn active';
            learnBtn.onclick = (function () { emitEvent('midiLearnCancel'); });
        } else {
            learnBtn.textContent = 'Learn';
            learnBtn.className = 'midi-learn-btn';
            (function (idx) { learnBtn.onclick = function () { emitEvent(MIDI_LEARN_EVENTS[idx]); }; })(i);
        }

        if (cc >= 0 && !isLearning) clearBtn.className = 'midi-clear-btn';
        else clearBtn.className = 'midi-clear-btn hidden';
    }
};

// ========== MIDI ACTIVITY INDICATOR (called from C++) ==========
window.updateMidiActivity = function (active) {
    var dot = document.getElementById('midiActivityDot');
    if (!dot) return;
    if (active) {
        dot.classList.add('midi-active');
        dot.classList.remove('midi-inactive');
    } else {
        dot.classList.remove('midi-active');
        dot.classList.add('midi-inactive');
    }
};

// ========== MIDI LEARN/CLEAR BUTTON LISTENERS ==========
// NOTE: Learn button onclick is set dynamically by updateMidiState() to either
// emit the learn event or midiLearnCancel depending on current state.
// Do NOT add a static addEventListener for learn buttons — it would fire the
// learn event even when the button shows "Cancel", causing start+cancel races.
MIDI_CLEAR_EVENTS.forEach(function (eventName, idx) {
    if (midiClearBtns[idx]) {
        midiClearBtns[idx].addEventListener('click', function () { emitEvent(eventName); });
    }
});

// ========== KEY LEARN BUTTONS ==========
for (var ki = 0; ki < 20; ki++) {
    (function (idx) {
        keyLearnBtns[idx].addEventListener('click', function () {
            if (keyLearning && keyLearnTarget === idx) {
                keyLearning = false;
                keyLearnTarget = -1;
                updateKeyBindingUI();
            } else {
                keyLearning = true;
                keyLearnTarget = idx;
                updateKeyBindingUI();
            }
        });
        keyClearBtns[idx].addEventListener('click', function () {
            keyBindingsMap[idx] = '';
            emitEventWithArgs('keyBindClear', [idx]);
            updateKeyBindingUI();
        });
    })(ki);
}

// ========== KEY BINDING UI ==========
function updateKeyBindingUI() {
    for (var i = 0; i < 20; i++) {
        var keyDisp = keyBindDisplays[i];
        var learnBtn = keyLearnBtns[i];
        var clearBtn = keyClearBtns[i];
        var isLearning = keyLearning && keyLearnTarget === i;
        var keyCode = keyBindingsMap[i];

        if (isLearning) {
            keyDisp.textContent = '...';
            keyDisp.className = 'midi-cc-display learning';
            learnBtn.textContent = 'Cancel';
            learnBtn.className = 'midi-learn-btn active';
        } else if (keyCode && keyCode.length > 0) {
            keyDisp.textContent = formatKeyName(keyCode);
            keyDisp.className = 'midi-cc-display assigned';
            learnBtn.textContent = 'Learn';
            learnBtn.className = 'midi-learn-btn';
        } else {
            keyDisp.textContent = '--';
            keyDisp.className = 'midi-cc-display';
            learnBtn.textContent = 'Learn';
            learnBtn.className = 'midi-learn-btn';
        }

        if (keyCode && keyCode.length > 0 && !isLearning) clearBtn.className = 'midi-clear-btn';
        else clearBtn.className = 'midi-clear-btn hidden';
    }
}

function formatKeyName(code) {
    if (code.startsWith('Key')) return code.substring(3);
    if (code.startsWith('Digit')) return code.substring(5);
    var names = {
        'Space': 'SPC', 'Escape': 'ESC', 'Enter': 'ENT', 'Backspace': 'BKSP',
        'Delete': 'DEL', 'ArrowUp': 'UP', 'ArrowDown': 'DN', 'ArrowLeft': 'LT',
        'ArrowRight': 'RT', 'Tab': 'TAB', 'ShiftLeft': 'LSH', 'ShiftRight': 'RSH',
        'ControlLeft': 'LCT', 'ControlRight': 'RCT'
    };
    return names[code] || code;
}

// ========== KEY BINDING STATE UPDATE (called from C++) ==========
window.updateKeyBindings = function (k0, k1, k2, k3, k4, k5, k6, k7, k8, k9, k10, k11, k12, k13, k14, k15, k16, k17, k18, k19) {
    keyBindingsMap = [k0, k1, k2, k3, k4, k5, k6, k7, k8, k9, k10, k11, k12, k13, k14, k15, k16, k17, k18, k19];
    updateKeyBindingUI();
};

// ========== KEYBOARD INPUT DISPATCH ==========
// Android WebView often returns empty string for e.code on external keyboards.
// Fall back to e.key and synthesize a code-compatible identifier.
function getKeyCode(e) {
    if (e.code && e.code.length > 0) return e.code;
    var k = e.key || '';
    if (k.length === 1) {
        if (k >= 'a' && k <= 'z') return 'Key' + k.toUpperCase();
        if (k >= 'A' && k <= 'Z') return 'Key' + k;
        if (k >= '0' && k <= '9') return 'Digit' + k;
    }
    var fallbacks = {
        ' ': 'Space', 'Escape': 'Escape', 'Enter': 'Enter',
        'Backspace': 'Backspace', 'Delete': 'Delete',
        'ArrowUp': 'ArrowUp', 'ArrowDown': 'ArrowDown',
        'ArrowLeft': 'ArrowLeft', 'ArrowRight': 'ArrowRight', 'Tab': 'Tab'
    };
    return fallbacks[k] || k;
}

// ========== KEYBOARD ACTION DISPATCH TABLE ==========
// Actions with dedicated behavior; anything not listed here falls back to
// emitEvent(ACTION_EVENTS[i]). Index 4 (footswitch) is gesture-based and
// handled separately in the keydown/keyup listeners.
var KEY_ACTION_HANDLERS = {
    0: function () { handleRecordAction(); },
    1: function () { if (isPreCounting) cancelPreCount(); else emitEvent('looperStop'); },
    6: function () { modeToggle.click(); },
    7: function () { clickToggle.click(); },
    8: function () { preCountToggle.click(); },
    9: function () { window.toggleArmOverdub(); },
    10: function () { playClickBtn.click(); },
    12: function () { window.toggleMonitor(); },
    13: function () { window.toggleLoopModeCycle(); },
    14: function () { setLoopMode(0); },
    15: function () { setLoopMode(1); },
    16: function () { setLoopMode(2); },
    17: function () { if (inputPanSliderState) inputPanSliderState.setNormalisedValue(0.0); },
    18: function () { if (inputPanSliderState) inputPanSliderState.setNormalisedValue(0.5); },
    19: function () { if (inputPanSliderState) inputPanSliderState.setNormalisedValue(1.0); }
};

document.addEventListener('keydown', function (e) {
    if (keyLearning && keyLearnTarget >= 0) {
        e.preventDefault();
        var keyCode = getKeyCode(e);
        keyBindingsMap[keyLearnTarget] = keyCode;
        emitEventWithArgs('keyBindSet', [keyLearnTarget, keyCode]);
        keyLearning = false;
        keyLearnTarget = -1;
        updateKeyBindingUI();
        return;
    }

    for (var i = 0; i < NUM_ACTIONS; i++) {
        if (keyBindingsMap[i] && keyBindingsMap[i] === getKeyCode(e)) {
            e.preventDefault();
            if (i === 4) {
                // Footswitch: gesture press-down (release handled on keyup)
                if (!e.repeat && !footswitchKeyHeld) {
                    footswitchKeyHeld = true;
                    emitEvent('looperFootswitchDown');
                }
            } else if (!e.repeat) {
                var handler = KEY_ACTION_HANDLERS[i];
                if (handler) handler();
                else if (ACTION_EVENTS[i]) emitEvent(ACTION_EVENTS[i]);
            }
            return;
        }
    }
});

document.addEventListener('keyup', function (e) {
    if (keyBindingsMap[4] && keyBindingsMap[4] === getKeyCode(e)) {
        e.preventDefault();
        if (footswitchKeyHeld) {
            footswitchKeyHeld = false;
            emitEvent('looperFootswitchUp');
        }
    }
});
