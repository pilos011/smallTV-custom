const $ = id => document.getElementById(id);
const show = id => document.querySelectorAll('.panel').forEach(p => p.classList.toggle('hidden', p.id !== id));
document.querySelectorAll('nav button').forEach(b => b.onclick = () => show(b.dataset.tab));

function guard(r){ if(r.status===401){ location.href='/login'; throw new Error('login required'); } return r; }
async function getText(path){ const r=guard(await fetch(path)); return await r.text(); }
async function postText(path, body){
  const opt={method:'POST'};
  if(body!==undefined){ opt.headers={'Content-Type':'application/json'}; opt.body=body; }
  const r=guard(await fetch(path,opt));
  return await r.text();
}
async function getJson(path){ const r=guard(await fetch(path)); return await r.json(); }
function pretty(x){ return typeof x === 'string' ? x : JSON.stringify(x,null,2); }
function minutesToTime(v){
  v=((+v||0)%1440+1440)%1440;
  return String(Math.floor(v/60)).padStart(2,'0')+':'+String(v%60).padStart(2,'0');
}
function timeToMinutes(v){
  const m=/^(\d{1,2}):(\d{2})$/.exec(v||'');
  if(!m) return 0;
  return ((+m[1]*60)+(+m[2]))%1440;
}

async function loadConfig(){
  const c=await getJson('/api/config');
  $('wifiSsid').value=c.ssid||'';
  $('wifiPass').value='';
  $('location').value=c.location||'';
  $('kmaKey').value=c.kma_key||'';
  $('nx').value=c.nx;
  $('ny').value=c.ny;
  $('tz').value=c.timezone_offset_minutes;
  $('brightness').value=c.brightness;
  $('nightModeEnabled').checked=!!c.night_mode_enabled;
  $('nightBrightness').value=c.night_brightness??20;
  $('nightStart').value=minutesToTime(c.night_start_minutes??1380);
  $('nightStop').value=minutesToTime(c.night_stop_minutes??420);
  $('weatherEnabled').checked=!!c.weather_enabled;
  $('clock24h').checked=!!c.clock_24h;
  const mask=+c.screens||1;
  document.querySelectorAll('.screen').forEach(b=>{ b.checked=!!(mask&(1<<(+b.dataset.bit))); });
  $('themeInterval').value=c.theme_interval_seconds??10;
  faces = Array.isArray(c.analog_faces) && c.analog_faces.length ? c.analog_faces : [blankFace()];
  buildFaceList(c.analog_face_count ?? faces.length);
  showFace(faceIdx);
  $('statusOut').textContent=pretty(c);
}
async function loadStatus(){ $('statusOut').textContent=pretty(await getJson('/status')); }
async function loadWeather(){ $('weatherOut').textContent=pretty(await getJson('/weather/status')); }

$('refreshStatus').onclick=async()=>{await loadStatus(); await loadWeather();};
async function saveConfig(){
  const body={
    ssid:$('wifiSsid').value,
    location:$('location').value,
    kma_key:$('kmaKey').value,
    nx:+$('nx').value,
    ny:+$('ny').value,
    timezone_offset_minutes:+$('tz').value,
    brightness:+$('brightness').value,
    night_mode_enabled:$('nightModeEnabled').checked,
    night_brightness:+$('nightBrightness').value,
    night_start_minutes:timeToMinutes($('nightStart').value),
    night_stop_minutes:timeToMinutes($('nightStop').value),
    weather_enabled:$('weatherEnabled').checked,
    clock_24h:$('clock24h').checked,
    screens:screenMask(),
    theme_interval_seconds:+$('themeInterval').value
  };
  if($('wifiPass').value) body.pass=$('wifiPass').value;
  $('weatherOut').textContent=await postText('/api/config',JSON.stringify(body));
  $('systemOut').textContent=$('weatherOut').textContent;
  await loadConfig(); await loadWeather();
}
// raw selection; 0 means the user ticked nothing
function selectedMask(){
  let m=0;
  document.querySelectorAll('.screen').forEach(b=>{ if(b.checked) m|=1<<(+b.dataset.bit); });
  return m;
}
// what the firmware will actually run: it falls back to Clock/Weather
function screenMask(){ return selectedMask()||1; }
$('saveConfig').onclick=saveConfig;
$('saveSystem').onclick=saveConfig;
$('saveDisplay').onclick=async()=>{
  if(!selectedMask()){ $('displayOut').textContent='Pick at least one theme.'; return; }
  $('displayOut').textContent='Saving...';
  await saveConfig();
  const on=[...document.querySelectorAll('.screen')].filter(b=>b.checked)
    .map(b=>b.parentElement.textContent.trim());
  $('displayOut').textContent='Saved. Themes: '+on.join(', ')+
    ' · interval '+$('themeInterval').value+'s';
};
$('logout').onclick=()=>{ location.href='/logout'; };
$('refreshWeather').onclick=async()=>{ $('weatherOut').textContent=await postText('/weather/refresh'); await loadWeather(); };
$('fsList').onclick=async()=>{ $('systemOut').textContent=await getText('/fs/list'); };
$('restart').onclick=async()=>{ $('systemOut').textContent=await getText('/restart'); };

