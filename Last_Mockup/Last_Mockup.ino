/*
 * HabiTooth Wand - Camera Web Server + White/UV LED + NeoPixel
 * Board: Seeed XIAO ESP32S3 Sense
 *
 * ===== 핀맵 (2026-09 신 회로 - 확정본) =====
 *   PIN_SW      = GPIO2 (D1)  스위치, 액티브-로우 (외부 10k 풀업 가정)
 *   PIN_UV_LED  = GPIO4 (D3)  UV  AO3400 게이트 (active-HIGH)   [구 회로 GPIO5에서 변경]
 *   PIN_W_LED   = GPIO5 (D4)  백색 AO3400 게이트 (active-HIGH)   [구 회로 GPIO6에서 변경]
 *   PIN_NEO     = GPIO6 (D5)  네오픽셀 DIN (WS2812/SK6812)      [신규]
 *
 * ===== 라이브러리 =====
 *   - Adafruit NeoPixel  (라이브러리 매니저에서 설치)
 *   - 기본 ESP32 카메라 (Boards - esp32 by Espressif Systems 3.x)
 *
 * ===== 노출 제어 =====
 *   AEC/AWB를 수동 고정. 데이터셋 색·밝기 일관성 확보가 목적.
 *   조명별로 노출값이 따로 있음:
 *     aec  = UV 촬영용   (UV LED가 어두워서 길게)
 *     waec = 백색광 촬영용 (백색 LED가 밝아서 짧게)
 *
 *   ※ 노란 필터를 상시 장착한 상태를 전제로 함.
 *     필터/LED/노출값을 바꾸면 흰 종이 기준 촬영부터 다시 할 것.
 *
 * ===== 스위치 =====
 *   누르면 백색 -> UV 연속 촬영. 두 장 모두 브라우저로 자동 다운로드.
 *
 * ===== 연속 촬영 속도 (2026-09 수정) =====
 *   - captureWithLight: 워밍업 350ms -> 120ms, 버림 프레임 사이 delay(60) 제거,
 *     applyProfileFor 중복 호출 제거 (setLed가 이미 호출함)
 *   - capturePair: 백색 이미지를 버퍼에 복사하기 전에 UV LED를 미리 점등
 *   - 웹 "백색 + UV 연속" 버튼이 /pair 로 capturePair를 직접 호출
 *     (기존 JS의 1800ms 고정 지연 제거)
 *   ※ 워밍업을 줄였으므로 첫 장 색이 이전 설정으로 찍히지 않는지 확인할 것.
 *     문제가 있으면 warmupMs보다 discard를 3으로 올리는 편이 빠름.
 *
 * ===== 접속 =====
 *   UI     : http://habitooth.local/
 *   스트림 : http://habitooth.local:81/stream
 */

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>
#include <ESPmDNS.h>

#include "board_config.h"
#include "config.h"
#include "page_html.h"

// ── LED / 스위치 / 네오픽셀 핀 (신 회로) ─────
#define PIN_UV_LED   4    // D3
#define PIN_W_LED    5    // D4
#define PIN_SW       2    // D1
#define PIN_NEO      6    // D5

#define NUM_PIXELS   2    // 네오픽셀 개수 (실제 개수에 맞게 조정)

#define LED_ACTIVE_HIGH  1

#define LEDC_CH_UV   4
#define LEDC_CH_W    5

#define PWM_FREQ   40000
#define PWM_RES    8
int brightW  = 76;
int brightUV = 230;

#define DEBOUNCE_MS  30
#define MDNS_NAME  "habitooth"

enum LedMode { LED_OFF = 0, LED_WHITE, LED_UV, LED_PREVIEW };

// ── 노출 설정 (OV3660: aec 0~1200, agc 0~30) ──
int g_uvAec    = 600;
int g_uvAgc    = 4;
int g_uvWb = 1;
int g_whiteAec = 750;    // 노란 필터 기준으로 맞춘 값
int g_whiteAgc = 0;

// ── 촬영 구역 ─────────────────────────────────
String VIEW_TYPE = "OUTER_CENTER";

static const char* const VALID_VIEWS[] = {
  "UPPER_RIGHT_MOLAR","UPPER_RIGHT_PREMOLAR","UPPER_FRONT",
  "UPPER_LEFT_PREMOLAR","UPPER_LEFT_MOLAR",
  "LOWER_RIGHT_MOLAR","LOWER_RIGHT_PREMOLAR","LOWER_FRONT",
  "LOWER_LEFT_PREMOLAR","LOWER_LEFT_MOLAR",
  "OUTER_RIGHT","OUTER_CENTER","OUTER_LEFT"
};

