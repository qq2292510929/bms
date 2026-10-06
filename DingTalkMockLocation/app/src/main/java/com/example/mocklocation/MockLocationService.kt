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
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Message
import android.os.Process
import android.os.SystemClock
import android.util.Log

/**
 * 模拟位置前台服务（参考 GoGoGo 项目方案）
 *
 * 关键改进：
 * 1. Android 12+ (S) 使用 ProviderProperties，旧版本使用 Criteria 常量
 * 2. 100ms / 10Hz 高频注入，压制真实定位
 * 3. HandlerThread + THREAD_PRIORITY_FOREGROUND，降低被回收概率
 * 4. Location 字段填全：accuracy/altitude/bearing/speed/time/elapsedRealtimeNanos/satellites
 * 5. 启动前先 removeTestProvider 再 addTestProvider，避免重复注册异常
 * 6. 同时覆盖 GPS + NETWORK 两个 Provider
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
        private const val HANDLER_MSG_ID = 100
        // 位置更新间隔（毫秒），GoGoGo 使用 100ms / 10Hz
        private const val UPDATE_INTERVAL_MS = 100L
    }

    private var isRunning = false
    private var isStop = true
    private var targetLatitude = 0.0
    private var targetLongitude = 0.0
    private var targetAltitude = 0.0
    private var targetAccuracy = 3.0f

    private lateinit var locationManager: LocationManager
    private var locHandlerThread: HandlerThread? = null
    private var locHandler: Handler? = null

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
            // 启动前台服务保活
            startForeground(NOTIFICATION_ID, buildNotification("正在模拟位置..."))

            // 先移除再注册，避免重复注册抛异常
            removeTestProviderNetwork()
            addTestProviderNetwork()
            removeTestProviderGPS()
            addTestProviderGPS()

            // 初始化位置注入循环
            initGoLocation()

            isRunning = true
            isStop = false
            Log.i(TAG, "Mock location started: $targetLatitude, $targetLongitude")
        } catch (e: SecurityException) {
            Log.e(TAG, "缺少模拟位置权限，请在开发者选项中授权", e)
            stopSelf()
        } catch (e: Exception) {
            Log.e(TAG, "启动模拟位置失败", e)
            stopSelf()
        }
    }

    /**
     * 注册 NETWORK Provider
     * Android 12+ (S) 使用 ProviderProperties，旧版本使用 Criteria 常量
     */
    private fun addTestProviderNetwork() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                locationManager.addTestProvider(
                    LocationManager.NETWORK_PROVIDER,
                    false, false, false, false,
                    true, true, true,
                    android.os.Build.VERSION_CODES.S.let {
                        Class.forName("android.location.ProviderProperties")
                            .getField("POWER_USAGE_LOW").getInt(null)
                    },
                    Class.forName("android.location.ProviderProperties")
                        .getField("ACCURACY_COARSE").getInt(null)
                )
            } else {
                @Suppress("DEPRECATION")
                locationManager.addTestProvider(
                    LocationManager.NETWORK_PROVIDER,
                    false, false, false, false,
                    true, true, true,
                    android.location.Criteria.POWER_LOW,
                    android.location.Criteria.ACCURACY_COARSE
                )
            }
            locationManager.setTestProviderEnabled(LocationManager.NETWORK_PROVIDER, true)
        } catch (e: Exception) {
            Log.w(TAG, "添加 NETWORK Provider 失败", e)
        }
    }

    /**
     * 注册 GPS Provider
     * Android 12+ (S) 使用 ProviderProperties，旧版本使用 Criteria 常量
     */
    private fun addTestProviderGPS() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                locationManager.addTestProvider(
                    LocationManager.GPS_PROVIDER,
                    false, true, false, false,
                    true, true, true,
                    Class.forName("android.location.ProviderProperties")
                        .getField("POWER_USAGE_HIGH").getInt(null),
                    Class.forName("android.location.ProviderProperties")
                        .getField("ACCURACY_FINE").getInt(null)
                )
            } else {
                @Suppress("DEPRECATION")
                locationManager.addTestProvider(
                    LocationManager.GPS_PROVIDER,
                    false, true, false, false,
                    true, true, true,
                    android.location.Criteria.POWER_HIGH,
                    android.location.Criteria.ACCURACY_FINE
                )
            }
            locationManager.setTestProviderEnabled(LocationManager.GPS_PROVIDER, true)
        } catch (e: Exception) {
            Log.w(TAG, "添加 GPS Provider 失败", e)
        }
    }

    /**
     * 初始化位置注入循环
     * 使用 HandlerThread + THREAD_PRIORITY_FOREGROUND，降低被回收概率
     */
    private fun initGoLocation() {
        locHandlerThread = HandlerThread("MockLocationThread", Process.THREAD_PRIORITY_FOREGROUND).apply {
            start()
        }
        locHandler = Handler(locHandlerThread!!.looper) { msg ->
            try {
                Thread.sleep(UPDATE_INTERVAL_MS)
                if (!isStop) {
                    setLocationNetwork()
                    setLocationGPS()
                    locHandler?.sendEmptyMessage(HANDLER_MSG_ID)
                }
            } catch (e: InterruptedException) {
                Log.w(TAG, "位置注入循环被中断", e)
            }
            true
        }
        locHandler?.sendEmptyMessage(HANDLER_MSG_ID)
    }

    /**
     * 向 NETWORK Provider 注入位置
     * 填全所有字段，增加可信度
     */
    private fun setLocationNetwork() {
        try {
            val loc = Location(LocationManager.NETWORK_PROVIDER).apply {
                latitude = targetLatitude
                longitude = targetLongitude
                altitude = targetAltitude
                accuracy = targetAccuracy
                speed = 0.0f
                bearing = 0.0f
                setTime(System.currentTimeMillis())
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1) {
                    setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos())
                }
                // 伪造卫星数量，增加可信度
                extras = Bundle().apply { putInt("satellites", 7) }
            }
            locationManager.setTestProviderLocation(LocationManager.NETWORK_PROVIDER, loc)
        } catch (e: Exception) {
            Log.w(TAG, "注入 NETWORK 位置失败", e)
        }
    }

    /**
     * 向 GPS Provider 注入位置
     * 填全所有字段，增加可信度
     */
    private fun setLocationGPS() {
        try {
            val loc = Location(LocationManager.GPS_PROVIDER).apply {
                latitude = targetLatitude
                longitude = targetLongitude
                altitude = targetAltitude
                accuracy = targetAccuracy
                speed = 0.0f
                bearing = 0.0f
                setTime(System.currentTimeMillis())
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1) {
                    setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos())
                }
                // 伪造卫星数量，增加可信度
                extras = Bundle().apply { putInt("satellites", 7) }
            }
            locationManager.setTestProviderLocation(LocationManager.GPS_PROVIDER, loc)
        } catch (e: Exception) {
            Log.w(TAG, "注入 GPS 位置失败", e)
        }
    }

    private fun removeTestProviderGPS() {
        try {
            locationManager.setTestProviderEnabled(LocationManager.GPS_PROVIDER, false)
            locationManager.removeTestProvider(LocationManager.GPS_PROVIDER)
        } catch (e: Exception) {
            // 忽略
        }
    }

    private fun removeTestProviderNetwork() {
        try {
            locationManager.setTestProviderEnabled(LocationManager.NETWORK_PROVIDER, false)
            locationManager.removeTestProvider(LocationManager.NETWORK_PROVIDER)
        } catch (e: Exception) {
            // 忽略
        }
    }

    private fun stopMocking() {
        isStop = true
        isRunning = false
        locHandler?.removeCallbacksAndMessages(null)
        locHandlerThread?.quitSafely()
        locHandlerThread = null
        locHandler = null

        removeTestProviderGPS()
        removeTestProviderNetwork()
        Log.i(TAG, "Mock location stopped")
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
