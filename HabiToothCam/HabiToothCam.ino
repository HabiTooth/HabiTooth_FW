/*
 * HabiTooth Wand - Camera Web Server + White/UV LED Control
 * Board: Seeed XIAO ESP32S3 Sense
 *
 * ===== 핀맵 (2026-08 실측 확인) =====
 *   PIN_SW      = GPIO2 (D1)  스위치, 액티브-로우
 *   PIN_UV_LED  = GPIO5 (D4)  UV BCR421U EN
 *   PIN_W_LED   = GPIO6 (D5)  백색 BCR421U EN
 *   GPIO4 (D3)                미사용 - 무반응 확인됨
 *
 * ===== 노출 제어 (2026-08 추가) =====
 *   AEC/AWB를 수동 고정. 데이터셋 색·밝기 일관성 확보가 목적.
 *   자동으로 두면 화면 내용에 따라 노출과 색이 매번 달라져
 *   같은 치석이 사진마다 다른 색으로 찍힘 -> 학습 불가.
 *
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

#include "board_config.h"
#include "config.h"

// ── LED / 스위치 핀 ───────────────────────────
#define PIN_UV_LED   5
#define PIN_W_LED    6
#define PIN_SW       2

#define LED_ACTIVE_HIGH  1

#define PWM_FREQ   5000
#define PWM_RES    8
int brightW  = 255;
int brightUV = 255;

#define DEBOUNCE_MS  30

// ── 노출 설정 (OV3660: aec 0~1200, agc 0~30) ──
int g_uvAec    = 200;   // FireBeetle 스윕에서 확정된 값
int g_uvAgc    = 4;
int g_whiteAec = 750;   // 흰 종이가 회색으로 나오면 올릴 것
int g_whiteAgc = 0;

// ── 촬영 구역 ─────────────────────────────────
String VIEW_TYPE = "OUTER_CENTER";

// ── 전역 ──────────────────────────────────────
WebServer      server(80);
WiFiServer     streamServer(81);
SemaphoreHandle_t camMutex;

enum LedMode { LED_OFF = 0, LED_WHITE, LED_UV };
LedMode currentLed = LED_OFF;

uint8_t*          pendBuf[2] = { nullptr, nullptr };
size_t            pendLen[2] = { 0, 0 };
String            pendName[2];
volatile uint32_t pendSeq    = 0;
volatile bool     capturing  = false;

// ─────────────────────────────────────────────
// 센서 프로파일
// ─────────────────────────────────────────────

// 공통: AEC 자동 제어 차단
// AWB는 조명 모드별로 다르므로 여기서 건드리지 않음
void applyCommonProfile(sensor_t* s) {
  if (!s) return;

  // AEC/AGC 수동 고정
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

// UV(405nm) 형광 모드
void applyUvProfile(sensor_t* s) {
  if (!s) return;
  applyCommonProfile(s);

  s->set_whitebal(s, 0);      // UV는 AWB 끔
  s->set_awb_gain(s, 0);
  s->set_wb_mode(s, 0);

  s->set_aec_value(s, g_uvAec);
  s->set_agc_gain(s, g_uvAgc);

  // OV3660 기본 분기의 saturation -2 / brightness +1을 덮어씀.
  // 형광 촬영에는 정반대 방향임.
  s->set_saturation(s, 2);
  s->set_contrast(s, 1);
  s->set_brightness(s, 0);
}

// 백색광 모드
void applyWhiteProfile(sensor_t* s) {
  if (!s) return;
  applyCommonProfile(s);

  s->set_whitebal(s, 1);      // 백색광은 AWB 켬
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);

  s->set_aec_value(s, g_whiteAec);
  s->set_agc_gain(s, g_whiteAgc);

  s->set_saturation(s, 0);
  s->set_contrast(s, 0);
  s->set_brightness(s, 0);
}

void applyProfileFor(LedMode m) {
  sensor_t* s = esp_camera_sensor_get();
  if (m == LED_UV) applyUvProfile(s);
  else             applyWhiteProfile(s);
}

// ─────────────────────────────────────────────
// LED 제어
// ─────────────────────────────────────────────
void ledInit() {
  ledcAttach(PIN_UV_LED, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_W_LED,  PWM_FREQ, PWM_RES);
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

void setLed(LedMode m) {
  currentLed = m;
  switch (m) {
    case LED_WHITE:
      ledcWrite(PIN_UV_LED, duty(0));
      ledcWrite(PIN_W_LED,  duty(brightW));
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
  // 조명이 바뀌면 센서 프로파일도 함께 전환.
  // 스트림 화면에서도 실제 촬영과 같은 노출로 보이게 하기 위함.
  if (m != LED_OFF) applyProfileFor(m);
}

const char* ledName() {
  return currentLed == LED_WHITE ? "WHITE" : (currentLed == LED_UV ? "UV" : "OFF");
}

// ─────────────────────────────────────────────
// WiFi
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
      return;
    }
    WiFi.disconnect();
    delay(500);
  }
  Serial.println("\nWiFi connect failed");
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
//   프로파일 적용은 다음 프레임부터 반영되므로
//   앞쪽 프레임을 버리고 마지막 것만 사용.
// ─────────────────────────────────────────────
camera_fb_t* captureWithLight(LedMode mode, int warmupMs = 350, int discard = 3) {
  setLed(mode);              // 내부에서 프로파일도 함께 적용됨
  applyProfileFor(mode);     // 명시적으로 한 번 더 (LED_OFF 경유 시 대비)
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
  if (stableSw == HIGH) capturePair();
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

// 부팅 시 보드의 현재 노출값을 슬라이더에 반영
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
  else                   setLed(LED_OFF);
  server.send(200, "text/plain", ledName());
}

void handleBright() {
  if (server.hasArg("w"))  brightW  = constrain(server.arg("w").toInt(),  0, 255);
  if (server.hasArg("uv")) brightUV = constrain(server.arg("uv").toInt(), 0, 255);
  setLed(currentLed);
  server.send(200, "text/plain", "ok");
}

// 노출 조회 / 설정
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

  // 현재 켜져 있는 조명 모드에 즉시 반영 (스트림에서 바로 확인 가능)
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
    // 해상도 변경 시 센서가 일부 설정을 되돌리므로 프로파일 재적용
    applyProfileFor(currentLed == LED_UV ? LED_UV : LED_WHITE);
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

  camera_fb_t* fb = captureWithLight(isUV ? LED_UV : LED_WHITE);
  if (!fb) {
    setLed(prev);
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
  xSemaphoreGive(camMutex);
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
  pinMode(PIN_SW, INPUT_PULLUP);

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
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_VGA);
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);

  // 기존 OV3660 분기(saturation -2, brightness +1)는 제거함.
  // 형광 촬영에 불리하고, 아래 프로파일에서 조명별로 다시 지정함.
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
  server.begin();

  xTaskCreatePinnedToCore(streamTask, "stream", 8192, NULL, 1, NULL, 0);

  Serial.println("========================================");
  Serial.print  ("  UI     : http://"); Serial.println(WiFi.localIP());
  Serial.print  ("  Stream : http://"); Serial.print(WiFi.localIP()); Serial.println(":81/stream");
  Serial.println("========================================");
  printHelp();
  printStatus();
}

void loop() {
  server.handleClient();
  swTask();

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if      (cmd == "w")  { setLed(LED_WHITE); Serial.println("WHITE"); }
    else if (cmd == "u")  { setLed(LED_UV);    Serial.println("UV"); }
    else if (cmd == "o")  { setLed(LED_OFF);   Serial.println("OFF"); }
    else if (cmd == "c")  { capturePair(); }
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
    else if (cmd == "status") { printStatus(); }
    else if (cmd == "help")   { printHelp(); }
    else if (cmd.length())    { Serial.println("[ERR] 알 수 없는 명령. 'help'"); }
  }
  delay(2);
}
