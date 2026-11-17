# ShiningSource 0.8.10 — crossplay scene callback hotfix

All players must update together: online rules are now 7. Older clients use
different room identities and cannot join updated clients.

Two multiplayer scene messages previously transmitted native function pointers.
Those addresses differ between Windows, Android, Linux and relocated images.
Android could consequently attempt to execute a Windows address and crash.
Both sender and receiver now convert callbacks through stable original game
function IDs. Unknown callbacks and malformed scene messages stop the session
with a diagnostic instead of executing an arbitrary address. Arguments and
unrelated messages retain their existing representation.

Verified: 42 automated regressions; Windows and ARM64 Android 15 emulator
crossplay through 42,000 frames, with forced image relocation and memfd code
loading on a 16 KiB page device. Both callback message types executed on both
peers, and captured wire messages contained the same portable IDs. Natural
scene callbacks also crossed in both directions. All three Android ABIs built;
the Linux package is cross-compiled, not executed on Linux for this hotfix.

The supplied Xiaomi Android 13 log identified an invalid foreign native PC.
A second 846-byte log only records startup and backgrounding, without a crash
marker; its later failure cannot yet be diagnosed from that file. Physical
device confirmation and long four-player sessions remain pending. This update
does not guarantee the absence of unrelated crashes.

Remote combat sounds remain experimental and enabled by default. Existing
Android compatibility improvements and RobNick's reporting credit are retained.
Automatic VPS report upload is deferred until the receiver is configured and
tested. No ROM, save or private player log is included.
