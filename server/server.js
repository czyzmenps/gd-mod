// Live Collab Editor - relay server
//
// A tiny dependency-free TCP server. Players connect with the mod, create or
// join a room, and every object change is relayed to everybody else in the
// room. The server also keeps the current level of each room so that people
// who join later (or after a restart) get the up-to-date level.
//
// Protocol: one JSON message per line over plain TCP (see README.md).
//
// Run:  node server.js          (default port 5588)
//       PORT=7000 node server.js

'use strict';

const net = require('net');
const fs = require('fs');
const path = require('path');

const PORT = parseInt(process.env.PORT || '5588', 10);
const HOST = process.env.HOST || '0.0.0.0';
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, 'data');
const MAX_CLIENTS_PER_ROOM = parseInt(process.env.MAX_CLIENTS_PER_ROOM || '16', 10);
const MAX_ROOMS = parseInt(process.env.MAX_ROOMS || '200', 10);
const ROOM_TTL_DAYS = parseFloat(process.env.ROOM_TTL_DAYS || '14');
const IDLE_TIMEOUT_MS = 60 * 1000;
const MAX_LINE_BYTES = 32 * 1024 * 1024;
const MAX_OBJECT_STRING = 256 * 1024;
const SNAPSHOT_CHUNK = 250;
const PROTOCOL_VERSION = 1;

fs.mkdirSync(DATA_DIR, { recursive: true });

/** @type {Map<string, Room>} */
const rooms = new Map();

function log(...args) {
    console.log(new Date().toISOString(), ...args);
}

// ---------------------------------------------------------------------------
// rooms + persistence
// ---------------------------------------------------------------------------

class Room {
    constructor(code) {
        this.code = code;
        this.objects = new Map(); // id -> object string
        this.clients = new Map(); // client id -> Client
        this.nextClientId = 0;
        this.loading = false; // true while the host is still uploading the level
        this.dirty = false;
        this.lastActive = Date.now();
    }
}

function roomFile(code) {
    return path.join(DATA_DIR, code + '.json');
}

function validCode(code) {
    return typeof code === 'string' && /^[A-Z0-9_-]{3,32}$/.test(code);
}

function loadRoomFromDisk(code) {
    try {
        const raw = fs.readFileSync(roomFile(code), 'utf8');
        const data = JSON.parse(raw);
        const room = new Room(code);
        room.nextClientId = Number(data.nextClientId) || 0;
        for (const entry of data.objects || []) {
            if (Array.isArray(entry) && typeof entry[0] === 'string' && typeof entry[1] === 'string') {
                room.objects.set(entry[0], entry[1]);
            }
        }
        return room;
    } catch {
        return null;
    }
}

function roomExists(code) {
    return rooms.has(code) || fs.existsSync(roomFile(code));
}

function getRoom(code) {
    let room = rooms.get(code);
    if (!room) {
        room = loadRoomFromDisk(code);
        if (room) rooms.set(code, room);
    }
    return room || null;
}

function saveRoom(room) {
    if (room.loading) return; // never persist a half-uploaded level
    const payload = JSON.stringify({
        nextClientId: room.nextClientId,
        objects: Array.from(room.objects.entries()),
    });
    const file = roomFile(room.code);
    const tmp = file + '.tmp';
    fs.writeFile(tmp, payload, (err) => {
        if (err) return log('could not save room', room.code, err.message);
        fs.rename(tmp, file, (err2) => {
            if (err2) log('could not save room', room.code, err2.message);
        });
    });
    room.dirty = false;
}

