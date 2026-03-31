#include <M5StickC.h>
#include <WiFi.h>

#define WIFI_SSID "Altibox150666"
#define WIFI_PASS "s7D77XZh"

int test = 0;
bool wifi_ok = false;
bool btnA_ok = false;
bool btnB_ok = false;

void show_test(const char* label, const char* status, uint16_t color) {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.println("=== HW TEST ===");
  M5.Lcd.println();
  M5.Lcd.setTextColor(color, BLACK);
  M5.Lcd.println(label);
  M5.Lcd.println(status);
}

void setup() {
  Serial.begin(115200);
  M5.begin();
  M5.Lcd.setRotation(3);

  Serial.println("starting hw test");

  // Test 1 — display
  M5.Lcd.fillScreen(RED);
  delay(600);
  M5.Lcd.fillScreen(GREEN);
  delay(600);
  M5.Lcd.fillScreen(BLUE);
  delay(600);
  M5.Lcd.fillScreen(BLACK);

  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("display: OK");
  M5.Lcd.println();
  Serial.println("display: OK");
  delay(800);

  // Test 2 — WiFi
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.println("wifi connecting");
  Serial.println("wifi connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    M5.Lcd.print(".");
    Serial.print(".");
    tries++;
  }
  M5.Lcd.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifi_ok = true;
    M5.Lcd.setTextColor(GREEN, BLACK);
    M5.Lcd.println("wifi: OK");
    M5.Lcd.println(WiFi.localIP());
    Serial.println("\nwifi: OK");
    Serial.println(WiFi.localIP());
  } else {
    M5.Lcd.setTextColor(RED, BLACK);
    M5.Lcd.println("wifi: FAILED");
    Serial.println("\nwifi: FAILED");
  }
  delay(1500);

  // Test 3 — buttons
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.println("button test:");
  M5.Lcd.println();
  M5.Lcd.setTextColor(YELLOW, BLACK);
  M5.Lcd.println("press BTN A");
  M5.Lcd.println("(front button)");
  Serial.println("waiting for btn A...");
}

void loop() {
  M5.update();

  if (!btnA_ok) {
    if (M5.BtnA.wasPressed()) {
      btnA_ok = true;
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.setTextColor(GREEN, BLACK);
      M5.Lcd.println("BTN A: OK");
      M5.Lcd.println();
      M5.Lcd.setTextColor(YELLOW, BLACK);
      M5.Lcd.println("press BTN B");
      M5.Lcd.println("(side button)");
      Serial.println("btn A: OK");
    }
    return;
  }

  if (!btnB_ok) {
    if (M5.BtnB.wasPressed()) {
      btnB_ok = true;
      Serial.println("btn B: OK");

      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.setTextColor(WHITE, BLACK);
      M5.Lcd.println("=== RESULTS ===");
      M5.Lcd.println();
      M5.Lcd.setTextColor(GREEN, BLACK);
      M5.Lcd.println("display:  OK");
      M5.Lcd.setTextColor(wifi_ok ? GREEN : RED, BLACK);
      M5.Lcd.printf("wifi:     %s\n", wifi_ok ? "OK" : "FAIL");
      M5.Lcd.setTextColor(GREEN, BLACK);
      M5.Lcd.println("btn A:    OK");
      M5.Lcd.println("btn B:    OK");
      M5.Lcd.println();
      M5.Lcd.setTextColor(WHITE, BLACK);
      bool all_ok = wifi_ok && btnA_ok && btnB_ok;
      M5.Lcd.setTextColor(all_ok ? GREEN : RED, BLACK);
      M5.Lcd.println(all_ok ? "ALL PASS!" : "SOME FAILED");
      Serial.println(all_ok ? "ALL PASS" : "SOME FAILED");
    }
    return;
  }

  delay(10);
}
