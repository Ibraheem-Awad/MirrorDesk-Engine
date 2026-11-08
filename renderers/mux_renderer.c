#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <string.h>
#include "mux_renderer.h"
#ifdef USE_AUDIOTOOLBOX_DECODER
#include "at_decoder.h"
/* MirrorDesk: when the sender sends no audio (between videos, during a pause),
 * there are simply no packets. MP4 stores AAC as a sequence of fixed-length
 * frames and ignores timestamps, so those silent stretches vanished from
 * recordings and later sound moved earlier. Fill them with silent frames. */
static uint8_t silent_eld_packet[2048];
static size_t silent_eld_packet_len = 0;
#define AAC_ELD_FRAME_NS ((GstClockTime) (480 * GST_SECOND / 44100))
#define MAX_FILLED_GAP (30 * 60 * GST_SECOND)
#endif

#define SECOND_IN_NSECS 1000000000UL
#define MAX_PENDING_FRAMES 1800

typedef struct {
    unsigned char *data;
    int data_len;
    uint64_t ntp_time;
} pending_frame_t;

static GQueue *pending_video = NULL;
static int pending_count = 0;
static GRecMutex mux_lock;
static gsize mux_lock_initialized = 0;

static logger_t *logger = NULL;
static const char *output_filename = NULL;
static int file_count = 0;
static gboolean no_audio = FALSE;
static gboolean no_video = FALSE;
static gboolean audio_is_alac = FALSE;
static gboolean video_is_h265 = FALSE;
static gboolean pipeline_ready = FALSE;
static gboolean pipeline_running = FALSE;
static guint64 pushed_video_frames = 0;
static guint64 pushed_audio_buffers = 0;
static gboolean recording_start_time_forced = FALSE;
static GstClockTime forced_recording_start_time = GST_CLOCK_TIME_NONE;
static gdouble last_audio_lead_in_seconds = 0.0;
static gboolean insert_leading_silence_in_output = TRUE;

typedef struct mux_renderer_s {
    GstElement *pipeline;
    GstElement *video_appsrc;
    GstElement *audio_appsrc;
    GstElement *filesink;
    GstBus *bus;
    GstClockTime base_time;
    GstClockTime first_video_time;
    GstClockTime first_audio_time;
    GstClockTime last_video_time;
    GstClockTime last_audio_time;
    gboolean audio_started;
    gboolean is_alac;
    gboolean is_h265;
    gboolean logged_first_video;
    gboolean logged_first_audio;
    GstClockTime next_audio_pts;   /* where the next audio frame should start */
} mux_renderer_t;

static mux_renderer_t *renderer = NULL;

static const char h264_caps[] = "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[] = "video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";
static const char aac_eld_caps[] = "audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";
static const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                                "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";

static void ensure_mux_lock(void) {
    if (g_once_init_enter(&mux_lock_initialized)) {
        g_rec_mutex_init(&mux_lock);
        g_once_init_leave(&mux_lock_initialized, 1);
    }
}

static void free_pending_frame(gpointer p) {
    pending_frame_t *frame = (pending_frame_t *) p;
    if (!frame) {
        return;
    }
    g_free(frame->data);
    g_free(frame);
}

static void clear_pending_video(void) {
    if (pending_video) {
        g_queue_foreach(pending_video, (GFunc) free_pending_frame, NULL);
        g_queue_clear(pending_video);
    }
    pending_count = 0;
}

void mux_renderer_set_recording_start_time(uint64_t ntp_time, bool insert_leading_silence) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    forced_recording_start_time = (GstClockTime) ntp_time;
    recording_start_time_forced = (forced_recording_start_time != GST_CLOCK_TIME_NONE && forced_recording_start_time > 0);
    insert_leading_silence_in_output = insert_leading_silence;
    logger_log(logger, LOGGER_INFO,
               "Mux recording start anchor set to ntp=%" G_GUINT64_FORMAT " insert_leading_silence=%d",
               (guint64) forced_recording_start_time,
               insert_leading_silence ? 1 : 0);
    if (renderer && renderer->base_time == GST_CLOCK_TIME_NONE && recording_start_time_forced && insert_leading_silence_in_output) {
        renderer->base_time = forced_recording_start_time;
    }
    g_rec_mutex_unlock(&mux_lock);
}

