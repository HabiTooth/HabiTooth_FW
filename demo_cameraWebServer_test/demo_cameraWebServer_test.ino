#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>

#include "board_config.h"
#include "config.h"

// ----- 웹서버 --------
void startCameraServer();
void setupLedFlash();

// ── WiFi / 서버 설정 ──────────────────────────
const char* SERVER_IP = "";  // connectWiFi()에서 자동 설정
const int   SERVER_PORT = 8080;

// ── 스캔 파라미터 ─────────────────────────────
long  USER_ID   = 3;
long  DEVICE_ID = 1;
const char* g_lightType = "WHITE_LIGHT";  // 런타임 변경 (domain.scan.entity 참고)
const char* REGION      = "OUTER_CENTER"; // domain.scan.entity 참고

// ── LED 핀 ────────────────────────────────────
// 백색 / UV 모듈을 물리적으로 교체해 사용 (동시 사용 불가).
// 따라서 펌웨어는 어떤 LED가 꽂혀 있는지 알 수 없고,
// 촬영 명령에서 지정한 값이 곧 센서 프로파일 + lightType이 됨.
#define LED_PIN 19

// ── 노출 튜닝 값 (시리얼로 런타임 변경 가능) ──
// OV3660: aec_value 0~1200, agc_gain 0~30
int g_uvAec    = 200;   // UV 모드 노출 — 클리핑 방지 위해 낮게 시작
int g_uvAgc    = 4;
int g_whiteAec = 150;   // 백색광 모드 — 훨씬 밝으므로 더 짧게
int g_whiteAgc = 0;

// 촬영 해상도 (QVGA → SVGA 상향)
// FB-OVF가 계속 뜨면 FRAMESIZE_VGA(640x480)로 낮출 것
#define CAPTURE_FRAMESIZE  FRAMESIZE_SVGA   // 800x600

// ─────────────────────────────────────────────
// LED 제어
// ─────────────────────────────────────────────
void ledWrite(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void ledInit() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  // 부팅 시 OFF. 기존 코드처럼 상시 ON이면 발열/전력 낭비이고,
  // 외부 UV 램프를 쓸 때 보드 LED가 섞여 형광 대비가 무너짐.
}

// ─────────────────────────────────────────────
// 센서 프로파일
// ─────────────────────────────────────────────

// 두 모드 공통 — AWB/AEC 자동 제어 차단
void applyCommonProfile(sensor_t* s) {
  // AWB 완전 차단: 405nm 청보라 캐스트를 색온도 오류로 오판해
  // 약한 주황 형광을 회색 쪽으로 눌러버리는 것을 막음
  s->set_whitebal(s, 0);
  s->set_awb_gain(s, 0);
  s->set_wb_mode(s, 0);

  // AEC/AGC 수동 고정
  s->set_exposure_ctrl(s, 0);
  s->set_aec2(s, 0);
  s->set_ae_level(s, 0);
  s->set_gain_ctrl(s, 0);
  s->set_gainceiling(s, GAINCEILING_4X);

  // 화질 보조
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);   // 렌즈 주변부 광량 보정
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_dcw(s, 0);    // 다운스케일 보간 off
  s->set_special_effect(s, 0);
  s->set_hmirror(s, 0);
  s->set_vflip(s, 0);
}

// UV(405nm) 형광 모드
void applyUvProfile(sensor_t* s) {
  applyCommonProfile(s);
  s->set_aec_value(s, g_uvAec);
  s->set_agc_gain(s, g_uvAgc);

  // OV3660 기본 분기의 saturation -2 / brightness +1을 반드시 덮어씀.
  // 형광 촬영에는 정확히 반대 방향임.
  s->set_saturation(s, 2);
  s->set_contrast(s, 1);
  s->set_brightness(s, 0);
}

// 백색광 모드
void applyWhiteProfile(sensor_t* s) {
  applyCommonProfile(s);
  s->set_aec_value(s, g_whiteAec);
  s->set_agc_gain(s, g_whiteAgc);

  s->set_saturation(s, 0);
  s->set_contrast(s, 0);
  s->set_brightness(s, 0);
}

