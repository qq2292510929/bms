package com.example.mocklocation

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.location.Location
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log

/**
 * 模拟位置前台服务
 * 利用 Android 开发者模式的 Mock Location 功能，持续向系统注入虚拟位置。
 */
class MockLocationService : Service() {

    companion object {
        const val CHANNEL_ID = "mock_location_channel"
        const val NOTIFICATION_ID = 1001
        const val ACTION_START = "com.example.mocklocation.START"
        const val ACTION_STOP = "com.example.mocklocation.STOP"
        const val EXTRA_LATITUDE = "extra_latitude"
        const val EXTRA_LONGITUDE = "extra_longitude"
        const val EXTRA_ALTITUDE = "extra_altitude"
        const val EXTRA_ACCURACY = "extra_accuracy"

        private const val TAG = "MockLocationService"
        // 位置更新间隔（毫秒），频繁更新可降低被其他定位源覆盖的概率
        private const val UPDATE_INTERVAL_MS = 1000L
    }

    private var isRunning = false
    private var targetLatitude = 0.0
    private var targetLongitude = 0.0
    private var targetAltitude = 0.0
    private var targetAccuracy = 3.0f

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var locationManager: LocationManager

    private val updateRunnable = object : Runnable {
        override fun run() {
            if (isRunning) {
                pushMockLocation()
                handler.postDelayed(this, UPDATE_INTERVAL_MS)
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        locationManager = getSystemService(Context.LOCATION_SERVICE) as LocationManager
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                targetLatitude = intent.getDoubleExtra(EXTRA_LATITUDE, 0.0)
                targetLongitude = intent.getDoubleExtra(EXTRA_LONGITUDE, 0.0)
                targetAltitude = intent.getDoubleExtra(EXTRA_ALTITUDE, 0.0)
                targetAccuracy = intent.getFloatExtra(EXTRA_ACCURACY, 3.0f)
                startMocking()
            }
            ACTION_STOP -> {
                stopMocking()
                stopSelf()
            }
        }
        return START_STICKY
    }

    private fun startMocking() {
        if (isRunning) return
        try {
            // 注册测试 Provider
            addTestProvider(LocationManager.GPS_PROVIDER)
            addTestProvider(LocationManager.NETWORK_PROVIDER)
            addTestProvider(LocationManager.PASSIVE_PROVIDER)

            isRunning = true
            startForeground(NOTIFICATION_ID, buildNotification("正在模拟位置..."))
            handler.post(updateRunnable)
            Log.i(TAG, "Mock location started: $targetLatitude, $targetLongitude")
        } catch (e: SecurityException) {
            Log.e(TAG, "缺少模拟位置权限，请在开发者选项中授权", e)
            stopSelf()
        } catch (e: Exception) {
            Log.e(TAG, "启动模拟位置失败", e)
            stopSelf()
        }
    }

    private fun addTestProvider(provider: String) {
        try {
            if (!locationManager.isProviderEnabled(provider)) {
                // 部分设备需要先启用
            }
            locationManager.addTestProvider(
                provider,
                false,  // requiresNetwork
                false,  // requiresSatellite
                false,  // requiresCell
                false,  // hasMonetaryCost
                true,   // supportsAltitude
                true,   // supportsSpeed
                true,   // supportsBearing
                0,      // powerRequirement
                1       // accuracy
            )
            locationManager.setTestProviderEnabled(provider, true)
        } catch (e: Exception) {
            Log.w(TAG, "添加测试 Provider 失败: $provider", e)
        }
    }

    private fun pushMockLocation() {
        val time = System.currentTimeMillis()
        val elapsedTime = SystemClock.elapsedRealtimeNanos()

        val location = Location(LocationManager.GPS_PROVIDER).apply {
            latitude = targetLatitude
            longitude = targetLongitude
            altitude = targetAltitude
            accuracy = targetAccuracy
            speed = 0.0f
            bearing = 0.0f
            setTime(time)
            // Android 4.1+ 需要设置 elapsedRealtimeNanos，否则某些应用会忽略
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1) {
                setElapsedRealtimeNanos(elapsedTime)
            }
        }

        // 同时向多个 Provider 注入位置
        injectLocation(LocationManager.GPS_PROVIDER, location)
        injectLocation(LocationManager.NETWORK_PROVIDER, location)
        injectLocation(LocationManager.PASSIVE_PROVIDER, location)
    }

    private fun injectLocation(provider: String, location: Location) {
        try {
            val loc = Location(location)
            loc.setProvider(provider)
            locationManager.setTestProviderLocation(provider, loc)
        } catch (e: Exception) {
            Log.w(TAG, "注入位置失败: $provider", e)
        }
    }

    private fun stopMocking() {
        isRunning = false
        handler.removeCallbacks(updateRunnable)
        try {
            removeTestProvider(LocationManager.GPS_PROVIDER)
            removeTestProvider(LocationManager.NETWORK_PROVIDER)
            removeTestProvider(LocationManager.PASSIVE_PROVIDER)
        } catch (e: Exception) {
            Log.w(TAG, "移除测试 Provider 失败", e)
        }
        Log.i(TAG, "Mock location stopped")
    }

    private fun removeTestProvider(provider: String) {
        try {
            locationManager.setTestProviderEnabled(provider, false)
            locationManager.removeTestProvider(provider)
        } catch (e: Exception) {
            // 忽略
        }
    }

    private fun buildNotification(contentText: String): Notification {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "模拟位置服务",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "用于保持虚拟定位服务运行"
                setShowBadge(false)
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("虚拟定位运行中")
            .setContentText(contentText)
            .setSmallIcon(R.drawable.ic_location_on)
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        stopMocking()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
