/*
 * MirrorDesk: AudioToolbox decoder for AirPlay audio (AAC-ELD, AAC-LC, ALAC).
 *
 * Decodes compressed AirPlay audio packets to interleaved S16LE PCM
 * (44100 Hz, 2 channels) using Apple's system codecs, so the engine needs
 * no FFmpeg/libav decoder plugin.
 *
 * This file is part of the MirrorDesk modifications to UxPlay and is
 * distributed under the GNU General Public License v3 or later.
 */

#ifndef AT_DECODER_H
#define AT_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct at_decoder_s at_decoder_t;

/* AirPlay compression types (ct) */
#define AT_CT_ALAC    2
#define AT_CT_AAC_LC  4
#define AT_CT_AAC_ELD 8

/* Create a decoder for AirPlay compression type ct using its standard
 * codec configuration. Returns NULL if the system can't decode it. */
at_decoder_t *at_decoder_create(int ct);

/* Same, with an explicit codec configuration ("magic cookie"):
 * raw AudioSpecificConfig for AAC, ALACSpecificConfig for ALAC.
 * frames_per_packet and channels describe the compressed stream. */
at_decoder_t *at_decoder_create_with_config(int ct, const uint8_t *config, size_t config_len,
                                            uint32_t frames_per_packet, uint32_t channels);

/* Decode one compressed packet. Writes up to out_capacity bytes of
 * interleaved S16LE PCM to out and returns the number of bytes written,
 * or -1 on error. */
int at_decoder_decode(at_decoder_t *dec, const uint8_t *packet, size_t packet_len,
                      uint8_t *out, size_t out_capacity);

/* Drop any decoder state (e.g. after a flush or stream restart). */
void at_decoder_reset(at_decoder_t *dec);

void at_decoder_destroy(at_decoder_t *dec);

/* Encode one silent packet in the standard AirPlay format for ct (AAC-ELD
 * 480 or AAC-LC 1024, 44100 Hz stereo). Used to fill silent gaps in
 * recordings. Returns the packet size, or 0 if unavailable. */
size_t at_make_silent_packet(int ct, uint8_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
