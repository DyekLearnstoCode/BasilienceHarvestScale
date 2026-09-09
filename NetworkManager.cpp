#include "NetworkManager.h"

#include <LittleFS.h>
#include "LogoAsset.h"

namespace
{

const char* WIFI_SSID_FILE     = "/wifi_ssid.txt";
const char* WIFI_PASSWORD_FILE = "/wifi_password.txt";

// 802.11 hard limit (SSID) and WPA2-Personal's own valid passphrase range
// (RSN, IEEE 802.11-2016 sec. 9.4.2.2 / 12.7.2) - not arbitrary choices.
// Enforced server-side, not just as an HTML maxlength hint, since a raw
// POST to /setup bypasses the form entirely: an unbounded string here
// gets read fully into heap by the web server before any of our own code
// runs, on a chip with only ~40-50KB of it free once WiFi + the portal's
// own DNS/HTTP servers are resident - the practical concern isn't that a
// garbage SSID does anything dangerous once stored (WiFi.begin() just
// fails to associate with it, and it's never echoed into any HTML this
// device serves), it's that an oversized one is a cheap way to crash the
// device via heap exhaustion.
constexpr size_t MAX_SSID_LEN         = 32;
constexpr size_t MIN_WPA2_PASSWORD_LEN = 8;
constexpr size_t MAX_PASSWORD_LEN      = 63;

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

bool writeTextFile(const char* path, const String& value)
{
    File f = LittleFS.open(path, "w");
    if (!f) { return false; }
    f.print(value);
    f.close();
    return true;
}

} // namespace

NetworkManager::NetworkManager()
{
}

bool NetworkManager::loadCredentials(String& ssid, String& password)
{
    if (!LittleFS.begin()) { return false; }

    ssid = readTextFile(WIFI_SSID_FILE);
    password = readTextFile(WIFI_PASSWORD_FILE);

    // An empty password is valid for an open target network; an empty
    // SSID means no credentials have ever been saved.
    return !ssid.isEmpty();
}

bool NetworkManager::saveCredentials(const String& ssid, const String& password)
{
    if (!LittleFS.begin()) { return false; }

    if (!writeTextFile(WIFI_SSID_FILE, ssid)) { return false; }
    // A failed password write with a non-empty password would leave a
    // stale/mismatched password file behind an already-saved new SSID -
    // treat that as a failure too rather than silently keeping the old one.
    if (password.length() > 0 && !writeTextFile(WIFI_PASSWORD_FILE, password)) { return false; }
    if (password.length() == 0) { writeTextFile(WIFI_PASSWORD_FILE, ""); }

    return true;
}

bool NetworkManager::connect(void (*onProvisioningStart)())
{
    if (loadCredentials(_ssid, _password) && attemptConnection())
    {
        return true;
    }

    // Deliberately non-blocking from here: this device has a fully
    // functional local job (weighing) that does not depend on WiFi or
    // Firebase, so it must not sit parked waiting for someone to submit
    // credentials. startProvisioningPortal() returns immediately; update()
    // (called every loop() iteration) services it in the background while
    // setup() continues on into warm-up/tare/weighing.
    Serial.println("[WIFI] No usable saved network - starting setup portal (scale continues offline)");
    if (onProvisioningStart != nullptr) { onProvisioningStart(); }
    startProvisioningPortal();
    return false;
}

void NetworkManager::update()
{
    if (_provisioning)
    {
        _dnsServer.processNextRequest();
        _server.handleClient();
        return;
    }

    reconnectIfNeeded();
}

bool NetworkManager::isProvisioning() const
{
    return _provisioning;
}

bool NetworkManager::attemptConnection()
{
    Serial.println();
    Serial.println("====================================");
    Serial.println(" WIFI CONNECTION");
    Serial.println("====================================");
    Serial.print("[WIFI] Connecting to: ");
    Serial.println(_ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid.c_str(), _password.c_str());

    uint8_t retries = 0;

    while (WiFi.status() != WL_CONNECTED && retries < MAX_RETRIES)
    {
        delay(RETRY_DELAY_MS);
        ESP.wdtFeed();
        Serial.print(".");
        retries++;
        yield();
    }

    Serial.println();

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WIFI] ERROR: Failed to connect.");
        Serial.print("[WIFI] SSID tried: ");
        Serial.println(_ssid);
        return false;
    }

    Serial.println("[WIFI] Connected!");
    Serial.print("[WIFI] IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] Signal: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.println("====================================");

    return true;
}

void NetworkManager::disconnect()
{
    WiFi.disconnect();
    Serial.println("[WIFI] Disconnected.");
}

bool NetworkManager::isConnected() const
{
    return WiFi.status() == WL_CONNECTED;
}

