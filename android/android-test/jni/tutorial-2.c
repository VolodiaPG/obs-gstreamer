#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <gst/gst.h>
#include <pthread.h>

#include <android/log.h>
#define TAG "MTAG"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG,  __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG,  __VA_ARGS__)

#include "../../../sender/sender.h"

GST_DEBUG_CATEGORY_STATIC (debug_category);
#define GST_CAT_DEFAULT debug_category

/*
 * These macros provide a way to store the native pointer to CustomData, which might be 32 or 64 bits, into
 * a jlong, which is always 64 bits, without warnings.
 */
#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData
{
    jobject app;                  /* Application instance, used to call its methods. A global reference is kept. */
    GstElement *pipeline;         /* The running pipeline */
    GMainContext *context;        /* GLib context used to run the main loop */
    GMainLoop *main_loop;         /* GLib main loop */
    gboolean initialized;         /* To avoid informing the UI multiple times about the initialization */
} CustomData;

/* These global variables cache values which are not changing during execution */
static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID custom_data_field_id;
static jmethodID set_message_method_id;
static jmethodID on_gstreamer_initialized_method_id;

/*
 * Private methods
 */

/* Register this thread with the VM */
static JNIEnv *
attach_current_thread (void)
{
    JNIEnv *env;
    JavaVMAttachArgs args;

    LOGD ("Attaching thread %p", g_thread_self ());
    args.version = JNI_VERSION_1_4;
    args.name = NULL;
    args.group = NULL;

    if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
        LOGE ("Failed to attach current thread");
        return NULL;
    }

    return env;
}

/* Unregister this thread from the VM */
static void
detach_current_thread (void *env)
{
    LOGD ("Detaching thread %p", g_thread_self ());
    (*java_vm)->DetachCurrentThread (java_vm);
}

/* Retrieve the JNI environment for this thread */
static JNIEnv *
get_jni_env (void)
{
    JNIEnv *env;

    if ((env = pthread_getspecific (current_jni_env)) == NULL) {
        env = attach_current_thread ();
        pthread_setspecific (current_jni_env, env);
    }

    return env;
}

/* Change the content of the UI's TextView */
static void
set_ui_message (const gchar * message, CustomData * data)
{
    JNIEnv *env = get_jni_env ();
    LOGD ("Setting message to: %s", message);
    jstring jmessage = (*env)->NewStringUTF (env, message);
    (*env)->CallVoidMethod (env, data->app, set_message_method_id, jmessage);
    if ((*env)->ExceptionCheck (env)) {
        LOGE ("Failed to call Java method");
        (*env)->ExceptionClear (env);
    }
    (*env)->DeleteLocalRef (env, jmessage);
}

/* Retrieve errors from the bus and show them on the UI */
static void
error_cb (GstBus * bus, GstMessage * msg, CustomData * data)
{
    GError *err;
    gchar *debug_info;
    gchar *message_string;

    gst_message_parse_error (msg, &err, &debug_info);
    message_string =
            g_strdup_printf ("Error received from element %s: %s",
                             GST_OBJECT_NAME (msg->src), err->message);
    g_clear_error (&err);
    g_free (debug_info);
    set_ui_message (message_string, data);
    g_free (message_string);
    gst_element_set_state (data->pipeline, GST_STATE_NULL);
}

/* Notify UI about pipeline state changes */
static void
state_changed_cb (GstBus * bus, GstMessage * msg, CustomData * data)
{
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
    /* Only pay attention to messages coming from the pipeline, not its children */
    if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
        gchar *message = g_strdup_printf ("State changed to %s",
                                          gst_element_state_get_name (new_state));
        set_ui_message (message, data);
        g_free (message);
    }
}

/* Check if all conditions are met to report GStreamer as initialized.
 * These conditions will change depending on the application */
static void
check_initialization_complete (CustomData * data)
{
    JNIEnv *env = get_jni_env ();
    if (!data->initialized && data->main_loop) {
        LOGD ("Initialization complete, notifying application. main_loop:%p",
              data->main_loop);
        (*env)->CallVoidMethod (env, data->app, on_gstreamer_initialized_method_id);
        if ((*env)->ExceptionCheck (env)) {
            LOGE ("Failed to call Java method");
            (*env)->ExceptionClear (env);
        }
        data->initialized = TRUE;
    }
}

