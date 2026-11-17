# Remote player combat sounds implementation plan

**Goal:** Hear other players' attacks online, controlled by an experimental option
that defaults on, without changing gameplay, wire messages, local effects or saves.

**Architecture:** Reuse the original packet-driven presentation in sub_0800D368.
Its normal-attack, charge-start and bow-release paths explicitly exclude remote
state13, as do five one-shot projectile/charged-task/throw sound entries.
Relax only those eight audio gates while online and enabled. Keep the
original action/animation transition checks, song selection and intentional
multi-song class fallthrough. Remote bow release additionally waits for the
animation clock to run, avoiding repeated zero-clock release sounds while frozen.
Use the existing guest option callback for live menu changes; persist through
the existing settings file. No new network event stream or protocol revision.

**Tech stack:** C runtime, generated-source JSON patches, SDL3 options menu,
existing host/guest option bridge, Python/compiled-C regression fixtures.

- [x] Demonstrate remote suppression using extracted original sound gates;
  cover four actors, repeated packet, local preservation, class fallthrough,
  offline/disabled modes and frozen remote bow animation.
- [x] Add core option/policy and eight precise generated-code patches; do not
  edit the matching decompilation or remote gameplay ownership checks.
- [x] Expose bilingual experimental option, default on, live switching and
  persistence; bridge through existing option hook for native hosts.
- [x] Build Windows and Android; verify offline reference hash/audio, real
  multiplayer SFX traces and unchanged shared blocks, menu persistence and ABI
  linkage. Prepare the next testable patch version with honest limits.
- [x] Update documentation and the ReKHCoM/GBANoCable handoffs: distinguish the
  user's confirmed Android/Windows connection from local automation and from
  this newly reported audio defect. Publish reviewable documentary PRs.

The user supplied the desired default and experimental toggle explicitly.
The original cartridge's remote state13 sound exclusions are code evidence;
intent about four nearby physical speakers is not established. Reporter devices,
ABIs, session length and four-player operation have not been established.

Integration exposed two menu-boundary failures before delivery: diagnostic host
calls inside the reverse option callback, and the existing fixed-ROM font read
on rebased hosts. Leaf setter/cached acknowledgment and bounded host-side file
font loading fix these; compiled guards and nine synthetic-font tests retain
the reproductions. Thirty Python tests pass. Windows and Android ARMv7/ARM64
canonical replay retains reference hash/audio; ARMv7/ARM64 rendered menus and
setting persistence pass. Two27000-frame Windows sessions show cue0x14 calls
8→18 with26509 matching labels each. Linux is rebuilt, not newly runtime-tested.

Published v0.8.9 from316a6bf691e9e905d9405f103497cded240f1035; downloaded
and verified all four platform assets against local/published checksums.
ReKHCoM PR648 updated at9a4076efcdd1d888debb984265c3a5e56f7ed3f2 and
GBANoCable PR2 atb62ea9b4a61a5287f2fd628fa4879624ca621ac5. Both remain open
documentation PRs; no foreign runtime/main changes or merges were performed.
