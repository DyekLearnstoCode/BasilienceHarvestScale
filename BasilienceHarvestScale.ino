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
// Re-refined:      112.21
// (Known ~500 g [500 mL pH Down solution, bottle net weight assumed ≈
// volume - not independently confirmed via label/other scale] → measured
// avg 507.6 g across 9 stable readings: 508, 507, 507, 509, 509, 508, 507,
// 507, 506)
//

constexpr float CALIBRATION_FACTOR = 112.21f;

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
// ADAPTIVE WARM-UP EARLY COMPLETION
// ------------------------------------------------------------
//
// WARMUP_TIME_MS above stays the hard MAXIMUM - unchanged, still 60000.
// This section only lets warmUpScale() finish EARLIER, never later, once
// the empty-platform baseline genuinely looks settled rather than always
// waiting out the full window regardless of hardware behavior.
//
// WARMUP_MIN_TIME_MS is a hard floor before early completion is even
// considered - deliberately the SAME 30s the warm-up window itself used
// to be, before physical testing showed 30s wasn't always enough (see
// WARMUP_TIME_MS's own comment above). Reusing that already-validated
// value as the floor means an unusually fast-settling unit still gets at
// least as much settling time as this firmware trusted before the 60s
// increase - this can only add MORE margin than that, never less.
//
// The stability check itself deliberately reuses ZERO_BASELINE_MAX_SPREAD_GRAMS
// (defined below) rather than STABLE_THRESHOLD_GRAMS - the two constants
// answer different physical questions. STABLE_THRESHOLD_GRAMS (10g) is
// how much a LOADED platform may still wobble while an object settles;
// ZERO_BASELINE_MAX_SPREAD_GRAMS (8g) is how tight an EMPTY platform must
// already be trusted to be a real zero - the same physical scenario
// warm-up is evaluating, just before the startup tare instead of after a
// removal. No new threshold is introduced.
//
constexpr unsigned long WARMUP_MIN_TIME_MS = 30000;

// How often (after the minimum) the rolling warm-up window is checked.
constexpr unsigned long WARMUP_EVAL_INTERVAL_MS = 3000;

// Rolling window size for the warm-up check - same shape as
// STABLE_READINGS (a small circular buffer, min/max spread checked), just
// applied here to the raw pre-tare baseline instead of a captured
// weighing. Deliberately does NOT reuse ZERO_BASELINE_SAMPLES/
// medianOfZeroBaseline() as-is - those are sized and hardcoded for a
// one-shot 9-sample POST-tare batch; a smaller continuously-updated
// buffer suits a window that's re-evaluated repeatedly over a full
// 30-60s span instead of collected once.
constexpr uint8_t WARMUP_STABILITY_WINDOW = 5;

// Consecutive passing evaluation windows required before early completion
// is allowed - a single quiet window (~15s into a 30s floor) must not be
// enough on its own; this needs roughly WARMUP_REQUIRED_CONSECUTIVE_PASSES
// * WARMUP_EVAL_INTERVAL_MS (here, 3 * 3s = 9s) of CONTINUED agreement
// before the load cell is trusted as genuinely settled rather than just
// momentarily quiet.
constexpr uint8_t WARMUP_REQUIRED_CONSECUTIVE_PASSES = 3;

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
// RESTACK DETECTION (cumulative harvest weight)
// ------------------------------------------------------------
//
// Lets the operator build up a harvest's total weight by adding items to
// the platform one at a time (or removing some without clearing it
// entirely) WITHOUT lifting everything off between each one - see RESTACK
// DETECTION in loop(). A version of this existed once before and was
// removed because it reused STABLE_THRESHOLD_GRAMS (10g): ordinary
// load-cell jitter was enough to cross that on its own, occasionally
// re-opening capture for an object that never actually changed. This
// constant is deliberately its own, much larger value - observed per-cycle
// noise in physical testing tops out around 2-3g, so 25g is roughly an
// order of magnitude above anything that's ever shown up as noise, while
// still well under a typical harvest increment (a single basil sprig is
// easily tens of grams).
//
constexpr float RESTACK_THRESHOLD_GRAMS = 25.0f;

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
// PENDING SYNC RETRY
// ------------------------------------------------------------
//
// A captured-but-unsynced measurement (see PENDING MEASUREMENT in
// FirebaseManager.h) is retried whenever Firebase is already ready -
// cooldown-gated, much shorter than FIREBASE_RETRY_INTERVAL_MS above,
// since this is just one lightweight RTDB write (not a multi-second NTP+
// auth sequence) and the normal case is "was already ready, succeeds on
// the very first retry." Only guards against hammering the network every
// single ~300ms reading cycle on the rare persistent-failure path.
//
constexpr unsigned long PENDING_SYNC_RETRY_INTERVAL_MS = 3000;

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
unsigned long lastPendingSyncRetryTime = 0;  // Cooldown for re-attempting a pending measurement's sync

// What's actually shown on the LCD - a plain copy of reportedGrams, updated
// only while no confirmed load is holding the screen frozen (see the LCD
// OUTPUT block in loop()). Kept as its own variable (rather than reading
// reportedGrams directly at display time) so the "SAVED"/"PENDING"
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
// INSTANT that measurement is durably persisted to flash (see
// FirebaseManager::stagePendingMeasurement()/restagePendingMeasurement()),
// regardless of whether a Firebase upload has happened yet. Reset back to
// false by RESTACK DETECTION below if the total on the platform changes
// meaningfully before the session actually ends (see
// currentSessionMeasurementId) - so "captured" means "confirmed as of the
// last stability check," not "this session can never be re-evaluated
// again." May still show "PENDING" on the LCD and keep retrying in the
// background while true - see firebase.hasPendingMeasurement().
bool  capturedThisLoad = false;

