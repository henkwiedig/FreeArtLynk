/*
 * arlink_fpv open-source re-implementation for Artosyn AR9301 SoC.
 *
 * Loads libldrt_pipeline.so via dlopen, starts the H.265 encoder pipeline,
 * then reads VENC NALUs and sends them as RTP/UDP (RFC 7798).
 *
 * Usage:
 *   arlink_stream [-i <dest_ip>] [-p <dest_port>] [-w <width>] [-h <height>]
 *                 [-f <fps>] [-b <kbps>] [-g <gop>] [-l <lib_path>]
 *                 [-flip] [-mirror] [-rotate]
 *
 * Defaults:
 *   dest_ip   = 192.168.0.1
 *   dest_port = 5600
 *   width     = 1920
 *   height    = 1080
 *   fps       = 60
 *   kbps      = 4000
 *   gop       = 60
 *   lib_path  = /usr/lib/libldrt_pipeline.so
 *
 * -flip   : vertical flip via ISP
 * -mirror : horizontal mirror via ISP
 * -rotate : both flip+mirror (= 180° rotation, matches original u16Rotation=1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

/*
 * libldrt_pipeline.so calls back into the hosting binary for cert verification.
 * The original arlink_fpv does RSA + JSON validation; we just return success.
 * This symbol must be exported (main binary linked with -Wl,-E).
 */
int check_cert(const char *pem_path, const char *cert_blob,
               const char *model, const char *serial)
{
    (void)pem_path; (void)cert_blob; (void)model; (void)serial;
    return 0;
}

#include "pipeline.h"
#include "rtp_h265.h"
#include "udp_sender.h"

/* ------------------------------------------------------------------ */

static volatile int g_running = 1;
static int g_wdt_fd = -1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static void *watchdog_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        if (g_wdt_fd >= 0)
            write(g_wdt_fd, "\0", 1);
        sleep(1);
    }
    return NULL;
}

static void watchdog_open(void)
{
    g_wdt_fd = open("/dev/watchdog0", O_WRONLY);
    if (g_wdt_fd < 0) {
        perror("[wdt] open /dev/watchdog0");
        return;
    }
    int timeout = 10;
    ioctl(g_wdt_fd, WDIOC_SETTIMEOUT, &timeout);
    printf("[wdt] opened, timeout=%ds\n", timeout);
    pthread_t t;
    pthread_create(&t, NULL, watchdog_thread, NULL);
    pthread_detach(t);
}

/* ------------------------------------------------------------------ */

typedef struct {
    char     dest_ip[64];
    uint16_t dest_port;
    uint16_t dest_port2;
    int      width;
    int      height;
    int      fps;
    int      kbps;
    int      gop;
    char     lib_path[256];
    /* ISP image controls — -1 means "leave at hardware default" */
    int      flip;        /* vertical flip */
    int      mirror;      /* horizontal mirror */
    int      saturation;  /* 0-100 */
    int      sharpness;   /* 0-100 */
    int      wb;          /* -1 = auto, >0 = manual CCT in Kelvin */
    int      ev_us;       /* -1 = auto AEC, >0 = manual exposure in µs */
    int      dnr3d;       /* 0 = off, 1 = on */
    int      dnr2d;       /* 0 = off, 1 = on */
    float    zoom;        /* 1.0 = no zoom, >1.0 = zoom in */
    int      aspect;      /* 0 = 16:9 (default), 1 = 4:3 */
} CONFIG_S;

static void config_defaults(CONFIG_S *c)
{
    strncpy(c->dest_ip,  "192.168.3.101",              sizeof(c->dest_ip)  - 1);
    strncpy(c->lib_path, "/usr/lib/libldrt_pipeline.so", sizeof(c->lib_path) - 1);
    c->dest_port  = 5600;
    c->dest_port2 = 5601;
    c->width      = 1920;
    c->height     = 1080;
    c->fps        = 60;
    c->kbps       = 4000;
    c->gop        = 60;
    c->flip       = 0;
    c->mirror     = 0;
    c->saturation = -1;
    c->sharpness  = -1;
    c->wb         = -2;   /* sentinel: unchanged */
    c->ev_us      = -2;   /* sentinel: unchanged */
    c->dnr3d      = -1;
    c->dnr2d      = -1;
    c->zoom       = 1.0f;
    c->aspect     = 0;
}

