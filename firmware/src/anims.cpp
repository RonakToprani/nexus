#include "anims.hpp"
#include <string.h>

// ---------------------------------------------------------------- helpers

static uint32_t s_rng = 0xC0FFEE;
static inline uint32_t rnd() {
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
  return s_rng;
}
static inline int rndi(int n) { return (int)(rnd() % (uint32_t)n); }
static inline float rndf() { return (rnd() & 0xFFFF) / 65535.0f; }

// deterministic hash for stable star/window positions & twinkle
static inline uint32_t hash2(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u;
  h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
  return h;
}

// linear palette ramp [i0..i1] from rgb0 to rgb1
static void ramp(LGFX_Sprite& g, int i0, int i1,
                 int r0, int g0, int b0, int r1, int g1, int b1) {
  int n = i1 - i0;
  for (int i = 0; i <= n; i++) {
    g.setPaletteColor(i0 + i,
      r0 + (r1 - r0) * i / n, g0 + (g1 - g0) * i / n, b0 + (b1 - b0) * i / n);
  }
}

// decrement every pixel index toward 0 — cheap phosphor-trail effect
static void fadeAll(LGFX_Sprite& g, uint8_t amt) {
  uint8_t* b = (uint8_t*)g.getBuffer();
  int n = g.width() * g.height();
  for (int i = 0; i < n; i++) {
    uint8_t v = b[i];
    b[i] = (v > amt) ? v - amt : 0;
  }
}

static uint8_t s8lut[256];
static bool s8ready = false;
static void makeS8() {
  if (s8ready) return;
  for (int i = 0; i < 256; i++)
    s8lut[i] = (uint8_t)(128.0f + 127.0f * sinf(i * 6.2831853f / 256.0f));
  s8ready = true;
}
static inline uint8_t sin8(uint8_t x) { return s8lut[x]; }

// ---------------------------------------------------------------- MATRIX

static const uint8_t GLYPHS[16][8] = { // katakana-flavored 8x8
  {0x7E,0x02,0x04,0x08,0x08,0x10,0x10,0x20}, // fu
  {0x08,0x08,0x7F,0x08,0x10,0x10,0x20,0x40}, // te
  {0x00,0x7F,0x01,0x01,0x02,0x04,0x18,0x60}, // fu2
  {0x10,0x10,0x7E,0x12,0x22,0x22,0x42,0x04}, // ka
  {0x04,0x7F,0x04,0x3C,0x04,0x04,0x08,0x30}, // o
  {0x00,0x3E,0x02,0x02,0x04,0x04,0x08,0x30}, // u
  {0x22,0x22,0x22,0x22,0x22,0x42,0x42,0x84}, // ri
  {0x7F,0x01,0x02,0x3E,0x40,0x40,0x40,0x3E}, // ra
  {0x10,0x7E,0x10,0x1C,0x12,0x10,0x10,0x60}, // sa-ish
  {0x00,0x42,0x22,0x24,0x08,0x10,0x20,0x40}, // no
  {0x18,0x08,0x7E,0x08,0x08,0x08,0x14,0x62}, // ki
  {0x02,0x3C,0x10,0x7E,0x10,0x28,0x44,0x82}, // ho
  {0x7E,0x42,0x42,0x7E,0x02,0x04,0x08,0x30}, // yo
  {0x40,0x42,0x44,0x48,0x50,0x60,0x40,0x3E}, // re
  {0x08,0x7F,0x08,0x08,0x3E,0x08,0x08,0x08}, // ju
  {0x00,0x7E,0x04,0x08,0x10,0x28,0x44,0x02}, // su
};

static void glyph(LGFX_Sprite& g, int x, int y, int gi, uint8_t c) {
  for (int r = 0; r < 8; r++) {
    uint8_t row = GLYPHS[gi & 15][r];
    for (int b = 0; b < 8; b++)
      if (row & (0x80 >> b)) g.drawPixel(x + b, y + r, c);
  }
}

#define MTX_COLS 40
static struct { float y, v; int gi; int lastCell; } mtx[MTX_COLS];

static void matrixBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 0, 0, 0);
  ramp(g, 1, 11, 0, 26, 8, 0, 224, 122);
  g.setPaletteColor(12, 102, 255, 176);
  g.setPaletteColor(13, 234, 255, 238);
  g.fillSprite(0);
  int cols = g.width() / 8;
  for (int i = 0; i < cols && i < MTX_COLS; i++) {
    mtx[i].y = -(float)rndi(g.height());
    mtx[i].v = 1.5f + rndf() * 3.5f;
    mtx[i].gi = rndi(16);
    mtx[i].lastCell = -999;
  }
}

static void matrixFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  if (t & 1) fadeAll(g, 1);
  int cols = g.width() / 8;
  if (cols > MTX_COLS) cols = MTX_COLS;
  for (int i = 0; i < cols; i++) {
    mtx[i].y += mtx[i].v * sp;
    int cell = ((int)mtx[i].y) / 9;
    if (cell != mtx[i].lastCell) {
      if (mtx[i].lastCell > -900)
        glyph(g, i * 8, mtx[i].lastCell * 9, mtx[i].gi, 10); // cool the old head
      mtx[i].gi = rndi(16);
      glyph(g, i * 8, cell * 9, mtx[i].gi, 13);
      mtx[i].lastCell = cell;
    }
    if (mtx[i].y > g.height() + 40 + rndi(120)) {
      mtx[i].y = -(float)rndi(60);
      mtx[i].v = 1.5f + rndf() * 3.5f;
      mtx[i].lastCell = -999;
    }
  }
}

// ---------------------------------------------------------------- WARP

#define NSTAR 90
static struct { float x, y, z, pz; } stars[NSTAR];

static void starReset(int i) {
  stars[i].x = rndf() * 2 - 1;
  stars[i].y = rndf() * 2 - 1;
  stars[i].z = 0.3f + rndf() * 0.7f;
  stars[i].pz = stars[i].z;
}

static void warpBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 0, 0, 4);
  ramp(g, 1, 13, 10, 16, 48, 223, 232, 255);
  g.setPaletteColor(14, 255, 255, 255);
  g.fillSprite(0);
  for (int i = 0; i < NSTAR; i++) { starReset(i); stars[i].z = 0.05f + rndf() * 0.95f; }
}

static void warpFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  fadeAll(g, 2);
  int W = g.width(), H = g.height(), cx = W / 2, cy = H / 2;
  for (int i = 0; i < NSTAR; i++) {
    stars[i].pz = stars[i].z;
    stars[i].z -= 0.010f * sp * (0.5f + stars[i].z);
    if (stars[i].z <= 0.03f) { starReset(i); continue; }
    float k = cx * 0.95f;
    int x0 = cx + (int)(stars[i].x / stars[i].pz * k);
    int y0 = cy + (int)(stars[i].y / stars[i].pz * k * 0.75f);
    int x1 = cx + (int)(stars[i].x / stars[i].z * k);
    int y1 = cy + (int)(stars[i].y / stars[i].z * k * 0.75f);
    if (x1 < 0 || x1 >= W || y1 < 0 || y1 >= H) { starReset(i); continue; }
    int c = 1 + (int)((1.0f - stars[i].z) * 12.9f);
    g.drawLine(x0, y0, x1, y1, c);
    if (stars[i].z < 0.25f) g.drawPixel(x1, y1, 14);
  }
}

// ---------------------------------------------------------------- HUD

static float hudA = 0, hudS = 0;
static float hudVal[5];

static void hudBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 1, 7, 12);
  ramp(g, 1, 8, 2, 34, 46, 0, 229, 255);
  g.setPaletteColor(9, 240, 255, 255);
  g.setPaletteColor(10, 255, 159, 64);
  g.setPaletteColor(11, 6, 48, 60);
  g.setPaletteColor(12, 255, 64, 96);
  hudA = 0; hudS = 0;
  for (int i = 0; i < 5; i++) hudVal[i] = rndf();
}

