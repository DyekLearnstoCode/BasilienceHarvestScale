# Basilience Harvest Scale Firmware Guide

This document explains how the Basilience Harvest Scale firmware works, for a developer picking up this codebase who did not write it. It covers every source file in this repository.

The Harvest Scale is a standalone digital scale used to weigh harvested crop from a Basilience fogponics (a misting-based hydroponics method) grow chamber. It is a separate physical device from the main grow-chamber controller, which runs different firmware (BasilienceFirmware_V2, on an ESP32). This scale runs on cheaper ESP8266 hardware and talks to the same Firebase backend, but as its own independent device with its own identity.

## 1. What this device is and what hardware it uses

The scale is built on an ESP8266 microcontroller. Three pieces of hardware are wired to it.

**HX711 load cell amplifier.** A load cell by itself only outputs a tiny, noisy electrical signal. The HX711 chip amplifies that signal and converts it into a digital reading the ESP8266 can read over two wires. In this firmware, DOUT is wired to pin D5 (GPIO14) and SCK (the clock line) is wired to pin D6 (GPIO12). These pins are set as `HX711_DOUT_PIN` and `HX711_SCK_PIN` near the top of `BasilienceHarvestScale.ino`.

**The load cell itself.** This is the physical strain gauge that the platform sits on and that actually flexes (by a tiny, invisible amount) under weight. The firmware is calibrated for a load cell rated up to 20 kg (`MAX_WEIGHT_GRAMS`).

**16x2 I2C LCD screen.** A two-line, sixteen-character-per-line display, connected through a PCF8574 "backpack" board that lets it talk over I2C (a two-wire bus) instead of needing many separate wires. SDA is on pin D2 (GPIO4) and SCL is on pin D1 (GPIO5). The firmware auto-detects the display at I2C address 0x27 or 0x3F, since different backpack boards ship with different default addresses.

## 2. How weighing works, end to end

**Warm-up.** Right after boot, the firmware waits 30 seconds (`WARMUP_TIME_MS`) with the platform expected to be empty, before it does anything else with the load cell. Load cells and the HX711 chip both drift slightly as they warm up electrically and thermally right after power-on. Weighing immediately at power-on can give a reading that looks fine but is actually still settling. This wait used to be 60 seconds. It was shortened to 30 seconds as a deliberate but still cautious choice, since it is a one-time cost paid on every single power-on. If harvest weighings taken right after boot ever look drifted compared to ones taken later in the same session, that is a sign this value needs to go back up.

**Taring.** Taring means telling the scale "whatever the platform reads right now is zero," so that only the weight of what gets placed on top of it counts. After warm-up, the firmware averages 30 raw readings (`TARE_SAMPLES`) and stores that average as the zero-offset. This must happen with the platform empty, which is why warm-up and taring both print reminders to keep the platform clear.

**Taking a reading.** Once running, the main loop takes a new reading every 500 milliseconds (`READING_INTERVAL_MS`). Each reading is itself an average of 10 raw samples (`READING_SAMPLES`) from the HX711, run through the tare offset and the calibration factor to produce a weight in grams. Averaging multiple samples per reading reduces the effect of any single noisy sample.

**Zero deadband.** Even with an empty platform, tiny electrical noise means the raw weight will rarely read exactly 0.0. If a reading falls within plus or minus 5 grams of zero (`ZERO_DEADBAND_GRAMS`), the firmware simply forces it to exactly 0. Separately, small negative readings between 0 and -20 grams (a value written directly in `loop()`, not a named constant) are also clamped to 0, since some negative drift near zero is normal load cell behavior rather than a real negative weight.

**Display smoothing.** Averaging 10 samples within one reading does not smooth things out across reading cycles. Every 500ms cycle computes its own fresh, independent average, so ordinary HX711 noise (a few grams, normal for this kind of DIY load cell setup without extra shielding) showed up on the LCD as constant flicker in the last digit or two, even though nothing was actually wrong. To fix this, the firmware keeps a second, separate value called `displayWeightGrams` that is only ever used for what gets printed on the LCD. Each cycle, it nudges partway from its current value toward the new raw reading, using a technique called an exponential moving average: rather than jumping straight to the new number, it moves 25% of the way there (`DISPLAY_SMOOTHING_ALPHA = 0.25`) and keeps the rest of its previous value. This makes the displayed number settle smoothly instead of flickering. It is important to note this smoothed value is used for display only. The upload-stability logic described in the next section always looks at the raw, unsmoothed weight, because that decision needs to see genuine reading-to-reading agreement, not a filtered number that could hide real instability. One exception: when the scale tares or an item is removed, the displayed value snaps immediately to 0 instead of gradually decaying down to it, since a real "removed" event should read 0 right away.

