package com.example.mocklocation

import android.app.AlarmManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log
import java.util.Calendar

/**
 * 定时打卡广播接收器
 * 在指定时间自动启动模拟位置服务，并可选地打开钉钉。
 */
class ScheduleReceiver : BroadcastReceiver() {

    companion object {
        const val TAG = "ScheduleReceiver"
        const val ACTION_CLOCK_IN = "com.example.mocklocation.CLOCK_IN"
        const val ACTION_CLOCK_OUT = "com.example.mocklocation.CLOCK_OUT"

        /**
         * 设置定时打卡任务
         */
        fun schedule(context: Context) {
            if (!PrefsManager.isScheduleEnabled()) {
                cancel(context)
                return
            }
            scheduleNext(context, PrefsManager.getClockInTime(), ACTION_CLOCK_IN)
            scheduleNext(context, PrefsManager.getClockOutTime(), ACTION_CLOCK_OUT)
            Log.i(TAG, "定时打卡已设置：上班=${PrefsManager.getClockInTime()} 下班=${PrefsManager.getClockOutTime()}")
        }

        private fun scheduleNext(context: Context, timeStr: String, action: String) {
            val parts = timeStr.split(":")
            if (parts.size != 2) return
            val hour = parts[0].toIntOrNull() ?: return
            val minute = parts[1].toIntOrNull() ?: return

            val calendar = Calendar.getInstance().apply {
                set(Calendar.HOUR_OF_DAY, hour)
                set(Calendar.MINUTE, minute)
                set(Calendar.SECOND, 0)
                set(Calendar.MILLISECOND, 0)
                // 如果时间已过，设置为明天
                if (before(Calendar.getInstance())) {
                    add(Calendar.DAY_OF_MONTH, 1)
                }
            }

            val intent = Intent(context, ScheduleReceiver::class.java).apply {
                this.action = action
            }
            val requestCode = if (action == ACTION_CLOCK_IN) 1001 else 1002
            val pendingIntent = PendingIntent.getBroadcast(
                context, requestCode, intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )

            val alarmManager = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            alarmManager.setExactAndAllowWhileIdle(
                AlarmManager.RTC_WAKEUP,
                calendar.timeInMillis,
                pendingIntent
            )
        }

        fun cancel(context: Context) {
            val alarmManager = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            for (requestCode in listOf(1001, 1002)) {
                val intent = Intent(context, ScheduleReceiver::class.java)
                val pendingIntent = PendingIntent.getBroadcast(
                    context, requestCode, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                )
                alarmManager.cancel(pendingIntent)
            }
        }
    }

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        Log.i(TAG, "收到定时任务: $action")

        // 仅工作日触发
        if (PrefsManager.isWeekdaysOnly()) {
            val dayOfWeek = Calendar.getInstance().get(Calendar.DAY_OF_WEEK)
            if (dayOfWeek == Calendar.SATURDAY || dayOfWeek == Calendar.SUNDAY) {
                Log.i(TAG, "今天是周末，跳过打卡")
                scheduleNextDay(context)
                return
            }
        }

        // 启动模拟位置服务
        val lastLat = PrefsManager.getLastLat()
        val lastLng = PrefsManager.getLastLng()
        if (lastLat != 0.0 && lastLng != 0.0) {
            startMockLocation(context, lastLat, lastLng)
        }

        // 尝试打开钉钉
        launchDingTalk(context)

        // 设置下一次定时
        scheduleNextDay(context)
    }

    private fun startMockLocation(context: Context, lat: Double, lng: Double) {
        val serviceIntent = Intent(context, MockLocationService::class.java).apply {
            this.action = MockLocationService.ACTION_START
            putExtra(MockLocationService.EXTRA_LATITUDE, lat)
            putExtra(MockLocationService.EXTRA_LONGITUDE, lng)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(serviceIntent)
        } else {
            context.startService(serviceIntent)
        }
        PrefsManager.setMocking(true)
    }

    private fun launchDingTalk(context: Context) {
        try {
            val pm = context.packageManager
            val launchIntent = pm.getLaunchIntentForPackage("com.alibaba.android.rimet")
            if (launchIntent != null) {
                launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(launchIntent)
                Log.i(TAG, "已打开钉钉")
            } else {
                Log.w(TAG, "未安装钉钉")
            }
        } catch (e: Exception) {
            Log.e(TAG, "打开钉钉失败", e)
        }
    }

    private fun scheduleNextDay(context: Context) {
        schedule(context)
    }
}
