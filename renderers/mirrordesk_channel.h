/*
 * MirrorDesk: message channel from the engine helper process to the app.
 *
 * The engine runs in its own process (MirrorDeskEngine). Instead of opening a
 * window, it sends the compressed H.264 video to the MirrorDesk app, which
 * decodes and shows it in its own window. Log text goes to stderr; these
 * binary messages go over a separate pipe.
 *
 * Message format: 1 byte type, 4 bytes payload length (little endian), payload.
 *
 * Part of the MirrorDesk modifications to UxPlay; GNU GPL v3 or later.
 */
#ifndef MIRRORDESK_CHANNEL_H
#define MIRRORDESK_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MD_MSG_VIDEO_START = 1,   /* 1 byte codec (0 = H.264, 1 = H.265): a new video stream begins */
    MD_MSG_VIDEO_FRAME = 2,   /* int64 delay_ns (LE), then Annex-B H.264    */
    MD_MSG_VIDEO_FLUSH = 3,   /* no payload: drop queued frames             */
    MD_MSG_VIDEO_STOP  = 4,   /* no payload: stream ended, close the window */
    MD_MSG_REPLY       = 5,   /* UTF-8 text: reply to a command             */
};

/* Send messages to fd (the helper's original stdout). */
void md_channel_open(int fd);

/* True when the engine runs as the MirrorDesk helper. */
bool md_channel_active(void);

/* Send one message whose payload is head followed by body (either may be empty). */
void md_channel_send(uint8_t type, const void *head, size_t head_len, const void *body, size_t body_len);

#ifdef __cplusplus
}
#endif

#endif
