// ============================================================================
// UI CONTROLS — Gain/pan sliders, loop level, text editing, confirm dialog
// ============================================================================

// ========== TEXT INPUT PARSING & EDITING ==========
function makeEditable(el, parser, onChange, defaultSnapValue) {
    if (!el) return;
    el.style.cursor = 'text'; // Visual tweak

    // Double click snaps to default
    el.addEventListener('dblclick', function (e) {
        e.preventDefault();
        e.stopPropagation();
        if (defaultSnapValue !== undefined) {
            onChange(defaultSnapValue);
        }
    });

    // Alt+Click or Shift+Click to manually type
    el.addEventListener('click', function (e) {
        if (!e.shiftKey && !e.altKey) return;

        var currentTxt = el.textContent.replace('dB', '').replace('%', '').trim();
        var input = document.createElement('input');
        input.type = 'text';
        input.value = currentTxt;
        input.style.width = '100%';
        input.style.boxSizing = 'border-box';
        input.style.background = '#1e1e2e';
        input.style.color = '#fff';
        input.style.border = '1px solid #33ff88';
        input.style.fontSize = '11px';
        input.style.textAlign = 'right';

        var originalHtml = el.innerHTML;
        el.innerHTML = '';
        el.appendChild(input);
        input.focus();

        var committed = false;
        function commit() {
            if (committed) return;
            committed = true;
            var finalVal = parser(input.value);
            if (finalVal !== null && !isNaN(finalVal)) {
                onChange(finalVal);
            } else {
                el.innerHTML = originalHtml;
            }
        }

        input.addEventListener('blur', commit);
        input.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') { input.blur(); }
            if (e.key === 'Escape') { committed = true; el.innerHTML = originalHtml; }
        });
    });
}

function parseTimeStr(txt) {
    if (txt.includes(':')) {
        var parts = txt.split(':');
        if (parts.length === 3) {
            return (parseFloat(parts[0]) || 0) * 60 + (parseFloat(parts[1]) || 0) + (parseFloat(parts[2]) || 0) / 100;
        } else if (parts.length === 2) {
            return (parseFloat(parts[0]) || 0) * 60 + (parseFloat(parts[1]) || 0);
        }
    }
    var val = parseFloat(txt);
    if (isNaN(val)) return null;
    if (txt.includes('m')) return val * 60;
    return val;
}

function parseDbStr(txt) {
    if (!txt || txt === '') return 0;
    var val = parseFloat(txt);
    if (isNaN(val)) return null;
    return val;
}

function showCustomConfirm(title, message, onOk) {
    document.getElementById('confirmTitle').innerText = title;
    document.getElementById('confirmMessage').innerText = message;
    var modal = document.getElementById('modalConfirm');
    modal.classList.add('open');

    var okBtn = document.getElementById('confirmOkBtn');
    var cancelBtn = document.getElementById('confirmCancelBtn');

    // Remove old listeners to prevent multiple firing
    var newOkBtn = okBtn.cloneNode(true);
    var newCancelBtn = cancelBtn.cloneNode(true);
    okBtn.parentNode.replaceChild(newOkBtn, okBtn);
    cancelBtn.parentNode.replaceChild(newCancelBtn, cancelBtn);

    newOkBtn.addEventListener('click', function () {
        modal.classList.remove('open');
        onOk();
    });

    newCancelBtn.addEventListener('click', function () {
        modal.classList.remove('open');
    });
}

// ========== GAIN DISPLAY ==========
function updateGainDisplay(which) {
    var val = (which === 'input') ? inputGainDB : outputGainDB;
    var fillEl = (which === 'input') ? inputGainFill : outputGainFill;
    var thumbEl = (which === 'input') ? inputGainThumb : outputGainThumb;
    var valueEl = (which === 'input') ? inputGainValueEl : outputGainValueEl;
    var pct;
    if (val <= 0) {
        pct = ((val - GAIN_MIN) / (0 - GAIN_MIN)) * 50;
    } else {
        pct = 50 + (val / GAIN_MAX) * 50;
    }
    pct = Math.max(0, Math.min(100, pct));
    fillEl.style.width = pct + '%';
    thumbEl.style.left = pct + '%';
    if (val <= GAIN_MIN + 0.1) {
        valueEl.textContent = '-∞ dB';
    } else {
        valueEl.textContent = (val > 0 ? '+' : '') + val.toFixed(1) + ' dB';
    }
}

function updateLoopLevelDisplay() {
    var pct = Math.max(0, Math.min(100, loopLevel));
    loopLevelFill.style.width = pct + '%';
    loopLevelThumb.style.left = pct + '%';
    loopLevelValueEl.textContent = Math.round(loopLevel);
}

