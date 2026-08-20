#include "NetworkManager.h"

NetworkManager::NetworkManager(const char* ssid, const char* password)
    : _ssid(ssid), _password(password), _lastReconnectAttempt(0)
{
}

bool NetworkManager::connect()
{
    Serial.println();
    Serial.println("====================================");
    Serial.println(" WIFI CONNECTION");
    Serial.println("====================================");
    Serial.print("[WIFI] Connecting to: ");
    Serial.println(_ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid, _password);

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

    WiFi.begin(_ssid, _password);

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