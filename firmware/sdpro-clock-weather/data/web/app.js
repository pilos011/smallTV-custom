const $ = id => document.getElementById(id);
const show = id => document.querySelectorAll('.panel').forEach(p => p.classList.toggle('hidden', p.id !== id));
// A tab loads its data the first time it is opened, not when the page is.
// The device serves one request at a time, and opening the page used to fire
// everything at once - five API calls, the radar map preview and every album
// photo - which queued so deep that the stylesheet request was dropped and
// the page arrived as bare text. Now the first paint costs two small calls
// and each tab pays its own way when someone actually looks at it.
const tabLoaded = {};
function openTab(id){
  if(!id) return;
  show(id);
  if(id === 'album' && !tabLoaded.album){
    tabLoaded.album = 1;
    loadAlbum().catch(e => { $('albumOut').textContent = e.message || String(e); });
  }
  if(id === 'radar' && !tabLoaded.radar){
    tabLoaded.radar = 1;
    loadRadar().catch(e => { $('radarOut').textContent = e.message || String(e); });
  }
  if(id === 'system') refreshGauges();
}
document.querySelectorAll('nav button').forEach(b => b.onclick = () => openTab(b.dataset.tab));

// Stands in for a password that exists on the device. Sending it back would
// set the literal mask as the password, so saveConfig checks for it by value -
// which is also why it has to be something nobody would type.
const WIFI_PASS_MASK = '\u2022'.repeat(8);
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
  // The card showed cfg.ssid and nothing else, so on a device that joined
  // through a saved profile it sat empty next to a working connection - which
  // reads as "no network is set". Fall back to what the device is actually on.
  $('wifiSsid').value = c.ssid || c.ssid_live || '';
  // A stored password is shown as a mask, not as an empty box. Empty said
  // "there is none"; the mask says "there is one, and it stays on the device".
  // Saving sends a password only when this has actually been typed over.
  $('wifiPass').value = c.pass_set ? WIFI_PASS_MASK : '';
  fillForecast(c);
  wifiProfiles=Array.isArray(c.wifi_profiles)?c.wifi_profiles:[];
  renderWifiProfiles();
  $('location').value=c.location||'';
  $('kmaKey').value=c.kma_key||'';
  $('nx').value=c.nx;
  $('ny').value=c.ny;
  weatherLive=c;
  weatherPresets=Array.isArray(c.weather_presets)?c.weather_presets:[];
  renderWeatherPresets();
  $('tz').value=c.timezone_offset_minutes;
  $('brightness').value=c.brightness;
  $('nightModeEnabled').checked=!!c.night_mode_enabled;
  $('nightBrightness').value=c.night_brightness??20;
  $('nightStart').value=minutesToTime(c.night_start_minutes??1380);
  $('nightStop').value=minutesToTime(c.night_stop_minutes??420);
  $('weatherEnabled').checked=!!c.weather_enabled;
  $('clock24h').checked=!!c.clock_24h;
  const mask=+c.screens||1;
  screenOn = SCREEN_NAMES.map((_,i)=>!!(mask&(1<<i)));
  // Trust the device's order, but never lose a screen if it sends a short list.
  const sent = Array.isArray(c.screen_order) ? c.screen_order.filter(
    (v,i,a) => Number.isInteger(v) && v>=0 && v<SCREEN_NAMES.length && a.indexOf(v)===i) : [];
  screenOrder = sent.concat(SCREEN_NAMES.map((_,i)=>i).filter(i=>sent.indexOf(i)<0));
  renderThemeList();
  $('themeInterval').value=c.theme_interval_seconds??10;
  faces = Array.isArray(c.analog_faces) && c.analog_faces.length ? c.analog_faces : [blankFace()];
  buildFaceList(c.analog_face_count ?? faces.length);
  showFace(faceIdx);
  $('apState').textContent = c.ap_active
    ? 'On the air now at ' + (c.ap_ip || '?') + ' as SDP-Recovery (open).'
    : 'Off. It starts by itself if the device cannot join your WiFi.';
  $('pwHint').textContent = c.web_password_is_default
    ? 'Still set to the factory password. Change it below.'
    : 'Used to sign in to this menu. 1 to 32 characters.';
  $('statusOut').textContent=pretty(c);
}
// The memory gauges in the System menu, read when it is opened rather than on
// a timer: a twenty-second poll kept the device busy for a page nobody was
// looking at. RAM measures against the heap as it stood after boot - the chip
// has no "total heap" - so 100 percent means "back to where a fresh boot
// starts", not the impossible 80 KB of the part.
function memBar(fillId, valId, used, total, text){
  const pct = total > 0 ? Math.min(100, Math.round(used * 100 / total)) : 0;
  const el = $(fillId);
  el.style.width = pct + '%';
  el.className = 'mfill' + (pct >= 85 ? ' hot' : pct >= 70 ? ' warn' : '');
  $(valId).textContent = pct + '%  ' + text;
}
function kb(n){ return Math.round(n / 1024) + 'K'; }
function updateMemBars(s){
  if(s.heap_boot) memBar('mbHeap','mtHeap', s.heap_boot - s.free_heap, s.heap_boot, kb(s.free_heap) + ' free');
  if(s.fw_total) memBar('mbFw','mtFw', s.fw_used, s.fw_total, kb(s.fw_total - s.fw_used) + ' free');
  if(s.fs_total) memBar('mbFs','mtFs', s.fs_used, s.fs_total, kb(s.fs_total - s.fs_used) + ' free');
}
function refreshGauges(){
  getJson('/status').then(updateMemBars).catch(()=>{});
}
async function loadStatus(){
  const s = await getJson('/status');
  $('statusOut').textContent = pretty(s);
  updateMemBars(s);
}
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
    screen_order:screenOrder,
    theme_interval_seconds:+$('themeInterval').value
  };
  const typed = $('wifiPass').value;
  if(typed && typed !== WIFI_PASS_MASK) body.pass = typed;
  $('weatherOut').textContent=await postText('/api/config',JSON.stringify(body));
  $('systemOut').textContent=$('weatherOut').textContent;
  await loadConfig(); await loadWeather();
}

// --- saved weather locations ------------------------------------------------
// Same shape as the radar's list: the device holds it, the page edits it whole.
// What varies with the place is the grid pair and the word on the panel; the
// API key and the timezone do not, so they are not in here.
let weatherPresets = [];
// What the device is set to right now, so an entry can be shown as the one in
// use. Compared by value rather than by a stored flag, which would go wrong the
// moment someone edited Grid X by hand.
let weatherLive = {};

function weatherPresetIsLive(p){
  if(weatherLive.nx === undefined) return false;
  return Number(p.nx) === Number(weatherLive.nx) && Number(p.ny) === Number(weatherLive.ny);
}

function renderWeatherPresets(){
  const box = $('wxPresetList');
  box.textContent = '';
  weatherPresets.forEach((p, i) => {
    const row = document.createElement('div');
    const live = weatherPresetIsLive(p);
    row.className = live ? 'preset-item active' : 'preset-item';
    if(live){
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = 'IN USE';
      row.appendChild(badge);
    }
    const label = document.createElement('span');
    // The label only earns a mention when it differs from the name - printing
    // 백석동 ("백석동") twice tells the reader nothing.
    label.textContent = p.name + '  (' + p.nx + ' / ' + p.ny + ')' +
      (p.label && p.label !== p.name ? '  shows as "' + p.label + '"' : '');
    const load = document.createElement('button');
    load.textContent = 'Load';
    load.onclick = async () => {
      $('weatherOut').textContent = 'Loading "' + p.name + '"...';
      await postText('/api/config', JSON.stringify({
        nx: Number(p.nx), ny: Number(p.ny), location: p.label || p.name
      }));
      // The device refetches the forecast as part of saving config, so by the
      // time this comes back the panel is already showing the new place.
      await loadConfig();
      await loadWeather();
      $('weatherOut').textContent = 'Loaded "' + p.name + '".';
    };
    const del = document.createElement('button');
    del.textContent = '\u2715';
    del.className = 'ghost';
    del.onclick = async () => {
      weatherPresets.splice(i, 1);
      await saveWeatherPresets('Deleted.');
    };
    row.append(label, load, del);
    box.appendChild(row);
  });
}