bool isValidView(const String& v) {
  for (auto* p : VALID_VIEWS) if (v == p) return true;
  return false;
}

// ── 전역 ──────────────────────────────────────
WebServer      server(80);
WiFiServer     streamServer(81);
SemaphoreHandle_t camMutex;

Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEO, NEO_GRB + NEO_KHZ800);


LedMode currentLed = LED_OFF;

// ── 프리뷰(상시) 조명 ─────────────────────────
int g_previewBright = 60;
int g_previewAec    = 750;
int g_previewAgc    = 0;

volatile int streamClients = 0;
uint32_t previewSince    = 0;
uint32_t previewCoolUntil = 0;

#define PREVIEW_MAX_MS   180000UL
#define PREVIEW_COOL_MS   30000UL
#define WHITE_MAX_MS      60000UL
#define PEND_CAP  (150 * 1024)

uint8_t*          pendBuf[2] = { nullptr, nullptr };
size_t            pendCap[2] = { 0, 0 };
size_t            pendLen[2] = { 0, 0 };
String            pendName[2];
volatile uint32_t pendSeq    = 0;
volatile bool     capturing  = false;

// ─────────────────────────────────────────────
// 네오픽셀 헬퍼
// ─────────────────────────────────────────────
void neoFill(uint32_t c) {
  for (int i = 0; i < NUM_PIXELS; i++) strip.setPixelColor(i, c);
  strip.show();
}
void neoOff()      { strip.clear(); strip.show(); }
void neoBoot()     { neoFill(strip.Color(30, 15, 0)); }    // dim amber = 부팅/연결중
void neoOk()       { neoFill(strip.Color(0, 40, 0)); }     // 초록 = 준비완료
void neoFlashW()   { neoFill(strip.Color(80, 80, 80)); }   // 흰색 = 백색 촬영중
void neoFlashUV()  { neoFill(strip.Color(60, 0, 80)); }    // 보라 = UV 촬영중

// ─────────────────────────────────────────────
// 센서 프로파일
// ─────────────────────────────────────────────
void applyCommonProfile(sensor_t* s) {
  if (!s) return;
  s->set_exposure_ctrl(s, 0);
  s->set_aec2(s, 0);
  s->set_ae_level(s, 0);
  s->set_gain_ctrl(s, 0);
  s->set_gainceiling(s, GAINCEILING_4X);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_dcw(s, 0);
  s->set_special_effect(s, 0);
}

void applyUvProfile(sensor_t* s) {
  if (!s) return;
  applyCommonProfile(s);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, g_uvWb);   // 0=auto 1=sunny 2=cloudy 3=office 4=home
  s->set_aec_value(s, g_uvAec);
  s->set_agc_gain(s, g_uvAgc);
  s->set_saturation(s, 0);
  s->set_contrast(s, 1);
  s->set_brightness(s, 0);
}

void applyWhiteProfile(sensor_t* s) {
  if (!s) return;
  applyCommonProfile(s);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_aec_value(s, g_whiteAec);
  s->set_agc_gain(s, g_whiteAgc);
  s->set_saturation(s, 0);
  s->set_contrast(s, 0);
  s->set_brightness(s, 0);
}

void applyPreviewProfile(sensor_t* s) {
  if (!s) return;
  applyCommonProfile(s);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_aec_value(s, g_previewAec);
  s->set_agc_gain(s, g_previewAgc);
  s->set_saturation(s, 0);
  s->set_contrast(s, 0);
  s->set_brightness(s, 0);
}

void applyProfileFor(LedMode m) {
  sensor_t* s = esp_camera_sensor_get();
  if      (m == LED_UV)      applyUvProfile(s);
  else if (m == LED_PREVIEW) applyPreviewProfile(s);
  else                       applyWhiteProfile(s);
}

// ─────────────────────────────────────────────
// LED 제어 (PWM - AO3400 게이트도 PWM 수용 가능)
// ─────────────────────────────────────────────
void ledInit() {
  ledcAttachChannel(PIN_UV_LED, PWM_FREQ, PWM_RES, LEDC_CH_UV);
  ledcAttachChannel(PIN_W_LED,  PWM_FREQ, PWM_RES, LEDC_CH_W);
  ledcWrite(PIN_UV_LED, 0);
  ledcWrite(PIN_W_LED,  0);
}

