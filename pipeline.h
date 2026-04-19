#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- AR SDK constant aliases (from DWARF debug info in arlink_fpv) ---- */

#define PIXEL_FORMAT_YVU_PLANAR_420  0x17

/* AR_LDRT_FRONTEND_TYPE_E */
#define AR_LDRT_CAMERA      0
#define AR_LDRT_HDMI        1
#define AR_LDRT_SDI         2
#define AR_LDRT_YUV_CAMERA  3

/* AR_LDRT_VIN_SYNC_MODE_E */
#define AR_LDRT_VIN_SYNC_EXTERNAL_MODE  0
#define AR_LDRT_VIN_SYNC_INTERNAL_MODE  1

/* AR_LDRT_INPUT_MODE_E */
#define AR_LDRT_VIN_INPUT_MODE   0
#define AR_LDRT_USR_INPUT_MODE   1

/* AR_LDRT_PIPELINE_MODE_E */
#define AR_LDRT_PIPELINE_NORMAL_MODE    0
#define AR_LDRT_PIPELINE_LOWDELAY_MODE  1

/* AR_LDRT_PIPELINE_LOWDELAY_LEVEL_E */
#define AR_LDRT_PIPELINE_LOWDELAY_LEVEL0  0
#define AR_LDRT_PIPELINE_LOWDELAY_LEVEL1  1

/* AR_LDRT_PIPE_TX_VENC_TYPE_E */
#define AR_LDRT_VENC_ENCODE_TYPE_H264   0
#define AR_LDRT_VENC_ENCODE_TYPE_H265   1
#define AR_LDRT_VENC_ENCODE_TYPE_MJPEG  2

/* AR_LDRT_PIPE_TX_VENC_RCMODE_E */
#define AR_LDRT_VENC_RC_MODE_CBR  0
#define AR_LDRT_VENC_RC_MODE_VBR  1

/* ---- VENC stream types (AR_LDRT_TX_VencGetStreamThread) -------------- */

typedef struct {
    void     *pstPack;       /* +0x00: pointer to VENC_PACK_S array */
    uint32_t  u32PackCount;  /* +0x08 */
    uint32_t  u32Seq;        /* +0x0C: frame sequence number */
} LDRT_STREAM_BUF_S;

typedef struct {           /* stride = 0x98 bytes */
    uint8_t  _pad0[0x08];
    void    *pu8Addr;      /* +0x08: NALU data pointer */
    uint8_t  _pad1[0x08];
    uint32_t  u32Len;      /* +0x10: NALU byte length */
    uint8_t  _pad2[0x18];
    uint32_t  DataType;    /* +0x2C: IDR=0x13 or 2, P=others */
    uint8_t  _pad3[0x68];
} VENC_PACK_S;

/* ---- AR_LDRT_PIPE_SYS_PARAMS_S (92 bytes) ---------------------------- */

typedef struct {
    uint8_t  u8LowdelayPipeEnable;   /* [0]    */
    uint8_t  _pad0[3];               /* [1-3]  align to 4 */
    uint32_t eFormat;                /* [4]    PIXEL_FORMAT_E */
    uint32_t eFrontType;             /* [8]    AR_LDRT_FRONTEND_TYPE_E */
    uint32_t u32WidthMax;            /* [12]   stMaxResolution */
    uint32_t u32HeightMax;           /* [16]  */
    uint32_t u32FpsMax;              /* [20]  */
    int32_t  s32VinBufferCnt;        /* [24]  */
    struct {
        uint32_t u32UsrVbSize;
        uint32_t u32UsrVbCnt;
    } stUsrVb[8];                    /* [28]   64 bytes */
} AR_LDRT_PIPE_SYS_PARAMS_S;        /* total 92 bytes */

/* ---- AR_LDRT_PIPE_TX_VIN_PARAMS_S (440 bytes) ------------------------ */

typedef struct ISP_SNS_OBJ_S ISP_SNS_OBJ_S;

