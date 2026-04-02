// ============================================================================
// MAIN — Logging, error handling, zoom, modals, settings, init, uiReady
// ============================================================================

// ========== UI LOGGING ==========
window.uiLog = function (msg) {
    const log = document.getElementById('debugLog');
    if (!log) {
        console.log("uiLog (buffered):", msg);
        return;
    }
    const entry = document.createElement('div');
    entry.style.marginBottom = '2px';
    entry.style.borderBottom = '1px solid #111';
    entry.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
    log.appendChild(entry);
    log.scrollTop = log.scrollHeight;
    console.log(msg);
};

// Catch global errors
window.onerror = function (msg, url, lineNo, columnNo, error) {
    uiLog(`JS ERROR: ${msg} at ${lineNo}:${columnNo}`);
    return false;
};

uiLog("OrbitLooper UI scripts loading...");

// ========== TOGGLE DEBUG LOG ==========
document.addEventListener('keydown', function (e) {
    if (e.ctrlKey && e.altKey && e.code === 'KeyD') {
        const log = document.getElementById('debugLog');
        if (log) log.style.display = log.style.display === 'none' ? 'block' : 'none';
        uiLog("OrbitLooper: Debug log toggled.");
    }
});

// ========== MODAL SYSTEM ==========
function toggleModal(modalId) {
    const modal = document.getElementById(modalId);
    const isOpen = modal.classList.contains('open');
    document.querySelectorAll('.modal-overlay').forEach(m => m.classList.remove('open'));
    document.querySelectorAll('.sidebar-icon').forEach(i => i.classList.remove('active'));

    if (!isOpen) {
        modal.classList.add('open');
        if (modalId === 'modalMetro') document.getElementById('iconMetro').classList.add('active');
        if (modalId === 'modalMidi') document.getElementById('iconMidi').classList.add('active');
        if (modalId === 'modalHelp') document.getElementById('iconHelp').classList.add('active');
        if (modalId === 'modalSettings') document.getElementById('iconGear').classList.add('active');
    }
}

// Sidebar click handlers
document.getElementById('iconMetro').onclick = () => toggleModal('modalMetro');
document.getElementById('iconMidi').onclick = () => toggleModal('modalMidi');
document.getElementById('iconHelp').onclick = () => toggleModal('modalHelp');
document.getElementById('iconAudio').onclick = () => emitEvent('openAudioSettings');
document.getElementById('iconGear').onclick = () => toggleModal('modalSettings');
document.getElementById('iconFullscreen').onclick = () => emitEvent('toggleFullscreen');

// ========== STANDALONE MODE ==========
window.setStandaloneMode = function (isStandalone) {
    console.log("OrbitLooper: setStandaloneMode =", isStandalone);
    const iconAudio = document.getElementById('iconAudio');
    if (iconAudio) {
        iconAudio.style.display = isStandalone ? 'flex' : 'none';
        console.log("OrbitLooper: Setting iconAudio display to", iconAudio.style.display);
    } else {
        console.warn("OrbitLooper: iconAudio not found");
    }
};

// ========== RESIZABLE ZOOM SUPPORT ==========
var currentZoomFactor = 1.0;

window.setZoom = function (factor) {
    if (Math.abs(currentZoomFactor - factor) < 0.0001) return;
    currentZoomFactor = factor;
    const wrapper = document.querySelector('.app-wrapper');
    if (wrapper) {
        wrapper.style.transform = `scale(${factor})`;
    }
};

// contentHeightChanged / ResizeObserver removed — caused resize feedback loop
// (scrollHeight changes with CSS zoom, feeding back incorrect designHeight to C++)

const resizer = document.getElementById('customResizer');
let isResizing = false;
let startX, startY;
let startWidth, startHeight;
const DESIGN_ASPECT = 580 / 720;

