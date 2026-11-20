# Restored multiplayer audio audit

The existing `remote_sfx` option now appears as **Restored Multiplayer Audio** / **Audio Multijugador Restaurado**. Its saved key, live guest callback, default-on behavior and `SS_REMOTE_SFX` environment override are unchanged. The option controls all additions described here, including the original 0.8.9 attack restoration.

## Confirmed omissions and fixes

| Event | Evidence from original single-player path | Replicated path used for restoration |
|---|---|---|
| Enemy struck by a companion | Player/enemy damage and weapon/projectile collision paths request generic impact `0x1A` | Accepted peer `0x8A` message, matching active enemy ID, nonzero low-15-bit damage; self echoes and stale IDs are silent |
| Explicit critical marker | `sub_0801F374` requests `0x132` and queues `0xA1` through `sub_08068030` | Matching active-enemy `0xA1` reception, excluding sender echo |
| Breakable object struck by a companion | `sub_0801FDD0` chooses `0x29`, `0x2A` or `0x2B` using the object's material byte | Accepted peer `0x94` damage list, same material selection, excluding zero damage, dead states, absent metadata and unknown materials |
| Ability launches and animation cues | `code_08068670.c` explicitly restricts 21 sound sites to the local owner (or an enemy owner) | Widen only the audio predicates to the existing option; retain animation-phase tests, coroutine waits and scene checks |
| Transformation and self-buff cues | Local-owner blocks contain cues `0x2D` and `0x165` alongside gameplay writes | Separate audio permission from effect allocation and resource costs; keep gameplay writes local |
| Companion hurt voice | Local damage paths select class + `0x45`, with class 8 mapped to `0x4B` | Remote state-13 actor's existing action-10 transition in `sub_0800D368` |
| Companion falling voice | Local falling transition selects class + `0x4D`, with the regular class-8 mapping `0x53` | Remote state-13 actor's existing action-11 transition; unchanged repeated records are silent |
| Field item collected by a companion | The addressed player plays `0x26` once their inventory accepts the item in the `0x9A` handler | The following `0x9B` carries the success bit above the original 16-bit item ID. Other players hear the cue when their existing field record is removed. Failed pickups and redrops stay silent. |
| Two boss contact impacts | `sub_0809C8F4` (map 3, area 14) and `sub_080AC5B4` (map 5, area 12) request `0xE2` and `0xBB` only after a hit on the local player | The accepted local hit sends its cue in one ordered `0xB1` message. Companions use the original cue if their map and area match and the restored-audio option is on. |

The 21 ability sites span `sub_08069C44`, `sub_0806A19C`, `sub_0806A88C`, `sub_0806ABC0`, `sub_0806B3C4`, `sub_0806BE24`, `sub_0806C568`, `sub_0806D4A4`, `sub_0806D7EC`, `sub_0806DB1C`, `sub_0806DE94`, `sub_0806E7D4`, `sub_0806F0CC`, `sub_0806FF1C`, `sub_080710DC`, `sub_08072390`, `sub_08072D5C` and `sub_0807356C`. Multiple sites in a task retain their original launch/relaunch or per-target animation timing; they are not collapsed into a new per-frame sound.

## Paths deliberately retained

The original restoration already covers normal weapon actions, charge starts, bow release, remote projectiles, a charged timer and thrown items. Status application in `sub_0801DB94` already emits its original elemental/status cues when invoked by the received hit path; adding them again would duplicate them. Existing shared resurrection/recovery messages already request `0x91`. Local UI sounds and inventory decisions remain local. Zone music keeps its separate existing policy.

The synchronized destructible task `sub_08027C1C` already plays the final break cue `0x22`, `0x23` or `0x2B` before the host-only drop work. Shared exits and the boss-retry transition already request `0x24` on each peer. Boss attack phase cues remain at their original animation boundaries.

The extra `state != 13` sound checks in local-indexed attack routines (`sub_08014B28`, `sub_08014C04`, `sub_08014E68`, `sub_080153A4` and `sub_08015464`) are not independent remote presentations: those routines select the local actor, while remote actions use the already audited presentation/projectile tasks. Their gameplay entry gates were not opened.

No generic miss sound is reconstructed from a damage packet: a miss has no positive accepted damage event. The damage halfword's `0x8000` flag is not treated as a critical flag. Random critical variation, local dodge outcomes and inventory failures are not guessed on another client. Death music is not copied to living companions.

## Validation and limits

Source-derived compiled fixtures exercise the real patched guards and receiver branches, including off/offline behavior, sender echoes, stale IDs, zero damage, material selection, animation-phase checks and repeated hurt/death packets. Live on/off Windows sessions and the canonical offline reference replay are recorded in the 0.8.14 release notes.

Two Windows clients replayed the game's field-item spawn, pickup and removal with each seat as the picker and the observer's option on and off. The picker played one `0x26`. The observer played one when enabled, and a stale request never replayed it. Both clients played one `0x24` when they returned from Boken's field to the castle.

An ordinary sword swing in Goblin Fort 2 produced one `0x29` impact and one `0x22` break cue on both clients. A position-assisted replay in the two boss rooms invoked the original stationary effect and the original charge-contact helper. The resulting `0xE2` went from host to guest and `0xBB` from guest to host. Each source and enabled observer played one cue. With the observer's option off, only the source played each cue.

This audit covers the confirmed source exclusions and replicated events above. It does not claim a complete playthrough with every weapon, every enemy/boss or every physical device, nor physical listening tests of channel priority and simultaneous effects. Existing mixer priorities can still mask a correctly requested cue. A new omission needs a concrete actor/action/event reproduction; gameplay state is not broadened simply to make it sound.
