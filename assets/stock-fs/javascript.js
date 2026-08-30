/**
 * Smart Weather Clock - Universal Controller
 * 完美兼容 index.html (全功能) 和 indexwifi.html (精简版)
 */

// ============================
// 1. 全局配置与初始化
// ============================
const THEME_DICT = [
    { id: 0, name: "Classic" },
    { id: 1, name: "Weather" },
    { id: 2, name: "Photo" },
    { id: 3, name: "Dial" },
    { id: 4, name: "Simple" },
    { id: 5, name: "Weather forecast" },
    { id: 6, name: "Flip Clock" }
];

// 全局防抖计时器 (只保留这一个)
let debounceTimers = {};

document.addEventListener('DOMContentLoaded', () => {
    console.log("System Initializing...");    
	// 初始化主题 UI
    initThemeUI();
    loadStorageData();
    
    // 为了防止部分页面没有这些函数报错，加上安全检查
    if(typeof initConfig === 'function') initConfig();
    if(typeof attachEventListeners === 'function') attachEventListeners();
    if(typeof refreshList === 'function') refreshList();
    

});

// ============================
// 2. 存储数据获取 (合并去重版)
// ============================
function loadStorageData() {
    fetch('/photo/list')
        .then(res => res.json())
        .then(data => {
            if(typeof updateStorageUI === 'function') updateStorageUI(data.total, data.used);
            if (document.getElementById('fileList') && typeof updateFileList === 'function') {
                updateFileList(data.files);
            }
        })
        .catch(err => {
            console.error("Storage Check Error:", err);
            const textEl = document.getElementById('storage-text');
            if(textEl) textEl.innerText = "Connection Error";
        });
}

// ============================
// 3. 核心优化：前端秒开 UI 
// ============================
function initThemeUI() {
    const list = document.getElementById('themeList');
    if (!list) return; // 如果当前页面没有主题列表框，直接退出
    
    list.innerHTML = ''; // 清除 "Loading themes..." 提示
    
    THEME_DICT.forEach(theme => {
        const li = document.createElement('li');
        li.className = 'file-item';
        li.innerHTML = `
            <div class="file-info">
                <input type="checkbox" id="theme_cb_${theme.id}" class="play-checkbox" 
                       onchange="toggleTheme(${theme.id}, this)"
                       style="width: 22px; height: 22px; margin-right: 12px; accent-color: #34c759;">
                <span class="file-name" style="font-size: 16px;">${theme.name}</span>
            </div>
        `;
        list.appendChild(li);
    });
    
    // 列表画好之后，再向 8266 询问哪些该打勾
    loadThemeSettings();
}

// ============================
// 4. 后台同步：仅同步状态
// ============================
function loadThemeSettings() {
    fetch('/theme/list')
        .then(res => res.json())
        .then(data => {
            const intervalSelect = document.getElementById('themeInterval');
            if(intervalSelect && data.interval) {
                intervalSelect.value = data.interval;
            }

            if (data.themes) {
                data.themes.forEach(t => {
                    const cb = document.getElementById('theme_cb_' + t.id);
                    if (cb) cb.checked = t.enabled;
                });
            }
        })
        .catch(err => console.log("Theme load failed", err));
}

// ============================
// 5. 用户交互 (防止全关白屏)
// ============================
function toggleTheme(id, checkbox) {
    const isChecked = checkbox.checked;

    if (!isChecked) {
        const allCheckboxes = document.querySelectorAll('.play-checkbox');
        let activeCount = 0;
        allCheckboxes.forEach(box => {
            if (box.checked) activeCount++;
        });

        if (activeCount === 0) {
            alert("At least one theme must be enabled!"); 
            checkbox.checked = true; 
            return; 
        }
    }

    const state = isChecked ? 1 : 0;
    fetch(`/theme/toggle?id=${id}&state=${state}`)
        .then(res => { 
            if(!res.ok) console.log("Toggle failed"); 
        })
        .catch(err => console.error(err));
}

function updateThemeInterval(seconds) {
    fetch(`/theme/interval?val=${seconds}`)
        .then(res => { if(res.ok) console.log("Theme interval updated"); })
        .catch(err => console.error(err));
}

