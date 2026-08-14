#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

#include "board_config.h"
#include "config.h"

// ----- 웹서버 --------
void startCameraServer();  // 추가
void setupLedFlash();      // 추가

// ── WiFi / 서버 설정 ──────────────────────────
const char* SERVER_IP = "";  // connectWiFi()에서 자동 설정
const int   SERVER_PORT = 8080;

// ── 스캔 파라미터 ─────────────────────────────
// 상황에 맞춰 수정 필요
long  USER_ID   = 3;
long  DEVICE_ID = 1;
const char* IMAGE_TYPE = "WHITE_LIGHT"; // domain.scan.entity 참고
const char* REGION     = "OUTER_CENTER"; // domain.scan.entity 참고

// ── LED 핀 ────────────────────────────────────
#define LED_PIN 19

// ─────────────────────────────────────────────
void connectWiFi() {
  const char* ssids[]     = { WIFI_SSID_1,     WIFI_SSID_2     };
  const char* passwords[] = { WIFI_PASSWORD_1, WIFI_PASSWORD_2 };
  const char* ips[]       = { SERVER_IP_1,     SERVER_IP_2     };

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (int i = 0; i < 2; i++) {
    WiFi.begin(ssids[i], passwords[i]);
    Serial.printf("Trying %s ...\n", ssids[i]);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500);
      Serial.print(".");
      retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      SERVER_IP = ips[i];
      Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
      Serial.printf("Server IP: %s\n", SERVER_IP);
      return;
    }
    WiFi.disconnect();
    delay(500);
  }
  Serial.println("\nWiFi connect failed");
}

void uploadImage(camera_fb_t* fb) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected");
    return;
  }

  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + SERVER_PORT + "/api/scan/upload";
  http.begin(url);
  http.setTimeout(15000);

  String boundary = "ESP32Boundary";
  String bodyHead = "";

  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"userId\"\r\n\r\n";
  bodyHead += String(USER_ID) + "\r\n";

  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"deviceId\"\r\n\r\n";
  bodyHead += String(DEVICE_ID) + "\r\n";

  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"lightType\"\r\n\r\n";
  bodyHead += String(IMAGE_TYPE) + "\r\n";

  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"viewType\"\r\n\r\n";
  bodyHead += String(REGION) + "\r\n";

  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"file\"; filename=\"scan.jpg\"\r\n";
  bodyHead += "Content-Type: image/jpeg\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

  // 전체 body를 하나의 버퍼로 조립
  int totalLen = bodyHead.length() + fb->len + tail.length();
  uint8_t* buf = (uint8_t*)malloc(totalLen);
  if (!buf) {
    Serial.println("[ERR] malloc failed");
    http.end();
    return;
  }

  memcpy(buf, bodyHead.c_str(), bodyHead.length());
  memcpy(buf + bodyHead.length(), fb->buf, fb->len);
  memcpy(buf + bodyHead.length() + fb->len, tail.c_str(), tail.length());

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  int httpCode = http.POST(buf, totalLen);
  free(buf);

  Serial.printf("[HTTP] %d\n", httpCode);
  if (httpCode == 200 || httpCode == 201) {
    Serial.println("[OK] " + http.getString());
  } else {
    Serial.println("[ERR] " + http.getString());
  }

  http.end();
}

// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  // ── LED 켜기 ─────────────────────────────────
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // ── 카메라 설정 ──────────────────────────────
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size  = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return;
  }

  // 센서 보정 (기존 그대로 유지)
  sensor_t* s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  s->set_framesize(s, FRAMESIZE_QVGA);   // 업로드 시엔 QVGA로 충분
  s->set_vflip(s, 0);  // 상하반전 OFF
  s->set_xclk(s, LEDC_TIMER_0, 10);  // XCLK 10MHz로 고정

  // ── WiFi 연결 ─────────────────────────────────
  connectWiFi();

  // ── 웹 서버 ─────────────────────────────────
  startCameraServer();  

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  // // ── LED 안정화 대기 → 캡처 → 업로드 ────────────
  // Serial.println("LED warm-up...");
  // delay(300);                              // LED 발광 안정화

  // camera_fb_t* fb = esp_camera_fb_get();
  // if (!fb) {
  //   Serial.println("Camera capture failed");
  //   return;
  // }
  // Serial.printf("Captured: %u bytes\n", fb->len);

  // uploadImage(fb);

  // esp_camera_fb_return(fb);              // 버퍼 반환 필수

  // // 업로드 후 LED 끄기 (선택)
  // // digitalWrite(LED_PIN, LOW);
}

// void loop() {
//   delay(10000);
// }

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("shoot")) {
      // "shoot 5 2" 형식이면 userId=5, deviceId=2 로 갱신
      // 그냥 "shoot" 이면 기존 값 유지
      int sp1 = cmd.indexOf(' ');
      if (sp1 > 0) {
        int sp2 = cmd.indexOf(' ', sp1 + 1);
        if (sp2 > 0) {
          USER_ID   = cmd.substring(sp1 + 1, sp2).toInt();
          DEVICE_ID = cmd.substring(sp2 + 1).toInt();
        }
      }
      Serial.printf("Shooting with userId=%ld, deviceId=%ld\n", USER_ID, DEVICE_ID);

      Serial.println("LED warm-up...");
      delay(100);
      camera_fb_t* fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("Camera capture failed");
        return;
      }
      Serial.printf("Captured: %u bytes\n", fb->len);
      uploadImage(fb);
      esp_camera_fb_return(fb);
    }
  }
}
