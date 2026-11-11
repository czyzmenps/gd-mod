# Live Collab Editor

A [Geode](https://geode-sdk.org) mod for Geometry Dash (Windows, GD 2.2081, Geode 5.x) that lets
several players work in the **same level editor at the same time**. Everyone sees objects being
placed, deleted, moved and edited by the others in near real time.

The mod talks to a small relay server (`server/`) that you run yourself.

## How it works

* Every object gets a unique id. Changes are detected locally (new objects, deleted objects,
  moved / rotated / edited objects) and sent to the server, which relays them to everybody else
  in the room.
* The server keeps the current level of every room, so people who join later get the up-to-date
  level, and rooms survive a server restart.
* Conflicts: last change wins.

## Using it

1. Run the server (see below) once, somewhere every player can reach, and enter its address in
   the mod's **Config** (Geode -> Live Collab Editor -> settings, or the **Config** button in the
   window). You only do this once. If you build the mod yourself you can bake the address in
   (`kDefaultServer` in `src/Config.hpp`) so nobody has to type anything.
2. In the editor press the small round **Collab** button (top right by default, move or hide it
   in the Config) or the shortcut **F8**. The light on the button shows the state (grey = offline,
   yellow = connecting, blue = syncing, green = live). The button sits in a corner, so the editor
   stays fully usable.
3. The window has two views:
   * **Start party** - creates a new party from the level that is open in your editor and copies
     the party code to your clipboard. Send that code to your friends.
   * **Join a party** - paste the code a friend sent you and press **Join**. Joining **replaces
     everything in your editor** with the party's level, so open an empty level (or a copy) first.
   * No server address or port to type: the server comes from the Config. (If a host uses a
     different server than yours, their party code looks like `ABCD2345@host:5588` and joining
     with it switches to that server automatically.)
4. Once you are in a party the window shows the code with a **Copy code** button, a progress bar
   while the level is uploaded / downloaded, the list of everybody with their own icon and what
   they have selected, an **Invite** box (type a friend's name) and **Leave room**.
5. While you are in a party a small pill at the top of the editor shows the party and how many
   people are online, other players' selections are outlined in their colour with a name tag, and
   a short notification pops up when somebody joins or leaves.

### Config

| Setting | What it does |
| --- | --- |
| Party server | Address of the party server (`host` or `host:port`). Empty = the address built into the mod. |
| Your name | Name the others see. Empty = your Geometry Dash account name. |
| Button position / size | Corner of the editor (top/bottom, left/right) or hidden, and how big it is. |
| Open Collab window | Shortcut (default F8). |
| Copy the party code when starting | On by default. |
| Show other players' selections | Outlines and name tags. |
| Join and leave notifications | Short messages when people come and go. |
| Receive invites | Stay connected to the server so friends can invite you by name. |

## Limitations (v1)

* Only objects are synced. Level settings, colour channels, groups used/free, song and so on are
  not. Agree on those outside of the mod, e.g. with a shared level that you all start from.
* Undo / redo is best effort. It is cleared when you join a room.
* Playtesting pauses syncing until you leave the playtest.
* If two people edit the very same object at the same time, the last change wins.
* The connection is plain TCP without encryption. The room code is the only "password", so use
  random codes and only host servers for people you trust.
* Windows only. Other platforms need a build for that platform.

## Server

Needs Node.js 18 or newer. No packages to install.

```
cd server
node server.js          # listens on port 5588
```

Options (environment variables): `PORT`, `HOST`, `DATA_DIR`, `MAX_CLIENTS_PER_ROOM`,
`MAX_ROOMS`, `ROOM_TTL_DAYS`. Open the TCP port in your firewall / router, or use a TCP tunnel.
`node test.js` runs the automated protocol test.

For a server that is always on, run it on any VPS (or a host that exposes raw TCP ports, for
example Fly.io) - `server/Dockerfile` is included. The mod speaks plain TCP, not HTTP, so hosts
that only offer web services will not work. The server is the only thing that has to stay online;
the mod itself needs no other infrastructure.

### Protocol (for the curious)

One JSON message per line over TCP.

| Direction | Message |
| --- | --- |
| client -> server | `hello` (`ver`, `mode` = `host`/`join`/`lobby`, `room`, `name`, optional `icon`, `c1`, `c2`, `glow`), `ready`, `snapshot` (`objs`), `snapshot_end`, `ops` (`ops`), `sel` (`ids`, `n`), `invite` (`to`), `ping` |
| server -> client | `welcome` (`id`, `room`, `empty`, `count`), `snapshot`, `snapshot_end`, `ops` (`from`, `ops`), `players` (`list` of `id`, `name`, `icon`, `c1`, `c2`, `glow`), `sel` (`from`, `ids`, `n`), `invite` (`from`, `room`), `invite_result` (`to`, `sent`, `reason`), `lobby`, `error` (`msg`), `pong` |

An op is `["a", id, objectString]` (add), `["u", id, objectString]` (update) or `["d", id]` (delete).

## Building the mod

**GitHub Actions (no setup on your PC):** push this folder to a GitHub repository. The workflow in
`.github/workflows/build.yml` builds the mod; download the `.geode` file from the finished run
(Actions tab -> the run -> Artifacts) and put it in `%LOCALAPPDATA%\GeometryDash\geode\mods`.

**Locally:** install the [Geode CLI](https://docs.geode-sdk.org/getting-started/geode-cli) and the
Windows build tools, then run `geode build` in this folder.