bool NetworkManager::reconnectIfNeeded()
{
    if (isConnected()) { return true; }

    // Only retry every 30 seconds — prevents watchdog crash
    unsigned long now = millis();
    if (now - _lastReconnectAttempt < RECONNECT_COOLDOWN_MS)
    {
        return false;
    }

    _lastReconnectAttempt = now;

    Serial.println("[WIFI] Reconnecting...");

    WiFi.begin(_ssid.c_str(), _password.c_str());

    uint8_t retries = 0;

    while (WiFi.status() != WL_CONNECTED && retries < MAX_RETRIES)
    {
        delay(RETRY_DELAY_MS);
        ESP.wdtFeed();
        retries++;
        yield();
    }

    if (isConnected())
    {
        Serial.println("[WIFI] Reconnected.");
        return true;
    }

    Serial.println("[WIFI] Reconnect failed.");
    return false;
}

String NetworkManager::getIPAddress() const
{
    return WiFi.localIP().toString();
}

int NetworkManager::getSignalStrength() const
{
    return WiFi.RSSI();
}

// ============================================================
// SETUP PORTAL — "Basilience-Scale-Setup" captive-portal AP
// ============================================================
//
// Non-blocking, matching the ESP32 firmware's own reasoning for why its
// provisioning AP never blocks loop(): there is real local work (here,
// weighing; there, plant safety automation) that must keep running while
// unprovisioned. update() services this every loop() iteration instead.
//

void NetworkManager::startProvisioningPortal()
{
    Serial.println();
    Serial.println("====================================");
    Serial.println(" WIFI SETUP PORTAL");
    Serial.println("====================================");

    WiFi.mode(WIFI_AP);
    const IPAddress apIp(192, 168, 4, 1);
    const IPAddress gateway(192, 168, 4, 1);
    const IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIp, gateway, subnet);

    if (!WiFi.softAP("Basilience-Scale-Setup"))
    {
        Serial.println("[AP] ERROR: Failed to start Basilience-Scale-Setup");
        return;
    }

    delay(500); // Wait for AP to initialize

    _dnsServer.start(53, "*", WiFi.softAPIP());
    setupAPServer();
    _server.begin();

    _provisioning = true;

    Serial.println("[AP] Connect to Wi-Fi network 'Basilience-Scale-Setup'");
    Serial.print("[AP] Then browse to: http://");
    Serial.println(WiFi.softAPIP());
    Serial.println("[AP] Waiting for setup submission (scale keeps weighing locally)...");
}

void NetworkManager::setupAPServer()
{
    _server.on("/setup", HTTP_POST, [this]() {
        Serial.println("[AP HTTP] POST /setup");

        if (!_server.hasArg("ssid") || _server.arg("ssid").isEmpty())
        {
            Serial.println("[AP HTTP] Invalid setup request");
            _server.send(400, "text/plain", "SSID cannot be empty");
            return;
        }

        String newSsid = _server.arg("ssid");
        String newPassword = _server.hasArg("password") ? _server.arg("password") : "";

        if (newSsid.length() > MAX_SSID_LEN)
        {
            Serial.println("[AP HTTP] Rejected: SSID too long");
            _server.send(400, "text/plain", "SSID must be 32 characters or fewer");
            return;
        }

        // An empty password means "open network" and is valid; anything
        // else must fall inside WPA2-Personal's own passphrase range - a
        // too-short value isn't a password anyone actually meant to set,
        // and WiFi.begin() would just fail to associate with it anyway.
        if (newPassword.length() > 0 &&
            (newPassword.length() < MIN_WPA2_PASSWORD_LEN || newPassword.length() > MAX_PASSWORD_LEN))
        {
            Serial.println("[AP HTTP] Rejected: password out of range");
            _server.send(400, "text/plain", "Password must be 8-63 characters, or blank for an open network");
            return;
        }

        Serial.print("[AP HTTP] SSID received: ");
        Serial.println(newSsid);

        if (!saveCredentials(newSsid, newPassword))
        {
            Serial.println("[AP HTTP] Unable to persist credentials");
            _server.send(500, "text/plain", "Unable to save credentials");
            return;
        }

        Serial.println("[AP] Credentials saved");
        _server.send(200, "text/html", buildSetupSuccessHtml());

        delay(1000);
        ESP.restart();
    });

    _server.on("/status", HTTP_GET, [this]() {
        _server.send(200, "application/json", "{\"status\":\"setup_mode\"}");
    });

    // Minimal captive-portal form, served at the root so a phone's
    // automatic captive-portal browser lands on something usable without
    // a companion app (this standalone device, unlike the ESP32 units,
    // has no matching Android setup screen driving /setup for it).
    _server.on("/", HTTP_GET, [this]() {
        _server.send(200, "text/html", buildSetupFormHtml());
    });

    // Every OS uses its own different probe URL to detect "is this
    // network actually open" (Android: /generate_204, iOS/macOS:
    // /hotspot-detect.html, Windows: /connecttest.txt, and others) -
    // rather than enumerating and special-casing each one, a plain
    // redirect to "/" for every unmatched request is the standard
    // technique (used by the official ESP8266 captive-portal examples)
    // that reliably triggers each OS's own sign-in prompt: those probes
    // specifically look for an unexpected REDIRECT, not just different
    // response content served with a plain 200 - which is what serving
    // the form directly from here previously did, and why the automatic
    // popup wasn't appearing even though the server itself was working.
    _server.onNotFound([this]() {
        _server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        _server.send(302, "text/plain", "");
    });
}

