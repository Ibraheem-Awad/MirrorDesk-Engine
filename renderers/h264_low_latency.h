/*
 * MirrorDesk: low-latency H.264 for the VideoToolbox decoder.
 *
 * AirPlay mirror streams never reorder frames, but their SPS doesn't say so
 * (no VUI bitstream restriction). GStreamer's vtdec then assumes the worst
 * case and holds back several decoded frames before showing them, which adds
 * latency and makes the video sink drop late frames. This rewrites each SPS
 * to declare "no reordering", so frames are output as soon as they're decoded.
 *
 * Part of the MirrorDesk modifications to UxPlay; GNU GPL v3 or later.
 */
#ifndef H264_LOW_LATENCY_H
#define H264_LOW_LATENCY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scans an Annex-B (start-code) H.264 buffer. If it contains an SPS, writes a
 * copy of the buffer with each SPS rewritten to *out (allocated with g_malloc,
 * free with g_free) and returns 1. Returns 0 (and leaves *out NULL) when there
 * is no SPS or it can't be rewritten -- the caller then uses the original. */
int h264_low_latency_rewrite(const uint8_t *in, int in_len, uint8_t **out, int *out_len);

#ifdef __cplusplus
}
#endif

#endif