static inline int duty(int v) {
#if LED_ACTIVE_HIGH
  return v;
#else
  return 255 - v;
#endif
}

static inline void driveLed(int pin, int v) {
  if (v >= 255) {                 // 풀출력이면 PWM 끄고 DC로
    ledcDetach(pin);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  } else if (v <= 0) {
    ledcWrite(pin, duty(0));
  } else {
    ledcWrite(pin, duty(v));
  }
}

void setLed(LedMode m) {
  currentLed = m;
  switch (m) {
    case LED_WHITE:
      ledcWrite(PIN_UV_LED, duty(0));
      ledcWrite(PIN_W_LED,  duty(brightW));
      break;
    case LED_PREVIEW:
      ledcWrite(PIN_UV_LED, duty(0));
      ledcWrite(PIN_W_LED,  duty(g_previewBright));
      break;
    case LED_UV:
      ledcWrite(PIN_W_LED,  duty(0));
      ledcWrite(PIN_UV_LED, duty(brightUV));
      break;
    default:
      ledcWrite(PIN_W_LED,  duty(0));
      ledcWrite(PIN_UV_LED, duty(0));
      break;
  }
  if (m == LED_WHITE || m == LED_PREVIEW) previewSince = millis();
  applyProfileFor(m == LED_OFF ? LED_WHITE : m);
}

const char* ledName() {
  switch (currentLed) {
    case LED_WHITE:   return "WHITE";
    case LED_UV:      return "UV";
    case LED_PREVIEW: return "PREVIEW";
    default:          return "OFF";
  }
}

// ─────────────────────────────────────────────
// WiFi - 핫스팟 4개 순차 시도
// ─────────────────────────────────────────────
void connectWiFi() {
  const char* ssids[] = {
    WIFI_SSID_1, WIFI_SSID_2, WIFI_SSID_3, WIFI_SSID_4
  };
  const char* passwords[] = {
    WIFI_PASSWORD_1, WIFI_PASSWORD_2, WIFI_PASSWORD_3, WIFI_PASSWORD_4
  };
  const int N = sizeof(ssids) / sizeof(ssids[0]);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  neoBoot();  // 연결중 표시

  for (int i = 0; i < N; i++) {
    if (strlen(ssids[i]) == 0) continue;
    WiFi.begin(ssids[i], passwords[i]);
    Serial.printf("Trying %s ...\n", ssids[i]);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500);
      Serial.print(".");
      retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
      if (MDNS.begin(MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
      }
      neoOk();
      delay(1500);
      neoOff();
      return;
    }
    WiFi.disconnect();
    delay(500);
  }
  Serial.println("\nWiFi connect failed");
  neoFill(strip.Color(60, 0, 0));  // 빨강 = 연결 실패
}

// ─────────────────────────────────────────────
// 파일명
// ─────────────────────────────────────────────
String timeStamp() {
  struct tm t;
  if (getLocalTime(&t, 100)) {
    char buf[24];
    strftime(buf, sizeof(buf), "%m%d_%H%M%S", &t);
    return String(buf);
  }
  return "t" + String(millis() / 1000);
}

String makeFilename(bool isUV) {
  return VIEW_TYPE + "-" + (isUV ? "UV" : "WHITE") + "-" + timeStamp() + ".jpg";
}

