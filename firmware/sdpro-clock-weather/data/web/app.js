const $ = id => document.getElementById(id);
const show = id => document.querySelectorAll('.panel').forEach(p => p.classList.toggle('hidden', p.id !== id));
document.querySelectorAll('nav button').forEach(b => b.onclick = () => show(b.dataset.tab));

async function getText(path){ const r=await fetch(path); return await r.text(); }
async function getJson(path){ const r=await fetch(path); return await r.json(); }
function pretty(x){ return typeof x === 'string' ? x : JSON.stringify(x,null,2); }

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
  $('weatherEnabled').checked=!!c.weather_enabled;
  $('clock24h').checked=!!c.clock_24h;
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
    weather_enabled:$('weatherEnabled').checked,
    clock_24h:$('clock24h').checked
  };
  if($('wifiPass').value) body.pass=$('wifiPass').value;
  $('weatherOut').textContent=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(r=>r.text());
  $('systemOut').textContent=$('weatherOut').textContent;
  await loadConfig(); await loadWeather();
}
$('saveConfig').onclick=saveConfig;
$('saveSystem').onclick=saveConfig;
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
