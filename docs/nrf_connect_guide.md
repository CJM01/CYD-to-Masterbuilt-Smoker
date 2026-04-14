# Using nRF Connect to Reverse Engineer a BLE Device

nRF Connect is a free app from Nordic Semiconductor that turns your phone into a full BLE explorer. It can scan for devices, connect, read every characteristic, subscribe to notifications, and even write values. It was the primary tool used to discover the Masterbuilt smoker's data format before writing a single line of Arduino code.

This guide walks through exactly what to do, using the Masterbuilt smoker as the worked example. The same steps apply to any unknown BLE device.

**Download:** nRF Connect for Mobile — available on Android and iOS (free).

---

## Step 1 — Scan and find your device

1. Open nRF Connect. It will show the **Scanner** tab.
2. Tap **SCAN** in the top right.
3. Devices will appear as they advertise. Each shows a name (if it has one), MAC address, and signal strength (RSSI in dBm — closer to 0 is stronger).
4. Power on the smoker. Look for **MASTERB...T SMOKER** to appear. The name is truncated by the device itself — this is normal.
5. Tap **CONNECT** next to it.

> If your device doesn't appear: make sure it's powered on, within range (~10 ft), and not already connected to another phone or app. Some devices only advertise when not connected.

---

## Step 2 — Explore the GATT service structure

After connecting you land on the device screen. You'll see two tabs: **CLIENT** and **SERVER**.

**Always use CLIENT.** The CLIENT tab shows what the remote device (the smoker) is offering. SERVER shows what your own phone is offering — not useful here.

You'll see a list of **services**. Each service is a logical grouping of related data. The smoker exposes:

- Generic Access (0x1800) — standard, ignore
- Generic Attribute (0x1801) — standard, ignore
- Device Information (0x180A) — standard, ignore
- **Unknown Service (426f7567-6854-6563-2d57-65694c69fff0)** — this is the proprietary service with all the useful data
- Battery Service (0x180F) — standard

Tap the **Unknown Service** row to expand it and see its characteristics.

---

## Step 3 — Read the characteristics

Inside the Unknown Service you'll see five characteristics labeled **Unknown Characteristic**, each with a different UUID ending in fff1 through fff5. For each one, look at two things:

**Properties** — listed under the UUID. This tells you what operations are allowed:
- `READ` — you can request the current value on demand
- `WRITE` — you can send data to the device
- `NOTIFY` — the device will push data to you automatically when it changes

**Descriptors** — listed below Properties. Look for:
- `Client Characteristic Configuration (0x2902)` — only appears on NOTIFY characteristics. This is the switch you flip to turn notifications on.
- `Characteristic User Description (0x2901)` — a human-readable name the device provides (usually blank on proprietary devices)

For the Masterbuilt smoker the characteristics look like this:

| Characteristic | Properties | What it means |
|---------------|------------|---------------|
| ...fff1 | READ, WRITE | A command/control channel — don't write to it without knowing what you're doing |
| ...fff2 | READ | Readable but returns zeros on this device |
| ...fff3 | READ, WRITE | Settings — unknown |
| **...fff4** | **NOTIFY** | **The live data feed — this is what you want** |
| ...fff5 | READ | Auxiliary data — unknown |

> **Key insight:** A characteristic with NOTIFY and a 0x2902 descriptor is the device's way of saying "subscribe here and I'll push data to you automatically." This is almost always where live sensor data lives on consumer BLE devices.

---

## Step 4 — Read a characteristic's value

For any characteristic showing READ in its properties, tap the **down-arrow icon** (↓) to the right of it. nRF Connect will request the current value and display it as hex bytes.

For the smoker, tapping READ on fff2 returns:
```
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
All zeros — not useful. That tells you to look elsewhere.

---

## Step 5 — Subscribe to a NOTIFY characteristic

For fff4 (the live data feed):

1. Tap the **triple-down-arrow icon** (↓↓↓) next to fff4. This writes `0x0100` to the 0x2902 CCCD descriptor, which tells the device to start sending notifications to your phone.
2. The icon changes to show it's active (highlighted).
3. Notifications start appearing immediately below the characteristic — each one shows the raw hex bytes and a timestamp.

For the Masterbuilt smoker, notifications look like:
```
Value: (0x) B2-1F-01-00-97-00-8F-00-87-00-11-B4-00-96-00
```
This updates approximately once per second while the smoker is running.

> If no notifications appear after subscribing: the device may require an initialization command first (written to a WRITE characteristic), or it may only send data when actively heating. Try setting the smoker to a temperature and letting it run.

---

## Step 6 — Decode the bytes

You now have the raw data. The challenge is figuring out what the bytes mean. The general approach:

1. **Count the bytes.** The Masterbuilt sends 15 bytes. Knowing the length helps rule out encodings.

2. **Look for values that match something you can verify.** Set the smoker to 225°F and let it stabilize. The bytes should contain 225 somewhere — either as the direct value `0xE1` (225 in decimal), or encoded as a 2-byte value, or in Celsius (225°F = 107°C = `0x6B`).

3. **Try every byte position.** Don't assume the data starts at byte 0. The Masterbuilt turned out to have temperatures at bytes 0, 4, 6, and 8 — with `0x00` padding bytes between them.

4. **Try the candidate decodings in order:**
   - Direct °F: `byte value = temperature in °F`
   - Direct °C then convert: `byte value = temp in °C, multiply by 9/5 + 32`
   - uint16 little-endian ÷10 as °C: `(byte[n] | byte[n+1]<<8) / 10.0 * 9/5 + 32`
   - uint16 big-endian ÷10 as °C: `(byte[n]<<8 | byte[n+1]) / 10.0 * 9/5 + 32`

5. **Use the diagnostic sketch** in this repo (`tools/smoker_diagnostic/`) to automate this — it prints every candidate decoding to Serial Monitor so you can compare against the device's display.

For the Masterbuilt, the answer turned out to be the simplest possible: **direct Fahrenheit at byte 4 (smoker temp) and byte 6 (probe temp)**. No conversion needed.

---

## Step 7 — Verify by watching values change

Once you have a candidate decoding, verify it by watching the values change in a predictable way:

- Set the smoker to a higher temperature and watch byte 4 climb
- Insert the probe into something hot and watch byte 6 rise
- Let the smoker cool and watch both fall

If the values track the physical display consistently over a range of temperatures, you have the right decoding.

---

## Tips for other BLE devices

**The NOTIFY characteristic is almost always the live data source.** Consumer devices — thermometers, scales, heart rate monitors, sensors — nearly all push live readings via NOTIFY rather than requiring you to poll with READ.

**Standard short UUIDs are documented.** If you see a 16-bit UUID like `0x2A6E` (Temperature) or `0x2A37` (Heart Rate Measurement), look it up at [bluetooth.com/specifications/gatt/](https://www.bluetooth.com/specifications/gatt/) — these are standardized and the format is publicly documented. You only need to reverse-engineer proprietary 128-bit UUIDs like the Masterbuilt's.

**Write carefully.** READ is safe — you're just asking the device for data. WRITE sends a command to the device. On an appliance like a smoker, writing unknown values to a WRITE characteristic could change settings or behave unexpectedly. Explore READ and NOTIFY first.

**Byte order matters.** Most BLE devices use little-endian (least significant byte first). If a two-byte value doesn't make sense as little-endian, try big-endian.

**0x00 bytes are often padding, not data.** The Masterbuilt packet alternates data byte / 0x00 byte throughout. Don't assume a zero byte is a temperature reading of 0°F.
