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
    if (!loadCredentials(_ssid, _password))
    {
        // No saved credentials at all - nothing to retry in the background,
        // so go straight to the setup portal. Deliberately non-blocking
        // from here: this device has a fully functional local job
        // (weighing) that does not depend on WiFi or Firebase, so it must
        // not sit parked waiting for someone to submit credentials.
        // startProvisioningPortal() returns immediately; update() (called
        // every loop() iteration) services it in the background while
        // setup() continues on into warm-up/tare/weighing.
        Serial.println("[WIFI] No saved credentials - starting setup portal (scale continues offline)");
        if (onProvisioningStart != nullptr) { onProvisioningStart(); }
        startProvisioningPortal();
        return false;
    }

    if (attemptConnection())
    {
        _disconnectedSince = 0;
        return true;
    }

    // Saved credentials exist but couldn't connect right now (router off,
    // password changed after a router swap, temporarily out of range...).
    // Do NOT jump straight to the setup portal on a single failed attempt -
    // this device only gives up on saved credentials after ~30s of
    // CONTINUED failure, handled by pollReconnect() from loop() onward (see
    // update()). Scale continues in offline/local mode in the meantime.
    Serial.println("[WIFI] Connection unavailable — local weighing remains active.");
    _disconnectedSince = millis();
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

    pollReconnect();
}

bool NetworkManager::isProvisioning() const
{
    return _provisioning;
}

