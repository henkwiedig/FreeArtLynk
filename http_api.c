#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http_api.h"

/* ------------------------------------------------------------------ */
/* ISP helpers (mirrored from main.c — both call into the same loaded  */
/* vendor libs in the global symbol namespace)                         */

typedef struct { uint32_t state; uint32_t ch_id; } ISP_STATE_ATTR_S;
typedef struct { uint32_t value;                  } ISP_U32_ATTR_S;
typedef struct { uint32_t type; uint32_t strength; } ISP_DE2D_STRENGTH_S;
typedef struct { ISP_DE2D_STRENGTH_S stDe2dStrength; } ISP_DE2D_ATTR_S;

#define ISP_AWB_MODE_AUTO    0
#define ISP_AWB_MODE_MANUAL  1
#define ISP_AEC_MODE_AUTO    0
#define ISP_AEC_MODE_MANUAL  1
#define ISP_NOISE_LUMA       0
#define ISP_NOISE_CHROMA     1

static int isp_call1(const char *sym, int pipe, void *attr)
{
    typedef int (*fn_t)(int, void *);
    fn_t fn = (fn_t)dlsym(RTLD_DEFAULT, sym);
    if (!fn) return -1;
    return fn(pipe, attr);
}

/* ------------------------------------------------------------------ */
/* Apply a single named parameter and update cfg.                      */
/* Returns an error string or NULL on success.                         */

