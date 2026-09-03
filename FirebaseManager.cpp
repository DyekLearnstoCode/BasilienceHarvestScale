#include "FirebaseManager.h"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <time.h>

namespace
{

// Cloud Function HTTPS endpoint that verifies a device's bootstrap secret
// and mints a Firebase custom token (uid = deviceId). Same deployed
// function the ESP32 firmware (BasilienceFirmware_V2/Config.h) already
// calls — one shared identity backend for every physical unit.
const char* BOOTSTRAP_ENDPOINT_URL =
    "https://asia-southeast1-basilience-database.cloudfunctions.net/deviceAuthBootstrap";

// Google Trust Services GTS Root R1 — fetched directly from Google's own
// published trust store (https://pki.goog/repo/certs/gtsr1.pem), the exact
// same root pinned in the ESP32 firmware's Config.h for this same
// endpoint. Long-lived root (valid to 2036); reconfirm against pki.goog
// only if the bootstrap call ever fails TLS validation unexpectedly.
const char BOOTSTRAP_CA_CERT[] PROGMEM = R"CERT(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
)CERT";

const char* SECRET_FILE    = "/device_secret.txt";
const char* REFRESH_FILE   = "/refresh_token.txt";
const char* DEVICE_ID_FILE = "/device_id.txt";

String readTextFile(const char* path)
{
    if (!LittleFS.exists(path)) { return ""; }

    File f = LittleFS.open(path, "r");
    if (!f) { return ""; }

    String value = f.readString();
    f.close();
    value.trim();
    return value;
}

void writeTextFile(const char* path, const String& value)
{
    File f = LittleFS.open(path, "w");
    if (!f)
    {
        Serial.println("[SECURITY] Unable to persist credential file");
        return;
    }
    f.print(value);
    f.close();
}

} // namespace

// ============================================================
// CONSTRUCTOR
// ============================================================

FirebaseManager::FirebaseManager(
    const char* apiKey,
    const char* databaseURL,
    const char* bootstrapDeviceSecret
)
    : _apiKey(apiKey),
      _databaseURL(databaseURL),
      _bootstrapDeviceSecret(bootstrapDeviceSecret),
      _ready(false),
      _readingCount(0)
{
}

String FirebaseManager::deviceRoot() const
{
    return "/devices/" + _deviceId + "/harvestScale";
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

    if (!LittleFS.begin())
    {
        Serial.println("[SECURITY] LittleFS mount failed - device identity cannot persist across reboots");
    }

    _config.api_key      = _apiKey;
    _config.database_url = _databaseURL;

    // The library's own internal TLS client (used by Firebase.begin() to
    // exchange the custom token for a real session - a SEPARATE
    // connection from the hand-rolled bootstrap HTTPS client in
    // bootstrapSecureAuth(), which already has its own buffers shrunk)
    // defaults to much larger BearSSL buffers than this board can spare
    // once WiFi + LittleFS are already resident. Confirmed root cause of
    // "Unhandled C++ exception: OOM" / "last failed alloc call:
    // ...(1496)" happening right after a successful bootstrap, during
    // Firebase.begin()'s own token exchange. 2048/512 matches the
    // library's own FireSense addon's documented reduced setting for
    // exactly this situation.
    _fbData.setBSSLBufferSize(2048, 512);

    loadDeviceId();

    Serial.print("[FB] Connecting");

    // BearSSL validates the server certificate's NotBefore/NotAfter dates
    // against the device's current system time - and this ESP8266 has no
    // hardware RTC (unlike the ESP32 firmware's RTCManager, which pairs a
    // physical RTC with NTP as a fallback), so every boot starts at the
    // Unix epoch (Jan 1 1970) until something syncs it. Without a valid
    // clock, Google's certificate looks "not yet valid" and BearSSL
    // aborts the TLS handshake immediately - confirmed root cause of the
    // bootstrap call failing with HTTPClient error -1
    // (CONNECTION_REFUSED) roughly 200ms after the request starts, far
    // too fast to be a genuine network failure.
    Serial.println();
    Serial.println("[FB] Syncing time via NTP (required for TLS certificate validation)...");
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    unsigned long ntpStartedAt = millis();
    while (time(nullptr) < 8 * 3600 * 2 && millis() - ntpStartedAt < 15000UL)
    {
        delay(200);
        yield();
    }

    if (time(nullptr) < 8 * 3600 * 2)
    {
        Serial.println("[FB] WARNING: NTP sync did not complete within 15s - certificate validation will likely fail");
    }
    else
    {
        Serial.print("[FB] Time synced: ");
        Serial.println((unsigned long)time(nullptr));
    }

    bool authenticated = trySecureAuthentication();

    if (!authenticated)
    {
        Serial.println();
        Serial.println("[FB] ERROR: Secure device authentication failed.");
        Serial.println("[FB] Check the device secret and bootstrap endpoint.");
        _ready = false;
        return false;
    }

    Firebase.reconnectWiFi(true);

    _ready = true;

    Serial.println();
    Serial.println("[FB] Firebase connected!");
    Serial.print("[FB] Database: ");
    Serial.println(_databaseURL);
    Serial.print("[FB] Device identity: ");
    Serial.println(_deviceId);
    Serial.println("====================================");

    return true;
}