// ========== MAX LOOP LENGTH SLIDER ==========
function updateMaxLengthDisplay() {
    if (!maxLengthFill || !maxLengthThumb || !maxLengthValue) return;
    var pct = ((maxLengthSec - MAX_LENGTH_MIN) / (MAX_LENGTH_MAX - MAX_LENGTH_MIN)) * 100;
    maxLengthFill.style.width = pct + '%';
    maxLengthThumb.style.left = pct + '%';

    var m = Math.floor(maxLengthSec / 60);
    var s = Math.floor(maxLengthSec % 60);
    var ms = Math.floor((maxLengthSec % 1) * 100);
    maxLengthValue.textContent = String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0') + ':' + String(ms).padStart(2, '0');
}

// ========== GAIN SLIDER SETUP ==========
function setupGainSlider(sliderEl, which) {
    sliderEl.addEventListener('pointerdown', function (e) {
        if (which === 'input') isDraggingInputGain = true;
        else if (which === 'output') isDraggingOutputGain = true;
        else if (which === 'looplevel') isDraggingLoopLevel = true;

        var rect = sliderEl.getBoundingClientRect();
        var pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));

        if (which === 'looplevel') {
            loopLevel = pct * 100;
            updateLoopLevelDisplay();
            if (loopLevelSliderState) {
                loopLevelSliderState.sliderDragStarted();
                loopLevelSliderState.setNormalisedValue(pct);
            }
        } else {
            var val;
            if (pct <= 0.5) val = GAIN_MIN + (pct / 0.5) * (0 - GAIN_MIN);
            else val = 0 + ((pct - 0.5) / 0.5) * GAIN_MAX;
            val = Math.max(GAIN_MIN, Math.min(GAIN_MAX, val));
            if (which === 'input') { inputGainDB = val; updateGainDisplay('input'); if (inputGainSliderState) { inputGainSliderState.sliderDragStarted(); inputGainSliderState.setNormalisedValue((val - GAIN_MIN) / (GAIN_MAX - GAIN_MIN)); } }
            else { outputGainDB = val; updateGainDisplay('output'); if (outputGainSliderState) { outputGainSliderState.sliderDragStarted(); outputGainSliderState.setNormalisedValue((val - GAIN_MIN) / (GAIN_MAX - GAIN_MIN)); } }
        }

        sliderEl.setPointerCapture(e.pointerId);
        e.preventDefault();
    });
}

// ========== PAN KNOB LOGIC ==========
function setupPanKnob(knobId, indicatorId, sliderState) {
    const knobEl = document.getElementById(knobId);
    const indicator = document.getElementById(indicatorId);
    if (!knobEl || !indicator || !sliderState) return;

    let isDragging = false;
    let startY = 0;
    let startVal = 0;

    const updateVisual = () => {
        let val = sliderState.getNormalisedValue();
        let pan = val * 2 - 1;
        let angle = pan * 135;
        indicator.style.transform = `rotate(${angle}deg)`;
    };

    sliderState.addValueChangedListener(updateVisual);

    knobEl.addEventListener('pointerdown', (e) => {
        isDragging = true;
        startY = e.clientY;
        startVal = sliderState.getNormalisedValue();
        sliderState.sliderDragStarted();
        knobEl.setPointerCapture(e.pointerId);
    });

    window.addEventListener('pointermove', (e) => {
        if (!isDragging) return;
        const delta = startY - e.clientY;
        let newVal = startVal + delta * 0.005;
        newVal = Math.max(0, Math.min(1, newVal));
        sliderState.setNormalisedValue(newVal);
    });

    window.addEventListener('pointerup', () => {
        if (isDragging) {
            isDragging = false;
            sliderState.sliderDragEnded();
        }
    });

    knobEl.addEventListener('dblclick', () => {
        sliderState.sliderDragStarted();
        sliderState.setNormalisedValue(0.5);
        sliderState.sliderDragEnded();
    });
}