async function saveWeatherPresets(doneMsg){
  await postText('/api/config', JSON.stringify({ weather_presets: weatherPresets }));
  await loadConfig();
  $('weatherOut').textContent = doneMsg;
}

$('wxPresetSave').onclick = async () => {
  const name = $('wxPresetName').value.trim();
  const nx = Number($('nx').value);
  const ny = Number($('ny').value);
  if(!name){ $('weatherOut').textContent = 'Give the place a name first.'; return; }
  // The KMA short-term grid is 149 x 253 cells over the peninsula. A pair
  // outside it is not a place, and its forecast comes back empty every time
  // without ever saying why.
  if(!Number.isInteger(nx) || !Number.isInteger(ny) ||
     nx < 1 || nx > 149 || ny < 1 || ny > 253){
    $('weatherOut').textContent = 'Grid X must be 1-149 and Grid Y 1-253.';
    return;
  }
  const entry = { name, nx, ny, label: $('location').value.trim() || name };
  const at = weatherPresets.findIndex(p => p.name === name);
  if(at >= 0) weatherPresets[at] = entry;
  else if(weatherPresets.length >= 6){
    $('weatherOut').textContent = 'All 6 slots are taken. Delete one first.';
    return;
  }
  else weatherPresets.push(entry);
  $('wxPresetName').value = '';
  await saveWeatherPresets('Saved "' + name + '".');
};

// --- files ------------------------------------------------------------------
// There was no way to delete anything. The filesystem could be filled - three
// copies of the same radar map, say - and the only way to get the space back
// was to rewrite the whole 2 MB image, which takes the settings and the photos
// with it. This lists what is there and removes one file at a time.
//
// The web files and the config are shown but not deletable here. Removing the
// page you are standing on has no upside and one obvious downside; a deliberate
// DELETE to /file still does it if a broken file ever needs replacing.
const FILES_KEPT = p => p.startsWith('/web/') || p === '/config.json';

function fmtBytes(n){
  return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB'
       : n >= 1024    ? Math.round(n / 1024) + ' KB'
       :                n + ' B';
}

async function loadFiles(){
  const txt = await getText('/fs/list');
  const files = [];
  txt.split(/[\r\n]+/).forEach(line => {
    const bits = line.split('\t');
    if(bits.length < 2) return;
    const path = bits[0], size = bits[1].trim();
    if(size === '<dir>' || !/^[0-9]+$/.test(size)) return;
    files.push({ path: path, size: parseInt(size, 10) });
  });

  const folders = new Map();
  files.forEach(f => {
    const cut = f.path.lastIndexOf('/');
    const dir = cut > 0 ? f.path.slice(0, cut) : '/';
    if(!folders.has(dir)) folders.set(dir, []);
    folders.get(dir).push(f);
  });

  const box = $('filesList');
  box.textContent = '';
  Array.from(folders.keys()).sort().forEach(dir => {
    const list = folders.get(dir).sort((a, b) => a.path < b.path ? -1 : 1);
    const sum = list.reduce((t, f) => t + f.size, 0);

    const head = document.createElement('div');
    head.className = 'file-folder';
    head.textContent = dir + '  -  ' + list.length + ' files, ' + fmtBytes(sum);
    box.appendChild(head);

    list.forEach(f => {
      const row = document.createElement('div');
      row.className = 'preset-item';
      const name = document.createElement('span');
      name.textContent = f.path.slice(dir === '/' ? 1 : dir.length + 1);
      const size = document.createElement('span');
      size.className = 'file-size';
      size.textContent = fmtBytes(f.size);
      row.appendChild(name);
      row.appendChild(size);
      if(FILES_KEPT(f.path)){
        const kept = document.createElement('span');
        kept.className = 'badge';
        kept.textContent = 'KEPT';
        row.appendChild(kept);
      } else {
        const del = document.createElement('button');
        del.className = 'ghost';
        del.textContent = 'Delete';
        del.onclick = () => removeFile(f, row, del);
        row.appendChild(del);
      }
      box.appendChild(row);
    });
  });

  const st = await getJson('/status');
  updateMemBars(st);
  $('filesOut').textContent = files.length + ' files, ' + fmtBytes(st.fs_used) +
    ' used, ' + fmtBytes(st.fs_total - st.fs_used) + ' free';
}

async function removeFile(f, row, btn){
  if(!confirm('Delete ' + f.path + ' (' + fmtBytes(f.size) + ')?' + '\n' +
              'This cannot be undone.')) return;
  btn.disabled = true;
  btn.textContent = 'Deleting...';
  const r = guard(await fetch('/file?path=' + encodeURIComponent(f.path), { method: 'DELETE' }));
  const txt = (await r.text()).trim();
  if(!r.ok){
    btn.disabled = false;
    btn.textContent = 'Delete';
    $('filesOut').textContent = txt;
    return;
  }
  row.remove();
  $('filesOut').textContent = txt;
  refreshGauges();
}

$('filesLoad').onclick = async () => {
  $('filesOut').textContent = 'Reading...';
  try { await loadFiles(); }
  catch(e){ $('filesOut').textContent = e.message || String(e); }
};

// --- weekly forecast --------------------------------------------------------
// The key is write-only from the page's side: the device answers whether it has
// one, never what it is, so the field carries the same mask the WiFi password
// does and a save sends nothing unless it was typed over.
let fcPresets = [];
let fcPresetIdx = 0;

function fillForecast(c){
  $('owKey').value = c.ow_key || '';
  // missing comes from the device: the panel's font holds 438 of the 11172
  // Hangul syllables, and one absent character sends the whole title to the
  // built-in font, where it draws as broken bytes. Only the device can say.
  fcPresets = (c.fc_presets || []).map(p => ({
    name: p.name, lat: p.lat, lon: p.lon, missing: p.missing || ''
  }));
  fcPresetIdx = c.fc_preset_idx || 0;
  renderForecastPresets();
}

// The list is both the editor and the selector, the way the radar presets are.
// A dropdown saying the same thing alongside it was one more place for the two
// to disagree.
function renderForecastPresets(){
  const box = $('fcList');
  box.textContent = '';
  fcPresets.forEach((p, i) => {
    const row = document.createElement('div');
    const live = (i === fcPresetIdx);
    row.className = live ? 'preset-item active' : 'preset-item';
    if(live){
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = 'IN USE';
      badge.title = 'The forecast screen is showing this place.';
      row.appendChild(badge);
    }
    if(p.missing){
      const warn = document.createElement('span');
      warn.className = 'badge edited';
      warn.textContent = 'NO GLYPH';
      warn.title = 'The panel has no character for ' + p.missing +
        ', so this name draws as broken bytes. Rename it to something the ' +
        'screen can show.';
      row.appendChild(warn);
    }
    const label = document.createElement('span');
    label.textContent = p.name + '  (' + Number(p.lat).toFixed(4) + ', ' +
      Number(p.lon).toFixed(4) + ')';
    row.appendChild(label);
    if(!live){
      const use = document.createElement('button');
      use.textContent = 'Use';
      use.className = 'ghost';
      use.onclick = async () => {
        $('fcOut').textContent = await postText('/api/config',
          JSON.stringify({ fc_preset_idx: i }));
        await loadConfig();
      };
      row.appendChild(use);
    }
    const del = document.createElement('button');
    del.textContent = '✕';
    del.className = 'ghost';
    del.onclick = async () => {
      // The device keeps the built-in pair rather than accept an empty list, so
      // removing the last one would look like the button had done nothing.
      if(fcPresets.length <= 1){
        $('fcOut').textContent = 'Keep at least one place.';
        return;
      }
      fcPresets.splice(i, 1);
      // Deleting shifts everything after it, and the device checks the index
      // against the count it still has - so the two have to travel together.
      let idx = fcPresetIdx;
      if(i < idx) idx -= 1;
      else if(i === idx) idx = 0;
      await saveForecastPresets(idx, 'Removed "' + p.name + '".');
    };
    row.appendChild(del);
    box.appendChild(row);
  });
}