double mux_renderer_get_last_audio_lead_in_seconds(void) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    gdouble value = last_audio_lead_in_seconds;
    g_rec_mutex_unlock(&mux_lock);
    return value;
}

static void do_start_pipeline(gboolean with_audio) {
    GError *error = NULL;
    GstCaps *video_caps = NULL;
    GstCaps *audio_caps = NULL;

    if (renderer && renderer->pipeline) {
        logger_log(logger, LOGGER_DEBUG, "Mux pipeline already running");
        return;
    }

    if (renderer) {
        g_free(renderer);
    }

    renderer = g_new0(mux_renderer_t, 1);
    renderer->base_time = GST_CLOCK_TIME_NONE;
    renderer->first_video_time = GST_CLOCK_TIME_NONE;
    renderer->first_audio_time = GST_CLOCK_TIME_NONE;
    renderer->last_video_time = GST_CLOCK_TIME_NONE;
    renderer->last_audio_time = GST_CLOCK_TIME_NONE;
    renderer->next_audio_pts = GST_CLOCK_TIME_NONE;
    renderer->audio_started = FALSE;
    renderer->is_alac = audio_is_alac;
    renderer->is_h265 = video_is_h265;
    renderer->logged_first_video = FALSE;
    renderer->logged_first_audio = FALSE;

    file_count++;
    GString *filename = g_string_new("");
    g_string_append_printf(filename, "%s.%d.", output_filename, file_count);
    if (!no_video && !audio_is_alac) {
        g_string_append(filename, video_is_h265 ? "H265." : "H264.");
    }
    if (!no_audio && with_audio) {
        g_string_append(filename, audio_is_alac ? "ALAC." : "AAC.");
    }
    g_string_append(filename, "mp4");

    GString *launch = g_string_new("");
    if (!no_video && !audio_is_alac) {
        g_string_append(launch, "appsrc name=video_src format=time is-live=true ! queue ! ");
        g_string_append(launch, video_is_h265 ? "h265parse ! " : "h264parse ! ");
        g_string_append(launch, "mux. ");
    }
    if (!no_audio && with_audio) {
        g_string_append(launch, "appsrc name=audio_src format=time is-live=true ! queue ! ");
        if (!audio_is_alac) {
            g_string_append(launch, "aacparse ! queue ! ");
        }
        g_string_append(launch, "mux. ");
    }
    g_string_append(launch, "mp4mux name=mux interleave-bytes=500000 interleave-time=500000000 ! filesink name=filesink location=");
    g_string_append(launch, filename->str);

    logger_log(logger, LOGGER_DEBUG, "Mux pipeline: %s", launch->str);
    renderer->pipeline = gst_parse_launch(launch->str, &error);
    g_string_free(launch, TRUE);

    if (error) {
        logger_log(logger, LOGGER_ERR, "Mux pipeline error: %s", error->message);
        g_clear_error(&error);
        g_string_free(filename, TRUE);
        g_free(renderer);
        renderer = NULL;
        return;
    }

    if (!no_video && !audio_is_alac) {
        renderer->video_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_src");
        video_caps = gst_caps_from_string(video_is_h265 ? h265_caps : h264_caps);
        g_object_set(renderer->video_appsrc, "caps", video_caps, NULL);
        gst_caps_unref(video_caps);
    }

    if (!no_audio && with_audio) {
        renderer->audio_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "audio_src");
        audio_caps = gst_caps_from_string(audio_is_alac ? alac_caps : aac_eld_caps);
        g_object_set(renderer->audio_appsrc, "caps", audio_caps, NULL);
        gst_caps_unref(audio_caps);
    }

    renderer->filesink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "filesink");
    renderer->bus = gst_element_get_bus(renderer->pipeline);

    gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
    pipeline_running = TRUE;
    logger_log(logger, LOGGER_INFO,
               "Mux timing: pipeline started with_audio=%d codec=%s base_time=NONE",
               with_audio ? 1 : 0, video_is_h265 ? "h265" : "h264");
    logger_log(logger, LOGGER_INFO, "Started recording to: %s", filename->str);
    g_string_free(filename, TRUE);
}

