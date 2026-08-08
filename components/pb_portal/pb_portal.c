// SPDX-License-Identifier: MIT
#include "pb_portal.h"
#include "pb_dns.h"
#include "pb_httpd.h"
#include "dc_wifi.h"
#include "dc_source.h"
#include "dc_moonraker.h"
#include "dc_bambu.h"
#include "pb_ha.h"
#include "db_klipper_mqtt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_portal";
#define NVS_NS "app_nvs"

// STA-mode dashboard: the gzip of components/pb_portal/www/app.html, embedded
// into flash rodata at build time (see CMakeLists.txt target_add_binary_data).
// Served verbatim as a single Content-Encoding: gzip response.
// cppcheck-suppress syntaxError  // GNU asm() label on an extern decl (embedded
// binary symbol) is valid GCC; cppcheck's C parser does not model it.
extern const uint8_t app_html_gz_start[] asm("_binary_app_html_gz_start");
extern const uint8_t app_html_gz_end[]   asm("_binary_app_html_gz_end");
// 32x32 PNG of the DragonBreath dragon mark, embedded from www/favicon.png (see
// CMakeLists.txt). Served at /favicon.ico so the browser's automatic favicon fetch
// gets a real icon instead of the SPA HTML from the "/*" catch-all.
extern const uint8_t favicon_png_start[] asm("_binary_favicon_png_start");
extern const uint8_t favicon_png_end[]   asm("_binary_favicon_png_end");
// Non-empty guard: httpd_resp_send_chunk() with a 0-length string terminates
// the chunked response early, so we must never send an empty chunk.
#define SEND(req, s) do { const char *_s = (s); if (_s && _s[0]) httpd_resp_send_chunk((req), _s, HTTPD_RESP_USE_STRLEN); } while (0)

// Shared client-side auth helpers, injected into every control page's <script>.
// window.DB_TOK holds the CSRF sentinel ("web") ONLY when no control token is
// configured. When a token IS configured the device sets window.DB_NEEDTOK
// instead and never emits the secret — we prompt for it and cache it in
// localStorage, so a configured token is genuine auth rather than a value baked
// into this public page. On a 403 the cached token is dropped and re-prompted.
#define DB_AUTH_JS \
    "function tok(){if(window.DB_TOK)return window.DB_TOK;" \
    "var t=localStorage.getItem('db_tok');" \
    "if(!t){t=prompt('DragonBreath control token')||'';if(t)localStorage.setItem('db_tok',t);}" \
    "return t;}" \
    "function hdr(){return {'X-DragonBreath-Auth':tok()};}" \
    "function post(u){return fetch(u,{method:'POST',headers:hdr()}).then(function(r){" \
    "if(r.status==403){localStorage.removeItem('db_tok');alert('Control token rejected \\u2014 try again.');}return r;});}"

// ---- form parsing (application/x-www-form-urlencoded) ----
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void urldecode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = hexval(p[1]), lo = hexval(p[2]);
            if (hi >= 0 && lo >= 0) { *o++ = (char)((hi << 4) | lo); p += 3; continue; }
        }
        if (*p == '+') { *o++ = ' '; p++; continue; }
        *o++ = *p++;
    }
    *o = '\0';
}
static bool form_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    for (const char *p = body; p && *p; ) {
        const char *amp = strchr(p, '&');
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            urldecode(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (outsz) out[0] = '\0';
    return false;
}

// Escape a string for insertion inside an HTML double-quoted attribute value,
// so a stored mk_host can't break out of value="..." and inject markup/script.
static void html_attr_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        const char *rep;
        switch (*p) {
            case '&':  rep = "&amp;";  break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            default:
                if (o + 1 >= outsz) { out[o] = '\0'; return; }
                out[o++] = *p;
                continue;
        }
        size_t rl = strlen(rep);
        if (o + rl >= outsz) break;
        memcpy(out + o, rep, rl);
        o += rl;
    }
    out[o] = '\0';
}

