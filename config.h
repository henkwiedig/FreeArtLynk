#pragma once
#include <stdint.h>

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
    int      flip;
    int      mirror;
    int      saturation;   /* -1 = unchanged */
    int      sharpness;    /* -1 = unchanged */
    int      wb;           /* -2 = unchanged, -1 = auto, >0 = manual CCT K */
    int      ev_us;        /* -2 = unchanged, -1 = auto AEC, >0 = manual µs */
    int      dnr3d;        /* -1 = unchanged, 0 = off, 1 = on */
    int      dnr2d;        /* -1 = unchanged, 0 = off, 1 = on */
    float    zoom;         /* 1.0 = none, >1.0 = zoom in */
    int      aspect;       /* 0 = 16:9, 1 = 4:3 */
    int      http_port;    /* 0 = disabled */
} CONFIG_S;
