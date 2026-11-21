package com.shiningsource.android;

import android.content.Context;
import android.os.SystemClock;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.DigestOutputStream;
import java.security.GeneralSecurityException;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.cert.CertificateFactory;
import java.util.regex.Pattern;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.TrustManagerFactory;

final class AndroidRelease {
    static final String PAGE = "https://github.com/MatyMeatBoy/ShiningSource/releases/latest";
    private static final String API = "https://api.github.com/repos/MatyMeatBoy/ShiningSource/releases/latest";
    private static final String DOWNLOAD = "https://github.com/MatyMeatBoy/ShiningSource/releases/download/";
    private static final Pattern VERSION = Pattern.compile("(0|[1-9][0-9]{0,8})(\\.(0|[1-9][0-9]{0,8})){2}");
    private static final long MAX_APK_SIZE = 256L * 1024 * 1024;

    final String version;
    private final String url;
    private final String digest;
    final long size;

    private AndroidRelease(String version, String url, String digest, long size) {
        this.version = version;
        this.url = url;
        this.digest = digest;
        this.size = size;
    }

    static AndroidRelease check(Context context, String installedVersion) throws IOException, JSONException {
        ByteArrayOutputStream metadata = new ByteArrayOutputStream();
        fetch(context, API, metadata, 1024 * 1024);
        return parse(new JSONObject(new String(metadata.toByteArray(), StandardCharsets.UTF_8)), installedVersion);
    }

    static AndroidRelease parse(JSONObject release, String installedVersion) throws IOException, JSONException {
        String tag = release.getString("tag_name");
        String version = tag.startsWith("v") ? tag.substring(1) : tag;
        if (release.getBoolean("draft") || release.getBoolean("prerelease")
                || !VERSION.matcher(version).matches() || installedVersion == null
                || !VERSION.matcher(installedVersion).matches()) {
            throw new IOException("Unexpected release version");
        }
        if (!isNewer(version, installedVersion)) return null;

        String name = "ShiningSource-" + version + "-android.apk";
        String url = DOWNLOAD + tag + "/" + name;
        JSONArray assets = release.getJSONArray("assets");
        AndroidRelease candidate = null;
        for (int i = 0; i < assets.length(); i++) {
            JSONObject asset = assets.getJSONObject(i);
            if (!name.equals(asset.optString("name"))) continue;
            String digest = asset.optString("digest");
            long size = asset.getLong("size");
            if (candidate != null || !url.equals(asset.optString("browser_download_url"))
                    || !"uploaded".equals(asset.optString("state"))
                    || !digest.matches("sha256:[0-9a-f]{64}") || size <= 0 || size > MAX_APK_SIZE) {
                throw new IOException("Unexpected Android release asset");
            }
            candidate = new AndroidRelease(version, url, digest.substring("sha256:".length()), size);
        }
        if (candidate == null) throw new IOException("The release has no Android APK");
        return candidate;
    }

    private static boolean isNewer(String candidate, String installed) {
        String[] left = candidate.split("\\.");
        String[] right = installed.split("\\.");
        for (int i = 0; i < left.length; i++) {
            int order = Integer.compare(Integer.parseInt(left[i]), Integer.parseInt(right[i]));
            if (order != 0) return order > 0;
        }
        return false;
    }

    void download(Context context, OutputStream output) throws IOException, NoSuchAlgorithmException {
        MessageDigest sha256 = MessageDigest.getInstance("SHA-256");
        long bytes = fetch(context, url, new DigestOutputStream(output, sha256), size);
        if (bytes != size || !digest.equals(hex(sha256.digest()))) {
            throw new IOException("APK download failed its checksum");
        }
    }

    private static String hex(byte[] bytes) {
        char[] alphabet = "0123456789abcdef".toCharArray();
        char[] result = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            result[2 * i] = alphabet[(bytes[i] & 0xff) >>> 4];
            result[2 * i + 1] = alphabet[bytes[i] & 0x0f];
        }
        return new String(result);
    }

    private static long fetch(Context context, String address, OutputStream output, long limit) throws IOException {
        URL url = new URL(address);
        long deadline = SystemClock.elapsedRealtime() + 120_000;
        for (int redirects = 0; redirects <= 4; redirects++) {
            String host = url.getHost();
            boolean github = "api.github.com".equals(host) || "github.com".equals(host);
            boolean cdn = "release-assets.githubusercontent.com".equals(host) || "objects.githubusercontent.com".equals(host);
            if (!"https".equals(url.getProtocol()) || url.getUserInfo() != null || url.getPort() != -1
                    || !(github || cdn)) {
                throw new IOException("Unexpected update server");
            }
            HttpsURLConnection connection = open(url);
            try {
                int status;
                try {
                    status = connection.getResponseCode();
                } catch (SSLHandshakeException untrusted) {
                    connection.disconnect();
                    connection = open(url);
                    useReleaseRoot(context, connection, github);
                    status = connection.getResponseCode();
                }
                if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                    String location = connection.getHeaderField("Location");
                    if (location == null) throw new IOException("Update redirect without a destination");
                    url = new URL(url, location);
                    continue;
                }
                if (status != 200) throw new IOException("Update server returned HTTP " + status);
                try (InputStream input = connection.getInputStream()) {
                    byte[] buffer = new byte[64 * 1024];
                    long total = 0;
                    for (int count; (count = input.read(buffer)) != -1; ) {
                        if (Thread.currentThread().isInterrupted()
                                || SystemClock.elapsedRealtime() > deadline) {
                            throw new IOException("Update request interrupted or timed out");
                        }
                        total += count;
                        if (total > limit) throw new IOException("Update response is too large");
                        output.write(buffer, 0, count);
                    }
                    return total;
                }
            } finally {
                connection.disconnect();
            }
        }
        throw new IOException("Too many update redirects");
    }

    private static HttpsURLConnection open(URL url) throws IOException {
        HttpsURLConnection connection = (HttpsURLConnection) url.openConnection();
        connection.setInstanceFollowRedirects(false);
        connection.setConnectTimeout(10_000);
        connection.setReadTimeout(15_000);
        connection.setRequestProperty("User-Agent", "ShiningSource-Android");
        if ("api.github.com".equals(url.getHost())) {
            connection.setRequestProperty("Accept", "application/vnd.github+json");
            connection.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
        }
        return connection;
    }

    // Older Android root stores lack the CAs serving GitHub's API and release assets.
    // Retry only the allowlisted connection, using its CA and the default hostname verifier.
    // https://www.sectigo.com/knowledge-base/detail/Sectigo-Public-Intermediates-and-Roots
    // https://letsencrypt.org/certificates/
    private static void useReleaseRoot(Context context, HttpsURLConnection connection, boolean github) throws IOException {
        String root = github ? "updater/sectigo-root-e46.pem" : "updater/isrg-root-x1.pem";
        try (InputStream input = context.getAssets().open(root)) {
            KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
            store.load(null, null);
            store.setCertificateEntry("release-root", CertificateFactory.getInstance("X.509").generateCertificate(input));
            TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            factory.init(store);
            SSLContext tls = SSLContext.getInstance("TLS");
            tls.init(null, factory.getTrustManagers(), null);
            connection.setSSLSocketFactory(tls.getSocketFactory());
        } catch (GeneralSecurityException error) {
            throw new IOException("Could not configure GitHub HTTPS trust", error);
        }
    }
}
