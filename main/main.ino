/*
  ============================================================
   CM-5 "Thinking Machine" Blinking-Light Panel Emulator
  ============================================================

  Inspired by the Connection Machine CM-5 supercomputer's iconic
  blinking-light panel (famously dressed up as the control-room
  computer in Jurassic Park). The real panel wasn't random noise —
  it visualized a binary counter, which produces a self-similar,
  cascading triangular pattern. This sketch reproduces that look
  with a Rule 90 cellular automaton scrolled down the display,
  plus a sparkle overlay for texture and a NeoPixel "status glow"
  that flares whenever the pattern reseeds.

  HARDWARE
  ---------------------------------------------------------------
  - ESP8266 (D1 Mini or similar)
  - 2x MAX7219 8x8 LED matrix modules, DAISY-CHAINED, wired
    side-by-side to form a 16 (wide) x 8 (tall) panel
  - 1x NeoPixel strip/ring for accent lighting

  LIBRARIES (install via Library Manager)
  ---------------------------------------------------------------
  - "LedControl" by Eberhard Fahle
  - "Adafruit NeoPixel"

  WIRING NOTE
  ---------------------------------------------------------------
  If your two matrices are stacked vertically (8 wide x 16 tall)
  instead of side-by-side, adjust setPixel() as noted inline.
  ============================================================
*/

#include <LedControl.h>
#include <Adafruit_NeoPixel.h>

// ---------------- Hardware Pin Configuration ----------------
const int PIN_DATA_IN  = D4; // MAX7219 DIN
const int PIN_CLK      = D2; // MAX7219 CLK
const int PIN_CS       = D3; // MAX7219 CS
const int PIN_NEOPIXEL = D6; // NeoPixel DIN (GPIO12)

// ---------------- Panel / Strip Configuration ----------------
#define NUM_MAX_DEVICES 2
#define NUM_PIXELS      8      // adjust to match your NeoPixel strip/ring
#define PANEL_WIDTH     16     // two 8x8 modules side-by-side
#define PANEL_HEIGHT    8

#define DEFAULT_INTENSITY 1    // 0-15, base MAX7219 brightness

LedControl lc = LedControl(PIN_DATA_IN, PIN_CLK, PIN_CS, NUM_MAX_DEVICES);
Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ---------------- Display State ----------------
uint16_t screenRows[PANEL_HEIGHT];   // one uint16_t per row, one bit per column
uint16_t sparkleMask[PANEL_HEIGHT];  // transient flicker overlay, doesn't persist
uint16_t currentGen = 0;             // current automaton generation

// ---------------- Timing (all non-blocking, millis-based) ----------------
unsigned long lastTick    = 0;
unsigned long lastSparkle = 0;
unsigned long lastGlow    = 0;

uint16_t tickInterval    = 800; // ms between automaton generations (cascade speed)
uint16_t sparkleInterval = 400; // ms between flicker refreshes
uint16_t glowInterval    = 200; // ms between NeoPixel updates

// Flare state for the NeoPixel when the pattern reseeds
float glowFlare = 0.0;

void setup() {
  randomSeed(analogRead(A0));

  for (int dev = 0; dev < NUM_MAX_DEVICES; dev++) {
    lc.shutdown(dev, false);
    lc.setIntensity(dev, DEFAULT_INTENSITY);
    lc.clearDisplay(dev);
  }

  strip.begin();
  strip.show();

  reseed();
}

void loop() {
  unsigned long now = millis();

  if (now - lastTick >= tickInterval) {
    lastTick = now;
    tickAutomaton();
  }

  if (now - lastSparkle >= sparkleInterval) {
    lastSparkle = now;
    rollSparkle();
    render();
  }

  if (now - lastGlow >= glowInterval) {
    lastGlow = now;
    updateGlow();
  }
}

// ---------------- Rule 90 cellular automaton ----------------
// new cell = XOR of its two neighbors from the previous generation.
// Seeded with a single bit, this produces a Sierpinski-triangle
// cascade -- visually very close to the real CM-5 panel's counter
// display, without needing to actually count in binary.

