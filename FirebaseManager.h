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
//     lastSeen   : epoch seconds of the last liveWeight write - see
//                  updateLiveWeight() below.
//     harvests/
//       {measurementId}/    ← child key IS the canonical measurementId,
//                              generated the instant a measurement is
//                              CAPTURED (staged to flash), regardless of
//                              whether WiFi/Firebase happen to be available
//                              at that moment - see "MEASUREMENT ID" below.
//                              Written with setJSON() - never pushJSON() -
//                              so every retry of the SAME measurement (an
//                              immediate attempt that failed, or a later
//                              one once connectivity returns) lands on the
//                              exact same node instead of creating a
//                              duplicate. Also stays the SAME node across a
//                              RESTACK (weight added/removed on the same
//                              physical session without a full clear - see
//                              the .ino's RESTACK DETECTION) via
//                              restagePendingMeasurement(): one record per
//                              physical session, always overwritten to the
//                              latest total, never two entries competing
//                              for "the" current weight.
//         grams      : float
//         kg         : float
//         capturedAt : epoch seconds when the physical measurement was
//                      confirmed - 0 only when the scale's clock genuinely
//                      wasn't synced yet at capture time AND it couldn't be
//                      safely reconstructed later either (see
//                      syncPendingMeasurement() in the .cpp). Never a
//                      fabricated value.
//         syncedAt   : epoch seconds when this entry was actually WRITTEN
//                      here - can be well after capturedAt if the device
//                      was offline in between.
//         uptimeMs   : device uptime (millis()) at capture, for diagnostics
//         measurementId : the same value as the RTDB key itself, duplicated
//                      into the record for convenience.
//
// CAPTURE ≠ UPLOAD, WITH RETRY — a confirmed physical weighing is persisted
// to flash FIRST (stagePendingMeasurement()), before network availability
// enters the picture at all, and is only cleared from flash once RTDB
// actually confirms the write (syncPendingMeasurement()). One slot only:
// this is a standalone single-platform scale, not a multi-item offline
// queue - a second physical load cannot be captured while an earlier one
// is still unsynced (see the .ino's CAPTURE block and STABILITY TRACKING
// gate). An unsynced measurement survives indefinitely on flash, including
// across a reboot, and is retried automatically every time Firebase is
// ready - see the .ino's PENDING SYNC RETRY block.
//
// MEASUREMENT ID — generated the instant a measurement is CAPTURED (staged
// to flash), independent of network state: "HS_<chipId>_<sequence>", where
// chipId is this unit's own ESP.getChipId() (fixed in silicon, needs no
// network) and sequence is a monotonic counter persisted to flash
// independently of any individual measurement, so a gap (a capture that
// then failed to even persist) is harmless but a value is never reused.
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
    // Mounts LittleFS (needed for device-identity credentials, the
    // measurement-sequence counter, and the pending-measurement record),
    // recovers any pending measurement left over from a previous boot (see
    // stagePendingMeasurement() below), and clears out the now-unused
    // offline-QUEUE format an earlier firmware revision briefly used - see
    // clearLegacyQueueData() in the .cpp. Independent of WiFi/Firebase
    // entirely - a confirmed physical measurement must be persistable even
    // on a unit that never reaches the internet at all. Idempotent: safe to
    // call again (from begin() below, which also needs LittleFS) without
    // remounting.
    bool beginLocalStorage();

    // --------------------------------------------------------
    // LIVE WEIGHT  (call every loop)
    // --------------------------------------------------------

    // Updates devices/{deviceId}/harvestScale/liveWeight AND lastSeen
    // (epoch seconds this write happened, or 0 if the clock isn't synced)
    // together in one partial PATCH - no history, just current values,
    // and harvests/ and every other sibling child is left untouched.
    // lastSeen exists so a consumer can tell a genuinely live number apart
    // from one that's just sitting there because this device went offline
    // and can no longer push anything at all (this library has no RTDB
    // onDisconnect() support to clear it server-side the instant that
    // happens - checked, not available - so staleness has to be inferred
    // from how old lastSeen is instead).
    bool updateLiveWeight(float grams);

    // --------------------------------------------------------
    // PENDING MEASUREMENT  (capture ≠ upload, retried until synced)
    // --------------------------------------------------------
    //
    // A confirmed physical weighing is persisted to flash BEFORE it is
    // considered "captured," and stays persisted until Firebase actually
    // confirms the write - network availability must never determine
    // whether the physical measurement exists. One slot only (see the
    // header comment).

    // Validates, generates a stable measurementId, persists everything to
    // flash, and only THEN reports success - call this the instant a
    // measurement is confirmed, regardless of WiFi/Firebase state. Refuses
    // (returns false) if a measurement is already pending - the caller (the
    // .ino) is expected to check hasPendingMeasurement() itself before even
    // attempting a new capture, but this is the enforcement point that
    // actually matters.
    bool stagePendingMeasurement(float grams, uint32_t capturedAtEpochSec);

    // RESTACK: updates an ALREADY-CAPTURED session's still-active weighing
    // to a new total - weight added on top, or partially removed, while the
    // same physical session is still ongoing (see the .ino's RESTACK
    // DETECTION block). Deliberately reuses the EXACT SAME measurementId
    // (passed in, not generated) rather than minting a new one: a restack
    // is the SAME physical weighing becoming more current, not a second,
    // competing one. This is what guarantees "whoever's most up to date
    // wins" on the app side - there is only ever ONE RTDB record for this
    // session, always overwritten in place via setJSON() in
    // syncPendingMeasurement(), so the app can never be offered a stale
    // intermediate total that's still sitting around unconsumed next to a
    // newer one.
    //
    // Refuses (returns false) if a DIFFERENT measurementId is currently
    // pending - that would mean the .ino's own session bookkeeping is
    // inconsistent with this class's state, and overwriting someone else's
    // pending record would be a real bug, not a safe no-op. Also refuses on
    // an empty measurementId. Same flash-persistence-first guarantee as
    // stagePendingMeasurement(): only reports success once durably written.
    bool restagePendingMeasurement(float grams, uint32_t capturedAtEpochSec, const String& measurementId);

    bool hasPendingMeasurement() const;

    // What's currently staged - for the LCD's "PENDING" screen and for
    // reconstructing the same weight-line text once a later sync succeeds.
    // Meaningless (returns 0 / empty) when hasPendingMeasurement() is false.
    float  getPendingGrams() const;
    String getPendingMeasurementId() const;

    // Writes ONE RTDB entry, via setJSON() (never pushJSON()), to
    // devices/{deviceId}/harvestScale/harvests/{measurementId} - the FIXED
    // path this measurement's own stable id resolves to, so calling this
    // again after an earlier failed/uncertain attempt overwrites the exact
    // same node instead of creating a duplicate. Synchronous: blocks for the
    // duration of one RTDB write - call only when isReady() (checked
    // internally too). Clears the pending record ONLY once RTDB confirms
    // the write succeeded; on any failure (including one where the write
    // may have actually landed but the response was lost) the pending
    // record is left untouched on flash for a later retry. No-op (returns
    // false) if nothing is pending or Firebase isn't ready.
    bool syncPendingMeasurement();

    // Explicit, deliberate data loss - the ONLY other way a pending
    // measurement ever leaves flash besides syncPendingMeasurement()
    // actually confirming the write. For when connectivity genuinely isn't
    // coming back (or the operator just wants the scale usable again right
    // now) and someone has consciously decided this one physical weighing
    // doesn't need to reach Firebase after all. Call sites must make this a
    // deliberate user action (e.g. a confirm step on the setup portal page
    // - see NetworkManager), never automatic/time-based - an unattended
    // timeout silently discarding a real harvest would defeat the entire
    // point of this class. No-op (returns false) if nothing is pending.
    bool discardPendingMeasurement();

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

    // Pending-measurement state, mirrored between RAM and small flat
    // LittleFS files (same pattern already used for the auth credentials
    // below) so hasPendingMeasurement()/getPendingGrams() are cheap enough
    // to call every loop() cycle without touching flash each time.
    bool     _hasPendingMeasurement;
    float    _pendingGrams;
    uint32_t _pendingCapturedAtEpoch;
    uint32_t _pendingUptimeMs;
    String   _pendingMeasurementId;

    // True only while the currently-staged pending measurement was captured
    // during THIS running process (a real stagePendingMeasurement() call,
    // not one recovered from flash by loadPendingMeasurementFromDisk() at
    // boot). This is what proves - not just assumes - that _pendingUptimeMs
    // is directly comparable to millis() right now, which is what makes
    // reconstructing capturedAt from elapsed uptime in
    // syncPendingMeasurement() safe. Never persisted to flash: it must
    // default to false on every fresh boot, and loadPendingMeasurementFromDisk()
    // only ever runs once per boot (see beginLocalStorage()'s idempotency
    // guard), so this is set correctly by construction, not by tracking
    // reboots explicitly.
    bool _pendingCapturedThisBoot;

    void loadPendingMeasurementFromDisk();
    void clearPendingMeasurementFile();

    // One-time cleanup of the offline-QUEUE format a brief earlier firmware
    // revision used (one file per queued measurement) - that specific
    // multi-item-queue design is not what's being reintroduced here (this
    // class still deliberately keeps the simpler single-pending-slot
    // design), so any leftover queue files from that revision must not be
    // mistaken for anything live. Safe to call every boot: a no-op once
    // nothing legacy is left on flash. See the .cpp for the exact
    // filenames/paths involved.
    void clearLegacyQueueData();

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