static void flush_pending_video(void) {
    if (!renderer || !renderer->video_appsrc || !pending_video) {
        return;
    }

    guint count = g_queue_get_length(pending_video);
    if (count == 0) {
        return;
    }
    logger_log(logger, LOGGER_DEBUG, "Flushing %u buffered video frame(s) into pipeline", count);

    while (!g_queue_is_empty(pending_video)) {
        pending_frame_t *frame = (pending_frame_t *) g_queue_pop_head(pending_video);
        pending_count--;

        if (renderer->base_time == GST_CLOCK_TIME_NONE) {
            renderer->base_time = (GstClockTime) frame->ntp_time;
            renderer->first_video_time = (GstClockTime) frame->ntp_time;
        }

        GstClockTime pts = (GstClockTime) frame->ntp_time - renderer->base_time;
        renderer->last_video_time = (GstClockTime) frame->ntp_time;
        if (!renderer->logged_first_video) {
            logger_log(logger, LOGGER_INFO,
                       "Mux timing: first buffered video ntp=%" G_GUINT64_FORMAT " pts=%.3f buffered_count=%u",
                       (guint64) frame->ntp_time, (double) pts / GST_SECOND, count);
            renderer->logged_first_video = TRUE;
        }
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, frame->data_len, NULL);
        if (buffer) {
            gst_buffer_fill(buffer, 0, frame->data, frame->data_len);
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = pts;
            gst_app_src_push_buffer(GST_APP_SRC(renderer->video_appsrc), buffer);
        }

        free_pending_frame(frame);
    }
}

void mux_renderer_init(logger_t *render_logger, const char *filename, bool use_audio, bool use_video) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    logger = render_logger;
    no_audio = !use_audio;
    no_video = !use_video;
    pipeline_ready = FALSE;
    pipeline_running = FALSE;
    pushed_video_frames = 0;
    pushed_audio_buffers = 0;
    recording_start_time_forced = FALSE;
    forced_recording_start_time = GST_CLOCK_TIME_NONE;
    last_audio_lead_in_seconds = 0.0;
    insert_leading_silence_in_output = TRUE;

    if (no_audio && no_video) {
        logger_log(logger, LOGGER_INFO, "both audio and video rendering are disabled: nothing to record: (not starting mux renderer)");
        return;
    } else if (no_audio) {
        logger_log(logger, LOGGER_INFO, "audio rendering is disabled: video only will be recorded");
    } else if (no_video) {
        logger_log(logger, LOGGER_INFO, "video rendering is disabled: audio only will be recorded");
    }

    output_filename = filename;
    file_count = 0;

    if (!pending_video) {
        pending_video = g_queue_new();
    }
    clear_pending_video();

    logger_log(logger, LOGGER_INFO, "Mux renderer initialized: %s", output_filename);
    g_rec_mutex_unlock(&mux_lock);
}

void mux_renderer_choose_audio_codec(unsigned char audio_ct) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    if (no_audio) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    audio_is_alac = (audio_ct == 2);
    if (renderer && renderer->pipeline && renderer->is_alac != audio_is_alac) {
        logger_log(logger, LOGGER_DEBUG, "Audio codec changed; recreating mux renderer");
        mux_renderer_destroy();
    }

    if (no_video) {
        pipeline_ready = TRUE;
        logger_log(logger, LOGGER_INFO,
                   "Mux audio-only path is now ready (codec=%s)",
                   audio_is_alac ? "alac" : "aac");
    }

    if (no_video && !pipeline_running && pipeline_ready) {
        logger_log(logger, LOGGER_INFO, "Mux audio-only pipeline starting after audio codec negotiation");
        do_start_pipeline(TRUE);
        flush_pending_video();
    } else if (audio_ct == 2 && !pipeline_running && pipeline_ready) {
        do_start_pipeline(TRUE);
        flush_pending_video();
    }
    g_rec_mutex_unlock(&mux_lock);
}

