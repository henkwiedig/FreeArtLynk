#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      sockfd;
    char     dest_ip[64];
    uint16_t dest_port;
} UDP_SENDER_S;

int  udp_sender_open(UDP_SENDER_S *s, const char *dest_ip, uint16_t dest_port);
int  udp_sender_write(void *ctx, const uint8_t *data, size_t len);
void udp_sender_close(UDP_SENDER_S *s);
