// ============================================================================
// TRANSPORT — State updates, button handlers, loop mode, playback ring, meters
// ============================================================================

// ========== PRE-COUNT LOGIC ==========
function handleRecordAction() {
    if (preCountEnabled && (currentState === STATES.EMPTY || currentState === STATES.STOPPED) && !isPreCounting) {
        startPreCount();
    } else if (isPreCounting) {
        return;
    } else {
        emitEvent('looperRecord');
    }
}

function startPreCount() {
    isPreCounting = true;
    preCountTotalBeats = preCountBars * beatsPerBar;
    preCountBeatsElapsed = 0;
    preCountBeatsRemaining = preCountTotalBeats;
    lastTotalBeats = -1;
    var barsLeft = Math.ceil(preCountBeatsRemaining / beatsPerBar);
    if (barsLeft < 1) barsLeft = 1;
    stateText.textContent = 'COUNT IN ' + barsLeft;
    stateText.className = 'state-text countin';
    emitEventWithArgs('metroSetAudible', [1]);
    emitEvent('metroStart');
}

function cancelPreCount() {
    if (!isPreCounting) return;
    isPreCounting = false;
    preCountBeatsRemaining = 0;
    emitEventWithArgs('metroSetAudible', [metronomeEnabled ? 1 : 0]);
    emitEvent('metroStop');
    clearBeatHighlights();
    stateText.textContent = 'READY';
    stateText.className = 'state-text empty';
}

// ========== TRANSPORT BUTTONS ==========
btnRecord.addEventListener('pointerdown', function (e) { e.preventDefault(); handleRecordAction(); });
btnStop.addEventListener('pointerdown', function (e) { e.preventDefault(); if (isPreCounting) { cancelPreCount(); return; } emitEvent('looperStop'); });
btnClear.addEventListener('pointerdown', function (e) { e.preventDefault(); if (isPreCounting) { cancelPreCount(); return; } emitEvent('looperClear'); });
btnUndo.addEventListener('pointerdown', function (e) { e.preventDefault(); emitEvent('looperUndo'); });

function toggleOverdubArm() {
    overdubArmEnabled = !overdubArmEnabled;
    btnArmOverdub.classList.toggle('active', overdubArmEnabled);
    emitEventWithArgs('setOverdubArm', [overdubArmEnabled ? 1 : 0]);
}
btnArmOverdub.addEventListener('pointerdown', function (e) {
    e.preventDefault();
    toggleOverdubArm();
});
window.toggleArmOverdub = function () {
    toggleOverdubArm();
};

var btnMonitor = document.getElementById('btnMonitor');
if (btnMonitor) {
    btnMonitor.addEventListener('pointerdown', function (e) {
        e.preventDefault();
        emitEvent('looperMonitor');
    });
}
window.toggleMonitor = function () {
    emitEvent('looperMonitor');
};

window.toggleLoopModeCycle = function () {
    var nextMode = (currentLoopMode + 1) % 3;
    setLoopMode(nextMode);
};

btnExport.addEventListener('pointerdown', function (e) { e.preventDefault(); emitEvent('looperExport'); });

// ========== LOOP MODE 3-WAY TOGGLE ==========
function updateLoopModeVisual(mode) {
    currentLoopMode = mode;
    if (!loopModeToggle || !modePill) return;

    loopModeToggle.setAttribute('data-mode', mode);

    modeSegments.forEach((seg, idx) => {
        if (seg) seg.classList.toggle('active', idx === mode);
    });

    isBarMode = (mode === 1);

    var metroModeDisp = document.getElementById('metroModeDisplay');
    if (metroModeDisp) {
        metroModeDisp.textContent = isBarMode ? 'BARS' : 'SEC';
    }
}

window.setLoopMode = function (mode) {
    updateLoopModeVisual(mode);

    if (mode === 0 || mode === 1) {
        autoLengthActive = false;
    } else {
        autoLengthActive = true;
    }
    updateAutoLengthVisual(autoLengthActive);

    emitEventWithArgs('setLoopMode', [mode]);
}