// ---- static page pieces ----
// Palette + dragon mark matching the STA-mode SPA (app.html): the same light-dark()
// tokens, so /setup, /fw, and the AP captive portal read as the same product in
// BOTH light and dark (following the device theme, or a pinned dashboard choice via
// localStorage db_theme). PAGE_HEAD is the shared head + CSS + <body>; PAGE_HDR is
// the product header (shown on /setup + AP captive, omitted on /fw); WRAP_OPEN opens
// the content column. /fw is header-free; /setup keeps the header.
static const char PAGE_HEAD[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<meta name=color-scheme content='light dark'><meta name=referrer content=no-referrer>"
    "<link rel=icon href=/favicon.ico>"
    "<title>DragonBreath</title><style>"
    // Shares the dashboard's light-dark() token values (app.html) so /setup, /fw
    // and the captive portal match the app in both themes. Keeps the portal's own
    // variable NAMES so the component CSS below is unchanged. Follows the device
    // theme by default; a pinned dashboard choice (localStorage db_theme) is
    // applied via the head script below.
    ":root{color-scheme:light dark;"
    "--text:light-dark(rgb(26 28 31),rgb(255 255 255));"
    "--bg:light-dark(rgb(255 255 255),rgb(24 24 24));"
    "--card:color-mix(in oklab,var(--text) 5%,transparent);"
    "--accent:light-dark(rgb(51 156 255),rgb(131 195 255));"
    "--accent-fg:light-dark(rgb(255 255 255),rgb(13 13 13));"
    "--muted:light-dark(rgb(26 28 31 / 49.4%),rgb(255 255 255 / 49.8%));"
    "--input:light-dark(rgb(26 28 31 / 11.8%),color-mix(in oklab,rgb(0 0 0) 10%,transparent));"
    "--border:light-dark(rgb(26 28 31 / 8%),rgb(255 255 255 / 8.2%));"
    "--bad:light-dark(rgb(226 85 7),rgb(255 133 73))}"
    ":root[data-theme=light]{color-scheme:light}:root[data-theme=dark]{color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--text);"
    "font:14px/1.4 -apple-system,system-ui,'Segoe UI',Roboto,sans-serif}"
    ".hdr{display:flex;align-items:center;justify-content:center;gap:9px;padding:16px;"
    "border-bottom:1px solid var(--border);background:var(--card)}"
    ".hdr svg{width:27px;height:27px;fill:currentColor}"
    ".hdr h1{margin:0;font-size:1.15rem;font-weight:700}"
    ".wrap{max-width:28em;margin:0 auto;padding:16px}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;margin:14px 0}"
    ".card h2{margin:0 0 .3em;font-size:1rem;font-weight:600}"
    "label{display:block;margin:.85em 0 .3em;font-size:.8rem;color:var(--muted)}"
    "input,select{width:100%;padding:11px 13px;font-size:1rem;background:var(--input);color:var(--text);"
    "border:1px solid var(--border);border-radius:8px}"
    "input:focus,select:focus{outline:2px solid var(--accent);outline-offset:1px;border-color:var(--accent)}"
    ".pw{display:flex;gap:8px}.pw input{flex:1}"
    ".pw button{width:50px;flex:none;background:var(--input);border:1px solid var(--border);border-radius:8px;font-size:1.1rem;cursor:pointer;color:var(--text)}"
    "button.go{width:100%;padding:13px;margin-top:8px;border:0;border-radius:9px;background:var(--accent);color:var(--accent-fg);font-size:1rem;font-weight:600;cursor:pointer}"
    "button.sec{width:100%;padding:10px;margin-top:12px;border:1px solid var(--border);border-radius:8px;background:transparent;color:var(--text);cursor:pointer}"
    "button.sec.active{background:var(--accent);border-color:var(--accent);color:var(--accent-fg);font-weight:600}"
    "button:disabled{opacity:.45;cursor:not-allowed}"
    ".srow{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--border)}.srow:last-child{border:0}"
    ".msg{min-height:1.2em;margin-top:.55em;font-size:.78rem;color:var(--muted)}.msg.bad{color:var(--bad)}"
    ".warn{color:var(--bad);font-weight:700}"
    "small{color:var(--muted)}a{color:var(--accent)}h3{margin:.2em 0}"
    "code{font-size:.9em;overflow-wrap:anywhere}</style>"
    // Honor a pinned dashboard theme (same-origin localStorage); 'auto'/unset falls
    // back to the CSS light-dark() tokens. Runs in <head> pre-render to avoid a flash.
    "<script>var _t=localStorage.getItem('db_theme');"
    "if(_t==='light'||_t==='dark')document.documentElement.setAttribute('data-theme',_t);</script>"
    "</head><body>";

// Product header (dragon mark + wordmark). Shown on /setup and the AP captive
// portal; omitted on /fw so the update page is header-free.
static const char PAGE_HDR[] =
    "<div class=hdr>"
    "<svg viewBox='0 0 32 32' aria-hidden=true><path fill-rule=evenodd "
    "d='M2 15 L8 13 L11 11 L13 8 L15 10 L21 3 L18 11 L21 13 L24 15 L28 17 L23 18 L27 22 L20 20 L18 20 L16 19 L3 19 Z "
    "M10 12 a1.4 1.4 0 100 2.8 1.4 1.4 0 000-2.8Z'/></svg>"
    "<h1>DragonBreath</h1></div>";

// Content wrapper, opened by each page after the optional header.
static const char WRAP_OPEN[] = "<div class=wrap>";

// Wi-Fi form card (config page).
static const char CONFIG_WIFI[] =
    "<form id=cfg onsubmit='return save(event)'>"
    "<div class=card><h2>Wi-Fi</h2>"
    "<label>Network</label><select id=ssid name=ssid><option value=''>scanning\xE2\x80\xA6</option></select>"
    "<label>\xE2\x80\xA6 or hidden SSID</label><input name=ssid_manual placeholder='(optional)'>"
    "<label>Password</label><div class=pw><input id=pw type=password name=password autocomplete=off>"
    // Eye toggles password visibility. Strikethrough (via CSS) = hidden; plain = shown.
    // Starts struck since the field starts masked. (No emoji swap — a plain eye only.)
    "<button type=button id=eye onclick='togglePw()' aria-label='show password' style='text-decoration:line-through'>\xF0\x9F\x91\x81</button></div>"
    "<button type=button class=sec onclick='rescan()'>Rescan networks</button></div>";

// Dedicated firmware-update page (GET /fw) — DragonBreath OTA only, its own page so
// it isn't mixed in with Wi-Fi/printer setup.
static const char FW_BODY[] =
    "<div id=upd class=card style='display:none'></div>"
    "<div class=card><h2>Firmware update</h2>"
    "<p style='margin:.2em 0 .7em;font-size:.85rem;color:var(--muted)'>Installs an "
    "<b>DragonBreath</b> firmware update (upload <code>dragonbreath.bin</code>). The "
    "image is verified and the device reboots into it; a bad image rolls back on "
    "the next boot.</p>"
    "<div class=card style='background:var(--input);font-size:.8rem;color:var(--muted)'>"
    "Want to go back to <b>stock Panda</b>? Upload your saved stock backup's <b>app</b> "
    "image here \xE2\x80\x94 this page accepts stock Panda Breath firmware too. Keep that "
    "backup safe (BIQU doesn't publish stock images).</div>"
    "<label>DragonBreath firmware (.bin)</label>"
    "<input type=file id=fw accept='.bin' onchange='fwsel()'>"
    "<button type=button id=fwbtn class=go onclick='doUpdate()' disabled>Upload &amp; flash</button>"
    "<div id=fwmsg style='margin-top:.6em'><small>Turn the heater OFF first "
    "(updates are refused while heating). <b class=warn>Do not power off "
    "during the update.</b></small></div></div>"
    "<p style='text-align:center'><small><a href='/'>\xE2\x86\x90 Back to status</a></small></p>"
    "<div id=ver style='text-align:center;color:var(--muted);font-size:.72rem;margin-top:2px'></div></div>"
    "<script>" DB_AUTH_JS
    "if(window.DB_VER)document.getElementById('ver').textContent='DragonBreath '+window.DB_VER;"
    // Enable the flash button only once a file is chosen.
    "function fwsel(){document.getElementById('fwbtn').disabled=!document.getElementById('fw').files.length;}"
    // Stream the chosen .bin to /update with the auth header; device validates,
    // reports the SHA-256 it computed over the received image, and reboots. A
    // dropped connection on .catch is the expected reboot path.
    // XHR (not fetch) so upload.onprogress can show a live %; the device writes bytes to
    // flash as it receives them, so upload progress tracks flashing. On success we poll
    // the rebooting device and return to the main screen automatically (waitBack).
    "function doUpdate(){var f=document.getElementById('fw').files[0];"
    "var m=document.getElementById('fwmsg');"
    "if(!f){m.innerHTML='<small>Choose a .bin file first.</small>';return;}"
    "document.getElementById('fwbtn').disabled=true;"
    "m.innerHTML='<small>Uploading &amp; flashing\\u2026 0%<br><b class=warn>Do not power off.</b></small>';"
    "var up=false;var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.setRequestHeader('X-DragonBreath-Auth',tok());"
    "x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);"
    "m.innerHTML='<small>Uploading &amp; flashing\\u2026 '+p+'%<br><b class=warn>Do not power off.</b></small>';}};"
    "x.upload.onload=function(){up=true;m.innerHTML='<small>Verifying image\\u2026 <b class=warn>Do not power off.</b></small>';};"
    "x.onload=function(){var j={};try{j=JSON.parse(x.responseText);}catch(e){}"
    "if(j&&j.ok){m.innerHTML='<h3>Flashed \\u2713</h3><small>SHA-256 of uploaded image:<br>"
    "<code>'+(j.sha256||'?')+'</code><br>Rebooting into the new firmware\\u2026 reconnecting.</small>';waitBack();}"
    "else{document.getElementById('fwbtn').disabled=false;"
    "m.innerHTML='<small>Update failed: '+((j&&j.error)||('HTTP '+x.status))+'</small>';}};"
    // A dropped connection AFTER the upload finished is the expected reboot; before it,
    // it's a genuine upload failure.
    "x.onerror=function(){if(up){m.innerHTML='<h3>Flashed \\u2713</h3><small>Connection lost \\u2014 the "
    "device is rebooting into the new firmware\\u2026 reconnecting.</small>';waitBack();}"
    "else{document.getElementById('fwbtn').disabled=false;"
    "m.innerHTML='<small>Update failed: connection lost during upload \\u2014 try again.</small>';}};"
    "x.send(f);}"
    // Poll the rebooting device until it answers on the new firmware, then go to the main
    // screen automatically. ~2 min fallback if we can't confirm (redirect anyway).
    "function waitBack(){var n=0;var iv=setInterval(function(){n++;"
    "fetch('/api/v2/info',{cache:'no-store'}).then(function(r){if(r.ok){clearInterval(iv);location.href='/';}})"
    ".catch(function(){});if(n>60){clearInterval(iv);location.href='/';}},2000);}"
    // Update check (Flow A): only on OFFICIAL builds (clean vX.Y.Z), ask GitHub for
    // the latest release; if newer, show a download link + expected SHA-256. The
    // browser can't read release-asset bytes (no CORS), so the user downloads then
    // flashes via the picker above. Fails silent on dev builds / offline / errors.
    "(function(){var REPO='plastikman/DragonBreath',cur=window.DB_VER||'';"
    "var m=/^v?(\\d+)\\.(\\d+)\\.(\\d+)$/.exec(cur);if(!m)return;"
    "var c=[+m[1],+m[2],+m[3]];"
    "fetch('https://api.github.com/repos/'+REPO+'/releases/latest')"
    ".then(function(r){return r.ok?r.json():null;}).then(function(d){"
    "if(!d||!d.tag_name)return;var l=/^v?(\\d+)\\.(\\d+)\\.(\\d+)/.exec(d.tag_name);if(!l)return;"
    "var n=[+l[1],+l[2],+l[3]];"
    "if(!(n[0]>c[0]||(n[0]==c[0]&&(n[1]>c[1]||(n[1]==c[1]&&n[2]>c[2])))))return;"
    "var a=(d.assets||[]).filter(function(x){return /^dragonbreath-.*\\.bin$/.test(x.name)&&x.name.indexOf('factory')<0;})[0];"
    "var sha=a&&a.digest?a.digest.replace('sha256:',''):'';"
    "var e=document.getElementById('upd');"
    "e.innerHTML='<b>\\uD83D\\uDC09 DragonBreath '+d.tag_name+' available</b> (you\\u2019re on '+cur+'). '"
    "+(a?'<a href='+a.browser_download_url+' target=_blank rel=noopener>Download '+a.name+'</a> \\u00b7 ':'')"
    "+'<a href='+d.html_url+' target=_blank rel=noopener>release notes</a>'"
    "+(sha?'<br><small>Expected SHA-256: <code>'+sha+'</code> \\u2014 verify your download, then flash it above.</small>'"
    ":'<br><small>Download it, then flash it above.</small>');"
    "e.style.display='block';}).catch(function(){});})();"
    "</script></body></html>";

static const char PAGE_TAIL[] =
    "<button type=submit class=go>Save &amp; Connect</button></form>"
    "<div id=msg style='text-align:center'><small>The device reboots and joins your network after saving.</small></div>"
    "<p style='text-align:center'><small><a href='/fw'>Firmware update</a></small></p></div>"
    "<script>" DB_AUTH_JS
    // Submit via fetch so we can attach the X-DragonBreath-Auth header (a plain form
    // POST can't). Required in STA /setup (the /save handler gates on it there);
    // harmless in AP mode. The device reboots on save, so a dropped connection on
    // the .then/.catch is the expected success path.
    "function save(e){e.preventDefault();"
    "var b=new URLSearchParams(new FormData(document.getElementById('cfg'))).toString();"
    "var done=function(){document.getElementById('msg').innerHTML="
    "'<h3>Saved \\u2713</h3><small>Rebooting and joining your Wi-Fi\\u2026 this page will disconnect.</small>';};"
    "var h=hdr();h['Content-Type']='application/x-www-form-urlencoded';"
    "fetch('/save',{method:'POST',headers:h,body:b}).then(done).catch(done);"
    "return false;}"
    // Unbind: clears the current source's config + drops to None, then reboots.
    "function unbind(){if(!confirm('Unbind the current control source? The heater will have no external controller until you select a new one.'))return;"
    "var d=function(){document.getElementById('msg').innerHTML='<h3>Unbound \\u2713</h3><small>Rebooting\\u2026 this page will disconnect.</small>';};"
    "post('/unbind').then(d).catch(d);}"
    // Toggle visibility; strikethrough the eye when masked (no monkey emoji).
    "function togglePw(){var p=document.getElementById('pw'),e=document.getElementById('eye');"
    "p.type=p.type==='password'?'text':'password';"
    "e.style.textDecoration=(p.type==='password')?'line-through':'none';}"
    // Network list. The first option is empty/selected by default: in STA it means
    // \"keep current Wi-Fi\" so saving without touching it does NOT rewrite creds
    // (window.DB_KEEPWIFI); in AP provisioning it's a \"choose a network\" prompt.
    // This prevents a config-only save (e.g. switching control source) from
    // silently overwriting Wi-Fi with a blank password.
    "function fill(l){var s=document.getElementById('ssid'),c=s.value;s.innerHTML='';"
    "var o0=document.createElement('option');o0.value='';"
    "o0.textContent=window.DB_KEEPWIFI?'\xE2\x80\x94 Keep current Wi-Fi \xE2\x80\x94':(l.length?'Select a network\xE2\x80\xA6':'(none found \xE2\x80\x94 tap Rescan)');"
    "s.appendChild(o0);"
    "l.forEach(function(n){var o=document.createElement('option');o.textContent=n;o.value=n;s.appendChild(o);});"
    "s.value=c||'';}"
    "function load(){fetch('/scan.json').then(function(r){return r.json();}).then(fill).catch(function(){});}"
    "function rescan(){fetch('/rescan',{method:'POST'}).then(function(){setTimeout(load,1800);});}"
    "load();setInterval(load,4000);"
    "</script></body></html>";

// Inject the client-side auth bootstrap. If a control token is configured we emit
// only a NEEDTOK flag (never the secret) so the page prompts for it; otherwise we
// emit the "web" CSRF sentinel. Paired with DB_AUTH_JS's tok()/hdr().
static void send_auth_inject(httpd_req_t *req)
{
    char tok[65];
    pb_httpd_ctl_token(tok, sizeof tok);   // 65-byte buffer: never false-negative
    SEND(req, tok[0]
        ? "<script>window.DB_NEEDTOK=1;</script>"
        : "<script>window.DB_TOK=\"web\";</script>");
}

// Inject the firmware version (git tag for releases, short hash for PR/local) as
// window.DB_VER so pages can show it. From the ESP app descriptor (see CMakeLists).
static void send_version_inject(httpd_req_t *req)
{
    char b[128];
    snprintf(b, sizeof b, "<script>window.DB_VER=\"%s\";</script>",
             esp_app_get_description()->version);
    SEND(req, b);
}

// ---- handlers ----
// Live dashboard SPA (root in STA mode). The embedded gzip of www/app.html is
// sent verbatim as a single Content-Encoding: gzip response — the whole shell +
// Dashboard, self-contained (inline icons/CSS/JS, no external requests). It binds
// itself to the v2 API at runtime (GET /api/v2/state + /api/v2/events); nothing is
// templated server-side.
static esp_err_t app_page(httpd_req_t *req)
{
    // start/end are the linker-provided bounds of ONE embedded blob
    // (target_add_binary_data), so end-start is its length — not UB.
    // cppcheck-suppress comparePointers
    const size_t len = (size_t)(app_html_gz_end - app_html_gz_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)app_html_gz_start, len);
}

