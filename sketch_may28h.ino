/*
 * JK BMS 蓝牙监控仪表盘 (横屏最终精简版)
 * 硬件: ESP32-32E + ST7789 2.8寸 TPM408-2.8 320x240
 * BMS:  JK_BD4A24S10P (JK02_32S协议)
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
#define BMS_MAC  "98:da:20:07:b9:00"
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
#define CLR_BG          0x0000
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
#define CLR_BAR_BG      0x18C3
#define CLR_ACCENT      0x055F
// ===================== 显示对象 =====================
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2CN(U8G2_R0, 33, 34, U8X8_PIN_NONE);
#define CN_BUF_MAX_PIXELS (256 * 20)
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
const unsigned long DISPLAY_INTERVAL = 500;
const unsigned long COMMAND_INTERVAL = 8000;
const unsigned long SCAN_INTERVAL = 5000;
const unsigned long DATA_TIMEOUT = 30000;
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
// ===================== 显示辅助函数 =====================
uint16_t getSocColor(int soc) {
  if (soc > 60) return CLR_GREEN;
  if (soc > 30) return CLR_YELLOW;
  if (soc > 15) return CLR_ORANGE;
  return CLR_RED;
}
uint16_t getPowerColor() {
  if (bms.isCharging)    return CLR_GREEN;
  if (bms.isDischarging) return CLR_RED;
  return CLR_GRAY;
}
uint16_t getPowerBarColor() {
  if (bms.isCharging)    return CLR_DIM_GREEN;
  if (bms.isDischarging) return CLR_DIM_RED;
  return CLR_DARK_GRAY;
}
void drawProgressBar(int x, int y, int w, int h, int percent, uint16_t fillColor) {
  tft.fillRect(x + 2, y + 2, w - 4, h - 4, CLR_BAR_BG);
  int fillW = (int)((w - 4) * (float)percent / 100.0f);
  if (fillW > 0) {
    tft.fillRect(x + 2, y + 2, fillW, h - 4, fillColor);
  }
  tft.drawFastHLine(x, y, w, CLR_GRAY);
  tft.drawFastHLine(x, y + h - 1, w, CLR_GRAY);
  tft.drawFastVLine(x, y, h, CLR_GRAY);
  tft.drawFastVLine(x + w - 1, y, h, CLR_GRAY);
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
// ===================== 绘制静态元素 (已按要求修改) =====================
void drawStaticElements() {
  tft.drawFastHLine(0, 32, SCREEN_W, CLR_ACCENT);
  drawCN12("功率", 20, 42, CLR_GRAY);
  // 已删除"电池电量"标签
  tft.drawFastVLine(160, 40, 140, CLR_DARK_GRAY);
  tft.drawFastHLine(0, 180, SCREEN_W, CLR_DARK_GRAY);
  drawCN12("温度", 15, 190, CLR_CYAN); // 已改为"温度"
  drawCN12("电压", 115, 190, CLR_YELLOW);
  drawCN12("电流", 220, 190, CLR_ORANGE);
}
// ===================== 绘制动态元素 (已按要求修改) =====================
void drawDynamicElements() {
  // ---- 标题栏 ----
  tft.fillRect(8, 4, 200, 14, CLR_BG);
  drawCN12("JK BMS 蓝牙监控", 8, 4, CLR_DIM_CYAN);
  tft.fillRect(8, 18, 200, 10, CLR_BG);
  tft.setTextSize(1);
  tft.setTextColor(CLR_DARK_GRAY);
  tft.setCursor(8, 20);
  tft.print(BMS_NAME);
  // BLE状态灯
  if (bleConnected) {
    tft.fillCircle(SCREEN_W - 10, 12, 4, CLR_GREEN);
    tft.drawCircle(SCREEN_W - 10, 12, 6, CLR_DIM_GREEN);
  } else {
    tft.fillCircle(SCREEN_W - 10, 12, 4, CLR_RED);
    tft.drawCircle(SCREEN_W - 10, 12, 6, CLR_DIM_RED);
  }
  // ---- 左侧功率区域 ----
  uint16_t pwrColor = getPowerColor();
  uint16_t pwrBarColor = getPowerBarColor();
  tft.fillRect(0, 34, 160, 4, pwrBarColor);
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
  tft.fillRect(20, 56, 120, 48, CLR_BG);
  tft.setTextSize(5);
  tft.setTextColor(pwrColor);
  tft.setCursor(20, 58);
  if (bms.isCharging && bms.dataValid) tft.print(F("+"));
  else if (bms.isDischarging && bms.dataValid) tft.print(F("-"));
  tft.print(pwrStr);
  tft.setTextSize(2);
  tft.setTextColor(pwrColor);
  tft.setCursor(120, 80);
  tft.print(F("W"));
  // 充放电状态 (已改为"待连接")
  tft.fillRect(15, 110, 120, 18, CLR_BG);
  if (!bms.dataValid) {
    drawCN14("待连接", 15, 110, CLR_GRAY);
  } else if (bms.isCharging) {
    drawCN14("\xe2\x96\xb2 充电中", 15, 110, CLR_GREEN);
  } else if (bms.isDischarging) {
    drawCN14("\xe2\x96\xbc 放电中", 15, 110, CLR_RED);
  } else {
    drawCN14("-- 待机", 15, 110, CLR_GRAY);
  }
  // ---- 右侧电池容量区域 ----
  uint16_t socColor = getSocColor(bms.soc);
  char socStr[8];
  if (bms.dataValid) {
    sprintf(socStr, "%d%%", bms.soc);
  } else {
    strcpy(socStr, "--%");
  }
  tft.fillRect(180, 56, 120, 48, CLR_BG);
  tft.setTextSize(5);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(180, 58);
  tft.print(socStr);
  drawProgressBar(180, 110, 120, 16, bms.dataValid ? bms.soc : 0, socColor);
  tft.fillRect(180, 132, 120, 12, CLR_BG);
  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(180, 132);
  if (bms.dataValid) {
    char capStr[32];
    sprintf(capStr, "%.1f/%.1fAh", bms.capacity_remain, bms.capacity_nominal);
    tft.print(capStr);
  } else {
    tft.print(F("---/---Ah"));
  }
  float usedCap = bms.capacity_nominal - bms.capacity_remain;
  if (bms.dataValid && usedCap > 0) {
    char usedStr[16];
    sprintf(usedStr, "-%.1fAh", usedCap);
    int16_t x1, y1;
    uint16_t w, h;
    tft.getTextBounds(usedStr, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor(300 - w, 132);
    tft.setTextColor(CLR_DIM_RED);
    tft.print(usedStr);
  }
  // ---- 底部状态栏 ----
  tft.fillRect(15, 210, 80, 22, CLR_BG);
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(15, 210);
  if (bms.dataValid) {
    char t1Str[12];
    dtostrf(bms.temp1, 1, 1, t1Str);
    tft.print(t1Str);
    tft.setTextSize(1);
    tft.print(F("°C"));
  } else {
    tft.print(F("--"));
  }
  tft.fillRect(115, 210, 80, 22, CLR_BG);
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(115, 210);
  if (bms.dataValid) {
    char vStr[12];
    dtostrf(bms.voltage, 1, 1, vStr);
    tft.print(vStr);
    tft.setTextSize(1);
    tft.print(F("V"));
  } else {
    tft.print(F("--"));
  }
  tft.fillRect(220, 210, 80, 22, CLR_BG);
  tft.setTextSize(2);
  tft.setTextColor(CLR_WHITE);
  tft.setCursor(220, 210);
  if (bms.dataValid) {
    char iStr[12];
    dtostrf(bms.current, 1, 2, iStr);
    tft.print(iStr);
    tft.setTextSize(1);
    tft.print(F("A"));
  } else {
    tft.print(F("--"));
  }
  // 连接状态
  tft.fillRect(260, 190, 50, 14, CLR_BG);
  if (bms.dataValid && (millis() - bms.lastUpdate > DATA_TIMEOUT)) {
    drawCN12("超时!", 260, 190, CLR_RED);
  } else if (bleConnected) {
    drawCN12("在线", 270, 190, CLR_DIM_GREEN);
  }
}
// ===================== 主界面绘制 =====================
void drawDashboard() {
  if (needFullRedraw) {
    tft.fillScreen(CLR_BG);
    drawStaticElements();
    drawDynamicElements();
    needFullRedraw = false;
    savePrevData();
    return;
  }
  if (!dataChanged() && !newDataReady) return;
  newDataReady = false;
  drawDynamicElements();
  savePrevData();
}
// ===================== 启动画面 =====================
void drawSplash() {
  tft.fillScreen(CLR_BG);
  tft.drawFastHLine(80, 60, 160, CLR_ACCENT);
  tft.drawFastHLine(80, 180, 160, CLR_ACCENT);
  drawCN14("JK BMS", 115, 70, CLR_CYAN);
  drawCN12("蓝牙监控", 105, 100, CLR_GRAY);
  tft.setTextSize(1);
  tft.setTextColor(CLR_DIM_CYAN);
  tft.setCursor(90, 130);
  tft.print(BMS_NAME);
  drawCN12("扫描中...", 125, 160, CLR_GRAY);
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
  tft.fillScreen(CLR_BG);
  u8g2CN.setFont(u8g2_font_wqy12_t_chinese3);
  u8g2CN.setFontMode(1);
  u8g2CN.setFontPosTop();
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  drawSplash();
  delay(1000);
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
    drawDashboard();
    lastDisplayUpdate = millis();
  }
  if (!bleConnected && !doConnect) {
    if (millis() - lastScanTime > SCAN_INTERVAL) {
      Serial.println(F("[系统] 重新扫描..."));
      BLEScan* pScan = BLEDevice::getScan();
      pScan->start(10, false);
      lastScanTime = millis();
    }
  }
  delay(50);
}