static void hudFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  g.fillSprite(0);
  int W = g.width(), H = g.height();
  int cx = W * 38 / 100, cy = H / 2;
  hudA += 1.6f * sp;
  hudS += 3.1f * sp;
  // static rings
  g.drawCircle(cx, cy, 86, 11);
  g.drawCircle(cx, cy, 62, 11);
  g.drawCircle(cx, cy, 40, 3);
  // tick ring
  for (int i = 0; i < 36; i++) {
    float a = i * 10 * 0.0174533f;
    int hot = ((int)(hudA / 10) % 36 == i) ? 9 : 4;
    g.drawLine(cx + cosf(a) * 88, cy + sinf(a) * 88,
               cx + cosf(a) * 93, cy + sinf(a) * 93, hot);
  }
  // rotating arcs
  g.fillArc(cx, cy, 80, 84, hudA, hudA + 70, 6);
  g.fillArc(cx, cy, 80, 84, hudA + 180, hudA + 250, 6);
  g.fillArc(cx, cy, 66, 70, 360 - hudA * 1.3f, 360 - hudA * 1.3f + 40, 8);
  g.fillArc(cx, cy, 66, 70, 120 - hudA * 1.3f, 160 - hudA * 1.3f, 8);
  g.fillArc(cx, cy, 66, 70, 240 - hudA * 1.3f, 280 - hudA * 1.3f, 8);
  // inner sweep
  g.fillArc(cx, cy, 8, 58, hudS, hudS + 6, 3);
  g.fillArc(cx, cy, 8, 58, hudS + 6, hudS + 10, 2);
  // crosshair + core
  g.drawFastHLine(cx - 96, cy, 192, 11);
  g.drawFastVLine(cx, cy - 96, 192, 11);
  float pulse = sinf(t * 0.09f);
  g.fillCircle(cx, cy, 5 + (int)(pulse * 2), 9);
  g.drawCircle(cx, cy, 12 + (int)(pulse * 3), 5);
  // right panel
  int px = W - 88;
  g.setFont(&fonts::Font0);
  g.setTextDatum(lgfx::top_left);
  g.setTextColor((t / 12) & 1 ? 10 : 2);
  g.drawString("SYS//ONLINE", px, 12);
  static const char* lbl[5] = {"PWR", "NAV", "SHLD", "CORE", "LINK"};
  for (int i = 0; i < 5; i++) {
    hudVal[i] += (rndf() - 0.5f) * 0.04f * sp;
    if (hudVal[i] < 0.05f) hudVal[i] = 0.05f;
    if (hudVal[i] > 1) hudVal[i] = 1;
    int y = 34 + i * 30;
    g.setTextColor(5);
    g.drawString(lbl[i], px, y);
    char v[8]; snprintf(v, 8, "%03d", (int)(hudVal[i] * 999));
    g.setTextColor(9);
    g.drawString(v, px + 52, y);
    g.drawRect(px, y + 10, 80, 7, 11);
    g.fillRect(px + 1, y + 11, (int)(78 * hudVal[i]), 5, hudVal[i] > 0.8f ? 10 : 6);
  }
  // hex ticker
  g.setTextColor(3);
  char hex[20];
  snprintf(hex, 20, "0x%08X %04X", (unsigned)hash2(t / 4, 7), (unsigned)(hash2(t / 4, 13) & 0xFFFF));
  g.drawString(hex, 10, H - 12);
  g.setTextColor(6);
  g.drawString("TGT-LOCK", cx - 24, cy - 110 < 2 ? 2 : cy - 110);
}

// ---------------------------------------------------------------- PLASMA

static float plA = 0, plB = 0, plC = 0;

static void plasmaBegin(LGFX_Sprite& g) {
  makeS8();
  for (int i = 0; i < 256; i++) { // smooth hue wheel
    float h = i / 255.0f * 6.0f;
    int s = (int)h;
    float f = h - s;
    uint8_t p = 30, q = (uint8_t)(255 * (1 - 0.88f * f)), u = (uint8_t)(255 * (1 - 0.88f * (1 - f)));
    uint8_t r, gg, b;
    switch (s % 6) {
      case 0: r = 255; gg = u; b = p; break;
      case 1: r = q; gg = 255; b = p; break;
      case 2: r = p; gg = 255; b = u; break;
      case 3: r = p; gg = q; b = 255; break;
      case 4: r = u; gg = p; b = 255; break;
      default: r = 255; gg = p; b = q; break;
    }
    g.setPaletteColor(i, r, gg, b);
  }
  plA = plB = plC = 0;
}

static void plasmaFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  plA += 2.2f * sp; plB += 1.4f * sp; plC += 0.9f * sp;
  uint8_t ta = (uint8_t)plA, tb = (uint8_t)plB, tc = (uint8_t)plC;
  uint8_t* buf = (uint8_t*)g.getBuffer();
  int W = g.width(), H = g.height();
  for (int y = 0; y < H; y += 2) {
    uint8_t* r0 = buf + y * W;
    uint8_t* r1 = r0 + W;
    uint8_t sy = sin8((uint8_t)(y * 3 + tb));
    for (int x = 0; x < W; x += 2) {
      uint8_t v = (uint8_t)((sin8((uint8_t)(x * 2 + ta)) + sy +
                             sin8((uint8_t)(((x + y) >> 1) + tc))) / 3 + (t >> 2));
      r0[x] = v; r0[x + 1] = v; r1[x] = v; r1[x + 1] = v;
    }
  }
}

// ---------------------------------------------------------------- GRID

static float gridPh = 0;

static void gridBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 5, 1, 13);
  ramp(g, 1, 16, 13, 2, 33, 90, 20, 96);       // night sky -> horizon glow
  ramp(g, 17, 24, 255, 107, 53, 255, 46, 136); // sun gradient
  g.setPaletteColor(25, 255, 79, 216);          // grid bright
  g.setPaletteColor(26, 122, 42, 110);          // grid dim
  g.setPaletteColor(27, 235, 235, 245);         // star
  g.setPaletteColor(28, 10, 1, 24);             // mountains
  gridPh = 0;
}

static void gridFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  int W = g.width(), H = g.height();
  int hor = H * 52 / 100;
  for (int y = 0; y < hor; y++) g.drawFastHLine(0, y, W, 1 + y * 15 / hor);
  g.fillRect(0, hor, W, H - hor, 0);
  // stars
  for (int i = 0; i < 40; i++) {
    uint32_t h = hash2(i, 77);
    int sx = h % W, sy = (h >> 9) % (hor * 7 / 10);
    if (((hash2(i, t / 8) >> 4) & 7) != 0) g.drawPixel(sx, sy, 27);
  }
  // sun with scanline gaps
  int scx = W / 2, scy = hor - 6, r = 46;
  for (int dy = -r; dy <= r; dy++) {
    int yy = scy + dy;
    if (yy < 0 || yy >= hor) continue;
    if (dy > 0 && (dy % 9) < 3 + dy / 12) continue;
    int half = (int)sqrtf((float)(r * r - dy * dy));
    g.drawFastHLine(scx - half, yy, half * 2, 17 + (dy + r) * 7 / (2 * r));
  }
  // mountains
  g.fillTriangle(-30, hor, 70, hor - 34, 150, hor, 28);
  g.fillTriangle(180, hor, 265, hor - 26, 350, hor, 28);
  // horizon glow
  g.drawFastHLine(0, hor, W, 25);
  g.drawFastHLine(0, hor + 1, W, 26);
  // vertical fan
  for (int k = -8; k <= 8; k++)
    g.drawLine(scx + k * 7, hor, scx + k * 46, H + 20, k == 0 ? 25 : 26);
  // moving horizontal lines
  gridPh += 0.016f * sp;
  if (gridPh >= 1) gridPh -= 1;
  for (int i = 0; i < 13; i++) {
    float z = i + gridPh;
    int y = hor + (int)((H - hor) * (z * z) / 144.0f);
    if (y >= H) continue;
    g.drawFastHLine(0, y, W, (i >= 10) ? 25 : 26);
  }
}

// ---------------------------------------------------------------- RADAR

static float radA = 0;
#define NBLIP 7
static struct { float ang, dist; int hot; } blips[NBLIP];

static void radarBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 1, 6, 4);
  ramp(g, 1, 11, 2, 22, 10, 43, 255, 122);
  g.setPaletteColor(12, 176, 255, 208);
  g.setPaletteColor(13, 255, 200, 80);
  radA = 0;
  for (int i = 0; i < NBLIP; i++) {
    blips[i].ang = rndf() * 360;
    blips[i].dist = 0.25f + rndf() * 0.7f;
    blips[i].hot = 0;
  }
}