// Dedicated firmware-update page (GET /fw).
static esp_err_t fw_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND(req, PAGE_HEAD);
    SEND(req, WRAP_OPEN);     // /fw is header-free (no PAGE_HDR)
    send_auth_inject(req);
    send_version_inject(req);
    SEND(req, FW_BODY);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Diagnostics page (GET /diag): the tools/diag.py logger in the browser. Pure
// client-side over the existing read-only SSE stream (/api/v2/events) + /api/v2/info
// — no new device state, no persistence. Shows chamber/element temps, SSR output,
// mode, fault, a running element-temp peak, a live trend, and a client-side CSV
// download of everything sampled since the page opened.
static const char DIAG_BODY[] =
    "<style>"
    ".dgrid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:.6em 0}"
    ".dgrid>div{background:var(--input);border-radius:8px;padding:10px}"
    ".dlab{font-size:.72rem;color:var(--muted)}"
    ".dval{font-size:1.2rem;font-weight:700;margin-top:2px}"
    ".dval.warn{color:var(--bad)}"
    "#d-chart{display:block;width:100%;height:120px;background:var(--input);border-radius:8px;margin:.5em 0}"
    ".drow{display:flex;gap:8px}.drow button{flex:1;margin-top:0}"
    "</style>"
    "<div class=card><h2>Diagnostics</h2>"
    "<div id=d-meta><small>connecting\xE2\x80\xA6</small></div>"
    "<div class=dgrid>"
    "<div><div class=dlab>Chamber</div><div class=dval id=d-ch>--</div></div>"
    "<div><div class=dlab>Element (PTC)</div><div class=dval id=d-ptc>--</div></div>"
    "<div><div class=dlab>Peak element</div><div class=dval id=d-peak>--</div></div>"
    "<div><div class=dlab>SSR output</div><div class=dval id=d-ssr>--</div></div>"
    "<div><div class=dlab>Mode</div><div class=dval id=d-mode>--</div></div>"
    "<div><div class=dlab>Fault</div><div class=dval id=d-fault>--</div></div>"
    "</div>"
    "<canvas id=d-chart></canvas>"
    "<div style='display:flex;justify-content:space-between;align-items:center'>"
    "<small><span style='color:#3399ff'>\xE2\x97\x8F</span> chamber "
    "<span style='color:#ff8a49'>\xE2\x97\x8F</span> element</small>"
    "<small id=d-stat>0 samples</small></div>"
    "<div class=drow style='margin-top:10px'>"
    "<button type=button class=go id=d-dl>Download CSV</button>"
    "<button type=button class=sec id=d-clear>Clear</button>"
    "</div></div>"
    "<p style='text-align:center'><small><a href='/'>\xE2\x86\x90 Back to status</a></small></p>"
    "<div id=ver style='text-align:center;color:var(--muted);font-size:.72rem;margin-top:2px'></div>"
    "<script>(function(){"
    "var peak=0,samples=[],t0=null,MAX=900;"      // cap ~30 min at 2 s to bound the tab's memory
    "function $(i){return document.getElementById(i);}"
    "if(window.DB_VER)$('ver').textContent='DragonBreath '+window.DB_VER;"
    "fetch('/api/v2/info',{cache:'no-store'}).then(function(r){return r.json();}).then(function(i){"
    "$('d-meta').innerHTML='<small>device '+i.device_id+' \\u00b7 fw '+i.firmware+' \\u00b7 Rref '+i.rref_kohm+'k</small>';"
    "}).catch(function(){});"
    "function f1(x){return (x==null)?'--':Number(x).toFixed(1);}"
    "function apply(s){"
    "if(!s||s.api_version!==2)return;"
    "var ch=s.sensors.chamber,pt=s.sensors.ptc,saf=s.safety||{};"
    "var chv=ch.temperature_c,ptv=pt.temperature_c,out=!!s.heater.output,fl=!!saf.fault_latched;"
    "if(ptv!=null&&ptv>peak)peak=ptv;"
    "$('d-ch').textContent=f1(chv)+' \\u00b0C';"
    "$('d-ptc').textContent=f1(ptv)+' \\u00b0C'+(pt.status!=='ok'?' ('+pt.status+')':'');"
    "$('d-peak').textContent=f1(peak)+' \\u00b0C';"
    "$('d-ssr').textContent=out?'ON':'off';"
    "$('d-mode').textContent=s.mode;"
    "var fe=$('d-fault');fe.textContent=fl?('FAULT: '+(saf.reason||'?')):'none';fe.className='dval'+(fl?' warn':'');"
    "var now=Date.now()/1000;if(t0==null)t0=now;"
    "samples.push({t:now-t0,ch:chv,pt:ptv,ps:pt.status,o:out?1:0,m:s.mode,fl:fl?1:0,rs:saf.reason||'',pk:peak});"
    "if(samples.length>MAX)samples.shift();"
    "$('d-stat').textContent=samples.length+' samples';draw();"
    "}"
    "function draw(){"
    "var c=$('d-chart');var w=c.clientWidth||300,h=120;c.width=w;c.height=h;"
    "var x=c.getContext('2d');x.clearRect(0,0,w,h);if(samples.length<2)return;"
    "var mx=1;samples.forEach(function(s){if(s.ch!=null&&s.ch>mx)mx=s.ch;if(s.pt!=null&&s.pt>mx)mx=s.pt;});"
    "mx=Math.ceil(mx/20)*20;"
    "function px(i){return i*(w-4)/(samples.length-1)+2;}"
    "function py(v){return h-4-(v/mx)*(h-8);}"
    "function ln(k,col){x.beginPath();x.strokeStyle=col;x.lineWidth=1.5;var st=false;"
    "for(var i=0;i<samples.length;i++){var v=samples[i][k];if(v==null){st=false;continue;}"
    "if(!st){x.moveTo(px(i),py(v));st=true;}else x.lineTo(px(i),py(v));}x.stroke();}"
    "ln('pt','#ff8a49');ln('ch','#3399ff');"
    "}"
    "$('d-dl').addEventListener('click',function(){"
    "var r=['t_s,chamber_c,ptc_c,ptc_status,out,mode,fault,reason,peak_c'];"
    "samples.forEach(function(s){r.push([s.t.toFixed(1),(s.ch==null?'':s.ch.toFixed(1)),(s.pt==null?'':s.pt.toFixed(1)),s.ps,s.o,s.m,s.fl,'\"'+String(s.rs).replace(/\"/g,'\"\"')+'\"',s.pk.toFixed(1)].join(','));});"
    "var b=new Blob([r.join('\\n')+'\\n'],{type:'text/csv'});"
    "var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='dragonbreath_diag.csv';a.click();"
    "setTimeout(function(){URL.revokeObjectURL(a.href);},1000);"
    "});"
    "$('d-clear').addEventListener('click',function(){samples=[];peak=0;t0=null;$('d-stat').textContent='0 samples';draw();});"
    "window.addEventListener('resize',draw);"
    "var es=new EventSource('/api/v2/events');"
    "function ev(e){try{apply(JSON.parse(e.data));}catch(_){}}"
    "es.addEventListener('state',ev);es.addEventListener('telemetry',ev);"
    "})();</script></body></html>";

