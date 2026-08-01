// ============================================================================
// MOBILE — Android-only layout/init (extracted from mobile-index.html)
// Loaded ONLY by mobile-index.html, after main.js, so its overrides
// (setZoom no-op, patched toggleModal) win over the shared definitions.
// ============================================================================

(function () {
    // ── Mobile layout init ───────────────────────────────────────────
    // No scaling: the app-wrapper fills the exact visible portrait area.
    // Android can report a landscape screen.width while this portrait-locked
    // activity starts, and innerWidth may exceed the physical surface width.
    var visW = 0;
    var visH = 0;
    var wrapper = document.querySelector('.app-wrapper');
    var muteBanner = document.getElementById('standaloneMuteBanner');

    function updateVisibleArea() {
        var screenWidth = Number(screen.width) || window.innerWidth;
        var screenHeight = Number(screen.height) || window.innerWidth;
        var nextWidth = Math.min(window.innerWidth, screenWidth, screenHeight);
        var nextHeight = window.innerHeight;
        if (nextWidth <= 0 || nextHeight <= 0) return false;

        visW = Math.round(nextWidth);
        visH = Math.round(nextHeight);
        document.documentElement.style.setProperty('--mobile-visible-width', visW + 'px');

        if (wrapper) {
            wrapper.style.position = 'fixed';
            wrapper.style.top      = '0';
            wrapper.style.left     = '0';
            wrapper.style.width    = visW + 'px';
            wrapper.style.height   = visH + 'px';
            wrapper.style.transform = 'none';
            wrapper.style.removeProperty('zoom');
        }
        if (muteBanner) muteBanner.style.width = visW + 'px';
        return true;
    }

    updateVisibleArea();
    document.body.style.overflow = 'hidden';

    // C++ calls setZoom() — ignore it; mobile layout is native-sized.
    window.setZoom = function () {};
    window.currentZoomFactor = 1.0;

    // ── Fix vh = 0 on Android WebView ───────────────────────────────
    // max-height: 90vh resolves to 0px; use concrete px instead.
    function fixVhHeights() {
        var maxH = Math.floor(visH * 0.85) + 'px';
        document.querySelectorAll('.modal-content, .modal').forEach(function (el) {
            el.style.maxHeight = maxH;
        });
    }
    fixVhHeights();

    // ── Teleport modals to <body> and constrain to visible area ─────
    // 1. position:fixed inside a CSS-transformed ancestor is clipped to
    //    the transformed box — teleport to <body> to fix this.
    // 2. The WebView viewport (innerW=646) is wider than the visible
    //    screen (visW=385) — explicitly size overlays to visW × visH.
    function applyModalSize(m) {
        if (!m) return;
        m.style.width  = visW + 'px';
        m.style.height = visH + 'px';
        m.style.top    = '0';
        m.style.left   = '0';
        var maxH    = Math.floor(visH * 0.88) + 'px';
        var modalW  = Math.floor(visW * 0.92) + 'px';
        // .modal (settings/help): flex-column, constrained to visible width + height
        m.querySelectorAll('.modal').forEach(function (el) {
            el.style.maxHeight = maxH;
            el.style.height    = maxH;
            el.style.width     = modalW;
            el.style.maxWidth  = modalW;
            // Also force the inner .modal-content to fill available space
            var inner = el.querySelector('.modal-content');
            if (inner) {
                inner.style.height = '100%';
                inner.style.maxHeight = 'none';
                inner.style.overflowY = 'auto';
            }
        });
        // .modal-content directly in overlay (metro/midi): just cap height
        m.querySelectorAll('.modal-content').forEach(function (el) {
            if (!el.closest('.modal')) {
                el.style.maxHeight = maxH;
                el.style.width     = modalW;
                el.style.maxWidth  = modalW;
            }
        });
    }

    function teleportModals() {
        document.querySelectorAll('.modal-overlay').forEach(function (m) {
            if (m.parentElement !== document.body) {
                document.body.appendChild(m);
            }
            applyModalSize(m);
        });
    }
    teleportModals();

    var layoutRefreshPending = false;
    function scheduleLayoutRefresh() {
        if (layoutRefreshPending) return;
        layoutRefreshPending = true;
        requestAnimationFrame(function () {
            requestAnimationFrame(function () {
                layoutRefreshPending = false;
                if (!updateVisibleArea()) return;
                fixVhHeights();
                document.querySelectorAll('.modal-overlay').forEach(applyModalSize);
            });
        });
    }

    window.addEventListener('resize', scheduleLayoutRefresh);
    window.addEventListener('orientationchange', scheduleLayoutRefresh);

    // ── Standalone input mute banner ─────────────────────────────
    // C++ timer pushes feedbackMuted state; this shows/hides the custom banner
    // and syncs the Settings toggle.
    window.setStandaloneMuteState = function (muted) {
        var banner = document.getElementById('standaloneMuteBanner');
        if (banner) banner.style.display = muted ? 'flex' : 'none';
        // Sync the feedbackMute toggle in Settings
        var t = document.getElementById('feedbackMuteToggle');
        if (t) {
            if (muted) t.classList.add('active');
            else t.classList.remove('active');
        }
    };

    // ── Feedback Mute toggle (manual mute/unmute in Settings) ────
    var fbToggle = document.getElementById('feedbackMuteToggle');
    if (fbToggle) {
        fbToggle.addEventListener('click', function () {
            var isActive = fbToggle.classList.toggle('active');
            emitEvent(isActive ? 'standaloneInputMute' : 'standaloneInputUnmute');
        });
    }

    // ── Mute on Startup toggle ──────────────────────────────────────
    var muteToggle = document.getElementById('muteOnStartupToggle');
    if (muteToggle) {
        muteToggle.addEventListener('click', function () {
            var isActive = muteToggle.classList.toggle('active');
            emitEventWithArgs('setMuteOnStartup', [isActive]);
        });
    }
    // C++ pushes the saved state on uiReady
    window.setMuteOnStartupState = function (enabled) {
        var t = document.getElementById('muteOnStartupToggle');
        if (!t) return;
        if (enabled) t.classList.add('active');
        else t.classList.remove('active');
    };

    // Also patch toggleModal so each open call re-applies sizing
    // (content may change e.g. after metroModalContent is populated).
    var _orig = window.toggleModal;
    window.toggleModal = function (modalId) {
        var m = document.getElementById(modalId);
        if (m && m.parentElement !== document.body) {
            document.body.appendChild(m);
        }
        applyModalSize(m);
        _orig(modalId);
    };
})();
