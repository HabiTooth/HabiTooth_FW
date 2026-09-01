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
 * ===== 접속 =====
 *   UI     : http://<보드IP>/
 *   스트림 : http://<보드IP>:81/stream
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
int brightW  = 255;
int brightUV = 255;

#define DEBOUNCE_MS  30
#define MDNS_NAME  "habitooth"

// ── 노출 설정 (OV3660: aec 0~1200, agc 0~30) ──
int g_uvAec    = 600;
int g_uvAgc    = 4;
int g_uvWb = 1;
int g_whiteAec = 750;    // 노란 필터 기준으로 맞춘 값
int g_whiteAgc = 0;

// ── 촬영 구역 ─────────────────────────────────
String VIEW_TYPE = "OUTER_CENTER";

// ── 전역 ──────────────────────────────────────
WebServer      server(80);
WiFiServer     streamServer(81);
SemaphoreHandle_t camMutex;

Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEO, NEO_GRB + NEO_KHZ800);

enum LedMode { LED_OFF = 0, LED_WHITE, LED_UV, LED_PREVIEW };
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

uint8_t*          pendBuf[2] = { nullptr, nullptr };
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
  return VIEW_TYPE + "_" + (isUV ? "UV" : "WHITE") + "_" + timeStamp() + ".jpg";
}

// ─────────────────────────────────────────────
// 조명 전환 후 안정화 캡처
// ─────────────────────────────────────────────
camera_fb_t* captureWithLight(LedMode mode, int warmupMs = 350, int discard = 3) {
  setLed(mode);
  applyProfileFor(mode);
  delay(warmupMs);

  camera_fb_t* fb = nullptr;
  for (int i = 0; i < discard; i++) {
    fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(60);
  }
  return esp_camera_fb_get();
}