async function saveForecastPresets(idx, doneMsg){
  // Only the three fields the device stores; missing is its answer, not ours.
  const wire = fcPresets.map(p => ({ name: p.name, lat: p.lat, lon: p.lon }));
  const reply = (await postText('/api/config',
    JSON.stringify({ fc_preset_idx: idx, fc_presets: wire })) || '').trim();
  await loadConfig();
  // guard() only throws on 401, so a device that refuses for any other reason
  // answers with text. Overwriting that with "Removed X." claimed a success
  // that had not happened and left the row to reappear unexplained.
  if(!/^ok\b/i.test(reply)){
    $('fcOut').textContent = 'The device refused: ' + (reply || '(no reply)');
    return;
  }
  // loadConfig has just refreshed fcPresets, so the device has now told us
  // whether what was saved can actually be drawn.
  const bad = fcPresets.filter(p => p.missing);
  $('fcOut').textContent = bad.length
    ? doneMsg + ' The panel cannot draw ' +
      bad.map(p => p.missing + ' in "' + p.name + '"').join(', ') +
      ' - those titles will come out as broken bytes.'
    : doneMsg;
}

$('fpAdd').onclick = async () => {
  const name = $('fpName').value.trim();
  const lat = Number($('fpLat').value);
  const lon = Number($('fpLon').value);
  if(!name){ $('fcOut').textContent = 'Give the place a name first.'; return; }
  if(!$('fpLat').value || !$('fpLon').value || !isFinite(lat) || !isFinite(lon)){
    $('fcOut').textContent = 'Latitude and longitude are both needed.';
    return;
  }
  if(lat < -90 || lat > 90 || lon < -180 || lon > 180){
    $('fcOut').textContent = 'Those are not degrees on this planet.';
    return;
  }
  // A name that is already listed is moved rather than duplicated, which is how
  // the WiFi profiles behave and saves a delete-then-add to correct a typo.
  const at = fcPresets.findIndex(p => p.name === name);
  if(at >= 0) fcPresets[at] = { name, lat, lon };
  else if(fcPresets.length >= 4){
    $('fcOut').textContent = 'All 4 slots are taken. Remove one first.';
    return;
  }
  else fcPresets.push({ name, lat, lon });
  $('fpName').value = '';
  $('fpLat').value = '';
  $('fpLon').value = '';
  await saveForecastPresets(fcPresetIdx, 'Saved "' + name + '".');
};

// The key is shown rather than masked, so what is in the box is what gets
// saved - including an empty box, which clears it.
$('fcSave').onclick = async () => {
  $('fcOut').textContent = await postText('/api/config',
    JSON.stringify({ ow_key: $('owKey').value.trim() }));
  await loadConfig();
};

$('fcNow').onclick = async () => {
  $('fcOut').textContent = 'Fetching...';
  $('fcOut').textContent = await postText('/api/forecast/fetch');
};

// --- wifi profiles -----------------------------------------------------------
// The device holds the list; the page edits it whole. The GET never carries
// passwords, so entries the user did not just type are sent back without one
// and the device keeps what it has for that SSID.
let wifiProfiles = [];

function renderWifiProfiles(){
  const box = $('wpList');
  box.textContent = '';
  wifiProfiles.forEach((p, i) => {
    const row = document.createElement('div');
    row.className = 'preset-item';
    const label = document.createElement('span');
    label.textContent = (i + 1) + '. ' + p.ssid + (p.pass ? '  (new password)' : '');
    const del = document.createElement('button');
    del.textContent = '\u2715';
    del.className = 'ghost';
    del.onclick = async () => {
      wifiProfiles.splice(i, 1);
      await saveWifiProfiles('Removed.');
    };
    row.append(label, del);
    box.appendChild(row);
  });
}

async function saveWifiProfiles(doneMsg){
  await postText('/api/config', JSON.stringify({ wifi_profiles: wifiProfiles }));
  await loadConfig();
  $('systemOut').textContent = doneMsg;
}

$('wpAdd').onclick = async () => {
  const ssid = $('wpSsid').value.trim();
  const pass = $('wpPass').value;
  if(!ssid){ $('systemOut').textContent = 'Give the network a name first.'; return; }
  const at = wifiProfiles.findIndex(p => p.ssid === ssid);
  const entry = pass ? { ssid, pass } : { ssid };
  if(at >= 0) wifiProfiles[at] = entry;
  else if(wifiProfiles.length >= 5){
    $('systemOut').textContent = 'All 5 slots are taken. Remove one first.';
    return;
  }
  else wifiProfiles.push(entry);
  $('wpSsid').value = '';
  $('wpPass').value = '';
  await saveWifiProfiles('Saved "' + ssid + '".');
};

// The rotation is a bitmask plus a sequence: the mask says which screens are in
// the loop, the sequence says in what order, which a mask cannot express. The
// list below is rendered in sequence order, so what the user sees is the order
// the device will run.
const SCREEN_NAMES = ['Clock / Weather', 'Analog', 'Mondaine', 'Mondaine White',
                      'Digital', 'Weather Digital', 'Date Digital', 'Photo Album',
                      'Plane Radar', 'Luftwaffe Junghans Borduhr', 'Weekly Forecast'];
let screenOrder = SCREEN_NAMES.map((_, i) => i);
let screenOn = SCREEN_NAMES.map(() => false);

function moveScreen(pos, delta){
  const to = pos + delta;
  if(to < 0 || to >= screenOrder.length) return;
  const tmp = screenOrder[pos];
  screenOrder[pos] = screenOrder[to];
  screenOrder[to] = tmp;
  renderThemeList();
}

// Index the row being dragged. Kept outside the render because rendering
// happens on every drop and would otherwise clear it mid-gesture.
let dragFrom = -1;

function dropAt(pos){
  if(dragFrom < 0 || dragFrom === pos) return;
  const moved = screenOrder.splice(dragFrom, 1)[0];
  screenOrder.splice(pos, 0, moved);
  dragFrom = -1;
  renderThemeList();
}

function renderThemeList(){
  const host = $('themeList');
  host.innerHTML = '';
  const lastOne = screenOrder.filter(id => screenOn[id]).length <= 1;
  screenOrder.forEach((id, pos) => {
    const row = document.createElement('div');
    row.className = screenOn[id] ? 'theme-row' : 'theme-row off';

    // Dragging moves a row; the arrows stay because HTML5 drag does not work
    // on most touch browsers, and this page is opened on phones.
    row.draggable = true;
    row.ondragstart = e => {
      dragFrom = pos;
      row.classList.add('dragging');
      e.dataTransfer.effectAllowed = 'move';
      // Firefox ignores a drag that carries no data.
      e.dataTransfer.setData('text/plain', String(pos));
    };
    row.ondragend = () => { dragFrom = -1; renderThemeList(); };
    row.ondragover = e => {
      e.preventDefault();
      e.dataTransfer.dropEffect = 'move';
      row.classList.add(pos < dragFrom ? 'drop-above' : 'drop-below');
    };
    row.ondragleave = () => row.classList.remove('drop-above', 'drop-below');
    row.ondrop = e => { e.preventDefault(); dropAt(pos); };

    const grip = document.createElement('span');
    grip.className = 'theme-grip';
    grip.textContent = '\u2630';
    grip.title = 'Drag to reorder';
    row.appendChild(grip);

    const seq = document.createElement('span');
    seq.className = 'theme-seq';
    seq.textContent = screenOn[id] ? String(screenOrder.filter(
      (x, i) => i <= pos && screenOn[x]).length) : '\u00B7';
    row.appendChild(seq);

    const label = document.createElement('label');
    label.className = 'check';
    const box = document.createElement('input');
    box.type = 'checkbox';
    box.className = 'screen';
    box.dataset.bit = id;
    box.checked = screenOn[id];
    // Something has to be on screen. Refusing the last untick here, rather than
    // at save time, means the list never shows a state the device cannot run.
    box.onchange = () => {
      if(!box.checked && lastOne){
        box.checked = true;
        $('displayOut').textContent = 'At least one screen has to stay on.';
        return;
      }
      screenOn[id] = box.checked;
      $('displayOut').textContent = '';
      renderThemeList();
    };
    label.appendChild(box);
    label.appendChild(document.createTextNode(' ' + SCREEN_NAMES[id]));
    row.appendChild(label);

    const up = document.createElement('button');
    up.className = 'ghost';
    up.textContent = '\u25B2';
    up.title = 'Show earlier';
    up.disabled = pos === 0;
    up.onclick = () => moveScreen(pos, -1);
    row.appendChild(up);

    const down = document.createElement('button');
    down.className = 'ghost';
    down.textContent = '\u25BC';
    down.title = 'Show later';
    down.disabled = pos === screenOrder.length - 1;
    down.onclick = () => moveScreen(pos, 1);
    row.appendChild(down);

    host.appendChild(row);
  });
}