bool FirebaseManager::isReady() const
{
    return _ready && Firebase.ready();
}

// ============================================================
// SECURE DEVICE AUTH
// ============================================================

bool FirebaseManager::trySecureAuthentication()
{
    String secret;
    String refreshToken;
    loadDeviceAuthCredentials(secret, refreshToken);

    if (refreshToken.length() > 0)
    {
        Serial.println();
        Serial.println("[SECURITY] Stored refresh token found");
        if (restoreFromRefreshToken(refreshToken))
        {
            Serial.println("[SECURITY] Refresh-token authentication succeeded");
            return true;
        }
        Serial.println("[SECURITY] Refresh-token authentication failed");
    }

    if (secret.length() > 0)
    {
        Serial.println("[SECURITY] Attempting bootstrap with device secret");
        if (bootstrapSecureAuth(secret))
        {
            return true;
        }
        Serial.println("[FB] Bootstrap failed");
    }
    else
    {
        Serial.println("[FB] ERROR: No device secret provisioned - cannot bootstrap identity");
    }

    return false;
}

bool FirebaseManager::restoreFromRefreshToken(const String& refreshToken)
{
    // A string that is not shaped like a JWT (header.payload.signature) is
    // auto-detected by this library as a bare refresh token and triggers a
    // refresh-grant sign-in directly against Google's securetoken endpoint.
    // No bootstrap call is made on this path.
    Firebase.setCustomToken(&_config, refreshToken);
    Firebase.begin(&_config, &_auth);

    unsigned long startedAt = millis();
    while (!Firebase.ready() && millis() - startedAt < 10000UL)
    {
        delay(100);
        yield();
    }

    if (!Firebase.ready()) { return false; }

    // The refresh-grant response can rotate the refresh token, not just
    // the short-lived ID token. Re-persist so flash always holds whatever
    // token the library is currently using.
    const char* rotatedRefreshToken = Firebase.getRefreshToken();
    if (rotatedRefreshToken != nullptr && strlen(rotatedRefreshToken) > 0
        && refreshToken != rotatedRefreshToken)
    {
        saveRefreshToken(String(rotatedRefreshToken));
        Serial.println("[SECURITY] Refresh token persisted");
    }

    return true;
}