static void parse_args(int argc, char **argv, CONFIG_S *c)
{
    for (int i = 1; i < argc; i++) {
        /* boolean flags (no argument) */
        if      (!strcmp(argv[i], "-flip"))   { c->flip   = 1; continue; }
        else if (!strcmp(argv[i], "-mirror")) { c->mirror = 1; continue; }
        else if (!strcmp(argv[i], "-rotate")) { c->flip   = 1; c->mirror = 1; continue; }

        /* key-value pairs */
        if (i + 1 >= argc) break;
        if      (!strcmp(argv[i], "-i"))          strncpy(c->dest_ip, argv[i+1], sizeof(c->dest_ip) - 1);
        else if (!strcmp(argv[i], "-p"))          c->dest_port  = (uint16_t)atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-q"))          c->dest_port2 = (uint16_t)atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-w"))          c->width      = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-h"))          c->height     = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-f"))          c->fps        = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-b"))          c->kbps       = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-g"))          c->gop        = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-l"))          strncpy(c->lib_path, argv[i+1], sizeof(c->lib_path) - 1);
        else if (!strcmp(argv[i], "-saturation")) c->saturation = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-sharpness"))  c->sharpness  = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-wb"))         c->wb  = strcmp(argv[i+1], "auto") ? atoi(argv[i+1]) : -1;
        else if (!strcmp(argv[i], "-ev"))         c->ev_us = strcmp(argv[i+1], "auto") ? atoi(argv[i+1]) : -1;
        else if (!strcmp(argv[i], "-dnr3d"))      c->dnr3d = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-dnr2d"))      c->dnr2d = atoi(argv[i+1]);
        else if (!strcmp(argv[i], "-zoom"))       c->zoom  = strtof(argv[i+1], NULL);
        else if (!strcmp(argv[i], "-aspect"))     c->aspect = (strcmp(argv[i+1], "43") == 0) ? 1 : 0;
        else { continue; }  /* unknown flag — don't skip next arg */
        i++;
    }
}

/* ------------------------------------------------------------------ */
/*
 * Callback-driven video delivery state.
 * ArDeviceThread inside libldrt_pipeline.so calls dev_video_send for
 * every ring buffer entry after PipelineStart().  We must NOT call
 * VencGetThreadStop() — that calls AR_MPI_VENC_StopRecvFrame() which
 * breaks VIN and causes a pipeline reset loop.
 */

typedef struct {
    RTP_H265_CTX_S rtp;
    UDP_SENDER_S   udp;
    volatile long      frame_count;
    volatile uint64_t  total_bytes;
    int                fps;
} STREAM_STATE_S;

static STREAM_STATE_S g_stream;   /* channel 0 */
static STREAM_STATE_S g_stream2;  /* channel 1 */
static volatile int   g_stream_ready = 0;

static uint32_t dev_query_min_pack(uint32_t dev_id)
{
    (void)dev_id; return 1024;
}

static uint64_t dev_query_stream(int32_t dev_id)
{
    (void)dev_id;
    return g_stream.total_bytes + g_stream2.total_bytes;
}

/*
 * Ring buffer entry layout (from Ghidra analysis of AR_LDRT_TX_ArDeviceThread):
 *   +0x00  uint32_t  magic = 0x12345678
 *   +0x04  uint32_t  frame_data_len
 *   +0x08  uint32_t  channel
 *   +0x0c  uint32_t  idr_flag
 *   +0x34  uint8_t   nalu_bytes[frame_data_len]   (Annex-B, start codes present)
 * len == frame_data_len + 0x38
 * Must return > 0 (bytes consumed) or ArDeviceThread retries forever.
 */
