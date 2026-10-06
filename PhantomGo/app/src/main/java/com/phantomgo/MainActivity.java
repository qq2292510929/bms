package com.phantomgo;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import com.google.android.material.floatingactionbutton.FloatingActionButton;
import com.phantomgo.mock.MockLocationService;
import com.phantomgo.ui.JoystickView;
import com.phantomgo.util.Geocoder;
import com.phantomgo.util.HistoryManager;

import org.osmdroid.config.Configuration;
import org.osmdroid.tileprovider.tilesource.TileSourceFactory;
import org.osmdroid.util.GeoPoint;
import org.osmdroid.views.MapView;
import org.osmdroid.views.overlay.Marker;
import org.osmdroid.views.overlay.Polyline;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executors;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "PhantomGo";
    private static final int REQ_LOCATION = 1001;

    private MapView map;
    private Marker pin;
    private Polyline trail;
    private List<GeoPoint> trailPoints = new ArrayList<>();

    private EditText searchInput;
    private ProgressBar searchSpinner;
    private LinearLayout searchResults;
    private View statusDot;
    private TextView statusText;
    private FloatingActionButton historyFab;
    private LinearLayout historyPanel;
    private ListView historyList;
    private TextView clearHistory;
    private EditText latInput, lngInput;
    private Button applyCoord;
    private FloatingActionButton startBtn;
    private TextView curLat, curLng, curName;
    private JoystickView joystick;

    private double currentLat = 39.9042;
    private double currentLng = 116.4074;
    private boolean mockActive = false;
    private float speed = 3f; // m/s
    private float joyX = 0f, joyY = 0f;
    private boolean joyActive = false;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private long lastMoveTick = 0;
    private Runnable moveRunnable;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // osmdroid config
        Configuration.getInstance().setUserAgentValue(getPackageName());
        org.osmdroid.config.IConfigurationProvider cfg =
                org.osmdroid.config.Configuration.getInstance();
        cfg.setOsmdroidTileCache(new java.io.File(getCacheDir().getAbsolutePath() + "/osmdroid"));

        setContentView(R.layout.activity_main);

        bindViews();
        setupMap();
        setupSearch();
        setupHistory();
        setupCoordinateInput();
        setupJoystick();
        setupStartButton();
        requestPermissions();

        updateLocationCard();
    }

    private void bindViews() {
        map = findViewById(R.id.mapView);
        searchInput = findViewById(R.id.searchInput);
        searchSpinner = findViewById(R.id.searchSpinner);
        searchResults = findViewById(R.id.searchResults);
        statusDot = findViewById(R.id.statusDot);
        statusText = findViewById(R.id.statusText);
        historyFab = findViewById(R.id.historyFab);
        historyPanel = findViewById(R.id.historyPanel);
        historyList = findViewById(R.id.historyList);
        clearHistory = findViewById(R.id.clearHistory);
        latInput = findViewById(R.id.latInput);
        lngInput = findViewById(R.id.lngInput);
        applyCoord = findViewById(R.id.applyCoord);
        startBtn = findViewById(R.id.startBtn);
        curLat = findViewById(R.id.curLat);
        curLng = findViewById(R.id.curLng);
        curName = findViewById(R.id.curName);
        joystick = findViewById(R.id.joystick);
    }

    private void setupMap() {
        map.setTileSource(TileSourceFactory.MAPNIK);
        map.setMultiTouchControls(true);
        map.getController().setZoom(13.0);
        map.getController().setCenter(new GeoPoint(currentLat, currentLng));

        pin = new Marker(map);
        pin.setPosition(new GeoPoint(currentLat, currentLng));
        pin.setIcon(getMarkerDrawable());
        pin.setAnchor(Marker.ANCHOR_CENTER, Marker.ANCHOR_CENTER);
        map.getOverlays().add(pin);

        trail = new Polyline();
        trail.setColor(0xFF22D3EE);
        trail.setWidth(3f);
        map.getOverlays().add(trail);

        map.setOnTouchListener((v, event) -> {
            if (event.getAction() == MotionEvent.ACTION_UP) {
                GeoPoint gp = (GeoPoint) map.getProjection().fromPixels(
                        (int) event.getX(), (int) event.getY());
                setLocation(gp.getLatitude(), gp.getLongitude(), true);
            }
            return false;
        });
    }

    private Drawable getMarkerDrawable() {
        Drawable d = ContextCompat.getDrawable(this, R.drawable.marker_pin);
        if (d != null) {
            int size = (int) (32 * getResources().getDisplayMetrics().density);
            d.setBounds(0, 0, size, size);
        }
        return d;
    }

    private void setupSearch() {
        searchInput.addTextChangedListener(new TextWatcher() {
            private Runnable debounce;
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {
                if (debounce != null) mainHandler.removeCallbacks(debounce);
                String q = s.toString().trim();
                if (q.isEmpty()) {
                    searchResults.removeAllViews();
                    searchResults.setVisibility(View.GONE);
                    return;
                }
                searchSpinner.setVisibility(View.VISIBLE);
                debounce = () -> doSearch(q);
                mainHandler.postDelayed(debounce, 400);
            }
            @Override public void afterTextChanged(Editable s) {}
        });

        searchInput.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_SEARCH) {
                hideKeyboard();
                return true;
            }
            return false;
        });
    }

    private void doSearch(String query) {
        Executors.newSingleThreadExecutor().execute(() -> {
            try {
                List<Geocoder.SearchResult> results = Geocoder.search(query);
                mainHandler.post(() -> renderSearchResults(results));
            } catch (Exception e) {
                Log.e(TAG, "search error", e);
                mainHandler.post(() -> Toast.makeText(this, "搜索失败", Toast.LENGTH_SHORT).show());
            } finally {
                mainHandler.post(() -> searchSpinner.setVisibility(View.GONE));
            }
        });
    }

    private void renderSearchResults(List<Geocoder.SearchResult> results) {
        searchResults.removeAllViews();
        if (results.isEmpty()) {
            searchResults.setVisibility(View.GONE);
            return;
        }
        for (Geocoder.SearchResult r : results) {
            View item = getLayoutInflater().inflate(R.layout.item_search_result, searchResults, false);
            ((TextView) item.findViewById(R.id.rName)).setText(r.name);
            String coord = String.format("%.5f, %.5f", r.lat, r.lng);
            if (r.detail != null && !r.detail.isEmpty()) coord += "  ·  " + r.detail;
            ((TextView) item.findViewById(R.id.rCoord)).setText(coord);
            item.setOnClickListener(v -> {
                setLocation(r.lat, r.lng, true);
                searchInput.setText(r.name);
                searchResults.setVisibility(View.GONE);
                hideKeyboard();
                Toast.makeText(this, "已定位到搜索结果", Toast.LENGTH_SHORT).show();
            });
            searchResults.addView(item);
        }
        searchResults.setVisibility(View.VISIBLE);
    }

    private void setupHistory() {
        historyFab.setOnClickListener(v -> {
            if (historyPanel.getVisibility() == View.VISIBLE) {
                historyPanel.setVisibility(View.GONE);
            } else {
                refreshHistory();
                historyPanel.setVisibility(View.VISIBLE);
            }
        });

        clearHistory.setOnClickListener(v -> {
            HistoryManager.clear(this);
            refreshHistory();
            Toast.makeText(this, "历史已清空", Toast.LENGTH_SHORT).show();
        });
    }

    private void refreshHistory() {
        List<HistoryManager.HistoryItem> items = HistoryManager.load(this);
        List<String> display = new ArrayList<>();
        for (HistoryManager.HistoryItem it : items) {
            display.add(it.name + "\n" + String.format("%.5f, %.5f", it.lat, it.lng)
                    + "  " + timeAgo(it.time));
        }
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                R.layout.item_history, R.id.hiName, display);
        historyList.setAdapter(adapter);
        historyList.setOnItemClickListener((parent, view, position, id) -> {
            HistoryManager.HistoryItem it = items.get(position);
            setLocation(it.lat, it.lng, true);
            historyPanel.setVisibility(View.GONE);
        });
    }

    private String timeAgo(long ts) {
        long s = (System.currentTimeMillis() - ts) / 1000;
        if (s < 60) return "刚刚";
        if (s < 3600) return (s / 60) + "分钟前";
        if (s < 86400) return (s / 3600) + "小时前";
        return (s / 86400) + "天前";
    }

    private void setupCoordinateInput() {
        applyCoord.setOnClickListener(v -> {
            try {
                double lat = Double.parseDouble(latInput.getText().toString().trim());
                double lng = Double.parseDouble(lngInput.getText().toString().trim());
                if (lat < -90 || lat > 90 || lng < -180 || lng > 180) {
                    Toast.makeText(this, "坐标超出范围", Toast.LENGTH_SHORT).show();
                    return;
                }
                setLocation(lat, lng, true);
                Toast.makeText(this, "坐标已应用", Toast.LENGTH_SHORT).show();
            } catch (NumberFormatException e) {
                Toast.makeText(this, "请输入有效坐标", Toast.LENGTH_SHORT).show();
            }
        });
    }

    private void setupJoystick() {
        joystick.setOnMoveListener((x, y, active) -> {
            joyX = x;
            joyY = y;
            joyActive = active;
            if (!mockActive && active) {
                Toast.makeText(this, "请先启动虚拟定位", Toast.LENGTH_SHORT).show();
            }
        });
    }

    private void setupStartButton() {
        startBtn.setOnClickListener(v -> toggleMock());
    }

    private void toggleMock() {
        if (mockActive) {
            stopMock();
        } else {
            if (checkMockPermission()) {
                startMock();
            } else {
                Toast.makeText(this, R.string.mock_not_enabled, Toast.LENGTH_LONG).show();
            }
        }
    }

    private boolean checkMockPermission() {
        try {
            // Try adding a test provider to verify mock location is enabled
            android.location.LocationManager lm =
                    (android.location.LocationManager) getSystemService(LOCATION_SERVICE);
            if (lm.getProvider(android.location.LocationManager.GPS_PROVIDER) != null) {
                lm.removeTestProvider(android.location.LocationManager.GPS_PROVIDER);
            }
            lm.addTestProvider(android.location.LocationManager.GPS_PROVIDER,
                    false, false, false, false,
                    true, true, true,
                    android.location.Criteria.POWER_LOW,
                    android.location.Criteria.ACCURACY_FINE);
            lm.setTestProviderEnabled(android.location.LocationManager.GPS_PROVIDER, true);
            lm.removeTestProvider(android.location.LocationManager.GPS_PROVIDER);
            return true;
        } catch (SecurityException e) {
            return false;
        } catch (Exception e) {
            return false;
        }
    }

    private void startMock() {
        mockActive = true;
        statusText.setText(R.string.status_active);
        statusText.setTextColor(ContextCompat.getColor(this, R.color.accent_green));
        startBtn.setImageResource(android.R.drawable.ic_media_pause);
        startBtn.setBackgroundTintList(
                android.content.res.ColorStateList.valueOf(ContextCompat.getColor(this, R.color.accent_green)));

        Intent intent = new Intent(this, MockLocationService.class);
        intent.putExtra(MockLocationService.EXTRA_LAT, currentLat);
        intent.putExtra(MockLocationService.EXTRA_LNG, currentLng);
        ContextCompat.startForegroundService(this, intent);

        startMovementLoop();
        Toast.makeText(this, "虚拟定位已启动", Toast.LENGTH_SHORT).show();
    }

    private void stopMock() {
        mockActive = false;
        statusText.setText(R.string.status_inactive);
        statusText.setTextColor(ContextCompat.getColor(this, R.color.accent_red));
        startBtn.setImageResource(R.drawable.ic_play);
        startBtn.setBackgroundTintList(
                android.content.res.ColorStateList.valueOf(ContextCompat.getColor(this, R.color.accent_cyan)));

        stopService(new Intent(this, MockLocationService.class));
        if (moveRunnable != null) mainHandler.removeCallbacks(moveRunnable);
        Toast.makeText(this, "虚拟定位已停止", Toast.LENGTH_SHORT).show();
    }

    private void startMovementLoop() {
        lastMoveTick = System.currentTimeMillis();
        moveRunnable = new Runnable() {
            @Override
            public void run() {
                if (!mockActive) return;
                long now = System.currentTimeMillis();
                float dt = (now - lastMoveTick) / 1000f;
                lastMoveTick = now;

                if (joyActive && (Math.abs(joyX) > 0.05 || Math.abs(joyY) > 0.05)) {
                    float mag = (float) Math.min(1, Math.hypot(joyX, joyY));
                    float moveSpeed = mag * speed;
                    // Convert meters to degrees
                    double dLat = (joyY * moveSpeed * dt) / 111320.0;
                    double dLng = (joyX * moveSpeed * dt) /
                            (111320.0 * Math.cos(Math.toRadians(currentLat)));
                    currentLat -= dLat; // screen y is inverted
                    currentLng += dLng;

                    pin.setPosition(new GeoPoint(currentLat, currentLng));
                    GeoPoint gp = new GeoPoint(currentLat, currentLng);
                    trailPoints.add(gp);
                    if (trailPoints.size() > 500) trailPoints.remove(0);
                    trail.setPoints(trailPoints);
                    map.getController().animateTo(gp);

                    // Update mock service with new location
                    updateServiceLocation();
                    updateLocationCard();
                }
                mainHandler.postDelayed(this, 100);
            }
        };
        mainHandler.post(moveRunnable);
    }

    private void updateServiceLocation() {
        Intent intent = new Intent(this, MockLocationService.class);
        intent.putExtra(MockLocationService.EXTRA_LAT, currentLat);
        intent.putExtra(MockLocationService.EXTRA_LNG, currentLng);
        startService(intent);
    }

    private void setLocation(double lat, double lng, boolean animate) {
        currentLat = lat;
        currentLng = lng;
        pin.setPosition(new GeoPoint(lat, lng));
        if (animate) {
            map.getController().animateTo(new GeoPoint(lat, lng), 14.0, 800L);
        } else {
            map.getController().setCenter(new GeoPoint(lat, lng));
        }
        if (mockActive) updateServiceLocation();
        updateLocationCard();
        HistoryManager.add(this, lat, lng, curName.getText().toString());
        reverseGeocode(lat, lng);
    }

    private void reverseGeocode(double lat, double lng) {
        Executors.newSingleThreadExecutor().execute(() -> {
            try {
                String name = Geocoder.reverse(lat, lng);
                mainHandler.post(() -> curName.setText(name));
            } catch (Exception e) {
                Log.e(TAG, "reverse geocode error", e);
            }
        });
    }

    private void updateLocationCard() {
        curLat.setText(String.format("%.6f", currentLat));
        curLng.setText(String.format("%.6f", currentLng));
        latInput.setText(String.format("%.6f", currentLat));
        lngInput.setText(String.format("%.6f", currentLng));
    }

    private void requestPermissions() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this,
                    new String[]{Manifest.permission.ACCESS_FINE_LOCATION,
                            Manifest.permission.ACCESS_COARSE_LOCATION}, REQ_LOCATION);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_LOCATION) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                Toast.makeText(this, "定位权限已授予", Toast.LENGTH_SHORT).show();
            }
        }
    }

    private void hideKeyboard() {
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (imm != null) imm.hideSoftInputFromWindow(searchInput.getWindowToken(), 0);
    }

    @Override
    protected void onResume() {
        super.onResume();
        map.onResume();
    }

    @Override
    protected void onPause() {
        super.onPause();
        map.onPause();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (mockActive) stopService(new Intent(this, MockLocationService.class));
    }
}
