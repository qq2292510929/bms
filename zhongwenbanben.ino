/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 320x240 横屏
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x04FF
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_PINK   0xF81F
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_DARK_BLUE   0x0011
#define CLR_DARK_GRAY   0x18E3
#define CLR_GRAY        0x4208
#define CLR_DIM_CYAN    0x0210

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2CN(U8G2_R0, 33, 34, U8X8_PIN_NONE);
static uint16_t cnRgbBuf[128 * 20];

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  isCharging;
  bool  isDischarging;
  bool  bleConnected;
  bool  dataValid;
};
PrevDisplay prev;

// ==================== 动画状态 ====================
struct AnimationState {
  unsigned long lastFrame;
  int pulsePhase;
  float needleAngle;
  float targetNeedleAngle;
  float displayPower;
  int glowIntensity;
};
AnimationState anim;

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 33;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;
const unsigned long DATA_TIMEOUT = 30000;

bool needFullRedraw = true;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) {
    return;
  }

  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  int16_t rawT2 = (int16_t)((uint16_t)frameBuf[133] << 8 | frameBuf[132]);
  bms.temp1 = rawT1 * 0.1f;
  bms.temp2 = rawT2 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) {
      pWriteChar = c;
    }
    if (c->canNotify() && !pNotifyChar) {
      pNotifyChar = c;
    }
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 中文渲染 ====================
void drawCN(const char* text, int x, int y, uint16_t fgColor, const uint8_t* font) {
  u8g2CN.setFont(font);
  u8g2CN.setFontMode(1);
  u8g2CN.setFontPosTop();

  int textW = u8g2CN.getUTF8Width(text);
  int textH = u8g2CN.getMaxCharHeight();

  if (textW <= 0 || textH <= 0) return;
  if (textW > 128) textW = 128;
  if (textH > 20) textH = 20;
  if (textW * textH > 128 * 20) return;

  u8g2CN.clearBuffer();
  u8g2CN.drawUTF8(0, 0, text);

  uint8_t* buf = u8g2CN.getBufferPtr();
  int pixelWidth = (int)u8g2CN.getBufferTileWidth() * 8;

  for (int py = 0; py < textH; py++) {
    for (int px = 0; px < textW; px++) {
      int byteIdx = (py / 8) * pixelWidth + px;
      int bit = py % 8;
      bool isSet = false;
      if (byteIdx >= 0 && byteIdx < 1024) {
        isSet = (buf[byteIdx] >> bit) & 1;
      }
      cnRgbBuf[py * textW + px] = isSet ? fgColor : CLR_BG;
    }
  }

  tft.drawRGBBitmap(x, y, cnRgbBuf, textW, textH);
}

void drawCN12(const char* text, int x, int y, uint16_t color) {
  drawCN(text, x, y, color, u8g2_font_wqy12_t_chinese3);
}

void drawCN14(const char* text, int x, int y, uint16_t color) {
  drawCN(text, x, y, color, u8g2_font_wqy14_t_chinese3);
}

void drawCN16(const char* text, int x, int y, uint16_t color) {
  drawCN(text, x, y, color, u8g2_font_wqy16_t_chinese3);
}

// ==================== 赛车模式：功率颜色系统 ====================
uint16_t getPowerColor(float power, bool isCharging) {
  if (!bms.dataValid) return CLR_DARK_GRAY;
  
  float absP = abs(power);
  
  if (isCharging) {
    if (absP < 500) return CLR_NEON_BLUE;
    if (absP < 1500) return CLR_NEON_CYAN;
    if (absP < 3000) return CLR_NEON_GREEN;
    return CLR_NEON_YELLOW;
  } else {
    if (absP < 500) return CLR_NEON_GREEN;
    if (absP < 1500) return CLR_NEON_YELLOW;
    if (absP < 3000) return CLR_NEON_ORANGE;
    return CLR_NEON_RED;
  }
}

// ==================== 赛车模式：绘制环形进度条 ====================
void drawArc(int cx, int cy, int r, int startAngle, int endAngle, uint16_t color, int thickness = 3) {
  for (int t = 0; t < thickness; t++) {
    int rr = r - t;
    for (int angle = startAngle; angle <= endAngle; angle++) {
      float rad = angle * PI / 180.0f;
      int x = cx + rr * cos(rad);
      int y = cy + rr * sin(rad);
      tft.drawPixel(x, y, color);
    }
  }
}

