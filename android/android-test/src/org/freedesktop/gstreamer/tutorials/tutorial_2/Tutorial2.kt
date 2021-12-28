package org.freedesktop.gstreamer.tutorials.tutorial_2

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.View
import org.freedesktop.gstreamer.GStreamer
import android.widget.Toast
import org.freedesktop.gstreamer.tutorials.tutorial_2.R
import android.widget.ImageButton
import android.widget.TextView
import org.freedesktop.gstreamer.tutorials.tutorial_2.Tutorial2
import java.lang.Exception

class Tutorial2 : Activity() {
    private external fun nativeInit() // Initialize native code, build pipeline, etc
    private external fun nativeFinalize() // Destroy pipeline and shutdown native code
    private external fun nativePlay() // Set pipeline to PLAYING
    private external fun nativePause() // Set pipeline to PAUSED
    private val native_custom_data // Native code will use this to keep private data
            : Long = 0
    private var is_playing_desired // Whether the user asked to go to PLAYING
            = false

    // Called when the activity is first created.
    public override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Initialize GStreamer and warn if it fails
        try {
            GStreamer.init(this)
        } catch (e: Exception) {
            Toast.makeText(this, e.message, Toast.LENGTH_LONG).show()
            finish()
            return
        }
        setContentView(R.layout.main)
        val play = findViewById<View>(R.id.button_play) as ImageButton
        play.setOnClickListener {
            is_playing_desired = true
            nativePlay()
        }
        val pause = findViewById<View>(R.id.button_stop) as ImageButton
        pause.setOnClickListener {
            is_playing_desired = false
            nativePause()
        }
        if (savedInstanceState != null) {
            is_playing_desired = savedInstanceState.getBoolean("playing")
            Log.i("GStreamer", "Activity created. Saved state is playing:$is_playing_desired")
        } else {
            is_playing_desired = false
            Log.i("GStreamer", "Activity created. There is no saved state, playing: false")
        }

        // Start with disabled buttons, until native code is initialized
        findViewById<View>(R.id.button_play).isEnabled = false
        findViewById<View>(R.id.button_stop).isEnabled = false
        nativeInit()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        Log.d("GStreamer", "Saving state, playing:$is_playing_desired")
        outState.putBoolean("playing", is_playing_desired)
    }

    override fun onDestroy() {
        nativeFinalize()
        super.onDestroy()
    }

    // Called from native code. This sets the content of the TextView from the UI thread.
    private fun setMessage(message: String) {
        val tv = findViewById<View>(R.id.textview_message) as TextView
        runOnUiThread { tv.text = message }
    }

    // Called from native code. Native code calls this once it has created its pipeline and
    // the main loop is running, so it is ready to accept commands.
    private fun onGStreamerInitialized() {
        Log.i("GStreamer", "Gst initialized. Restoring state, playing:$is_playing_desired")
        // Restore previous playing state
        if (is_playing_desired) {
            nativePlay()
        } else {
            nativePause()
        }

        // Re-enable buttons, now that GStreamer is initialized
        val activity: Activity = this
        runOnUiThread {
            activity.findViewById<View>(R.id.button_play).isEnabled = true
            activity.findViewById<View>(R.id.button_stop).isEnabled = true
        }
    }

    companion object {
        @JvmStatic
        private external fun nativeClassInit(): Boolean // Initialize native class: cache Method IDs for callbacks

        init {
            System.loadLibrary("gstreamer_android")
            System.loadLibrary("tutorial-2")
            nativeClassInit()
        }
    }
}