// ============================
// 6. UI 更新与格式化工具
// ============================
function updateStorageUI(total, used) {
    // 【修复】加上 const 防止变量泄漏到全局
    const currentFreeSpace = total - used;
    // 防止除以 0 导致报错
    if (total === 0) return;

    const percent = Math.round((used / total) * 100);
    const bar = document.getElementById('storage-bar');
    
    if (bar) {
        bar.style.width = percent + '%';
        if(percent > 90) bar.style.backgroundColor = 'var(--danger-color, #ff3b30)';
        else bar.style.backgroundColor = 'var(--success-color, #34c759)';
    }

    const textEl = document.getElementById('storage-text');
    const freeEl = document.getElementById('storage-free');
    
    if (textEl) textEl.innerText = `${formatSize(used)} / ${formatSize(total)}`;
    if (freeEl) freeEl.innerText = `Free: ${formatSize(currentFreeSpace)}`;
}

function formatSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

// ============================================
// 7. 表单回显逻辑 (Config Load)
// ============================================
function initConfig() {
    fetch('/config')
        .then(response => response.json())
        .then(data => {
            console.log("Config Data:", data);

            if (data.freespace !== undefined) {
                const storageEl = document.getElementById("storage-info");
                if (storageEl) {
                    const freeBytes = data.freespace;
                    let displayStr = (freeBytes > 1024 * 1024) 
                        ? (freeBytes / (1024 * 1024)).toFixed(2) + " MB" 
                        : (freeBytes / 1024).toFixed(2) + " KB";
                    storageEl.innerHTML = `Free Storage: ${displayStr}`;
                    if (freeBytes < 20480) storageEl.style.color = "red"; 
                }
            }

            safeSetValue('giflist', data.gifnum);
            safeSetValue('themeselect', data.theme);
            safeSetValue('timelist', data.timeformat);
            safeSetValue('cityInput', data.city);
            safeSetValue('ledBrightness', data.brightness); 
            
            safeSetCheck('led-switch', data.celsius);
            safeSetCheck('hourtime-switch', data.hour12);
            safeSetCheck('Mile-switch', data.mile);
            safeSetCheck('sync-with-phone', data.sync);

            safeSetCheck('Nightmode', data.nightmode);
            safeSetValue('starttime', data.starttime);
            safeSetValue('stoptime', data.stoptime);
            safeSetValue('NightBrightness', data.nightbrightness);

            safeSetValue('ntp', data.ntp);
            safeSetValue('openweathermapKey', data.weatherkey);
            safeSetValue('new_ssid', data.ssid);
            safeSetValue('password', data.password);

            if (data.color1) safeSetValue('colorPicker0', rgb565ToHex(data.color1));
            if (data.color2) safeSetValue('colorPicker1', rgb565ToHex(data.color2));
            if (data.color3) safeSetValue('colorsecond2', rgb565ToHex(data.color3));
        })
        .catch(err => console.error('Config Load Error:', err));
}

// ============================================
// 8. 事件监听 (安全绑定)
// ============================================
function attachEventListeners() {
    safeBindListener('btnRestart', 'click', performRestart);
    window.restartESP = performRestart;

    const wifiForm = document.querySelector('form[action="/connect"]');
    if (wifiForm) {
        wifiForm.addEventListener('submit', function(e) {
            e.preventDefault(); 
            const ssid = document.getElementById('new_ssid').value;
            const pass = document.getElementById('password').value;
            if (!ssid) { alert("SSID is required!"); return; }

            sendConfig('ssid', ssid);
            setTimeout(() => {
                sendConfig('password', pass);
                alert("Wi-Fi configuration successful");
            }, 500);
        });
    }

    safeBindListener('ledBrightness', 'input', (e) => sendConfigDebounced('lcd_brightness', e.target.value));

    safeBindListener('giflist', 'change', (e) => sendConfig('gifnum', e.target.value));
    safeBindListener('themeselect', 'change', (e) => sendConfig('theme', e.target.value));
    safeBindListener('timelist', 'change', (e) => sendConfig('timeformat', e.target.value));

    safeBindListener('led-switch', 'change', (e) => sendConfig('celsius', e.target.checked ? '1' : '0'));
    safeBindListener('hourtime-switch', 'change', (e) => sendConfig('time12_24', e.target.checked ? '1' : '0'));
    safeBindListener('Mile-switch', 'change', (e) => sendConfig('mile', e.target.checked ? '1' : '0'));
    
    safeBindListener('sync-with-phone', 'change', (e) => {
        const checked = e.target.checked;
        sendConfig('timesync', checked ? '1' : '0');
        if (checked) {
            const tz = -new Date().getTimezoneOffset() / 60;
            setTimeout(() => {
                sendConfig('zone', tz);
                alert("Synced Timezone: " + tz);
            }, 200);
        }
    });

    safeBindListener('Nightmode', 'change', (e) => sendConfig('nightmode', e.target.checked ? '1' : '0'));
    safeBindListener('starttime', 'change', (e) => sendConfig('starttime', e.target.value));
    safeBindListener('stoptime', 'change', (e) => sendConfig('stoptime', e.target.value));
    safeBindListener('NightBrightness', 'input', (e) => sendConfigDebounced('nightbrightness', e.target.value));

    ['colorPicker0', 'colorPicker1', 'colorsecond2'].forEach((id, idx) => {
        safeBindListener(id, 'input', (e) => {
            const key = idx === 0 ? 'color1' : (idx === 1 ? 'color2' : 'color3');
            sendConfigDebounced(key, hexToRgb565(e.target.value), 200);
            if(idx === 0) handleColorChange(e.target.value);
        });
    });
}

