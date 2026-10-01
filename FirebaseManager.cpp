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

// This counter must keep advancing (and stay durable) even across an
// upload attempt that fails partway through, so a later attempt never
// reuses an id. See generateMeasurementId() below.
const char* MEASUREMENT_SEQUENCE_FILE = "/measurement_seq.txt";

// ------------------------------------------------------------------
// PENDING MEASUREMENT — single-slot, capture-before-upload, retried until
// synced (see the .h class comment). Four small flat files rather than one
// JSON blob: each field stays independently readable/writable with no
// read-modify-write merge risk, and it avoids pulling in a JSON library
// (not otherwise needed for local persistence) just to persist four
// scalars.
// ------------------------------------------------------------------
const char* PENDING_GRAMS_FILE          = "/pending_grams.txt";
const char* PENDING_CAPTURED_EPOCH_FILE = "/pending_captured_epoch.txt";
const char* PENDING_UPTIME_FILE         = "/pending_uptime_ms.txt";
const char* PENDING_MEASUREMENT_ID_FILE = "/pending_measurement_id.txt";

// ------------------------------------------------------------------
// LEGACY QUEUE DATA — a brief earlier firmware revision supported a
// multi-item OFFLINE QUEUE (one file per queued measurement, "/q_<id>"),
// which this revision does not bring back (still single-slot, by design -
// see the .h). Kept here ONLY so clearLegacyQueueData() (see .h) can find
// and delete any leftover queue files on boot - never read for their
// content, never uploaded.
// ------------------------------------------------------------------
const char* LEGACY_QUEUE_FILE_PREFIX = "q_";

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

