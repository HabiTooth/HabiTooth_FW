/*
 * HabiTooth Wand - Camera Web Server + White/UV LED + NeoPixel
 * Board: Seeed XIAO ESP32S3 Sense
 *
 * ===== 핀맵 (2026-09 신 회로 - 확정본) =====
 *   PIN_SW      = GPIO2 (D1)  스위치, 액티브-로우 (외부 10k 풀업 + 100nF)
 *   PIN_UV_LED  = GPIO4 (D3)  UV  AO3400 게이트 (active-HIGH, 100k 풀다운)
 *   PIN_W_LED   = GPIO5 (D4)  백색 AO3400 게이트 (active-HIGH, 100k 풀다운)
 *   PIN_NEO     = GPIO6 (D5)  네오픽셀 DIN (330R 경유)
 *   ※ 배터리 전압 / 충전 상태 신호는 XIAO에 연결돼 있지 않음
 *     → 배터리 부족, 충전 중, 완충 표시는 하지 않음 (코드로 감지 불가)
 *
 * ===== 버튼 동작 (2026-10 최종안) =====
 *   버튼은 하나 (전원 + 촬영 겸용). 시연 기준.
 *   - 절전 상태에서 짧게 누름   → 켜기 (딥슬립 웨이크 = 재부팅)
 *       깨울 때 누른 버튼을 떼는 동작은 촬영으로 처리하지 않음
 *   - 켜진 상태에서 짧게 누름   → 백색 + UV 연속 촬영 (뗄 때 촬영)
 *   - 켜진 상태에서 2초 길게    → 절전 (딥슬립)
 *       2초가 되면 네오픽셀이 켜져서 알려주고, 손을 떼면 페이드아웃 후 절전
 *       (누른 채로 잠들면 LOW 레벨 때문에 바로 다시 깨므로 반드시 뗀 뒤 진입)
 *   - 자동 절전 없음
 *   ※ 딥슬립은 완전 차단이 아님 (부스터, 3V3, 네오픽셀 대기전류는 남음)
 *   ※ TODO: 부스터 모듈이 저부하에서 출력을 끊는지 테스트할 것
 *     (절전 후 1~2분 뒤 버튼으로 깨워보기. 시리얼 'sleep' 명령으로도 진입 가능)
 *
 * ===== 네오픽셀 상태 (2026-10 최종안, 색 + 패턴) =====
 *   부팅 / 와이파이 스캔·연결 중  파랑   숨쉬기 (천천히 밝아졌다 어두워짐 반복)
 *     (자동 재연결 포함)
 *   와이파이 연결됨           초록   1.5초 켜졌다 꺼짐
 *   와이파이 실패             빨강   빠른 점멸 (10초 뒤 자동 재스캔 → 다시 파랑)
 *   LED 쿨다운 중             주황   느린 점멸 (과열 보호로 강제 소등된 경우만)
 *   절전 진입                 현재 색 페이드아웃 (꺼져 있었으면 흰색)
 *   촬영 중 / 대기            꺼짐   (촬영 중 빛이 사진에 섞이지 않게)
 *   카메라 초기화 오류        빨강   계속 켜짐
 *     (PSRAM 버퍼 확보 실패는 시리얼 경고만, 동작은 계속)
 *   - 꺼진(절전) 상태에선 아무것도 표시하지 않음
 *   - 우선순위: 오류 > 절전 대기 > 촬영 > 와이파이 연결중/실패 > 쿨다운 > 연결됨
 *
 * ===== 와이파이 우선순위 (2026-10-28) =====
 *   config.h 의 WIFI_SSID_1 ~ 4 순서가 곧 우선순위.
 *     1번: 팀 핫스팟 (교수님 심사용 - 데이터 연결로 안정화)
 *     2번~: 행사장 / 학교 와이파이 (일반인 시연용)
 *   부팅·재연결 시 먼저 주변을 스캔해서, 등록된 SSID 중 실제로 보이는 것만
 *   순서대로 시도한다. 핫스팟이 꺼져 있으면 기다리지 않고 바로 다음으로 넘어감.
 *   스캔에 하나도 안 잡히면(숨김 SSID 등) 예전처럼 전부 순서대로 시도.
 *   ※ ESP32-S3는 2.4GHz 전용 → 핫스팟을 2.4GHz로 (아이폰: "최대 호환성 모드")
 *   ※ 행사장 와이파이는 AP 격리(클라이언트 간 통신 차단)가 걸려 있는 경우가 많아
 *     폰에서 habitooth.local / IP 접근이 안 될 수 있음 → 현장 사전 테스트 필수
 *     시리얼 'scan' 명령으로 그 자리에서 보이는 SSID / 채널 확인 가능
 *   ※ 캡티브 포털(동의 페이지)·WPA2-Enterprise(아이디+비번) 와이파이는 사용 불가
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
 * ===== 연속 촬영 속도 (2026-09 수정) =====
 *   - captureWithLight: 워밍업 350ms -> 120ms, 버림 프레임 사이 delay(60) 제거,
 *     applyProfileFor 중복 호출 제거 (setLed가 이미 호출함)
 *   - capturePair: 백색 이미지를 버퍼에 복사하기 전에 UV LED를 미리 점등
 *   - 웹 "백색 + UV 연속" 버튼이 /pair 로 capturePair를 직접 호출
 *   ※ 워밍업을 줄였으므로 첫 장 색이 이전 설정으로 찍히지 않는지 확인할 것.
 *     문제가 있으면 warmupMs보다 discard를 3으로 올리는 편이 빠름.
 *
 * ===== 접속 =====
 *   UI     : http://habitooth.local/
 *   스트림 : http://habitooth.local:81/stream
 */