bool FirebaseManager::bootstrapSecureAuth(const String& secret)
{
    String mac = WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF" - matches the
                                     // colon-separated wire format the
                                     // Cloud Function normalizes.

    // Not sensitive (already broadcast over the air to anything in range)
    // - printed so the RTDB /provisioning/{mac}/deviceToken mapping can be
    // verified against the device's own authoritative Station-mode MAC,
    // rather than one obtained some other way (ESP8266 has a SEPARATE
    // SoftAP MAC, one byte different from this Station MAC - easy to
    // mix up if this value was found while the setup portal AP was up).
    Serial.print("[SECURITY] Station MAC (bootstrap identity): ");
    Serial.println(mac);

    // Bounded retry around the connection + request itself, NOT around a
    // real server response - a connection-level failure (httpCode <= 0,
    // e.g. a dropped TLS handshake on a marginal WiFi link) is worth
    // retrying a few times, but a genuine HTTP status from the server
    // (200, 401, 500, 429...) is a deterministic result that retrying
    // cannot change - and retrying a real rejection would only burn
    // through the server's own BOOTSTRAP_FAIL_LIMIT lockout counter for
    // no benefit. Confirmed same-day: this exact request has reached the
    // server and gotten real HTTP responses (401, then 500) on other
    // attempts, so a bare connection failure here is transient network
    // flakiness, not a deterministic bug - each retry builds a fresh
    // client/connection rather than reusing one that may be in a bad
    // state after a failed attempt.
    const uint8_t MAX_CONNECT_ATTEMPTS = 3;
    int httpCode = 0;
    String response;

    for (uint8_t attempt = 1; attempt <= MAX_CONNECT_ATTEMPTS; attempt++)
    {
        BearSSL::WiFiClientSecure secureClient;
        // BearSSL defaults to 16KB receive + 16KB transmit buffers
        // allocated from the heap - roughly 32KB, which is most or all of
        // the ESP8266's free heap once WiFi, the Firebase library,
        // LittleFS, and the setup portal's WebServer/DNSServer are all
        // resident. That reliably drives an out-of-memory abort right at
        // this handshake (silent and immediate, since Config.h's "C++
        // Exceptions: aborts on oom" build setting means a failed `new`
        // crashes on the spot rather than throwing/returning). This
        // device's bootstrap response is a small JSON body (a custom
        // token + deviceId) and the request body is smaller still, so 1KB
        // receive / 512B transmit is generous headroom, not a tight fit -
        // and frees ~30KB of heap for everything else.
        secureClient.setBufferSizes(1024, 512);
        BearSSL::X509List caCert(BOOTSTRAP_CA_CERT);
        secureClient.setTrustAnchors(&caCert);

        HTTPClient http;
        http.setTimeout(15000);
        if (!http.begin(secureClient, BOOTSTRAP_ENDPOINT_URL))
        {
            Serial.println("[FIREBASE-AUTH] Unable to open bootstrap connection");
            httpCode = 0;
        }
        else
        {
            http.addHeader("Content-Type", "application/json");

            FirebaseJson payload;
            payload.set("mac", mac);
            payload.set("deviceSecret", secret);
            String body;
            payload.toString(body);

            Serial.print("[SECURITY] Requesting device bootstrap token (attempt ");
            Serial.print(attempt);
            Serial.print("/");
            Serial.print(MAX_CONNECT_ATTEMPTS);
            Serial.println(")");
            httpCode = http.POST(body);
            // The secret existed only in `payload`/`body`, local to this
            // scope - cleared immediately after send; never logged, never
            // echoed anywhere.
            body = "";
            payload.clear();

            if (httpCode > 0) { response = http.getString(); }
            http.end();
        }

        if (httpCode > 0) { break; } // a real server response - stop retrying regardless of what it says

        if (attempt < MAX_CONNECT_ATTEMPTS)
        {
            Serial.print("[FIREBASE-AUTH] Connection-level failure (code ");
            Serial.print(httpCode);
            Serial.println("), retrying...");
            delay(1500);
        }
    }

    if (httpCode <= 0)
    {
        Serial.print("[FIREBASE-AUTH] Bootstrap connection failed after ");
        Serial.print(MAX_CONNECT_ATTEMPTS);
        Serial.print(" attempts, last code ");
        Serial.println(httpCode);
        return false;
    }

    if (httpCode != 200)
    {
        Serial.print("[FIREBASE-AUTH] Bootstrap rejected, HTTP ");
        Serial.println(httpCode);
        return false;
    }

    FirebaseJson responseJson;
    responseJson.setJsonData(response);
    FirebaseJsonData field;

    String customToken;
    if (responseJson.get(field, "customToken")) { customToken = field.stringValue; }

    // deviceId is not secret (it is already the claim code shown to Admins
    // when the device is registered) - returned alongside the token so a
    // first-time unit that has not yet persisted one can learn the
    // server-resolved value.
    String resolvedDeviceId;
    if (responseJson.get(field, "deviceId")) { resolvedDeviceId = field.stringValue; }

    response = "";

    if (customToken.isEmpty())
    {
        Serial.println("[FIREBASE-AUTH] Bootstrap response missing token");
        return false;
    }
    Serial.println("[SECURITY] Bootstrap succeeded");

    if (!resolvedDeviceId.isEmpty() && resolvedDeviceId != _deviceId)
    {
        saveDeviceId(resolvedDeviceId);
    }

    Firebase.setCustomToken(&_config, customToken);
    customToken = "";
    Firebase.begin(&_config, &_auth);

    unsigned long startedAt = millis();
    while (!Firebase.ready() && millis() - startedAt < 10000UL)
    {
        delay(100);
        yield();
    }

    if (!Firebase.ready())
    {
        Serial.println("[FIREBASE-AUTH] Sign-in with minted token did not complete");
        return false;
    }
    Serial.println("[SECURITY] Firebase custom-token authentication succeeded");

    const char* newRefreshToken = Firebase.getRefreshToken();
    if (newRefreshToken != nullptr && strlen(newRefreshToken) > 0)
    {
        saveRefreshToken(String(newRefreshToken));
        Serial.println("[SECURITY] Refresh token persisted");
    }

    return true;
}

