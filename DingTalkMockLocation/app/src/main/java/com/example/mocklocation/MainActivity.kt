package com.example.mocklocation

import android.Manifest
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.LinearLayoutManager
import com.example.mocklocation.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var favoritesAdapter: FavoritesAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        PrefsManager.init(this)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupViews()
        setupFavorites()
        requestPermissions()
    }

    private fun setupViews() {
        // 恢复上次的位置
        binding.etLatitude.setText(PrefsManager.getLastLat().toString().takeIf { it != "0.0" } ?: "")
        binding.etLongitude.setText(PrefsManager.getLastLng().toString().takeIf { it != "0.0" } ?: "")

        // 恢复定时设置
        binding.swSchedule.isChecked = PrefsManager.isScheduleEnabled()
        binding.etClockInTime.setText(PrefsManager.getClockInTime())
        binding.etClockOutTime.setText(PrefsManager.getClockOutTime())
        binding.cbWeekdaysOnly.isChecked = PrefsManager.isWeekdaysOnly()

        binding.btnStartMock.setOnClickListener { startMockLocation() }
        binding.btnStopMock.setOnClickListener { stopMockLocation() }
        binding.btnAddFavorite.setOnClickListener { addFavorite() }
        binding.btnOpenDingTalk.setOnClickListener { openDingTalk() }

        binding.swSchedule.setOnCheckedChangeListener { _, isChecked ->
            PrefsManager.setScheduleEnabled(isChecked)
            if (isChecked) {
                saveScheduleSettings()
                ScheduleReceiver.schedule(this)
                Toast.makeText(this, "定时打卡已开启", Toast.LENGTH_SHORT).show()
            } else {
                ScheduleReceiver.cancel(this)
                Toast.makeText(this, "定时打卡已关闭", Toast.LENGTH_SHORT).show()
            }
        }

        updateStatus()
    }

    private fun setupFavorites() {
        favoritesAdapter = FavoritesAdapter(
            items = PrefsManager.getFavorites(),
            onUse = { loc ->
                binding.etLatitude.setText(loc.latitude.toString())
                binding.etLongitude.setText(loc.longitude.toString())
                binding.etAltitude.setText(loc.altitude.toString())
                Toast.makeText(this, "已加载位置：${loc.name}", Toast.LENGTH_SHORT).show()
            },
            onDelete = { loc ->
                AlertDialog.Builder(this)
                    .setTitle("确认删除")
                    .setMessage("确定要删除「${loc.name}」吗？")
                    .setPositiveButton("删除") { _, _ ->
                        PrefsManager.removeFavorite(loc.id)
                        favoritesAdapter.updateList(PrefsManager.getFavorites())
                    }
                    .setNegativeButton("取消", null)
                    .show()
            }
        )
        binding.rvFavorites.apply {
            layoutManager = LinearLayoutManager(this@MainActivity)
            adapter = favoritesAdapter
        }
    }

    private fun startMockLocation() {
        val lat = binding.etLatitude.text.toString().toDoubleOrNull()
        val lng = binding.etLongitude.text.toString().toDoubleOrNull()

        if (lat == null || lng == null) {
            Toast.makeText(this, "请输入有效的经纬度", Toast.LENGTH_SHORT).show()
            return
        }
        if (lat < -90 || lat > 90 || lng < -180 || lng > 180) {
            Toast.makeText(this, "经纬度超出有效范围", Toast.LENGTH_SHORT).show()
            return
        }

        val alt = binding.etAltitude.text.toString().toDoubleOrNull() ?: 0.0

        // 检查是否已授予模拟位置权限
        if (!isMockLocationEnabled()) {
            showMockLocationPermissionDialog()
            return
        }

        PrefsManager.setLastLocation(lat, lng)

        val intent = Intent(this, MockLocationService::class.java).apply {
            action = MockLocationService.ACTION_START
            putExtra(MockLocationService.EXTRA_LATITUDE, lat)
            putExtra(MockLocationService.EXTRA_LONGITUDE, lng)
            putExtra(MockLocationService.EXTRA_ALTITUDE, alt)
            putExtra(MockLocationService.EXTRA_ACCURACY, 3.0f)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
        PrefsManager.setMocking(true)
        updateStatus()
        Toast.makeText(this, "模拟位置已启动", Toast.LENGTH_SHORT).show()
    }

    private fun stopMockLocation() {
        val intent = Intent(this, MockLocationService::class.java).apply {
            action = MockLocationService.ACTION_STOP
        }
        startService(intent)
        PrefsManager.setMocking(false)
        updateStatus()
        Toast.makeText(this, "模拟位置已停止", Toast.LENGTH_SHORT).show()
    }

    private fun addFavorite() {
        val lat = binding.etLatitude.text.toString().toDoubleOrNull()
        val lng = binding.etLongitude.text.toString().toDoubleOrNull()
        if (lat == null || lng == null) {
            Toast.makeText(this, "请先输入有效的经纬度", Toast.LENGTH_SHORT).show()
            return
        }

        val editText = android.widget.EditText(this).apply {
            hint = "请输入位置名称"
            setPadding(40, 30, 40, 30)
        }
        AlertDialog.Builder(this)
            .setTitle("收藏位置")
            .setView(editText)
            .setPositiveButton("保存") { _, _ ->
                val name = editText.text.toString().trim()
                if (name.isEmpty()) {
                    Toast.makeText(this, "名称不能为空", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                val loc = FavoriteLocation(name = name, latitude = lat, longitude = lng)
                PrefsManager.addFavorite(loc)
                favoritesAdapter.updateList(PrefsManager.getFavorites())
                Toast.makeText(this, "收藏成功", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun openDingTalk() {
        try {
            val intent = packageManager.getLaunchIntentForPackage("com.alibaba.android.rimet")
            if (intent != null) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                startActivity(intent)
            } else {
                Toast.makeText(this, "未安装钉钉", Toast.LENGTH_SHORT).show()
            }
        } catch (e: Exception) {
            Toast.makeText(this, "打开钉钉失败", Toast.LENGTH_SHORT).show()
        }
    }

    private fun saveScheduleSettings() {
        val clockIn = binding.etClockInTime.text.toString()
        val clockOut = binding.etClockOutTime.text.toString()
        val weekdaysOnly = binding.cbWeekdaysOnly.isChecked

        if (isValidTime(clockIn) && isValidTime(clockOut)) {
            PrefsManager.setClockInTime(clockIn)
            PrefsManager.setClockOutTime(clockOut)
            PrefsManager.setWeekdaysOnly(weekdaysOnly)
        } else {
            Toast.makeText(this, "时间格式错误，请使用 HH:mm", Toast.LENGTH_SHORT).show()
        }
    }

    private fun isValidTime(time: String): Boolean {
        val parts = time.split(":")
        if (parts.size != 2) return false
        val h = parts[0].toIntOrNull() ?: return false
        val m = parts[1].toIntOrNull() ?: return false
        return h in 0..23 && m in 0..59
    }

    private fun isMockLocationEnabled(): Boolean {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                // Android 6.0+ 通过 AppOpsManager 检查
                val ops = getSystemService(APP_OPS_SERVICE) as android.app.AppOpsManager
                val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    ops.unsafeCheckOpNoThrow(
                        "android:mock_location",
                        android.os.Process.myUid(),
                        packageName
                    )
                } else {
                    @Suppress("DEPRECATION")
                    ops.checkOpNoThrow(
                        "android:mock_location",
                        android.os.Process.myUid(),
                        packageName
                    )
                }
                mode == android.app.AppOpsManager.MODE_ALLOWED
            } else {
                @Suppress("DEPRECATION")
                Settings.Secure.getString(contentResolver, Settings.Secure.ALLOW_MOCK_LOCATION) != "0"
            }
        } catch (e: Exception) {
            false
        }
    }

    private fun showMockLocationPermissionDialog() {
        AlertDialog.Builder(this)
            .setTitle("需要授权模拟位置")
            .setMessage("请前往「设置 → 开发者选项 → 选择模拟位置信息应用」，选择本应用以启用虚拟定位。\n\n是否现在前往开发者选项？")
            .setPositiveButton("前往设置") { _, _ ->
                try {
                    startActivity(Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS))
                } catch (e: Exception) {
                    startActivity(Intent(Settings.ACTION_SETTINGS))
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun updateStatus() {
        if (PrefsManager.isMocking()) {
            binding.tvStatus.text = "● 模拟位置运行中：${PrefsManager.getLastLat()}, ${PrefsManager.getLastLng()}"
            binding.tvStatus.setTextColor(0xFF4CAF50.toInt())
        } else {
            binding.tvStatus.text = "○ 未启动模拟位置"
            binding.tvStatus.setTextColor(0xFF1565C0.toInt())
        }
    }

    private fun requestPermissions() {
        val permissions = mutableListOf<String>()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
            != PackageManager.PERMISSION_GRANTED) {
            permissions.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION)
            != PackageManager.PERMISSION_GRANTED) {
            permissions.add(Manifest.permission.ACCESS_COARSE_LOCATION)
        }
        if (permissions.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, permissions.toTypedArray(), 100)
        }
    }

    override fun onResume() {
        super.onResume()
        updateStatus()
        favoritesAdapter.updateList(PrefsManager.getFavorites())
    }
}
