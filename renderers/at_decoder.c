/*
 * MirrorDesk: AudioToolbox decoder for AirPlay audio (AAC-ELD, AAC-LC, ALAC).
 * See at_decoder.h. GNU General Public License v3 or later.
 */

#include "at_decoder.h"

#include <stdlib.h>
#include <string.h>
#include <AudioToolbox/AudioToolbox.h>

#define OUT_RATE      44100.0
#define OUT_CHANNELS  2

struct at_decoder_s {
    AudioConverterRef converter;
    uint32_t frames_per_packet;
    /* the packet currently handed to the converter */
    const uint8_t *packet;
    UInt32 packet_len;
    int packet_consumed;
    AudioStreamPacketDescription packet_desc;
};

/* Standard AirPlay codec configurations */
/* AAC-ELD 44100 Hz stereo, 480 samples/frame (AudioSpecificConfig) */
static const uint8_t asc_aac_eld[] = { 0xf8, 0xe8, 0x50, 0x00 };
/* AAC-LC 44100 Hz stereo (AudioSpecificConfig) */
static const uint8_t asc_aac_lc[] = { 0x12, 0x10 };
/* ALACSpecificConfig: frameLength 352, 16 bit, 2 channels, 44100 Hz */
static const uint8_t alac_config[] = {
    0x00, 0x00, 0x01, 0x60,  /* frameLength = 352 */
    0x00,                    /* compatibleVersion */
    0x10,                    /* bitDepth = 16 */
    0x28, 0x0a, 0x0e,        /* pb, mb, kb */
    0x02,                    /* numChannels */
    0x00, 0xff,              /* maxRun */
    0x00, 0x00, 0x00, 0x00,  /* maxFrameBytes */
    0x00, 0x00, 0x00, 0x00,  /* avgBitRate */
    0x00, 0x00, 0xac, 0x44   /* sampleRate = 44100 */
};

/* AudioToolbox's AAC decoders take their configuration as an MPEG-4
 * ES_Descriptor (the payload of an 'esds' box), not a bare
 * AudioSpecificConfig. Wrap the ASC accordingly. Returns the length. */
static size_t make_es_descriptor(const uint8_t *asc, size_t asc_len, uint8_t *out, size_t cap) {
    size_t dsi_len = 2 + asc_len;                 /* tag 0x05 + len + ASC */
    size_t dcd_len = 2 + 13 + dsi_len;            /* tag 0x04 + len + 13 bytes + DSI */
    size_t sl_len = 3;                            /* tag 0x06 + len + predefined */
    size_t es_body = 3 + dcd_len + sl_len;        /* ES_ID(2) + flags(1) + ... */
    size_t total = 2 + es_body;
    if (asc_len > 64 || total > cap) return 0;
    size_t i = 0;
    out[i++] = 0x03; out[i++] = (uint8_t) es_body;
    out[i++] = 0x00; out[i++] = 0x00;             /* ES_ID */
    out[i++] = 0x00;                              /* flags */
    out[i++] = 0x04; out[i++] = (uint8_t) (13 + dsi_len);
    out[i++] = 0x40;                              /* objectTypeIndication: MPEG-4 audio */
    out[i++] = 0x15;                              /* streamType: audio */
    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00;           /* bufferSizeDB */
    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; /* maxBitrate */
    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; /* avgBitrate */
    out[i++] = 0x05; out[i++] = (uint8_t) asc_len;
    memcpy(out + i, asc, asc_len); i += asc_len;
    out[i++] = 0x06; out[i++] = 0x01; out[i++] = 0x02;          /* SLConfig: predefined MP4 */
    return i;
}

