# Joining a game in progress

Status: **prototype, off by default** (`SS_NET_JOIN=1`). A program started while
a session is already being played is let into it at the next set-up of a
dungeon map. Measured on one machine only (section 4). The single-player
reference and the online tests without the setting are unchanged.

Source revision of the game's C: the submodule `games/ss2/decomp` as of
ShiningSource v0.7.4. Function names are the decomp's (`sub_08xxxxxx`); line
numbers are those of `games/ss2/decomp/src`. "Seat" is the game's player
number, 0 to 3; seat 0 is the host (the game's parent). Statements marked
*traffic* were read off the message log of a scripted two-player session
(`SS_NET_MSG_LOG`, `SS_NET_MSG_DUMP=1`); everything else is from the code.

## 1. What the game does (and what a newcomer therefore lacks)

### 1.1 A session is made once

- **Lobby** (`code_08002118.c:3180-3370`). A client writes `'O','K'` into bytes
  8 and 9 of its block and waits until the host's block says `'G','O'`. The
  host says `GO` when every other seat says `OK` and A is pressed. Byte 10 of
  the host's block carries two settings of the session (bit 0 and bit 1; the
  client applies them: `sub_0800CE08`/`sub_0800CE1C`, and `mode`). That is the
  whole handshake; `sub_08001DF0` (the "NOS/OK" barrier) has no caller in C.
- **Seats are counted, not named.** `sub_08002164` counts the connected bits of
  the link status word and every loop over the players runs `i = 0 .. count-1`.
  A seat therefore has to be the next free number.
- **Per-seat things are made at the session's start and never again.**
  `sub_0800BD5C` (the world scene's lifetime task) calls `sub_08065CA8` (the
  message queue, the token rings, one receiver task per connected seat) and
  `sub_080135C4(i)` for every connected seat (the player record at
  `gUnk_03003CE0 + i*0x14C`, its link-block pointers, its task). It runs once
  per session: the player tasks survive every change of map (*traffic*: the
  messages a player task sends when it starts, 0x82/0x83/0x81, appear once).
- **The scene's start** (`sub_080226A4`, `code_08021DC4.c:640-720`): a barrier,
  the host's settings block (0xAB, `sub_08066624`), a second barrier, then by
  `scene+0x70`: a new multiplayer game selects map 12 (`sub_08022E60(12, 1)`),
  a resumed one map 0 area 4 (`sub_08023274(0, 4, 1)`); then the loop that
  calls the current area's function every frame.

### 1.2 The barrier

`sub_08067C18` queues message 0x84 with the token `0x37564`; the receiver puts
a token into the ring of its sender (`gUnk_03006260`, ten words per seat,
cursors at `gUnk_03006210`); `sub_08067C08` is true when every counted seat's
ring holds the token; `sub_08067BF8` then takes one out of every ring. A
barrier is always those three in a row. Tokens are not numbered: **a token too
many in a ring lets the next barrier pass early, a token too few holds it for
ever.** Bosses use the same tokens to step through their phases
(`code_08097E28.c`, `code_0809D878.c`, `code_080A3D2C.c`, `code_080A9680.c`).

### 1.3 A change of map

What starts one (all of them need *every counted seat*):

| Trigger | Code | Condition |
|---|---|---|
| exit of a map | `sub_080245CC`, host part at 2538-2640 | every seat's block is in the exit's zone (or marked dead) in three checks running; the host writes `scene+0x40` and sends 0x8D |
| portal back | `sub_08025890`, 3277-3292 | every seat's block has the "ready" bit (byte 7, bit 4); 0x8D |
| boss portal | `sub_080263B4` | every seat in the zone and ready; 0x9F |
| world map | `sub_0800BEC0` | every seat's cursor on the same place; a barrier; `sub_08022E60(place, 1)` |
| story | 0xA3 (`sub_08068070`) | a machine names a function and an argument, every machine runs it (*traffic*: this is how map 12 moves from area to area) |

Then every machine sets the new map up. For the maps the scripted session
reaches (map 12, areas 0 to 2) the area's function calls **`sub_0802704C`**
(`code_08021DC4.c:4292`) when `scene+0x10` is 0:

1. barrier **A** (4365-4376);
2. the host waits five frames and calls `sub_08022E60(map, 0)`: it chooses a
   layout for every record of the area, sends them (**0x85**, twenty bytes of
   `scene+0x22`), and stands in barrier **B**; the others call the same
   function, which for them is only barrier B;