// raw selection; 0 means the user ticked nothing
function selectedMask(){
  let m=0;
  screenOn.forEach((on,i)=>{ if(on) m|=1<<i; });
  return m;
}
// what the firmware will actually run: it falls back to Clock/Weather
function screenMask(){ return selectedMask()||1; }
$('saveConfig').onclick=saveConfig;
$('saveSystem').onclick=saveConfig;
$('saveDisplay').onclick=async()=>{
  // Belt and braces: the list will not let the count reach zero, but a save
  // must never send an empty mask whatever the page has been through.
  if(!selectedMask()){ $('displayOut').textContent='At least one screen has to stay on.'; return; }
  $('displayOut').textContent='Saving...';
  await saveConfig();
  const on=screenOrder.filter(id=>screenOn[id]).map(id=>SCREEN_NAMES[id]);
  $('displayOut').textContent='Saved. In order: '+on.join(' \u2192 ')+
    ' · interval '+$('themeInterval').value+'s';
};
$('logout').onclick=()=>{ location.href='/logout'; };

$('savePassword').onclick=async()=>{
  const next=$('pwNew').value, confirm=$('pwConfirm').value;
  if(!next){ $('pwOut').textContent='Enter a new password.'; return; }
  if(next.length>32){ $('pwOut').textContent='At most 32 characters.'; return; }
  if(next!==confirm){ $('pwOut').textContent='The two new entries do not match.'; return; }
  const r=await fetch('/api/password',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({current:$('pwCurrent').value,new:next})
  });
  const text=(await r.text()).trim();
  if(!r.ok){ $('pwOut').textContent=text||('HTTP '+r.status); return; }
  // The device replaced the session cookie in that response, so this browser
  // stays signed in and every other one is out.
  $('pwCurrent').value=$('pwNew').value=$('pwConfirm').value='';
  $('pwOut').textContent='Changed. Other browsers have been signed out.';
  await loadConfig();
};
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
  4: {dial:'Background', case:null, lume:'Hours, colon & temperature', hand:'Minutes & condition', accent:null},
  5: {dial:'Background', case:'Date', lume:'Hours, colon & weekday text', hand:'Minutes', accent:'Weekday background'}
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
// The page opens on System. Every panel starts hidden in the markup, so the
// choice lives here rather than in whichever section happened to be missing
// the class - and the gauges that sit inside System are filled from here for
// the same reason.
//
// show() rather than openTab(): openTab would fetch /status straight away, and
// the note at the top of this file is about what the first paint costs. The
// device answers one request at a time, so the gauges wait their turn behind
// the two calls that were always there.
show('system');
loadConfig()
  .then(loadWeather)
  .then(refreshGauges)
  .catch(e => {
    // Reported where the reader is standing. This used to go to the Status
    // panel, which is no longer the one on screen when the page opens.
    const msg = e.stack || String(e);
    $('systemOut').textContent = msg;
    $('statusOut').textContent = msg;
  });

// --- photo album -----------------------------------------------------------
// The device has no image decoder and no room for a decoded frame, so the
// conversion happens here: crop square, resize to the panel, and pack RGB565
// big-endian, which is the order the panel reads. What is uploaded is what gets
// pushed to the screen, with nothing in between.

// slot is the estimate for one JPEG photo; the device sends the real figure
// with every listing, and once a photo exists its measured size is used instead.
let album = {photos: [], slot: 30720, bytes: 0, fsFree: 0, fsTotal: 0, w: 240, h: 240, thumb: 40, max: 16};