if (resizer) {
    resizer.addEventListener('pointerdown', (e) => {
        isResizing = true;
        startX = e.clientX;
        startY = e.clientY;

        // Use actual viewport dimensions (matches native window size)
        startWidth = window.innerWidth;
        startHeight = window.innerHeight;

        document.body.style.cursor = 'nwse-resize';
        resizer.classList.add('dragging');
        e.preventDefault();
        resizer.setPointerCapture(e.pointerId);
    });

    window.addEventListener('pointermove', (e) => {
        if (!isResizing) return;

        const dx = e.clientX - startX;
        const dy = e.clientY - startY;

        // Project mouse delta onto the aspect-ratio diagonal so both
        // dimensions always scale proportionally (maintains aspect ratio).
        const startDiag = Math.sqrt(startWidth * startWidth + startHeight * startHeight);
        const projectedDelta = (dx * startWidth + dy * startHeight) / startDiag;
        const scaleFactor = Math.max(0.1, (startDiag + projectedDelta) / startDiag);

        let newWidth = Math.round(startWidth * scaleFactor);
        let newHeight = Math.round(newWidth / DESIGN_ASPECT);

        // Enforce minimum bounds (height minimum may override width)
        if (newWidth < 240) {
            newWidth = 240;
            newHeight = Math.round(240 / DESIGN_ASPECT);
        }
        if (newHeight < 350) {
            newHeight = 350;
            newWidth = Math.round(350 * DESIGN_ASPECT);
        }

        emitEventWithArgs('resizeWindow', [newWidth, newHeight]);
    });

    window.addEventListener('pointerup', (e) => {
        if (isResizing) {
            isResizing = false;
            document.body.style.cursor = '';
            resizer.classList.remove('dragging');
            if (resizer.hasPointerCapture(e.pointerId)) {
                resizer.releasePointerCapture(e.pointerId);
            }
        }
    });
}

// ========== SETTINGS PANEL LOGIC ==========
const maxLenOptions = document.getElementById('maxLenOptions');
if (maxLenOptions) {
    maxLenOptions.querySelectorAll('.settings-opt').forEach(opt => {
        opt.addEventListener('click', function () {
            const val = parseInt(this.dataset.value);
            const prevActive = maxLenOptions.querySelector('.settings-opt.active');
            const prevVal = prevActive ? parseInt(prevActive.dataset.value) : 300;

            const applyChange = () => {
                maxLenOptions.querySelectorAll('.settings-opt').forEach(b => b.classList.remove('active'));
                this.classList.add('active');
                emitEventWithArgs('setGlobalMaxLength', [val]);
            };

            if (val > 300 && prevVal <= 300) {
                showCustomConfirm("High Memory Usage Warning",
                    "Setting a Global Max Length over 5 minutes will require significant active system RAM to allocate the 8 loop layers (e.g. 10 mins ≈ 1.8 GB, 20 mins ≈ 3.7 GB).\n\nAre you sure you want to allocate this much memory?",
                    applyChange
                );
            } else {
                applyChange();
            }
        });
    });
}

// ========== MAX LAYERS SETTING ==========
const maxLayerOptions = document.getElementById('maxLayerOptions');
if (maxLayerOptions) {
    maxLayerOptions.querySelectorAll('.settings-opt').forEach(opt => {
        opt.addEventListener('click', function () {
            const val = parseInt(this.dataset.value);
            const prevActive = maxLayerOptions.querySelector('.settings-opt.active');
            const prevVal = prevActive ? parseInt(prevActive.dataset.value) : 8;

            const applyChange = () => {
                maxLayerOptions.querySelectorAll('.settings-opt').forEach(b => b.classList.remove('active'));
                this.classList.add('active');
                emitEventWithArgs('setMaxLayers', [val]);
                // Update Help modal layer count display
                const helpLayers = document.getElementById('helpMaxLayers');
                if (helpLayers) helpLayers.textContent = val;
            };

            if (val > 4 && prevVal <= 4) {
                showCustomConfirm("High Layer Count Warning",
                    "Setting more than 4 layers increases active RAM usage significantly, especially with longer Max Length settings (e.g. 8 layers × 10 min ≈ 1.8 GB).\n\nAre you sure you want to use " + val + " layers?",
                    applyChange
                );
            } else {
                applyChange();
            }
        });
    });
}

