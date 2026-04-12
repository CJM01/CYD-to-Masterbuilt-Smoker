# Development History

A chronological record of how this project evolved, including every approach tried and why it changed. Useful context if you're debugging a similar device.

---

## Stage 1 — Hardware identification

Identified the CYD model as Hosyond ESP32-3248S040 with ST7796 driver. Configured TFT_eSPI `User_Setup` with correct pin mappings and ran touch calibration. Screen works at 320×480 portrait.

Identified the smoker via nRF Connect (Android):
- Found advertising as `MASTERB...T SMOKER`
- MAC: `54:4A:16:17:2D:B3` (public)
- Enumerated 5 characteristics under the custom service
- Noted fff4 as NOTIFY-only (primary data source)

---

## Stage 2 — First attempt (NimBLE v1 approach)

**Sketch:** `smoker_cyd.ino` (v1)

Used NimBLE-Arduino. Connected successfully. Subscribed to fff4 NOTIFY. Polled fff2 every 5 seconds for box temp.

**Result:** Probe temp showed 352.4°F and never changed. Box temp showed `---`.

**Problems identified:**
- NimBLE v2.x constructor change: `NimBLEAddress(mac)` → `NimBLEAddress(std::string(mac), BLE_ADDR_PUBLIC)`
- Byte decoding wrong — assumed single byte °C, got 178°C → 352.4°F which was actually the raw byte value misinterpreted

---

## Stage 3 — Switch to built-in BLE library

**Sketch:** `smoker_cyd_v2.ino`

Switched to built-in `BLEDevice.h` after reference document (smoker_ble_reference.docx) indicated it as the tested library.

Fixed `registerForNotify` + explicit CCCD write. Fixed `readValue()` returning `String` not `std::string` in core 3.x.

**Result:** Probe still stuck at 352.4°F. Box temp showed 33.8°F (= `cToF(1)` — fff2 returning a single `0x01` byte).

**Problems identified:**
- Notifications still only firing once
- fff2 still not returning useful data
- Built-in BLE library unreliable with TI CC254x

---

## Stage 4 — Poll-based approach

**Sketch:** `smoker_cyd_v3.ino`

Abandoned notifications entirely. Switched to polling fff4 every 2 seconds with `readValue()`.

**Result:** No updates at all. Probe and box both stuck.

**Problem identified:** fff4 has **NOTIFY property only** — confirmed by re-examining nRF Connect screenshots. `readValue()` on a NOTIFY-only characteristic does not return live data. This approach fundamentally cannot work.

---

## Stage 5 — Back to NimBLE, correct architecture

**Sketch:** `smoker_cyd_v4.ino` (initial)

Returned to NimBLE. Used `subscribe()` (not `registerForNotify`). NimBLE handles CCCD internally. Removed fff2 polling, attempted to extract box temp from the notify packet bytes 2-3.

**Result:** Notifications flowing! But temperatures still wrong. Serial output showed candidate decodings all wildly off.

---

## Stage 6 — Diagnostic sketch

**Sketch:** `smoker_diagnostic_v2.ino`

Wrote a dedicated diagnostic that:
- Connects with correct NimBLE v2.x syntax
- Subscribes to fff4
- Prints full raw hex of every notification
- Prints 6 candidate decodings side by side
- Polls fff2 and prints it too

**Output sample:**
```
fff4 HEX: B2 1F 01 00 97 00 8F 00 87 00 11 B4 00 96 00
  LE/10°C->F: 1492.5  BE/10°C->F: 8239.8  byte0°C->F: 352.4  byte0F: 178
fff2 HEX: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
  LE/10°C->F: 32.0
```

**fff2 verdict:** Always 20 zero bytes. Useless. Abandoned.

**fff4 analysis:** None of the standard candidate decodings on bytes 0-1 matched the smoker display (152°F box, 146°F probe). Examined every byte individually.

---

## Stage 7 — Packet cracked

Compared each byte position to known display values:

```
B2  00  01  00  97  00  8F  00  87  00  11  B4  00  96  00
[0]     [2]     [4]     [6]     [8]
178     1       151     143     135
```

- `d[4]` = `0x97` = **151°F** → smoker display showed **152°F** ✓
- `d[6]` = `0x8F` = **143°F** → smoker display showed **~146°F** ✓

**Encoding confirmed: direct Fahrenheit, single unsigned byte, at offsets 0/4/6/8.**

Also noted that bytes 4, 6, 8 were changing over time in the Serial output (the smoker was cooling down during the test session), confirming they were live temperature readings.

---

## Stage 8 — Final sketch

**Sketch:** `src/Masterbuilt_Temp/Masterbuilt_Temp.ino`

Updated `decodePacket()` to read `d[0]`, `d[4]`, `d[6]`, `d[8]` as direct °F values. Both probe and box temp now update correctly in real time, matching the smoker's physical display temperature for temperature.

**Verified:** CYD display tracks smoker display at 152°F box / 146°F probe. ✓
