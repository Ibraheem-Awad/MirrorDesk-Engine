/*
 * MirrorDesk: low-latency H.264 for the VideoToolbox decoder. See header.
 * GNU GPL v3 or later.
 */
#include "h264_low_latency.h"

#include <string.h>
#include <glib.h>
#define GST_USE_UNSTABLE_API 1
#include <gst/codecparsers/gsth264parser.h>
#include <gst/codecparsers/gsth264bitwriter.h>

/* Rewrite one parsed SPS to say: no frame reordering, and the decoder only
 * needs to keep the reference frames. Returns a new NAL (with 4-byte start
 * code and emulation prevention) in *nal / *nal_len, or FALSE. */
static gboolean rewrite_sps(GstH264SPS *sps, guint8 **nal, guint *nal_len) {
    GstH264VUIParams *vui = &sps->vui_parameters;
    guint8 dpb = (guint8) MAX(1, sps->num_ref_frames);

    if (sps->vui_parameters_present_flag && vui->bitstream_restriction_flag &&
        vui->num_reorder_frames == 0 && vui->max_dec_frame_buffering <= dpb) {
        return FALSE;  /* already low latency */
    }
    if (!sps->vui_parameters_present_flag) {
        memset(vui, 0, sizeof(*vui));
        sps->vui_parameters_present_flag = 1;
    }
    if (!vui->bitstream_restriction_flag) {
        /* "unrestricted" defaults from the H.264 spec (E.2.1) */
        vui->bitstream_restriction_flag = 1;
        vui->motion_vectors_over_pic_boundaries_flag = 1;
        vui->max_bytes_per_pic_denom = 2;
        vui->max_bits_per_mb_denom = 1;
        vui->log2_max_mv_length_horizontal = 16;
        vui->log2_max_mv_length_vertical = 16;
    }
    vui->num_reorder_frames = 0;
    vui->max_dec_frame_buffering = dpb;

    guint8 raw[1024];
    guint raw_size = sizeof(raw);
    if (gst_h264_bit_writer_sps(sps, TRUE, raw, &raw_size) != GST_H264_BIT_WRITER_OK) {
        return FALSE;
    }
    guint out_size = raw_size * 2 + 16;
    guint8 *out = g_malloc(out_size);
    /* raw_size is in bytes; convert_to_nal takes the size in bits */
    if (gst_h264_bit_writer_convert_to_nal(4, FALSE, TRUE, FALSE, raw, raw_size * 8, out, &out_size)
        != GST_H264_BIT_WRITER_OK) {
        g_free(out);
        return FALSE;
    }
    *nal = out;
    *nal_len = out_size;
    return TRUE;
}

int h264_low_latency_rewrite(const uint8_t *in, int in_len, uint8_t **out, int *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!in || in_len < 5) return 0;

    GstH264NalParser *parser = gst_h264_nal_parser_new();
    GByteArray *result = NULL;
    guint copied_upto = 0;   /* bytes of the input already emitted */
    guint offset = 0;
    GstH264NalUnit nalu;

    while (offset < (guint) in_len) {
        GstH264ParserResult r = gst_h264_parser_identify_nalu(parser, in, offset, (gsize) in_len, &nalu);
        if (r == GST_H264_PARSER_NO_NAL_END) {
            r = GST_H264_PARSER_OK;   /* last NAL in the buffer */
        }
        if (r != GST_H264_PARSER_OK) break;

        if (nalu.type == GST_H264_NAL_SPS) {
            GstH264SPS sps;
            guint8 *nal = NULL;
            guint nal_len = 0;
            if (gst_h264_parser_parse_sps(parser, &nalu, &sps) == GST_H264_PARSER_OK) {
                if (rewrite_sps(&sps, &nal, &nal_len)) {
                    if (!result) result = g_byte_array_sized_new((guint) in_len + 32);
                    /* copy everything before this NAL's start code, then the new SPS */
                    guint nal_start = nalu.sc_offset;
                    g_byte_array_append(result, in + copied_upto, nal_start - copied_upto);
                    g_byte_array_append(result, nal, nal_len);
                    copied_upto = nalu.offset + nalu.size;
                    g_free(nal);
                }
                gst_h264_sps_clear(&sps);
            }
        }
        offset = nalu.offset + nalu.size;
    }
    gst_h264_nal_parser_free(parser);

    if (!result) return 0;
    if (copied_upto < (guint) in_len) {
        g_byte_array_append(result, in + copied_upto, (guint) in_len - copied_upto);
    }
    *out_len = (int) result->len;
    *out = g_byte_array_free(result, FALSE);
    return 1;
}