static void radarFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  if (t & 1) fadeAll(g, 1);
  int W = g.width(), H = g.height(), cx = W / 2, cy = H / 2;
  int R = (H / 2) - 14;
  g.drawCircle(cx, cy, R, 4);
  g.drawCircle(cx, cy, R * 2 / 3, 3);
  g.drawCircle(cx, cy, R / 3, 3);
  g.drawFastHLine(cx - R, cy, 2 * R, 3);
  g.drawFastVLine(cx, cy - R, 2 * R, 3);
  for (int i = 0; i < 24; i++) {
    float a = i * 15 * 0.0174533f;
    g.drawPixel(cx + (int)(cosf(a) * (R + 5)), cy + (int)(sinf(a) * (R + 5)), 6);
  }
  radA += 2.6f * sp;
  if (radA >= 360) radA -= 360;
  float ar = radA * 0.0174533f;
  g.drawLine(cx, cy, cx + (int)(cosf(ar) * R), cy + (int)(sinf(ar) * R), 12);
  g.drawLine(cx, cy, cx + (int)(cosf(ar - 0.06f) * R), cy + (int)(sinf(ar - 0.06f) * R), 8);
  for (int i = 0; i < NBLIP; i++) {
    float d = radA - blips[i].ang;
    while (d < 0) d += 360;
    if (d < 4 * sp + 2) blips[i].hot = 26;
    if (blips[i].hot > 0) {
      blips[i].hot--;
      int bx = cx + (int)(cosf(blips[i].ang * 0.0174533f) * R * blips[i].dist);
      int by = cy + (int)(sinf(blips[i].ang * 0.0174533f) * R * blips[i].dist);
      g.fillCircle(bx, by, 2, 11);
      if (blips[i].hot > 18) g.drawCircle(bx, by, 26 - blips[i].hot + 4, 7);
      if (blips[i].hot == 0 && rndi(3) == 0) { // contact relocates
        blips[i].ang = rndf() * 360;
        blips[i].dist = 0.25f + rndf() * 0.7f;
      }
    }
  }
  g.setFont(&fonts::Font0);
  g.setTextDatum(lgfx::top_left);
  g.setTextColor(9);
  char s[24];
  snprintf(s, 24, "SCAN %03d", (int)radA);
  g.drawString(s, 6, 6);
  g.setTextColor(6);
  snprintf(s, 24, "CONTACTS %d", NBLIP);
  g.drawString(s, 6, H - 14);
  g.setTextColor((t / 10) & 1 ? 13 : 5);
  g.drawString("TRACKING", W - 60, 6);
}

// ---------------------------------------------------------------- CORE

static float coreA = 0;

static void coreBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 3, 1, 9);
  ramp(g, 1, 12, 6, 40, 58, 232, 255, 255);   // cyan glow ramp
  ramp(g, 13, 18, 120, 60, 10, 255, 179, 71); // orange
  g.setPaletteColor(19, 255, 255, 255);
  coreA = 0;
}

static void coreFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  g.fillSprite(0);
  int W = g.width(), H = g.height(), cx = W / 2, cy = H / 2;
  coreA += 2.0f * sp;
  float pulse = sinf(t * 0.07f * sp + 1);
  // layered glow
  for (int i = 0; i < 9; i++)
    g.fillCircle(cx, cy, 58 - i * 5 + (int)(pulse * 4), 1 + i);
  g.fillCircle(cx, cy, 11 + (int)(pulse * 3), 12);
  g.fillCircle(cx, cy, 5, 19);
  // segmented rotating rings
  for (int k = 0; k < 3; k++)
    g.fillArc(cx, cy, 68, 74, coreA + k * 120, coreA + k * 120 + 70, 10);
  for (int k = 0; k < 4; k++)
    g.fillArc(cx, cy, 82, 85, -coreA * 0.7f + k * 90, -coreA * 0.7f + k * 90 + 55, 5);
  for (int k = 0; k < 8; k++) {
    float a = (coreA * 1.8f + k * 45) * 0.0174533f;
    g.fillCircle(cx + (int)(cosf(a) * 63), cy + (int)(sinf(a) * 63), 2, 16);
  }
  // random discharge
  if (rndi(9) == 0) {
    float a = rndf() * 6.2832f;
    g.drawLine(cx + (int)(cosf(a) * 16), cy + (int)(sinf(a) * 16),
               cx + (int)(cosf(a + 0.2f) * (48 + rndi(30))),
               cy + (int)(sinf(a + 0.2f) * (48 + rndi(30))), 19);
  }
  g.setFont(&fonts::Font0);
  g.setTextDatum(lgfx::top_center);
  g.setTextColor((t / 16) & 1 ? 17 : 15);
  char s[28];
  snprintf(s, 28, "CORE OUTPUT %d.%02d GW", 1 + (int)(pulse * 0.4f + 0.4f),
           (int)(50 + pulse * 45));
  g.drawString(s, cx, H - 16);
}

// ---------------------------------------------------------------- SCOPE

static float scPh = 0;
static int scGlitch = 0;

static void scopeBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 6, 2, 2);
  ramp(g, 1, 10, 42, 5, 5, 255, 92, 92);
  g.setPaletteColor(11, 255, 224, 224);
  g.setPaletteColor(12, 48, 16, 16);
  scPh = 0; scGlitch = 0;
}

static void scopeFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  fadeAll(g, 1);
  int W = g.width(), H = g.height(), cy = H / 2;
  for (int x = 0; x < W; x += 32) g.drawFastVLine(x, 0, H, 12);
  for (int y = 0; y < H; y += 30) g.drawFastHLine(0, y, W, 12);
  scPh += 0.16f * sp;
  if (scGlitch == 0 && rndi(110) == 0) scGlitch = 10 + rndi(10);
  int py = cy;
  for (int x = 0; x < W; x++) {
    float v = sinf(x * 0.045f + scPh) * 28 * (1 + 0.5f * sinf(scPh * 0.21f))
            + sinf(x * 0.013f - scPh * 0.6f) * 14;
    int y = cy + (int)v;
    if (scGlitch && rndi(6) == 0) y += rndi(30) - 15;
    if (y < 1) y = 1; if (y > H - 2) y = H - 2;
    g.drawLine(x - 1, py, x, y, 10);
    py = y;
  }
  if (scGlitch) {
    scGlitch--;
    uint8_t* buf = (uint8_t*)g.getBuffer();
    for (int k = 0; k < 3; k++) { // shear random horizontal bands
      int y0 = rndi(H - 12), hh = 4 + rndi(9), dx = rndi(24) - 12;
      for (int y = y0; y < y0 + hh; y++) {
        uint8_t* row = buf + y * W;
        if (dx > 0) { memmove(row + dx, row, W - dx); memset(row, 0, dx); }
        else if (dx < 0) { memmove(row, row - dx, W + dx); memset(row + W + dx, 0, -dx); }
      }
    }
    g.fillRect(rndi(W - 8), rndi(H - 24), 5 + rndi(10), 18, 4);
  }
  g.setFont(&fonts::Font0);
  g.setTextDatum(lgfx::top_left);
  if (scGlitch) {
    g.setTextColor(11);
    g.drawString("SIGNAL LOST // RECOVER", 8, 8);
  } else {
    g.setTextColor(6);
    g.drawString("CH-7 LIVE", 8, 8);
  }
  char s[16];
  snprintf(s, 16, "%03d.%01d MHz", 87 + (int)(sinf(scPh * 0.1f) * 10 + 10), (int)(t / 7) % 10);
  g.setTextColor(8);
  g.drawString(s, W - 70, 8);
}

// ---------------------------------------------------------------- LOFI RAIN
// Rainy night city, warm windows, balcony railing, cat silhouette.

#define NDROP 46
static struct { float x, y, v; int len; } drops[NDROP];
static struct { float x; int y, tail; } cars[2];

static void dropReset(int i, int W) {
  drops[i].x = (float)rndi(W + 40);
  drops[i].y = -(float)rndi(60);
  drops[i].v = 0.8f + rndf() * 1.4f;
  drops[i].len = 6 + rndi(9);
}

static void rainBegin(LGFX_Sprite& g) {
  ramp(g, 0, 15, 6, 11, 24, 27, 39, 66);        // night sky
  g.setPaletteColor(16, 216, 224, 234);          // moon
  g.setPaletteColor(17, 14, 21, 38);             // cloud
  g.setPaletteColor(18, 13, 19, 34);             // far building
  g.setPaletteColor(19, 8, 12, 22);              // near building / floor
  ramp(g, 20, 25, 58, 42, 12, 255, 217, 138);    // warm windows
  g.setPaletteColor(26, 130, 160, 190);          // cool window (TV)
  g.setPaletteColor(27, 56, 208, 224);           // neon cyan
  g.setPaletteColor(28, 224, 88, 152);           // neon pink
  g.setPaletteColor(29, 58, 42, 18);             // street glow
  g.setPaletteColor(30, 107, 74, 30);            // street glow bright
  g.setPaletteColor(31, 95, 123, 166);           // rain
  g.setPaletteColor(32, 157, 184, 221);          // rain bright
  g.setPaletteColor(33, 38, 49, 64);             // railing
  g.setPaletteColor(34, 4, 6, 12);               // silhouette
  g.setPaletteColor(35, 28, 58, 38);             // leaves
  g.setPaletteColor(36, 47, 92, 58);             // leaves lit
  g.setPaletteColor(37, 74, 47, 30);             // pot
  g.setPaletteColor(38, 255, 90, 90);            // taillight
  for (int i = 0; i < NDROP; i++) dropReset(i, g.width());
  cars[0] = { -20, 0, 0 };
  cars[1] = { 340, 0, 1 };
}

