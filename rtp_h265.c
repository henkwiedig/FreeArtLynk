#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "rtp_h265.h"

/* ---- helpers ---- */

static void write_u16be(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xff;
}

static void write_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >>  8) & 0xff;
    p[3] =  v        & 0xff;
}

static void build_rtp_header(uint8_t *hdr, uint8_t pt, int marker,
                              uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    hdr[0] = 0x80;                          /* V=2, P=0, X=0, CC=0 */
    hdr[1] = (marker ? 0x80 : 0x00) | (pt & 0x7f);
    write_u16be(hdr + 2, seq);
    write_u32be(hdr + 4, ts);
    write_u32be(hdr + 8, ssrc);
}

/* ---- public API ---- */

void rtp_h265_init(RTP_H265_CTX_S *ctx, uint32_t ssrc,
                   rtp_send_fn send_fn, void *send_ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->ssrc         = ssrc;
    ctx->seq          = (uint16_t)(ssrc ^ 0xA5C3);
    ctx->clock_rate   = RTP_H265_CLOCK;
    ctx->payload_type = RTP_H265_PT;
    ctx->mtu          = RTP_DEFAULT_MTU;
    ctx->send_fn      = send_fn;
    ctx->send_ctx     = send_ctx;
}

const uint8_t *rtp_h265_skip_startcode(const uint8_t *p, size_t *remaining)
{
    if (*remaining >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        *remaining -= 4;
        return p + 4;
    }
    if (*remaining >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        *remaining -= 3;
        return p + 3;
    }
    return p;
}

int rtp_h265_send_nalu(RTP_H265_CTX_S *ctx,
                       const uint8_t *nalu, size_t nalu_len,
                       uint64_t pts_us, int last_nalu_in_frame)
{
    /* RTP timestamp: 90 kHz clock */
    uint32_t rtp_ts = (uint32_t)((pts_us * (uint64_t)ctx->clock_rate) / 1000000ULL);
    ctx->timestamp  = rtp_ts;

    int max_payload = ctx->mtu - RTP_HDR_SIZE;

    if ((int)nalu_len <= max_payload) {
        /* --- Single NAL unit packet --- */
        uint8_t pkt[RTP_HDR_SIZE + RTP_DEFAULT_MTU];
        build_rtp_header(pkt, ctx->payload_type, last_nalu_in_frame,
                         ctx->seq++, rtp_ts, ctx->ssrc);
        memcpy(pkt + RTP_HDR_SIZE, nalu, nalu_len);
        return ctx->send_fn(ctx->send_ctx, pkt, RTP_HDR_SIZE + nalu_len);
    }

    /* --- Fragmentation Unit (FU) --- RFC 7798 §4.4.3 ---
     *
     * PayloadHdr (2 bytes): nal_unit_type=49 (FU), nuh_layer_id=0, nuh_temporal_id_plus1=1
     * FU header  (1 byte):  S=1/E=0/0, followed by nal_unit_type from original NALU
     */
    uint8_t nal_unit_type = (nalu[0] >> 1) & 0x3f;
    /* PayloadHdr for FU: type=49 */
    uint8_t payload_hdr_0 = (49 << 1) | 0;   /* nuh_layer_id=0 in bits[5:1] */
    uint8_t payload_hdr_1 = 1;                /* nuh_temporal_id_plus1 = 1  */

    const uint8_t *src     = nalu + 2;        /* skip original 2-byte NALU header */
    size_t         src_rem = nalu_len - 2;
    int            first   = 1;

    while (src_rem > 0) {
        int frag_max = max_payload - 3; /* 2 PayloadHdr + 1 FU header */
        int frag_len = (int)src_rem < frag_max ? (int)src_rem : frag_max;
        int is_last  = (frag_len == (int)src_rem);
        int marker   = is_last && last_nalu_in_frame;

        uint8_t pkt[RTP_HDR_SIZE + 3 + RTP_DEFAULT_MTU];
        build_rtp_header(pkt, ctx->payload_type, marker,
                         ctx->seq++, rtp_ts, ctx->ssrc);

        pkt[RTP_HDR_SIZE + 0] = payload_hdr_0;
        pkt[RTP_HDR_SIZE + 1] = payload_hdr_1;
        pkt[RTP_HDR_SIZE + 2] = (first ? 0x80 : 0x00)
                               | (is_last ? 0x40 : 0x00)
                               | (nal_unit_type & 0x3f);

        memcpy(pkt + RTP_HDR_SIZE + 3, src, frag_len);

        int ret = ctx->send_fn(ctx->send_ctx, pkt,
                               RTP_HDR_SIZE + 3 + frag_len);
        if (ret < 0)
            return ret;

        src     += frag_len;
        src_rem -= frag_len;
        first    = 0;
    }
    return 0;
}
