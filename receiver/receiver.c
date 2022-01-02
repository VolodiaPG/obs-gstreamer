#include "receiver.h"

static GstPad *link_pads(GstElement *rtpbin, const gchar *format, guint id, GstElement *static_pad_from, gchar *type)
{
	GstPad *requested_pad = NULL;

	gchar *pad_name = g_strdup_printf(format, id);
	GstPad *static_pad = gst_element_get_static_pad(static_pad_from, type);
	requested_pad = gst_element_get_request_pad(rtpbin, pad_name);
	if (strcmp(type, "src") == 0)
	{
		gst_pad_link(static_pad, requested_pad);
	}
	else
	{
		gst_pad_link(requested_pad, static_pad);
	}

	g_object_unref(static_pad);
	g_free(pad_name);

	return requested_pad;
}

static void unlink_release_and_unref(GstElement *rtpbin, GstPad *requested_pad, GstElement *static_pad_from, gchar *type)
{
	GstPad *static_pad = gst_element_get_static_pad(static_pad_from, type);
	if (strcmp(type, "src") == 0)
	{
		gst_pad_unlink(static_pad, requested_pad);
	}
	else
	{
		gst_pad_unlink(requested_pad, static_pad);
	}

	gst_element_release_request_pad(rtpbin, requested_pad);

	g_object_unref(static_pad);
	g_object_unref(requested_pad);
}
static void cb_new_pad(GstElement *element, GstPad *pad, gpointer raw_data)
{
	gchar *name;
	cb_new_pad_t *data = raw_data;

	name = gst_pad_get_name(pad);

	guint id;
	guint ssrc;
	guint pt;

	if (sscanf(name, "recv_rtp_src_%u_%u_%u", &id, &ssrc, &pt) > 0 //
		&& id == data->id)
	{
		if (gst_pad_is_linked(data->sink))
		{
			LOGW("Sink pad is already linked\n");

			gst_pad_unlink(data->source, data->sink);
		}

		LOGI("A new pad %s was created for %s\n", name, gst_element_get_name(element));

		LOGW("element %s will be linked to %s\n",
			 gst_element_get_name(element),
			 gst_element_get_name(data->sink));
		data->source = pad;
		gst_pad_link(pad, data->sink);
	}

	g_free(name);
	// gst_element_link(element, sink);
}

GstElement *create_streaminsync_pipeline(pipeline_config_t *config)
{
	GstElement *pipe = gst_pipeline_new("pipe");

	gst_pipeline_set_latency(GST_PIPELINE(pipe), config->latency * GST_MSECOND);

	GstClock *clock = gst_ntp_clock_new("main_ntp_clock", config->clock_ip, config->clock_port, 0);

	gst_pipeline_use_clock(GST_PIPELINE(pipe), clock);
	gst_clock_wait_for_sync(clock, GST_CLOCK_TIME_NONE);

	GstElement *rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
	g_object_set(rtpbin, "ntp-time-source", 3, // clock-time
				 "ntp-sync", TRUE,
				 "buffer-mode", 4, // synced
				 "max-rtcp-rtp-time-diff", 1,
				 "do-lost", TRUE,
				 "do-retransmission", TRUE,
				 "drop-on-latency", TRUE,
				 NULL);

	gst_bin_add(GST_BIN(pipe), rtpbin);

	return pipe;
}

