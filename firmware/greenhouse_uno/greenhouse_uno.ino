// ============================================================================
// Smart Greenhouse — Arduino UNO controller
// Author : Mohammed El-SiSi
//
// Reads the DHT22 (temperature/humidity), a soil-moisture probe and an LDR,
// drives the fans, water pump and RGB LED strip, shows live values on a
// 16x2 I2C LCD, takes commands from a 4x4 keypad, and talks to the ESP32-CAM
// over UART: it sends a status frame every 2 s and executes the commands the
// web dashboard sends back.
//
// Pin map
//   DHT22 = D2            Soil = A0           LDR = A1
//   Keypad rows = D3..D6  Keypad cols = D7..D10
//   Fans MOSFET = D11     Pump relay = D12
//   LED R = D13           LED G = A2          LED B = A3
//   LCD I2C = A4 (SDA), A5 (SCL)
//   UART to ESP32-CAM = D1 (TX, through a 1k/2k divider), D0 (RX)
//
// Libraries: "DHT sensor library" (Adafruit), "LiquidCrystal I2C", "Keypad"
// (Mark Stanley). EEPROM ships with the Arduino core.
// ============================================================================

#include <EEPROM.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// ── Pins ────────────────────────────────────────────────────────────────────
const uint8_t PIN_DHT   = 2;
const uint8_t PIN_SOIL  = A0;
const uint8_t PIN_LDR   = A1;
const uint8_t PIN_FAN   = 11;
const uint8_t PIN_PUMP  = 12;
const uint8_t PIN_LED_R = 13;
const uint8_t PIN_LED_G = A2;
const uint8_t PIN_LED_B = A3;

// ── Hardware options ────────────────────────────────────────────────────────
const uint8_t LCD_ADDRESS      = 0x27;  // try 0x3F if the screen stays blank
const bool    RELAY_ACTIVE_LOW = false; // set true for relay boards that switch ON when IN is LOW

// ── Soil-probe calibration (raw ADC) ───────────────────────────────────────
// Resistive and capacitive probes read HIGH when dry and LOW when wet.
// Note the reading in dry air and in a glass of water and put them here.
const int SOIL_RAW_DRY = 1000;
const int SOIL_RAW_WET = 350;

// ── Control tuning ─────────────────────────────────────────────────────────
const float         TEMP_HYSTERESIS  = 1.0;      // °C below the limit before the fans stop
const int           SOIL_HYSTERESIS  = 5;        // % above the limit before the pump stops
const int           LIGHT_HYSTERESIS = 30;       // ADC counts above the limit before the LEDs go off
const unsigned long PUMP_MAX_ON_MS   = 30000UL;  // safety: never water longer than this in one go
const unsigned long PUMP_REST_MS     = 60000UL;  // then let the water soak in before re-checking
const unsigned long SENSOR_PERIOD_MS = 2000UL;
const unsigned long MESSAGE_MS       = 1500UL;

// ── Thresholds (editable from the keypad, kept in EEPROM) ─────────────────
struct Settings {
  uint8_t magic;
  float   tempMax;   // °C   — fans ON above this
  uint8_t soilMin;   // %    — pump ON below this
  int     lightMin;  // ADC  — LEDs ON below this
};
const uint8_t SETTINGS_MAGIC = 0x5A;
Settings settings = { SETTINGS_MAGIC, 30.0, 40, 300 };

// ── Peripherals ─────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
DHT dht(PIN_DHT, DHT22);

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte rowPins[ROWS] = { 3, 4, 5, 6 };
byte colPins[COLS] = { 7, 8, 9, 10 };
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ── State ───────────────────────────────────────────────────────────────────
bool autoMode = true;
bool fanOn = false, pumpOn = false, ledOn = false;
bool pumpResting = false;

float temperature = NAN, humidity = NAN;
int soilPercent = 0, lightLevel = 0;

unsigned long lastSensorMs = 0;
unsigned long pumpStartedMs = 0;
unsigned long pumpRestStartedMs = 0;
unsigned long messageStartedMs = 0;
bool messageShown = false;
bool displayDirty = true;

// Fixed-size buffer instead of String: no heap fragmentation on the 2 KB UNO.
char rxBuffer[24];
uint8_t rxLength = 0;

// Keypad threshold editor: '#' starts it, digits + '#' confirm each field, '*' cancels.
enum EditField : uint8_t { EDIT_NONE, EDIT_TEMP, EDIT_SOIL, EDIT_LIGHT };
EditField editField = EDIT_NONE;
char editBuffer[5];
uint8_t editLength = 0;

// ============================================================================
// Outputs
// ============================================================================
void setFan(bool on) {
  fanOn = on;
  digitalWrite(PIN_FAN, on ? HIGH : LOW);
}

void setPump(bool on) {
  if (on && !pumpOn) pumpStartedMs = millis();
  pumpOn = on;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_PUMP, level ? HIGH : LOW);
}

void setLeds(bool on) {
  ledOn = on;
  digitalWrite(PIN_LED_R, on ? HIGH : LOW);
  digitalWrite(PIN_LED_G, on ? HIGH : LOW);
  digitalWrite(PIN_LED_B, on ? HIGH : LOW);
}

