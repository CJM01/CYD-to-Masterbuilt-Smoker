# Lessons Learned

Every dead end we hit, documented so future projects don't repeat them.

---

## 1. Use NimBLE-Arduino, not the built-in ESP32 BLE library

**What happened:** The first working diagnostic was written against the built-in `BLEDevice.h` / `BLEClient.h` library (ESP32 Arduino core 3.x). It connected fine but `readValue()` on fff4 silently returned garbage, and `registerForNotify()` only fired once then stopped.

**Root cause:** The built-in library has known instability with the TI CC254x BLE chip used in this smoker. Notifications stop after the first delivery. The `readValue()` return type also changed from `std::string` to Arduino `String` in core 3.x, causing silent byte-access bugs.

**Fix:** Switch to **NimBLE-Arduino** (by h2zero, available in Library Manager). NimBLE handles CCCD subscription reliably with a single `subscribe()` call. Connection is stable, notifications flow continuously.

**Rule:** For any ESP32 BLE project, start with NimBLE. Only fall back to the built-in library if you have a specific reason.

---

## 2. NimBLE v2.x changed the NimBLEAddress constructor

**What happened:** `NimBLEAddress addr(SMOKER_MAC)` compiled fine on NimBLE v1.x but produced a compile error on v2.x:

```
error: no matching function for call to 'NimBLEAddress::NimBLEAddress(const char [18])'
```

**Root cause:** NimBLE v2.x requires an explicit address type as the second argument. The single-argument constructor was removed.

**Fix:**
```cpp
// Wrong (v1.x only)
NimBLEAddress addr(SMOKER_MAC);

// Correct (v2.x)
NimBLEAddress addr(std::string(SMOKER_MAC), BLE_ADDR_PUBLIC);
```

Use `BLE_ADDR_PUBLIC` for consumer devices with a fixed MAC. Use `BLE_ADDR_RANDOM` only if the device advertises with a random address.

---

## 3. fff4 is NOTIFY-only — you cannot poll it

**What happened:** v3 of the sketch switched from notifications to polling (`readValue()` on fff4 in a loop) after notifications seemed unreliable. The result was that probe temp was always stuck — no updates at all.

**Root cause:** `fff4` has **NOTIFY property only**. There is no READ property. Calling `readValue()` on a NOTIFY-only characteristic either returns empty or returns a cached value from the initial connection — it never fetches live data.

**How to confirm:** Check the characteristic properties in nRF Connect. If it shows only `NOTIFY` and no `READ`, you must subscribe. You cannot poll.

**Fix:** Subscribe with `pChar->subscribe(true, callbackFn, true)`. NimBLE writes the CCCD automatically. Do not attempt to write the CCCD descriptor manually — NimBLE handles it.

---

## 4. Do not name your callback `notify_callback`

**What happened:** An early version used `void notify_callback(...)` as the BLE notification handler. It compiled but behaved unpredictably.

**Root cause:** `notify_callback` is a typedef'd name inside `BLERemoteCharacteristic.h` (built-in library). Defining a function with the same name causes a silent conflict.

**Fix:** Use any other name. We used `onSmokerNotify`. This applies to the built-in library; NimBLE does not have this conflict, but it's a good habit to avoid generic callback names regardless.

---

## 5. fff2 does not contain live temperature data

**What happened:** Based on the characteristic description ("box temp READ"), several versions of the sketch polled fff2 for smoker box temperature. It always returned 20 zero bytes, producing a display reading of 32.0°F (= `cToF(0)`).

**Root cause:** Unknown. Leading hypothesis is that fff2 is only populated after the original Masterbuilt app sends an initialization command via fff1. Without that command, the device serves zeros.

**Fix:** Ignore fff2 entirely. Box temperature is in the fff4 notification packet at byte offset 4.

---

## 6. All temperatures are in the fff4 packet — direct Fahrenheit

**What happened:** Every candidate temperature encoding was tried (uint16 LE, uint16 BE, single byte °C, single byte °F, ÷10 variants) on bytes 0 and 1 of the fff4 packet. None matched the smoker display.

**Root cause:** The temps are not in bytes 0-1. The packet is structured with a value at every **even byte offset** separated by `0x00` padding bytes. The relevant offsets are 0 (set temp), 4 (box temp), 6 (probe 1), 8 (probe 2).

**Fix:** Parse by offset, not by treating the packet as a packed struct from byte 0:
```cpp
float setTemp   = (float)data[0];
float boxTemp   = (float)data[4];
float probeTemp = (float)data[6];
```

**Lesson:** When a packet doesn't decode at offset 0, print the entire packet and look at each byte position individually. The diagnostic sketch (tools/smoker_diagnostic/) prints all bytes and multiple candidate decodings — use it.

---

## 7. The diagnostic sketch is the most important tool

The single most productive thing in this project was a sketch that:
- Connected to the smoker
- Subscribed to fff4
- Printed every notification as raw hex + every candidate decoding formula
- Polled all other characteristics and printed them too

This let us identify the correct byte offsets by comparing Serial output to the smoker's physical display at a known temperature. Without it, we were guessing at encodings blindly.

**Template:** `tools/smoker_diagnostic/smoker_diagnostic.ino`

**Reuse this pattern** for any future BLE device where the protocol is unknown:
1. Connect and dump all characteristics
2. Subscribe to any NOTIFY characteristics
3. Print every candidate decoding
4. Compare to a known ground truth (physical display, another app, a known input)

---

## 8. The smoker drops BLE after idle

The smoker disconnects BLE clients after a period of inactivity (no data being sent/received). This is normal behavior for TI CC254x devices.

**Fix:** Always implement a reconnect loop:
```cpp
void loop() {
  if (!pClient->isConnected()) {
    delay(3000);
    connectToSmoker();
    return;
  }
  // ... normal operation
}
```

The 3-second delay avoids hammering the smoker with rapid reconnect attempts.
