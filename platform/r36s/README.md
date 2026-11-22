# ShiningSource for R36S

This Linux AArch64 build targets the RK3326 on 64-bit firmware with glibc 2.31 or later, KMSDRM and ALSA. It uses the same game image as the ARM64 Android build. The desktop launcher and Windows split-screen feature are platform-specific.

1. Extract `ShiningSource.sh` and the `shiningsource` folder together into the `ports` directory of the game SD card.
2. Copy your own unmodified European Shining Soul II cartridge dump (`AU2P`) to `shiningsource/roms/rom.gba`.
3. Open ShiningSource from **Ports** in the device frontend.

The launcher uses the firmware's SDL controller mapping through PortMaster when available. Use Start + Select inside the game for options and exit. Saves, settings and `last-run.log` live in `shiningsource/user/`.

This is a manual installation from the ShiningSource release. Physical R36S video, audio, input and performance still need device verification.
