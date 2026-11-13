# ShiningSource 0.1

First test release of the native port of Shining Soul II (Europe), for Windows
x64 and Android. No ROM, BIOS or game data is included: you supply your own
cartridge dump (SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`).

## Windows

Unpack `ShiningSource-0.1-windows-x64.zip` anywhere and start
`ShiningSource.exe`. Choose your ROM, choose how you want to see the game
(Classic or Enhanced) and press Play.

Keys: arrows, `X` = A, `Z` = B, `C` = L, `V` = R, `Enter` = Start, right
`Shift` = Select, `F11` full screen, `Esc` quits. Keys and controller buttons
can be changed in the Controls page.

## Android

Install `ShiningSource-0.1-android.apk` (arm64-v8a and x86_64; Android 5 or
later, 64-bit only). It is signed with a debug key, so Android asks to allow
the installation. Choose the ROM, the screen mode and press Play. Touch
controls are drawn over the picture; a controller hides them.

## Online, 2 to 4 players

In the launcher open Online (Android: the first screen), tick Play online and
give everyone the same room name. The first player to enter is the host. In the
game choose MULTIPLAYER MODE.

- Everyone needs this same version and the same ROM. Players with another
  version or ROM do not meet: they land in a different room.
- The host chooses the delay: same country 3 or 4, same continent 6, between
  continents 10 to 14. Too low and the game runs slower; it does not
  disconnect. The window title says who is being waited for.
- A player who leaves frees the others at once; one whose connection dies is
  waited for 15 seconds.

## What has been checked, and what has not

Checked: the same scripted 32,000-frame play-through gives the same result on
Windows, Linux and Android; sessions of four players for 80,000 frames with
packet loss and lag simulated; sessions between Android, Linux ARM64 and
Windows through the public relay.

Not checked yet: a real phone (the Android build ran in the x86_64 emulator
only, so the arm64 part of the APK has not run on Android), players in
different places, multiplayer inside the dungeons.

## Known limits

- Sound is a first version of the music engine: music and effects play;
  modulation, reverb and exact envelope timing are missing.
- Per-scanline video effects are not drawn.
- No achievements yet (the RetroAchievements list and Rebel mode are planned).
- The delay is fixed for a session; it does not adapt by itself.
- Crash and hang reports (`shiningsource_crash.log`) exist on Windows only.
- Android: pause and resume have not been exercised.