void mux_renderer_choose_video_codec(bool is_h265) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    video_is_h265 = is_h265;
    if (renderer && renderer->pipeline && renderer->is_h265 != video_is_h265) {
        logger_log(logger, LOGGER_DEBUG, "Video codec changed; recreating mux renderer");
        mux_renderer_destroy();
    }
    logger_log(logger, LOGGER_DEBUG, "Mux video codec: h265=%s", is_h265 ? "true" : "false");
    pipeline_ready = TRUE;

    if (no_audio && !pipeline_running) {
        do_start_pipeline(FALSE);
        flush_pending_video();
    }
    g_rec_mutex_unlock(&mux_lock);
}

void mux_renderer_begin_immediate_recording(bool expect_audio) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    if (!pipeline_ready || pipeline_running || !output_filename) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    if (expect_audio) {
        audio_is_alac = FALSE;
        logger_log(logger, LOGGER_INFO, "Mux immediate start: opening audio+video pipeline before first audio buffer arrives");
        do_start_pipeline(TRUE);
    } else {
        logger_log(logger, LOGGER_INFO, "Mux immediate start: opening video-only pipeline immediately");
        do_start_pipeline(FALSE);
    }
    flush_pending_video();
    g_rec_mutex_unlock(&mux_lock);
}

void mux_renderer_push_video(unsigned char *data, int data_len, uint64_t ntp_time) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    if (no_video) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    if (!pipeline_running) {
        if (!pipeline_ready || !output_filename) {
            g_rec_mutex_unlock(&mux_lock);
            return;
        }

        pending_frame_t *frame = g_new(pending_frame_t, 1);
        frame->data = (unsigned char *) g_malloc(data_len);
        memcpy(frame->data, data, data_len);
        frame->data_len = data_len;
        frame->ntp_time = ntp_time;

        if (!pending_video) {
            pending_video = g_queue_new();
        }
        g_queue_push_tail(pending_video, frame);
        pending_count++;

        if (pending_count > MAX_PENDING_FRAMES) {
            pending_frame_t *oldest = (pending_frame_t *) g_queue_pop_head(pending_video);
            free_pending_frame(oldest);
            pending_count--;
        }
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    if (!renderer || !renderer->pipeline || !renderer->video_appsrc) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, data_len, NULL);
    if (!buffer) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }
    gst_buffer_fill(buffer, 0, data, data_len);

    if (renderer->base_time == GST_CLOCK_TIME_NONE) {
        renderer->base_time = (GstClockTime) ntp_time;
        renderer->first_video_time = (GstClockTime) ntp_time;
    }

    GstClockTime pts = (GstClockTime) ntp_time - renderer->base_time;
    renderer->last_video_time = (GstClockTime) ntp_time;
    if (!renderer->logged_first_video) {
        logger_log(logger, LOGGER_INFO,
                   "Mux timing: first live video ntp=%" G_GUINT64_FORMAT " pts=%.3f base_time=%" G_GUINT64_FORMAT,
                   (guint64) ntp_time, (double) pts / GST_SECOND, (guint64) renderer->base_time);
        renderer->logged_first_video = TRUE;
    }
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    gst_app_src_push_buffer(GST_APP_SRC(renderer->video_appsrc), buffer);
    pushed_video_frames++;
    g_rec_mutex_unlock(&mux_lock);
}