// ─────────────────────────────────────────────
// 조명 전환 후 안정화 캡처
//   setLed 안에서 applyProfileFor가 호출되므로 여기서 다시 부르지 않음.
//   esp_camera_fb_get() 자체가 다음 프레임을 기다리므로 delay 불필요.
// ─────────────────────────────────────────────
camera_fb_t* captureWithLight(LedMode mode, int warmupMs = 120, int discard = 2) {
  setLed(mode);
  delay(warmupMs);

  for (int i = 0; i < discard; i++) {   // 파이프라인에 남은 구 설정 프레임 배출
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
  return esp_camera_fb_get();
}

// ─────────────────────────────────────────────
// 스위치 촬영: 백색 + UV
// ─────────────────────────────────────────────
bool storePending(int idx, camera_fb_t* fb, const String& name) {
  if (!pendBuf[idx]) { Serial.println("[ERR] pending buffer 미할당"); return false; }
  if (fb->len > pendCap[idx]) {
    Serial.printf("[ERR] 이미지가 버퍼보다 큼 %u > %u\n", fb->len, pendCap[idx]);
    return false;
  }
  memcpy(pendBuf[idx], fb->buf, fb->len);
  pendLen[idx]  = fb->len;
  pendName[idx] = name;
  return true;
}

void capturePair() {
  if (capturing) return;
  capturing = true;

  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
    Serial.println("[PAIR] camera busy");
    capturing = false;
    return;
  }

  Serial.printf("[PAIR] %s 촬영 시작\n", VIEW_TYPE.c_str());
  uint32_t t0 = millis();
  bool ok = true;

  for (int i = 0; i < 2; i++) {
    bool isUV = (i == 1);
    if (isUV) neoFlashUV(); else neoFlashW();

    camera_fb_t* fb = captureWithLight(isUV ? LED_UV : LED_WHITE);
    if (!fb) { Serial.println("[PAIR] capture failed"); ok = false; break; }

    // 백색 이미지를 복사하는 동안 UV 조명을 미리 켜둔다 (워밍업 선점)
    if (!isUV) { neoFlashUV(); setLed(LED_UV); }

    String name = makeFilename(isUV);
    if (storePending(i, fb, name))
      Serial.printf("  [%d] %s  %u bytes  (aec=%d)\n", i, name.c_str(), fb->len,
                    isUV ? g_uvAec : g_whiteAec);
    else { ok = false; esp_camera_fb_return(fb); break; }

    esp_camera_fb_return(fb);
  }

  setLed(LED_OFF);
  neoOff();
  xSemaphoreGive(camMutex);

  if (ok) {
    pendSeq++;
    Serial.printf("[PAIR] 완료 seq=%lu  (%lu ms)\n",
                  (unsigned long)pendSeq, (unsigned long)(millis() - t0));
  } else {
    Serial.println("[PAIR] 실패 - seq 미증가, 브라우저 저장 안 됨");
  }

  capturing = false;
}

// ─────────────────────────────────────────────
// 스위치 처리
// ─────────────────────────────────────────────
void swTask() {
  static bool     lastRaw  = HIGH;
  static bool     stableSw = HIGH;
  static uint32_t tEdge    = 0;

  bool     raw = digitalRead(PIN_SW);
  uint32_t now = millis();

  if (raw != lastRaw) { lastRaw = raw; tEdge = now; return; }
  if ((now - tEdge) < DEBOUNCE_MS) return;
  if (raw == stableSw) return;

  stableSw = raw;
  if (stableSw == HIGH) capturePair();   // 뗄 때 촬영 (릴리즈 트리거)
}

// ─────────────────────────────────────────────
// MJPEG 스트림 태스크 (포트 81)
// ─────────────────────────────────────────────
void streamTask(void* pv) {
  streamServer.begin();
  streamServer.setNoDelay(true);

  for (;;) {
    WiFiClient client = streamServer.available();
    if (!client) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

    Serial.println("[STREAM] connected");
    streamClients++;
    client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Cache-Control: no-cache\r\n\r\n");

    while (client.connected()) {
      if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(6000)) != pdTRUE) break;
      camera_fb_t* fb = esp_camera_fb_get();
      if (!fb) { xSemaphoreGive(camMutex); vTaskDelay(pdMS_TO_TICKS(50)); continue; }

      client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
      client.write(fb->buf, fb->len);
      client.print("\r\n");

      esp_camera_fb_return(fb);
      xSemaphoreGive(camMutex);
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    client.stop();
    streamClients--;
    Serial.println("[STREAM] disconnected");
  }
}

// ─────────────────────────────────────────────
// 핸들러
// ─────────────────────────────────────────────
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

void handleLed() {
  String m = server.arg("mode");
  if      (m == "white") setLed(LED_WHITE);
  else if (m == "uv")    setLed(LED_UV);
  else {
    setLed(LED_OFF);
    previewCoolUntil = millis() + PREVIEW_COOL_MS;
  }
  server.send(200, "text/plain", ledName());
}

void handleBright() {
  if (server.hasArg("w"))  brightW  = constrain(server.arg("w").toInt(),  0, 255);
  if (server.hasArg("uv")) brightUV = constrain(server.arg("uv").toInt(), 0, 255);
  if (server.args() > 0) setLed(currentLed);
  String j = "{\"w\":" + String(brightW) + ",\"uv\":" + String(brightUV) + "}";
  server.send(200, "application/json", j);
}