source_data_t *add_incoming_source(GstElement *pipe, receiver_config_t *config)
{
	gchar *buf; // a general purpose buffer that can be freed after use

	GstElement *rtpbin = gst_bin_get_by_name(GST_BIN(pipe), "rtpbin");
	if (!rtpbin)
	{
		LOGW("rtpbin not found");
		return NULL;
	}

	source_data_t *data = g_new0(source_data_t, 1);
	data->pipe = pipe;

	// Video
	data->vudpsrc = gst_element_factory_make("udpsrc", NULL);
	GstCaps *vcaps = gst_caps_new_simple("application/x-rtp",
										 "media", G_TYPE_STRING, "video",
										 "clock-rate", G_TYPE_INT, 90000,
										 "encoding-name", G_TYPE_STRING, "H264",
										 "payload", G_TYPE_INT, 96,
										 NULL);

	g_object_set(data->vudpsrc, "caps", vcaps,
				 "port", config->ports[0], NULL);

	gst_caps_unref(vcaps);

	data->vdepay = gst_element_factory_make("rtph264depay", NULL);
	data->vparse = gst_element_factory_make("h264parse", NULL);
	data->vdec = gst_element_factory_make("avdec_h264", NULL);
	data->vconv = gst_element_factory_make("videoconvert", NULL);

	buf = g_strdup_printf(VSINK_NAME_FORMAT, config->video_id);
	data->vsink = gst_element_factory_make(vsink_bin, buf);
	g_free(buf);

	g_object_set(data->vsink, "sync", FALSE,
				 "async", FALSE, NULL);

	data->vudpsrc_1 = gst_element_factory_make("udpsrc", NULL);
	g_object_set(data->vudpsrc_1, "port", config->ports[1], NULL);

	data->vudpsink = gst_element_factory_make("udpsink", NULL);
	g_object_set(data->vudpsink, "port", config->ports[2],
				 "host", config->dest,
				 "sync", FALSE,
				 "async", FALSE, NULL);

	// Audio
	data->audpsrc = gst_element_factory_make("udpsrc", NULL);
	GstCaps *acaps = gst_caps_new_simple("application/x-rtp",
										 "media", G_TYPE_STRING, "audio",
										 "clock-rate", G_TYPE_INT, 48000,
										 "encoding-name", G_TYPE_STRING, "OPUS",
										 "payload", G_TYPE_INT, 96,
										 NULL);

	g_object_set(data->audpsrc, "caps", acaps,
				 "port", config->ports[3], NULL);
	gst_caps_unref(acaps);

	data->adepay = gst_element_factory_make("rtpopusdepay", NULL);
	data->adec = gst_element_factory_make("opusdec", NULL);
	data->aconv = gst_element_factory_make("audioconvert", NULL);
	data->aresample = gst_element_factory_make("audioresample", NULL);

	buf = g_strdup_printf(ASINK_NAME_FORMAT, config->audio_id);
	data->asink = gst_element_factory_make(asink_bin, buf);
	g_free(buf);

	g_object_set(data->asink, "sync", FALSE,
				 "async", FALSE, NULL);

	data->audpsrc_1 = gst_element_factory_make("udpsrc", NULL);
	g_object_set(data->audpsrc_1, "port", config->ports[4], NULL);

	data->audpsink = gst_element_factory_make("udpsink", NULL);
	g_object_set(data->audpsink, "port", config->ports[5],
				 "host", config->dest,
				 "sync", FALSE,
				 "async", FALSE, NULL);

	if (!pipe || !data->vudpsrc || !data->vdepay || !data->vparse || !data->vdec || !data->vconv || !data->vsink ||
		!data->vudpsrc_1 || !data->vudpsink || !data->audpsrc || !data->adepay || !data->adec || !data->aconv || !data->aresample || !data->asink ||
		!data->audpsrc_1 || !data->audpsink)
	{
		GST_WARNING("Not all elements could be created.\n");
		return NULL;
	}

	gst_bin_add_many(GST_BIN(pipe),
					 // video
					 data->vudpsrc,
					 data->vdepay,
					 data->vparse,
					 data->vdec,
					 data->vconv,
					 data->vudpsrc_1,
					 data->vudpsink,
					 data->vsink,
					 // audio
					 data->audpsrc,
					 data->adepay,
					 data->adec,
					 data->aconv,
					 data->aresample,
					 data->audpsrc_1,
					 data->audpsink,
					 data->asink,
					 NULL);
	if (!gst_element_link_many(data->vdepay, data->vparse, data->vdec, data->vconv, data->vsink, NULL) //
		|| !gst_element_link_many(data->adepay, data->adec, data->aconv, data->aresample, data->asink, NULL))
	{
		GST_WARNING("can't link elements");
		return NULL;
	}
	// linking

	// RTP bin pads
	data->vrecv_rtp_sink = link_pads(rtpbin, "recv_rtp_sink_%d", config->video_id, data->vudpsrc, "src");
	data->vrecv_rtcp_sink = link_pads(rtpbin, "recv_rtcp_sink_%d", config->video_id, data->vudpsrc_1, "src");
	data->vsend_rtcp_src = link_pads(rtpbin, "send_rtcp_src_%d", config->video_id, data->vudpsink, "sink");

	data->arecv_rtp_sink = link_pads(rtpbin, "recv_rtp_sink_%d", config->audio_id, data->audpsrc, "src");
	data->arecv_rtcp_sink = link_pads(rtpbin, "recv_rtcp_sink_%d", config->audio_id, data->audpsrc_1, "src");
	data->asend_rtcp_src = link_pads(rtpbin, "send_rtcp_src_%d", config->audio_id, data->audpsink, "sink");

	GstPad *vdepay_pad = gst_element_get_static_pad(data->vdepay, "sink");
	GstPad *adepay_pad = gst_element_get_static_pad(data->adepay, "sink");

	data->config = config;

	data->cb_video.id = config->video_id;
	data->cb_video.sink = vdepay_pad;
	data->cb_video.source = NULL;
	data->cb_video.parent = data;
	data->cb_audio.id = config->audio_id;
	data->cb_audio.sink = adepay_pad;
	data->cb_audio.source = NULL;
	data->cb_audio.parent = data;

	data->vdepay_cb_id = g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), &data->cb_video);
	data->adepay_cb_id = g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), &data->cb_audio);

	data->video_id = config->video_id;
	data->audio_id = config->audio_id;
	return data;
}

