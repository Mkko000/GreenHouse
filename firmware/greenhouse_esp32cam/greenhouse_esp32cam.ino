// ============================================================================
// Smart Greenhouse — ESP32-CAM (AI-Thinker) Wi-Fi + MQTT gateway
// Author : Mohammed El-SiSi
//
// Receives status frames from the Arduino UNO over UART and exposes them two ways:
//   • a built-in web dashboard (live readings, controls, camera snapshot)
//   • MQTT — state is published as JSON, commands are accepted on a topic,
//     so Node-RED / Home Assistant / any MQTT client can monitor and control it.
// Commands from either side are checked against a whitelist and forwarded to the UNO.
//
// Board     : "AI Thinker ESP32-CAM" (esp32 by Espressif, Boards Manager)
// Libraries : PubSubClient (Nick O'Leary) — everything else ships with the ESP32 core
// Setup     : copy secrets.example.h to secrets.h and fill in Wi-Fi + MQTT broker.
//
// HTTP endpoints
//   GET /                 dashboard page
//   GET /status           JSON with the latest readings
//   GET /ctrl?cmd=PUMP_ON command for the Arduino (PUMP_/FAN_/LED_ ON/OFF, AUTO)
//   GET /capture          one JPEG frame from the camera
//
// MQTT topics (TOPIC_BASE = "greenhouse")
//   greenhouse/state         JSON state, retained, every 2 s     (ESP32 → broker)
//   greenhouse/availability  "online" / "offline" (last will), retained
//   greenhouse/cmd           PUMP_ON, FAN_OFF, AUTO, ...         (broker → ESP32)
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include "esp_camera.h"
#include "secrets.h"         // WIFI_SSID, WIFI_PASSWORD, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD
#include "dashboard_html.h"  // DASHBOARD_HTML

// UART0 (U0R = GPIO3, U0T = GPIO1) is wired to the Arduino at 9600 baud.
// The same port prints the dashboard address at boot; the Arduino ignores
// any line that is not a known command.
const unsigned long LINK_BAUD = 9600;
const unsigned long ARDUINO_TIMEOUT_MS = 10000;  // no frame for 10 s → "offline"

// ── MQTT ────────────────────────────────────────────────────────────────────
const char* const TOPIC_STATE        = "greenhouse/state";
const char* const TOPIC_AVAILABILITY = "greenhouse/availability";
const char* const TOPIC_COMMAND      = "greenhouse/cmd";
const unsigned long MQTT_RETRY_MS = 5000;  // reconnect attempt interval (non-blocking)

const char* const ALLOWED_COMMANDS[] = { "PUMP_ON", "PUMP_OFF", "FAN_ON", "FAN_OFF", "LED_ON", "LED_OFF", "AUTO" };

WebServer server(80);
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
bool cameraReady = false;
bool mqttEnabled = false;
unsigned long lastMqttAttemptMs = 0;
bool lastPublishedOnline = false;
String mqttClientId;

struct GreenhouseStatus {
  float temperature = NAN;
  float humidity = NAN;
  int soil = 0;      // %
  int light = 0;     // raw ADC 0..1023
  bool fan = false;
  bool pump = false;
  bool leds = false;
  bool autoMode = true;
  unsigned long lastFrameMs = 0;
  bool received = false;
} status;

String rxLine;

// ── Camera (AI-Thinker pin-out) ─────────────────────────────────────────────
bool setupCamera() {
  camera_config_t cfg = {};  // zero every field, including ones newer core versions added
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = 5;   cfg.pin_d1 = 18;  cfg.pin_d2 = 19;  cfg.pin_d3 = 21;
  cfg.pin_d4 = 36;  cfg.pin_d5 = 39;  cfg.pin_d6 = 34;  cfg.pin_d7 = 35;
  cfg.pin_xclk = 0;
  cfg.pin_pclk = 22;
  cfg.pin_vsync = 25;
  cfg.pin_href = 23;
  cfg.pin_sccb_sda = 26;
  cfg.pin_sccb_scl = 27;
  cfg.pin_pwdn = 32;
  cfg.pin_reset = -1;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size = FRAMESIZE_VGA;
  cfg.jpeg_quality = 12;
  cfg.fb_count = 1;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode = CAMERA_GRAB_LATEST;  // always serve the newest frame
  return esp_camera_init(&cfg) == ESP_OK;
}

// ── Commands (web dashboard and MQTT share the same whitelist) ──────────────
bool forwardCommand(const String& cmd) {
  for (const char* allowed : ALLOWED_COMMANDS) {
    if (cmd == allowed) {
      Serial.println(cmd);
      return true;
    }
  }
  return false;
}

// ── State as JSON (served on /status and published on MQTT) ─────────────────
bool arduinoOnline() {
  return status.received && millis() - status.lastFrameMs < ARDUINO_TIMEOUT_MS;
}

String jsonNumber(float v, int decimals) {
  return isnan(v) ? String("null") : String(v, decimals);
}