void mux_renderer_push_audio(unsigned char *data, int data_len, uint64_t ntp_time) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    if (no_audio) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    if (!pipeline_running) {
        if (!pipeline_ready || !output_filename) {
            g_rec_mutex_unlock(&mux_lock);
            return;
        }
        do_start_pipeline(TRUE);
        flush_pending_video();
        if (!renderer || !renderer->audio_appsrc) {
            g_rec_mutex_unlock(&mux_lock);
            return;
        }
    }

    if (!renderer || !renderer->pipeline || !renderer->audio_appsrc) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    if (renderer->base_time == GST_CLOCK_TIME_NONE) {
        if (recording_start_time_forced && insert_leading_silence_in_output) {
            renderer->base_time = forced_recording_start_time;
        } else {
            renderer->base_time = (GstClockTime) ntp_time;
        }
    }

    if (!renderer->audio_started) {
        renderer->audio_started = TRUE;
        renderer->first_audio_time = (GstClockTime) ntp_time;
        GstClockTime reference_time = GST_CLOCK_TIME_NONE;
        if (recording_start_time_forced) {
            reference_time = forced_recording_start_time;
        } else if (renderer->first_video_time != GST_CLOCK_TIME_NONE) {
            reference_time = renderer->first_video_time;
        } else {
            reference_time = renderer->base_time;
        }
        if (reference_time != GST_CLOCK_TIME_NONE && renderer->first_audio_time > reference_time) {
            GstClockTime silence_duration = renderer->first_audio_time - reference_time;
            last_audio_lead_in_seconds = (gdouble) silence_duration / GST_SECOND;
            if (insert_leading_silence_in_output) {
                guint64 num_samples = (silence_duration * 44100) / GST_SECOND;
                gsize silence_size = num_samples * 2 * 2;
                GstBuffer *silence = gst_buffer_new_allocate(NULL, silence_size, NULL);
                if (silence) {
                    GstMapInfo map;
                    if (gst_buffer_map(silence, &map, GST_MAP_WRITE)) {
                        memset(map.data, 0, map.size);
                        gst_buffer_unmap(silence, &map);
                    }
                    GST_BUFFER_PTS(silence) = 0;
                    GST_BUFFER_DTS(silence) = 0;
                    GST_BUFFER_DURATION(silence) = silence_duration;
                    gst_app_src_push_buffer(GST_APP_SRC(renderer->audio_appsrc), silence);
                }
                logger_log(logger, LOGGER_INFO,
                           "Mux timing: inserted %.3fs of leading silence before audio (first_audio_ntp=%" G_GUINT64_FORMAT
                           " reference_ntp=%" G_GUINT64_FORMAT ")",
                           (double) silence_duration / GST_SECOND,
                           (guint64) renderer->first_audio_time,
                           (guint64) reference_time);
            } else {
                logger_log(logger, LOGGER_INFO,
                           "Mux timing: measured %.3fs audio lead-in without inserting silence (first_audio_ntp=%" G_GUINT64_FORMAT
                           " reference_ntp=%" G_GUINT64_FORMAT ")",
                           (double) silence_duration / GST_SECOND,
                           (guint64) renderer->first_audio_time,
                           (guint64) reference_time);
            }
        } else {
            last_audio_lead_in_seconds = 0.0;
            logger_log(logger, LOGGER_INFO,
                       "Mux timing: first audio arrived without leading silence (first_audio_ntp=%" G_GUINT64_FORMAT
                       " reference_ntp=%" G_GUINT64_FORMAT ")",
                       (guint64) renderer->first_audio_time,
                       (guint64) reference_time);
        }
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, data_len, NULL);
    if (!buffer) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    gst_buffer_fill(buffer, 0, data, data_len);
    GstClockTime pts = (GstClockTime) ntp_time - renderer->base_time;
    renderer->last_audio_time = (GstClockTime) ntp_time;
    if (!renderer->logged_first_audio) {
        logger_log(logger, LOGGER_INFO,
                   "Mux timing: first audio ntp=%" G_GUINT64_FORMAT " pts=%.3f base_time=%" G_GUINT64_FORMAT,
                   (guint64) ntp_time, (double) pts / GST_SECOND, (guint64) renderer->base_time);
        renderer->logged_first_audio = TRUE;
    }
#ifdef USE_AUDIOTOOLBOX_DECODER
    if (!renderer->is_alac) {
        if (silent_eld_packet_len == 0) {
            silent_eld_packet_len = at_make_silent_packet(AT_CT_AAC_ELD, silent_eld_packet, sizeof(silent_eld_packet));
        }
        GstClockTime next = renderer->next_audio_pts;
        if (silent_eld_packet_len > 0 && next != GST_CLOCK_TIME_NONE &&
            pts > next + AAC_ELD_FRAME_NS / 2 && pts - next < MAX_FILLED_GAP) {
            guint filled = 0;
            while (next + AAC_ELD_FRAME_NS / 2 < pts) {
                GstBuffer *silence = gst_buffer_new_memdup(silent_eld_packet, silent_eld_packet_len);
                GST_BUFFER_PTS(silence) = next;
                GST_BUFFER_DTS(silence) = next;
                gst_app_src_push_buffer(GST_APP_SRC(renderer->audio_appsrc), silence);
                next += AAC_ELD_FRAME_NS;
                filled++;
            }
            if (filled > 4) {
                logger_log(logger, LOGGER_INFO, "Mux timing: filled %.2fs of silence in recording (no audio from sender)",
                           (double) filled * AAC_ELD_FRAME_NS / GST_SECOND);
            }
        }
        renderer->next_audio_pts = pts + AAC_ELD_FRAME_NS;
    }
