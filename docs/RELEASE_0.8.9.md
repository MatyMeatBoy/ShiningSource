# ShiningSource 0.8.9 — experimental companion combat sounds

Other players' attacks can now be heard through the existing action packets.
The original game explicitly excluded remote characters from several sound
paths. This update relaxes only those audio checks; it does not stream another
player's audio, change attack ownership or add network messages.

- **Experimental: remote sounds** is **on by default**, in the in-game options
  menu. It covers normal weapon attacks, charge starts, bow release, three
  projectile starts, the remote charged-attack timer and thrown items.
- Turn it off to restore the previous remote-sound exclusions. The choice is
  local, changes immediately and is saved as `remote_sfx` in `shiningsource.ini`.
  `SS_REMOTE_SFX=0` overrides it at startup. Local sound paths are retained.
- Existing action/animation transition checks prevent unchanged packets from
  replaying an attack. Remote bow release waits through a frozen animation
  clock. Original class-specific layered sound sequences are retained.
- Opening the menu on rebased Android/Linux hosts no longer reads the font at
  an unmapped cartridge address. It reads a bounded font resource from the
  user's ROM file, with the existing dot-font fallback if unavailable/invalid.
- Guest option callbacks remain leaf operations. Logging runs in the host
  after the callback returns, avoiding re-entry into its active service stack.

Zone music retains the existing `SS_MP_MUSIC` behavior introduced earlier:
enabled by default; `SS_MP_MUSIC=0` restores the cartridge's multiplayer rule.

Online rules remain **6**, compatible with 0.8.8. Each updated client controls
what it hears; older clients retain their old presentation. Package ID,
certificate, ROM requirement and save format are unchanged. No ROM, font asset
or save is distributed. Android supports ARMv7, ARM64 and x86_64 from API21.

The user confirmed an Android/Windows connection with0.8.8. Device, ABI, session
duration and four-player coverage were not supplied; this does not specifically
confirm the previously reported Galaxy A11 or RobNick's device.

Verification and its limits are in
[the verification record](https://github.com/MatyMeatBoy/ShiningSource/blob/v0.8.9/docs/ANDROID_0.8.9_VERIFICATION.md).
All weapon classes on physical phones and four-device audio listening remain
unverified. The original mixer's channel/priority rules still apply.

Lessons were returned through [ReKHCoM #648](https://github.com/MatyMeatBoy/ReKHCoM/pull/648)
and [GBANoCable #2](https://github.com/MatyMeatBoy/GBANoCable/pull/2).
The carried Vita0.8.5 package remains an untested older build with an incompatible
online protocol.