if (loopModeToggle) {
    modeSegments.forEach((seg, idx) => {
        if (seg) {
            seg.addEventListener('pointerdown', function (e) {
                e.preventDefault();
                setLoopMode(idx);
            });
        }
    });
}

if (btnAutoLength) {
    btnAutoLength.onclick = function () {
        window.toggleAutoLength();
    };
}

updateLoopModeVisual(0);

// ========== AUTO LENGTH VISUAL ==========
function updateAutoLengthVisual(active) {
    autoLengthActive = active;
    if (btnAutoLength) {
        btnAutoLength.classList.toggle('active', active);
        btnAutoLength.title = active ? 'Auto Length: ON' : 'Auto Length: OFF';
    }
}

// ========== RECORD/STOP BUTTON ICONS ==========
function updateRecordButtonIcon(state) {
    var iconHTML;
    switch (state) {
        case STATES.EMPTY:
            iconHTML = '<circle cx="12" cy="12" r="8" fill="var(--color-empty)"/>'; btnRecord.title = 'Record'; break;
        case STATES.RECORDING:
            iconHTML = '<circle cx="12" cy="12" r="8" fill="var(--color-recording)"/>'; btnRecord.title = 'Stop Recording'; break;
        case STATES.PLAYING:
            iconHTML = '<circle cx="12" cy="12" r="6" fill="none" stroke="var(--color-playing)" stroke-width="2"/><circle cx="12" cy="12" r="2" fill="var(--color-playing)"/>'; btnRecord.title = 'Overdub'; break;
        case STATES.OVERDUBBING:
            iconHTML = '<circle cx="12" cy="12" r="8" fill="var(--color-overdubbing)"/>'; btnRecord.title = 'Stop Overdubbing'; break;
        case STATES.STOPPED:
            iconHTML = '<circle cx="12" cy="12" r="6" fill="none" stroke="var(--color-stopped)" stroke-width="2"/><circle cx="12" cy="12" r="2" fill="var(--color-stopped)"/>'; btnRecord.title = 'Overdub'; break;
    }
    recordIcon.innerHTML = iconHTML;
}

function updateStopButtonIcon(state) {
    if (state === STATES.STOPPED) {
        stopIcon.innerHTML = '<polygon points="8,5 20,12 8,19" fill="currentColor"/>';
        btnStop.title = 'Play';
    } else {
        stopIcon.innerHTML = '<rect x="6" y="6" width="12" height="12" rx="1"/>';
        btnStop.title = 'Stop';
    }
}

// ========== PLAYBACK RING ==========
function updatePlaybackRing(state, position) {
    var isActive = state === STATES.PLAYING || state === STATES.OVERDUBBING || state === STATES.RECORDING;
    if (!isActive || position <= 0) {
        playbackArc.setAttribute('stroke-dasharray', '0 ' + RING_CIRCUMFERENCE);
        playheadDot.setAttribute('opacity', '0');
        return;
    }
    var ringColor;
    switch (state) {
        case STATES.RECORDING: ringColor = 'var(--color-recording)'; break;
        case STATES.OVERDUBBING: ringColor = 'var(--color-overdubbing)'; break;
        default: ringColor = 'var(--color-playing)'; break;
    }
    playbackArc.setAttribute('stroke', ringColor);
    playheadDot.setAttribute('fill', ringColor);
    var arcLength = position * RING_CIRCUMFERENCE;
    playbackArc.setAttribute('stroke-dasharray', arcLength + ' ' + RING_CIRCUMFERENCE);
    var angle = position * 2 * Math.PI - Math.PI / 2;
    var cx = 155 + 145 * Math.cos(angle);
    var cy = 155 + 145 * Math.sin(angle);
    playheadDot.setAttribute('cx', cx);
    playheadDot.setAttribute('cy', cy);
    playheadDot.setAttribute('opacity', '1');
}

