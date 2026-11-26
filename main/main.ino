/*
  ================================================================================
   CM-5 "Thinking Machine" Automaton & Smart Clock Display (Horizontal Edition)
  ================================================================================
   Description:
   Drives two horizontal 32x8 LED matrix banks using 8x MAX7219 modules (4 per bank),
   plus an accompanying WS2812B NeoPixel strip for ambient weather/AQI lighting.

   Screen 1 (Modules 0-3): 32x8 horizontal telemetry-driven load simulation
              sliding left-to-right, seeded by humidity & clouds, paced by wind
              speed, stirred by barometric pressure/precipitation, with AQI/PM2.5
              sparkles and a hybrid PM2.5 + AQI load meter on column x = 0.
   Screen 2 (Modules 4-7): Horizontal 4-digit 24-hour clock (HHMM) rendered using
              Japanese kanji numerals (〇, 一, 二, 三, 四, 五, 六, 七, 八, 九).
   Matrix Brightness:
     - Night (sundown to sunrise): Intensity 0.
     - First hour after sunrise: Smoothly ramps from 0 to 5.
     - Daytime: Intensity 5.
   NeoPixels:
     - Displays 8-hour weather forecast palette.
     - Red (AQI 4) & Purple (AQI 5) emergency air quality warnings.
     - Real-time Day/Night ambient dimming.
     - Half-speed twinkle animation.
  ================================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <LedControl.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <sys/time.h>

// ===============================================================================
// --- User Configuration & Network Credentials ---
// ===============================================================================
const char* WIFI_SSID       = "CHANGEME";
const char* WIFI_PASS       = "CHANGEME";
const char* OPENWEATHER_KEY = "CHANGEME";

// POSIX Timezone string (US Eastern Time with automatic DST adjustments)
const char* TZ_INFO         = "EST5EDT,M3.2.0,M11.1.0";
const char* CITY_NAME       = "Boston,US";
const char* LATITUDE        = "42.3601";
const char* LONGITUDE       = "-71.0589";

// Hardware Orientation configuration
// Set to true if Screen 2 (modules 4-7) is physically mounted upside-down (180 deg).
const bool REVERSE_PANEL_2  = true;

// ===============================================================================
// --- Hardware Pin Configuration (ESP8266) ---
// ===============================================================================
const int PIN_DATA_IN  = D4; // MAX7219 DIN
const int PIN_CLK      = D2; // MAX7219 CLK
const int PIN_CS       = D3; // MAX7219 CS
const int PIN_NEOPIXEL = D6; // NeoPixel DIN

#define NUM_MAX_DEVICES 8
#define NUM_PIXELS      8

// Horizontal 32x8 dimensions for Screen 1 load simulation
#define PANEL_WIDTH     32
#define PANEL_HEIGHT    8

#define SCREEN1_DEVICE_OFFSET 0
#define SCREEN1_NUM_DEVICES   4
#define SCREEN2_DEVICE_OFFSET 4
#define SCREEN2_NUM_DEVICES   4

LedControl lc = LedControl(PIN_DATA_IN, PIN_CLK, PIN_CS, NUM_MAX_DEVICES);
Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ===============================================================================
// --- Horizontal Font Definitions (Kanji 0-9 + SYNC Status Glyphs in 8x8) ---
// ===============================================================================
const uint8_t FONT_HORIZ_KANJI[14][8] PROGMEM = {
  // '0' -> 〇
  {0b00111100, 0b01000010, 0b01000010, 0b01000010, 0b01000010, 0b01000010, 0b01000010, 0b00111100},
  // '1' -> 一
  {0b00000000, 0b00000000, 0b00000000, 0b11111111, 0b01111110, 0b00000000, 0b00000000, 0b00000000},
  // '2' -> 二
  {0b00000000, 0b01111110, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111, 0b00000000},
  // '3' -> 三
  {0b01111110, 0b00000000, 0b00000000, 0b00111100, 0b00000000, 0b00000000, 0b00000000, 0b11111111},
  // '4' -> 四
  {0b11111111, 0b10101001, 0b10101001, 0b10101001, 0b11101001, 0b10000101, 0b10000011, 0b11111111},
  // '5' -> 五
  {0b11111111, 0b00100000, 0b00100000, 0b11111111, 0b00100000, 0b01111110, 0b00100010, 0b11111111},
  // '6' -> 六
  {0b00010000, 0b00010000, 0b11111111, 0b00000000, 0b00100100, 0b01000010, 0b10000001, 0b10000001},
  // '7' -> 七
  {0b00100000, 0b00100001, 0b11111111, 0b10100000, 0b00100000, 0b00100000, 0b00100001, 0b00011110},
  // '8' -> 八
  {0b00100000, 0b00001000, 0b00100100, 0b00100100, 0b00100100, 0b01000010, 0b10000001, 0b10000001},
  // '9' -> 九
  {0b00000100, 0b00000100, 0b11111111, 0b00100100, 0b00100100, 0b00100100, 0b01000101, 0b10000110},
  // 'S' (index 10)
  {0b01111110, 0b11000000, 0b11000000, 0b01111110, 0b00000011, 0b00000011, 0b00000011, 0b11111110},
  // 'Y' (index 11)
  {0b11000011, 0b01100110, 0b00111100, 0b00011000, 0b00011000, 0b00011000, 0b00011000, 0b00011000},
  // 'N' (index 12)
  {0b11000011, 0b11100011, 0b10110011, 0b10011011, 0b10001111, 0b10000111, 0b10000011, 0b11000011},
  // 'C' (index 13)
  {0b01111110, 0b11000011, 0b11000000, 0b11000000, 0b11000000, 0b11000000, 0b11000011, 0b01111110},
};

int8_t glyphIndexFor(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  switch (c) {
    case 'S': return 10;
    case 'Y': return 11;
    case 'N': return 12;
    case 'C': return 13;
  }
  return -1;
}

// ===============================================================================
// --- Application State & Buffers ---
// ===============================================================================

// Screen 1: Horizontal 32x8 Load Simulation Buffers (bit 31 = left x=0, bit 0 = right x=31)
uint32_t loadRows[PANEL_HEIGHT];
uint32_t sparkleMask[PANEL_HEIGHT];
bool rowDirections[PANEL_HEIGHT];
uint8_t rowLifetimes[PANEL_HEIGHT];
uint8_t currentGen = 0; // 8-bit vertical slice entering at x=0 (left edge)
bool alertPipBlinkState = false;

// Screen 2: Clock Buffer
char timeChars[SCREEN2_NUM_DEVICES] = {'S', 'Y', 'N', 'C'};

// Time Synchronization Anchor
time_t lastKnownValidEpoch        = 0;
unsigned long lastSyncLocalMillis = 0;
bool isTimeEverSynced             = false;

// Compact Environmental & Telemetry State
int forecast8HrWeatherId          = 800;
float forecast8HrTempC            = 20.0f;
uint8_t currentHumidity           = 50;     // % (0..100)
uint16_t currentPressure          = 1013;   // hPa
float currentWindSpeed            = 2.0f;   // m/s
uint8_t currentClouds             = 20;     // % (0..100)
int currentAQI                    = 1;
float currentPM25                 = 0.0f;
unsigned long sunriseEpoch        = 0;
unsigned long sunsetEpoch         = 0;
bool isDaytime                    = true;
uint8_t currentMatrixIntensity    = 0;

// NeoPixel States
float pixelSpeeds[NUM_PIXELS];
float pixelOffsets[NUM_PIXELS];
float pixelPhaseOffsets[NUM_PIXELS];

// Execution Timers
unsigned long lastTick            = 0;
unsigned long lastSparkle         = 0;
unsigned long lastTwinkle         = 0;
unsigned long lastWeatherPoll     = 0;
unsigned long lastTimeUpdate      = 0;
unsigned long lastNtpRetry        = 0;

// Refresh Rates (ms)
uint16_t tickInterval             = 1000;
uint16_t sparkleInterval          = 350;
uint16_t twinkleInterval          = 30;

void reseed();
uint8_t nextGeneration(uint8_t gen);
uint16_t computeWindTickInterval();
bool hasAtmosphericTurbulence();
void tickLoadSimulation();
void rollSparkle();
uint8_t computeHybridMeterHeight();
void renderScreen1_Load();
void drawBigCharToDevice(int8_t glyphIdx, int logicalSlot);
void renderScreen2_Clock();
time_t updateClockData();
void updateMatrixBrightness(time_t now);
void updateTwinkle();
void fetchEnvironmentData();
uint32_t blendHSV(uint16_t h1, uint8_t s1, uint8_t v1, uint16_t h2, uint8_t s2, uint8_t v2, float progress);

// ===============================================================================
// --- Main Setup ---
// ===============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(F("\n\n--- CM-5 Telemetry Load & Smart Clock Booting ---"));

  randomSeed(analogRead(A0));

  for (int dev = 0; dev < NUM_MAX_DEVICES; dev++) {
    lc.shutdown(dev, false);
    lc.setIntensity(dev, 0);
    lc.clearDisplay(dev);
  }

  strip.begin();
  strip.show();

  // Half-speed twinkle frequencies
  for (int i = 0; i < NUM_PIXELS; i++) {
    pixelSpeeds[i]       = 0.40 + (float)random(15, 45) / 100.0;
    pixelOffsets[i]      = (float)random(0, 628) / 100.0;
    pixelPhaseOffsets[i] = (float)random(0, 628) / 100.0;
  }

  for (int r = 0; r < PANEL_HEIGHT; r++) {
    loadRows[r]      = 0;
    sparkleMask[r]   = 0;
    rowDirections[r] = (random(0, 2) == 1);
    rowLifetimes[r]  = random(4, 12);
  }

  reseed();
  renderScreen1_Load();
  renderScreen2_Clock();

  setenv("TZ", TZ_INFO, 1);
  tzset();

  Serial.print(F("[WIFI] Connecting to "));
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(400);
    Serial.print(F("."));
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[WIFI] Connected!"));
    configTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "time.google.com");
    lastNtpRetry = millis();
    fetchEnvironmentData();
  } else {
    Serial.println(F("\n[WIFI] Connection pending/failed. Entering background retry mode."));
  }
}

// ===============================================================================
// --- Main Loop ---
// ===============================================================================
void loop() {
  unsigned long now = millis();

  // 1. Maintain WiFi and Periodic SNTP Background Watchdog
  if (WiFi.status() == WL_CONNECTED) {
    if (!isTimeEverSynced && (now - lastNtpRetry >= 60000)) {
      lastNtpRetry = now;
      Serial.println(F("[NTP] Retriggering configTime() background sync..."));
      configTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "time.google.com");
    }
  }

  // 2. Poll OpenWeather API every 15 minutes (or on initial boot)
  if (now - lastWeatherPoll >= 900000 || lastWeatherPoll == 0) {
    lastWeatherPoll = now;
    if (WiFi.status() == WL_CONNECTED) {
      fetchEnvironmentData();
    }
  }

  // 3. Active Clock Update, Matrix Brightness & Screen 2 Render every 1 second
  if (now - lastTimeUpdate >= 1000) {
    lastTimeUpdate = now;
    time_t currentEpoch = updateClockData();
    updateMatrixBrightness(currentEpoch);
    renderScreen2_Clock();
  }

  // 4. Step Horizontal Telemetry-Driven Load Simulation (Left-to-Right)
  tickInterval = computeWindTickInterval();
  if (now - lastTick >= tickInterval) {
    lastTick = now;
    alertPipBlinkState = !alertPipBlinkState;
    tickLoadSimulation();
    renderScreen1_Load();
  }

  // 5. Update Screen 1 Sparkles & Render
  if (now - lastSparkle >= sparkleInterval) {
    lastSparkle = now;
    rollSparkle();
    renderScreen1_Load();
  }

  // 6. Asynchronous NeoPixel Twinkle
  if (now - lastTwinkle >= twinkleInterval) {
    lastTwinkle = now;
    updateTwinkle();
  }
}

// ===============================================================================
// --- Non-Destructive Active Clock Engine ---
// ===============================================================================
time_t updateClockData() {
  time_t sysNow = time(nullptr);
  unsigned long currentMillis = millis();

  if (sysNow > 1000000000) {
    isTimeEverSynced    = true;
    lastKnownValidEpoch = sysNow;
    lastSyncLocalMillis = currentMillis;
  } else if (isTimeEverSynced && lastKnownValidEpoch > 0) {
    sysNow = lastKnownValidEpoch + ((currentMillis - lastSyncLocalMillis) / 1000);
  }

  if (!isTimeEverSynced && sysNow < 1000000000) {
    timeChars[0] = 'S';
    timeChars[1] = 'Y';
    timeChars[2] = 'N';
    timeChars[3] = 'C';
    return sysNow;
  }

  if (sunriseEpoch > 0 && sunsetEpoch > 0) {
    isDaytime = ((unsigned long)sysNow >= sunriseEpoch && (unsigned long)sysNow < sunsetEpoch);
  } else {
    struct tm* ti = localtime(&sysNow);
    isDaytime = (ti->tm_hour >= 6 && ti->tm_hour < 20);
  }

  struct tm* ti = localtime(&sysNow);
  char hh[4], mm[4];
  strftime(hh, sizeof(hh), "%H", ti);
  strftime(mm, sizeof(mm), "%M", ti);

  timeChars[0] = hh[0];
  timeChars[1] = hh[1];
  timeChars[2] = mm[0];
  timeChars[3] = mm[1];

  return sysNow;
}

// ===============================================================================
// --- Dynamic Matrix Brightness Ramp (0 Night -> 5 Day) ---
// ===============================================================================
void updateMatrixBrightness(time_t now) {
  uint8_t targetIntensity = 0;

  if (sunriseEpoch > 0 && sunsetEpoch > 0) {
    if (now >= sunriseEpoch && now < sunsetEpoch) {
      unsigned long secondsSinceSunrise = now - sunriseEpoch;

      if (secondsSinceSunrise < 3600) {
        targetIntensity = (uint8_t)((secondsSinceSunrise * 5) / 3600);
      } else {
        targetIntensity = 5;
      }
    } else {
      targetIntensity = 0;
    }
  } else {
    struct tm* ti = localtime(&now);
    if (ti->tm_hour >= 7 && ti->tm_hour < 20) {
      targetIntensity = 5;
    } else {
      targetIntensity = 0;
    }
  }

  if (targetIntensity != currentMatrixIntensity) {
    currentMatrixIntensity = targetIntensity;
    for (int dev = 0; dev < NUM_MAX_DEVICES; dev++) {
      lc.setIntensity(dev, currentMatrixIntensity);
    }
  }
}

// ===============================================================================
// --- Screen 1: Telemetry-Driven Left-to-Right Load Simulation ---
// ===============================================================================

// Map wind speed (0..15 m/s) to tick cadence (1500 ms calm down to 350 ms brisk)
uint16_t computeWindTickInterval() {
  float w = currentWindSpeed;
  if (w < 0.0f)  w = 0.0f;
  if (w > 15.0f) w = 15.0f;
  float ratio = w / 15.0f;
  return (uint16_t)(1500.0f - (ratio * 1150.0f));
}

// Low barometric pressure or incoming storm/rain/snow increases per-row turbulence
bool hasAtmosphericTurbulence() {
  bool lowPressure = (currentPressure > 0 && currentPressure < 1005);
  bool precipAhead = (forecast8HrWeatherId >= 200 && forecast8HrWeatherId < 700);
  return lowPressure || precipAhead;
}

// Seed the 8-bit intake slice using humidity and cloud cover
void reseed() {
  currentGen = 0;
  uint8_t moistureScore = (uint8_t)(((uint16_t)currentHumidity + (uint16_t)currentClouds) / 2);

  uint8_t activeBits = 1;
  if (moistureScore >= 80) {
    activeBits = 4;
  } else if (moistureScore >= 55) {
    activeBits = 3;
  } else if (moistureScore >= 25) {
    activeBits = 2;
  }

  for (uint8_t i = 0; i < activeBits; i++) {
    currentGen |= (1 << random(0, PANEL_HEIGHT));
  }
  if (currentGen == 0) {
    currentGen = (1 << (PANEL_HEIGHT / 2));
  }
}

// Rule 90 over the 8-bit vertical slice entering at the left edge (x = 0)
uint8_t nextGeneration(uint8_t gen) {
  uint8_t next = 0;
  for (int y = 0; y < PANEL_HEIGHT; y++) {
    bool up   = (y > 0)                  ? bitRead(gen, y - 1) : 0;
    bool down = (y < PANEL_HEIGHT - 1) ? bitRead(gen, y + 1) : 0;
    if (up ^ down) next |= (1 << y);
  }
  return next;
}

void tickLoadSimulation() {
  bool turbulent = hasAtmosphericTurbulence();
  uint8_t driftThreshold = turbulent ? 9 : 6; // 90% in turbulence, 60% normally

  // 1. Slide all 8 horizontal rows from left (bit 31 / x=0) to right (bit 0 / x=31)
  for (int y = 0; y < PANEL_HEIGHT; y++) {
    uint32_t incomingLeftBit = bitRead(currentGen, y) ? (1UL << 31) : 0UL;
    uint32_t val = (loadRows[y] >> 1) | incomingLeftBit;

    // 2. Independent per-row horizontal load drift along the 32-bit lane
    if (val != 0 && random(0, 10) < driftThreshold) {
      if (rowDirections[y]) {
        val = (val >> 1) | (val << 31);
      } else {
        val = (val << 1) | (val >> 31);
      }

      if (rowLifetimes[y] > 0) {
        rowLifetimes[y]--;
      } else {
        rowDirections[y] = (random(0, 2) == 1);
        rowLifetimes[y]  = turbulent ? random(2, 6) : random(4, 12);
      }
    }

    loadRows[y] = val;
  }

  currentGen = nextGeneration(currentGen);

  static uint8_t genCount = 0;
  genCount++;
  uint8_t maxGenSpan = turbulent ? 24 : 40;
  if (currentGen == 0 || genCount > maxGenSpan) {
    reseed();
    genCount = 0;
  }
}

void rollSparkle() {
  for (int y = 0; y < PANEL_HEIGHT; y++) {
    sparkleMask[y] = 0;
  }

  uint8_t minSparkles = 0;
  uint8_t maxSparkles = 2;

  if (currentAQI >= 5 || currentPM25 >= 150.0f) {
    minSparkles = 70;
    maxSparkles = 110;
  } else if (currentAQI == 4 || currentPM25 >= 75.0f) {
    minSparkles = 35;
    maxSparkles = 65;
  } else if (currentAQI == 3 || currentPM25 >= 35.0f) {
    minSparkles = 3;
    maxSparkles = 6;
  } else {
    minSparkles = 0;
    maxSparkles = 2;
  }

  uint8_t sparkleCount = random(minSparkles, maxSparkles + 1);
  for (uint8_t n = 0; n < sparkleCount; n++) {
    uint8_t x = random(1, PANEL_WIDTH);  // Keep x=0 dedicated to the load meter
    uint8_t y = random(0, PANEL_HEIGHT); // 0..7 top to bottom
    sparkleMask[y] |= (1UL << (31 - x));
  }
}

// Hybrid Meter: Continuous PM2.5 scaling (0..120 ug/m3 -> 0..7 bars) + AQI tier floor
uint8_t computeHybridMeterHeight() {
  float clampedPM = currentPM25;
  if (clampedPM < 0.0f)   clampedPM = 0.0f;
  if (clampedPM > 120.0f) clampedPM = 120.0f;

  uint8_t pmBars = (uint8_t)((clampedPM / 120.0f) * 7.0f + 0.5f);
  if (currentPM25 > 1.0f && pmBars == 0) {
    pmBars = 1;
  }

  uint8_t aqiFloor = 0;
  if (currentAQI >= 5) {
    aqiFloor = 8;
  } else if (currentAQI == 4) {
    aqiFloor = 6;
  } else if (currentAQI == 3) {
    aqiFloor = 3;
  } else if (currentAQI == 2) {
    aqiFloor = 1;
  }

  uint8_t bars = (pmBars > aqiFloor) ? pmBars : aqiFloor;
  if (bars > PANEL_HEIGHT) bars = PANEL_HEIGHT;
  return bars;
}

void renderScreen1_Load() {
  const uint32_t METER_BIT = (1UL << 31); // Column x = 0 (leftmost column of Module 0)
  uint8_t meterBars = computeHybridMeterHeight();
  bool severeAlert = (currentAQI >= 4);

  for (int y = 0; y < PANEL_HEIGHT; y++) {
    // Combine sliding simulation and sparkles on columns x = 1..31, clearing x = 0
    uint32_t combined = (loadRows[y] ^ sparkleMask[y]) & ~METER_BIT;

    // Bottom-up bar fills rows y = (8 - meterBars) .. 7
    bool barLit = (meterBars > 0) && (y >= (PANEL_HEIGHT - meterBars));

    // In severe AQI (4 or 5), pulse the top-most pip (y = 0) on the tick
    if (severeAlert && y == 0) {
      barLit = alertPipBlinkState;
    }

    if (barLit) {
      combined |= METER_BIT;
    }

    // Split 32-bit horizontal row across 4 side-by-side modules (left = dev 0, right = dev 3)
    for (int m = 0; m < SCREEN1_NUM_DEVICES; m++) {
      int dev = SCREEN1_DEVICE_OFFSET + m;
      int shiftAmount = (3 - m) * 8;
      uint8_t byteVal = (uint8_t)((combined >> shiftAmount) & 0xFF);
      lc.setRow(dev, y, byteVal);
    }
  }
}

// ===============================================================================
// --- Screen 2: Horizontal 4-Digit Kanji Clock ---
// ===============================================================================
void drawBigCharToDevice(int8_t glyphIdx, int logicalSlot) {
  int dev = SCREEN2_DEVICE_OFFSET + (REVERSE_PANEL_2 ? (SCREEN2_NUM_DEVICES - 1 - logicalSlot) : logicalSlot);

  for (int r = 0; r < 8; r++) {
    uint8_t rowVal = 0x00;
    if (glyphIdx >= 0) {
      rowVal = pgm_read_byte(&(FONT_HORIZ_KANJI[glyphIdx][r]));
    }

    int outRow = r;
    if (REVERSE_PANEL_2) {
      outRow = 7 - r;
      uint8_t mirrored = 0;
      for (int b = 0; b < 8; b++) {
        if (rowVal & (1 << b)) mirrored |= (1 << (7 - b));
      }
      rowVal = mirrored;
    }
    lc.setRow(dev, outRow, rowVal);
  }
}

void renderScreen2_Clock() {
  for (int slot = 0; slot < SCREEN2_NUM_DEVICES; slot++) {
    int8_t idx = glyphIndexFor(timeChars[slot]);
    drawBigCharToDevice(idx, slot);
  }
}

// ===============================================================================
// --- NeoPixel Twinkle & 8-Hour Forecast Weather Palette Engine ---
// ===============================================================================
uint32_t blendHSV(uint16_t h1, uint8_t s1, uint8_t v1, uint16_t h2, uint8_t s2, uint8_t v2, float progress) {
  int32_t diff = (int32_t)h2 - (int32_t)h1;
  if (diff > 32767)  diff -= 65536;
  if (diff < -32767) diff += 65536;

  uint16_t h = (uint16_t)((int32_t)h1 + (int32_t)(diff * progress));
  uint8_t  s = (uint8_t)(s1 + (s2 - s1) * progress);
  uint8_t  v = (uint8_t)(v1 + (v2 - v1) * progress);

  return strip.ColorHSV(h, s, v);
}

void updateTwinkle() {
  float sec = millis() / 1000.0;

  float minBright = isDaytime ? 0.08 : 0.003;
  float maxBright = isDaytime ? 1.00 : 0.040;

  for (int i = 0; i < NUM_PIXELS; i++) {
    float rawWave = (sin(sec * pixelSpeeds[i] + pixelOffsets[i]) + 1.0) / 2.0;
    float sharpTwinkle = rawWave * rawWave * rawWave;
    float brightness = minBright + (sharpTwinkle * (maxBright - minBright));

    float phase = (sin(sec * 0.02 + pixelPhaseOffsets[i]) + 1.0) / 2.0;

    uint32_t baseColor = 0;

    if (currentAQI >= 5) {
      baseColor = strip.ColorHSV(52000, 255, 255); // Purple
    } else if (currentAQI == 4) {
      baseColor = strip.ColorHSV(0, 255, 255);     // Red
    } else if (currentAQI == 3) {
      baseColor = strip.ColorHSV(4500, 255, 255);  // Orange
    } else if (!isDaytime) {
      baseColor = blendHSV(43500, 255, 160, 48500, 240, 100, phase);
    } else if (forecast8HrWeatherId >= 200 && forecast8HrWeatherId < 300) {
      baseColor = blendHSV(39000, 255, 255, 54000, 255, 255, phase);

      if (random(0, 400) == 77) {
        baseColor = strip.ColorHSV(0, 0, 255);
        brightness = 1.0;
      }
    } else if ((forecast8HrWeatherId >= 600 && forecast8HrWeatherId < 700) ||
               ((forecast8HrWeatherId >= 300 && forecast8HrWeatherId < 600) && forecast8HrTempC <= 1.0f)) {
      baseColor = blendHSV(33500, 230, 255, 37500, 70, 230, phase);
    } else if (forecast8HrWeatherId >= 300 && forecast8HrWeatherId < 600) {
      if (phase < 0.5f) {
        baseColor = blendHSV(34500, 255, 255, 43500, 255, 255, phase * 2.0f);
      } else {
        baseColor = blendHSV(43500, 255, 255, 50500, 255, 255, (phase - 0.5f) * 2.0f);
      }
    } else if ((forecast8HrWeatherId >= 700 && forecast8HrWeatherId < 800) || forecast8HrWeatherId >= 803) {
      baseColor = blendHSV(38000, 25, 255, 36000, 70, 140, phase);
    } else if (forecast8HrWeatherId == 802) {
      if (phase < 0.5f) {
        baseColor = blendHSV(3200, 255, 255, 9500, 255, 255, phase * 2.0f);
      } else {
        baseColor = blendHSV(9500, 255, 255, 8000, 35, 255, (phase - 0.5f) * 2.0f);
      }
    } else {
      uint16_t hue = (uint16_t)((sec * 250.0f) + (i * 3500)) % 65535;
      baseColor = strip.ColorHSV(hue, 240, 255);
    }

    uint8_t r = (uint8_t)(((baseColor >> 16) & 0xFF) * brightness);
    uint8_t g = (uint8_t)(((baseColor >> 8)  & 0xFF) * brightness);
    uint8_t b = (uint8_t)(((baseColor)       & 0xFF) * brightness);

    strip.setPixelColor(i, strip.Color(r, g, b));
  }

  strip.show();
}

// ===============================================================================
// --- Memory-Safe Stream & Filtered Environment Ingestion ---
// ===============================================================================
void fetchEnvironmentData() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(2500);

  // 1. Current Weather Endpoint: Supplies 'now', sunrise/sunset, and telemetry
  String currentWeatherUrl = "http://api.openweathermap.org/data/2.5/weather?q=" +
                             String(CITY_NAME) + "&units=metric&appid=" + String(OPENWEATHER_KEY);

  if (http.begin(client, currentWeatherUrl)) {
    if (http.GET() == HTTP_CODE_OK) {
      StaticJsonDocument<192> weatherFilter;
      weatherFilter["dt"] = true;
      weatherFilter["sys"]["sunrise"] = true;
      weatherFilter["sys"]["sunset"] = true;
      weatherFilter["main"]["humidity"] = true;
      weatherFilter["main"]["pressure"] = true;
      weatherFilter["wind"]["speed"] = true;
      weatherFilter["clouds"]["all"] = true;

      StaticJsonDocument<384> doc;
      if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(weatherFilter))) {
        sunriseEpoch     = doc["sys"]["sunrise"] | sunriseEpoch;
        sunsetEpoch      = doc["sys"]["sunset"]  | sunsetEpoch;
        currentHumidity  = doc["main"]["humidity"] | currentHumidity;
        currentPressure  = doc["main"]["pressure"] | currentPressure;
        currentWindSpeed = doc["wind"]["speed"]    | currentWindSpeed;
        currentClouds    = doc["clouds"]["all"]    | currentClouds;

        unsigned long serverNow = doc["dt"] | 0UL;
        if (serverNow > 1000000000UL) {
          if (time(nullptr) < 1000000000) {
            struct timeval tv = { (time_t)serverNow, 0 };
            settimeofday(&tv, nullptr);
            setenv("TZ", TZ_INFO, 1);
            tzset();
          }
          lastKnownValidEpoch = (time_t)serverNow;
          lastSyncLocalMillis = millis();
          isTimeEverSynced    = true;
        }
      }
    }
    http.end();
  }

  // 2. 5-Day / 3-Hour Forecast: Strict cnt=4 + stream filter for 8-hour lookup
  String forecastUrl = "http://api.openweathermap.org/data/2.5/forecast?q=" +
                       String(CITY_NAME) + "&cnt=4&units=metric&appid=" + String(OPENWEATHER_KEY);

  if (http.begin(client, forecastUrl)) {
    if (http.GET() == HTTP_CODE_OK) {
      StaticJsonDocument<192> forecastFilter;
      forecastFilter["list"][0]["dt"] = true;
      forecastFilter["list"][0]["main"]["temp"] = true;
      forecastFilter["list"][0]["weather"][0]["id"] = true;

      StaticJsonDocument<1024> doc;
      if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(forecastFilter))) {
        time_t now = time(nullptr);
        unsigned long baseEpoch = (now > 1000000000) ? (unsigned long)now : (unsigned long)lastKnownValidEpoch;
        unsigned long targetForecastEpoch = baseEpoch + (8UL * 3600UL);

        JsonArray list = doc["list"];
        unsigned long bestDiff = 0xFFFFFFFFUL;

        for (JsonObject entry : list) {
          unsigned long entryDt = entry["dt"] | 0UL;
          if (entryDt == 0UL) continue;
          unsigned long diff = (entryDt > targetForecastEpoch) ? (entryDt - targetForecastEpoch) : (targetForecastEpoch - entryDt);

          if (diff < bestDiff) {
            bestDiff = diff;
            forecast8HrWeatherId = entry["weather"][0]["id"] | forecast8HrWeatherId;
            forecast8HrTempC     = entry["main"]["temp"]        | forecast8HrTempC;
          }
        }
      }
    }
    http.end();
  }

  // 3. Air Pollution Data (AQI & PM2.5)
  String aqiUrl = "http://api.openweathermap.org/data/2.5/air_pollution?lat=" +
                  String(LATITUDE) + "&lon=" + String(LONGITUDE) + "&appid=" + String(OPENWEATHER_KEY);

  if (http.begin(client, aqiUrl)) {
    if (http.GET() == HTTP_CODE_OK) {
      StaticJsonDocument<128> aqiFilter;
      aqiFilter["list"][0]["main"]["aqi"] = true;
      aqiFilter["list"][0]["components"]["pm2_5"] = true;

      StaticJsonDocument<384> doc;
      if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(aqiFilter))) {
        currentAQI  = doc["list"][0]["main"]["aqi"]          | currentAQI;
        currentPM25 = doc["list"][0]["components"]["pm2_5"] | currentPM25;
      }
    }
    http.end();
  }
}
