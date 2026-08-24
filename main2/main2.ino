/*
  ================================================================================
   CM-5 "Thinking Machine" Automaton & Smart Clock Display
  ================================================================================
   Description:
   Drives two vertical LED matrix towers (8x32 each) using 8x MAX7219 modules,
   plus an accompanying WS2812B NeoPixel strip for ambient weather condition lighting.

   Screen 1 (Modules 0-3): Rule 90 cellular automaton (CM-5 replica) with sparkles.
   Screen 2 (Modules 4-7): Vertical 4-digit 24-hour clock.
   NeoPixels: Smooth asynchronous twinkle; color mapped to OpenWeather API conditions;
              brightness scales automatically based on local sunrise/sunset epochs.

   Hardware:
   - ESP8266 (e.g., Wemos D1 Mini or NodeMCU)
   - 8x MAX7219 8x8 LED Matrix Modules (daisy-chained)
   - WS2812B NeoPixel Strip/Ring

   Dependencies (Install via Arduino Library Manager):
   - LedControl by Eberhard Fahle
   - Adafruit NeoPixel by Adafruit
   - ArduinoJson by Benoit Blanchon
  ================================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <LedControl.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

// ===============================================================================
// --- User Configuration & Network Credentials ---
// ===============================================================================
const char* WIFI_SSID       = "YOUR_WIFI_SSID_HERE";
const char* WIFI_PASS       = "YOUR_WIFI_PASSWORD_HERE";

// OpenWeatherMap API Key (Free tier)
const char* OPENWEATHER_KEY = "YOUR_OPENWEATHER_API_KEY_HERE";
// Target city for weather and sunrise/sunset (Format: "City,CountryCode")
const char* CITY_NAME       = "YOUR_CITY,YOUR_COUNTRY_CODE"; 

// Hardware Orientation configuration
// Set to true if Screen 2 (modules 4-7) is physically mounted 180 degrees 
// (upside-down) relative to Screen 1.
const bool REVERSE_PANEL_2  = true;

// POSIX Timezone string (Default: US Eastern Time with automatic DST adjustments)
const char* TZ_INFO = "EST5EDT,M3.2.0,M11.1.0";

// ===============================================================================
// --- Hardware Pin Configuration (ESP8266) ---
// ===============================================================================
const int PIN_DATA_IN  = D4; // MAX7219 DIN (GPIO2)
const int PIN_CLK      = D2; // MAX7219 CLK (GPIO4)
const int PIN_CS       = D3; // MAX7219 CS  (GPIO0)
const int PIN_NEOPIXEL = D6; // NeoPixel DIN (GPIO12)

#define NUM_MAX_DEVICES 8
#define NUM_PIXELS      8

#define PANEL_WIDTH     8    // 8 pixels wide per tower
#define PANEL_HEIGHT    32   // 4 modules stacked vertically = 32 pixels tall
#define DEFAULT_INTENSITY 1  // MAX7219 Brightness: 0 (dimmest) to 15 (brightest)

#define SCREEN2_DEVICE_OFFSET 4
#define SCREEN2_NUM_DEVICES   4

LedControl lc = LedControl(PIN_DATA_IN, PIN_CLK, PIN_CS, NUM_MAX_DEVICES);
Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ===============================================================================
// --- Font Definitions ---
// ===============================================================================
// Pre-Rotated 90 Degree Clockwise Big Font (5x7 footprint in an 8x8 matrix).
// Hardcoded rotation saves CPU cycles during the render loop.
const uint8_t FONT_BIG_ROTATED[14][8] PROGMEM = {
  // '0'
  {0b00000000, 0b00111110, 0b01010001, 0b01001001, 0b01000101, 0b00111110, 0b00000000, 0b00000000},
  // '1'
  {0b00000000, 0b00000000, 0b01000001, 0b01111111, 0b01000000, 0b00000000, 0b00000000, 0b00000000},
  // '2'
  {0b00000000, 0b01100010, 0b01010001, 0b01001001, 0b01000101, 0b01000010, 0b00000000, 0b00000000},
  // '3'
  {0b00000000, 0b00100010, 0b01000001, 0b01001001, 0b01001001, 0b00110110, 0b00000000, 0b00000000},
  // '4'
  {0b00000000, 0b00011000, 0b00010100, 0b00010010, 0b01111111, 0b00010000, 0b00000000, 0b00000000},
  // '5'
  {0b00000000, 0b00100111, 0b01000101, 0b01000101, 0b01000101, 0b00111001, 0b00000000, 0b00000000},
  // '6'
  {0b00000000, 0b00111110, 0b01001001, 0b01001001, 0b01001001, 0b00110000, 0b00000000, 0b00000000},
  // '7'
  {0b00000000, 0b00000001, 0b01110001, 0b00001001, 0b00000101, 0b00000011, 0b00000000, 0b00000000},
  // '8'
  {0b00000000, 0b00110110, 0b01001001, 0b01001001, 0b01001001, 0b00110110, 0b00000000, 0b00000000},
  // '9'
  {0b00000000, 0b00001100, 0b01010011, 0b01010011, 0b01010011, 0b00111110, 0b00000000, 0b00000000},
  // 'S' (index 10 - fallback status char)
  {0b00000000, 0b00100110, 0b01001001, 0b01001001, 0b01001001, 0b00110010, 0b00000000, 0b00000000},
  // 'Y' (index 11)
  {0b00000000, 0b00000011, 0b00000100, 0b01111000, 0b00000100, 0b00000011, 0b00000000, 0b00000000},
  // 'N' (index 12)
  {0b00000000, 0b01111111, 0b00000100, 0b00001000, 0b00010000, 0b01111111, 0b00000000, 0b00000000},
  // 'C' (index 13)
  {0b00000000, 0b00111110, 0b01000001, 0b01000001, 0b01000001, 0b00100010, 0b00000000, 0b00000000},
};

// Maps an ASCII char to the corresponding FONT_BIG_ROTATED index
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

// Screen 1: Automaton Buffers
uint8_t cm5Rows[PANEL_HEIGHT];
uint8_t sparkleMask[PANEL_HEIGHT];
uint8_t currentGen = 0;

// Screen 2: Clock Buffer (One character per physical 8x8 module)
char timeChars[SCREEN2_NUM_DEVICES] = {'-', '-', '-', '-'};

// Environmental State
int currentWeatherId       = 800; // Default: 800 (Clear Sky)
unsigned long sunriseEpoch = 0;
unsigned long sunsetEpoch  = 0;
bool isDaytime             = true;

// NeoPixel Dynamics
float pixelSpeeds[NUM_PIXELS];
float pixelOffsets[NUM_PIXELS];

// Execution Timers
unsigned long lastTick        = 0;
unsigned long lastSparkle     = 0;
unsigned long lastTwinkle     = 0;
unsigned long lastWeatherPoll = 0;
unsigned long lastTimeUpdate  = 0;
unsigned long lastDebugPrint  = 0;

// Refresh Rates (ms)
uint16_t tickInterval    = 650; // Automaton cascade cadence
uint16_t sparkleInterval = 350; // Flicker overlay cadence
uint16_t twinkleInterval = 30;  // NeoPixel refresh rate (~33 FPS)

// Function Prototypes
void reseed();
uint8_t nextGeneration(uint8_t gen);
void tickAutomaton();
void rollSparkle();
void renderScreen1_CM5();
void drawBigCharToDevice(int8_t glyphIdx, int logicalSlot);
void renderScreen2_Clock();
void updateClockData();
void updateTwinkle();
void fetchWeather();

// ===============================================================================
// --- Main Setup ---
// ===============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000); // Allow Serial port to initialize

  Serial.println(F("\n\n--- CM-5 Display & Smart Clock Booting ---"));

  randomSeed(analogRead(A0));

  // Initialize MAX7219 Modules
  for (int dev = 0; dev < NUM_MAX_DEVICES; dev++) {
    lc.shutdown(dev, false);
    lc.setIntensity(dev, DEFAULT_INTENSITY);
    lc.clearDisplay(dev);
  }

  // Initialize NeoPixels
  strip.begin();
  strip.show();

  // Assign random sinusoidal phase offsets for organic twinkling
  for (int i = 0; i < NUM_PIXELS; i++) {
    pixelSpeeds[i]  = 0.4 + (float)random(30, 80) / 100.0;
    pixelOffsets[i] = (float)random(0, 628) / 100.0;
  }

  reseed();
  
  // Show "SYNC" on Screen 2 immediately
  updateClockData();
  renderScreen2_Clock();

  // Connect to Wi-Fi
  Serial.print(F("[WIFI] Connecting to "));
  Serial.print(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(F("."));
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[WIFI] Connected!"));
    Serial.print(F("[WIFI] IP: "));
    Serial.println(WiFi.localIP());
    Serial.println(F("[NTP] Requesting Time Sync..."));
    
    // Configure NTP (Primary + Fallbacks)
    configTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "time.google.com");
    setenv("TZ", TZ_INFO, 1);
    tzset();

    time_t now = time(nullptr);
    int syncRetries = 0;
    
    // Await initial NTP lock (up to 10 seconds)
    while (now < 100000 && syncRetries < 20) {
      delay(500);
      now = time(nullptr);
      Serial.print(F("[NTP] Waiting... Current epoch: "));
      Serial.println((long)now);
      syncRetries++;
    }

    if (now > 100000) {
      Serial.println(F("[NTP] Time Sync SUCCESS!"));
    } else {
      Serial.println(F("[NTP] Time Sync FAILED during boot. Will retry in background."));
    }

    // Initial weather fetch & screen update
    fetchWeather();
    updateClockData();
    renderScreen2_Clock();
  } else {
    Serial.println(F("\n[WIFI] Connection FAILED."));
  }
}

// ===============================================================================
// --- Main Loop ---
// ===============================================================================
void loop() {
  unsigned long now = millis();

  // 1. Poll OpenWeather API (every 15 minutes)
  if (now - lastWeatherPoll >= 900000 || lastWeatherPoll == 0) {
    lastWeatherPoll = now;
    if (WiFi.status() == WL_CONNECTED) {
      fetchWeather();
    }
  }

  // 2. Update Date/Time & Screen 2 Render (every 1 second)
  if (now - lastTimeUpdate >= 1000) {
    lastTimeUpdate = now;
    updateClockData();
    renderScreen2_Clock();

    // Diagnostics: Alert if still trapped in SYNC phase
    time_t rawtime = time(nullptr);
    if (rawtime < 100000 && now - lastDebugPrint >= 5000) {
      lastDebugPrint = now;
      Serial.print(F("[DEBUG] Still waiting for NTP. Wi-Fi status: "));
      Serial.print(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
      Serial.print(F(" | Epoch: "));
      Serial.println((long)rawtime);
    }
  }

  // 3. Step Rule 90 Automaton Logic
  if (now - lastTick >= tickInterval) {
    lastTick = now;
    tickAutomaton();
  }

  // 4. Update Screen 1 Sparkles & Render
  if (now - lastSparkle >= sparkleInterval) {
    lastSparkle = now;
    rollSparkle();
    renderScreen1_CM5();
  }

  // 5. Asynchronous NeoPixel Twinkle
  if (now - lastTwinkle >= twinkleInterval) {
    lastTwinkle = now;
    updateTwinkle();
  }
}

// ===============================================================================
// --- Screen 1: CM-5 Cellular Automaton ---
// ===============================================================================

void reseed() {
  currentGen = 0;
  if (random(0, 3) == 0) {
    // Scatter a few live bits for variety
    for (uint8_t i = 0; i < 2; i++) {
      currentGen |= (1 << random(0, PANEL_WIDTH));
    }
  } else {
    // Classic single-bit centered seed
    currentGen = (1 << (PANEL_WIDTH / 2));
  }
}

uint8_t nextGeneration(uint8_t gen) {
  uint8_t next = 0;
  for (int i = 0; i < PANEL_WIDTH; i++) {
    bool left  = (i > 0)               ? bitRead(gen, i - 1) : 0;
    bool right = (i < PANEL_WIDTH - 1) ? bitRead(gen, i + 1) : 0;
    if (left ^ right) next |= (1 << i);
  }
  return next;
}

void tickAutomaton() {
  // Shift rows downwards
  for (int r = PANEL_HEIGHT - 1; r > 0; r--) {
    cm5Rows[r] = cm5Rows[r - 1];
  }
  cm5Rows[0] = currentGen;
  currentGen = nextGeneration(currentGen);

  static uint8_t genCount = 0;
  genCount++;
  
  // Reseed if the automaton dies out or runs long enough
  if (currentGen == 0 || genCount > 40) {
    reseed();
    genCount = 0;
  }
}

void rollSparkle() {
  for (uint8_t r = 0; r < PANEL_HEIGHT; r++) {
    sparkleMask[r] = 0;
  }
  uint8_t sparkleCount = random(2, 6);
  for (uint8_t n = 0; n < sparkleCount; n++) {
    uint8_t x = random(0, PANEL_WIDTH);
    uint8_t y = random(0, PANEL_HEIGHT);
    sparkleMask[y] |= (1 << x);
  }
}

void renderScreen1_CM5() {
  for (int y = 0; y < PANEL_HEIGHT; y++) {
    uint8_t rowVal = cm5Rows[y] ^ sparkleMask[y];
    int dev = y / 8;
    int localRow = y % 8;
    lc.setRow(dev, localRow, rowVal);
  }
}

// ===============================================================================
// --- Screen 2: Dedicated Clock ---
// ===============================================================================

// Renders one large character to a specific 8x8 MAX7219 module.
void drawBigCharToDevice(int8_t glyphIdx, int logicalSlot) {
  int dev = SCREEN2_DEVICE_OFFSET + (REVERSE_PANEL_2 ? (SCREEN2_NUM_DEVICES - 1 - logicalSlot) : logicalSlot);

  for (int r = 0; r < 8; r++) {
    uint8_t rowVal = 0x00;
    if (glyphIdx >= 0) {
      rowVal = pgm_read_byte(&(FONT_BIG_ROTATED[glyphIdx][r]));
    }

    int outRow = r;
    if (REVERSE_PANEL_2) {
      // Hardware inversion if the physical strip is mounted upside-down
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

void updateClockData() {
  time_t now = time(nullptr);

  // Display 'SYNC' fallback if NTP has not locked
  if (now < 100000) {
    timeChars[0] = 'S';
    timeChars[1] = 'Y';
    timeChars[2] = 'N';
    timeChars[3] = 'C';
    return;
  }

  // Assess Day/Night for NeoPixel brightness ceiling
  if (sunriseEpoch > 0 && sunsetEpoch > 0) {
    isDaytime = (now >= sunriseEpoch && now < sunsetEpoch);
  } else {
    // Fallback if API failed
    struct tm* ti = localtime(&now);
    isDaytime = (ti->tm_hour >= 6 && ti->tm_hour < 20);
  }

  struct tm* ti = localtime(&now);
  char hh[4], mm[4];
  strftime(hh, sizeof(hh), "%H", ti); 
  strftime(mm, sizeof(mm), "%M", ti); 

  timeChars[0] = hh[0];
  timeChars[1] = hh[1];
  timeChars[2] = mm[0];
  timeChars[3] = mm[1];
}

void renderScreen2_Clock() {
  for (int slot = 0; slot < SCREEN2_NUM_DEVICES; slot++) {
    int8_t idx = glyphIndexFor(timeChars[slot]);
    drawBigCharToDevice(idx, slot);
  }
}

// ===============================================================================
// --- NeoPixel Twinkle & OpenWeatherMap API ---
// ===============================================================================

void updateTwinkle() {
  float sec = millis() / 1000.0;

  uint16_t baseHue   = 7000;
  uint8_t saturation = 255;

  // Modulate color profile based on OpenWeather Condition IDs
  if (currentWeatherId >= 200 && currentWeatherId < 300) {        
    baseHue = 50000; // Thunderstorm
  } else if (currentWeatherId >= 300 && currentWeatherId < 600) { 
    baseHue = 38000; // Rain / Drizzle
  } else if (currentWeatherId >= 600 && currentWeatherId < 700) { 
    baseHue = 33000; saturation = 90; // Snow
  } else if (currentWeatherId >= 700 && currentWeatherId < 800) { 
    baseHue = 21000; saturation = 110; // Atmosphere (Fog/Mist)
  } else if (currentWeatherId == 800) {                           
    baseHue = 6500;  // Clear Sky
  } else if (currentWeatherId > 800) {                            
    baseHue = 42000; saturation = 160; // Clouds
  }

  // Apply Day/Night Dimming profile
  float minBright = isDaytime ? 0.15 : 0.01;
  float maxBright = isDaytime ? 0.85 : 0.07; // <7% max duty cycle at night

  for (int i = 0; i < NUM_PIXELS; i++) {
    // Generate an independent sinusoidal wave for each pixel
    float wave = (sin(sec * pixelSpeeds[i] + pixelOffsets[i]) + 1.0) / 2.0;
    float brightness = minBright + (wave * (maxBright - minBright));

    uint8_t val = (uint8_t)(brightness * 255);
    strip.setPixelColor(i, strip.ColorHSV(baseHue, saturation, val));
  }
  strip.show();
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  String url = "http://api.openweathermap.org/data/2.5/weather?q=" +
               String(CITY_NAME) + "&appid=" + String(OPENWEATHER_KEY);

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        currentWeatherId = doc["weather"][0]["id"];
        sunriseEpoch     = doc["sys"]["sunrise"];
        sunsetEpoch      = doc["sys"]["sunset"];
      } else {
        Serial.print(F("[WEATHER] JSON Parse Error: "));
        Serial.println(error.f_str());
      }
    } else {
      Serial.print(F("[WEATHER] HTTP Request Failed. Code: "));
      Serial.println(httpCode);
    }
    http.end();
  }
}