// ========== METER DISPLAY ==========
function updateIntegratedMeter(meterFillEl, peakValueEl, level, isInput) {
    if (!meterFillEl) return;

    // Convert linear amplitude to decibels
    var db = -60.0;
    if (level > 0.00001) {
        db = 20 * Math.log10(level);
    }

    // Scale linear db to 0-1 range for the CSS transform scaleX. We cap at 0 for visual bar.
    var visualDb = Math.max(-60.0, Math.min(0.0, db));
    var scale = (visualDb + 60.0) / 60.0;
    meterFillEl.style.transform = 'scaleX(' + scale + ')';

    // Update peak hold (uncapped, allowing > 0.0 values)
    if (peakValueEl) {
        var currentMax = isInput ? maxInputPeakDb : maxOutputPeakDb;
        if (db > currentMax) {
            currentMax = db;
            if (isInput) maxInputPeakDb = currentMax;
            else maxOutputPeakDb = currentMax;

            // Format to 1 decimal place
            peakValueEl.textContent = currentMax.toFixed(1);
        }
    }
}

// ========== METRONOME ENGINE HELPER ==========
function updateMetronomeEngine(forceRestart) {
    if (isPreCounting) return;

    if (playClickActive) {
        emitEventWithArgs('metroSetAudible', [1]);
        if (!jsMetroEngineActive || forceRestart) {
            emitEvent('metroStart');
            jsMetroEngineActive = true;
        }
        return;
    }

    var looperActive = (currentState === STATES.RECORDING || currentState === STATES.PLAYING || currentState === STATES.OVERDUBBING);
    var shouldRun = looperActive && (metronomeEnabled || isBarMode);

    emitEventWithArgs('metroSetAudible', [metronomeEnabled ? 1 : 0]);

    if (shouldRun) {
        if (!jsMetroEngineActive || forceRestart) {
            emitEvent('metroStart');
            jsMetroEngineActive = true;
        }
    } else {
        if (jsMetroEngineActive) {
            emitEvent('metroStop');
            jsMetroEngineActive = false;
            clearBeatHighlights();
        }
    }
}

