package org.freedesktop.gstreamer.camera;

import android.content.Context;
import android.content.SharedPreferences;

public class Config {

    private final SharedPreferences sp;

    public Config(SharedPreferences sp){
        this.sp = sp;
    }

    public String getIp()
    {
        return sp.getString("ip","127.0.0.1");
    }

    public void setIP(String ip)
    {
        SharedPreferences.Editor editor = sp.edit();
        editor.putString("ip", ip);
        editor.apply();
    }

    public int getPort()
    {
        return sp.getInt("port",5000);
    }

    public void setPort(int port)
    {
        SharedPreferences.Editor editor = sp.edit();
        editor.putInt("port", port);
        editor.apply();
    }

    public int getID()
    {
        return sp.getInt("id",0);
    }

    public void setID(int id)
    {
        SharedPreferences.Editor editor = sp.edit();
        editor.putInt("id", id);
        editor.apply();
    }


}
