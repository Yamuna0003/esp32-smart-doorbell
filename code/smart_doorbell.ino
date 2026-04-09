// ESP32 Cam Smart Doorbell + Motion Alert | Send Image to Telegram

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"

// ===================== CONFIGURATION =====================
const char* ssid     = " ";
const char* password = " ";
String token   = " ";
String chat_id = " ";

// ===================== CAMERA PINS (AI Thinker) =====================
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

#define FLASH_LED    4
#define PIR_PIN     13
#define BUTTON_PIN  14   // ← NEW: Doorbell button

// ===================== DEBOUNCE CONFIG =====================
#define DEBOUNCE_MS     50    // ms to wait after first edge
#define COOLDOWN_MS   8000   // ms before button can trigger again

static unsigned long lastButtonTime = 0;
static bool          buttonArmed    = true;

// ===================== FLASH BLINK HELPER =====================
void blinkFlash(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(FLASH_LED, HIGH);
    delay(150);
    digitalWrite(FLASH_LED, LOW);
    delay(150);
  }
}

// ===================== CAMERA INIT =====================
bool initCamera() {
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(200);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(200);

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
  config.pin_reset      = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 10000000;
  config.pixel_format  = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    Serial.println("PSRAM found — VGA mode");
  } else {
    config.frame_size   = FRAMESIZE_QQVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    Serial.println("No PSRAM — QQVGA mode");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("Cannot get sensor handle!");
    return false;
  }
  Serial.printf("Sensor PID: 0x%x\n", s->id.PID);

  if (psramFound()) s->set_framesize(s, FRAMESIZE_XGA);

  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_gain_ctrl(s, 1);

  Serial.print("Warming up");
  for (int i = 0; i < 5; i++) {
    camera_fb_t* f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
    delay(150);
    Serial.print(".");
  }
  Serial.println(" done");
  return true;
}

// ===================== CAPTURE WITH RETRY =====================
camera_fb_t* captureFrame() {
  digitalWrite(FLASH_LED, HIGH);
  delay(300);

  for (int i = 0; i < 2; i++) {
    camera_fb_t* stale = esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);
    delay(100);
  }

  digitalWrite(FLASH_LED, LOW);
  delay(50);

  for (int attempt = 1; attempt <= 5; attempt++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb && fb->len > 5000) {
      Serial.printf("Captured on attempt %d — %d bytes\n", attempt, fb->len);
      return fb;
    }
    if (fb) {
      Serial.printf("Attempt %d: too small (%d bytes)\n", attempt, fb->len);
      esp_camera_fb_return(fb);
    } else {
      Serial.printf("Attempt %d: fb_get returned null\n", attempt);
    }
    delay(300);
  }
  return nullptr;
}

// ===================== SEND PHOTO TO TELEGRAM =====================
// caption lets us pass a custom message ("🔔 Doorbell!" vs "🚨 Motion!")
String sendPhoto(String caption) {
  String getBody = "";

  camera_fb_t* fb = captureFrame();
  if (!fb) {
    Serial.println("All capture attempts failed");
    return "Camera capture failed";
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  const char* myDomain = "api.telegram.org";
  Serial.println("Connecting to Telegram...");

  if (!client.connect(myDomain, 443)) {
    esp_camera_fb_return(fb);
    Serial.println("Connection to Telegram failed");
    return "Connection failed";
  }
  Serial.println("Connected!");

  String boundary = "ESP32CAMBoundary";

  // Build multipart body with caption field
  String head =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
    chat_id + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
    caption + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"photo\"; filename=\"esp32-cam.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

  size_t imageLen = fb->len;
  size_t extraLen = head.length() + tail.length();
  size_t totalLen = imageLen + extraLen;

  client.println("POST /bot" + token + "/sendPhoto HTTP/1.1");
  client.println("Host: " + String(myDomain));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println("Content-Length: " + String(totalLen));
  client.println("Connection: close");
  client.println();
  client.print(head);

  uint8_t* fbBuf = fb->buf;
  size_t   fbLen = fb->len;
  for (size_t n = 0; n < fbLen; n += 1024) {
    size_t toWrite = min((size_t)1024, fbLen - n);
    client.write(fbBuf + n, toWrite);
  }

  client.print(tail);
  esp_camera_fb_return(fb);
  Serial.println("Image sent, waiting for response...");

  String getAll    = "";
  long   startTime = millis();
  bool   state     = false;

  while (millis() - startTime < 10000) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') {
        if (getAll.length() == 0) state = true;
        getAll = "";
      } else if (c != '\r') {
        getAll += c;
      }
      if (state) getBody += c;
      startTime = millis();
    }
    if (getBody.length() > 0) break;
    delay(50);
  }

  client.stop();
  Serial.println("Response: " + getBody.substring(0, 200));

  if (getBody.indexOf("\"ok\":true") >= 0) {
    Serial.println("Photo sent successfully!");
  } else {
    Serial.println("Photo send FAILED");
  }

  return getBody;
}