// ==================== 赛车模式：绘制霓虹边框 ====================
void drawNeonRect(int x, int y, int w, int h, uint16_t color) {
  tft.drawFastHLine(x, y, w, color);
  tft.drawFastHLine(x, y + h - 1, w, color);
  tft.drawFastVLine(x, y, h, color);
  tft.drawFastVLine(x + w - 1, y, h, color);
}

// ==================== 赛车模式：绘制环形SOC ====================
void drawRaceSOC(int cx, int cy, int radius, int soc, uint16_t color) {
  int startAngle = 135;
  int endAngle = 405;
  int fillAngle = startAngle + (endAngle - startAngle) * soc / 100;
  
  drawArc(cx, cy, radius, startAngle, endAngle, CLR_DARK_GRAY, 4);
  
  if (soc > 0) {
    drawArc(cx, cy, radius, startAngle, fillAngle, color, 4);
  }
  
  drawArc(cx, cy, radius + 6, startAngle - 2, startAngle + 2, CLR_DIM_CYAN, 2);
  drawArc(cx, cy, radius + 6, endAngle - 2, endAngle + 2, CLR_DIM_CYAN, 2);
}

// ==================== 赛车模式：绘制扫描线效果 ====================
void drawScanline() {
  static int scanY = 0;
  unsigned long now = millis();
  
  if (now - anim.lastFrame > 50) {
    scanY = (scanY + 2) % SCREEN_H;
    anim.lastFrame = now;
  }
  
  tft.drawFastHLine(0, scanY, SCREEN_W, 0x0841);
  if (scanY > 0) tft.drawFastHLine(0, scanY - 1, SCREEN_W, 0x0420);
}

// ==================== 赛车模式：主界面 ====================
void drawRaceDashboard() {
  unsigned long now = millis();
  
  if (needFullRedraw) {
    tft.fillScreen(CLR_BG);
    needFullRedraw = false;
  }

  tft.fillRect(0, 0, SCREEN_W, SCREEN_H, CLR_BG);

  drawNeonRect(2, 2, SCREEN_W - 4, SCREEN_H - 4, CLR_DIM_CYAN);

  uint16_t powerColor = getPowerColor(bms.power, bms.isCharging);
  
  tft.setTextColor(powerColor);
  tft.setTextSize(1);
  tft.setCursor(10, 8);
  tft.print("RACE MODE");

  if (bleConnected) {
    tft.fillCircle(SCREEN_W - 15, 12, 5, CLR_NEON_GREEN);
    tft.drawCircle(SCREEN_W - 15, 12, 8, CLR_DIM_CYAN);
  } else {
    tft.fillCircle(SCREEN_W - 15, 12, 5, CLR_NEON_RED);
  }

  int powerCX = 100;
  int powerCY = 120;
  
  char powerStr[16];
  if (!bms.dataValid) {
    strcpy(powerStr, "---");
  } else {
    float absP = abs(bms.power);
    if (absP >= 10000) {
      sprintf(powerStr, "%d", (int)absP);
    } else if (absP >= 1000) {
      dtostrf(absP, 1, 0, powerStr);
    } else if (absP >= 100) {
      dtostrf(absP, 1, 1, powerStr);
    } else {
      dtostrf(absP, 1, 2, powerStr);
    }
  }

  tft.setTextSize(6);
  tft.setTextColor(powerColor);
  tft.setCursor(powerCX - 60, powerCY - 30);
  if (bms.isCharging && bms.dataValid) tft.print("+");
  else if (bms.isDischarging && bms.dataValid) tft.print("-");
  tft.print(powerStr);

  tft.setTextSize(3);
  tft.setCursor(powerCX + 40, powerCY - 10);
  tft.print("W");

  tft.setTextSize(2);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(powerCX - 50, powerCY + 35);
  if (!bms.dataValid) {
    tft.print("NO SIGNAL");
  } else if (bms.isCharging) {
    tft.setTextColor(CLR_NEON_GREEN);
    tft.print("CHARGING");
  } else if (bms.isDischarging) {
    tft.setTextColor(CLR_NEON_RED);
    tft.print("DISCHARGE");
  } else {
    tft.print("IDLE");
  }

  int socCX = 260;
  int socCY = 120;
  int socRadius = 60;
  
  uint16_t socColor;
  if (!bms.dataValid) socColor = CLR_DARK_GRAY;
  else if (bms.soc > 60) socColor = CLR_NEON_GREEN;
  else if (bms.soc > 30) socColor = CLR_NEON_YELLOW;
  else if (bms.soc > 15) socColor = CLR_NEON_ORANGE;
  else socColor = CLR_NEON_RED;

  drawRaceSOC(socCX, socCY, socRadius, bms.dataValid ? bms.soc : 0, socColor);

  tft.setTextSize(4);
  tft.setTextColor(socColor);
  char socStr[8];
  if (bms.dataValid) sprintf(socStr, "%d", bms.soc);
  else strcpy(socStr, "--");
  tft.setCursor(socCX - 25, socCY - 15);
  tft.print(socStr);
  
  tft.setTextSize(2);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(socCX + 10, socCY - 5);
  tft.print("%");

  tft.setTextSize(1);
  tft.setTextColor(CLR_DIM_CYAN);
  tft.setCursor(socCX - 25, socCY + 20);
  tft.print("BATTERY");

  int gaugeY = 200;
  drawNeonRect(10, gaugeY - 5, SCREEN_W - 20, 45, CLR_DARK_GRAY);

  tft.setTextSize(1);
  
  tft.setTextColor(CLR_NEON_CYAN);
  tft.setCursor(25, gaugeY + 5);
  tft.print("TEMP");
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(20, gaugeY + 20);
  if (bms.dataValid) {
    char tStr[8];
    dtostrf(bms.temp1, 1, 0, tStr);
    tft.print(tStr);
    tft.setTextSize(1);
    tft.print("C");
  } else {
    tft.print("--");
  }

  tft.setTextSize(1);
  tft.setTextColor(CLR_NEON_YELLOW);
  tft.setCursor(115, gaugeY + 5);
  tft.print("VOLT");
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(110, gaugeY + 20);
  if (bms.dataValid) {
    char vStr[8];
    dtostrf(bms.voltage, 1, 1, vStr);
    tft.print(vStr);
    tft.setTextSize(1);
    tft.print("V");
  } else {
    tft.print("--");
  }

  tft.setTextSize(1);
  tft.setTextColor(CLR_NEON_ORANGE);
  tft.setCursor(210, gaugeY + 5);
  tft.print("CURR");
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(205, gaugeY + 20);
  if (bms.dataValid) {
    char iStr[8];
    dtostrf(bms.current, 1, 1, iStr);
    tft.print(iStr);
    tft.setTextSize(1);
    tft.print("A");
  } else {
    tft.print("--");
  }

  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(5, SCREEN_H - 10);
  tft.print("JK BMS");

  drawScanline();

  savePrevData();
}

