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
// Previous factor: 128.17
// Refined factor:  124.64
// (Known 196 g → measured 190.6 g)
//

constexpr float CALIBRATION_FACTOR = 124.64f;

// The empty weighing platform/tray itself weighs ~50 g and is not zeroed
// out by the boot-time tare, so every raw reading carries that fixed
// offset on top of whatever is actually placed on it. Subtracted in
// loop() right after each read so downstream logic (deadband, overload
// check, stability, upload) all sees only the item's actual weight.
constexpr float PLATFORM_TARE_GRAMS = 50.0f;

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
// DISPLAY SMOOTHING
// ------------------------------------------------------------
//
// get_units(READING_SAMPLES) already averages within one reading, but
// nothing smooths ACROSS the 500ms reading cycles - every loop() computed
// a fresh, independent average and printed it immediately, so ordinary
// HX711 noise (a few grams, normal for a DIY load cell without extra
// shielding) showed up on the LCD as constant decimal jitter with nothing
// ever looking "settled," even though the underlying upload-stability
// logic below was working correctly the whole time on the same raw
// readings. This is a simple exponential moving average applied ONLY to
// what's shown on screen - the upload-stability check further below
// deliberately keeps using the raw, unsmoothed weightGrams, since that
// decision needs to see genuine reading-to-reading agreement, not a
// filtered value that could mask real instability.
//
// 0.25 reaches ~90% of a real step change (an item actually placed/
// removed) within about 4 update cycles (~2 seconds) while still damping
// single-cycle noise significantly - responsive enough to feel live,
// smooth enough to stop the flicker.
//
constexpr float DISPLAY_SMOOTHING_ALPHA = 0.25f;

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

unsigned long lastReadingTime    = 0;
unsigned long lastLiveUpdateTime = 0;   // Throttle Firebase liveWeight updates

// Display-only smoothed weight (see DISPLAY_SMOOTHING_ALPHA) - kept
// entirely separate from the raw weightGrams the stability/upload logic
// below evaluates every cycle.
float displayWeightGrams = 0.0f;

// Stability tracking
float         stableReadings[STABLE_READINGS];
uint8_t       stableIndex      = 0;
bool          bufferFull       = false;
unsigned long stableStartTime  = 0;
bool          isStable         = false;
bool          uploadedThisLoad = false;   // Prevent duplicate uploads

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

    // Snaps the displayed weight to match immediately rather than letting
    // it EMA-decay back down over several cycles - a real tare or "load
    // removed" event should read 0 g right away, not drift toward it.
    displayWeightGrams = value;

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
    // WARM-UP (10 seconds)
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
    // PLATFORM TARE
    // --------------------------------------------------------

    weightGrams -= PLATFORM_TARE_GRAMS;

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
    // RESET UPLOAD FLAG WHEN SCALE IS EMPTY AGAIN
    // --------------------------------------------------------

    if (weightGrams < UPLOAD_MIN_GRAMS)
    {
        if (uploadedThisLoad)
        {
            Serial.println("[SCALE] Load removed. Ready for next.");
            uploadedThisLoad = false;
            resetStabilityBuffer(0.0f);
        }
    }

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
    // Smoothed across cycles (see DISPLAY_SMOOTHING_ALPHA) so the number on
    // screen settles instead of flickering with ordinary HX711 noise - the
    // upload-stability logic below still evaluates the raw weightGrams
    // directly, unaffected by this.
    //

    displayWeightGrams += (weightGrams - displayWeightGrams) * DISPLAY_SMOOTHING_ALPHA;
    display.showWeight(displayWeightGrams, displayWeightGrams / 1000.0f);

    // --------------------------------------------------------
    // LIVE WEIGHT → RTDB devices/{deviceId}/harvestScale/liveWeight  (every 5 seconds)
    // --------------------------------------------------------
    //
    // Throttled — Firebase SSL calls are slow on ESP8266.
    // Calling every 500ms would flood the board and cause crashes.
    //

    if (firebase.isReady() &&
        millis() - lastLiveUpdateTime >= 5000)
    {
        lastLiveUpdateTime = millis();
        firebase.updateLiveWeight(weightGrams);
    }


    if (weightGrams >= UPLOAD_MIN_GRAMS && !uploadedThisLoad)
    {
        bool nowStable = checkStability(weightGrams);

        if (nowStable)
        {
            // Start stability timer on first stable detection
            if (!isStable)
            {
                isStable        = true;
                stableStartTime = millis();
                Serial.println("[SCALE] Weight stable — waiting to confirm...");
            }

            // Upload after stable hold time
            if (millis() - stableStartTime >= STABLE_HOLD_MS)
            {
                Serial.println("[SCALE] Stable confirmed. Uploading...");

                display.showError("Uploading...", String(weightGrams, 1) + " g");

                if (firebase.isReady())
                {
                    bool uploaded =
                        firebase.uploadWeight(weightGrams, weightKg);

                    if (uploaded)
                    {
                        uploadedThisLoad = true;
                        isStable         = false;

                        Serial.println("[SCALE] Upload SUCCESS.");
                        display.showError("Uploaded! OK", String(weightGrams, 1) + " g");
                        delay(1500);
                    }
                    else
                    {
                        Serial.println("[SCALE] Upload FAILED.");
                        display.showError("Upload failed!", "Check WiFi/FB");
                        delay(1500);
                    }
                }
                else
                {
                    Serial.println("[SCALE] Firebase not ready — skipping upload.");
                    uploadedThisLoad = true;   // Skip, don't retry forever
                    isStable         = false;
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