#include <Arduino.h>
#include "esp_camera.h"
#include "esp_sleep.h"
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

#define DEBOUNCE_MS     30
#define LONG_PRESS_MS   2000     // 이 시간 이상 누르면 절전
#define MDNS_NAME  "habitooth"

// ── 와이파이 재시도 ───────────────────────────
#define WIFI_SCAN_MS    8000UL   // 주변 스캔 최대 대기
#define WIFI_TRY_MS     10000UL  // SSID 하나당 연결 시도 시간
#define WIFI_RETRY_MS   10000UL  // 전부 실패 후 재스캔까지 대기 (빨강 점멸)
#define WIFI_OK_SHOW_MS  1500UL  // 연결 성공 초록 표시 시간

enum LedMode { LED_OFF = 0, LED_WHITE, LED_UV, LED_PREVIEW };
enum NetState { NET_IDLE = 0, NET_SCANNING, NET_TRYING, NET_CONNECTED, NET_FAILWAIT };

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
SemaphoreHandle_t camMutex = nullptr;

Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEO, NEO_GRB + NEO_KHZ800);

LedMode currentLed = LED_OFF;

// ── 프리뷰(상시) 조명 ─────────────────────────
int g_previewBright = 60;
int g_previewAec    = 750;
int g_previewAgc    = 0;

volatile int streamClients = 0;
uint32_t previewSince    = 0;
uint32_t previewCoolUntil = 0;
bool     g_guardCool      = false;   // 과열 보호로 인한 쿨다운인지 (수동 OFF와 구분)

#define PREVIEW_MAX_MS   180000UL
#define PREVIEW_COOL_MS   30000UL
#define WHITE_MAX_MS      60000UL
#define PEND_CAP  (512 * 1024)

framesize_t g_streamFs  = FRAMESIZE_VGA;
framesize_t g_captureFs = FRAMESIZE_UXGA;

uint8_t*          pendBuf[2] = { nullptr, nullptr };
size_t            pendCap[2] = { 0, 0 };
size_t            pendLen[2] = { 0, 0 };
String            pendName[2];
volatile uint32_t pendSeq    = 0;
volatile bool     capturing  = false;

// ── 상태 플래그 ───────────────────────────────
bool     g_fatal         = false;   // 카메라 초기화 오류
bool     g_sleepArmed    = false;   // 2초 길게 누름 → 손 떼면 절전
uint32_t g_sleepColor    = 0;
bool     g_serverStarted = false;
uint32_t g_whiteTestUntil = 0;      // 첫 연결 시 백색 LED 3초 점검

// ── 와이파이 상태 ─────────────────────────────
NetState netState      = NET_IDLE;
int      netIdx        = 0;         // 지금 시도 중인 config.h SSID 번호
uint32_t netT          = 0;
bool     everConnected = false;
uint32_t g_okUntil     = 0;

// ── 네오픽셀 상태 ─────────────────────────────
uint32_t g_neoLast = 0xFFFFFFFF;    // 마지막으로 내보낸 색 (중복 show 방지)
uint32_t g_neoBase = 0;             // 현재 상태의 기준 색 (절전 페이드용)
bool     g_neoTest = false;         // 테스트 명령으로 수동 색 표시 중