static OSStatus input_proc(AudioConverterRef converter, UInt32 *io_packets, AudioBufferList *io_data,
                           AudioStreamPacketDescription **out_desc, void *user) {
    (void) converter;
    at_decoder_t *dec = (at_decoder_t *) user;
    if (dec->packet_consumed || !dec->packet || dec->packet_len == 0) {
        *io_packets = 0;
        return 1; /* no more data for this call; any non-zero status ends the fill */
    }
    io_data->mNumberBuffers = 1;
    io_data->mBuffers[0].mData = (void *) dec->packet;
    io_data->mBuffers[0].mDataByteSize = dec->packet_len;
    io_data->mBuffers[0].mNumberChannels = 0;
    dec->packet_desc.mStartOffset = 0;
    dec->packet_desc.mVariableFramesInPacket = 0;
    dec->packet_desc.mDataByteSize = dec->packet_len;
    if (out_desc) {
        *out_desc = &dec->packet_desc;
    }
    *io_packets = 1;
    dec->packet_consumed = 1;
    return noErr;
}

at_decoder_t *at_decoder_create_with_config(int ct, const uint8_t *config, size_t config_len,
                                            uint32_t frames_per_packet, uint32_t channels) {
    AudioStreamBasicDescription in = {0};
    in.mSampleRate = OUT_RATE;
    in.mChannelsPerFrame = channels;
    in.mFramesPerPacket = frames_per_packet;
    switch (ct) {
    case AT_CT_AAC_ELD:
        in.mFormatID = kAudioFormatMPEG4AAC_ELD;
        break;
    case AT_CT_AAC_LC:
        in.mFormatID = kAudioFormatMPEG4AAC;
        break;
    case AT_CT_ALAC:
        in.mFormatID = kAudioFormatAppleLossless;
        in.mFormatFlags = 1; /* kAppleLosslessFormatFlag_16BitSourceData */
        break;
    default:
        return NULL;
    }

    AudioStreamBasicDescription out = {0};
    out.mSampleRate = OUT_RATE;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mChannelsPerFrame = OUT_CHANNELS;
    out.mBitsPerChannel = 16;
    out.mFramesPerPacket = 1;
    out.mBytesPerFrame = 2 * OUT_CHANNELS;
    out.mBytesPerPacket = out.mBytesPerFrame;

    at_decoder_t *dec = (at_decoder_t *) calloc(1, sizeof(at_decoder_t));
    if (!dec) return NULL;
    dec->frames_per_packet = frames_per_packet;

    if (AudioConverterNew(&in, &out, &dec->converter) != noErr) {
        free(dec);
        return NULL;
    }
    if (config && config_len) {
        uint8_t esds[128];
        const void *cookie = config;
        size_t cookie_len = config_len;
        if (ct == AT_CT_AAC_ELD || ct == AT_CT_AAC_LC) {
            cookie_len = make_es_descriptor(config, config_len, esds, sizeof(esds));
            cookie = esds;
            if (cookie_len == 0) {
                AudioConverterDispose(dec->converter);
                free(dec);
                return NULL;
            }
        }
        if (AudioConverterSetProperty(dec->converter, kAudioConverterDecompressionMagicCookie,
                                      (UInt32) cookie_len, cookie) != noErr) {
            AudioConverterDispose(dec->converter);
            free(dec);
            return NULL;
        }
    }
    return dec;
}

at_decoder_t *at_decoder_create(int ct) {
    switch (ct) {
    case AT_CT_AAC_ELD:
        return at_decoder_create_with_config(ct, asc_aac_eld, sizeof(asc_aac_eld), 480, 2);
    case AT_CT_AAC_LC:
        return at_decoder_create_with_config(ct, asc_aac_lc, sizeof(asc_aac_lc), 1024, 2);
    case AT_CT_ALAC:
        return at_decoder_create_with_config(ct, alac_config, sizeof(alac_config), 352, 2);
    default:
        return NULL;
    }
}

