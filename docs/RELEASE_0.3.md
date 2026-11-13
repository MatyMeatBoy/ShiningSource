# ShiningSource 0.3

Native port of Shining Soul II (Europe) for Windows x64 and Android, and a first
build for the browser. No ROM, BIOS or game data is included: you supply your
own cartridge dump (SHA-1 `aa1288be9257337abdfe3034898cf6c7aa778fbc`).

**Online, 0.3 only meets 0.3.** The link changed underneath (see below), so
every player in a room needs this version.

## Online: rebuilt around what the game really does

The first real sessions with three and four players showed items and gold that
did not appear or took seconds to pick up, enemies arriving one at a time,
companions' sprites showing late, players seeing each other in different
places, a delay that crept back up, and everybody thrown out when one game
closed. Reading the game's link code explained all of it: the game is not a
simulation that every machine steps in unison. Each machine decides for its own
character and tells the others, and everything that happens once (an enemy
appears, an item is dropped or taken, a hit, a change of map) is a message
passed in small steps, each step waiting for every player's answer. On the
cable a step took a frame or two; through a network with a buffered delay each
step cost many frames, and a single item needed five of them.

- **Direct transport.** No shared timeline and no fixed delay any more. Each
  player's data is handed to the game the moment it arrives; nobody waits for a
  slow player and there is no delay to creep up. The latency is the network's
  own.
- **Messages in one trip.** A whole game message now travels at once on a
  reliable channel of the port instead of step by step. Measured on one
  machine with 60 ms of simulated latency: a message reaches the other players
  in about 100 ms instead of about 650, and the first enemy of a map is there
  after about 90 frames instead of about 630.
- **Enemies stay together.** The host corrects each enemy's position on the
  other machines every second instead of every five.
- **A player who leaves does not end the session.** A player who closes the
  game, crashes or loses the connection stays as a standing character and the
  others play on. The exception is player 1: the game gives player 1 the
  enemies and the items, so when player 1 leaves the session ends for
  everybody (today the game then closes; start it again).
- **Skip scene** online can no longer leave a session dark and silent: it
  skips only the scene that was voted on, and for a minute at most.
- Online play is on by default in the launcher and on Android. The fixed-delay
  transport is still there when a delay is set by hand.

## Picture

- **The scrolling descriptions of the character selection** are whole (the
  game moves that text with an interrupt at a screen line, which the port did
  not raise).
- **Wide view**
  - The status display (HP, SP, Soul, the L and R menus and their icons) sits
    at the edges of the screen.
  - Pages and menus over the field can no longer break: the console's own
    columns are always drawn from what the game put there, and a full-screen
    page keeps everything in the middle.
  - Characters, enemies and objects stay visible beside the console's columns
    (up to 128 pixels on each side) instead of vanishing at the old edge.
  - Beside a map narrower than the view the picture is black, still, with a
    short fade on the map's last columns.
- **Real transparency.** The game shows shadows, ghosts, the arrow that points
  to a companion and a character that was just hurt on every other frame,
  which a modern display shows as hard blinking. They are drawn at half
  strength in every frame instead.

## Achievements

- **While playing**: the RetroAchievements set of your ROM is followed in the
  game, with a notice and the achievement's badge when one unlocks.
- **Without an account** the set (once it has been downloaded by a signed-in
  session) and the port's own twelve achievements work with no network and are
  kept in a local record.
- **Sending unlocks to your account is off by default** and is a switch on the
  Achievements page. RetroAchievements only counts programs it has approved,
  this port is not one of them, the set has not been checked against the port,
  and an unlock cannot be taken back: it is your decision.
- **Rebel mode**: cheats (full HP, full SP, no defeat, gold). A run started
  with cheats never talks to RetroAchievements, keeps its own record and its
  own save file (a copy of your normal one, never copied back), and cheats stop
  while the game's link is running.

## Also

- **Launcher**: takes a ROM that sits next to it, without asking.
- **Android**: the system file picker, the ROM copied once and checked before
  it replaces a stored one, automatic pickup from the app's folders, Play
  always in reach with the reason when it is disabled, Back twice to leave the
  game, and a note when the game failed to start.
- **Icon**: the port's own Soul jewel, on the programs, the Android package
  and the web page.
- **Browser build** (`tools/build_web.py`): the game runs as WebAssembly from
  your own ROM file. Offline only; saves are kept in the browser.
  It is built from source; no hosted page ships with this release.

## Install

- Windows: unpack `ShiningSource-0.3-windows-x64.zip` and start
  `ShiningSource.exe`.
- Android: install `ShiningSource-0.3-android.apk` (arm64-v8a and x86_64,
  Android 5 or later; signed with a debug key, so Android asks to allow it).

## Checked

- The scripted 32,000-frame play-through gives the same result on the 32-bit
  development build and Windows x64 (`5F1DB1A8`; the value changed with the
  sprite limits of the wide view).
- Online on one machine, two and three players, with 60 ms of latency, jitter
  and 3 to 5% loss simulated: lobby, map entry, enemies and messages, and a
  player killed mid-session with the others playing on.
- Android: start screen, ROM handling and play on the x86_64 emulator
  (Android 14).
- Browser: from the language screen into the introduction.

## Not checked

- Real sessions over the internet with this version, and four players.
- Items, gold and a map change after a player has left the session.
- The crash reported when a player opened a shop in multiplayer: it needs that
  player's `shiningsource_crash.log`.
- Transparency of alternating sprites and the companion arrow in a real
  session (no scripted scene shows them).
- Signing in to RetroAchievements, and sending an unlock (no account was
  available); the Rebel cheats after real damage.
- A real phone; Linux builds with this version's changes.
- Music in multiplayer: the only cause found and fixed is the skip that left a
  session muted; if it is still silent, say where.
