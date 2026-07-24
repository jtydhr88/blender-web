/**
 * Web Input Device Detector
 * Inspired by Blender's GHOST input system
 * Detects whether user is using mouse or trackpad
 */

class InputDeviceDetector {
    constructor() {
        // Device detection state
        this.deviceType = 'unknown';
        this.confidence = 0;
        
        // Event tracking
        this.eventHistory = [];
        this.stats = {
            total: 0,
            wheel: 0,
            trackpad: 0,
            gesture: 0
        };
        
        // Detection parameters (inspired by Blender's approach)
        this.detection = {
            wheelDeltaHistory: [],
            lastWheelTime: 0,
            consecutiveSmallDeltas: 0,
            hasInertia: false,
            hasPrecisionScrolling: false,
            deltaMode: null
        };
        
        // Platform detection
        this.platform = this.detectPlatform();
        
        // UI elements
        this.elements = {
            deviceIndicator: document.getElementById('deviceIndicator'),
            confidence: document.getElementById('confidence'),
            totalEvents: document.getElementById('totalEvents'),
            wheelEvents: document.getElementById('wheelEvents'),
            trackpadEvents: document.getElementById('trackpadEvents'),
            gestureEvents: document.getElementById('gestureEvents'),
            eventLog: document.getElementById('eventLog'),
            testArea: document.getElementById('testArea'),
            testObject: document.getElementById('testObject'),
            platform: document.getElementById('platform'),
            touchSupport: document.getElementById('touchSupport'),
            deltaMode: document.getElementById('deltaMode'),
            gestureSupport: document.getElementById('gestureSupport'),
            lastDeltaY: document.getElementById('lastDeltaY'),
            lastDeltaX: document.getElementById('lastDeltaX'),
            isFloatDelta: document.getElementById('isFloatDelta'),
            eventFrequency: document.getElementById('eventFrequency')
        };
        
        // Transform state for test object
        this.transform = {
            scale: 1,
            rotation: 0,
            translateX: 0,
            translateY: 0
        };
        
        this.init();
    }
    
    detectPlatform() {
        const ua = navigator.userAgent;
        const platform = navigator.platform;
        
        if (ua.indexOf('Win') !== -1) return 'Windows';
        if (ua.indexOf('Mac') !== -1) return 'macOS';
        if (ua.indexOf('Linux') !== -1) return 'Linux';
        if (ua.indexOf('Android') !== -1) return 'Android';
        if (ua.indexOf('iOS') !== -1) return 'iOS';
        
        return platform || 'Unknown';
    }
    
    init() {
        // Display platform info
        this.elements.platform.textContent = this.platform;
        this.elements.touchSupport.textContent = 'ontouchstart' in window ? 'Yes' : 'No';
        this.elements.gestureSupport.textContent = 'ongesturestart' in window ? 'Yes (Safari)' : 'No';
        
        // Wheel event listener (similar to Blender's WM_MOUSEWHEEL handling)
        this.elements.testArea.addEventListener('wheel', (e) => {
            e.preventDefault();
            this.handleWheelEvent(e);
        }, { passive: false });
        
        // Gesture events (Safari/iOS - similar to macOS NSEventTypeMagnify)
        if ('ongesturestart' in window) {
            this.elements.testArea.addEventListener('gesturestart', (e) => {
                e.preventDefault();
                this.handleGestureStart(e);
            });
            
            this.elements.testArea.addEventListener('gesturechange', (e) => {
                e.preventDefault();
                this.handleGestureChange(e);
            });
            
            this.elements.testArea.addEventListener('gestureend', (e) => {
                e.preventDefault();
                this.handleGestureEnd(e);
            });
        }
        
        // Touch events for additional detection
        let touchStartDistance = 0;
        let touchStartAngle = 0;
        
        this.elements.testArea.addEventListener('touchstart', (e) => {
            if (e.touches.length === 2) {
                const touch1 = e.touches[0];
                const touch2 = e.touches[1];
                touchStartDistance = Math.hypot(
                    touch2.clientX - touch1.clientX,
                    touch2.clientY - touch1.clientY
                );
                touchStartAngle = Math.atan2(
                    touch2.clientY - touch1.clientY,
                    touch2.clientX - touch1.clientX
                );
            }
        });
        
        this.elements.testArea.addEventListener('touchmove', (e) => {
            if (e.touches.length === 2) {
                e.preventDefault();
                const touch1 = e.touches[0];
                const touch2 = e.touches[1];
                
                // Calculate pinch scale (similar to GHOST_kTrackpadEventMagnify)
                const currentDistance = Math.hypot(
                    touch2.clientX - touch1.clientX,
                    touch2.clientY - touch1.clientY
                );
                const scale = currentDistance / touchStartDistance;
                
                // Calculate rotation (similar to GHOST_kTrackpadEventRotate)
                const currentAngle = Math.atan2(
                    touch2.clientY - touch1.clientY,
                    touch2.clientX - touch1.clientX
                );
                const rotation = (currentAngle - touchStartAngle) * 180 / Math.PI;
                
                this.handleMultitouch(scale, rotation);
            }
        });
        
        // Reset test object on double click
        this.elements.testArea.addEventListener('dblclick', () => {
            this.resetTransform();
        });
    }
    
