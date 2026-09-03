#include "LoadCellManager.h"

// ============================================================
// CONSTRUCTOR
// ============================================================

LoadCellManager::LoadCellManager(
    uint8_t doutPin,
    uint8_t sckPin
)
    : doutPin(doutPin),
      sckPin(sckPin),
      available(false),
      calibrationFactor(1.0f)
{
}

// ============================================================
// INITIALIZATION
// ============================================================

bool LoadCellManager::begin(
    unsigned long timeoutMs
)
{
    // Deliberately does NOT call scale.begin(doutPin, sckPin) yet. That
    // library call ends by internally calling set_gain() -> read() ->
    // wait_ready() - and that innermost wait_ready() has NO timeout of
    // its own (a bare `while (!is_ready()) { delay(...); }`). With no
    // HX711 physically connected, that blocks forever, before our own
    // timeoutMs bound below ever gets a chance to run - confirmed root
    // cause of setup() hanging indefinitely (no reset, no further serial
    // output, ever) when testing without the sensor wired up.
    //
    // Fix: set up the pins ourselves and poll DOUT directly (not through
    // `scale`, whose internal DOUT/PD_SCK members aren't set until
    // begin() runs) with OUR OWN bounded wait. Only once the chip has
    // actually signaled ready do we call scale.begin() - at that point
    // its internal wait_ready() call finds is_ready() true immediately
    // and returns at once instead of blocking.
    pinMode(sckPin, OUTPUT);
    pinMode(doutPin, INPUT);
    digitalWrite(sckPin, LOW);

    unsigned long startTime = millis();
    bool chipReady = false;

    while (millis() - startTime < timeoutMs)
    {
        if (digitalRead(doutPin) == LOW)
        {
            chipReady = true;
            break;
        }

        yield();
        delay(1);
    }

    if (!chipReady)
    {
        available = false;

        return false;
    }

    scale.begin(
        doutPin,
        sckPin
    );

    available = true;

    return true;
}

// ============================================================
// STATUS
// ============================================================

bool LoadCellManager::isReady()
{
    bool ready =
        scale.is_ready();

    available = ready;

    return ready;
}

bool LoadCellManager::isAvailable() const
{
    return available;
}

// ============================================================
// WAIT UNTIL HX711 READY
// ============================================================

bool LoadCellManager::waitUntilReady(
    unsigned long timeoutMs
)
{
    unsigned long startTime =
        millis();

    while (!scale.is_ready()) {

        if (
            millis() - startTime
            >=
            timeoutMs
        ) {
            available = false;

            return false;
        }

        yield();

        delay(1);
    }

    available = true;

    return true;
}

// ============================================================
// RAW READING
// ============================================================

bool LoadCellManager::readRaw(
    long &value,
    unsigned long timeoutMs
)
{
    if (!waitUntilReady(timeoutMs)) {
        return false;
    }

    value =
        scale.read();

    available = true;

    return true;
}

// ============================================================
// RAW AVERAGE
// ============================================================

bool LoadCellManager::readRawAverage(
    uint8_t samples,
    long &value,
    unsigned long timeoutMs
)
{
    if (samples == 0) {
        return false;
    }

    if (!waitUntilReady(timeoutMs)) {
        return false;
    }

    value =
        scale.read_average(samples);

    available = true;

    return true;
}

// ============================================================
// TARE
// ============================================================

bool LoadCellManager::tare(
    uint8_t samples,
    unsigned long timeoutMs
)
{
    if (samples == 0) {
        return false;
    }

    if (!waitUntilReady(timeoutMs)) {
        return false;
    }

    scale.tare(samples);

    available = true;

    return true;
}

// ============================================================
// TARE OFFSET
// ============================================================

long LoadCellManager::getOffset()
{
    return scale.get_offset();
}

// ============================================================
// CALIBRATION
// ============================================================

void LoadCellManager::setCalibrationFactor(
    float factor
)
{
    calibrationFactor = factor;

    scale.set_scale(
        calibrationFactor
    );
}

float LoadCellManager::getCalibrationFactor() const
{
    return calibrationFactor;
}

// ============================================================
// READ CALIBRATED WEIGHT
// ============================================================

bool LoadCellManager::readWeightGrams(
    uint8_t samples,
    float &weightGrams,
    unsigned long timeoutMs
)
{
    if (samples == 0) {
        return false;
    }

    if (!waitUntilReady(timeoutMs)) {
        return false;
    }

    // HX711 library calculation:
    //
    // get_units =
    // (average raw value - tare offset)
    // /
    // calibration factor
    //
    // Our calibration factor is counts per gram,
    // therefore the returned result is grams.

    weightGrams =
        scale.get_units(samples);

    available = true;

    return true;
}