void handleExp() {
  bool changed = false;

  if (server.hasArg("aec")) {
    g_uvAec = constrain(server.arg("aec").toInt(), 0, 1200);
    changed = true;
  }
  if (server.hasArg("waec")) {
    g_whiteAec = constrain(server.arg("waec").toInt(), 0, 1200);
    changed = true;
  }
  if (server.hasArg("agc")) {
    g_uvAgc = constrain(server.arg("agc").toInt(), 0, 30);
    changed = true;
  }

  if (changed && currentLed != LED_OFF) applyProfileFor(currentLed);

  String j = "{";
  j += "\"aec\":"   + String(g_uvAec);
  j += ",\"waec\":" + String(g_whiteAec);
  j += ",\"agc\":"  + String(g_uvAgc);
  j += "}";
  server.send(200, "application/json", j);
}

void handleRes() {
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, (framesize_t)server.arg("v").toInt());
    delay(100);
    applyProfileFor(currentLed);
  }
  server.send(200, "text/plain", "ok");
}

void handleView() {
  if (server.hasArg("v")) VIEW_TYPE = server.arg("v");
  server.send(200, "text/plain", VIEW_TYPE);
}

// 웹 "백색 + UV 연속" 버튼 - 스위치와 같은 경로
void handlePair() {
  if (capturing) { server.send(409, "text/plain", "busy"); return; }
  capturePair();
  server.send(200, "text/plain", "ok");
}

void handlePending() {
  String j = "{";
  j += "\"seq\":"  + String((unsigned long)pendSeq);
  j += ",\"w\":\"" + pendName[0] + "\"";
  j += ",\"u\":\"" + pendName[1] + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

void handlePend() {
  int i = server.arg("i").toInt();
  if (i < 0 || i > 1 || !pendBuf[i] || pendLen[i] == 0) {
    server.send(404, "text/plain", "no pending image");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + pendName[i] + "\"");
  server.setContentLength(pendLen[i]);
  server.send(200, "image/jpeg", "");
  server.client().write(pendBuf[i], pendLen[i]);
}

void handleSnap() {
  if (server.hasArg("view")) VIEW_TYPE = server.arg("view");
  bool isUV = (server.arg("light") == "uv");
  LedMode prev = currentLed;

  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
    server.send(503, "text/plain", "camera busy");
    return;
  }

  if (isUV) neoFlashUV(); else neoFlashW();

  camera_fb_t* fb = captureWithLight(isUV ? LED_UV : LED_WHITE);
  if (!fb) {
    setLed(prev);
    neoOff();
    xSemaphoreGive(camMutex);
    server.send(500, "text/plain", "capture failed");
    return;
  }

  String fname = makeFilename(isUV);
  Serial.printf("[SNAP] %s  %u bytes  (aec=%d)\n", fname.c_str(), fb->len,
                isUV ? g_uvAec : g_whiteAec);

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  server.client().write(fb->buf, fb->len);

  esp_camera_fb_return(fb);
  setLed(prev);
  neoOff();
  xSemaphoreGive(camMutex);
}

void handlePreview() {
  bool changed = false;
  if (server.hasArg("b"))   { g_previewBright = constrain(server.arg("b").toInt(), 0, 255);   changed = true; }
  if (server.hasArg("aec")) { g_previewAec    = constrain(server.arg("aec").toInt(), 0, 1200); changed = true; }
  if (server.hasArg("agc")) { g_previewAgc    = constrain(server.arg("agc").toInt(), 0, 30);   changed = true; }
  if (changed && currentLed == LED_PREVIEW) setLed(LED_PREVIEW);

  String j = "{\"b\":" + String(g_previewBright) +
             ",\"aec\":" + String(g_previewAec) +
             ",\"agc\":" + String(g_previewAgc) + "}";
  server.send(200, "application/json", j);
}

void handleNeo() {
  String c = server.arg("c");
  if      (c == "off") neoOff();
  else if (c == "r")   neoFill(strip.Color(60, 0, 0));
  else if (c == "g")   neoFill(strip.Color(0, 60, 0));
  else if (c == "b")   neoFill(strip.Color(0, 0, 60));
  else if (c == "w")   neoFill(strip.Color(50, 50, 50));
  else if (c == "u")   neoFill(strip.Color(40, 0, 60));
  server.send(200, "text/plain", "ok");
}