static void rainFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  int W = g.width(), H = g.height();
  int skyH = H * 5 / 8;                 // 150 on 240
  int railY = H * 4 / 5;                // 192
  // sky
  for (int y = 0; y < skyH; y++) g.drawFastHLine(0, y, W, y * 15 / skyH);
  // moon + drifting cloud bands
  g.fillCircle(W - 52, 30, 10, 16);
  g.fillCircle(W - 56, 27, 9, (30 * 15) / skyH); // crescent bite in sky color
  for (int c = 0; c < 3; c++) {
    int cw = 60 + c * 30;
    int cx = (int)((t * (0.08f + c * 0.04f) * sp) + c * 140) % (W + cw) - cw;
    g.fillRect(cx, 22 + c * 16, cw, 5, 17);
  }
  // far skyline with lit windows
  int x = -6, bi = 0;
  while (x < W) {
    uint32_t h = hash2(bi, 3301);
    int bw = 20 + (h % 24);
    int bh = 38 + ((h >> 8) % 72);
    int top = skyH + 20 - bh;
    g.fillRect(x, top, bw, skyH + 20 - top, 18);
    for (int wy = top + 4; wy < skyH + 14; wy += 7) {
      for (int wx = x + 3; wx < x + bw - 3; wx += 6) {
        uint32_t wh = hash2(wx * 131 + wy, 911);
        if ((wh % 10) < 4) {
          int lvl = 20 + ((wh >> 6) % 5);
          if (((hash2(wh, t / 40) >> 3) & 31) == 0) lvl = 20; // rare dim flicker
          g.fillRect(wx, wy, 3, 4, ((wh >> 9) % 13 == 0) ? 26 : lvl);
        }
      }
    }
    // occasional rooftop neon sign
    if ((h >> 16) % 5 == 0 && bh > 70)
      g.fillRect(x + bw / 2 - 2, top - 10, 4, 10, ((h >> 18) & 1) ? 27 : 28);
    x += bw + 2;
    bi++;
  }
  // wet street glow between skyline and balcony
  for (int y = skyH + 20; y < railY; y++)
    g.drawFastHLine(0, y, W, (y - skyH - 20) < (railY - skyH - 20) / 2 ? 29 : 19);
  g.drawFastHLine(0, skyH + 20, W, 30);
  // two cars with headlights/taillights
  for (int c = 0; c < 2; c++) {
    float dir = c == 0 ? 1.0f : -1.0f;
    cars[c].x += dir * (0.9f + c * 0.5f) * sp;
    if (cars[c].x > W + 30) cars[c].x = -30;
    if (cars[c].x < -30) cars[c].x = W + 30;
    int cy2 = skyH + 26 + c * 5;
    g.fillRect((int)cars[c].x, cy2, 7, 3, 34);
    g.drawPixel((int)cars[c].x + (dir > 0 ? 7 : -1), cy2 + 1, c == 0 ? 25 : 38);
  }
  // rain (behind balcony)
  for (int i = 0; i < NDROP; i++) {
    drops[i].y += drops[i].v * 5.5f * sp;
    drops[i].x -= drops[i].v * 1.1f * sp;
    if (drops[i].y > H + 10) dropReset(i, W);
    int dx = (int)drops[i].x, dy = (int)drops[i].y;
    g.drawLine(dx + 2, dy, dx, dy + drops[i].len, drops[i].v > 1.6f ? 32 : 31);
  }
  // balcony floor + railing
  g.fillRect(0, railY + 4, W, H - railY - 4, 19);
  g.fillRect(0, railY, W, 4, 33);
  for (int px = 6; px < W; px += 24) g.fillRect(px, railY + 4, 3, H - railY - 4, 33);
  // potted plant, left
  int pxl = 26;
  g.fillRect(pxl - 8, railY - 14, 16, 14, 37);
  g.fillCircle(pxl - 6, railY - 18, 4, 35);
  g.fillCircle(pxl + 5, railY - 17, 5, 35);
  g.fillCircle(pxl, railY - 23, 5, 35);
  g.fillCircle(pxl + 2, railY - 20, 3, 36);
  // cat on the railing, watching the city (back view)
  int cx2 = W - 68, cy2 = railY - 8;
  float sway = sinf(t * 0.045f * sp);
  g.fillEllipse(cx2, cy2, 10, 8, 34);                    // body
  g.fillCircle(cx2 + 9, cy2 - 9, 6, 34);                 // head
  int tw = (int)(sway * 2);                              // ear twitch
  g.fillTriangle(cx2 + 5, cy2 - 13, cx2 + 8, cy2 - 20 + tw / 2, cx2 + 10, cy2 - 13, 34);
  g.fillTriangle(cx2 + 10, cy2 - 13, cx2 + 13, cy2 - 19, cx2 + 15, cy2 - 13, 34);
  for (int s = 0; s < 10; s++) {                          // swaying tail
    float fr = s / 9.0f;
    int tx = cx2 - 10 - (int)(fr * 14);
    int ty = cy2 + 2 - (int)(fr * fr * 16) + (int)(sway * fr * 5);
    g.fillCircle(tx, ty, 2, 34);
  }
}

// ---------------------------------------------------------------- SAKURA

#define NPETAL 34
static struct { float x, y, ph, v; int sz; } petals[NPETAL];

static void petalReset(int i, int W) {
  petals[i].x = (float)rndi(W);
  petals[i].y = -(float)rndi(40);
  petals[i].ph = rndf() * 6.28f;
  petals[i].v = 0.5f + rndf();
  petals[i].sz = 1 + rndi(3);
}

static void sakuraBegin(LGFX_Sprite& g) {
  ramp(g, 0, 17, 11, 8, 30, 44, 22, 66);       // indigo night
  g.setPaletteColor(18, 242, 230, 200);         // moon
  g.setPaletteColor(19, 106, 74, 110);          // halo inner
  g.setPaletteColor(20, 74, 49, 86);            // halo mid
  g.setPaletteColor(21, 56, 36, 74);            // halo outer
  g.setPaletteColor(22, 221, 208, 178);         // crater
  g.setPaletteColor(23, 176, 96, 138);          // petal far
  g.setPaletteColor(24, 217, 123, 166);
  g.setPaletteColor(25, 255, 183, 213);
  g.setPaletteColor(26, 255, 224, 236);         // petal glint
  g.setPaletteColor(27, 17, 8, 28);              // branch
  g.setPaletteColor(28, 150, 74, 120);           // blossom cluster
  g.setPaletteColor(29, 226, 222, 238);          // star
  g.setPaletteColor(30, 18, 10, 34);             // hills
  for (int i = 0; i < NPETAL; i++) { petalReset(i, 320); petals[i].y = (float)rndi(240); }
}

static void sakuraFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  int W = g.width(), H = g.height();
  for (int y = 0; y < H; y++) g.drawFastHLine(0, y, W, y * 17 / H);
  // stars
  for (int i = 0; i < 34; i++) {
    uint32_t h = hash2(i, 555);
    int sx = h % W, sy = (h >> 9) % (H / 2);
    if (((hash2(i, t / 10) >> 5) & 7) < 6) g.drawPixel(sx, sy, 29);
  }
  // moon + halo
  int mx = W - 72, my = 52;
  g.fillCircle(mx, my, 40, 21);
  g.fillCircle(mx, my, 34, 20);
  g.fillCircle(mx, my, 29, 19);
  g.fillCircle(mx, my, 24, 18);
  g.fillCircle(mx - 8, my - 6, 4, 22);
  g.fillCircle(mx + 6, my + 8, 3, 22);
  g.fillCircle(mx + 9, my - 9, 2, 22);
  // hills
  g.fillEllipse(W / 4, H + 14, W / 2, 40, 30);
  g.fillEllipse(W - 40, H + 18, W / 2, 34, 30);
  // branch from the left with blossom clusters
  int bx = 0, by = 34;
  for (int s = 0; s < 7; s++) {
    int nx = bx + 18 + (s * 3), ny = by + (s * s) / 2 + ((s & 1) ? 3 : -2);
    for (int w = 0; w < 3 - s / 3; w++) g.drawLine(bx, by + w, nx, ny + w, 27);
    if (s > 1) {
      uint32_t h = hash2(s, 42);
      int gx = nx - 4, gy = ny - 3;
      g.fillCircle(gx, gy, 4 + (h % 3), 28);
      g.fillCircle(gx + 4, gy - 3, 3, 24);
      g.fillCircle(gx - 3, gy + 2, 2, 25);
    }
    bx = nx; by = ny;
  }
  // drifting petals
  for (int i = 0; i < NPETAL; i++) {
    petals[i].ph += 0.035f * sp;
    petals[i].y += petals[i].v * (0.9f + petals[i].sz * 0.25f) * sp;
    petals[i].x += sinf(petals[i].ph) * 1.15f + 0.45f * sp;
    if (petals[i].y > H + 6 || petals[i].x > W + 8) petalReset(i, W);
    int c = 22 + petals[i].sz + ((int)(petals[i].ph * 2) % 2);
    if (c > 25) c = 25;
    int px = (int)petals[i].x, py = (int)petals[i].y;
    float tilt = sinf(petals[i].ph * 1.7f);
    if (petals[i].sz == 1) g.drawPixel(px, py, c);
    else if (tilt > 0.3f) g.fillEllipse(px, py, petals[i].sz, 1, c);
    else if (tilt < -0.3f) g.fillEllipse(px, py, 1, petals[i].sz, c);
    else { g.fillCircle(px, py, petals[i].sz - 1, c); g.drawPixel(px, py - 1, 26); }
  }
  // fallen petals on the near hill
  for (int i = 0; i < 16; i++) {
    uint32_t h = hash2(i, 902);
    g.drawPixel(h % W, H - 12 - (h >> 8) % 18, 24);
  }
}

