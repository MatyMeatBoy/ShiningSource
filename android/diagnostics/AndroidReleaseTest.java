package com.shiningsource.android;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.IOException;

/** Run with AndroidRelease on an Android device via app_process. */
public final class AndroidReleaseTest {
    private static final String APK = "ShiningSource-0.10.0-android.apk";
    private static final String URL = "https://github.com/MatyMeatBoy/ShiningSource/releases/download/v0.10.0/" + APK;
    private static final String DIGEST = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    private static JSONObject release() throws Exception {
        JSONObject apk = new JSONObject().put("name", APK).put("browser_download_url", URL)
                .put("state", "uploaded").put("size", 12345).put("digest", DIGEST);
        return new JSONObject().put("tag_name", "v0.10.0").put("draft", false).put("prerelease", false)
                .put("assets", new JSONArray().put(apk));
    }

    private static JSONObject asset(JSONObject release) throws Exception {
        return release.getJSONArray("assets").getJSONObject(0);
    }

    private static void rejects(JSONObject release) throws Exception {
        try {
            AndroidRelease.parse(release, "0.9.9");
        } catch (IOException expected) {
            return;
        }
        throw new AssertionError("Accepted invalid metadata " + release);
    }

    public static void main(String[] args) throws Exception {
        AndroidRelease update = AndroidRelease.parse(release(), "0.9.9");
        if (update == null || !"0.10.0".equals(update.version) || update.size != 12345) {
            throw new AssertionError("Could not select the newer stable APK");
        }
        if (AndroidRelease.parse(release(), "0.10.0") != null
                || AndroidRelease.parse(release(), "1.0.0") != null) {
            throw new AssertionError("Offered a reinstall or downgrade");
        }
        rejects(release().put("tag_name", "v0.10.0-rc.1"));
        rejects(release().put("prerelease", true));
        rejects(release().put("draft", true));
        rejects(release().put("assets", new JSONArray()));
        JSONObject missingDigest = release();
        asset(missingDigest).remove("digest");
        rejects(missingDigest);
        JSONObject wrongUrl = release();
        asset(wrongUrl).put("browser_download_url", "https://example.com/" + APK);
        rejects(wrongUrl);
        JSONObject oversized = release();
        asset(oversized).put("size", 1L << 30);
        rejects(oversized);
        JSONObject duplicate = release();
        duplicate.getJSONArray("assets").put(asset(duplicate));
        rejects(duplicate);
        System.out.println("Android release metadata checks passed");
    }
}
