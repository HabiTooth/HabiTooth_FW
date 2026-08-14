#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

// #include <ESP32_OV5640_AF.h> // ov5640 추가
// OV5640 ov5640 = OV5640(); // ov5640 추가

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// Enter your WiFi credentials
// ===========================
#include "config.h"

const char *ssid = nullptr;
const char *password = nullptr;

void startCameraServer();
void setupLedFlash();

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.println("WiFi 스캔 중...");
  int n = WiFi.scanNetworks();

  // 스캔 결과에서 켜져 있는 SSID 선택
  for (int i = 0; i < n; i++) {
    String found = WiFi.SSID(i);
    if (found == WIFI_SSID_1) {
      ssid = WIFI_SSID_1;
      password = WIFI_PASSWORD_1;
      break;
    } else if (found == WIFI_SSID_2) {
      ssid = WIFI_SSID_2;
      password = WIFI_PASSWORD_2;
      break;
    } else if (found == WIFI_SSID_3) {
      ssid = WIFI_SSID_3;
      password = WIFI_PASSWORD_3;
      break;
    }
  }

  if (ssid == nullptr) {
    Serial.println("등록된 WiFi를 찾을 수 없음. SSID_1로 시도");
    ssid = WIFI_SSID_1;
    password = WIFI_PASSWORD_1;
  }

  Serial.printf("연결 대상: %s\n", ssid);
  WiFi.begin(ssid, password);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // UV LED 항상 켜기
  pinMode(19, OUTPUT);
  digitalWrite(19, HIGH);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();

  // // --- [추가 시작] OV5640 오토포커스 모터 초기화 및 1회 실행 ---
  // if (s->id.PID == OV5640_PID) {
  //   ov5640.start(s);
  //   if (ov5640.focusInit() == 0) {
  //     Serial.println("OV5640 Focus Motor Init Success!");
  //   }
  //   if (ov5640.autoFocusMode() == 0) {
  //     Serial.println("OV5640 Auto Focus Done!");
  //   }
  // }
  // // --- [추가 끝] ---

  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  connectWiFi();
  
  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

// // 수동 카메라 loop 문
void loop() {
  // Do nothing. Everything is done in another task by the web server
  delay(10000);
}

// // 자동 카메라 loop 문
// // === 시작 ===
// unsigned long lastFocusTime = 0; // 마지막으로 초점을 맞춘 시간 저장

// void loop() {
//   // 5초(5000밀리초)마다 한 번씩 자동으로 초점을 다시 맞춤
//   if (millis() - lastFocusTime > 5000) {
//     Serial.println("초점 재조정 중...");
//     ov5640.autoFocusMode();
//     lastFocusTime = millis();
//   }
  
//   delay(10); // 웹 서버 작업이 원활하게 돌아가도록 짧은 대기 시간 부여
// }
// // == 끝 ==