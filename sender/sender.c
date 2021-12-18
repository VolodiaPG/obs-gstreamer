/*
 * obs-gstreamer. OBS Studio plugin.
 * Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-gstreamer.
 *
 * obs-gstreamer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-gstreamer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"

#define NB_PORTS 6
typedef struct
{
    bool restart_on_eos;
    bool restart_on_error;
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
} settings_t;

typedef struct
{
    GstElement *pipe;
    settings_t *settings;
    gint64 frame_count;
    gint64 audio_count;
    GSource *timeout;
    GThread *thread;
    GMainLoop *loop;
    GMutex mutex;
    GCond cond;
} data_t;

static void create_pipeline(data_t *data);

static void timeout_destroy(gpointer user_data)
{
    data_t *data = user_data;

    g_source_destroy(data->timeout);
    g_source_unref(data->timeout);
    data->timeout = NULL;
}

static gboolean start_pipe(gpointer user_data)
{
    data_t *data = user_data;

    GstBus *bus = gst_element_get_bus(data->pipe);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);

    gst_object_unref(data->pipe);
    data->pipe = NULL;

    create_pipeline(data);

    if (data->pipe)
        gst_element_set_state(data->pipe, GST_STATE_PLAYING);

    return G_SOURCE_REMOVE;
}

static gboolean bus_callback(GstBus *bus, GstMessage *message,
                             gpointer user_data)
{
    data_t *data = user_data;

    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
    {
        GError *err;
        gst_message_parse_error(message, &err, NULL);
        log_error(err->message);
        g_error_free(err);
    } // fallthrough
    case GST_MESSAGE_EOS:
        gst_element_set_state(data->pipe, GST_STATE_NULL);
        if ((GST_MESSAGE_TYPE(message) ==
                     GST_MESSAGE_ERROR
                 ? data->settings->restart_on_error
                 : data->settings->restart_on_eos) &&
            data->timeout == NULL)
        {
            data->timeout = data->settings->restart_timeout;
            g_source_set_callback(data->timeout, start_pipe, data,
                                  timeout_destroy);
            g_source_attach(data->timeout,
                            g_main_context_get_thread_default());
        }
        break;
    case GST_MESSAGE_WARNING:
    {
        GError *err;
        gst_message_parse_warning(message, &err, NULL);
        log_warn(err->message);
        g_error_free(err);
    }
    break;
    default:
        break;
    }

    return TRUE;
}

static gboolean loop_startup(gpointer user_data)
{
    data_t *data = user_data;

    create_pipeline(data);

    g_mutex_lock(&data->mutex);
    g_cond_signal(&data->cond);
    g_mutex_unlock(&data->mutex);

    if (data->pipe)
        gst_element_set_state(data->pipe, GST_STATE_PLAYING);

    return G_SOURCE_REMOVE;
}

static void cb_new_pad(GstElement *element, GstPad *pad, gpointer data)
{
    gchar *name;
    GstPad *sink = data;

    name = gst_pad_get_name(pad);
    g_print("A new pad %s was created for %s\n", name, gst_element_get_name(element));
    g_free(name);

    g_print("element %s will be linked to %s\n",
            gst_element_get_name(element),
            gst_element_get_name(sink));
    gst_pad_link(pad, sink);
    // gst_element_link(element, sink);
}

static void create_pipeline(data_t *data)
{
    GError *err = NULL;

    GstElement *pipe = gst_pipeline_new("pipe");

    GstClock *clock = gst_ntp_clock_new("main_ntp_clock", data->settings->clock_ip, data->settings->clock_port, 0);
    gst_pipeline_use_clock(GST_PIPELINE(pipe), clock);

    GstElement *rtpbin = gst_element_factory_make("rtpbin", NULL);
    g_object_set(rtpbin, "rtp-profile", "avpf", NULL);
    g_object_set(rtpbin, "rtcp-sync-send-time", FALSE, NULL);
    g_object_set(rtpbin, "ntp-time-source", 3, NULL); // 3 = clock-time

    // VIDEO

    GstElement *vsource = gst_element_factory_make("videotestsrc", NULL);
    GstCaps *vcaps = gst_caps_new_simple("video/x-raw",
                                         "format", G_TYPE_STRING, "I420",
                                         "width", G_TYPE_INT, 1920,
                                         "height", G_TYPE_INT, 1080,
                                         "framerate", GST_TYPE_FRACTION, 30, 1,
                                         NULL);
    GstElement *vcapsfilter = gst_element_factory_make("capsfilter", NULL);
    g_object_set(vcapsfilter, "caps", vcaps, NULL);
    gst_caps_unref(vcaps); // TODO check if this is needed

    GstElement *vscale = gst_element_factory_make("videoscale", NULL);
    GstElement *vconvert = gst_element_factory_make("videoconvert", NULL);
    GstElement *vqueue = gst_element_factory_make("queue", NULL);

    GstElement *venc = gst_element_factory_make("x264enc", NULL);
    g_object_set(venc,
                 "tune", 0,
                 "profile", 0,
                 "key-int-max", 30,
                 "bframes", 30,
                 "byte-stream", TRUE,
                 "bitrate", 3000,
                 "speed-preset", 0,
                 "threads", 1,
                 "pass", 1, NULL);

    GstElement *vparse = gst_element_factory_make("h264parse", NULL);
    GstElement *vpay = gst_element_factory_make("rtph264pay", NULL);
    g_object_set(vpay,
                 "pt", 96,
                 "config-interval", 2,
                 NULL);
    GstElement *vrtpqueue = gst_element_factory_make("rtprtxqueue", NULL);

    GstElement *vrtpsink = gst_element_factory_make("udpsink", NULL);
    g_object_set(vrtpsink,
                 "port", data->settings->receiver_ports[0],
                 "host", data->settings->receiver_ip,
                 "ts-offset", 0,
                 NULL);
    GstElement *vrtcpsink = gst_element_factory_make("udpsink", NULL);
    g_object_set(vrtcpsink,
                 "port", data->settings->receiver_ports[1],
                 "host", data->settings->receiver_ip,
                 "sync", FALSE,
                 "async", FALSE,
                 NULL);

    GstElement *vrtcpsrc = gst_element_factory_make("udpsrc", NULL);
    g_object_set(vrtcpsrc, "port", data->settings->receiver_ports[2], NULL);

    // AUDIO

    GstElement *asource = gst_element_factory_make("audiotestsrc", NULL);
    GstElement *aconvert = gst_element_factory_make("audioconvert", NULL);
    GstCaps *acaps = gst_caps_new_simple("audio/x-raw",
                                         "format", G_TYPE_STRING, "S16LE",
                                         "rate", G_TYPE_INT, 48000,
                                         "channels", G_TYPE_INT, 2,
                                         NULL);
    GstElement *acapsfilter = gst_element_factory_make("capsfilter", NULL);
    g_object_set(acapsfilter, "caps", acaps, NULL);

    GstElement *aenc = gst_element_factory_make("opusenc", NULL);
    GstElement *apay = gst_element_factory_make("rtpopuspay", NULL);
    g_object_set(apay, "pt", 96, NULL);

    GstElement *artpqueue = gst_element_factory_make("rtprtxqueue", NULL);
    GstElement *artpsink = gst_element_factory_make("udpsink", NULL);
    g_object_set(artpsink,
                 "port", data->settings->receiver_ports[3],
                 "host", data->settings->receiver_ip,
                 "ts-offset", 0,
                 NULL);

    GstElement *artcpsink = gst_element_factory_make("udpsink", NULL);
    g_object_set(artcpsink,
                 "port", data->settings->receiver_ports[4],
                 "host", data->settings->receiver_ip,
                 "sync", FALSE,
                 "async", FALSE,
                 NULL);

    GstElement *artcpsrc = gst_element_factory_make("udpsrc", NULL);
    g_object_set(artcpsrc, "port", data->settings->receiver_ports[5], NULL);

    // Add all elements to the pipe
    gst_bin_add_many(GST_BIN(pipe),
                     rtpbin,
                     vsource,
                     vcapsfilter,
                     vscale,
                     vconvert,
                     vqueue,
                     venc,
                     vparse,
                     vpay,
                     vrtpqueue,
                     vrtcpsink,
                     vrtpsink,
                     vrtcpsrc,

                     asource,
                     aconvert,
                     acapsfilter,
                     aenc,
                     apay,
                     artpqueue,
                     artpsink,
                     artcpsink,
                     artcpsrc,
                     NULL);

    if (!gst_element_link_many(vsource, vscale, vconvert, vcapsfilter, vqueue, venc, vparse, vpay, NULL) //
        || !gst_element_link_many(asource, aconvert, acapsfilter, aenc, apay, NULL))
    {
        log_warn("can't link elements");
    }

    gst_element_link_pads(vrtpqueue, "src", rtpbin, "send_rtp_sink_0");
    gst_element_link_pads(rtpbin, "send_rtp_src_0", vrtcpsink, "sink");
    gst_element_link_pads(rtpbin, "send_rtcp_src_0", vrtpsink, "sink");
    gst_element_link_pads(vrtcpsrc, "src", rtpbin, "recv_rtcp_sink_0");

    gst_element_link_pads(artpqueue, "src", rtpbin, "send_rtp_sink_1");
    gst_element_link_pads(rtpbin, "send_rtp_src_1", artcpsink, "sink");
    gst_element_link_pads(rtpbin, "send_rtcp_src_1", artpsink, "sink");
    gst_element_link_pads(artcpsrc, "src", rtpbin, "recv_rtcp_sink_1");

    GstPad *vscalesink = gst_element_get_static_pad(vscale, "sink");
    GstPad *aconvertsink = gst_element_get_static_pad(aconvert, "sink");
    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), vscalesink);
    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), aconvertsink);
    gst_object_unref(vscalesink);
    gst_object_unref(aconvertsink);

    if (err != NULL)
    {
        log_error("Cannot start GStreamer: %s", err->message);
        g_error_free(err);

        return;
    }

    data->frame_count = 0;
    data->audio_count = 0;

    GstBus *bus = gst_element_get_bus(data->pipe);
    gst_bus_add_watch(bus, bus_callback, data);
    gst_object_unref(bus);
}

static gpointer _start(gpointer user_data)
{
    data_t *data = user_data;

    GMainContext *context = g_main_context_new();

    g_main_context_push_thread_default(context);

    data->loop = g_main_loop_new(context, FALSE);

    GSource *source = g_idle_source_new();
    g_source_set_callback(source, loop_startup, data, NULL);
    g_source_attach(source, context);

    g_main_loop_run(data->loop);

    if (data->pipe != NULL)
    {
        gst_element_set_state(data->pipe, GST_STATE_NULL);

        GstBus *bus = gst_element_get_bus(data->pipe);
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);

        gst_object_unref(data->pipe);
        data->pipe = NULL;
    }

    g_main_loop_unref(data->loop);
    data->loop = NULL;

    g_main_context_unref(context);

    return NULL;
}

static void start(data_t *data)
{
    g_mutex_lock(&data->mutex);

    data->thread = g_thread_new("GStreamer Source", _start, data);

    g_cond_wait(&data->cond, &data->mutex);
    g_mutex_unlock(&data->mutex);
}

data_t *gstreamer_source_create(settings_t *settings)
{
    data_t *data = g_new0(data_t, 1);

    data->settings = settings;

    g_mutex_init(&data->mutex);
    g_cond_init(&data->cond);

    return data;
}

static void stop(data_t *data)
{
    if (data->thread == NULL)
        return;

    g_main_loop_quit(data->loop);

    g_thread_join(data->thread);
    data->thread = NULL;
}

void gstreamer_source_destroy(void *user_data)
{
    data_t *data = user_data;

    stop(data);

    g_mutex_clear(&data->mutex);
    g_cond_clear(&data->cond);

    g_free(data);
}

void gstreamer_source_get_defaults(settings_t *settings)
{
    settings->restart_on_eos = true;
    settings->restart_on_error = true;
    settings->restart_timeout = 2000;
    settings->receiver_ip = "127.0.0.1";

    for (int ii = 0; ii < NB_PORTS; ii++)
        settings->receiver_ports[ii] = 5000 + ii;
    settings->clock_ip = "45.159.204.28";
    settings->clock_port = 123;
}

int main()
{
    settings_t settings;
    gstreamer_source_get_defaults(&settings);
    data_t *data = gstreamer_source_create(&settings);

    create_pipeline(data);
    start(data);

    printf("Press enter to end the sender.\n");
    getchar();

    stop(data);

    g_free(data);

    return EXIT_SUCCESS;
}