// Whether ANY load ≥ UPLOAD_MIN_GRAMS is currently on the platform,
// regardless of capturedThisLoad - tracks the physical weighing SESSION
// itself, separately from whether it happened to already produce a
// confirmed measurement. Used purely to detect the active→empty
// transition exactly once (see the RESET block in loop()), so stability
// state is always reset when the platform empties - even if this load
// was removed mid-sample or mid-hold and never actually got captured.
bool  loadActive = false;

// Identity of the CURRENT physical weighing session - set once, the
// instant its FIRST capture succeeds, and left unchanged (reused, never
// regenerated) across any number of later restacks within that same
// session. Empty whenever there is no active captured session (before the
// first capture, and again once the platform goes fully empty - see the
// RESET block). This is what lets a restack update the SAME RTDB record
// instead of creating a competing one: the CAPTURE block checks this to
// decide stagePendingMeasurement() (fresh id) vs.
// restagePendingMeasurement() (reuse this id). Also what the STABILITY
// TRACKING gate uses to tell "a pending measurement that's blocking a
// genuinely new/different session" apart from "this session's own,
// perfectly normal in-progress pending record."
String currentSessionMeasurementId = "";

// What the CURRENT session was last captured/restacked AT - compared
// against the live filtered reading by RESTACK DETECTION below to decide
// whether the total on the platform has changed enough to re-open capture.
// Meaningless while currentSessionMeasurementId is empty.
float lastCapturedWeightGrams = 0.0f;

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

// Cache of what's currently on the LCD, so the per-cycle static-text
// states (READY/WEIGHING/ZEROING, and a persisting error) aren't
// rewritten over I2C every single ~300ms cycle when nothing has actually
// changed - see showIfChanged() below. LiquidCrystal_I2C has no way to
// query its own current contents, so this is tracked ourselves.
String lastDisplayLine1;
String lastDisplayLine2;
bool   lastDisplayValid = false;

// [PERF] timing reference points - see showIfChanged()'s neighboring
// helpers, setup(), and the CAPTURE/RESET blocks in loop().
unsigned long perfScaleReadyTime      = 0; // last moment the scale became available for a NEW weighing
unsigned long perfLoadDetectedTime    = 0; // when the CURRENT load was first detected
unsigned long perfRemovalDetectedTime = 0; // when the current object's removal was first detected

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