// ========== JUCE BRIDGE INIT ==========
function initJuceBridge() {
    if (typeof window.Juce === 'undefined' || typeof window.Juce.getSliderState !== 'function') {
        console.warn('JUCE slider API not available');
        return;
    }
    try {
        loopLevelSliderState = window.Juce.getSliderState("loop_level");
        maxLoopLengthSliderState = window.Juce.getSliderState("max_loop_length");

        if (loopLevelSliderState) {
            loopLevelSliderState.addValueChangedListener(function () {
                if (isDraggingLoopLevel) return;
                loopLevel = loopLevelSliderState.getScaledValue();
                updateLoopLevelDisplay();
            });
            loopLevel = loopLevelSliderState.getScaledValue();
            updateLoopLevelDisplay();
        }
        if (maxLoopLengthSliderState) {
            maxLoopLengthSliderState.addValueChangedListener(function () {
                if (isDraggingMaxLength) return;
                maxLengthSec = maxLoopLengthSliderState.getScaledValue();
                updateMaxLengthDisplay();
            });
            maxLengthSec = maxLoopLengthSliderState.getScaledValue();
            updateMaxLengthDisplay();
        }

        inputGainSliderState = window.Juce.getSliderState("input_gain");
        outputGainSliderState = window.Juce.getSliderState("output_gain");
        inputPanSliderState = window.Juce.getSliderState("input_pan");
        outputPanSliderState = window.Juce.getSliderState("output_pan");

        setupPanKnob('inputPanKnob', 'inputPanIndicator', inputPanSliderState);
        setupPanKnob('outputPanKnob', 'outputPanIndicator', outputPanSliderState);

        if (inputGainSliderState) {
            inputGainSliderState.addValueChangedListener(function () {
                if (isDraggingInputGain) return;
                inputGainDB = inputGainSliderState.getScaledValue();
                updateGainDisplay('input');
            });
            inputGainDB = inputGainSliderState.getScaledValue();
            updateGainDisplay('input');
        }
        if (outputGainSliderState) {
            outputGainSliderState.addValueChangedListener(function () {
                if (isDraggingOutputGain) return;
                outputGainDB = outputGainSliderState.getScaledValue();
                updateGainDisplay('output');
            });
            outputGainDB = outputGainSliderState.getScaledValue();
            updateGainDisplay('output');
        }

        console.log('JUCE bridge initialized successfully');
    } catch (e) {
        console.error('Error initializing JUCE bridge:', e);
    }
}

// ========== WIRE UP EDITABLE CONTROLS ==========
makeEditable(inputGainValueEl, parseDbStr, function (val) {
    inputGainDB = Math.max(GAIN_MIN, Math.min(GAIN_MAX, val));
    if (inputGainSliderState) {
        var apvtsPct = (inputGainDB - GAIN_MIN) / (GAIN_MAX - GAIN_MIN);
        inputGainSliderState.setNormalisedValue(apvtsPct);
        inputGainSliderState.sliderDragEnded();
    }
    updateGainDisplay('input');
}, 0);

makeEditable(outputGainValueEl, parseDbStr, function (val) {
    outputGainDB = Math.max(GAIN_MIN, Math.min(GAIN_MAX, val));
    if (outputGainSliderState) {
        var apvtsPct = (outputGainDB - GAIN_MIN) / (GAIN_MAX - GAIN_MIN);
        outputGainSliderState.setNormalisedValue(apvtsPct);
        outputGainSliderState.sliderDragEnded();
    }
    updateGainDisplay('output');
}, 0);

makeEditable(loopLevelValueEl, parseDbStr, function (val) {
    loopLevel = Math.max(0, Math.min(100, val));
    if (loopLevelSliderState) {
        loopLevelSliderState.setNormalisedValue(loopLevel / 100.0);
        loopLevelSliderState.sliderDragEnded();
    }
    updateLoopLevelDisplay();
}, 95);

makeEditable(maxLengthValue, parseTimeStr, function (val) {
    if (val > 300) {
        showCustomConfirm("High Memory Usage Warning",
            "Setting a Max Length over 5 minutes will require significant active system RAM to allocate the 8 loop layers (e.g. 5 mins ≈ 0.9 GB, 20 mins ≈ 3.7 GB).\n\nAre you sure you want to allocate this much memory?",
            function () {
                maxLengthSec = Math.max(MAX_LENGTH_MIN, Math.min(MAX_LENGTH_MAX, val));
                if (maxLoopLengthSliderState) {
                    var pct = (maxLengthSec - MAX_LENGTH_MIN) / (MAX_LENGTH_MAX - MAX_LENGTH_MIN);
                    maxLoopLengthSliderState.setNormalisedValue(pct);
                    maxLoopLengthSliderState.sliderDragEnded();
                }
                updateMaxLengthDisplay();
            }
        );
        return;
    }
    maxLengthSec = Math.max(MAX_LENGTH_MIN, Math.min(MAX_LENGTH_MAX, val));
    if (maxLoopLengthSliderState) {
        var pct = (maxLengthSec - MAX_LENGTH_MIN) / (MAX_LENGTH_MAX - MAX_LENGTH_MIN);
        maxLoopLengthSliderState.setNormalisedValue(pct);
        maxLoopLengthSliderState.sliderDragEnded();
    }
    updateMaxLengthDisplay();
}, 0);