// 센서 레지스터 변경은 다음 프레임부터 반영됨.
// fb_count=2 + GRAB_LATEST 조합이라 버리지 않으면 직전 설정 프레임이 나옴.
void discardFrames(int n) {
  for (int i = 0; i < n; i++) {
    camera_fb_t* d = esp_camera_fb_get();
    if (d) esp_camera_fb_return(d);
  }
}

// ─────────────────────────────────────────────
void connectWiFi() {
  const char* ssids[]     = { WIFI_SSID_1,     WIFI_SSID_2,     WIFI_SSID_3,     WIFI_SSID_4 };
  const char* passwords[] = { WIFI_PASSWORD_1, WIFI_PASSWORD_2, WIFI_PASSWORD_3, WIFI_PASSWORD_4 };
  const char* ips[]       = { SERVER_IP_1,     SERVER_IP_2,     SERVER_IP_3,     SERVER_IP_4 };

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (int i = 0; i < 4; i++) {
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
  bodyHead += String(g_lightType) + "\r\n";

  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"viewType\"\r\n\r\n";
  bodyHead += String(REGION) + "\r\n";

  bodyHead += "--" + boundary + "\r\n";
  bodyHead += "Content-Disposition: form-data; name=\"file\"; filename=\"scan.jpg\"\r\n";
  bodyHead += "Content-Type: image/jpeg\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

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
// 촬영
// ─────────────────────────────────────────────
void shootWith(bool uv, bool doUpload) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) { Serial.println("[ERR] sensor null"); return; }

  ledWrite(true);  // 꽂혀 있는 LED 점등 (외부 램프 사용 시엔 'led off'로 끄고 촬영)

  if (uv) applyUvProfile(s);
  else    applyWhiteProfile(s);

  g_lightType = uv ? "UV_LIGHT" : "WHITE_LIGHT";
  // ↑ 백엔드 enum 실제 값 확인 필요 (domain.scan.entity)

  delay(300);        // LED 안정화 + 레지스터 반영 대기
  discardFrames(3);  // stale frame 폐기

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERR] Camera capture failed");
    ledWrite(false);
    return;
  }

  Serial.printf("[CAP] %s  %ux%u  %u bytes  (aec=%d agc=%d)\n",
                g_lightType, fb->width, fb->height, fb->len,
                uv ? g_uvAec : g_whiteAec,
                uv ? g_uvAgc : g_whiteAgc);

  if (doUpload) uploadImage(fb);
  esp_camera_fb_return(fb);

  ledWrite(false);
}

void printHelp() {
  Serial.println();
  Serial.println("── 명령어 ─────────────────────────────");
  Serial.println("  * white/uv = 지금 꽂혀 있는 LED에 맞춰 직접 지정할 것");
  Serial.println("  shoot white [uid] [did]  백색광 프로파일 촬영 + 업로드");
  Serial.println("  shoot uv    [uid] [did]  UV 프로파일 촬영 + 업로드");
  Serial.println("  test uv                  UV 촬영 (업로드 안 함, 로그만)");
  Serial.println("  test white               백색광 촬영 (업로드 안 함)");
  Serial.println("  aec <0-1200>             UV 노출값 설정");
  Serial.println("  agc <0-30>               UV 게인 설정");
  Serial.println("  waec <0-1200>            백색광 노출값 설정");
  Serial.println("  region <VIEW_TYPE>       촬영 부위 변경");
  Serial.println("  led on | led off         LED 수동 제어");
  Serial.println("  status                   현재 설정 출력");
  Serial.println("  help");
  Serial.println("  * 노출 튜닝은 'aec' 로 값 바꾼 뒤");
  Serial.println("    브라우저에서 http://<IP>/capture 로 확인하는 게 빠름");
  Serial.println("───────────────────────────────────────");
}

// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  ledInit();

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
  config.xclk_freq_hz = 10000000;   // 20MHz → 10MHz, 초기화 시점에 확정
                                    // (런타임 set_xclk과 이중 설정하면 FB-OVF 발생)
  config.frame_size   = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (psramFound()) {
    config.jpeg_quality = 12;  // FB-OVF 방지. 안정화된 뒤 10 → 8로 조금씩 낮춰볼 것
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

  sensor_t* s = esp_camera_sensor_get();
  Serial.printf("Sensor PID: 0x%x\n", s->id.PID);

  s->set_framesize(s, CAPTURE_FRAMESIZE);
  // set_xclk() 런타임 호출 제거 — config.xclk_freq_hz에서 이미 10MHz로 설정됨.
  // 여기서 다시 바꾸면 드라이버 DMA 타이밍과 어긋나 FB-OVF 발생.

  // 기본은 백색광 프로파일
  applyWhiteProfile(s);
  delay(100);
  discardFrames(2);

  connectWiFi();
  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  printHelp();
}

