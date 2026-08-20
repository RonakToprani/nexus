#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
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

// ------------------------------------------------------------ custom media
// /api/image.bin: "NX01" kind:u8 frames:u8 delay:u16 w:u16 h:u16, RGB565-LE.
// Stills (320x240) stream straight to the panel. GIF packs (160x120 frames)
// are saved once to LittleFS, then played locally with 2x pixel doubling.

static const char* GIF_PATH = "/gif.bin";
static int mKind = -1, mFrames = 0, mDelay = 100, mW = 0, mH = 0;
static int gifIdx = 0;
static uint32_t loadedRev = 0xFFFFFFFF;
static uint32_t lastFrameAt = 0;
static uint8_t* frameBuf = nullptr;   // one 160x120 GIF frame (38.4 KB)

// panel is BGR-wired and raw pushImage bypasses LGFX color conversion:
// swap R/B on the standard RGB565 the server sends
static inline void swapRB(uint16_t* px, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t v = px[i];
    px[i] = (uint16_t)(((v & 0x001F) << 11) | (v & 0x07E0) | ((v & 0xF800) >> 11));
  }
}

static bool readFull(WiFiClient* s, uint8_t* dst, size_t need) {
  size_t got = 0;
  while (got < need) {
    size_t r = s->readBytes(dst + got, need - got);
    if (r == 0) return false;
    got += r;
  }
  return true;
}

static bool fetchMedia() {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(15000);
  if (!http.begin(String(serverUrl) + "/api/image.bin")) return false;
  if (http.GET() != 200) { http.end(); return false; }
  WiFiClient* s = http.getStreamPtr();
  s->setTimeout(5000);
  uint8_t hdr[12];
  bool ok = readFull(s, hdr, 12) && memcmp(hdr, "NX01", 4) == 0;
  if (ok) {
    mKind = hdr[4]; mFrames = hdr[5];
    mDelay = hdr[6] | (hdr[7] << 8);
    mW = hdr[8] | (hdr[9] << 8);
    mH = hdr[10] | (hdr[11] << 8);
    if (mDelay < 30) mDelay = 100;
  }
  static uint8_t row[320 * 2];
  if (ok && mKind == 0 && mW == 320 && mH == 240) {   // still: straight to panel
    lcd.setSwapBytes(true);
    lcd.startWrite();
    for (int y = 0; y < mH && ok; y++) {
      ok = readFull(s, row, mW * 2);
      if (ok) { swapRB((uint16_t*)row, mW); lcd.pushImage(0, y, mW, 1, (uint16_t*)row); }
    }
    lcd.endWrite();
  } else if (ok && mKind == 1 && mW == 160 && mH == 120 && mFrames >= 1) {
    File f = LittleFS.open(GIF_PATH, "w");   // gif: swap once while saving
    ok = (bool)f;
    size_t left = (size_t)mFrames * mW * mH * 2;
    while (ok && left > 0) {
      size_t chunk = left < sizeof(row) ? left : sizeof(row);
      ok = readFull(s, row, chunk);
      if (ok) {
        swapRB((uint16_t*)row, chunk / 2);
        ok = f.write(row, chunk) == chunk;
        left -= chunk;
      }
    }
    if (f) f.close();
    gifIdx = 0;
    Serial.printf("[media] gif %d frames, %d ms/frame, ok=%d\n", mFrames, mDelay, ok);
  } else {
    ok = false;
  }
  http.end();
  if (!ok) mKind = -1;
  return ok;
}

static void drawGifFrame() {
  if (!frameBuf) frameBuf = (uint8_t*)malloc(160 * 120 * 2);
  if (!frameBuf) return;
  File f = LittleFS.open(GIF_PATH, "r");
  if (!f) { mKind = -1; return; }
  f.seek((size_t)gifIdx * 160 * 120 * 2);
  size_t n = f.read(frameBuf, 160 * 120 * 2);
  f.close();
  if (n != 160 * 120 * 2) { mKind = -1; return; }
  static uint16_t line[320];
  lcd.setSwapBytes(true);
  lcd.startWrite();
  for (int y = 0; y < 120; y++) {
    uint16_t* src = (uint16_t*)(frameBuf + y * 160 * 2);
    for (int x = 0; x < 160; x++) { line[2 * x] = src[x]; line[2 * x + 1] = src[x]; }
    lcd.pushImage(0, y * 2, 320, 1, line);
    lcd.pushImage(0, y * 2 + 1, 320, 1, line);
  }
  lcd.endWrite();
  gifIdx = (gifIdx + 1) % mFrames;
}

static void customEnter() {
  // resume a cached GIF without re-downloading; anything else refetches
  if (!(mKind == 1 && loadedRev == vImgRev)) loadedRev = 0xFFFFFFFF;
}

static void customMediaMode() {
  if (vImgRev != loadedRev) {
    if (fetchMedia()) {
      loadedRev = vImgRev;
    } else {
      lcd.fillScreen(TFT_BLACK);
      lcd.setFont(&fonts::Font2);
      lcd.setTextDatum(lgfx::middle_center);
      lcd.setTextColor(0x07FF);
      lcd.drawString("NO MEDIA YET", lcd.width() / 2, lcd.height() / 2 - 10);
      lcd.setTextColor(0xFFFF);
      lcd.drawString("Upload from the portal", lcd.width() / 2, lcd.height() / 2 + 14);
      delay(3000);   // retry on the next pass
      return;
    }
  }
  if (mKind == 1 && millis() - lastFrameAt >= (uint32_t)mDelay) {
    lastFrameAt = millis();
    drawGifFrame();
  }
  delay(mKind == 1 ? 4 : 60);
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

  LittleFS.begin(true);   // GIF frame cache; formats on first use
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
    if (curAnim == ANIM_COUNT) customEnter();   // entering custom media mode
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
    customMediaMode();
    return;
  }

  uint32_t start = millis();
  ANIMS[curAnim].frame(spr, tick++, vSpeed / 5.0f);

  if (pushScale == 1) spr.pushSprite(0, 0);
  else spr.pushRotateZoom(lcd.width() / 2.0f, lcd.height() / 2.0f, 0, 2.0f, 2.0f);

  uint32_t el = millis() - start;
  if (el < 33) delay(33 - el);
}
