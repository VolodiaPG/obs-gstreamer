#include <stdio.h>

#include "receiver.h"

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
} data_t;

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
        LOGE("EOS");
        gst_element_set_state(data->pipe, GST_STATE_NULL);
        return FALSE;
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

    {
        pipeline_config_t config = {
            .clock_ip = "45.159.204.28",
            .clock_port = 123,
            .latency = 5000};
        data->pipe = create_streaminsync_pipeline(&config);
    }

    GstBus *bus = gst_element_get_bus(data->pipe);
    gst_bus_add_watch(bus, bus_callback, data);
    gst_object_unref(bus);

    g_mutex_lock(&data->mutex);
    g_cond_signal(&data->cond);
    g_mutex_unlock(&data->mutex);

    if (data->pipe)
        gst_element_set_state(data->pipe, GST_STATE_PLAYING);

    return G_SOURCE_REMOVE;
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

int main(int argc, char **argv)
{
    source_data_t *source_data = NULL;

    gst_init(NULL, NULL);

    // get the current time
    GstClockTime start_time = gst_clock_get_time(gst_system_clock_obtain());
    gboolean done = FALSE;

    data_t *data = g_new0(data_t, 1);

    g_mutex_init(&data->mutex);
    g_cond_init(&data->cond);

    start(data);

    receiver_config_t config1 = {
        .video_id = 0,
        .audio_id = 1,
        .dest = "127.0.0.1",
        .ports = PORTS_FROM(5000)};

    receiver_config_t config2 = {
        .video_id = 2,
        .audio_id = 3,
        .dest = "127.0.0.1",
        .ports = PORTS_FROM(6000)};

    g_mutex_lock(&data->mutex);
    source_data = add_incoming_source(data->pipe, &config1);
    set_source_to(source_data, GST_STATE_PLAYING);
    g_mutex_unlock(&data->mutex);

#define TIME 7

    while (!done)
    {
        if (gst_clock_get_time(gst_system_clock_obtain()) - start_time > TIME * GST_SECOND)
        {
            gst_println("Adding new source");

            g_mutex_lock(&data->mutex);

            source_data = add_incoming_source(data->pipe, &config2);
            set_source_to(source_data, GST_STATE_PLAYING);

            g_mutex_unlock(&data->mutex);

            done = TRUE;
        }
    }

    done = FALSE;
    start_time = gst_clock_get_time(gst_system_clock_obtain());

    while (!done)
    {
        if (gst_clock_get_time(gst_system_clock_obtain()) - start_time > TIME * GST_SECOND)
        {
            gst_println("Remove the last source");

            g_mutex_lock(&data->mutex);

            remove_incoming_source(source_data);

            g_mutex_unlock(&data->mutex);

            g_free(source_data);
            source_data = NULL;

            done = TRUE;
        }
    }

    done = FALSE;
    start_time = gst_clock_get_time(gst_system_clock_obtain());

    while (!done)
    {
        if (gst_clock_get_time(gst_system_clock_obtain()) - start_time > TIME * GST_SECOND)
        {
            gst_println("Adding new source");

            g_mutex_lock(&data->mutex);

            source_data = add_incoming_source(data->pipe, &config2);
            set_source_to(source_data, GST_STATE_PLAYING);

            g_mutex_unlock(&data->mutex);

            done = TRUE;
        }
    }

#undef TIME

    gst_println("=======================================");
    gst_println("Press enter to quit the program");

    getchar();

    g_main_loop_quit(data->loop);

    g_thread_join(data->thread);

    g_free(data);
    return EXIT_SUCCESS;
}