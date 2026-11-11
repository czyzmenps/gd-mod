// Tests the mod's built-in party server (src/LocalServer.cpp) with fake clients.
//   g++ -std=c++20 -O1 -o localserver tools/localserver_main.cpp src/LocalServer.cpp -pthread
//   node tools/test-local.js ./localserver
'use strict';

const net = require('net');
const assert = require('assert');
const { spawn } = require('child_process');

const BIN = process.argv[2] || './localserver';
const PORT = 5598;

function startServer() {
    const proc = spawn(BIN, [String(PORT)], { stdio: ['ignore', 'pipe', 'inherit'] });
    return new Promise((resolve, reject) => {
        proc.on('error', reject);
        proc.stdout.on('data', (d) => { if (d.toString().includes('listening')) resolve(proc); });
    });
}

class FakeClient {
    constructor() { this.messages = []; this.buffer = ''; this.waiters = []; this.closed = false; }
    connect() {
        return new Promise((resolve) => {
            this.socket = net.connect(PORT, '127.0.0.1', resolve);
            this.socket.setEncoding('utf8');
            this.socket.on('data', (d) => {
                this.buffer += d;
                let nl;
                while ((nl = this.buffer.indexOf('\n')) !== -1) {
                    const line = this.buffer.slice(0, nl);
                    this.buffer = this.buffer.slice(nl + 1);
                    if (line) this.messages.push(JSON.parse(line));
                }
                this.notify();
            });
            this.socket.on('close', () => { this.closed = true; this.notify(); });
            this.socket.on('error', () => {});
        });
    }
    notify() { this.waiters = this.waiters.filter((w) => !w()); }
    send(obj) { this.socket.write(JSON.stringify(obj) + '\n'); }
    raw(text) { this.socket.write(text); }
    next(pred, timeoutMs = 4000) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('timeout waiting for message')), timeoutMs);
            const check = () => {
                const i = this.messages.findIndex(pred);
                if (i === -1) return false;
                clearTimeout(timer);
                resolve(this.messages.splice(i, 1)[0]);
                return true;
            };
            if (!check()) this.waiters.push(check);
        });
    }
    nextType(t, ms) { return this.next((m) => m.t === t, ms); }
    waitClosed(ms = 3000) {
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('timeout waiting for close')), ms);
            const check = () => { if (!this.closed) return false; clearTimeout(timer); resolve(); return true; };
            if (!check()) this.waiters.push(check);
        });
    }
    end() { this.socket.destroy(); }
}