// ============================================================
// PERSISTENCE (LittleFS)
// ============================================================

void FirebaseManager::loadDeviceAuthCredentials(String& outSecret, String& outRefreshToken)
{
    outSecret = readTextFile(SECRET_FILE);
    outRefreshToken = readTextFile(REFRESH_FILE);

    if (outSecret.isEmpty() && _bootstrapDeviceSecret != nullptr && strlen(_bootstrapDeviceSecret) > 0)
    {
        // First boot: no persisted secret yet - seed from the compiled-in
        // provisioning constant (this unit's one-time injection, set in
        // the .ino) and persist it to flash immediately. Every later boot
        // reads from flash, so a subsequent reflash with that constant
        // blanked out does not lose this device's identity.
        outSecret = _bootstrapDeviceSecret;
        writeTextFile(SECRET_FILE, outSecret);
        Serial.println("[SECURITY] Device secret seeded from firmware constant and persisted to flash");
    }
}

void FirebaseManager::saveRefreshToken(const String& token)
{
    writeTextFile(REFRESH_FILE, token);
}

void FirebaseManager::loadDeviceId()
{
    _deviceId = readTextFile(DEVICE_ID_FILE);
}

void FirebaseManager::saveDeviceId(const String& id)
{
    writeTextFile(DEVICE_ID_FILE, id);
    _deviceId = id;
}

// ============================================================
// LIVE WEIGHT — updates devices/{deviceId}/harvestScale/liveWeight
// ============================================================
//
// Uses setFloat() — overwrites the same node every call.
// No new entries created, no quota used per reading.
//

bool FirebaseManager::updateLiveWeight(float grams)
{
    if (!isReady()) { return false; }

    String path = deviceRoot() + "/liveWeight";

    if (Firebase.RTDB.setFloat(&_fbData, path, grams))
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
// HARVEST LOG — pushes to devices/{deviceId}/harvestScale/harvests
// on stable reading only
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

    String path = deviceRoot() + "/harvests";

    if (Firebase.RTDB.pushJSON(&_fbData, path, &json))
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