// P6: skips the LCD write entirely when the requested text matches what's
// already shown. Every loop()-driven display call goes through this (not
// setup()'s one-shot boot messages, which never repeat anyway).
//
// Logs every ACTUAL change (never a no-op call that got skipped above) to
// Serial as "[LCD] line1 / line2" - this is the only place in loop() that
// ever writes to the screen, so this one log line is a complete record of
// exactly what the LCD showed and when, correlatable against the
// [SCALE]/[PERF]/[FB] lines around it without needing to look at the
// physical screen at all.
void showIfChanged(const String& line1, const String& line2)
{
    if (lastDisplayValid && line1 == lastDisplayLine1 && line2 == lastDisplayLine2) { return; }
    display.showError(line1, line2);
    lastDisplayLine1 = line1;
    lastDisplayLine2 = line2;
    lastDisplayValid = true;

    Serial.print("[LCD] ");
    Serial.print(line1);
    Serial.print(" / ");
    Serial.println(line2);
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

// startTime is when the HX711 finished its own init (see setup()) - NOT
// necessarily "now". Physical settling begins the instant the load cell is
// powered/initialized, and everything setup() does between then and here
// (WiFi connect, Firebase auth, boot LCD messages) happens while that
// settling is ALREADY underway - so it counts toward the same 60s window
// instead of being paid twice. The while condition below naturally waits
// only for whatever's left of WARMUP_TIME_MS by the time we get here (zero
// iterations, i.e. no wait at all, if WiFi/Firebase already burned the
// whole 60s) - the load cell still always receives the FULL 60 seconds of
// physical settling before tare; this only removes duplicate waiting on
// top of that, never shortens the physical requirement itself.
void warmUpScale(unsigned long startTime)
{
    Serial.println();
    Serial.println("====================================");
    Serial.println(" SCALE WARM-UP");
    Serial.println("====================================");
    Serial.println();
    Serial.println("[SCALE] Keep platform EMPTY.");
    Serial.println("[SCALE] Do NOT touch the scale.");

    unsigned long lastPrintTime = 0;
    unsigned long lastLcdTime   = 0;

    // Adaptive early-completion state - see the ADAPTIVE WARM-UP EARLY
    // COMPLETION comment above WARMUP_MIN_TIME_MS. Entirely inert (buffer
    // fills, nothing else happens) until WARMUP_MIN_TIME_MS has elapsed;
    // before that this behaves exactly like the old fixed-time wait.
    float         warmupWindow[WARMUP_STABILITY_WINDOW];
    uint8_t       warmupWindowCount  = 0;
    uint8_t       warmupWindowIndex  = 0;
    bool          haveLastWindowAvg  = false;
    float         lastWindowAvgGrams = 0.0f;
    uint8_t       consecutivePasses  = 0;
    unsigned long lastEvalTime       = 0;
    bool          completedEarly     = false;

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

            // ----------------------------------------------------
            // ADAPTIVE EARLY COMPLETION
            // ----------------------------------------------------
            //
            // Untared "grams-equivalent" from the SAME rawValue already
            // fetched above (no extra HX711 read) - tare hasn't run yet at
            // this point in setup(), so this isn't a real weight, but
            // spread/drift are translation-invariant: whatever constant
            // offset the eventual tare removes cancels out of a max-min or
            // window-to-window comparison either way.
            float gramsEquiv = (float)rawValue / loadCell.getCalibrationFactor();

            warmupWindow[warmupWindowIndex] = gramsEquiv;
            warmupWindowIndex = (warmupWindowIndex + 1) % WARMUP_STABILITY_WINDOW;
            if (warmupWindowCount < WARMUP_STABILITY_WINDOW) { warmupWindowCount++; }

            if (elapsed >= WARMUP_MIN_TIME_MS &&
                warmupWindowCount >= WARMUP_STABILITY_WINDOW &&
                millis() - lastEvalTime >= WARMUP_EVAL_INTERVAL_MS)
            {
                lastEvalTime = millis();

                float minVal = warmupWindow[0];
                float maxVal = warmupWindow[0];
                float sum    = warmupWindow[0];
                for (uint8_t i = 1; i < WARMUP_STABILITY_WINDOW; i++)
                {
                    if (warmupWindow[i] < minVal) { minVal = warmupWindow[i]; }
                    if (warmupWindow[i] > maxVal) { maxVal = warmupWindow[i]; }
                    sum += warmupWindow[i];
                }

                // spread: short-term noise within THIS window (same shape
                // as checkStability()'s own min/max spread).
                float spread = maxVal - minVal;

                // drift: how far the window's average has moved since the
                // LAST evaluation (~WARMUP_EVAL_INTERVAL_MS ago) - catches
                // a slow monotonic slide that a short window's own spread
                // could miss entirely (a steady 1g/s drift barely shows up
                // across ~5 samples spanning well under a second, but does
                // show up compared against a checkpoint several seconds
                // earlier). First window has no prior checkpoint, so it
                // can't pass on drift alone yet - falls back to spread.
                float windowAvg = sum / WARMUP_STABILITY_WINDOW;
                float drift     = haveLastWindowAvg ? fabsf(windowAvg - lastWindowAvgGrams) : spread;
                haveLastWindowAvg  = true;
                lastWindowAvgGrams = windowAvg;

                // Reuses ZERO_BASELINE_MAX_SPREAD_GRAMS for BOTH checks -
                // see that constant's neighboring comment for why it (not
                // STABLE_THRESHOLD_GRAMS) is the physically-correct
                // threshold for an EMPTY platform.
                bool windowPasses = (spread <= ZERO_BASELINE_MAX_SPREAD_GRAMS) &&
                                    (drift  <= ZERO_BASELINE_MAX_SPREAD_GRAMS);

                consecutivePasses = windowPasses ? (consecutivePasses + 1) : 0;

                Serial.print("[WARMUP] ");
                Serial.print(elapsed / 1000);
                Serial.print("s | spread=");
                Serial.print(spread, 1);
                Serial.print(" drift=");
                Serial.print(drift, 1);
                Serial.print(" | ");
                Serial.println(windowPasses ? "ok" : "waiting");

                if (consecutivePasses >= WARMUP_REQUIRED_CONSECUTIVE_PASSES)
                {
                    Serial.print("[WARMUP] Stable early at ");
                    Serial.print(elapsed / 1000.0f, 1);
                    Serial.println("s");
                    completedEarly = true;
                    break;
                }
            }
        }

        delay(10);
    }

    if (!completedEarly)
    {
        Serial.print("[WARMUP] Maximum ");
        Serial.print(WARMUP_TIME_MS / 1000);
        Serial.println("s reached");
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
    capturedThisLoad            = false;
    loadActive                  = false;
    awaitingAutoZero            = false;
    collectingZeroBaseline      = false;
    zeroBaselineCount           = 0;
    // Session genuinely over - the platform is confirmed empty and
    // re-zeroed. The next capture (whenever it happens) is a brand new
    // session with its own fresh measurementId, not a restack of this one.
    currentSessionMeasurementId = "";
    lastCapturedWeightGrams     = 0.0f;

    // Same reasoning as the other loadActive=false site in loop() - forces
    // the LIVE WEIGHT block, later in this same loop() cycle, to push
    // immediately instead of waiting out its normal 5s throttle, so a
    // stale nonzero liveWeight doesn't linger in Firebase after the
    // platform has already finished re-zeroing empty.
    lastLiveUpdateTime = 0;

    // [PERF] Re-armed: time since removal was first detected until the
    // scale is available again - also becomes the new reference point for
    // the NEXT "[PERF] Load detected" line.
    perfScaleReadyTime = millis();
    Serial.print("[PERF] Re-armed: +");
    Serial.print(perfScaleReadyTime - perfRemovalDetectedTime);
    Serial.println("ms");
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
    delay(200); // brief USB-serial settle - not required by any hardware init step below

    Serial.println();
    Serial.println("====================================");
    Serial.println("   Basilience Harvest Scale v3");
    Serial.println("====================================");

    // --------------------------------------------------------
    // LCD
    // --------------------------------------------------------

    display.begin();
    display.showBoot();
    delay(400); // brief branding pause - UI only, not a hardware requirement

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
        delay(1000);
    }
    else
    {
        Serial.println("[SCALE] HX711 detected.");
    }
    loadCell.setCalibrationFactor(CALIBRATION_FACTOR);

    // --------------------------------------------------------
    // PHYSICAL WARM-UP TIMING START (see warmUpScale()'s own comment)
    // --------------------------------------------------------

    unsigned long warmupStartTime = millis();
    Serial.print("[PERF] Boot HX711 ready: ");
    Serial.print(warmupStartTime);
    Serial.println("ms");

    // --------------------------------------------------------
    // LOCAL STORAGE
    // --------------------------------------------------------
    //
    // Deliberately BEFORE WiFi - device identity, the measurement-sequence
    // counter, and any pending measurement left over from a previous boot
    // must be usable/recovered even on a unit that never reaches a network
    // at all. Also clears out the now-unused offline-QUEUE format a brief
    // earlier firmware revision used (see
    // FirebaseManager::clearLegacyQueueData()) - this revision still only
    // supports a single pending slot, just a persistent/retried one again.
    // firebase.begin() further below also mounts this (idempotently) since
    // it separately needs LittleFS for auth credentials.
    //

    if (!firebase.beginLocalStorage())
    {
        Serial.println("[SCALE] WARNING: Local storage unavailable - a confirmed measurement could not be recovered/persisted across reboots.");
    }

    // Registered BEFORE network.connect() below, since that call can itself
    // start the setup portal (no saved credentials at all) - the portal
    // must already be able to show/discard a pending measurement from its
    // very first page load, not just once some later reconnect attempt
    // falls back to it.
    network.setPendingMeasurementProvider(
        [](){ return firebase.hasPendingMeasurement(); },
        [](){ return firebase.getPendingGrams(); },
        [](){ return firebase.getPendingMeasurementId(); },
        []() {
            bool discarded = firebase.discardPendingMeasurement();
            if (discarded)
            {
                // Full reset, not just the Firebase-side state - "discard"
                // means forget this physical weighing entirely. If its
                // object happens to still be sitting on the platform at the
                // exact moment someone does this from the portal (unusual,
                // but possible), this makes the scale treat it as a brand
                // new, uncaptured load from here on, rather than leaving a
                // stale "PENDING" message frozen on the LCD with nothing
                // left to actually be pending.
                capturedThisLoad            = false;
                loadActive                  = false;
                resetStabilityBuffer(0.0f);
                // The whole session is abandoned, not just this one record -
                // a restack after this must start a fresh measurementId, not
                // try to update the id that was just thrown away.
                currentSessionMeasurementId = "";
                lastCapturedWeightGrams     = 0.0f;
            }
            return discarded;
        }
    );

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
        delay(500);
    }
    else if (network.isProvisioning())
    {
        Serial.println("[WIFI] Setup portal active - scale continues in offline/local mode.");
        delay(1000); // Let showWifiSetupModeOnDisplay()'s message stay readable briefly.
    }
    else
    {
        Serial.println("[WIFI] Offline mode — no Firebase upload.");
        display.showError("WiFi FAILED", "Offline mode");
        delay(1000);
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
            delay(400);
        }
        else
        {
            Serial.println("[FB] Firebase init failed.");
            display.showError("Firebase FAILED", "Check config");
            delay(1000);
        }
    }

    // --------------------------------------------------------
    // WARM-UP (60 seconds of physical settling, minus whatever elapsed
    // above since the HX711 finished initializing - see warmUpScale())
    // --------------------------------------------------------

    warmUpScale(warmupStartTime);
    Serial.print("[PERF] Physical warmup complete: ");
    Serial.print(millis());
    Serial.println("ms");

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
        delay(1000);
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
    delay(500);

    perfScaleReadyTime = millis();
    Serial.print("[PERF] Scale ready: ");
    Serial.print(perfScaleReadyTime);
    Serial.println("ms");
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

    // Always non-blocking (see FirebaseManager::pollReconnect()'s own
    // comment) - safe to call unconditionally every iteration, same as
    // network.update() above. Advances any in-progress reconnect attempt
    // one small step; a no-op the rest of the time.
    firebase.pollReconnect();

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
        showIfChanged("Read timeout!", "Check HX711");
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
        showIfChanged("Bad reading!", "");
        return;
    }

    if (sensorWeightGrams > MAX_WEIGHT_GRAMS)
    {
        Serial.println("[SCALE] ERROR: Weight exceeds 20 kg.");
        showIfChanged("OVERLOAD!", "Max: 20 kg");
        return;
    }

    if (sensorWeightGrams < -1000.0f)
    {
        Serial.println("[SCALE] ERROR: Invalid negative reading.");
        showIfChanged("Bad reading!", "");
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
    //
    // zeroingCancelledThisCycle guards against a thrash this used to cause:
    // the RESET block further down decides "object removed" from
    // filteredWeightGrams (the 3-sample median - necessarily 1-2 cycles
    // behind a fresh raw reading), so on the exact cycle a load reappears,
    // this check can correctly cancel zeroing here on the raw value while
    // the RESET block, still seeing a stale low filtered value THIS SAME
    // cycle, immediately re-triggers "Object removed" and flips
    // awaitingAutoZero back to true - undoing the cancel that was just
    // decided a few lines above it. This flag tells the RESET block "a real
    // load was just confirmed THIS cycle - don't re-enter the removal path
    // again until the filter has had a chance to catch up."
    bool zeroingCancelledThisCycle = false;

    if (awaitingAutoZero && correctedWeightGrams >= UPLOAD_MIN_GRAMS)
    {
        Serial.println("[SCALE] Zeroing cancelled — load detected.");
        awaitingAutoZero          = false;
        collectingZeroBaseline    = false;
        zeroBaselineCount         = 0;
        zeroingCancelledThisCycle = true;
        // capturedThisLoad/loadActive are untouched - still true, so the
        // scale simply resumes showing whatever result it already had
        // (SAVED if already synced, PENDING if not yet), rather than
        // starting a new capture.
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
    // showing the same frozen result it already had. A later removal
    // re-enters this same block and starts a fresh attempt.
    //
    // Deliberately does NOT touch anything in `firebase` in either case - a
    // pending, not-yet-synced measurement (see PENDING MEASUREMENT in
    // FirebaseManager.h) is a SEPARATE concept from this local physical-
    // load/re-zeroing state and must survive the platform being emptied,
    // the re-zero sequence running, and even a reboot. It only ever clears
    // once RTDB actually confirms the write - see the PENDING SYNC RETRY
    // block further down in this function.
    //
    // Gated on !zeroingCancelledThisCycle - see that flag's own comment
    // above. Without this, a load reappearing mid-zeroing could cancel
    // correctly on the raw value above, then get immediately re-classified
    // as "removed" right here on the same cycle's still-lagging filtered
    // value, flipping awaitingAutoZero straight back to true the instant it
    // was cleared. Skipping this block for one cycle lets the median filter
    // catch up; the next cycle re-evaluates normally either way.
    //

    if (platformEmpty && !zeroingCancelledThisCycle)
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
            // Use currentSessionMeasurementId, not capturedThisLoad, to
            // decide whether this session was ever confirmed/saved.
            // RESTACK DETECTION (below) clears capturedThisLoad the moment
            // the weight drops below the last captured total - including
            // when the operator is removing everything at once - so by the
            // time filteredWeightGrams actually reaches empty here,
            // capturedThisLoad is already false even for a session that WAS
            // saved to Firebase. Without this, a restack-ended session fell
            // into the "mid-sample pullback, no re-zero" branch below and
            // the post-removal software zero never ran - the exact bug that
            // let softwareZeroBiasGrams drift uncorrected across repeated
            // restack sessions until it hit the ~173g phantom reading.
            // currentSessionMeasurementId stays set across restacks
            // (deliberately - see RESTACK DETECTION's own comment) and is
            // only cleared once a session is fully closed out, so it
            // reflects "was this session ever captured", not "right now".
            bool sessionWasCaptured = capturedThisLoad || !currentSessionMeasurementId.isEmpty();
            if (sessionWasCaptured)
            {
                Serial.println("[SCALE] Object removed — waiting for empty platform to settle.");
                awaitingAutoZero       = true;
                collectingZeroBaseline = false;
                emptySettleStartTime   = millis();
                perfRemovalDetectedTime = millis();
            }
            else
            {
                Serial.println("[SCALE] Platform empty. Weighing session reset.");
                resetStabilityBuffer(0.0f);
                loadActive = false;
                // Defensive, not strictly required here - capturedThisLoad
                // was already false in this branch, so a session id should
                // never have been assigned yet. Cheap to reset anyway.
                currentSessionMeasurementId = "";
                lastCapturedWeightGrams     = 0.0f;
                // Forces the LIVE WEIGHT block further down THIS SAME cycle
                // to push immediately instead of waiting out its normal 5s
                // throttle - otherwise a stale nonzero liveWeight could sit
                // in Firebase for up to 5s after the platform is genuinely
                // already empty again.
                lastLiveUpdateTime = 0;
            }
        }
    }

    // --------------------------------------------------------
    // RESTACK DETECTION (cumulative harvest weight)
    // --------------------------------------------------------
    //
    // Lets the operator keep adding to (or partially taking back off) a
    // harvest already captured on the platform, without lifting everything
    // off first - e.g. place one item, let it confirm, place a second item
    // on top, let the NEW total confirm, and so on. A version of this
    // existed once, was removed because it reused STABLE_THRESHOLD_GRAMS
    // (10g) and ordinary load-cell jitter crossed that on its own; this one
    // uses the deliberately much larger RESTACK_THRESHOLD_GRAMS (25g)
    // instead - see that constant's own comment.
    //
    // Gated on capturedThisLoad (a session must already be captured) and
    // !awaitingAutoZero (the post-removal re-zero sequence handles its own
    // "load came back" cancellation separately - see the ZEROING
    // CANCELLATION check earlier in this same loop() cycle; restack must
    // not also fire during that window). Checked against
    // lastCapturedWeightGrams, not the stability buffer average, since the
    // buffer is about to be reset below anyway.
    //
    // Only resets capturedThisLoad and re-opens stability tracking - it
    // deliberately does NOT touch currentSessionMeasurementId. That's the
    // whole point: the CAPTURE block below sees capturedThisLoad false but
    // currentSessionMeasurementId still set, and knows to REUSE that same
    // id (restagePendingMeasurement()) once the new total re-confirms,
    // rather than minting a fresh one. One RTDB record per session, always
    // overwritten to the latest total - never two competing entries where
    // an app reading "the newest" could land on a now-superseded partial
    // weight instead of the current one.
    //

    if (capturedThisLoad && !awaitingAutoZero &&
        filteredWeightGrams >= UPLOAD_MIN_GRAMS &&
        fabsf(filteredWeightGrams - lastCapturedWeightGrams) > RESTACK_THRESHOLD_GRAMS)
    {
        Serial.print("[SCALE] Weight changed on platform (restack) - re-evaluating: ");
        Serial.print(lastCapturedWeightGrams, 1);
        Serial.print(" g -> ~");
        Serial.print(filteredWeightGrams, 1);
        Serial.println(" g");
        capturedThisLoad = false;
        restartStabilityTracking();
    }

    // --------------------------------------------------------
    // FIREBASE RECONNECT
    // --------------------------------------------------------
    //
    // Retried here, cooldown-gated to FIREBASE_RETRY_INTERVAL_MS, whenever
    // WiFi is up but Firebase itself isn't ready yet. Non-blocking:
    // startReconnect() below only ever KICKS OFF an attempt (never blocks);
    // firebase.pollReconnect(), called unconditionally near the top of
    // loop(), does the actual stepping, one small bounded check per call -
    // see FirebaseManager's class comment for the full reasoning and for
    // why a fresh HTTPS bootstrap is deliberately out of scope for this
    // path (only setup()'s one-time firebase.begin() does that).
    //
    // Still gated on the platform being empty this cycle, same as before -
    // not because starting an attempt could block anything anymore, but to
    // keep priority ordering simple: never kick off new network work while
    // an active weighing is in progress. Network reconnection itself
    // (NetworkManager::pollReconnect(), via network.update() at the top of
    // loop()) has never needed this gate - it was already fully
    // non-blocking (see NetworkManager.cpp).
    //
    // This block exists purely to keep Firebase authenticated and ready -
    // for the NEXT measurement, and for the PENDING SYNC RETRY step right
    // below, which is what actually delivers an already-captured
    // measurement once this reconnect succeeds.
    //

    if (platformEmpty && network.isConnected() && !firebase.isReady() &&
        millis() - lastFirebaseRetryTime >= FIREBASE_RETRY_INTERVAL_MS)
    {
        lastFirebaseRetryTime = millis();
        firebase.startReconnect();
    }

    // --------------------------------------------------------
    // PENDING SYNC RETRY
    // --------------------------------------------------------
    //
    // A measurement captured while offline (or one whose immediate sync
    // attempt in the CAPTURE block below simply failed) stays durably
    // persisted on flash - see FirebaseManager::stagePendingMeasurement().
    // Retried here, cooldown-gated, any time Firebase is ready. Cheap and
    // NOT gated on platformEmpty - this is a single lightweight RTDB write,
    // not the multi-second reconnect sequence above, and reusing the same
    // gate would mean a load placed back on the platform before this synced
    // could stall it indefinitely (see the STABILITY TRACKING gate below
    // for why a new load is blocked anyway while this is pending).
    //
    // On success, explicitly refreshes the LCD to "SAVED" if this is still
    // the frozen screen showing that measurement's result - the CAPTURE
    // block's own showIfChanged() calls only ever cover the moment of
    // capture itself, not a sync that completes later, asynchronously, from
    // right here.
    //

    if (firebase.isReady() && firebase.hasPendingMeasurement() &&
        millis() - lastPendingSyncRetryTime >= PENDING_SYNC_RETRY_INTERVAL_MS)
    {
        lastPendingSyncRetryTime = millis();

        if (firebase.syncPendingMeasurement())
        {
            Serial.println("[FB] Pending measurement synced (retry).");

            if (capturedThisLoad)
            {
                showIfChanged("SAVED", String(displayGrams(displayWeightGrams)) + " g");
            }
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
    // Gated on blockedByOtherSessionPending, not a blanket
    // !firebase.hasPendingMeasurement(): a still-unsynced measurement only
    // blocks a GENUINELY NEW/different session from accumulating stability
    // samples (the one-slot design - see FirebaseManager.h) - it must NOT
    // block THIS session's own restack from re-stabilizing, since restack
    // reuses the exact same pending slot (currentSessionMeasurementId) via
    // restagePendingMeasurement() rather than contending for a new one.
    // currentSessionMeasurementId empty is exactly "no session currently
    // owns whatever's pending," which is the only time a stray pending
    // record can legitimately block a new one from starting.
    //
    // loadActive itself still tracks normally regardless (see just below),
    // so the RESET block above still correctly notices a blocked load being
    // removed; tracking simply resumes, from a clean buffer, the moment the
    // pending measurement clears.

    bool nowStable = false;
    bool blockedByOtherSessionPending =
        firebase.hasPendingMeasurement() && currentSessionMeasurementId.isEmpty();

    if (filteredWeightGrams >= UPLOAD_MIN_GRAMS)
    {
        if (!loadActive)
        {
            // [PERF] Load detected: time since the scale last became
            // available (boot's "Scale ready" or the last "Re-armed").
            perfLoadDetectedTime = millis();
            Serial.print("[PERF] Load detected: +");
            Serial.print(perfLoadDetectedTime - perfScaleReadyTime);
            Serial.println("ms");
        }

        loadActive = true;

        if (!capturedThisLoad && !blockedByOtherSessionPending)
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
    // Five states, checked in this order:
    //   1. awaitingAutoZero -> static "ZEROING... / Please wait" - covers
    //      the initial empty-settle wait, baseline sample collection, and
    //      any unstable-baseline retries (see evaluateZeroBaseline()). No
    //      fluctuating numbers, and never the signed bias itself - Serial
    //      is where that's logged. No blocking hardware operation happens
    //      during this state anymore (see POST-REMOVAL SOFTWARE ZERO
    //      CORRECTION), just per-cycle sample collection. Takes priority
    //      over everything below even if this same load is also unsynced -
    //      the physical re-zero is the more time-sensitive thing to show.
    //   2. blockedByOtherSessionPending - a genuinely different/new load,
    //      blocked entirely from capture because some OTHER session's
    //      measurement is still unsynced (the one-slot design - see the
    //      STABILITY TRACKING gate above) -> always "PENDING / Check WiFi",
    //      never stored grams, since they wouldn't belong to whatever's
    //      actually sitting on the platform right now.
    //   3. Not yet captured, something on the platform and loadActive
    //      (!capturedThisLoad) -> static "WEIGHING... / Hold still" - the
    //      raw fluctuating number is deliberately not shown while
    //      stabilizing. Covers BOTH this session's very first capture
    //      attempt AND re-stabilizing after a RESTACK DETECTION re-open -
    //      either way there's nothing useful to show except "still
    //      settling," never a stale PENDING value from before the restack.
    //      Checked BEFORE state 4 below so an in-progress restack is never
    //      masked by its own session's still-unsynced PRIOR total.
    //   4. THIS session's own measurement is pending sync
    //      (firebase.hasPendingMeasurement(), reached only once state 3
    //      above no longer applies - i.e. settled, or the load's been
    //      removed): shows the confirmed "PENDING / {g} g" while the
    //      platform still holds it, or "PENDING / Check WiFi" once it's
    //      been removed (that load is gone; showing its old grams would be
    //      misleading).
    //   5. Platform empty, nothing pending (filteredWeightGrams <
    //      UPLOAD_MIN_GRAMS) -> "READY / Place harvest".
    //   6. Captured, nothing pending, platform not empty (capturedThisLoad)
    //      -> falls through untouched, leaving the "SAVED" message the
    //      CAPTURE block (or the PENDING SYNC RETRY block, if the sync
    //      completed later) already wrote on-screen. Stays frozen - once
    //      captured, nothing reopens this session except a genuine removal
    //      (see the RESET / POST-REMOVAL SOFTWARE ZERO block above) or a
    //      restack (state 3 above).
    //

    if (awaitingAutoZero)
    {
        showIfChanged("ZEROING...", "Please wait");
    }
    else if (blockedByOtherSessionPending)
    {
        // A genuinely different/new load, blocked entirely from capture
        // while some OTHER (not this session's) measurement is still
        // unsynced - never show stored grams here, since they wouldn't
        // belong to whatever's actually on the platform right now.
        showIfChanged("PENDING", "Check WiFi");
    }
    else if (!capturedThisLoad && loadActive)
    {
        // Item is on the platform but not yet confirmed - hide the raw
        // fluctuating number (it used to look like the scale was
        // "counting") and show a static message instead. displayWeightGrams
        // is still kept up to date here so the capture block below has the
        // right final value for the "SAVED"/"PENDING" message.
        displayWeightGrams = reportedGrams;
        showIfChanged("WEIGHING...", "Hold still");
    }
    else if (firebase.hasPendingMeasurement())
    {
        // THIS session's own total, already captured (or its load already
        // removed), still unsynced.
        if (platformEmpty)
        {
            showIfChanged("PENDING", "Check WiFi");
        }
        else
        {
            // displayGrams() (whole-gram rounding), not a raw one-decimal
            // String() cast - matches the exact value the CAPTURE block
            // itself just showed via displayWeightGrams, so this state
            // never visibly flickers precision (e.g. "86 g" then "86.3 g")
            // for the same underlying pending measurement across cycles.
            showIfChanged("PENDING", String(displayGrams(firebase.getPendingGrams())) + " g");
        }
    }
    else if (platformEmpty)
    {
        showIfChanged("READY", "Place harvest");
    }

    // --------------------------------------------------------
    // LIVE WEIGHT → RTDB devices/{deviceId}/harvestScale/liveWeight  (every 5 seconds)
    // --------------------------------------------------------
    //
    // Throttled — Firebase SSL calls are slow on ESP8266.
    // Calling every reading cycle would flood the board and cause crashes.
    //
    // Suspended for the ENTIRE active-weighing session (loadActive), not
    // just after capture - HX711 sampling/stability timing takes priority
    // over this nonessential telemetry, and a blocking RTDB write landing
    // mid-stabilization could otherwise delay reaching STABLE_HOLD_MS. Also
    // stays suspended through the post-removal re-zeroing window, since
    // loadActive only clears once that finishes - nothing meaningful to
    // report live during either window anyway. Does not touch the final
    // confirmed-weight upload, which is a separate, unconditional call in
    // the CAPTURE block below.
    //
    // Fires immediately, bypassing the 5s throttle for one cycle, the
    // instant loadActive transitions back to false (both sites that do
    // that reset lastLiveUpdateTime to 0 - see the RESET block above and
    // evaluateZeroBaseline()) - otherwise a stale nonzero liveWeight from
    // the just-finished load could sit in Firebase for up to 5 more
    // seconds after the platform is already genuinely empty again.
    //
    // updateLiveWeight() also stamps a lastSeen timestamp alongside the
    // weight (see FirebaseManager.h/.cpp) - this library has no RTDB
    // onDisconnect() support, so there's no way for the SERVER to notice
    // this device going offline and clear liveWeight itself; a consumer
    // has to infer staleness from how old lastSeen is instead.
    //

    if (firebase.isReady() &&
        !loadActive &&
        millis() - lastLiveUpdateTime >= 5000)
    {
        lastLiveUpdateTime = millis();
        firebase.updateLiveWeight(reportedGrams);
    }

    if (filteredWeightGrams >= UPLOAD_MIN_GRAMS && !capturedThisLoad && !blockedByOtherSessionPending)
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

                Serial.print("[PERF] Stable confirmed: +");
                Serial.print(millis() - perfLoadDetectedTime);
                Serial.println("ms");

                // capturedAt: the physical measurement's own timestamp,
                // set the moment it's confirmed - epoch seconds if the
                // clock is synced, 0 if it genuinely isn't (never a faked
                // absolute time). Same NTP-sync sanity check FirebaseManager
                // itself uses.
                uint32_t capturedAtEpoch =
                    (time(nullptr) > 8 * 3600 * 2) ? (uint32_t)time(nullptr) : 0;

                // A non-empty currentSessionMeasurementId means RESTACK
                // DETECTION re-opened an ALREADY-captured session (weight
                // added/removed without a full clear) rather than this
                // being the session's first-ever capture - reuse that same
                // id (restagePendingMeasurement) instead of minting a new
                // one, so this total overwrites the SAME RTDB record rather
                // than creating a second, competing one. See RESTACK
                // DETECTION above and FirebaseManager.h's class comment.
                bool isRestack = !currentSessionMeasurementId.isEmpty();

                // CAPTURED the instant this returns true - persisted to
                // flash already, regardless of what happens next. This is
                // the whole point of separating capture from upload: no
                // network state can make a confirmed physical measurement
                // disappear, whether Firebase is unreachable right now or
                // fails mid-write a moment later.
                bool staged = isRestack
                    ? firebase.restagePendingMeasurement(reportedGrams, capturedAtEpoch, currentSessionMeasurementId)
                    : firebase.stagePendingMeasurement(reportedGrams, capturedAtEpoch);

                if (staged)
                {
                    capturedThisLoad        = true;
                    isStable                = false;
                    lastCapturedWeightGrams = reportedGrams;

                    String weightLine     = String(displayGrams(displayWeightGrams)) + " g";
                    String measurementId  = firebase.getPendingMeasurementId(); // captured now - syncPendingMeasurement() below clears this on success
                    currentSessionMeasurementId = measurementId; // first capture: assigns it fresh. restack: already equal, no-op.

                    if (isRestack)
                    {
                        Serial.print("[SCALE] Restacked - new total: ");
                        Serial.print(reportedGrams, 1);
                        Serial.println(" g");
                    }

                    // Best-effort IMMEDIATE sync - most of the time Firebase
                    // is already ready and this is instant, so the common
                    // case still shows "SAVED" right away rather than
                    // "PENDING" for even one cycle. If Firebase isn't ready
                    // (or this one write happens to fail), the pending
                    // record stays on disk exactly as staged, and the
                    // PENDING SYNC RETRY block above picks it up
                    // automatically on a later cycle - no separate retry
                    // path needed here.
                    bool willAttemptUpload = network.isConnected() && firebase.isReady();
                    if (willAttemptUpload) { showIfChanged("Saving...", weightLine); }

                    unsigned long uploadStart = millis();
                    bool wasUploaded = willAttemptUpload && firebase.syncPendingMeasurement();
                    if (willAttemptUpload)
                    {
                        Serial.print("[PERF] Firebase upload: ");
                        Serial.print(millis() - uploadStart);
                        Serial.println("ms");
                    }

                    if (wasUploaded)
                    {
                        Serial.print("[SCALE] Firebase measurement saved: ");
                        Serial.println(measurementId);
                        showIfChanged("SAVED", weightLine);
                    }
                    else
                    {
                        Serial.print("[SCALE] Measurement captured and pending sync: ");
                        Serial.print(reportedGrams, 1);
                        Serial.print(" g | id: ");
                        Serial.println(measurementId);
                        showIfChanged("PENDING", weightLine);
                    }
                    delay(1500);
                }
                else
                {
                    // Local persistence itself failed - the physical
                    // measurement is explicitly NOT considered captured
                    // (capturedThisLoad stays false), so the next
                    // qualifying cycle retries rather than the reading
                    // being silently discarded. On a restack specifically,
                    // this can also discard an earlier good pending total
                    // for this same session - see
                    // restagePendingMeasurement()'s own comment.
                    Serial.println(isRestack
                        ? "[SCALE] ERROR: Unable to persist restacked measurement locally."
                        : "[SCALE] ERROR: Unable to persist measurement locally.");
                    showIfChanged("Save failed!", "Retrying...");
                    delay(1000);
                }
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