// ============================================
// 9. 辅助工具函数集
// ============================================
function performRestart() {
    console.log("Restart Triggered");
    let confirmed = true;
    try { confirmed = confirm("System Restart: Are you sure?"); } 
    catch(e) { console.log("Confirm dialog blocked."); }

    if (confirmed) {
        fetch('/restart')
            .then(() => alert('Command Sent: Restarting...'))
            .catch(e => {
                console.error(e);
                alert("Restarting..."); 
            });
    }
}

function safeBindListener(id, type, func) {
    const el = document.getElementById(id);
    if (el) el.addEventListener(type, func);
}

function safeSetValue(id, val) {
    const el = document.getElementById(id);
    if (el && val !== undefined) el.value = val;
}

function safeSetCheck(id, val) {
    const el = document.getElementById(id);
    if (el && val !== undefined) {
        el.checked = (val === true || val === "true" || val == 1);
    }
}

function sendConfig(key, value) {
    fetch(`/api/set?key=${key}&value=${encodeURIComponent(value)}`).catch(console.error);
}

function sendConfigDebounced(key, value, delay = 300) {
    if (debounceTimers[key]) clearTimeout(debounceTimers[key]);
    debounceTimers[key] = setTimeout(() => sendConfig(key, value), delay);
}

window.refreshList = function() {
    fetch('/scanwifi')
        .then(res => res.text())
        .then(html => {
            const list = document.getElementById("wifi_list");
            if(list) list.innerHTML = html;
        });
};