int at_decoder_decode(at_decoder_t *dec, const uint8_t *packet, size_t packet_len,
                      uint8_t *out, size_t out_capacity) {
    if (!dec || !packet || packet_len == 0 || !out) return -1;

    dec->packet = packet;
    dec->packet_len = (UInt32) packet_len;
    dec->packet_consumed = 0;

    size_t written = 0;
    /* A packet decodes to frames_per_packet frames; loop in case the
     * converter hands them back in more than one piece. */
    for (int pass = 0; pass < 4; pass++) {
        UInt32 frames = (UInt32) ((out_capacity - written) / (2 * OUT_CHANNELS));
        if (frames == 0) break;
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = OUT_CHANNELS;
        list.mBuffers[0].mDataByteSize = frames * 2 * OUT_CHANNELS;
        list.mBuffers[0].mData = out + written;
        OSStatus status = AudioConverterFillComplexBuffer(dec->converter, input_proc, dec, &frames, &list, NULL);
        written += list.mBuffers[0].mDataByteSize;
        if (frames == 0 || (status != noErr && status != 1)) {
            if (status != noErr && status != 1 && written == 0) {
                dec->packet = NULL;
                return -1;
            }
            break;
        }
        if (dec->packet_consumed && status == 1) break;
    }
    dec->packet = NULL;
    return (int) written;
}

void at_decoder_reset(at_decoder_t *dec) {
    if (dec && dec->converter) {
        AudioConverterReset(dec->converter);
    }
}

void at_decoder_destroy(at_decoder_t *dec) {
    if (!dec) return;
    if (dec->converter) {
        AudioConverterDispose(dec->converter);
    }
    free(dec);
}

typedef struct { UInt32 frames_left; int16_t zeros[2 * 1024]; } silence_src_t;

static OSStatus silence_input(AudioConverterRef c, UInt32 *io_packets, AudioBufferList *data,
                              AudioStreamPacketDescription **desc, void *user) {
    (void) c; (void) desc;
    silence_src_t *src = (silence_src_t *) user;
    if (src->frames_left == 0) { *io_packets = 0; return noErr; }
    UInt32 n = *io_packets;
    if (n > 1024) n = 1024;
    if (n > src->frames_left) n = src->frames_left;
    data->mNumberBuffers = 1;
    data->mBuffers[0].mData = src->zeros;
    data->mBuffers[0].mDataByteSize = n * 4;
    data->mBuffers[0].mNumberChannels = 2;
    src->frames_left -= n;
    *io_packets = n;
    return noErr;
}

size_t at_make_silent_packet(int ct, uint8_t *out, size_t capacity) {
    AudioStreamBasicDescription in = {0};
    in.mSampleRate = OUT_RATE;
    in.mFormatID = kAudioFormatLinearPCM;
    in.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    in.mChannelsPerFrame = 2; in.mBitsPerChannel = 16; in.mFramesPerPacket = 1;
    in.mBytesPerFrame = 4; in.mBytesPerPacket = 4;
    AudioStreamBasicDescription enc_out = {0};
    enc_out.mSampleRate = OUT_RATE;
    enc_out.mChannelsPerFrame = 2;
    if (ct == AT_CT_AAC_ELD) {
        enc_out.mFormatID = kAudioFormatMPEG4AAC_ELD;
        enc_out.mFramesPerPacket = 480;
    } else if (ct == AT_CT_AAC_LC) {
        enc_out.mFormatID = kAudioFormatMPEG4AAC;
        enc_out.mFramesPerPacket = 1024;
    } else {
        return 0;
    }
    AudioConverterRef enc;
    if (AudioConverterNew(&in, &enc_out, &enc) != noErr) return 0;
    /* encode a little silence and keep a packet from after the encoder's warm-up */
    silence_src_t *src = (silence_src_t *) calloc(1, sizeof(silence_src_t));
    src->frames_left = (UInt32) (OUT_RATE / 4);
    uint8_t packet[2048];
    size_t result = 0;
    for (int i = 0; i < 12; i++) {
        UInt32 n = 1;
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 2;
        list.mBuffers[0].mDataByteSize = sizeof(packet);
        list.mBuffers[0].mData = packet;
        AudioStreamPacketDescription desc;
        if (AudioConverterFillComplexBuffer(enc, silence_input, src, &n, &list, &desc) != noErr || n == 0) break;
        if (i >= 6 && desc.mDataByteSize > 0 && desc.mDataByteSize <= capacity) {
            memcpy(out, packet, desc.mDataByteSize);
            result = desc.mDataByteSize;
            break;
        }
    }
    free(src);
    AudioConverterDispose(enc);
    return result;
}
