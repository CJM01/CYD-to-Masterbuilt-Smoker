/**
 * Masterbuilt Smoker BLE Diagnostic (v2)
 * Uses built-in ESP32 BLEDevice library (NOT NimBLE)
 * Reference: smoker_ble_reference.docx
 *
 * Upload, then open Tools > Serial Monitor at 115200 baud.
 * Keep smoker powered on and within ~15 ft.
 */

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>

#define SMOKER_MAC          "54:4a:16:17:2d:b3"
#define SERVICE_UUID        "426f7567-6854-6563-2d57-65694c69fff0"
#define CHAR_NOTIFY_UUID    "426f7567-6854-6563-2d57-65694c69fff4"
#define CHAR_STATUS_UUID    "426f7567-6854-6563-2d57-65694c69fff2"
#define CHAR_CONTROL_UUID   "426f7567-6854-6563-2d57-65694c69fff1"
#define CHAR_SETTINGS_UUID  "426f7567-6854-6563-2d57-65694c69fff3"
#define CHAR_EXTRA_UUID     "426f7567-6854-6563-2d57-65694c69fff5"

BLEClient* pClient = nullptr;
bool connected = false;

// ─── Print raw bytes — all candidate decodings ────────────────────────────────
void printRawBytes(const char* label, const uint8_t* data, size_t len) {
  Serial.print("\n[");
  Serial.print(label);
  Serial.print("] ");
  Serial.print(len);
  Serial.println(" bytes:");

  if (len == 0) { Serial.println("  (empty)"); return; }

  Serial.print("  HEX: ");
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  Serial.print("  DEC: ");
  for (size_t i = 0; i < len; i++) {
    Serial.print(data[i]);
    Serial.print(" ");
  }
  Serial.println();

  if (len >= 2) {
    uint16_t le = data[0] | (data[1] << 8);
    uint16_t be = (data[0] << 8) | data[1];

    Serial.println("  --- Candidate decodings (first 2 bytes) ---");
    Serial.print("  LE uint16 / 10 as F (direct):  ");
    Serial.print(le / 10.0, 1); Serial.println(" F");

    Serial.print("  LE uint16 / 10 as C -> F:       ");
    Serial.print((le / 10.0) * 9.0 / 5.0 + 32.0, 1); Serial.println(" F");

    Serial.print("  BE uint16 / 10 as F (direct):  ");
    Serial.print(be / 10.0, 1); Serial.println(" F");

    Serial.print("  BE uint16 / 10 as C -> F:       ");
    Serial.print((be / 10.0) * 9.0 / 5.0 + 32.0, 1); Serial.println(" F");

    Serial.print("  byte[0] direct as F:            ");
    Serial.print(data[0]); Serial.println(" F");

    Serial.print("  byte[0] as C -> F:              ");
    Serial.print(data[0] * 9.0 / 5.0 + 32.0, 1); Serial.println(" F");
  }

  // If 4 bytes, also decode bytes 2-3
  if (len >= 4) {
    uint16_t le2 = data[2] | (data[3] << 8);
    uint16_t be2 = (data[2] << 8) | data[3];
    Serial.println("  --- Bytes 2-3 ---");
    Serial.print("  LE: "); Serial.print(le2 / 10.0, 1); Serial.print(" F  |  ");
    Serial.print("  BE: "); Serial.print(be2 / 10.0, 1); Serial.println(" F");
  }
}

// ─── NOTIFY callback — NOTE: NOT named notify_callback (reserved name!) ───────
void on_smoker_notify(BLERemoteCharacteristic* pChar,
                      uint8_t* pData, size_t length, bool isNotify) {
  Serial.println("\n══ NOTIFY (fff4) ══════════════════════════════════");
  printRawBytes("fff4 probe", pData, length);
}

// ─── Read a characteristic and print it ───────────────────────────────────────
void readAndPrint(BLERemoteService* pSvc, const char* uuid, const char* name) {
  BLERemoteCharacteristic* ch = pSvc->getCharacteristic(uuid);
  if (!ch) {
    Serial.print("  "); Serial.print(name); Serial.println(": NOT FOUND");
    return;
  }
  if (!ch->canRead()) {
    Serial.print("  "); Serial.print(name); Serial.println(": not readable");
    return;
  }
  // readValue() returns String in ESP32 core 3.x
  String val = ch->readValue();
  printRawBytes(name, (const uint8_t*)val.c_str(), val.length());
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== Masterbuilt BLE Diagnostic v2 ===");
  Serial.println("Library: ESP32 BLEDevice (built-in, NOT NimBLE)");
  Serial.println("Make sure smoker is ON and within range.\n");

  BLEDevice::init("CYD-Diag");

  pClient = BLEDevice::createClient();
  Serial.print("Connecting to "); Serial.println(SMOKER_MAC);

  BLEAddress addr(SMOKER_MAC);
  if (!pClient->connect(addr)) {
    Serial.println("ERROR: Connection failed. Check smoker is on & in range.");
    return;
  }

  Serial.println("Connected!\n");
  connected = true;

  BLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
  if (!pSvc) {
    Serial.println("ERROR: Custom service not found!");
    return;
  }
  Serial.println("Custom service found.");

  // Subscribe to notify
  BLERemoteCharacteristic* pNotify = pSvc->getCharacteristic(CHAR_NOTIFY_UUID);
  if (pNotify && pNotify->canNotify()) {
    pNotify->registerForNotify(on_smoker_notify);
    Serial.println("Subscribed to fff4 NOTIFY.\n");
  } else {
    Serial.println("WARNING: fff4 notify not available!");
  }

  // Read all other characteristics once
  Serial.println("── Initial read of all characteristics ──────────");
  readAndPrint(pSvc, CHAR_CONTROL_UUID,  "fff1 control");
  readAndPrint(pSvc, CHAR_STATUS_UUID,   "fff2 box temp");
  readAndPrint(pSvc, CHAR_SETTINGS_UUID, "fff3 settings");
  readAndPrint(pSvc, CHAR_EXTRA_UUID,    "fff5 extra");

  Serial.println("\n── Streaming fff4 notify + fff2 poll every 5s ───");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  if (!pClient || !pClient->isConnected()) {
    if (connected) {
      Serial.println("Disconnected!");
      connected = false;
    }
    delay(1000);
    return;
  }

  // Poll fff2 every 5 seconds
  static unsigned long last = 0;
  if (millis() - last > 5000) {
    last = millis();
    BLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
    if (pSvc) {
      Serial.println("\n── fff2 poll ─────────────────────────────────");
      readAndPrint(pSvc, CHAR_STATUS_UUID, "fff2 box temp");
    }
  }

  delay(100);
}
