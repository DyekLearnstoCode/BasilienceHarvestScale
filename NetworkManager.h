#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <ESP8266WiFi.h>

class NetworkManager
{
public:

    NetworkManager(const char* ssid, const char* password);

    bool connect();
    void disconnect();

    bool isConnected() const;
    bool reconnectIfNeeded();

    String getIPAddress() const;
    int    getSignalStrength() const;

private:

    const char* _ssid;
    const char* _password;

    static const uint8_t  MAX_RETRIES           = 20;
    static const uint16_t RETRY_DELAY_MS        = 500;
    static const uint32_t RECONNECT_COOLDOWN_MS = 30000;

    unsigned long _lastReconnectAttempt = 0;
};

#endif // NETWORK_MANAGER_H