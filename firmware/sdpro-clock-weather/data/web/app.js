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
  $('pwHint').textContent = c.web_password_is_default
    ? 'Still set to the factory password. Change it below.'
    : 'Used to sign in to this menu. 1 to 32 characters.';
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
    screen_order:screenOrder,
    theme_interval_seconds:+$('themeInterval').value
  };
  if($('wifiPass').value) body.pass=$('wifiPass').value;
  $('weatherOut').textContent=await postText('/api/config',JSON.stringify(body));
  $('systemOut').textContent=$('weatherOut').textContent;
  await loadConfig(); await loadWeather();
}

// The rotation is a bitmask plus a sequence: the mask says which screens are in
// the loop, the sequence says in what order, which a mask cannot express. The
// list below is rendered in sequence order, so what the user sees is the order
// the device will run.
const SCREEN_NAMES = ['Clock / Weather', 'Analog', 'Mondaine', 'Mondaine White',
                      'Digital', 'Weather Digital', 'Date Digital', 'Photo Album',
                      'Plane Radar'];
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
loadConfig().then(loadWeather).catch(e=>$('statusOut').textContent=e.stack||String(e));

// --- photo album -----------------------------------------------------------
// The device has no image decoder and no room for a decoded frame, so the
// conversion happens here: crop square, resize to the panel, and pack RGB565
// big-endian, which is the order the panel reads. What is uploaded is what gets
// pushed to the screen, with nothing in between.

let album = {photos: [], slot: 118400, fsFree: 0, fsTotal: 0, w: 240, h: 240, thumb: 40, max: 16};

function pack565(imageData){
  const src = imageData.data;
  const out = new Uint8Array((src.length / 4) * 2);
  for(let i = 0, o = 0; i < src.length; i += 4, o += 2){
    const v = ((src[i] & 0xF8) << 8) | ((src[i+1] & 0xFC) << 3) | (src[i+2] >> 3);
    out[o] = v >> 8;      // high byte first: the panel's order, not the CPU's
    out[o+1] = v & 0xFF;
  }
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
  const r = guard(await fetch('/file?path=' + encodeURIComponent(path), {method: 'POST', body: form}));
  const txt = await r.text();
  if(!r.ok || /fail/i.test(txt)) throw new Error(txt.trim() || ('upload failed: ' + path));
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
  const full = pack565(squareTo(bitmap, album.w));
  const thumb = pack565(squareTo(bitmap, album.thumb));
  if(bitmap.close) bitmap.close();

  note(file.name + ': uploading ' + Math.round(full.length / 1024) + ' KB');
  await putFile('/album/' + id + '.rgb', full);
  await putFile('/album/' + id + '.thm', thumb);
  album.photos.push({id: id, name: file.name.slice(0, 32), on: true});
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

    const cv = document.createElement('canvas');
    cv.width = album.thumb; cv.height = album.thumb;
    cv.className = 'album-thumb';
    cell.appendChild(cv);
    paintThumb(cv, p.id);

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
  const room = Math.max(0, Math.min(Math.floor(album.fsFree / album.slot),
                                    album.max - album.photos.length));
  $('albumSpace').textContent =
    album.photos.length + ' of ' + album.max + ' photos · ' +
    Math.round(used / 1024) + ' / ' + Math.round(album.fsTotal / 1024) + ' KB used · ' +
    'room for ' + room + ' more';
}

async function loadAlbum(){
  const r = guard(await fetch('/api/album'));
  const d = await r.json();
  album.photos = d.photos || [];
  album.slot = d.slot_bytes || album.slot;
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

loadAlbum().catch(e => { $('albumOut').textContent = e.message || String(e); });

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
async function drawBgPreview(){
  const cv = $('radarBgPreview');
  if(!radarBgId){ cv.classList.add('hidden'); return; }
  try {
    const r = guard(await fetch('/api/radar/bg?id=' + encodeURIComponent(radarBgId)));
    const raw = new Uint8Array(await r.arrayBuffer());
    if(raw.length < 240 * 240 * 2){ cv.classList.add('hidden'); return; }
    const img = new ImageData(240, 240);
    for(let i = 0; i < 240 * 240; i++){
      const v = (raw[i * 2] << 8) | raw[i * 2 + 1];   // big-endian, as stored
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

// --- background image. Decoded and packed by the browser, like the album:
// the device has no room for a JPG decoder, so it only ever streams raw pixels.
let radarBgId = '';

$('radarBgFile').onchange = async () => {
  const f = $('radarBgFile').files[0];
  if(!f) return;
  let bitmap;
  try { bitmap = await createImageBitmap(f); }
  catch(e){ $('radarBgState').textContent = 'Not an image this browser can read.'; return; }
  $('radarBgState').textContent = 'Converting...';
  const bytes = pack565(squareTo(bitmap, 240));
  if(bitmap.close) bitmap.close();
  const id = makeId('bg-' + f.name);
  $('radarBgState').textContent = 'Uploading ' + Math.round(bytes.length / 1024) + ' KB...';
  await putFile('/radar/' + id + '.rgb', bytes);
  await postText('/api/config', JSON.stringify({ radar_bg: id }));
  $('radarBgFile').value = '';
  await loadRadar();
};

$('radarBgClear').onclick = async () => {
  await postText('/api/config', JSON.stringify({ radar_bg: '' }));
  await loadRadar();
};

// --- saved locations. The device holds the list; the page just edits it whole.
let radarPresets = [];

function renderPresets(){
  const box = $('presetList');
  box.textContent = '';
  radarPresets.forEach((p, i) => {
    const row = document.createElement('div');
    const live = presetIsLive(p);
    row.className = live ? 'preset-item active' : 'preset-item';
    if(live){
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = 'IN USE';
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
      await postText('/api/config', JSON.stringify({
        radar_lat: Number(p.lat), radar_lon: Number(p.lon), radar_range_km: Number(p.km),
        radar_up_deg: Number(p.up ?? 0), radar_min_alt_ft: Number(p.min_alt ?? 0),
        radar_bg: p.bg || ''
      }));
      await loadRadar();
      $('radarOut').textContent = 'Loaded "' + p.name + '". The radar is watching there now.';
      // loadRadar has already refreshed the list and the preview from the
      // device, so what the page shows is what the device is set to.
    };
    const del = document.createElement('button');
    del.textContent = '✕';
    del.className = 'ghost';
    del.onclick = async () => {
      radarPresets.splice(i, 1);
      await savePresets('Deleted.');
    };
    row.append(label, load, del);
    box.appendChild(row);
  });
}

async function savePresets(doneMsg){
  await postText('/api/config', JSON.stringify({ radar_presets: radarPresets }));
  await loadRadar();
  $('radarOut').textContent = doneMsg;
}

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
  $('presetName').value = '';
  await savePresets('Saved "' + name + '".');
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

loadRadar().catch(e => { $('radarOut').textContent = e.message || String(e); });
