/*
  ================================================================================
   CM-5 "Thinking Machine" Automaton & Smart Clock Display
  ================================================================================
   Screen 1 (Modules 0-3): Rule 90 cellular automaton (CM-5 replica) with 
              independent row shifts & PM2.5-driven sparkles.
   Screen 2 (Modules 4-7): Vertical 4-digit 24-hour clock.
   Matrix Brightness: 
     - Night (sundown to sunrise): Intensity 0 (ultra-dim).
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
// ALSO UNCOMMENT THESE AND SET APPROPRIATELY
// const char* TZ_INFO = "EST5EDT,M3.2.0,M11.1.0";
// const char* CITY_NAME       = "Boston,US"; 
// const char* LATITUDE        = "42.3601";  
// const char* LONGITUDE       = "-71.0589"; 

// Hardware Orientation configuration
const bool REVERSE_PANEL_2  = true;

// ===============================================================================
// --- Hardware Pin Configuration (ESP8266) ---
// ===============================================================================
const int PIN_DATA_IN  = D4; // MAX7219 DIN
const int PIN_CLK      = D2; // MAX7219 CLK
const int PIN_CS       = D3; // MAX7219 CS 
const int PIN_NEOPIXEL = D6; // NeoPixel DIN

#define NUM_MAX_DEVICES 8
#define NUM_PIXELS      18

#define PANEL_WIDTH     8    
#define PANEL_HEIGHT    32   

#define SCREEN2_DEVICE_OFFSET 4
#define SCREEN2_NUM_DEVICES   4

LedControl lc = LedControl(PIN_DATA_IN, PIN_CLK, PIN_CS, NUM_MAX_DEVICES);
Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ===============================================================================
// --- Font Definitions ---
// ===============================================================================
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
  // 'S'
  {0b00000000, 0b00100110, 0b01001001, 0b01001001, 0b01001001, 0b00110010, 0b00000000, 0b00000000},
  // 'Y'
  {0b00000000, 0b00000011, 0b00000100, 0b01111111, 0b00000100, 0b00000011, 0b00000000, 0b00000000},
  // 'N'
  {0b00000000, 0b01111111, 0b00000100, 0b00001000, 0b00010000, 0b01111111, 0b00000000, 0b00000000},
  // 'C'
  {0b00000000, 0b00111110, 0b01000001, 0b01000001, 0b01000001, 0b00100010, 0b00000000, 0b00000000},
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

uint8_t cm5Rows[PANEL_HEIGHT];
bool rowDirections[PANEL_HEIGHT];   
uint8_t rowLifetimes[PANEL_HEIGHT];  
uint8_t sparkleMask[PANEL_HEIGHT];
uint8_t currentGen = 0;

char timeChars[SCREEN2_NUM_DEVICES] = {'S', 'Y', 'N', 'C'};

// Time Synchronization Anchor
time_t lastKnownValidEpoch        = 0;
unsigned long lastSyncLocalMillis = 0;
bool isTimeEverSynced             = false;

// Environmental Data
int forecast8HrWeatherId          = 800;
float forecast8HrTempC            = 20.0;
int currentAQI                    = 1;   
float currentPM25                 = 0.0; 
unsigned long sunriseEpoch        = 0;
unsigned long sunsetEpoch         = 0;
bool isDaytime                    = true;
uint8_t currentMatrixIntensity    = 0; // Starts at 0 (ultra-dim)

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
void tickAutomaton();
void rollSparkle();
void renderScreen1_CM5();
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

  Serial.println(F("\n\n--- CM-5 Display Booting ---"));

  randomSeed(analogRead(A0));

  for (int dev = 0; dev < NUM_MAX_DEVICES; dev++) {
    lc.shutdown(dev, false);
    lc.setIntensity(dev, 0); // Initialize at dimmest level
    lc.clearDisplay(dev);
  }

  strip.begin();
  strip.show();

  for (int i = 0; i < NUM_PIXELS; i++) {
    pixelSpeeds[i]       = 0.40 + (float)random(15, 45) / 100.0;
    pixelOffsets[i]      = (float)random(0, 628) / 100.0;
    pixelPhaseOffsets[i] = (float)random(0, 628) / 100.0;
  }

  for (int r = 0; r < PANEL_HEIGHT; r++) {
    rowDirections[r] = (random(0, 2) == 1);
    rowLifetimes[r]  = random(4, 12);
  }

  reseed();
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

  // 4. Step Rule 90 Automaton Logic
  if (now - lastTick >= tickInterval) {
    lastTick = now;
    tickAutomaton();
  }

  // 5. Update Screen 1 Sparkles & Render
  if (now - lastSparkle >= sparkleInterval) {
    lastSparkle = now;
    rollSparkle();
    renderScreen1_CM5();
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
  }
  else if (isTimeEverSynced && lastKnownValidEpoch > 0) {
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
  uint8_t targetIntensity = 0; // Default to ultra-dim night intensity

  if (sunriseEpoch > 0 && sunsetEpoch > 0) {
    if (now >= sunriseEpoch && now < sunsetEpoch) {
      unsigned long secondsSinceSunrise = now - sunriseEpoch;

      if (secondsSinceSunrise < 3600) {
        // First hour after sunrise: Scale smoothly from 0 to 5
        targetIntensity = (uint8_t)((secondsSinceSunrise * 5) / 3600);
      } else {
        // Full daytime
        targetIntensity = 5;
      }
    } else {
      // Sundown to sunup
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
// --- Screen 1: CM-5 Cellular Automaton & Multi-Direction Shifts ---
// ===============================================================================
void reseed() {
  currentGen = 0;
  if (random(0, 3) == 0) {
    for (uint8_t i = 0; i < 2; i++) currentGen |= (1 << random(0, PANEL_WIDTH));
  } else {
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
  for (int r = PANEL_HEIGHT - 1; r > 0; r--) {
    cm5Rows[r] = cm5Rows[r - 1];
  }
  cm5Rows[0] = currentGen;
  currentGen = nextGeneration(currentGen);

  for (int r = 0; r < PANEL_HEIGHT; r++) {
    uint8_t val = cm5Rows[r];
    if (val != 0 && random(0, 10) < 6) {
      if (rowDirections[r]) {
        cm5Rows[r] = (val << 1) | (val >> 7);
      } else {
        cm5Rows[r] = (val >> 1) | (val << 7);
      }

      if (rowLifetimes[r] > 0) {
        rowLifetimes[r]--;
      } else {
        rowDirections[r] = (random(0, 2) == 1);
        rowLifetimes[r]  = random(4, 12);
      }
    }
  }

  static uint8_t genCount = 0;
  genCount++;
  if (currentGen == 0 || genCount > 40) {
    reseed();
    genCount = 0;
  }
}

void rollSparkle() {
  for (uint8_t r = 0; r < PANEL_HEIGHT; r++) sparkleMask[r] = 0;
  
  uint8_t minSparkles = 0;
  uint8_t maxSparkles = 2;

  if (currentAQI >= 5 || currentPM25 >= 150.0) {
    minSparkles = 70;
    maxSparkles = 110;
  } else if (currentAQI == 4 || currentPM25 >= 75.0) {
    minSparkles = 35;
    maxSparkles = 65;
  } else if (currentAQI == 3 || currentPM25 >= 35.0) {
    minSparkles = 3;
    maxSparkles = 6;
  } else {
    minSparkles = 0;
    maxSparkles = 2;
  }

  uint8_t sparkleCount = random(minSparkles, maxSparkles + 1);
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
void drawBigCharToDevice(int8_t glyphIdx, int logicalSlot) {
  int dev = SCREEN2_DEVICE_OFFSET + (REVERSE_PANEL_2 ? (SCREEN2_NUM_DEVICES - 1 - logicalSlot) : logicalSlot);

  for (int r = 0; r < 8; r++) {
    uint8_t rowVal = 0x00;
    if (glyphIdx >= 0) rowVal = pgm_read_byte(&(FONT_BIG_ROTATED[glyphIdx][r]));

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
    }
    else if (!isDaytime) {
      baseColor = blendHSV(43500, 255, 160, 48500, 240, 100, phase);
    }
    else if (forecast8HrWeatherId >= 200 && forecast8HrWeatherId < 300) {
      baseColor = blendHSV(39000, 255, 255, 54000, 255, 255, phase);

      if (random(0, 400) == 77) {
        baseColor = strip.ColorHSV(0, 0, 255); 
        brightness = 1.0;
      }
    }
    else if ((forecast8HrWeatherId >= 600 && forecast8HrWeatherId < 700) || 
             ((forecast8HrWeatherId >= 300 && forecast8HrWeatherId < 600) && forecast8HrTempC <= 1.0)) {
      baseColor = blendHSV(33500, 230, 255, 37500, 70, 230, phase);
    }
    else if (forecast8HrWeatherId >= 300 && forecast8HrWeatherId < 600) {
      if (phase < 0.5) {
        baseColor = blendHSV(34500, 255, 255, 43500, 255, 255, phase * 2.0);
      } else {
        baseColor = blendHSV(43500, 255, 255, 50500, 255, 255, (phase - 0.5) * 2.0);
      }
    }
    else if ((forecast8HrWeatherId >= 700 && forecast8HrWeatherId < 800) || forecast8HrWeatherId >= 803) {
      baseColor = blendHSV(38000, 25, 255, 36000, 70, 140, phase);
    }
    else if (forecast8HrWeatherId == 802) {
      if (phase < 0.5) {
        baseColor = blendHSV(3200, 255, 255, 9500, 255, 255, phase * 2.0);
      } else {
        baseColor = blendHSV(9500, 255, 255, 8000, 35, 255, (phase - 0.5) * 2.0);
      }
    }
    else {
      uint16_t hue = (uint16_t)((sec * 250.0) + (i * 3500)) % 65535;
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
// --- Environment & Forecast Ingestion ---
// ===============================================================================
void fetchEnvironmentData() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(2500);

  // 1. Current Weather Endpoint
  String currentWeatherUrl = "http://api.openweathermap.org/data/2.5/weather?q=" +
                             String(CITY_NAME) + "&units=metric&appid=" + String(OPENWEATHER_KEY);

  if (http.begin(client, currentWeatherUrl)) {
    if (http.GET() == HTTP_CODE_OK) {
      StaticJsonDocument<768> doc;
      if (!deserializeJson(doc, http.getStream())) {
        sunriseEpoch = doc["sys"]["sunrise"];
        sunsetEpoch  = doc["sys"]["sunset"];

        unsigned long serverNow = doc["dt"];
        if (serverNow > 1000000000) {
          if (time(nullptr) < 1000000000) {
            struct timeval tv = { (time_t)serverNow, 0 };
            settimeofday(&tv, nullptr);
            setenv("TZ", TZ_INFO, 1);
            tzset();
          }
          lastKnownValidEpoch = serverNow;
          lastSyncLocalMillis = millis();
          isTimeEverSynced    = true;
        }
      }
    }
    http.end();
  }

  // 2. 5-Day / 3-Hour Forecast: Strict count limit (cnt=4)
  String forecastUrl = "http://api.openweathermap.org/data/2.5/forecast?q=" +
                       String(CITY_NAME) + "&cnt=4&units=metric&appid=" + String(OPENWEATHER_KEY);

  if (http.begin(client, forecastUrl)) {
    if (http.GET() == HTTP_CODE_OK) {
      StaticJsonDocument<2048> doc;
      if (!deserializeJson(doc, http.getStream())) {
        time_t now = time(nullptr);
        unsigned long targetForecastEpoch = (now > 1000000000 ? now : lastKnownValidEpoch) + (8 * 3600);
        
        JsonArray list = doc["list"];
        unsigned long bestDiff = 0xFFFFFFFF;

        for (JsonObject entry : list) {
          unsigned long entryDt = entry["dt"];
          unsigned long diff = (entryDt > targetForecastEpoch) ? (entryDt - targetForecastEpoch) : (targetForecastEpoch - entryDt);

          if (diff < bestDiff) {
            bestDiff = diff;
            forecast8HrWeatherId = entry["weather"][0]["id"];
            forecast8HrTempC     = entry["main"]["temp"];
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
      StaticJsonDocument<768> doc;
      if (!deserializeJson(doc, http.getStream())) {
        currentAQI  = doc["list"][0]["main"]["aqi"]; 
        currentPM25 = doc["list"][0]["components"]["pm2_5"];
      }
    }
    http.end();
  }
}