typedef struct {
    char     s8Sensor[32];           /* [0]   sensor name string */
    uint32_t eFrontendType;          /* [32]  AR_LDRT_FRONTEND_TYPE_E */
    uint32_t eSyncMode;              /* [36]  AR_LDRT_VIN_SYNC_MODE_E */
    uint32_t u32DefaultWidth;        /* [40] */
    uint32_t u32DefaultHeight;       /* [44] */
    float    f32DefaultFps;          /* [48] */
    uint32_t eVinFormat;             /* [52]  PIXEL_FORMAT_E */
    int32_t  s32I2cDev;              /* [56] */
    int32_t  s32VifUseVideoStable;   /* [60] */
    int32_t  s32MipiDev;             /* [64] */
    int32_t  s32CamMode;             /* [68] */
    int32_t  s32HdrFreHz;            /* [72] */
    int32_t  s32VifFreHz;            /* [76] */
    int32_t  s32IspFreHz;            /* [80] */
    int32_t  s32MipiFreHz;           /* [84] */
    uint32_t u32SnsMode;             /* [88] */
    uint8_t  _pad0[4];              /* [92-95]  align to 8 */
    ISP_SNS_OBJ_S *pstSnsObj;        /* [96]  8 bytes */
    int32_t  s32UsrGpioDefined;      /* [104] */
    int32_t  s32ResetGpio[3];        /* [108] */
    int32_t  s32PowerGpio[3];        /* [120] */
    int32_t  s32CommonGpio[3];       /* [132] */
    int32_t  s32FrameOutEnable;      /* [144] */
    int32_t  s32FrameOutDepth;       /* [148] */
    uint8_t  u8SettleCountEnable;    /* [152] */
    uint8_t  _pad1[3];              /* [153-155] */
    int32_t  s32SettleCount;         /* [156] */
    uint8_t  u8MclkIdEnable;         /* [160] */
    uint8_t  _pad2[3];              /* [161-163] */
    int32_t  s32MclkId;              /* [164] */
    int32_t  s32PipeExtAttrEnable;   /* [168] */
    uint8_t  _pad3[4];              /* [172-175]  align to 8 */
    void    *AR_LDRT_VIN_GET_PIPE_EXT_ATTR; /* [176] fn ptr */
    uint8_t  u8ExtChnEnable;         /* [184] */
    uint8_t  _pad4[3];              /* [185-187] */
    uint8_t  stExtChnAttr[244];      /* [188]  VI_EXTCHN_ATTR_S */
    uint8_t  u8PcsCfgByUsr;          /* [432] */
    uint8_t  _pad5[7];              /* [433-439] */
} AR_LDRT_PIPE_TX_VIN_PARAMS_S;     /* total 440 bytes */

/* ---- AR_LDRT_PIPE_TX_VENC_PARAMS_S (60 bytes) ------------------------ */

typedef struct {
    int32_t  u32SkipThreshold;          /* [0]  */
    uint8_t  u8KeyFrameMultiplier;      /* [4]  */
    uint8_t  u8NonKeyFrameMultiplier;   /* [5]  */
    uint8_t  _pad0[2];                  /* [6-7] */
    uint32_t u32AvgBps;                 /* [8]  in kbps (not bps) */
    uint32_t u32Gop;                    /* [12] */
    uint32_t u32QpMaxI;                 /* [16] */
    uint32_t u32QpMinI;                 /* [20] */
    uint32_t u32QpMaxP;                 /* [24] */
    uint32_t u32QpMinP;                 /* [28] */
    uint32_t u32StatTime;               /* [32] */
    uint32_t enVencType;                /* [36] AR_LDRT_PIPE_TX_VENC_TYPE_E */
    uint32_t enRcMode;                  /* [40] AR_LDRT_PIPE_TX_VENC_RCMODE_E */
    uint32_t u32VencCoreFrequency;      /* [44] */
    uint32_t u32VencBpuFrequency;       /* [48] */
    uint32_t u32VencMjpegFrequency;     /* [52] */
    uint32_t u32MinThroughputRate;      /* [56] */
} AR_LDRT_PIPE_TX_VENC_PARAMS_S;       /* total 60 bytes */