// ---------------------------------------------------------------- FIREFLIES

#define NFLY 24
static struct { float bx, by, w1, w2, w3, p1, p2, p3; } flies[NFLY];

static void flyBegin(LGFX_Sprite& g) {
  ramp(g, 0, 15, 6, 16, 28, 42, 74, 62);       // dusk teal
  g.setPaletteColor(16, 226, 230, 214);         // star
  g.setPaletteColor(17, 9, 26, 15);             // far hill
  g.setPaletteColor(18, 4, 14, 8);              // near hill / tree
  ramp(g, 19, 26, 34, 46, 6, 234, 255, 160);   // firefly glow ramp
  for (int i = 0; i < NFLY; i++) {
    flies[i].bx = 15 + rndf() * 290;
    flies[i].by = 100 + rndf() * 110;
    flies[i].w1 = 0.010f + rndf() * 0.02f;
    flies[i].w2 = 0.008f + rndf() * 0.017f;
    flies[i].w3 = 0.03f + rndf() * 0.05f;
    flies[i].p1 = rndf() * 6.28f;
    flies[i].p2 = rndf() * 6.28f;
    flies[i].p3 = rndf() * 6.28f;
  }
}

static void flyFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  int W = g.width(), H = g.height();
  int horizon = H * 5 / 8;
  for (int y = 0; y < horizon; y++) g.drawFastHLine(0, y, W, y * 15 / horizon);
  // stars
  for (int i = 0; i < 26; i++) {
    uint32_t h = hash2(i, 321);
    int sx = h % W, sy = (h >> 9) % (horizon - 30);
    if (((hash2(i, t / 12) >> 5) & 7) < 6) g.drawPixel(sx, sy, 16);
  }
  // rolling hills
  for (int x2 = 0; x2 < W; x2++) {
    int y1 = horizon + (int)(12 * sinf(x2 * 0.021f + 1.2f));
    g.drawFastVLine(x2, y1, H - y1, 17);
    int y2 = horizon + 34 + (int)(10 * sinf(x2 * 0.013f + 4.0f));
    g.drawFastVLine(x2, y2, H - y2, 18);
  }
  // tree silhouette
  g.fillRect(W - 38, horizon - 32, 7, 46, 18);
  g.fillCircle(W - 35, horizon - 44, 22, 18);
  g.fillCircle(W - 52, horizon - 32, 13, 18);
  g.fillCircle(W - 18, horizon - 34, 12, 18);
  // grass blades on the near crest
  for (int x2 = 2; x2 < W; x2 += 5) {
    int yb = horizon + 34 + (int)(10 * sinf(x2 * 0.013f + 4.0f));
    int lean = (int)(2 * sinf(t * 0.02f * sp + x2 * 0.3f));
    g.drawLine(x2, yb, x2 + lean, yb - 4 - (x2 % 3), 18);
  }
  // fireflies
  float ft = (float)t * sp;
  for (int i = 0; i < NFLY; i++) {
    float fx = flies[i].bx + 20 * sinf(ft * flies[i].w1 + flies[i].p1);
    float fy = flies[i].by + 12 * sinf(ft * flies[i].w2 + flies[i].p2);
    float br = 0.5f + 0.5f * sinf(ft * flies[i].w3 + flies[i].p3);
    int x2 = (int)fx, y2 = (int)fy;
    if (x2 < 2 || x2 >= W - 2 || y2 < 40 || y2 >= H - 2) continue;
    int c = 19 + (int)(br * 7.0f);
    g.drawPixel(x2, y2, c);
    if (br > 0.55f) {
      g.drawPixel(x2 - 1, y2, c - 2); g.drawPixel(x2 + 1, y2, c - 2);
      g.drawPixel(x2, y2 - 1, c - 2); g.drawPixel(x2, y2 + 1, c - 2);
    }
    if (br > 0.9f) g.drawCircle(x2, y2, 2, 21);
  }
}

// ---------------------------------------------------------------- KOI POND
// Top-down pond: koi gliding under lily pads, ripples, drifting petals.

#define NKOI 4
static struct { float x, y, a, turn, ph, v; int type; } koi[NKOI];
static struct { float r; int x, y; bool alive; } rip[5];
static struct { float x, y, ph; } pondPetal[3];

static void koiBegin(LGFX_Sprite& g) {
  ramp(g, 0, 13, 8, 38, 44, 22, 82, 88);        // deep water
  g.setPaletteColor(14, 42, 118, 124);           // caustic dim
  g.setPaletteColor(15, 64, 148, 152);           // caustic bright
  g.setPaletteColor(16, 20, 67, 42);             // lily pad
  g.setPaletteColor(17, 34, 94, 58);             // lily rim
  g.setPaletteColor(18, 255, 140, 58);           // koi orange
  g.setPaletteColor(19, 255, 176, 102);          // koi orange lit
  g.setPaletteColor(20, 240, 235, 224);          // koi white
  g.setPaletteColor(21, 40, 38, 36);             // koi black spot
  g.setPaletteColor(22, 154, 219, 224);          // ripple
  g.setPaletteColor(23, 255, 183, 213);          // petal
  g.setPaletteColor(24, 6, 30, 34);              // fish shadow
  for (int i = 0; i < NKOI; i++) {
    koi[i].x = 40 + rndf() * 240; koi[i].y = 40 + rndf() * 160;
    koi[i].a = rndf() * 6.28f; koi[i].turn = 0;
    koi[i].ph = rndf() * 6.28f; koi[i].v = 0.5f + rndf() * 0.5f;
    koi[i].type = i % 3;
  }
  for (int i = 0; i < 5; i++) rip[i].alive = false;
  for (int i = 0; i < 3; i++) {
    pondPetal[i].x = rndf() * 320; pondPetal[i].y = rndf() * 240;
    pondPetal[i].ph = rndf() * 6.28f;
  }
}

