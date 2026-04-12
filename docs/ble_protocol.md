# Masterbuilt 20070215 — BLE Protocol Reference

**Model:** 20070215  
**Generation:** Gen 2.5 ("Bluetooth generation")  
**BLE version:** 4.0 (Bluetooth Smart / LE)  
**BLE chip:** Texas Instruments CC254x series (estimated)  
**Discovery method:** Passive — nRF Connect app + Arduino Serial Monitor diagnostics. No hardware sniffer, no app decompilation.

---

## Connection

| Parameter | Value |
|-----------|-------|
| MAC address | 54:4A:16:17:2D:B3 |
| MAC type | Public |
| Advertised name | `MASTERB...T SMOKER` (truncated) |
| Bonding required | No |
| Encryption | None |
| Connect directly | Yes — any BLE central can connect |

---

## GATT Services

| Service | UUID |
|---------|------|
| Generic Access | 0x1800 |
| Generic Attribute | 0x1801 |
| Device Information | 0x180A |
| **Custom (proprietary)** | **426f7567-6854-6563-2d57-65694c69fff0** |
| Battery Service | 0x180F |

---

## Custom Service Characteristics

Base UUID: `426f7567-6854-6563-2d57-65694c69fff0`

| Short UUID | Full UUID (last segment) | Properties | Used |
|------------|--------------------------|------------|------|
| fff1 | ...65694c69fff1 | READ, WRITE | Unknown — do not write without investigation |
| fff2 | ...65694c69fff2 | READ | Returns 20 zero bytes. Not useful. |
| fff3 | ...65694c69fff3 | READ, WRITE | Unknown — settings/config |
| **fff4** | **...65694c69fff4** | **NOTIFY** | **Primary data — subscribe here** |
| fff5 | ...65694c69fff5 | READ | Unknown — possibly firmware version or aux data |

> **Note:** fff4 has **NOTIFY only** — it has no READ property. You cannot poll it with `readValue()`. You must subscribe to notifications.

---

## fff4 Notification Packet

The smoker pushes a **15-byte packet** on fff4 approximately once per second while connected.

### Confirmed packet layout

```
Byte offset:  00  01  02  03  04  05  06  07  08  09  10  11  12  13  14
Example:      B2  00  01  00  97  00  8F  00  87  00  11  B4  00  96  00
              ──      ──      ──      ──      ──
              [0]     [2]     [4]     [6]     [8]
```

| Byte | Content | Example value | Meaning |
|------|---------|---------------|---------|
| 0 | Set/target temp | `0xB2` = 178 | 178°F (smoker set point) |
| 1 | `0x00` | padding | |
| 2 | Status/flags | `0x01` | Unknown — possibly heating active |
| 3 | `0x00` | padding | |
| 4 | Box/smoker temp | `0x97` = 151 | 151°F (display showed 152°F ✓) |
| 5 | `0x00` | padding | |
| 6 | Probe 1 temp | `0x8F` = 143 | 143°F (display showed ~146°F ✓) |
| 7 | `0x00` | padding | |
| 8 | Probe 2 temp | `0x87` = 135 | 135°F if second probe inserted, else low/0 |
| 9 | `0x00` | padding | |
| 10–14 | Unknown | `11 B4 00 96 00` | Stable across readings — possibly device ID or CRC |

### Temperature encoding

**Direct Fahrenheit, single unsigned byte, no conversion.**

```cpp
float boxTemp   = (float)data[4];   // °F
float probeTemp = (float)data[6];   // °F
float setTemp   = (float)data[0];   // °F
```

This was confirmed against live smoker display readings:
- Smoker display: 152°F box / 146°F probe
- Packet bytes: `0x97` (151) / `0x8F` (143)
- Difference within sensor tolerance ✓

### What we ruled out

During diagnostics, every other encoding was tested and eliminated:

| Encoding tried | Result | Why wrong |
|---------------|--------|-----------|
| LE uint16 bytes 0-1, ÷10 as °C→°F | 1492.5°F | Wildly wrong |
| BE uint16 bytes 0-1, ÷10 as °C→°F | 8239.8°F | Wildly wrong |
| byte[0] as °C→°F | 352.4°F | Consistent but not matching display |
| LE uint16 bytes 0-1, ÷10 as °F | 1492.5°F | Wrong |
| Single byte direct °F at offset 0 | 178°F | Matched set temp, not current temp |
| **Single byte direct °F at offsets 4, 6** | **151, 143°F** | **Matched display ✓** |

---

## How to Subscribe (NimBLE-Arduino)

```cpp
#include <NimBLEDevice.h>

#define SMOKER_MAC      "54:4a:16:17:2d:b3"
#define SERVICE_UUID    "426f7567-6854-6563-2d57-65694c69fff0"
#define CHAR_NOTIFY_UUID "426f7567-6854-6563-2d57-65694c69fff4"

void onSmokerNotify(NimBLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 9) {
    float setTemp   = (float)pData[0];
    float boxTemp   = (float)pData[4];
    float probeTemp = (float)pData[6];
    float probe2    = (float)pData[8];
  }
}

// In connect:
NimBLEClient* pClient = NimBLEDevice::createClient();
pClient->connect(NimBLEAddress(std::string(SMOKER_MAC), BLE_ADDR_PUBLIC));

NimBLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
NimBLERemoteCharacteristic* pChar = pSvc->getCharacteristic(CHAR_NOTIFY_UUID);
pChar->subscribe(true, onSmokerNotify, true);
```

> **Important:** Pass `BLE_ADDR_PUBLIC` as the second argument to `NimBLEAddress`. Required in NimBLE v2.x — the single-argument constructor was removed.

---

## fff2 — Why It Returns Zeros

`fff2` (READ) was expected to carry box/smoker temperature based on its property description. In practice it returns 20 zero bytes on every read, regardless of smoker state. Hypothesis: it may only be populated by the original Masterbuilt app after sending a specific initialization command via fff1. Since fff4 already carries box temp, fff2 is not needed.

---

## Unknown Characteristics

These were not explored. Do not write to fff1 or fff3 without further investigation.

| Char | Notes |
|------|-------|
| fff1 READ/WRITE | Likely the command channel. May support power on/off, set target temp, set timer. |
| fff3 READ/WRITE | Probably settings/configuration, possibly persistent across power cycles. |
| fff5 READ | Could be firmware version, cook timer, or auxiliary sensor. Reads a small static value. |

To investigate: use nRF Connect on Android, write common byte sequences (`0x01`, `0x02`...) to fff1 and observe smoker behavior.

---

## Discovery Method

1. Used **nRF Connect** (Android) to scan and identify `MASTERB...T SMOKER`
2. Connected and browsed the GATT CLIENT tab to enumerate all services and characteristics
3. Identified fff4 as primary data source (only characteristic with NOTIFY property)
4. Built a **diagnostic Arduino sketch** that subscribes to fff4 and prints raw bytes + every candidate decoding formula to Serial Monitor
5. Cross-referenced Serial output against the smoker's physical display at known temperatures
6. Matched `byte[4]` and `byte[6]` as direct °F values

---

## Confirmed Test Conditions

| Smoker display | Packet bytes | Decoded |
|---------------|--------------|---------|
| Box: 152°F, Probe: 146°F | `...97 00 8F 00...` | Box: 151°F, Probe: 143°F |

Readings within sensor tolerance. Temperature tracks correctly in real time as smoker heats and cools.
