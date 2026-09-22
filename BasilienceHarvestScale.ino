#include <time.h>
#include <math.h>

#include "LoadCellManager.h"
#include "DisplayManager.h"
#include "NetworkManager.h"
#include "FirebaseManager.h"
#include "Secrets.h"

// ============================================================
// BASILIENCE HARVEST SCALE
// Phase 3 - WiFi + Firebase Integration
// ESP8266 + HX711 + 20 kg Load Cell + 16x2 I2C LCD
// ============================================================

// ------------------------------------------------------------
// PIN ASSIGNMENTS
// ------------------------------------------------------------

constexpr uint8_t HX711_DOUT_PIN = D5;   // GPIO14
constexpr uint8_t HX711_SCK_PIN  = D6;   // GPIO12

// I2C pins (handled by Wire):
//   SDA -> D2 (GPIO4)
//   SCL -> D1 (GPIO5)

// ------------------------------------------------------------
// LCD SETTINGS
// ------------------------------------------------------------

constexpr uint8_t LCD_I2C_ADDRESS = 0x27;
constexpr uint8_t LCD_COLS        = 16;
constexpr uint8_t LCD_ROWS        = 2;

// ------------------------------------------------------------
// WIFI PROVISIONING
// ------------------------------------------------------------
//
// SECURE WIFI PROVISIONING — no network name/password is compiled into
// this firmware (the previous version hardcoded a real home-router
// password directly into source under version control). NetworkManager
// persists credentials to flash once entered and, on first boot or
// whenever the saved network can't be reached, opens its own
// "Basilience-Scale-Setup" access point with a setup form at
// http://192.168.4.1/ — connect a phone/laptop to that network to
// provision this scale. See NetworkManager.h/.cpp.
//

// ------------------------------------------------------------
// FIREBASE CREDENTIALS
// ------------------------------------------------------------
//
// FB_API_KEY, FB_DATABASE_URL and HARVEST_SCALE_DEVICE_SECRET are no
// longer compiled in here — they live in "Secrets.h", a gitignored file
// in this same sketch folder that is never committed. Copy
// Secrets.h.example to Secrets.h and fill in real values there; see
// that file for full setup instructions (bootstrap secret generation,
// server-side registration, etc).
//
// SECURE DEVICE AUTH — this unit does not use the project-wide RTDB
// "Database Secret" (that credential is a master key that bypasses
// every security rule for the ENTIRE database, not just this device -
// see FirebaseManager.h/.cpp). Instead it bootstraps its own scoped
// identity, the same way the ESP32 firmware does: it exchanges its
// per-device HARVEST_SCALE_DEVICE_SECRET for a Firebase custom token
// (uid = deviceId) via deviceAuthBootstrap on first boot, and persists
// the resulting refresh token to flash from then on.
//
// ------------------------------------------------------------
// CALIBRATION
// ------------------------------------------------------------
//
// Previous factor: 110.98
// Refined factor:  110.54
// (Known 250 g [250 mL water] → measured 249 g)
//

constexpr float CALIBRATION_FACTOR = 110.54f;

// ------------------------------------------------------------
// SCALE SETTINGS
// ------------------------------------------------------------

constexpr uint8_t  TARE_SAMPLES        = 30;
constexpr uint8_t  READING_SAMPLES     = 3;    // was 10 - the 3-sample median filter below provides the rejection this used to rely on the HX711 average alone for
constexpr unsigned long HX711_TIMEOUT_MS   = 1500;
constexpr unsigned long READING_INTERVAL_MS = 300;   // was 500

// ------------------------------------------------------------
// WARM-UP
// ------------------------------------------------------------
//
// Raised back from 30s to 60s - physical testing showed the empty-platform
// baseline can still be drifting tens of grams away from true zero shortly
// after boot, which the 30s window wasn't giving the HX711/load cell
// enough time to settle out of before the startup tare locked it in. This
// is a one-time boot-time cost, paid every power-on, not a per-weighing
// one - see EMPTY_SETTLE_MS below for the (much shorter) per-weighing
// re-zero this same drift investigation added.
//
constexpr unsigned long WARMUP_TIME_MS = 60000;

// ------------------------------------------------------------
// ZERO DEADBAND
// ------------------------------------------------------------

constexpr float ZERO_DEADBAND_GRAMS = 5.0f;

// ------------------------------------------------------------
// SCALE CAPACITY
// ------------------------------------------------------------

constexpr float MAX_WEIGHT_GRAMS = 20000.0f;

// ------------------------------------------------------------
// STABILITY-BASED UPLOAD SETTINGS
// ------------------------------------------------------------
//
// Upload fires ONCE when:
//   1. Weight > UPLOAD_MIN_GRAMS (something is on the scale)
//   2. Weight has not changed more than STABLE_THRESHOLD_GRAMS
//      across STABLE_READINGS consecutive readings
//   3. At least STABLE_HOLD_MS has passed since stability began
//
// After upload, scale waits for weight to drop back to near-zero
// before allowing the next upload (prevents duplicate uploads).
//

constexpr float        UPLOAD_MIN_GRAMS       = 50.0f;
constexpr float        STABLE_THRESHOLD_GRAMS = 10.0f;
constexpr uint8_t      STABLE_READINGS        = 5;    // was 6
constexpr unsigned long STABLE_HOLD_MS        = 1500;  // was 3000

// ------------------------------------------------------------
// POST-REMOVAL SOFTWARE ZERO CORRECTION
// ------------------------------------------------------------
//
// After a COMPLETED weighing session (capturedThisLoad already true) ends
// with the object removed, the platform must read continuously empty for
// EMPTY_SETTLE_MS before the firmware starts trusting readings again - not
// on the very first below-threshold sample, which could just be platform
// bounce or an object still being lifted off.
//
// IMPORTANT: this NO LONGER re-runs the HX711 hardware tare
// (loadCell.tare()). Physical testing proved that unsafe - an automatic
// hardware tare occasionally latched onto a transient/bad reading as the
// new zero reference, producing wrong absolute offsets (see the commit
// history for real examples: correct ~222-224g readings after a good tare
// vs. the same object reading ~155-165g, or a placed object reading ~38g,
// after a bad automatic one). The ONE hardware tare that's trusted is the
// startup tare below, after the 60s warm-up - it never changes again for
// the rest of this boot.
//
// Instead, drift is compensated entirely in software: softwareZeroBiasGrams
// (declared in STATE below) is measured fresh after every completed
// session's removal and SUBTRACTED from the hardware-tared reading before
// any other processing - see SOFTWARE ZERO CORRECTION in loop(). A bad
// baseline sample here can only ever produce a bad software bias, which is
// validated (spread + absolute-range checks below) before being accepted,
// and worst case just gets discarded/retried - it can never corrupt the
// underlying HX711 tare offset itself the way the removed hardware
// re-tare could.
//
constexpr unsigned long EMPTY_SETTLE_MS = 1500;