static const char *apply_param(HTTP_API_S *h, const char *key, const char *val)
{
    CONFIG_S   *cfg = h->cfg;
    LDRT_API_S *api = h->api;

    if (!strcmp(key, "bitrate")) {
        int v = atoi(val);
        if (v < 100 || v > 60000) return "bitrate out of range (100-60000)";
        cfg->kbps = v;
        /* Update rate control on the running pipeline */
        AR_LDRT_PIPELINE_TX_RC_PARAM_S rc;
        memset(&rc, 0, sizeof(rc));
        rc.u32BitRateKbps  = (uint32_t)v;
        rc.u32DstFrameRate = (uint32_t)cfg->fps;
        rc.u32MinQp        = 10;
        api->PipelineSetRcParam(&rc);
        return NULL;
    }
    if (!strcmp(key, "flip")) {
        int v = atoi(val) ? 1 : 0;
        cfg->flip = v;
        ISP_STATE_ATTR_S a = { (uint32_t)v, 0 };
        isp_call1("AR_MPI_ISP_SetFlipStateTidyAttr", 0, &a);
        return NULL;
    }
    if (!strcmp(key, "mirror")) {
        int v = atoi(val) ? 1 : 0;
        cfg->mirror = v;
        ISP_STATE_ATTR_S a = { (uint32_t)v, 0 };
        isp_call1("AR_MPI_ISP_SetMirrorStateTidyAttr", 0, &a);
        return NULL;
    }
    if (!strcmp(key, "saturation")) {
        int v = atoi(val);
        if (v < 0 || v > 100) return "saturation out of range (0-100)";
        cfg->saturation = v;
        ISP_U32_ATTR_S a = { (uint32_t)v };
        isp_call1("AR_MPI_ISP_SetSaturationTidyAttr", 0, &a);
        return NULL;
    }
    if (!strcmp(key, "sharpness")) {
        int v = atoi(val);
        if (v < 0 || v > 100) return "sharpness out of range (0-100)";
        cfg->sharpness = v;
        ISP_U32_ATTR_S a = { (uint32_t)v };
        isp_call1("AR_MPI_ISP_SetSharpnessTidyAttr", 0, &a);
        return NULL;
    }
    if (!strcmp(key, "wb")) {
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        isp_call1("AR_MPI_ISP_GetAwbManuTidyAttr", 0, buf);
        if (!strcmp(val, "auto")) {
            cfg->wb = -1;
            *(uint32_t *)(buf + 0) = ISP_AWB_MODE_AUTO;
        } else {
            int v = atoi(val);
            if (v < 1000 || v > 10000) return "wb CCT out of range (1000-10000 K)";
            cfg->wb = v;
            *(uint32_t *)(buf + 0) = ISP_AWB_MODE_MANUAL;
            *(uint32_t *)(buf + 4) = (uint32_t)v;
        }
        isp_call1("AR_MPI_ISP_SetAwbManuTidyAttr", 0, buf);
        return NULL;
    }
    if (!strcmp(key, "ev")) {
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        isp_call1("AR_MPI_ISP_GetAecManuTidyAttr", 0, buf);
        if (!strcmp(val, "auto")) {
            cfg->ev_us = -1;
            *(uint32_t *)(buf + 0) = ISP_AEC_MODE_AUTO;
        } else {
            int v = atoi(val);
            if (v < 1 || v > 500000) return "ev out of range (1-500000 µs)";
            cfg->ev_us = v;
            float exp = (float)v;
            *(uint32_t *)(buf + 0) = ISP_AEC_MODE_MANUAL;
            memcpy(buf + 8, &exp, 4);
        }
        isp_call1("AR_MPI_ISP_SetAecManuTidyAttr", 0, buf);
        return NULL;
    }
    if (!strcmp(key, "dnr3d")) {
        int v = atoi(val) ? 1 : 0;
        cfg->dnr3d = v;
        ISP_U32_ATTR_S a = { v ? 50u : 0u };
        isp_call1("AR_MPI_ISP_SetDe3dStrengthTidyAttr", 0, &a);
        return NULL;
    }
    if (!strcmp(key, "dnr2d")) {
        int v = atoi(val) ? 1 : 0;
        cfg->dnr2d = v;
        uint32_t strength = v ? 50u : 0u;
        ISP_DE2D_ATTR_S luma   = { { ISP_NOISE_LUMA,   strength } };
        ISP_DE2D_ATTR_S chroma = { { ISP_NOISE_CHROMA, strength } };
        isp_call1("AR_MPI_ISP_SetDe2dStrengthTidyAttr", 0, &luma);
        isp_call1("AR_MPI_ISP_SetDe2dStrengthTidyAttr", 0, &chroma);
        return NULL;
    }
    if (!strcmp(key, "zoom")) {
        float v = strtof(val, NULL);
        if (v < 1.0f || v > 10.0f) return "zoom out of range (1.0-10.0)";
        cfg->zoom = v;
        goto apply_roi;
    }
    if (!strcmp(key, "aspect")) {
        cfg->aspect = (strcmp(val, "43") == 0) ? 1 : 0;
        goto apply_roi;
    }
    return "unknown parameter";

apply_roi: {
        uint32_t base_w, base_h;
        if (cfg->aspect == 1) {
            base_h = (uint32_t)cfg->height;
            base_w = ((uint32_t)cfg->height * 4u / 3u) & ~1u;
            if (base_w > (uint32_t)cfg->width) base_w = (uint32_t)cfg->width;
        } else {
            base_w = (uint32_t)cfg->width;
            base_h = (uint32_t)cfg->height;
        }
        if (cfg->zoom > 1.0f || cfg->aspect == 1) {
            uint32_t crop_w = ((uint32_t)((float)base_w / cfg->zoom)) & ~1u;
            uint32_t crop_h = ((uint32_t)((float)base_h / cfg->zoom)) & ~1u;
            if (crop_w > 0 && crop_h > 0 &&
                crop_w <= (uint32_t)cfg->width && crop_h <= (uint32_t)cfg->height)
                api->PipelineRoiEnable(1, crop_w, crop_h);
        } else {
            api->PipelineRoiEnable(0, 0, 0);
        }
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* URL decoding                                                         */

static void url_decode(char *dst, const char *src, size_t max)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < max; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTP response helpers                                                */

static void send_response(int fd, int code, const char *ctype,
                          const char *body, size_t blen)
{
    char hdr[256];
    const char *reason = (code == 200) ? "OK"
                       : (code == 400) ? "Bad Request" : "Not Found";
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, ctype, blen);
    write(fd, hdr, (size_t)hlen);
    if (blen) write(fd, body, blen);
}

static void send_json(int fd, const char *json)
{
    send_response(fd, 200, "application/json", json, strlen(json));
}

/* ------------------------------------------------------------------ */
/* Embedded web UI                                                      */

static const char WEB_UI[] =
"<!DOCTYPE html>\n"
"<html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>arlink controls</title>"
"<style>\n"
"*{box-sizing:border-box}body{font-family:sans-serif;max-width:640px;"
"margin:0 auto;padding:12px 16px;background:#f0f0f0}"
"h1{margin:0 0 4px;font-size:1.3em}h2{border-bottom:2px solid #ccc;"
"margin:18px 0 8px;font-size:1em;color:#444}"
"label{display:block;margin:6px 0 2px;font-size:.9em}"
"input[type=range]{width:100%;margin:2px 0}"
".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".row label{margin:0}.row input[type=number]{width:90px;padding:4px}"
"button{padding:6px 14px;background:#2196F3;color:#fff;border:none;"
"border-radius:4px;cursor:pointer;font-size:.9em}"
"button:hover{background:#1976D2}"
"button.warn{background:#f44336}button.warn:hover{background:#c62828}"
".chip{display:inline-block;padding:2px 8px;border-radius:10px;"
"font-size:.8em;background:#ddd;margin:2px}"
"#status{background:#fff;border:1px solid #ccc;padding:8px;font-size:.75em;"
"font-family:monospace;white-space:pre;overflow:auto;max-height:120px;"
"border-radius:4px;margin-bottom:8px}"
".toggle{position:relative;display:inline-block;width:42px;height:24px}"
".toggle input{display:none}.slider{position:absolute;cursor:pointer;"
"top:0;left:0;right:0;bottom:0;background:#ccc;border-radius:24px;"
"transition:.2s}.slider:before{position:absolute;content:'';"
"height:18px;width:18px;left:3px;bottom:3px;background:#fff;"
"border-radius:50%;transition:.2s}"
"input:checked+.slider{background:#2196F3}"
"input:checked+.slider:before{transform:translateX(18px)}"
"</style></head><body>\n"
"<h1>arlink_stream</h1>\n"
"<div id='status'>loading...</div>\n"

"<h2>Stream</h2>\n"
"<label>Bitrate: <span id='bval'></span> kbps</label>\n"
"<input type='range' id='bitrate' min='500' max='30000' step='100'"
" oninput=\"bval.textContent=this.value\">\n"
"<div class='row' style='margin-top:6px'>"
"<button onclick=\"set('bitrate',bitrate.value)\">Apply bitrate</button>\n"
"<button onclick=\"fetch('/api/idr').then(refresh)\">Request IDR</button></div>\n"

"<h2>Image</h2>\n"
"<div class='row'>\n"
"<label>Flip <label class='toggle'><input type='checkbox' id='flip'"
" onchange=\"set('flip',this.checked?1:0)\"><span class='slider'></span></label></label>\n"
"<label>Mirror <label class='toggle'><input type='checkbox' id='mirror'"
" onchange=\"set('mirror',this.checked?1:0)\"><span class='slider'></span></label></label>\n"
"</div>\n"
"<label>Saturation: <span id='sval'></span></label>\n"
"<input type='range' id='saturation' min='0' max='100'"
" oninput=\"sval.textContent=this.value\" onchange=\"set('saturation',this.value)\">\n"
"<label>Sharpness: <span id='shval'></span></label>\n"
"<input type='range' id='sharpness' min='0' max='100'"
" oninput=\"shval.textContent=this.value\" onchange=\"set('sharpness',this.value)\">\n"

"<h2>Color</h2>\n"
"<div class='row'>\n"
"<label>Auto WB <label class='toggle'><input type='checkbox' id='wbauto'"
" onchange=\"if(this.checked)set('wb','auto')\"><span class='slider'></span></label></label>\n"
"<input type='number' id='wbcct' min='1500' max='10000' step='100' placeholder='CCT K'>\n"
"<button onclick=\"wbauto.checked=false;set('wb',wbcct.value)\">Apply WB</button>\n"
"</div>\n"
"<div class='row' style='margin-top:8px'>\n"
"<label>Auto EV <label class='toggle'><input type='checkbox' id='evauto'"
" onchange=\"if(this.checked)set('ev','auto')\"><span class='slider'></span></label></label>\n"
"<input type='number' id='evus' min='50' max='500000' step='50' placeholder='exp µs'>\n"
"<button onclick=\"evauto.checked=false;set('ev',evus.value)\">Apply EV</button>\n"
"</div>\n"

"<h2>Noise Reduction</h2>\n"
"<div class='row'>\n"
"<label>3D DNR <label class='toggle'><input type='checkbox' id='dnr3d'"
" onchange=\"set('dnr3d',this.checked?1:0)\"><span class='slider'></span></label></label>\n"
"<label>2D DNR <label class='toggle'><input type='checkbox' id='dnr2d'"
" onchange=\"set('dnr2d',this.checked?1:0)\"><span class='slider'></span></label></label>\n"
"</div>\n"

"<h2>Framing</h2>\n"
"<label>Zoom: <span id='zval'></span>x</label>\n"
"<input type='range' id='zoom' min='100' max='400' step='5'"
" oninput=\"zval.textContent=(this.value/100).toFixed(2)\""
" onchange=\"set('zoom',(this.value/100).toFixed(2))\">\n"
"<div class='row' style='margin-top:6px'>\n"
"<label>Aspect:</label>\n"
"<label><input type='radio' name='aspect' value='169'"
" onchange=\"set('aspect','169')\"> 16:9</label>\n"
"<label><input type='radio' name='aspect' value='43'"
" onchange=\"set('aspect','43')\"> 4:3</label>\n"
"</div>\n"

"<script>\n"
"function set(k,v){"
"fetch('/api/set?'+k+'='+encodeURIComponent(v))"
".then(r=>r.json()).then(d=>{if(d.error)alert(d.error);else refresh()})"
".catch(e=>console.error(e))}\n"
"function refresh(){"
"fetch('/api/status').then(r=>r.json()).then(s=>{"
"status.textContent=JSON.stringify(s,null,2);"
"bitrate.value=s.bitrate;bval.textContent=s.bitrate;"
"flip.checked=!!s.flip;mirror.checked=!!s.mirror;"
"saturation.value=s.saturation>=0?s.saturation:50;"
"sval.textContent=saturation.value;"
"sharpness.value=s.sharpness>=0?s.sharpness:50;"
"shval.textContent=sharpness.value;"
"wbauto.checked=s.wb===-1;"
"if(s.wb>0)wbcct.value=s.wb;"
"evauto.checked=s.ev_us===-1;"
"if(s.ev_us>0)evus.value=s.ev_us;"
"dnr3d.checked=s.dnr3d===1;dnr2d.checked=s.dnr2d===1;"
"zoom.value=Math.round(s.zoom*100);zval.textContent=s.zoom.toFixed(2);"
"var ar=document.querySelector('input[name=aspect][value=\"'+(s.aspect?'43':'169')+'\"]');"
"if(ar)ar.checked=true;"
"}).catch(e=>status.textContent='error: '+e)}\n"
"refresh();setInterval(refresh,3000);\n"
"</script></body></html>\n";

/* ------------------------------------------------------------------ */
/* Request handler                                                      */

static void handle_request(int fd, HTTP_API_S *h)
{
    char buf[2048];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse first line: METHOD PATH HTTP/x.x */
    char method[8], path[512];
    if (sscanf(buf, "%7s %511s", method, path) != 2) return;

    /* Split path and query string */
    char *query = strchr(path, '?');
    if (query) *query++ = '\0';

    if (!strcmp(path, "/")) {
        send_response(fd, 200, "text/html", WEB_UI, sizeof(WEB_UI) - 1);
        return;
    }

    if (!strcmp(path, "/api/status")) {
        pthread_mutex_lock(&h->lock);
        CONFIG_S *c = h->cfg;
        char json[512];
        snprintf(json, sizeof(json),
            "{"
            "\"bitrate\":%d,"
            "\"fps\":%d,"
            "\"width\":%d,"
            "\"height\":%d,"
            "\"flip\":%d,"
            "\"mirror\":%d,"
            "\"saturation\":%d,"
            "\"sharpness\":%d,"
            "\"wb\":%d,"
            "\"ev_us\":%d,"
            "\"dnr3d\":%d,"
            "\"dnr2d\":%d,"
            "\"zoom\":%.2f,"
            "\"aspect\":%d"
            "}",
            c->kbps, c->fps, c->width, c->height,
            c->flip, c->mirror,
            c->saturation, c->sharpness,
            c->wb, c->ev_us,
            c->dnr3d, c->dnr2d,
            (double)c->zoom, c->aspect);
        pthread_mutex_unlock(&h->lock);
        send_json(fd, json);
        return;
    }

    if (!strcmp(path, "/api/set")) {
        if (!query) { send_json(fd, "{\"error\":\"no params\"}"); return; }

        /* Iterate all key=value pairs in the query string */
        char qcopy[512];
        strncpy(qcopy, query, sizeof(qcopy) - 1);
        qcopy[sizeof(qcopy) - 1] = '\0';

        pthread_mutex_lock(&h->lock);
        const char *err = NULL;
        char *tok = qcopy;
        while (tok && *tok) {
            char *amp = strchr(tok, '&');
            if (amp) *amp = '\0';
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = '\0';
                char key[64], val[256];
                url_decode(key, tok,   sizeof(key));
                url_decode(val, eq+1,  sizeof(val));
                err = apply_param(h, key, val);
                if (err) break;
            }
            tok = amp ? amp + 1 : NULL;
        }
        pthread_mutex_unlock(&h->lock);

        if (err) {
            char json[256];
            snprintf(json, sizeof(json), "{\"error\":\"%s\"}", err);
            send_json(fd, json);
        } else {
            send_json(fd, "{\"ok\":true}");
        }
        return;
    }

    if (!strcmp(path, "/api/idr")) {
        pthread_mutex_lock(&h->lock);
        h->api->PipelineIdrEnable();
        pthread_mutex_unlock(&h->lock);
        send_json(fd, "{\"ok\":true}");
        return;
    }

    send_response(fd, 404, "application/json",
                  "{\"error\":\"not found\"}", 21);
}

