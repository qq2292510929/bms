/*
 * JK BMS 蓝牙监控仪表盘 - 赛车模式
 * 硬件: ESP32-32E + ST7789 2.8寸 320x240 横屏
 * BMS: JK_BD4A24S10P (JK02_32S协议)
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

// ==================== 颜色定义 ====================
#define CLR_BG          0x0000
#define CLR_NEON_BLUE   0x04FF
#define CLR_NEON_GREEN  0x07E0
#define CLR_NEON_YELLOW 0xFFE0
#define CLR_NEON_RED    0xF800
#define CLR_NEON_CYAN   0x07FF
#define CLR_NEON_ORANGE 0xFD20
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x4208
#define CLR_DIM_CYAN    0x0210

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
bool bleConnected = false;

// ==================== BLE 全局变量 ====================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pWriteChar = nullptr;
BLERemoteCharacteristic* pNotifyChar = nullptr;

bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* advDevice = nullptr;

uint8_t frameBuf[MAX_FRAME_SIZE];
int framePos = 0;
bool frameStarted = false;

unsigned long lastDisplayUpdate = 0;
unsigned long lastCommandTime = 0;
unsigned long lastScanTime = 0;
const unsigned long DISPLAY_INTERVAL = 100;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;

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
uint16_t getPowerColor(float power, bool isCharging) {
  if (!bms.dataValid) return CLR_GRAY;
  
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

// ==================== 绘制环形进度条 ====================
void drawArc(int cx, int cy, int r, int startAngle, int endAngle, uint16_t color, int thickness) {
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

// ==================== 绘制赛车风格仪表盘 ====================
void drawRaceDashboard() {
  if (needFullRedraw) {
    tft.fillScreen(CLR_BG);
    needFullRedraw = false;
  }

  tft.fillRect(0, 0, SCREEN_W, SCREEN_H, CLR_BG);

  uint16_t powerColor = getPowerColor(bms.power, bms.isCharging);

  tft.setTextColor(CLR_NEON_CYAN);
  tft.setTextSize(1);
  tft.setCursor(10, 8);
  tft.print("JK BMS RACE");

  if (bleConnected) {
    tft.fillCircle(SCREEN_W - 15, 12, 5, CLR_NEON_GREEN);
  } else {
    tft.fillCircle(SCREEN_W - 15, 12, 5, CLR_NEON_RED);
  }

  // 功率显示区 - 左侧
  tft.setTextSize(2);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(10, 45);
  tft.print("POWER");

  tft.setTextSize(5);
  tft.setTextColor(powerColor);
  tft.setCursor(10, 65);
  
  char powerStr[16];
  if (!bms.dataValid) {
    strcpy(powerStr, "---");
  } else {
    float absP = abs(bms.power);
    if (absP >= 1000) {
      sprintf(powerStr, "%d", (int)absP);
    } else if (absP >= 100) {
      dtostrf(absP, 1, 1, powerStr);
    } else {
      dtostrf(absP, 1, 2, powerStr);
    }
  }
  
  if (bms.isCharging && bms.dataValid) tft.print("+");
  else if (bms.isDischarging && bms.dataValid) tft.print("-");
  tft.print(powerStr);
  
  tft.setTextSize(2);
  tft.setCursor(130, 85);
  tft.print("W");

  // 状态文字
  tft.setTextSize(1);
  tft.setCursor(10, 120);
  if (!bms.dataValid) {
    tft.setTextColor(CLR_GRAY);
    tft.print("NO SIGNAL");
  } else if (bms.isCharging) {
    tft.setTextColor(CLR_NEON_GREEN);
    tft.print("CHARGING");
  } else if (bms.isDischarging) {
    tft.setTextColor(CLR_NEON_RED);
    tft.print("DISCHARGE");
  } else {
    tft.setTextColor(CLR_GRAY);
    tft.print("IDLE");
  }

  // SOC环形显示 - 右侧
  int socCX = 250;
  int socCY = 100;
  int socRadius = 45;
  
  uint16_t socColor;
  if (!bms.dataValid) socColor = CLR_GRAY;
  else if (bms.soc > 60) socColor = CLR_NEON_GREEN;
  else if (bms.soc > 30) socColor = CLR_NEON_YELLOW;
  else if (bms.soc > 15) socColor = CLR_NEON_ORANGE;
  else socColor = CLR_NEON_RED;

  drawArc(socCX, socCY, socRadius, 135, 405, CLR_GRAY, 3);
  if (bms.dataValid) {
    int fillAngle = 135 + (405 - 135) * bms.soc / 100;
    drawArc(socCX, socCY, socRadius, 135, fillAngle, socColor, 3);
  }

  tft.setTextSize(3);
  tft.setTextColor(socColor);
  char socStr[8];
  if (bms.dataValid) sprintf(socStr, "%d", bms.soc);
  else strcpy(socStr, "--");
  tft.setCursor(socCX - 20, socCY - 12);
  tft.print(socStr);
  
  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(socCX + 15, socCY - 5);
  tft.print("%");

  tft.setTextSize(1);
  tft.setTextColor(CLR_DIM_CYAN);
  tft.setCursor(socCX - 25, socCY + 20);
  tft.print("SOC");

  // 底部状态栏
  tft.drawFastHLine(0, 160, SCREEN_W, CLR_DIM_CYAN);

  tft.setTextSize(1);
  tft.setTextColor(CLR_NEON_CYAN);
  tft.setCursor(10, 175);
  tft.print("TEMP:");
  tft.setTextColor(CLR_WHITE);
  if (bms.dataValid) {
    char tStr[8];
    dtostrf(bms.temp1, 1, 1, tStr);
    tft.print(tStr);
    tft.print("C");
  } else {
    tft.print("--");
  }

  tft.setTextSize(1);
  tft.setTextColor(CLR_NEON_YELLOW);
  tft.setCursor(110, 175);
  tft.print("VOLT:");
  tft.setTextColor(CLR_WHITE);
  if (bms.dataValid) {
    char vStr[8];
    dtostrf(bms.voltage, 1, 1, vStr);
    tft.print(vStr);
    tft.print("V");
  } else {
    tft.print("--");
  }

  tft.setTextSize(1);
  tft.setTextColor(CLR_NEON_ORANGE);
  tft.setCursor(210, 175);
  tft.print("CURR:");
  tft.setTextColor(CLR_WHITE);
  if (bms.dataValid) {
    char iStr[8];
    dtostrf(bms.current, 1, 1, iStr);
    tft.print(iStr);
    tft.print("A");
  } else {
    tft.print("--");
  }

  // 容量显示
  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(10, 195);
  if (bms.dataValid) {
    char capStr[24];
    sprintf(capStr, "CAP: %.1f/%.1f Ah", bms.capacity_remain, bms.capacity_nominal);
    tft.print(capStr);
  } else {
    tft.print("CAP: --/-- Ah");
  }
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

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(SCREEN_W, SCREEN_H);
  tft.setRotation(1);
  tft.invertDisplay(false);
  tft.fillScreen(CLR_BG);

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