// Once the platform has settled empty for EMPTY_SETTLE_MS, this many valid
// signed sensorWeightGrams samples (hardware-tared, but NOT yet
// bias-corrected) are collected to compute the new software zero - see
// ZERO BASELINE SAMPLE COLLECTION in loop().
constexpr uint8_t ZERO_BASELINE_SAMPLES = 9;

// The 9-sample baseline's own (max - min) spread must be at or below this
// before it's trusted as a genuinely stable empty reading. Too wide a
// spread means the platform probably isn't actually settled yet (bounce,
// vibration, something still being placed) - the baseline collection
// simply restarts rather than accepting a noisy zero.
constexpr float ZERO_BASELINE_MAX_SPREAD_GRAMS = 8.0f;

// Defensive absolute limit: even a spread-validated baseline is rejected if
// its median magnitude exceeds this, so a wildly wrong reading (something
// like -185g) can never silently become the new zero - only genuine,
// observed drift in roughly the -50 to -60g range this hardware has shown
// is meant to pass. Rejection here does NOT fall back to a hardware tare -
// see the class-wide comment above - it simply keeps the previous software
// bias and keeps observing.
constexpr float MAX_SOFTWARE_ZERO_ABS_GRAMS = 100.0f;

// ------------------------------------------------------------
// FIREBASE RECONNECT
// ------------------------------------------------------------
//
// firebase.begin() used to run exactly once, in setup() - if WiFi wasn't
// up yet at that point, or the auth handshake failed transiently, Firebase
// stayed permanently unready for the rest of that boot with no way to
// recover short of a power cycle. Retried from loop() now instead (see the
// FIREBASE RECONNECT block below), cooldown-gated so
// this never re-runs the multi-second NTP+auth sequence on every reading
// cycle - only every 30s, and only while actually not yet ready.
//
constexpr unsigned long FIREBASE_RETRY_INTERVAL_MS = 30000;

// ------------------------------------------------------------
// OBJECTS
// ------------------------------------------------------------

LoadCellManager loadCell(
    HX711_DOUT_PIN,
    HX711_SCK_PIN
);

DisplayManager display(
    LCD_I2C_ADDRESS,
    LCD_COLS,
    LCD_ROWS
);

NetworkManager network;

FirebaseManager firebase(
    FB_API_KEY,
    FB_DATABASE_URL,
    HARVEST_SCALE_DEVICE_SECRET
);

// ------------------------------------------------------------
// STATE
// ------------------------------------------------------------

unsigned long lastReadingTime      = 0;
unsigned long lastLiveUpdateTime   = 0;   // Throttle Firebase liveWeight updates
unsigned long lastFirebaseRetryTime = 0;  // Cooldown for re-attempting firebase.begin()

// What's actually shown on the LCD - a plain copy of reportedGrams, updated
// only while no confirmed load is holding the screen frozen (see the LCD
// OUTPUT block in loop()). Kept as its own variable (rather than reading
// reportedGrams directly at display time) so the "SAVED"/"OFFLINE"
// messages can show the exact same number the weighing screen last
// displayed, and so it still holds that value while frozen.
float displayWeightGrams = 0.0f;

// Stability tracking
float         stableReadings[STABLE_READINGS];
uint8_t       stableIndex      = 0;
bool          bufferFull       = false;
unsigned long stableStartTime  = 0;
bool          isStable         = false;

// 3-reading median filter — sits between the raw HX711 reading and the
// stability buffer (see NEGATIVE TRANSIENT SANITIZATION / 3-READING MEDIAN
// FILTER in loop()). A single bad or negative-noise cycle is outvoted by
// the other two recent readings instead of reaching stability detection
// directly. Independent of the stability buffer's own history/state.
float   medianWindow[3]   = { 0.0f, 0.0f, 0.0f };
uint8_t medianFilterIndex = 0;
uint8_t medianFilterCount = 0;   // caps at 3 - guards against uninitialized slots right after startup/reset

// Whether the CURRENT physical load has already gone through the
// capture-time decision (see the CAPTURE block in loop()) - set true the
// instant that decision is made, regardless of whether the Firebase upload
// it attempted succeeded, failed, or was never attempted at all. There is
// no "captured but not yet finished" in-between state anymore: a
// measurement either becomes an online "SAVED" or a local-only "OFFLINE"
// result in one step, and either way this load is done - the scale is
// immediately ready to weigh the next one.
bool  capturedThisLoad = false;

// Whether ANY load ≥ UPLOAD_MIN_GRAMS is currently on the platform,
// regardless of capturedThisLoad - tracks the physical weighing SESSION
// itself, separately from whether it happened to already produce a
// confirmed measurement. Used purely to detect the active→empty
// transition exactly once (see the RESET block in loop()), so stability
// state is always reset when the platform empties - even if this load
// was removed mid-sample or mid-hold and never actually got captured.
bool  loadActive = false;

// Post-removal software-zero state (see evaluateZeroBaseline() /
// EMPTY_SETTLE_MS above). awaitingAutoZero is true from the moment a
// COMPLETED session's object is detected removed until a valid new
// softwareZeroBiasGrams is accepted (or the attempt is cancelled by a
// returning load) - deliberately a SEPARATE flag from
// capturedThisLoad/loadActive (both stay true throughout this window,
// untouched) so a load returning mid-zeroing can cleanly cancel back to
// "still captured, still on platform" without losing or reconstructing any
// state - see the RESET block in loop().
bool          awaitingAutoZero     = false;
unsigned long emptySettleStartTime = 0;   // when the current settle/retry window started

// True once the initial EMPTY_SETTLE_MS wait has elapsed and the firmware
// is actively gathering the ZERO_BASELINE_SAMPLES-sample baseline (see ZERO
// BASELINE SAMPLE COLLECTION in loop()). False during the initial settle
// itself. Both sub-phases show the same "ZEROING..." LCD state.
bool    collectingZeroBaseline = false;
float   zeroBaselineSamples[ZERO_BASELINE_SAMPLES];
uint8_t zeroBaselineCount      = 0;