// Shared head/style/card wrapper for both setup-portal pages. Matches the
// Android app's own palette (colors.xml: primary #116F59, text_dark
// #0B3D33, nav_inactive #2E4F46, snow #FFFAFA) so this device's own UI
// doesn't look like a bare, unbranded utility page. System font stack
// only - no external fonts/CDN, since this page is served with no real
// internet access behind it.
String NetworkManager::pageShell(const String& bodyHtml) const
{
    return
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Basilience Scale Setup</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
        "background:#FFFAFA;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
        "color:#0B3D33;padding:24px 16px}"
        ".card{width:100%;max-width:360px;background:#fff;border-radius:16px;"
        "box-shadow:0 4px 24px rgba(11,61,51,.12);padding:32px 28px;text-align:left}"
        // Celadon (colors.xml #ACE1AF) as the badge backdrop - a light
        // enough tint that the logo's own dark-to-mid greens (#11320B/
        // #2E691C/#4D8C35) stay legible on top of it, unlike the previous
        // solid primary-teal badge which was designed around a plain
        // white letter, not a multi-tone illustrated mark.
        ".badge{width:88px;height:88px;border-radius:50%;background:#ACE1AF;"
        "display:flex;align-items:center;justify-content:center;margin:0 auto 16px;overflow:hidden}"
        ".badge svg{width:60px;height:60px}"
        ".badge.badge--check{font-size:32px;font-weight:700;color:#116F59}"
        "h1{font-size:20px;margin:0 0 4px;text-align:center}"
        "p.sub{margin:0 0 24px;font-size:14px;color:#2E4F46;line-height:1.4;text-align:center}"
        "label{display:block;font-size:13px;font-weight:600;color:#2E4F46;margin-bottom:6px}"
        "input[type=text],input[type=password]{width:100%;padding:12px 14px;margin-bottom:18px;"
        "border:1px solid #D8E3E0;border-radius:10px;font-size:15px;color:#0B3D33;background:#FFFAFA}"
        "input[type=text]:focus,input[type=password]:focus{outline:none;border-color:#116F59}"
        "button{width:100%;padding:14px;border:none;border-radius:10px;background:#116F59;color:#fff;"
        "font-size:15px;font-weight:600}"
        "button:active{background:#0B3D33}"
        ".hint{margin-top:16px;font-size:12px;color:#2E4F46;text-align:center}"
        "</style></head><body><div class=\"card\">"
        + bodyHtml +
        "</div></body></html>";
}

String NetworkManager::buildSetupFormHtml() const
{
    // FPSTR() is the correct way to build a String from a PROGMEM char
    // array on this core - a plain String(LOGO_SVG) would misread it as a
    // regular RAM pointer instead of a flash address.
    String body =
        "<div class=\"badge\">" + String(FPSTR(LOGO_SVG)) + "</div>"
        "<h1>Harvest Scale Setup</h1>"
        "<p class=\"sub\">Enter the Wi-Fi network this scale should join.</p>"
        "<form method=\"POST\" action=\"/setup\">"
        "<label for=\"ssid\">Network name (SSID)</label>"
        "<input id=\"ssid\" name=\"ssid\" type=\"text\" maxlength=\"32\" autocapitalize=\"off\" autocorrect=\"off\" required>"
        "<label for=\"password\">Password</label>"
        "<input id=\"password\" name=\"password\" type=\"password\" maxlength=\"63\">"
        "<button type=\"submit\">Connect</button>"
        "</form>"
        "<div class=\"hint\">The scale keeps weighing locally while you set this up.</div>";

    return pageShell(body);
}

String NetworkManager::buildSetupSuccessHtml() const
{
    // A checkmark, not the full illustrated logo - "success" and "this is
    // Basilience" are different messages, and the checkmark is the more
    // immediately legible one for a confirmation screen.
    String body =
        "<div class=\"badge badge--check\">&#10003;</div>"
        "<h1>Saved</h1>"
        "<p class=\"sub\">Restarting the scale so it can join your Wi-Fi network...</p>";

    return pageShell(body);
}