void enterDeepSleep();

// ─────────────────────────────────────────────
// 네오픽셀 헬퍼
// ─────────────────────────────────────────────
#define NEO_BLUE    strip.Color(0, 0, 60)
#define NEO_GREEN   strip.Color(0, 40, 0)
#define NEO_RED     strip.Color(60, 0, 0)
#define NEO_ORANGE  strip.Color(60, 20, 0)
#define NEO_SOFTW   strip.Color(30, 30, 30)

void neoFill(uint32_t c) {
  for (int i = 0; i < NUM_PIXELS; i++) strip.setPixelColor(i, c);
  strip.show();
  g_neoLast = c;
}
void neoOff() { neoFill(0); }

uint32_t neoScale(uint32_t c, float k) {
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  return strip.Color((uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k));
}

// 숨쉬기: 2초 주기로 부드럽게 밝아졌다 어두워짐
uint32_t neoBreathe(uint32_t c, uint32_t now) {
  float ph = (now % 2000) / 2000.0f;
  float lv = (1.0f - cosf(2.0f * PI * ph)) * 0.5f;   // 0~1
  lv = 0.06f + 0.94f * lv * lv;                       // 눈에 자연스럽게 + 완전 꺼짐 방지
  return neoScale(c, lv);
}

// 점멸: onMs 켜짐 / offMs 꺼짐
uint32_t neoBlink(uint32_t c, uint32_t now, uint32_t onMs, uint32_t offMs) {
  return (now % (onMs + offMs)) < onMs ? c : 0;
}

// 우선순위: 오류 > 절전 대기 > 촬영 > 와이파이 연결중/실패 > 쿨다운 > 연결됨 > 꺼짐
uint32_t neoCompute(uint32_t now) {
  if (g_fatal)      { g_neoBase = NEO_RED;      return NEO_RED; }
  if (g_sleepArmed) { g_neoBase = g_sleepColor; return g_sleepColor; }
  if (capturing)    { g_neoBase = 0;            return 0; }

  if (netState == NET_TRYING || netState == NET_SCANNING || netState == NET_IDLE) {
    g_neoBase = NEO_BLUE;  return neoBreathe(NEO_BLUE, now);
  }
  if (netState == NET_FAILWAIT) {
    g_neoBase = NEO_RED;   return neoBlink(NEO_RED, now, 120, 120);
  }
  if (g_guardCool && (int32_t)(previewCoolUntil - now) > 0) {
    g_neoBase = NEO_ORANGE; return neoBlink(NEO_ORANGE, now, 500, 1000);
  }
  if ((int32_t)(g_okUntil - now) > 0) {
    g_neoBase = NEO_GREEN; return NEO_GREEN;
  }
  g_neoBase = 0;
  return 0;
}

void neoTask() {
  static uint32_t tLast = 0;
  uint32_t now = millis();
  if (now - tLast < 20) return;      // 50Hz 갱신
  tLast = now;
  if (g_neoTest) return;

  uint32_t c = neoCompute(now);
  if (c != g_neoLast) neoFill(c);
}

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
// WiFi - 스캔 후 config.h 순서대로 "보이는" SSID만 시도 (비동기, 끊기면 자동 재연결)
//   NET_SCANNING : 주변 AP 비동기 스캔 (파랑 숨쉬기, loop 안 멈춤)
//   NET_TRYING   : 파랑 숨쉬기
//   NET_FAILWAIT : 빨강 빠른 점멸, WIFI_RETRY_MS 뒤 다시 스캔부터
//   NET_CONNECTED: 초록 1.5초 후 꺼짐
//   config.h 순서 = 우선순위 (1번 핫스팟, 2번~ 행사장 와이파이)
//   스캔에 하나도 안 잡히면(숨김 SSID 등) 전부 순서대로 시도 (예전 동작)
// ─────────────────────────────────────────────
static const char* const SSIDS[] = {
  WIFI_SSID_1, WIFI_SSID_2, WIFI_SSID_3, WIFI_SSID_4
};
static const char* const PASSWORDS[] = {
  WIFI_PASSWORD_1, WIFI_PASSWORD_2, WIFI_PASSWORD_3, WIFI_PASSWORD_4
};
static const int NUM_SSID = sizeof(SSIDS) / sizeof(SSIDS[0]);

