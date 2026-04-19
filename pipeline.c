#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "pipeline.h"

/*
 * libldrt_pipeline.so has only libc in its DT_NEEDED but uses symbols from
 * the entire vendor stack. arlink_fpv links all of them directly, putting
 * every symbol into the global namespace before libldrt_pipeline.so is
 * loaded. We must reproduce that by pre-loading every dependency with
 * RTLD_LAZY | RTLD_GLOBAL first.
 *
 * Load order: low-level HAL → MPI → pipeline.
 * RTLD_LAZY is enough here — we just need the symbols visible, not
 * necessarily resolved until called.
 */
static const char *const DEPS[] = {
    /* OSAL / utils — no AR deps */
    "libosal.so",
    "libutils.so",
    "libmbuf.so",

    /* HAL layer */
    "libhal_dbglog.so",
    "libhal_sys.so",
    "libhal_vb.so",
    "libhal_vin.so",
    "libhal_venc.so",
    "libhal_vdec.so",
    "libhal_vo.so",
    "libhal_region.so",
    "libhal_scaler.so",
    "libhal_gdc.so",
    "libhal_vgs.so",
    "libhal_ge2d.so",
    "libhal_analyze_time.so",
    "libhal_proc.so",
    "libhal_i2c.so",
    "libhal_sysctl.so",
    "libhal_clk.so",
    "libhal_efuse.so",
    "libhal_gpio.so",
    "libhal_aio.so",
    "libhal_adc.so",
    "libhal_temp.so",
    "libhal_adec.so",
    "libhal_aenc.so",
    "libhal_npu.so",
    "libhal_dsp.so",

    /* MPI layer */
    "libmpi_sys.so",
    "libmpi_vb.so",
    "libmpi_sysctl.so",
    "libmpi_vin.so",
    "libmpi_venc.so",
    "libmpi_vdec.so",
    "libmpi_vo.so",
    "libmpi_region.so",
    "libmpi_scaler.so",
    "libmpi_gdc.so",
    "libmpi_vgs.so",
    "libmpi_vpss.so",
    "libmpi_ifc.so",
    "libmpi_npu.so",
    "libmpi_dsp.so",
    "libmpi_hdmi.so",
    "libmpi_acodec.so",
    "libmpi_adec.so",
    "libmpi_aenc.so",
    "libmpi_ai.so",
    "libmpi_ao.so",

    /* misc vendor libs referenced by pipeline */
    "libbinder_ipc.so",
    "libmpp_service.so",
    "librpc_proxy.so",
    "librpc_fs.so",
    "libtools.so",

    NULL
};

/* Keep handles so they stay loaded */
#define MAX_DEP_HANDLES 64
static void *dep_handles[MAX_DEP_HANDLES];
static int   dep_count;

static int preload_deps(void)
{
    dep_count = 0;
    for (int i = 0; DEPS[i] && dep_count < MAX_DEP_HANDLES; i++) {
        void *h = dlopen(DEPS[i], RTLD_LAZY | RTLD_GLOBAL);
        if (!h) {
            /* Non-fatal: some HALs may not exist on every firmware build */
            fprintf(stderr, "[deps] warning: dlopen %s: %s\n",
                    DEPS[i], dlerror());
        } else {
            dep_handles[dep_count++] = h;
        }
    }
    return 0;
}

static void unload_deps(void)
{
    for (int i = dep_count - 1; i >= 0; i--)
        dlclose(dep_handles[i]);
    dep_count = 0;
}

/* ------------------------------------------------------------------ */

#define LOAD_SYM_EXACT(api, handle, field, sym) \
    do { \
        (api)->field = dlsym(handle, sym); \
        if (!(api)->field) { \
            fprintf(stderr, "dlsym %s failed: %s\n", sym, dlerror()); \
            return -1; \
        } \
    } while (0)

int pipeline_load(LDRT_API_S *api, const char *lib_path)
{
    memset(api, 0, sizeof(*api));

    /* Step 1: populate global symbol namespace with all vendor deps */
    preload_deps();

    /* Step 2: now load the pipeline lib — all symbols should resolve */
    api->handle = dlopen(lib_path, RTLD_NOW | RTLD_GLOBAL);
    if (!api->handle) {
        fprintf(stderr, "dlopen %s failed: %s\n", lib_path, dlerror());
        unload_deps();
        return -1;
    }

    LOAD_SYM_EXACT(api, api->handle, SysInit,            "AR_LDRT_TX_SYS_Init");
    LOAD_SYM_EXACT(api, api->handle, TxInit,             "AR_LDRT_TX_Init");
    LOAD_SYM_EXACT(api, api->handle, PipelineCreate,     "AR_LDRT_TX_PIPELINE_Create");
    LOAD_SYM_EXACT(api, api->handle, PipelineDestroy,    "AR_LDRT_TX_PIPELINE_Destroy");
    LOAD_SYM_EXACT(api, api->handle, PipelineSetRcParam, "AR_LDRT_TX_PIPELINE_SetRcParam");
    LOAD_SYM_EXACT(api, api->handle, PipelineStart,      "AR_LDRT_TX_PIPELINE_Start");
    LOAD_SYM_EXACT(api, api->handle, PipelineStop,       "AR_LDRT_TX_PIPELINE_Stop");
    LOAD_SYM_EXACT(api, api->handle, PipelineIdrEnable,  "AR_LDRT_TX_PIPELINE_IdrEnable");
    LOAD_SYM_EXACT(api, api->handle, PipelineRoiEnable,  "AR_LDRT_TX_PIPELINE_RoiEnable");
    LOAD_SYM_EXACT(api, api->handle, VencGetFd,          "AR_LDRT_TX_VENC_GetFd");
    LOAD_SYM_EXACT(api, api->handle, VencGetStream,      "AR_LDRT_TX_VENC_GetStream");
    LOAD_SYM_EXACT(api, api->handle, VencReleaseStream,  "AR_LDRT_TX_VENC_ReleaseStream");
    LOAD_SYM_EXACT(api, api->handle, VencRequestIdr,     "AR_LDRT_TX_VENC_RequestIdr");
    LOAD_SYM_EXACT(api, api->handle, VencGetThreadStop,  "AR_LDRT_TX_VencGetThreadStop");
    LOAD_SYM_EXACT(api, api->handle, VencSendThreadStop, "AR_LDRT_TX_VencSendThreadStop");

    return 0;
}

void pipeline_unload(LDRT_API_S *api)
{
    if (api->handle) {
        dlclose(api->handle);
        api->handle = NULL;
    }
    unload_deps();
}
