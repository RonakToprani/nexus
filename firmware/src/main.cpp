#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "display.hpp"
#include "anims.hpp"

LGFX lcd;
LGFX_Sprite spr(&lcd);
Preferences prefs;

static char serverUrl[96] = DEFAULT_SERVER_URL;
static bool shouldSaveCfg = false;

// written by the poll task (core 0), read by the render loop (core 1)
// vAnim == ANIM_COUNT is the special "custom image" mode
static volatile int vAnim = 2;      // "hud"
static volatile int vSpeed = 5;
static volatile int vBright = 90;
static volatile uint32_t vImgRev = 0;
static volatile bool vOnline = false;

static int curAnim = -1;
static int curBright = -1;
static int pushScale = 1;

// ------------------------------------------------------------ boot screen

static void bootMsg(const char* l1, const char* l2, const char* l3, const char* l4) {
  lcd.fillScreen(TFT_BLACK);
  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(lgfx::middle_center);
  int cx = lcd.width() / 2, cy = lcd.height() / 2;
  lcd.setTextColor(0x07FF);
  lcd.drawString("NEXUS DISPLAY", cx, cy - 50);
  lcd.setTextColor(0xFFFF);
  if (l1) lcd.drawString(l1, cx, cy - 15);
  if (l2) lcd.drawString(l2, cx, cy + 8);
  if (l3) lcd.drawString(l3, cx, cy + 31);
  lcd.setTextColor(0xF81F);
  if (l4) lcd.drawString(l4, cx, cy + 60);
}

static void apModeCallback(WiFiManager* wm) {
  bootMsg("WiFi setup needed:",
          "1. Join WiFi \"" WIFI_AP_NAME "\"",
          "2. Open 192.168.4.1",
          "Set WiFi + portal URL there");
}

// ------------------------------------------------------------ server poll

static void pollTask(void*) {
  uint32_t lastRev = 0;
  int fails = 0;
  bool onFallback = false;
  uint32_t lastReconnect = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vOnline = false;
      if (millis() - lastReconnect > 30000) {  // keep retrying WiFi forever
        lastReconnect = millis();
        WiFi.reconnect();
      }
      vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
      continue;
    }
    const char* base = onFallback ? DEFAULT_SERVER_URL : serverUrl;
    HTTPClient http;
    String url = String(base) + "/api/state?device=1";
    http.setConnectTimeout(2000);
    http.setTimeout(2000);
    bool ok = false;
    if (http.begin(url)) {
      int code = http.GET();
      if (code == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
          uint32_t rev = doc["rev"] | 0;
          if (rev != lastRev) {   // only apply changes, so touch-cycling sticks
            lastRev = rev;
            const char* id = doc["anim"] | "hud";
            if (strcmp(id, "custom") == 0) {
              vAnim = ANIM_COUNT;
            } else {
              int idx = animIndexById(id);
              if (idx >= 0) vAnim = idx;
            }
            vImgRev = doc["img_rev"] | 0;
            vSpeed = constrain((int)(doc["speed"] | 5), 1, 10);
            vBright = constrain((int)(doc["brightness"] | 90), 10, 100);
          }
          ok = true;
        }
      }
      http.end();
    }
    vOnline = ok;
    if (ok) {
      fails = 0;
      if (onFallback && strcmp(serverUrl, DEFAULT_SERVER_URL) != 0) {
        // the firmware default answered while the stored URL didn't: adopt it
        strlcpy(serverUrl, DEFAULT_SERVER_URL, sizeof(serverUrl));
        prefs.putString("url", serverUrl);
        onFallback = false;
        Serial.printf("[poll] adopted portal %s\n", serverUrl);
      }
    } else {
      Serial.printf("[poll] no answer from %s\n", base);
      // after 3 straight misses, alternate with the firmware default URL
      if (++fails >= 3 && strcmp(serverUrl, DEFAULT_SERVER_URL) != 0) {
        fails = 0;
        onFallback = !onFallback;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }
}

// ------------------------------------------------------------ custom image

// Stream /api/image.bin (native RGB565) row-by-row straight to the panel.
static bool drawCustomImage() {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(8000);
  if (!http.begin(String(serverUrl) + "/api/image.bin")) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  WiFiClient* s = http.getStreamPtr();
  s->setTimeout(4000);
  static uint8_t row[320 * 2];
  bool ok = true;
  lcd.setSwapBytes(true);   // buffer is native little-endian; panel wants MSB first
  lcd.startWrite();
  for (int y = 0; y < lcd.height(); y++) {
    size_t need = lcd.width() * 2, got = 0;
    while (got < need) {
      size_t r = s->readBytes(row + got, need - got);
      if (r == 0) { ok = false; break; }
      got += r;
    }
    if (!ok) break;
    // panel is BGR-wired; raw pushImage bypasses LGFX color conversion,
    // so swap the R/B channels of the standard RGB565 the server sends
    uint16_t* px = (uint16_t*)row;
    for (int x = 0; x < lcd.width(); x++) {
      uint16_t v = px[x];
      px[x] = (uint16_t)(((v & 0x001F) << 11) | (v & 0x07E0) | ((v & 0xF800) >> 11));
    }
    lcd.pushImage(0, y, lcd.width(), 1, px);
  }
  lcd.endWrite();
  http.end();
  return ok;
}

