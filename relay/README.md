# ss_relay

The server side of ShiningSource online: one small process that forwards the
players' packets inside a room. It knows nothing about the game and keeps no
data; a room exists while somebody is in it.

- One UDP port (default **47900**). ShiningSource has its own process and port;
  it never shares another game's relay.
- Up to 512 rooms of 4 seats. The first player of a room is the host.
- A seat is freed when its player leaves, or after 30 seconds of silence.
- A player that comes back with the same token gets its seat back if it is free.

## Build

```
cc -O2 -o ss_relay ss_relay.c            # Linux, macOS, BSD
cc -O2 -o ss_relay.exe ss_relay.c -lws2_32   # Windows (mingw)
```

## Run on a server (systemd)

```
sudo install -m 755 ss_relay /usr/local/bin/ss_relay
sudo install -m 644 ss_relay.service /etc/systemd/system/ss_relay.service
sudo systemctl daemon-reload
sudo systemctl enable --now ss_relay
sudo ufw allow 47900/udp        # or the firewall the server uses
journalctl -u ss_relay -f       # one line per player that joins or leaves
```

The unit runs it as an unprivileged dynamic user and restarts it if it stops.
Restarting the relay does not end a session: the players keep asking for their
seat and get it back within a quarter of a second.

## Run as a container

```
docker build -t ss_relay relay
docker run -d --restart=always -p 47900:47900/udp ss_relay
```

The image is a static binary on an empty base, running as an unprivileged
user. On a panel that builds from a pasted Dockerfile, publish UDP port 47900
to the same port of the container.

## Testing a bad network on one machine

```
SS_RELAY_LOSS=10 SS_RELAY_LAG=120 SS_RELAY_JITTER=40 ./ss_relay 47955
```

drops 10% of the forwarded packets and holds each one 120 to 160 ms, which also
reorders them. `tools/net_test.sh` in the repository starts the relay this way
and runs 2 to 4 players against it.

## Protocol

Every packet starts with the same 60-byte header (little-endian):

| Offset | Field |
| --- | --- |
| 0 | `SSN1` |
| 4 | type: 1 hello, 2 welcome, 3 data, 4 end, 5 ping |
| 5 | seat (set by the relay on forwarded packets) |
| 6 | delay in frames (host's choice) |
| 7 | number of 24-byte blocks that follow (welcome: seats in use, as bits) |
| 8 | room: FNV-1a of the room name |
| 12 | token of the player |
| 16 | stream number; 20: first label of the stream; 24: label of the first block |
| 28 | next label expected from each seat (4 x u32); 44: the stream that refers to |

The relay reads the type, seat, room and token; everything else is between the
players (`runtime/core/netlink.c`). A ping is echoed to its sender and needs no
room.