3. `scene+0x44` (the way the exit gave) picks the layout's entry point, the
   local player is put there, the exits are started (not in maps 11 and 12);
4. barrier **C** (4536-4547);
5. every player says its statistics and flags again (*traffic*: 0x8E, 0x8C,
   0x83), and the host begins to make enemies (0x86).

*Traffic*, change from map 12 area 0 to area 1, two players (`pN` = sender,
labels are frames of the host's timeline):

```
26055 p1 8C, A3     26081 p2 8C, A3        the story's step, from each machine
26120 p1 84         26123 p2 84            barrier A
26127 p2 84                                p2 is in barrier B already
26130 p1 85, 84                            the host's layouts, and its token for B
26161 p1 93                                (encounter state)
26273 p2 84         26277 p1 84            barrier C
26279 p1 8E, 8C, 83 26282 p2 8E, 8C, 83    statistics, flags, weapon
26285 p1 A3 ...                            the next steps of the story
```

Not said again at a change of map: the name (0x81), 0x82, the second block of
statistics (0x8F), the session's settings (0xAB). Enemies and floor items are
not carried over: `sub_08057170` clears the spawn slots and the host makes
them again after the set-up.

**The town (map 0) is different.** `sub_08022E60` returns at once for map 0 on
every machine: no 0x85 and no barrier B. In the scripted session the two
entries into the town (map 0 area 0, then area 2, both by the story) do not
pass through `sub_0802704C` at all (traced: the function was entered at the
three set-ups of map 12 and never after).

### 1.4 What a program started later lacks

1. Its game expects a lobby in which a host says `GO`.
2. Its seat does not exist in the others' games (no record, no task, the
   count is one short).
3. Everything said once: the others' names, classes, weapons, statistics,
   flags; the session's settings; the map and area and layouts.
4. The state of the barrier: which tokens are in the rings.
5. Enemies, items and the flags of the current map, if it enters in the middle
   of one.

## 2. The mechanism

**One idea carries it: the newcomer becomes a copy of the host's position in
the order of messages at one well-chosen point, and from there on is given
what the host is given.** The point is the host's 0x85 inside `sub_0802704C`
(between barrier A and barrier B). At that point of a set-up there are no
enemies and no items yet, the layouts are about to be announced, and every
machine is on its way to barrier B. So item 5 above does not arise, and items
3 and 4 are finite and small.

- **Lobby.** The network layer plays it for the newcomer's game: the host's
  block says `GO` with the byte 10 the real host had, the other seats say
  `OK`, the game is told the session's seats plus its own. The host sends
  those facts first (entry `INFO`).
- **Waiting.** The newcomer's game runs the start of a session up to its
  first barrier and stands there (the game draws its own waiting mark). Its
  barrier tokens of that stretch are for nobody and are dropped; of its other
  messages only those that say who it is (0x81, 0x82, 0x83, 0x8C, 0x8E, 0x8F)
  are kept, in its own stream, until it is let in. Until then it is a
  *candidate*: the others' games are given nothing of it, and it is given
  nothing of the order.
- **Letting in.** When the host's game hands over the 0x85 of a set-up
  (`ss_net_join_anchor` marks that place; the same message from the world map
  is not one, a second set-up follows it there), the host puts into the order,
  before the 0x85:
  - `ADMIT`: the seat; map, area, `scene+0x44`, `scene+0x36`; and which seats'
    tokens the host's rings hold at that moment (players that are in barrier B
    already);
  - `SNAP` entries: the last 0xAB, 0xAC, 0xAD, 0xAE of the session and the
    last 0x81, 0x82, 0x83, 0x8C, 0x8E, 0x8F of every seat, each wrapped so
    that only the newcomer's game is given it;
  - `SNAP_END`.