static int32_t dev_video_send(int32_t dev_id, uint8_t *data, uint32_t len, uint32_t flags)
{
    (void)dev_id; (void)flags;

    if (!g_stream_ready)
        return (int32_t)len;

    if (len < 0x38)
        return (int32_t)len;

    uint32_t magic          = *(uint32_t *)(data + 0x00);
    uint32_t frame_data_len = *(uint32_t *)(data + 0x04);
    uint32_t channel        = *(uint32_t *)(data + 0x08);

    if (magic != 0x12345678 || frame_data_len + 0x38 > len)
        return (int32_t)len;

    /* Always report bytes consumed so dev_query_stream stays monotonically
     * increasing; level control will skip frames if it stalls. */
    if (channel == 0)
        g_stream.total_bytes  += len;
    else
        g_stream2.total_bytes += len;

    STREAM_STATE_S *st = (channel == 0) ? &g_stream : &g_stream2;

    const uint8_t *nalu_start = data + 0x34;
    size_t         nalu_total = frame_data_len;
    uint64_t       pts_us     = (uint64_t)st->frame_count * 1000000ULL
                                / (uint64_t)(st->fps ? st->fps : 60);

    /* Walk Annex-B NALUs and packetize each one */
    const uint8_t *p   = nalu_start;
    size_t         rem = nalu_total;

    while (rem > 0) {
        size_t skip_before = rem;
        const uint8_t *nalu = rtp_h265_skip_startcode(p, &rem);

        if (nalu == p && rem == skip_before) {
            /* No start code found — treat entire remainder as one NALU */
            rtp_h265_send_nalu(&st->rtp, nalu, rem, pts_us, 1);
            break;
        }

        /* Find the next start code to delimit this NALU */
        size_t nalu_len = rem;
        const uint8_t *next = nalu + rem;
        for (size_t j = 0; j + 3 < rem; j++) {
            if (nalu[j] == 0 && nalu[j+1] == 0 &&
                (nalu[j+2] == 1 ||
                 (nalu[j+2] == 0 && j + 4 < rem && nalu[j+3] == 1))) {
                nalu_len = j;
                next     = nalu + j;
                rem     -= j;
                break;
            }
        }

        int last = (next >= nalu_start + nalu_total);
        rtp_h265_send_nalu(&st->rtp, nalu, nalu_len, pts_us, last);

        if (last) break;
        p   = next;
        rem = (size_t)(nalu_start + nalu_total - next);
    }

    st->frame_count++;

    if (st->frame_count % (st->fps ? st->fps : 60) == 0)
        printf("[stream%u] %ld frames\n", channel, st->frame_count);

    return (int32_t)len;
}

static int32_t dev_rst_stream(int32_t dev_id)
{
    (void)dev_id; return 0;
}

/* ------------------------------------------------------------------ */

/*
 * ISP attribute structs from Ghidra analysis of AR_LOWDELAY_TX_SYSCTRL_SetCameraInfo.
 * Simple scalar attrs: single uint32_t field.
 * Complex attrs (AWB, AEC): use a 256-byte opaque buffer with Get+Set.
 *
 * Enum values (0=AUTO/OFF, 1=MANUAL/ON follow C convention):
 *   AWB:  0=auto, 1=manual  AEC:  0=auto, 1=manual
 *   2D DNR noise_type: 0=LUMA, 1=CHROMA
 */

typedef struct { uint32_t state;   uint32_t ch_id; } ISP_STATE_ATTR_S;
typedef struct { uint32_t value;                   } ISP_U32_ATTR_S;
typedef struct { uint32_t type;    uint32_t strength; } ISP_DE2D_STRENGTH_S;
typedef struct { ISP_DE2D_STRENGTH_S stDe2dStrength; } ISP_DE2D_ATTR_S;

#define ISP_AWB_MODE_AUTO   0
#define ISP_AWB_MODE_MANUAL 1
#define ISP_AEC_MODE_AUTO   0
#define ISP_AEC_MODE_MANUAL 1
#define ISP_NOISE_LUMA      0
#define ISP_NOISE_CHROMA    1