void reseed() {
  currentGen = 0;

  if (random(0, 3) == 0) {
    // occasionally scatter a few live bits for variety
    for (uint8_t i = 0; i < 3; i++) {
      currentGen |= (1UL << random(0, PANEL_WIDTH));
    }
  } else {
    // classic centered single-bit seed -> clean symmetric cascade
    currentGen = (1UL << (PANEL_WIDTH / 2));
  }

  // trigger a NeoPixel flare so the reset reads as a deliberate
  // "processing burst" rather than a glitch
  glowFlare = 1.0;
}

uint16_t nextGeneration(uint16_t gen) {
  uint16_t next = 0;
  for (int i = 0; i < PANEL_WIDTH; i++) {
    bool left  = (i > 0)               ? bitRead(gen, i - 1) : 0;
    bool right = (i < PANEL_WIDTH - 1) ? bitRead(gen, i + 1) : 0;
    if (left ^ right) next |= (1UL << i);
  }
  return next;
}

void tickAutomaton() {
  // scroll existing rows down, insert the current generation at top
  for (int r = PANEL_HEIGHT - 1; r > 0; r--) {
    screenRows[r] = screenRows[r - 1];
  }
  screenRows[0] = currentGen;

  currentGen = nextGeneration(currentGen);

  // if the automaton has died out (all zeros) or run long enough,
  // kick off a fresh cascade -- mirrors the real panel's periodic resets
  static uint8_t generationsSinceReseed = 0;
  generationsSinceReseed++;
  if (currentGen == 0 || generationsSinceReseed > 40) {
    reseed();
    generationsSinceReseed = 0;
  }
}

// ---------------- Sparkle overlay ----------------
// A few transient bits layered on top of the main pattern each
// render pass -- gives the panel that restless "always something
// blinking somewhere" texture without touching the automaton state.

void rollSparkle() {
  for (uint8_t r = 0; r < PANEL_HEIGHT; r++) {
    sparkleMask[r] = 0;
  }
  uint8_t sparkleCount = random(2, 6);
  for (uint8_t n = 0; n < sparkleCount; n++) {
    uint8_t x = random(0, PANEL_WIDTH);
    uint8_t y = random(0, PANEL_HEIGHT);
    sparkleMask[y] |= (1UL << x);
  }
}

// ---------------- Rendering ----------------

void setPixel(uint8_t x, uint8_t y, bool state) {
  if (x >= PANEL_WIDTH || y >= PANEL_HEIGHT) return;

  // Side-by-side wiring (16 wide x 8 tall). The two modules are
  // daisy-chained (not mounted in parallel orientation), so the
  // second physical module (device 1) is rotated 180 degrees
  // relative to the first -- flip both row and column for it.
  uint8_t device = x / 8;
  uint8_t col    = x % 8;
  uint8_t row    = y;

  if (device == 1) {
    row = 7 - y;
    col = 7 - col;
  }

  lc.setLed(device, row, col, state);

  // --- If your modules are stacked vertically instead (8 wide x
  //     16 tall), use this addressing instead (adjust rotation
  //     handling the same way if needed):
  // uint8_t device = y / 8;
  // uint8_t localY = y % 8;
  // lc.setLed(device, localY, x, state);
}

void render() {
  for (uint8_t y = 0; y < PANEL_HEIGHT; y++) {
    uint16_t combined = screenRows[y] ^ sparkleMask[y];
    for (uint8_t x = 0; x < PANEL_WIDTH; x++) {
      setPixel(x, y, bitRead(combined, x));
    }
  }
}

// ---------------- NeoPixel status glow ----------------
// Slow amber/red breathing to sell the "1980s mainframe" vibe,
// with a bright flare layered on top whenever the pattern reseeds.

void updateGlow() {
  float t = millis() / 1000.0;
  float breathe = (sin(t * 1.3) + 1.0) / 2.0; // 0..1

  // decay the reseed flare over time
  glowFlare *= 0.90;
  if (glowFlare < 0.01) glowFlare = 0.0;

  float level = 0.15 + 0.35 * breathe + 0.9 * glowFlare;
  if (level > 1.0) level = 1.0;

  uint8_t r = (uint8_t)(255 * level);
  uint8_t g = (uint8_t)(60 * level);   // slight amber tint, not pure red
  uint8_t b = 0;

  for (uint16_t i = 0; i < NUM_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}