// ─────────────────────────────────────────────
void loop() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  // ── shoot / test ──────────────────────────
  if (cmd.startsWith("shoot") || cmd.startsWith("test")) {
    bool doUpload = cmd.startsWith("shoot");
    bool uv = false;

    if (cmd.indexOf("uv") > 0) {
      uv = true;
    } else if (cmd.indexOf("white") > 0) {
      uv = false;
    } else {
      Serial.println("[ERR] 조명을 지정하세요: shoot white | shoot uv");
      return;
    }

    // "shoot uv 5 2" 형식이면 userId=5, deviceId=2
    int sp1 = cmd.indexOf(' ');
    int sp2 = (sp1 > 0) ? cmd.indexOf(' ', sp1 + 1) : -1;
    if (sp2 > 0) {
      int sp3 = cmd.indexOf(' ', sp2 + 1);
      if (sp3 > 0) {
        USER_ID   = cmd.substring(sp2 + 1, sp3).toInt();
        DEVICE_ID = cmd.substring(sp3 + 1).toInt();
      }
    }

    Serial.printf("Shooting: light=%s userId=%ld deviceId=%ld region=%s\n",
                  uv ? "UV" : "WHITE", USER_ID, DEVICE_ID, REGION);
    shootWith(uv, doUpload);
    return;
  }

  // ── 노출 튜닝 ─────────────────────────────
  if (cmd.startsWith("aec ")) {
    g_uvAec = constrain(cmd.substring(4).toInt(), 0, 1200);
    sensor_t* s = esp_camera_sensor_get();
    applyUvProfile(s);
    discardFrames(2);
    Serial.printf("[SET] uv aec = %d  → http://%s/capture 에서 확인\n",
                  g_uvAec, WiFi.localIP().toString().c_str());
    return;
  }

  if (cmd.startsWith("agc ")) {
    g_uvAgc = constrain(cmd.substring(4).toInt(), 0, 30);
    sensor_t* s = esp_camera_sensor_get();
    applyUvProfile(s);
    discardFrames(2);
    Serial.printf("[SET] uv agc = %d\n", g_uvAgc);
    return;
  }

  if (cmd.startsWith("waec ")) {
    g_whiteAec = constrain(cmd.substring(5).toInt(), 0, 1200);
    Serial.printf("[SET] white aec = %d\n", g_whiteAec);
    return;
  }

  // ── 부위 변경 ─────────────────────────────
  if (cmd.startsWith("region ")) {
    static String regionBuf;
    regionBuf = cmd.substring(7);
    regionBuf.trim();
    REGION = regionBuf.c_str();
    Serial.printf("[SET] region = %s\n", REGION);
    return;
  }

  // ── LED 수동 제어 ─────────────────────────
  if (cmd == "led on" || cmd == "led off") {
    bool on = cmd.endsWith("on");
    ledWrite(on);
    Serial.printf("[LED] %s\n", on ? "ON" : "OFF");
    return;
  }

  // ── 상태 ──────────────────────────────────
  if (cmd == "status") {
    sensor_t* s = esp_camera_sensor_get();
    Serial.printf("PID=0x%x  framesize=%d  light=%s  region=%s\n",
                  s->id.PID, s->status.framesize, g_lightType, REGION);
    Serial.printf("uv:    aec=%d agc=%d\n", g_uvAec, g_uvAgc);
    Serial.printf("white: aec=%d agc=%d\n", g_whiteAec, g_whiteAgc);
    Serial.printf("userId=%ld deviceId=%ld\n", USER_ID, DEVICE_ID);
    return;
  }

  if (cmd == "help") { printHelp(); return; }

  Serial.println("[ERR] 알 수 없는 명령. 'help' 입력");
}
