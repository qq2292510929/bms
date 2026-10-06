package com.phantomgo.util;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

public class HistoryManager {

    private static final String PREF_NAME = "phantomgo_history";
    private static final String KEY_LIST = "list";
    private static final int MAX_SIZE = 30;

    public static class HistoryItem {
        public double lat;
        public double lng;
        public String name;
        public long time;

        public HistoryItem(double lat, double lng, String name, long time) {
            this.lat = lat;
            this.lng = lng;
            this.name = name;
            this.time = time;
        }
    }

    public static List<HistoryItem> load(Context context) {
        List<HistoryItem> list = new ArrayList<>();
        SharedPreferences sp = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
        String json = sp.getString(KEY_LIST, null);
        if (json == null) return list;
        try {
            JSONArray arr = new JSONArray(json);
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                list.add(new HistoryItem(
                        o.getDouble("lat"),
                        o.getDouble("lng"),
                        o.getString("name"),
                        o.getLong("time")
                ));
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return list;
    }

    public static void save(Context context, List<HistoryItem> list) {
        JSONArray arr = new JSONArray();
        try {
            for (HistoryItem item : list) {
                JSONObject o = new JSONObject();
                o.put("lat", item.lat);
                o.put("lng", item.lng);
                o.put("name", item.name);
                o.put("time", item.time);
                arr.put(o);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
                .edit()
                .putString(KEY_LIST, arr.toString())
                .apply();
    }

    public static void add(Context context, double lat, double lng, String name) {
        List<HistoryItem> list = load(context);
        // remove duplicates by rounded coordinate
        String key = String.format("%.4f,%.4f", lat, lng);
        list.removeIf(it -> String.format("%.4f,%.4f", it.lat, it.lng).equals(key));
        list.add(0, new HistoryItem(lat, lng, name, System.currentTimeMillis()));
        while (list.size() > MAX_SIZE) list.remove(list.size() - 1);
        save(context, list);
    }

    public static void clear(Context context) {
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE).edit().clear().apply();
    }
}
