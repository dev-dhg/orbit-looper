// ============================================================================
// JUCE BRIDGE — SliderState class and parameter binding
// ============================================================================

// Android WebView bootstrap: JUCE can't auto-inject user scripts on Android
// (no equivalent of WKUserScript / AddScriptToExecuteOnDocumentCreated).
// The bridge scripts are stored in Java and retrieved via getAndroidUserScripts().
// We must eval them before window.__JUCE__.backend becomes available.
if (typeof window.__JUCE__ !== 'undefined' &&
    typeof window.__JUCE__.getAndroidUserScripts !== 'undefined' &&
    typeof window.__JUCE__.backend === 'undefined') {
    try {
        eval(window.__JUCE__.getAndroidUserScripts());
    } catch (e) {
        console.error("JUCE Android bridge bootstrap failed:", e);
    }
}

(function () {
    'use strict';
    try {
        if (typeof window.__JUCE__ === 'undefined') {
            console.warn("JUCE backend not available");
            return;
        }
        console.log("JUCE backend detected - initializing");

        // SliderState class
        class SliderState {
            constructor(name) {
                this.name = name;
                this.identifier = "__juce__slider" + name;
                this.scaledValue = 0;
                this.properties = { start: 0, end: 1, skew: 1, name: "", label: "", numSteps: 100, interval: 0, parameterIndex: -1 };
                this.valueChangedListeners = [];
                if (window.__JUCE__ && window.__JUCE__.backend) {
                    window.__JUCE__.backend.addEventListener(this.identifier, (event) => this.handleEvent(event));
                    window.__JUCE__.backend.emitEvent(this.identifier, { eventType: "requestInitialUpdate" });
                }
            }
            setNormalisedValue(newValue) {
                this.scaledValue = this.snapToLegalValue(this.normalisedToScaledValue(newValue));
                if (window.__JUCE__ && window.__JUCE__.backend) {
                    window.__JUCE__.backend.emitEvent(this.identifier, { eventType: "valueChanged", value: this.scaledValue });
                }
            }
            sliderDragStarted() {
                if (window.__JUCE__ && window.__JUCE__.backend) {
                    window.__JUCE__.backend.emitEvent(this.identifier, { eventType: "sliderDragStarted" });
                }
            }
            sliderDragEnded() {
                if (window.__JUCE__ && window.__JUCE__.backend) {
                    window.__JUCE__.backend.emitEvent(this.identifier, { eventType: "sliderDragEnded" });
                }
            }
            handleEvent(event) {
                if (event.eventType === "valueChanged") {
                    this.scaledValue = event.value;
                    this.valueChangedListeners.forEach(fn => fn());
                }
                if (event.eventType === "propertiesChanged") {
                    const { eventType, ...rest } = event;
                    this.properties = rest;
                }
            }
            getScaledValue() { return this.scaledValue; }
            getNormalisedValue() {
                return Math.pow((this.scaledValue - this.properties.start) / (this.properties.end - this.properties.start), this.properties.skew);
            }
            normalisedToScaledValue(normalisedValue) {
                return Math.pow(normalisedValue, 1 / this.properties.skew) * (this.properties.end - this.properties.start) + this.properties.start;
            }
            snapToLegalValue(value) {
                const interval = this.properties.interval;
                if (interval === 0) return value;
                const start = this.properties.start;
                const clamp = (val, min = 0, max = 1) => Math.max(min, Math.min(max, val));
                return clamp(start + interval * Math.floor((value - start) / interval + 0.5), this.properties.start, this.properties.end);
            }
            addValueChangedListener(fn) { this.valueChangedListeners.push(fn); }
        }

        const sliderStates = new Map();
        window.Juce = window.Juce || {};
        window.Juce.getSliderState = function (name) {
            if (!sliderStates.has(name)) sliderStates.set(name, new SliderState(name));
            return sliderStates.get(name);
        };
        console.log("JUCE parameter binding initialized");
    } catch (error) {
        console.error("JUCE library init error:", error);
    }
})();

// ========== JUCE EVENT HELPERS ==========
function emitEvent(name) {
    if (typeof window.__JUCE__ !== 'undefined' && window.__JUCE__.backend) {
        window.__JUCE__.backend.emitEvent(name, {});
    }
}

function emitEventWithArgs(name, args) {
    if (typeof window.__JUCE__ !== 'undefined' && window.__JUCE__.backend) {
        window.__JUCE__.backend.emitEvent(name, args);
    }
}