## 3. How the scale decides a weight is "final" and uploads it

The firmware does not upload every reading. It waits until it is confident someone has placed an item and it has settled, then uploads once automatically. This works in three stages, using the raw (unsmoothed) weight:

**Minimum weight threshold.** Nothing is tracked for stability until the weight rises above 50 grams (`UPLOAD_MIN_GRAMS`). Below that, the firmware treats the platform as empty.

**Stability check.** Once above the threshold, the firmware keeps a rolling buffer of the last 6 readings (`STABLE_READINGS`). It checks the difference between the highest and lowest value in that buffer. If that spread is 10 grams or less (`STABLE_THRESHOLD_GRAMS`), the readings are considered "stable," meaning the item has stopped moving or settling on the platform.

**Hold time.** The first moment the readings become stable, the firmware starts a timer. It does not upload immediately. It waits for the readings to stay stable for a further 3 seconds (`STABLE_HOLD_MS`) before actually uploading. This avoids uploading a false "settled" reading that was really just a brief pause while someone was still adjusting the item on the platform.

Once all three conditions are met, the firmware uploads the weight once and sets an `uploadedThisLoad` flag so it will not upload again for the same item, even if the reading keeps sitting there stable.

**Resetting after removal.** The upload flag and the stability buffer only reset once the weight drops back below the 50 gram minimum threshold, meaning the item has actually been taken off the platform. At that point the scale is ready to detect and upload the next item.

## 4. Wi-Fi setup

The firmware does not have a Wi-Fi network name or password built into it. An earlier version of this code had a real home router password hardcoded directly into the source file, which is a security problem since source code often ends up in version control. The current approach, handled by `NetworkManager`, works like this.

On boot, it looks for previously saved Wi-Fi credentials in the chip's flash storage (a small file system called LittleFS that persists even when the power is off). If it finds credentials and can connect with them, it proceeds normally.

If there are no saved credentials, or the saved network cannot be reached, the scale starts its own Wi-Fi access point named "Basilience-Scale-Setup." Connecting a phone or laptop to that network and browsing to `http://192.168.4.1/` (or often having a setup page pop up automatically, since the firmware also runs a small DNS server that redirects all lookups to itself) shows a simple form to enter the real Wi-Fi network name and password. Submitting the form saves the credentials to flash and restarts the device, which then tries to connect using them.

Importantly, this setup process does not block the rest of the device. The scale keeps weighing locally the whole time it is waiting for someone to complete Wi-Fi setup. Only the Firebase upload feature is unavailable until a network is connected. If the connection later drops while running normally, the firmware retries reconnecting every 30 seconds rather than freezing to retry constantly.

## 5. Cloud connection

The scale uploads data to Firebase's Realtime Database, but it does not use a single master password shared across the whole system. An earlier version used what is called a Database Secret, a project-wide key that bypasses every security rule for the entire database, not just this one device. That has been replaced with a per-device identity, handled by `FirebaseManager`.

Here is the flow. Each physical scale is given its own unique secret, generated once and registered with the backend ahead of time. On its very first boot, the scale sends that secret (along with its Wi-Fi MAC address) to a Cloud Function called `deviceAuthBootstrap` over an encrypted connection. If the secret checks out, the function hands back a Firebase custom token scoped only to that one device's own little slice of the database. The scale exchanges that for a working session and saves the resulting refresh token to flash, so on every later boot it can restore its identity from that saved token instead of sending the original secret again. The original secret is only read from the firmware source once, on a completely fresh device.

Because this whole process relies on validating a security certificate, and the ESP8266 has no built-in clock hardware (unlike the ESP32 controller, which has an RTC), every boot starts thinking it is January 1st 1970 until something sets the clock. The firmware fixes this by syncing time from an NTP time server before attempting to connect to Firebase, since a certificate that looks "not valid yet" from the device's point of view will otherwise cause the secure connection to fail.