    handleWheelEvent(event) {
        const now = Date.now();
        const timeDelta = now - this.detection.lastWheelTime;
        this.detection.lastWheelTime = now;
        
        // Analyze delta values (inspired by Blender's wheel delta accumulation)
        const deltaX = event.deltaX;
        const deltaY = event.deltaY;
        const deltaMode = event.deltaMode; // 0: pixel, 1: line, 2: page
        
        // Update delta mode display
        const deltaModes = ['Pixel', 'Line', 'Page'];
        this.elements.deltaMode.textContent = deltaModes[deltaMode] || 'Unknown';
        
        // Update debug info
        this.elements.lastDeltaY.textContent = deltaY.toFixed(2);
        this.elements.lastDeltaX.textContent = deltaX.toFixed(2);
        this.elements.eventFrequency.textContent = timeDelta + 'ms';
        
        // Detection logic based on patterns observed in Blender's code
        const features = {
            isPixelMode: deltaMode === 0,
            isLineMode: deltaMode === 1,
            isPageMode: deltaMode === 2,
            hasFloatDelta: (deltaY % 1 !== 0 || deltaX % 1 !== 0),
            hasLargeDelta: Math.abs(deltaY) >= 100 || Math.abs(deltaX) >= 100,
            hasSmallDelta: Math.abs(deltaY) < 10 && Math.abs(deltaX) < 10,
            hasInertia: timeDelta < 50 && this.detection.wheelDeltaHistory.length > 0,
            hasBothAxes: deltaX !== 0 && deltaY !== 0,
            isHighFrequency: timeDelta < 20
        };
        
        // Update float delta display
        this.elements.isFloatDelta.textContent = features.hasFloatDelta ? 'Yes' : 'No';
        
        // Track delta history for pattern recognition
        this.detection.wheelDeltaHistory.push({
            deltaX,
            deltaY,
            deltaMode,
            timestamp: now,
            features
        });
        
        // Keep only recent history (last 20 events)
        if (this.detection.wheelDeltaHistory.length > 20) {
            this.detection.wheelDeltaHistory.shift();
        }
        
        // Improved detection logic
        let deviceType = 'unknown';
        let confidence = 0;
        let reasons = [];
        
        // Primary detection: deltaMode
        if (deltaMode === 1 || deltaMode === 2) {
            // Line or Page mode - strong indicator of mouse wheel
            deviceType = 'mouse';
            confidence = 70;
            reasons.push('Line/Page scroll mode');
        } else if (deltaMode === 0) {
            // Pixel mode - could be either, need more analysis
            
            // Check delta patterns
            if (Math.abs(deltaY) === 120 || Math.abs(deltaY) === 100) {
                // Standard mouse wheel increments
                deviceType = 'mouse';
                confidence = 80;
                reasons.push('Standard wheel increment');
            } else if (features.hasFloatDelta) {
                // Fractional values - strong trackpad indicator
                deviceType = 'trackpad';
                confidence = 85;
                reasons.push('Fractional delta values');
            } else if (features.hasSmallDelta && features.isHighFrequency) {
                // Small, high-frequency events - trackpad scrolling
                deviceType = 'trackpad';
                confidence = 75;
                reasons.push('Smooth scrolling pattern');
            } else if (features.hasLargeDelta && !features.hasFloatDelta) {
                // Large integer deltas - likely mouse wheel
                deviceType = 'mouse';
                confidence = 65;
                reasons.push('Large integer deltas');
            } else {
                // Default to trackpad for pixel mode if uncertain
                deviceType = 'trackpad';
                confidence = 50;
                reasons.push('Pixel mode (uncertain)');
            }
        }
        
        // Secondary checks to refine confidence
        if (features.hasBothAxes && features.hasFloatDelta) {
            // Diagonal scrolling with float values - definitely trackpad
            deviceType = 'trackpad';
            confidence = Math.min(95, confidence + 15);
            reasons.push('Multi-axis scrolling');
        }
        
        // Check for mouse-specific patterns
        if (!features.hasFloatDelta && !features.hasBothAxes && 
            (Math.abs(deltaY) % 3 === 0 || Math.abs(deltaY) % 120 === 0)) {
            // Mouse wheels often scroll in multiples of 3 or 120
            if (deviceType !== 'trackpad' || confidence < 70) {
                deviceType = 'mouse';
                confidence = Math.max(confidence, 75);
                reasons.push('Mouse wheel pattern');
            }
        }
        
        // Cap confidence at 95%
        confidence = Math.min(confidence, 95);
        
        // Update detection state
        if (confidence > this.confidence || deviceType !== this.deviceType) {
            this.deviceType = deviceType;
            this.confidence = confidence;
            this.updateDeviceIndicator();
        }
        
        // Apply scroll to test object
        const scaleDelta = -deltaY * 0.001;
        this.transform.scale = Math.max(0.5, Math.min(3, this.transform.scale + scaleDelta));
        
        this.transform.translateX += deltaX * 0.5;
        this.transform.translateY += deltaY * 0.5;
        
        this.updateTransform();
        
        // Log event with detection reasons
        const reasonsStr = reasons.length > 0 ? ` [${reasons.join(', ')}]` : '';
        this.logEvent(deviceType === 'trackpad' ? 'trackpad' : 'wheel', 
            `${deviceType === 'trackpad' ? 'Trackpad' : 'Mouse'}: ΔY=${deltaY.toFixed(1)}, ΔX=${deltaX.toFixed(1)}, Mode=${deltaMode}${reasonsStr}`);
        
        // Update stats
        this.stats.total++;
        if (deviceType === 'trackpad') {
            this.stats.trackpad++;
        } else if (deviceType === 'mouse') {
            this.stats.wheel++;
        }
        this.updateStats();
    }
    
