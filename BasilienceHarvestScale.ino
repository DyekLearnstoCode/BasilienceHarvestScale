#include "LoadCellManager.h"
#include "DisplayManager.h"
#include "NetworkManager.h"
#include "FirebaseManager.h"

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
// WIFI CREDENTIALS
// ------------------------------------------------------------
//
// Replace with your actual WiFi network details.
//

const char* WIFI_SSID     = "PochixFloydie";
const char* WIFI_PASSWORD = "REDACTED-ROTATE-THIS-CREDENTIAL";

// ------------------------------------------------------------
// FIREBASE CREDENTIALS
// ------------------------------------------------------------
//
// API Key:      Firebase Console > Project Settings > General
// Database URL: Firebase Console > Realtime Database
// DB Secret:    Project Settings > Service Accounts >
//               Database Secrets
//

const char* FB_API_KEY      = "AIzaSyDaJ7F8tAREnCo7zrrY_sJ6SgfNuYQtra0";
const char* FB_DATABASE_URL = "https://basilience-database-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FB_DB_SECRET    = "REDACTED-ROTATE-THIS-CREDENTIAL";
// ------------------------------------------------------------
// CALIBRATION
// ------------------------------------------------------------
//
// Previous factor: 128.17
// Refined factor:  124.64
// (Known 196 g → measured 190.6 g)
//

constexpr float CALIBRATION_FACTOR = 124.64f;

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

NetworkManager network(
    WIFI_SSID,
    WIFI_PASSWORD
);

FirebaseManager firebase(
    FB_API_KEY,
    FB_DATABASE_URL,
    FB_DB_SECRET
);

// ------------------------------------------------------------
// STATE
// ------------------------------------------------------------

unsigned long lastReadingTime    = 0;
unsigned long lastLiveUpdateTime = 0;   // Throttle Firebase /LoadCell updates

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
        Serial.println("[SCALE] ERROR: HX711 not detected.");
        display.showError("HX711 ERROR!", "Check wiring");
        return;
    }

    Serial.println("[SCALE] HX711 detected.");
    loadCell.setCalibrationFactor(CALIBRATION_FACTOR);

    // --------------------------------------------------------
    // WIFI
    // --------------------------------------------------------

    display.showError("Connecting WiFi", "Please wait...");

    Serial.println();
    Serial.println("====================================");
    Serial.print("[WIFI] SSID: ");
    Serial.println(WIFI_SSID);
    Serial.println("====================================");

    bool wifiOk = network.connect();

    if (wifiOk)
    {
        display.showError("WiFi Connected!", network.getIPAddress());
        delay(1500);
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
        Serial.println("[SCALE] ERROR: Unable to tare.");
        display.showError("Tare failed!", "Restart scale");
        return;
    }

    Serial.println("[SCALE] Tare complete.");
    Serial.print("[SCALE] Tare offset: ");
    Serial.println(loadCell.getOffset());

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
    // READING INTERVAL
    // --------------------------------------------------------

    if (millis() - lastReadingTime < READING_INTERVAL_MS) { return; }
    lastReadingTime = millis();

    // --------------------------------------------------------
    // WIFI KEEPALIVE
    // --------------------------------------------------------

    network.reconnectIfNeeded();

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

    display.showWeight(weightGrams, weightKg);

    // --------------------------------------------------------
    // LIVE WEIGHT → RTDB /LoadCell  (every 5 seconds)
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