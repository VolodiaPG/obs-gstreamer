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

#include <obs/obs-module.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>

#include "receiver/receiver.h"

#define PORTS_FROM(port)                                       \
	{                                                          \
		port, port + 1, port + 2, port + 3, port + 4, port + 5 \
	}

typedef struct
{
	GstElement *pipe;
	GThread *thread;
	GMainLoop *loop;
	GMutex mutex;
	GCond cond;
	GSource *timeout;
} global_data_t;

typedef struct
{
	source_data_t *source_data;
	obs_source_t *source;
	obs_data_t *settings;
	gint64 frame_count;
	gint64 audio_count;
	global_data_t *parent;
} data_t;

global_data_t global_data;

static void timeout_destroy(gpointer user_data)
{
	global_data_t *data = user_data;

	g_source_destroy(data->timeout);
	g_source_unref(data->timeout);
	data->timeout = NULL;
}

static gboolean bus_callback(GstBus *bus, GstMessage *message,
							 gpointer user_data);

static void create_pipeline(global_data_t *data)
{
	pipeline_config_t config = {
		.clock_ip = "45.159.204.28",
		.clock_port = 123,
		.latency = 10000};
	data->pipe = create_streaminsync_pipeline(&config);

	if (!data->pipe)
	{
		LOGE("Failed to create pipeline");
		return;
	}

	GstBus *bus = gst_element_get_bus(data->pipe);
	gst_bus_add_watch(bus, bus_callback, data);
	gst_object_unref(bus);
}

static gboolean start_pipe(gpointer user_data)
{
	global_data_t *data = user_data;

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
	global_data_t *data = user_data;

	switch (GST_MESSAGE_TYPE(message))
	{
	case GST_MESSAGE_ERROR:
	{
		GError *err;
		gst_message_parse_error(message, &err, NULL);
		blog(LOG_ERROR, "%s", err->message);
		g_error_free(err);
	} // fallthrough
	case GST_MESSAGE_EOS:
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		// if (obs_data_get_bool(data->settings, "clear_on_end"))
		// 	obs_source_output_video(data->source, NULL);
		if (
			// obs_data_get_bool(data->settings,
			// 				  GST_MESSAGE_TYPE(message) ==
			// 						  GST_MESSAGE_ERROR
			// 					  ? "restart_on_error"
			// 					  : "restart_on_eos") &&
			data->timeout == NULL)
		{
			// data->timeout = g_timeout_source_new(obs_data_get_int(
			// 	data->settings, "restart_timeout"));
			data->timeout = g_timeout_source_new(2000);
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
		blog(LOG_WARNING, "%s", err->message);
		g_error_free(err);
	}
	break;
	default:
		break;
	}

	return TRUE;
}