// A radar map is written one byte per pixel against a palette the picture
// chose for itself, which halves it: 115,200 bytes becomes 58,116. Raw is
// still the point - repainting reads back arbitrary rectangles and no
// compressed format can be opened at a rectangle - but two bytes a pixel was
// paying for colours a map does not have. A tile of Seoul holds about 2,700
// distinct RGB565 colours; 256 chosen for that tile cost 2.5/255 of mean
// channel error.
//
//   0    "SDP8"
//   4    256 colours, RGB565 big-endian
//   516  240*240 index bytes
//
// Median cut, run over the 565 histogram rather than the pixels: the boxes
// hold at most a few thousand distinct colours instead of 57,600, and every
// pixel sharing a colour necessarily shares an index, so the mapping back is a
// table rather than a nearest-colour search.
function quantise565(imageData){
  const src = imageData.data;
  const n = src.length / 4;
  const px = new Uint16Array(n);
  const hist = new Uint32Array(65536);
  for(let i = 0, p = 0; p < n; i += 4, p++){
    const v = ((src[i] & 0xF8) << 8) | ((src[i+1] & 0xFC) << 3) | (src[i+2] >> 3);
    px[p] = v;
    hist[v]++;
  }

  const r8 = v => (v >>> 11) << 3, g8 = v => ((v >>> 5) & 63) << 2, b8 = v => (v & 31) << 3;
  const colours = [];
  for(let v = 0; v < 65536; v++) if(hist[v]) colours.push(v);

  // Extent along the widest channel, which is the axis a split gains most on.
  function widest(box){
    let lo = [255,255,255], hi = [0,0,0];
    for(const v of box){
      const c = [r8(v), g8(v), b8(v)];
      for(let k = 0; k < 3; k++){ if(c[k] < lo[k]) lo[k] = c[k]; if(c[k] > hi[k]) hi[k] = c[k]; }
    }
    let ch = 0, ext = hi[0] - lo[0];
    for(let k = 1; k < 3; k++) if(hi[k] - lo[k] > ext){ ext = hi[k] - lo[k]; ch = k; }
    return {ch, ext};
  }

  let boxes = [colours.slice()];
  while(boxes.length < 256){
    // Split the box with the most pixels times the widest spread. Six rules
    // were measured against a tile of Seoul; extent alone chases a handful of
    // outlying pixels, population alone leaves the worst pixel further out,
    // and the product came in at 2.54 mean channel error with the fewest
    // visibly wrong pixels of any of them.
    let pick = -1, best = 0, info = null;
    for(let i = 0; i < boxes.length; i++){
      if(boxes[i].length < 2) continue;
      const w = widest(boxes[i]);
      if(w.ext === 0) continue;
      let pop = 0;
      for(const v of boxes[i]) pop += hist[v];
      const score = w.ext * pop;
      if(score > best){ best = score; pick = i; info = w; }
    }
    if(pick < 0) break;
    const key = info.ch === 0 ? r8 : info.ch === 1 ? g8 : b8;
    const box = boxes[pick].slice().sort((a, b) => key(a) - key(b));
    // Cut at the population median, so both halves carry a similar share of
    // the picture rather than a similar span of the colour cube.
    let total = 0;
    for(const v of box) total += hist[v];
    let run = 0, cut = 0;
    while(cut < box.length - 1 && run + hist[box[cut]] <= total / 2){ run += hist[box[cut]]; cut++; }
    // One colour covering more than half the map - the sea, or the paper the
    // streets are drawn on - leaves the median at the very first entry, and a
    // cut there makes an empty box. Empty boxes are palette slots spent on
    // nothing, and a map is exactly the picture that has such a colour.
    if(cut < 1) cut = 1;
    if(cut > box.length - 1) cut = box.length - 1;
    boxes.splice(pick, 1, box.slice(0, cut), box.slice(cut));
  }

  const pal = boxes.map(box => {
    let wr = 0, wg = 0, wb = 0, wt = 0;
    for(const v of box){ const c = hist[v]; wr += r8(v)*c; wg += g8(v)*c; wb += b8(v)*c; wt += c; }
    return wt ? [wr/wt, wg/wt, wb/wt] : [0, 0, 0];
  });
  while(pal.length < 256) pal.push([0, 0, 0]);

  // Median cut draws boxes; it does not ask afterwards whether each colour
  // ended up beside the entry actually closest to it. Four Lloyd passes over
  // the histogram - not the pixels, so a few thousand points rather than
  // 57,600 - take the mean error from 3.18 to 2.54 and cut the visibly wrong
  // pixels from 4.6 percent to 1.2. It costs well under a second, once, on a
  // machine with a browser on it.
  const assign = new Uint8Array(65536);
  for(let pass = 0; pass < 4; pass++){
    for(const v of colours){
      const r = r8(v), g = g8(v), b = b8(v);
      let bi = 0, bd = Infinity;
      for(let i = 0; i < 256; i++){
        const p = pal[i];
        const dr = r - p[0], dg = g - p[1], db = b - p[2];
        const d = dr*dr + dg*dg + db*db;
        if(d < bd){ bd = d; bi = i; }
      }
      assign[v] = bi;
    }
    const sr = new Float64Array(256), sg = new Float64Array(256),
          sb = new Float64Array(256), st = new Float64Array(256);
    for(const v of colours){
      const i = assign[v], c = hist[v];
      sr[i] += r8(v)*c; sg[i] += g8(v)*c; sb[i] += b8(v)*c; st[i] += c;
    }
    for(let i = 0; i < 256; i++) if(st[i]) pal[i] = [sr[i]/st[i], sg[i]/st[i], sb[i]/st[i]];
  }

  const palette = new Uint16Array(256);
  for(let i = 0; i < 256; i++){
    const r = Math.round(pal[i][0]), g = Math.round(pal[i][1]), b = Math.round(pal[i][2]);
    palette[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
  const indices = new Uint8Array(n);
  for(let p = 0; p < n; p++) indices[p] = assign[px[p]];
  return {palette, indices, boxes: boxes.length};
}

function packIndexed(imageData){
  const {palette, indices, boxes} = quantise565(imageData);
  const out = new Uint8Array(516 + indices.length);
  out[0] = 0x53; out[1] = 0x44; out[2] = 0x50; out[3] = 0x38;   // SDP8
  for(let i = 0; i < 256; i++){
    out[4 + i*2] = palette[i] >> 8;      // big-endian, the panel's order
    out[5 + i*2] = palette[i] & 0xFF;
  }
  out.set(indices, 516);
  out.colours = boxes;
  return out;
}


// Square centre crop, then scale. Cropping before scaling keeps the aspect
// ratio honest; scaling a non-square source straight to 240x240 would squash it.
function squareTo(bitmap, size){
  const side = Math.min(bitmap.width, bitmap.height);
  const sx = (bitmap.width - side) / 2;
  const sy = (bitmap.height - side) / 2;
  const c = document.createElement('canvas');
  c.width = size; c.height = size;
  const g = c.getContext('2d', {willReadFrequently: true});
  g.imageSmoothingEnabled = true;
  g.imageSmoothingQuality = 'high';
  // Anything with alpha composites onto black, which is what the panel shows
  // where nothing is drawn.
  g.fillStyle = '#000';
  g.fillRect(0, 0, size, size);
  g.drawImage(bitmap, sx, sy, side, side, 0, 0, size, size);
  return g.getImageData(0, 0, size, size);
}

// Ids become filenames on the device, so two photos must never land on one.
// The timestamp alone does not settle it: two files with the same name picked
// in one go can be stamped in the same millisecond, and the second would
// overwrite the first. The counter breaks that tie.
let idSeq = 0;
function makeId(name){
  const stem = name.replace(/\.[^.]*$/, '').replace(/[^A-Za-z0-9_-]/g, '').slice(0, 10);
  const stamp = Date.now().toString(36).slice(-5);
  const seq = (idSeq++).toString(36);
  return (stem || 'photo') + '-' + stamp + seq;
}

async function putFile(path, bytes){
  const form = new FormData();
  form.append('file', new Blob([bytes], {type: 'application/octet-stream'}), path.split('/').pop());
  // The length goes with the request so the device can refuse before it takes
  // a single byte, and can tell afterwards whether it got all of them. Without
  // it a full filesystem is only discovered by a write coming up short, which
  // leaves a truncated file behind.
  const url = '/file?path=' + encodeURIComponent(path) + '&size=' + bytes.length;
  const r = guard(await fetch(url, {method: 'POST', body: form}));
  const txt = await r.text();
  if(!r.ok || /fail/i.test(txt)) throw new Error(txt.trim() || ('upload failed: ' + path));
}

// A photo goes up as JPEG now - about 20 KB against the 115 KB the raw pixels
// used to cost, which is what filled the device to 96 percent. The device
// decodes it itself, so the browser's job shrinks to crop, scale and encode.
// No thumbnail file either: the grid shows the JPEG directly.
function squareCanvas(bitmap, size){
  const side = Math.min(bitmap.width, bitmap.height);
  const sx = (bitmap.width - side) / 2;
  const sy = (bitmap.height - side) / 2;
  const c = document.createElement('canvas');
  c.width = size; c.height = size;
  const g = c.getContext('2d');
  g.imageSmoothingEnabled = true;
  g.imageSmoothingQuality = 'high';
  g.fillStyle = '#000';
  g.fillRect(0, 0, size, size);
  g.drawImage(bitmap, sx, sy, side, side, 0, 0, size, size);
  return c;
}

function toJpeg(canvas, quality){
  return new Promise((resolve, reject) => {
    canvas.toBlob(b => b ? resolve(b) : reject(new Error('encode failed')),
                  'image/jpeg', quality);
  });
}

async function addPhoto(file, note){
  let bitmap;
  try {
    bitmap = await createImageBitmap(file);
  } catch(e){
    throw new Error(file.name + ': not an image this browser can read');
  }
  const id = makeId(file.name);
  note(file.name + ': converting');
  const canvas = squareCanvas(bitmap, album.w);
  if(bitmap.close) bitmap.close();
  // 0.85 keeps a 240x240 photo well under 30 KB almost always; a rare busy
  // image that overshoots is re-encoded a step lower rather than shipped fat.
  let blob = await toJpeg(canvas, 0.85);
  if(blob.size > 40 * 1024) blob = await toJpeg(canvas, 0.72);
  const bytes = new Uint8Array(await blob.arrayBuffer());

  note(file.name + ': uploading ' + Math.round(bytes.length / 1024) + ' KB');
  await putFile('/album/' + id + '.jpg', bytes);
  album.photos.push({id: id, name: file.name.slice(0, 32), on: true, fmt: 'jpg'});
}

async function uploadFiles(files){
  const list = Array.from(files).filter(
    f => f.type.startsWith('image/') || /\.(png|jpe?g|gif|bmp|webp)$/i.test(f.name));
  if(!list.length){ $('albumOut').textContent = 'No image files in that selection.'; return; }

  const room = Math.max(0, Math.min(Math.floor(album.fsFree / album.slot),
                                    album.max - album.photos.length));
  let queue = list;
  if(list.length > room){
    $('albumOut').textContent = 'Only room for ' + room + ' more photo(s). '
      + 'Delete some first, or pick fewer.';
    if(room <= 0) return;
    queue = list.slice(0, room);
  }

  const note = m => { $('albumOut').textContent = m; };
  let done = 0;
  for(const f of queue){
    try {
      await addPhoto(f, m => note('(' + (done + 1) + '/' + queue.length + ') ' + m));
      done++;
    } catch(e){
      note('Stopped after ' + done + ': ' + (e.message || e));
      break;
    }
  }
  if(done){
    note('Uploaded ' + done + ' photo(s). Saving order...');
    await saveAlbum();
  }
}

// One photo at a time, in order - see the note at the img creation site - and
// each fetched once per visit. The grid re-renders on every reorder click, and
// without this cache each click refetched every photo through the one
// connection the device has. The object URLs live in the cache, so nothing is
// revoked until loadAlbum drops the whole map for fresh data.
const photoQueue = [];
const photoUrls = {};
let photoBusy = false;
let photoGen = 0;
function queuePhoto(img, id){
  if(photoUrls[id]){ img.src = photoUrls[id]; return; }
  photoQueue.push({img, id, gen: photoGen});
  pumpPhotos();
}
function dropPhotoUrls(){
  Object.values(photoUrls).forEach(u => URL.revokeObjectURL(u));
  Object.keys(photoUrls).forEach(k => delete photoUrls[k]);
  // Abandon what the previous grid was still waiting for. Without this a
  // reload queued every photo a second time behind the first run's leftovers,
  // and on a device that answers one request at a time that doubles the wait
  // for a grid whose <img> elements have already been thrown away.
  photoQueue.length = 0;
  photoGen++;
}
async function pumpPhotos(){
  if(photoBusy) return;
  photoBusy = true;
  while(photoQueue.length){
    const {img, id, gen} = photoQueue.shift();
    if(gen !== photoGen) continue;   // queued for a grid that no longer exists
    try {
      if(!photoUrls[id]){
        const r = await fetch('/api/album/photo?id=' + encodeURIComponent(id));
        if(r.ok) photoUrls[id] = URL.createObjectURL(await r.blob());
      }
      if(photoUrls[id]) img.src = photoUrls[id];
    } catch(e){ /* leave the cell blank rather than stall the queue */ }
  }
  photoBusy = false;
}

// Thumbnails come down as raw RGB565, the same format the device stores, so
// they are unpacked here rather than asking the device to encode a PNG.
async function paintThumb(canvas, id){
  const r = await fetch('/api/album/thumb?id=' + encodeURIComponent(id));
  if(!r.ok) return;
  const raw = new Uint8Array(await r.arrayBuffer());
  const n = album.thumb;
  if(raw.length < n * n * 2) return;
  const g = canvas.getContext('2d');
  const img = g.createImageData(n, n);
  for(let i = 0, o = 0; i < n * n; i++, o += 2){
    const v = (raw[o] << 8) | raw[o+1];
    img.data[i*4]     = (v >> 8) & 0xF8;
    img.data[i*4 + 1] = (v >> 3) & 0xFC;
    img.data[i*4 + 2] = (v << 3) & 0xF8;
    img.data[i*4 + 3] = 255;
  }
  g.putImageData(img, 0, 0);
}

function move(i, delta){
  const j = i + delta;
  if(j < 0 || j >= album.photos.length) return;
  const tmp = album.photos[i];
  album.photos[i] = album.photos[j];
  album.photos[j] = tmp;
  renderAlbum();
}

function renderAlbum(){
  const grid = $('albumGrid');
  grid.innerHTML = '';
  if(!album.photos.length){
    const empty = document.createElement('p');
    empty.className = 'hint';
    empty.textContent = 'No photos on the device yet.';
    grid.appendChild(empty);
  }
  album.photos.forEach((p, i) => {
    const cell = document.createElement('div');
    cell.className = p.on ? 'album-cell' : 'album-cell off';

    if(p.fmt === 'jpg'){
      // The photo is a real JPEG, and a browser needs no help showing one -
      // but the device needs help being asked. A browser fires six image
      // requests at once and the ESP serves one; the rest sit in a backlog it
      // barely has, and other requests get reset. The queue below fetches the
      // photos strictly one at a time instead of letting the <img> tags race.
      const im = document.createElement('img');
      im.className = 'album-thumb';
      queuePhoto(im, p.id);
      cell.appendChild(im);
    } else {
      const cv = document.createElement('canvas');
      cv.width = album.thumb; cv.height = album.thumb;
      cv.className = 'album-thumb';
      cell.appendChild(cv);
      paintThumb(cv, p.id);
    }

    const name = document.createElement('div');
    name.className = 'album-name';
    name.textContent = (i + 1) + '. ' + p.name;
    name.title = p.name;
    cell.appendChild(name);

    const row = document.createElement('div');
    row.className = 'album-actions';

    const left = document.createElement('button');
    left.className = 'ghost';
    left.textContent = '◀';
    left.title = 'Move earlier';
    left.disabled = i === 0;
    left.onclick = () => move(i, -1);
    row.appendChild(left);

    const right = document.createElement('button');
    right.className = 'ghost';
    right.textContent = '▶';
    right.title = 'Move later';
    right.disabled = i === album.photos.length - 1;
    right.onclick = () => move(i, 1);
    row.appendChild(right);

    const onOff = document.createElement('label');
    onOff.className = 'check';
    const box = document.createElement('input');
    box.type = 'checkbox';
    box.checked = p.on;
    box.onchange = () => { p.on = box.checked; renderAlbum(); };
    onOff.appendChild(box);
    onOff.appendChild(document.createTextNode(' show'));
    row.appendChild(onOff);

    const del = document.createElement('button');
    del.className = 'ghost';
    del.textContent = 'Delete';
    del.onclick = async () => {
      if(!confirm('Delete ' + p.name + ' from the device?')) return;
      $('albumOut').textContent = 'Deleting...';
      await postText('/api/album/delete?id=' + encodeURIComponent(p.id));
      await loadAlbum();
      $('albumOut').textContent = 'Deleted ' + p.name + '.';
    };
    row.appendChild(del);

    cell.appendChild(row);
    grid.appendChild(cell);
  });
  paintSpace();
}

function paintSpace(){
  const used = album.fsTotal - album.fsFree;
  const pct = album.fsTotal ? Math.round((used / album.fsTotal) * 100) : 0;
  $('albumFill').style.width = pct + '%';
  // Two different numbers, kept apart. The line used to show the whole
  // filesystem's usage on the album's own row, so an album holding nothing
  // still read as though it were using 900 KB - the firmware, the web files,
  // the clock face and the radar maps all counted as photos.
  const n = album.photos.length;
  const kb = b => Math.round(b / 1024) + ' KB';
  // Room left is whichever runs out first: index slots, or free space at what
  // this album's photos actually weigh. A guess is only used until there is a
  // real photo to measure.
  const each = n > 0 ? Math.max(1, album.bytes / n) : album.slot;
  const bySpace = Math.floor(album.fsFree / each);
  const bySlots = album.max - n;
  const room = Math.max(0, Math.min(bySpace, bySlots));
  // Which of the two ran out is worth saying. A megabyte free next to "room
  // for 10 more" is a puzzle; "room for 10 more (list holds 16)" is an answer.
  const why = bySlots < bySpace ? ' (list holds ' + album.max + ')' : '';
  $('albumSpace').textContent =
    n + ' of ' + album.max + ' photos · album ' + kb(album.bytes) + ' · ' +
    'device ' + kb(used) + ' / ' + kb(album.fsTotal) + ' · ' +
    'room for ' + room + ' more' + why;
}

async function loadAlbum(){
  const r = guard(await fetch('/api/album'));
  const d = await r.json();
  dropPhotoUrls();   // fresh data, fresh images - and the old blobs released
  album.photos = d.photos || [];
  album.slot = d.slot_bytes || album.slot;
  album.bytes = d.album_bytes || 0;
  album.fsFree = d.fs_free || 0;
  album.fsTotal = d.fs_total || 0;
  album.w = d.width || 240;
  album.h = d.height || 240;
  album.thumb = d.thumb || 40;
  album.max = d.max_photos || 16;
  $('albumInterval').value = d.interval_seconds || 10;
  renderAlbum();
}

async function saveAlbum(){
  $('albumOut').textContent = 'Saving...';
  const body = JSON.stringify({
    interval_seconds: Number($('albumInterval').value) || 10,
    photos: album.photos.map(p => ({id: p.id, name: p.name, on: p.on}))
  });
  const txt = await postText('/api/album', body);
  await loadAlbum();
  $('albumOut').textContent = txt.trim() === 'ok' ? 'Saved.' : txt;
}

$('pick').onchange = e => { uploadFiles(e.target.files); e.target.value = ''; };
$('saveAlbum').onclick = saveAlbum;
$('reloadAlbum').onclick = () => loadAlbum().then(() => { $('albumOut').textContent = 'Reloaded.'; });

const drop = $('drop');
['dragenter', 'dragover'].forEach(ev => drop.addEventListener(ev, e => {
  e.preventDefault();
  drop.classList.add('over');
}));
['dragleave', 'drop'].forEach(ev => drop.addEventListener(ev, e => {
  e.preventDefault();
  drop.classList.remove('over');
}));
drop.addEventListener('drop', e => uploadFiles(e.dataTransfer.files));

// Paste only lands here while the card is on screen, so it does not steal a
// paste meant for one of the other tabs' text fields.
document.addEventListener('paste', e => {
  if($('album').classList.contains('hidden')) return;
  const files = Array.from(e.clipboardData.files || []);
  if(files.length) uploadFiles(files);
});

// Loaded when its tab is first opened - see openTab.

// --- plane radar -----------------------------------------------------------
async function loadRadar(){
  const c = await getJson('/api/config');
  $('radarLat').value = c.radar_lat ?? 0;
  $('radarLon').value = c.radar_lon ?? 0;
  $('radarRange').value = c.radar_range_km ?? 10;
  $('radarPoll').value = c.radar_poll_sec ?? 10;
  $('radarMinAlt').value = c.radar_min_alt_ft ?? 0;
  $('radarUp').value = c.radar_up_deg ?? 0;
  $('radarRoutes').checked = c.radar_routes !== false;
  radarPresets = Array.isArray(c.radar_presets) ? c.radar_presets : [];
  radarBgId = c.radar_bg || '';
  radarLive = c;
  renderPresets();
  $('radarBgState').textContent = radarBgId
    ? 'In use: ' + radarBgId + '. The dial draws over it, dimmed to 35%.'
    : 'No background - the dial stays black.';
  await drawBgPreview();
  await showRadarState();
}

// What the device is set to right now, so a saved location can be shown as the
// one in use. Compared by value rather than by a stored flag: the answer stays
// right when the fields above are edited by hand, which a flag would not.
let radarLive = {};

function presetIsLive(p){
  if(radarLive.radar_lat === undefined) return false;
  const near = (a, b) => Math.abs(Number(a) - Number(b)) < 0.0002;
  return near(p.lat, radarLive.radar_lat) && near(p.lon, radarLive.radar_lon)
      && Number(p.km) === Number(radarLive.radar_range_km)
      && Number(p.up ?? 0) === Number(radarLive.radar_up_deg ?? 0)
      && Number(p.min_alt ?? 0) === Number(radarLive.radar_min_alt_ft ?? 0)
      && (p.bg || '') === (radarLive.radar_bg || '');
}

// The device stores the raw 240x240 it pushes to the panel, so the preview is
// built from that here - there is no encoder at the other end to ask for a PNG.
// Both formats have to be read: maps written before the palette change are
// still 115,200 bytes of RGB565 and still on devices, and a preview that only
// knew one of them hid itself for the other - which looks like a failed
// upload rather than a page that cannot read what it just wrote.
async function drawBgPreview(){
  const cv = $('radarBgPreview');
  if(!radarBgId){ cv.classList.add('hidden'); return; }
  try {
    const r = guard(await fetch('/api/radar/bg?id=' + encodeURIComponent(radarBgId)));
    const raw = new Uint8Array(await r.arrayBuffer());
    const indexed = raw.length >= 516 + 240 * 240 && raw.length < 240 * 240 * 2 &&
                    raw[0] === 0x53 && raw[1] === 0x44 && raw[2] === 0x50 && raw[3] === 0x38;
    if(!indexed && raw.length < 240 * 240 * 2){ cv.classList.add('hidden'); return; }
    const pal = indexed ? new Uint16Array(256) : null;
    if(indexed) for(let i = 0; i < 256; i++) pal[i] = (raw[4 + i*2] << 8) | raw[5 + i*2];
    const img = new ImageData(240, 240);
    for(let i = 0; i < 240 * 240; i++){
      // big-endian, as stored; indexed files keep their colours in the palette
      const v = indexed ? pal[raw[516 + i]] : (raw[i * 2] << 8) | raw[i * 2 + 1];
      img.data[i * 4]     = ((v >> 11) & 0x1F) * 255 / 31;
      img.data[i * 4 + 1] = ((v >> 5) & 0x3F) * 255 / 63;
      img.data[i * 4 + 2] = (v & 0x1F) * 255 / 31;
      img.data[i * 4 + 3] = 255;
    }
    const full = document.createElement('canvas');
    full.width = full.height = 240;
    full.getContext('2d').putImageData(img, 0, 0);
    const ctx = cv.getContext('2d');
    ctx.clearRect(0, 0, cv.width, cv.height);
    ctx.drawImage(full, 0, 0, cv.width, cv.height);
    cv.classList.remove('hidden');
  } catch(e){
    cv.classList.add('hidden');
  }
}

// --- background image. Decoded and packed by the browser. The device CAN
// decode JPEG now (the album does), but the radar map stays raw on purpose:
// repainting reads it back rectangle by rectangle to restore the ground under
// moved aircraft, and JPEG cannot be read at a random rectangle.
let radarBgId = '';

// Discarding a map used to leave the image behind. Nothing in the radar tab
// could reach it afterwards, so it sat there costing 113 KB of a 2 MB
// filesystem until someone went looking in Files under System - and on a
// device this full, four of them is the difference between a map fitting and
// not. Taking the map off a place now takes the file with it.
//
// The device is asked who still points at the image rather than the page's own
// copy of the list. This deletes a file for good, and the page's copy can be
// behind whenever anything has changed the config from outside it.
async function dropUnusedMap(id){
  if(!id) return '';
  const c = await getJson('/api/config');
  if((c.radar_bg || '') === id) return '';
  if((c.radar_presets || []).some(q => (q.bg || '') === id)) return '';
  const r = guard(await fetch('/file?path=' + encodeURIComponent('/radar/' + id + '.rgb'),
                              { method: 'DELETE' }));
  return r.ok ? ' The image was deleted from the device.'
              : ' The image could not be deleted - see Files under System.';
}

$('radarBgFile').onchange = async () => {
  const f = $('radarBgFile').files[0];
  if(!f) return;
  let bitmap;
  try { bitmap = await createImageBitmap(f); }
  catch(e){ $('radarBgState').textContent = 'Not an image this browser can read.'; return; }
  $('radarBgState').textContent = 'Choosing 256 colours...';
  const bytes = packIndexed(squareTo(bitmap, 240));
  if(bitmap.close) bitmap.close();
  const id = makeId('bg-' + f.name);
  $('radarBgState').textContent = 'Uploading ' + Math.round(bytes.length / 1024) +
    ' KB (' + bytes.colours + ' colours)...';
  await putFile('/radar/' + id + '.rgb', bytes);
  // The map belongs to the place it was uploaded for, not only to the live
  // config. Setting just radar_bg made the upload last until that same place
  // was loaded again - loading sends the place's own map, and for a place
  // saved without one that is an empty string, so it wiped the upload and left
  // the file as an orphan. From the outside the upload had simply not worked.
  const body = { radar_bg: id };
  const loaded = radarLive.radar_preset || '';
  const at = radarPresets.findIndex(p => p.name === loaded);
  if(at >= 0){
    radarPresets[at] = Object.assign({}, radarPresets[at], { bg: id });
    body.radar_presets = radarPresets;
  }
  await postText('/api/config', JSON.stringify(body));
  $('radarBgFile').value = '';
  await loadRadar();
  $('radarBgState').textContent = at >= 0
    ? 'Map saved with "' + loaded + '".'
    : 'Map set. It belongs to no saved place yet - save one to keep it.';
};

$('radarBgClear').onclick = async () => {
  // A map uploaded with no place loaded belongs to nothing, so clearing it
  // here is the only chance to notice. If a saved place does claim it, the
  // file stays and only the dial goes plain.
  const victim = radarBgId;
  await postText('/api/config', JSON.stringify({ radar_bg: '' }));
  const note = await dropUnusedMap(victim);
  await loadRadar();
  $('radarBgState').textContent = 'Map cleared.' + note;
};

// --- saved locations. The device holds the list; the page just edits it whole.
let radarPresets = [];
// Which saved location the fields were last filled from. presetIsLive only says
// "the device matches this one exactly", so it goes dark the moment a value is
// edited - which is precisely when you still want to know what you are editing.
// This survives the edit, so Save can offer to overwrite instead of asking for
// a name that already exists.
// No local copy of "which place is loaded". The device holds that now, and a
// second copy here could only ever disagree with it - the page already had one
// state too many when it was comparing values to work the same thing out.
function refreshSaveButton(){
  const typed = $('presetName').value.trim();
  const known = radarPresets.some(p => p.name === typed);
  $('presetSave').textContent = typed && known ? 'Update "' + typed + '"' : 'Save as new';
  const note = $('presetEditing');
  if(note){
    const loaded = radarLive.radar_preset || '';
    note.textContent = loaded ? 'Editing: ' + loaded : '';
  }
}

function renderPresets(){
  const box = $('presetList');
  box.textContent = '';
  // The device remembers which place it was loaded from, by name. Comparing
  // values instead could not tell apart two places holding the same settings -
  // both claimed IN USE - and lost the answer the moment a field was edited.
  // The value comparison still earns its keep: it says whether what is on the
  // device is still that place, or an edited version of it.
  const loadedName = radarLive.radar_preset || '';
  radarPresets.forEach((p, i) => {
    const row = document.createElement('div');
    const live = (p.name === loadedName);
    const same = live && presetIsLive(p);
    row.className = live ? 'preset-item active' : 'preset-item';
    if(live){
      const badge = document.createElement('span');
      badge.className = same ? 'badge' : 'badge edited';
      badge.textContent = same ? 'IN USE' : 'EDITED';
      badge.title = same ? 'The radar is set to this place.'
                         : 'Loaded from this place, then changed. Press the Update button above to keep the changes.';
      row.appendChild(badge);
    }
    const label = document.createElement('span');
    label.textContent = p.name + '  (' + Number(p.lat).toFixed(4) + ', ' +
      Number(p.lon).toFixed(4) + ' · ' + p.km + 'km · ' + (p.up ?? 0) + '°' +
      ((p.min_alt ?? 0) > 0 ? ' · ≥' + p.min_alt + 'ft' : '') +
      (p.bg ? ' · map' : '') + ')';
    const load = document.createElement('button');
    load.textContent = 'Load';
    load.onclick = async () => {
      $('radarOut').textContent = 'Loading "' + p.name + '"...';
      // Carry the name into the form, so editing a loaded place and pressing
      // Save updates it rather than demanding a new name for the same location.
      $('presetName').value = p.name;
      // The device is told which place this is, so the answer survives a reload
      // and an edit - the page no longer has to guess it back from the numbers.
      await postText('/api/config', JSON.stringify({
        radar_lat: Number(p.lat), radar_lon: Number(p.lon), radar_range_km: Number(p.km),
        radar_up_deg: Number(p.up ?? 0), radar_min_alt_ft: Number(p.min_alt ?? 0),
        radar_bg: p.bg || '', radar_preset: p.name
      }));
      await loadRadar();
      $('presetName').value = p.name;
      refreshSaveButton();
      $('radarOut').textContent = 'Loaded "' + p.name + '". Edit the fields above, then press Update to keep the changes.';
      // loadRadar has already refreshed the list and the preview from the
      // device, so what the page shows is what the device is set to.
    };
    // No Update button on the row. It wrote the form's current values into
    // this place whether or not this place was the one loaded, so pressing it
    // on the wrong line moved a saved location to wherever the form happened to
    // be pointing - silently, and over a name that still read correctly. The
    // Save button above already does the safe version: it changes to
    // Update "<name>" once the name in the field matches a saved place, which
    // Load fills in, so updating means editing what you loaded and pressing it.

    const del = document.createElement('button');
    del.textContent = '✕';
    del.className = 'ghost';
    del.onclick = async () => {
      // If the device was pointing at this one, the pointer goes too - otherwise
      // a later place with the same name would inherit its badge.
      const wasLoaded = (radarLive.radar_preset || '') === p.name;
      // The place is going, so its map goes too - both the pointer on the dial
      // and the file, if no other place kept a claim on it. Leaving the image
      // behind was how the collection grew maps nothing could reach.
      const victim = p.bg || '';
      const onDial = victim && (radarLive.radar_bg || '') === victim;
      radarPresets.splice(i, 1);
      await savePresets('Deleted.', wasLoaded ? '' : undefined,
                        onDial ? { radar_bg: '' } : undefined);
      const note = await dropUnusedMap(victim);
      if(note) $('radarOut').textContent = 'Deleted.' + note;
    };
    if(p.bg){
      // Takes the map off this location and deletes the image, unless some
      // other place still wants it. If this map is also the one on the dial
      // right now, the dial goes back to plain in the same request rather than
      // keeping a picture its owner just discarded.
      const unmap = document.createElement('button');
      unmap.textContent = 'Map ✕';
      unmap.className = 'ghost';
      unmap.title = 'Take the map off this place and delete the image.';
      unmap.onclick = async () => {
        const victim = p.bg;
        const body = { radar_presets: radarPresets.map((q, j) => {
          if(j !== i) return q;
          const copy = Object.assign({}, q);
          copy.bg = '';
          return copy;
        }) };
        if((radarLive.radar_bg || '') === victim) body.radar_bg = '';
        await postText('/api/config', JSON.stringify(body));
        const note = await dropUnusedMap(victim);
        await loadRadar();
        $('radarOut').textContent = 'Map removed from "' + p.name + '".' + note;
      };
      row.append(label, load, unmap, del);
    } else {
      row.append(label, load, del);
    }
    box.appendChild(row);
  });
  refreshSaveButton();
}

// One request. Sending the list and the loaded name separately meant the device
// rewrote config twice for one action, and left a moment where the two
// disagreed - a name pointing at a place the list had not been told about yet.
async function savePresets(doneMsg, loadedName, extra){
  const body = Object.assign({ radar_presets: radarPresets }, extra || {});
  if(loadedName !== undefined) body.radar_preset = loadedName;
  await postText('/api/config', JSON.stringify(body));
  await loadRadar();
  refreshSaveButton();
  $('radarOut').textContent = doneMsg;
}

$('presetName').oninput = refreshSaveButton;
$('presetSave').onclick = async () => {
  const name = $('presetName').value.trim();
  const lat = Number($('radarLat').value);
  const lon = Number($('radarLon').value);
  const km = Number($('radarRange').value) || 10;
  if(!name){ $('radarOut').textContent = 'Give the place a name first.'; return; }
  if(!isFinite(lat) || !isFinite(lon) || (lat === 0 && lon === 0) ||
     Math.abs(lat) > 90 || Math.abs(lon) > 180){
    $('radarOut').textContent = 'Latitude and longitude look wrong.';
    return;
  }
  const entry = { name, lat, lon, km,
    up: ((Number($('radarUp').value) || 0) % 360 + 360) % 360,
    min_alt: Number($('radarMinAlt').value) || 0,
    bg: radarBgId };
  const at = radarPresets.findIndex(p => p.name === name);
  if(at >= 0) radarPresets[at] = entry;
  else if(radarPresets.length >= 6){
    $('radarOut').textContent = 'All 6 slots are taken. Delete one first.';
    return;
  }
  else radarPresets.push(entry);
  await savePresets((at >= 0 ? 'Updated "' : 'Saved "') + name + '".', name);
};

// The device only polls while the radar is the screen on show, so this reports
// the last fetch rather than forcing one - asking for a TLS handshake just to
// fill in a settings page is not a fair trade on this chip.
async function showRadarState(){
  try {
    const r = guard(await fetch('/api/radar'));
    const d = await r.json();
    const lines = ['status: ' + d.status + '   ' + d.fetch_ms + ' ms   ' + d.count + ' aircraft',
                   'routes: ' + d.route_status + '   ' + d.route_ms + ' ms   ' + d.routes_cached + ' cached'];
    (d.ac || []).forEach(a => lines.push(
      '  ' + String(a.cs || '?').padEnd(9) + ' ' +
      Number(a.km).toFixed(1).padStart(5) + ' km   brg ' +
      Number(a.brg).toFixed(0).padStart(3) + '   ' + a.alt + ' ft'));
    $('radarOut').textContent = lines.join(String.fromCharCode(10));
  } catch(e){
    $('radarOut').textContent = e.message || String(e);
  }
}

$('saveRadar').onclick = async () => {
  const lat = Number($('radarLat').value);
  const lon = Number($('radarLon').value);
  if(!isFinite(lat) || !isFinite(lon) || Math.abs(lat) > 90 || Math.abs(lon) > 180){
    $('radarOut').textContent = 'Latitude and longitude look wrong.';
    return;
  }
  if(lat === 0 && lon === 0){
    $('radarOut').textContent = 'Set your own position: 0,0 is the radar off switch.';
    return;
  }
  $('radarOut').textContent = 'Saving...';
  await postText('/api/config', JSON.stringify({
    radar_lat: lat,
    radar_lon: lon,
    radar_range_km: Number($('radarRange').value) || 10,
    radar_poll_sec: Number($('radarPoll').value) || 10,
    radar_min_alt_ft: Number($('radarMinAlt').value) || 0,
    radar_up_deg: ((Number($('radarUp').value) || 0) % 360 + 360) % 360,
    radar_routes: $('radarRoutes').checked
  }));
  await loadRadar();
};

$('fetchRadar').onclick = async () => {
  $('radarOut').textContent = 'Fetching...';
  const txt = await postText('/api/radar/fetch');
  await showRadarState();
  if(/^(?!ok)/.test(txt.trim())) $('radarOut').textContent = txt + $('radarOut').textContent;
};

// Loaded when its tab is first opened - see openTab.