String buildStateJson() {
  String json = "{";
  json += "\"online\":" + String(arduinoOnline() ? "true" : "false");
  json += ",\"temperature\":" + jsonNumber(status.temperature, 1);
  json += ",\"humidity\":" + jsonNumber(status.humidity, 1);
  json += ",\"soil\":" + String(status.soil);
  json += ",\"light\":" + String(status.light);
  json += ",\"fan\":" + String(status.fan ? "true" : "false");
  json += ",\"pump\":" + String(status.pump ? "true" : "false");
  json += ",\"leds\":" + String(status.leds ? "true" : "false");
  json += ",\"auto\":" + String(status.autoMode ? "true" : "false");
  json += ",\"camera\":" + String(cameraReady ? "true" : "false");
  json += "}";
  return json;
}

// ── MQTT ────────────────────────────────────────────────────────────────────
void publishState() {
  if (!mqttEnabled || !mqtt.connected()) return;
  String json = buildStateJson();
  mqtt.publish(TOPIC_STATE, json.c_str(), true);  // retained: new subscribers get the last state at once
  lastPublishedOnline = arduinoOnline();
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_COMMAND) != 0 || length > 16) return;
  String cmd;
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  cmd.trim();
  forwardCommand(cmd);  // unknown commands are silently ignored
}

// Non-blocking: one connection attempt every MQTT_RETRY_MS, the loop keeps running meanwhile.
void maintainMqtt(unsigned long now) {
  if (!mqttEnabled) return;
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED || now - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = now;

  const char* user = strlen(MQTT_USER) > 0 ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_PASSWORD) > 0 ? MQTT_PASSWORD : nullptr;
  // Last will: the broker marks us "offline" if the ESP32 drops off the network.
  if (mqtt.connect(mqttClientId.c_str(), user, pass, TOPIC_AVAILABILITY, 1, true, "offline")) {
    mqtt.publish(TOPIC_AVAILABILITY, "online", true);
    mqtt.subscribe(TOPIC_COMMAND);
    publishState();
  }
}

// ── UART frames from the Arduino ────────────────────────────────────────────
// T:25.3,H:61.0,S:42,L:512,F:1,P:0,E:1,M:A
bool parseFrame(const String& line) {
  int start = 0;
  while (start < (int)line.length()) {
    int end = line.indexOf(',', start);
    if (end < 0) end = line.length();
    String field = line.substring(start, end);
    start = end + 1;

    int colon = field.indexOf(':');
    if (colon != 1) continue;
    char key = field.charAt(0);
    String value = field.substring(2);

    switch (key) {
      case 'T': status.temperature = value.startsWith("nan") ? NAN : value.toFloat(); break;
      case 'H': status.humidity = value.startsWith("nan") ? NAN : value.toFloat();    break;
      case 'S': status.soil = value.toInt(); break;
      case 'L': status.light = value.toInt(); break;
      case 'F': status.fan = value == "1"; break;
      case 'P': status.pump = value == "1"; break;
      case 'E': status.leds = value == "1"; break;
      case 'M': status.autoMode = value == "A"; break;
      default: return false;  // not one of our frames
    }
  }
  status.lastFrameMs = millis();
  status.received = true;
  return true;
}

void readLink() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (rxLine.startsWith("T:") && parseFrame(rxLine)) publishState();
      rxLine = "";
    } else if (rxLine.length() < 96) {
      rxLine += c;
    } else {
      rxLine = "";
    }
  }
}

// ── HTTP handlers ───────────────────────────────────────────────────────────
void handleStatus() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buildStateJson());
}

void handleControl() {
  if (forwardCommand(server.arg("cmd"))) server.send(200, "text/plain", "OK");
  else server.send(400, "text/plain", "Unknown command");
}

void handleCapture() {
  if (!cameraReady) { server.send(503, "text/plain", "Camera not available"); return; }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { server.send(500, "text/plain", "Capture failed"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

// ============================================================================
void setup() {
  Serial.begin(LINK_BAUD);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // keeps the dashboard, camera and MQTT responsive
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  cameraReady = setupCamera();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/ctrl", handleControl);
  server.on("/capture", handleCapture);
  server.begin();

  // MQTT is optional: leave MQTT_HOST empty in secrets.h to run with the web dashboard only.
  mqttEnabled = strlen(MQTT_HOST) > 0;
  if (mqttEnabled) {
    uint64_t mac = ESP.getEfuseMac();
    mqttClientId = "greenhouse-" + String((uint32_t)(mac & 0xFFFFFF), HEX);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(512);   // the state JSON is larger than the 256-byte default
    mqtt.setSocketTimeout(3);  // don't stall the dashboard if the broker is down
  }

  // Open this address in a browser on the same Wi-Fi network.
  Serial.print("DASHBOARD http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  unsigned long now = millis();
  server.handleClient();
  readLink();
  maintainMqtt(now);

  // Tell MQTT subscribers when the UNO stops (or resumes) sending frames.
  if (mqtt.connected() && arduinoOnline() != lastPublishedOnline) publishState();
}
