#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Every animation renders a full frame into an 8-bit palette sprite.
// begin() sets the palette + resets state; frame() draws one frame.
// sp is a speed multiplier (portal speed 1-10 -> 0.2..2.0, 1.0 at 5).
struct Anim {
  const char* id;
  const char* name;
  void (*begin)(LGFX_Sprite&);
  void (*frame)(LGFX_Sprite&, uint32_t t, float sp);
};

extern const Anim ANIMS[];
extern const int ANIM_COUNT;
int animIndexById(const char* id);
