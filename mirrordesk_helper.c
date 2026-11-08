/*
 * MirrorDesk: the engine helper process (Contents/Helpers/MirrorDeskEngine).
 *
 * The MirrorDesk app starts one of these per AirPlay receiver. It runs the
 * UxPlay engine as a separate program and talks to the app through its
 * standard streams:
 *
 *   stdin   commands from the app, one per line (see handle_command)
 *   stdout  binary messages to the app: video frames and replies
 *           (see renderers/mirrordesk_channel.h)
 *   stderr  the engine's log text
 *
 * It never creates an NSApplication or windows, so it has no Dock icon; the
 * app shows the video. When the app quits, stdin closes and the helper stops.
 *
 * Part of the MirrorDesk modifications to UxPlay; GNU GPL v3 or later.
 */
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "renderers/mirrordesk_channel.h"

/* engine entry points (uxplay.cpp) */
int uxplay_entry(int argc, char *argv[]);
void uxplay_stop_engine(void);
void uxplay_set_playback_volume(double fraction);
void uxplay_set_playback_muted(bool muted);
bool uxplay_start_direct_recording(const char *filename, bool include_audio, bool include_video);
void uxplay_stop_direct_recording(void);
double uxplay_get_last_direct_recording_audio_delay_seconds(void);

static void reply(const char *text) {
    md_channel_send(MD_MSG_REPLY, text, strlen(text), NULL, 0);
}

static void handle_command(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    if (strcmp(line, "stop") == 0) {
        uxplay_stop_engine();
    } else if (strncmp(line, "volume ", 7) == 0) {
        uxplay_set_playback_volume(atof(line + 7));
    } else if (strncmp(line, "mute ", 5) == 0) {
        uxplay_set_playback_muted(atoi(line + 5) != 0);
    } else if (strncmp(line, "record-start ", 13) == 0) {
        /* record-start <audio 0|1> <video 0|1> <file path without extension> */
        int audio = 0, video = 0, offset = 0;
        bool ok = false;
        if (sscanf(line + 13, "%d %d %n", &audio, &video, &offset) >= 2 && line[13 + offset]) {
            ok = uxplay_start_direct_recording(line + 13 + offset, audio != 0, video != 0);
        }
        reply(ok ? "record-start 1" : "record-start 0");
    } else if (strcmp(line, "record-stop") == 0) {
        uxplay_stop_direct_recording();
        char text[64];
        snprintf(text, sizeof text, "record-stop %.6f", uxplay_get_last_direct_recording_audio_delay_seconds());
        reply(text);
    } else if (len > 0) {
        fprintf(stderr, "MirrorDeskEngine: unknown command '%s'\n", line);
    }
}

static void *exit_if_stuck(void *arg) {
    (void) arg;
    sleep(5);
    fprintf(stderr, "MirrorDeskEngine: engine did not stop in time, exiting\n");
    _exit(0);
    return NULL;
}

static void *command_reader(void *arg) {
    (void) arg;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) > 0) {
        handle_command(line);
    }
    free(line);
    /* stdin closed: the app quit or crashed. Stop the engine, and make sure we exit. */
    uxplay_stop_engine();
    pthread_t watchdog;
    pthread_create(&watchdog, NULL, exit_if_stuck, NULL);
    pthread_detach(watchdog);
    return NULL;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    /* Keep the real stdout for binary messages; send everything the engine
     * prints (printf, logs) to stderr instead, so it can't corrupt them. */
    int message_fd = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    md_channel_open(message_fd);

    pthread_t reader;
    pthread_create(&reader, NULL, command_reader, NULL);
    pthread_detach(reader);

    int result = uxplay_entry(argc, argv);
    fflush(stdout);
    fflush(stderr);
    return result;
}