/* ---- AR_LDRT_PIPE_TX_DEVICE_PARAMS_S (56 bytes) ---------------------- */

typedef uint32_t (*fn_dev_query_min_pack)(uint32_t dev_id);
typedef uint64_t (*fn_dev_query_stream)(int32_t dev_id);
typedef int32_t  (*fn_dev_video_send)(int32_t dev_id, uint8_t *data,
                                      uint32_t len, uint32_t flags);
typedef int32_t  (*fn_dev_rst_stream)(int32_t dev_id);

typedef struct {
    int32_t  s32ApDeviceNum;            /* [0]  must be 1 or 2 */
    int32_t  s32ApDeviceId[2];          /* [4]  */
    uint32_t u32MacAddr[2];             /* [12] */
    fn_dev_query_min_pack  AR_TX_AP_DEVICE_QueryMinPackSizeCb; /* [24] */
    fn_dev_query_stream    AR_TX_AP_DEVICE_QueryStreamSizeCb;  /* [32] */
    fn_dev_video_send      AR_TX_AP_DEVICE_VideoSendCb;        /* [40] */
    fn_dev_rst_stream      AR_TX_AP_DEVICE_RstStreamSizeCb;    /* [48] */
} AR_LDRT_PIPE_TX_DEVICE_PARAMS_S;     /* total 56 bytes */

/* ---- AR_LDRT_PIPE_TX_INPUT_PARAMS_S (24 bytes) ----------------------- */

typedef struct {
    uint32_t eInputMode;                /* [0]  AR_LDRT_INPUT_MODE_E */
    uint8_t  _pad0[4];                  /* [4-7] align to 8 */
    void    *AR_LDRT_GET_USR_FRAME;     /* [8]  fn ptr */
    void    *AR_LDRT_RELEASE_USR_FRAME; /* [16] fn ptr */
} AR_LDRT_PIPE_TX_INPUT_PARAMS_S;      /* total 24 bytes */

/* ---- AR_LDRT_PIPE_TX_MODULE_PARAMS_S (728 bytes) --------------------- */

typedef struct {
    uint8_t  u8StreamOutEnable;             /* [0]    */
    uint8_t  _pad0[7];                      /* [1-7]  align to 8 */
    AR_LDRT_PIPE_TX_INPUT_PARAMS_S  stArInputInfo;  /* [8]    24 bytes */
    AR_LDRT_PIPE_TX_VIN_PARAMS_S    stArLdrtVi;     /* [32]   440 bytes */
    AR_LDRT_PIPE_TX_VENC_PARAMS_S   stArLdrtVenc;   /* [472]  60 bytes */
    uint8_t  _pad1[4];                      /* [532-535] align to 8 */
    AR_LDRT_PIPE_TX_DEVICE_PARAMS_S stArLdrtDevice; /* [536]  56 bytes */
    uint8_t  stArLdrtVo[104];               /* [592]  VO params (zero-filled) */
    uint8_t  stArCryptoInfo[32];            /* [696]  Crypto params (zero-filled) */
} AR_LDRT_PIPE_TX_MODULE_PARAMS_S;         /* total 728 bytes */

/* ---- AR_LDRT_PIPELINE_MEDIA_PARAMS_S (128 bytes) --------------------- */

typedef struct {
    uint32_t eLdrtPipeMode;             /* [0]    AR_LDRT_PIPELINE_MODE_E */
    uint32_t eLowdelayLevel;            /* [4]    AR_LDRT_PIPELINE_LOWDELAY_LEVEL_E */
    uint32_t bIsInterLaced;             /* [8]    AR_BOOL */
    uint32_t u32VideoWidth;             /* [12]  */
    uint32_t u32VideoHeight;            /* [16]  */
    uint32_t u32VideoFps;               /* [20]  */
    uint32_t u32ScaleEnable;            /* [24]  */
    float    f32ScaleRatio;             /* [28]  */
    uint32_t eVideoScaleMode;           /* [32]  */
    uint8_t  _pad0[4];                  /* [36-39] align to 8 */
    uint8_t  stVoMediaParams[80];       /* [40]   VO params (zero-filled) */
    int32_t  s32CpuAffinity;            /* [120]  -1 = any CPU */
    uint8_t  _pad1[4];                  /* [124-127] */
} AR_LDRT_PIPELINE_MEDIA_PARAMS_S;     /* total 128 bytes */

