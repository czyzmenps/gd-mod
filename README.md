# Live Collab Editor

A [Geode](https://geode-sdk.org) mod for Geometry Dash (Windows, GD 2.2081, Geode 5.x) that lets
several players work in the **same level editor at the same time**. Everyone sees objects being
placed, deleted, moved and edited by the others in near real time.

**No server to set up:** when you press *Start party*, your own game becomes the party server for as
long as the party runs (and asks your router to open the port, UPnP). A stand-alone server
(`server/`) is still included for people who prefer one that is always on.

## How it works

* Every object gets a unique id. Changes are detected locally (new objects, deleted objects,
  moved / rotated / edited objects) and sent to the server, which relays them to everybody else
  in the room.
* The server keeps the current level of every room, so people who join later get the up-to-date
  level, and rooms survive a server restart.
* Conflicts: last change wins.

## Using it

1. In the editor press the small round **Collab** button (top right by default, move or hide it in
   the Config) or the shortcut **F8**. The light on the button shows the state (grey = offline,
   yellow = connecting, blue = syncing, green = live). The button sits in a corner, so the editor
   stays fully usable.
2. **Start party** creates a party from the level that is open in your editor. Your game starts the
   party server, opens the port in your router and copies the **party code** to your clipboard.
   Send that code to your friends. It looks like `ABCD2345@12.34.56.78:5588` - it contains the
   address of your PC, so nobody types an address or a port.
   * The first time, Windows may ask whether Geometry Dash may accept connections: allow it.
   * Keep the editor open while you host: the party ends when the host leaves.
3. **Join a party** (friends): open an **empty** level in the editor (joining **replaces
   everything in your editor** with the party's level), press F8, paste the code and press **Join**.
4. Once you are in a party the window shows the code with a **Copy code** button, a progress bar
   while the level is uploaded / downloaded, the list of everybody with their own icon and what
   they have selected, and **Leave room**.
5. While you are in a party a small pill at the top of the editor shows the party and how many
   people are online, other players' selections are outlined in their colour with a name tag, and
   a short notification pops up when somebody joins or leaves.

### If the party does not start

The window says what went wrong and what to do:

* **"Your router does not allow automatic port opening (UPnP)"** - turn UPnP on in your router's
  settings, or forward TCP port **5588** to your PC yourself and type your public address in the
  Config (**Public address**).
* **"Your internet provider shares one address between customers"** (CGNAT, common on mobile
  internet) - friends cannot reach your PC directly. Use a tunnel such as playit.gg (TCP tunnel to
  local port 5588) and type the tunnel address in the Config (**Public address**).
* Or run the stand-alone server (see below) on a machine that is reachable, and type its address
  in the Config (**Party server**). Then everybody on that server can also invite each other by name.

### Config

| Setting | What it does |
| --- | --- |
| Party server (optional) | Empty = your own game hosts the party. Or the address of a stand-alone server (`host` or `host:port`), then invites by name work too. |
| Public address (advanced) | Only if automatic port opening does not work: the address friends can reach your PC on (forwarded port or tunnel). |
| Your name | Name the others see. Empty = your Geometry Dash account name. |
| Button position / size | Corner of the editor (top/bottom, left/right) or hidden, and how big it is. |
| Open Collab window | Shortcut (default F8). |
| Copy the party code when starting | On by default. |
| Show other players' selections | Outlines and name tags. |
| Join and leave notifications | Short messages when people come and go. |
| Receive invites | Stay connected to the party server so friends can invite you by name (needs a party server). |

## Limitations (v1)

* Only objects are synced. Level settings, colour channels, groups used/free, song and so on are
  not. Agree on those outside of the mod, e.g. with a shared level that you all start from.
* Undo / redo is best effort. It is cleared when you join a room.
* Playtesting pauses syncing until you leave the playtest.
* If two people edit the very same object at the same time, the last change wins.
* The connection is plain TCP without encryption. The party code is the only "password", so only
  send it to people you trust. A party hosted by your game shows your public IP address to the
  people you give the code to.
* Invites by name need a stand-alone party server; with a party hosted by your game, send the code.
* Parties hosted by a game are not saved: when the host leaves, the party is over. The stand-alone
  server keeps rooms.
* Windows only. Other platforms need a build for that platform.

## Server

Needs Node.js 18 or newer. No packages to install.

```
cd server
node server.js          # listens on port 5588
```

Options (environment variables): `PORT`, `HOST`, `DATA_DIR`, `MAX_CLIENTS_PER_ROOM`,
`MAX_ROOMS`, `ROOM_TTL_DAYS`. Open the TCP port in your firewall / router, or use a TCP tunnel.
`node test.js` runs the automated protocol test. The server that is built into the mod
(`src/LocalServer.cpp`) speaks the same protocol and is tested with `tools/test-local.js`.

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
