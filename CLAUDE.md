# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Open-source reimplementation of `arlink_fpv` for the **Artosyn AR9301 SoC** (OpenIPC FPV hardware). Captures H.265 from a camera sensor, encodes via the closed-source vendor library `libldrt_pipeline.so`, and streams RTP/UDP H.265 (RFC 7798) to a GCS.

Target binary name: `arlink_stream`. Runs on-device (AArch64 Linux).

## Build

```sh
make          # downloads Linaro GCC 7.5 toolchain on first run, then cross-compiles
make clean    # remove objects and binary
make distclean  # also removes the downloaded toolchain
```

Cross-compiler: `aarch64-linux-gnu-gcc` (Linaro 7.5). Output is a stripped AArch64 ELF.

The binary must be linked with `-Wl,-E` (re-exports all symbols) so that `libldrt_pipeline.so` can resolve symbols from the hosting process at runtime.

## GhidraMCP_sdio

For revseing the original binary, there is a GhidraMCP_sdio MCP server connect and the original arlink_fpv binary is loaded.

## Serial MCP interface

There is a serial-mcp-server connect to run command on the serial console of the device
Port: /dev/ttyUSB0
baudrate: 1152000

The serial console is a auto started via inttab on the device `::askfirst:-/bin/sh`

ssh access does not work, you need to start a telnetd to have another console connection using telnet

`telnetd -p 1337 -l /bin/sh`

The network is auto configured.
Device: 192.168.3.99
Local: 192.168.3.101

To connect using telnet use `telnet 192.168.3.99 1337`

Transfering files can be done by the `dump_file` binary.
Example: dump_file receive 192.168.3.101 /local/path/to/file /usrdata
Alternatively run a local http server and use wget on the device

When running out binary make sure to run it as `arlink_fpv`otherwise the watchdog might restart the board.

If the device is stuck you can try to restart it useing `printf '\xAA\x05\x00\x1D\x00\x00\xCC\xEB\xAA\x00' | socat - /dev/ttyACM0,b1152000,raw,echo=0`
This will reboot the device into uboot it will wait whit this message `get_arusb size: 0xb700000 0x10000000@0x20000000`
You need to hit strg+c quickly twice to get to the uboot promt, then run `reset` do reboot to normal operation.
When you hit hit Strg+C once it will auto countdown then reset and do a normal boot.
This restart functionality is provided by the original `ota_upgrade` binary.

Writeable locations /usrdata or /tmp

## Persistence

Cou can add custom startup useing `/usrdata/run_dbg.sh` and `cp /usr/usrdata/buildtime /usrdata/buildtime`
This will make the original `/usr/usrdata/run.sh` to run `/usrdata/run_dbg.sh` during boot.
Make sure you run `/etc/usb_gadget_configfs.sh rndis+uart 0 dwc2_9311 0x1d6b 0x0101` to bring up networking.
Also make sure it's executeable.

## Architecture

### Pipeline (the hard part)

`libldrt_pipeline.so` is a proprietary vendor library that wraps the Artosyn MPI subsystem (VIN → ISP → VENC chain). Our code loads it via `dlopen` with all its dependencies pre-loaded into the global symbol namespace first (see `pipeline.c`).

**Critical constraint: never call `VencGetThreadStop()`.**
That function calls `AR_MPI_VENC_StopRecvFrame()` internally, which stops the VENC encoder from accepting VIN frames. The VIN thread then fails every 200 ms (`Ldrt Get Vi Frame Chn [0] error! s32Ret=0xffffffff`), triggering a full pipeline reset loop and making `VencGetFd()` return -1 forever.

The correct approach is **callback-driven delivery**: `AR_LDRT_TX_ArDeviceThread` (internal to the library) reads from ring buffers and calls our `dev_video_send` callback automatically after `PipelineStart()`. We do not poll VENC directly.

### Startup sequence (main.c)

1. `pipeline_load()` — dlopen deps (RTLD_GLOBAL), then dlopen pipeline lib
2. `SysInit()` — allocates VB media pools
3. `sensor_probe_vin()` — reads `/usr/usrdata/sensor_board_cfg.cjson` via `AR_MPI_VIN_ProbeDev`, resolves sensor `.so` via `AR_MPI_VIN_GetSensorObj`
4. `TxInit()` — sets up VIN/VENC/VO, registers device callbacks
5. `PipelineCreate()` — low-delay mode
6. `PipelineSetRcParam()` — rate control
7. `PipelineStart(0)` — starts ArDeviceThread, VencGetThread, VinGetFrameThread
8. Main loop: `while (g_running) sleep(1)` — frames delivered via callback

### Device callbacks (registered in `AR_LDRT_PIPE_TX_DEVICE_PARAMS_S`)

| Callback | Purpose | Required return |
|---|---|---|
| `dev_video_send(dev_id, data, len, flags)` | called by ArDeviceThread with one ring buffer entry | must return `> 0` (bytes consumed); `0` → infinite retry; `-2` → thread exits |
| `dev_query_stream(dev_id)` | level control: how many bytes has the device consumed | return monotonically increasing `total_bytes` to prevent frame skipping |
| `dev_query_min_pack(dev_id)` | minimum packet size hint | return e.g. 1024 |
| `dev_rst_stream(dev_id)` | reset notification | return 0 |

