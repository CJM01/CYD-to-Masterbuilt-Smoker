/**
 * Masterbuilt Smoker BLE Monitor (v5)
 * Hardware : Hosyond 4" ESP32-3248S040 (CYD)
 * Libraries: TFT_eSPI, NimBLE-Arduino (by h2zero)
 *
 * New in v5:
 *   - Cook timer: counts up HH:MM from 0:00, tap to start/stop
 *   - Temperature graph: dual sparkline (probe + smoker) over last 120 readings
 *     One reading stored per notify (~1/sec), so ~2 min of history at full res,
 *     scrolling left as new readings arrive
 *
 * Layout (320x480 portrait):
 *   y=5–55    Header
 *   y=60–115  Probe temp (left) + Smoker temp (right) — side by side
 *   y=120–166 Cook timer + Start/Stop button
 *   y=171–404 Temperature graph
 *   y=410–475 Status bar
 *
 * Packet format (confirmed):
 *   fff4 NOTIFY byte[0]=set  byte[4]=box  byte[6]=probe  byte[8]=probe2
 *   All direct °F values.
 */

#include <TFT_eSPI.h>
#include <NimBLEDevice.h>

// ─── BLE ──────────────────────────────────────────────────────────────────────
#define SMOKER_MAC      "54:4a:16:17:2d:b3"
#define SERVICE_UUID    "426f7567-6854-6563-2d57-65694c69fff0"
#define CHAR_NOTIFY_UUID "426f7567-6854-6563-2d57-65694c69fff4"

// ─── Display ──────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);  // sprite for flicker-free graph redraws

#define C_BG      0x0000   // black
#define C_PANEL   0x1082   // dark grey
#define C_PROBE   0xFD20   // orange  — probe line + temp
#define C_BOX     0x07E0   // green   — smoker/box line + temp
#define C_WHITE   0xFFFF
#define C_GRAY    0x8410
#define C_DKGRAY  0x2104
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_YELLOW  0xFFE0

// ─── Layout constants ─────────────────────────────────────────────────────────
#define HDR_Y      5
#define HDR_H      50
#define TEMP_Y     60
#define TEMP_H     55
#define TIMER_Y    120
#define TIMER_H    46
#define GRAPH_Y    171
#define GRAPH_H    234
#define STATUS_Y   410
#define STATUS_H   65
#define MARGIN     5
#define SCREEN_W   320



// Graph inner area (inside the panel with some padding)
#define GX         22    // left edge of plot area
#define GY         (GRAPH_Y + 20)
#define GW         (SCREEN_W - MARGIN*2 - GX - 5)
#define GH         (GRAPH_H - 45)   // leaves room for labels at bottom

// ─── History buffer ───────────────────────────────────────────────────────────
#define HISTORY    120   // number of readings to store

uint8_t probeHistory[HISTORY];
uint8_t boxHistory[HISTORY];
uint8_t historyCount = 0;   // how many slots filled (0–HISTORY)
uint8_t historyHead  = 0;   // circular buffer write index

void pushHistory(uint8_t probe, uint8_t box) {
  probeHistory[historyHead] = probe;
  boxHistory[historyHead]   = box;
  historyHead = (historyHead + 1) % HISTORY;
  if (historyCount < HISTORY) historyCount++;
}

// Get reading at position i (0 = oldest, historyCount-1 = newest)
uint8_t getProbe(uint8_t i) {
  uint8_t idx = (historyHead - historyCount + i + HISTORY) % HISTORY;
  return probeHistory[idx];
}
uint8_t getBox(uint8_t i) {
  uint8_t idx = (historyHead - historyCount + i + HISTORY) % HISTORY;
  return boxHistory[idx];
}

// ─── State ────────────────────────────────────────────────────────────────────
NimBLEClient* pClient = nullptr;

volatile bool  newData     = false;
volatile float probeTempF  = 0.0f;
volatile float boxTempF    = 0.0f;
volatile float targetTempF = 0.0f;

