#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define DC_PIN        16
#define RST_PIN       17
#define CS_PIN        5

void setup() {
  Serial.begin(115200);
  if(!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println("SSD1306 allocation failed or not found");
    for(;;);
  }
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0)); // 4 MHz вместо 8
  display.begin(DISPLAY_ADDRESS, true);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("SPI OLED OK!");
  display.display();
}

void loop() {}