#endif
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    gst_app_src_push_buffer(GST_APP_SRC(renderer->audio_appsrc), buffer);
    pushed_audio_buffers++;
    g_rec_mutex_unlock(&mux_lock);
}

void mux_renderer_stop(void) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    logger_log(logger, LOGGER_INFO, "Mux stop requested: videoFrames=%" G_GUINT64_FORMAT " audioBuffers=%" G_GUINT64_FORMAT " pendingVideo=%d pipelineRunning=%d",
               pushed_video_frames, pushed_audio_buffers, pending_count, pipeline_running);
    if (renderer) {
        logger_log(logger, LOGGER_INFO,
                   "Mux timing summary: base=%" G_GUINT64_FORMAT " firstVideo=%" G_GUINT64_FORMAT " lastVideo=%" G_GUINT64_FORMAT
                   " firstAudio=%" G_GUINT64_FORMAT " lastAudio=%" G_GUINT64_FORMAT,
                   (guint64) renderer->base_time,
                   (guint64) renderer->first_video_time,
                   (guint64) renderer->last_video_time,
                   (guint64) renderer->first_audio_time,
                   (guint64) renderer->last_audio_time);
    }
    if (!pipeline_running && pipeline_ready && pending_count > 0) {
        logger_log(logger, LOGGER_INFO, "No audio received during recording; finalizing as video-only");
        do_start_pipeline(FALSE);
        flush_pending_video();
    }

    clear_pending_video();
    pipeline_ready = FALSE;
    pipeline_running = FALSE;
    recording_start_time_forced = FALSE;
    forced_recording_start_time = GST_CLOCK_TIME_NONE;

    if (!renderer || !renderer->pipeline) {
        g_rec_mutex_unlock(&mux_lock);
        return;
    }

    if (renderer->video_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(renderer->video_appsrc));
    }
    if (renderer->audio_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(renderer->audio_appsrc));
    }

    GstMessage *msg = gst_bus_timed_pop_filtered(renderer->bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *error = NULL;
            gchar *debug_info = NULL;
            gst_message_parse_error(msg, &error, &debug_info);
            logger_log(logger, LOGGER_ERR, "Mux pipeline error on stop: %s", error ? error->message : "Unknown");
            if (debug_info && *debug_info) {
                logger_log(logger, LOGGER_DEBUG, "Mux stop debug info: %s", debug_info);
            }
            g_clear_error(&error);
            g_free(debug_info);
        } else {
            logger_log(logger, LOGGER_DEBUG, "Mux pipeline reached EOS cleanly");
        }
        gst_message_unref(msg);
    } else {
        logger_log(logger, LOGGER_ERR, "Mux pipeline stop timed out waiting for EOS/error");
    }

    gst_element_set_state(renderer->pipeline, GST_STATE_NULL);

    if (renderer->video_appsrc) {
        gst_object_unref(renderer->video_appsrc);
        renderer->video_appsrc = NULL;
    }
    if (renderer->audio_appsrc) {
        gst_object_unref(renderer->audio_appsrc);
        renderer->audio_appsrc = NULL;
    }
    gst_object_unref(renderer->filesink);
    renderer->filesink = NULL;
    gst_object_unref(renderer->bus);
    renderer->bus = NULL;
    gst_object_unref(renderer->pipeline);
    renderer->pipeline = NULL;
    renderer->base_time = GST_CLOCK_TIME_NONE;

    logger_log(logger, LOGGER_INFO, "Stopped recording");
    g_rec_mutex_unlock(&mux_lock);
}

void mux_renderer_destroy(void) {
    ensure_mux_lock();
    g_rec_mutex_lock(&mux_lock);
    mux_renderer_stop();
    clear_pending_video();
    if (renderer) {
        g_free(renderer);
        renderer = NULL;
    }
    pipeline_ready = FALSE;
    pipeline_running = FALSE;
    g_rec_mutex_unlock(&mux_lock);
}
