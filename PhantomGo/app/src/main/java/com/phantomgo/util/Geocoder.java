package com.phantomgo.util;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;

public class Geocoder {

    private static final OkHttpClient client = new OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(10, TimeUnit.SECONDS)
            .build();

    public static class SearchResult {
        public double lat;
        public double lng;
        public String name;
        public String detail;

        public SearchResult(double lat, double lng, String name, String detail) {
            this.lat = lat;
            this.lng = lng;
            this.name = name;
            this.detail = detail;
        }
    }

    public static List<SearchResult> search(String query) throws IOException {
        List<SearchResult> results = new ArrayList<>();
        String url = "https://photon.komoot.io/api/?q=" +
                java.net.URLEncoder.encode(query, "UTF-8") + "&limit=8";
        Request req = new Request.Builder().url(url).build();
        try (Response res = client.newCall(req).execute()) {
            if (!res.isSuccessful() || res.body() == null) return results;
            String body = res.body().string();
            JSONObject json = new JSONObject(body);
            JSONArray features = json.optJSONArray("features");
            if (features == null) return results;
            for (int i = 0; i < features.length(); i++) {
                JSONObject f = features.getJSONObject(i);
                JSONArray coords = f.getJSONObject("geometry").getJSONArray("coordinates");
                double lng = coords.getDouble(0);
                double lat = coords.getDouble(1);
                JSONObject p = f.optJSONObject("properties");
                String name = p != null ? opt(p, "name", "name") : "未知位置";
                if (name == null || name.isEmpty()) {
                    name = p != null ? opt(p, "city", "locality") : "未知位置";
                }
                String detail = "";
                if (p != null) {
                    String city = p.optString("city", "");
                    String state = p.optString("state", "");
                    String country = p.optString("country", "");
                    detail = joinNonEmpty(", ", city, state, country);
                }
                results.add(new SearchResult(lat, lng, name, detail));
            }
        } catch (Exception e) {
            throw new IOException("Search failed: " + e.getMessage(), e);
        }
        return results;
    }

    public static String reverse(double lat, double lng) throws IOException {
        String url = "https://photon.komoot.io/reverse?lon=" + lng + "&lat=" + lat;
        Request req = new Request.Builder().url(url).build();
        try (Response res = client.newCall(req).execute()) {
            if (!res.isSuccessful() || res.body() == null) return "未知位置";
            String body = res.body().string();
            JSONObject json = new JSONObject(body);
            JSONArray features = json.optJSONArray("features");
            if (features == null || features.length() == 0) return "未知位置";
            JSONObject f = features.getJSONObject(0);
            JSONObject p = f.optJSONObject("properties");
            if (p == null) return "未知位置";
            String name = opt(p, "name", "city");
            if (name == null || name.isEmpty()) name = p.optString("country", "未知位置");
            return name;
        } catch (Exception e) {
            throw new IOException("Reverse geocode failed: " + e.getMessage(), e);
        }
    }

    private static String opt(JSONObject obj, String... keys) {
        for (String k : keys) {
            String v = obj.optString(k, "");
            if (!v.isEmpty()) return v;
        }
        return "";
    }

    private static String joinNonEmpty(String sep, String... parts) {
        StringBuilder sb = new StringBuilder();
        for (String p : parts) {
            if (p != null && !p.isEmpty()) {
                if (sb.length() > 0) sb.append(sep);
                sb.append(p);
            }
        }
        return sb.toString();
    }
}
