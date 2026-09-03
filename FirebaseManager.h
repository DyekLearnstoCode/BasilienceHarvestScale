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
//       {auto-id}/
//         grams  : float
//         kg     : float
//         millis : unsigned long
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
    // LIVE WEIGHT  (call every loop)
    // --------------------------------------------------------

    // Overwrites devices/{deviceId}/harvestScale/liveWeight with the
    // current reading. Fast set — no history, just current value.
    bool updateLiveWeight(float grams);

    // --------------------------------------------------------
    // HARVEST LOG  (call once per stable reading)
    // --------------------------------------------------------

    // Pushes a new entry to devices/{deviceId}/harvestScale/harvests
    // with an auto-ID. Only fires when weight is confirmed stable.
    bool uploadWeight(float grams, float kg);

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
