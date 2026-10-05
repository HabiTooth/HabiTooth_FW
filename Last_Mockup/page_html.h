// HabiTooth Wand - 웹 UI (가이드샷 버전)
//   - 스트림 | 가이드샷을 나란히, 그 아래 구역 선택 + 촬영 버튼
//   - 가이드샷은 노트북 폴더를 한 번 선택하면 브라우저(IndexedDB)에 저장되어
//     이후 새로고침/재부팅해도 자동으로 불러옴. ESP32에는 저장하지 않음.
//   - 파일명 = 구역 이름 (확장자 무관, 대소문자 무관)
//     UPPER_CENTER / LOWER_CENTER 는 UPPER_FRONT / LOWER_FRONT 로 인식
#pragma once

static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="ko"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HabiTooth Wand</title>
<style>
 :root{--bg:#0f1220;--card:#191d33;--line:#2b3153;--txt:#e8ebff;
       --dim:#8b93c4;--acc:#7c8cff;--uv:#a855f7;--ok:#34d399}
 *{box-sizing:border-box}
 body{margin:0;background:var(--bg);color:var(--txt);
      font-family:system-ui,-apple-system,'Pretendard',sans-serif;padding:12px}
 h1{font-size:16px;margin:0 0 10px}
 h2{font-size:12px;color:var(--dim);margin:0 0 8px;font-weight:600;
    letter-spacing:.04em;text-transform:uppercase}

 .wrap{max-width:1400px;margin:0 auto}
 .cols{display:grid;grid-template-columns:minmax(0,1.7fr) minmax(300px,1fr);
       gap:12px;align-items:start}
 .left{position:sticky;top:12px;display:flex;flex-direction:column;gap:12px}
 .right{display:flex;flex-direction:column;gap:12px}
 @media(max-width:900px){
   .cols{grid-template-columns:1fr}
   .left{position:static}
 }

 .card{background:var(--card);border:1px solid var(--line);
       border-radius:14px;padding:12px}

 /* 스트림 | 가이드 */
 .views{display:grid;grid-template-columns:1fr 1fr;gap:10px}
 .pane{display:flex;flex-direction:column;gap:6px;min-width:0}
 .pane-h{display:flex;justify-content:space-between;align-items:center;
         font-size:12px;color:var(--dim);height:26px}
 .frame{width:100%;aspect-ratio:4/3;border-radius:10px;background:#000;
        object-fit:contain;display:block}
 .empty{display:flex;align-items:center;justify-content:center;text-align:center;
        font-size:12px;color:var(--dim);line-height:1.6;padding:10px}
 .zone{font-size:13px;font-weight:700;color:var(--txt)}

 .row{display:flex;gap:8px;flex-wrap:wrap}
 .row+.row{margin-top:8px}
 button{flex:1;min-width:80px;padding:11px;border:0;border-radius:10px;
        background:#2a3054;color:var(--txt);font-size:14px;font-weight:600;
        cursor:pointer;transition:transform .06s}
 button:active{transform:scale(.97);opacity:.8}
 .w{background:#e8ecff;color:#101425}
 .u{background:var(--uv);color:#fff}
 .go{background:var(--acc);color:#fff}
 .big{flex-basis:100%;padding:16px;font-size:16px}
 .mini{flex:0 0 auto;min-width:0;padding:4px 10px;font-size:11px;border-radius:8px}
 .nav{flex:0 0 48px;min-width:0}

 label{display:block;font-size:12px;color:var(--dim);margin:10px 0 4px}
 label:first-child{margin-top:0}
 input[type=range]{width:100%;accent-color:var(--acc)}
 select{width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);
        background:#12162a;color:var(--txt);font-size:14px}
 .row select{flex:1;width:auto;min-width:0}

 .badge{display:inline-block;padding:3px 10px;border-radius:999px;
        font-size:11px;font-weight:700;background:#2a3054}
 .badge.on-w{background:#e8ecff;color:#101425}
 .badge.on-u{background:var(--uv);color:#fff}

 .hint{font-size:12px;color:var(--dim);line-height:1.7;
       background:#12162a;border-radius:8px;padding:10px;margin-top:10px}
 .hint b{color:var(--txt)}
 .dot{display:inline-block;width:7px;height:7px;border-radius:50%;
      background:var(--ok);margin-right:6px;vertical-align:middle}

 #log{font-size:12px;color:var(--dim);white-space:pre-wrap;word-break:break-all;
      max-height:160px;overflow:auto;line-height:1.6}
</style></head><body><div class="wrap">

<h1>HabiTooth Wand</h1>

<div class="cols">

  <!-- ───── 왼쪽: 스트림 + 가이드 + 촬영 ───── -->
  <div class="left">

    <div class="card">
      <div class="views">
        <div class="pane">
          <div class="pane-h">
            <span>실시간 스트림</span>
            <span>조명 <b class="badge" id="cur">OFF</b></span>
          </div>
          <img id="v" class="frame" src="">
        </div>
        <div class="pane">
          <div class="pane-h">
            <span>가이드 · <span class="zone" id="gname"></span></span>
            <button class="mini" onclick="gdir.click()">가이드 폴더</button>
          </div>
          <img id="gimg" class="frame" style="display:none">
          <div id="gempty" class="frame empty">
            가이드샷이 없습니다.<br>[가이드 폴더]로 사진 폴더를 선택하세요.
          </div>
          <input type="file" id="gdir" webkitdirectory multiple hidden onchange="pickGuides(this.files)">
        </div>
      </div>
    </div>

    <div class="card">
      <div class="row">
        <button class="nav" onclick="stepView(-1)">◀</button>
        <select id="view" onchange="setView()">
          <optgroup label="설측 상악">
            <option value="UPPER_RIGHT_MOLAR">상악 우 대구치 · UPPER_RIGHT_MOLAR</option>
            <option value="UPPER_RIGHT_PREMOLAR">상악 우 소구치 · UPPER_RIGHT_PREMOLAR</option>
            <option value="UPPER_FRONT">상악 전치부 · UPPER_FRONT</option>
            <option value="UPPER_LEFT_PREMOLAR">상악 좌 소구치 · UPPER_LEFT_PREMOLAR</option>
            <option value="UPPER_LEFT_MOLAR">상악 좌 대구치 · UPPER_LEFT_MOLAR</option>
          </optgroup>
          <optgroup label="설측 하악">
            <option value="LOWER_RIGHT_MOLAR">하악 우 대구치 · LOWER_RIGHT_MOLAR</option>
            <option value="LOWER_RIGHT_PREMOLAR">하악 우 소구치 · LOWER_RIGHT_PREMOLAR</option>
            <option value="LOWER_FRONT">하악 전치부 · LOWER_FRONT</option>
            <option value="LOWER_LEFT_PREMOLAR">하악 좌 소구치 · LOWER_LEFT_PREMOLAR</option>
            <option value="LOWER_LEFT_MOLAR">하악 좌 대구치 · LOWER_LEFT_MOLAR</option>
          </optgroup>
          <optgroup label="외측">
            <option value="OUTER_RIGHT">외측 우 · OUTER_RIGHT</option>
            <option value="OUTER_CENTER" selected>외측 중앙 · OUTER_CENTER</option>
            <option value="OUTER_LEFT">외측 좌 · OUTER_LEFT</option>
          </optgroup>
        </select>
        <button class="nav" onclick="stepView(1)">▶</button>
      </div>
      <div class="row">
        <button class="go big" onclick="pair()">백색 + UV 연속 촬영</button>
      </div>
      <div class="row">
        <button onclick="snap('white')">백색만</button>
        <button onclick="snap('uv')">UV만</button>
      </div>
      <div class="hint">
        <span class="dot"></span><b>디바이스 스위치</b>를 눌러도 백색 + UV 연속 촬영됩니다.
        이 페이지가 열려 있어야 파일이 저장됩니다.
      </div>
    </div>

  </div>

  <!-- ───── 오른쪽: 설정 ───── -->
  <div class="right">

    <div class="card">
      <h2>조명</h2>
      <div class="row">
        <button class="w" onclick="led('white')">백색광</button>
        <button class="u" onclick="led('uv')">UV</button>
        <button       onclick="led('off')">OFF</button>
      </div>
      <label>백색 밝기 <span id="bw">255</span></label>
      <input id="bwR" type="range" min="0" max="255" value="255"
            oninput="bw.textContent=this.value" onchange="bright('w',this.value)">
      <label>UV 밝기 <span id="bu">255</span></label>
      <input id="buR" type="range" min="0" max="255" value="255"
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
      <h2>네오픽셀</h2>
      <div class="row">
        <button onclick="neo('off')">OFF</button>
        <button onclick="neo('g')" style="background:#0f7a3f">G</button>
        <button onclick="neo('r')" style="background:#a53030">R</button>
        <button onclick="neo('b')" style="background:#3060a5">B</button>
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
async function neo(c){ await fetch('/neo?c=' + c); }

// ── 구역 ─────────────────────────────────────
async function setView(){
  showGuide();
  try{ await fetch('/view?v=' + view.value); log('구역: ' + view.value); }
  catch(e){ log('구역 전송 실패'); }
}
function stepView(d){
  const n = view.options.length;
  view.selectedIndex = (view.selectedIndex + d + n) % n;
  setView();
}
// 새로고침해도 디바이스(스위치 촬영)와 화면 구역이 어긋나지 않게 맞춤
async function loadView(){
  try{
    const v = (await (await fetch('/view')).text()).trim();
    if ([...view.options].some(o => o.value === v)) view.value = v;
  }catch(e){}
  showGuide();
}

// ── 가이드샷 (IndexedDB) ─────────────────────
const ALIAS = { UPPER_CENTER:'UPPER_FRONT', LOWER_CENTER:'LOWER_FRONT' };
let guides = {};
let guideUrl = null;

function idb(){
  return new Promise((ok, ng) => {
    const r = indexedDB.open('habitooth-guide', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('img');
    r.onsuccess = () => ok(r.result);
    r.onerror   = () => ng(r.error);
  });
}
async function idbSave(map){
  const db = await idb();
  return new Promise((ok, ng) => {
    const tx = db.transaction('img', 'readwrite');
    const st = tx.objectStore('img');
    st.clear();
    for (const k in map) st.put(map[k], k);
    tx.oncomplete = ok;
    tx.onerror    = () => ng(tx.error);
  });
}
async function idbLoad(){
  const db = await idb();
  return new Promise((ok, ng) => {
    const out = {};
    const c = db.transaction('img').objectStore('img').openCursor();
    c.onsuccess = () => {
      const cur = c.result;
      if (cur) { out[cur.key] = cur.value; cur.continue(); } else ok(out);
    };
    c.onerror = () => ng(c.error);
  });
}

function keyOf(fname){
  const k = fname.replace(/\.[^.]+$/, '').trim().toUpperCase();
  return ALIAS[k] || k;
}

async function pickGuides(files){
  const valid = new Set([...view.options].map(o => o.value));
  const map = {};
  for (const f of files) {
    if (!f.type.startsWith('image/')) continue;
    const k = keyOf(f.name);
    if (!valid.has(k)) continue;
    map[k] = new Blob([await f.arrayBuffer()], { type: f.type });
  }
  const n = Object.keys(map).length;
  if (!n) { log('가이드: 구역 이름과 맞는 사진이 없습니다'); return; }
  try { await idbSave(map); } catch(e) { log('가이드 저장 실패: ' + e); }
  guides = map;
  const miss = [...valid].filter(v => !map[v]);
  log('가이드 ' + n + '장 저장' + (miss.length ? ' · 없음: ' + miss.join(', ') : ''));
  showGuide();
  gdir.value = '';
}

function showGuide(){
  const k = view.value;
  const img = document.getElementById('gimg');
  const empty = document.getElementById('gempty');
  document.getElementById('gname').textContent =
    view.options[view.selectedIndex].text.split(' · ')[0];

  if (guideUrl) { URL.revokeObjectURL(guideUrl); guideUrl = null; }
  if (guides[k]) {
    guideUrl = URL.createObjectURL(guides[k]);
    img.src = guideUrl;
    img.style.display = 'block';
    empty.style.display = 'none';
  } else {
    img.style.display = 'none';
    empty.style.display = 'flex';
  }
}

(async () => {
  try { guides = await idbLoad(); } catch(e) { guides = {}; }
  const n = Object.keys(guides).length;
  if (n) log('가이드 ' + n + '장 불러옴');
  loadView();
})();

// ── 설정값 불러오기 ──────────────────────────
async function loadExp(){
  try{
    const d = await (await fetch('/exp')).json();
    aeR.value = d.aec;   ae.textContent = d.aec;
    waR.value = d.waec;  wa.textContent = d.waec;
    agR.value = d.agc;   ag.textContent = d.agc;
  }catch(e){}
}
loadExp();

async function loadBright(){
  try{
    const d = await (await fetch('/bright')).json();
    bwR.value = d.w;  bw.textContent = d.w;
    buR.value = d.uv; bu.textContent = d.uv;
  }catch(e){}
}
loadBright();

// ── 촬영 ─────────────────────────────────────
function snap(l){
  log(l.toUpperCase() + ' 촬영 → 다운로드');
  const a = document.createElement('a');
  a.href = '/snap?light=' + l + '&view=' + encodeURIComponent(view.value) + '&t=' + Date.now();
  a.download = '';
  document.body.appendChild(a); a.click(); a.remove();
}

// 스위치와 동일 경로(capturePair). 저장은 poll()이 처리한다.
async function pair(){
  log('백색 + UV 연속 촬영');
  try{
    await fetch('/view?v=' + view.value);
    await fetch('/pair');
  }catch(e){ log('연속 촬영 실패'); }
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
    const d = await (await fetch('/pending')).json();
    if (lastSeq === null) { lastSeq = d.seq; }
    else if (d.seq > lastSeq) {
      lastSeq = d.seq;
      log('촬영 수신: ' + d.w);
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