#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>

#include "app_config.h"
#include "data_model.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

static const char *INDEX_HTML =
    "<!doctype html><html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>Pachislo Counter</title>"
    "<style>"
    ":root{color-scheme:dark;--bg:#0b1020;--bg2:#111936;--card:rgba(14,20,41,.84);--line:rgba(148,163,184,.18);"
    "--text:#e5eefb;--muted:#8fa3bf;--accent:#67e8f9;--accent2:#34d399;--warn:#fbbf24;--danger:#fb7185;}"
    "*{box-sizing:border-box} body{margin:0;min-height:100vh;font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:radial-gradient(circle at top left,rgba(103,232,249,.22),transparent 28%),"
    "radial-gradient(circle at top right,rgba(52,211,153,.18),transparent 26%),linear-gradient(160deg,var(--bg),var(--bg2));"
    "color:var(--text);padding:20px;}"
    ".shell{max-width:960px;margin:0 auto;display:grid;gap:16px;}"
    ".hero{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;padding:8px 4px;}"
    ".eyebrow{font-size:12px;letter-spacing:.18em;text-transform:uppercase;color:var(--muted);margin:0 0 6px;}"
    "h1{margin:0;font-size:clamp(24px,4vw,36px);line-height:1.1;}"
    ".subtitle{margin:8px 0 0;color:var(--muted);max-width:60ch;}"
    ".status{display:flex;flex-wrap:wrap;gap:8px;justify-content:flex-end;}"
    ".pill{display:inline-flex;align-items:center;gap:8px;padding:10px 14px;border:1px solid var(--line);border-radius:999px;"
    "background:rgba(255,255,255,.04);backdrop-filter:blur(12px);font-size:13px;color:var(--muted);}"
    ".pill strong{color:var(--text)} .dot{width:10px;height:10px;border-radius:50%;background:var(--accent2);box-shadow:0 0 16px currentColor;}"
    ".danger .dot{background:var(--danger)} .warn .dot{background:var(--warn)}"
    ".grid{display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:16px;}"
    ".card{grid-column:span 6;background:var(--card);border:1px solid var(--line);border-radius:24px;padding:18px;box-shadow:0 18px 40px rgba(0,0,0,.22);}"
    ".card.full{grid-column:1/-1} .card h2{margin:0 0 14px;font-size:14px;letter-spacing:.12em;text-transform:uppercase;color:var(--muted);}"
    ".metric{display:flex;justify-content:space-between;align-items:baseline;gap:12px;padding:12px 0;border-top:1px solid rgba(148,163,184,.12);}"
    ".metric:first-of-type{border-top:0;padding-top:0} .metric span{color:var(--muted);font-size:13px;min-width:80px;}"
    ".metric b{font-size:clamp(20px,3vw,34px);font-variant-numeric:tabular-nums;letter-spacing:.02em;}"
    ".metric small{display:block;color:var(--muted);font-size:12px;margin-top:2px;}"
    ".big{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px;}"
    ".tile{padding:16px;border-radius:18px;background:rgba(255,255,255,.04);border:1px solid rgba(148,163,184,.12);}"
    ".tile span{display:block;color:var(--muted);font-size:12px;letter-spacing:.12em;text-transform:uppercase;}"
    ".tile b{display:block;margin-top:8px;font-size:28px;font-variant-numeric:tabular-nums;}"
    ".foot{display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap;color:var(--muted);font-size:12px;padding:2px 4px 0;}"
    "@media (max-width:720px){body{padding:14px}.hero{align-items:flex-start;flex-direction:column}.status{justify-content:flex-start}"
    ".card{grid-column:1/-1}.big{grid-template-columns:1fr}}"
    "</style></head><body><div class='shell'>"
    "<section class='hero'><div><p class='eyebrow'>Pachislo Data Counter</p><h1>Machine Dashboard</h1>"
    "<div class='status' id='status-bar'>"
    "<span class='pill' id='bonus1-pill'><span class='dot'></span>" BONUS1_LABEL " <strong id='bonus1-state'>OFF</strong></span>"
    "<span class='pill' id='bonus2-pill'><span class='dot'></span>" BONUS2_LABEL " <strong id='bonus2-state'>OFF</strong></span>"
    "<span class='pill' id='door-pill'><span class='dot'></span>DOOR <strong id='door-state'>OFF</strong></span>"
    "<span class='pill' id='error-pill'><span class='dot'></span>ERROR <strong id='error-state'>OFF</strong></span>"
    "<span class='pill' id='alarm-pill'><span class='dot'></span>ALARM <strong id='alarm'>OFF</strong></span>"
    "</div></section>"
    "<section class='grid'>"
    "<div class='card full'><h2>Game Counts</h2><div class='big'>"
    "<div class='tile'><span>ボーナス間ゲーム数</span><b id='g'>0</b><small>G</small></div>"
    "<div class='tile'><span>差枚数</span><b id='d'>0</b><small>枚</small></div>"
    "<div class='tile'><span>総ゲーム数</span><b id='ga'>0</b><small>G</small></div>"
    "</div></div>"
    "<div class='card'><h2>Medals</h2>"
    "<div class='metric'><span>IN MEDAL</span><b id='i'>0</b></div>"
    "<div class='metric'><span>OUT MEDAL</span><b id='o'>0</b></div>"
    "</div>"
    "<div class='card'><h2>Bonus</h2>"
    "<div class='metric'><span>BONUS TOTAL</span><b id='b'>0</b></div>"
    "<div class='metric'><span>" BONUS1_LABEL "</span><b id='bns-1'>0</b><small>bonus active: <span id='bonus1-state-text'>OFF</span></small></div>"
    "<div class='metric'><span>" BONUS2_LABEL "</span><b id='bns-2'>0</b><small>bonus active: <span id='bonus2-state-text'>OFF</span></small></div>"
    "<div class='metric'><span>DOOR / ERROR</span><b id='door'>OFF</b><small id='err'>OFF</small></div>"
    "</div>"
    "<div class='card full'><h2>Status</h2><div class='foot'><span>ALARM: <strong id='alarm2'>OFF</strong></span>"
    "<span>DOOR: <strong id='door2'>OFF</strong></span><span>ERROR: <strong id='err2'>OFF</strong></span></div></div>"
    "<div class='card full'><h2>スランプグラフ</h2>"
    "<canvas id='slump-graph' width='900' height='300' style='width:100%;height:auto;'></canvas>"
    "</div>"
    "<div class='card full'><h2>ボーナス履歴</h2>"
    "<canvas id='bonus-history' width='900' height='200' style='width:100%;height:auto;'></canvas>"
    "</div>"
    "</section></div><script>"
    "function pill(el,on,ac,wa){"
    "if(!el)return;const d=el.querySelector('.dot'),s=el.querySelector('strong');"
    "if(s)s.textContent=on?'ON':'OFF';"
    "const c=on?ac:wa;"
    "el.style.borderColor=c;"
    "if(d){d.style.background=c;d.style.boxShadow='0 0 12px '+c;}"
    "}"
    "async function tick(){"
    "const r=await fetch('/api/stats'); const j=await r.json();"
    "document.getElementById('g').textContent=j.total_games;"
    "document.getElementById('d').textContent=j.diff_medal;"
    "document.getElementById('ga').textContent=j.total_games_including_bonus;"
    "document.getElementById('i').textContent=j.in_medal;"
    "document.getElementById('o').textContent=j.out_medal;"
    "document.getElementById('b').textContent=j.bonus_total;"
    "document.getElementById('bns-1').textContent=j.bonus1_count;"
    "document.getElementById('bns-2').textContent=j.bonus2_count;"
    "const ac='rgba(52,211,153,.45)';const wa='rgba(251,191,36,.4)';const da='rgba(251,113,133,.5)';"
    "pill(document.getElementById('bonus1-pill'),j.bonus1_active,ac,wa);"
    "pill(document.getElementById('bonus2-pill'),j.bonus2_active,ac,wa);"
    "pill(document.getElementById('door-pill'),j.door_open,ac,wa);"
    "pill(document.getElementById('error-pill'),j.error_active,da,wa);"
    "pill(document.getElementById('alarm-pill'),j.alarm_active,da,ac);"
    "const door=j.door_open?'ON':'OFF';const err=j.error_active?'ON':'OFF';const alarm=j.alarm_active?'ON':'OFF';"
    "document.getElementById('door').textContent=door;document.getElementById('err').textContent=err;"
    "document.getElementById('alarm').textContent=alarm;document.getElementById('alarm2').textContent=alarm;"
    "document.getElementById('door2').textContent=door;document.getElementById('err2').textContent=err;"
    "document.getElementById('bonus1-state-text').textContent=j.bonus1_active?'ON':'OFF';"
    "document.getElementById('bonus2-state-text').textContent=j.bonus2_active?'ON':'OFF';"
    "document.body.style.background=j.alarm_active?'radial-gradient(circle at top left,rgba(251,113,133,.22),transparent 28%),radial-gradient(circle at top right,rgba(251,191,36,.12),transparent 26%),linear-gradient(160deg,#190b13,#300)':'';"
    "drawSlumpGraph(j.slump_graph);"
    "drawBonusHistory(j.bonus_history,j.bonus1_label,j.bonus2_label);"
    "} setInterval(tick,1000); tick();"
    "function drawSlumpGraph(data){"
    "const canvas=document.getElementById('slump-graph');"
    "if(!canvas||!data||data.length<2)return;"
    "const ctx=canvas.getContext('2d');"
    "const w=canvas.width,h=canvas.height;"
    "ctx.clearRect(0,0,w,h);"
    "let minD=0,maxD=0;"
    "for(let i=0;i<data.length;i++){"
    "if(data[i].d<minD)minD=data[i].d;"
    "if(data[i].d>maxD)maxD=data[i].d;"
    "}"
    "let range=maxD-minD;"
    "if(range===0)range=1;"
    "const pad=40;"
    "ctx.strokeStyle='rgba(148,163,184,.3)';"
    "ctx.lineWidth=1;"
    "if(minD<0&&maxD>0){"
    "const zeroY=pad+(maxD/range)*(h-2*pad);"
    "ctx.beginPath();ctx.moveTo(pad,zeroY);ctx.lineTo(w-pad,zeroY);ctx.stroke();"
    "}"
    "ctx.strokeStyle='rgba(103,232,249,.8)';"
    "ctx.lineWidth=2;"
    "ctx.beginPath();"
    "for(let i=0;i<data.length;i++){"
    "const x=pad+(i/(data.length-1))*(w-2*pad);"
    "const y=pad+((maxD-data[i].d)/range)*(h-2*pad);"
    "if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);"
    "}"
    "ctx.stroke();"
    "ctx.fillStyle='rgba(148,163,184,.8)';"
    "ctx.font='12px sans-serif';"
    "ctx.fillText(maxD.toString(),5,pad+10);"
    "ctx.fillText(minD.toString(),5,h-pad);"
    "ctx.fillText('G',w-pad+5,pad+10);"
    "// ボーナス区間の反転表示"
    "for(let i=0;i<data.length;i++){"
    "if(data[i].b){"
    "const x=pad+(i/(data.length-1))*(w-2*pad);"
    "ctx.fillStyle='rgba(251,191,36,.15)';"
    "ctx.fillRect(x-2,pad,4,h-2*pad);"
    "}"
    "}"
    "}"
    "function drawBonusHistory(data,label1,label2){"
    "const canvas=document.getElementById('bonus-history');"
    "if(!canvas||!data||data.length<1)return;"
    "const ctx=canvas.getContext('2d');"
    "const w=canvas.width,h=canvas.height;"
    "ctx.clearRect(0,0,w,h);"
    "const pad=40;"
    "const maxInterval=1000;"
    "ctx.strokeStyle='rgba(148,163,184,.3)';"
    "ctx.lineWidth=1;"
    "ctx.beginPath();ctx.moveTo(pad,h-pad);ctx.lineTo(w-pad,h-pad);ctx.stroke();"
    "const barWidth=(w-2*pad)/data.length;"
    "for(let i=0;i<data.length;i++){"
    "const interval=Math.min(data[i].i,maxInterval);"
    "const barHeight=(interval/maxInterval)*(h-2*pad);"
    "const x=pad+i*barWidth;"
    "const y=h-pad-barHeight;"
    "if(data[i].t===1){"
    "ctx.fillStyle='rgba(52,211,153,.8)';"
    "ctx.fillRect(x+2,y,barWidth-4,barHeight);"
    "}else{"
    "ctx.fillStyle='rgba(103,232,249,.8)';"
    "ctx.fillRect(x+2,y,barWidth-4,barHeight);"
    "}"
    "ctx.fillStyle='rgba(148,163,184,.8)';"
    "ctx.font='10px sans-serif';"
    "ctx.fillText(data[i].i.toString(),x+2,h-pad+12);"
    "ctx.fillText(data[i].t===1?'1':'2',x+2,y-4);"
    "}"
    "ctx.fillStyle='rgba(52,211,153,.8)';"
    "ctx.fillRect(10,10,10,10);"
    "ctx.fillStyle='rgba(148,163,184,.8)';"
    "ctx.fillText(label1,25,20);"
    "ctx.fillStyle='rgba(103,232,249,.8)';"
    "ctx.fillRect(80,10,10,10);"
    "ctx.fillStyle='rgba(148,163,184,.8)';"
    "ctx.fillText(label2,95,20);"
    "}"
    "</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stats_handler(httpd_req_t *req)
{
    counter_stats_t *stats = malloc(sizeof(counter_stats_t));
    if (!stats) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    data_model_get_snapshot(stats);

    char ip_str[32] = {0};
    wifi_manager_get_ip_str(ip_str, sizeof(ip_str));

    // Calculate required buffer size for slump graph and bonus history
    size_t graph_size = stats->slump_graph_count * 40; // ~40 bytes per point
    size_t history_size = stats->bonus_history_count * 30; // ~30 bytes per entry
    size_t json_size = 1024 + graph_size + history_size;
    char *json = malloc(json_size);
    if (!json) {
        free(stats);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int n = snprintf(json,
                     json_size,
                     "{"
                     "\"total_games\":%lu,\"in_medal\":%lu,\"out_medal\":%lu,"
                     "\"diff_medal\":%ld,"
                     "\"bonus_total\":%lu,\"bonus1_count\":%lu,\"bonus2_count\":%lu,"
                     "\"bonus1_label\":\"%s\",\"bonus2_label\":\"%s\","
                     "\"total_games_including_bonus\":%lu,"
                     "\"bonus_active\":%s,\"bonus1_active\":%s,\"bonus2_active\":%s,"
                     "\"door_open\":%s,\"error_active\":%s,\"alarm_active\":%s,\"ip\":\"%s\","
                     "\"slump_graph_count\":%lu,\"slump_graph\":",
                     (unsigned long)stats->total_games,
                     (unsigned long)stats->in_medal,
                     (unsigned long)stats->out_medal,
                     (long)stats->diff_medal,
                     (unsigned long)stats->bonus_total,
                     (unsigned long)stats->bonus1_count,
                     (unsigned long)stats->bonus2_count,
                     BONUS1_LABEL,
                     BONUS2_LABEL,
                     (unsigned long)stats->total_games_including_bonus,
                     stats->bonus_active ? "true" : "false",
                     stats->bonus1_active ? "true" : "false",
                     stats->bonus2_active ? "true" : "false",
                     stats->door_open ? "true" : "false",
                     stats->error_active ? "true" : "false",
                     (stats->door_open || stats->error_active) ? "true" : "false",
                     ip_str,
                     (unsigned long)stats->slump_graph_count);

    if (n >= (int)json_size) {
        free(json);
        free(stats);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Add slump graph array
    int offset = n;
    if (offset < (int)json_size) {
        json[offset++] = '[';
        int32_t running_diff = 0;
        for (uint32_t i = 0; i < stats->slump_graph_count && offset < (int)json_size - 50; i++) {
            if (i > 0) {
                json[offset++] = ',';
            }
            running_diff += stats->slump_graph[i].delta;
            bool is_bonus = (stats->slump_graph[i].flags & SLUMP_FLAG_BONUS) != 0;
            int written = snprintf(json + offset, json_size - offset,
                                   "{\"g\":%lu,\"d\":%ld,\"b\":%s}",
                                   (unsigned long)(i + 1),
                                   (long)running_diff,
                                   is_bonus ? "true" : "false");
            if (written > 0) {
                offset += written;
            }
        }
        json[offset++] = ']';
        json[offset++] = ',';
        
        // Add bonus history
        offset += snprintf(json + offset, json_size - offset,
                           "\"bonus_history_count\":%lu,\"bonus_history\":",
                           (unsigned long)stats->bonus_history_count);
        json[offset++] = '[';
        for (uint32_t i = 0; i < stats->bonus_history_count && offset < (int)json_size - 50; i++) {
            if (i > 0) {
                json[offset++] = ',';
            }
            int written = snprintf(json + offset, json_size - offset,
                                   "{\"i\":%lu,\"t\":%u}",
                                   (unsigned long)stats->bonus_history[i].interval_games,
                                   stats->bonus_history[i].bonus_type);
            if (written > 0) {
                offset += written;
            }
        }
        json[offset++] = ']';
        json[offset++] = '}';
        json[offset] = '\0';
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    free(stats);
    return ESP_OK;
}

bool web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t api_uri = {
        .uri = "/api/stats",
        .method = HTTP_GET,
        .handler = stats_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &api_uri);
    ESP_LOGI(TAG, "web server started on :%d", config.server_port);
    return true;
}
