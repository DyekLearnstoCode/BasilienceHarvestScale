#ifndef FIREBASE_MANAGER_H
#define FIREBASE_MANAGER_H

#include <Arduino.h>
#include <Firebase_ESP_Client.h>

// ============================================================
// FIREBASE MANAGER
// Firebase Realtime Database — weight data uploader
//
// SECURE DEVICE AUTH — mirrors the main BasilienceFirmware_V2 (ESP32)
// pattern: this device signs in as its OWN scoped RTDB identity
// (uid == deviceId), obtained by exchanging a per-device bootstrap
// secret for a Firebase custom token via the same deviceAuthBootstrap
// Cloud Function the ESP32 units use. It never holds the legacy
// project-wide RTDB "Database Secret" that the previous version of
// this file used — that credential bypasses every security rule for
// the ENTIRE database, not just this device.
//
// Required library: "Firebase ESP Client" by Mobizt
// Install via Arduino Library Manager
// (LittleFS, ESP8266HTTPClient and WiFiClientSecure/BearSSL ship with
// the ESP8266 Arduino core — no separate install needed)
//
// Database structure — scoped under this device's own subtree once
// bootstrapped, matching database.rules.json's devices/$deviceId
// (auth.uid === $deviceId) rule:
//
//   devices/{deviceId}/harvestScale/
//     liveWeight : float  ← live reading (overwrite every loop)
//     harvests/
//       {measurementId}/    ← child key IS the canonical measurementId,
//                              generated the instant a measurement is
//                              confirmed ONLINE (see "MEASUREMENT ID"
//                              below) and written with setJSON() - never
//                              pushJSON(). If this exact call is ever
//                              retried within the same attempt (not
//                              currently done, but the path itself stays
//                              idempotent by design), it would overwrite
//                              the same node rather than creating a
//                              duplicate.
//         grams      : float
//         kg         : float
//         capturedAt : epoch seconds when the physical measurement was
//                      confirmed - 0 only when the scale's clock genuinely
//                      wasn't synced yet at that exact moment. Never a
//                      fabricated value.
//         syncedAt   : epoch seconds when this entry was written - always
//                      essentially the same instant as capturedAt now,
//                      since uploadMeasurement() only ever runs
//                      synchronously at confirmation time (see below).
//         uptimeMs   : device uptime (millis()) at capture, for diagnostics
//
// ONLINE-ONLY UPLOAD (no offline queue) — a confirmed physical weighing is
// EITHER uploaded to RTDB synchronously, right at the moment it's
// confirmed (if WiFi is connected and Firebase is already authenticated),
// OR simply displayed on the LCD as a local-only result and then
// forgotten. There is deliberately no persistence layer, retry queue, or
// later "catch up and upload" path for offline measurements - see
// uploadMeasurement() below and the .ino's CAPTURE block for the full
// capture-time decision. This is an intentional product decision: an
// offline measurement is meant for manual entry in the Android app, and
// must never later compete with a genuinely new online measurement by
// silently appearing in Firebase after the fact.
//
// MEASUREMENT ID — generated only for an ONLINE upload attempt, the
// instant that attempt is made: "HS_<chipId>_<sequence>", where chipId is
// this unit's own ESP.getChipId() (fixed in silicon, needs no network) and
// sequence is a monotonic counter persisted to flash independently of any
// individual upload, so a gap (an attempt that then failed) is harmless
// but a value is never reused.
// ============================================================

class FirebaseManager
{
public:

    // --------------------------------------------------------
    // CONSTRUCTOR
    // --------------------------------------------------------
    //
    // apiKey               — Firebase project Web API Key
    // databaseURL          — https://basilience-database-default-rtdb
    //                        .asia-southeast1.firebasedatabase.app
    // bootstrapDeviceSecret — this unit's one-time provisioning secret,
    //                        exchanged for a scoped Firebase identity on
    //                        first boot (see deviceAuthBootstrap). NOT
    //                        the project's RTDB Database Secret. Must be
    //                        registered server-side first — see
    //                        FirebaseManager.cpp's bootstrapSecureAuth()
    //                        comment for the required provisioning step.
    //

    FirebaseManager(
        const char* apiKey,
        const char* databaseURL,
        const char* bootstrapDeviceSecret
    );

    // --------------------------------------------------------
    // INITIALIZATION
    // --------------------------------------------------------

    // Call once in setup() AFTER WiFi is connected.
    bool begin();

    bool isReady() const;

    // --------------------------------------------------------
    // LOCAL STORAGE  (call once in setup(), BEFORE WiFi)
    // --------------------------------------------------------
    //
    // Mounts LittleFS (needed for device-identity credentials and the
    // measurement-sequence counter) and clears out any pending-measurement
    // data left behind by an earlier firmware revision that supported
    // offline sync - see clearLegacyPendingData() in the .cpp. Independent
    // of WiFi/Firebase entirely. Idempotent: safe to call again (from
    // begin() below, which also needs LittleFS) without remounting.
    bool beginLocalStorage();

    // --------------------------------------------------------
    // LIVE WEIGHT  (call every loop)
    // --------------------------------------------------------

    // Overwrites devices/{deviceId}/harvestScale/liveWeight with the
    // current reading. Fast set — no history, just current value.
    bool updateLiveWeight(float grams);

