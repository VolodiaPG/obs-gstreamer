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
#include "sender.h"

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
        LOGE("%s", err->message);
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
            data->timeout = g_timeout_source_new(data->settings->restart_timeout);
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
        LOGW("%s", err->message);
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

gboolean create_pipeline(data_t *data)
{
    GError *err = NULL;

    data->pipe = gst_pipeline_new("pipe");

    GstClock *clock = gst_ntp_clock_new("main_ntp_clock", data->settings->clock_ip, data->settings->clock_port, 0);
    // gst_system_clock_set_default(clock);
    gst_pipeline_use_clock(GST_PIPELINE(data->pipe), clock);
    gst_clock_wait_for_sync(clock, GST_CLOCK_TIME_NONE);

    if (data->settings->video_id == 0)
    {
        gst_pipeline_set_latency(GST_PIPELINE(data->pipe), 500 * GST_MSECOND);
        printf("Added latency");
    }

    GstElement *rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
    //g_object_set(rtpbin, "rtp-profile", 3, NULL); // 3 = RTP/AVPF
    g_object_set(rtpbin, "ntp-time-source", 3, // 3 = clock-time
                 "rtcp-sync-send-time", FALSE, // TRUE = send time, FALSE = capture time
                                               //  "do-lost", TRUE,
                                               //  "do-retransmission", TRUE,
                                               //  "rtp-profile", 3,
                 NULL);

    // VIDEO

    GstElement *vsource = gst_element_factory_make(data->settings->videosource, "vsource");
    GstElement *clockoverlay = gst_element_factory_make("clockoverlay", "clockoverlay");
    GstCaps *vcaps = gst_caps_new_simple("video/x-raw",
                                         "format", G_TYPE_STRING, "I420",
                                         "width", G_TYPE_INT, data->settings->width,
                                         "height", G_TYPE_INT, data->settings->height,
                                         "framerate", GST_TYPE_FRACTION, data->settings->framerate, 1,
                                         NULL);
    GstElement *vcapsfilter = gst_element_factory_make("capsfilter", "vcapsfilter");
    g_object_set(vcapsfilter, "caps", vcaps, NULL);
    gst_caps_unref(vcaps); // TODO check if this is needed

    GstElement *vscale = gst_element_factory_make("videoscale", "vscale");
    GstElement *vconvert = gst_element_factory_make("videoconvert", "vconvert");
    GstElement *vqueue = gst_element_factory_make("queue", "vqueue");

    GstElement *venc = gst_element_factory_make("x264enc", "venc");
    g_object_set(venc,
                 "tune", 4,
                 //   "profile", 0,
                 //   "key-int-max", 30,
                 //   "bframes", 2,
                 //   "byte-stream", TRUE,
                 "bitrate", data->settings->bitrate,
                 "speed-preset", 2, // superfast
                                    //   "threads", 1,
                                    //   "pass", 0, // O: cbr
                 NULL);
    GstElement *venccapsfilter = gst_element_factory_make("capsfilter", "venccapsfilter");
    g_object_set(venccapsfilter, "caps", gst_caps_new_simple("video/x-h264", "profile", G_TYPE_STRING, "high", NULL),
                 NULL);
    GstElement *vparse = gst_element_factory_make("h264parse", "vparse");
    GstElement *vpay = gst_element_factory_make("rtph264pay", "vpay");
    g_object_set(vpay,
                 "pt", 96,
                 "config-interval", 2,
                 NULL);
    GstElement *vrtpqueue = gst_element_factory_make("rtprtxqueue", "vrtpqueue");

    GstElement *vrtpsink = gst_element_factory_make("udpsink", "vrtpsink");
    g_object_set(vrtpsink,
                 "port", data->settings->receiver_ports[0],
                 "host", data->settings->receiver_ip,
                 "ts-offset", 0,
                 NULL);
    GstElement *vrtcpsink = gst_element_factory_make("udpsink", "vrtcpsink");
    g_object_set(vrtcpsink,
                 "port", data->settings->receiver_ports[1],
                 "host", data->settings->receiver_ip,
                 "sync", FALSE,
                 "async", FALSE,
                 NULL);

    GstElement *vrtcpsrc = gst_element_factory_make("udpsrc", "vrtcpsrc");
    g_object_set(vrtcpsrc, "port", data->settings->receiver_ports[2], NULL);

    // AUDIO

    GstElement *asource = gst_element_factory_make("audiotestsrc", "asource");
    GstElement *aconvert = gst_element_factory_make("audioconvert", "aconvert");
    GstCaps *acaps = gst_caps_new_simple("audio/x-raw",
                                         "format", G_TYPE_STRING, "S16LE",
                                         "rate", G_TYPE_INT, 48000,
                                         "channels", G_TYPE_INT, 2,
                                         NULL);
    GstElement *acapsfilter = gst_element_factory_make("capsfilter", "acapsfilter");
    g_object_set(acapsfilter, "caps", acaps, NULL);

    GstElement *aenc = gst_element_factory_make("opusenc", "aenc");
    GstElement *apay = gst_element_factory_make("rtpopuspay", "apay");
    g_object_set(apay, "pt", 96, NULL);

    GstElement *artpqueue = gst_element_factory_make("rtprtxqueue", "artpqueue");
    GstElement *artpsink = gst_element_factory_make("udpsink", "artpsink");
    g_object_set(artpsink,
                 "port", data->settings->receiver_ports[3],
                 "host", data->settings->receiver_ip,
                 "ts-offset", 0,
                 NULL);

    GstElement *artcpsink = gst_element_factory_make("udpsink", "artcpsink");
    g_object_set(artcpsink,
                 "port", data->settings->receiver_ports[4],
                 "host", data->settings->receiver_ip,
                 "sync", FALSE,
                 "async", FALSE,
                 NULL);

    GstElement *artcpsrc = gst_element_factory_make("udpsrc", "artcpsrc");
    g_object_set(artcpsrc, "port", data->settings->receiver_ports[5], NULL);

    // Add all elements to the pipe

    GstElement *elements[] = {
        rtpbin,

        vsource,
        clockoverlay,
        vcapsfilter,
        vscale,
        vconvert,
        vqueue,
        venc,
        venccapsfilter,
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
    };

    for (gint ii = 0; ii < sizeof(elements) / sizeof(GstElement *); ii++)
    {
        if (elements[ii] == NULL)
        {
            LOGE("Failed to create element %d\n", ii);
            return FALSE;
        }
        gst_bin_add_many(GST_BIN(data->pipe), elements[ii], NULL);
    }

    if (!gst_element_link_many(vsource, clockoverlay, vscale, vconvert, vcapsfilter, vqueue, venc, venccapsfilter, vparse, vpay, vrtpqueue, NULL) //
        || !gst_element_link_many(asource, aconvert, acapsfilter, aenc, apay, artpqueue, NULL))
    {
        LOGE("can't link elements");
        return FALSE;
    }

    gchar *buf = g_strdup_printf("send_rtp_sink_%d", data->settings->video_id);
    gst_element_link_pads(vrtpqueue, "src", rtpbin, buf);
    g_free(buf);

    buf = g_strdup_printf("send_rtcp_src_%d", data->settings->video_id);
    gst_element_link_pads(rtpbin, buf, vrtcpsink, "sink");
    g_free(buf);

    buf = g_strdup_printf("send_rtp_src_%d", data->settings->video_id);
    gst_element_link_pads(rtpbin, buf, vrtpsink, "sink");
    g_free(buf);

    buf = g_strdup_printf("recv_rtcp_sink_%d", data->settings->video_id);
    gst_element_link_pads(vrtcpsrc, "src", rtpbin, buf);
    g_free(buf);

    buf = g_strdup_printf("send_rtp_sink_%d", data->settings->audio_id);
    gst_element_link_pads(artpqueue, "src", rtpbin, buf);
    g_free(buf);

    buf = g_strdup_printf("send_rtcp_src_%d", data->settings->audio_id);
    gst_element_link_pads(rtpbin, buf, artcpsink, "sink");
    g_free(buf);

    buf = g_strdup_printf("send_rtp_src_%d", data->settings->audio_id);
    gst_element_link_pads(rtpbin, buf, artpsink, "sink");
    g_free(buf);

    buf = g_strdup_printf("recv_rtcp_sink_%d", data->settings->audio_id);
    gst_element_link_pads(artcpsrc, "src", rtpbin, buf);
    g_free(buf);

    GstPad *vscalesink = gst_element_get_static_pad(vscale, "sink");
    GstPad *aconvertsink = gst_element_get_static_pad(aconvert, "sink");
    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), vscalesink);
    g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), aconvertsink);
    gst_object_unref(vscalesink);
    gst_object_unref(aconvertsink);

    if (err != NULL)
    {
        LOGE("Cannot start GStreamer: %s", err->message);
        g_error_free(err);

        return FALSE;
    }

    GstBus *bus = gst_element_get_bus(data->pipe);
    gst_bus_add_watch(bus, bus_callback, data);
    gst_object_unref(bus);

    return TRUE;
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

void start(data_t *data)
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

void stop(data_t *data)
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
    settings->restart_on_eos = TRUE;
    settings->restart_on_error = TRUE;
    settings->restart_timeout = 2000;
    settings->clock_ip = "45.159.204.28";
    settings->clock_port = 123;
    settings->receiver_ip = "127.0.0.1";
    for (int ii = 0; ii < NB_PORTS; ii++)
        settings->receiver_ports[ii] = 5000 + ii;
    settings->videosource = "videotestsrc";
    settings->audiosource = "audiotestsrc";
    settings->bitrate = 3000;
    settings->framerate = 30;
    settings->width = 1920;
    settings->height = 1080;
    settings->video_id = VIDEO_ID;
    settings->audio_id = AUDIO_ID;
}
