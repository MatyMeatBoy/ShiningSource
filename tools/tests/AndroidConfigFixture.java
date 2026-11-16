import android.content.res.Configuration;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;

/** API21 app_process fixture, run as adb shell (CHANGE_CONFIGURATION). */
public final class AndroidConfigFixture {
    private static final int MAGIC = 0x53534346;
    private static Object manager;
    private static Method get, update;
    private static Configuration current() throws Exception {
        return new Configuration((Configuration)get.invoke(manager));
    }
    private static File backup(String path) throws Exception {
        File file = new File(path).getCanonicalFile();
        if (!file.getPath().startsWith("/data/local/tmp/ss-mccmnc-") || !file.getParent().equals("/data/local/tmp"))
            throw new IllegalArgumentException("Use /data/local/tmp/ss-mccmnc-NAME.backup");
        return file;
    }
    private static void show(String prefix, Configuration value) {
        System.out.println(prefix + " mcc=" + value.mcc + " mnc=" + value.mnc);
    }
    public static void main(String[] args) throws Exception {
        manager = Class.forName("android.app.ActivityManagerNative").getDeclaredMethod("getDefault").invoke(null);
        Class<?> api = Class.forName("android.app.IActivityManager");
        get = api.getMethod("getConfiguration");
        update = api.getMethod("updateConfiguration", Configuration.class);
        Configuration value = current();
        if (args.length == 1 && "status".equals(args[0])) { show("current", value); return; }
        if (args.length < 2) throw new IllegalArgumentException("status | inject BACKUP MCC MNC | restore BACKUP");
        File file = backup(args[1]);
        show("before", value);
        if ("inject".equals(args[0]) && args.length == 4) {
            int mcc = Integer.parseInt(args[2]), mnc = Integer.parseInt(args[3]);
            if (mcc < 1 || mcc > 999 || mnc < 1 || mnc > 999) throw new IllegalArgumentException("MCC/MNC 1..999");
            if (file.exists()) throw new IllegalStateException("Backup already exists; restore it first");
            try (FileOutputStream bytes = new FileOutputStream(file); DataOutputStream out = new DataOutputStream(bytes)) {
                out.writeInt(MAGIC); out.writeInt(value.mcc); out.writeInt(value.mnc); out.flush(); bytes.getFD().sync();
            }
            value.mcc = mcc; value.mnc = mnc;
        } else if ("restore".equals(args[0]) && args.length == 2) {
            try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
                if (in.readInt() != MAGIC) throw new IllegalArgumentException("Not this fixture's backup");
                value.mcc = in.readInt(); value.mnc = in.readInt();
            }
        } else throw new IllegalArgumentException("status | inject BACKUP MCC MNC | restore BACKUP");
        update.invoke(manager, value); // A copy of current config; only MCC/MNC change.
        Configuration after = current();
        show("after", after);
        if (after.mcc != value.mcc || after.mnc != value.mnc)
            throw new IllegalStateException("Update not observed; backup retained");
        if ("restore".equals(args[0]) && !file.delete()) throw new IllegalStateException("Restored; backup delete failed");
    }
}