async function upload(path, field, input, out){
  if(!input.files[0]) return;
  const fd=new FormData(); fd.append(field,input.files[0],input.files[0].name);
  out.textContent='Uploading...';
  const r=await fetch(path,{method:'POST',body:fd});
  out.textContent=await r.text();
}
$('fwForm').onsubmit=e=>{e.preventDefault(); upload('/update_ota','update',$('fwFile'),$('recoveryOut'));};
$('fsForm').onsubmit=e=>{e.preventDefault(); upload('/api/ota/fs','fs',$('fsFile'),$('recoveryOut'));};

// ---- analog face colours -------------------------------------------------
// The firmware reports how many faces it can render, so adding a variant there
// grows this list without touching the page.
const CHANNELS = [
  {key:'dial', id:'Dial', def:0x000000},
  {key:'case', id:'Case', def:0x000008},
  {key:'lume', id:'Lume', def:0x00F0FF},
  {key:'hand', id:'Hand', def:0xFF0000},
  {key:'accent', id:'Accent', def:0x000000}
];
let faces = [];      // mirrors analog_faces from the device
let faceIdx = 0;

const toHex = v => '#'+(v&0xFFFFFF).toString(16).padStart(6,'0');
const fromHex = t => {
  const m=/^#?([0-9a-fA-F]{6})$/.exec((t||'').trim());
  return m ? parseInt(m[1],16) : null;
};
// what the 16-bit panel can actually show: 5 bits red, 6 green, 5 blue
const to565 = v => (((v>>16&0xFF)>>3)<<11) | (((v>>8&0xFF)>>2)<<5) | ((v&0xFF)>>3);
const from565 = c => (((c>>11&0x1F)<<3)<<16) | (((c>>5&0x3F)<<2)<<8) | ((c&0x1F)<<3);

function blankFace(){ const f={}; CHANNELS.forEach(c=>f[c.key]=c.def); return f; }

function paintChannel(c, v){
  const q = to565(v);
  $('col'+c.id).value = toHex(from565(q));
  $('hex'+c.id).value = toHex(v);
  const el = $('p565'+c.id);
  el.textContent = '0x'+q.toString(16).toUpperCase().padStart(4,'0');
  el.style.background = toHex(from565(q));
}
// Channel meanings differ by face: the digital one has no rim, and its ink is
// hours and minutes rather than markers and hands.
const LABELS = {
  0: {dial:'Dial', case:'Rim', lume:'Numerals & ticks', hand:'Hands', accent:null},
  1: {dial:'Dial', case:'Rim', lume:'Markers & hands', hand:'Seconds hand', accent:null},
  2: {dial:'Dial', case:'Rim', lume:'Markers & hands', hand:'Seconds hand', accent:null},
  3: {dial:'Background', case:null, lume:'Hours', hand:'Minutes', accent:null},
  4: {dial:'Background', case:null, lume:'Hours & temperature', hand:'Minutes & condition', accent:null},
  5: {dial:'Background', case:'Date', lume:'Hours', hand:'Minutes', accent:'Weekday'}
};
function showFace(i){
  faceIdx = i;
  const f = faces[i] || blankFace();
  const map = LABELS[i] || LABELS[0];
  CHANNELS.forEach(c => {
    paintChannel(c, f[c.key] ?? c.def);
    const row = $('col'+c.id).closest('.colour-row');
    const label = map[c.key];
    row.style.display = label ? '' : 'none';
    if(label) row.querySelector('label').textContent = label;
  });
}
function stashChannel(c, v){
  if(!faces[faceIdx]) faces[faceIdx] = blankFace();
  faces[faceIdx][c.key] = v;
  paintChannel(c, v);
}

function buildFaceList(count){
  const sel = $('faceSel');
  const keep = faceIdx;
  sel.innerHTML = '';
  const names=['Analog','Mondaine','Mondaine White','Digital','Weather Digital','Date Digital'];
  for(let i=0;i<Math.max(1,count);i++){
    const o=document.createElement('option');
    o.value=i; o.textContent=names[i] ?? ('Face '+(i+1));
    sel.appendChild(o);
  }
  faceIdx = Math.min(keep, Math.max(0,count-1));
  sel.value = faceIdx;
  sel.disabled = count <= 1;
}
$('faceSel').onchange = e => showFace(+e.target.value);

CHANNELS.forEach(c => {
  $('col'+c.id).oninput = e => { const v=fromHex(e.target.value); stashChannel(c, v===null?c.def:v); };
  $('hex'+c.id).oninput = e => { const v=fromHex(e.target.value); if(v!==null) stashChannel(c, v); };
});

async function saveColours(){
  $('colourOut').textContent = 'Saving...';
  const txt = await postText('/api/config', JSON.stringify({analog_faces: faces}));
  await loadConfig();
  $('colourOut').textContent = txt.trim() === 'ok'
    ? 'Saved. ' + CHANNELS.map(c => c.key + ' ' + $('p565'+c.id).textContent).join('   ')
    : txt;
}
$('saveColours').onclick = saveColours;
$('resetColours').onclick = () => {
  faces[faceIdx] = blankFace();
  showFace(faceIdx);
  $('colourOut').textContent = 'Defaults loaded for this face. Press Save Colours to apply.';
};
loadConfig().then(loadWeather).catch(e=>$('statusOut').textContent=e.stack||String(e));