### Ring buffer entry format (passed to `dev_video_send` as `data`)

```
+0x00  uint32_t  magic = 0x12345678
+0x04  uint32_t  frame_data_len
+0x08  uint32_t  channel
+0x0c  uint32_t  idr_flag
+0x34  uint8_t   nalu_bytes[frame_data_len]
total entry size = frame_data_len + 0x38
```

NALU data at `data+0x34` is Annex-B (start codes present). `len` passed to the callback equals `frame_data_len + 0x38`.

### cert check

`libldrt_pipeline.so` calls back into the hosting binary for cert/device verification. We export `check_cert()` returning 0 unconditionally. `u32MacAddr[0]` must be non-zero or the library rejects the device before even calling cert check.

### Source files

- `main.c` — entry point, config parsing, sensor probe, pipeline init, device callbacks, RTP+UDP init
- `pipeline.c` / `pipeline.h` — dlopen loading of all vendor deps + `libldrt_pipeline.so`; `LDRT_API_S` function pointer table; all struct layouts reverse-engineered from DWARF + Ghidra
- `rtp_h265.c` / `rtp_h265.h` — RFC 7798 packetizer (single NAL + FU fragmentation)
- `udp_sender.c` / `udp_sender.h` — plain UDP socket wrapper

### Ghidra project

The binary being reverse-engineered is `arlink_fpv` (the original closed-source binary). A Ghidra MCP server (`mcp__GhidraMCP_sdio__*`) is available in this session for live analysis. Key addresses in `libldrt_pipeline.so`:

| Symbol | Address |
|---|---|
| `AR_LDRT_TX_ArDeviceThread` | `0x0012fd48` |
| `AR_LDRT_TX_VencGetStreamThread` | `0x0012e4f0` |
| `AR_LDRT_TX_VencGetThreadStop` | `0x00132290` ← calls `AR_MPI_VENC_StopRecvFrame` |
| `AR_LDRT_TX_VencThreadStart` | `0x00132a00` |
| `AR_LDRT_TX_PIPELINE_Start` | `0x001337e0` |
| `AR_LDRT_TX_Init` | `0x00137ad8` |
| `FUN_001361c8` (VinGetFrameThread) | `0x001361c8` |

Handle field offsets (from `AR_LDRT_TX_Init` analysis):
- `handle+0x60` — copy of module params
- `handle+0x278` — device params
- `handle+0x298` — `QueryStreamSizeCb`
- `handle+0x2a0` — `VideoSendCb`
- `handle+0xbb0` — ring buffer base; per-device stride `0xb8`


## Reference logging from the original software

### Inital start of the arlink_fpv binary

/ # kill -9 293 ; arlink_fpv -m 2 -t 1
arlink_fpv compiled at: Mar 25 2026 18:20:42
[Sky]
mallopt 0x400000 failed.
cmd_exitcode is NULL!!!
pid 379's OOM score adjust value changed from 0 to -300
Sky Begain Start...
audio enable 1 dev id 1, mode 0
MID Config Current Board Is 2...
[APP ERR] [ platform/cfg/src/ar_lowdelay_cfg_common.c, Line:78 ]  open file /usrdata/lowdelay/lowdelay_cfg/cfg_ability_common.json failed

vb id[0], l=blk size[3350528], blk cnt[5]
vb id[1], l=blk size[110592], blk cnt[10]
vb id[2], l=blk size[180224], blk cnt[5]
vb id[3], l=blk size[3350528], blk cnt[5]
ar9311_clk_probe 2767 start!
artosyn,9311,clk_ctrl clk probe success!
ar_axi_dma_signal_semaphore_init success!
 [src/vtx.c setOpenedFd 605] uart_name: /dev/ttyS2