// ============================================================================
// Settings
// ============================================================================
void loadSettings() {
  Settings stored;
  EEPROM.get(0, stored);
  bool valid = stored.magic == SETTINGS_MAGIC
               && stored.tempMax >= 10 && stored.tempMax <= 50
               && stored.soilMin <= 100
               && stored.lightMin >= 0 && stored.lightMin <= 1023;
  if (valid) settings = stored;
}

void saveSettings() {
  EEPROM.put(0, settings);  // put() only rewrites bytes that changed
}

// ============================================================================
// LCD helpers
// ============================================================================
void printLine(uint8_t row, const char* text) {
  lcd.setCursor(0, row);
  uint8_t n = 0;
  for (; text[n] != '\0' && n < 16; n++) lcd.print(text[n]);
  for (; n < 16; n++) lcd.print(' ');  // clear leftovers from a longer previous line
}

void showMessage(const char* line1, const char* line2) {
  printLine(0, line1);
  printLine(1, line2);
  messageShown = true;
  messageStartedMs = millis();
}

void showStatus() {
  char line[17];
  char t[7], h[5];

  if (isnan(temperature) || isnan(humidity)) {
    snprintf(line, sizeof line, "DHT22 error!");
  } else {
    dtostrf(temperature, 4, 1, t);
    dtostrf(humidity, 3, 0, h);
    snprintf(line, sizeof line, "T:%sC H:%s%%", t, h);
  }
  printLine(0, line);

  snprintf(line, sizeof line, "S:%d%% L:%d %s", soilPercent, lightLevel, autoMode ? "[A]" : "[M]");
  printLine(1, line);
}

void showThresholds() {
  char line[17], t[5];
  dtostrf(settings.tempMax, 2, 0, t);
  snprintf(line, sizeof line, "T>%sC S<%u%%", t, settings.soilMin);
  char line2[17];
  snprintf(line2, sizeof line2, "L<%d %s", settings.lightMin, autoMode ? "AUTO" : "MANUAL");
  showMessage(line, line2);
}

void showEditor(const char* error) {
  const char* title = error != nullptr         ? error
                    : editField == EDIT_TEMP ? "Fan above (C):"
                    : editField == EDIT_SOIL ? "Water below (%):"
                                             : "Lights below:";
  char line[17];
  snprintf(line, sizeof line, "> %s_", editBuffer);
  printLine(0, title);
  printLine(1, line);
}

// ============================================================================
// Sensors & automatic control
// ============================================================================
void readSensors() {
  temperature = dht.readTemperature();
  humidity = dht.readHumidity();

  int soilRaw = analogRead(PIN_SOIL);
  soilPercent = constrain(map(soilRaw, SOIL_RAW_DRY, SOIL_RAW_WET, 0, 100), 0, 100);

  // LDR on top of a 10k pull-down: more light → higher reading.
  lightLevel = analogRead(PIN_LDR);
}

void autoControl(unsigned long now) {
  // Fans — with hysteresis so they don't flicker around the limit.
  if (!isnan(temperature)) {
    if (!fanOn && temperature > settings.tempMax) setFan(true);
    else if (fanOn && temperature < settings.tempMax - TEMP_HYSTERESIS) setFan(false);
  }

  // Pump — stops once the soil is wet enough; rests after a long run (see pumpSafety()).
  if (pumpResting && now - pumpRestStartedMs >= PUMP_REST_MS) pumpResting = false;
  if (!pumpOn && !pumpResting && soilPercent < settings.soilMin) setPump(true);
  else if (pumpOn && soilPercent >= settings.soilMin + SOIL_HYSTERESIS) setPump(false);

  // Grow lights.
  if (!ledOn && lightLevel < settings.lightMin) setLeds(true);
  else if (ledOn && lightLevel > settings.lightMin + LIGHT_HYSTERESIS) setLeds(false);
}

// Applies in AUTO and MANUAL mode: a broken probe or a forgotten manual
// command can never flood the greenhouse.
void pumpSafety(unsigned long now) {
  if (pumpOn && now - pumpStartedMs >= PUMP_MAX_ON_MS) {
    setPump(false);
    pumpResting = true;
    pumpRestStartedMs = now;
  }
}

// ============================================================================
// UART link with the ESP32-CAM
// ============================================================================
// Frame: T:25.3,H:61.0,S:42,L:512,F:1,P:0,E:1,M:A   (E = LEDs, M = A/M mode)
void sendStatus() {
  Serial.print(F("T:")); Serial.print(temperature, 1);
  Serial.print(F(",H:")); Serial.print(humidity, 1);
  Serial.print(F(",S:")); Serial.print(soilPercent);
  Serial.print(F(",L:")); Serial.print(lightLevel);
  Serial.print(F(",F:")); Serial.print(fanOn ? 1 : 0);
  Serial.print(F(",P:")); Serial.print(pumpOn ? 1 : 0);
  Serial.print(F(",E:")); Serial.print(ledOn ? 1 : 0);
  Serial.print(F(",M:")); Serial.println(autoMode ? 'A' : 'M');
}

