#include "FirebaseManager.h"

// ============================================================
// CONSTRUCTOR
// ============================================================

FirebaseManager::FirebaseManager(
    const char* apiKey,
    const char* databaseURL,
    const char* dbSecret
)
    : _apiKey(apiKey),
      _databaseURL(databaseURL),
      _dbSecret(dbSecret),
      _ready(false),
      _readingCount(0)
{
}

// ============================================================
// INITIALIZATION
// ============================================================

bool FirebaseManager::begin()
{
    Serial.println();
    Serial.println("====================================");
    Serial.println(" FIREBASE INIT");
    Serial.println("====================================");

    _config.api_key      = _apiKey;
    _config.database_url = _databaseURL;

    // Legacy database secret — simple IoT auth, no user login needed.
    _config.signer.tokens.legacy_token = _dbSecret;

    Firebase.begin(&_config, &_auth);
    Firebase.reconnectWiFi(true);

    Serial.print("[FB] Connecting");

    uint8_t retries = 0;

    while (!Firebase.ready() && retries < 30)
    {
        Serial.print(".");
        delay(500);
        retries++;
        yield();
    }

    Serial.println();

    if (!Firebase.ready())
    {
        Serial.println("[FB] ERROR: Firebase not ready.");
        Serial.println("[FB] Check API key and database URL.");
        _ready = false;
        return false;
    }

    _ready = true;

    Serial.println("[FB] Firebase connected!");
    Serial.print("[FB] Database: ");
    Serial.println(_databaseURL);
    Serial.println("====================================");

    return true;
}

bool FirebaseManager::isReady() const
{
    return _ready && Firebase.ready();
}

// ============================================================
// LIVE WEIGHT — updates /LoadCell every loop
// ============================================================
//
// Uses setFloat() — overwrites the same node every call.
// No new entries created, no quota used per reading.
//

bool FirebaseManager::updateLiveWeight(float grams)
{
    if (!isReady()) { return false; }

    if (Firebase.RTDB.setFloat(&_fbData, "/LoadCell", grams))
    {
        return true;
    }
    else
    {
        Serial.print("[FB] LiveWeight error: ");
        Serial.println(_fbData.errorReason());
        return false;
    }
}

// ============================================================
// HARVEST LOG — pushes to /harvests/ on stable reading only
// ============================================================
//
// Uses pushJSON() — creates a new auto-ID entry each call.
// Only called once per stable weighing event.
//

bool FirebaseManager::uploadWeight(float grams, float kg)
{
    if (!isReady())
    {
        Serial.println("[FB] ERROR: Not ready.");
        return false;
    }

    FirebaseJson json;

    json.set("grams",  grams);
    json.set("kg",     kg);
    json.set("millis", (int)millis());

    if (Firebase.RTDB.pushJSON(&_fbData, "/harvests", &json))
    {
        _readingCount++;

        Serial.print("[FB] Harvest logged: ");
        Serial.print(grams, 1);
        Serial.print(" g | Key: ");
        Serial.println(_fbData.pushName());

        return true;
    }
    else
    {
        Serial.print("[FB] Upload failed: ");
        Serial.println(_fbData.errorReason());
        return false;
    }
}

// ============================================================
// STATS
// ============================================================

int FirebaseManager::getTotalReadings()
{
    return (int)_readingCount;
}