// Throttles "[ZERO] Empty baseline unstable — waiting." so a persistently
// noisy empty platform logs at most once every couple seconds instead of
// once per 300ms reading cycle.
unsigned long lastZeroUnstableLogTime = 0;

// The current best estimate of what the hardware-tared sensor reads when
// the platform is genuinely empty - subtracted from every sensor reading
// before any other processing (see SOFTWARE ZERO CORRECTION in loop()).
// Reset to 0 immediately after every successful STARTUP hardware tare (see
// setup()); from then on, only evaluateZeroBaseline() ever assigns a new
// value, always by REPLACING it outright, never accumulating into it.
float softwareZeroBiasGrams = 0.0f;

// ============================================================
// HELPERS
// ============================================================

// Clears the median filter's history. Called anywhere the stability buffer
// itself is reset (see resetStabilityBuffer()/restartStabilityTracking()
// below), so a new physical load never inherits filtered samples from
// whatever was on the platform before.
void resetMedianFilter()
{
    medianWindow[0]   = 0.0f;
    medianWindow[1]   = 0.0f;
    medianWindow[2]   = 0.0f;
    medianFilterIndex = 0;
    medianFilterCount = 0;
}

// Pushes a sanitized candidate (negative noise already floored to 0 by the
// caller - see NEGATIVE TRANSIENT SANITIZATION in loop()) into the 3-slot
// median filter and returns the median of however many valid samples have
// been pushed so far.
//
// Below 3 samples (right after startup/reset - medianFilterCount not yet
// full), there's no real history to outvote a bad sample with, so the
// candidate passes through unfiltered rather than mixing in stale/
// uninitialized slots.
float pushMedianFilter(float candidate)
{
    medianWindow[medianFilterIndex] = candidate;
    medianFilterIndex = (medianFilterIndex + 1) % 3;

    if (medianFilterCount < 3) { medianFilterCount++; }

    if (medianFilterCount < 3)
    {
        return candidate;
    }

    float a = medianWindow[0];
    float b = medianWindow[1];
    float c = medianWindow[2];

    float lo = fminf(a, fminf(b, c));
    float hi = fmaxf(a, fmaxf(b, c));

    return a + b + c - lo - hi;   // median of 3 = sum - min - max
}

// Whole-gram LCD rounding (item 1: sub-gram values aren't reliably
// measurable on this hardware yet). Round-half-away-from-zero, not
// truncation. Internal values (reportedGrams, Firebase payloads, stability
// math) stay float and unrounded - only what's printed on the 16x2 LCD is
// rounded here.
long displayGrams(float grams)
{
    return lroundf(grams);
}

// Fill stability buffer with a value (e.g. on tare/reset)
void resetStabilityBuffer(float value = 0.0f)
{
    for (uint8_t i = 0; i < STABLE_READINGS; i++)
    {
        stableReadings[i] = value;
    }

    stableIndex     = 0;
    bufferFull      = false;
    stableStartTime = 0;
    isStable        = false;

    resetMedianFilter();
}

// Re-open stability tracking for a NEW total that's still resting on the
// platform (more was added on top of an already-confirmed load, rather
// than the platform being emptied) - unlike resetStabilityBuffer() above,
// this must NOT snap displayWeightGrams to any fixed value, since the
// object never left the platform and the real weight right now is
// whatever the next reading actually comes back as.
void restartStabilityTracking()
{
    for (uint8_t i = 0; i < STABLE_READINGS; i++)
    {
        stableReadings[i] = 0.0f;
    }

    stableIndex     = 0;
    bufferFull      = false;
    stableStartTime = 0;
    isStable        = false;

    resetMedianFilter();
}

// Push reading into circular buffer and check stability
bool checkStability(float newReading)
{
    stableReadings[stableIndex] = newReading;
    stableIndex = (stableIndex + 1) % STABLE_READINGS;

    if (stableIndex == 0) { bufferFull = true; }

    if (!bufferFull) { return false; }

    float minVal = stableReadings[0];
    float maxVal = stableReadings[0];

    for (uint8_t i = 1; i < STABLE_READINGS; i++)
    {
        if (stableReadings[i] < minVal) { minVal = stableReadings[i]; }
        if (stableReadings[i] > maxVal) { maxVal = stableReadings[i]; }
    }

    return (maxVal - minVal) <= STABLE_THRESHOLD_GRAMS;
}

// Mean of the stability buffer - only meaningful once bufferFull (checkStability()
// has pushed STABLE_READINGS consecutive real samples; earlier slots are still the
// zero-fill from the last reset). Used for what's shown/uploaded once something is
// actually settling on the platform, instead of whichever single raw cycle happens
// to land at that instant - ordinary HX711 cycle-to-cycle noise (a few tenths of a
// gram on this load cell) otherwise made the same physical object read as a
// slightly different final number on the LCD vs. what got uploaded vs. the next
// time it was weighed.
float stabilityAverageGrams()
{
    float sum = 0.0f;
    for (uint8_t i = 0; i < STABLE_READINGS; i++)
    {
        sum += stableReadings[i];
    }
    return sum / STABLE_READINGS;
}

// Current min/max spread across the stability buffer - the same value
// checkStability() compares against STABLE_THRESHOLD_GRAMS internally, but
// exposed here too for the [SCALE] diagnostic log. 0 (not meaningful) until
// bufferFull, same guard checkStability() itself uses.
float stabilitySpreadGrams()
{
    if (!bufferFull) { return 0.0f; }

    float minVal = stableReadings[0];
    float maxVal = stableReadings[0];

    for (uint8_t i = 1; i < STABLE_READINGS; i++)
    {
        if (stableReadings[i] < minVal) { minVal = stableReadings[i]; }
        if (stableReadings[i] > maxVal) { maxVal = stableReadings[i]; }
    }

    return maxVal - minVal;
}