// ========== STATE UPDATE (called from C++) ==========
window.updateLooperState = function (state, position, inputLevel, outputLevel, loopLengthSec, canUndo, maxLoopLen, flashType, isArmed, isMuted, loopMode, isAutoLength) {
    try {
        updateLoopModeVisual(loopMode);
        updateAutoLengthVisual(!!isAutoLength);
        overdubIsArmed = !!isArmed;

        if (state === STATES.RECORDING && state !== prevLooperState && loopLengthSec === 0
            && preCountEnabled && !isPreCounting) {
            if (preCountJustCompleted) {
                preCountJustCompleted = false;
            } else {
                emitEvent('looperClear');
                startPreCount();
                return;
            }
        }

        currentState = state;

        var className = STATE_CLASSES[state] || 'empty';
        var label = STATE_NAMES[state] || 'READY';

        if (overdubIsArmed && state === STATES.PLAYING) {
            className = 'armed';
            label = 'ARMED';
        }

        if (!isPreCounting) {
            stateText.textContent = label;
            stateText.className = 'state-text ' + className;
        }

        knob.className = 'knob';
        if (overdubIsArmed && state === STATES.PLAYING) knob.classList.add('armed');
        else if (state === STATES.RECORDING) knob.classList.add('recording');
        else if (state === STATES.PLAYING) knob.classList.add('active');
        else if (state === STATES.OVERDUBBING) knob.classList.add('overdubbing');

        btnRecord.className = 'transport-btn btn-record state-' + className;
        updateRecordButtonIcon(state);

        btnStop.classList.toggle('disabled', state === STATES.EMPTY);
        updateStopButtonIcon(state);
        btnClear.classList.toggle('disabled', state === STATES.EMPTY);
        btnUndo.classList.toggle('disabled', !canUndo);
        btnExport.classList.toggle('disabled', state === STATES.EMPTY);

        var monitorIcon = document.getElementById('monitorIcon');
        if (btnMonitor && monitorIcon) {
            if (isMuted) {
                btnMonitor.classList.add('muted');
                btnMonitor.style.color = '#f43f5e';
                btnMonitor.title = 'Enable Monitoring (Muted)';
                monitorIcon.innerHTML = '<polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5" fill="currentColor"></polygon><line x1="23" y1="1" x2="1" y2="23" stroke="currentColor" stroke-width="2" stroke-linecap="round"></line>';
            } else {
                btnMonitor.classList.remove('muted');
                btnMonitor.style.color = '';
                btnMonitor.title = 'Disable Monitoring (Active)';
                monitorIcon.innerHTML = '<polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5" fill="currentColor"></polygon><path d="M15.54 8.46a5 5 0 0 1 0 7.07" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"></path><path d="M19.07 4.93a10 10 0 0 1 0 14.14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"></path>';
            }
        }

        updatePlaybackRing(state, position);

        if (loopLengthSec > 0) {
            var mins = Math.floor(loopLengthSec / 60);
            var secs = Math.floor(loopLengthSec % 60);
            var ms = Math.floor((loopLengthSec % 1) * 10);
            loopTime.textContent = String(mins).padStart(2, '0') + ':' + String(secs).padStart(2, '0') + '.' + ms;
        } else {
            loopTime.textContent = '--:--';
        }

        updateIntegratedMeter(inputMeterFill, inputPeakValue, inputLevel, true);
        updateIntegratedMeter(outputMeterFill, outputPeakValue, outputLevel, false);

        var displayValue = '0:00';
        var subText = isBarMode ? 'BEAT' : 'TIME';

        if (isBarMode) {
            if (currentState === STATES.EMPTY || currentState === STATES.STOPPED) {
                displayValue = '--';
            } else {
                var beatNum = (currentBeat >= 0) ? (currentBeat + 1) : 1;
                displayValue = String(beatNum);
            }
        } else {
            var timeSec = 0;
            if (state === STATES.RECORDING) {
                timeSec = position * maxLengthSec;
            } else if ((state === STATES.PLAYING || state === STATES.OVERDUBBING) && loopLengthSec > 0) {
                timeSec = position * loopLengthSec;
            } else if (state === STATES.STOPPED && loopLengthSec > 0) {
                timeSec = loopLengthSec;
                subText = 'TOTAL';
            }

            var tMin = Math.floor(timeSec / 60);
            var tSec = Math.floor(timeSec % 60);
            var tMs = Math.floor((timeSec % 1) * 100);
            displayValue = String(tMin).padStart(2, '0') + ':' + String(tSec).padStart(2, '0') + ':' + String(tMs).padStart(2, '0');
        }

        mainDisplay.textContent = displayValue;
        subDisplay.textContent = subText;

        var isActive = (state === STATES.RECORDING || state === STATES.PLAYING || state === STATES.OVERDUBBING);
        var wasActive = (prevLooperState === STATES.RECORDING || prevLooperState === STATES.PLAYING || prevLooperState === STATES.OVERDUBBING);

        if (isActive && !wasActive) {
            if (skipNextMetroAutoStart) {
                skipNextMetroAutoStart = false;
                jsMetroEngineActive = true;
            } else {
                updateMetronomeEngine(true);
            }
        }
        else if (!isActive && wasActive) {
            if (!isPreCounting && !playClickActive) {
                updateMetronomeEngine(false);
            }
        }

        prevLooperState = state;

        if (flashType === 1 || flashType === 2) {
            var flashColor = (flashType === 1) ? 'var(--color-overdubbing)' : 'var(--color-recording)';
            var bgRing = document.querySelector('.playback-ring circle:first-child');
            bgRing.setAttribute('stroke', flashColor);
            bgRing.setAttribute('stroke-width', '10');
            bgRing.setAttribute('opacity', '1');
            setTimeout(function () {
                bgRing.setAttribute('stroke', '#1e1e2e');
                bgRing.setAttribute('stroke-width', '6');
                bgRing.setAttribute('opacity', '0.5');
            }, 500);
        }
    } catch (e) {
        uiLog(`ERROR in updateLooperState: ${e.message}`);
    }
};