function hexToRgb565(hex) {
    hex = hex.replace(/^#/, '');
    const r = parseInt(hex.substring(0, 2), 16);
    const g = parseInt(hex.substring(2, 4), 16);
    const b = parseInt(hex.substring(4, 6), 16);
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

function rgb565ToHex(val) {
    if (!val) return "#000000";
    const r = (val >> 11) & 0x1F;
    const g = (val >> 5) & 0x3F;
    const b = val & 0x1F;
    const toHex = c => c.toString(16).padStart(2, '0');
    return "#" + toHex((r * 255 / 31)|0) + toHex((g * 255 / 63)|0) + toHex((b * 255 / 31)|0);
}

function handleColorChange(hex) {
    const r = parseInt(hex.substr(1, 2), 16);
    const g = parseInt(hex.substr(3, 2), 16);
    const b = parseInt(hex.substr(5, 2), 16);
    
    if (debounceTimers['main_color']) clearTimeout(debounceTimers['main_color']);
    debounceTimers['main_color'] = setTimeout(() => {
        sendConfig('ws_r', r);
        setTimeout(() => sendConfig('ws_g', g), 50);
        setTimeout(() => sendConfig('ws_b', b), 100);
    }, 200);
}

window.submitForm = function(inputId) {
    const fileInput = document.getElementById(inputId || 'imageFile1');
    if (!fileInput || !fileInput.files[0]) return;
    
    const file = fileInput.files[0];
    const statusDiv = document.querySelector(".helper-text"); 
    
    if (file.type !== 'image/gif') { alert("Only GIF allowed"); return; }
    
    const img = new Image();
    const objUrl = URL.createObjectURL(file);
    
    if(statusDiv) statusDiv.innerText = "Checking size...";
    
    img.onload = () => {
        if (img.width !== 80 || img.height !== 80) {
            alert(`Size Error: ${img.width}x${img.height}. Require 80x80.`);
            URL.revokeObjectURL(objUrl);
            return;
        }
        if(statusDiv) statusDiv.innerText = "Uploading...";
        
        const formData = new FormData();
        formData.append("imageFile", file, "4.gif"); // 注意：原代码这里写死了 4.gif，你可能需要根据 inputId 动态获取序号
        
        fetch("/upload", { method: "POST", body: formData })
            .then(res => {
                if (res.ok) {
                    alert("Success!");
                    statusDiv.innerText = "Upload complete.";
                    // 【核心修复】：GIF 上传成功后同样刷新存储条
                    loadStorageData(); 
                } else {
                    alert("Failed");
                    statusDiv.innerText = "Upload failed.";
                }
            })
            .catch(() => alert("Error"))
            .finally(() => URL.revokeObjectURL(objUrl));
    };
    img.src = objUrl;
};

window.uploadOTA = function() {
    const fileInput = document.getElementById('otaFile');
    if (!fileInput || !fileInput.files[0]) { alert("Select File"); return; }
    
    const file = fileInput.files[0];
    if (!file.name.startsWith("SDP")) { alert("Please use the correct firmware"); return; }
    
    if(!confirm("Start OTA Update?")) return;
    
    const formData = new FormData();
    formData.append("update", file);
    
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/update_ota");
    xhr.upload.onprogress = (e) => {
        const p = document.getElementById('ota-bar');
        if(p && e.lengthComputable) p.value = (e.loaded / e.total) * 100;
    };
    xhr.onload = () => alert(xhr.status === 200 ? "Update Success! Rebooting..." : "Failed");
    xhr.send(formData);
};

window.openConvertedGIF = () => window.open("https://ezgif.com/", "_blank");

window.uploadGenericFile = function() {
    const fileInput = document.getElementById('genericFile');
    if (!fileInput || !fileInput.files[0]) { alert("Please select a file first."); return; }
    
    const file = fileInput.files[0];
    const btn = document.querySelector("button[onclick='uploadGenericFile()']");
    const originalText = btn.innerText;

    if (!confirm(`Upload "${file.name}" (${formatSize(file.size)}) to root directory?`)) return;

    btn.disabled = true;
    btn.innerText = "Uploading...";

    const formData = new FormData();
    formData.append("file", file, file.name);

    fetch("/upload", { method: "POST", body: formData })
    .then(res => {
        if (res.ok) {
            alert("Upload Success: " + file.name);
            fileInput.value = ''; 
            
            // 【核心修复】：上传成功后，立刻重新拉取最新存储数据并刷新 UI 和列表！
            loadStorageData(); 

            if (['index.html', 'javascript.js', 'style.css'].includes(file.name)) {
                if(confirm("System file updated. Refresh page now?")) location.reload();
            }
        } else { alert("Server Error: " + res.status); }
    })
    .catch(err => {
        console.error(err);
        alert("Upload Failed (Network Error)");
    })
    .finally(() => {
        btn.disabled = false;
        btn.innerText = originalText;
    });
};

document.addEventListener('DOMContentLoaded', function() {
    const btnUpdateCity = document.getElementById('btnUpdateCity');
    if(btnUpdateCity) {
        btnUpdateCity.addEventListener('click', async function() {
            const cityInput = document.getElementById('cityInput');
            const cityName = cityInput ? cityInput.value.trim() : '';
            const keyInput = document.getElementById('openweathermapKey');
            const apiKey = keyInput ? keyInput.value.trim() : '';

            if (!cityName) { alert('Please enter a city name.'); if(cityInput) cityInput.focus(); return; }
            if (!apiKey) { alert('Please enter the OpenWeatherMap API Key first.'); if(keyInput) keyInput.focus(); return; }

            const btn = this;
            const originalText = btn.innerText;
            btn.innerText = 'Validating in OWM...';
            btn.disabled = true;

            try {
                const isNumeric = /^\d+$/.test(cityName);
                const url = isNumeric 
                    ? `https://api.openweathermap.org/data/2.5/weather?id=${cityName}&appid=${apiKey}&units=metric`
                    : `https://api.openweathermap.org/data/2.5/weather?q=${encodeURIComponent(cityName)}&appid=${apiKey}&units=metric`;

                const response = await fetch(url);

                if (response.ok) { 
                    const data = await response.json();
                    alert(`Success! City confirmed: ${data.name}, ${data.sys.country}\n(ID: ${data.id})\nUpdating clock...`);
                    if(typeof sendConfig === "function") sendConfig('city', cityName); 
                } else if (response.status === 404) {
                    alert('City not found on OpenWeatherMap. Please check the name.');
                } else if (response.status === 401) {
                    alert('Error 401: Invalid OpenWeatherMap API Key.');
                } else {
                    alert(`Validation error. Code: ${response.status}`);
                }
            } catch (error) {
                console.error('Network error:', error);
                alert('A network error occurred while connecting to OpenWeatherMap.');
            } finally {
                btn.innerText = originalText;
                btn.disabled = false;
            }
        });
    }

    const btnSaveKey = document.getElementById('btnSaveKey');
    if(btnSaveKey) {
        btnSaveKey.addEventListener('click', async function() {
            const keyInput = document.getElementById('openweathermapKey');
            const apiKey = keyInput ? keyInput.value.trim() : '';

            if (!apiKey) { alert('Please enter an API Key before updating.'); if(keyInput) keyInput.focus(); return; }

            const btn = this;
            const originalText = btn.innerText;
            btn.innerText = 'Validating...'; 
            btn.disabled = true;

            try {
                const testCity = "Shenzhen";
                const url = `https://api.openweathermap.org/data/2.5/weather?q=${testCity}&appid=${apiKey}`;
                const response = await fetch(url);

                if (response.ok) { 
                    alert('Key successfully validated! Saving to device...');
                    if(typeof sendConfig === "function") sendConfig('weatherkey', apiKey); 
                } else if (response.status === 401) {
                    alert('The API Key is invalid or not yet active. Please check and try again.');
                } else {
                    alert(`Unexpected error validating the key. Code: ${response.status}`);
                }
            } catch (error) {
                console.error('Network error:', error);
                alert('A network error occurred.');
            } finally {
                btn.innerText = originalText;
                btn.disabled = false;
            }
        });
    }

    const btnSaveNTP = document.getElementById('btnSaveNTP');
    if(btnSaveNTP) {
        btnSaveNTP.addEventListener('click', async function() {
            const ntpInput = document.getElementById('ntp');
            const ntpValue = ntpInput ? ntpInput.value.trim() : '';

            if (!ntpValue) { alert('Please enter the NTP server address.'); if(ntpInput) ntpInput.focus(); return; }

            const btn = this;
            const originalText = btn.innerText;
            btn.innerText = 'Validating...'; 
            btn.disabled = true;

            try {
                const isIPv4 = /^(?:[0-9]{1,3}\.){3}[0-9]{1,3}$/.test(ntpValue);
                let isValid = false;

                if (isIPv4) {
                    isValid = true;
                } else {
                    const checkDNS = async (url, headers = {}) => {
                        const response = await fetch(url, { headers });
                        if (response.ok) {
                            const data = await response.json();
                            if (data.Status === 0 && data.Answer && data.Answer.length > 0) return true;
                        }
                        throw new Error('Resolution failed'); 
                    };

                    const encodedDomain = encodeURIComponent(ntpValue);
                    const aliDNS = checkDNS(`https://dns.alidns.com/resolve?name=${encodedDomain}`);
                    const tencentDNS = checkDNS(`https://doh.pub/dns-query?name=${encodedDomain}`, { 'Accept': 'application/dns-json' });
                    const googleDNS = checkDNS(`https://dns.google/resolve?name=${encodedDomain}`);

                    try {
                        await Promise.any([aliDNS, tencentDNS, googleDNS]);
                        isValid = true;
                    } catch (aggregateError) { isValid = false; }
                }

                if (isValid) {
                    alert('NTP server successfully validated! Saving...');
                    if(typeof sendConfig === "function") sendConfig('ntp', ntpValue); 
                } else {
                    alert('Invalid NTP server. Resolution failed on all DNS servers.');
                }
            } catch (error) {
                console.error('Critical error:', error);
                if(confirm('Local network error unable to access public DNS. Force save?')) {
                    if(typeof sendConfig === "function") sendConfig('ntp', ntpValue);
                }
            } finally {
                btn.innerText = originalText;
                btn.disabled = false;
            }
        });
    }
});