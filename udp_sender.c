#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "udp_sender.h"

int udp_sender_open(UDP_SENDER_S *s, const char *dest_ip, uint16_t dest_port)
{
    memset(s, 0, sizeof(*s));
    s->sockfd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* match the original: 1 MB tx/rx buffers */
    int bufsize = 0x100000;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    struct sockaddr_in peer = {0};
    peer.sin_family      = AF_INET;
    peer.sin_port        = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &peer.sin_addr) != 1) {
        fprintf(stderr, "invalid dest IP: %s\n", dest_ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    s->sockfd    = fd;
    s->dest_port = dest_port;
    strncpy(s->dest_ip, dest_ip, sizeof(s->dest_ip) - 1);

    printf("[udp] sending RTP to %s:%u\n", dest_ip, dest_port);
    return 0;
}

int udp_sender_write(void *ctx, const uint8_t *data, size_t len)
{
    UDP_SENDER_S *s = (UDP_SENDER_S *)ctx;
    ssize_t sent = send(s->sockfd, data, len, 0);
    if (sent < 0) {
        perror("send");
        return -1;
    }
    return 0;
}

void udp_sender_close(UDP_SENDER_S *s)
{
    if (s->sockfd >= 0) {
        close(s->sockfd);
        s->sockfd = -1;
    }
}