static void koiFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  int W = g.width(), H = g.height();
  for (int y = 0; y < H; y++) g.drawFastHLine(0, y, W, y * 13 / H);
  // caustic shimmer: wavy light lines
  for (int y0 = 8; y0 < H; y0 += 18) {
    int py = -1;
    for (int x = 0; x <= W; x += 8) {
      int y = y0 + (int)(4.5f * sinf(x * 0.045f + t * 0.05f * sp + y0 * 0.7f));
      if (py >= 0) g.drawLine(x - 8, py, x, y, ((y0 / 18) & 1) ? 14 : 15);
      py = y;
    }
  }
  // koi (shadow pass, then body)
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < NKOI; i++) {
      if (pass == 0) { // steer
        koi[i].turn += (rndf() - 0.5f) * 0.012f;
        koi[i].turn = constrain(koi[i].turn, -0.035f, 0.035f);
        if (koi[i].x < 34) koi[i].turn += 0.01f * (cosf(koi[i].a) < 0 ? 1 : -1) + 0.008f;
        if (koi[i].x > W - 34) koi[i].turn += 0.008f;
        if (koi[i].y < 34 || koi[i].y > H - 34) koi[i].turn += 0.008f;
        koi[i].a += koi[i].turn * sp;
        koi[i].ph += 0.14f * sp;
        koi[i].x += cosf(koi[i].a) * koi[i].v * 1.4f * sp;
        koi[i].y += sinf(koi[i].a) * koi[i].v * 1.4f * sp;
        koi[i].x = constrain(koi[i].x, 10.0f, (float)W - 10);
        koi[i].y = constrain(koi[i].y, 10.0f, (float)H - 10);
      }
      float ca = cosf(koi[i].a), sa = sinf(koi[i].a);
      static const int rad[8] = {4, 5, 5, 4, 3, 2, 2, 1};
      for (int s = 7; s >= 0; s--) {
        float wig = sinf(koi[i].ph - s * 0.7f) * s * 0.9f;
        int px = (int)(koi[i].x - ca * s * 4 - sa * wig);
        int py = (int)(koi[i].y - sa * s * 4 + ca * wig);
        if (pass == 0) { g.fillCircle(px + 3, py + 4, rad[s], 24); continue; }
        int body = (koi[i].type == 1) ? 20 : 18;
        int c = body;
        if (koi[i].type == 0 && (s == 2 || s == 5)) c = 19;
        if (koi[i].type == 1 && (s == 1 || s == 4)) c = 18;   // white koi, orange patches
        if (koi[i].type == 2 && (s == 2 || s == 4)) c = 21;   // spotted
        g.fillCircle(px, py, rad[s], c);
        if (s == 7) { // tail fin flare
          float tw = sinf(koi[i].ph - 5.6f) * 3.5f;
          g.fillEllipse(px - (int)(ca * 4), py - (int)(sa * 4) + (int)tw, 3, 2, body);
        }
      }
      if (pass == 1) { // pectoral fins
        int hx = (int)koi[i].x, hy = (int)koi[i].y;
        g.fillEllipse(hx - (int)(sa * 5), hy + (int)(ca * 5), 2, 1, koi[i].type == 1 ? 20 : 18);
        g.fillEllipse(hx + (int)(sa * 5), hy - (int)(ca * 5), 2, 1, koi[i].type == 1 ? 20 : 18);
      }
    }
  }
  // lily pads (float above fish)
  static const int lily[3][3] = {{52, 42, 16}, {268, 66, 12}, {84, 198, 11}};
  for (int i = 0; i < 3; i++) {
    int lx = lily[i][0] + (int)(2 * sinf(t * 0.012f * sp + i * 2));
    g.fillCircle(lx, lily[i][1], lily[i][2], 16);
    g.fillTriangle(lx, lily[i][1], lx + lily[i][2] + 2, lily[i][1] - lily[i][2],
                   lx + lily[i][2] + 2, lily[i][1] + 3, 2 + lily[i][1] * 13 / H);
    g.drawCircle(lx, lily[i][1], lily[i][2], 17);
  }
  // ripples
  for (int i = 0; i < 5; i++) {
    if (!rip[i].alive) {
      if (rndi(90) == 0) {
        rip[i].alive = true; rip[i].r = 2;
        int k = rndi(NKOI);
        rip[i].x = (int)koi[k].x; rip[i].y = (int)koi[k].y;
      }
      continue;
    }
    rip[i].r += 0.5f * sp;
    if (rip[i].r > 26) { rip[i].alive = false; continue; }
    g.drawCircle(rip[i].x, rip[i].y, (int)rip[i].r, 22);
    if (rip[i].r > 8) g.drawCircle(rip[i].x, rip[i].y, (int)rip[i].r - 6, 14);
  }
  // drifting petals
  for (int i = 0; i < 3; i++) {
    pondPetal[i].ph += 0.02f * sp;
    pondPetal[i].x += 0.2f * sp + 0.3f * sinf(pondPetal[i].ph);
    pondPetal[i].y += 0.12f * sp;
    if (pondPetal[i].x > W || pondPetal[i].y > H) {
      pondPetal[i].x = rndf() * W; pondPetal[i].y = -4;
    }
    g.fillEllipse((int)pondPetal[i].x, (int)pondPetal[i].y, 2, 1, 23);
  }
}

// ---------------------------------------------------------------- TRAIN WINDOW
// Dusk countryside rolling past a rainy train window.

static float trM, trH, trN;
#define NTRAIL 26
static struct { float x, y; } trRain[NTRAIL];
static struct { float x, y; } trDrop[4];

static void trainBegin(LGFX_Sprite& g) {
  ramp(g, 0, 15, 30, 18, 56, 255, 138, 74);      // indigo -> sunset horizon
  g.setPaletteColor(16, 255, 217, 160);           // sun
  g.setPaletteColor(17, 58, 35, 80);              // far mountains
  g.setPaletteColor(18, 29, 42, 26);              // mid hills
  g.setPaletteColor(19, 16, 26, 16);              // near field
  g.setPaletteColor(20, 11, 18, 11);              // trees
  g.setPaletteColor(21, 10, 10, 12);              // poles/wires
  g.setPaletteColor(22, 255, 207, 122);           // town lights
  g.setPaletteColor(23, 201, 212, 228);           // rain bright
  g.setPaletteColor(24, 143, 160, 184);           // rain dim
  g.setPaletteColor(25, 217, 140, 74);            // paddy water (sky reflection)
  g.setPaletteColor(26, 74, 47, 94);              // clouds
  g.setPaletteColor(27, 232, 240, 250);           // glass drop
  trM = trH = trN = 0;
  for (int i = 0; i < NTRAIL; i++) { trRain[i].x = rndi(340); trRain[i].y = rndi(240); }
  for (int i = 0; i < 4; i++) { trDrop[i].x = rndi(320); trDrop[i].y = rndi(200); }
}

static void trainFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  int W = g.width(), H = g.height();
  int hor = H * 55 / 100;
  trM += 0.28f * sp; trH += 0.9f * sp; trN += 3.0f * sp;
  for (int y = 0; y < hor; y++) g.drawFastHLine(0, y, W, y * 15 / hor);
  // sun low on the horizon
  g.fillCircle(W / 4, hor - 14, 30, 15);
  g.fillCircle(W / 4, hor - 14, 22, 16);
  // clouds
  for (int c = 0; c < 3; c++) {
    int cw = 70 + c * 26;
    int cx = (int)(t * 0.05f * sp + c * 150) % (W + cw) - cw;
    g.fillEllipse(cx + cw / 2, 26 + c * 20, cw / 2, 5, 26);
  }
  // far mountains
  for (int x = 0; x < W; x++) {
    float wx = x + trM;
    int ym = hor - 26 - (int)(15 * sinf(wx * 0.012f) + 9 * sinf(wx * 0.031f + 2));
    g.drawFastVLine(x, ym, hor - ym, 17);
  }
  // mid hills with trees and town lights
  for (int x = 0; x < W; x++) {
    float wx = x + trH;
    int yh = hor - 5 - (int)(9 * sinf(wx * 0.02f + 1) + 4 * sinf(wx * 0.05f));
    g.drawFastVLine(x, yh, hor + 12 - yh, 18);
    int cell = (int)wx / 26;
    if (((int)wx % 26) == 13) {
      uint32_t h = hash2(cell, 71);
      if (h % 3 == 0) { // tree
        g.fillCircle(x, yh - 7, 5, 20);
        g.drawFastVLine(x, yh - 4, 5, 20);
      } else if (h % 7 == 1) { // hamlet lights
        g.drawPixel(x, yh - 1, 22); g.drawPixel(x + 3, yh - 2, 22);
      }
    }
  }
  // near field + paddy strips catching the sky
  g.fillRect(0, hor + 12, W, H - hor - 12, 19);
  for (int b = 0; b < 3; b++) {
    int y = hor + 22 + b * 16;
    for (int x = 0; x < W; x += 8) {
      int cell = (x + (int)(trN * (1.2f + b * 0.5f))) / 40;
      if (hash2(cell, b + 5) % 3 != 0) g.drawFastHLine(x, y, 8, b == 0 ? 25 : 24);
    }
  }
  // utility poles + sagging wires (nearest, fastest layer)
  int off = (int)(trN * 2.2f) % 130;
  for (int xp = -off; xp < W + 130; xp += 130) {
    g.fillRect(xp, hor - 74, 3, hor + 14 - (hor - 74), 21);
    g.fillRect(xp - 6, hor - 70, 15, 2, 21);
    for (int s = 0; s < 8; s++) { // wire sag to the next pole
      int x0 = xp + 1 + s * 130 / 8, x1 = xp + 1 + (s + 1) * 130 / 8;
      float f0 = s / 8.0f - 0.5f, f1 = (s + 1) / 8.0f - 0.5f;
      g.drawLine(x0, hor - 69 + (int)(9 * (0.25f - f0 * f0) * 4),
                 x1, hor - 69 + (int)(9 * (0.25f - f1 * f1) * 4), 21);
    }
  }
  // rain streaking almost horizontally across the glass
  for (int i = 0; i < NTRAIL; i++) {
    trRain[i].x -= 10.0f * sp; trRain[i].y += 1.8f * sp;
    if (trRain[i].x < -12 || trRain[i].y > H) { trRain[i].x = W + rndi(30); trRain[i].y = rndi(H); }
    g.drawLine((int)trRain[i].x, (int)trRain[i].y,
               (int)trRain[i].x + 11, (int)trRain[i].y + 2, (i & 3) ? 24 : 23);
  }
  // fat drops crawling down the glass
  for (int i = 0; i < 4; i++) {
    trDrop[i].y += 0.35f * sp; trDrop[i].x += 0.18f * sp * sinf(t * 0.07f + i * 3);
    if (trDrop[i].y > H) { trDrop[i].y = -3; trDrop[i].x = rndi(W); }
    g.drawPixel((int)trDrop[i].x, (int)trDrop[i].y, 27);
    g.drawPixel((int)trDrop[i].x, (int)trDrop[i].y - 1, 27);
    g.drawPixel((int)trDrop[i].x, (int)trDrop[i].y - 3, 24);
  }
}

