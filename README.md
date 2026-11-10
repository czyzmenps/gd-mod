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

1. Run the server (see below) somewhere every player can reach.
2. In the editor click the **Collab** button on the left side of the screen.
3. Enter the server address (`address:port`, default port is `5588`) and your name.
   * **Host new room** uploads the level that is currently open in your editor. Leave the room
     code empty to get a random one, then press **Copy code** and send it to your team.
   * **Join room** (everyone else): enter the code. Joining **replaces everything in your editor**
     with the room's level, so open an empty level (or a copy) first.
4. The top of the screen shows `Collab <code> - N online` while you are connected.

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

### Protocol (for the curious)

One JSON message per line over TCP.

| Direction | Message |
| --- | --- |
| client -> server | `hello` (`ver`, `mode` = `host`/`join`, `room`, `name`), `ready`, `snapshot` (`objs`), `snapshot_end`, `ops` (`ops`), `ping` |
| server -> client | `welcome` (`id`, `room`, `empty`, `count`), `snapshot`, `snapshot_end`, `ops` (`from`, `ops`), `players` (`list`), `error` (`msg`), `pong` |

An op is `["a", id, objectString]` (add), `["u", id, objectString]` (update) or `["d", id]` (delete).

## Building the mod

**GitHub Actions (no setup on your PC):** push this folder to a GitHub repository. The workflow in
`.github/workflows/build.yml` builds the mod; download the `.geode` file from the finished run
(Actions tab -> the run -> Artifacts) and put it in `%LOCALAPPDATA%\GeometryDash\geode\mods`.

**Locally:** install the [Geode CLI](https://docs.geode-sdk.org/getting-started/geode-cli) and the
Windows build tools, then run `geode build` in this folder.