static int isp_call1(const char *sym, int pipe, void *attr)
{
    typedef int (*fn_t)(int, void *);
    fn_t fn = (fn_t)dlsym(RTLD_DEFAULT, sym);
    if (!fn) { fprintf(stderr, "[isp] %s not found\n", sym); return -1; }
    return fn(pipe, attr);
}

static void isp_apply_settings(const CONFIG_S *cfg, LDRT_API_S *api)
{
    /* --- flip / mirror ------------------------------------------------ */
    if (cfg->flip || cfg->mirror) {
        if (cfg->flip) {
            ISP_STATE_ATTR_S a = { (uint32_t)cfg->flip, 0 };
            isp_call1("AR_MPI_ISP_SetFlipStateTidyAttr", 0, &a);
            printf("[isp] flip=%d\n", cfg->flip);
        }
        if (cfg->mirror) {
            ISP_STATE_ATTR_S a = { (uint32_t)cfg->mirror, 0 };
            isp_call1("AR_MPI_ISP_SetMirrorStateTidyAttr", 0, &a);
            printf("[isp] mirror=%d\n", cfg->mirror);
        }
    }

    /* --- saturation --------------------------------------------------- */
    if (cfg->saturation >= 0) {
        ISP_U32_ATTR_S a = { (uint32_t)cfg->saturation };
        isp_call1("AR_MPI_ISP_SetSaturationTidyAttr", 0, &a);
        printf("[isp] saturation=%d\n", cfg->saturation);
    }

    /* --- sharpness ---------------------------------------------------- */
    if (cfg->sharpness >= 0) {
        ISP_U32_ATTR_S a = { (uint32_t)cfg->sharpness };
        isp_call1("AR_MPI_ISP_SetSharpnessTidyAttr", 0, &a);
        printf("[isp] sharpness=%d\n", cfg->sharpness);
    }

    /* --- white balance ------------------------------------------------ */
    if (cfg->wb != -2) {
        /* Struct layout: [0] awb_mode (u32), [4] cct (u32), [8+] gains (floats) */
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        isp_call1("AR_MPI_ISP_GetAwbManuTidyAttr", 0, buf);
        if (cfg->wb == -1) {
            *(uint32_t *)(buf + 0) = ISP_AWB_MODE_AUTO;
            printf("[isp] wb=auto\n");
        } else {
            *(uint32_t *)(buf + 0) = ISP_AWB_MODE_MANUAL;
            *(uint32_t *)(buf + 4) = (uint32_t)cfg->wb;
            printf("[isp] wb=manual cct=%dK\n", cfg->wb);
        }
        isp_call1("AR_MPI_ISP_SetAwbManuTidyAttr", 0, buf);
    }

    /* --- exposure (EV) ------------------------------------------------ */
    if (cfg->ev_us != -2) {
        /* Struct layout: [0] aec_mode (u32), [4] gain (float), [8] exp_time_us (float) */
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        isp_call1("AR_MPI_ISP_GetAecManuTidyAttr", 0, buf);
        if (cfg->ev_us == -1) {
            *(uint32_t *)(buf + 0) = ISP_AEC_MODE_AUTO;
            printf("[isp] ev=auto\n");
        } else {
            float exp = (float)cfg->ev_us;
            *(uint32_t *)(buf + 0) = ISP_AEC_MODE_MANUAL;
            memcpy(buf + 8, &exp, 4);
            printf("[isp] ev=manual exp_time=%dµs\n", cfg->ev_us);
        }
        isp_call1("AR_MPI_ISP_SetAecManuTidyAttr", 0, buf);
    }

    /* --- 3D denoising ------------------------------------------------- */
    if (cfg->dnr3d >= 0) {
        ISP_U32_ATTR_S a = { cfg->dnr3d ? 50u : 0u };
        isp_call1("AR_MPI_ISP_SetDe3dStrengthTidyAttr", 0, &a);
        printf("[isp] dnr3d=%d (strength=%u)\n", cfg->dnr3d, a.value);
    }

    /* --- 2D denoising ------------------------------------------------- */
    if (cfg->dnr2d >= 0) {
        uint32_t strength = cfg->dnr2d ? 50u : 0u;
        ISP_DE2D_ATTR_S luma  = { { ISP_NOISE_LUMA,   strength } };
        ISP_DE2D_ATTR_S chroma = { { ISP_NOISE_CHROMA, strength } };
        isp_call1("AR_MPI_ISP_SetDe2dStrengthTidyAttr", 0, &luma);
        isp_call1("AR_MPI_ISP_SetDe2dStrengthTidyAttr", 0, &chroma);
        printf("[isp] dnr2d=%d (strength=%u)\n", cfg->dnr2d, strength);
    }

    /* --- zoom + aspect ratio (via VENC ROI crop) ---------------------- */
    if (cfg->zoom > 1.0f || cfg->aspect == 1) {
        /* Base dimensions for the requested aspect ratio */
        uint32_t base_w, base_h;
        if (cfg->aspect == 1) {
            base_h = (uint32_t)cfg->height;
            base_w = ((uint32_t)cfg->height * 4u / 3u) & ~1u;  /* e.g. 1080 → 1440 */
        } else {
            base_w = (uint32_t)cfg->width;
            base_h = (uint32_t)cfg->height;
        }
        /* Apply zoom on top of the aspect crop */
        uint32_t crop_w = ((uint32_t)((float)base_w / cfg->zoom)) & ~1u;
        uint32_t crop_h = ((uint32_t)((float)base_h / cfg->zoom)) & ~1u;
        if (crop_w > 0 && crop_h > 0 &&
            crop_w <= (uint32_t)cfg->width && crop_h <= (uint32_t)cfg->height) {
            api->PipelineRoiEnable(1, crop_w, crop_h);
            printf("[isp] roi %ux%u (zoom=%.2f aspect=%s)\n",
                   crop_w, crop_h, (double)cfg->zoom,
                   cfg->aspect ? "4:3" : "16:9");
        } else {
            fprintf(stderr, "[isp] invalid roi %ux%u, skipping\n", crop_w, crop_h);
        }
    }
}

