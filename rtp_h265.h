#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * RFC 7798 RTP H.265 packetizer.
 *
 * Supports three packetization modes:
 *   - Single NAL unit packet  (NALU <= MTU-RTP_HDR_SIZE)
 *   - Fragmentation unit (FU) (NALU >  MTU-RTP_HDR_SIZE)
 *   - AP (aggregation) not implemented; not needed for single-stream FPV use
 */

#define RTP_HDR_SIZE       12
#define RTP_H265_FU_HDR    4   /* PayloadHdr(2) + FU header(1) + DONL(0) */
#define RTP_DEFAULT_MTU    1400
#define RTP_H265_PT        96  /* dynamic payload type */
#define RTP_H265_CLOCK     90000

/* Opaque sender callback: write `len` bytes from `data` to network */
typedef int (*rtp_send_fn)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    uint32_t     ssrc;
    uint16_t     seq;
    uint32_t     timestamp;   /* updated per frame */
    uint32_t     clock_rate;  /* 90000 */
    uint8_t      payload_type;
    int          mtu;

    rtp_send_fn  send_fn;
    void        *send_ctx;
} RTP_H265_CTX_S;

void rtp_h265_init(RTP_H265_CTX_S *ctx, uint32_t ssrc,
                   rtp_send_fn send_fn, void *send_ctx);

/*
 * Packetize one H.265 NALU (without start code) and send via ctx->send_fn.
 * `pts_us` is the presentation timestamp in microseconds.
 * `last_nalu_in_frame` marks the last NALU — sets the RTP marker bit.
 */
int rtp_h265_send_nalu(RTP_H265_CTX_S *ctx,
                       const uint8_t *nalu, size_t nalu_len,
                       uint64_t pts_us, int last_nalu_in_frame);

/* Strip Annex-B start code (3- or 4-byte) and return pointer past it */
const uint8_t *rtp_h265_skip_startcode(const uint8_t *p, size_t *remaining);
