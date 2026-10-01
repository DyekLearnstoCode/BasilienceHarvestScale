#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <functional>

// ============================================================
// NETWORK MANAGER
//
// SECURE WIFI PROVISIONING — mirrors the main BasilienceFirmware_V2
// (ESP32) pattern: this device no longer holds a real WiFi network
// name/password in compiled source (that password used to be a real
// home-router credential committed to git). Instead it persists
// credentials to flash (LittleFS) once entered, and whenever none are
// saved - or the saved network can't be reached - it opens its own
// "Basilience-Scale-Setup" access point with a captive-portal form at
// http://192.168.4.1/, matching the ESP32 firmware's Basilience-Setup
// AP + /setup route (a distinct AP name so both units can coexist).
// ============================================================

class NetworkManager
{
public:

    NetworkManager();

    // Loads saved credentials and connects (blocking for up to ~10s, the
    // same bounded retry the original firmware always did). If none are
    // saved, or the saved network can't be reached, this instead starts
    // the setup portal and returns immediately (false) - it does NOT
    // block waiting for someone to submit credentials. The load cell has
    // no dependency on WiFi/Firebase, so the caller (see the .ino's
    // setup()) continues on into warm-up/tare/weighing either way; only
    // cloud upload stays unavailable until the portal is used or a
    // reflash. onProvisioningStart (if given) is invoked once, right
    // before the portal starts serving, so the caller can update its own
    // UI (e.g. the LCD) - this class has no display dependency of its own.
    bool connect(void (*onProvisioningStart)() = nullptr);

    // Call every loop() iteration, REGARDLESS of weighing state - safe to do
    // so unconditionally. While the setup portal is active this services
    // its DNS/HTTP requests (non-blocking, as before); otherwise it defers
    // to pollReconnect(), which is now itself fully non-blocking (see that
    // method's comment) - never a multi-second stall here, so callers no
    // longer need to gate this on the load cell being idle.
    void update();

    bool isProvisioning() const;

    // --------------------------------------------------------
    // PENDING-MEASUREMENT PORTAL PAGE
    // --------------------------------------------------------
    //
    // This class owns the only web server this device ever runs (the setup
    // portal), but has no knowledge of FirebaseManager/the scale's own
    // pending-measurement concept - loose coupling, same as
    // onProvisioningStart above. The .ino registers these four small
    // callbacks once, in setup(), so that WHENEVER the portal happens to be
    // serving (not just during first-time setup - see pollReconnect()'s
    // ~30s fallback-to-portal behavior), a person connected to it can see
    // a stuck unsynced measurement and - as a deliberate, explicit action -
    // discard it, rather than the scale being unusable for a new weighing
    // with no recourse until connectivity happens to return. See
    // FirebaseManager::discardPendingMeasurement()'s own comment for why
    // this must stay a manual action, never automatic.
    void setPendingMeasurementProvider(
        std::function<bool()>   hasPending,
        std::function<float()>  getGrams,
        std::function<String()> getMeasurementId,
        std::function<bool()>   discard
    );

    void disconnect();

    bool isConnected() const;

    String getIPAddress() const;
    int    getSignalStrength() const;

private:

    bool attemptConnection();

    // Non-blocking reconnect - called from update() every loop() iteration.
    // Never calls delay(): WiFi.begin() itself returns immediately on
    // ESP8266 (association happens in the background), so this only ever
    // issues that call and polls WiFi.status() on later calls - no long
    // blocking retry loop here anymore. After ~30s of continued failure it
    // falls back to the same setup portal a from-scratch boot would open,
    // so obsolete credentials are recoverable without a reflash. See the
    // .cpp for the full reasoning.
    void pollReconnect();

    void startProvisioningPortal();
    void setupAPServer();

    // Shared page chrome (head/style/card wrapper) both setup-portal pages
    // are built on, so the two never drift out of visual sync and the CSS
    // exists in exactly one place.
    String pageShell(const String& bodyHtml) const;
    String buildSetupFormHtml() const;
    String buildSetupSuccessHtml() const;
    String buildSetupFailureHtml() const;
    String buildPendingViewHtml() const;
    String buildPendingDiscardedHtml() const;

    bool loadCredentials(String& ssid, String& password);
    bool saveCredentials(const String& ssid, const String& password);

    // Unset (empty std::function) until setPendingMeasurementProvider() is
    // called - every route/page that uses these checks first, so a device
    // whose .ino never registers them (shouldn't happen in practice, but
    // cheap to guard) just omits the pending-measurement UI entirely rather
    // than crashing on an empty std::function call.
    std::function<bool()>   _hasPendingMeasurement;
    std::function<float()>  _getPendingGrams;
    std::function<String()> _getPendingMeasurementId;
    std::function<bool()>   _discardPendingMeasurement;

    String _ssid;
    String _password;

    ESP8266WebServer _server{80};
    DNSServer        _dnsServer;
    bool             _provisioning = false;

    static const uint8_t  MAX_RETRIES           = 20;
    static const uint16_t RETRY_DELAY_MS        = 500;

    // Cooldown between individual non-blocking WiFi.begin() attempts while
    // disconnected - frequent enough to reconnect quickly once the router
    // is back, not so frequent it spams the radio with begin() calls.
    static const uint32_t RECONNECT_RETRY_INTERVAL_MS = 5000;

    // ~30s of continued failure to reconnect with saved credentials before
    // falling back to the setup portal (Part D's "unable to reconnect for
    // approximately 30 seconds" requirement).
    static const uint32_t RECONNECT_TIMEOUT_BEFORE_PORTAL_MS = 30000;

    unsigned long _lastReconnectAttempt = 0;

    // 0 = currently connected (or not yet tracking a disconnect). Set the
    // moment isConnected() is first observed false in pollReconnect(), so
    // RECONNECT_TIMEOUT_BEFORE_PORTAL_MS is measured from the actual start
    // of this disconnection, not from some fixed boot-time reference.
    unsigned long _disconnectedSince = 0;
};

#endif // NETWORK_MANAGER_H
