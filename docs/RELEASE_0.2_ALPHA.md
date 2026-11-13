# ShiningSource 0.2 alpha

Native port of Shining Soul II (Europe) for Windows x64 and Android. No ROM,
BIOS or game data is included: you supply your own cartridge dump (SHA-1
`aa1288be9257337abdfe3034898cf6c7aa778fbc`). This is an alpha: it is for
testing, and the list of what has not been checked is as important as the rest.

Plays online with 0.1.1 and 0.1.2. It does not meet 0.1.

## What is in it

- **The game**, compiled from its matching decompilation, with picture, sound,
  saving, keyboard, controller and touch controls.
- **Online for 2 to 4 players** through the port's relay: same room name for
  everyone, MULTIPLAYER MODE in the game. The delay is automatic: it starts at
  2 frames and rises only when the connection needs it.
- **Two screen modes**, asked on first start: Classic (the console's picture at
  a whole scale inside an adjustable border, the port's own Soul frame) and
  Enhanced (the picture adapts to the window; the Soul field fills what the
  game does not).
- **Launcher** in the port's look, with the game's title screen made on your
  machine from your ROM. Pages: Play, Video (mode, border, VSync / VRR, skip
  scene), Sound, Controls (every button can be reassigned), Online,
  Achievements, About. F11 is full screen.
- **Skip scene**: Start during a story scene asks whether to skip it; online it
  is a vote and everyone skips together.
- **Achievements page**: sign in to RetroAchievements and see the game's list.
- **Reports**: a crash or a hang on Windows is written to
  `shiningsource_crash.log` next to the program.

## Install

- Windows: unpack `ShiningSource-0.2-alpha-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.2-alpha-android.apk` (arm64-v8a and x86_64,
  Android 5 or later). It is signed with a debug key, so Android asks to allow
  the installation.

## Checked

- The same scripted 32,000-frame play-through gives the same result on Windows
  (32 and 64-bit), Linux x64, Linux ARM64 and Android (x86_64 emulator).
- Four players online for 80,000 frames with packet loss and lag simulated;
  sessions between Android, Linux ARM64 and Windows through the public relay.
- Saving: the game's own save routine answers "saved and verified" on Windows,
  and the save file is written on Android.
- A first real two-player session by the owner (0.1), whose problems this
  version answers: save error, delay, wide-view menus, controller bindings.

## Not checked

- Saving by hand from the game's menu and after leaving an online session.
- A real phone: the Android build ran in the x86_64 emulator, so the arm64
  part of the package has not run on Android.
- Signing in to RetroAchievements (it needs an account).
- Skip scene with several players; VSync and VRR on real displays; the
  controller page with a controller.
- Players in different places, and multiplayer inside the dungeons.

## Known limits

- Sound is a first version of the music engine: music and effects play;
  modulation, reverb and exact envelope timing are missing.
- Per-scanline video effects are not drawn.
- Online, other players and enemies are seen one delay late (2 frames on a good
  connection; the cable's own is 1). There is no prediction with rollback.
- The game does not evaluate achievements while you play, and Rebel mode (a
  separate record for sessions with cheats) is not there yet.
- Crash and hang reports exist on Windows only. Android pause and resume have
  not been exercised.
- No web or console versions yet.