- **The others.** A machine that reaches `ADMIT` is given one seat more: its
  game makes the record and the task (`sub_080135C4`, called from the
  receiver's loop through `ss_net_join_grow`), and the next frame the link
  driver counts the seat. This must not happen while that machine is still in
  barrier A (it would wait for a token of the new seat there, which belongs
  to B). A machine is past A exactly if its token for B is in the order
  before `ADMIT` (the mask in `ADMIT` says so) or if it has handed a token
  over that has not come back yet; otherwise it holds the order at `ADMIT`
  until it hands its next token over, which it does the moment it is through
  A. Nothing it needs lies behind `ADMIT` before that.
- **The newcomer.** After `SNAP_END` its first barriers pass alone
  (`ss_net_msg_gone` answers for everybody) and it is given nothing more. Its
  scene task then asks where the game is (`ss_net_join_place`), calls the
  game's own `sub_08023684(map, area, 1)`, which clears the layouts and ends
  in a barrier (that is B), and its first `sub_0802704C` leaves out A and the
  0x85 step (`ss_net_join_first_init`). Before anything else it is given
  again the tokens the host's rings held. Its rings are now the host's at
  that point, its game is at the host's place in the code, and it is given
  the host's messages from the 0x85 on. From here it is an ordinary player:
  it sends its token for B and for C and the others wait for them.
- **A seat whose player left.** The relay gives a newcomer the lowest free
  seat. If that is the seat of a player that left (its standing character),
  the record and the task exist: `ADMIT` only ends the standing character and
  the newcomer's blocks and messages take its place.

Why not the other shapes that were considered:

- *Free seats from the start, as standing or idle characters.* Every trigger
  in 1.3 needs every counted seat, so a seat nobody plays blocks every exit,
  portal and the world map unless each of those checks is patched; the game
  also scales and aims by the count. (This is already true today of a player
  that left: see 5.)
- *Letting in at once, in the middle of a map.* Needs the live enemies
  (identifier, health, position, state), the floor items and the map's flag
  words to be copied, and a point where no barrier is under way. Nothing in
  the messages carries an enemy's health absolutely. Not attempted.

## 3. Where it is

| What | Where |
|---|---|
| the setting, the room | `runtime/core/netlink.c`, `configure`: `SS_NET_JOIN=1`, direct transport only; the room's name is hashed with `"join"`, so programs with and without the setting never meet |
| everything else in the layer | `netlink.c`, "joining a game in progress": `join_tell`, `join_admit_now`, `join_entry`, `ss_net_join_*`, and the places marked with it in `receive`, `receive_messages`, `message_part`, `deliver_direct`, `drop`, `ss_net_msg_send`, `ss_net_msg_next` |
| the game: a seat is added | `patches.json`, `code_080659B0.c`: `ss_join_grow` in the online receiver loop |
| the game: the newcomer's map | `patches.json`, `code_08021DC4.c`: before the scene task's loop (`ss_net_join_place`, `sub_08023684`); in `sub_0802704C` the opening is left out once and the 0x85 step is marked (`ss_net_join_anchor`) |
| test | `tools/net_join_test.sh`, `tools/net_join_report.py` |
| test hooks | `runtime/core/frame.c`: `SS_STATE_LOG`, `SS_INPUT_GOTO`, `SS_INPUT_EXIT` (with a note from `sub_080245CC`, `patches.json`), `SS_INPUT_HUNT_REACH`; `netlink.c`: `SS_NET_MSG_DUMP` |

**Packets.** The layout did not change and `rules` stays 5. Added: entries of
the message channel with tags 0xF1, 0xF2, 0xF4, 0xF5 (the game's are 0x81 to
0xB0, the game's receiver does nothing for any other), and bit 6 of the
header's delay byte ("this candidate's game stands at its barrier"; the delay
itself is at most 30). Both only ever travel in a room made with the setting.
**Everybody in a session, the host first, must have the setting**: a program
without it is in another room.

The layer reads four things from the game's memory by address, as the test
hooks in `frame.c` do, and they are those of games/ss2: the scene record
(`gUnk_03005CF4`: +0, +0x36, +0x40, +0x44), the token rings (`gUnk_03006210`,
`gUnk_03006260`), and the game's queue of unsent messages (`gUnk_03006300`,
`gUnk_03006338`).

## 4. Measured

`tools/net_join_test.sh`, one machine, a relay on the same machine, eight
times real time unless said otherwise, 2026-11-13. With the code as delivered:

