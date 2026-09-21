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
//                              generated ONCE when the physical measurement
//                              is staged (see "MEASUREMENT ID" below) and
//                              written with setJSON() - never pushJSON().
//                              A retry after an uncertain network outcome
//                              (push landed, response lost) reuses this
//                              same key and overwrites the same node
//                              in-place instead of creating a duplicate.
//         grams      : float
//         kg         : float
//         capturedAt : epoch seconds when the physical measurement was
//                      confirmed - 0 only when the scale genuinely could
//                      not establish (or reconstruct, within the same boot
//                      - see syncPendingWeight()) when it was captured.
//                      Never a fabricated value, and never syncedAt used
//                      as a stand-in.
//         syncedAt   : epoch seconds when this entry was actually written
//                      here (may be well after capturedAt if the device
//                      was offline in between - see the pending-measurement
//                      API below)
//         uptimeMs   : device uptime (millis()) at capture, for diagnostics
//
// CAPTURE != UPLOAD — a confirmed physical weighing is persisted to flash
// (stagePendingWeight()) before network availability ever enters the
// picture, and only cleared from flash once RTDB confirms the write
// (syncPendingWeight()). See the pending-measurement API below.
//
// MEASUREMENT ID — generated once, offline, the instant a measurement is
// staged: "HS_<chipId>_<sequence>", where chipId is this unit's own
// ESP.getChipId() (fixed in silicon, needs no network) and sequence is a
// monotonic counter persisted to flash independently of the pending record
// itself, so it survives even a failed staging attempt. Gaps in the
// sequence are fine; the same value is never reused. This ID is what makes
// a retry after an ambiguous network outcome idempotent - see
// stagePendingWeight()/syncPendingWeight() below.
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
    // Mounts LittleFS and recovers any pending measurement left over from
    // a previous boot (e.g. the device lost power/Wi-Fi before it could
    // sync). Independent of WiFi/Firebase entirely - a confirmed physical
    // measurement must be persistable even on a unit that has never once
    // reached the internet. Idempotent: safe to call again (from begin()
    // below, which also needs LittleFS) without remounting.
    bool beginLocalStorage();

    // --------------------------------------------------------
    // LIVE WEIGHT  (call every loop)
    // --------------------------------------------------------

    // Overwrites devices/{deviceId}/harvestScale/liveWeight with the
    // current reading. Fast set — no history, just current value.
    bool updateLiveWeight(float grams);

    // --------------------------------------------------------
    // PENDING MEASUREMENT  (capture ≠ upload — see .cpp)
    // --------------------------------------------------------
    //
    // A confirmed physical weighing is persisted to flash BEFORE it is
    // considered "captured," and stays persisted until Firebase actually
    // confirms the write - network availability must never determine
    // whether the physical measurement exists. One slot only: this is a
    // standalone single-platform scale, not a multi-entry offline queue.

    // Validates, persists to flash, and only THEN reports success. Refuses
    // (returns false) if a pending measurement is already staged — the
    // caller must not overwrite an unsynced measurement with a new one.
    bool stagePendingWeight(float grams, uint32_t capturedAtEpochSec);

    bool hasPendingWeight() const;

    // What's currently staged, for LCD display while unsynced. Meaningless
    // (returns 0) when hasPendingWeight() is false.
    float getPendingGrams() const;

    // Writes (or re-writes) exactly one RTDB entry, at a fixed path keyed by
    // this measurement's stable measurementId, for the staged measurement -
    // via setJSON(), never pushJSON(), so a retry always lands on the SAME
    // node instead of minting a new one. Clears the local pending file ONLY
    // after Firebase confirms the write succeeded; on any failure (including
    // an ambiguous one - the write may have actually landed) the pending
    // record is left untouched so a later call safely retries the identical
    // write. No-op (returns false) if nothing is pending or Firebase isn't
    // ready.
    bool syncPendingWeight();

    // --------------------------------------------------------
    // STATS
    // --------------------------------------------------------

    // Get total number of readings stored (optional).
    int getTotalReadings();

private:

    const char* _apiKey;
    const char* _databaseURL;
    const char* _bootstrapDeviceSecret;

    FirebaseData   _fbData;
    FirebaseAuth   _auth;
    FirebaseConfig _config;

    bool     _ready;
    uint32_t _readingCount;
    String   _deviceId;

    // Pending-measurement state, mirrored between RAM and small flat
    // LittleFS files (same "small flat files, no read-modify-write merge
    // risk" pattern already used for the auth credentials below) so
    // hasPendingWeight()/getPendingGrams() are cheap enough to call every
    // loop() cycle without touching flash each time.
    bool     _localStorageReady;
    bool     _hasPendingWeight;
    float    _pendingGrams;
    uint32_t _pendingCapturedAtEpoch;
    uint32_t _pendingUptimeMs;
    String   _pendingMeasurementId;

    // True only while the currently-staged pending measurement was captured
    // during THIS running process (a real stagePendingWeight() call, not one
    // recovered from flash by loadPendingWeightFromDisk() at boot). This is
    // what proves - not just assumes - that _pendingUptimeMs is directly
    // comparable to millis() right now, which is what makes reconstructing
    // capturedAt from elapsed uptime in syncPendingWeight() safe. Never
    // persisted to flash: it must default to false on every fresh boot, and
    // loadPendingWeightFromDisk() only ever runs once per boot (see
    // beginLocalStorage()'s idempotency guard), so this is set correctly by
    // construction, not by tracking reboots explicitly.
    bool _pendingCapturedThisBoot;

    void loadPendingWeightFromDisk();
    void clearPendingWeightFile();

    // Offline-safe, collision-free measurementId: "HS_<chipId>_<sequence>".
    // Advances (and durably persists) the sequence counter as its first
    // step, before this call can fail for any other reason - see the .cpp
    // for why a gap is acceptable here but reuse is not. Returns false (and
    // leaves outId untouched) only if the counter itself could not be
    // durably advanced, in which case the caller must not stage a
    // measurement at all rather than risk a reused/unstable id.
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