// Live diagnostics page (GET /diag). Read-only; reuses the SSE stream.
static esp_err_t diag_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND(req, PAGE_HEAD);           // header-free (like /fw) — no PAGE_HDR
    SEND(req, WRAP_OPEN);
    send_version_inject(req);       // window.DB_VER for the footer
    SEND(req, DIAG_BODY);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Firmware console page (GET /console): the raw ESP_LOGx stream captured into the
// dc_evlog byte ring, fetched from the auth-gated GET /api/v2/console and shown in a
// scrolling monospace view with auto-refresh + Download. Read-only / non-interactive.
// The device page itself is open (a GET can't carry the auth header); the DATA fetch
// is gated (JS sends X-DragonBreath-Auth), so the chatty log isn't world-readable.
static const char CONSOLE_BODY[] =
    "<style>"
    // Widen the console page well past the default 28em so ~80-column log lines fit
    // as real lines instead of wrapping mid-content.
    ".wrap{max-width:min(96vw,900px)}"
    // Terminal-style: monospace, NO wrap, horizontal scroll for long lines.
    "#c-log{font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;white-space:pre;tab-size:4;"
    "background:var(--input);border-radius:8px;padding:10px 12px;"
    "max-height:70vh;overflow:auto;margin:.4em 0}"
    ".drow{display:flex;gap:8px}.drow button{flex:1;margin-top:0}"
    "</style>"
    "<div class=card><h2>Console</h2>"
    "<div id=c-meta><small>firmware log\xE2\x80\xA6</small></div>"
    "<pre id=c-log>loading\xE2\x80\xA6</pre>"
    "<div class=drow>"
    "<button type=button class=go id=c-dl>Download</button>"
    "<button type=button class=sec id=c-pause>Pause</button>"
    "</div></div>"
    "<p style='text-align:center'><small><a href='/'>\xE2\x86\x90 Back to status</a></small></p>"
    "<div id=ver style='text-align:center;color:var(--muted);font-size:.72rem;margin-top:2px'></div>"
    "<script>" DB_AUTH_JS
    "(function(){"
    "var paused=false,last='';"
    "function $(i){return document.getElementById(i);}"
    "if(window.DB_VER)$('ver').textContent='DragonBreath '+window.DB_VER;"
    "function load(){"
    "fetch('/api/v2/console',{cache:'no-store',headers:hdr()}).then(function(r){"
    "if(r.status==403){localStorage.removeItem('db_tok');"
    "$('c-meta').innerHTML='<small class=warn>Auth rejected \\u2014 reload and re-enter the control token.</small>';return null;}"
    "return r.text();"
    "}).then(function(t){"
    "if(t==null)return;t=t.replace(/\\x1b\\[[0-9;]*m/g,'');last=t;var pre=$('c-log');"
    "var atEnd=pre.scrollTop+pre.clientHeight>=pre.scrollHeight-6;"
    "pre.textContent=t||'(no log captured yet)';"
    "if(atEnd)pre.scrollTop=pre.scrollHeight;"
    "$('c-meta').innerHTML='<small>'+t.length+' bytes \\u00b7 '+(paused?'paused':'auto-refresh 2s')+'</small>';"
    "}).catch(function(){});"
    "}"
    "$('c-dl').addEventListener('click',function(){"
    "var b=new Blob([last||''],{type:'text/plain'});"
    "var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='dragonbreath-console.txt';a.click();"
    "setTimeout(function(){URL.revokeObjectURL(a.href);},1000);"
    "});"
    "$('c-pause').addEventListener('click',function(){paused=!paused;this.textContent=paused?'Resume':'Pause';if(!paused)load();});"
    "load();setInterval(function(){if(!paused)load();},2000);"
    "})();</script></body></html>";

// Firmware console page (GET /console). Header-free (like /fw); auth bootstrap so
// the JS can fetch the gated /api/v2/console.
static esp_err_t console_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND(req, PAGE_HEAD);
    SEND(req, WRAP_OPEN);
    send_auth_inject(req);          // DB_TOK / DB_NEEDTOK for the gated fetch
    send_version_inject(req);
    SEND(req, CONSOLE_BODY);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Wi-Fi + control-source config page (AP captive root, and /setup in STA mode).