// ==================== 变化检测 ====================
bool dataChanged() {
  bool changed = false;
  if (bms.power != prev.power) changed = true;
  if (bms.voltage != prev.voltage) changed = true;
  if (bms.current != prev.current) changed = true;
  if (bms.temp1 != prev.temp1) changed = true;
  if (bms.soc != prev.soc) changed = true;
  if (bms.isCharging != prev.isCharging) changed = true;
  if (bms.isDischarging != prev.isDischarging) changed = true;
  if (bms.dataValid != prev.dataValid) changed = true;
  if (bleConnected != prev.bleConnected) changed = true;
  return changed;
}

void savePrevData() {
  prev.power = bms.power;
  prev.voltage = bms.voltage;
  prev.current = bms.current;
  prev.temp1 = bms.temp1;
  prev.soc = bms.soc;
  prev.isCharging = bms.isCharging;
  prev.isDischarging = bms.isDischarging;
  prev.dataValid = bms.dataValid;
  prev.bleConnected = bleConnected;
}

// ==================== 启动画面 ====================
void drawSplash() {
  tft.fillScreen(CLR_BG);
  
  tft.setTextSize(3);
  tft.setTextColor(CLR_NEON_CYAN);
  tft.setCursor(80, 80);
  tft.print("JK BMS");
  
  tft.setTextSize(2);
  tft.setTextColor(CLR_DIM_CYAN);
  tft.setCursor(60, 120);
  tft.print("RACE MODE");
  
  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(100, 160);
  tft.print("Connecting...");
  
  drawNeonRect(10, 10, SCREEN_W - 20, SCREEN_H - 20, CLR_DIM_CYAN);
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);

  bms.dataValid = false;
  bms.soc = 0;
  bms.voltage = 0;
  bms.power = 0;
  bms.current = 0;
  bms.temp1 = 0;
  bms.temp2 = 0;
  bms.capacity_remain = 0;
  bms.capacity_nominal = 0;
  bms.isCharging = false;
  bms.isDischarging = false;
  bms.lastUpdate = 0;

  memset(&prev, 0xFF, sizeof(prev));
  prev.bleConnected = !bleConnected;
  needFullRedraw = true;

  anim.lastFrame = 0;
  anim.pulsePhase = 0;
  anim.needleAngle = 0;
  anim.targetNeedleAngle = 0;
  anim.displayPower = 0;
  anim.glowIntensity = 0;

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(SCREEN_H, SCREEN_W);
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(CLR_BG);

  u8g2CN.setFont(u8g2_font_wqy12_t_chinese3);
  u8g2CN.setFontMode(1);
  u8g2CN.setFontPosTop();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  drawSplash();
  delay(1500);

  BLEDevice::init("");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new MyScanCallback());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(30, false);
}