static GstFlowReturn video_new_sample(GstAppSink *appsink, gpointer user_data)
{
	data_t *data = user_data;
	GstSample *sample = gst_app_sink_pull_sample(appsink);
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	GstMapInfo info;
	GstVideoInfo video_info;

	gst_video_info_from_caps(&video_info, caps);
	gst_buffer_map(buffer, &info, GST_MAP_READ);

	struct obs_source_frame frame = {};

	frame.timestamp =
		obs_data_get_bool(data->settings, "use_timestamps_video")
			? GST_BUFFER_PTS(buffer)
			: data->frame_count++;

	frame.width = video_info.width;
	frame.height = video_info.height;
	frame.linesize[0] = video_info.stride[0];
	frame.linesize[1] = video_info.stride[1];
	frame.linesize[2] = video_info.stride[2];
	frame.data[0] = info.data + video_info.offset[0];
	frame.data[1] = info.data + video_info.offset[1];
	frame.data[2] = info.data + video_info.offset[2];

	enum video_range_type range = VIDEO_RANGE_DEFAULT;
	switch (video_info.colorimetry.range)
	{
	case GST_VIDEO_COLOR_RANGE_0_255:
		range = VIDEO_RANGE_FULL;
		frame.full_range = 1;
		break;
	case GST_VIDEO_COLOR_RANGE_16_235:
		range = VIDEO_RANGE_PARTIAL;
		break;
	default:
		break;
	}

	enum video_colorspace cs = VIDEO_CS_DEFAULT;
	switch (video_info.colorimetry.matrix)
	{
	case GST_VIDEO_COLOR_MATRIX_BT709:
		cs = VIDEO_CS_709;
		break;
	case GST_VIDEO_COLOR_MATRIX_BT601:
		cs = VIDEO_CS_601;
		break;
	default:
		break;
	}

	video_format_get_parameters(cs, range, frame.color_matrix,
								frame.color_range_min,
								frame.color_range_max);

	switch (video_info.finfo->format)
	{
	case GST_VIDEO_FORMAT_I420:
		frame.format = VIDEO_FORMAT_I420;
		break;
	case GST_VIDEO_FORMAT_NV12:
		frame.format = VIDEO_FORMAT_NV12;
		break;
	case GST_VIDEO_FORMAT_BGRA:
		frame.format = VIDEO_FORMAT_BGRA;
		break;
	case GST_VIDEO_FORMAT_BGRx:
		frame.format = VIDEO_FORMAT_BGRX;
		break;
	case GST_VIDEO_FORMAT_RGBx:
	case GST_VIDEO_FORMAT_RGBA:
		frame.format = VIDEO_FORMAT_RGBA;
		break;
	case GST_VIDEO_FORMAT_UYVY:
		frame.format = VIDEO_FORMAT_UYVY;
		break;
	case GST_VIDEO_FORMAT_YUY2:
		frame.format = VIDEO_FORMAT_YUY2;
		break;
	case GST_VIDEO_FORMAT_YVYU:
		frame.format = VIDEO_FORMAT_YVYU;
		break;
	default:
		frame.format = VIDEO_FORMAT_NONE;
		blog(LOG_ERROR, "Unknown video format: %s",
			 video_info.finfo->name);
		break;
	}

	obs_source_output_video(data->source, &frame);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

static GstFlowReturn audio_new_sample(GstAppSink *appsink, gpointer user_data)
{
	data_t *data = user_data;
	GstSample *sample = gst_app_sink_pull_sample(appsink);
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	GstMapInfo info;
	GstAudioInfo audio_info;

	gst_audio_info_from_caps(&audio_info, caps);
	gst_buffer_map(buffer, &info, GST_MAP_READ);

	struct obs_source_audio audio = {};

	audio.frames = info.size / audio_info.bpf;
	audio.samples_per_sec = audio_info.rate;
	audio.data[0] = info.data;

	audio.timestamp =
		obs_data_get_bool(data->settings, "use_timestamps_audio")
			? GST_BUFFER_PTS(buffer)
			: data->audio_count++ * GST_SECOND *
				  (audio.frames / (double)audio_info.rate);

	switch (audio_info.channels)
	{
	case 1:
		audio.speakers = SPEAKERS_MONO;
		break;
	case 2:
		audio.speakers = SPEAKERS_STEREO;
		break;
	case 3:
		audio.speakers = SPEAKERS_2POINT1;
		break;
	case 4:
		audio.speakers = SPEAKERS_4POINT0;
		break;
	case 5:
		audio.speakers = SPEAKERS_4POINT1;
		break;
	case 6:
		audio.speakers = SPEAKERS_5POINT1;
		break;
	case 8:
		audio.speakers = SPEAKERS_7POINT1;
		break;
	default:
		audio.speakers = SPEAKERS_UNKNOWN;
		blog(LOG_ERROR, "Unsupported channel count: %d",
			 audio_info.channels);
		break;
	}

	switch (audio_info.finfo->format)
	{
	case GST_AUDIO_FORMAT_U8:
		audio.format = AUDIO_FORMAT_U8BIT;
		break;
	case GST_AUDIO_FORMAT_S16LE:
		audio.format = AUDIO_FORMAT_16BIT;
		break;
	case GST_AUDIO_FORMAT_S32LE:
		audio.format = AUDIO_FORMAT_32BIT;
		break;
	case GST_AUDIO_FORMAT_F32LE:
		audio.format = AUDIO_FORMAT_FLOAT;
		break;
	default:
		audio.format = AUDIO_FORMAT_UNKNOWN;
		blog(LOG_ERROR, "Unknown audio format: %s",
			 audio_info.finfo->name);
		break;
	}

	obs_source_output_audio(data->source, &audio);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

const char *gstreamer_source_get_name(void *type_data)
{
	return "GStreamer Source";
}

static gboolean loop_startup(gpointer user_data)
{
	global_data_t *data = user_data;

	create_pipeline(data);

	g_mutex_lock(&data->mutex);
	g_cond_signal(&data->cond);
	g_mutex_unlock(&data->mutex);

	if (data->pipe)
		gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	return G_SOURCE_REMOVE;
}

static void add_endpoint(data_t *data)
{
	const gint video_id = obs_data_get_int(data->settings, "source_id") * 2;
	const gint audio_id = video_id + 1;

	if (data->source_data != NULL)
	{
		blog(LOG_ERROR, "source_data is not null, please remove the endpoint first");
		return;
	}

	blog(LOG_INFO, "Adding a new endpoint");
	receiver_config_t *config = g_new0(receiver_config_t, 1);

	config->dest = obs_data_get_string(data->settings, "client_ip");
	config->audio_id = audio_id;
	config->video_id = video_id;
	const int port = obs_data_get_int(data->settings, "port");
	for (int ii = 0; ii < NB_PORTS; ++ii)
	{
		config->ports[ii] = port + ii;
	}

	data->source_data = add_incoming_source(data->parent->pipe, config);
	set_source_to(data->source_data, GST_STATE_PLAYING);

	if (data->source_data == NULL)
	{
		blog(LOG_ERROR, "Cannot add source");

		obs_source_output_video(data->source, NULL);

		return;
	}

	{
		gchar *buf = g_strdup_printf(VSINK_NAME_FORMAT, video_id);
		GstElement *appsink_video = gst_bin_get_by_name(GST_BIN(data->parent->pipe),
														buf);
		g_free(buf);
		if (appsink_video == NULL)
		{
			blog(LOG_ERROR, "Could not find appsink video");
			obs_source_output_video(data->source, NULL);
			return;
		}

		GstAppSinkCallbacks video_cbs = {NULL, NULL, video_new_sample};
		gst_app_sink_set_callbacks(GST_APP_SINK(appsink_video), &video_cbs, data,
								   NULL);

		if (obs_data_get_bool(data->settings, "block_video"))
			g_object_set(appsink_video, "max-buffers", 1, NULL);
		// check if connected and remove if not
		GstPad *pad = gst_element_get_static_pad(appsink_video, "sink");
		if (!gst_pad_is_linked(pad))
			gst_bin_remove(GST_BIN(data->parent->pipe), appsink_video);

		gst_object_unref(pad);
		gst_object_unref(appsink_video);
	}

	{
		gchar *buf = g_strdup_printf(ASINK_NAME_FORMAT, audio_id);
		GstElement *appsink_audio = gst_bin_get_by_name(GST_BIN(data->parent->pipe),
														buf);
		g_free(buf);

		if (appsink_audio == NULL)
		{
			blog(LOG_ERROR, "Could not find appsink audio");
			obs_source_output_video(data->source, NULL);
			return;
		}

		GstAppSinkCallbacks audio_cbs = {NULL, NULL, audio_new_sample};
		gst_app_sink_set_callbacks(GST_APP_SINK(appsink_audio), &audio_cbs, data,
								   NULL);

		if (obs_data_get_bool(data->settings, "block_audio"))
			g_object_set(appsink_audio, "max-buffers", 1, NULL);
		// check if connected and remove if not
		// sink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio");
		GstPad *pad = gst_element_get_static_pad(appsink_audio, "sink");
		if (!gst_pad_is_linked(pad))
			gst_bin_remove(GST_BIN(data->parent->pipe), appsink_audio);

		gst_object_unref(pad);
		gst_object_unref(appsink_audio);
	}

	data->frame_count = 0;
	data->audio_count = 0;
}

static void remove_endpoint(data_t *data)
{
	if (data->source_data == NULL)
	{
		blog(LOG_WARNING, "source_data is null, please add the endpoint first");
		return;
	}

	remove_incoming_source(data->source_data);

	g_free(data->source_data->config);
	g_free(data->source_data);
	data->source_data = NULL;
}

static gpointer _start(gpointer user_data)
{
	global_data_t *data = user_data;

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

static void start_pipeline(global_data_t *data)
{
	g_mutex_lock(&data->mutex);

	data->thread = g_thread_new("GStreamer Source", _start, data);

	g_cond_wait(&data->cond, &data->mutex);
	g_mutex_unlock(&data->mutex);
}

static void stop_pipeline(global_data_t *data)
{
	if (data->thread == NULL)
		return;

	g_main_loop_quit(data->loop);

	g_thread_join(data->thread);
	data->thread = NULL;
}

static void start_endpoint(data_t *data)
{
	g_mutex_lock(&data->parent->mutex);

	add_endpoint(data);

	g_mutex_unlock(&data->parent->mutex);
}

void *gstreamer_source_create(obs_data_t *settings, obs_source_t *source)
{
	LOGD("Create new gstreamer source");
	data_t *data = g_new0(data_t, 1);
	data->parent = &global_data;

	data->source = source;
	data->settings = settings;

	if (obs_data_get_bool(settings, "stop_on_hide") == false)
		start_endpoint(data);

	return data;
}

static void stop_endpoint(data_t *data)
{
	LOGD("Stopping endpoint");
	g_mutex_lock(&data->parent->mutex);

	remove_endpoint(data);

	g_mutex_unlock(&data->parent->mutex);

	obs_source_output_video(data->source, NULL);
}

void gstreamer_source_destroy(void *user_data)
{
	LOGD("Destroying source");
	data_t *data = user_data;

	stop_endpoint(data);

	// g_mutex_clear(&data->parent->mutex);
	// g_cond_clear(&data->parent->cond);

	g_free(data);
}

void gstreamer_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "source_id", 0);
	obs_data_set_default_string(settings, "sender_ip", "127.0.0.1");
	obs_data_set_default_int(settings, "port", 5000);

	obs_data_set_default_bool(settings, "restart_on_eos", true);
	obs_data_set_default_bool(settings, "restart_on_error", false);
	obs_data_set_default_int(settings, "restart_timeout", 2000);
	obs_data_set_default_bool(settings, "stop_on_hide", true);
	obs_data_set_default_bool(settings, "block_video", false);
	obs_data_set_default_bool(settings, "block_audio", false);
	obs_data_set_default_bool(settings, "clear_on_end", true);
}

void gstreamer_source_update(void *data, obs_data_t *settings);

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property,
							 void *data)
{
	gstreamer_source_update(data, ((data_t *)data)->settings);

	return false;
}