void processCommand(const char* cmd) {
  if      (strcmp(cmd, "PUMP_ON") == 0)  { autoMode = false; setPump(true); }
  else if (strcmp(cmd, "PUMP_OFF") == 0) { autoMode = false; setPump(false); }
  else if (strcmp(cmd, "FAN_ON") == 0)   { autoMode = false; setFan(true); }
  else if (strcmp(cmd, "FAN_OFF") == 0)  { autoMode = false; setFan(false); }
  else if (strcmp(cmd, "LED_ON") == 0)   { autoMode = false; setLeds(true); }
  else if (strcmp(cmd, "LED_OFF") == 0)  { autoMode = false; setLeds(false); }
  else if (strcmp(cmd, "AUTO") == 0)     { autoMode = true; }
  else return;  // ignore anything else (ESP32 boot log, IP address line...)

  sendStatus();  // let the dashboard see the change right away
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxBuffer[rxLength] = '\0';
      processCommand(rxBuffer);
      rxLength = 0;
    } else if (rxLength < sizeof rxBuffer - 1) {
      rxBuffer[rxLength++] = c;
    } else {
      rxLength = 0;  // line too long — not a command, drop it
    }
  }
}

// ============================================================================
// Keypad
// ============================================================================
void startEditing(EditField field) {
  editField = field;
  editLength = 0;
  editBuffer[0] = '\0';
  if (field == EDIT_NONE) { messageShown = false; displayDirty = true; }
  else showEditor(nullptr);
}

// Stores the typed value (empty = keep the current one). Returns an error text if out of range.
const char* commitEdit() {
  if (editLength > 0) {
    int value = atoi(editBuffer);
    switch (editField) {
      case EDIT_TEMP:
        if (value < 10 || value > 50) return "Range: 10-50 C";
        settings.tempMax = value;
        break;
      case EDIT_SOIL:
        if (value > 100) return "Range: 0-100 %";
        settings.soilMin = value;
        break;
      case EDIT_LIGHT:
        if (value > 1023) return "Range: 0-1023";
        settings.lightMin = value;
        break;
      default:
        break;
    }
  }
  return nullptr;
}

void handleEditorKey(char key) {
  if (key >= '0' && key <= '9') {
    if (editLength < sizeof editBuffer - 1) {
      editBuffer[editLength++] = key;
      editBuffer[editLength] = '\0';
    }
    showEditor(nullptr);
  } else if (key == '#') {
    const char* error = commitEdit();
    if (error != nullptr) {
      editLength = 0;
      editBuffer[0] = '\0';
      showEditor(error);  // the range stays on screen until a valid value is typed
      return;
    }
    if (editField == EDIT_TEMP) startEditing(EDIT_SOIL);
    else if (editField == EDIT_SOIL) startEditing(EDIT_LIGHT);
    else {
      editField = EDIT_NONE;
      saveSettings();
      showMessage("Settings saved", "");
    }
  } else if (key == '*') {
    loadSettings();  // drop unsaved changes
    editField = EDIT_NONE;
    showMessage("Edit cancelled", "");
  }
}

void handleKey(char key) {
  if (editField != EDIT_NONE) { handleEditorKey(key); return; }

  switch (key) {
    case 'A': autoMode = false; setPump(!pumpOn); showMessage(pumpOn ? "PUMP ON" : "PUMP OFF", "Manual mode"); break;
    case 'B': autoMode = false; setFan(!fanOn);   showMessage(fanOn ? "FAN ON" : "FAN OFF", "Manual mode");    break;
    case 'C': autoMode = false; setLeds(!ledOn);  showMessage(ledOn ? "LED ON" : "LED OFF", "Manual mode");    break;
    case 'D': showThresholds(); break;
    case '*': autoMode = true; showMessage("AUTO MODE ON", ""); break;
    case '#': startEditing(EDIT_TEMP); return;
    default: return;
  }
  sendStatus();
}

// ============================================================================
void setup() {
  Serial.begin(9600);
  dht.begin();
  loadSettings();

  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setFan(false);
  setPump(false);
  setLeds(false);

  lcd.init();
  lcd.backlight();
  showMessage("Smart Greenhouse", "Starting...");
}

// Non-blocking loop: the keypad and UART are polled every pass, the sensors
// every 2 s — no delay(), so key presses and dashboard commands are never missed.
void loop() {
  unsigned long now = millis();

  handleSerial();

  char key = keypad.getKey();
  if (key) handleKey(key);

  pumpSafety(now);

  if (now - lastSensorMs >= SENSOR_PERIOD_MS) {
    lastSensorMs = now;
    readSensors();
    if (autoMode) autoControl(now);
    sendStatus();
    displayDirty = true;
  }

  if (messageShown && now - messageStartedMs >= MESSAGE_MS) {
    messageShown = false;
    displayDirty = true;
  }
  if (displayDirty && !messageShown && editField == EDIT_NONE) {
    showStatus();
    displayDirty = false;
  }
}
