// Host-side render of the buddy UI — writes raw RGB888 for each screen state.
// Usage: preview_ui outdir
// Emits one <state>.rgb per screen with representative arc/digits/label.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../art_states.h"
#include "../ui_buddy.h"

static uint16_t layer[ui::W * ui::H], fb[ui::W * ui::H];

static void writeRgb(const std::string &path, const uint16_t *p) {
  FILE *f = fopen(path.c_str(), "wb");
  for (int i = 0; i < ui::W * ui::H; i++) {
    uint16_t c = p[i];
    unsigned char px[3] = {(unsigned char)(((c >> 11) & 0x1F) << 3),
                           (unsigned char)(((c >> 5) & 0x3F) << 2),
                           (unsigned char)((c & 0x1F) << 3)};
    fwrite(px, 1, 3, f);
  }
  fclose(f);
}

struct Case {
  const char *name;
  ui::Screen scr;
  const char *lbl;
  bool digits;
  uint32_t secs;
  float frac;
  bool dim;
};

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : ".";
  static const Case CASES[] = {
      {"wifi", ui::SCR_SEARCH, "WIFI", false, 0, -1, true},
      {"looking", ui::SCR_SEARCH, "LOOKING", false, 0, -1, true},
      {"nolink", ui::SCR_SEARCH, "NO LINK", true, 754, -1, true},
      {"idle", ui::SCR_IDLE, "IDLE", false, 0, -1, false},
      {"sleep", ui::SCR_SLEEP, "ASLEEP", false, 0, -1, true},
      {"ready", ui::SCR_READY, "READY", true, 1520, -1, false},
      {"framing", ui::SCR_FRAME, "FRAMING", true, 43, -1, false},
      {"cutting", ui::SCR_CUT, "REMAINING", true, 754, 0.37f, false},
      {"cut_long", ui::SCR_CUT, "REMAINING", true, 7509, 0.62f, false},
      {"paused", ui::SCR_PAUSE, "PAUSED", true, 754, 0.37f, true},
      {"done", ui::SCR_DONE, "DONE", true, 4212, 1.0f, false},
      {"error", ui::SCR_ERROR, "ERROR", false, 0, -1, false},
  };
  for (const Case &c : CASES) {
    ui::expandArt(layer, ui::STATE_ART[c.scr].pal, ui::STATE_ART[c.scr].px,
                  c.digits ? 0.72f : 1.0f, c.digits ? 42 : 0);
    ui::bakeArcTrack(layer);
    if (c.digits) ui::bakePill(layer, ui::DIG_X0 + 12, ui::DIG_Y0, ui::DIG_X1 - 12, ui::DIG_Y1, 0.68f);
    memcpy(fb, layer, sizeof fb);
    float amt = c.dim ? 0.45f : 1.0f;
    int bx0, by0, bx1, by1;
    if (c.frac >= 0) ui::arcPaint(fb, layer, 0, c.frac, amt, 0.8f, bx0, by0, bx1, by1);
    if (c.digits) ui::drawTime(fb, layer, c.secs, amt);
    ui::drawLabel(fb, layer, c.lbl, c.dim ? 0.7f : 1.0f);
    writeRgb(std::string(out) + "/" + c.name + ".rgb", fb);
    printf("%s.rgb\n", c.name);
  }
  return 0;
}