    // --------------------------------------------------------
    // ONLINE MEASUREMENT UPLOAD  (no offline persistence/retry - see header)
    // --------------------------------------------------------
    //
    // Call ONLY when the caller has already decided this measurement
    // should be attempted online (i.e. after checking WiFi + isReady() -
    // see the .ino's CAPTURE block). Generates a fresh, stable
    // measurementId and writes ONE RTDB entry via setJSON() (never
    // pushJSON()) to devices/{deviceId}/harvestScale/harvests/{measurementId}.
    //
    // Synchronous: blocks for the duration of one RTDB write. This is a
    // deliberate, bounded, one-shot call made exactly once at confirmation
    // time - NOT a reconnect/reauthentication operation, and NOT retried
    // by this class if it fails. On failure, returns false and the caller
    // (the .ino) treats this measurement as a local-only "OFFLINE" result;
    // nothing is queued or persisted for a later attempt.
    //
    // outMeasurementId is set only on success, for the caller's own log
    // line.
    bool uploadMeasurement(float grams, uint32_t capturedAtEpochSec, String& outMeasurementId);

    // --------------------------------------------------------
    // NON-BLOCKING RECONNECT  (loop()-driven, refresh-token only)
    // --------------------------------------------------------
    //
    // begin() (above) is the one-time, fully blocking setup()-time path -
    // acceptable there since nothing else is running yet. Calling it again
    // from loop() to recover a lost Firebase session could stall HX711
    // sampling, the LCD, and the Wi-Fi setup portal for ~25s (NTP wait +
    // auth wait) or, on a fresh bootstrap, far longer - unacceptable while
    // someone could be actively placing a harvest.
    //
    // startReconnect()/pollReconnect() instead step through the SAME
    // refresh-token restore begin() already does, in small non-blocking
    // increments (one state check per call, no delay()), bounded to
    // roughly the same NTP/auth timeouts begin() itself uses. Deliberately
    // does NOT fall back to a fresh bootstrapSecureAuth() HTTPS POST from
    // here - that call uses the stock (synchronous-only) HTTPClient API
    // with no non-blocking primitive available, and converting it would be
    // a much larger TLS/HTTP state-machine rewrite than this pass calls
    // for. A device with no persisted refresh token yet (or one that's
    // been permanently revoked) keeps local weighing fully available but
    // needs a reboot to re-run the one-time bootstrap in begin() - an
    // explicit, accepted trade-off, not a silent gap.
    //

    // Kicks off one reconnect attempt if none is already in progress -
    // never blocks. The caller (the .ino) supplies its own cooldown gate
    // around when this is called, same as it previously gated begin().
    void startReconnect();

    // Call every loop() iteration, unconditionally (like NetworkManager's
    // own update()) - advances any in-progress attempt by one cheap,
    // non-blocking step. No-op when nothing is in progress.
    void pollReconnect();

    // --------------------------------------------------------
    // STATS
    // --------------------------------------------------------

    // Get total number of readings stored (optional).
    int getTotalReadings();

private:

    enum class ReconnectPhase : uint8_t
    {
        Idle,
        WaitingNtp,
        WaitingAuth
    };

    ReconnectPhase _reconnectPhase          = ReconnectPhase::Idle;
    unsigned long  _reconnectPhaseStartedAt = 0;
    bool           _loggedNoRefreshToken    = false;

    static const unsigned long RECONNECT_NTP_TIMEOUT_MS  = 15000;
    static const unsigned long RECONNECT_AUTH_TIMEOUT_MS = 10000;

    const char* _apiKey;
    const char* _databaseURL;
    const char* _bootstrapDeviceSecret;

    FirebaseData   _fbData;
    FirebaseAuth   _auth;
    FirebaseConfig _config;

    bool     _ready;
    uint32_t _readingCount;
    String   _deviceId;

    bool _localStorageReady;

    // One-time cleanup of pending-measurement data left behind by an
    // earlier firmware revision (either the original single-pending-slot
    // design or the later multi-measurement offline queue) - offline sync
    // no longer exists, so nothing from either scheme may ever reach
    // Firebase. Safe to call every boot: a no-op once nothing legacy is
    // left on flash. See the .cpp for the exact filenames/paths involved.
    void clearLegacyPendingData();

    // Offline-safe, collision-free measurementId: "HS_<chipId>_<sequence>".
    // Advances (and durably persists) the sequence counter as its first
    // step, before this call can fail for any other reason - see the .cpp
    // for why a gap is acceptable here but reuse is not. Returns false (and
    // leaves outId untouched) only if the counter itself could not be
    // durably advanced, in which case the caller must not attempt an
    // upload at all rather than risk a reused/unstable id.
    bool generateMeasurementId(String& outId);

    // devices/{deviceId}/harvestScale — every RTDB path this class
    // touches lives under here, matching the rules' devices/$deviceId
    // scope. Empty/invalid until bootstrapSecureAuth() resolves an
    // identity.
    String deviceRoot() const;

    //==================================================
    // Secure Device Auth (bootstrap + refresh-token identity)
    //==================================================

    // Orchestrates the boot-time auth flow: refresh-token restore, then
    // secret-based bootstrap, in that order. Returns false if neither
    // credential works.
    bool trySecureAuthentication();

    // Restores a previously-established identity from a persisted
    // refresh token (Firebase.setCustomToken() auto-detects a non-JWT
    // string as a refresh token and performs a refresh-grant sign-in
    // directly — no bootstrap call needed).
    bool restoreFromRefreshToken(const String& refreshToken);

    // Calls the HTTPS bootstrap endpoint with {mac, deviceSecret} over a
    // TLS connection pinned to Google's own root CA, exchanges the
    // returned custom token for a full Firebase identity, and persists
    // the resulting refresh token. The secret is never logged.
    bool bootstrapSecureAuth(const String& secret);

    // Persistence — LittleFS-backed (this platform has no NVS/
    // Preferences API). Three small flat files instead of one JSON blob
    // to keep each field independently readable/writable with no
    // read-modify-write merge risk.
    void loadDeviceAuthCredentials(String& outSecret, String& outRefreshToken);
    void saveRefreshToken(const String& token);
    void loadDeviceId();
    void saveDeviceId(const String& id);
};

#endif // FIREBASE_MANAGER_H