int netOrder[NUM_SSID];   // 이번 라운드에 시도할 config.h 번호들 (우선순위 순)
int netCount = 0;
int netPos   = 0;

void streamTask(void* pv);

void netStartScan(uint32_t now) {
  WiFi.disconnect();
  WiFi.scanDelete();
  WiFi.scanNetworks(true);          // 비동기 → 네오픽셀 숨쉬기 유지
  netState = NET_SCANNING;
  netT = now;
  Serial.println("[WIFI] 주변 네트워크 스캔...");
}

// 스캔 결과(found개) 중 등록된 SSID만 config.h 순서로 netOrder에 담는다
void netBuildOrder(int found) {
  netCount = 0;
  for (int i = 0; i < NUM_SSID; i++) {
    if (strlen(SSIDS[i]) == 0) continue;
    bool seen = false;
    int  best = -127;
    for (int k = 0; k < found; k++) {
      if (WiFi.SSID(k) == SSIDS[i]) {
        seen = true;
        if (WiFi.RSSI(k) > best) best = WiFi.RSSI(k);
      }
    }
    if (seen) {
      netOrder[netCount++] = i;
      Serial.printf("[WIFI]  %d순위  %s  (%d dBm)\n", netCount, SSIDS[i], best);
    }
  }
  if (netCount == 0) {
    Serial.println("[WIFI] 등록된 SSID가 안 보임 - 전부 순서대로 시도");
    for (int i = 0; i < NUM_SSID; i++)
      if (strlen(SSIDS[i])) netOrder[netCount++] = i;
  }
  netPos = 0;
}

void netTryNext(uint32_t now) {
  if (netPos >= netCount) {
    netState = NET_FAILWAIT;
    netT = now;
    Serial.printf("[WIFI] 전부 실패 - %lu초 후 재스캔\n", WIFI_RETRY_MS / 1000);
    return;
  }
  netIdx = netOrder[netPos];
  WiFi.disconnect();
  WiFi.begin(SSIDS[netIdx], PASSWORDS[netIdx]);
  Serial.printf("[WIFI] Trying %s ...\n", SSIDS[netIdx]);
  netState = NET_TRYING;
  netT = now;
}

void onWifiConnected(uint32_t now) {
  netState = NET_CONNECTED;
  g_okUntil = now + WIFI_OK_SHOW_MS;
  Serial.println("[WIFI] connected: " + WiFi.SSID() + "  " + WiFi.localIP().toString());

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", MDNS_NAME);
  }

  if (!everConnected) {           // 첫 연결 때만 서버 시작
    everConnected = true;
    server.begin();
    g_serverStarted = true;
    xTaskCreatePinnedToCore(streamTask, "stream", 8192, NULL, 1, NULL, 0);

    setLed(LED_WHITE);            // 백색 LED 3초 점검 (비동기로 끔)
    g_whiteTestUntil = now + 3000;

    Serial.println("========================================");
    Serial.print  ("  UI     : http://"); Serial.println(WiFi.localIP());
    Serial.print  ("  Stream : http://"); Serial.print(WiFi.localIP()); Serial.println(":81/stream");
    Serial.println("========================================");
  }
}

void wifiTask() {
  uint32_t now = millis();
  switch (netState) {
    case NET_IDLE:
      netStartScan(now);
      break;

    case NET_SCANNING: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        if (now - netT > WIFI_SCAN_MS) {
          Serial.println("[WIFI] 스캔 시간 초과 - 전부 순서대로 시도");
          netBuildOrder(0);
          netTryNext(now);
        }
        break;
      }
      if (n < 0) n = 0;               // WIFI_SCAN_FAILED → 전부 시도
      netBuildOrder(n);
      WiFi.scanDelete();
      netTryNext(now);
      break;
    }

    case NET_TRYING:
      if (WiFi.status() == WL_CONNECTED) onWifiConnected(now);
      else if (now - netT > WIFI_TRY_MS) { netPos++; netTryNext(now); }
      break;

    case NET_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] 끊김 - 재스캔 후 자동 재연결");
        MDNS.end();
        netState = NET_IDLE;        // 다음 loop에서 스캔부터
      }
      break;

    case NET_FAILWAIT:
      if (now - netT > WIFI_RETRY_MS) netState = NET_IDLE;
      break;
  }
}

const char* netName() {
  switch (netState) {
    case NET_SCANNING:  return "스캔 중";
    case NET_TRYING:    return "연결 중";
    case NET_CONNECTED: return "연결됨";
    case NET_FAILWAIT:  return "실패(재스캔 대기)";
    default:            return "대기";
  }
}