/* ------------------------------------------------------------------ */
/* Server thread                                                        */

static void *server_thread(void *arg)
{
    HTTP_API_S *h = arg;
    while (h->running) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int cfd = accept(h->server_fd, (struct sockaddr *)&addr, &addrlen);
        if (cfd < 0) {
            if (h->running) perror("[http] accept");
            break;
        }
        handle_request(cfd, h);
        close(cfd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */

int http_api_start(HTTP_API_S *h)
{
    pthread_mutex_init(&h->lock, NULL);
    h->running = 1;

    h->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (h->server_fd < 0) { perror("[http] socket"); return -1; }

    int yes = 1;
    setsockopt(h->server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)h->cfg->http_port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(h->server_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("[http] bind"); close(h->server_fd); return -1;
    }
    listen(h->server_fd, 4);

    if (pthread_create(&h->thread, NULL, server_thread, h) != 0) {
        perror("[http] pthread_create"); close(h->server_fd); return -1;
    }
    printf("[http] API listening on port %d\n", h->cfg->http_port);
    return 0;
}

void http_api_stop(HTTP_API_S *h)
{
    h->running = 0;
    if (h->server_fd >= 0) {
        shutdown(h->server_fd, SHUT_RDWR);
        close(h->server_fd);
        h->server_fd = -1;
    }
    pthread_join(h->thread, NULL);
    pthread_mutex_destroy(&h->lock);
}
