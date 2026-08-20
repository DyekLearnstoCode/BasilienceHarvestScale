#include "DisplayManager.h"

DisplayManager::DisplayManager(uint8_t i2cAddress, uint8_t cols, uint8_t rows)
    : _lcd(i2cAddress, cols, rows), _i2cAddress(i2cAddress),
      _cols(cols), _rows(rows), _available(false) {}

bool DisplayManager::begin() {
    Wire.begin(4, 5);
    uint8_t candidates[] = { 0x27, 0x3F };
    uint8_t found = 0;
    for (uint8_t i = 0; i < 2; i++) {
        Wire.beginTransmission(candidates[i]);
        if (Wire.endTransmission() == 0) { found = candidates[i]; break; }
    }
    if (found == 0) { _available = false; Serial.println("[LCD] Not found."); return false; }
    _lcd.init(); _lcd.backlight(); _available = true;
    Serial.print("[LCD] Found at 0x"); Serial.println(found, HEX);
    return true;
}
bool DisplayManager::isAvailable() const { return _available; }
void DisplayManager::showBoot() { if (!_available) return; _lcd.clear(); printCentered(0, "  Basilience   "); printCentered(1, "Harvest Scale  "); }
void DisplayManager::showWarmUp(unsigned long s) { if (!_available) return; printPadded(0, "Warming up..."); String r = "Wait: "; r += s; r += " sec"; printPadded(1, r); }
void DisplayManager::showTaring() { if (!_available) return; _lcd.clear(); printCentered(0, "Taring..."); printCentered(1, "Keep empty!"); }
void DisplayManager::showReady() { if (!_available) return; _lcd.clear(); printCentered(0, "Scale Ready!"); printCentered(1, "Place item..."); }
void DisplayManager::showWeight(float g, float kg) { if (!_available) return; String r0 = String(g,1)+" g"; printPadded(0,r0); String r1=String(kg,3)+" kg"; printPadded(1,r1); }
void DisplayManager::showError(const String& l1, const String& l2) { if (!_available) return; printPadded(0,l1); printPadded(1,l2); }
void DisplayManager::backlightOn()  { if (_available) _lcd.backlight(); }
void DisplayManager::backlightOff() { if (_available) _lcd.noBacklight(); }
void DisplayManager::clear()        { if (_available) _lcd.clear(); }

void DisplayManager::printCentered(uint8_t row, const String& text) {
    uint8_t pad = (text.length() < _cols) ? (_cols - text.length()) / 2 : 0;
    String s = ""; for (uint8_t i=0;i<pad;i++) s+=' '; s+=text;
    while (s.length() < _cols) s+=' ';
    _lcd.setCursor(0, row); _lcd.print(s);
}
void DisplayManager::printPadded(uint8_t row, const String& text) {
    String s = text;
    while (s.length() < _cols) s+=' ';
    if (s.length() > _cols) s = s.substring(0, _cols);
    _lcd.setCursor(0, row); _lcd.print(s);
}