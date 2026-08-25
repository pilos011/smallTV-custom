const $ = id => document.getElementById(id);
const show = id => document.querySelectorAll('.panel').forEach(p => p.classList.toggle('hidden', p.id !== id));
document.querySelectorAll('nav button').forEach(b => b.onclick = () => show(b.dataset.tab));

function guard(r){ if(r.status===401){ location.href='/login'; throw new Error('login required'); } return r; }
async function getText(path){ const r=guard(await fetch(path)); return await r.text(); }
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
  $('weatherOut').textContent=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(r=>r.text());
  $('systemOut').textContent=$('weatherOut').textContent;
  await loadConfig(); await loadWeather();
}
function screenMask(){
  let m=0;
  document.querySelectorAll('.screen').forEach(b=>{ if(b.checked) m|=1<<(+b.dataset.bit); });
  return m||1;
}
$('saveConfig').onclick=saveConfig;
$('saveSystem').onclick=saveConfig;
$('saveDisplay').onclick=async()=>{
  if(!screenMask()){ $('displayOut').textContent='Pick at least one theme.'; return; }
  $('displayOut').textContent='Saving...';
  await saveConfig();
  $('displayOut').textContent='Saved. Active themes: '+
    [...document.querySelectorAll('.screen')].filter(b=>b.checked).length+
    ', interval '+$('themeInterval').value+'s';
};
$('logout').onclick=()=>{ location.href='/logout'; };
$('refreshWeather').onclick=async()=>{ $('weatherOut').textContent=await fetch('/weather/refresh',{method:'POST'}).then(r=>r.text()); await loadWeather(); };
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

loadConfig().then(loadWeather).catch(e=>$('statusOut').textContent=e.stack||String(e));