// The device binds to exactly ONE control source; the card carries all three
// field groups and reveals only the selected one (JS). Non-secret values are
// pre-filled (escaped); secrets (Bambu access code, HA password) are never echoed
// — an empty field means "leave unchanged" (see save_post).
static esp_err_t config_page(httpd_req_t *req)
{
    char mk_host[64] = {0};   uint16_t mk_port = 7125;
    char bb_host[64] = {0},   bb_serial[32] = {0};
    char ha_host[64] = {0};   uint16_t ha_port = 1883;
    char ha_user[32] = {0},   ha_topic[48] = {0};
    char km_host[64] = {0};   uint16_t km_port = 1883;
    char km_user[32] = {0},   km_inst[48] = {0},  km_topic[48] = {0};
    uint8_t km_tls = 0,       km_wb = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz;
        sz = sizeof mk_host;   nvs_get_str(h, "mk_host", mk_host, &sz);
        nvs_get_u16(h, "mk_port", &mk_port);
        sz = sizeof bb_host;   nvs_get_str(h, "bb_host", bb_host, &sz);
        sz = sizeof bb_serial; nvs_get_str(h, "bb_serial", bb_serial, &sz);
        sz = sizeof ha_host;   nvs_get_str(h, "ha_host", ha_host, &sz);
        nvs_get_u16(h, "ha_port", &ha_port);
        sz = sizeof ha_user;   nvs_get_str(h, "ha_user", ha_user, &sz);
        sz = sizeof ha_topic;  nvs_get_str(h, "ha_topic", ha_topic, &sz);
        sz = sizeof km_host;   nvs_get_str(h, "km_host", km_host, &sz);
        nvs_get_u16(h, "km_port", &km_port);
        sz = sizeof km_user;   nvs_get_str(h, "km_user", km_user, &sz);
        sz = sizeof km_inst;   nvs_get_str(h, "km_inst", km_inst, &sz);
        sz = sizeof km_topic;  nvs_get_str(h, "km_topic", km_topic, &sz);
        nvs_get_u8(h, "km_tls", &km_tls);
        nvs_get_u8(h, "km_wb", &km_wb);
        nvs_close(h);
    }
    dc_ctl_source_t src = dc_source_get();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND(req, PAGE_HEAD);
    SEND(req, PAGE_HDR);     // product header kept on /setup + AP captive portal
    SEND(req, WRAP_OPEN);
    send_auth_inject(req);
    // If a prior join attempt failed and dropped us back to the portal, say why —
    // server-side so it renders even in a phone's captive-portal mini-browser.
    {
        char fssid[33], freason[160];
        if (dc_wifi_last_sta_fail(fssid, sizeof fssid, freason, sizeof freason)) {
            char eb[200], banner[512];
            html_attr_escape(fssid, eb, sizeof eb);
            snprintf(banner, sizeof banner,
                "<div class=card><b class=warn>Couldn't join \xE2\x80\x9C%s\xE2\x80\x9D</b>"
                "<br><small>%s. Check the details below and try again.</small></div>",
                eb, freason);
            SEND(req, banner);
        }
    }
    // In STA mode Wi-Fi is already provisioned, so the network dropdown defaults to
    // "keep current Wi-Fi" (blank) — a config-only save won't rewrite creds. In AP
    // provisioning there are no creds yet, so a network must be chosen.
    if (dc_wifi_state() != DC_WIFI_STATE_AP_PORTAL)
        SEND(req, "<script>window.DB_KEEPWIFI=1;</script>");
    SEND(req, CONFIG_WIFI);

    // Reused buffers: emit each piece before reusing, to keep httpd-task stack
    // small. esc holds a single HTML-attribute-escaped user value at a time; buf
    // is sized for the largest piece (the HA group + the reveal <script>).
    char buf[640], esc[256];

    // Source selector. One controller at a time (see docs/control-source.md).
    SEND(req,
        "<div class=card><h2>Control source</h2>"
        "<small style='color:var(--muted);display:block;margin-bottom:10px'>"
        "DragonBreath follows <b>one</b> controller at a time \xE2\x80\x94 the others stay disconnected. "
        "Home Assistant has full control only while it is the selected source; when a printer is bound, HA is not connected. "
        "To hand control to a different source, <b>Unbind</b> the current one first. "
        "<a href='https://github.com/plastikman/DragonBreath/blob/main/docs/control-source.md' target=_blank rel=noopener>How control works &rarr;</a></small>"
        "<label>Bind this heater to</label>");
    snprintf(buf, sizeof buf,
        "<select id=ctlsrc name=ctl_src onchange='srcshow()'>"
        "<option value=0%s>Klipper (Moonraker)</option>"
        "<option value=4%s>Klipper (MQTT)</option>"
        "<option value=1%s>Bambu (LAN)</option>"
        "<option value=2%s>Home Assistant</option>"
        "<option value=3%s>None (unbound)</option></select>"
        "<button type=button class=sec onclick='unbind()' style='margin-top:8px'>Unbind current source</button>",
        src == DC_SRC_KLIPPER      ? " selected" : "",
        src == DC_SRC_KLIPPER_MQTT ? " selected" : "",
        src == DC_SRC_BAMBU        ? " selected" : "",
        src == DC_SRC_HA           ? " selected" : "",
        src == DC_SRC_NONE         ? " selected" : "");
    SEND(req, buf);

    // Klipper group.
    html_attr_escape(mk_host, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<div class=grp data-src=0 style='display:%s'>"
        "<label>Host / IP</label><input name=mk_host value=\"%s\" placeholder='e.g. 10.168.2.34'>"
        "<label>Port</label><input name=mk_port value=\"%u\"></div>",
        src == DC_SRC_KLIPPER ? "block" : "none", esc, (unsigned)mk_port);
    SEND(req, buf);

    // Bambu group.
    html_attr_escape(bb_host, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<div class=grp data-src=1 style='display:%s'>"
        "<label>Printer IP</label><input name=bb_host value=\"%s\" placeholder='e.g. 10.168.2.50'>",
        src == DC_SRC_BAMBU ? "block" : "none", esc);
    SEND(req, buf);
    html_attr_escape(bb_serial, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<label>Serial</label><input name=bb_serial value=\"%s\" placeholder='e.g. 01P00A000000000'>"
        "<label>LAN access code</label><input name=bb_code type=password placeholder='(unchanged)' autocomplete=off>"
        "<small style='color:var(--muted)'>Enable <b>LAN Only Mode</b> on the printer; use its Access Code.</small>",
        esc);
    SEND(req, buf);
    // Filament chamber zones (Bambu only) — lives inside the Bambu group so it's
    // shown only when Bambu is the control source (Klipper uses M141/M191 instead).
    SEND(req,
        "<label style='margin-top:10px'>Chamber zones \xC2\xB7 \xC2\xB0""C (0 = off)</label>"
        "<small style='color:var(--muted);display:block;margin:-4px 0 6px'>"
        "When a Bambu print runs, the chamber follows the active filament's target "
        "here (overrides bed-follow).</small>");
    {
        dc_bambu_zone_t zones[DC_BAMBU_ZONE_COUNT];
        int nz = dc_bambu_zone_get_all(zones, DC_BAMBU_ZONE_COUNT);
        for (int i = 0; i < nz; i++) {
            snprintf(buf, sizeof buf,
                "<div style='display:flex;align-items:center;gap:8px;margin-bottom:4px'>"
                "<label style='flex:1;margin:0'>%s</label>"
                "<input name=z_%s type=number min=0 max=70 value=%u style='width:5.5em'></div>",
                zones[i].name, zones[i].name, (unsigned)zones[i].target_c);
            SEND(req, buf);
        }
    }
    SEND(req, "</div>");   // close the Bambu group

    // Home Assistant group.
    html_attr_escape(ha_host, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<div class=grp data-src=2 style='display:%s'>"
        "<label>MQTT broker</label><input name=ha_host value=\"%s\" placeholder='e.g. 10.168.2.10'>"
        "<label>Port</label><input name=ha_port value=\"%u\">",
        src == DC_SRC_HA ? "block" : "none", esc, (unsigned)ha_port);
    SEND(req, buf);
    html_attr_escape(ha_user, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<label>Username</label><input name=ha_user value=\"%s\" autocomplete=off>"
        "<label>Password</label><input name=ha_pass type=password placeholder='(unchanged)' autocomplete=off>",
        esc);
    SEND(req, buf);
    html_attr_escape(ha_topic, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<label>Topic prefix</label><input name=ha_topic value=\"%s\" placeholder='dragonbreath'>",
        esc);
    SEND(req, buf);
    // HA control-mechanism note (static, so it isn't subject to format-truncation).
    SEND(req,
        "<small style='color:var(--muted);display:block;margin-top:8px'>"
        "Home Assistant controls the heater only while it is the selected control source above. When "
        "Klipper or Bambu is bound, HA is not connected \xE2\x80\x94 Unbind that source first to hand control to HA. "
        "<a href='https://github.com/plastikman/DragonBreath/blob/main/docs/control-source.md' target=_blank rel=noopener>Details &rarr;</a>"
        "</small></div>");   // close HA group

    // Klipper (MQTT) group — for locked/managed Klipper installs (no klippy extra).
    // Controller over the printer's own Moonraker broker; see the generated config.
    html_attr_escape(km_host, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<div class=grp data-src=4 style='display:%s'>"
        "<label>MQTT broker (Moonraker's)</label><input name=km_host value=\"%s\" placeholder='e.g. mosquitto.lan'>"
        "<label>Port</label><input name=km_port value=\"%u\">",
        src == DC_SRC_KLIPPER_MQTT ? "block" : "none", esc, (unsigned)km_port);
    SEND(req, buf);
    html_attr_escape(km_user, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<label>Username</label><input name=km_user value=\"%s\" autocomplete=off>"
        "<label>Password</label><input name=km_pass type=password placeholder='(unchanged)' autocomplete=off>",
        esc);
    SEND(req, buf);
    html_attr_escape(km_inst, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<label>Moonraker instance_name</label>"
        "<input name=km_inst value=\"%s\" placeholder='e.g. myprinter'>"
        "<small style='color:var(--muted);display:block;margin:-4px 0 6px'>"
        "Must match <code>instance_name</code> in <code>[mqtt]</code> \xE2\x80\x94 it derives every topic.</small>",
        esc);
    SEND(req, buf);
    snprintf(buf, sizeof buf,
        "<label><input type=checkbox name=km_tls value=1 %s> Use TLS (mqtts)</label>"
        "<label><input type=checkbox name=km_wb value=1 %s> Write live temp back to macros (advanced)</label>",
        km_tls ? "checked" : "", km_wb ? "checked" : "");
    SEND(req, buf);
    html_attr_escape(km_topic, esc, sizeof esc);
    snprintf(buf, sizeof buf,
        "<details style='margin-top:6px'><summary style='cursor:pointer;color:var(--muted)'>Advanced</summary>"
        "<label>Device topic base</label><input name=km_topic value=\"%s\" placeholder='dragonbreath'>"
        "</details>", esc);
    SEND(req, buf);
    // Generated-config link + the mutual-exclusion note (static).
    SEND(req,
        "<small style='color:var(--muted);display:block;margin-top:8px'>"
        "Save first, then <a href='/km-config' target=_blank rel=noopener>generate your Klipper config</a> "
        "(moonraker.conf + printer.cfg + Mosquitto ACL) filled in from these settings. "
        "<b>Do not also run the native <code>dragonbreath-klipper</code> extra</b> \xE2\x80\x94 pick one Klipper integration."
        "</small></div>");   // close MQTT group

    SEND(req, "</div>");   // close the control-source card
    // Reveal-only-the-selected-group script (static — kept out of the snprintf
    // above so it isn't subject to format-truncation on a long topic value).
    SEND(req,
        "<script>function srcshow(){var v=document.getElementById('ctlsrc').value,"
        "g=document.querySelectorAll('.grp');"
        "for(var i=0;i<g.length;i++)g[i].style.display=(g[i].getAttribute('data-src')==v)?'block':'none';}"
        "srcshow();</script>");

    SEND(req, PAGE_TAIL);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Browser-tab favicon (GET /favicon.ico). Serves the embedded 32x32 PNG of the