void printStatus() {
  sensor_t* s = esp_camera_sensor_get();
  Serial.println("── 상태 ───────────────────────────────");
  if (s) Serial.printf("  PID       : 0x%x\n", s->id.PID);
  Serial.printf("  조명      : %s\n", ledName());
  Serial.printf("  viewType  : %s\n", VIEW_TYPE.c_str());
  Serial.printf("  UV        : aec=%d  agc=%d  밝기=%d\n", g_uvAec, g_uvAgc, brightUV);
  Serial.printf("  백색광    : aec=%d  agc=%d  밝기=%d\n", g_whiteAec, g_whiteAgc, brightW);
  Serial.printf("  프리뷰    : aec=%d  agc=%d  밝기=%d\n", g_previewAec, g_previewAgc, g_previewBright);
  Serial.println("───────────────────────────────────────");
}

void printHelp() {
  Serial.println("── 시리얼 명령 ────────────────────────");
  Serial.println("  w / u / o      백색 / UV / 끄기");
  Serial.println("  c              백색+UV 연속 촬영");
  Serial.println("  sw             스위치 상태");
  Serial.println("  aec <0-1200>   UV 노출");
  Serial.println("  waec <0-1200>  백색광 노출");
  Serial.println("  agc <0-30>     UV 게인");
  Serial.println("  wb <0-4>       UV 화이트밸런스 (0auto 1sunny 2cloudy 3office 4home)");
  Serial.println("  bw <0-255>     백색 밝기    bu <0-255>  UV 밝기");
  Serial.println("  n              네오픽셀 색상 순환 (테스트)");
  Serial.println("  no             네오픽셀 OFF");
  Serial.println("  status / help");
  Serial.println("───────────────────────────────────────");
}

// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(800);

  Serial.printf("[BOOT] PSRAM %s  size=%u\n",
                psramFound() ? "감지됨" : "없음", ESP.getPsramSize());

  ledInit();
  setLed(LED_OFF);
  pinMode(PIN_SW, INPUT);   // 외부 10k 풀업 사용 (신 회로)

  // 네오픽셀 초기화 (부팅 잔상 즉시 제거)
  strip.begin();
  strip.setBrightness(255);
  strip.clear();
  strip.show();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
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
    neoFill(strip.Color(80, 0, 0));  // 빨강 = 카메라 실패
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_VGA);
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);

  applyWhiteProfile(s);

  camMutex = xSemaphoreCreateMutex();

  size_t cap = psramFound() ? PEND_CAP : (40 * 1024);
  for (int i = 0; i < 2; i++) {
    pendBuf[i] = (uint8_t*)ps_malloc(cap);
    if (!pendBuf[i] && cap > 40 * 1024) {      // 크게 실패하면 작게 재시도
      cap = 40 * 1024;
      pendBuf[i] = (uint8_t*)ps_malloc(cap);
    }
    pendCap[i] = pendBuf[i] ? cap : 0;
    pendLen[i] = 0;
    Serial.printf("[PEND] buf%d %s (%u bytes)\n",
                  i, pendBuf[i] ? "OK" : "FAIL", (unsigned)pendCap[i]);
  }
  if (!pendBuf[0] || !pendBuf[1]) {
    Serial.println("[PEND] 버퍼 확보 실패 - PSRAM 설정을 확인하세요");
    neoFill(strip.Color(60, 30, 0));   // 주황 = 메모리 경고
  }
  Serial.printf("[MEM] PSRAM free = %u\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setLed(LED_WHITE);
    delay(3000);
    setLed(LED_OFF);
  }

  configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com");

  server.on("/",        handleRoot);
  server.on("/led",     handleLed);
  server.on("/bright",  handleBright);
  server.on("/exp",     handleExp);
  server.on("/res",     handleRes);
  server.on("/view",    handleView);
  server.on("/snap",    handleSnap);
  server.on("/pair",    handlePair);
  server.on("/pending", handlePending);
  server.on("/pend",    handlePend);
  server.on("/preview", handlePreview);
  server.on("/neo",     handleNeo);
  server.enableCORS(true);
  server.begin();

  xTaskCreatePinnedToCore(streamTask, "stream", 8192, NULL, 1, NULL, 0);

  Serial.println("========================================");
  Serial.print  ("  UI     : http://"); Serial.println(WiFi.localIP());
  Serial.print  ("  Stream : http://"); Serial.print(WiFi.localIP()); Serial.println(":81/stream");
  Serial.println("========================================");
  printHelp();
  printStatus();
}