| Case | Let in at | Runs joined |
|---|---|---|
| 2 players, newcomer 30 s later | change to 12/1 | 1 of 1 |
| 2 players, 50 s | change to 12/2 | 1 of 1 |
| 3 players, 30 s and 50 s (fourth seat) | 12/1, 12/2 | 2 of 2 |
| 3 players, 10 s (before the session's first map) | set-up of 12/0 | 2 of 2 |
| 64-bit Windows build: 2 players 30 s, 3 players 10 s | 12/1, 12/0 | 2 of 2 |

With earlier states of the same mechanism (three small corrections came
after): 32 more runs joined, among them 5% loss with 10 to 30 ms of lag, and
three did not: a newcomer that took the session's players for candidates
(corrected); a run in which a player's program stalled for 0.2 s under load
during the introduction, at eight times the speed, and every game reset
(before anybody plays the game gives a silent seat 110 frames; the same case
alone joined 2 of 2); and the seat of a player that left (section 5). A
newcomer that started when only the town lay ahead was not let in (2 of 2).
The newcomer was let in 12 to 24 seconds of test time after its link opened
(its game needs about 2800 frames to its barrier, then waits for a set-up).

What the test asks of a run: every program ran to its
end; the newcomer was let in; its game is in the same map and area as the
host's at the same moments of the clock, through the changes of map that
follow; it dispatched exactly the host's messages from the point it was let
in; what it sent was dispatched by everybody once and in order; the others'
copy of its state and its copy of theirs move and agree at rest.

## 5. Limits, and what is not known

- **Only at the set-up of a map through `sub_0802704C`.** In the town nobody
  is let in: map 0 announces no layouts, and the town's maps in the scripted
  session are not entered through that function. A newcomer waits, with the
  game's waiting mark on a black screen, until the party enters a dungeon
  map. Whether every dungeon area uses `sub_0802704C` (the area functions in
  `code_08087CC0.c` and `code_0808D4A8.c` call it; `sub_080277E0` is another
  set-up, with its own barrier and `sub_08023684`) was not checked map by map.
- **Not run: a join at a map with exits, enemies that fight back, a boss.**
  The scripted session reaches map 12 (areas 0 to 2, moved by the story) and
  the town. The mechanism does not depend on the map, but that is reasoning.
- **The scene record is not copied.** The newcomer is told map, area and two
  fields. Flag words the others changed during the session by story steps
  (0xA3, 0xA4, 0x90, 0x8D) and that outlive a change of map are not sent. In
  the scripted session the newcomer's mode word and scene flags follow the
  others' through three changes of map; a session with more history may differ.
- **A second newcomer** is told about the session only after the first is in
  (one at a time); two at once were not run. A newcomer that gives up while it
  waits is forgotten; one that vanishes between `ADMIT` and its first token
  leaves the others waiting in barrier B until the silence timeout drops it.
- **A player that left blocks the others' changes of map**, and with them
  every later join. This is not new and not changed here, but it is now
  measured: with three players, one ended in the first field, the two that
  remained killed the enemy, sent the story's step (0x8C, 0xA3) and then stood
  in map 12 area 0 to the end of the run; no set-up followed. By the code the
  same holds for exits and portals (its standing block is not in the zone and
  has no "ready" bit). So the seat of a player that left is of no use to a
  newcomer today: the newcomer was given that seat by the relay, was told
  about the session and waited, and was never let in because no map was set
  up. The part of the mechanism that hands such a seat over (`ADMIT` on a seat
  that is marked as left) has therefore **never run**.
- **Without the setting** (as released) a program that enters the room after
  the lobby is taken for a player at once: in the run made for comparison the
  two players' session stopped making progress (12 messages dispatched in the
  whole run). The setting's room keeps programs with and without it apart;
  inside it a late program is a candidate and the others are not disturbed.
- **Memory.** `sub_080135C4` takes 0x680 + 0x100 + 0x140 bytes of the game's
  heap for the new seat, in the middle of a game, and does not check for
  failure (it does not at a session's start either).
- **The lobby's settings may not reach the newcomer's game.** The game's
  lobby reads the host's byte 10 only in a pass in which the host has not said
  `GO`; the lobby this layer plays says `GO` at once, so the newcomer keeps
  its defaults for both settings. One effect is wanted (bit 1: it is not shown
  the introduction alone), the other (bit 0, `sub_0800CE30`) would differ from
  the others in a session whose host has it set. Also, a host's real block
  still says `GO` until its game's first message, and a program that comes in
  that stretch may take it for its own before it is told that it joins. Both
  have a written-out correction (show the host waiting until the newcomer's
  block says `OK`, with bit 1 cleared; keep a `GO` that was there from the
  first block away from the game); it was not run and is not in the code.
- **Skipping the introduction.** A newcomer alone cannot gather the votes; the
  tests run with `SS_SKIP=0`.
- **0xA3 carries the address of a function of the build** (seen in the
  traffic as a native code address). Not part of this work, but two builds
  whose functions lie at different addresses would not agree on it.
- Real network, Android, Linux, a window: not run. The 64-bit Windows build:
  see the report.
