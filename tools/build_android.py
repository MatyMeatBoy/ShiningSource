#!/usr/bin/env python3
"""Build the Android app (debug-signed APK) for arm64-v8a and x86_64.

  build_android.py [--game ss2] [--abi arm64-v8a x86_64] [--test]

Needs, under SHININGSOURCE_ANDROID (default D:/01Claude/ReKHCoM-build/toolchain/android): the Android SDK
(platform 35, build-tools, cmake 3.22.1), the NDK, a JDK 17, the SDL3 source tree and a Gradle cache.
Output: build/android/ShiningSource-debug.apk. No ROM is ever packaged; the build checks it.

Per ABI the APK carries libSDL3.so, libmain.so (the host, runtime/host64/host_linux.c) and, as an asset, the game
image: ss2_game.macho for arm64-v8a (tools/build_arm64.py), ss2_game.elf for x86_64 (tools/build_win64.py; the
image is the same file on every x86-64 system). --test makes the game activity startable from adb.
"""
import argparse, os, shutil, subprocess, sys, zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build

ROOT = build.ROOT
IMAGES = {"arm64-v8a": ("build/arm64/%s/%s_game.macho", "build_arm64.py"), "x86_64": ("build/win64/%s/%s_game.elf", "build_win64.py")}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("--abi", nargs="+", default=["arm64-v8a", "x86_64"], choices=sorted(IMAGES))
    parser.add_argument("--test", action="store_true", help="test build: the game activity can be started with adb")
    parser.add_argument("-j", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    args = parser.parse_args()

    tools = Path(os.environ.get("SHININGSOURCE_ANDROID", "D:/01Claude/ReKHCoM-build/toolchain/android"))
    sdk = tools / "sdk"
    ndk = next((tools / "native").glob("android-ndk-*"), None)
    java = next((tools / "java").glob("jdk-*"), None)
    sdl = next(tools.glob("SDL3-*/"), None)
    if not (sdk.is_dir() and ndk and java and sdl):
        sys.exit("Android tools not found under %s (sdk, native/android-ndk-*, java/jdk-*, SDL3-*)" % tools)
    cmake = sdk / "cmake/3.22.1/bin/cmake.exe"
    ninja = next((p for p in (tools.parent / "bin/ninja.exe", sdk / "cmake/3.22.1/bin/ninja.exe") if p.exists()), None)
    ndk_version = next(l.split("=", 1)[1].strip() for l in (ndk / "source.properties").read_text().splitlines() if l.startswith("Pkg.Revision"))
    env = dict(os.environ, JAVA_HOME=str(java), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk),
               GRADLE_USER_HOME=str(tools / "gradle-cache"), CMAKE_BUILD_PARALLEL_LEVEL=str(args.j))

    def run(cmd, cwd=ROOT):
        done = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, capture_output=True, text=True)
        if done.returncode:
            print((done.stdout + done.stderr)[-4000:])
            sys.exit("failed: " + " ".join(str(c) for c in cmd[:3]))

    stage = ROOT / "build/android/app-project"
    main_dir = stage / "app/src/main"
    if main_dir.exists():
        shutil.rmtree(main_dir)
    shutil.copytree(ROOT / "android/app", stage / "app", dirs_exist_ok=True)
    skeleton = sdl / "android-project"
    shutil.copytree(skeleton / "app/src/main/java/org", main_dir / "java/org", dirs_exist_ok=True)
    # the icon is the port's own (android/app/src/main/res/mipmap-*, drawn by tools/make_icon.py)
    shutil.copytree(skeleton / "gradle", stage / "gradle", dirs_exist_ok=True)
    shutil.copy2(skeleton / "gradlew.bat", stage / "gradlew.bat")
    if args.test:
        manifest = main_dir / "AndroidManifest.xml"
        text = manifest.read_text(encoding="utf-8")
        assert 'android:process=":game"\n            android:exported="false"' in text
        manifest.write_text(text.replace('android:process=":game"\n            android:exported="false"',
                                         'android:process=":game"\n            android:exported="true"'), encoding="utf-8")

    for abi in args.abi:
        image = ROOT / (IMAGES[abi][0] % (args.game, args.game))
        # The image is always built again here. Taking whatever file was there shipped several packages whose
        # ARM64 half was hours older than the rest: both halves must be the same game.
        done = subprocess.run([sys.executable, str(ROOT / "tools" / IMAGES[abi][1]), "--game", args.game])
        if done.returncode or not image.exists():
            sys.exit("%s could not be built (tools/%s)" % (image.relative_to(ROOT), IMAGES[abi][1]))
        native = ROOT / "build/android" / ("native-" + abi)
        run([cmake, "-S", ROOT / "android", "-B", native, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=%s" % ninja,
             "-DCMAKE_TOOLCHAIN_FILE=%s/build/cmake/android.toolchain.cmake" % ndk, "-DANDROID_ABI=" + abi,
             "-DANDROID_PLATFORM=android-21", "-DSDL_SOURCE_DIR=%s" % sdl, "-DCMAKE_BUILD_TYPE=Release"])
        run([ninja, "-C", native, "-j", args.j])
        libs = main_dir / "jniLibs" / abi
        libs.mkdir(parents=True, exist_ok=True)
        strip = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
        for name, source in (("libSDL3.so", native / "SDL/libSDL3.so"), ("libmain.so", native / "libmain.so")):
            shutil.copy2(source, libs / name)
            run([strip, "--strip-unneeded", libs / name])
        assets = main_dir / "assets"
        assets.mkdir(parents=True, exist_ok=True)
        shutil.copy2(image, assets / image.name)
    notices = main_dir / "assets/licenses"
    notices.mkdir(parents=True, exist_ok=True)
    shutil.copy2(sdl / "LICENSE.txt", notices / "SDL3.txt")

    (stage / "gradle/wrapper/gradle-wrapper.properties").write_text(
        "distributionBase=GRADLE_USER_HOME\ndistributionPath=wrapper/dists\n"
        "distributionUrl=https\\://services.gradle.org/distributions/gradle-8.12-bin.zip\n"
        "zipStoreBase=GRADLE_USER_HOME\nzipStorePath=wrapper/dists\n")
    (stage / "settings.gradle").write_text(
        "pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }\n"
        "dependencyResolutionManagement { repositories { google(); mavenCentral() } }\n"
        "rootProject.name='ShiningSource'\ninclude ':app'\n")
    (stage / "build.gradle").write_text("plugins { id 'com.android.application' version '8.7.3' apply false }\n")
    (stage / "gradle.properties").write_text("org.gradle.jvmargs=-Xmx1536m -Dfile.encoding=UTF-8\norg.gradle.daemon=false\n")
    (stage / "local.properties").write_text("sdk.dir=%s\nndk.dir=%s\n" % (str(sdk).replace("\\", "/"), str(ndk).replace("\\", "/")))
    (stage / "app/build.gradle").write_text("""plugins { id 'com.android.application' }
android {
 namespace 'com.shiningsource.android'
 compileSdk 35
 ndkVersion '%s'
 defaultConfig { applicationId 'com.shiningsource.android'; minSdk 21; targetSdk 35; versionCode 7; versionName '0.6' }
 compileOptions { sourceCompatibility JavaVersion.VERSION_1_8; targetCompatibility JavaVersion.VERSION_1_8 }
 buildTypes { debug { debuggable true }; release { minifyEnabled false } }
 lint { abortOnError false }
 packaging { jniLibs { useLegacyPackaging true } }
 androidResources { noCompress 'macho', 'elf' }
}
""" % ndk_version)
    run([stage / "gradlew.bat", "--no-daemon", "--offline", "assembleDebug"], cwd=stage)
    apk = stage / "app/build/outputs/apk/debug/app-debug.apk"
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        assert not any(Path(n).suffix.lower() in (".gba", ".sav") for n in names), "a ROM or a save must never be packaged"
        for abi in args.abi:
            assert "lib/%s/libmain.so" % abi in names, "host library missing for " + abi
    target = ROOT / "build/android/ShiningSource-debug.apk"
    shutil.copy2(apk, target)
    print("%s (%d KiB, %s; debug-signed%s)" % (target.relative_to(ROOT), target.stat().st_size // 1024, ", ".join(args.abi),
                                               ", test build" if args.test else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
