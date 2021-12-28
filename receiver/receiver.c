#include "receiver.h"

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

GstElement *create_streaminsync_pipeline(pipeline_config_t *config)
{
	GstElement *pipe = gst_pipeline_new("pipe");

	GstClock *clock = gst_ntp_clock_new("main_ntp_clock", config->clock_ip, config->clock_port, 0);

	// gst_system_clock_set_default(clock);
	gst_pipeline_use_clock(GST_PIPELINE(pipe), clock);
	gst_clock_wait_for_sync(clock, GST_CLOCK_TIME_NONE);

	gst_pipeline_set_latency(GST_PIPELINE(pipe), config->latency * GST_MSECOND);

	GstElement *rtpbin = gst_element_factory_make("rtpbin", "rtpbin");
	// g_object_set(rtpbin, "latency", config->latency, NULL);
	// g_object_set(rtpbin, "ntp-time-source", 0, NULL);
	g_object_set(rtpbin, "ntp-time-source", 3, // clock-time
				 "ntp-sync", TRUE,
				 "buffer-mode", 4, // synced
				 "max-rtcp-rtp-time-diff", 1,
				 "do-lost", TRUE,
				 "do-retransmission", FALSE,
				 //  "latency", config->latency,
				 "drop-on-latency", TRUE,
				 NULL);

	gst_bin_add(GST_BIN(pipe), rtpbin);

	return pipe;
}

void add_incoming_source(GstElement *pipe, receiver_config_t *config)
{
	GstElement *rtpbin = gst_bin_get_by_name(GST_BIN(pipe), "rtpbin");
	if (!rtpbin)
	{
		LOGW("rtpbin not found");
		return;
	}

	// Video
	GstElement *vudpsrc = gst_element_factory_make("udpsrc", NULL);
	GstCaps *vcaps = gst_caps_new_simple("application/x-rtp",
										 "media", G_TYPE_STRING, "video",
										 "clock-rate", G_TYPE_INT, 90000,
										 "encoding-name", G_TYPE_STRING, "H264",
										 "payload", G_TYPE_INT, 96,
										 NULL);

	g_object_set(vudpsrc, "caps", vcaps,
				 "port", config->ports[0], NULL);

	GstElement *vdepay = gst_element_factory_make("rtph264depay", NULL);
	GstElement *vparse = gst_element_factory_make("h264parse", NULL);
	GstElement *vdec = gst_element_factory_make("avdec_h264", NULL);
	GstElement *vconv = gst_element_factory_make("videoconvert", NULL);
	// GstElement *vsink = gst_element_factory_make("appsink", NULL);
	GstElement *vsink = gst_element_factory_make("autovideosink", NULL);
	g_object_set(vsink, "sync", FALSE,
				 "async", FALSE, NULL);

	GstElement *vudpsrc_1 = gst_element_factory_make("udpsrc", NULL);
	g_object_set(vudpsrc_1, "port", config->ports[1], NULL);

	GstElement *vudpsink = gst_element_factory_make("udpsink", NULL);
	g_object_set(vudpsink, "port", config->ports[2],
				 "host", config->dest,
				 "sync", FALSE,
				 "async", FALSE, NULL);

	// Audio
	GstElement *audpsrc = gst_element_factory_make("udpsrc", NULL);
	GstCaps *acaps = gst_caps_new_simple("application/x-rtp",
										 "media", G_TYPE_STRING, "audio",
										 "clock-rate", G_TYPE_INT, 48000,
										 "encoding-name", G_TYPE_STRING, "OPUS",
										 "payload", G_TYPE_INT, 96,
										 NULL);

	g_object_set(audpsrc, "caps", acaps,
				 "port", config->ports[3], NULL);

	GstElement *adepay = gst_element_factory_make("rtpopusdepay", NULL);
	GstElement *adec = gst_element_factory_make("opusdec", NULL);
	GstElement *aconv = gst_element_factory_make("audioconvert", NULL);
	GstElement *aresample = gst_element_factory_make("audioresample", NULL);
	GstElement *asink = gst_element_factory_make("appsink", NULL);
	g_object_set(asink, "sync", FALSE,
				 "async", FALSE, NULL);

	GstElement *audpsrc_1 = gst_element_factory_make("udpsrc", NULL);
	g_object_set(audpsrc_1, "port", config->ports[4], NULL);

	GstElement *audpsink = gst_element_factory_make("udpsink", NULL);
	g_object_set(audpsink, "port", config->ports[5],
				 "host", config->dest,
				 "sync", FALSE,
				 "async", FALSE, NULL);

	if (!pipe || !vudpsrc || !vdepay || !vparse || !vdec || !vconv || !vsink ||
		!vudpsrc_1 || !vudpsink || !audpsrc || !adepay || !adec || !aconv || !aresample || !asink ||
		!audpsrc_1 || !audpsink)
	{
		GST_WARNING("Not all elements could be created.\n");
		return;
	}

	gst_bin_add_many(GST_BIN(pipe),
					 // video
					 vudpsrc,
					 vdepay,
					 vparse,
					 vdec,
					 vconv,
					 vudpsrc_1,
					 vudpsink,
					 vsink,
					 // audio
					 audpsrc,
					 adepay,
					 adec,
					 aconv,
					 aresample,
					 audpsrc_1,
					 audpsink,
					 asink,
					 NULL);
	if (!gst_element_link_many(vdepay, vparse, vdec, vconv, vsink, NULL) //
		|| !gst_element_link_many(adepay, adec, aconv, aresample, asink, NULL))
	{
		GST_WARNING("can't link elements");
		return;
	}
	// linking

	// RTP bin pads
	gchar *buf = g_strdup_printf("recv_rtp_sink_%d", config->video_id);
	gst_element_link_pads(vudpsrc, "src", rtpbin, buf);
	g_free(buf);

	buf = g_strdup_printf("recv_rtcp_sink_%d", config->video_id);
	gst_element_link_pads(vudpsrc_1, "src", rtpbin, buf);
	g_free(buf);

	buf = g_strdup_printf("send_rtcp_src_%d", config->video_id);
	gst_element_link_pads(rtpbin, buf, vudpsink, "sink");
	g_free(buf);

	buf = g_strdup_printf("recv_rtp_sink_%d", config->audio_id);
	gst_element_link_pads(audpsrc, "src", rtpbin, buf);
	g_free(buf);

	buf = g_strdup_printf("recv_rtcp_sink_%d", config->audio_id);
	gst_element_link_pads(audpsrc_1, "src", rtpbin, buf);
	g_free(buf);

	buf = g_strdup_printf("send_rtcp_src_%d", config->audio_id);
	gst_element_link_pads(rtpbin, buf, audpsink, "sink");
	g_free(buf);

	GstPad *vdepay_pad = gst_element_get_static_pad(vdepay, "sink");
	GstPad *adepay_pad = gst_element_get_static_pad(adepay, "sink");
	g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), vdepay_pad);
	g_signal_connect(rtpbin, "pad-added", G_CALLBACK(cb_new_pad), adepay_pad);
	gst_object_unref(vdepay_pad);
	gst_object_unref(adepay_pad);
}