obs_properties_t *gstreamer_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_int(props, "source_id", "Pipe's id",
						   0, 32, 1);
	obs_properties_add_text(props, "sender_ip", "The sender's IP address",
							OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", "The first port to use",
						   1000, 65535, 1);
	obs_properties_add_bool(props, "restart_on_eos",
							"Try to restart when end of stream is reached");
	obs_properties_add_bool(
		props, "restart_on_error",
		"Try to restart after pipeline encountered an error");
	obs_properties_add_int(props, "restart_timeout", "Error timeout (ms)",
						   0, 10000, 100);
	obs_properties_add_bool(props, "stop_on_hide",
							"Stop pipeline when hidden");
	obs_properties_add_bool(props, "block_video",
							"Block video path when sink not fast enough");
	obs_properties_add_bool(props, "block_audio",
							"Block audio path when sink not fast enough");
	obs_properties_add_bool(
		props, "clear_on_end",
		"Clear image data after end-of-stream or error");
	obs_properties_add_button2(props, "apply", "Apply", on_apply_clicked,
							   data);

	return props;
}

void gstreamer_source_update(void *data, obs_data_t *settings)
{
	LOGD("Update gstreamer source");

	stop_endpoint(data);

	// Don't start the pipeline if source is hidden and 'stop_on_hide' is set.
	// From GUI this is probably irrelevant but works around some quirks when
	// controlled from script.
	if (obs_data_get_bool(settings, "stop_on_hide") &&
		!obs_source_showing(((data_t *)data)->source))
		return;

	start_endpoint(data);
}

void gstreamer_source_show(void *data)
{
	// if (((data_t *)data)->pipe == NULL)
	// 	start(data);
	add_endpoint(data);
}

void gstreamer_source_hide(void *data)
{
	if (obs_data_get_bool(((data_t *)data)->settings, "stop_on_hide"))
	{
		// stop(data);
		remove_endpoint(data);
	}
}

void on_load()
{
	g_mutex_init(&global_data.mutex);
	g_cond_init(&global_data.cond);
	start_pipeline(&global_data);
}