// ========== WIRE UP GAIN SLIDERS ==========
setupGainSlider(inputGainSlider, 'input');
setupGainSlider(outputGainSlider, 'output');
setupGainSlider(loopLevelSlider, 'looplevel');

document.addEventListener('pointermove', function (e) {
    if (isDraggingInputGain) {
        var rect = inputGainSlider.getBoundingClientRect();
        var pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
        if (pct <= 0.5) inputGainDB = GAIN_MIN + (pct / 0.5) * (0 - GAIN_MIN);
        else inputGainDB = 0 + ((pct - 0.5) / 0.5) * GAIN_MAX;
        updateGainDisplay('input');
        if (inputGainSliderState) inputGainSliderState.setNormalisedValue((inputGainDB - GAIN_MIN) / (GAIN_MAX - GAIN_MIN));
    }
    if (isDraggingOutputGain) {
        var rect = outputGainSlider.getBoundingClientRect();
        var pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
        if (pct <= 0.5) outputGainDB = GAIN_MIN + (pct / 0.5) * (0 - GAIN_MIN);
        else outputGainDB = 0 + ((pct - 0.5) / 0.5) * GAIN_MAX;
        updateGainDisplay('output');
        if (outputGainSliderState) outputGainSliderState.setNormalisedValue((outputGainDB - GAIN_MIN) / (GAIN_MAX - GAIN_MIN));
    }
    if (isDraggingLoopLevel) {
        var rect = loopLevelSlider.getBoundingClientRect();
        var pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
        loopLevel = pct * 100;
        updateLoopLevelDisplay();
        if (loopLevelSliderState) loopLevelSliderState.setNormalisedValue(pct);
    }
});

document.addEventListener('pointerup', function () {
    if (isDraggingInputGain) { isDraggingInputGain = false; if (inputGainSliderState) inputGainSliderState.sliderDragEnded(); }
    if (isDraggingOutputGain) { isDraggingOutputGain = false; if (outputGainSliderState) outputGainSliderState.sliderDragEnded(); }
    if (isDraggingLoopLevel) { isDraggingLoopLevel = false; if (loopLevelSliderState) loopLevelSliderState.sliderDragEnded(); }
});

// ========== MAX LENGTH SLIDER DRAG ==========
if (maxLengthSlider) {
    maxLengthSlider.addEventListener('pointerdown', function (e) {
        isDraggingMaxLength = true;
        var rect = maxLengthSlider.getBoundingClientRect();
        var pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
        maxLengthSec = MAX_LENGTH_MIN + pct * (MAX_LENGTH_MAX - MAX_LENGTH_MIN);
        updateMaxLengthDisplay();
        maxLengthSlider.setPointerCapture(e.pointerId);
        if (maxLoopLengthSliderState) { maxLoopLengthSliderState.sliderDragStarted(); maxLoopLengthSliderState.setNormalisedValue(pct); }
        e.preventDefault();
    });

    document.addEventListener('pointermove', function (e) {
        if (!isDraggingMaxLength) return;
        var rect = maxLengthSlider.getBoundingClientRect();
        var pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
        maxLengthSec = MAX_LENGTH_MIN + pct * (MAX_LENGTH_MAX - MAX_LENGTH_MIN);
        updateMaxLengthDisplay();
        if (maxLoopLengthSliderState) maxLoopLengthSliderState.setNormalisedValue(pct);
    });

    document.addEventListener('pointerup', function () {
        if (!isDraggingMaxLength) return;
        isDraggingMaxLength = false;
        if (maxLengthSec > 300) {
            showCustomConfirm("High Memory Usage Warning",
                "Setting a Max Length over 5 minutes will require significant active system RAM to allocate the 8 loop layers (e.g. 5 mins ≈ 0.9 GB, 20 mins ≈ 3.7 GB).\n\nAre you sure you want to allocate this much memory?",
                function () {
                    if (maxLoopLengthSliderState) maxLoopLengthSliderState.sliderDragEnded();
                }
            );

            var cancelBtn = document.getElementById('confirmCancelBtn');
            var handleCancel = function () {
                maxLengthSec = 300;
                updateMaxLengthDisplay();
                if (maxLoopLengthSliderState) maxLoopLengthSliderState.setNormalisedValue((300 - MAX_LENGTH_MIN) / (MAX_LENGTH_MAX - MAX_LENGTH_MIN));
                if (maxLoopLengthSliderState) maxLoopLengthSliderState.sliderDragEnded();
                cancelBtn.removeEventListener('click', handleCancel);
            };
            cancelBtn.addEventListener('click', handleCancel);

        } else {
            if (maxLoopLengthSliderState) maxLoopLengthSliderState.sliderDragEnded();
        }
    });
}