/* Main method for the native code. This is executed on its own thread. */
static void *
app_function (void *userdata)
{
    CustomData *custom_data = (CustomData *) userdata;

    settings_t settings;
    gstreamer_source_get_defaults(&settings);
    settings.receiver_ip = "192.168.1.92";
    settings.videosource = "ahcsrc";


    gst_init(NULL, NULL);

    data_t *data = gstreamer_source_create(&settings);

    if (create_pipeline(data))
    {
        start(data);
        sleep(60);
        getchar();
        stop(data);
    }

    g_free(data);

    return NULL;
}

/*
 * Java Bindings
 */

/* Instruct the native code to create its internal data structure, pipeline and thread */
static void
gst_native_init (JNIEnv * env, jobject thiz)
{
    CustomData *data = g_new0 (CustomData, 1);
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, data);
    GST_DEBUG_CATEGORY_INIT (debug_category, "tutorial-2", 0,
                             "Android tutorial 2");
    gst_debug_set_threshold_for_name ("tutorial-2", GST_LEVEL_DEBUG);
    LOGD ("Created CustomData at %p", data);
    data->app = (*env)->NewGlobalRef (env, thiz);
    LOGD ("Created GlobalRef for app object at %p", data->app);
    pthread_create (&gst_app_thread, NULL, &app_function, data);
}

/* Quit the main loop, remove the native thread and free resources */
static void
gst_native_finalize (JNIEnv * env, jobject thiz)
{
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data)
        return;
    LOGD ("Quitting main loop...");
    g_main_loop_quit (data->main_loop);
    LOGD ("Waiting for thread to finish...");
    pthread_join (gst_app_thread, NULL);
    LOGD ("Deleting GlobalRef for app object at %p", data->app);
    (*env)->DeleteGlobalRef (env, data->app);
    LOGD ("Freeing CustomData at %p", data);
    g_free (data);
    SET_CUSTOM_DATA (env, thiz, custom_data_field_id, NULL);
    LOGD ("Done finalizing");
}

/* Set pipeline to PLAYING state */
static void
gst_native_play (JNIEnv * env, jobject thiz)
{
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data)
        return;
    LOGD ("Setting state to PLAYING");
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
}

/* Set pipeline to PAUSED state */
static void
gst_native_pause (JNIEnv * env, jobject thiz)
{
    CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
    if (!data)
        return;
    LOGD ("Setting state to PAUSED");
    gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
}

/* Static class initializer: retrieve method and field IDs */
static jboolean
gst_native_class_init (JNIEnv * env, jclass klass)
{
    custom_data_field_id =
            (*env)->GetFieldID (env, klass, "native_custom_data", "J");
    set_message_method_id =
            (*env)->GetMethodID (env, klass, "setMessage", "(Ljava/lang/String;)V");
    on_gstreamer_initialized_method_id =
            (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");

    if (!custom_data_field_id || !set_message_method_id
        || !on_gstreamer_initialized_method_id) {
        /* We emit this message through the Android log instead of the GStreamer log because the later
         * has not been initialized yet.
         */
        __android_log_print (ANDROID_LOG_ERROR, "tutorial-2",
                             "The calling class does not implement all necessary interface methods");
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

/* List of implemented native methods */
static JNINativeMethod native_methods[] = {
        {"nativeInit", "()V", (void *) gst_native_init},
        {"nativeFinalize", "()V", (void *) gst_native_finalize},
        {"nativePlay", "()V", (void *) gst_native_play},
        {"nativePause", "()V", (void *) gst_native_pause},
        {"nativeClassInit", "()Z", (void *) gst_native_class_init}
};

/* Library initializer */
jint
JNI_OnLoad (JavaVM * vm, void *reserved)
{
    JNIEnv *env = NULL;

    java_vm = vm;

    if ((*vm)->GetEnv (vm, (void **) &env, JNI_VERSION_1_4) != JNI_OK) {
        __android_log_print (ANDROID_LOG_ERROR, "tutorial-2",
                             "Could not retrieve JNIEnv");
        return 0;
    }
    jclass klass = (*env)->FindClass (env,
                                      "org/freedesktop/gstreamer/tutorials/tutorial_2/Tutorial2");
    (*env)->RegisterNatives (env, klass, native_methods,
                             G_N_ELEMENTS (native_methods));

    pthread_key_create (&current_jni_env, detach_current_thread);

    return JNI_VERSION_1_4;
}
