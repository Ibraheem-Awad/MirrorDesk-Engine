/*
 * MirrorDesk: message channel from the engine helper process to the app.
 * See mirrordesk_channel.h. GNU GPL v3 or later.
 */
#include "mirrordesk_channel.h"

#include <errno.h>
#include <pthread.h>
#include <unistd.h>

static int channel_fd = -1;
static pthread_mutex_t channel_lock = PTHREAD_MUTEX_INITIALIZER;

void md_channel_open(int fd) {
    channel_fd = fd;
}

bool md_channel_active(void) {
    return channel_fd >= 0;
}

static bool write_all(const void *data, size_t len) {
    const uint8_t *p = data;
    while (len > 0) {
        ssize_t n = write(channel_fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t) n;
    }
    return true;
}

void md_channel_send(uint8_t type, const void *head, size_t head_len, const void *body, size_t body_len) {
    if (channel_fd < 0) return;
    uint32_t len = (uint32_t) (head_len + body_len);
    uint8_t header[5] = {
        type,
        (uint8_t) (len & 0xff), (uint8_t) ((len >> 8) & 0xff),
        (uint8_t) ((len >> 16) & 0xff), (uint8_t) ((len >> 24) & 0xff)
    };
    pthread_mutex_lock(&channel_lock);
    bool ok = write_all(header, sizeof header) &&
              (head_len == 0 || write_all(head, head_len)) &&
              (body_len == 0 || write_all(body, body_len));
    if (!ok) {
        /* the app is gone: stop sending (the helper exits when its stdin closes) */
        channel_fd = -1;
    }
    pthread_mutex_unlock(&channel_lock);
}
