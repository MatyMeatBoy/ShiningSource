"""Verify the live SDL game survives MCC/MNC changes on an isolated API21 emulator.

Build the fixture with a JDK and Android SDK (directories must exist):
  javac -source 8 -target 8 -cp SDK/platforms/android-35/android.jar \
      -d CLASSES tools/tests/AndroidConfigFixture.java
  d8 --min-api 21 --lib SDK/platforms/android-35/android.jar \
      --output DEX CLASSES/AndroidConfigFixture.class
  jar cf android-config-fixture.jar -C DEX classes.dex
Use d8.bat on Windows, with JAVA_HOME pointing to the JDK.

Install the candidate APK and start a sufficiently long rendered game run first.
This test changes only MCC/MNC and restores their original values in finally.
It never installs APKs, launches/stops games, reboots, or accesses a ROM/save.
Example:
  python tools/tests/android_configuration_test.py --serial emulator-5560 \
      --fixture build/android/config-fixture/android-config-fixture.jar \
      --out build/android/configuration-receipt
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time


def run_test(args):
    args.out.mkdir(parents=True, exist_ok=True)
    receipt = {"serial": args.serial, "passed": False, "commands": []}

    def adb(*command):
        result = subprocess.run([args.adb, "-s", args.serial, *command],
                                capture_output=True, text=True, timeout=30)
        receipt["commands"].append({"args": list(command), "code": result.returncode,
                                    "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise RuntimeError("adb command failed: " + " ".join(command) + "\n" + result.stderr)
        return result.stdout.strip()

    def game_pid():
        rows = [row.split() for row in adb("shell", "ps").splitlines()
                if row.split() and row.split()[-1] == "com.shiningsource.android:game"]
        if len(rows) != 1 or len(rows[0]) < 2 or not rows[0][1].isdigit():
            raise RuntimeError("Expected exactly one live ShiningSource game process")
        return int(rows[0][1])

    remote = "/data/local/tmp/ss-config-fixture.jar"
    backup = "/data/local/tmp/ss-mccmnc-%d.backup" % time.time_ns()

    def fixture(*parts):
        # All remote tokens are fixed here or numeric; the local fixture path is
        # sent as a separate adb push argument, never interpolated into a shell.
        return adb("shell", "CLASSPATH=%s app_process /system/bin AndroidConfigFixture %s"
                   % (remote, " ".join(parts)))

    def configuration(text, prefix):
        # API21 adb shell may return zero even when app_process throws. Require
        # the fixture's positive acknowledgement, never merely its exit status.
        match = re.search(r"^" + re.escape(prefix) + r" mcc=(\d+) mnc=(\d+)\s*$", text, re.MULTILINE)
        if not match:
            raise RuntimeError("Missing fixture %s configuration acknowledgement: %s" % (prefix, text))
        return tuple(map(int, match.groups()))

    def require_configuration(text, prefix, expected):
        observed = configuration(text, prefix)
        if observed != expected:
            raise RuntimeError("Expected %s configuration %s, observed %s" % (prefix, expected, observed))

    attempted = False
    failure = None
    try:
        if not re.fullmatch(r"emulator-\d+", args.serial):
            raise RuntimeError("Only a disposable emulator serial is accepted")
        if adb("shell", "getprop ro.kernel.qemu") != "1":
            raise RuntimeError("Target must identify itself as an emulator")
        if adb("shell", "getprop ro.build.version.sdk") != "21":
            raise RuntimeError("Fixture is restricted to Android API21")
        if adb("shell", "getprop sys.boot_completed") != "1":
            raise RuntimeError("Wait until emulator boot completes")
        before = game_pid()
        receipt["pid_before"] = before
        adb("push", str(args.fixture.resolve()), remote)
        status = fixture("status")
        receipt["configuration_before"] = status
        mcc, mnc = configuration(status, "current")
        receipt["original"] = {"mcc": mcc, "mnc": mnc}
        # Ensure both fields change even if a previous test used 311/481.
        target_mcc, target_mnc = (311 if mcc != 311 else 312), (481 if mnc != 481 else 482)
        receipt["target"] = {"mcc": target_mcc, "mnc": target_mnc}
        attempted = True
        receipt["injection"] = fixture("inject", backup, str(target_mcc), str(target_mnc))
        require_configuration(receipt["injection"], "before", (mcc, mnc))
        require_configuration(receipt["injection"], "after", (target_mcc, target_mnc))
        receipt["configuration_after_injection"] = fixture("status")
        require_configuration(receipt["configuration_after_injection"], "current", (target_mcc, target_mnc))
        time.sleep(3)
        receipt["pid_after_injection"] = game_pid()
        if receipt["pid_after_injection"] != before:
            raise RuntimeError("Game process restarted during MCC/MNC injection")
    except Exception as error:
        failure = str(error)
    finally:
        if attempted:
            try:
                receipt["restoration"] = fixture("restore", backup)
                require_configuration(receipt["restoration"], "after", (mcc, mnc))
                receipt["configuration_after_restoration"] = fixture("status")
                require_configuration(receipt["configuration_after_restoration"], "current", (mcc, mnc))
                time.sleep(3)
                receipt["pid_after_restoration"] = game_pid()
                if receipt["pid_after_restoration"] != receipt["pid_before"]:
                    raise RuntimeError("Game process restarted during MCC/MNC restoration")
            except Exception as error:
                receipt["restore_error"] = str(error)
                receipt["backup_for_manual_restore"] = backup
                failure = (failure + "; " if failure else "") + "Restoration failed: " + str(error)
        receipt["passed"] = failure is None
        if failure:
            receipt["error"] = failure
        (args.out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if failure:
        raise RuntimeError(failure)
    print("MCC/MNC injection and restoration: game PID %d survived" % receipt["pid_before"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adb", default="adb", help="ADB executable")
    parser.add_argument("--serial", default="emulator-5560")
    parser.add_argument("--fixture", required=True, type=Path, help="Compiled AndroidConfigFixture DEX JAR")
    parser.add_argument("--out", required=True, type=Path, help="Receipt directory")
    args = parser.parse_args()
    if not args.fixture.is_file():
        parser.error("Fixture JAR does not exist")
    run_test(args)


if __name__ == "__main__":
    main()
