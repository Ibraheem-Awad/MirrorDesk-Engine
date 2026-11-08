/*
 * MirrorDesk: video renderer for the engine helper process.
 *
 * Implements the same interface as video_renderer.c, but instead of decoding
 * and showing the video in a GStreamer window, it forwards the compressed
 * H.264 stream to the MirrorDesk app (see mirrordesk_channel.h), which decodes
 * it with VideoToolbox and shows it in its own window. Each frame carries how
 * long until it should be shown, so video stays in sync with the audio that
 * the engine plays.
 *
 * AirPlay screen mirroring in H.264 or H.265 is supported; HLS video and
 * cover art are not enabled in MirrorDesk.
 *
 * Part of the MirrorDesk modifications to UxPlay; GNU GPL v3 or later.
 */
#include <glib.h>
#include "video_renderer.h"
#include "mirrordesk_channel.h"
#ifdef MIRRORDESK_LOW_LATENCY_H264
#include "h264_low_latency.h"
#endif

#include <pthread.h>
#include <string.h>
#include <time.h>

static logger_t *logger = NULL;
static char *active_server_name = NULL;
static bool video_sync = true;
/* stream_active: the app is showing this receiver's window. A stream starts
 * with its first frame and ends with video_renderer_stop() (TEARDOWN, reset,
 * or the last client disconnecting). */
static bool stream_active = false;
static bool first_packet = true;
static bool stream_is_h265 = false;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned short width, height, width_source, height_source;

static uint64_t realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

void video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser,
                         const char *rtp_pipeline, const char *decoder, const char *converter, const char *videosink,
                         const char *videosink_options, bool initial_fullscreen, bool sync, bool h265_support,
                         bool coverart_support, guint playbin_version, const char *uri) {
    (void) videoflip; (void) parser; (void) rtp_pipeline; (void) decoder; (void) converter; (void) videosink;
    (void) videosink_options; (void) initial_fullscreen; (void) h265_support; (void) coverart_support;
    (void) playbin_version; (void) uri;
    logger = render_logger;
    g_free(active_server_name);
    active_server_name = g_strdup(server_name);
    video_sync = sync;
    logger_log(logger, LOGGER_DEBUG, "MirrorDesk: video is shown by the app (helper renderer, sync=%d)", (int) sync);
}

void video_renderer_start() {
    pthread_mutex_lock(&state_lock);
    first_packet = true;
    pthread_mutex_unlock(&state_lock);
}

/* Ends the current stream: the app closes the window. The next frame starts a new one. */
void video_renderer_stop() {
    pthread_mutex_lock(&state_lock);
    if (stream_active) {
        stream_active = false;
        md_channel_send(MD_MSG_VIDEO_STOP, NULL, 0, NULL, 0);
    }
    first_packet = true;
    pthread_mutex_unlock(&state_lock);
}

void video_renderer_destroy() {
    video_renderer_stop();
}

void video_renderer_flush() {
    md_channel_send(MD_MSG_VIDEO_FLUSH, NULL, 0, NULL, 0);
}

uint64_t video_renderer_render_buffer(unsigned char *data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    (void) nal_count;
    if (*data_len <= 4) return 0;
    /* valid H.264 starts 00 00 00 01; a non-zero first byte means decryption failed */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
        return 0;
    }
    pthread_mutex_lock(&state_lock);
    bool starting = first_packet;
    if (first_packet) {
        uint8_t codec = stream_is_h265 ? 1 : 0;
        md_channel_send(MD_MSG_VIDEO_START, &codec, 1, NULL, 0);
        stream_active = true;
        first_packet = false;
    }
    pthread_mutex_unlock(&state_lock);
    if (starting) {
        logger_log(logger, LOGGER_INFO, "Begin streaming video to MirrorDesk");
    }

    /* ntp_time is when the frame should be shown, on the local real-time clock */
    int64_t delay_ns = video_sync ? (int64_t) (*ntp_time - realtime_ns()) : 0;
    uint8_t head[8];
    for (int i = 0; i < 8; i++) head[i] = (uint8_t) (((uint64_t) delay_ns >> (8 * i)) & 0xff);

#ifdef MIRRORDESK_LOW_LATENCY_H264
    uint8_t *low_latency = NULL;
    int low_latency_len = 0;
    if (!stream_is_h265 && (data[4] & 0x1f) == 7 && h264_low_latency_rewrite(data, *data_len, &low_latency, &low_latency_len)) {
        md_channel_send(MD_MSG_VIDEO_FRAME, head, sizeof head, low_latency, (size_t) low_latency_len);
        g_free(low_latency);
        return 0;
    }
#endif
    md_channel_send(MD_MSG_VIDEO_FRAME, head, sizeof head, data, (size_t) *data_len);
    return 0;
}

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    unsigned short new_width_source = (unsigned short) *f_width_source;
    unsigned short new_height_source = (unsigned short) *f_height_source;
    unsigned short new_width = (unsigned short) *f_width;
    unsigned short new_height = (unsigned short) *f_height;
    if (new_width != width || new_height != height ||
        new_width_source != width_source || new_height_source != height_source) {
        width = new_width;
        height = new_height;
        width_source = new_width_source;
        height_source = new_height_source;
        /* the app follows rotation using this line */
        if (logger) logger_log(logger, LOGGER_INFO,
                   "MirrorDesk Debug: video size changed receiver=%s width=%d height=%d sourceWidth=%d sourceHeight=%d",
                   active_server_name ? active_server_name : "(unknown)", width, height, width_source, height_source);
    }
}

int video_renderer_choose_codec(bool video_is_jpeg, bool video_is_h265) {
    if (video_is_jpeg) {
        if (logger) logger_log(logger, LOGGER_ERR, "MirrorDesk: cover art video is not supported");
        return -1;
    }
    pthread_mutex_lock(&state_lock);
    if (stream_active && stream_is_h265 != video_is_h265) {
        /* codec changed mid-stream: start a new stream so the app switches decoder */
        stream_active = false;
        md_channel_send(MD_MSG_VIDEO_STOP, NULL, 0, NULL, 0);
        first_packet = true;
    }
    stream_is_h265 = video_is_h265;
    pthread_mutex_unlock(&state_lock);
    if (logger) logger_log(logger, LOGGER_INFO, "MirrorDesk: video codec %s", video_is_h265 ? "H.265" : "H.264");
    return 0;
}

/* Not used for AirPlay screen mirroring */
void video_renderer_pause() {}
void video_renderer_resume() {}
bool video_renderer_is_paused() { return false; }
void video_renderer_set_device_model(const char *model, const char *name) { (void) model; (void) name; }
void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album) {
    (void) title; (void) artist; (void) album;
}
void video_renderer_hls_ready() {}
void video_renderer_seek(float position) { (void) position; }
void video_renderer_set_start(float position) { (void) position; }
int video_renderer_cycle() { return -1; }
bool video_renderer_eos_watch() { return false; }
void video_renderer_display_jpeg(const void *data, int *data_len) { (void) data; (void) data_len; }
unsigned int video_renderer_listen(void *loop, int id) { (void) loop; (void) id; return 0; }
bool waiting_for_x11_window() { return false; }
void video_renderer_hls_set_volume(double volume) { (void) volume; }
bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration,
                             float *rate, bool *buffer_empty, bool *buffer_full) {
    (void) duration; (void) position; (void) seek_start; (void) seek_duration;
    (void) rate; (void) buffer_empty; (void) buffer_full;
    return false;
}