// dragon mark as image/png. Registered before the "/*" catch-all so the browser's
// automatic /favicon.ico request never falls through to the SPA HTML. The inline
// SVG <link> in app.html's <head> provides the crisp, theme-adaptive icon for
// browsers that honor it; this PNG is the universal fallback.
static esp_err_t favicon_ico(httpd_req_t *req)
{
    // start/end are the linker-provided bounds of ONE embedded blob
    // (target_add_binary_data), so end-start is its length — not UB.
    // cppcheck-suppress comparePointers
    const size_t len = (size_t)(favicon_png_end - favicon_png_start);
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)favicon_png_start, len);
}

// Catch-all root: in AP mode serve the config page so captive-portal probes land
// on setup; in STA mode serve the live dashboard SPA.
static esp_err_t root_page(httpd_req_t *req)
{
    if (dc_wifi_state() == DC_WIFI_STATE_AP_PORTAL) return config_page(req);
    return app_page(req);
}

static esp_err_t scan_json(httpd_req_t *req)
{
    wifi_ap_record_t recs[DC_WIFI_SCAN_MAX];
    int n = dc_wifi_get_scan_results(recs, DC_WIFI_SCAN_MAX);
    if (n == 0 && !dc_wifi_is_scanning()) dc_wifi_scan_start();

    // cJSON handles comma placement and escaping (quotes/backslashes/control chars),
    // so a skipped/odd SSID can't produce invalid JSON.
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n && arr; i++) {
        recs[i].ssid[sizeof recs[i].ssid - 1] = '\0';   // guarantee NUL-terminated
        if (recs[i].ssid[0] == '\0') continue;
        cJSON *s = cJSON_CreateString((const char *)recs[i].ssid);
        if (s) cJSON_AddItemToArray(arr, s);
    }
    char *out = arr ? cJSON_PrintUnformatted(arr) : NULL;
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out ? out : "[]");
    if (out) cJSON_free(out);
    cJSON_Delete(arr);
    return r;
}