// ---------------------------------------------------------------- FIRST SNOW
// The lofi city under gentle snowfall; the cat stays, curled up.

#define NFLAKE 42
static struct { float x, y, v, ph; } flakes[NFLAKE];

static void snowBegin(LGFX_Sprite& g) {
  ramp(g, 0, 15, 13, 20, 36, 48, 60, 84);        // cold night sky
  g.setPaletteColor(16, 222, 228, 238);           // moon
  g.setPaletteColor(17, 22, 30, 48);              // cloud
  g.setPaletteColor(18, 16, 24, 40);              // far building
  g.setPaletteColor(19, 10, 15, 26);              // near building / floor
  ramp(g, 20, 25, 58, 42, 12, 255, 217, 138);     // warm windows
  g.setPaletteColor(26, 130, 160, 190);           // cool window
  g.setPaletteColor(27, 56, 208, 224);            // neon cyan
  g.setPaletteColor(28, 224, 88, 152);            // neon pink
  g.setPaletteColor(29, 46, 42, 58);              // street glow (cold)
  g.setPaletteColor(30, 82, 74, 96);
  g.setPaletteColor(31, 185, 196, 216);           // flake dim
  g.setPaletteColor(32, 242, 246, 255);           // flake bright
  g.setPaletteColor(33, 38, 49, 64);              // railing
  g.setPaletteColor(34, 4, 6, 12);                // silhouette
  g.setPaletteColor(35, 232, 238, 248);           // snow caps
  g.setPaletteColor(36, 74, 47, 30);              // pot
  g.setPaletteColor(37, 255, 90, 90);             // taillight
  for (int i = 0; i < NFLAKE; i++) {
    flakes[i].x = rndf() * 320; flakes[i].y = rndf() * 240;
    flakes[i].v = 0.35f + rndf() * 0.6f; flakes[i].ph = rndf() * 6.28f;
  }
  cars[0] = { -20, 0, 0 };
  cars[1] = { 340, 0, 1 };
}

static void snowFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  int W = g.width(), H = g.height();
  int skyH = H * 5 / 8, railY = H * 4 / 5;
  for (int y = 0; y < skyH; y++) g.drawFastHLine(0, y, W, y * 15 / skyH);
  g.fillCircle(W - 52, 30, 10, 16);
  for (int c = 0; c < 3; c++) {
    int cw = 60 + c * 30;
    int cx = (int)((t * (0.05f + c * 0.03f) * sp) + c * 140) % (W + cw) - cw;
    g.fillRect(cx, 22 + c * 16, cw, 5, 17);
  }
  // skyline, snow-capped roofs
  int x = -6, bi = 0;
  while (x < W) {
    uint32_t h = hash2(bi, 3301);
    int bw = 20 + (h % 24);
    int bh = 38 + ((h >> 8) % 72);
    int top = skyH + 20 - bh;
    g.fillRect(x, top, bw, skyH + 20 - top, 18);
    g.drawFastHLine(x, top, bw, 35);
    g.drawFastHLine(x, top + 1, bw, 31);
    for (int wy = top + 5; wy < skyH + 14; wy += 7)
      for (int wx = x + 3; wx < x + bw - 3; wx += 6) {
        uint32_t wh = hash2(wx * 131 + wy, 911);
        if ((wh % 10) < 3)
          g.fillRect(wx, wy, 3, 4, ((wh >> 9) % 13 == 0) ? 26 : 20 + ((wh >> 6) % 5));
      }
    if ((h >> 16) % 5 == 0 && bh > 70)
      g.fillRect(x + bw / 2 - 2, top - 10, 4, 10, ((h >> 18) & 1) ? 27 : 28);
    x += bw + 2;
    bi++;
  }
  // hushed street
  for (int y = skyH + 20; y < railY; y++)
    g.drawFastHLine(0, y, W, (y - skyH - 20) < (railY - skyH - 20) / 2 ? 29 : 19);
  g.drawFastHLine(0, skyH + 20, W, 30);
  for (int c = 0; c < 2; c++) {
    float dir = c == 0 ? 1.0f : -1.0f;
    cars[c].x += dir * 0.45f * sp;   // slow, careful drivers
    if (cars[c].x > W + 30) cars[c].x = -30;
    if (cars[c].x < -30) cars[c].x = W + 30;
    int cy2 = skyH + 26 + c * 5;
    g.fillRect((int)cars[c].x, cy2, 7, 3, 34);
    g.drawPixel((int)cars[c].x + (dir > 0 ? 7 : -1), cy2 + 1, c == 0 ? 25 : 37);
  }
  // balcony: snowy floor + capped railing
  g.fillRect(0, railY + 4, W, H - railY - 4, 19);
  for (int i = 0; i < 40; i++) {
    uint32_t h = hash2(i, 77);
    g.drawPixel(h % W, railY + 6 + (h >> 8) % (H - railY - 8), 31);
  }
  g.fillEllipse(W / 3, H - 6, 40, 5, 31);
  g.fillRect(0, railY, W, 4, 33);
  g.drawFastHLine(0, railY - 1, W, 35);
  g.drawFastHLine(0, railY - 2, W, 35);
  for (int px = 6; px < W; px += 24) {
    g.fillRect(px, railY + 4, 3, H - railY - 4, 33);
    g.drawPixel(px + 1, railY + 3, 35);
  }
  // snowy potted plant
  int pxl = 26;
  g.fillRect(pxl - 8, railY - 14, 16, 14, 36);
  g.fillCircle(pxl - 6, railY - 18, 4, 34);
  g.fillCircle(pxl + 5, railY - 17, 5, 34);
  g.fillCircle(pxl, railY - 23, 5, 34);
  g.drawFastHLine(pxl - 4, railY - 26, 9, 35);
  g.drawFastHLine(pxl - 8, railY - 15, 16, 35);
  // the cat, curled up against the cold
  int cx2 = W - 68, cy2 = railY - 7;
  g.fillEllipse(cx2, cy2, 11, 7, 34);
  g.fillCircle(cx2 + 7, cy2 - 4, 5, 34);            // head tucked low
  int tw = ((t / 40) % 8 == 0) ? 1 : 0;              // sleepy ear twitch
  g.fillTriangle(cx2 + 4, cy2 - 7, cx2 + 6, cy2 - 11 + tw, cx2 + 8, cy2 - 7, 34);
  g.fillTriangle(cx2 + 8, cy2 - 7, cx2 + 11, cy2 - 10, cx2 + 12, cy2 - 6, 34);
  g.fillEllipse(cx2 - 6, cy2 + 3, 6, 3, 34);         // tail wrapped around
  // snowfall (over everything)
  for (int i = 0; i < NFLAKE; i++) {
    flakes[i].ph += 0.03f * sp;
    flakes[i].y += flakes[i].v * 1.7f * sp;
    flakes[i].x += sinf(flakes[i].ph) * 0.55f;
    if (flakes[i].y > H) { flakes[i].y = -2; flakes[i].x = rndf() * W; }
    g.drawPixel((int)flakes[i].x, (int)flakes[i].y, flakes[i].v > 0.7f ? 32 : 31);
    if (flakes[i].v > 0.85f) g.drawPixel((int)flakes[i].x + 1, (int)flakes[i].y, 31);
  }
}

// ---------------------------------------------------------------- ION TUNNEL

static float tunPh = 0, tunTotal = 0;

static void tunnelBegin(LGFX_Sprite& g) {
  g.setPaletteColor(0, 2, 4, 8);
  ramp(g, 1, 10, 4, 48, 56, 0, 255, 208);
  g.setPaletteColor(11, 240, 255, 252);
  ramp(g, 12, 15, 90, 56, 22, 255, 159, 64);
  g.setPaletteColor(16, 36, 68, 74);
  tunPh = 0; tunTotal = 0;
}

