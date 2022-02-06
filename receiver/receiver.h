#ifndef _RECEIVER_H
#define _RECEIVER_H

#include <gst/app/app.h>
#include <gst/gst.h>
#include <gst/net/gstnet.h>
#include <stdio.h>

#define LOGD(...) GST_DEBUG(__VA_ARGS__)
#define LOGI(...) GST_INFO(__VA_ARGS__)
#define LOGW(...) GST_WARNING(__VA_ARGS__)
#define LOGE(...) GST_ERROR(__VA_ARGS__)

#define VSINK_NAME_FORMAT "video_sink_%d"
#define ASINK_NAME_FORMAT "audio_sink_%d"

#define NB_PORTS 6

extern const char *vsink_bin;
extern const char *asink_bin;

typedef struct
{
	const gchar *clock_ip;
	const gint clock_port;
	const gint64 latency;
} pipeline_config_t;

typedef struct
{
	gint video_id;
	gint audio_id;
	const gchar *dest;
	// Used ports, in order:
	// 0: video
	// 1: video
	// 2: video
	// 3: audio
	// 4: audio
	// 5: audio
	gint ports[NB_PORTS];
} receiver_config_t;

typedef struct
{
	// source is only relevant if it was assigned, otherwise it is NULL
	GstPad *source;
	GstPad *sink;
	guint id;
	void *parent; //source_data_t
} cb_new_pad_t;

typedef struct
{
	GstElement *vudpsrc;
	GstElement *vdepay;
	GstElement *vdepayqueue;
	GstElement *vparse;
	GstElement *vdec;
	GstElement* vdecqueue;
	GstElement *vconv;
	GstElement *vsink;
	GstElement *vudpsrc_1;
	GstElement *vudpsink;
	GstElement *audpsrc;
	GstElement *adepay;
	GstElement *adepayqueue;
	GstElement *adec;
	GstElement *adecqueue;
	GstElement *aconv;
	GstElement *aresample;
	GstElement *asink;
	GstElement *audpsink;
	GstElement *audpsrc_1;
	GstPad *vrecv_rtp_sink;
	GstPad *vrecv_rtcp_sink;
	GstPad *vsend_rtcp_src;
	GstPad *arecv_rtp_sink;
	GstPad *arecv_rtcp_sink;
	GstPad *asend_rtcp_src;
	guint video_id;
	guint audio_id;
	gulong vdepay_cb_id;
	gulong adepay_cb_id;
	cb_new_pad_t cb_video;
	cb_new_pad_t cb_audio;
	receiver_config_t *config;
	GstElement *pipe;
} source_data_t;

GstElement *create_streaminsync_pipeline(pipeline_config_t *config);
source_data_t *add_incoming_source(GstElement *pipe, receiver_config_t *config);
void set_source_to(source_data_t *data, GstState state);
void remove_incoming_source(source_data_t *data);
#endif