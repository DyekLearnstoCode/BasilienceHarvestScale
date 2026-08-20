#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ============================================================
// DISPLAY MANAGER
// 16x2 I2C LCD via PCF8574 backpack
//
// Wiring (ESP8266):
//   VCC -> 3.3V
//   GND -> GND
//   SDA -> D2 (GPIO4)
//   SCL -> D1 (GPIO5)
//
// Common I2C addresses: 0x27 or 0x3F
// Use I2C scanner sketch to find yours.
// ============================================================

class DisplayManager
{
public:

    // --------------------------------------------------------
    // CONSTRUCTOR
    // --------------------------------------------------------

    DisplayManager(
        uint8_t i2cAddress = 0x27,
        uint8_t cols       = 16,
        uint8_t rows       = 2
    );

    // --------------------------------------------------------
    // INITIALIZATION
    // --------------------------------------------------------

    // Call once in setup().
    // Returns false if LCD not found on I2C bus.
    bool begin();

    bool isAvailable() const;

    // --------------------------------------------------------
    // SCREEN STATES
    // --------------------------------------------------------

    // Splash screen on power-on
    void showBoot();

    // During 60-second warm-up countdown
    void showWarmUp(unsigned long remainingSeconds);

    // While taring
    void showTaring();

    // After tare — before first reading
    void showReady();

    // Live weight display (called every reading interval)
    void showWeight(float weightGrams, float weightKg);

    // Error message (up to 16 chars per line)
    void showError(
        const String& line1,
        const String& line2 = ""
    );

    // --------------------------------------------------------
    // BACKLIGHT
    // --------------------------------------------------------

    void backlightOn();
    void backlightOff();

    // --------------------------------------------------------
    // UTILITY
    // --------------------------------------------------------

    void clear();

private:

    LiquidCrystal_I2C _lcd;
    uint8_t           _i2cAddress;
    uint8_t           _cols;
    uint8_t           _rows;
    bool              _available;

    // Print text centered on a given row.
    // Pads with spaces to overwrite stale characters.
    void printCentered(uint8_t row, const String& text);

    // Print left-aligned, padded to full row width.
    void printPadded(uint8_t row, const String& text);
};

#endif // DISPLAY_MANAGER_H