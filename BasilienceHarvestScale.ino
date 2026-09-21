#include <time.h>

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
constexpr uint8_t  READING_SAMPLES     = 10;
constexpr unsigned long HX711_TIMEOUT_MS   = 1500;
constexpr unsigned long READING_INTERVAL_MS = 500;

// ------------------------------------------------------------
// WARM-UP
// ------------------------------------------------------------
//
// Lowered from 60s - that was a conservative round-number default, not a
// value actually tuned to this specific load cell/HX711 pairing. 30s is
// still a deliberate, cautious choice (this is a one-time boot-time cost,
// paid every power-on) while cutting the wait roughly in half. If harvest
// weighings taken shortly after boot ever look drifted compared to ones
// taken later in the same session, that's a sign this needs to go back up
// rather than lower still.
//
constexpr unsigned long WARMUP_TIME_MS = 30000;

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
constexpr uint8_t      STABLE_READINGS        = 6;
constexpr unsigned long STABLE_HOLD_MS        = 3000;

// ------------------------------------------------------------
// FIREBASE RECONNECT
// ------------------------------------------------------------
//
// firebase.begin() used to run exactly once, in setup() - if WiFi wasn't
// up yet at that point, or the auth handshake failed transiently, Firebase
// stayed permanently unready for the rest of that boot with no way to
// recover short of a power cycle. Retried from loop() now instead (see the
// FIREBASE RECONNECT / PENDING SYNC RETRY block below), cooldown-gated so
// this never re-runs the multi-second NTP+auth sequence on every 500ms
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
// reportedGrams directly at display time) so the "Pending Sync"/"Saved:"
// messages can show the exact same number the weighing screen last
// displayed, and so it still holds that value while frozen.
float displayWeightGrams = 0.0f;

// Stability tracking
float         stableReadings[STABLE_READINGS];
uint8_t       stableIndex      = 0;
bool          bufferFull       = false;
unsigned long stableStartTime  = 0;
bool          isStable         = false;

// Whether the CURRENT physical load has already produced a confirmed
// measurement - NOT whether it reached Firebase. A measurement is
// "captured" the instant it's durably staged to flash (see
// FirebaseManager::stagePendingWeight()); syncing to RTDB can happen
// later, or on a retry, without this ever needing to be re-evaluated.
// Renamed from the old uploadedThisLoad, which conflated "captured" with
// "upload succeeded" - the actual cause of measurements silently
// disappearing whenever Firebase happened to be unavailable at the exact
// moment a weighing stabilized (see the removed "Firebase not ready -
// skipping upload" branch this replaces).
bool  capturedThisLoad = false;

// Whether ANY load ≥ UPLOAD_MIN_GRAMS is currently on the platform,
// regardless of capturedThisLoad - tracks the physical weighing SESSION
// itself, separately from whether it happened to already produce a
// confirmed measurement. Used purely to detect the active→empty
// transition exactly once (see the RESET block in loop()), so stability
// state is always reset when the platform empties - even if this load
// was removed mid-sample or mid-hold and never actually got captured.
bool  loadActive = false;

float lastCapturedWeightGrams = 0.0f;   // What the last confirmed measurement (this load) was for - restack detection