// Returns whether the write actually succeeded.
//
// Writes to a temporary sibling file first, then replaces the canonical
// path with LittleFS.rename() - rather than opening the canonical path
// directly in "w" mode, which truncates it to zero bytes immediately and
// would leave that truncated/empty file behind for good if power is lost
// before the new content is fully written and closed. With this pattern,
// the canonical file is only ever touched by the rename step itself, once
// the new content is already fully and durably written to the temp file.
//
// Residual power-loss window: this ESP8266 LittleFS port does not publish a
// hard atomicity guarantee for rename() the way, say, POSIX rename(2) does.
// A power loss during the rename call itself could still (in principle)
// leave the canonical path missing rather than holding either the old or
// the new content - a strictly smaller and different failure mode than the
// old in-place truncate (which had a much longer exposure window, spanning
// the entire data write), but not a mathematically proven zero-risk one.
bool writeTextFile(const char* path, const String& value)
{
    String tmpPath = String(path) + ".tmp";

    File f = LittleFS.open(tmpPath, "w");
    if (!f)
    {
        Serial.println("[SECURITY] Unable to open temp file for write");
        return false;
    }

    size_t written = f.print(value);
    f.flush();
    f.close();

    if (written != (size_t)value.length())
    {
        Serial.println("[SECURITY] Temp file write incomplete");
        LittleFS.remove(tmpPath);
        return false;
    }

    // Try a direct rename first (fully atomic if this LittleFS port
    // supports replacing an existing destination in one operation). Only
    // fall back to a separate remove-then-rename if that's refused - this
    // reopens a brief window where `path` doesn't exist, but bounded to a
    // single filesystem metadata operation rather than the old write's full
    // duration.
    if (LittleFS.rename(tmpPath, path)) { return true; }

    LittleFS.remove(path);
    if (LittleFS.rename(tmpPath, path)) { return true; }

    Serial.println("[SECURITY] Unable to finalize file replace (rename failed)");
    return false;
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
      _readingCount(0),
      _localStorageReady(false),
      _hasPendingMeasurement(false),
      _pendingGrams(0.0f),
      _pendingCapturedAtEpoch(0),
      _pendingUptimeMs(0),
      _pendingMeasurementId(""),
      _pendingCapturedThisBoot(false)
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

    // Idempotent - a no-op if the .ino already called this in setup()
    // before WiFi, which it does, so device-identity persistence works
    // even on a unit that never reaches the internet. Called again here too
    // since this function also needs LittleFS for credentials, and begin()
    // itself is now re-callable (see the retry logic in loop()).
    beginLocalStorage();

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

    // A refresh token can authenticate successfully while /device_id.txt is
    // separately missing/corrupt (they're two independent files) -
    // trySecureAuthentication() already tries to recover this by falling
    // through to a fresh bootstrap (which re-resolves and persists
    // deviceId server-side) whenever that happens, but this is the final
    // guard: never let _ready become true with an empty _deviceId, since
    // deviceRoot() would silently build a malformed "/devices//harvestScale"
    // path and every subsequent read/write would go to the wrong place.
    if (!authenticated || _deviceId.length() == 0)
    {
        Serial.println();
        Serial.println("[FB] ERROR: Secure device authentication failed or device identity unavailable.");
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
            // The refresh token and the device-ID file are two independent
            // LittleFS files - a refresh-grant sign-in can succeed while
            // /device_id.txt is separately missing/corrupt. Don't accept
            // this as a full success in that case; fall through to the
            // bootstrap path below instead, which re-resolves and persists
            // a fresh deviceId server-side rather than leaving this device
            // "authenticated" with no identity to write data under.
            if (_deviceId.length() > 0)
            {
                Serial.println("[SECURITY] Refresh-token authentication succeeded");
                return true;
            }
            Serial.println("[SECURITY] Refresh-token authentication succeeded but device identity is missing/corrupt - falling back to bootstrap to recover it");
        }
        else
        {
            Serial.println("[SECURITY] Refresh-token authentication failed");
        }
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
// LOCAL STORAGE — mount + one-time legacy pending-data cleanup
// ============================================================
//
// Deliberately independent of WiFi/Firebase auth: called from the .ino's
// setup() before WiFi is even attempted, so device-identity credentials
// and the measurement-sequence counter are available on a unit that never
// reaches the internet at all. begin() above also calls this (idempotently)
// since it separately needs LittleFS for credentials.
//

bool FirebaseManager::beginLocalStorage()
{
    if (_localStorageReady) { return true; }

    if (!LittleFS.begin())
    {
        Serial.println("[SECURITY] LittleFS mount failed - device identity and pending measurements cannot persist");
        return false;
    }

    _localStorageReady = true;

    clearLegacyQueueData();
    loadPendingMeasurementFromDisk();

    return true;
}

// A brief earlier firmware revision supported a multi-item offline QUEUE
// (one file per queued measurement, "/q_<id>") - this revision still does
// not bring that back (single-slot only, see the .h), so any leftover
// queue files from that revision must be deleted, never read for their
// content or acted on. Safe to call every boot: a cheap no-op (one
// directory scan with nothing matching) once nothing legacy remains.
void FirebaseManager::clearLegacyQueueData()
{
    // Collected into a small fixed list first, then removed in a SEPARATE
    // pass - safer than calling LittleFS.remove() on a path while a Dir
    // iterator is still walking the very same directory. Bounded to 32
    // entries; the queue this is cleaning up after was itself bounded to
    // 20, so this comfortably covers a fully-maxed queue with headroom.
    constexpr uint8_t MAX_LEGACY_QUEUE_FILES = 32;
    String toRemove[MAX_LEGACY_QUEUE_FILES];
    uint8_t toRemoveCount = 0;

    Dir dir = LittleFS.openDir("/");
    while (dir.next() && toRemoveCount < MAX_LEGACY_QUEUE_FILES)
    {
        String name = dir.fileName();
        if (name.startsWith("/")) { name = name.substring(1); }
        if (name.startsWith(LEGACY_QUEUE_FILE_PREFIX))
        {
            toRemove[toRemoveCount++] = "/" + name;
        }
    }

    for (uint8_t i = 0; i < toRemoveCount; i++)
    {
        LittleFS.remove(toRemove[i]);
    }

    if (toRemoveCount > 0)
    {
        Serial.println("[SCALE] Legacy offline-queue files cleared.");
    }
}

void FirebaseManager::loadPendingMeasurementFromDisk()
{
    String gramsStr    = readTextFile(PENDING_GRAMS_FILE);
    String capturedStr = readTextFile(PENDING_CAPTURED_EPOCH_FILE);
    String uptimeStr   = readTextFile(PENDING_UPTIME_FILE);
    String idStr       = readTextFile(PENDING_MEASUREMENT_ID_FILE);

    if (gramsStr.isEmpty())
    {
        _hasPendingMeasurement = false;
        return;
    }

    _pendingGrams           = gramsStr.toFloat();
    _pendingCapturedAtEpoch = (uint32_t)capturedStr.toInt();
    _pendingUptimeMs        = (uint32_t)uptimeStr.toInt();
    _pendingMeasurementId   = idStr;
    _hasPendingMeasurement  = _pendingGrams > 0.0f && _pendingMeasurementId.length() > 0;

    // Recovered from flash at boot, NOT staged during this running process -
    // _pendingUptimeMs was written by a PREVIOUS boot's millis(), which has
    // no relationship whatsoever to this boot's millis() clock. See
    // _pendingCapturedThisBoot's declaration in the header.
    _pendingCapturedThisBoot = false;

    if (_hasPendingMeasurement)
    {
        // Survives an ESP restart/power loss by design - this is exactly
        // that recovery happening: a measurement captured before this boot
        // that never confirmed as synced is picked back up here, and the
        // normal loop()/syncPendingMeasurement() retry logic takes it from
        // here.
        Serial.print("[FB] Pending measurement recovered from flash: ");
        Serial.print(_pendingGrams, 1);
        Serial.print(" g | id: ");
        Serial.println(_pendingMeasurementId);
    }
    else if (!gramsStr.isEmpty())
    {
        // Grams present but something else about the record is unusable
        // (e.g. a zero/negative grams value, or a missing id - shouldn't
        // happen given stagePendingMeasurement()'s all-or-nothing write
        // below, but a half-written record from a very old firmware
        // revision isn't impossible). Discard rather than risk acting on a
        // malformed pending record.
        Serial.println("[FB] WARNING: Discarding unusable pending-measurement record on disk.");
        clearPendingMeasurementFile();
    }
}

void FirebaseManager::clearPendingMeasurementFile()
{
    if (_localStorageReady)
    {
        LittleFS.remove(PENDING_GRAMS_FILE);
        LittleFS.remove(PENDING_CAPTURED_EPOCH_FILE);
        LittleFS.remove(PENDING_UPTIME_FILE);
        LittleFS.remove(PENDING_MEASUREMENT_ID_FILE);
    }
    _hasPendingMeasurement   = false;
    _pendingGrams            = 0.0f;
    _pendingCapturedAtEpoch  = 0;
    _pendingUptimeMs         = 0;
    _pendingMeasurementId    = "";
    _pendingCapturedThisBoot = false;
}

// ============================================================
// MEASUREMENT ID — offline-safe, collision-free, unique per upload attempt
// ============================================================
//
// "HS_<chipId>_<sequence>": chipId (ESP.getChipId()) identifies the
// physical unit, needs no network and never changes; sequence is a
// monotonic counter persisted independently of any individual upload,
// advanced durably as the very FIRST step of every call - before the
// caller has done anything else that could fail. A gap in the sequence
// (this call succeeds but the upload that follows it fails) is harmless;
// the same sequence value being handed out twice is not, since two
// different physical measurements would then collide on one RTDB path.
//

bool FirebaseManager::generateMeasurementId(String& outId)
{
    if (!_localStorageReady && !beginLocalStorage()) { return false; }

    uint32_t sequence = (uint32_t)readTextFile(MEASUREMENT_SEQUENCE_FILE).toInt();
    sequence++;

    if (!writeTextFile(MEASUREMENT_SEQUENCE_FILE, String(sequence)))
    {
        // Could not durably advance the counter - do NOT hand out an id
        // built from it, since an un-persisted increment could be handed
        // out again (reused) on a later call after a reboot.
        return false;
    }

    outId = "HS_" + String(ESP.getChipId(), HEX) + "_" + String(sequence);
    return true;
}

// ============================================================
// LIVE WEIGHT — updates devices/{deviceId}/harvestScale/{liveWeight,lastSeen}
// ============================================================
//
// Uses updateNode() (a partial PATCH) — overwrites just these two children
// every call. No new entries created, no quota used per reading, and
// harvests/ is never touched from here.
//

bool FirebaseManager::updateLiveWeight(float grams)
{
    if (!isReady()) { return false; }

    // Same NTP-sync-sanity check used elsewhere in this file (begin()) -
    // an un-synced clock reads as roughly the Unix epoch, so anything
    // before "a couple hours past 1970" is treated as "no real clock yet"
    // rather than fabricating a false timestamp.
    uint32_t nowEpoch = (time(nullptr) > 8 * 3600 * 2) ? (uint32_t)time(nullptr) : 0;

    // updateNode() - a partial PATCH - rather than two separate setFloat()
    // calls: one request instead of two (this already runs every 5s, so
    // halving the SSL round-trips matters on this chip), and it guarantees
    // liveWeight and lastSeen always land together, in the same write.
    // Only these two children are touched; harvests/ and everything else
    // under harvestScale/ is left completely alone.
    FirebaseJson json;
    json.set("liveWeight", grams);
    json.set("lastSeen",   (double)nowEpoch);

    if (Firebase.RTDB.updateNode(&_fbData, deviceRoot(), &json))
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
// PENDING MEASUREMENT — CAPTURE != UPLOAD, RETRIED UNTIL SYNCED
// ============================================================
//
// A confirmed physical weighing is persisted to flash FIRST (this is what
// "captured" means from the .ino's point of view) and only written to RTDB
// once Firebase is actually ready to accept it - possibly immediately, on
// a following loop() cycle, or after a reconnect that happens minutes
// later. stagePendingMeasurement() only ever reports success once the
// flash write itself is confirmed; syncPendingMeasurement() only ever
// clears that persisted record after RTDB confirms the write landed.
//

bool FirebaseManager::stagePendingMeasurement(float grams, uint32_t capturedAtEpochSec)
{
    if (!(grams > 0.0f) || isnan(grams) || isinf(grams)) { return false; }

    // One slot only (see the header comment) - never silently overwrite an
    // already-staged, not-yet-synced measurement with a new physical
    // capture. The .ino is expected to check hasPendingMeasurement() itself
    // before even attempting a new capture, but this is the enforcement
    // point that actually matters.
    if (_hasPendingMeasurement) { return false; }

    if (!_localStorageReady && !beginLocalStorage()) { return false; }

    // Generated FIRST, before any of this measurement's own fields are
    // written: this durably advances the sequence counter itself as its
    // own first step, so even if everything below fails partway, no future
    // measurement can ever be handed this same id again. See
    // generateMeasurementId().
    String measurementId;
    if (!generateMeasurementId(measurementId)) { return false; }

    bool persisted =
        writeTextFile(PENDING_GRAMS_FILE, String(grams, 4)) &&
        writeTextFile(PENDING_CAPTURED_EPOCH_FILE, String(capturedAtEpochSec)) &&
        writeTextFile(PENDING_UPTIME_FILE, String((unsigned long)millis())) &&
        writeTextFile(PENDING_MEASUREMENT_ID_FILE, measurementId);

    if (!persisted)
    {
        // A partial write is worse than no write at all - a future boot
        // loading a half-written record would trust a grams value with no
        // matching timestamp/id, or vice versa. Clean slate instead; the
        // physical measurement is NOT considered captured, so the caller's
        // own stability hold can simply retry on its next qualifying cycle
        // (which calls generateMeasurementId() again and gets a fresh,
        // never-before-used id - the sequence gap left behind here is
        // expected and harmless).
        clearPendingMeasurementFile();
        return false;
    }

    _hasPendingMeasurement   = true;
    _pendingGrams            = grams;
    _pendingCapturedAtEpoch  = capturedAtEpochSec;
    _pendingUptimeMs         = (uint32_t)millis();
    _pendingMeasurementId    = measurementId;
    // Staged right here, in this running process - see the header comment
    // on this member for why that's exactly what makes reconstructing
    // capturedAt from elapsed uptime safe later, in syncPendingMeasurement().
    _pendingCapturedThisBoot = true;
    return true;
}

bool FirebaseManager::restagePendingMeasurement(float grams, uint32_t capturedAtEpochSec, const String& measurementId)
{
    if (!(grams > 0.0f) || isnan(grams) || isinf(grams)) { return false; }
    if (measurementId.isEmpty()) { return false; }

    // Updating a DIFFERENT pending record than the one actually on disk
    // would silently corrupt it - the .ino's own session bookkeeping
    // (currentSessionMeasurementId) should make this impossible in normal
    // operation, so treat it as a real inconsistency, not a case to paper
    // over.
    if (_hasPendingMeasurement && _pendingMeasurementId != measurementId)
    {
        Serial.println("[FB] ERROR: restagePendingMeasurement id mismatch - refusing.");
        return false;
    }

    if (!_localStorageReady && !beginLocalStorage()) { return false; }

    // All four fields rewritten, not just grams/capturedAt - the id file
    // might not currently exist on disk at all (the prior total for this
    // same session could have already synced and been cleared, in which
    // case _hasPendingMeasurement is false here even though this is still
    // the same ongoing physical session from the .ino's point of view) and
    // this must leave a fully self-consistent record behind either way.
    bool persisted =
        writeTextFile(PENDING_GRAMS_FILE, String(grams, 4)) &&
        writeTextFile(PENDING_CAPTURED_EPOCH_FILE, String(capturedAtEpochSec)) &&
        writeTextFile(PENDING_UPTIME_FILE, String((unsigned long)millis())) &&
        writeTextFile(PENDING_MEASUREMENT_ID_FILE, measurementId);

    if (!persisted)
    {
        // Same reasoning as stagePendingMeasurement(): a half-written
        // record is worse than none. This does mean a restack that fails
        // partway through can discard an earlier, still-good pending total
        // for this same session rather than leaving it be - an accepted,
        // documented edge case (flash write failures are rare, and the
        // caller's own stability hold simply retries on a later cycle,
        // same as any other staging failure).
        clearPendingMeasurementFile();
        return false;
    }

    _hasPendingMeasurement   = true;
    _pendingGrams            = grams;
    _pendingCapturedAtEpoch  = capturedAtEpochSec;
    _pendingUptimeMs         = (uint32_t)millis();
    _pendingMeasurementId    = measurementId;
    _pendingCapturedThisBoot = true;
    return true;
}

bool FirebaseManager::hasPendingMeasurement() const
{
    return _hasPendingMeasurement;
}

float FirebaseManager::getPendingGrams() const
{
    return _hasPendingMeasurement ? _pendingGrams : 0.0f;
}

String FirebaseManager::getPendingMeasurementId() const
{
    return _hasPendingMeasurement ? _pendingMeasurementId : String("");
}

// Writes devices/{deviceId}/harvestScale/harvests/{measurementId} via
// setJSON() — deliberately NOT pushJSON(). pushJSON() mints a brand-new key
// on every single call, so a retry after an AMBIGUOUS network outcome (the
// write actually reached RTDB and was stored, but the success response
// itself was lost, so this firmware believes it failed) would push a SECOND
// entry for the exact same physical weighing. setJSON() to a path fixed by
// this measurement's own stable measurementId means every retry, no matter
// how many, overwrites the SAME node with the SAME content instead.
bool FirebaseManager::syncPendingMeasurement()
{
    if (!_hasPendingMeasurement) { return false; }
    if (_pendingMeasurementId.isEmpty()) { return false; } // see loadPendingMeasurementFromDisk()'s discard-if-unusable path
    if (!isReady()) { return false; }

    // Same NTP-sync-sanity check used elsewhere in this file (begin()) -
    // an un-synced clock reads as roughly the Unix epoch, so anything
    // before "a couple hours past 1970" is treated as "no real clock yet"
    // rather than fabricating a false timestamp.
    uint32_t syncedAtEpoch = (time(nullptr) > 8 * 3600 * 2) ? (uint32_t)time(nullptr) : 0;

    // capturedAt was 0 at staging time only if the clock genuinely wasn't
    // synced yet at that exact moment - reconstruct it here from elapsed
    // uptime ONLY when this pending measurement was staged during THIS same
    // boot (_pendingCapturedThisBoot), which is the one condition that
    // actually proves _pendingUptimeMs and millis() right now share the
    // same clock origin. A measurement recovered from a PREVIOUS boot
    // (_pendingCapturedThisBoot false) can never be reconstructed this way
    // - millis() resets on every reboot, so there is no valid arithmetic
    // relationship between that old uptime value and this boot's millis().
    // capturedAt simply stays 0 in that case, on purpose: a consumer must
    // treat an unverifiable capture time as exactly that, never as "just
    // captured."
    uint32_t capturedAtToSend = _pendingCapturedAtEpoch;

    if (capturedAtToSend == 0 && _pendingCapturedThisBoot && syncedAtEpoch != 0)
    {
        uint32_t elapsedMs  = (uint32_t)millis() - _pendingUptimeMs; // unsigned subtraction - correct even across one millis() rollover
        uint32_t elapsedSec = elapsedMs / 1000;

        if (elapsedSec <= syncedAtEpoch) // sanity guard against an underflowed/bogus result
        {
            capturedAtToSend = syncedAtEpoch - elapsedSec;
            Serial.print("[FB] Reconstructed capturedAt from same-boot uptime: ");
            Serial.println(capturedAtToSend);
        }
    }

    // Passed as double, not float/int: a float's 24-bit mantissa cannot
    // exactly hold a ~1.7-billion-second epoch timestamp (corrupting it by
    // tens of seconds), and this library's exact integer-type overload set
    // isn't verifiable from this repo alone (the HX711/Firebase_ESP_Client
    // library sources aren't vendored here) - double's 53-bit mantissa
    // exactly represents every value these fields can hold on this
    // firmware (uint32_t epoch seconds, uint32_t millis) with no risk of
    // hitting an unsupported overload.
    FirebaseJson json;
    json.set("grams",         _pendingGrams);
    json.set("kg",             _pendingGrams / 1000.0f);
    json.set("capturedAt",    (double)capturedAtToSend);
    json.set("syncedAt",      (double)syncedAtEpoch);
    json.set("uptimeMs",      (double)_pendingUptimeMs);
    json.set("measurementId",  _pendingMeasurementId);

    String path = deviceRoot() + "/harvests/" + _pendingMeasurementId;

    if (!Firebase.RTDB.setJSON(&_fbData, path, &json))
    {
        Serial.print("[FB] Pending measurement sync failed: ");
        Serial.println(_fbData.errorReason());
        return false; // pending record stays on disk (same measurementId) - retried next call
    }

    _readingCount++;

    Serial.print("[FB] Pending measurement synced: ");
    Serial.print(_pendingGrams, 1);
    Serial.print(" g | id: ");
    Serial.println(_pendingMeasurementId);

    // Clear ONLY now that RTDB has confirmed the write - never before.
    clearPendingMeasurementFile();
    return true;
}

bool FirebaseManager::discardPendingMeasurement()
{
    if (!_hasPendingMeasurement) { return false; }

    Serial.print("[FB] WARNING: Pending measurement discarded by user request: ");
    Serial.print(_pendingGrams, 1);
    Serial.print(" g | id: ");
    Serial.println(_pendingMeasurementId);

    clearPendingMeasurementFile();
    return true;
}

// ============================================================
// STATS
// ============================================================

int FirebaseManager::getTotalReadings()
{
    return (int)_readingCount;
}

// ============================================================
// NON-BLOCKING RECONNECT — see the .h's class comment for why this
// deliberately does not attempt a fresh HTTPS bootstrap.
// ============================================================

void FirebaseManager::startReconnect()
{
    if (_reconnectPhase != ReconnectPhase::Idle) { return; } // already in progress

    if (!_localStorageReady && !beginLocalStorage()) { return; }

    if (_deviceId.length() == 0) { loadDeviceId(); }

    String secret, refreshToken;
    loadDeviceAuthCredentials(secret, refreshToken);

    if (refreshToken.length() == 0)
    {
        // Nothing this non-blocking path can do - a fresh bootstrap needs
        // the synchronous HTTPS POST that only begin() (setup()-time) runs.
        // Logged once per boot, not every cooldown, so a device that's
        // never been bootstrapped doesn't spam this line every 30s.
        if (!_loggedNoRefreshToken)
        {
            _loggedNoRefreshToken = true;
            Serial.println("[FIREBASE] No stored refresh token - reconnect needs a reboot to bootstrap a new identity.");
        }
        return;
    }

    _config.api_key      = _apiKey;
    _config.database_url = _databaseURL;
    _fbData.setBSSLBufferSize(2048, 512);

    Serial.println("[FIREBASE] Reconnect: syncing time via NTP...");
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    _reconnectPhase          = ReconnectPhase::WaitingNtp;
    _reconnectPhaseStartedAt = millis();
}

void FirebaseManager::pollReconnect()
{
    if (_reconnectPhase == ReconnectPhase::Idle) { return; }

    if (_reconnectPhase == ReconnectPhase::WaitingNtp)
    {
        // Same NTP-sync sanity check used elsewhere in this file - no
        // delay() here, just a single non-blocking check per call.
        if (time(nullptr) > 8 * 3600 * 2)
        {
            Serial.println("[FIREBASE] Reconnect: time synced, starting auth...");

            String secret, refreshToken;
            loadDeviceAuthCredentials(secret, refreshToken);

            // Same auto-detected refresh-grant sign-in restoreFromRefreshToken()
            // uses, just polled across loop() iterations instead of a local
            // blocking while-loop.
            Firebase.setCustomToken(&_config, refreshToken);
            Firebase.begin(&_config, &_auth);

            _reconnectPhase          = ReconnectPhase::WaitingAuth;
            _reconnectPhaseStartedAt = millis();
            return;
        }

        if (millis() - _reconnectPhaseStartedAt >= RECONNECT_NTP_TIMEOUT_MS)
        {
            Serial.println("[FIREBASE] Reconnect: NTP sync timed out - will retry later.");
            _reconnectPhase = ReconnectPhase::Idle;
        }
        return;
    }

    if (_reconnectPhase == ReconnectPhase::WaitingAuth)
    {
        if (Firebase.ready())
        {
            // Two independent files - see begin()'s matching guard.
            if (_deviceId.length() == 0) { loadDeviceId(); }

            if (_deviceId.length() == 0)
            {
                Serial.println("[FIREBASE] Reconnect: authenticated but device identity missing - not marking ready.");
                _reconnectPhase = ReconnectPhase::Idle;
                return;
            }

            const char* rotatedRefreshToken = Firebase.getRefreshToken();
            if (rotatedRefreshToken != nullptr && strlen(rotatedRefreshToken) > 0)
            {
                saveRefreshToken(String(rotatedRefreshToken));
            }

            Firebase.reconnectWiFi(true);
            _ready = true;

            Serial.println("[FIREBASE] Reconnect: authentication restored.");
            _reconnectPhase = ReconnectPhase::Idle;
            return;
        }

        if (millis() - _reconnectPhaseStartedAt >= RECONNECT_AUTH_TIMEOUT_MS)
        {
            Serial.println("[FIREBASE] Reconnect: authentication timed out - will retry later.");
            _reconnectPhase = ReconnectPhase::Idle;
        }
        return;
    }
}