// 시리얼 'scan' - 현장에서 보이는 네트워크 확인용 (동기, 몇 초 멈춤)
void printScan() {
  if (netState == NET_SCANNING) { Serial.println("[SCAN] 자동 스캔 중 - 잠시 후 다시"); return; }
  Serial.println("[SCAN] 스캔 중 (2~4초)...");
  int n = WiFi.scanNetworks();
  if (n <= 0) { Serial.println("[SCAN] 보이는 네트워크 없음"); WiFi.scanDelete(); return; }
  for (int k = 0; k < n; k++) {
    bool reg = false;
    for (int i = 0; i < NUM_SSID; i++) if (strlen(SSIDS[i]) && WiFi.SSID(k) == SSIDS[i]) reg = true;
    Serial.printf("  %2d. %-24s %4d dBm  ch%-2d  %s%s\n",
                  k + 1, WiFi.SSID(k).c_str(), WiFi.RSSI(k), WiFi.channel(k),
                  WiFi.encryptionType(k) == WIFI_AUTH_OPEN ? "open" : "secured",
                  reg ? "   <- config.h 등록됨" : "");
  }
  WiFi.scanDelete();
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

// 해상도
void setFs(framesize_t fs) {
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->status.framesize != fs) s->set_framesize(s, fs);
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
// 스위치 촬영: 백색 + UV  (촬영 중 네오픽셀 꺼짐)
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
  if (capturing || g_fatal || !camMutex) return;
  capturing = true;
  neoOff();                       // 촬영 중 네오픽셀 빛이 사진에 섞이지 않게

  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
    Serial.println("[PAIR] camera busy");
    capturing = false;
    return;
  }

  Serial.printf("[PAIR] %s 촬영 시작\n", VIEW_TYPE.c_str());
  uint32_t t0 = millis();
  bool ok = true;
  setFs(g_captureFs);

  for (int i = 0; i < 2; i++) {
    bool isUV = (i == 1);

    camera_fb_t* fb = captureWithLight(isUV ? LED_UV : LED_WHITE);
    if (!fb) { Serial.println("[PAIR] capture failed"); ok = false; break; }

    // 백색 이미지를 복사하는 동안 UV 조명을 미리 켜둔다 (워밍업 선점)
    if (!isUV) setLed(LED_UV);

    String name = makeFilename(isUV);
    if (storePending(i, fb, name))
      Serial.printf("  [%d] %s  %u bytes  (aec=%d)\n", i, name.c_str(), fb->len,
                    isUV ? g_uvAec : g_whiteAec);
    else { ok = false; esp_camera_fb_return(fb); break; }

    esp_camera_fb_return(fb);
  }

  setFs(g_streamFs);
  setLed(LED_OFF);
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
//   짧게 눌렀다 뗌 → 촬영
//   2초 이상 누름  → 네오픽셀 켜서 알림, 손 떼면 절전
//   부팅 시 이미 눌려 있으면(웨이크 누름) 그 릴리즈는 무시
// ─────────────────────────────────────────────
void swTask() {
  static bool     inited        = false;
  static bool     lastRaw       = HIGH;
  static bool     stableSw      = HIGH;
  static uint32_t tEdge         = 0;
  static uint32_t tPress        = 0;
  static bool     ignoreRelease = false;

  bool     raw = digitalRead(PIN_SW);
  uint32_t now = millis();

  if (!inited) {
    inited = true;
    lastRaw = stableSw = raw;
    if (raw == LOW) ignoreRelease = true;   // 깨울 때 누른 버튼
  }

  if (raw != lastRaw) { lastRaw = raw; tEdge = now; }

  if ((now - tEdge) >= DEBOUNCE_MS && raw != stableSw) {
    stableSw = raw;
    if (stableSw == LOW) {                  // 눌림
      tPress = now;
    } else {                                // 뗌
      if (ignoreRelease)    ignoreRelease = false;
      else if (g_sleepArmed) enterDeepSleep();
      else                   capturePair();
    }
  }

  // 길게 누름 감지 (누르고 있는 동안 2초가 차는 순간)
  if (stableSw == LOW && !ignoreRelease && !g_sleepArmed &&
      (now - tPress) >= LONG_PRESS_MS) {
    g_sleepColor = g_neoBase ? g_neoBase : NEO_SOFTW;   // 꺼져 있었으면 흰색
    g_sleepArmed = true;
    Serial.println("[PWR] 길게 누름 - 손을 떼면 절전");
  }
}