/* ------------------------------------------------------------------ */
/*
 * Probe the camera sensor via libmpi_vin.so (already in global namespace).
 * Fills vi with the sensor name, pstSnsObj, and hardware bus assignments
 * extracted from the /usr/usrdata/sensor_board_cfg.cjson config.
 *
 * Raw probe element layout (stride 0x198):
 *   [0x000] sensor name string
 *   [0x0cc] match_flag  (bit0=MIPI dev, bit1=SnsMode, bit2=MipiFreHz)
 *   [0x0d0] match_data[0] = s32MipiDev
 *   [0x0d4] match_data[1] = u32SnsMode
 *   [0x0d8] match_data[2] = s32MipiFreHz
 *   [0x110] STRU_SEN_IF_BOARD_INFO_S (136 bytes)
 *             [+0x04] i2c_index
 *             [+0x08] rst_valid
 *             [+0x0c] rst[3]
 *             [+0x18] power_valid
 *             [+0x1c] power[3]
 *             [+0x74] mclk_id
 */
static int sensor_probe_vin(AR_LDRT_PIPE_TX_VIN_PARAMS_S *vi,
                             int width, int height, float fps)
{
    typedef uint8_t *(*fn_probe_t)(const char *, int *, uint32_t, int);
    typedef void    *(*fn_get_sns_t)(const char *, void **);
    typedef void     (*fn_free_t)(void *);

    fn_probe_t   probe_fn   = (fn_probe_t)  dlsym(RTLD_DEFAULT, "AR_MPI_VIN_ProbeDev");
    fn_get_sns_t get_sns_fn = (fn_get_sns_t)dlsym(RTLD_DEFAULT, "AR_MPI_VIN_GetSensorObj");
    fn_free_t    free_fn    = (fn_free_t)   dlsym(RTLD_DEFAULT, "AR_MPI_VIN_FreeDevInfor");

    if (!probe_fn || !get_sns_fn) {
        fprintf(stderr, "[vin] dlsym failed: %s\n", dlerror());
        return -1;
    }

    int cnt = 0;
    uint8_t *devs = probe_fn("/usr/usrdata/sensor_board_cfg.cjson",
                             &cnt, 0xffffffff, 0);
    if (!devs || cnt < 1) {
        fprintf(stderr, "[vin] sensor probe failed (cnt=%d)\n", cnt);
        return -1;
    }

    /* First element at devs[0], stride 0x198 */
    const char *sns_name = (const char *)devs;
    printf("[vin] probed sensor: %s\n", sns_name);

    uint32_t match_flag = *(uint32_t *)(devs + 0x0cc);
    int32_t  mipi_dev   = (match_flag & 1) ? *(int32_t *)(devs + 0x0d0) : 0;
    uint32_t sns_mode   = (match_flag & 2) ? *(uint32_t *)(devs + 0x0d4) : 0;
    (void)((match_flag & 4) ? *(int32_t *)(devs + 0x0d8) : 0); /* mipi freq from cfg_vin.json */

    /* STRU_SEN_IF_BOARD_INFO_S at element+0x110 */
    const uint8_t *bi = devs + 0x110;
    int32_t  i2c_dev    = *(int32_t *)(bi + 0x04);  /* i2c_index */
    int32_t  rst_valid  = *(int32_t *)(bi + 0x08);
    int32_t  rst0       = *(int32_t *)(bi + 0x0c);
    int32_t  rst1       = *(int32_t *)(bi + 0x10);
    int32_t  rst2       = *(int32_t *)(bi + 0x14);
    int32_t  pwr_valid  = *(int32_t *)(bi + 0x18);
    int32_t  pwr0       = *(int32_t *)(bi + 0x1c);
    int32_t  pwr1       = *(int32_t *)(bi + 0x20);
    int32_t  pwr2       = *(int32_t *)(bi + 0x24);
    int32_t  mclk_valid = *(int32_t *)(bi + 0x68);  /* mcld_valid */
    int32_t  mclk_id    = *(int32_t *)(bi + 0x74);  /* mclk_id */

    /* Get ISP sensor object from the sensor driver .so */
    ISP_SNS_OBJ_S *sns_obj = NULL;
    void *handle = get_sns_fn(sns_name, (void **)&sns_obj);
    if (!handle || !sns_obj) {
        fprintf(stderr, "[vin] AR_MPI_VIN_GetSensorObj(%s) failed\n", sns_name);
        if (free_fn) free_fn(devs);
        return -1;
    }
    printf("[vin] sensor obj: %p  handle: %p\n", (void *)sns_obj, handle);

    memset(vi, 0, sizeof(*vi));
    strncpy(vi->s8Sensor,     sns_name, sizeof(vi->s8Sensor) - 1);
    vi->eFrontendType  = AR_LDRT_CAMERA;
    vi->eSyncMode      = AR_LDRT_VIN_SYNC_EXTERNAL_MODE;  /* 0, matches original */
    vi->u32DefaultWidth  = (uint32_t)width;
    vi->u32DefaultHeight = (uint32_t)height;
    vi->f32DefaultFps    = fps;
    vi->eVinFormat     = PIXEL_FORMAT_YVU_PLANAR_420;
    vi->s32I2cDev      = i2c_dev;
    vi->s32VifUseVideoStable = 1;
    vi->s32MipiDev     = mipi_dev;
    vi->s32CamMode     = 0;
    vi->s32HdrFreHz    = 300000000;
    vi->s32VifFreHz    = 300000000;
    vi->s32IspFreHz    = 300000000;
    vi->s32MipiFreHz   = 112000000;
    vi->u32SnsMode     = sns_mode;
    vi->pstSnsObj      = sns_obj;

    if (rst_valid) {
        vi->s32UsrGpioDefined = 1;
        vi->s32ResetGpio[0] = rst0;
        vi->s32ResetGpio[1] = rst1;
        vi->s32ResetGpio[2] = rst2;
    }
    if (pwr_valid) {
        vi->s32UsrGpioDefined = 1;
        vi->s32PowerGpio[0] = pwr0;
        vi->s32PowerGpio[1] = pwr1;
        vi->s32PowerGpio[2] = pwr2;
    }
    if (mclk_valid) {
        vi->u8MclkIdEnable = 1;
        vi->s32MclkId = mclk_id;
    }

    if (free_fn) free_fn(devs);
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    CONFIG_S cfg;
    config_defaults(&cfg);
    parse_args(argc, argv, &cfg);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    watchdog_open();

    /* 1. Load libldrt_pipeline.so (also pre-loads all vendor deps) */
    LDRT_API_S api;
    if (pipeline_load(&api, cfg.lib_path) != 0)
        return 1;

    /* 2. SYS init — allocates internal handle and VB media pools */
    AR_LDRT_PIPE_SYS_PARAMS_S sys_params;
    memset(&sys_params, 0, sizeof(sys_params));
    sys_params.u8LowdelayPipeEnable = 0;
    sys_params.eFormat   = PIXEL_FORMAT_YVU_PLANAR_420;
    sys_params.eFrontType = AR_LDRT_CAMERA;
    sys_params.u32WidthMax  = (uint32_t)cfg.width;
    sys_params.u32HeightMax = (uint32_t)cfg.height;
    sys_params.u32FpsMax    = (uint32_t)cfg.fps;
    sys_params.s32VinBufferCnt = 0;  /* 0 → library defaults to 5 */

    if (api.SysInit(&sys_params) != 0) {
        fprintf(stderr, "AR_LDRT_TX_SYS_Init failed\n");
        goto err_unload;
    }

    /* 3. Probe sensor and build TX module params */
    AR_LDRT_PIPE_TX_MODULE_PARAMS_S mod;
    memset(&mod, 0, sizeof(mod));

    mod.u8StreamOutEnable = 0;  /* no AR8030 BB — disables wireless queue path in ArDeviceThread */
    mod.stArInputInfo.eInputMode = AR_LDRT_VIN_INPUT_MODE;

    if (sensor_probe_vin(&mod.stArLdrtVi, cfg.width, cfg.height,
                         (float)cfg.fps) != 0) {
        fprintf(stderr, "sensor_probe_vin failed\n");
        goto err_unload;
    }

    /* VENC params */
    mod.stArLdrtVenc.enVencType  = AR_LDRT_VENC_ENCODE_TYPE_H265;
    mod.stArLdrtVenc.enRcMode    = AR_LDRT_VENC_RC_MODE_CBR;
    mod.stArLdrtVenc.u32AvgBps   = (uint32_t)cfg.kbps;  /* kbps, not bps */
    mod.stArLdrtVenc.u32Gop      = (uint32_t)cfg.gop;
    mod.stArLdrtVenc.u32QpMaxI   = 51;
    mod.stArLdrtVenc.u32QpMinI   = 10;
    mod.stArLdrtVenc.u32QpMaxP   = 51;
    mod.stArLdrtVenc.u32QpMinP   = 10;
    mod.stArLdrtVenc.u32StatTime = 1;
    mod.stArLdrtVenc.u32MinThroughputRate = 0x400;  /* 1024 kbps minimum */
    mod.stArLdrtVenc.u32VencCoreFrequency  = 333;   /* MHz, from arlink_fpv globals */
    mod.stArLdrtVenc.u32VencBpuFrequency   = 333;
    mod.stArLdrtVenc.u32VencMjpegFrequency = 300;

    /* Device params — one AP device with stub callbacks.
     * u32MacAddr[0] is used as the "BB device ID" by the cert check;
     * it must be non-zero or the library rejects it immediately.
     * Our check_cert stub returns success unconditionally, so any
     * non-zero value works here.                                     */
    mod.stArLdrtDevice.s32ApDeviceNum = 1;
    mod.stArLdrtDevice.u32MacAddr[0]  = 0x12345678;  /* non-zero BB id */
    mod.stArLdrtDevice.AR_TX_AP_DEVICE_QueryMinPackSizeCb = dev_query_min_pack;
    mod.stArLdrtDevice.AR_TX_AP_DEVICE_QueryStreamSizeCb  = dev_query_stream;
    mod.stArLdrtDevice.AR_TX_AP_DEVICE_VideoSendCb        = dev_video_send;
    mod.stArLdrtDevice.AR_TX_AP_DEVICE_RstStreamSizeCb    = dev_rst_stream;

    /* 4. TX init — sets up VIN/VENC/VO subsystems */
    int tx_ret = api.TxInit(&mod);
    if (tx_ret != 0) {
        fprintf(stderr, "AR_LDRT_TX_Init failed (ret=%d)\n", tx_ret);
        goto err_unload;
    }

    /* 5. Build pipeline creation params */
    AR_LDRT_PIPELINE_MEDIA_PARAMS_S media_params;
    memset(&media_params, 0, sizeof(media_params));
    media_params.eLdrtPipeMode   = AR_LDRT_PIPELINE_LOWDELAY_MODE;
    media_params.eLowdelayLevel  = AR_LDRT_PIPELINE_LOWDELAY_LEVEL1;
    media_params.bIsInterLaced   = 0;
    media_params.u32VideoWidth   = (uint32_t)cfg.width;
    media_params.u32VideoHeight  = (uint32_t)cfg.height;
    media_params.u32VideoFps     = (uint32_t)cfg.fps;
    media_params.s32CpuAffinity  = -1;

    if (api.PipelineCreate(&media_params) != 0) {
        fprintf(stderr, "AR_LDRT_TX_PIPELINE_Create failed\n");
        goto err_uninit;
    }

    /* 6. Set RC (rate control) parameters */
    AR_LDRT_PIPELINE_TX_RC_PARAM_S rc_param;
    memset(&rc_param, 0, sizeof(rc_param));
    rc_param.u32BitRateKbps  = (uint32_t)cfg.kbps;
    rc_param.u32DstFrameRate = (uint32_t)cfg.fps;
    rc_param.u32MinQp        = 10;

    api.PipelineSetRcParam(&rc_param);  /* void — no return value to check */

    /* 7. Init UDP and RTP before PipelineStart so dev_video_send is ready */
    memset(&g_stream,  0, sizeof(g_stream));
    memset(&g_stream2, 0, sizeof(g_stream2));
    g_stream.fps  = cfg.fps;
    g_stream2.fps = cfg.fps;
    if (udp_sender_open(&g_stream.udp,  cfg.dest_ip, cfg.dest_port)  != 0)
        goto err_uninit;
    if (udp_sender_open(&g_stream2.udp, cfg.dest_ip, cfg.dest_port2) != 0) {
        udp_sender_close(&g_stream.udp);
        goto err_uninit;
    }
    rtp_h265_init(&g_stream.rtp,  0x12345678, udp_sender_write, &g_stream.udp);
    rtp_h265_init(&g_stream2.rtp, 0x87654321, udp_sender_write, &g_stream2.udp);
    g_stream_ready = 1;

    /* 8. Start pipeline — ArDeviceThread starts calling dev_video_send */
    api.PipelineStart(0);

    /* Apply ISP settings after pipeline is running */
    isp_apply_settings(&cfg, &api);

    /* Request an IDR on startup so the receiver can decode immediately */
    api.PipelineIdrEnable();

    printf("[main] streaming %dx%d@%d fps  %d kbps  H.265\n"
           "       ch0 -> %s:%u\n"
           "       ch1 -> %s:%u\n",
           cfg.width, cfg.height, cfg.fps, cfg.kbps,
           cfg.dest_ip, cfg.dest_port,
           cfg.dest_ip, cfg.dest_port2);

    /* 9. Main loop — frames delivered via dev_video_send callback */
    while (g_running)
        sleep(1);

    printf("[main] shutting down after ch0=%ld ch1=%ld frames\n",
           g_stream.frame_count, g_stream2.frame_count);

    g_stream_ready = 0;
    udp_sender_close(&g_stream.udp);
    udp_sender_close(&g_stream2.udp);

    api.PipelineStop(0);
    api.PipelineDestroy(1);  /* 1 = also disable VIN */
err_uninit:
    /* No explicit AR_LDRT_TX_Uninit in public API — pipeline destroy handles it */
err_unload:
    pipeline_unload(&api);
    return 0;
}
