#ifndef LOAD_CELL_MANAGER_H
#define LOAD_CELL_MANAGER_H

#include <Arduino.h>
#include <HX711.h>

// ============================================================
// LOAD CELL MANAGER
// ============================================================

class LoadCellManager
{
public:

    // --------------------------------------------------------
    // CONSTRUCTOR
    // --------------------------------------------------------

    LoadCellManager(
        uint8_t doutPin,
        uint8_t sckPin
    );

    // --------------------------------------------------------
    // INITIALIZATION
    // --------------------------------------------------------

    bool begin(
        unsigned long timeoutMs = 1000
    );

    // --------------------------------------------------------
    // STATUS
    // --------------------------------------------------------

    bool isReady();

    bool isAvailable() const;

    bool waitUntilReady(
        unsigned long timeoutMs
    );

    // --------------------------------------------------------
    // RAW READINGS
    // --------------------------------------------------------

    bool readRaw(
        long &value,
        unsigned long timeoutMs = 1000
    );

    bool readRawAverage(
        uint8_t samples,
        long &value,
        unsigned long timeoutMs = 1000
    );

    // --------------------------------------------------------
    // TARE
    // --------------------------------------------------------

    bool tare(
        uint8_t samples = 20,
        unsigned long timeoutMs = 1000
    );

    long getOffset();

    void setOffset(
        long offset
    );

    // --------------------------------------------------------
    // CALIBRATION
    // --------------------------------------------------------

    void setCalibrationFactor(
        float factor
    );

    float getCalibrationFactor() const;

    // --------------------------------------------------------
    // WEIGHT
    // --------------------------------------------------------

    bool readWeightGrams(
        uint8_t samples,
        float &weightGrams,
        unsigned long timeoutMs = 1000
    );

private:

    HX711 scale;

    uint8_t doutPin;
    uint8_t sckPin;

    bool available;

    float calibrationFactor;
};

#endif