// Copy this file to "secrets.h" (same folder) and fill in your settings.
// secrets.h is ignored by git, so your passwords never end up on GitHub.
#pragma once

// ── Wi-Fi ───────────────────────────────────────────────────────────────────
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ── MQTT broker (e.g. Mosquitto on the PC/Raspberry Pi that runs Node-RED) ──
// Leave MQTT_HOST empty ("") to disable MQTT and use the web dashboard only.
#define MQTT_HOST     "192.168.1.10"
#define MQTT_PORT     1883
#define MQTT_USER     ""   // empty = broker without authentication
#define MQTT_PASSWORD ""