Once connected, two kinds of data go up:

- **Live weight.** The current weight is written to `devices/{deviceId}/harvestScale/liveWeight` every 5 seconds. This just overwrites the same single value each time, so it does not pile up records. It is throttled to once every 5 seconds (rather than every 500ms reading) because Firebase network calls are slow on this chip, and calling them that often would overload it and can cause crashes.
- **Confirmed harvest weight.** When the stability and hold-time checks described in section 3 confirm a final reading, a new record is added under `devices/{deviceId}/harvestScale/harvests/` containing the weight in grams, the weight in kilograms, and a timestamp (milliseconds since the device booted, not a real-world date, since the device has no persistent clock backing that). This only happens once per item.

## 6. How this scale is tied to one grow-chamber device

This scale does not have its own independent owner or set of authorized users. Instead, its permissions are borrowed entirely from whichever grow-chamber device it is paired to.

In the backend (see `regenerateSmsRecipients` in `functions/index.js`), each scale's device record carries a `parentDeviceId` field pointing at the grow-chamber device it belongs to. Whenever that grow-chamber device's own access list changes (someone is added or removed, ownership changes, and so on), the same access list is copied over to the scale automatically. This means the person who physically set up the scale, or whoever's Wi-Fi credentials are stored on it, has no bearing on who can see its data. Access always flows from the paired grow-chamber device.

Pairing itself is set from the grow-chamber device's side, through a `harvestScaleId` field on that device's record, not from the scale itself. If a scale is re-paired to point at a different grow-chamber device, the backend updates `parentDeviceId` on the scale to match, pushes the new device's access list onto it, and clears out the access list left over from the old pairing so that old access does not linger.

## 7. Settings a developer would actually need to change

These are the named constants near the top of `BasilienceHarvestScale.ino`, and what each one actually controls in practice.

- **`HX711_DOUT_PIN`, `HX711_SCK_PIN`**: which physical pins the HX711 amplifier is wired to. Only change these if the wiring changes.
- **`LCD_I2C_ADDRESS`, `LCD_COLS`, `LCD_ROWS`**: the display's I2C address and its size in characters. The firmware already tries both 0x27 and 0x3F automatically, so this constant mainly matters if a different-sized display is used.
- **`CALIBRATION_FACTOR`** (currently 124.64): this is the number of raw HX711 counts that equal one gram, on this specific load cell and amplifier pairing. It is found by placing a known weight on the platform, comparing the raw reading to the expected weight, and solving for the factor. The comment in the code notes it was refined from an earlier value of 128.17 using a known 196 gram weight that measured as 190.6 grams. This needs to be redone (following the same process) any time the physical load cell or HX711 board is replaced, since it is specific to that exact piece of hardware.
- **`PLATFORM_TARE_GRAMS`** (50.0): the weight, in grams, of the empty weighing platform/tray itself. The boot-time tare zeroes the bare load cell, not the platform sitting on it, so this fixed amount is subtracted from every raw reading in `loop()` before any other check runs. If the physical platform/tray is ever swapped for a different one, re-weigh it empty and update this constant to match.
- **`TARE_SAMPLES`** (30): how many raw samples get averaged together when zeroing the scale. A higher number gives a more stable zero point but makes the tare step at boot take slightly longer.
- **`READING_SAMPLES`** (10): how many raw samples get averaged into each individual weight reading during normal operation. Raising this smooths out noise further but makes each reading take a bit longer to compute.
- **`HX711_TIMEOUT_MS`** (1500): how long, in milliseconds, the firmware will wait for the HX711 chip to signal it has a reading ready before giving up and reporting a read failure. This matters mainly for detecting a disconnected or faulty HX711.
- **`READING_INTERVAL_MS`** (500): how often, in milliseconds, the main loop takes a new weight reading and updates the LCD. This is the base "tick rate" of the whole weighing process.
- **`WARMUP_TIME_MS`** (30000, i.e. 30 seconds): how long the firmware waits after boot, with the platform expected empty, before trusting the load cell's readings. See section 2 for why this exists.
- **`ZERO_DEADBAND_GRAMS`** (5.0): the window around zero, in grams, inside which a reading gets forced to exactly 0 instead of showing tiny noise values.
- **`DISPLAY_SMOOTHING_ALPHA`** (0.25): controls how quickly the number shown on the LCD catches up to the real, raw reading. A smaller value makes the display slower to change but smoother. A larger value makes it react faster but flicker more. This only affects what is shown on screen, never the upload logic.
- **`MAX_WEIGHT_GRAMS`** (20000, i.e. 20 kg): the overload cutoff, matching the load cell's rated capacity. Readings above this are rejected as an overload rather than trusted.
- **`UPLOAD_MIN_GRAMS`** (50.0): the minimum weight, in grams, before the firmware considers something to actually be sitting on the platform and starts tracking it for a possible upload.
- **`STABLE_THRESHOLD_GRAMS`** (10.0): how much the readings in the stability buffer are allowed to spread apart (highest minus lowest) and still count as "stable."
- **`STABLE_READINGS`** (6): how many of the most recent readings are checked together for that stability spread.
- **`STABLE_HOLD_MS`** (3000, i.e. 3 seconds): once readings first become stable, how long they must stay stable before the firmware actually uploads.
- **`FB_API_KEY`, `FB_DATABASE_URL`, `HARVEST_SCALE_DEVICE_SECRET`**: the Firebase project's public API key, the database's URL, and this individual scale's one-time bootstrap secret described in section 5. These no longer live in the `.ino` — they're defined in `Secrets.h`, a gitignored file you create locally by copying `Secrets.h.example` and filling in real values. The secret must be generated and registered on the backend before a new scale unit can connect at all, following the steps documented in `Secrets.h.example`.