// ==================== LOOP ====================
void loop() {
  if (doConnect) {
    doConnect = false;
    connectToBMS();
  }

  if (bleConnected && (millis() - lastCommandTime > COMMAND_INTERVAL)) {
    sendBMSCommand(CMD_CELL_INFO);
    lastCommandTime = millis();
  }

  if (millis() - lastDisplayUpdate > DISPLAY_INTERVAL) {
    drawRaceDashboard();
    lastDisplayUpdate = millis();
  }

  if (!bleConnected && !doConnect) {
    if (millis() - lastScanTime > SCAN_INTERVAL) {
      BLEScan* pScan = BLEDevice::getScan();
      pScan->start(10, false);
      lastScanTime = millis();
    }
  }

  delay(10);
}
/*
 * JK BMS/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLE/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FR/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adaf/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarg/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL =/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA,/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

///*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) */*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint3/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frame/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.0/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw =/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connect/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!adv/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient =/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  p/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemote/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService)/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->get/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLER/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() &&/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotify/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar &&/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotify/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime =/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 功率颜色系统 ====================/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 功率颜色系统 ====================
uint16_t getPowerColor(float power/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 功率颜色系统 ====================
uint16_t getPowerColor(float power) {
  if (!bms.dataValid) return CLR_GRAY;
  float/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 功率颜色系统 ====================
uint16_t getPowerColor(float power) {
  if (!bms.dataValid) return CLR_GRAY;
  float absP = abs(power);
  
  if/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 功率颜色系统 ====================
uint16_t getPowerColor(float power) {
  if (!bms.dataValid) return CLR_GRAY;
  float absP = abs(power);
  
  if (bms.isCharging) {
/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 功率颜色系统 ====================
uint16_t getPowerColor(float power) {
  if (!bms.dataValid) return CLR_GRAY;
  float absP = abs(power);
  
  if (bms.isCharging) {
    if (absP < 500) return CLR_NEON_BLUE;
/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 240x320 竖屏改横屏显示
 * BMS: JK_BD4A24S10P (JK02_32S协议)
 * 风格: 赛车仪表盘 - 霓虹科技感
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ==================== 引脚配置 ====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ==================== BLE 配置 ====================
#define BMS_MAC  "98:DA:20:07:B9:00"
#define BMS_NAME "JK_BD4A24S10P"

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charUUID((uint16_t)0xFFE1);

#define CMD_CELL_INFO   0x96
#define CMD_DEVICE_INFO 0x97

#define FRAME_HEADER_0 0x55
#define FRAME_HEADER_1 0xAA
#define FRAME_HEADER_2 0xEB
#define FRAME_HEADER_3 0x90
#define FRAME_TYPE_CELL_INFO 0x02
#define MIN_FRAME_SIZE 300
#define MAX_FRAME_SIZE 320

// ==================== 屏幕配置 ====================
// ST7789原生是240x320竖屏，我们用rotation=1转成横屏320x240
#define SCREEN_W 320
#define SCREEN_H 240

// ==================== 赛车模式颜色 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x001F
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x7BEF
#define CLR_DIM_CYAN    0x031F

// ==================== 显示对象 ====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// ==================== BMS 数据结构 ====================
struct BMSData {
  float voltage;
  float power;
  float current;
  float temp1;
  float temp2;
  int   soc;
  float capacity_remain;
  float capacity_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  dataValid;
  unsigned long lastUpdate;
};

BMSData bms;

struct PrevDisplay {
  float power;
  float voltage;
  float current;
  float temp1;
  int   soc;
  bool  bleConnected;
};
PrevDisplay prev = {0};

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool bleConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;
volatile bool newDataReady = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

bool needFullRedraw = true;
int scanlineY = 0;

// ==================== CRC 计算 ====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ==================== 发送BLE命令 ====================
void sendBMSCommand(uint8_t cmd) {
  if (!pWriteChar) return;
  uint8_t cmdFrame[20] = {
    0xAA, 0x55, 0x90, 0xEB,
    cmd, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
  };
  cmdFrame[19] = calcCRC(cmdFrame, 19);
  pWriteChar->writeValue(cmdFrame, 20, true);
}

// ==================== 解析JK02_32S帧 ====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) return;
  if (frameBuf[4] != FRAME_TYPE_CELL_INFO) return;

  bms.voltage = ((uint32_t)frameBuf[121] << 24 | (uint32_t)frameBuf[120] << 16 |
                 (uint32_t)frameBuf[119] << 8  | (uint32_t)frameBuf[118]) * 0.001f;

  bms.power = ((uint32_t)frameBuf[125] << 24 | (uint32_t)frameBuf[124] << 16 |
               (uint32_t)frameBuf[123] << 8  | (uint32_t)frameBuf[122]) * 0.001f;

  int32_t rawCurrent = (int32_t)(
    (uint32_t)frameBuf[129] << 24 | (uint32_t)frameBuf[128] << 16 |
    (uint32_t)frameBuf[127] << 8  | (uint32_t)frameBuf[126]);
  bms.current = rawCurrent * 0.001f;

  int16_t rawT1 = (int16_t)((uint16_t)frameBuf[131] << 8 | frameBuf[130]);
  bms.temp1 = rawT1 * 0.1f;

  bms.soc = frameBuf[141];

  bms.capacity_remain = ((uint32_t)frameBuf[145] << 24 | (uint32_t)frameBuf[144] << 16 |
                         (uint32_t)frameBuf[143] << 8  | (uint32_t)frameBuf[142]) * 0.001f;

  bms.capacity_nominal = ((uint32_t)frameBuf[149] << 24 | (uint32_t)frameBuf[148] << 16 |
                          (uint32_t)frameBuf[147] << 8  | (uint32_t)frameBuf[146]) * 0.001f;

  bms.isCharging    = (bms.current > 0.05f);
  bms.isDischarging = (bms.current < -0.05f);
  bms.dataValid     = true;
  bms.lastUpdate    = millis();
  newDataReady      = true;
}

// ==================== BLE 通知回调 ====================
void notifyCallback(BLERemoteCharacteristic* pChar,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length >= 4 &&
      pData[0] == FRAME_HEADER_0 && pData[1] == FRAME_HEADER_1 &&
      pData[2] == FRAME_HEADER_2 && pData[3] == FRAME_HEADER_3) {
    framePos = 0;
    frameStarted = true;
  }

  if (frameStarted) {
    for (size_t i = 0; i < length; i++) {
      if (framePos < MAX_FRAME_SIZE) {
        frameBuf[framePos++] = pData[i];
      }
      if (framePos >= MIN_FRAME_SIZE) {
        frameStarted = false;
        parseCellInfoFrame();
        break;
      }
    }
  }
}

// ==================== BLE 客户端回调 ====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
  }
};

// ==================== BLE 扫描回调 ====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ==================== 连接BMS ====================
bool connectToBMS() {
  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) pWriteChar = c;
    if (c->canNotify() && !pNotifyChar) pNotifyChar = c;
  }

  if (!pNotifyChar) {
    pClient->disconnect();
    return false;
  }

  pNotifyChar->registerForNotify(notifyCallback);

  if (!pWriteChar && pNotifyChar && pNotifyChar->canWrite()) {
    pWriteChar = pNotifyChar;
  }

  delay(500);
  sendBMSCommand(CMD_DEVICE_INFO);
  delay(500);
  sendBMSCommand(CMD_CELL_INFO);

  lastCommandTime = millis();
  return true;
}

// ==================== 功率颜色系统 ====================
uint16_t getPowerColor(float power) {
  if (!bms.dataValid) return CLR_GRAY;
  float absP = abs(power);
  
  if (bms.isCharging) {
    if (absP < 500) return CLR_NEON_BLUE;
    if (absP < 1500) return CLR_NEON_CY