const hello = (mode, room, name, extra = {}) => ({ t: 'hello', ver: 1, mode, room, name, ...extra });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
    const proc = await startServer();
    let passed = 0;
    const ok = (name) => { passed++; console.log('  ok  ' + name); };

    try {
        // --- the host creates the party and uploads the level ---
        const a = new FakeClient();
        await a.connect();
        a.send(hello('host', 'abcd2345', 'Alice', { icon: 7, c1: 5, c2: 12, glow: true }));
        const welcomeA = await a.nextType('welcome');
        assert.deepStrictEqual(welcomeA, { t: 'welcome', id: 1, room: 'ABCD2345', empty: true, count: 0 });
        const playersA = await a.nextType('players');
        assert.deepStrictEqual(playersA.list, [{ id: 1, name: 'Alice', icon: 7, c1: 5, c2: 12, glow: true }]);
        ok('host creates the party (code upper-cased) and sees itself in the player list');

        // --- joining before the upload is finished is refused ---
        const early = new FakeClient();
        await early.connect();
        early.send(hello('join', 'ABCD2345', 'Early'));
        assert.match((await early.nextType('error')).msg, /still being created/);
        await early.waitClosed();
        ok('joining while the level is uploading is refused and the connection closed');

        // level with awkward characters, many objects (several snapshot chunks)
        const objs = [];
        for (let i = 0; i < 1200; i++) objs.push([`1.${i}`, `1,${i},2,${i * 3},3,0,"q\\${i}"é;`]);
        for (let i = 0; i < objs.length; i += 300) a.send({ t: 'snapshot', objs: objs.slice(i, i + 300) });
        a.send({ t: 'snapshot_end' });
        await sleep(150);

        // --- refusals ---
        const e1 = new FakeClient();
        await e1.connect();
        e1.send(hello('join', 'NOPE123', 'X'));
        assert.match((await e1.nextType('error')).msg, /not found/i);
        ok('wrong party code: "Room not found"');

        const e2 = new FakeClient();
        await e2.connect();
        e2.send(hello('host', 'OTHER123', 'X'));
        assert.match((await e2.nextType('error')).msg, /already in use/);
        ok('a second host is refused');

        const e3 = new FakeClient();
        await e3.connect();
        e3.send({ t: 'hello', ver: 2, mode: 'join', room: 'ABCD2345', name: 'Old' });
        assert.match((await e3.nextType('error')).msg, /Version mismatch/);
        ok('wrong protocol version is refused');

        const e4 = new FakeClient();
        await e4.connect();
        e4.send(hello('join', 'a b!', 'X'));
        assert.match((await e4.nextType('error')).msg, /Invalid room code/);
        ok('invalid code is refused');

        const e5 = new FakeClient();
        await e5.connect();
        e5.send(hello('lobby', '', 'X'));
        assert.match((await e5.nextType('error')).msg, /no invite service/);
        ok('lobby (invite service) is politely refused');

        // --- a friend joins and gets the whole level ---
        const b = new FakeClient();
        await b.connect();
        // the hello arrives in two pieces on purpose
        const helloText = JSON.stringify(hello('join', 'abcd2345', '  Bob\u0001  with a very long name that is cut ', { icon: 9999, c1: -4, c2: 3.7 })) + '\n';
        b.raw(helloText.slice(0, 20));
        await sleep(50);
        b.raw(helloText.slice(20));
        const welcomeB = await b.nextType('welcome');
        assert.strictEqual(welcomeB.id, 2);
        assert.strictEqual(welcomeB.empty, false);
        assert.strictEqual(welcomeB.count, 1200);
        const players = (await b.nextType('players')).list;
        assert.strictEqual(players.length, 2);
        assert.strictEqual(players[1].name, 'Bob  with a very lon');
        assert.strictEqual(players[1].icon, 484);
        assert.strictEqual(players[1].c1, 0);
        assert.strictEqual(players[1].c2, 3);
        assert.strictEqual(players[1].glow, false);
        ok('join: ids, cleaned name, clamped icon/colours, split hello handled');

        assert.strictEqual((await a.nextType('players')).list.length, 2);
        ok('the host is told about the new player');

        b.send({ t: 'ready' });
        const got = new Map();
        let chunks = 0;
        for (;;) {
            const m = await b.next((x) => x.t === 'snapshot' || x.t === 'snapshot_end');
            if (m.t === 'snapshot_end') break;
            chunks++;
            for (const [id, str] of m.objs) got.set(id, str);
        }
        assert.strictEqual(got.size, 1200);
        assert.strictEqual(chunks, 5); // 250 per chunk
        assert.deepStrictEqual([...got.entries()], objs);
        ok('snapshot is complete, in the original order, with quotes/backslashes/unicode intact');

        // --- live editing ---
        a.send({ t: 'ops', ops: [['a', '1.5000', '1,9,2,1,3,1;'], ['u', '1.5', 'upd'], ['d', '1.6'], ['x', 'bad'], ['a', '', 'no id']] });
        const opsB = await b.nextType('ops');
        assert.deepStrictEqual(opsB, { t: 'ops', from: 1, ops: [['a', '1.5000', '1,9,2,1,3,1;'], ['u', '1.5', 'upd'], ['d', '1.6']] });
        ok('ops are relayed (invalid ones dropped) with the sender id');

        // nobody gets their own ops back
        await sleep(100);
        assert.ok(!a.messages.some((m) => m.t === 'ops'));
        ok('the sender does not get its own ops back');

        // --- selections ---
        b.send({ t: 'sel', ids: ['1.5', '1.7', 5, ''], n: 40 });
        const selA = await a.nextType('sel');
        assert.deepStrictEqual(selA, { t: 'sel', from: 2, ids: ['1.5', '1.7'], n: 40 });
        ok('selection is shared');

        const c = new FakeClient();
        await c.connect();
        c.send(hello('join', 'ABCD2345', 'Carol'));
        await c.nextType('welcome');
        c.send({ t: 'ready' });
        await c.nextType('snapshot_end');
        const selC = await c.nextType('sel');
        assert.deepStrictEqual(selC, { t: 'sel', from: 2, ids: ['1.5', '1.7'], n: 40 });
        ok('a late joiner is shown the current selections');

        // late joiner sees the edits made after the upload
        c.end();
        const c2 = new FakeClient();
        await c2.connect();
        c2.send(hello('join', 'ABCD2345', 'Carol'));
        const w = await c2.nextType('welcome');
        assert.strictEqual(w.count, 1200 + 1 - 1);   // +1.5000 added, 1.6 deleted
        c2.send({ t: 'ready' });
        const all = new Map();
        for (;;) {
            const m = await c2.next((x) => x.t === 'snapshot' || x.t === 'snapshot_end');
            if (m.t === 'snapshot_end') break;
            for (const [id, str] of m.objs) all.set(id, str);
        }
        assert.strictEqual(all.get('1.5'), 'upd');
        assert.ok(!all.has('1.6'));
        assert.strictEqual(all.get('1.5000'), '1,9,2,1,3,1;');
        ok('the server keeps the edited level for later joiners');

        // --- misc messages ---
        b.send({ t: 'ping' });
        await b.nextType('pong');
        b.send({ t: 'invite', to: 'Dave' });
        const inv = await b.nextType('invite_result');
        assert.strictEqual(inv.sent, 0);
        assert.strictEqual(inv.reason, 'no_server');
        ok('ping/pong and "invites need a server" answer');

        b.raw('this is not json\n{"t":"nothing"}\n[1,2,3]\n\n');
        b.send({ t: 'ping' });
        await b.nextType('pong');
        ok('garbage lines are ignored');

        // --- somebody leaves ---
        c2.end();
        await sleep(150);
        b.end();
        const after = await a.next((m) => m.t === 'players' && m.list.length === 1);
        assert.strictEqual(after.list[0].name, 'Alice');
        ok('leaving players disappear from the list');

        // --- the host leaves: the party is over ---
        const d = new FakeClient();
        await d.connect();
        d.send(hello('join', 'ABCD2345', 'Dan'));
        await d.nextType('welcome');
        a.end();
        assert.match((await d.nextType('error')).msg, /host/i);
        await d.waitClosed();
        ok('when the host leaves everybody is told and disconnected');

        const f = new FakeClient();
        await f.connect();
        f.send(hello('join', 'ABCD2345', 'Late'));
        assert.match((await f.nextType('error')).msg, /not found/i);
        ok('nobody can join a finished party');
    } finally {
        proc.kill('SIGTERM');
    }
    console.log(`\n${passed} checks passed`);
}

main().then(() => process.exit(0), (e) => { console.error('\nFAILED:', e); process.exit(1); });