// ===================== DOORBELL HANDLER =====================
void handleDoorbell() {
  Serial.println("🔔 Doorbell pressed!");
  blinkFlash(3);   // 3 quick blinks = doorbell acknowledged

  // Send a Telegram text alert first (fast)
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);

  if (client.connect("api.telegram.org", 443)) {
    String msg    = "🔔 Someone is at the door!";
    String urlEnc = "";
    // Simple URL-encode the emoji message
    for (int i = 0; i < msg.length(); i++) {
      char c = msg[i];
      if (isAlphaNumeric(c) || c == ' ' || c == '!' || c == '.') {
        urlEnc += c;
      } else {
        char buf[4];
        sprintf(buf, "%%%02X", (unsigned char)c);
        urlEnc += buf;
      }
    }

    String path = "/bot" + token +
                  "/sendMessage?chat_id=" + chat_id +
                  "&text=" + urlEnc;

    client.println("GET " + path + " HTTP/1.1");
    client.println("Host: api.telegram.org");
    client.println("Connection: close");
    client.println();
    delay(500);
    client.stop();
    Serial.println("Text alert sent");
  }

  // Then send the photo with doorbell caption
  sendPhoto("🔔 Doorbell! Someone is at your door.");
}

// ===================== SETUP =====================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-CAM Smart Doorbell Starting ===");

  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);
  pinMode(PIR_PIN,    INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // ← NEW: internal pull-up, no resistor needed

  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - startTime > 15000) break;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed — restarting");
    blinkFlash(1);
    delay(1000);
    ESP.restart();
  }

  Serial.println("WiFi connected! IP: " + WiFi.localIP().toString());
  Serial.println("Signal: " + String(WiFi.RSSI()) + " dBm");
  blinkFlash(5);

  if (!initCamera()) {
    Serial.println("FATAL: Camera init failed — restarting");
    delay(3000);
    ESP.restart();
  }

  Serial.println("=== Smart Doorbell Ready ===");
}

// ===================== LOOP =====================
void loop() {
  unsigned long now = millis();

  // ── DOORBELL BUTTON (active LOW with INPUT_PULLUP) ──────────────────
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (buttonArmed && (now - lastButtonTime > DEBOUNCE_MS)) {
      lastButtonTime = now;
      buttonArmed    = false;       // disarm until cooldown expires
      handleDoorbell();
    }
  }

  // Re-arm button after cooldown
  if (!buttonArmed && (now - lastButtonTime > COOLDOWN_MS)) {
    buttonArmed = true;
    Serial.println("Doorbell re-armed.");
  }

  // ── PIR MOTION SENSOR ───────────────────────────────────────────────
  if (digitalRead(PIR_PIN) == HIGH) {
    Serial.println("Motion detected! Sending photo...");
    sendPhoto("🚨 Motion detected near your door!");

    Serial.print("Waiting for PIR to clear");
    unsigned long waitStart = millis();
    while (digitalRead(PIR_PIN) == HIGH && millis() - waitStart < 30000) {
      delay(200);
      Serial.print(".");
    }
    Serial.println();

    delay(5000);
    Serial.println("PIR re-armed.");
  }

  delay(50);   // tight loop for responsive button feel
}
