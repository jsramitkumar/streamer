/**
 * ESP32-CAM UDP Streamer with MQTT Control
 *
 * Board:   AI-Thinker ESP32-CAM (or compatible)
 * Flash:   4MB – Partition: "Huge APP (3MB No OTA/1MB SPIFFS)"
 *
 * Features:
 *  - Captures JPEG frames from OV2640 camera
 *  - Sends frames over UDP to the backend server (protocol: "<camId>|<jpeg>")
 *  - Subscribes to MQTT for remote commands: flash, reboot, config
 *  - Status LED (GPIO 33 built-in) blinks on frame send
 *  - Reconnects WiFi and MQTT automatically
 *  - Deep-sleep idle (optional, comment ENABLE_DEEP_SLEEP to disable)
 *  - Watchdog timer to recover from hangs
 *
 * Dependencies (install via Arduino Library Manager):
 *   - esp32 board support (Espressif Systems)
 *   - PubSubClient  (Nick O'Leary)
 *
 * Compile settings:
 *   Board: "AI Thinker ESP32-CAM"
 *   Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
 *   CPU Frequency: 240MHz
 */

#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include "esp_task_wdt.h"

// ─── USER CONFIGURATION ────────────────────────────────────────────────────────
#define WIFI_SSID        "High Link"
#define WIFI_PASSWORD    "VMware@123"

#define BACKEND_UDP_IP   "192.168.0.51"   // IP of the machine running docker-compose
#define BACKEND_UDP_PORT 5000

#define MQTT_BROKER      "192.168.0.51"   // Same host as MQTT container
#define MQTT_PORT        1883
#define MQTT_USER        ""                // Leave empty if no auth
#define MQTT_PASS        ""

// Unique identifier for this camera (alphanumeric, no spaces)
#define CAMERA_ID        "CAM01"

// Frame interval in milliseconds (100 = ~10 fps, 33 = ~30 fps)
#define FRAME_INTERVAL_MS  100

// JPEG quality 10 (best) – 63 (worst). Lower = larger frame but sharper image.
#define JPEG_QUALITY       12

// Frame size: FRAMESIZE_QVGA (320x240), FRAMESIZE_VGA (640x480), FRAMESIZE_SVGA (800x600)
#define FRAME_SIZE         FRAMESIZE_VGA

// Watchdog timeout in seconds
#define WDT_TIMEOUT_S      15

// Flash LED GPIO (GPIO 4 on AI-Thinker)
#define FLASH_GPIO         4

// Status LED GPIO (GPIO 33 built-in, active LOW on AI-Thinker)
#define STATUS_LED_GPIO    33

// Maximum UDP payload in bytes. Fragmented packets are discarded by many routers.
// Keep below your network MTU. Frames larger than this are skipped.
#define MAX_UDP_PAYLOAD    65507

// MQTT topic prefix: streamer/<CAMERA_ID>/cmd
// Supported payloads: "flash_on", "flash_off", "reboot", "quality:<n>", "interval:<ms>"
// ───────────────────────────────────────────────────────────────────────────────

// ─── AI-THINKER PIN MAP ────────────────────────────────────────────────────────
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
// ───────────────────────────────────────────────────────────────────────────────

WiFiUDP      udp;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

String       mqttCommandTopic;
String       mqttStatusTopic;
uint32_t     frameInterval = FRAME_INTERVAL_MS;
uint8_t      jpegQuality   = JPEG_QUALITY;
bool         streaming      = true;

// ─── HELPERS ──────────────────────────────────────────────────────────────────

void blinkStatus() {
  digitalWrite(STATUS_LED_GPIO, LOW);   // LED on (active LOW)
  delayMicroseconds(5000);
  digitalWrite(STATUS_LED_GPIO, HIGH);  // LED off
}

void setFlash(bool on) {
  digitalWrite(FLASH_GPIO, on ? HIGH : LOW);
}

// ─── CAMERA INIT ──────────────────────────────────────────────────────────────

bool initCamera() {
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sscb_sda  = SIOD_GPIO_NUM;
  config.pin_sscb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_JPEG;
  config.frame_size    = FRAME_SIZE;
  config.jpeg_quality  = jpegQuality;
  config.fb_count      = psramFound() ? 2 : 1;
  config.fb_location   = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init failed: 0x%x\n", err);
    return false;
  }

  // Fine-tune sensor
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_sharpness(s, 0);
    s->set_denoise(s, 1);
    s->set_gainceiling(s, (gainceiling_t)2);
    s->set_whitebal(s, 1);      // auto white balance
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1); // auto exposure
    s->set_aec2(s, 1);
    s->set_ae_level(s, 0);
    s->set_gain_ctrl(s, 1);     // auto gain
    s->set_agc_gain(s, 0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_colorbar(s, 0);
  }
  Serial.println("[CAM] Init OK");
  return true;
}

// ─── WIFI ─────────────────────────────────────────────────────────────────────

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed – restarting");
    ESP.restart();
  }
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────

