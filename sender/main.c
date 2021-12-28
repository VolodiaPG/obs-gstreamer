#include <argp.h>

#include "sender.h"
#include "log.h"

extern const char *argp_program_version;

const char *argp_program_bug_address =
    "https://github.com/VolodiaPG/obs-gstreamer";

/* Program documentation. */
static char doc[] =
    "Stream in Sync sender -- a program to send audio and video to a receiver synchronising all those sources";

/* A description of the arguments we accept. */
#define NB_CLI_ARGS 2
static char args_doc[] = "RECEIVER_IP RECEIVER_PORT";

#define SHORT_VIDEO_SOURCE 'i'
#define SHORT_AUDIO_SOURCE 'a'
#define SHORT_BITRATE 'b'
#define SHORT_WITDH 'w'
#define SHORT_HEIGHT 'h'
#define SHORT_FRAMERATE 'f'
#define SHORT_NTP_IP 'n'
#define SHORT_NTP_PORT 'p'
#define SHORT_VIDEO_ID 'q'
#define SHORT_AUDIO_ID 'r'

/* The options we understand. */
static struct argp_option options[] = {
    {"videosrc", SHORT_VIDEO_SOURCE, "NAME", 0, "Video source to use."},
    {"audiosrc", SHORT_VIDEO_SOURCE, "NAME", 0, "Audio source to use."},
    {"vbitrate", SHORT_BITRATE, "BITRATE", 0, "Video bitrate to use."},
    {"width", SHORT_WITDH, "WIDTH", 0, "Video width to use."},
    {"height", SHORT_HEIGHT, "HEIGHT", 0, "Video height to use."},
    {"framerate", SHORT_FRAMERATE, "FPS", 0, "Video framerate to use."},
    {"ntp-ip", SHORT_NTP_IP, "IP", 0, "IP address of the NTP server."},
    {"ntp-port", SHORT_NTP_PORT, "PORT", 0, "Port of the NTP server."},
    {"video-id", SHORT_VIDEO_ID, "ID", 0, "ID of the Video channel."},
    {"audio-id", SHORT_AUDIO_ID, "ID", 0, "ID of the Audio channel."},
    {0}};

/* Parse a single option. */
static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    /* Get the input argument from argp_parse, which we
       know is a pointer to our arguments structure. */
    settings_t *settings = state->input;

    switch (key)
    {
    case SHORT_VIDEO_SOURCE:
        settings->videosource = arg;
        break;
    case SHORT_AUDIO_SOURCE:
        settings->audiosource = arg;
        break;
    case SHORT_BITRATE:
        settings->bitrate = atoi(arg);
        break;
    case SHORT_WITDH:
        settings->width = atoi(arg);
        break;
    case SHORT_HEIGHT:
        settings->height = atoi(arg);
        break;
    case SHORT_FRAMERATE:
        settings->framerate = atoi(arg);
        break;
    case SHORT_NTP_PORT:
        settings->clock_port = atoi(arg);
        break;
    case SHORT_NTP_IP:
        settings->clock_ip = arg;
        break;
    case SHORT_VIDEO_ID:
        settings->video_id = atoi(arg);
        break;
    case SHORT_AUDIO_ID:
        settings->audio_id = atoi(arg);
        break;

    case ARGP_KEY_ARG:
        if (state->arg_num >= NB_CLI_ARGS)
            /* Too many arguments. */
            argp_usage(state);

        switch (state->arg_num)
        {
        case 0:
            settings->receiver_ip = arg;
            break;
        case 1:
            const gint port = atoi(arg);
            for (gint ii = 0; ii < NB_PORTS; ii++)
                settings->receiver_ports[ii] = port + ii;
            break;
        default:
            log_warn("Missing at least one positional argument transformation in the argument parser (it's the dev's fault...)");
        }

        break;

    case ARGP_KEY_END:
        if (state->arg_num < NB_CLI_ARGS)
            /* Not enough arguments. */
            argp_usage(state);
        break;

    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

/* Our argp parser. */
static struct argp argp = {options, parse_opt, args_doc, doc};

int main(int argc, char **argv)
{
    settings_t settings;
    gstreamer_source_get_defaults(&settings);
    argp_parse(&argp, argc, argv, 0, 0, &settings);

    gst_init(NULL, NULL);

    data_t *data = gstreamer_source_create(&settings);

    if (create_pipeline(data))
    {
        start(data);
        printf("---------------------------------\n");
        printf("Running. Press ENTER to stop.\n");
        getchar();
        stop(data);
    }

    g_free(data);

    return EXIT_SUCCESS;
}