// ─────────────────────────────────────────────
// 절전 (딥슬립)
//   조명 끔 → 네오픽셀 페이드아웃 → 카메라 정지 → WiFi 끔 → 버튼 뗀 것 확인 → 슬립
//   LED 게이트는 100k 풀다운이 있어 슬립 중 오점등 없음
//   깨어나면 setup()부터 다시 실행됨
// ─────────────────────────────────────────────
void enterDeepSleep() {
  Serial.println("[PWR] 절전 진입");
  g_neoTest = false;

  if (!g_fatal) setLed(LED_OFF);
  else { ledcWrite(PIN_W_LED, 0); ledcWrite(PIN_UV_LED, 0); }

  // 페이드아웃 (약 0.8초)
  uint32_t c = g_sleepColor ? g_sleepColor : NEO_SOFTW;
  for (int i = 40; i >= 0; i--) {
    neoFill(neoScale(c, i / 40.0f));
    delay(20);
  }
  neoOff();

  // 카메라 정지 (스트림 태스크가 카메라를 못 잡게 뮤텍스 확보 후 반환 안 함)
  if (camMutex) xSemaphoreTake(camMutex, pdMS_TO_TICKS(2000));
  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_reg(s, 0x3008, 0xFF, 0x42);   // OV3660 소프트웨어 파워다운 (효과는 전류 측정으로 확인)
  esp_camera_deinit();

  MDNS.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // 버튼이 눌린 채로 잠들면 바로 깨어나므로 확실히 뗄 때까지 대기
  while (digitalRead(PIN_SW) == LOW) delay(10);
  delay(50);

  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_SW, 0);   // LOW(누름)에서 깨어남
  Serial.flush();
  esp_deep_sleep_start();
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
    g_guardCool = false;          // 수동 OFF는 과열 쿨다운 표시 안 함
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
  if (server.hasArg("v")) {
    g_streamFs = (framesize_t)server.arg("v").toInt();
    setFs(g_streamFs);
    delay(100);
    applyProfileFor(currentLed);
  }
  server.send(200, "text/plain", String((int)g_streamFs));
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
  if (capturing) { server.send(409, "text/plain", "busy"); return; }
  if (server.hasArg("view")) VIEW_TYPE = server.arg("view");
  bool isUV = (server.arg("light") == "uv");
  LedMode prev = currentLed;

  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
    server.send(503, "text/plain", "camera busy");
    return;
  }

  capturing = true;
  neoOff();                       // 촬영 중 네오픽셀 꺼짐

  camera_fb_t* fb = captureWithLight(isUV ? LED_UV : LED_WHITE);
  if (!fb) {
    setFs(g_streamFs);
    setLed(prev);
    xSemaphoreGive(camMutex);
    capturing = false;
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
  setFs(g_streamFs);
  setLed(prev);
  xSemaphoreGive(camMutex);
  capturing = false;
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

// 테스트용 수동 색 (off를 보내면 상태 표시로 복귀)
void handleNeo() {
  String c = server.arg("c");
  g_neoTest = (c != "off");
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
  Serial.printf("  WiFi      : %s", netName());
  if (netState == NET_CONNECTED)
    Serial.printf("  (%s  %s  %d dBm)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.println();
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
  Serial.println("  scan           주변 와이파이 목록 (현장 확인용)");
  Serial.println("  wifi           와이파이 재스캔·재연결");
  Serial.println("  sleep          절전 진입 (부스터 차단 테스트용)");
  Serial.println("  aec <0-1200>   UV 노출");
  Serial.println("  waec <0-1200>  백색광 노출");
  Serial.println("  agc <0-30>     UV 게인");
  Serial.println("  wb <0-4>       UV 화이트밸런스 (0auto 1sunny 2cloudy 3office 4home)");
  Serial.println("  bw <0-255>     백색 밝기    bu <0-255>  UV 밝기");
  Serial.println("  n              네오픽셀 색상 순환 (테스트)");
  Serial.println("  no             네오픽셀 테스트 종료 (상태 표시로 복귀)");
  Serial.println("  status / help");
  Serial.println("───────────────────────────────────────");
}

// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  // 네오픽셀/LED/스위치를 먼저 초기화 (부팅 잔상 제거)
  ledInit();
  pinMode(PIN_SW, INPUT);   // 외부 10k 풀업 사용 (신 회로)
  strip.begin();
  strip.setBrightness(255);
  neoOff();

  delay(800);

  esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
  Serial.printf("[BOOT] %s\n", wc == ESP_SLEEP_WAKEUP_EXT0 ? "버튼으로 절전 해제" : "전원 인가 / 리셋");
  Serial.printf("[BOOT] PSRAM %s  size=%u\n",
                psramFound() ? "감지됨" : "없음", ESP.getPsramSize());

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
    g_fatal = true;               // 빨강 계속 켜짐. 길게 눌러 절전은 가능
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, psramFound() ? g_streamFs : FRAMESIZE_SVGA);
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);

  setLed(LED_OFF);

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
    // 경고만 하고 계속 동작 (단일 촬영 /snap은 버퍼 없이도 가능)
    Serial.println("[PEND] 버퍼 확보 실패 - PSRAM 설정을 확인하세요");
  }
  Serial.printf("[MEM] PSRAM free = %u\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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

  // 와이파이는 loop의 wifiTask에서 비동기로 스캔·연결 (연결 중 파랑 숨쉬기)
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);   // 재연결은 wifiTask가 직접 관리
  netState = NET_IDLE;

  printHelp();
  printStatus();
}

