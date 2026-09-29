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
// LEGACY PENDING DATA — offline sync no longer exists in this firmware.
// These are the on-disk footprints of the TWO earlier designs that did
// support it, kept here ONLY so clearLegacyPendingData() (see .h) can find
// and delete them on boot - never read for their content, never uploaded.
// ------------------------------------------------------------------
//
// Design 1 (original): one single pending measurement across four flat
// files.
const char* LEGACY_SINGLE_PENDING_GRAMS_FILE          = "/pending_grams.txt";
const char* LEGACY_SINGLE_PENDING_CAPTURED_EPOCH_FILE  = "/pending_captured_epoch.txt";
const char* LEGACY_SINGLE_PENDING_UPTIME_FILE          = "/pending_uptime_ms.txt";
const char* LEGACY_SINGLE_PENDING_MEASUREMENT_ID_FILE  = "/pending_measurement_id.txt";
//
// Design 2 (the offline queue this revision removes): one file per queued
// measurement, named "/q_<measurementId>" - e.g. "/q_HS_1a2b3c_42".
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
      _localStorageReady(false)
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
        Serial.println("[SECURITY] LittleFS mount failed - device identity cannot persist");
        return false;
    }

    _localStorageReady = true;

    clearLegacyPendingData();

    return true;
}

// Offline sync has been removed from this firmware entirely (see the .h's
// class-level comment) - any pending-measurement data left on flash by an
// earlier revision must be deleted, never read for its content or acted on,
// so an old test/offline measurement can never surface in Firebase after
// this revision is flashed. Safe to call every boot: a cheap no-op (one
// existence check, one directory scan with nothing matching) once nothing
// legacy remains.
void FirebaseManager::clearLegacyPendingData()
{
    bool foundLegacySingleSlot = LittleFS.exists(LEGACY_SINGLE_PENDING_GRAMS_FILE);

    if (foundLegacySingleSlot)
    {
        LittleFS.remove(LEGACY_SINGLE_PENDING_GRAMS_FILE);
        LittleFS.remove(LEGACY_SINGLE_PENDING_CAPTURED_EPOCH_FILE);
        LittleFS.remove(LEGACY_SINGLE_PENDING_UPTIME_FILE);
        LittleFS.remove(LEGACY_SINGLE_PENDING_MEASUREMENT_ID_FILE);
    }

    // Offline-queue format: one file per queued measurement, "/q_<id>".
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

    if (foundLegacySingleSlot || toRemoveCount > 0)
    {
        Serial.println("[SCALE] Legacy pending measurement cleared — offline sync disabled.");
    }
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
// ONLINE MEASUREMENT UPLOAD — synchronous, one-shot, never retried
// ============================================================
//
// Called only when the .ino has already confirmed WiFi is connected and
// isReady() is true - see the CAPTURE block in the .ino. Writes via
// setJSON() — deliberately NOT pushJSON(), so the RTDB path is fixed by
// this measurement's own freshly generated measurementId rather than a
// library-minted push key.
//
// There is no retry here and no persistence of the outcome: if this call
// fails, the .ino treats the measurement as a local-only "OFFLINE" result
// and moves on - this is an intentional product decision (see the .h),
// not an oversight. capturedAt/syncedAt end up essentially identical since
// this always runs synchronously at confirmation time - no elapsed-uptime
// reconstruction is needed the way the old offline-queue design required.
//
bool FirebaseManager::uploadMeasurement(float grams, uint32_t capturedAtEpochSec, String& outMeasurementId)
{
    if (!(grams > 0.0f) || isnan(grams) || isinf(grams)) { return false; }
    if (!isReady()) { return false; }

    String measurementId;
    if (!generateMeasurementId(measurementId)) { return false; }

    // Same NTP-sync-sanity check used elsewhere in this file (begin()) -
    // an un-synced clock reads as roughly the Unix epoch, so anything
    // before "a couple hours past 1970" is treated as "no real clock yet"
    // rather than fabricating a false timestamp.
    uint32_t syncedAtEpoch = (time(nullptr) > 8 * 3600 * 2) ? (uint32_t)time(nullptr) : 0;

    // Passed as double, not float/int: a float's 24-bit mantissa cannot
    // exactly hold a ~1.7-billion-second epoch timestamp (corrupting it by
    // tens of seconds), and this library's exact integer-type overload set
    // isn't verifiable from this repo alone (the HX711/Firebase_ESP_Client
    // library sources aren't vendored here) - double's 53-bit mantissa
    // exactly represents every value these fields can hold on this
    // firmware (uint32_t epoch seconds, uint32_t millis) with no risk of
    // hitting an unsupported overload.
    FirebaseJson json;
    json.set("grams",         grams);
    json.set("kg",             grams / 1000.0f);
    json.set("capturedAt",    (double)capturedAtEpochSec);
    json.set("syncedAt",      (double)syncedAtEpoch);
    json.set("uptimeMs",      (double)millis());
    json.set("measurementId",  measurementId);

    String path = deviceRoot() + "/harvests/" + measurementId;

    if (!Firebase.RTDB.setJSON(&_fbData, path, &json))
    {
        Serial.print("[FB] Upload failed: ");
        Serial.println(_fbData.errorReason());
        return false; // not retried - the .ino falls back to an OFFLINE/local-only result
    }

    _readingCount++;
    outMeasurementId = measurementId;
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
