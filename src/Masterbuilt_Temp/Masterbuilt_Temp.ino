/**
 * Masterbuilt Smoker BLE Monitor (v4 — final)
 * Hardware : Hosyond 4" ESP32-3248S040 (CYD)
 * Libraries: TFT_eSPI, NimBLE-Arduino (by h2zero)
 *
 * Packet format CONFIRMED from live Serial data:
 *   fff4 NOTIFY — 15 bytes, temps are direct °F single bytes:
 *     byte 0  = set/target temp
 *     byte 4  = box/smoker temp  (0x97=151°F, actual display 152°F ✓)
 *     byte 6  = probe temp       (0x8F=143°F, actual display ~146°F ✓)
 *     byte 8  = probe 2 (spare slot)
 *     odd bytes = 0x00 padding/flags
 *   fff2 READ — always 20 zero bytes, not used.
 */

#include <TFT_eSPI.h>
#include <NimBLEDevice.h>

// ─── BLE Config ───────────────────────────────────────────────────────────────
#define SMOKER_MAC       "54:4a:16:17:2d:b3"
#define SERVICE_UUID     "426f7567-6854-6563-2d57-65694c69fff0"
#define CHAR_PROBE_UUID  "426f7567-6854-6563-2d57-65694c69fff4"  // NOTIFY

// ─── Display ──────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

#define C_BG     TFT_BLACK
#define C_PANEL  0x1082
#define C_ACCENT 0xFD20
#define C_WHITE  TFT_WHITE
#define C_GRAY   0x8410
#define C_GREEN  TFT_GREEN
#define C_RED    TFT_RED
#define C_YELLOW TFT_YELLOW

// ─── State ────────────────────────────────────────────────────────────────────
NimBLEClient* pClient = nullptr;

volatile bool  newData     = false;
volatile float probeTempF  = 0.0f;
volatile float boxTempF    = 0.0f;
volatile float targetTempF = 0.0f;

float lastProbeTempF = -999.0f;
float lastBoxTempF   = -999.0f;

// ─── Packet decode ────────────────────────────────────────────────────────────
void decodePacket(const uint8_t* d, size_t len) {
  Serial.print("fff4:");
  for (size_t i = 0; i < len; i++) Serial.printf(" %02X", d[i]);
  if (len >= 9) {
    Serial.printf("  => set:%d  box:%d  probe:%d  probe2:%d\n",
                  d[0], d[4], d[6], d[8]);
    targetTempF = (float)d[0];
    boxTempF    = (float)d[4];
    probeTempF  = (float)d[6];
    newData = true;
  } else {
    Serial.println(" (short packet)");
  }
}

// ─── Display helpers ──────────────────────────────────────────────────────────
void drawBackground() {
  tft.fillScreen(C_BG);

  tft.fillRoundRect(10, 10, 300, 50, 8, C_PANEL);
  tft.setTextColor(C_ACCENT, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.drawString("SMOKER MONITOR", 160, 35);

  tft.fillRoundRect(10, 75, 300, 160, 10, C_PANEL);
  tft.setTextColor(C_GRAY, C_PANEL);
  tft.setTextSize(1);
  tft.drawString("PROBE TEMP", 160, 95);

  tft.fillRoundRect(10, 255, 300, 120, 10, C_PANEL);
  tft.setTextColor(C_GRAY, C_PANEL);
  tft.drawString("SMOKER TEMP", 160, 275);

  tft.fillRoundRect(10, 395, 300, 50, 8, C_PANEL);
}

void updateProbeTemp(float tempF) {
  tft.fillRect(15, 105, 290, 120, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  if (tempF > 32.0f && tempF < 700.0f) {
    tft.setTextColor(tempF > 200.0f ? C_RED : C_ACCENT, C_PANEL);
    tft.setTextSize(5);
    char buf[10]; snprintf(buf, sizeof(buf), "%d", (int)tempF);
    tft.drawString(buf, 145, 155);
    tft.setTextSize(2);
    tft.setTextColor(C_WHITE, C_PANEL);
    tft.drawString("F", 272, 138);
  } else {
    tft.setTextColor(C_GRAY, C_PANEL);
    tft.setTextSize(3);
    tft.drawString("---", 160, 155);
  }
}

void updateBoxTemp(float tempF) {
  tft.fillRect(15, 285, 290, 80, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  if (tempF > 32.0f && tempF < 700.0f) {
    tft.setTextColor(C_WHITE, C_PANEL);
    tft.setTextSize(3);
    char buf[10]; snprintf(buf, sizeof(buf), "%d", (int)tempF);
    tft.drawString(buf, 140, 325);
    tft.setTextSize(1);
    tft.drawString("F", 220, 313);
  } else {
    tft.setTextColor(C_GRAY, C_PANEL);
    tft.setTextSize(3);
    tft.drawString("---", 160, 325);
  }
}

void updateStatus(const String& msg, uint16_t color = C_WHITE) {
  tft.fillRect(15, 400, 290, 40, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, C_PANEL);
  tft.setTextSize(1);
  tft.drawString(msg, 160, 420);
}

// ─── NimBLE notify callback ───────────────────────────────────────────────────
void onSmokerNotify(NimBLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  decodePacket(pData, length);
}

// ─── NimBLE client callbacks ──────────────────────────────────────────────────
class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pC) {
    Serial.println("Smoker disconnected.");
  }
};

// ─── Connect ──────────────────────────────────────────────────────────────────
bool connectToSmoker() {
  updateStatus("Connecting...", C_YELLOW);

  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCB(), false);
  } else if (pClient->isConnected()) {
    pClient->disconnect();
    delay(500);
  }

  if (!pClient->connect(NimBLEAddress(std::string(SMOKER_MAC), BLE_ADDR_PUBLIC))) {
    updateStatus("Connect failed. Retrying...", C_RED);
    return false;
  }
  Serial.println("Connected.");

  NimBLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
  if (!pSvc) {
    updateStatus("Service not found!", C_RED);
    pClient->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* pProbe = pSvc->getCharacteristic(CHAR_PROBE_UUID);
  if (!pProbe || !pProbe->canNotify()) {
    updateStatus("Notify char missing!", C_RED);
    pClient->disconnect();
    return false;
  }

  bool ok = pProbe->subscribe(true, onSmokerNotify, true);
  Serial.printf("Subscribe: %s\n", ok ? "OK" : "FAILED");

  updateStatus("Connected  OK", C_GREEN);
  return true;
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BG);
  uint16_t calData[5] = { 305, 3590, 251, 3482, 7 };
  tft.setTouch(calData);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  drawBackground();
  updateStatus("Initialising BLE...", C_GRAY);

  NimBLEDevice::init("CYD-Smoker");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  while (!connectToSmoker()) { delay(3000); }
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  if (!pClient || !pClient->isConnected()) {
    updateStatus("Reconnecting...", C_YELLOW);
    delay(3000);
    connectToSmoker();
    return;
  }

  if (newData) {
    newData = false;
    if (probeTempF != lastProbeTempF) {
      updateProbeTemp(probeTempF);
      lastProbeTempF = probeTempF;
    }
    if (boxTempF != lastBoxTempF) {
      updateBoxTemp(boxTempF);
      lastBoxTempF = boxTempF;
    }
  }

  delay(50);
}
