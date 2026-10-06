package com.phantomgo.mock;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.location.Location;
import android.location.LocationManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import androidx.annotation.Nullable;
import androidx.core.app.NotificationCompat;

public class MockLocationService extends Service {

    private static final String TAG = "MockLocationService";
    private static final String CHANNEL_ID = "phantomgo_mock";
    public static final String EXTRA_LAT = "lat";
    public static final String EXTRA_LNG = "lng";
    public static final String EXTRA_BEARING = "bearing";

    private LocationManager locationManager;
    private Handler handler;
    private Runnable tickRunnable;
    private volatile double currentLat;
    private volatile double currentLng;
    private volatile float currentBearing = 0f;
    private volatile boolean running = false;

    @Override
    public void onCreate() {
        super.onCreate();
        locationManager = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        handler = new Handler(Looper.getMainLooper());
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null) {
            currentLat = intent.getDoubleExtra(EXTRA_LAT, 0);
            currentLng = intent.getDoubleExtra(EXTRA_LNG, 0);
            currentBearing = intent.getFloatExtra(EXTRA_BEARING, 0f);
        }

        startForeground(1, buildNotification());
        running = true;
        setupMockProvider();
        startTick();
        return START_STICKY;
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "PhantomGo 虚拟定位",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("正在运行虚拟定位服务");
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) nm.createNotificationChannel(channel);
        }
    }

    private Notification buildNotification() {
        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("PhantomGo")
                .setContentText(String.format("虚拟定位运行中: %.5f, %.5f", currentLat, currentLng))
                .setSmallIcon(android.R.drawable.ic_menu_mylocation)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .setOngoing(true)
                .build();
    }

    private void setupMockProvider() {
        try {
            if (locationManager.getProvider(LocationManager.GPS_PROVIDER) != null) {
                locationManager.removeTestProvider(LocationManager.GPS_PROVIDER);
            }
        } catch (Exception e) {
            Log.w(TAG, "remove provider: " + e.getMessage());
        }
        try {
            locationManager.addTestProvider(
                    LocationManager.GPS_PROVIDER,
                    false, false, false, false,
                    true, true, true,
                    android.location.Criteria.POWER_LOW,
                    android.location.Criteria.ACCURACY_FINE);
            locationManager.setTestProviderEnabled(LocationManager.GPS_PROVIDER, true);
        } catch (SecurityException e) {
            Log.e(TAG, "Mock location not enabled in developer settings", e);
        } catch (Exception e) {
            Log.e(TAG, "addTestProvider error", e);
        }
    }

    private void startTick() {
        tickRunnable = new Runnable() {
            @Override
            public void run() {
                if (running) {
                    pushLocation();
                    handler.postDelayed(this, 1000);
                }
            }
        };
        handler.post(tickRunnable);
    }

    private void pushLocation() {
        try {
            Location loc = new Location(LocationManager.GPS_PROVIDER);
            loc.setLatitude(currentLat);
            loc.setLongitude(currentLng);
            loc.setAccuracy(3f);
            loc.setBearing(currentBearing);
            loc.setSpeed(0f);
            loc.setAltitude(0);
            loc.setTime(System.currentTimeMillis());
            loc.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());

            locationManager.setTestProviderLocation(LocationManager.GPS_PROVIDER, loc);

            // Also update network provider for broader coverage
            try {
                if (locationManager.getProvider(LocationManager.NETWORK_PROVIDER) != null) {
                    locationManager.setTestProviderLocation(LocationManager.NETWORK_PROVIDER, loc);
                }
            } catch (Exception ignored) {}

        } catch (SecurityException e) {
            Log.e(TAG, "SecurityException - mock location permission not granted", e);
        } catch (Exception e) {
            Log.e(TAG, "setTestProviderLocation error", e);
        }
    }

    public void updateLocation(double lat, double lng, float bearing) {
        this.currentLat = lat;
        this.currentLng = lng;
        this.currentBearing = bearing;
        pushLocation();
        updateNotification();
    }

    private void updateNotification() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.notify(1, buildNotification());
        }
    }

    @Override
    public void onDestroy() {
        running = false;
        if (handler != null && tickRunnable != null) {
            handler.removeCallbacks(tickRunnable);
        }
        try {
            locationManager.removeTestProvider(LocationManager.GPS_PROVIDER);
        } catch (Exception ignored) {}
        super.onDestroy();
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
