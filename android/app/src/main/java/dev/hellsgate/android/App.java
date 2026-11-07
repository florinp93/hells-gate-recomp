package dev.hellsgate.android;

import android.app.Application;

public class App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        DiagLog.init(this);
    }
}
