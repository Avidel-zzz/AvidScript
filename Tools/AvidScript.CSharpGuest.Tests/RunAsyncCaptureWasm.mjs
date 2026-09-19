import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

assert.ok(process.argv[2], 'Pass async-captures-direct.wasm or async-captures-cfg.wasm.');
const module = await WebAssembly.compile(readFileSync(process.argv[2]));
let instance;
let active;
let nextToken = 1n;
const pending = [];
const frames = new Map();
let stores = 0;
let reads = 0;
const memory = () => Object.values(instance.exports).find(value => value instanceof WebAssembly.Memory);
const env = {
    continuation_delay(seconds, callback) {
        assert.equal(seconds, 0);
        const token = nextToken++;
        pending.push({ token, callback, seed: active.seed, step: active.step + 1 });
        return token;
    },
    continuation_state_store(token, address, count) {
        assert.ok(count > 0);
        frames.set(token, new Uint8Array(memory().buffer, address, count).slice());
        if (process.env.AVIDSCRIPT_CAPTURE_TRACE) console.log('store', token, address, [...frames.get(token)]);
        ++stores;
        return 1;
    },
    continuation_state_read(token, address, count) {
        const frame = frames.get(token);
        if (process.env.AVIDSCRIPT_CAPTURE_TRACE) console.log('read', token, address, frame && [...frame]);
        assert.equal(frame?.length, count);
        new Uint8Array(memory().buffer, address, count).set(frame);
        frames.delete(token);
        ++reads;
        return 1;
    },
    continuation_cancel() { assert.fail('Valid capture execution must not cancel'); },
};
for (const imported of WebAssembly.Module.imports(module))
    assert.ok(imported.module === 'env' && imported.name in env, `Unexpected import ${imported.name}`);
instance = await WebAssembly.instantiate(module, { env });
let cases = 0;
for (let round = 0; round < 20; ++round) {
    // Two live activations with distinct captured cells, resumed in interleaved order.
    for (const seed of [11 + round, 101 + round]) {
        active = { seed, step: 0 };
        instance.exports.set_seed(seed);
        instance.exports.avid_on_begin_play();
    }
    let resumes = 0;
    while (pending.length) {
        assert.ok(++resumes <= 4, 'Each activation must await exactly twice');
        active = pending.shift();
        instance.exports.avid_on_continuation_v2(active.callback, active.token, 1, 0, 0);
        assert.equal(instance.exports.result(), active.seed + 4 * active.step, `seed ${active.seed}, step ${active.step}`);
        ++cases;
    }
    assert.equal(resumes, 4);
    assert.equal(frames.size, 0);
}
assert.equal(stores, 80);
assert.equal(reads, stores);
console.log(`AvidScript.AsyncCaptures.WasmExecution: ${cases}/${cases} passed; ${stores} stored/${reads} restored`);
