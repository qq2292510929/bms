package com.example.mocklocation

import android.content.Context
import android.content.SharedPreferences
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken

/**
 * 位置收藏与配置的本地存储工具
 */
object PrefsManager {

    private const val PREFS_NAME = "mock_location_prefs"
    private const val KEY_FAVORITES = "favorites"
    private const val KEY_LAST_LAT = "last_lat"
    private const val KEY_LAST_LNG = "last_lng"
    private const val KEY_IS_MOCKING = "is_mocking"
    private const val KEY_SCHEDULE_ENABLED = "schedule_enabled"
    private const val KEY_CLOCK_IN_TIME = "clock_in_time"
    private const val KEY_CLOCK_OUT_TIME = "clock_out_time"
    private const val KEY_WEEKDAYS_ONLY = "weekdays_only"

    private lateinit var prefs: SharedPreferences
    private val gson = Gson()

    fun init(context: Context) {
        prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    }

    fun getFavorites(): MutableList<FavoriteLocation> {
        val json = prefs.getString(KEY_FAVORITES, null) ?: return mutableListOf()
        val type = object : TypeToken<MutableList<FavoriteLocation>>() {}.type
        return gson.fromJson(json, type) ?: mutableListOf()
    }

    fun saveFavorites(list: List<FavoriteLocation>) {
        prefs.edit().putString(KEY_FAVORITES, gson.toJson(list)).apply()
    }

    fun addFavorite(loc: FavoriteLocation) {
        val list = getFavorites()
        list.add(loc)
        saveFavorites(list)
    }

    fun removeFavorite(id: Long) {
        val list = getFavorites().filter { it.id != id }
        saveFavorites(list)
    }

    fun setLastLocation(lat: Double, lng: Double) {
        prefs.edit()
            .putString(KEY_LAST_LAT, lat.toString())
            .putString(KEY_LAST_LNG, lng.toString())
            .apply()
    }

    fun getLastLat(): Double = prefs.getString(KEY_LAST_LAT, "0")?.toDoubleOrNull() ?: 0.0
    fun getLastLng(): Double = prefs.getString(KEY_LAST_LNG, "0")?.toDoubleOrNull() ?: 0.0

    fun setMocking(value: Boolean) {
        prefs.edit().putBoolean(KEY_IS_MOCKING, value).apply()
    }
    fun isMocking(): Boolean = prefs.getBoolean(KEY_IS_MOCKING, false)

    fun setScheduleEnabled(value: Boolean) {
        prefs.edit().putBoolean(KEY_SCHEDULE_ENABLED, value).apply()
    }
    fun isScheduleEnabled(): Boolean = prefs.getBoolean(KEY_SCHEDULE_ENABLED, false)

    fun setClockInTime(time: String) {
        prefs.edit().putString(KEY_CLOCK_IN_TIME, time).apply()
    }
    fun getClockInTime(): String = prefs.getString(KEY_CLOCK_IN_TIME, "08:30") ?: "08:30"

    fun setClockOutTime(time: String) {
        prefs.edit().putString(KEY_CLOCK_OUT_TIME, time).apply()
    }
    fun getClockOutTime(): String = prefs.getString(KEY_CLOCK_OUT_TIME, "18:00") ?: "18:00"

    fun setWeekdaysOnly(value: Boolean) {
        prefs.edit().putBoolean(KEY_WEEKDAYS_ONLY, value).apply()
    }
    fun isWeekdaysOnly(): Boolean = prefs.getBoolean(KEY_WEEKDAYS_ONLY, true)
}
