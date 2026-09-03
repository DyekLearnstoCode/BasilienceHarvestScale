#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>

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

    // Call every loop() iteration. While the setup portal is active this
    // services its DNS/HTTP requests (non-blocking); otherwise it defers
    // to reconnectIfNeeded()'s own cooldown-gated retry.
    void update();

    bool isProvisioning() const;

    void disconnect();

    bool isConnected() const;
    bool reconnectIfNeeded();

    String getIPAddress() const;
    int    getSignalStrength() const;

private:

    bool attemptConnection();
    void startProvisioningPortal();
    void setupAPServer();

    // Shared page chrome (head/style/card wrapper) both setup-portal pages
    // are built on, so the two never drift out of visual sync and the CSS
    // exists in exactly one place.
    String pageShell(const String& bodyHtml) const;
    String buildSetupFormHtml() const;
    String buildSetupSuccessHtml() const;

    bool loadCredentials(String& ssid, String& password);
    bool saveCredentials(const String& ssid, const String& password);

    String _ssid;
    String _password;

    ESP8266WebServer _server{80};
    DNSServer        _dnsServer;
    bool             _provisioning = false;

    static const uint8_t  MAX_RETRIES           = 20;
    static const uint16_t RETRY_DELAY_MS        = 500;
    static const uint32_t RECONNECT_COOLDOWN_MS = 30000;

    unsigned long _lastReconnectAttempt = 0;
};

#endif // NETWORK_MANAGER_H
