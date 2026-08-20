#pragma once

// ---- Board: Freenove FNK0114A 2.4" ESP32 Display (CYD class) ----
// ESP32-WROOM-32E, ST7789 240x320 IPS on HSPI, XPT2046 resistive touch.
// Pinout verified against Freenove's own TFT_eSPI setup file
// (FNK0114A_2.4_240x320_ST7789.h) and schematic.
#define PIN_TFT_SCLK 14
#define PIN_TFT_MOSI 13
#define PIN_TFT_MISO 12
#define PIN_TFT_CS   15
#define PIN_TFT_DC    2
#define PIN_TFT_RST  -1   // tied to board reset
#define PIN_TFT_BL   21   // backlight (PWM), active high

#define TFT_SPI_FREQ 80000000
#define TFT_INVERT   true   // IPS ST7789 needs inversion on
#define TFT_RGB_ORDER_BGR true

// XPT2046 resistive touch (separate soft-SPI bus)
#define PIN_TOUCH_CLK 25
#define PIN_TOUCH_CS  33
#define PIN_TOUCH_DIN 32
#define PIN_TOUCH_DO  39
#define PIN_TOUCH_IRQ 36

// RGB status LED, common anode (active LOW) — driven HIGH to keep it off
#define PIN_LED_R 22
#define PIN_LED_G 16
#define PIN_LED_B 17

#define PIN_BOOT_BTN 0    // hold at power-up to open the WiFi config portal

// Landscape, USB on the right. Use 3 to flip 180 degrees.
#define SCREEN_ROTATION 1

// Default portal address; changeable from the WiFi setup page.
#define DEFAULT_SERVER_URL "http://10.0.0.192:8484"

#define POLL_INTERVAL_MS 2500
#define WIFI_AP_NAME "NEXUS-DISPLAY"