void ledGuardTask() {
  if (capturing) return;

  uint32_t now = millis();

  if (currentLed == LED_PREVIEW && (now - previewSince) > PREVIEW_MAX_MS) {
    setLed(LED_OFF);
    previewCoolUntil = now + PREVIEW_COOL_MS;
    g_guardCool = true;           // 주황 느린 점멸
    Serial.println("[LED] 프리뷰 연속 점등 상한 - 강제 소등");
    return;
  }
  if (currentLed == LED_WHITE && (now - previewSince) > WHITE_MAX_MS) {
    setLed(LED_OFF);
    previewCoolUntil = now + PREVIEW_COOL_MS;
    g_guardCool = true;
    Serial.println("[LED] 백색 풀출력 상한 - 강제 소등");
    return;
  }
  if (g_guardCool && (int32_t)(previewCoolUntil - now) <= 0) g_guardCool = false;

  bool want = (streamClients > 0);

  if (want && currentLed == LED_OFF && (int32_t)(now - previewCoolUntil) >= 0) {
    setLed(LED_PREVIEW);
  } else if (!want && currentLed == LED_PREVIEW) {
    setLed(LED_OFF);
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if      (cmd == "w")  { setLed(LED_WHITE); Serial.println("WHITE"); }
  else if (cmd == "u")  { setLed(LED_UV);    Serial.println("UV"); }
  else if (cmd == "o")  { setLed(LED_OFF);   Serial.println("OFF"); }
  else if (cmd == "c")  { capturePair(); }
  else if (cmd == "sleep") { g_sleepColor = NEO_SOFTW; enterDeepSleep(); }
  else if (cmd == "scan")  { printScan(); }
  else if (cmd == "wifi")  {
    if (netState == NET_CONNECTED) MDNS.end();
    netState = NET_IDLE;
    Serial.println("[WIFI] 재스캔·재연결");
  }
  else if (cmd == "n")  {
    static int idx = 0;
    const uint32_t seq[] = {
      strip.Color(60,0,0), strip.Color(0,60,0), strip.Color(0,0,60),
      strip.Color(50,50,50), strip.Color(60,30,0), 0
    };
    g_neoTest = true;
    neoFill(seq[idx]);
    idx = (idx + 1) % 6;
    Serial.printf("[NEO] color %d\n", idx);
  }
  else if (cmd == "no") { g_neoTest = false; neoOff(); Serial.println("[NEO] 상태 표시로 복귀"); }
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

void loop() {
  swTask();
  neoTask();

  if (g_fatal) {                  // 오류 상태: 버튼(절전)과 표시만 동작
    handleSerial();
    delay(2);
    return;
  }

  wifiTask();
  if (g_serverStarted) server.handleClient();

  // 첫 연결 시 백색 LED 3초 점검 종료
  if (g_whiteTestUntil && (int32_t)(millis() - g_whiteTestUntil) >= 0) {
    g_whiteTestUntil = 0;
    if (currentLed == LED_WHITE) setLed(LED_OFF);
  }

  ledGuardTask();
  handleSerial();
  delay(2);
}