static bool customDirty = true;    // set on mode entry and when img_rev changes

static void customImageMode() {
  static uint32_t shownRev = 0;
  if (vImgRev != shownRev) customDirty = true;
  if (customDirty) {
    if (drawCustomImage()) {
      shownRev = vImgRev;
      customDirty = false;
    } else {
      lcd.fillScreen(TFT_BLACK);
      lcd.setFont(&fonts::Font2);
      lcd.setTextDatum(lgfx::middle_center);
      lcd.setTextColor(0x07FF);
      lcd.drawString("NO IMAGE YET", lcd.width() / 2, lcd.height() / 2 - 10);
      lcd.setTextColor(0xFFFF);
      lcd.drawString("Upload one from the portal", lcd.width() / 2, lcd.height() / 2 + 14);
      delay(3000);   // stay dirty: retry on the next pass
    }
  }
  delay(80);
}

// ------------------------------------------------------------ setup / loop

void setup() {
  Serial.begin(115200);

  // keep the RGB status LED dark (common anode, active low)
  pinMode(PIN_LED_R, OUTPUT); digitalWrite(PIN_LED_R, HIGH);
  pinMode(PIN_LED_G, OUTPUT); digitalWrite(PIN_LED_G, HIGH);
  pinMode(PIN_LED_B, OUTPUT); digitalWrite(PIN_LED_B, HIGH);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  pinMode(PIN_TOUCH_IRQ, INPUT);   // XPT2046 PENIRQ, low while touched

  lcd.init();
  lcd.setRotation(SCREEN_ROTATION);
  lcd.setBrightness(220);
  bootMsg("Linking WiFi...", nullptr, nullptr, nullptr);

  prefs.begin("nexus");
  String saved = prefs.getString("url", DEFAULT_SERVER_URL);
  strlcpy(serverUrl, saved.c_str(), sizeof(serverUrl));

  WiFiManager wm;
  WiFiManagerParameter pServer("server", "Portal URL (http://ip:8484)",
                               serverUrl, sizeof(serverUrl) - 1);
  wm.addParameter(&pServer);
  wm.setSaveConfigCallback([] { shouldSaveCfg = true; });
  wm.setAPCallback(apModeCallback);
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(15);
  wm.setConnectRetries(3);

  bool connected;
  if (digitalRead(PIN_BOOT_BTN) == LOW) {       // held BOOT: force config portal
    connected = wm.startConfigPortal(WIFI_AP_NAME);
  } else {
    connected = wm.autoConnect(WIFI_AP_NAME);
  }
  if (shouldSaveCfg) {
    strlcpy(serverUrl, pServer.getValue(), sizeof(serverUrl));
    prefs.putString("url", serverUrl);
  }

  if (connected) {
    bootMsg("WiFi connected", WiFi.localIP().toString().c_str(),
            serverUrl, "Tap screen to cycle scenes");
    delay(2600);
  } else {
    bootMsg("No WiFi (offline mode)", "Hold BOOT at power-up",
            "to open WiFi setup", "Tap screen to cycle scenes");
    delay(2600);
  }

  // full-frame 8-bit palette framebuffer; half resolution if RAM is tight
  spr.setColorDepth(8);
  if (!spr.createSprite(lcd.width(), lcd.height())) {
    pushScale = 2;
    if (!spr.createSprite(lcd.width() / 2, lcd.height() / 2)) {
      lcd.fillScreen(TFT_RED);
      for (;;) delay(1000);
    }
  }
  spr.createPalette();
  spr.setPivot(spr.width() / 2.0f, spr.height() / 2.0f);
  Serial.printf("[init] sprite %dx%d scale=%d heap=%u\n",
                spr.width(), spr.height(), pushScale, ESP.getFreeHeap());

  xTaskCreatePinnedToCore(pollTask, "poll", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t tick = 0;
  static uint32_t lastTap = 0;

  if (vAnim != curAnim) {
    curAnim = vAnim;
    if (curAnim == ANIM_COUNT) customDirty = true;   // entering custom image mode
    else ANIMS[curAnim].begin(spr);
  }
  if (vBright != curBright) {
    curBright = vBright;
    lcd.setBrightness(curBright * 255 / 100);
  }

  // tap anywhere: next scene (a portal change will override on its next rev)
  if (digitalRead(PIN_TOUCH_IRQ) == LOW && millis() - lastTap > 450) {
    lastTap = millis();
    vAnim = (curAnim + 1) % ANIM_COUNT;   // custom mode taps back to scene 0
  }

  if (curAnim == ANIM_COUNT) {
    customImageMode();
    return;
  }

  uint32_t start = millis();
  ANIMS[curAnim].frame(spr, tick++, vSpeed / 5.0f);

  if (pushScale == 1) spr.pushSprite(0, 0);
  else spr.pushRotateZoom(lcd.width() / 2.0f, lcd.height() / 2.0f, 0, 2.0f, 2.0f);

  uint32_t el = millis() - start;
  if (el < 33) delay(33 - el);
}
