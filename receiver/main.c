#include "receiver.h"

#define PORTS_FROM(port)                                       \
    {                                                          \
        port, port + 1, port + 2, port + 3, port + 4, port + 5 \
    }

int main(int argc, char **argv)
{
    gst_init(NULL, NULL);

    GstElement *pipe;

    {
        pipeline_config_t config = {
            .clock_ip = "45.159.204.28",
            .clock_port = 123,
            .latency = 10000};
        pipe = create_streaminsync_pipeline(&config);
    }

    // {
    //     receiver_config_t config = {
    //         .video_id = 2,
    //         .audio_id = 3,
    //         .dest = "127.0.0.1",
    //         .ports = PORTS_FROM(6000)};
    //     add_incoming_source(pipe, &config);
    // }

    {
        receiver_config_t config = {
            .video_id = 0,
            .audio_id = 1,
            .dest = "192.168.1.44",
            .ports = PORTS_FROM(5000)};
        add_incoming_source(pipe, &config);
    }
    {
        receiver_config_t config = {
            .video_id = 2,
            .audio_id = 3,
            .dest = "127.0.0.1",
            .ports = PORTS_FROM(6000)};
        add_incoming_source(pipe, &config);
    }

    if (gst_element_set_state(pipe,
                              GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        GST_ERROR("Failed to set state to PLAYING\n");
        return EXIT_FAILURE;
    };

    // GMainLoop *loop = g_main_loop_new(context, FALSE);
    // g_main_loop_run(loop);
    // g_main_loop_unref(loop);
    // g_main_context_unref(context);
    // gst_element_set_state(pipe, GST_STATE_NULL);
    // gst_object_unref(pipeline->asink);
    // gst_object_unref(pipeline->vsink);
    // gst_object_unref(pipeline->pipe);
    /* Listen to the bus */
    GstMessage *msg;
    gboolean terminate = FALSE;
    GstBus *bus = gst_element_get_bus(pipe);
    do
    {
        msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                         GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

        /* Parse message */
        if (msg != NULL)
        {
            GError *err;
            gchar *debug_info;

            switch (GST_MESSAGE_TYPE(msg))
            {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg, &err, &debug_info);
                g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
                g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
                g_clear_error(&err);
                g_free(debug_info);
                terminate = TRUE;
                break;
            case GST_MESSAGE_EOS:
                g_print("End-Of-Stream reached.\n");
                terminate = TRUE;
                break;
            case GST_MESSAGE_STATE_CHANGED:
                /* We are only interested in state-changed messages from the pipeline */
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipe))
                {
                    GstState old_state, new_state, pending_state;
                    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                    g_print("Pipeline state changed from %s to %s:\n",
                            gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
                }
                break;
            default:
                /* We should not reach here */
                g_printerr("Unexpected message received.\n");
                break;
            }
            gst_message_unref(msg);
        }
    } while (!terminate);

    return EXIT_SUCCESS;
}