setInterval(() => {
    const now = Date.now();
    for (const room of rooms.values()) {
        if (room.dirty) saveRoom(room);
        // free the memory of rooms nobody has used for a while (they stay on disk)
        if (room.clients.size === 0 && !room.loading && now - room.lastActive > 10 * 60 * 1000) {
            rooms.delete(room.code);
        }
    }
    // delete old rooms from disk
    const maxAge = ROOM_TTL_DAYS * 24 * 60 * 60 * 1000;
    fs.readdir(DATA_DIR, (err, files) => {
        if (err) return;
        for (const f of files) {
            if (!f.endsWith('.json')) continue;
            const full = path.join(DATA_DIR, f);
            fs.stat(full, (e, st) => {
                if (e) return;
                const code = f.slice(0, -5);
                const inMemory = rooms.get(code);
                if (now - st.mtimeMs > maxAge && !(inMemory && inMemory.clients.size > 0)) {
                    fs.unlink(full, () => log('deleted expired room', code));
                    rooms.delete(code);
                }
            });
        }
    });
}, 20 * 1000).unref();

// ---------------------------------------------------------------------------
// clients
// ---------------------------------------------------------------------------

class Client {
    constructor(socket) {
        this.socket = socket;
        this.id = 0;
        this.name = 'Player';
        this.room = null;
        this.state = 'new'; // new -> awaiting_ready -> live
        this.buffer = '';
        this.closed = false;
        this.timer = null;
        this.resetIdle();
    }

    resetIdle() {
        clearTimeout(this.timer);
        this.timer = setTimeout(() => this.kick('Timed out'), IDLE_TIMEOUT_MS);
    }

    send(obj) {
        if (this.closed || this.socket.destroyed) return;
        this.socket.write(JSON.stringify(obj) + '\n');
    }

    error(msg) {
        this.send({ t: 'error', msg });
        this.socket.end();
    }

    kick(reason) {
        if (this.closed) return;
        this.send({ t: 'error', msg: reason });
        this.socket.destroy();
    }
}

function broadcastPlayers(room) {
    const list = Array.from(room.clients.values()).map((c) => ({ id: c.id, name: c.name }));
    for (const c of room.clients.values()) c.send({ t: 'players', list });
}

function leaveRoom(client) {
    const room = client.room;
    if (!room) return;
    room.clients.delete(client.id);
    room.lastActive = Date.now();
    client.room = null;
    log(`[${room.code}] ${client.name} (#${client.id}) left, ${room.clients.size} online`);

    if (room.loading) {
        // the host disappeared before the level was fully uploaded
        rooms.delete(room.code);
        for (const c of room.clients.values()) c.kick('The host left before the room was ready');
        return;
    }
    if (room.clients.size > 0) broadcastPlayers(room);
}

function onHello(client, msg) {
    if (client.room) return;
    if (msg.ver !== PROTOCOL_VERSION) {
        return client.error('Version mismatch: update the mod and the server to the same version');
    }
    const code = String(msg.room || '').toUpperCase();
    if (!validCode(code)) {
        return client.error('Invalid room code (use 3-32 letters or digits)');
    }
    const name = String(msg.name || 'Player').replace(/[\u0000-\u001f]/g, '').trim().slice(0, 20) || 'Player';
    const host = msg.mode === 'host';

    let room;
    if (host) {
        if (roomExists(code)) {
            return client.error('That room code is already in use - pick another one, or press Join');
        }
        if (rooms.size >= MAX_ROOMS) return client.error('The server is full, try again later');
        room = new Room(code);
        room.loading = true;
        rooms.set(code, room);
    } else {
        room = getRoom(code);
        if (!room) return client.error('Room not found - check the code');
        if (room.loading) return client.error('The room is still being created, try again in a few seconds');
    }
    if (room.clients.size >= MAX_CLIENTS_PER_ROOM) {
        return client.error('The room is full');
    }

    client.id = ++room.nextClientId;
    client.name = name;
    client.room = room;
    client.state = host ? 'live' : 'awaiting_ready';
    room.clients.set(client.id, client);
    room.dirty = true;
    room.lastActive = Date.now();

    log(`[${code}] ${name} (#${client.id}) ${host ? 'created' : 'joined'}, ${room.clients.size} online`);
    client.send({ t: 'welcome', id: client.id, room: code, empty: host, count: room.objects.size });
    broadcastPlayers(room);
}