static void set_state_many(GstState state, GstElement *element, ...)
{
	va_list args;

	va_start(args, element);

	while (element)
	{
		gst_element_set_state(element, state);

		element = va_arg(args, GstElement *);
	}

	va_end(args);
}

#define SOURCE_ELEMS data->vudpsrc,   \
					 data->vdepay,    \
					 data->vparse,    \
					 data->vdec,      \
					 data->vconv,     \
					 data->vsink,     \
					 data->vudpsrc_1, \
					 data->vudpsink,  \
					 data->audpsrc,   \
					 data->adepay,    \
					 data->adec,      \
					 data->aconv,     \
					 data->aresample, \
					 data->asink,     \
					 data->audpsink,  \
					 data->audpsrc_1
void set_source_to(source_data_t *data, GstState state)
{
	set_state_many(state, SOURCE_ELEMS, NULL);
}

void remove_incoming_source(source_data_t *data)
{
	GstElement *rtpbin = gst_bin_get_by_name(GST_BIN(data->pipe), "rtpbin");
	if (!rtpbin)
	{
		LOGW("rtpbin not found");
	}

	g_signal_handler_disconnect(rtpbin, data->vdepay_cb_id);
	g_signal_handler_disconnect(rtpbin, data->adepay_cb_id);

	set_state_many(GST_STATE_NULL, SOURCE_ELEMS, NULL);

	if (data->cb_video.source)
	{
		LOGD("removing calback video source");
		gst_pad_unlink(data->cb_video.source, data->cb_video.sink);
		gst_element_release_request_pad(rtpbin, data->cb_video.sink);
		gst_object_unref(data->cb_video.sink);
	}
	if (data->cb_audio.source)
	{
		LOGD("removing calback audio source");
		gst_pad_unlink(data->cb_audio.source, data->cb_audio.sink);
		gst_element_release_request_pad(rtpbin, data->cb_audio.sink);
		gst_object_unref(data->cb_audio.sink);
	}

	unlink_release_and_unref(rtpbin, data->vrecv_rtp_sink, data->vudpsrc, "src");
	unlink_release_and_unref(rtpbin, data->vrecv_rtcp_sink, data->vudpsrc_1, "src");
	unlink_release_and_unref(rtpbin, data->vsend_rtcp_src, data->vudpsink, "sink");

	unlink_release_and_unref(rtpbin, data->arecv_rtp_sink, data->audpsrc, "src");
	unlink_release_and_unref(rtpbin, data->arecv_rtcp_sink, data->audpsrc_1, "src");
	unlink_release_and_unref(rtpbin, data->asend_rtcp_src, data->audpsink, "sink");

	gst_bin_remove_many(GST_BIN(data->pipe), SOURCE_ELEMS, NULL);

	gst_object_unref(rtpbin);
}
#undef SOURCE_ELEMS