void ledGuardTask() {
  if (capturing) return;

  uint32_t now = millis();

  if (currentLed == LED_PREVIEW && (now - previewSince) > PREVIEW_MAX_MS) {
    setLed(LED_OFF);
    previewCoolUntil = now + PREVIEW_COOL_MS;
    Serial.println("[LED] 프리뷰 연속 점등 상한 - 강제 소등");
    return;
  }
  if (currentLed == LED_WHITE && (now - previewSince) > WHITE_MAX_MS) {
    setLed(LED_OFF);
    previewCoolUntil = now + PREVIEW_COOL_MS;
    Serial.println("[LED] 백색 풀출력 상한 - 강제 소등");
    return;
  }

  bool want = (streamClients > 0);

  if (want && currentLed == LED_OFF && now >= previewCoolUntil) {
    setLed(LED_PREVIEW);
  } else if (!want && currentLed == LED_PREVIEW) {
    setLed(LED_OFF);
  }
}

void loop() {
  server.handleClient();
  swTask();

  ledGuardTask();

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if      (cmd == "w")  { setLed(LED_WHITE); Serial.println("WHITE"); }
    else if (cmd == "u")  { setLed(LED_UV);    Serial.println("UV"); }
    else if (cmd == "o")  { setLed(LED_OFF);   Serial.println("OFF"); }
    else if (cmd == "c")  { capturePair(); }
    else if (cmd == "n")  {
      static int idx = 0;
      const uint32_t seq[] = {
        strip.Color(60,0,0), strip.Color(0,60,0), strip.Color(0,0,60),
        strip.Color(50,50,50), strip.Color(60,30,0), 0
      };
      neoFill(seq[idx]);
      idx = (idx + 1) % 6;
      Serial.printf("[NEO] color %d\n", idx);
    }
    else if (cmd == "no") { neoOff(); Serial.println("[NEO] OFF"); }
    else if (cmd == "sw") {
      Serial.printf("[SW] GPIO%d = %s\n", PIN_SW,
                    digitalRead(PIN_SW) == LOW ? "LOW (눌림)" : "HIGH (뗌)");
    }
    else if (cmd.startsWith("aec ")) {
      g_uvAec = constrain(cmd.substring(4).toInt(), 0, 1200);
      if (currentLed != LED_OFF) applyProfileFor(currentLed);
      Serial.printf("[SET] uv aec = %d\n", g_uvAec);
    }
    else if (cmd.startsWith("waec ")) {
      g_whiteAec = constrain(cmd.substring(5).toInt(), 0, 1200);
      if (currentLed != LED_OFF) applyProfileFor(currentLed);
      Serial.printf("[SET] white aec = %d\n", g_whiteAec);
    }
    else if (cmd.startsWith("agc ")) {
      g_uvAgc = constrain(cmd.substring(4).toInt(), 0, 30);
      if (currentLed != LED_OFF) applyProfileFor(currentLed);
      Serial.printf("[SET] uv agc = %d\n", g_uvAgc);
    }
    else if (cmd.startsWith("wb ")) {
      g_uvWb = constrain(cmd.substring(3).toInt(), 0, 4);
      if (currentLed != LED_OFF) applyProfileFor(currentLed);
      const char* n[] = {"auto","sunny","cloudy","office","home"};
      Serial.printf("[SET] uv wb = %d (%s)\n", g_uvWb, n[g_uvWb]);
    }
    else if (cmd.startsWith("bw ")) {
      brightW = constrain(cmd.substring(3).toInt(), 0, 255);
      setLed(currentLed);
      Serial.printf("[SET] white bright = %d\n", brightW);
    }
    else if (cmd.startsWith("bu ")) {
      brightUV = constrain(cmd.substring(3).toInt(), 0, 255);
      setLed(currentLed);
      Serial.printf("[SET] uv bright = %d\n", brightUV);
    }
    else if (cmd == "status") { printStatus(); }
    else if (cmd == "help")   { printHelp(); }
    else if (cmd.length())    { Serial.println("[ERR] 알 수 없는 명령. 'help'"); }
  }
  delay(2);
}