function onReady(client) {
    const room = client.room;
    if (!room || client.state !== 'awaiting_ready') return;

    let chunk = [];
    for (const [id, str] of room.objects) {
        chunk.push([id, str]);
        if (chunk.length >= SNAPSHOT_CHUNK) {
            client.send({ t: 'snapshot', objs: chunk });
            chunk = [];
        }
    }
    if (chunk.length) client.send({ t: 'snapshot', objs: chunk });
    client.send({ t: 'snapshot_end' });
    client.state = 'live';
}

function onSnapshot(client, msg) {
    const room = client.room;
    if (!room || !room.loading || !Array.isArray(msg.objs)) return;
    for (const entry of msg.objs) {
        if (!Array.isArray(entry)) continue;
        const [id, str] = entry;
        if (typeof id === 'string' && typeof str === 'string' && id.length < 64 && str.length <= MAX_OBJECT_STRING) {
            room.objects.set(id, str);
        }
    }
}

function onSnapshotEnd(client) {
    const room = client.room;
    if (!room || !room.loading) return;
    room.loading = false;
    room.dirty = true;
    log(`[${room.code}] level uploaded: ${room.objects.size} objects`);
}

function onOps(client, msg) {
    const room = client.room;
    if (!room || room.loading || client.state !== 'live' || !Array.isArray(msg.ops)) return;

    const accepted = [];
    for (const op of msg.ops) {
        if (!Array.isArray(op) || typeof op[0] !== 'string' || typeof op[1] !== 'string') continue;
        const [kind, id, str] = op;
        if (id.length === 0 || id.length >= 64) continue;
        if (kind === 'd') {
            room.objects.delete(id);
            accepted.push(['d', id]);
        } else if ((kind === 'a' || kind === 'u') && typeof str === 'string' && str.length <= MAX_OBJECT_STRING) {
            room.objects.set(id, str);
            accepted.push([kind, id, str]);
        }
    }
    if (accepted.length === 0) return;

    room.dirty = true;
    room.lastActive = Date.now();
    const out = { t: 'ops', from: client.id, ops: accepted };
    for (const other of room.clients.values()) {
        if (other !== client && other.state === 'live') other.send(out);
    }
}

function onMessage(client, msg) {
    switch (msg.t) {
        case 'hello': return onHello(client, msg);
        case 'ready': return onReady(client);
        case 'snapshot': return onSnapshot(client, msg);
        case 'snapshot_end': return onSnapshotEnd(client);
        case 'ops': return onOps(client, msg);
        case 'ping': return client.send({ t: 'pong' });
        default: return; // ignore unknown messages
    }
}

const server = net.createServer((socket) => {
    socket.setNoDelay(true);
    const client = new Client(socket);

    socket.on('data', (chunk) => {
        client.resetIdle();
        client.buffer += chunk.toString('utf8');
        if (client.buffer.length > MAX_LINE_BYTES) {
            return client.kick('Message too large');
        }
        let nl;
        while ((nl = client.buffer.indexOf('\n')) !== -1) {
            const line = client.buffer.slice(0, nl).trim();
            client.buffer = client.buffer.slice(nl + 1);
            if (!line) continue;
            let msg;
            try {
                msg = JSON.parse(line);
            } catch {
                continue;
            }
            if (msg && typeof msg === 'object') {
                try {
                    onMessage(client, msg);
                } catch (e) {
                    log('error handling message:', e);
                }
            }
        }
    });

    const cleanup = () => {
        if (client.closed) return;
        client.closed = true;
        clearTimeout(client.timer);
        leaveRoom(client);
    };
    socket.on('close', cleanup);
    socket.on('error', cleanup);
});

function shutdown() {
    log('shutting down, saving rooms...');
    for (const room of rooms.values()) {
        if (!room.loading) {
            try {
                fs.writeFileSync(
                    roomFile(room.code),
                    JSON.stringify({ nextClientId: room.nextClientId, objects: Array.from(room.objects.entries()) })
                );
            } catch (e) {
                log('could not save room', room.code, e.message);
            }
        }
    }
    process.exit(0);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

server.listen(PORT, HOST, () => {
    log(`Live Collab server listening on ${HOST}:${PORT} (data: ${DATA_DIR})`);
});
