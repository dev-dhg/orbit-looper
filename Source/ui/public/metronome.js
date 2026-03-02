// ============================================================================
// METRONOME — BPM/beats controls, rhythm matrix, bar mode, click toggles
// ============================================================================

// ========== PATTERN BITMASK ==========
function sendPatternBits() {
    var accentBits = 0, regularBits = 0;
    for (var i = 0; i < beatsPerBar; i++) {
        if (accentPattern[i]) accentBits |= (1 << i);
        if (regularPattern[i]) regularBits |= (1 << i);
    }
    emitEventWithArgs('metroSetPatterns', [accentBits, regularBits]);
}

// ========== BEAT FEEDBACK FROM C++ ==========
window.updateMetroBeat = function (beatInBar, newTotalBeats) {
    currentBeat = beatInBar;

    if (isBarMode && currentState !== STATES.EMPTY && currentState !== STATES.STOPPED) {
        var beatNum = (beatInBar >= 0) ? (beatInBar + 1) : 1;
        mainDisplay.textContent = String(beatNum);
    }

    if (beatInBar >= 0) {
        highlightBeat(beatInBar);
    }

    // Pre-count beat tracking
    if (isPreCounting) {
        if (lastTotalBeats < 0) {
            lastTotalBeats = newTotalBeats;
        } else if (newTotalBeats > lastTotalBeats) {
            preCountBeatsElapsed += (newTotalBeats - lastTotalBeats);
        }
        preCountBeatsRemaining = preCountTotalBeats - preCountBeatsElapsed;
        if (preCountBeatsRemaining > 0) {
            var barsLeft = Math.ceil(preCountBeatsRemaining / beatsPerBar);
            stateText.textContent = 'COUNT IN ' + barsLeft;
        } else if (preCountBeatsElapsed > 0) {
            isPreCounting = false;
            preCountJustCompleted = true;

            emitEventWithArgs('metroSetAudible', [metronomeEnabled ? 1 : 0]);

            if (!metronomeEnabled && !isBarMode) {
                emitEvent('metroStop');
                jsMetroEngineActive = false;
                clearBeatHighlights();
            } else {
                skipNextMetroAutoStart = true;
                jsMetroEngineActive = true;
            }
            stateText.textContent = 'RECORDING';
            stateText.className = 'state-text recording';
            emitEvent('looperRecord');
        }
    }
    lastTotalBeats = newTotalBeats;
};

// ========== BEAT HIGHLIGHTS ==========
function highlightBeat(beatIdx) {
    var accentCells = accentRow.querySelectorAll('.beat-cell');
    var regularCells = regularRow.querySelectorAll('.beat-cell');
    accentCells.forEach(function (c) { c.classList.remove('current-beat'); });
    regularCells.forEach(function (c) { c.classList.remove('current-beat'); });
    if (beatIdx < accentCells.length) accentCells[beatIdx].classList.add('current-beat');
    if (beatIdx < regularCells.length) regularCells[beatIdx].classList.add('current-beat');
}

function clearBeatHighlights() {
    var cells = document.querySelectorAll('.beat-cell');
    cells.forEach(function (c) { c.classList.remove('current-beat'); });
}

// ========== RHYTHM MATRIX ==========
function buildRhythmMatrix() {
    accentRow.innerHTML = '';
    regularRow.innerHTML = '';
    while (accentPattern.length < beatsPerBar) accentPattern.push(false);
    while (regularPattern.length < beatsPerBar) regularPattern.push(false);
    accentPattern.length = beatsPerBar;
    regularPattern.length = beatsPerBar;
    for (var i = 0; i < beatsPerBar; i++) {
        (function (idx) {
            var aCell = document.createElement('div');
            aCell.className = 'beat-cell accent' + (accentPattern[idx] ? ' active' : '');
            aCell.innerHTML = '<span class="beat-num">' + (idx + 1) + '</span>';
            aCell.addEventListener('click', function () {
                accentPattern[idx] = !accentPattern[idx];
                if (accentPattern[idx]) regularPattern[idx] = false;
                buildRhythmMatrix();
            });
            accentRow.appendChild(aCell);

            var rCell = document.createElement('div');
            rCell.className = 'beat-cell regular' + (regularPattern[idx] ? ' active' : '');
            rCell.innerHTML = '<span class="beat-num">' + (idx + 1) + '</span>';
            rCell.addEventListener('click', function () {
                regularPattern[idx] = !regularPattern[idx];
                if (regularPattern[idx]) accentPattern[idx] = false;
                buildRhythmMatrix();
            });
            regularRow.appendChild(rCell);
        })(i);
    }
    sendPatternBits();
}