static void tunnelFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  g.fillSprite(0);
  int W = g.width(), H = g.height();
  tunPh += 0.055f * sp;
  if (tunPh >= 1) { tunPh -= 1; tunTotal += 1; }
  float cx = W / 2.0f + 26 * sinf(t * 0.011f * sp);
  float cy = H / 2.0f + 18 * sinf(t * 0.014f * sp + 2);
  // distant star dust
  for (int i = 0; i < 20; i++) {
    uint32_t h = hash2(i, 33);
    g.drawPixel(h % W, (h >> 9) % H, 16);
  }
  const int NR = 11;
  float vx[2][6], vy[2][6];
  int cur = 0;
  for (int i = NR; i >= 1; i--) {
    float z = i + 1 - tunPh;
    float r = 300.0f / z;
    float ox = W / 2 + (cx - W / 2) * (2.2f / z);
    float oy = H / 2 + (cy - H / 2) * (2.2f / z);
    float rot = t * 0.008f * sp + z * 0.22f;
    long world = (long)tunTotal + i;
    bool hot = (world % 4) == 0;
    int c = hot ? (int)constrain(15.0f - z * 0.55f, 12.0f, 15.0f)
                : (int)constrain(10.5f - z * 0.85f, 1.0f, 10.0f);
    for (int k = 0; k < 6; k++) {
      float a = rot + k * 1.0472f;
      vx[cur][k] = ox + cosf(a) * r;
      vy[cur][k] = oy + sinf(a) * r * 0.82f;
    }
    for (int k = 0; k < 6; k++) {
      int k2 = (k + 1) % 6;
      g.drawLine((int)vx[cur][k], (int)vy[cur][k], (int)vx[cur][k2], (int)vy[cur][k2], c);
      if (i < NR)  // rails to the ring behind
        g.drawLine((int)vx[cur][k], (int)vy[cur][k], (int)vx[1 - cur][k], (int)vy[1 - cur][k], 2);
    }
    cur = 1 - cur;
  }
  g.fillCircle((int)cx, (int)cy, 2, 11);   // vanishing point flare
  if (rndi(4) == 0)                          // speed streaks
    g.drawLine((int)cx, (int)cy,
               (int)cx + rndi(W) - W / 2, (int)cy + rndi(H) - H / 2, 3);
}

// ---------------------------------------------------------------- LOW ORBIT
// A planet from orbit: banded dayside, terminator, city lights in the dark.

static float orbScroll = 0;
static struct { float x, y, vx, vy; int life; } meteor;
static float satX = -40;

static void orbitBegin(LGFX_Sprite& g) {
  makeS8();
  g.setPaletteColor(0, 2, 2, 8);
  g.setPaletteColor(1, 106, 112, 128);            // star dim
  g.setPaletteColor(2, 232, 236, 244);            // star bright
  ramp(g, 3, 18, 14, 52, 104, 130, 190, 216);     // dayside bands
  ramp(g, 19, 23, 4, 6, 14, 12, 18, 34);          // night side
  g.setPaletteColor(24, 255, 202, 106);           // city lights
  g.setPaletteColor(25, 106, 208, 255);           // atmosphere rim
  g.setPaletteColor(26, 42, 106, 154);            // rim dim
  g.setPaletteColor(27, 24, 52, 80);              // terminator dusk
  g.setPaletteColor(28, 216, 220, 228);           // satellite
  g.setPaletteColor(29, 255, 80, 80);             // beacon
  g.setPaletteColor(30, 255, 240, 200);           // sun
  orbScroll = 0; meteor.life = 0; satX = -40;
}

static void orbitFrame(LGFX_Sprite& g, uint32_t t, float sp) {
  g.fillSprite(0);
  int W = g.width(), H = g.height();
  orbScroll += 0.35f * sp;
  // stars
  for (int i = 0; i < 60; i++) {
    uint32_t h = hash2(i, 909);
    int sx = h % W, sy = (h >> 9) % H;
    g.drawPixel(sx, sy, ((hash2(i, t / 14) >> 4) & 7) < 2 ? 2 : 1);
  }
  // sun, upper left
  g.fillCircle(24, 20, 4, 30);
  g.drawFastHLine(14, 20, 21, 30);
  g.drawFastVLine(24, 10, 21, 30);
  // shooting star
  if (meteor.life == 0 && rndi(160) == 0) {
    meteor.x = rndi(W); meteor.y = rndi(H / 3);
    meteor.vx = 4 + rndf() * 3; meteor.vy = 1.5f + rndf(); meteor.life = 14;
  }
  if (meteor.life > 0) {
    meteor.life--;
    meteor.x += meteor.vx * sp; meteor.y += meteor.vy * sp;
    g.drawLine((int)meteor.x, (int)meteor.y,
               (int)(meteor.x - meteor.vx * 2.2f), (int)(meteor.y - meteor.vy * 2.2f), 2);
  }
  // planet: big disc rising past the bottom-right
  int pcx = W * 64 / 100, pcy = H + 34, R = 128;
  uint8_t* buf = (uint8_t*)g.getBuffer();
  int scroll = (int)orbScroll;
  for (int y = pcy - R; y < H; y++) {
    int dy = y - pcy;
    int half = (int)sqrtf((float)(R * R - dy * dy));
    int x0 = pcx - half, x1 = pcx + half;
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    int termX = pcx - half / 3;                       // sun from the left
    uint8_t* row = buf + y * W;
    uint8_t band = sin8((uint8_t)(dy * 2 + scroll / 3));
    for (int x = x0; x <= x1; x++) {
      int c;
      if (x < termX - 8) {                            // dayside
        c = 3 + band * 13 / 255;
        c += (hash2((uint32_t)((x + scroll) >> 3), (uint32_t)(y >> 3)) % 3);
        int edge = x - (pcx - half);
        if (edge < 10) c -= (10 - edge) / 4;          // limb darkening
        if (c < 3) c = 3;
        if (c > 18) c = 18;
      } else if (x < termX + 8) {                     // dusk band
        c = 27;
      } else {                                        // night side
        c = 19 + (int)((uint32_t)(x * 7 + y * 13) % 4);
        uint32_t hc = hash2((uint32_t)((x + scroll / 4) >> 2), (uint32_t)(y >> 2));
        if ((hc % 97) < 2 && (hash2(hc, 5) % 3) == 0) c = 24;   // city clusters
      }
      row[x] = (uint8_t)c;
    }
  }
  // atmosphere rim
  g.drawCircle(pcx, pcy, R + 3, 26);
  g.drawArc(pcx, pcy, R + 1, R + 2, 150, 280, 25);   // lit limb toward the sun
  // a satellite drifting by
  satX += 0.55f * sp;
  if (satX > W + 40) satX = -40;
  int sy2 = 52 + (int)(satX * 0.06f);
  g.drawFastHLine((int)satX - 4, sy2, 3, 28);
  g.fillRect((int)satX, sy2 - 1, 2, 2, 28);
  g.drawFastHLine((int)satX + 3, sy2, 3, 28);
  if ((t / 9) % 5 == 0) g.drawPixel((int)satX + 1, sy2 - 2, 29);
}

// ---------------------------------------------------------------- registry

const Anim ANIMS[] = {
  {"matrix", "Digital Rain",   matrixBegin, matrixFrame},
  {"warp",   "Starfield Warp", warpBegin,   warpFrame},
  {"hud",    "Neon HUD",       hudBegin,    hudFrame},
  {"plasma", "Plasma Flux",    plasmaBegin, plasmaFrame},
  {"grid",   "Synthwave Grid", gridBegin,   gridFrame},
  {"radar",  "Tactical Radar", radarBegin,  radarFrame},
  {"core",   "Reactor Core",   coreBegin,   coreFrame},
  {"scope",  "Ghost Signal",   scopeBegin,  scopeFrame},
  {"rain",   "Lofi Rain",      rainBegin,   rainFrame},
  {"sakura", "Sakura Night",   sakuraBegin, sakuraFrame},
  {"fire",   "Firefly Meadow", flyBegin,    flyFrame},
  {"tunnel", "Ion Tunnel",     tunnelBegin, tunnelFrame},
  {"orbit",  "Low Orbit",      orbitBegin,  orbitFrame},
  {"koi",    "Koi Pond",       koiBegin,    koiFrame},
  {"train",  "Train Window",   trainBegin,  trainFrame},
  {"snow",   "First Snow",     snowBegin,   snowFrame},
};
const int ANIM_COUNT = sizeof(ANIMS) / sizeof(ANIMS[0]);

int animIndexById(const char* id) {
  for (int i = 0; i < ANIM_COUNT; i++)
    if (strcmp(ANIMS[i].id, id) == 0) return i;
  return -1;
}