// ============================================================
// HELPERS
// ============================================================

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
    // Deliberately BEFORE WiFi - a confirmed physical measurement must be
    // persistable even on a unit that never reaches a network at all.
    // Recovers any pending measurement left over from a previous boot
    // (power/WiFi lost before it could sync) so it survives an ESP restart.
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
    // WARM-UP (30 seconds, WARMUP_TIME_MS)
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
    // READ WEIGHT
    // --------------------------------------------------------

    float weightGrams = 0.0f;

    if (!loadCell.readWeightGrams(READING_SAMPLES, weightGrams, HX711_TIMEOUT_MS))
    {
        Serial.println("[SCALE] ERROR: HX711 read timeout.");
        display.showError("Read timeout!", "Check HX711");
        return;
    }

    // --------------------------------------------------------
    // ZERO DEADBAND
    // --------------------------------------------------------

    if (weightGrams >= -ZERO_DEADBAND_GRAMS && weightGrams <= ZERO_DEADBAND_GRAMS)
    {
        weightGrams = 0.0f;
    }

    // --------------------------------------------------------
    // NEGATIVE WEIGHT HANDLING
    // --------------------------------------------------------

    if (weightGrams < 0.0f && weightGrams > -20.0f)
    {
        weightGrams = 0.0f;
    }

    // --------------------------------------------------------
    // SANITY VALIDATION
    // --------------------------------------------------------

    if (weightGrams > MAX_WEIGHT_GRAMS)
    {
        Serial.println("[SCALE] ERROR: Weight exceeds 20 kg.");
        display.showError("OVERLOAD!", "Max: 20 kg");
        return;
    }

    if (weightGrams < -1000.0f)
    {
        Serial.println("[SCALE] ERROR: Invalid negative reading.");
        display.showError("Bad reading!", "");
        return;
    }

    // --------------------------------------------------------
    // RESET PHYSICAL WEIGHING SESSION WHEN SCALE IS EMPTY AGAIN
    // --------------------------------------------------------
    //
    // Fires on the active→empty TRANSITION (loadActive was true), not on
    // capturedThisLoad specifically - a load pulled off mid-sample or
    // mid-hold, before ever producing a confirmed measurement, must reset
    // exactly the same as one that was fully captured. The old check here
    // (`if (uploadedThisLoad)`) left the stability buffer, isStable, and
    // stableStartTime all still populated from the removed load whenever
    // nothing had been uploaded yet - a similar weight placed back down
    // could inherit stale samples instead of starting completely fresh.
    //
    // Deliberately does NOT touch anything in `firebase` - a pending,
    // not-yet-synced measurement is a SEPARATE concept from this local
    // physical-load state and must survive the platform being emptied.
    //

    if (weightGrams < UPLOAD_MIN_GRAMS)
    {
        if (loadActive)
        {
            Serial.println("[SCALE] Platform empty. Weighing session reset.");
            resetStabilityBuffer(0.0f);
            capturedThisLoad = false;
            loadActive       = false;
        }
    }

    // --------------------------------------------------------
    // RESTACK DETECTION (weight changed without a full clear)
    // --------------------------------------------------------
    //
    // The reset above only fires once the platform empties - so weight
    // added on TOP of an already-confirmed load, or partially taken back
    // off without dropping below UPLOAD_MIN_GRAMS, would otherwise never
    // re-enter the stability/capture logic below, and the new total would
    // never get logged or reach liveWeight (frozen once locked, below).
    // Meaningfully different from what was last confirmed in EITHER
    // direction (same threshold the stability check itself uses for "is
    // this actually different, or just noise") is treated the same as a
    // fresh load: re-open tracking for the NEW total rather than
    // requiring a full clear-and-reload.
    //
    // Gated on !firebase.hasPendingWeight(): if the last confirmed
    // measurement hasn't synced yet, the previous physical measurement is
    // still "in flight" from the platform's point of view - restack must
    // not reopen tracking for a second measurement that could contend
    // with it. Resumes on its own the moment the pending one clears.
    //

    if (capturedThisLoad && !firebase.hasPendingWeight() &&
        fabsf(weightGrams - lastCapturedWeightGrams) > STABLE_THRESHOLD_GRAMS)
    {
        Serial.println("[SCALE] Weight changed on the platform - re-evaluating.");
        capturedThisLoad = false;
        restartStabilityTracking();
    }

    // --------------------------------------------------------
    // FIREBASE RECONNECT / PENDING SYNC RETRY
    // --------------------------------------------------------
    //
    // firebase.begin() only ever ran once, in setup() - retried here
    // instead whenever WiFi is up but Firebase itself isn't ready yet,
    // cooldown-gated to FIREBASE_RETRY_INTERVAL_MS so this never re-runs
    // the multi-second NTP+auth sequence every 500ms cycle. Runs before
    // STABILITY TRACKING below so a sync that completes THIS cycle can
    // already unblock capture on this same cycle instead of lagging one
    // behind.
    //
    // ALSO gated on the platform being empty this cycle (weightGrams below
    // UPLOAD_MIN_GRAMS): firebase.begin() performs NTP sync + a full TLS
    // auth handshake and can legitimately block for several seconds to
    // over ten. Running it while something is actively resting on the
    // platform would starve HX711 sampling for that entire span - the
    // stability buffer keeps advancing on wall-clock time (STABLE_HOLD_MS)
    // without ever actually observing the object holding still, so the
    // very next sample after the block could satisfy the hold timer
    // immediately even though nothing was genuinely watched settle. A
    // pending, already-persisted measurement is never blocked by this -
    // syncPendingWeight() just below is a normal, fast RTDB write that
    // only ever runs once Firebase is ALREADY ready, so it's left
    // ungated.
    //

    bool platformEmpty = weightGrams < UPLOAD_MIN_GRAMS;

    if (platformEmpty && network.isConnected() && !firebase.isReady() &&
        millis() - lastFirebaseRetryTime >= FIREBASE_RETRY_INTERVAL_MS)
    {
        lastFirebaseRetryTime = millis();
        Serial.println("[FB] Retrying Firebase initialization (platform empty)...");
        firebase.begin();
    }

    if (firebase.isReady() && firebase.hasPendingWeight())
    {
        firebase.syncPendingWeight();
    }

    // --------------------------------------------------------
    // STABILITY TRACKING
    // --------------------------------------------------------
    //
    // Pushed here, before display/live-weight/capture below all read the
    // buffer, so the averaged value they use already includes this cycle's
    // own sample rather than lagging a cycle behind.
    //
    // Gated on !pendingBlocksCapture (see below): while an earlier
    // measurement is still unsynced, a NEW load must not accumulate
    // stability samples at all - this is the one-slot design (Part H):
    // exactly one unsynced measurement at a time, correctness over an
    // offline queue. Tracking simply resumes, from a clean buffer, the
    // moment the pending measurement clears.

    bool nowStable = false;
    bool pendingBlocksCapture = firebase.hasPendingWeight();

    if (weightGrams >= UPLOAD_MIN_GRAMS)
    {
        loadActive = true;

        if (!capturedThisLoad && !pendingBlocksCapture)
        {
            nowStable = checkStability(weightGrams);
        }
    }

    // What actually gets shown/sent: once real samples fill the stability
    // buffer, the settled average - see stabilityAverageGrams(). Before
    // that (nothing on the platform yet, or an item only just placed),
    // falls back to this cycle's raw reading, since there's no real
    // buffer yet to average.
    float reportedGrams = bufferFull ? stabilityAverageGrams() : weightGrams;

    // --------------------------------------------------------
    // CONVERT
    // --------------------------------------------------------

    float weightKg = weightGrams / 1000.0f;

    // --------------------------------------------------------
    // SERIAL OUTPUT
    // --------------------------------------------------------

    Serial.print("[SCALE] ");
    Serial.print(weightGrams, 1);
    Serial.print(" g | ");
    Serial.print(weightKg, 3);
    Serial.println(" kg");

    // --------------------------------------------------------
    // LCD OUTPUT
    // --------------------------------------------------------
    //
    // Four distinct states, checked in this order:
    //   1. A measurement is pending sync (firebase.hasPendingWeight()):
    //        - the exact load that produced it is STILL physically on the
    //          platform (weightGrams >= UPLOAD_MIN_GRAMS && capturedThisLoad)
    //          -> show its confirmed value, "Pending Sync / {g} g" - the
    //          persisted pending grams, not an unstable raw reading.
    //        - otherwise - platform is empty (that load was removed), or a
    //          DIFFERENT new load is present and blocked from capture
    //          entirely while the one pending slot is occupied (see
    //          STABILITY TRACKING above) -> show "Pending Sync / Check
    //          WiFi". Showing the stored grams here would misleadingly
    //          suggest the scale still physically reads that weight (empty
    //          case) or that it's weighing this new item (blocked case).
    //          The pending record on flash is completely untouched either
    //          way - this is display-only; nothing here clears or alters
    //          it.
    //   2. Nothing pending, platform empty (weightGrams < UPLOAD_MIN_GRAMS)
    //      -> "Ready" screen. Also what's reached once a pending sync
    //      finally succeeds while the platform is empty (hasPendingWeight()
    //      goes false, falls straight through to here next cycle) and once
    //      a captured-and-saved load is removed (see state 4 below).
    //   3. Not yet captured, something on the platform (!capturedThisLoad,
    //      nothing pending, weightGrams >= UPLOAD_MIN_GRAMS) -> normal live
    //      weighing screen, reportedGrams as before - unchanged.
    //   4. Captured and fully synced (capturedThisLoad, nothing pending) ->
    //      falls through untouched, leaving the "Saved: ..." message the
    //      capture block below already wrote on-screen. Restack detection
    //      above re-opens state 3 the moment the weight genuinely changes;
    //      removing the load re-enters state 2 above.
    //

    if (pendingBlocksCapture)
    {
        if (weightGrams >= UPLOAD_MIN_GRAMS && capturedThisLoad)
        {
            display.showError("Pending Sync", String(firebase.getPendingGrams(), 1) + " g");
        }
        else
        {
            display.showError("Pending Sync", "Check WiFi");
        }
    }
    else if (platformEmpty)
    {
        display.showError("Ready", "Place harvest");
    }
    else if (!capturedThisLoad)
    {
        displayWeightGrams = reportedGrams;
        display.showWeight(displayWeightGrams, displayWeightGrams / 1000.0f);
    }

    // --------------------------------------------------------
    // LIVE WEIGHT → RTDB devices/{deviceId}/harvestScale/liveWeight  (every 5 seconds)
    // --------------------------------------------------------
    //
    // Throttled — Firebase SSL calls are slow on ESP8266.
    // Calling every 500ms would flood the board and cause crashes.
    //
    // Frozen at the confirmed value once a load is locked in
    // (capturedThisLoad) or while a pending measurement is blocking a new
    // one, instead of continuing to overwrite it with ordinary raw-reading
    // noise - the harvests/ entry is the source of truth once logged, so a
    // live number that keeps wiggling next to an already-confirmed (or
    // still-syncing) one reads as contradictory. Restack detection above
    // re-opens this the instant enough extra weight is added to be a real
    // new total rather than noise.
    //

    if (firebase.isReady() &&
        !capturedThisLoad &&
        !pendingBlocksCapture &&
        millis() - lastLiveUpdateTime >= 5000)
    {
        lastLiveUpdateTime = millis();
        firebase.updateLiveWeight(reportedGrams);
    }

    if (weightGrams >= UPLOAD_MIN_GRAMS && !capturedThisLoad && !pendingBlocksCapture)
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
                Serial.println("[SCALE] Stable confirmed. Capturing measurement...");

                // capturedAt: the physical measurement's own timestamp,
                // set the moment it's confirmed - epoch seconds if the
                // clock is synced, 0 if it genuinely isn't (never a faked
                // absolute time). Same NTP-sync sanity check FirebaseManager
                // itself uses.
                uint32_t capturedAtEpoch =
                    (time(nullptr) > 8 * 3600 * 2) ? (uint32_t)time(nullptr) : 0;

                if (firebase.stagePendingWeight(reportedGrams, capturedAtEpoch))
                {
                    // CAPTURED the instant this returns true - persisted to
                    // flash already, regardless of what happens next. This
                    // is the fix for the old "Firebase not ready - skipping
                    // upload" branch: network state can no longer make a
                    // confirmed physical measurement disappear.
                    capturedThisLoad        = true;
                    isStable                = false;
                    lastCapturedWeightGrams = reportedGrams;

                    Serial.println("[SCALE] Measurement captured and persisted locally.");

                    // Best-effort immediate sync - most of the time
                    // Firebase is already up and this is instant. If not,
                    // the pending record stays on disk exactly as staged,
                    // and the FIREBASE RECONNECT / PENDING SYNC RETRY block
                    // above picks it up automatically on a later cycle -
                    // no separate retry path needed.
                    if (firebase.isReady() && firebase.syncPendingWeight())
                    {
                        display.showError("Saved:", String(displayWeightGrams, 1) + " g");
                    }
                    else
                    {
                        display.showError("Pending Sync", String(displayWeightGrams, 1) + " g");
                    }
                    delay(1500);
                }
                else
                {
                    // Local persistence itself failed - the physical
                    // measurement is explicitly NOT considered captured
                    // (capturedThisLoad stays false), so the next
                    // qualifying cycle retries rather than the reading
                    // being silently discarded.
                    Serial.println("[SCALE] ERROR: Unable to persist measurement locally.");
                    display.showError("Save failed!", "Retrying...");
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