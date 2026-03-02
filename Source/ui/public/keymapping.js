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

// ========== MIDI LEARN/CLEAR BUTTON LISTENERS ==========
MIDI_LEARN_EVENTS.forEach(function (eventName, idx) {
    if (midiLearnBtns[idx]) {
        midiLearnBtns[idx].addEventListener('click', function () { emitEvent(eventName); });
    }
});
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
document.addEventListener('keydown', function (e) {
    if (keyLearning && keyLearnTarget >= 0) {
        e.preventDefault();
        var keyCode = e.code;
        keyBindingsMap[keyLearnTarget] = keyCode;
        emitEventWithArgs('keyBindSet', [keyLearnTarget, keyCode]);
        keyLearning = false;
        keyLearnTarget = -1;
        updateKeyBindingUI();
        return;
    }

    for (var i = 0; i < 20; i++) {
        if (keyBindingsMap[i] && keyBindingsMap[i] === e.code) {
            e.preventDefault();
            if (i === 4) {
                if (!e.repeat && !footswitchKeyHeld) {
                    footswitchKeyHeld = true;
                    emitEvent('looperFootswitchDown');
                }
            } else if (i === 0) {
                if (!e.repeat) handleRecordAction();
            } else if (i === 1 && isPreCounting) {
                if (!e.repeat) cancelPreCount();
            } else if (i === 5) {
                if (!e.repeat) emitEvent('looperOverdub');
            } else if (i === 6) {
                if (!e.repeat) modeToggle.click();
            } else if (i === 7) {
                if (!e.repeat) clickToggle.click();
            } else if (i === 8) {
                if (!e.repeat) preCountToggle.click();
            } else if (i === 9) {
                if (!e.repeat) window.toggleArmOverdub();
            } else if (i === 10) {
                if (!e.repeat) playClickBtn.click();
            } else if (i === 11) {
                if (!e.repeat) emitEvent('looperPlay');
            } else if (i === 12) {
                if (!e.repeat) window.toggleMonitor();
            } else if (i === 13) {
                if (!e.repeat) window.toggleLoopModeCycle();
            } else if (i === 14) {
                if (!e.repeat) setLoopMode(0);
            } else if (i === 15) {
                if (!e.repeat) setLoopMode(1);
            } else if (i === 16) {
                if (!e.repeat) setLoopMode(2);
            } else if (i === 17) {
                if (!e.repeat && inputPanSliderState) inputPanSliderState.setNormalisedValue(0.0);
            } else if (i === 18) {
                if (!e.repeat && inputPanSliderState) inputPanSliderState.setNormalisedValue(0.5);
            } else if (i === 19) {
                if (!e.repeat && inputPanSliderState) inputPanSliderState.setNormalisedValue(1.0);
            } else {
                if (!e.repeat) {
                    emitEvent(ACTION_EVENTS[i]);
                }
            }
            return;
        }
    }
});

document.addEventListener('keyup', function (e) {
    if (keyBindingsMap[4] && keyBindingsMap[4] === e.code) {
        e.preventDefault();
        if (footswitchKeyHeld) {
            footswitchKeyHeld = false;
            emitEvent('looperFootswitchUp');
        }
    }
});