void handleMqttCommand(byte *payload, unsigned int length) {
  String cmd = "";
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.printf("[MQTT] Command: %s\n", cmd.c_str());

  if (cmd == "flash_on")   { setFlash(true); }
  else if (cmd == "flash_off") { setFlash(false); }
  else if (cmd == "reboot")    { ESP.restart(); }
  else if (cmd == "stream_on") { streaming = true; }
  else if (cmd == "stream_off"){ streaming = false; }
  else if (cmd.startsWith("quality:")) {
    int q = cmd.substring(8).toInt();
    if (q >= 10 && q <= 63) {
      jpegQuality = (uint8_t)q;
      sensor_t *s = esp_camera_sensor_get();
      if (s) s->set_quality(s, jpegQuality);
      Serial.printf("[MQTT] JPEG quality set to %d\n", jpegQuality);
    }
  } else if (cmd.startsWith("interval:")) {
    int ms = cmd.substring(9).toInt();
    if (ms >= 33 && ms <= 5000) {
      frameInterval = (uint32_t)ms;
      Serial.printf("[MQTT] Frame interval set to %d ms\n", frameInterval);
    }
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  handleMqttCommand(payload, length);
}

void connectMqtt() {
  if (mqtt.connected()) return;
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  String clientId = String("esp32cam-") + CAMERA_ID + "-" + String(random(0xffff), HEX);

  uint8_t attempts = 0;
  while (!mqtt.connected() && attempts < 5) {
    Serial.printf("[MQTT] Connecting as %s...\n", clientId.c_str());
    bool ok;
    if (strlen(MQTT_USER) > 0) {
      ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                        mqttStatusTopic.c_str(), 1, true, "offline");
    } else {
      ok = mqtt.connect(clientId.c_str(), nullptr, nullptr,
                        mqttStatusTopic.c_str(), 1, true, "offline");
    }
    if (ok) {
      mqtt.subscribe(mqttCommandTopic.c_str(), 1);
      mqtt.publish(mqttStatusTopic.c_str(), "online", true);
      Serial.println("[MQTT] Connected");
    } else {
      Serial.printf("[MQTT] Failed rc=%d – retry\n", mqtt.state());
      delay(2000);
      attempts++;
    }
  }
}

// ─── FRAME SEND ───────────────────────────────────────────────────────────────

bool sendFrame(camera_fb_t *fb) {
  if (!fb || fb->len == 0) return false;

  // Protocol: "<camId>|<raw jpeg bytes>"
  const char *cam = CAMERA_ID;
  size_t      camLen = strlen(cam);
  size_t      totalLen = camLen + 1 + fb->len;  // "camId|<jpeg>"

  if (totalLen > MAX_UDP_PAYLOAD) {
    Serial.printf("[UDP] Frame too large (%u bytes) – skipped\n", totalLen);
    return false;
  }

  // Allocate send buffer
  uint8_t *buf = (uint8_t *)malloc(totalLen);
  if (!buf) {
    Serial.println("[UDP] malloc failed");
    return false;
  }
  memcpy(buf, cam, camLen);
  buf[camLen] = '|';
  memcpy(buf + camLen + 1, fb->buf, fb->len);

  udp.beginPacket(BACKEND_UDP_IP, BACKEND_UDP_PORT);
  udp.write(buf, totalLen);
  int r = udp.endPacket();
  free(buf);

  if (!r) {
    Serial.println("[UDP] Send failed");
    return false;
  }
  return true;
}

// ─── SETUP ────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println("\n\n[BOOT] ESP32-CAM Streamer starting…");

  // GPIO init
  pinMode(FLASH_GPIO, OUTPUT);
  digitalWrite(FLASH_GPIO, LOW);
  pinMode(STATUS_LED_GPIO, OUTPUT);
  digitalWrite(STATUS_LED_GPIO, HIGH); // off

  // Watchdog
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  // MQTT topic strings
  mqttCommandTopic = String("streamer/") + CAMERA_ID + "/cmd";
  mqttStatusTopic  = String("streamer/") + CAMERA_ID + "/status";

  // Init peripherals
  connectWiFi();
  if (!initCamera()) {
    Serial.println("[BOOT] Camera init failed – halting");
    while (true) { delay(1000); }
  }
  connectMqtt();

  udp.begin(WiFi.localIP(), 0);  // bind to a random local port

  Serial.println("[BOOT] Ready. Streaming…");
}

// ─── LOOP ─────────────────────────────────────────────────────────────────────

void loop() {
  esp_task_wdt_reset();

  // Maintain connectivity
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected – reconnecting");
    connectWiFi();
  }
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  if (!streaming) {
    delay(200);
    return;
  }

  unsigned long start = millis();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CAM] Capture failed");
    delay(100);
    return;
  }

  bool sent = sendFrame(fb);
  esp_camera_fb_return(fb);

  if (sent) blinkStatus();

  // Maintain target frame rate
  long elapsed = (long)(millis() - start);
  long wait    = (long)frameInterval - elapsed;
  if (wait > 0) delay((uint32_t)wait);
}