// ========== BAR MODE DURATION ==========
function updateBarModeDuration() {
    var duration = numBars * beatsPerBar * 60 / bpm;
    if (duration > MAX_LENGTH_MAX) {
        numBars = Math.floor(MAX_LENGTH_MAX * bpm / (beatsPerBar * 60));
        if (numBars < 1) numBars = 1;
        barsInput.value = numBars;
        duration = numBars * beatsPerBar * 60 / bpm;
    }
    if (duration >= 60) {
        var dm = Math.floor(duration / 60);
        var ds = (duration % 60).toFixed(1);
        metroDuration.textContent = 'Duration: ' + dm + 'm ' + ds + 's';
    } else {
        metroDuration.textContent = 'Duration: ' + duration.toFixed(1) + 's';
    }
    maxLengthSec = Math.max(MAX_LENGTH_MIN, Math.min(MAX_LENGTH_MAX, duration));
    updateMaxLengthDisplay();
    if (maxLoopLengthSliderState) {
        var pct = (maxLengthSec - MAX_LENGTH_MIN) / (MAX_LENGTH_MAX - MAX_LENGTH_MIN);
        if (isNaN(pct) || !isFinite(pct)) pct = 0;
        maxLoopLengthSliderState.sliderDragStarted();
        maxLoopLengthSliderState.setNormalisedValue(pct);
        maxLoopLengthSliderState.sliderDragEnded();
    }
}

// ========== TOGGLE FUNCTIONS (called from C++ via MIDI) ==========
window.toggleBarMode = function () {
    var nextMode = (currentLoopMode + 1) % 3;
    setLoopMode(nextMode);
};

window.toggleClick = function () { clickToggle.click(); };
window.togglePreCount = function () { preCountToggle.click(); };
window.togglePlayClick = function () { playClickBtn.click(); };

// ========== METRONOME UI EVENT LISTENERS ==========
clickToggle.addEventListener('click', function () {
    metronomeEnabled = !metronomeEnabled;
    clickToggle.classList.toggle('active', metronomeEnabled);
    updateMetronomeEngine(false);
});

playClickBtn.addEventListener('click', function () {
    playClickActive = !playClickActive;
    playClickBtn.classList.toggle('active', playClickActive);

    var iconPlay = '<svg viewBox="0 0 24 24" width="10" height="10" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>';
    var iconStop = '<svg viewBox="0 0 24 24" width="10" height="10" fill="currentColor"><rect x="6" y="6" width="12" height="12"/></svg>';

    playClickBtn.innerHTML = playClickActive ? iconStop : iconPlay;

    updateMetronomeEngine(playClickActive);
});

bpmInput.addEventListener('input', function () {
    bpm = parseInt(bpmInput.value) || 120;
    bpm = Math.max(30, Math.min(300, bpm));
    emitEventWithArgs('metroSetBPM', [bpm]);
    updateBarModeDuration();
});

barsInput.addEventListener('input', function () {
    numBars = parseInt(barsInput.value) || 4;
    numBars = Math.max(1, Math.min(999, numBars));
    emitEventWithArgs('metroSetNumBars', [numBars]);
    updateBarModeDuration();
});

beatsInput.addEventListener('input', function () {
    var newBeats = parseInt(beatsInput.value) || 4;
    newBeats = Math.max(1, Math.min(16, newBeats));
    beatsPerBar = newBeats;
    emitEventWithArgs('metroSetBeatsPerBar', [beatsPerBar]);
    buildRhythmMatrix();
    updateBarModeDuration();
});

preCountToggle.addEventListener('click', function () {
    preCountEnabled = !preCountEnabled;
    preCountToggle.classList.toggle('active', preCountEnabled);
    preCountGroup.style.opacity = preCountEnabled ? '1' : '0.4';
    preCountGroup.style.pointerEvents = preCountEnabled ? 'auto' : 'none';
});

preCountBarsInput.addEventListener('input', function () {
    preCountBars = parseInt(preCountBarsInput.value) || 2;
    preCountBars = Math.max(1, Math.min(16, preCountBars));
});
