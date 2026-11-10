// End-to-end test for the relay server: node test.js
// Starts a server on a temporary port/data folder and plays two or three
// fake clients against it, exactly like the mod would.

'use strict';

const net = require('net');
const os = require('os');
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const { spawn } = require('child_process');

const PORT = 5599;
const DATA_DIR = fs.mkdtempSync(path.join(os.tmpdir(), 'collab-test-'));

function startServer() {
    const proc = spawn(process.execPath, [path.join(__dirname, 'server.js')], {
        env: { ...process.env, PORT: String(PORT), HOST: '127.0.0.1', DATA_DIR },
        stdio: ['ignore', 'pipe', 'inherit'],
    });
    return new Promise((resolve) => {
        proc.stdout.on('data', (d) => {
            if (d.toString().includes('listening')) resolve(proc);
        });
    });
}

class FakeClient {
    constructor() {
        this.messages = [];
        this.buffer = '';
        this.waiters = [];
        this.closed = false;
    }
    connect() {
        return new Promise((resolve) => {
            this.socket = net.connect(PORT, '127.0.0.1', resolve);
            this.socket.on('data', (d) => {
                this.buffer += d.toString();
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
    notify() {
        this.waiters = this.waiters.filter((w) => !w());
    }
    send(obj) { this.socket.write(JSON.stringify(obj) + '\n'); }
    // resolves with the first not-yet-consumed message matching the predicate
    next(pred, timeoutMs = 3000) {
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
    nextType(t, timeoutMs) { return this.next((m) => m.t === t, timeoutMs); }
    end() { this.socket.destroy(); }
}

const hello = (mode, room, name) => ({ t: 'hello', ver: 1, mode, room, name });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
    let server = await startServer();
    let passed = 0;
    const ok = (name) => { passed++; console.log('  ok  ' + name); };

    // --- host a room and upload a level ---
    const a = new FakeClient();
    await a.connect();
    a.send(hello('host', 'testroom', 'Alice'));
    const welcomeA = await a.nextType('welcome');
    assert.strictEqual(welcomeA.empty, true);
    assert.strictEqual(welcomeA.room, 'TESTROOM');
    assert.strictEqual(welcomeA.id, 1);
    ok('host creates a room and gets client id 1');

    // --- joining while the host is still uploading is refused ---
    const early = new FakeClient();
    await early.connect();
    early.send(hello('join', 'TESTROOM', 'Early'));
    const earlyErr = await early.nextType('error');
    assert.match(earlyErr.msg, /still being created/);
    ok('joining a room that is still uploading is refused');

    a.send({ t: 'snapshot', objs: [['1.1', '1,1,2,0,3,0;'], ['1.2', '1,1,2,30,3,0;']] });
    a.send({ t: 'snapshot', objs: [['1.3', '1,1,2,60,3,0;']] });
    a.send({ t: 'snapshot_end' });
    await sleep(100);

    // --- error cases ---
    const e1 = new FakeClient();
    await e1.connect();
    e1.send(hello('join', 'NOPE123', 'X'));
    assert.match((await e1.nextType('error')).msg, /not found/i);
    ok('joining an unknown room fails');

    const e2 = new FakeClient();
    await e2.connect();
    e2.send(hello('host', 'TESTROOM', 'X'));
    assert.match((await e2.nextType('error')).msg, /already in use/);
    ok('hosting an existing room code fails');

    const e3 = new FakeClient();
    await e3.connect();
    e3.send({ t: 'hello', ver: 99, mode: 'join', room: 'TESTROOM', name: 'X' });
    assert.match((await e3.nextType('error')).msg, /Version mismatch/);
    ok('protocol version mismatch is reported');

    // --- join and receive the snapshot ---
    const b = new FakeClient();
    await b.connect();
    b.send(hello('join', 'testroom', 'Bob'));
    const welcomeB = await b.nextType('welcome');
    assert.strictEqual(welcomeB.empty, false);
    assert.strictEqual(welcomeB.count, 3);
    assert.strictEqual(welcomeB.id, 2);
    b.send({ t: 'ready' });
    const snap = await b.nextType('snapshot');
    assert.deepStrictEqual(snap.objs.map((o) => o[0]), ['1.1', '1.2', '1.3']);
    await b.nextType('snapshot_end');
    ok('joiner receives the full snapshot');

    const players = await a.next((m) => m.t === 'players' && m.list.length === 2);
    assert.deepStrictEqual(players.list.map((p) => p.name), ['Alice', 'Bob']);
    ok('both players see the player list');

    // --- live changes are relayed, but not echoed ---
    a.send({ t: 'ops', ops: [['a', '1.4', '1,1,2,90,3,0;'], ['u', '1.1', '1,1,2,5,3,5;'], ['d', '1.2']] });
    const relayed = await b.nextType('ops');
    assert.strictEqual(relayed.from, 1);
    assert.deepStrictEqual(relayed.ops, [['a', '1.4', '1,1,2,90,3,0;'], ['u', '1.1', '1,1,2,5,3,5;'], ['d', '1.2']]);
    await sleep(100);
    assert.strictEqual(a.messages.filter((m) => m.t === 'ops').length, 0);
    ok('ops from Alice reach Bob and are not echoed back');

    b.send({ t: 'ops', ops: [['a', '2.1', '1,1,2,120,3,0;'], ['d', '1.3']] });
    const relayed2 = await a.nextType('ops');
    assert.strictEqual(relayed2.from, 2);
    assert.strictEqual(relayed2.ops.length, 2);
    ok('ops from Bob reach Alice');

    // malformed ops are ignored and do not crash the server
    a.send({ t: 'ops', ops: [null, 5, ['x', 'y'], ['a'], ['a', '', 'z'], ['d']] });
    a.send({ t: 'ping' });
    await a.nextType('pong');
    ok('malformed ops are ignored');

    // --- a third player joining mid-session gets the *current* level ---
    const c = new FakeClient();
    await c.connect();
    c.send(hello('join', 'TESTROOM', 'Carol'));
    const welcomeC = await c.nextType('welcome');
    assert.strictEqual(welcomeC.count, 3); // 1.1, 1.4, 2.1
    c.send({ t: 'ready' });
    const snapC = await c.nextType('snapshot');
    assert.deepStrictEqual(snapC.objs.map((o) => o[0]).sort(), ['1.1', '1.4', '2.1']);
    assert.strictEqual(snapC.objs.find((o) => o[0] === '1.1')[1], '1,1,2,5,3,5;');
    await c.nextType('snapshot_end');
    ok('late joiner gets the up-to-date level');

    // --- leaving updates the player list ---
    b.end();
    const afterLeave = await a.next((m) => m.t === 'players' && m.list.length === 2 && m.list.some((p) => p.name === 'Carol'));
    assert.ok(!afterLeave.list.some((p) => p.name === 'Bob'));
    ok('player list updates when somebody leaves');

    // --- persistence across a restart ---
    a.end();
    c.end();
    await sleep(200);
    server.kill('SIGTERM');
    await sleep(300);
    server = await startServer();

    const d = new FakeClient();
    await d.connect();
    d.send(hello('join', 'TESTROOM', 'Dave'));
    const welcomeD = await d.nextType('welcome');
    assert.strictEqual(welcomeD.count, 3);
    assert.ok(welcomeD.id > 3, 'client ids must not be reused after a restart, got ' + welcomeD.id);
    ok('room survives a server restart and client ids are not reused');

    d.end();
    server.kill('SIGTERM');
    fs.rmSync(DATA_DIR, { recursive: true, force: true });
    console.log(`\nAll ${passed} checks passed`);
}

main().catch((e) => {
    console.error('\nTEST FAILED:', e);
    process.exit(1);
});