// ========== DOMContentLoaded — Move content to modals ==========
window.addEventListener('DOMContentLoaded', () => {
    const metroContentEl = document.getElementById('metroContent');
    const midiRowsEl = document.getElementById('midiRows');

    if (metroContentEl) document.getElementById('metroModalContent').appendChild(metroContentEl);
    if (midiRowsEl) document.getElementById('midiModalContent').appendChild(midiRowsEl);

    const helpDetailContent = document.getElementById('helpDetailContent');
    if (helpDetailContent) document.getElementById('helpModalContent').appendChild(helpDetailContent);

    const settingsContent = document.getElementById('settingsContent');
    if (settingsContent) document.getElementById('settingsModalContent').appendChild(settingsContent);

    uiLog("OrbitLooper: UI Loaded. Ctrl+Alt+D to toggle logs.");

    if (metroContentEl) metroContentEl.style.display = 'flex';
    if (midiRowsEl) midiRowsEl.style.display = 'flex';
    if (helpDetailContent) helpDetailContent.style.display = 'block';
    if (settingsContent) settingsContent.style.display = 'block';
});

// ========== INIT ==========
buildRhythmMatrix();
updateBarModeDuration();
updateMaxLengthDisplay();
updateGainDisplay('input');
updateGainDisplay('output');
setTimeout(initJuceBridge, 50);

// Send initial metronome settings to C++ after bridge is ready
setTimeout(function () {
    emitEventWithArgs('metroSetBPM', [bpm]);
    emitEventWithArgs('metroSetBeatsPerBar', [beatsPerBar]);
    sendPatternBits();
}, 200);



if (inputPeakValue) {
    inputPeakValue.addEventListener('click', function (e) {
        maxInputPeakDb = -60.0;
        inputPeakValue.textContent = '0.0';
        e.preventDefault();
    });
}

if (outputPeakValue) {
    outputPeakValue.addEventListener('click', function (e) {
        maxOutputPeakDb = -60.0;
        outputPeakValue.textContent = '0.0';
        e.preventDefault();
    });
}

if (typeof window.__JUCE__ !== 'undefined' && window.__JUCE__.backend) {
    window.__JUCE__.backend.emitEvent('uiReady', {});
}

// ========== SETTINGS SYNC (called once from C++ after uiReady) ==========
// Syncs the Settings modal buttons with the actual processor state on startup.
// Fixes: default showing "5m" active while processor uses 1m (60s).
window.syncSettings = function (maxLenSeconds, maxLayers, monitorMuted) {
    // Sync Global Max Length buttons
    var maxLenOpts = document.getElementById('maxLenOptions');
    if (maxLenOpts) {
        maxLenOpts.querySelectorAll('.settings-opt').forEach(function (btn) {
            btn.classList.remove('active');
            if (parseInt(btn.dataset.value) === maxLenSeconds) {
                btn.classList.add('active');
            }
        });
    }

    // Sync Max Layers buttons
    var maxLayerOpts = document.getElementById('maxLayerOptions');
    if (maxLayerOpts) {
        maxLayerOpts.querySelectorAll('.settings-opt').forEach(function (btn) {
            btn.classList.remove('active');
            if (parseInt(btn.dataset.value) === maxLayers) {
                btn.classList.add('active');
            }
        });
    }

    // Update the help modal layer count display
    var helpLayers = document.getElementById('helpMaxLayers');
    if (helpLayers) helpLayers.textContent = maxLayers;
};

// ========== BLUETOOTH CLASSIC MIDI ==========

// Show BT Classic section on Android (mobile-index.html is only served on Android)
window.addEventListener('DOMContentLoaded', function () {
    var btGroup = document.getElementById('btClassicGroup');
    if (btGroup) btGroup.style.display = '';
});

