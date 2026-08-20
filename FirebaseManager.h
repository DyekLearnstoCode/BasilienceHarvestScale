#ifndef FIREBASE_MANAGER_H
#define FIREBASE_MANAGER_H

#include <Arduino.h>
#include <Firebase_ESP_Client.h>

// ============================================================
// FIREBASE MANAGER
// Firebase Realtime Database — weight data uploader
//
// Required library: "Firebase ESP Client" by Mobizt
// Install via Arduino Library Manager
//
// Database structure:
//
//   LoadCell        : float  ← live reading (overwrite every loop)
//   harvests/
//     {auto-id}/
//       grams  : float
//       kg     : float
//       millis : unsigned long
// ============================================================

class FirebaseManager
{
public:

    // --------------------------------------------------------
    // CONSTRUCTOR
    // --------------------------------------------------------
    //
    // apiKey       — Firebase project Web API Key
    // databaseURL  — https://basilience-database-default-rtdb
    //                .asia-southeast1.firebasedatabase.app
    // dbSecret     — Project Settings > Service Accounts >
    //                Database Secrets
    //

    FirebaseManager(
        const char* apiKey,
        const char* databaseURL,
        const char* dbSecret
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

    // Overwrites /LoadCell with current reading.
    // Fast set — no history, just current value.
    bool updateLiveWeight(float grams);

    // --------------------------------------------------------
    // HARVEST LOG  (call once per stable reading)
    // --------------------------------------------------------

    // Pushes a new entry to /harvests/ with auto-ID.
    // Only fires when weight is confirmed stable.
    bool uploadWeight(float grams, float kg);

    // --------------------------------------------------------
    // STATS
    // --------------------------------------------------------

    // Get total number of readings stored (optional).
    int getTotalReadings();

private:

    const char* _apiKey;
    const char* _databaseURL;
    const char* _dbSecret;

    FirebaseData   _fbData;
    FirebaseAuth   _auth;
    FirebaseConfig _config;

    bool     _ready;
    uint32_t _readingCount;

    static const char* BASE_PATH;
};

#endif // FIREBASE_MANAGER_H