/*
 * JK BMS 蓝牙监控仪表盘 (战斗/赛车运动风格)
 * 硬件: ESP32-32E + ST7789 2.8寸 320x240 横屏
 * BMS:  JK_BD4A24S10P (JK02_32S协议)
 * 风格: 战斗/汽车运动风 - 圆角卡片、渐变色彩、流光动画
 *
 * 需安装库:
 *   - Adafruit ST7789
 *   - Adafruit GFX Library
 *   - U8g2  (中文字体渲染)
 *   - ESP32 BLE Arduino (随ESP32核心自带)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2lib.h>

// ===================== 引脚配置 =====================
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1
#define TFT_SCLK  14
#define TFT_MOSI  13
#define TFT_BL    21

// ===================== BLE 配置 =====================
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

// ===================== 显示参数 =====================
#define SCREEN_W 320
#define SCREEN_H 240

// 战斗风格色彩系统
#define CLR_BG          0x0841  // 深蓝背景 (5, 10, 20)
#define CLR_BG_DARK     0x0420  // 更深的蓝
#define CLR_CARD_BG     0x10A2  // 卡片背景 (8, 20, 30)
#define CLR_BORDER      0x2965  // 边框色 (20, 45, 70)
#define CLR_BORDER_GLOW 0x42C9 // 发光边框 (30, 90, 150)

#define CLR_CYAN        0x07FF
#define CLR_GREEN       0x07E0
#define CLR_RED         0xF800
#define CLR_ORANGE      0xFD20
#define CLR_YELLOW      0xFFE0
#define CLR_WHITE       0xFFFF
#define CLR_GRAY        0x632C
#define CLR_DARK_GRAY   0x2104
#define CLR_DIM_CYAN    0x0418
#define CLR_DIM_GREEN   0x0340
#define CLR_DIM_RED     0x6000
#define CLR_ACCENT      0x055F

// 功率警示色阶
#define CLR_PWR_IDLE    0x4208  // 深紫
#define CLR_PWR_LOW     0x03E0  // 绿
#define CLR_PWR_MED     0x07FF  // 青
#define CLR_PWR_HIGH    0xFD20  // 橙
#define CLR_PWR_EXTREME 0xF800  // 红

// ===================== 显示对象 =====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2CN(U8G2_R0, 33, 34, U8X8_PIN_NONE);
#define CN_BUF_MAX_PIXELS (128 * 20)
static uint16_t cnRgbBuf[CN_BUF_MAX_PIXELS];

// ===================== BMS 数据结构 =====================
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
  float cap_remain;
  float cap_nominal;
  bool  isCharging;
  bool  isDischarging;
  bool  bleConnected;
  bool  dataValid;
};

PrevDisplay prev;

// ===================== 动画状态 =====================
uint8_t glowPhase = 0;
uint8_t streamerPos = 0;
unsigned long lastAnimUpdate = 0;

// ===================== BLE 全局变量 =====================
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
const unsigned long DATA_TIMEOUT = 30000;
const unsigned long ANIM_INTERVAL = 50;

bool needFullRedraw = true;

// ===================== CRC 计算 =====================
uint8_t calcCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ===================== 发送BLE命令 =====================
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

// ===================== 解析JK02_32S帧 =====================
void parseCellInfoFrame() {
  if (framePos < MIN_FRAME_SIZE) return;

  uint8_t crc = calcCRC(frameBuf, framePos - 1);
  if (crc != frameBuf[framePos - 1]) {
    Serial.println(F("[BMS] CRC校验失败"));
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

  Serial.printf("[BMS] V=%.2f P=%.1f I=%.2f SOC=%d%% T1=%.1f Cap=%.1f/%.1f\n",
    bms.voltage, bms.power, bms.current, bms.soc, bms.temp1,
    bms.capacity_remain, bms.capacity_nominal);
}

// ===================== BLE 通知回调 =====================
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

// ===================== BLE 客户端回调 =====================
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    needFullRedraw = true;
    Serial.println(F("[BLE] 已连接"));
  }
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    doConnect = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    needFullRedraw = true;
    Serial.println(F("[BLE] 已断开"));
  }
};

// ===================== BLE 扫描回调 =====================
class MyScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == BMS_MAC) {
      Serial.println(F("[BLE] 发现目标BMS!"));
      BLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// ===================== 连接BMS =====================
bool connectToBMS() {
  Serial.println(F("[BLE] 正在连接..."));

  if (!advDevice) return false;

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(advDevice)) {
    Serial.println(F("[BLE] 连接失败!"));
    delete pClient;
    pClient = nullptr;
    return false;
  }

  BLERemoteService* pService = pClient->getService(serviceUUID);
  if (!pService) {
    Serial.println(F("[BLE] 未找到服务!"));
    pClient->disconnect();
    return false;
  }

  std::map<std::string, BLERemoteCharacteristic*>* charMap = pService->getCharacteristics();
  for (auto& kv : *charMap) {
    BLERemoteCharacteristic* c = kv.second;
    if (c->canWrite() && !pWriteChar) {
      pWriteChar = c;
      Serial.println(F("[BLE] 找到写入特征"));
    }
    if (c->canNotify() && !pNotifyChar) {
      pNotifyChar = c;
      Serial.println(F("[BLE] 找到通知特征"));
    }
  }

  if (!pNotifyChar) {
    Serial.println(F("[BLE] 未找到通知特征!"));
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

// ===================== 中文渲染核心 =====================
void drawCN(const char* text, int x, int y, uint16_t fgColor, const uint8_t* font) {
  u8g2CN.setFont(font);
  u8g2CN.setFontMode(1);
  u8g2CN.setFontPosTop();

  int textW = u8g2CN.getUTF8Width(text);
  int textH = u8g2CN.getMaxCharHeight();

  if (textW <= 0 || textH <= 0) return;
  if (textW > 128) textW = 128;
  if (textH > 20) textH = 20;
  if (textW * textH > CN_BUF_MAX_PIXELS) return;

  u8g2CN.clearBuffer();
  u8g2CN.drawUTF8(0, 0, text);

  uint8_t* buf = u8g2CN.getBufferPtr();
  int pixelWidth = 128;

  for (int py = 0; py < textH; py++) {
    for (int px = 0; px < textW; px++) {
      int byteIdx = (py / 8) * pixelWidth + px;
      int bit = py % 8;
      bool isSet = (buf[byteIdx] >> bit) & 1;
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

// ===================== 图形绘制辅助函数 =====================
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t lerpColor(uint16_t c1, uint16_t c2, float t) {
  uint8_t r1 = (c1 >> 11) & 0x1F;
  uint8_t g1 = (c1 >> 5) & 0x3F;
  uint8_t b1 = c1 & 0x1F;
  
  uint8_t r2 = (c2 >> 11) & 0x1F;
  uint8_t g2 = (c2 >> 5) & 0x3F;
  uint8_t b2 = c2 & 0x1F;
  
  uint8_t r = r1 + (r2 - r1) * t;
  uint8_t g = g1 + (g2 - g1) * t;
  uint8_t b = b1 + (b2 - b1) * t;
  
  return (r << 11) | (g << 5) | b;
}

void drawRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, r, color);
}

void drawRoundRectBorder(int x, int y, int w, int h, int r, uint16_t borderColor, uint16_t bgColor) {
  tft.fillRoundRect(x, y, w, h, r, borderColor);
  tft.fillRoundRect(x + 2, y + 2, w - 4, h - 4, r - 1, bgColor);
}

uint16_t getPowerColor(float power) {
  float absP = fabs(power);
  if (absP < 50) return CLR_PWR_IDLE;
  if (absP < 200) return lerpColor(CLR_PWR_LOW, CLR_PWR_MED, (absP - 50) / 150);
  if (absP < 500) return lerpColor(CLR_PWR_MED, CLR_PWR_HIGH, (absP - 200) / 300);
  return lerpColor(CLR_PWR_HIGH, CLR_PWR_EXTREME, min((absP - 500) / 500, 1.0f));
}

uint16_t getGlowColor(float power, uint8_t phase) {
  uint16_t base = getPowerColor(power);
  float t = (sin(phase * PI / 128) + 1) / 2;
  return lerpColor(base, CLR_WHITE, t * 0.3f);
}

uint16_t getSocColor(int soc) {
  if (soc > 60) return CLR_GREEN;
  if (soc > 30) return CLR_YELLOW;
  if (soc > 15) return CLR_ORANGE;
  return CLR_RED;
}

void drawGlowLine(int x, int y, int w, uint16_t color, uint8_t thickness) {
  for (int i = 0; i < thickness; i++) {
    float alpha = 1.0f - (float)i / thickness;
    uint16_t dimColor = lerpColor(color, CLR_BG, 1 - alpha * 0.5f);
    tft.drawFastHLine(x, y + i, w, dimColor);
    if (i > 0) tft.drawFastHLine(x, y - i, w, dimColor);
  }
}

void drawProgressBarRacing(int x, int y, int w, int h, int percent, uint16_t fillColor, uint8_t glowPhase) {
  tft.fillRoundRect(x, y, w, h, 4, CLR_CARD_BG);
  tft.drawRoundRect(x, y, w, h, 4, CLR_BORDER);
  
  int fillW = (int)((w - 4) * (float)percent / 100.0f);
  if (fillW > 0) {
    tft.fillRoundRect(x + 2, y + 2, fillW, h - 4, 3, fillColor);
    
    uint16_t glowColor = lerpColor(fillColor, CLR_WHITE, 0.3f);
    tft.drawFastHLine(x + 2, y + 2, fillW, glowColor);
  }
}

void drawStreamer(int x, int y, int w, uint8_t pos, uint16_t color) {
  int streamerW = 40;
  int startX = x + (pos * (w + streamerW)) / 255 - streamerW;
  
  for (int i = 0; i < streamerW; i++) {
    float t = (float)i / streamerW;
    float alpha = sin(t * PI);
    if (startX + i >= x && startX + i < x + w) {
      uint16_t c = lerpColor(CLR_BG, color, alpha);
      tft.drawPixel(startX + i, y, c);
      tft.drawPixel(startX + i, y + 1, c);
    }
  }
}

// ===================== 变化检测 =====================
bool dataChanged() {
  bool changed = false;
  if (bms.power != prev.power) changed = true;
  if (bms.voltage != prev.voltage) changed = true;
  if (bms.current != prev.current) changed = true;
  if (bms.temp1 != prev.temp1) changed = true;
  if (bms.soc != prev.soc) changed = true;
  if (bms.capacity_remain != prev.cap_remain) changed = true;
  if (bms.capacity_nominal != prev.cap_nominal) changed = true;
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
  prev.cap_remain = bms.capacity_remain;
  prev.cap_nominal = bms.capacity_nominal;
  prev.isCharging = bms.isCharging;
  prev.isDischarging = bms.isDischarging;
  prev.dataValid = bms.dataValid;
  prev.bleConnected = bleConnected;
}

// ===================== 绘制战斗风格UI =====================
void drawStaticElementsRacing() {
  tft.fillScreen(CLR_BG);
  
  tft.drawFastHLine(0, 0, SCREEN_W, CLR_BORDER_GLOW);
  tft.drawFastHLine(0, SCREEN_H - 1, SCREEN_W, CLR_BORDER_GLOW);
  tft.drawFastVLine(0, 0, SCREEN_H, CLR_BORDER_GLOW);
  tft.drawFastVLine(SCREEN_W - 1, 0, SCREEN_H, CLR_BORDER_GLOW);
  
  drawRoundRectBorder(5, 5, 310, 35, 8, CLR_BORDER, CLR_BG_DARK);
  drawCN16("JK BMS 动力监测系统", 20, 10, CLR_CYAN);
  
  drawRoundRectBorder(5, 45, 195, 125, 12, CLR_BORDER, CLR_CARD_BG);
  drawCN14("输出功率", 15, 55, CLR_DIM_CYAN);
  
  drawRoundRectBorder(205, 45, 110, 125, 12, CLR_BORDER, CLR_CARD_BG);
  drawCN14("电池状态", 215, 55, CLR_DIM_CYAN);
  
  drawRoundRectBorder(5, 175, 310, 60, 10, CLR_BORDER, CLR_CARD_BG);
  drawCN12("电池温度", 20, 182, CLR_DIM_CYAN);
  drawCN12("总电压", 125, 182, CLR_DIM_CYAN);
  drawCN12("电流", 230, 182, CLR_DIM_CYAN);
}

void drawDynamicElementsRacing() {
  uint16_t pwrColor = getPowerColor(bms.power);
  uint16_t glowColor = getGlowColor(bms.power, glowPhase);
  
  tft.fillRoundRect(7, 47, 191, 121, 10, CLR_CARD_BG);
  
  drawCN14("输出功率", 15, 55, CLR_DIM_CYAN);
  
  float absPower = fabs(bms.power);
  char pwrStr[16];
  
  if (!bms.dataValid) {
    strcpy(pwrStr, "---");
  } else if (absPower >= 1000.0f) {
    dtostrf(absPower, 1, 0, pwrStr);
  } else if (absPower >= 100.0f) {
    dtostrf(absPower, 1, 1, pwrStr);
  } else if (absPower >= 10.0f) {
    dtostrf(absPower, 1, 1, pwrStr);
  } else {
    dtostrf(absPower, 1, 2, pwrStr);
  }
  
  tft.setTextSize(5);
  tft.setTextColor(pwrColor);
  tft.setCursor(20, 80);
  if (bms.isCharging && bms.dataValid) tft.print(F("+"));
  else if (bms.isDischarging && bms.dataValid) tft.print(F("-"));
  tft.print(pwrStr);
  
  tft.setTextSize(2);
  tft.setTextColor(glowColor);
  tft.setCursor(155, 95);
  tft.print(F("W"));
  
  drawGlowLine(15, 135, 175, glowColor, 2);
  drawStreamer(15, 135, 175, streamerPos, glowColor);
  
  tft.fillRect(15, 145, 175, 18, CLR_CARD_BG);
  if (!bms.dataValid) {
    drawCN14("等待连接...", 15, 145, CLR_GRAY);
  } else if (bms.isCharging) {
    drawCN14("\xe2\x96\xb2 充电中", 15, 145, CLR_GREEN);
  } else if (bms.isDischarging) {
    drawCN14("\xe2\x96\xbc 放电中", 15, 145, CLR_RED);
  } else {
    drawCN14("-- 待机", 15, 145, CLR_GRAY);
  }
  
  tft.fillRoundRect(207, 47, 106, 121, 10, CLR_CARD_BG);
  drawCN14("电池状态", 215, 55, CLR_DIM_CYAN);
  
  uint16_t socColor = getSocColor(bms.soc);
  char socStr[8];
  if (bms.dataValid) {
    sprintf(socStr, "%d%%", bms.soc);
  } else {
    strcpy(socStr, "--%");
  }
  
  tft.setTextSize(4);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(220, 75);
  tft.print(socStr);
  
  drawProgressBarRacing(215, 115, 90, 14, bms.dataValid ? bms.soc : 0, socColor, glowPhase);
  
  tft.fillRect(215, 135, 90, 25, CLR_CARD_BG);
  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(215, 135);
  if (bms.dataValid) {
    char capStr[32];
    sprintf(capStr, "%.1f/%.1fAh", bms.capacity_remain, bms.capacity_nominal);
    tft.print(capStr);
    
    float usedCap = bms.capacity_nominal - bms.capacity_remain;
    if (usedCap > 0) {
      char usedStr[16];
      sprintf(usedStr, "-%.1fAh", usedCap);
      int16_t x1, y1;
      uint16_t w, h;
      tft.getTextBounds(usedStr, 0, 0, &x1, &y1, &w, &h);
      tft.setCursor(300 - w, 145);
      tft.setTextColor(CLR_DIM_RED);
      tft.print(usedStr);
    }
  } else {
    tft.print(F("---/---Ah"));
  }
  
  tft.fillRoundRect(7, 177, 306, 56, 8, CLR_CARD_BG);
  drawCN12("电池温度", 20, 182, CLR_DIM_CYAN);
  drawCN12("总电压", 125, 182, CLR_DIM_CYAN);
  drawCN12("电流", 230, 182, CLR_DIM_CYAN);
  
  tft.fillRect(15, 200, 90, 28, CLR_CARD_BG);
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(15, 202);
  if (bms.dataValid) {
    char t1Str[12];
    dtostrf(bms.temp1, 1, 1, t1Str);
    tft.print(t1Str);
    tft.setTextSize(1);
    tft.print(F("\xF7""C"));
  } else {
    tft.print(F("--"));
  }
  
  tft.fillRect(120, 200, 90, 28, CLR_CARD_BG);
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(120, 202);
  if (bms.dataValid) {
    char vStr[12];
    dtostrf(bms.voltage, 1, 1, vStr);
    tft.print(vStr);
    tft.setTextSize(1);
    tft.print(F("V"));
  } else {
    tft.print(F("--"));
  }
  
  tft.fillRect(225, 200, 85, 28, CLR_CARD_BG);
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(225, 202);
  if (bms.dataValid) {
    char iStr[12];
    dtostrf(bms.current, 1, 2, iStr);
    tft.print(iStr);
    tft.setTextSize(1);
    tft.print(F("A"));
  } else {
    tft.print(F("--"));
  }
  
  tft.fillCircle(SCREEN_W - 20, 22, 8, CLR_BG_DARK);
  if (bleConnected) {
    tft.fillCircle(SCREEN_W - 20, 22, 6, CLR_GREEN);
    tft.drawCircle(SCREEN_W - 20, 22, 8, CLR_DIM_GREEN);
    if (glowPhase % 32 < 16) {
      tft.drawCircle(SCREEN_W - 20, 22, 10, CLR_DIM_GREEN);
    }
  } else {
    tft.fillCircle(SCREEN_W - 20, 22, 6, CLR_RED);
    tft.drawCircle(SCREEN_W - 20, 22, 8, CLR_DIM_RED);
  }
  
  if (bms.dataValid && (millis() - bms.lastUpdate > DATA_TIMEOUT)) {
    tft.fillRect(240, 10, 60, 14, CLR_BG_DARK);
    drawCN12("超时!", 245, 10, CLR_RED);
  } else if (bleConnected) {
    tft.fillRect(240, 10, 50, 14, CLR_BG_DARK);
    drawCN12("在线", 250, 10, CLR_DIM_GREEN);
  }
}

void drawSplashRacing() {
  tft.fillScreen(CLR_BG);
  
  drawRoundRectBorder(40, 40, 240, 160, 15, CLR_BORDER_GLOW, CLR_CARD_BG);
  
  drawCN16("JK BMS", 110, 60, CLR_CYAN);
  drawCN14("动力监测系统", 85, 90, CLR_GRAY);
  
  tft.setTextSize(1);
  tft.setTextColor(CLR_DIM_CYAN);
  tft.setCursor(80, 120);
  tft.print(BMS_NAME);
  
  for (int i = 0; i < 3; i++) {
    uint16_t color = lerpColor(CLR_CYAN, CLR_BG, 0.5f);
    tft.drawFastHLine(80 + i * 10, 150 + i * 2, 160 - i * 20, color);
  }
  
  drawCN12("扫描中...", 125, 165, CLR_GRAY);
}

void drawDashboardRacing() {
  if (needFullRedraw) {
    drawStaticElementsRacing();
    drawDynamicElementsRacing();
    needFullRedraw = false;
    savePrevData();
    return;
  }
  
  if (!dataChanged() && !newDataReady) return;
  newDataReady = false;
  
  drawDynamicElementsRacing();
  savePrevData();
}

void updateAnimation() {
  unsigned long now = millis();
  if (now - lastAnimUpdate > ANIM_INTERVAL) {
    glowPhase = (glowPhase + 4) % 256;
    streamerPos = (streamerPos + 5) % 256;
    lastAnimUpdate = now;
    
    if (bleConnected || bms.dataValid) {
      tft.fillRoundRect(7, 133, 191, 5, 2, CLR_CARD_BG);
      uint16_t pwrColor = getPowerColor(bms.power);
      uint16_t glowColor = getGlowColor(bms.power, glowPhase);
      drawGlowLine(15, 135, 175, glowColor, 2);
      drawStreamer(15, 135, 175, streamerPos, glowColor);
      
      tft.fillCircle(SCREEN_W - 20, 22, 10, CLR_BG_DARK);
      if (bleConnected) {
        tft.fillCircle(SCREEN_W - 20, 22, 6, CLR_GREEN);
        tft.drawCircle(SCREEN_W - 20, 22, 8, CLR_DIM_GREEN);
        if (glowPhase % 32 < 16) {
          tft.drawCircle(SCREEN_W - 20, 22, 10, CLR_DIM_GREEN);
        }
      } else {
        tft.fillCircle(SCREEN_W - 20, 22, 6, CLR_RED);
        tft.drawCircle(SCREEN_W - 20, 22, 8, CLR_DIM_RED);
      }
    }
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  Serial.println(F("[系统] 启动中..."));

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

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setRotation(3);
  tft.invertDisplay(false);

  u8g2CN.setFont(u8g2_font_wqy12_t_chinese3);
  u8g2CN.setFontMode(1);
  u8g2CN.setFontPosTop();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  drawSplashRacing();
  delay(1500);

  BLEDevice::init("");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new MyScanCallback());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(30, false);

  Serial.println(F("[系统] BLE扫描已启动"));
}

// ===================== LOOP =====================
void loop() {
  if (doConnect) {
    doConnect = false;
    if (connectToBMS()) {
      Serial.println(F("[系统] BMS连接成功!"));
    } else {
      Serial.println(F("[系统] BMS连接失败，将重试..."));
      doScan = true;
    }
  }

  if (bleConnected && (millis() - lastCommandTime > COMMAND_INTERVAL)) {
    sendBMSCommand(CMD_CELL_INFO);
    lastCommandTime = millis();
  }

  if (millis() - lastDisplayUpdate > DISPLAY_INTERVAL) {
    drawDashboardRacing();
    lastDisplayUpdate = millis();
  }

  updateAnimation();

  if (!bleConnected && !doConnect) {
    if (millis() - lastScanTime > SCAN_INTERVAL) {
      Serial.println(F("[系统] 重新扫描..."));
      BLEScan* pScan = BLEDevice::getScan();
      pScan->start(10, false);
      lastScanTime = millis();
    }
  }

  delay(10);
}