static esp_err_t rescan_post(httpd_req_t *req)
{
    dc_wifi_scan_start();
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_post(httpd_req_t *req)
{
    // Provisioning is open only in AP/setup mode (no credentials yet to send a
    // header from). Once joined to a network (STA), rewriting Wi-Fi config is a
    // mutating control action, so require the CSRF header like the other POSTs.
    if (dc_wifi_state() != DC_WIFI_STATE_AP_PORTAL && !pb_httpd_auth_ok(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"missing/invalid X-DragonBreath-Auth header\"}");
    }

    // Larger than the Wi-Fi-only form: now also carries the control-source
    // selector + Klipper/Bambu/HA field groups (all groups submit, even hidden).
    char body[2048];   // fits all field groups (Bambu zones + the Klipper-MQTT group)
    int total = 0, r;
    while ((r = httpd_req_recv(req, body + total, sizeof body - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof body - 1) break;
    }
    body[total > 0 ? total : 0] = '\0';

    char ssid[33] = {0}, ssid_manual[33] = {0}, pass[65] = {0};
    char mk_host[64] = {0}, mk_port_s[8] = {0};
    char src_s[4] = {0};
    char bb_host[64] = {0}, bb_serial[32] = {0}, bb_code[32] = {0};
    char ha_host[64] = {0}, ha_port_s[8] = {0}, ha_user[32] = {0}, ha_pass[64] = {0}, ha_topic[48] = {0};
    form_get(body, "ssid", ssid, sizeof ssid);
    form_get(body, "ssid_manual", ssid_manual, sizeof ssid_manual);
    form_get(body, "password", pass, sizeof pass);
    form_get(body, "ctl_src", src_s, sizeof src_s);
    form_get(body, "mk_host", mk_host, sizeof mk_host);
    form_get(body, "mk_port", mk_port_s, sizeof mk_port_s);
    form_get(body, "bb_host", bb_host, sizeof bb_host);
    form_get(body, "bb_serial", bb_serial, sizeof bb_serial);
    form_get(body, "bb_code", bb_code, sizeof bb_code);
    form_get(body, "ha_host", ha_host, sizeof ha_host);
    form_get(body, "ha_port", ha_port_s, sizeof ha_port_s);
    form_get(body, "ha_user", ha_user, sizeof ha_user);
    form_get(body, "ha_pass", ha_pass, sizeof ha_pass);
    form_get(body, "ha_topic", ha_topic, sizeof ha_topic);
    char km_host[64] = {0}, km_port_s[8] = {0}, km_user[32] = {0}, km_pass[64] = {0};
    char km_inst[48] = {0}, km_topic[48] = {0}, km_tls_s[4] = {0}, km_wb_s[4] = {0};
    form_get(body, "km_host", km_host, sizeof km_host);
    form_get(body, "km_port", km_port_s, sizeof km_port_s);
    form_get(body, "km_user", km_user, sizeof km_user);
    form_get(body, "km_pass", km_pass, sizeof km_pass);
    form_get(body, "km_inst", km_inst, sizeof km_inst);
    form_get(body, "km_topic", km_topic, sizeof km_topic);
    form_get(body, "km_tls", km_tls_s, sizeof km_tls_s);   // checkbox: present iff checked
    form_get(body, "km_wb", km_wb_s, sizeof km_wb_s);

    // Validator: the Klipper-MQTT mode must never be enabled without a broker AND
    // credentials — an open/anonymous broker exposing printer.gcode.script is unsafe
    // (design §8). A password already saved (secret left blank) counts.
    if (src_s[0] && atoi(src_s) == DC_SRC_KLIPPER_MQTT) {
        bool pass_ok = km_pass[0] != '\0';
        if (!pass_ok) {
            nvs_handle_t hv; char saved[8]; size_t ss = sizeof saved;
            if (nvs_open(NVS_NS, NVS_READONLY, &hv) == ESP_OK) {
                pass_ok = (nvs_get_str(hv, "km_pass", saved, &ss) == ESP_OK && saved[0]);
                nvs_close(hv);
            }
        }
        if (!km_host[0] || !km_user[0] || !km_inst[0] || !pass_ok) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "Klipper (MQTT) needs a broker host, username, password, and instance_name");
            return ESP_FAIL;
        }
    }

    const char *chosen = ssid_manual[0] ? ssid_manual : ssid;
    // In AP/provisioning mode there are no saved creds yet, so a Wi-Fi network is
    // mandatory. Once joined (STA), Wi-Fi is OPTIONAL: leaving it blank keeps the
    // existing creds and just applies the control-source/config change — so
    // switching source (or editing Bambu/HA fields) never forces a Wi-Fi re-entry.
    bool have_wifi = chosen[0] != '\0';
    if (!have_wifi && dc_wifi_state() == DC_WIFI_STATE_AP_PORTAL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no Wi-Fi network chosen");
        return ESP_FAIL;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        // Control source (0=klipper/1=bambu/2=ha/3=none/4=klipper-mqtt); ignore
        // out-of-range. DC_SRC_MAX is an exclusive sentinel, so the range is
        // [DC_SRC_KLIPPER, DC_SRC_MAX).
        if (src_s[0]) {
            int s = atoi(src_s);
            if (s >= DC_SRC_KLIPPER && s < DC_SRC_MAX) nvs_set_u8(h, "ctl_src", (uint8_t)s);
        }
        // Klipper.
        if (mk_host[0]) nvs_set_str(h, "mk_host", mk_host);
        int port = atoi(mk_port_s);
        if (port > 0 && port < 65536) nvs_set_u16(h, "mk_port", (uint16_t)port);
        // Bambu (secret bb_code only written when re-entered).
        if (bb_host[0])   nvs_set_str(h, "bb_host", bb_host);
        if (bb_serial[0]) nvs_set_str(h, "bb_serial", bb_serial);
        if (bb_code[0])   nvs_set_str(h, "bb_code", bb_code);
        // Home Assistant (secret ha_pass only written when re-entered).
        if (ha_host[0])  nvs_set_str(h, "ha_host", ha_host);
        int hp = atoi(ha_port_s);
        if (hp > 0 && hp < 65536) nvs_set_u16(h, "ha_port", (uint16_t)hp);
        if (ha_user[0])  nvs_set_str(h, "ha_user", ha_user);
        if (ha_pass[0])  nvs_set_str(h, "ha_pass", ha_pass);
        if (ha_topic[0]) nvs_set_str(h, "ha_topic", ha_topic);
        // Klipper (MQTT) — secret km_pass only written when re-entered. The TLS /
        // writeback checkboxes are persisted from their submitted state (present =
        // checked) whenever the MQTT group carries a host, so toggling either sticks.
        if (km_host[0])  nvs_set_str(h, "km_host", km_host);
        int kp = atoi(km_port_s);
        if (kp > 0 && kp < 65536) nvs_set_u16(h, "km_port", (uint16_t)kp);
        if (km_user[0])  nvs_set_str(h, "km_user", km_user);
        if (km_pass[0])  nvs_set_str(h, "km_pass", km_pass);
        if (km_inst[0])  nvs_set_str(h, "km_inst", km_inst);
        if (km_topic[0]) nvs_set_str(h, "km_topic", km_topic);
        if (km_host[0]) {
            nvs_set_u8(h, "km_tls", km_tls_s[0] ? 1 : 0);
            nvs_set_u8(h, "km_wb",  km_wb_s[0]  ? 1 : 0);
        }
        nvs_commit(h);
        nvs_close(h);
    }

    // Filament chamber zones (z_<TYPE>). dc_bambu_zone_set() opens its own NVS
    // handle, so do this after the block above closes. Only write on change.
    {
        dc_bambu_zone_t zones[DC_BAMBU_ZONE_COUNT];
        int nz = dc_bambu_zone_get_all(zones, DC_BAMBU_ZONE_COUNT);
        for (int i = 0; i < nz; i++) {
            char field[12] = {0}, val[8] = {0};
            snprintf(field, sizeof field, "z_%.7s", zones[i].name);   // built-ins only; names ≤4 chars
            form_get(body, field, val, sizeof val);
            if (val[0]) {
                int t = atoi(val);
                if (t >= 0 && t <= 70 && (uint8_t)t != zones[i].target_c)
                    dc_bambu_zone_set(zones[i].name, (uint8_t)t);
            }
        }
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:system-ui,sans-serif;text-align:center;margin-top:3em;background:#141414;color:#eee'>"
        "<h2>Saved &#10003;</h2><p>Rebooting&hellip;</p>"
        "<p><small>This page will disconnect \xE2\x80\x94 that's expected.</small></p>");
    if (have_wifi) {
        ESP_LOGI(TAG, "provisioned SSID='%s' — rebooting", chosen);
        dc_wifi_save_creds_and_reboot(chosen, pass);   // writes ssid/password + reboots
    } else {
        // STA config-only change: keep Wi-Fi creds, apply the new source on reboot.
        ESP_LOGI(TAG, "config saved (Wi-Fi unchanged) — rebooting");
        esp_restart();
    }
    return ESP_OK;                                  // unreachable
}

// Unbind (stock-style "disconnect"): clear the currently-bound source's config and
// drop the control source to None, then reboot so the client is fully torn down. A
// mutating action, so it needs the CSRF header in STA mode (open in AP provisioning).
static esp_err_t unbind_post(httpd_req_t *req)
{
    if (dc_wifi_state() != DC_WIFI_STATE_AP_PORTAL && !pb_httpd_auth_ok(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"missing/invalid X-DragonBreath-Auth header\"}");
    }
    dc_ctl_source_t src = dc_source_get();
    switch (src) {
    case DC_SRC_KLIPPER:      dc_moonraker_clear_config();    break;
    case DC_SRC_BAMBU:        dc_bambu_clear_config();        break;
    case DC_SRC_HA:           pb_ha_clear_config();           break;
    case DC_SRC_KLIPPER_MQTT: db_klipper_mqtt_clear_config(); break;
    default: break;   // already None — nothing to clear
    }
    dc_source_set(DC_SRC_NONE);
    ESP_LOGI(TAG, "unbind: cleared %s config, control source -> none; rebooting", dc_source_str(src));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"unbound\":true}");
    vTaskDelay(pdMS_TO_TICKS(250));   // let the response flush before we reboot
    esp_restart();
    return ESP_OK;                     // unreachable
}