    handleGestureStart(event) {
        this.logEvent('gesture', 'Gesture Start (Safari/iOS Trackpad Detected)');
        this.deviceType = 'trackpad';
        this.confidence = 100;
        this.updateDeviceIndicator();
    }
    
    handleGestureChange(event) {
        // Handle pinch/zoom (similar to GHOST_kTrackpadEventMagnify)
        this.transform.scale *= event.scale;
        this.transform.scale = Math.max(0.5, Math.min(3, this.transform.scale));
        
        // Handle rotation (similar to GHOST_kTrackpadEventRotate)
        this.transform.rotation += event.rotation;
        
        this.updateTransform();
        this.logEvent('gesture', `Gesture: Scale=${event.scale.toFixed(2)}, Rotation=${event.rotation.toFixed(1)}°`);
        
        this.stats.total++;
        this.stats.gesture++;
        this.updateStats();
    }
    
    handleGestureEnd(event) {
        this.logEvent('gesture', 'Gesture End');
    }
    
    handleMultitouch(scale, rotation) {
        // Touch-based gesture detection (for devices without native gesture events)
        this.transform.scale *= scale;
        this.transform.scale = Math.max(0.5, Math.min(3, this.transform.scale));
        this.transform.rotation = rotation;
        
        this.updateTransform();
        this.logEvent('trackpad', `Touch Gesture: Scale=${scale.toFixed(2)}, Rotation=${rotation.toFixed(1)}°`);
        
        // Multi-touch gestures indicate trackpad or touchscreen
        this.deviceType = 'trackpad';
        this.confidence = 90;
        this.updateDeviceIndicator();
        
        this.stats.total++;
        this.stats.trackpad++;
        this.updateStats();
    }
    
    updateDeviceIndicator() {
        const indicator = this.elements.deviceIndicator;
        indicator.className = 'device-indicator ' + this.deviceType;
        
        let displayText = 'Unknown';
        if (this.deviceType === 'mouse') {
            displayText = '🖱️ Mouse';
        } else if (this.deviceType === 'trackpad') {
            displayText = '🖲️ Trackpad';
        }
        
        indicator.textContent = displayText;
        this.elements.confidence.textContent = `Confidence: ${this.confidence}%`;
    }
    
    updateTransform() {
        const obj = this.elements.testObject;
        obj.style.transform = `
            translate(${this.transform.translateX}px, ${this.transform.translateY}px)
            scale(${this.transform.scale})
            rotate(${this.transform.rotation}deg)
        `;
    }
    
    resetTransform() {
        this.transform = {
            scale: 1,
            rotation: 0,
            translateX: 0,
            translateY: 0
        };
        this.updateTransform();
        this.logEvent('info', 'Transform Reset');
    }
    
    updateStats() {
        this.elements.totalEvents.textContent = this.stats.total;
        this.elements.wheelEvents.textContent = this.stats.wheel;
        this.elements.trackpadEvents.textContent = this.stats.trackpad;
        this.elements.gestureEvents.textContent = this.stats.gesture;
    }
    
    logEvent(type, message) {
        const entry = document.createElement('div');
        entry.className = 'event-entry ' + type;
        
        const timestamp = new Date().toLocaleTimeString();
        entry.textContent = `[${timestamp}] ${message}`;
        
        const log = this.elements.eventLog;
        log.insertBefore(entry, log.firstChild);
        
        // Keep only last 50 entries
        while (log.children.length > 50) {
            log.removeChild(log.lastChild);
        }
    }
}

// Initialize detector when page loads
document.addEventListener('DOMContentLoaded', () => {
    const detector = new InputDeviceDetector();
    
    // Add some initial guidance
    detector.logEvent('info', 'Scroll in the test area to detect your input device');
    detector.logEvent('info', 'Double-click to reset the test object');
    detector.logEvent('info', 'Detection algorithm inspired by Blender\'s GHOST system');
});