void NetworkManager::setPendingMeasurementProvider(
    std::function<bool()>   hasPending,
    std::function<float()>  getGrams,
    std::function<String()> getMeasurementId,
    std::function<bool()>   discard
)
{
    _hasPendingMeasurement     = hasPending;
    _getPendingGrams           = getGrams;
    _getPendingMeasurementId   = getMeasurementId;
    _discardPendingMeasurement = discard;
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

// ============================================================
// NON-BLOCKING RECONNECT
// ============================================================
//
// The previous version of this method blocked for up to MAX_RETRIES *
// RETRY_DELAY_MS (~10s) inside a delay() loop, called unconditionally from
// update() every loop() iteration once a 30s cooldown elapsed - meaning a
// missing router could stall the ENTIRE sketch, including active weighing,
// for up to 10 seconds at a time. Fixed here by never blocking at all:
// WiFi.begin() itself returns immediately on ESP8266 (the actual
// association happens in the WiFi stack's own background task), so this
// just issues that call and lets later update()/pollReconnect() calls poll
// isConnected() - detection latency is at most one loop() iteration
// (currently ~300ms), not a synchronous wait.
//

void NetworkManager::pollReconnect()
{
    if (_ssid.isEmpty()) { return; } // never had credentials - connect() already routed this case to the portal directly

    if (isConnected())
    {
        // Logged only on the TRANSITION back to connected (not every poll)
        // - _disconnectedSince being non-zero is exactly "we were
        // disconnected a moment ago."
        if (_disconnectedSince != 0)
        {
            Serial.println("[WIFI] Connected.");
        }
        _disconnectedSince = 0;
        return;
    }

    unsigned long now = millis();

    // Logged only on the TRANSITION into disconnected, not every poll -
    // this is the one line proving to anyone watching Serial that a lost
    // connection does NOT stop local weighing.
    if (_disconnectedSince == 0)
    {
        _disconnectedSince = now;
        Serial.println("[WIFI] Connection unavailable — local weighing remains active.");
    }

    // ~30s of continued failure to reconnect with saved credentials -> fall
    // back to the same setup portal a from-scratch boot would open, so
    // obsolete credentials (router replaced, password changed) are
    // recoverable without a manual reflash. Checked before the retry
    // cooldown below so it still fires even on a cycle that would otherwise
    // also be due for another WiFi.begin() attempt.
    if (now - _disconnectedSince >= RECONNECT_TIMEOUT_BEFORE_PORTAL_MS)
    {
        Serial.println("[WIFI] Starting Basilience-Scale-Setup.");
        startProvisioningPortal();
        return;
    }

    // Not logged per-attempt (unlike the rest of this method's transition-
    // only logging, this would otherwise repeat every RECONNECT_RETRY_
    // INTERVAL_MS for the whole outage) - the connect/disconnect
    // transition lines above already say everything useful here.
    if (now - _lastReconnectAttempt < RECONNECT_RETRY_INTERVAL_MS) { return; }
    _lastReconnectAttempt = now;

    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid.c_str(), _password.c_str());
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

    _disconnectedSince = 0; // no longer meaningful once we're in provisioning mode

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

        Serial.println("[WIFI] New credentials received.");

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

        Serial.print("[AP HTTP] Verifying new network before saving: ");
        Serial.println(newSsid);

        // Validate BEFORE persisting - do not overwrite still-possibly-
        // usable saved credentials with ones that turn out to be wrong (a
        // typo'd password, a network that's out of range from where the
        // portal's being set up, etc). WIFI_AP_STA keeps THIS portal
        // connection itself alive on the AP interface while the STA side
        // tests the new network, so a response can be sent back either way
        // - a plain WiFi.mode(WIFI_STA) here would drop the phone's
        // connection to the AP before it could ever receive it.
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(newSsid.c_str(), newPassword.c_str());

        uint8_t retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < MAX_RETRIES)
        {
            delay(RETRY_DELAY_MS);
            ESP.wdtFeed();
            retries++;
            yield();
        }

        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("[AP HTTP] New network could not be verified - not saved, portal remains open");
            WiFi.mode(WIFI_AP); // drop the failed STA attempt, keep serving the portal as AP-only
            _server.send(200, "text/html", buildSetupFailureHtml());
            return;
        }

        Serial.println("[AP HTTP] New network verified");

        if (!saveCredentials(newSsid, newPassword))
        {
            Serial.println("[AP HTTP] Unable to persist verified credentials");
            WiFi.mode(WIFI_AP);
            _server.send(500, "text/plain", "Verified but unable to save credentials - try again");
            return;
        }

        Serial.println("[AP] Credentials verified and saved");
        _server.send(200, "text/html", buildSetupSuccessHtml());

        delay(1000);
        ESP.restart();
    });

    _server.on("/status", HTTP_GET, [this]() {
        _server.send(200, "application/json", "{\"status\":\"setup_mode\"}");
    });

    // View-only - shows the stuck measurement (if any) so a person can see
    // it before deciding whether to discard it. GET is safe/idempotent
    // here on purpose: the actual destructive action lives on the POST
    // route below, not here, so a browser prefetch or anything else that
    // might issue a stray GET can never discard real data by accident.
    _server.on("/pending", HTTP_GET, [this]() {
        _server.send(200, "text/html", buildPendingViewHtml());
    });

    // The one and only way a pending measurement is ever discarded without
    // actually syncing - see FirebaseManager::discardPendingMeasurement()'s
    // comment for why this must stay a deliberate POST from a person
    // looking at /pending, never automatic.
    _server.on("/pending/discard", HTTP_POST, [this]() {
        Serial.println("[AP HTTP] POST /pending/discard");

        bool discarded = _discardPendingMeasurement ? _discardPendingMeasurement() : false;

        if (!discarded)
        {
            Serial.println("[AP HTTP] Nothing pending to discard");
        }

        _server.send(200, "text/html", buildPendingDiscardedHtml());
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
        // Pending-measurement banner on the main setup page, and the
        // /pending view/discard page's own value display + danger action -
        // same card chrome, distinct enough coloring to read as "something
        // needs your attention" without a whole second visual language.
        ".notice{background:#FFF6E0;border:1px solid #E8D9A8;border-radius:10px;"
        "padding:12px 14px;margin-bottom:18px;font-size:13px;color:#5C4A12;line-height:1.4}"
        ".notice a{color:#116F59;font-weight:600;text-decoration:none}"
        ".weight{font-size:36px;font-weight:700;text-align:center;margin:4px 0 4px;color:#0B3D33}"
        ".weight-id{font-size:12px;color:#2E4F46;text-align:center;margin-bottom:20px;word-break:break-all}"
        ".btn-danger{background:#B3261E}"
        ".btn-danger:active{background:#8C1D17}"
        ".btn-secondary{background:#fff;color:#116F59;border:1px solid #D8E3E0;margin-top:10px}"
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
        "<p class=\"sub\">Enter the Wi-Fi network this scale should join.</p>";

    // Surfaced here too, not just at /pending directly, so someone who
    // lands on this page (the captive-portal default) actually notices a
    // stuck measurement exists rather than needing to already know the URL.
    if (_hasPendingMeasurement && _hasPendingMeasurement())
    {
        body +=
            "<div class=\"notice\">A weighing is saved on this scale but hasn't reached "
            "the server yet. <a href=\"/pending\">View it</a></div>";
    }

    body +=
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

String NetworkManager::buildSetupFailureHtml() const
{
    String body =
        "<div class=\"badge badge--check\">&#33;</div>"
        "<h1>Couldn't Connect</h1>"
        "<p class=\"sub\">That network couldn't be reached. Double-check the name and password and try again "
        "- nothing was changed, and the scale is still weighing locally.</p>"
        "<a href=\"/\" style=\"display:block;text-align:center;color:#116F59;font-weight:600;text-decoration:none;margin-top:8px\">&larr; Try again</a>";

    return pageShell(body);
}

// Shows whatever's actually staged on flash right now (via the provider
// callbacks - see setPendingMeasurementProvider()), and offers the ONE
// deliberate way to discard it without it ever syncing. Reachable any time
// the setup portal happens to be serving, same as every other route here -
// that includes the ~30s auto-fallback case (NetworkManager::pollReconnect()),
// not just a from-scratch unprovisioned boot.
String NetworkManager::buildPendingViewHtml() const
{
    bool hasPending = _hasPendingMeasurement && _hasPendingMeasurement();

    if (!hasPending)
    {
        String body =
            "<h1>Nothing Pending</h1>"
            "<p class=\"sub\">There's no unsynced measurement waiting right now - either it "
            "already synced, or nothing's been weighed yet.</p>"
            "<a href=\"/\" style=\"display:block;text-align:center;color:#116F59;font-weight:600;text-decoration:none;margin-top:8px\">&larr; Back</a>";
        return pageShell(body);
    }

    float  grams = _getPendingGrams ? _getPendingGrams() : 0.0f;
    String id    = _getPendingMeasurementId ? _getPendingMeasurementId() : "";

    String body =
        "<h1>Pending Measurement</h1>"
        "<p class=\"sub\">Captured on this scale, not yet saved to the server.</p>"
        "<div class=\"weight\">" + String(grams, 1) + " g</div>"
        "<div class=\"weight-id\">" + id + "</div>"
        "<p class=\"sub\">This will sync automatically the moment the scale reconnects - "
        "nothing is lost by waiting. Only discard it if you're sure you don't need this "
        "weighing recorded.</p>"
        "<form method=\"POST\" action=\"/pending/discard\">"
        "<button type=\"submit\" class=\"btn-danger\">Discard This Measurement</button>"
        "</form>"
        "<a href=\"/\"><button type=\"button\" class=\"btn-secondary\">&larr; Back</button></a>";

    return pageShell(body);
}

String NetworkManager::buildPendingDiscardedHtml() const
{
    String body =
        "<div class=\"badge badge--check\">&#10003;</div>"
        "<h1>Discarded</h1>"
        "<p class=\"sub\">The pending measurement was removed. The scale is ready to weigh "
        "the next item.</p>"
        "<a href=\"/\" style=\"display:block;text-align:center;color:#116F59;font-weight:600;text-decoration:none;margin-top:8px\">&larr; Back</a>";

    return pageShell(body);
}