// GET /km-config — render the Klipper-side config (moonraker.conf + printer.cfg +
// Mosquitto ACL) as text/plain, filled in from the saved Klipper-MQTT settings so
// the two ends can't drift (design §8). Braces in the Jinja templates are literal
// here — only the substituted lines go through snprintf; brace-heavy blocks are
// static. The broker password is never echoed (placeholder only).
static esp_err_t km_config_page(httpd_req_t *req)
{
    char host[64] = {0}, user[32] = {0}, inst[48] = {0}, base[48] = {0};
    uint16_t port = 1883;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz;
        sz = sizeof host;  nvs_get_str(h, "km_host", host, &sz);
        sz = sizeof user;  nvs_get_str(h, "km_user", user, &sz);
        sz = sizeof inst;  nvs_get_str(h, "km_inst", inst, &sz);
        sz = sizeof base;  nvs_get_str(h, "km_topic", base, &sz);
        nvs_get_u16(h, "km_port", &port);
        nvs_close(h);
    }
    if (!host[0]) strcpy(host, "mosquitto.lan");
    if (!user[0]) strcpy(user, "dragonbreath");
    if (!inst[0]) strcpy(inst, "myprinter");
    if (!base[0]) strcpy(base, "dragonbreath");
    // Generate a separate Moonraker identity so the broker ACL can preserve
    // directional least privilege. The saved km_user remains DragonBreath's login.
    char mrk_user[48];
    snprintf(mrk_user, sizeof mrk_user, "%s_moonraker", user);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char b[640];

    SEND(req,
        "# ===== DragonBreath - Klipper (MQTT) config =====\n"
        "# Generated from your /setup values. Paste each block into the named file,\n"
        "# restart Moonraker + Klipper, then set the control source to 'Klipper (MQTT)'.\n"
        "# Do NOT also run the native dragonbreath-klipper extra - pick one integration.\n\n");

    // ---- moonraker.conf ----
    snprintf(b, sizeof b,
        "########## moonraker.conf ##########\n"
        "[mqtt]\n"
        "address: %s\n"
        "port: %u\n"
        "username: %s\n"
        "password: <your broker password>\n"
        "instance_name: %s\n"
        "enable_moonraker_api: True\n"
        "publish_split_status: True\n"
        "status_objects:\n"
        "  gcode_macro DRAGONBREATH\n"
        "  gcode_macro DB_LINK\n\n",
        host, (unsigned)port, mrk_user, inst);
    SEND(req, b);

    snprintf(b, sizeof b, "[sensor %s]\ntype: mqtt\nname: DragonBreath\nstate_topic: %s/telemetry\n",
             base, base);
    SEND(req, b);
    SEND(req,
        "state_response_template:\n"
        "  {% set s = payload|fromjson %}\n"
        "  {set_result(\"chamber_temperature\", s[\"chamber_temperature\"]|float)}\n"
        "  {set_result(\"element_temperature\", s[\"element_temperature\"]|float)}\n"
        "parameter_chamber_temperature:\n  units=\xC2\xB0""C\n"
        "parameter_element_temperature:\n  units=\xC2\xB0""C\n\n");

    snprintf(b, sizeof b, "[power %s]\ntype: mqtt\ncommand_topic: %s/power/set\n", base, base);
    SEND(req, b);
    snprintf(b, sizeof b,
        "command_payload:\n  {command}\nstate_topic: %s/power/state\n"
        "state_response_template:\n  {payload}\noff_when_shutdown: True\n\n", base);
    SEND(req, b);

    // ---- printer.cfg (all static: brace-heavy, no substitution) ----
    SEND(req,
        "########## printer.cfg ##########\n"
        "[gcode_macro DRAGONBREATH]\n"
        "variable_seq: 0\nvariable_target: 0.0\nvariable_mode: \"off\"\n"
        "variable_fan: 0\nvariable_armed: 0\nvariable_purge_nonce: 0\n"
        "variable_temperature: -1.0\nvariable_humidity: -1.0\nvariable_fault: \"\"\ngcode:\n\n"
        "[gcode_macro DB_LINK]\nvariable_heartbeat: 0\ngcode:\n\n"
        "[delayed_gcode DB_HEARTBEAT]\ninitial_duration: 5\ngcode:\n"
        "  {% set hb = printer[\"gcode_macro DB_LINK\"].heartbeat|int %}\n"
        "  SET_GCODE_VARIABLE MACRO=DB_LINK VARIABLE=heartbeat VALUE={hb + 1}\n"
        "  UPDATE_DELAYED_GCODE ID=DB_HEARTBEAT DURATION=5\n\n");
    SEND(req,
        "# Arm + set chamber target. Writes all fields, then bumps seq LAST so the\n"
        "# device applies a coherent update. Call M141 in your filament/print START.\n"
        "[gcode_macro M141]\ngcode:\n"
        "  {% set s = params.S|default(0)|float %}\n"
        "  {% set m = \"heat\" if s > 0 else \"off\" %}\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=target VALUE={s}\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=mode VALUE='\"{m}\"'\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=armed VALUE={1 if s > 0 else 0}\n"
        "  SET_GCODE_VARIABLE MACRO=DRAGONBREATH VARIABLE=seq VALUE={printer[\"gcode_macro DRAGONBREATH\"].seq|int + 1}\n\n");
    SEND(req,
        "# M191 in MQTT mode sets the target but does NOT block (a true wait needs a\n"
        "# real Klipper sensor). To refuse printing without a chamber wait, comment out\n"
        "# this alias and uncomment the strict variant below.\n"
        "[gcode_macro M191]\ngcode:\n"
        "  {action_respond_info(\"M191: MQTT mode sets chamber target but does NOT wait.\")}\n"
        "  M141 S{params.S|default(0)}\n"
        "# [gcode_macro M191]\n# gcode:\n"
        "#   { action_raise_error(\"M191 unsupported in MQTT mode - use M141\") }\n\n");

    // ---- mosquitto ACL ----
    snprintf(b, sizeof b,
        "########## mosquitto ACL (least privilege) ##########\n"
        "# Add/update both users (do not use -c on an existing password file):\n"
        "#   mosquitto_passwd /etc/mosquitto/passwd %s\n"
        "#   mosquitto_passwd /etc/mosquitto/passwd %s\n"
        "# DragonBreath device identity\n"
        "user %s\n"
        "topic write %s/telemetry\n"
        "topic write %s/power/state\n"
        "topic write %s/status\n"
        "topic read  %s/power/set\n",
        user, mrk_user, user, base, base, base, base);
    SEND(req, b);
    snprintf(b, sizeof b,
        "topic read  %s/moonraker/status\n"
        "topic read  %s/klipper/state/gcode_macro DRAGONBREATH/#\n"
        "topic read  %s/klipper/state/gcode_macro DB_LINK/#\n"
        "# writeback (only if you enabled it):\n"
        "topic write %s/moonraker/api/request\n",
        inst, inst, inst, inst);
    SEND(req, b);
    snprintf(b, sizeof b,
        "\n"
        "# Moonraker identity (opposite direction on the same scoped topics)\n"
        "user %s\n"
        "topic read  %s/telemetry\n"
        "topic read  %s/power/state\n"
        "topic read  %s/status\n"
        "topic write %s/power/set\n",
        mrk_user, base, base, base, base);
    SEND(req, b);
    snprintf(b, sizeof b,
        "topic write %s/moonraker/status\n"
        "topic write %s/klipper/state/gcode_macro DRAGONBREATH/#\n"
        "topic write %s/klipper/state/gcode_macro DB_LINK/#\n"
        "topic read  %s/moonraker/api/request\n"
        "topic write %s/moonraker/api/response\n",
        inst, inst, inst, inst, inst);
    SEND(req, b);

    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t pb_portal_start(void)
{
    httpd_handle_t s = pb_httpd_handle();
    if (s == NULL) return ESP_ERR_INVALID_STATE;

    httpd_uri_t save   = { .uri = "/save",      .method = HTTP_POST, .handler = save_post };
    httpd_uri_t unbind = { .uri = "/unbind",    .method = HTTP_POST, .handler = unbind_post };
    httpd_uri_t rescan = { .uri = "/rescan",    .method = HTTP_POST, .handler = rescan_post };
    httpd_uri_t scan   = { .uri = "/scan.json", .method = HTTP_GET,  .handler = scan_json };
    httpd_uri_t setup  = { .uri = "/setup",     .method = HTTP_GET,  .handler = config_page };
    httpd_uri_t fw     = { .uri = "/fw",        .method = HTTP_GET,  .handler = fw_page };
    httpd_uri_t diag   = { .uri = "/diag",      .method = HTTP_GET,  .handler = diag_page };
    httpd_uri_t kmcfg  = { .uri = "/km-config", .method = HTTP_GET,  .handler = km_config_page };
    httpd_uri_t cons   = { .uri = "/console",   .method = HTTP_GET,  .handler = console_page };
    httpd_uri_t favic  = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_ico };
    httpd_uri_t root   = { .uri = "/*",          .method = HTTP_GET,  .handler = root_page };
    httpd_register_uri_handler(s, &save);
    httpd_register_uri_handler(s, &unbind);
    httpd_register_uri_handler(s, &rescan);
    httpd_register_uri_handler(s, &scan);
    httpd_register_uri_handler(s, &setup);
    httpd_register_uri_handler(s, &fw);
    httpd_register_uri_handler(s, &diag);
    httpd_register_uri_handler(s, &kmcfg);
    httpd_register_uri_handler(s, &cons);
    httpd_register_uri_handler(s, &favic);  // before the catch-all so /favicon.ico != SPA
    httpd_register_uri_handler(s, &root);   // catch-all LAST (captive-portal probes)

    if (dc_wifi_state() == DC_WIFI_STATE_AP_PORTAL) {
        dc_wifi_ap_config_t ap;
        dc_wifi_get_ap_config(&ap);
        pb_dns_start(htonl(ap.ip));
        dc_wifi_scan_start();
        ESP_LOGI(TAG, "AP captive portal active");
    } else {
        ESP_LOGI(TAG, "config portal available (STA mode)");
    }
    return ESP_OK;
}