// Median of the ZERO_BASELINE_SAMPLES collected empty-baseline samples -
// sorts a small local copy (plain insertion sort; ZERO_BASELINE_SAMPLES is
// tiny, so this is cheap and non-blocking) and returns the middle value.
// Preferred over a mean so ONE transient sample among the nine can't skew
// the accepted zero.
float medianOfZeroBaseline()
{
    float sorted[ZERO_BASELINE_SAMPLES];
    for (uint8_t i = 0; i < ZERO_BASELINE_SAMPLES; i++) { sorted[i] = zeroBaselineSamples[i]; }

    for (uint8_t i = 1; i < ZERO_BASELINE_SAMPLES; i++)
    {
        float value = sorted[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && sorted[j] > value)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = value;
    }

    return sorted[ZERO_BASELINE_SAMPLES / 2];
}

// ============================================================
// WARM-UP
// ============================================================

void warmUpScale()
{
    Serial.println();
    Serial.println("====================================");
    Serial.println(" SCALE WARM-UP");
    Serial.println("====================================");
    Serial.println();
    Serial.println("[SCALE] Keep platform EMPTY.");
    Serial.println("[SCALE] Do NOT touch the scale.");

    unsigned long startTime    = millis();
    unsigned long lastPrintTime = 0;
    unsigned long lastLcdTime   = 0;

    while (millis() - startTime < WARMUP_TIME_MS)
    {
        yield();

        // This warm-up wait runs right after WiFi setup, in the window
        // someone is most likely to actually be trying to connect to the
        // setup portal - without servicing it here too, the DNS/HTTP
        // server would sit completely unanswered for the whole warm-up,
        // not just delayed. Confirmed cause of "connects to the AP but no
        // setup form ever appears."
        network.update();

        long rawValue = 0;

        if (loadCell.readRawAverage(5, rawValue, HX711_TIMEOUT_MS))
        {
            unsigned long elapsed      = millis() - startTime;
            unsigned long remainingSec = (WARMUP_TIME_MS - elapsed) / 1000;

            if (millis() - lastPrintTime >= 5000)
            {
                lastPrintTime = millis();
                Serial.print("[WARMUP] Raw: ");
                Serial.print(rawValue);
                Serial.print(" | Remaining: ");
                Serial.print(remainingSec);
                Serial.println(" sec");
            }

            if (millis() - lastLcdTime >= 1000)
            {
                lastLcdTime = millis();
                display.showWarmUp(remainingSec);
            }
        }

        delay(10);
    }

    Serial.println("[SCALE] Warm-up complete.");
}

// ============================================================
// POST-REMOVAL SOFTWARE ZERO
// ============================================================

// Called once ZERO_BASELINE_SAMPLES valid signed samples have been
// collected (see ZERO BASELINE SAMPLE COLLECTION in loop()) - validates
// them and either accepts a new softwareZeroBiasGrams or restarts/holds
// collection. Never touches the HX711 hardware tare - see this file's
// POST-REMOVAL SOFTWARE ZERO CORRECTION comment for why that was removed.
// Non-blocking: this is pure math over 9 already-collected floats, no
// hardware I/O, so it costs nothing to call from inside the normal
// reading-interval-gated loop() cycle.
void evaluateZeroBaseline()
{
    float minVal = zeroBaselineSamples[0];
    float maxVal = zeroBaselineSamples[0];

    for (uint8_t i = 1; i < ZERO_BASELINE_SAMPLES; i++)
    {
        if (zeroBaselineSamples[i] < minVal) { minVal = zeroBaselineSamples[i]; }
        if (zeroBaselineSamples[i] > maxVal) { maxVal = zeroBaselineSamples[i]; }
    }

    float spread = maxVal - minVal;

    if (spread > ZERO_BASELINE_MAX_SPREAD_GRAMS)
    {
        // Throttled - a persistently unsettled platform would otherwise log
        // this once per 300ms reading cycle for as long as it stays noisy.
        if (millis() - lastZeroUnstableLogTime >= 2000)
        {
            lastZeroUnstableLogTime = millis();
            Serial.println("[ZERO] Empty baseline unstable — waiting.");
        }
        zeroBaselineCount = 0; // restart collection, keep waiting
        return;
    }

    float candidateBias = medianOfZeroBaseline();

    if (fabsf(candidateBias) > MAX_SOFTWARE_ZERO_ABS_GRAMS)
    {
        Serial.println("[ZERO] Baseline outside safe correction range — keeping previous zero.");
        zeroBaselineCount = 0; // restart collection, keep observing - never falls back to a hardware tare
        return;
    }

    Serial.println("[ZERO] Software zero updated.");
    Serial.print("[ZERO] Old bias: ");
    Serial.print(softwareZeroBiasGrams, 1);
    Serial.println(" g");
    Serial.print("[ZERO] New bias: ");
    Serial.print(candidateBias, 1);
    Serial.println(" g");
    Serial.print("[ZERO] Baseline spread: ");
    Serial.print(spread, 1);
    Serial.println(" g");

    // REPLACE, never accumulate - candidateBias is always measured directly
    // from the hardware-tared sensorWeightGrams, not relative to the old
    // software bias.
    softwareZeroBiasGrams = candidateBias;

    resetStabilityBuffer(0.0f);
    capturedThisLoad       = false;
    loadActive             = false;
    awaitingAutoZero       = false;
    collectingZeroBaseline = false;
    zeroBaselineCount      = 0;
}