// ─────────────────────────────────────────────
// 스위치 촬영: 백색 + UV
// ─────────────────────────────────────────────
bool storePending(int idx, camera_fb_t* fb, const String& name) {
  if (pendBuf[idx]) { free(pendBuf[idx]); pendBuf[idx] = nullptr; pendLen[idx] = 0; }

  uint8_t* b = (uint8_t*)ps_malloc(fb->len);
  if (!b) b = (uint8_t*)malloc(fb->len);
  if (!b) { Serial.println("[ERR] pending malloc failed"); return false; }

  memcpy(b, fb->buf, fb->len);
  pendBuf[idx]  = b;
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
  bool ok = true;

  for (int i = 0; i < 2; i++) {
    bool isUV = (i == 1);
    if (isUV) neoFlashUV(); else neoFlashW();

    camera_fb_t* fb = captureWithLight(isUV ? LED_UV : LED_WHITE);
    if (!fb) { Serial.println("[PAIR] capture failed"); ok = false; break; }

    String name = makeFilename(isUV);
    if (storePending(i, fb, name))
      Serial.printf("  [%d] %s  %u bytes  (aec=%d)\n", i, name.c_str(), fb->len,
                    isUV ? g_uvAec : g_whiteAec);
    else ok = false;

    esp_camera_fb_return(fb);
  }

  setLed(LED_OFF);
  neoOff();
  xSemaphoreGive(camMutex);

  if (ok) {
    pendSeq++;
    Serial.printf("[PAIR] 완료 seq=%lu\n", (unsigned long)pendSeq);
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
// 웹 UI
// ─────────────────────────────────────────────
static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="ko"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HabiTooth Wand</title>
<style>
 :root{--bg:#0f1220;--card:#191d33;--line:#2b3153;--txt:#e8ebff;
       --dim:#8b93c4;--acc:#7c8cff;--uv:#a855f7;--ok:#34d399}
 *{box-sizing:border-box}
 body{margin:0;background:var(--bg);color:var(--txt);
      font-family:system-ui,-apple-system,'Pretendard',sans-serif;padding:16px}
 h1{font-size:18px;margin:0 0 14px}
 h2{font-size:13px;color:var(--dim);margin:0 0 10px;font-weight:600;
    letter-spacing:.04em;text-transform:uppercase}

 .wrap{max-width:1280px;margin:0 auto}
 .cols{display:grid;grid-template-columns:minmax(0,1.35fr) minmax(340px,1fr);
       gap:16px;align-items:start}
 .left{position:sticky;top:16px}
 .right{display:flex;flex-direction:column;gap:12px}
 @media(max-width:900px){
   .cols{grid-template-columns:1fr}
   .left{position:static}
 }

 .card{background:var(--card);border:1px solid var(--line);
       border-radius:14px;padding:14px}
 .stage{padding:10px}
 img{width:100%;border-radius:10px;background:#000;display:block}

 .row{display:flex;gap:8px;flex-wrap:wrap}
 .row+.row{margin-top:8px}
 button{flex:1;min-width:90px;padding:11px;border:0;border-radius:10px;
        background:#2a3054;color:var(--txt);font-size:14px;font-weight:600;
        cursor:pointer;transition:transform .06s}
 button:active{transform:scale(.97);opacity:.8}
 .w{background:#e8ecff;color:#101425}
 .u{background:var(--uv);color:#fff}
 .go{background:var(--acc);color:#fff}
 .wide{flex-basis:100%}

 label{display:block;font-size:12px;color:var(--dim);margin:10px 0 4px}
 label:first-child{margin-top:0}
 input[type=range]{width:100%;accent-color:var(--acc)}
 select{width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);
        background:#12162a;color:var(--txt);font-size:14px}

 .badge{display:inline-block;padding:4px 12px;border-radius:999px;
        font-size:12px;font-weight:700;background:#2a3054}
 .badge.on-w{background:#e8ecff;color:#101425}
 .badge.on-u{background:var(--uv);color:#fff}
 .meta{display:flex;justify-content:space-between;align-items:center;
       font-size:12px;color:var(--dim);margin-top:10px}

 .hint{font-size:12px;color:var(--dim);line-height:1.7;
       background:#12162a;border-radius:8px;padding:10px;margin-top:10px}
 .hint b{color:var(--txt)}
 .dot{display:inline-block;width:7px;height:7px;border-radius:50%;
      background:var(--ok);margin-right:6px;vertical-align:middle}

 #log{font-size:12px;color:var(--dim);white-space:pre-wrap;word-break:break-all;
      max-height:180px;overflow:auto;line-height:1.6}
</style></head><body><div class="wrap">

<h1>HabiTooth Wand</h1>

<div class="cols">

  <div class="left">
    <div class="card stage">
      <img id="v" src="">
      <div class="meta">
        <span>실시간 스트림</span>
        <span>조명 <b class="badge" id="cur">OFF</b></span>
      </div>
    </div>
  </div>

  <div class="right">

    <div class="card">
      <h2>조명</h2>
      <div class="row">
        <button class="w" onclick="led('white')">백색광</button>
        <button class="u" onclick="led('uv')">UV</button>
        <button       onclick="led('off')">OFF</button>
      </div>
      <label>백색 밝기 <span id="bw">255</span></label>
      <input type="range" min="0" max="255" value="255"
            oninput="bw.textContent=this.value" onchange="bright('w',this.value)">
      <label>UV 밝기 <span id="bu">255</span></label>
      <input type="range" min="0" max="255" value="255"
            oninput="bu.textContent=this.value" onchange="bright('uv',this.value)">
    </div>

    <div class="card">
      <h2>노출 (수동 고정)</h2>
      <label>UV 노출 aec <span id="ae">200</span></label>
      <input id="aeR" type="range" min="0" max="1200" step="10" value="200"
             oninput="ae.textContent=this.value" onchange="exp('aec',this.value)">
      <label>백색 노출 waec <span id="wa">750</span></label>
      <input id="waR" type="range" min="0" max="1200" step="10" value="750"
            oninput="wa.textContent=this.value" onchange="exp('waec',this.value)">
      <label>UV 게인 agc <span id="ag">4</span></label>
      <input id="agR" type="range" min="0" max="30" value="4"
             oninput="ag.textContent=this.value" onchange="exp('agc',this.value)">
      <div class="hint">
        조명 버튼을 누르면 해당 모드 노출이 스트림에 바로 적용됩니다.<br>
        <b>흰 종이를 화면에 꽉 채우고</b> 백색 노출을 올려
        회색이 아닌 흰색으로 보이는 값을 찾으세요.
      </div>
    </div>

    <div class="card">
      <h2>촬영 설정</h2>
      <label>촬영 구역 (viewType)</label>
      <select id="view" onchange="setView()">
        <optgroup label="설측 상악">
          <option value="UPPER_LEFT">UPPER_LEFT 상악 좌</option>
          <option value="UPPER_CENTER">UPPER_CENTER 상악 중앙</option>
          <option value="UPPER_RIGHT">UPPER_RIGHT 상악 우</option>
        </optgroup>
        <optgroup label="설측 하악">
          <option value="LOWER_LEFT">LOWER_LEFT 하악 좌</option>
          <option value="LOWER_CENTER">LOWER_CENTER 하악 중앙</option>
          <option value="LOWER_RIGHT">LOWER_RIGHT 하악 우</option>
        </optgroup>
        <optgroup label="외측">
          <option value="OUTER_LEFT">OUTER_LEFT 외측 좌</option>
          <option value="OUTER_CENTER" selected>OUTER_CENTER 외측 중앙</option>
          <option value="OUTER_RIGHT">OUTER_RIGHT 외측 우</option>
        </optgroup>
      </select>
      <label>해상도</label>
      <select onchange="res(this.value)">
        <option value="6">VGA 640x480</option>
        <option value="8">SVGA 800x600</option>
        <option value="9">XGA 1024x768</option>
        <option value="10">HD 1280x720</option>
        <option value="13">UXGA 1600x1200</option>
        <option value="4">QVGA 320x240</option>
      </select>
    </div>

    <div class="card">
      <h2>촬영</h2>
      <div class="row">
        <button class="go" onclick="snap('white')">백색 촬영</button>
        <button class="go" onclick="snap('uv')">UV 촬영</button>
      </div>
      <div class="row">
        <button class="wide" onclick="pair()">백색 + UV 연속</button>
      </div>
      <div class="hint">
        <span class="dot"></span><b>디바이스 스위치</b>를 누르면 백색 + UV 연속 촬영됩니다.<br>
        <span style="color:#8b93c4">※ 이 페이지가 열려 있어야 파일이 저장됩니다</span>
      </div>
    </div>

    <div class="card">
      <h2>네오픽셀</h2>
      <div class="row">
        <button onclick="neo('off')">OFF</button>
        <button onclick="neo('g')" style="background:#0f7a3f">GREEN</button>
        <button onclick="neo('r')" style="background:#a53030">RED</button>
        <button onclick="neo('b')" style="background:#3060a5">BLUE</button>
      </div>
    </div>

    <div class="card">
      <h2>로그</h2>
      <div id="log">준비됨</div>
    </div>

  </div>
</div>

<script>
const H = location.hostname;
document.getElementById('v').src = 'http://' + H + ':81/stream';

function log(t){
  const l = document.getElementById('log');
  l.textContent = new Date().toLocaleTimeString() + '  ' + t + '\n' + l.textContent;
}

async function led(m){
  const r = await fetch('/led?mode=' + m);
  const s = await r.text();
  const b = document.getElementById('cur');
  b.textContent = s;
  b.className = 'badge' + (s === 'WHITE' ? ' on-w' : s === 'UV' ? ' on-u' : '');
}
async function bright(k, v){ await fetch('/bright?' + k + '=' + v); }
async function exp(k, v){ await fetch('/exp?' + k + '=' + v); log(k + ' = ' + v); }
async function res(v){ await fetch('/res?v=' + v); log('해상도 변경'); }
async function setView(){ await fetch('/view?v=' + view.value); log('구역: ' + view.value); }
async function neo(c){ await fetch('/neo?c=' + c); }

async function loadExp(){
  try{
    const d = await (await fetch('/exp')).json();
    aeR.value = d.aec;   ae.textContent = d.aec;
    waR.value = d.waec;  wa.textContent = d.waec;
    agR.value = d.agc;   ag.textContent = d.agc;
  }catch(e){}
}
loadExp();

function snap(l){
  log(l.toUpperCase() + ' 촬영 → 다운로드');
  const a = document.createElement('a');
  a.href = '/snap?light=' + l + '&view=' + encodeURIComponent(view.value) + '&t=' + Date.now();
  a.download = '';
  document.body.appendChild(a); a.click(); a.remove();
}
function pair(){
  snap('white');
  setTimeout(() => snap('uv'), 1800);
}

function grab(i){
  const a = document.createElement('a');
  a.href = '/pend?i=' + i + '&t=' + Date.now();
  a.download = '';
  document.body.appendChild(a); a.click(); a.remove();
}

let lastSeq = null;
async function poll(){
  try {
    const r = await fetch('/pending');
    const d = await r.json();

    if (lastSeq === null) { lastSeq = d.seq; }
    else if (d.seq > lastSeq) {
      lastSeq = d.seq;
      log('스위치 촬영 수신: ' + d.w);
      grab(0);
      setTimeout(() => grab(1), 900);
    }
  } catch(e) {}
}
setInterval(poll, 1200);
poll();
</script>

</div></body></html>
)HTML";

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
  setLed(currentLed);
  server.send(200, "text/plain", "ok");
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
