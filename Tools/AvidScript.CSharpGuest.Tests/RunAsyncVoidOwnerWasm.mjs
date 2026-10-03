import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

// Bounded ABI test host for emitted WASM. UE Session/VM acceptance is separate.
assert.ok(process.argv[2], 'Pass the directory emitted by --async-void-error-owners.');
const directory = process.argv[2];
const cases = JSON.parse(readFileSync(join(directory, 'cases.json')));
assert.equal(cases.length, 24);
let passed = 0;
for (const fixture of cases) {
    await execute(fixture, false);
    ++passed;
}
await execute(cases.find(fixture => fixture.name === 'after-await-deferred'), true);
++passed;
console.log(`AvidScript.AsyncVoidOwner.WasmExecution: ${passed}/${passed} passed; ABI test host; 24 clean exits, 1 rejected-report trap`);

async function execute(fixture, rejectReport) {
    const wasm = await WebAssembly.compile(readFileSync(join(directory, fixture.name + '.wasm')));
    const ir = JSON.parse(readFileSync(join(directory, fixture.name + '.guest-ir.json')));
    let instance;
    let next = 1n;
    const token = () => next++;
    const tasks = new Map();
    const continuations = new Map();
    const ready = [];
    const frames = [];
    const roots = new Map();
    const objects = new Map();
    const layouts = new Map();
    const reports = [];
    let taskAllocations = 0;
    const memory = () => Object.values(instance.exports).find(value => value instanceof WebAssembly.Memory);
    const view = () => new DataView(memory().buffer);
    const task = id => {
        const value = tasks.get(id);
        assert.ok(value && value.refs > 0, `${fixture.name}: stale task ${id}`);
        return value;
    };
    const retain = id => { ++task(id).refs; };
    const release = id => {
        const value = task(id);
        if (--value.refs === 0 && value.state !== 0) tasks.delete(id);
    };
    const collect = () => {
        const live = new Set([...roots.values()].map(root => root.object));
        for (const value of tasks.values()) if (value.error) live.add(value.error.root);
        for (const id of objects.keys()) if (!live.has(id)) objects.delete(id);
    };
    const continuation = (callback, source = null) => {
        const id = token();
        continuations.set(id, { id, callback, source, retained: [], frame: null });
        return continuations.get(id);
    };
    const complete = (id, state, value = 0, error = null) => {
        const result = task(id);
        assert.equal(result.state, 0);
        Object.assign(result, { state, value, error });
        for (const waiter of result.waiters) ready.push(continuations.get(waiter));
        result.waiters.length = 0;
        return 1;
    };
    const heldError = id => {
        const result = task(id);
        assert.ok(result.state === 2 || result.state === 3);
        assert.ok(result.error && objects.has(result.error.root));
        return result.error;
    };
    const heap = (address, length, output, outputLength) => {
        const bytes = view();
        assert.equal(bytes.getUint32(address, true), 0x3150484d);
        let cursor = address + 8;
        const u32 = () => { const value = bytes.getUint32(cursor, true); cursor += 4; return value; };
        const u64 = () => { const value = bytes.getBigUint64(cursor, true); cursor += 8; return value; };
        const writeToken = value => { assert.equal(outputLength, 8); bytes.setBigUint64(output, value, true); };
        const root = id => { const value = roots.get(id); assert.ok(value, `stale managed root ${id}`); return value; };
        switch (bytes.getUint32(address + 4, true)) {
            case 1: {
                assert.equal(outputLength, 0);
                const count = u32();
                for (let index = 0; index < count; ++index) {
                    const type = u32(), size = u32(), references = u32();
                    assert.equal(references, 0, 'Fixture heap stores language-error values only');
                    layouts.set(type, size);
                }
                break;
            }
            case 2: { const id = token(); frames.push(id); writeToken(id); break; }
            case 3: {
                const id = u64(); assert.equal(frames.pop(), id);
                for (const [key, value] of roots) if (value.frame === id) roots.delete(key);
                assert.equal(outputLength, 0); break;
            }
            case 4: {
                const frame = u64(), object = u64(); assert.ok(frames.includes(frame));
                const id = token(); roots.set(id, { frame, object }); writeToken(id); break;
            }
            case 5: { const value = root(u64()); value.object = u64(); assert.equal(outputLength, 0); break; }
            case 6: { const id = u64(); root(id); roots.delete(id); assert.equal(outputLength, 0); break; }
            case 7: {
                const type = u32(), target = root(u64()); assert.ok(layouts.has(type));
                const id = token(); objects.set(id, { type, bytes: new Uint8Array(layouts.get(type)) });
                target.object = id; writeToken(id); break;
            }
            case 8:
            case 9: {
                const command = bytes.getUint32(address + 4, true);
                const object = objects.get(u64()), type = u32(), offset = u32(), count = u32();
                assert.ok(object && object.type === type && offset + count <= object.bytes.length);
                if (command === 8) {
                    assert.equal(outputLength, count);
                    new Uint8Array(memory().buffer, output, count).set(object.bytes.subarray(offset, offset + count));
                } else {
                    assert.equal(outputLength, 0);
                    object.bytes.set(new Uint8Array(memory().buffer, cursor, count), offset); cursor += count;
                }
                break;
            }
            case 12: collect(); assert.equal(outputLength, 0); break;
            default: assert.fail(`Unexpected managed heap command ${bytes.getUint32(address + 4, true)}`);
        }
        assert.equal(cursor, address + length, 'Complete managed heap packet');
        return 1;
    };
    const env = {
        continuation_state_store(id, address, count) {
            const active = continuations.get(id); assert.ok(active && !active.frame && count > 0);
            active.frame = new Uint8Array(memory().buffer, address, count).slice(); return 1;
        },
        continuation_state_read(id, address, count) {
            const active = continuations.get(id); assert.equal(active?.frame?.length, count);
            new Uint8Array(memory().buffer, address, count).set(active.frame); active.frame = null; return 1;
        },
        continuation_cancel() { assert.fail('Successful fixture must not cancel a continuation during registration'); },
    };
    const avidscript = {
        avid_managed_heap_v1: heap,
        avid_continuation_delay_cancel_resume_v1(seconds, callback) {
            assert.equal(seconds, 0); const active = continuation(callback); ready.push(active); return active.id;
        },
        avid_task_i32_v1(command, id, argument, reserved) {
            assert.equal(reserved, 0);
            switch (command) {
                case 1: { assert.equal(id, 0n); ++taskAllocations; const fresh = token(); tasks.set(fresh, { refs: 1, state: 0, value: 0, error: null, waiters: [] }); return fresh; }
                case 2: retain(id); return 1n;
                case 3: release(id); return 1n;
                case 4: {
                    const source = task(id); if (source.state !== 0) return 0n;
                    const waiter = continuation(argument, id); retain(id); source.waiters.push(waiter.id); return waiter.id;
                }
                case 5: complete(id, 1, argument); return 1n;
                case 7: { const value = task(id); assert.notEqual(value.state, 0); return (BigInt(value.state) << 32n) | BigInt(value.value >>> 0); }
                default: assert.fail(`Unexpected Task command ${command}`);
            }
        },
        avid_task_bind_producer_v1(id, target) {
            const active = continuations.get(target); assert.ok(active); retain(id); active.retained.push(id); return 1;
        },
        avid_task_retain_for_continuation_v1(id, target) {
            const active = continuations.get(target); assert.ok(active?.frame); retain(id); active.retained.push(id); return 1;
        },
        avid_task_fault_language_error_v1(id, type, source, root) {
            assert.ok(objects.has(root)); return complete(id, 2, 0, { type, source, root });
        },
        avid_task_cancel_language_error_v2(id, type, source, root, proof) {
            assert.equal(proof, 0n); assert.ok(fixture.cancel && objects.has(root));
            return complete(id, 3, 0, { type, source, root });
        },
        avid_task_terminal_error_meta_v1(id) { const error = heldError(id); return (BigInt(error.type) << 32n) | BigInt(error.source); },
        avid_task_terminal_error_root_v1(id) { return heldError(id).root; },
        avid_task_cancellation_token_v1(id) { assert.equal(task(id).state, 3); return 0n; },
        avid_task_propagate_failure_v1(source, target) {
            const value = task(source); return complete(target, value.state, 0, { ...heldError(source) });
        },
        avid_language_error_report_v1(type, source, root) {
            // Reporting must precede releasing the carrier, and the same object
            // must still carry the checked type code and a valid source token.
            assert.ok([...tasks.values()].some(value => value.refs > 0 && value.error?.root === root));
            const object = objects.get(root); assert.ok(object);
            assert.equal(new DataView(object.bytes.buffer).getInt32(0, true), type);
            assert.ok(ir.language_error_catalog.sources.some(entry => entry.token === source));
            reports.push({ type, source, root }); return rejectReport ? 0 : 1;
        },
    };
    for (const imported of WebAssembly.Module.imports(wasm))
        assert.ok(imported.name in ({ env, avidscript }[imported.module] ?? {}), `Unexpected import ${imported.module}/${imported.name}`);
    instance = await WebAssembly.instantiate(wasm, { env, avidscript });
    let trapped = false;
    try {
        instance.exports.avid_on_begin_play();
        for (let tick = 0; ready.length; ++tick) {
            assert.ok(tick < 256, 'Bounded scheduler');
            const active = ready.shift();
            instance.exports.avid_on_continuation_v2(active.callback, active.id,
                active.source ? 1 : fixture.cancel ? 3 : 1, 0, 0);
            assert.equal(active.frame, null, 'State consumed exactly once');
            if (active.source) release(active.source);
            for (const retained of active.retained) release(retained);
            continuations.delete(active.id); collect();
        }
    } catch (error) {
        if (!rejectReport || !(error instanceof WebAssembly.RuntimeError)) throw error;
        trapped = true;
    }
    assert.equal(trapped, rejectReport);
    assert.equal(view().getInt32(fixture.traceOffset, true), fixture.trace, fixture.name + ' finally order');
    assert.equal(reports.length, fixture.errorType ? 1 : 0, fixture.name + ' report count');
    if (fixture.errorType) {
        const type = ir.language_error_catalog.types.find(type => type.token === reports[0].type);
        assert.ok(type.type_id.endsWith(`.${fixture.errorType}`), `${fixture.name}: unexpected error ${type.type_id}`);
    }
    if (rejectReport) {
        assert.ok(tasks.size > 0, 'Rejected report did not release its error owner');
        // A trapped call needs host unwind; this probe does not claim UE teardown.
        return;
    }
    collect();
    if (fixture.name === 'direct-success') assert.equal(taskAllocations, 0, 'Successful async void Timer await allocated no Task');
    assert.equal(tasks.size, 0, fixture.name + ' Task owners');
    assert.equal(continuations.size, 0, fixture.name + ' continuations');
    assert.equal(frames.length, 0, fixture.name + ' managed frames');
    assert.equal(roots.size, 0, fixture.name + ' managed roots');
    assert.equal(objects.size, 0, fixture.name + ' error objects');
}