// Shown once, right before NetworkManager blocks on the setup portal -
// NetworkManager has no display dependency of its own, so it invokes this
// via a plain callback instead.
void showWifiSetupModeOnDisplay()
{
    display.showError("Setup Mode", "Join: Basilience-Scale-Setup");
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("====================================");
    Serial.println("   Basilience Harvest Scale v3");
    Serial.println("====================================");

    // --------------------------------------------------------
    // LCD
    // --------------------------------------------------------

    display.begin();
    display.showBoot();
    delay(1500);

    // --------------------------------------------------------
    // HX711
    // --------------------------------------------------------

    Serial.println("[SCALE] Initializing HX711...");

    if (!loadCell.begin(HX711_TIMEOUT_MS))
    {
        // Deliberately NOT a `return` - WiFi provisioning, Firebase
        // connectivity, and the setup portal must all stay reachable even
        // with the load cell unplugged or faulty (e.g. testing/bring-up
        // without the full hardware assembled, or diagnosing a wiring
        // fault remotely). loop()'s own weight-reading path already
        // tolerates a HX711 read failure per-iteration without crashing -
        // this unit will just never produce a real weight until the HX711
        // is actually connected.
        Serial.println("[SCALE] ERROR: HX711 not detected. Continuing without it - WiFi/Firebase setup still runs.");
        display.showError("HX711 ERROR!", "Check wiring");
        delay(1500);
    }
    else
    {
        Serial.println("[SCALE] HX711 detected.");
    }
    loadCell.setCalibrationFactor(CALIBRATION_FACTOR);

    // --------------------------------------------------------
    // LOCAL STORAGE
    // --------------------------------------------------------
    //
    // Deliberately BEFORE WiFi - device identity and the measurement-
    // sequence counter must be usable even on a unit that never reaches a
    // network at all. Also clears out any pending-measurement data left
    // behind by an earlier firmware revision that supported offline sync
    // (see FirebaseManager::clearLegacyPendingData()) - that concept no
    // longer exists, so nothing old can surface in Firebase later.
    // firebase.begin() further below also mounts this (idempotently) since
    // it separately needs LittleFS for auth credentials.
    //

    if (!firebase.beginLocalStorage())
    {
        Serial.println("[SCALE] WARNING: Local storage unavailable - a confirmed measurement could not be recovered/persisted across reboots.");
    }

    // --------------------------------------------------------
    // WIFI
    // --------------------------------------------------------

    display.showError("Connecting WiFi", "Please wait...");

    Serial.println();
    Serial.println("====================================");
    Serial.println("[WIFI] Loading saved credentials...");
    Serial.println("====================================");

    // No saved network (or the saved one can't be reached) starts the
    // Basilience-Scale-Setup portal instead, in the background - it does
    // NOT block here. Weighing continues locally either way; only
    // Firebase upload is unavailable until the portal is used (or a
    // saved network is later reachable) and the device restarts.
    bool wifiOk = network.connect(showWifiSetupModeOnDisplay);

    if (wifiOk)
    {
        display.showError("WiFi Connected!", network.getIPAddress());
        delay(1500);
    }
    else if (network.isProvisioning())
    {
        Serial.println("[WIFI] Setup portal active - scale continues in offline/local mode.");
        delay(1500); // Let showWifiSetupModeOnDisplay()'s message stay readable briefly.
    }
    else
    {
        Serial.println("[WIFI] Offline mode — no Firebase upload.");
        display.showError("WiFi FAILED", "Offline mode");
        delay(2000);
    }

    // --------------------------------------------------------
    // FIREBASE — only if WiFi connected
    // --------------------------------------------------------

    if (wifiOk)
    {
        display.showError("Firebase init...", "Please wait...");

        if (firebase.begin())
        {
            display.showError("Firebase OK!", "");
            delay(1000);
        }
        else
        {
            Serial.println("[FB] Firebase init failed.");
            display.showError("Firebase FAILED", "Check config");
            delay(2000);
        }
    }

    // --------------------------------------------------------
    // WARM-UP (60 seconds, WARMUP_TIME_MS)
    // --------------------------------------------------------

    warmUpScale();

    // --------------------------------------------------------
    // TARE
    // --------------------------------------------------------

    Serial.println();
    Serial.println("====================================");
    Serial.println(" TARE");
    Serial.println("====================================");
    Serial.println("[SCALE] Platform must be EMPTY. Taring...");

    display.showTaring();

    if (!loadCell.tare(TARE_SAMPLES, HX711_TIMEOUT_MS))
    {
        // Deliberately NOT a `return` here either - see the matching
        // comment on the HX711 begin() check above. Without this, a
        // missing/failed HX711 would let setup() reach this point (WiFi/
        // Firebase already started) and then dead-end here, before ever
        // reaching loop() - meaning network.update() would stop being
        // serviced entirely (only warmUpScale() drives it before loop()
        // starts), silently killing the setup portal a full minute or so
        // into every boot with a bad/absent load cell.
        Serial.println("[SCALE] ERROR: Unable to tare. Continuing without it - WiFi/Firebase and the setup portal stay up.");
        display.showError("Tare failed!", "Check HX711");
        delay(1500);
    }
    else
    {
        Serial.println("[SCALE] Tare complete.");

        Serial.print("[SCALE] Tare offset: ");
        Serial.println(loadCell.getOffset());

        // This startup tare is the ONE trusted hardware zero reference for
        // the rest of this boot - see POST-REMOVAL SOFTWARE ZERO CORRECTION
        // above. Any drift observed after this point is compensated in
        // software (softwareZeroBiasGrams), never by re-taring the HX711
        // again.
        softwareZeroBiasGrams = 0.0f;
    }

    Serial.println();
    Serial.println("====================================");
    Serial.println(" SCALE READY");
    Serial.println("====================================");

    resetStabilityBuffer(0.0f);

    display.showReady();
    delay(1500);
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    // --------------------------------------------------------
    // WIFI KEEPALIVE / SETUP PORTAL SERVICING
    // --------------------------------------------------------
    //
    // Deliberately BEFORE the reading-interval gate below, and runs every
    // single loop() iteration - not throttled to it. While the setup
    // portal is active this drives _dnsServer.processNextRequest() and
    // _server.handleClient(); gating it to the same 500ms weight-sampling
    // cadence meant a captive-portal handshake (DNS query, then a
    // separate TCP connect + HTTP GET) could sit unanswered for multiple
    // ticks in a row - easily enough to blow past how impatient mobile
    // OS captive-portal detection actually is. This was the confirmed
    // cause of "connects to the AP but no setup form ever appears."
    //

    network.update();

    // --------------------------------------------------------
    // READING INTERVAL
    // --------------------------------------------------------

    if (millis() - lastReadingTime < READING_INTERVAL_MS) { return; }
    lastReadingTime = millis();

    // --------------------------------------------------------
    // READ WEIGHT (hardware-tared, calibrated sensor reading)
    // --------------------------------------------------------
    //
    // Despite going through loadCell's own hardware tare offset and
    // calibration factor, this is NOT yet the final reported weight -
    // softwareZeroBiasGrams (see below) still needs to be subtracted. Named
    // sensorWeightGrams (not "raw") to make that distinction explicit.
    //

    float sensorWeightGrams = 0.0f;

    if (!loadCell.readWeightGrams(READING_SAMPLES, sensorWeightGrams, HX711_TIMEOUT_MS))
    {
        Serial.println("[SCALE] ERROR: HX711 read timeout.");
        display.showError("Read timeout!", "Check HX711");
        return;
    }

    // --------------------------------------------------------
    // BASIC SANITY VALIDATION
    // --------------------------------------------------------
    //
    // Rejected outright - never inserted into the zero-baseline collection,
    // median/stability buffers, unlike ordinary negative noise (see
    // NEGATIVE TRANSIENT SANITIZATION further below). A NaN/Inf/overload/
    // impossible sample means this cycle's reading can't be trusted at all,
    // not just "reads oddly." Deliberately operates on the raw
    // sensorWeightGrams, before any software zero correction.
    //

    if (isnan(sensorWeightGrams) || isinf(sensorWeightGrams))
    {
        Serial.println("[SCALE] ERROR: Corrupted reading (NaN/Inf).");
        display.showError("Bad reading!", "");
        return;
    }

    if (sensorWeightGrams > MAX_WEIGHT_GRAMS)
    {
        Serial.println("[SCALE] ERROR: Weight exceeds 20 kg.");
        display.showError("OVERLOAD!", "Max: 20 kg");
        return;
    }

    if (sensorWeightGrams < -1000.0f)
    {
        Serial.println("[SCALE] ERROR: Invalid negative reading.");
        display.showError("Bad reading!", "");
        return;
    }

    // --------------------------------------------------------
    // ZERO BASELINE SAMPLE COLLECTION (software zero re-acquisition)
    // --------------------------------------------------------
    //
    // Only active during the baseline-collection sub-phase of post-removal
    // zeroing (see the RESET / POST-REMOVAL SOFTWARE ZERO block below) -
    // collects the TRUE signed, hardware-tare-relative reading, before ANY
    // correction/clamping/filtering touches it. This is deliberate: using
    // an already bias-corrected or already negative-clamped value here
    // would destroy exactly the information needed to compensate a
    // negative empty-platform drift (see SOFTWARE ZERO CORRECTION below for
    // the worked example).
    //

    if (collectingZeroBaseline && zeroBaselineCount < ZERO_BASELINE_SAMPLES)
    {
        zeroBaselineSamples[zeroBaselineCount] = sensorWeightGrams;
        zeroBaselineCount++;
    }

    // --------------------------------------------------------
    // SOFTWARE ZERO CORRECTION
    // --------------------------------------------------------
    //
    // Applied BEFORE negative sanitization - subtracting the bias first
    // (rather than clamping sensorWeightGrams to 0 first) is what correctly
    // compensates a NEGATIVE empty-platform drift. Example: sensor reads
    // -56g while empty, softwareZeroBiasGrams becomes -56g, so a later
    // empty reading of -56g corrects to 0g, and a later loaded reading of
    // 167g corrects to 167 - (-56) = 223g. Uses the OLD bias until (and
    // unless) evaluateZeroBaseline() accepts a new one - see the ZEROING
    // CANCELLATION check immediately below, which relies on that.
    //

    float correctedWeightGrams = sensorWeightGrams - softwareZeroBiasGrams;

    // A load reappearing while a new baseline is still being measured must
    // cancel that attempt outright - accepting a baseline collected with
    // something (even briefly) on the platform would bake a wrong zero in.
    // Checked here, on the raw corrected value, rather than waiting for the
    // (necessarily lagged) median-filtered value below, so this is as
    // responsive as possible.
    if (awaitingAutoZero && correctedWeightGrams >= UPLOAD_MIN_GRAMS)
    {
        Serial.println("[SCALE] Zeroing cancelled — load detected.");
        awaitingAutoZero       = false;
        collectingZeroBaseline = false;
        zeroBaselineCount      = 0;
        // capturedThisLoad/loadActive are untouched - still true, so the
        // scale simply resumes showing the frozen SAVED/OFFLINE result it
        // already had, rather than starting a new capture.
    }

    // --------------------------------------------------------
    // NEGATIVE TRANSIENT SANITIZATION
    // --------------------------------------------------------
    //
    // Ordinary negative sensor noise (drift the software zero correction
    // above didn't fully cancel, a settling knock - not the extreme/
    // corrupted values already rejected above) is treated as 0 for
    // filtering purposes only. correctedWeightGrams itself is left
    // untouched for diagnostics; this candidate is what actually feeds the
    // median filter.
    //

    float medianCandidate = (correctedWeightGrams < 0.0f) ? 0.0f : correctedWeightGrams;

    // --------------------------------------------------------
    // 3-READING MEDIAN FILTER
    // --------------------------------------------------------
    //
    // A single bad/negative cycle (already floored to 0 above) is outvoted
    // by the two other recent readings instead of reaching the stability
    // buffer directly - e.g. {250, -45→0, 249} filters to 249, not 0.
    //

    float filteredWeightGrams = pushMedianFilter(medianCandidate);

    // --------------------------------------------------------
    // ZERO DEADBAND
    // --------------------------------------------------------

    if (filteredWeightGrams >= -ZERO_DEADBAND_GRAMS && filteredWeightGrams <= ZERO_DEADBAND_GRAMS)
    {
        filteredWeightGrams = 0.0f;
    }

    bool platformEmpty = filteredWeightGrams < UPLOAD_MIN_GRAMS;

    // --------------------------------------------------------
    // RESET / POST-REMOVAL SOFTWARE ZERO WHEN SCALE IS EMPTY AGAIN
    // --------------------------------------------------------
    //
    // Two different "empty" cases, handled separately:
    //
    //   - A load pulled off mid-sample or mid-hold, before ever producing a
    //     confirmed measurement (loadActive but !capturedThisLoad): reset
    //     immediately, exactly as before - no re-zeroing. This is ordinary
    //     idle/noise territory, not a completed weighing, and the scale
    //     must NOT be constantly chasing zero while idle.
    //
    //   - A COMPLETED session's object being removed (loadActive AND
    //     capturedThisLoad): do NOT reset yet. capturedThisLoad is
    //     deliberately left true (see awaitingAutoZero's own comment) and
    //     the post-removal software-zero sequence begins instead, in two
    //     sub-phases:
    //       1. Initial settle: platform must read continuously empty for
    //          EMPTY_SETTLE_MS before anything is trusted (platform bounce/
    //          an object still being lifted off).
    //       2. Baseline collection: once settled, ZERO_BASELINE_SAMPLES
    //          signed samples are gathered (see ZERO BASELINE SAMPLE
    //          COLLECTION above) and handed to evaluateZeroBaseline(),
    //          which validates and either accepts a new
    //          softwareZeroBiasGrams or restarts collection - see that
    //          function's own comment.
    //     This is the fix for the baseline/zero drift observed in physical
    //     testing: each new weighing starts from a freshly measured
    //     software zero instead of whatever the empty-platform reading has
    //     wandered to - WITHOUT ever touching the HX711 hardware tare
    //     again after startup (a hardware re-tare here was proven unsafe -
    //     see this file's POST-REMOVAL SOFTWARE ZERO CORRECTION comment).
    //
    // A load reappearing during EITHER sub-phase is handled by the ZEROING
    // CANCELLATION check earlier in this same loop() cycle (right after
    // correctedWeightGrams is computed) - loadActive/capturedThisLoad are
    // never touched by that cancellation, so the scale simply resumes
    // showing the same frozen SAVED/OFFLINE result it already had. A later
    // removal re-enters this same block and starts a fresh attempt.
    //
    // Nothing to touch in `firebase` in either case - there is no
    // persisted state tied to the previous load at all (see the CAPTURE
    // block's comment): its outcome (SAVED or OFFLINE) was already final
    // the instant it was decided.
    //

    if (platformEmpty)
    {
        if (awaitingAutoZero)
        {
            if (!collectingZeroBaseline)
            {
                if (millis() - emptySettleStartTime >= EMPTY_SETTLE_MS)
                {
                    Serial.println("[SCALE] Platform settled empty — collecting zero baseline.");
                    collectingZeroBaseline = true;
                    zeroBaselineCount      = 0;
                }
                // else: still in the initial settle wait.
            }
            else if (zeroBaselineCount >= ZERO_BASELINE_SAMPLES)
            {
                evaluateZeroBaseline(); // accepts, or restarts collection - see its own comment
            }
            // else: still collecting samples this cycle onward.
        }
        else if (loadActive)
        {
            if (capturedThisLoad)
            {
                Serial.println("[SCALE] Object removed — waiting for empty platform to settle.");
                awaitingAutoZero       = true;
                collectingZeroBaseline = false;
                emptySettleStartTime   = millis();
            }
            else
            {
                Serial.println("[SCALE] Platform empty. Weighing session reset.");
                resetStabilityBuffer(0.0f);
                loadActive = false;
            }
        }
    }

    // --------------------------------------------------------
    // NOTE: once capturedThisLoad is true, this physical weighing session
    // is FINAL - later drift in filteredWeightGrams (the same untouched
    // object reading a few grams different a few seconds later) must NOT
    // reopen capture. There used to be a restack-detection check here that
    // did exactly that; removed because ordinary load-cell drift was
    // enough to cross STABLE_THRESHOLD_GRAMS and silently recapture the
    // same object at a different weight. The only way out of a captured
    // session is the empty-platform reset/software-zero above - see RESET
    // / POST-REMOVAL SOFTWARE ZERO WHEN SCALE IS EMPTY AGAIN.
    // --------------------------------------------------------
    // FIREBASE RECONNECT
    // --------------------------------------------------------
    //
    // firebase.begin() only ever ran once, in setup() - retried here
    // instead whenever WiFi is up but Firebase itself isn't ready yet,
    // cooldown-gated to FIREBASE_RETRY_INTERVAL_MS so this never re-runs
    // the multi-second NTP+auth sequence every reading cycle.
    //
    // Gated on the platform being empty this cycle (filteredWeightGrams
    // below UPLOAD_MIN_GRAMS): firebase.begin() performs NTP sync + a full
    // TLS auth handshake and can legitimately block for several seconds to
    // over ten. Running it while something is actively resting on the
    // platform would starve HX711 sampling for that span - the stability
    // buffer keeps advancing on wall-clock time (STABLE_HOLD_MS) without
    // ever actually observing the object holding still, so the very next
    // sample after the block could satisfy the hold timer immediately even
    // though nothing was genuinely watched settle.
    //
    // Network reconnection itself (NetworkManager::pollReconnect(), via
    // network.update() at the top of loop()) is NOT gated this way - it is
    // genuinely non-blocking (see NetworkManager.cpp), so it's always safe
    // to run regardless of weighing state.
    //
    // There is no queue-sync retry here anymore - offline measurements are
    // never persisted or retried (see the CAPTURE block below and
    // FirebaseManager's class comment). This block exists purely to keep
    // Firebase authenticated and ready for the NEXT measurement.
    //

    if (platformEmpty && network.isConnected() && !firebase.isReady() &&
        millis() - lastFirebaseRetryTime >= FIREBASE_RETRY_INTERVAL_MS)
    {
        lastFirebaseRetryTime = millis();
        if (firebase.begin())
        {
            Serial.println("[FIREBASE] Authentication restored.");
        }
    }

    // --------------------------------------------------------
    // STABILITY TRACKING
    // --------------------------------------------------------
    //
    // Pushed here, before display/live-weight/capture below all read the
    // buffer, so the averaged value they use already includes this cycle's
    // own sample rather than lagging a cycle behind.
    //
    // NOT gated on network/Firebase state - physical weighing state and
    // network state are deliberately independent. A new load accumulates
    // stability samples exactly the same whether Firebase is ready,
    // unreachable, or mid-reconnect; the CAPTURE block below always marks
    // this load captured once confirmed, regardless of whether the upload
    // to Firebase succeeds - see its own comment.

    bool nowStable = false;

    if (filteredWeightGrams >= UPLOAD_MIN_GRAMS)
    {
        loadActive = true;

        if (!capturedThisLoad)
        {
            nowStable = checkStability(filteredWeightGrams);
        }
    }

    // What actually gets shown/sent: once real (filtered) samples fill the
    // stability buffer, the settled average - see stabilityAverageGrams().
    // Before that (nothing on the platform yet, or an item only just
    // placed), falls back to this cycle's filtered reading, since there's
    // no real buffer yet to average.
    float reportedGrams = bufferFull ? stabilityAverageGrams() : filteredWeightGrams;

    // --------------------------------------------------------
    // SERIAL DIAGNOSTICS
    // --------------------------------------------------------
    //
    // One concise line per reading cycle. sensor vs. corrected makes the
    // software zero bias' effect obvious at a glance (e.g. sensor=-55.8
    // corrected=0.4 while empty, or sensor=166.0 corrected=222.2 while
    // loaded); corrected vs. filtered then shows whether a negative/noise
    // spike occurred without it reaching the effective measurement.
    // spread/hold are only meaningful once the stability buffer is
    // actually full / actively holding.
    //

    Serial.print("[SCALE] sensor=");
    Serial.print(sensorWeightGrams, 1);
    Serial.print(" corrected=");
    Serial.print(correctedWeightGrams, 1);
    Serial.print(" filtered=");
    Serial.print(filteredWeightGrams, 1);
    Serial.print(" spread=");
    Serial.print(stabilitySpreadGrams(), 1);
    Serial.print(" stable=");
    Serial.print(isStable ? "YES" : "NO");

    if (isStable)
    {
        Serial.print(" hold=");
        Serial.print(millis() - stableStartTime);
        Serial.print("/");
        Serial.print(STABLE_HOLD_MS);
    }

    Serial.println();

    // --------------------------------------------------------
    // LCD OUTPUT
    // --------------------------------------------------------
    //
    // Four states, checked in this order - deliberately independent of
    // network/Firebase state entirely - there is no "pending sync" concept
    // left to show (see the CAPTURE block below):
    //   1. awaitingAutoZero -> static "ZEROING... / Please wait" - covers
    //      the initial empty-settle wait, baseline sample collection, and
    //      any unstable-baseline retries (see evaluateZeroBaseline()). No
    //      fluctuating numbers, and never the signed bias itself - Serial
    //      is where that's logged. No blocking hardware operation happens
    //      during this state anymore (see POST-REMOVAL SOFTWARE ZERO
    //      CORRECTION), just per-cycle sample collection.
    //   2. Platform empty, not awaiting software zero (filteredWeightGrams
    //      < UPLOAD_MIN_GRAMS) -> "READY / Place harvest".
    //   3. Not yet captured, something on the platform (!capturedThisLoad)
    //      -> static "WEIGHING... / Hold still" - the raw fluctuating
    //      number is deliberately not shown while stabilizing.
    //      reportedGrams is still computed and fed to stability/capture/
    //      live-weight exactly as before.
    //   4. Captured (capturedThisLoad) -> falls through untouched, leaving
    //      whatever the CAPTURE block below already wrote on-screen
    //      ("SAVED" if the Firebase upload succeeded, "OFFLINE" if it
    //      didn't or was never attempted). Stays frozen - once captured,
    //      nothing reopens this session except a genuine removal (see the
    //      RESET / POST-REMOVAL SOFTWARE ZERO block above) - no lingering
    //      network message either way.
    //

    if (awaitingAutoZero)
    {
        display.showError("ZEROING...", "Please wait");
    }
    else if (platformEmpty)
    {
        display.showError("READY", "Place harvest");
    }
    else if (!capturedThisLoad)
    {
        // Item is on the platform but not yet confirmed - hide the raw
        // fluctuating number (it used to look like the scale was
        // "counting") and show a static message instead. displayWeightGrams
        // is still kept up to date here so the capture block below has the
        // right final value for the "SAVED"/"OFFLINE" message.
        displayWeightGrams = reportedGrams;
        display.showError("WEIGHING...", "Hold still");
    }

    // --------------------------------------------------------
    // LIVE WEIGHT → RTDB devices/{deviceId}/harvestScale/liveWeight  (every 5 seconds)
    // --------------------------------------------------------
    //
    // Throttled — Firebase SSL calls are slow on ESP8266.
    // Calling every reading cycle would flood the board and cause crashes.
    //
    // Frozen at the confirmed value once a load is locked in
    // (capturedThisLoad), instead of continuing to overwrite it with
    // ordinary raw-reading noise - the harvests/ entry is the source of
    // truth once logged, so a live number that keeps wiggling next to an
    // already-confirmed one reads as contradictory. Restack detection above
    // re-opens this the instant enough extra weight is added to be a real
    // new total rather than noise.
    //

    if (firebase.isReady() &&
        !capturedThisLoad &&
        millis() - lastLiveUpdateTime >= 5000)
    {
        lastLiveUpdateTime = millis();
        firebase.updateLiveWeight(reportedGrams);
    }

    if (filteredWeightGrams >= UPLOAD_MIN_GRAMS && !capturedThisLoad)
    {
        if (nowStable)
        {
            // Start stability timer on first stable detection
            if (!isStable)
            {
                isStable        = true;
                stableStartTime = millis();
                Serial.println("[SCALE] Weight stable — waiting to confirm...");
            }

            // Confirm after stable hold time
            if (millis() - stableStartTime >= STABLE_HOLD_MS)
            {
                Serial.print("[SCALE] Stable confirmed: ");
                Serial.print(reportedGrams, 1);
                Serial.println(" g");

                // capturedAt: the physical measurement's own timestamp,
                // set the moment it's confirmed - epoch seconds if the
                // clock is synced, 0 if it genuinely isn't (never a faked
                // absolute time). Same NTP-sync sanity check FirebaseManager
                // itself uses.
                uint32_t capturedAtEpoch =
                    (time(nullptr) > 8 * 3600 * 2) ? (uint32_t)time(nullptr) : 0;

                // The ONE deterministic decision point (see FirebaseManager's
                // class comment): attempt the upload ONLY if WiFi is
                // connected AND Firebase is already authenticated/ready -
                // never a reconnect or reauth attempt here, just the single
                // RTDB write itself, which is what keeps this bounded rather
                // than "however long a fresh connection would take." Either
                // way, this load is ALWAYS considered captured immediately
                // after this decision - there is no local-persistence
                // failure mode left to retry (nothing is written to flash
                // for an offline result), and no later upload attempt for
                // this measurement regardless of the outcome.
                bool wasUploaded = false;

                if (network.isConnected() && firebase.isReady())
                {
                    String measurementId;
                    if (firebase.uploadMeasurement(reportedGrams, capturedAtEpoch, measurementId))
                    {
                        wasUploaded = true;
                        Serial.print("[SCALE] Firebase measurement saved: ");
                        Serial.println(measurementId);
                    }
                }

                capturedThisLoad = true;
                isStable         = false;

                if (wasUploaded)
                {
                    display.showError("SAVED", String(displayGrams(displayWeightGrams)) + " g");
                }
                else
                {
                    Serial.print("[SCALE] Offline/local-only measurement: ");
                    Serial.print(reportedGrams, 1);
                    Serial.println(" g");
                    display.showError("OFFLINE", String(displayGrams(displayWeightGrams)) + " g");
                }
                delay(1500);
            }
        }
        else
        {
            // Weight is fluctuating — reset stability timer
            isStable        = false;
            stableStartTime = 0;
        }
    }
}