[17524][0379][0x7f80133720][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_SYS_Init : 3334 AR_LDRT_TX_SYS_Init Init Success

[17525][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_EventMessageProcessThread : 1445 AR_FSM_TX_EventMessageProcessThread start
[17525][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_common.c: AR_COMMON_TIMER_Init : 215 ******** timer thread init ********

[17525][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_common.c: AR_COMMON_TIMER_AddTask : 290 timer 0x430b00 add success.

[17525][0379][0x7f69d991e0][APP_LOWDELAY][WARN] ar_lowdelay_common.c: timer_thread : 172 ld_timer thread start

[17528][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_rc.c: AR_LOWDELAY_TX_RC_Init : 178 vtxfc_init VTX_MSP_V1_PROTOCOL success. ><><><><><><><><><><><><><>

[17528][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_rc.c: AR_LOWDELAY_TX_RC_Init : 191  AR_LOWDELAY_TX_RC_Init Init Success.

[17528][0379][0x7f69dba1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_CommonMessageProcessThread : 1591 AR_FSM_TX_CommonMessageProcessThread start
[17529][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_Init : 1946  Tx Fsm Init Success .

[17529][0379][0x7f80133720][APP_LOWDELAY][WARN] board_common.c: boardCommAdcInit : 274 ar_hal_adc_enable success.

[17530][0379][0x7f80133720][APP_LOWDELAY][WARN] fpv_air.c: fpvAirInit : 206 customer RBOX sysctrl init success

[17530][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_ResetStandyMode : 1003  Reset to normal mode.

[17531][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_common.c: AR_COMMON_TIMER_AddTask : 290 timer 0x4786d8 add success.

[17531][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_Init : 1376  AR_LOWDELAY_TX_SYSCTRL_Init Init Success .

[17535][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_Init : 2172 bb_dev_getlist num 1 success.
[17537][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_SetCb : 1395 [17.537213] AR_AR8030_TX_SetCb BB_EVENT_LINK_STATE callback start!

[17538][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_SetCb : 1402 [17.537985] AR_AR8030_TX_SetCb BB_EVENT_LINK_STATE callback success!

[17538][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_SetCb : 1395 [17.538035] AR_AR8030_TX_SetCb BB_EVENT_MCS_CHANGE callback start!

netlink socket 37 bind to port 10001
[17538][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_SetCb : 1402 [17.538915] AR_AR8030_TX_SetCb BB_EVENT_MCS_CHANGE callback success!

[17538][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_SetCb : 1395 [17.538959] AR_AR8030_TX_SetCb BB_EVENT_MCS_CHANGE_END callback start!

[17539][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_SetCb : 1402 [17.539582] AR_AR8030_TX_SetCb BB_EVENT_MCS_CHANGE_END callback success!

[17539][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_common.c: direct_socket_open : 687 socket 35 bind to port 1000, peer 10.0.0.1

[17540][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_common.c: add_socket_info : 481 add_socket_info:socket 35 added to 0

[17540][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_common.c: add_socket_info : 481 add_socket_info:socket 36 added to 1

[17540][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_Init : 2288 BB Init Status BBMsgSock[1:35], BBRcSock[0:-1], BBAudioSock[0:-1], BBVideoSock[1:36], BBVideoNlSock[1:37].
[17541][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_GetInitStatus : 1211 [8030 INFO] Init BB role [dev], mode[single user], sync mode[0], sync master[0x1], cfg sbmp[0x1], rt sbmp[0x]

[17542][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_GetApMac : 1141 [8030 TX INFO ]Tx dev get ap mac 0x646619be

[17543][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_GetInitStatus : 1261 [8030 INFO] init cur mcs [23], pre mcs [23]

[17543][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_PwrCtlEnable : 174  AR_LOWDELAY_SKY_AR8030_PwrCtlEnable success.

[17543][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_Init : 2302 bb init success.
[17543][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_transmedium.c: AR_LOWDELAY_TX_TRANSMEDIUM_Init : 892 Tx Init AR803x Success, Init BufferSize 1048576.

[17543][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_transmedium.c: AR_LOWDELAY_TX_TRANSMEDIUM_Init : 912 Success.

[17543][0379][0x7f633901e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_transmedium.c: AR_LOWDELAY_TX_TRANSMEDIUM_MsgSendThread : 386 Thread TxCommMsgSend0 start.
[17543][0379][0x7f62b8f1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_transmedium.c: AR_LOWDELAY_TX_TRANSMEDIUM_MsgRcvThread : 338 Start thread TxCommMsgRecv0_0
get board info /usr/usrdata/sensor_board_cfg.cjson
nt99235_cmos_power_on  power_gpio=104 reset_gpio=107
Found 2 devices:
get board info /usr/usrdata/sensor_board_cfg.cjson mipi0_interface
i2c_index: 0
rst: 0, 6, 13
power: 0, 6, 10
mclk_id: 0
get board info /usr/usrdata/sensor_board_cfg.cjson mipi0_interface
i2c_index: 0
rst: 0, 6, 13
power: 0, 6, 10
mclk_id: 0
probe all:
open the driver sns:nt99235 obj:stSnsnt99235Obj lib:libsns_nt99235.so
 ==== nt99235_cmos_power_on ====
nt99235_cmos_power_off  power_gpio=104 reset_gpio=107
nt99235 probe sucess data 0x0300 is : 0
sc231ai_cmos_power_on  power_gpio=104 reset_gpio=107
open the driver sns:sc231ai obj:stSnssc231aiObj lib:libsns_sc231ai.so
I2C master read fail, ret = -1 slv_addr:30
I2C master read fail, ret = -1 slv_addr:30
sc231ai_cmos_power_off  power_gpio=104 reset_gpio=107
sc231ai probe data 0x3107 is : 0 0x3108 is : 0
 [platform/modules/src/ar_lowdelay_tx_vin.c AR_LOWDELAY_TX_VIN_SensorProbe 183] nt99235 probe success !!!!+++++++++


dev_cnt:1
        sensor:nt99235 -> driver:nt99235
sopen the driver sns:nt99235 obj:stSnsnt99235Obj lib:libsns_nt99235.so
[17617][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_GetAllSnsObj : 542 visible sns enable 1, ir sns enable 0, inter vin mode 0.

[17627][0379][0x7f80133720][USR_LDRT][WARN] ar_ldrt_cert_verify.c: AR_LDRT_CERT_CheckCertification : 193 AUTH_VERIFY_CERTIFICATION Success. (ChipId:176e515131532625)-(BbId:e51153e2)-(CertificationSize:512)
[17627][0379][0x7f80133720][USR_LDRT][WARN] ar_ldrt_cert_verify.c: AR_LDRT_CERT_CheckAuthKey : 106 AUTH_VERIFY_KEY: [ /usr/usrdata/public.pem ] ReadSize: [ 451 ] Success.
[17628][0379][0x7f80133720][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_EventInit : 3071 AR_LDRT_TX_EventInit Start.
[17628][0379][0x7f623711e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_EventThread : 2986 LDRT_EVENT Start.
[17633][0379][0x7f80133720][SYS_CORE][VI_KEY] server.c: start_camera_server : 3685 load vin driver start
[17673][0379][0x7f80133720][SYS_CORE][VI_KEY] server.c: start_camera_server : 3944 load vin driver end max_sensor_count=5
[17680][0379][0x7f80133720][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_Init : 1016 ok.

[17681][0379][0x7f80133720][CORE_VCODEC][WARN] ar_hal_vcodec_ctrl_impl.c: ar_hal_venc_ctrl_set_mode_param : 341 pVencModParam.flags=0
[17681][0379][0x7f80133720][CORE_VCODEC][WARN] ar_hal_vcodec_ctrl_impl.c: ar_hal_venc_ctrl_set_mode_param : 341 pVencModParam.flags=0x8
[17681][0379][0x7f80133720][CORE_VCODEC][WARN] ar_hal_vcodec_ctrl_impl.c: ar_hal_venc_ctrl_set_mode_param : 349 pVencModParam.flags=0x8
[17681][0379][0x7f80133720][CORE_VCODEC][WARN] ar_hal_vcodec_ctrl_impl.c: ar_hal_vcodec_ctrl_module_bootup : 83 h26x power on
[17682][0379][0x7f80133720][CORE_VCODEC][WARN] ar_hal_vcodec_ctrl_impl.c: ar_hal_vcodec_ctrl_module_bootup : 86 stH26xParam.u64Flags=0x8
[17682][0379][0x7f80133720][CORE_VCODEC][WARN] ar_video_h26x_util.c: ar_video_h26x_boot_vpu : 174 use low mem fw
[17703][0379][0x7f80133720][CORE_VCODEC][WARN] ar_video_h26x_util.c: ar_video_h26x_boot_vpu : 189 get ucode 0x188d3f0 size 999360, 20898 us

[17709][0379][0x7f80133720][CORE_VCODEC][WARN] ar_video_h26x_util.c: ar_video_h26x_boot_vpu : 201 Firmware : productID:2, version:0, revision:330638
[17710][0379][0x7f80133720][USR_LDRT][WARN] ar_ldrt_tx_venc.c: AR_LDRT_TX_VENC_SetFrequency : 186 Set Venc Frequency Core_Clk : [333M], Bpu_Clk : [333M].

[17711][0379][0x7f80133720][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_Init : 3254 AR_LDRT_TX_Init Init SUCCESS.
[17711][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_HDMI_CONNECT] ApId [0] DevId [0] Cnt [1]
[17711][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_SnsPlugThreadCreate : 797 [ w h fps ] = [ 1920 1080 60 ]

[17711][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_LOWDELAY_TX_LdrtInit : 399 AR_LDRT_TX_Init Success.
[17711][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_SYS_TX_BasePtsInit : 41 Wireless LinkDown

[17711][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_SYS_TX_BasePtsInit : 94 Get 8030 u64DurTime 0. get PTS 0. 8030 PTS 0

[17712][0379][0x7f80133720][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_LOWDELAY_SkySysInit : 499 Init Success.


The streaming does not start emmediately.


### Connect the VRX

When the VRX is connected it goes further:


 bb link state link_down in sync process. role SKY
[57217][0379][0x7f653941e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_transmedium.c: AR_LOWDELAY_TX_TRANSMEDIUM_SetLinkStatus : 549 Recv Medium [ AR8030 ] Linkstatus [ LinkDown ], Cur Medium [ INVALID ] eLinkStatus [ LinkDown ]

[57217][0379][0x7f653941e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_GetLinkStatus : 1309 [8030 INFO] slot = 0 , cur = 1 , prev = 0

 bb link state link_up in sync process. role SKY
[57245][0379][0x7f653941e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_transmedium.c: AR_LOWDELAY_TX_TRANSMEDIUM_SetLinkStatus : 549 Recv Medium [ AR8030 ] Linkstatus [ LinkUp ], Cur Medium [ AR8030 ] eLinkStatus [ LinkDown ]

[57245][0379][0x7f653941e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_8030.c: AR_LOWDELAY_SKY_AR8030_GetLinkStatus : 1309 [8030 INFO] slot = 0 , cur = 2 , prev = 1

[57245][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_LINK_UP] ApId [0] DevId [0] Cnt [1]
[57245][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] customer_haming.c: customerDefaultEventNotify : 38  customerDefaultEventNotify event 1

[57336][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_SYS_TX_BasePtsInit : 94 Get 8030 u64DurTime 682. get PTS 0. 8030 PTS 1169000

[57337][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_ProcessLinkUp : 734 FPV mode change fsm state to AR_FSM_TX_STATE_READY later while enable pipeline.
[57337][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_MCS_CHANGE] ApId [0] DevId [0] Cnt [1]
[57339][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_ProcessMcsChange : 1092 cur ap[0], link status [1], media status [0], mcs change from [-1] to [10]
[57339][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_MCS_CHANGE_FINISHED] ApId [0] DevId [0] Cnt [1]
[57341][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_ProcessMcsChangeFinished : 1145 cur ap [0], link status [1], media status [0], mcs change finished from [-1] to [10]
[57341][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_MCS_CHANGE] ApId [0] DevId [0] Cnt [2]
[57343][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_ProcessMcsChange : 1092 cur ap[0], link status [1], media status [0], mcs change from [-1] to [10]
[57343][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_MCS_CHANGE_FINISHED] ApId [0] DevId [0] Cnt [2]
[57345][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_ProcessMcsChangeFinished : 1145 cur ap [0], link status [1], media status [0], mcs change finished from [-1] to [10]
[57524][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_MCS_CHANGE] ApId [0] DevId [0] Cnt [3]
[57526][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_ProcessMcsChange : 1092 cur ap[0], link status [1], media status [0], mcs change from [-1] to [10]
[57527][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_MCS_CHANGE_FINISHED] ApId [0] DevId [0] Cnt [3]
[57528][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_ProcessMcsChangeFinished : 1145 cur ap [0], link status [1], media status [0], mcs change finished from [-1] to [10]
[57713][0379][0x7f5a3c71e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_LOWDELAY_TX_SYS_SyncTime : 187 Sync Success PTS 1562000, Success Cnt 1
[60142][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_SYSTEM_SYNC_SYS_CONFIG] ApId [0] DevId [0] Cnt [1]
[60142][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_SnsSwitch : 2540 visible sns 1, ir sns enable 0, sns switch to [ VISIBLE SNS ].
[60142][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1306 Zoom Ratio 1.000000, Enable 0 force set to 0.

[60142][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1321 old u32CameraType 0 new u8SnsType 0 RstStatus 0
[60142][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_SetLdCfg : 475 AR_LOWDELAY_TX_SYSCTRL_SetLdCfg need not set 8030 power when flight lock!

[60142][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_UpdateStandyMode : 979  Update mode 0 to mode 1 fps 15 period 6 power 5.

[60142][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_media_ext.c: AR_LOWDELAY_TX_REC_UpdateParams : 1262  update cfg_params_ready 1 s8RecChnId 4 u16RecWidth 1920 u16RecHeight 1080 u8AutoRecord 0 u8RecModeCyc 0 u16

[60143][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_RealTimePipelineInit : 243 Tx Init RealTime PipeLine
[60144][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_cert_verify.c: AR_LDRT_CERT_VERIFY_CheckCert : 209 Auth Cert Success.
[60144][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_Enable : 286 Vin Get Obj[ 0x7f6238edc8  ], Sns Mode[ 3 ], FrontType[ AR_LDRT_CAMERA ] i2c[ 0 ]
[60144][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_Enable : 302 MipiFrehz config by dirver, origin = 112000000Hz current = 112500000.
is_big_pic:0
[60146][0379][0x7f6a5bb1e0][VI_HAL][ERROR] mpi_vin.c: AR_MPI_VI_CreatePipe : 3176 !!!!!!!!!! Note:your app cfg the isp read and vif write  as no compress,this mode isp read and vif write will cost more ddr band width, we su!
[60146][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_Enable : 560 Current Scale Is 16:9, Real Video Width 1920 Height 1080

 cmos_fps_set 1445
[60146][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_Enable : 581 Lowdelay Chn 0 Line 559
[60146][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_Enable : 581 Lowdelay Chn 1 Line 1079
[60146][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_RegiterSnsCallback : 36 Sns Register Callback, SnsObj[ 0x7f6238edc8 ] I2C-Dev[ 0 ]

[60147][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_open_camera : 930 camera_id=0
AR_MPI_VI_GetChnBufferSize:buffer size =4155392 pipe 0 ch 2 w 1920 h 1080 y_buffer_width 3840 format 19
[60151][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_creat_stream_man_link : 2048 camera_id=0
[60153][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60153][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60153][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60156][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60156][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60157][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_creat_stream_man_link : 2167 exit stream_id=0
[60159][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_start_stream : 1510 camera_id=0 stream_id=0
nt99235_cmos_power_on  power_gpio=104 reset_gpio=107
[60170][0379][0x7f567da1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_set_ctl : 4248 power on 0
 ==== nt99235_cmos_power_on ====
[60192][0379][0x7f567da1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_set_ctl : 4250 power on end 0
u8DevNum=0 /dev/i2c-0
 ====   nt99235_linear_1920x1080_60fps_2lane_init  ====
[60216][0379][0x7f567da1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_init : 1448 sensor init done
[60217][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] mipi_rx_9311.c: mipi_rx_power_up : 67 enable the mipi cfg and pcs clock, and set pcs to clk=112500000hz
[60218][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] mipi_rx.c: one_mipi_init : 1200 mipi 0 init done
[60219][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_set_ctl : 4346 vin_dev_0 stream on done
[60219][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_start_stream : 1542 exit
[60254][0379][0x7f5faa71e0][SYS_CORE][VI_KEY] vif_mid.c: vif_bpintr_process : 1146 stable ,ento ddr view 0
[60303][0379][0x7f5faa71e0][SYS_CORE][VI_KEY] vif_mid.c: vif_bpintr_process : 1195 cam_id[0], first frame done 1
[60320][0379][0x7f5faa71e0][SYS_CORE][VI_KEY] vif_mid.c: vif_bpintr_process : 1195 cam_id[0], first frame done 2
[60320][0379][0x7f6a5bb1e0][VI_HAL][ERROR] mpi_vin.c: AR_MPI_VI_GetHightPricisionPipeFPS : 1924 !!!!!!  The api is Dangerous,Unless you know what you're doing and have a deep understanding of this API, and you have to use t!
Thread is running on CPU 0
the fps is 60.000061 16666650
============ nt99235 exit
nt99235_cmos_power_off  power_gpio=104 reset_gpio=107
[60509][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_Enable : 714 AR_MPI_VI_GetHightPricisionPipeFPS Get Fps[60.000061]...
[60511][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_stop_stream : 1548 camera_id=0 stream_id=0
[60534][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_stop_stream : 1578 camera_id=0 stream_id=0 exit
[60536][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_creat_stream_man_link : 2048 camera_id=0
[60537][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60537][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60537][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60538][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60538][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60538][0379][0x7f6a5bb1e0][VI_CORE][ERROR] cam_sensor.c: sensor_pick_res : 3858 !!!note:: too small hblank vblank (276 41) , we suggest larger than (200 45) if you encounter some abnormal
[60539][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_creat_stream_man_link : 2167 exit stream_id=0
nt99235_cmos_power_on  power_gpio=104 reset_gpio=107
[60540][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_start_stream : 1510 camera_id=0 stream_id=0
[60543][0379][0x7f55edb1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_set_ctl : 4248 power on 0
 ==== nt99235_cmos_power_on ====
[60564][0379][0x7f55edb1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_set_ctl : 4250 power on end 0
 ====   nt99235_linear_1920x1080_60fps_2lane_init  ====
[60588][0379][0x7f55edb1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_init : 1448 sensor init done
[60588][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] mipi_rx_9311.c: mipi_rx_power_up : 67 enable the mipi cfg and pcs clock, and set pcs to clk=112500000hz
[60588][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] mipi_rx.c: one_mipi_init : 1200 mipi 0 init done
[60589][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_sensor.c: sensor_set_ctl : 4346 vin_dev_0 stream on done
[60589][0379][0x7f6a5bb1e0][SYS_CORE][VI_KEY] cam_api.c: ar_camera_start_stream : 1542 exit
[60589][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_venc.c: AR_LDRT_TX_VENC_Enable : 559 Venc Codec W[1920] h[1080] Fps[60], Codec Type[1], Rcmode[0], Gop[0], Bps[2000], StatTime[0], TotChn[2]
[60594][0379][0x7f6a5bb1e0][VENC_MPP][WARN] mpi_venc.c: venc_mpi_attr_to_hal_attr : 1921 VeChn[0] do calc subsample
[60599][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: checkIfNeedEnableLargeVLCBuffer : 1334 low mem fw
[60602][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_set_base_attr : 1609 Chn[0->0x2a1dd70] cmdqueue enabled need enable bParallelismOn.

[60609][0379][0x7f6a5bb1e0][VENC_MPP][WARN] mpi_venc.c: venc_mpi_attr_to_hal_attr : 1921 VeChn[1] do calc subsample
[60612][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: checkIfNeedEnableLargeVLCBuffer : 1334 low mem fw
[60613][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_set_base_attr : 1609 Chn[1->0x2a423b0] cmdqueue enabled need enable bParallelismOn.

[60613][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_Set_LevelControlThreshold : 535 AR_RT_TX_WaterLevel_init AlertLevel is 43520 threshold 5 splitNum 2 bitrate 2000 fps 60 u32SplitSize 2133 u32SkbPackSize 4.

[60613][0379][0x7f502f41e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VinGetFrameThread : 960 Tx Vin Start AR_LDRT_TX_VinGetFrameThread, ViChn[0], Pipe[0].
[60614][0379][0x7f502b21e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VencSendFrameThread : 1415 AR_LDRT_TX_VencSendFrameThread start

[60614][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_PIPELINE_Create : 2563 LDRT TX Create Success 1920x1080@60.

[60624][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_PIPELINE_RoiEnable : 2689 ROI STATUS 0 width 0 height 0

[60625][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 318 u16ExposureManual 0 u16ExposureTime 5

[60625][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 324 u16Saturation 70

[60625][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 330 u32Sharpness 55

[60630][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 349 u16WhiteBalanceManual 0 u16WhiteBalance 0

[60631][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 361 u16Rotation 1

[60632][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 367 u16NR3DEn 1 u32De3dStrength 50

[60632][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 377 u16NR2DEn 1 strength 50

[60632][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 386 u16ISO 100

[60632][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_CameraInit : 404 u16Banding 0

[60633][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_PARAMS_REQUEST] ApId [0] DevId [0] Cnt [1]
[60633][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_GetHightPricisionPipeFPS : 219 Vin Get HightPricisionFps is 60.000061
[60633][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_SYSTEM_SYNC_SYS_CONFIG] ApId [0] DevId [0] Cnt [2]
[60633][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_SnsSwitch : 2540 visible sns 1, ir sns enable 0, sns switch to [ VISIBLE SNS ].
[60634][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1306 Zoom Ratio 1.000000, Enable 0 force set to 0.

[60634][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1321 old u32CameraType 0 new u8SnsType 0 RstStatus 0
[60634][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_SetLdCfg : 475 AR_LOWDELAY_TX_SYSCTRL_SetLdCfg need not set 8030 power when flight lock!

[60634][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_UpdateStandyMode : 979  Update mode 0 to mode 1 fps 15 period 6 power 5.

[60634][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_media_ext.c: AR_LOWDELAY_TX_REC_UpdateParams : 1262  update cfg_params_ready 1 s8RecChnId 4 u16RecWidth 1920 u16RecHeight 1080 u8AutoRecord 0 u8RecModeCyc 0 u16

[60651][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_PIPELINE_RoiEnable : 2689 ROI STATUS 0 width 0 height 0

[60651][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_PARAMS_REQUEST] ApId [0] DevId [0] Cnt [2]
[60651][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_GetHightPricisionPipeFPS : 219 Vin Get HightPricisionFps is 60.000061
[60651][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_SYSTEM_SYNC_SYS_CONFIG] ApId [0] DevId [0] Cnt [3]
[60651][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_SnsSwitch : 2540 visible sns 1, ir sns enable 0, sns switch to [ VISIBLE SNS ].
[60651][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1306 Zoom Ratio 1.000000, Enable 0 force set to 0.

[60651][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1321 old u32CameraType 0 new u8SnsType 0 RstStatus 0
[60651][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_SetLdCfg : 475 AR_LOWDELAY_TX_SYSCTRL_SetLdCfg need not set 8030 power when flight lock!

[60652][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_UpdateStandyMode : 979  Update mode 0 to mode 1 fps 15 period 6 power 5.

[60652][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_media_ext.c: AR_LOWDELAY_TX_REC_UpdateParams : 1262  update cfg_params_ready 1 s8RecChnId 4 u16RecWidth 1920 u16RecHeight 1080 u8AutoRecord 0 u8RecModeCyc 0 u16

[60664][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_PIPELINE_RoiEnable : 2689 ROI STATUS 0 width 0 height 0

[60664][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_PARAMS_REQUEST] ApId [0] DevId [0] Cnt [3]
[60664][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_GetHightPricisionPipeFPS : 219 Vin Get HightPricisionFps is 60.000061
[61064][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_SYSTEM_SYNC_SYS_CONFIG] ApId [0] DevId [0] Cnt [4]
[61064][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_SnsSwitch : 2540 visible sns 1, ir sns enable 0, sns switch to [ VISIBLE SNS ].
[61064][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1306 Zoom Ratio 1.000000, Enable 0 force set to 0.

[61064][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1321 old u32CameraType 0 new u8SnsType 0 RstStatus 0
[61064][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_SetLdCfg : 475 AR_LOWDELAY_TX_SYSCTRL_SetLdCfg need not set 8030 power when flight lock!

[61064][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_UpdateStandyMode : 979  Update mode 0 to mode 1 fps 15 period 6 power 5.

[61064][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_media_ext.c: AR_LOWDELAY_TX_REC_UpdateParams : 1262  update cfg_params_ready 1 s8RecChnId 4 u16RecWidth 1920 u16RecHeight 1080 u8AutoRecord 0 u8RecModeCyc 0 u16

[61076][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_PIPELINE_RoiEnable : 2689 ROI STATUS 0 width 0 height 0

[61078][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_PARAMS_REQUEST] ApId [0] DevId [0] Cnt [4]
[61078][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_GetHightPricisionPipeFPS : 219 Vin Get HightPricisionFps is 60.000061
[61266][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_SYSTEM_SYNC_SYS_CONFIG] ApId [0] DevId [0] Cnt [5]
[61266][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_SnsSwitch : 2540 visible sns 1, ir sns enable 0, sns switch to [ VISIBLE SNS ].
[61266][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1306 Zoom Ratio 1.000000, Enable 0 force set to 0.

[61266][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1321 old u32CameraType 0 new u8SnsType 0 RstStatus 0
[61266][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_SetLdCfg : 475 AR_LOWDELAY_TX_SYSCTRL_SetLdCfg need not set 8030 power when flight lock!

[61267][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_UpdateStandyMode : 979  Update mode 0 to mode 1 fps 15 period 6 power 5.

[61267][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_media_ext.c: AR_LOWDELAY_TX_REC_UpdateParams : 1262  update cfg_params_ready 1 s8RecChnId 4 u16RecWidth 1920 u16RecHeight 1080 u8AutoRecord 0 u8RecModeCyc 0 u16

[61278][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_PIPELINE_RoiEnable : 2689 ROI STATUS 0 width 0 height 0

[61581][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_SYSTEM_SYNC_SYS_CONFIG] ApId [0] DevId [0] Cnt [6]
[61581][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_vin.c: AR_LOWDELAY_TX_VIN_SnsSwitch : 2540 visible sns 1, ir sns enable 0, sns switch to [ VISIBLE SNS ].
[61581][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1306 Zoom Ratio 1.000000, Enable 0 force set to 0.

[61581][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_FpvSyncMediaParams : 1321 old u32CameraType 0 new u8SnsType 0 RstStatus 0
[61581][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_SetLdCfg : 475 AR_LOWDELAY_TX_SYSCTRL_SetLdCfg need not set 8030 power when flight lock!

[61581][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_UpdateStandyMode : 979  Update mode 0 to mode 1 fps 15 period 6 power 5.

[61581][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_media_ext.c: AR_LOWDELAY_TX_REC_UpdateParams : 1262  update cfg_params_ready 1 s8RecChnId 4 u16RecWidth 1920 u16RecHeight 1080 u8AutoRecord 0 u8RecModeCyc 0 u16

[61592][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_PIPELINE_RoiEnable : 2689 ROI STATUS 0 width 0 height 0

[61595][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_PARAMS_REQUEST] ApId [0] DevId [0] Cnt [5]
[61595][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx_vin.c: AR_LDRT_TX_VIN_GetHightPricisionPipeFPS : 219 Vin Get HightPricisionFps is 60.000061
[61713][0379][0x7f6a5bb1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_fsm.c: AR_FSM_TX_DebugPrint : 40 Cur MsgId [AR_FSM_TX_MESSAGE_IDR_REQUEST] ApId [0] DevId [0] Cnt [1]
[61721][0379][0x7f5b4c71e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_allocate_framebuffer : 2963 Chn[0]: return vlc buffer size 1583552

[61725][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VencGetThreadStart : 1856 ==Start Venc Chn[0] Recv Frame

[61726][0379][0x7f5b4c71e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_allocate_framebuffer : 2963 Chn[1]: return vlc buffer size 1583552

[61727][0379][0x7f6a5bb1e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VencGetThreadStart : 1856 ==Start Venc Chn[1] Recv Frame

[61728][0379][0x7f502f41e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_LevelControl : 489 ApId 0 FrameId 0, LeaveStreamIn8030 is 0
[61728][0379][0x7f502f41e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_Set_LevelControlThreshold : 535 AR_RT_TX_WaterLevel_init AlertLevel is 296960 threshold 5 splitNum 2 bitrate 14630 fps 60 u32SplitSize 15605 u32SkbPackSiz.

[61728][0379][0x7f502f41e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VinGetFrameThread : 1076 Tx Venc Bitrate Change to [14630]kbps 60fps.

[61733][0379][0x7f502f41e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VinGetFrameThread : 1123 Cur Frame Id 0 CurPTS 5570369 - LastPTS 0 larger than 100ms
[61733][0379][0x7f502b21e0][VENC_MPP][WARN] mpi_venc.c: venc_mpi_attr_to_hal_attr : 1921 VeChn[0] do calc subsample
[61734][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_set_dynamic_param : 2414 Chn[0][0x2a1dd70] codec_type:1 use yuv size as key frame size
[61734][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_set_dynamic_param : 2423 Chn[0][0x2a1dd70] codec_type:1 use yuv/2 size as non key frame size.

[61740][0379][0x7f502b21e0][VENC_MPP][WARN] mpi_venc.c: venc_mpi_attr_to_hal_attr : 1921 VeChn[1] do calc subsample
[61741][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_set_dynamic_param : 2414 Chn[1][0x2a423b0] codec_type:1 use yuv size as key frame size
[61741][0379][0x7f5bcc81e0][CORE_VCODEC][WARN] ar_video_h26x_enc_exe.c: ar_video_h26x_enc_exe_set_dynamic_param : 2423 Chn[1][0x2a423b0] codec_type:1 use yuv/2 size as non key frame size.

[61742][0379][0x7f502d31e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VencSendStream : 410 Send IDR Stream Chn [0], IDR FrameId [0], IDR Len[7094]
[61746][0379][0x7f502d31e0][USR_LDRT][WARN] ar_ldrt_tx.c: AR_LDRT_TX_VencSendStream : 410 Send IDR Stream Chn [1], IDR FrameId [0], IDR Len[4849]
[65721][0379][0x7f5a3c71e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_LOWDELAY_TX_SYS_SyncTime : 146 Begain Get 8030 Time
[65725][0379][0x7f5a3c71e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sys.c: AR_LOWDELAY_TX_SYS_SyncTime : 155 Get 8030 u64DurTime 3523 larger than 1ms or get PTS 9571000.

[68709][0379][0x7f69dba1e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_StbAck : 1015  Stb Receive Ack Msg, CurWorkMode 0, NextWorkMode 1

[68712][0379][0x7f663961e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_StbThread : 1071  mode 0 15 15
[68712][0379][0x7f663961e0][APP_LOWDELAY][WARN] ar_lowdelay_tx_sysctrl.c: AR_LOWDELAY_TX_SYSCTRL_StbThread : 1075 u8AirscrewEn 0 u8StandbyModeEn 1 bStbEn 1



# Know facts

- the original firmware transmit not ont but two streams in parallel in 1080p 1920x560 and 1920x552
- The original firmware is here: https://drive.usercontent.google.com/download?id=1JZ17WKLQK0cysCFlLsHTg7nyBegQhUWG&export=download&authuser=0&confirm=t&uuid=10afa287-97fb-4863-87f6-fdb6de7fe43c&at=ALBwUgkkm5Hpy95UPffEHcGdsx62%3A1776521732029
- use p1-ota-extract.py to unpack the firmware partitions, then use binwalkv3 to unpack userdata to get the rootfs
- you cannot run both the original an our instance of the arlink_fpv at the same time
- after any start of our binary issue a reboot to have a clean boot