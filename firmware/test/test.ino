#include <M5StickC.h>

void setup() {
  M5.begin();
  M5.Lcd.setRotation(3);
  M5.Lcd.fillScreen(RED);
  M5.Lcd.setTextColor(WHITE, RED);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 20);
  M5.Lcd.println("HELLO");
}

void loop() {
  M5.update();
  delay(100);
}