/**
 * Called from C++ to populate the BT Classic device list.
 * @param {string|Array} devicesJson - JSON string or array of {name, address} objects
 */
window.updateBtClassicDevices = function (devicesJson) {
    var devices;
    if (typeof devicesJson === 'string') {
        try { devices = JSON.parse(devicesJson); } catch (e) { devices = []; }
    } else {
        devices = devicesJson || [];
    }

    var list = document.getElementById('btClassicDeviceList');
    if (!list) return;
    list.innerHTML = '';

    // Check for permission error marker
    var permError = document.getElementById('btClassicPermError');
    if (devices.length === 1 && devices[0].address === 'PERMISSION_ERROR') {
        if (permError) permError.style.display = '';
        return;
    }
    if (permError) permError.style.display = 'none';

    if (devices.length === 0) {
        var empty = document.createElement('div');
        empty.className = 'settings-desc';
        empty.textContent = 'No compatible Bluetooth Classic devices are paired.';
        list.appendChild(empty);
        return;
    }

    // Retrieve last-connected MAC for highlighting
    var lastMac = '';
    try { lastMac = localStorage.getItem('btClassicLastDevice') || ''; } catch (e) {}

    for (var i = 0; i < devices.length; i++) {
        (function (dev) {
            var row = document.createElement('div');
            row.className = 'bt-device-row';
            row.setAttribute('data-mac', dev.address);

            var indicator = document.createElement('span');
            indicator.className = 'bt-status-dot';

            var name = document.createElement('span');
            name.className = 'bt-device-name';
            name.textContent = dev.name || dev.address;

            var status = document.createElement('span');
            status.className = 'bt-device-status';
            status.textContent = 'Disconnected';

            // Highlight last-connected device
            if (dev.address === lastMac) {
                row.classList.add('bt-last-connected');
            }

            row.appendChild(indicator);
            row.appendChild(name);
            row.appendChild(status);

            row.addEventListener('click', function () {
                var st = status.textContent.toLowerCase();
                if (st === 'connected') {
                    emitEvent('btClassicDisconnect');
                } else if (st !== 'connecting...') {
                    emitEventWithArgs('btClassicConnect', [dev.address]);
                }
            });

            list.appendChild(row);
        })(devices[i]);
    }
};

/**
 * Called from C++ 30Hz timer to update connection status for a device.
 * @param {string} status - "connected", "connecting", "disconnected"
 * @param {string} macAddress - MAC address of the device
 */
window.updateBtClassicStatus = function (status, macAddress) {
    var list = document.getElementById('btClassicDeviceList');
    if (!list) return;

    var rows = list.querySelectorAll('.bt-device-row');
    for (var i = 0; i < rows.length; i++) {
        var row = rows[i];
        var mac = row.getAttribute('data-mac');
        var statusEl = row.querySelector('.bt-device-status');
        var dotEl = row.querySelector('.bt-status-dot');
        if (!statusEl || !dotEl) continue;

        if (mac === macAddress) {
            if (status === 'connected') {
                statusEl.textContent = 'Connected';
                statusEl.className = 'bt-device-status bt-status-connected';
                dotEl.className = 'bt-status-dot bt-dot-connected';
                try { localStorage.setItem('btClassicLastDevice', macAddress); } catch (e) {}
            } else if (status === 'connecting') {
                statusEl.textContent = 'Connecting...';
                statusEl.className = 'bt-device-status bt-status-connecting';
                dotEl.className = 'bt-status-dot bt-dot-connecting';
            } else {
                statusEl.textContent = 'Disconnected';
                statusEl.className = 'bt-device-status';
                dotEl.className = 'bt-status-dot';
            }
        } else {
            // Other devices reset to disconnected when one connects
            if (status === 'connected' || status === 'connecting') {
                statusEl.textContent = 'Disconnected';
                statusEl.className = 'bt-device-status';
                dotEl.className = 'bt-status-dot';
            }
        }
    }
};

console.log('Orbit Looper UI initialized');
