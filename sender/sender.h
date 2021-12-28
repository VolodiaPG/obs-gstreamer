#ifndef _SENDER_H
#define _SENDER_H

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>
#include <gst/net/gstnet.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ANDROID
#include <android/log.h>
#define TAG "MTAG"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG,  __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG,  __VA_ARGS__)
#define VIDEO_ID 0
#define AUDIO_ID 1
#else
#define LOGD(...) GST_DEBUG(__VA_ARGS__)
#define LOGI(...) GST_INFO(__VA_ARGS__)
#define LOGW(...) GST_WARNING(__VA_ARGS__)
#define LOGE(...) GST_ERROR(__VA_ARGS__)
#define VIDEO_ID 0
#define AUDIO_ID 1
#endif

#define NB_PORTS 6
typedef struct
{
    gboolean restart_on_eos;
    gboolean restart_on_error;
    guint64 restart_timeout;
    const gchar *clock_ip;
    gint clock_port;
    const gchar *receiver_ip;
    // Used ports, in order:
    // 0: video
    // 1: video
    // 2: video
    // 3: audio
    // 4: audio
    // 5: audio
    gint receiver_ports[NB_PORTS];
    const gchar *videosource;
    const gchar *audiosource;
    gint bitrate;
    gint framerate;
    gint width;
    gint height;
    gint video_id;
    gint audio_id;
} settings_t;

typedef struct
{
    GstElement *pipe;
    settings_t *settings;
    GSource *timeout;
    GThread *thread;
    GMainLoop *loop;
    GMutex mutex;
    GCond cond;
} data_t;

data_t *gstreamer_source_create(settings_t *settings);
void start(data_t *data);
void stop(data_t *data);
gboolean create_pipeline(data_t *data);
void gstreamer_source_destroy(void *user_data);
void gstreamer_source_get_defaults(settings_t *settings);

#endif