/* ---- AR_LDRT_PIPELINE_TX_RC_PARAM_S (12 bytes) ----------------------- */

typedef struct {
    uint32_t u32BitRateKbps;
    uint32_t u32DstFrameRate;
    uint32_t u32MinQp;
} AR_LDRT_PIPELINE_TX_RC_PARAM_S;

/* ---- LDRT API function pointer table --------------------------------- */

typedef int (*fn_AR_LDRT_TX_SYS_Init)(AR_LDRT_PIPE_SYS_PARAMS_S *);
typedef int (*fn_AR_LDRT_TX_Init)(AR_LDRT_PIPE_TX_MODULE_PARAMS_S *);
typedef int (*fn_AR_LDRT_TX_PIPELINE_Create)(AR_LDRT_PIPELINE_MEDIA_PARAMS_S *);
typedef void (*fn_AR_LDRT_TX_PIPELINE_Destroy)(int vin_disable);
typedef void (*fn_AR_LDRT_TX_PIPELINE_SetRcParam)(AR_LDRT_PIPELINE_TX_RC_PARAM_S *);
typedef void (*fn_AR_LDRT_TX_PIPELINE_Start)(int ap_dev_id);
typedef void (*fn_AR_LDRT_TX_PIPELINE_Stop)(int ap_dev_id);
typedef void (*fn_AR_LDRT_TX_PIPELINE_IdrEnable)(void);
typedef int (*fn_AR_LDRT_TX_VENC_GetFd)(int chn);
typedef int (*fn_AR_LDRT_TX_VENC_GetStream)(int chn, LDRT_STREAM_BUF_S *buf);
typedef int (*fn_AR_LDRT_TX_VENC_ReleaseStream)(int chn, LDRT_STREAM_BUF_S *buf);
typedef int (*fn_AR_LDRT_TX_VENC_RequestIdr)(int chn);
typedef int (*fn_AR_LDRT_TX_VencGetThreadStop)(void);
typedef int (*fn_AR_LDRT_TX_VencSendThreadStop)(void);
typedef void (*fn_AR_LDRT_TX_PIPELINE_RoiEnable)(uint32_t enable, uint32_t width, uint32_t height);

typedef struct {
    void *handle;

    fn_AR_LDRT_TX_SYS_Init            SysInit;
    fn_AR_LDRT_TX_Init                TxInit;
    fn_AR_LDRT_TX_PIPELINE_Create     PipelineCreate;
    fn_AR_LDRT_TX_PIPELINE_Destroy    PipelineDestroy;
    fn_AR_LDRT_TX_PIPELINE_SetRcParam PipelineSetRcParam;  /* void — no return value */
    fn_AR_LDRT_TX_PIPELINE_Start      PipelineStart;
    fn_AR_LDRT_TX_PIPELINE_Stop       PipelineStop;
    fn_AR_LDRT_TX_PIPELINE_IdrEnable  PipelineIdrEnable;
    fn_AR_LDRT_TX_PIPELINE_RoiEnable  PipelineRoiEnable;
    fn_AR_LDRT_TX_VENC_GetFd          VencGetFd;
    fn_AR_LDRT_TX_VENC_GetStream      VencGetStream;
    fn_AR_LDRT_TX_VENC_ReleaseStream  VencReleaseStream;
    fn_AR_LDRT_TX_VENC_RequestIdr     VencRequestIdr;
    fn_AR_LDRT_TX_VencGetThreadStop   VencGetThreadStop;
    fn_AR_LDRT_TX_VencSendThreadStop  VencSendThreadStop;
} LDRT_API_S;

int  pipeline_load(LDRT_API_S *api, const char *lib_path);
void pipeline_unload(LDRT_API_S *api);