float lastProbeTempF = -999.0f;
float lastBoxTempF   = -999.0f;

// Cook timer
bool          cookTimerRunning = false;
unsigned long cookTimerStart   = 0;    // millis() when timer was last started
unsigned long cookTimerAccum   = 0;    // accumulated ms before last stop



// ─── Packet decode ────────────────────────────────────────────────────────────
void decodePacket(const uint8_t* d, size_t len) {
  if (len >= 9) {
    targetTempF = (float)d[0];
    boxTempF    = (float)d[4];
    probeTempF  = (float)d[6];
    newData = true;
    Serial.printf("set:%d box:%d probe:%d\n", d[0], d[4], d[6]);
  }
}

// ─── Draw static background (called once) ────────────────────────────────────
void drawBackground() {
  tft.fillScreen(C_BG);

  // Header
  tft.fillRoundRect(MARGIN, HDR_Y, SCREEN_W - MARGIN*2, HDR_H, 6, C_PANEL);
  tft.setTextColor(C_PROBE, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.drawString("SMOKER MONITOR", 160, HDR_Y + HDR_H/2);

  // Probe card (left half)
  tft.fillRoundRect(MARGIN, TEMP_Y, 153, TEMP_H, 6, C_PANEL);
  tft.setTextColor(C_GRAY, C_PANEL);
  tft.setTextSize(1);
  tft.drawString("PROBE", 81, TEMP_Y + 12);

  // Box temp card (right half)
  tft.fillRoundRect(162, TEMP_Y, 153, TEMP_H, 6, C_PANEL);
  tft.setTextColor(C_GRAY, C_PANEL);
  tft.drawString("SMOKER", 238, TEMP_Y + 12);

  // Timer panel
  tft.fillRoundRect(MARGIN, TIMER_Y, SCREEN_W - MARGIN*2, TIMER_H, 6, C_PANEL);
  tft.setTextColor(C_GRAY, C_PANEL);
  tft.drawString("COOK TIME", 90, TIMER_Y + 9);

  // Graph panel
  tft.fillRoundRect(MARGIN, GRAPH_Y, SCREEN_W - MARGIN*2, GRAPH_H, 6, C_PANEL);
  tft.setTextColor(C_GRAY, C_PANEL);
  tft.setTextSize(1);
  tft.drawString("TEMPERATURE HISTORY", 160, GRAPH_Y + 10);

  // Status bar
  tft.fillRoundRect(MARGIN, STATUS_Y, SCREEN_W - MARGIN*2, STATUS_H, 6, C_PANEL);
}

// ─── Update probe temp ────────────────────────────────────────────────────────
void updateProbeTemp(float tempF) {
  tft.fillRect(MARGIN+2, TEMP_Y + 20, 149, TEMP_H - 22, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  if (tempF > 32.0f && tempF < 700.0f) {
    tft.setTextColor(tempF > 200.0f ? C_RED : C_PROBE, C_PANEL);
    tft.setTextSize(3);
    char buf[8]; snprintf(buf, sizeof(buf), "%d F", (int)tempF);
    tft.drawString(buf, 81, TEMP_Y + TEMP_H/2 + 8);
  } else {
    tft.setTextColor(C_GRAY, C_PANEL);
    tft.setTextSize(2);
    tft.drawString("---", 81, TEMP_Y + TEMP_H/2 + 8);
  }
}

// ─── Update box temp ─────────────────────────────────────────────────────────
void updateBoxTemp(float tempF) {
  tft.fillRect(164, TEMP_Y + 20, 149, TEMP_H - 22, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  if (tempF > 32.0f && tempF < 700.0f) {
    tft.setTextColor(C_BOX, C_PANEL);
    tft.setTextSize(3);
    char buf[8]; snprintf(buf, sizeof(buf), "%d F", (int)tempF);
    tft.drawString(buf, 238, TEMP_Y + TEMP_H/2 + 8);
  } else {
    tft.setTextColor(C_GRAY, C_PANEL);
    tft.setTextSize(2);
    tft.drawString("---", 238, TEMP_Y + TEMP_H/2 + 8);
  }
}

// ─── Update cook timer display ────────────────────────────────────────────────
void updateTimer() {
  unsigned long elapsed = cookTimerAccum;
  if (cookTimerRunning) elapsed += millis() - cookTimerStart;

  unsigned long totalMin = elapsed / 60000UL;
  unsigned long hh = totalMin / 60;
  unsigned long mm = totalMin % 60;

  tft.fillRect(MARGIN+2, TIMER_Y + 18, SCREEN_W - MARGIN*2 - 4, TIMER_H - 20, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_GREEN, C_PANEL);
  tft.setTextSize(3);
  char buf[8]; snprintf(buf, sizeof(buf), "%lu:%02lu", hh, mm);
  tft.drawString(buf, 160, TIMER_Y + TIMER_H/2 + 9);
}

// ─── Draw graph ───────────────────────────────────────────────────────────────
void drawGraph() {
  if (historyCount < 2) return;

  // Use a sprite to avoid flickering
  spr.createSprite(GW, GH);
  spr.fillSprite(C_PANEL);

  // Find min/max across both series for auto-scaling
  uint8_t minT = 255, maxT = 0;
  for (uint8_t i = 0; i < historyCount; i++) {
    uint8_t p = getProbe(i), b = getBox(i);
    if (p < minT) minT = p;
    if (b < minT) minT = b;
    if (p > maxT) maxT = p;
    if (b > maxT) maxT = b;
  }

  // Add breathing room — round to nearest 10°F
  uint8_t lo = (minT > 10) ? ((minT - 10) / 10) * 10 : 0;
  uint8_t hi = ((maxT + 10) / 10) * 10;
  if (hi == lo) hi = lo + 50;

  // Grid lines + y-axis labels (drawn into sprite, offset from sprite origin)
  uint8_t gridLines = 4;
  for (uint8_t g = 0; g <= gridLines; g++) {
    int yy = GH - 1 - (g * (GH-1) / gridLines);
    spr.drawFastHLine(0, yy, GW, C_DKGRAY);
    uint8_t labelTemp = lo + g * (hi - lo) / gridLines;
    char lbl[5]; snprintf(lbl, sizeof(lbl), "%d", labelTemp);
    spr.setTextColor(C_GRAY);
    spr.setTextSize(1);
    spr.setTextDatum(MR_DATUM);
    // labels need to go left of the plot — we'll draw them on the tft directly
  }

  // Plot both lines
  for (uint8_t i = 1; i < historyCount; i++) {
    int x0 = (int)((i-1) * (long)(GW-1) / (HISTORY-1));
    int x1 = (int)(i     * (long)(GW-1) / (HISTORY-1));

    // probe line
    int yp0 = GH - 1 - (int)((long)(getProbe(i-1) - lo) * (GH-1) / (hi - lo));
    int yp1 = GH - 1 - (int)((long)(getProbe(i)   - lo) * (GH-1) / (hi - lo));
    yp0 = constrain(yp0, 0, GH-1);
    yp1 = constrain(yp1, 0, GH-1);
    spr.drawLine(x0, yp0, x1, yp1, C_PROBE);

    // box line
    int yb0 = GH - 1 - (int)((long)(getBox(i-1) - lo) * (GH-1) / (hi - lo));
    int yb1 = GH - 1 - (int)((long)(getBox(i)   - lo) * (GH-1) / (hi - lo));
    yb0 = constrain(yb0, 0, GH-1);
    yb1 = constrain(yb1, 0, GH-1);
    spr.drawLine(x0, yb0, x1, yb1, C_BOX);
  }

  // Push sprite to screen
  spr.pushSprite(GX, GY);
  spr.deleteSprite();

  // Y-axis labels (drawn on tft, left of sprite area)
  tft.fillRect(MARGIN+1, GY, GX - MARGIN - 2, GH, C_PANEL);
  tft.setTextSize(1);
  tft.setTextDatum(MR_DATUM);
  for (uint8_t g = 0; g <= gridLines; g++) {
    int yy = GY + GH - 1 - (g * (GH-1) / gridLines);
    uint8_t labelTemp = lo + g * (hi - lo) / gridLines;
    char lbl[5]; snprintf(lbl, sizeof(lbl), "%d", labelTemp);
    tft.setTextColor(C_GRAY, C_PANEL);
    tft.drawString(lbl, GX - 2, yy);
  }

  // Legend — bottom of graph panel
  int legendY = GRAPH_Y + GRAPH_H - 12;
  tft.fillRect(MARGIN+2, legendY - 8, SCREEN_W - MARGIN*2 - 4, 18, C_PANEL);
  tft.setTextDatum(ML_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(C_PROBE, C_PANEL);
  tft.drawString("- PROBE", 30, legendY);
  tft.setTextColor(C_BOX, C_PANEL);
  tft.drawString("- SMOKER", 120, legendY);
}

// ─── Status bar ───────────────────────────────────────────────────────────────
void updateStatus(const String& msg, uint16_t color = C_WHITE) {
  tft.fillRect(MARGIN+2, STATUS_Y + 2, SCREEN_W - MARGIN*2 - 4, STATUS_H - 4, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, C_PANEL);
  tft.setTextSize(1);
  tft.drawString(msg, 160, STATUS_Y + STATUS_H/2);
}



// ─── NimBLE callback ──────────────────────────────────────────────────────────
void onSmokerNotify(NimBLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  decodePacket(pData, length);
}

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pC) {
    Serial.println("Disconnected.");
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

  NimBLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
  if (!pSvc) {
    updateStatus("Service not found!", C_RED);
    pClient->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* pChar = pSvc->getCharacteristic(CHAR_NOTIFY_UUID);
  if (!pChar || !pChar->canNotify()) {
    updateStatus("Notify char missing!", C_RED);
    pClient->disconnect();
    return false;
  }

  pChar->subscribe(true, onSmokerNotify, true);
  Serial.println("Subscribed.");
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
  // Auto-start cook timer at boot
  cookTimerStart   = millis();
  cookTimerRunning = true;
  updateTimer();
  updateStatus("Initialising BLE...", C_GRAY);

  NimBLEDevice::init("CYD-Smoker");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  while (!connectToSmoker()) { delay(3000); }
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
unsigned long lastTimerDraw = 0;
unsigned long lastGraphDraw = 0;
bool graphNeedsRedraw = false;

void loop() {
  // Reconnect if dropped
  if (!pClient || !pClient->isConnected()) {
    updateStatus("Reconnecting...", C_YELLOW);
    delay(3000);
    connectToSmoker();
    return;
  }

  // Process new BLE data
  if (newData) {
    newData = false;

    uint8_t pb = (uint8_t)constrain(probeTempF, 0, 255);
    uint8_t bx = (uint8_t)constrain(boxTempF,   0, 255);
    pushHistory(pb, bx);
    graphNeedsRedraw = true;

    if (probeTempF != lastProbeTempF) {
      updateProbeTemp(probeTempF);
      lastProbeTempF = probeTempF;
    }
    if (boxTempF != lastBoxTempF) {
      updateBoxTemp(boxTempF);
      lastBoxTempF = boxTempF;
    }
  }

  // Update timer display every second
  unsigned long now = millis();
  if (now - lastTimerDraw >= 1000) {
    lastTimerDraw = now;
    updateTimer();
  }

  // Redraw graph at most once per second (throttle sprite allocation)
  if (graphNeedsRedraw && now - lastGraphDraw >= 1000) {
    lastGraphDraw = now;
    graphNeedsRedraw = false;
    drawGraph();
  }

  delay(50);
}
