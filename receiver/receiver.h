#ifndef _RECEIVER_H
#define _RECEIVER_H

#include <gst/app/app.h>
#include <gst/gst.h>
#include <gst/net/gstnet.h>

#define LOGD(...) GST_DEBUG(__VA_ARGS__)
#define LOGI(...) GST_INFO(__VA_ARGS__)
#define LOGW(...) GST_WARNING(__VA_ARGS__)
#define LOGE(...) GST_ERROR(__VA_ARGS__)

#define NB_PORTS 6
typedef struct
{
	const gchar *clock_ip;
	const gint clock_port;
	const gint latency;
} pipeline_config_t;

typedef struct
{
	const gint video_id;
	const gint audio_id;
	const gchar *dest;
	// Used ports, in order:
	// 0: video
	// 1: video
	// 2: video
	// 3: audio
	// 4: audio
	// 5: audio
	const gint ports[NB_PORTS];
} receiver_config_t;

GstElement *create_streaminsync_pipeline(pipeline_config_t *config);
void add_incoming_source(GstElement *pipe, receiver_config_t *config);
#endif