## 8. File-by-file reference

- **`BasilienceHarvestScale.ino`**: the main sketch. Holds all the tunable constants covered above, wires the pin assignments, and contains `setup()` (boot sequence: display and load cell init, Wi-Fi connect, Firebase connect, warm-up, tare) and `loop()` (the repeating read-display-check-upload cycle described in sections 2 and 3).
- **`LoadCellManager.h` / `LoadCellManager.cpp`**: wraps the third-party HX711 library. Handles safely starting up the HX711 chip without hanging forever if it is not actually connected, taking raw and averaged readings, taring, storing the calibration factor, and converting raw readings into a weight in grams.
- **`FirebaseManager.h` / `FirebaseManager.cpp`**: manages the scale's secure identity with Firebase, described in section 5. Handles the one-time secret bootstrap, syncing time over NTP so TLS certificate checks succeed, saving and restoring the refresh token from flash, and sending both the live weight and confirmed harvest weight up to the Realtime Database.
- **`DisplayManager.h` / `DisplayManager.cpp`**: drives the 16x2 I2C LCD. Provides the different screens the device shows at each stage (boot splash, warm-up countdown, taring, ready, live weight, and error messages), and detects which I2C address the display is actually on.
- **`NetworkManager.h` / `NetworkManager.cpp`**: manages the Wi-Fi connection described in section 4. Loads and saves credentials to flash, attempts to connect, and if that fails, runs the "Basilience-Scale-Setup" access point and web form so a new network can be provisioned without reflashing the device.
- **`LogoAsset.h`**: a single-color inline SVG (a small vector image, small enough to embed directly in code) of the Basilience logo, used only inside the Wi-Fi setup portal's web form so that page has some branding without needing to fetch an image from the internet, since the setup portal by definition has no real internet connection behind it yet. It intentionally only includes one of the three color layers the full app logo uses, to keep it small enough that the ESP8266's limited memory can serve it reliably.
- **`ScaleOperationManager.h` and `ScaleOperationManager.cpp`**: both files exist but are completely empty. There is no code in either of them at all. They appear to be placeholder files for some scale-operation logic that was never actually written. Currently all of that logic (reading, stability checking, and upload decisions) lives directly inside `loop()` in `BasilienceHarvestScale.ino` instead. A future maintainer should treat these two files as unused until someone actually moves that logic into them, or should consider removing them if no such refactor is planned.
- **`Secrets.h.example`**: tracked template for `Secrets.h`, the file that actually holds `FB_API_KEY`, `FB_DATABASE_URL`, and `HARVEST_SCALE_DEVICE_SECRET`. Copy this to `Secrets.h` in the same folder and fill in real values — `Secrets.h` is listed in `.gitignore` and must never be committed, since it holds this device's live credentials.
