'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

assert.equal(process.argv.length, 3, 'Usage: node RunMemberErrorWasm.cjs <fixture-directory>');
const directory = process.argv[2];
const fixtures = JSON.parse(fs.readFileSync(path.join(directory, 'manifest.json'), 'utf8'));
assert.ok(fixtures.length > 0);
let passed = 0;

for (const fixture of fixtures) {
    const module = new WebAssembly.Module(fs.readFileSync(path.join(directory, fixture.name + '.wasm')));
    let instance, next = 1n, collections = 0;
    const layouts = new Map(), objects = new Map(), frames = new Map(), roots = new Map();
    function collect() {
        const live = new Set(), pending = [...roots.values()];
        while (pending.length) {
            const token = pending.pop();
            if (token === 0n || live.has(token)) continue;
            assert.ok(objects.has(token), 'a live root must reference a live object');
            live.add(token);
            const object = objects.get(token), layout = layouts.get(object.layout);
            const view = new DataView(object.bytes.buffer);
            for (const offset of layout.references) pending.push(view.getBigUint64(offset, true));
        }
        for (const token of objects.keys()) if (!live.has(token)) objects.delete(token);
        collections++;
    }
    function heap(input, inputBytes, output, outputBytes) {
        const memory = instance.exports.memory.buffer, view = new DataView(memory);
        const u32 = offset => view.getUint32(offset, true);
        const u64 = offset => view.getBigUint64(offset, true);
        const put64 = value => view.setBigUint64(output, value, true);
        assert.ok(input > 0 && inputBytes >= 8 && input + inputBytes <= memory.byteLength);
        assert.ok(outputBytes === 0 ? output === 0 : output > 0 && output + outputBytes <= memory.byteLength);
        assert.equal(u32(input), 0x3150484d);
        const command = u32(input + 4);
        if (command === 1) {
            assert.equal(layouts.size, 0);
            let cursor = input + 12;
            for (let index = 0; index < u32(input + 8); index++) {
                const id = u32(cursor), size = u32(cursor + 4), count = u32(cursor + 8);
                cursor += 12;
                assert.ok(id > 0 && size > 0 && !layouts.has(id));
                const references = [], targets = new Map();
                for (let edge = 0; edge < count; edge++, cursor += 8) {
                    const offset = u32(cursor);
                    assert.ok(offset + 8 <= size);
                    references.push(offset);
                    targets.set(offset, u32(cursor + 4));
                }
                layouts.set(id, { size, references, targets });
            }
            assert.equal(cursor, input + inputBytes);
        } else if (command === 2) {
            assert.equal(inputBytes, 8); assert.equal(outputBytes, 8);
            const token = next++;
            frames.set(token, new Set()); put64(token);
        } else if (command === 3) {
            const frame = u64(input + 8);
            assert.ok(frames.has(frame));
            for (const root of frames.get(frame)) roots.delete(root);
            frames.delete(frame);
        } else if (command === 4) {
            const frame = u64(input + 8), value = u64(input + 16);
            assert.ok(frames.has(frame));
            assert.ok(value === 0n || objects.has(value));
            const token = next++;
            frames.get(frame).add(token); roots.set(token, value); put64(token);
        } else if (command === 5) {
            const root = u64(input + 8), value = u64(input + 16);
            assert.ok(roots.has(root)); assert.ok(value === 0n || objects.has(value));
            roots.set(root, value);
        } else if (command === 6) {
            assert.ok(roots.delete(u64(input + 8)));
        } else if (command === 7) {
            // Collect at every allocation, including when an exception is
            // allocated while the receiver is reachable only through a local.
            collect();
            const layout = u32(input + 8), root = u64(input + 12);
            assert.ok(layouts.has(layout) && roots.has(root));
            const token = next++;
            objects.set(token, { layout, bytes: new Uint8Array(layouts.get(layout).size) });
            roots.set(root, token); put64(token);
        } else if (command === 8 || command === 9) {
            const object = objects.get(u64(input + 8));
            const layout = u32(input + 16), offset = u32(input + 20), size = u32(input + 24);
            assert.ok(object && (layout === 0 || layout === object.layout) && offset + size <= object.bytes.length);
            assert.ok(layouts.get(object.layout).references.every(edge => offset + size <= edge || offset >= edge + 8),
                'raw byte access must not overlap a reference field');
            if (command === 8) {
                assert.equal(inputBytes, 28); assert.equal(outputBytes, size);
                new Uint8Array(memory, output, size).set(object.bytes.subarray(offset, offset + size));
            } else {
                assert.equal(inputBytes, 28 + size); assert.equal(outputBytes, 0);
                object.bytes.set(new Uint8Array(memory, input + 28, size), offset);
            }
        } else if (command === 10 || command === 11) {
            const object = objects.get(u64(input + 8));
            const layout = u32(input + 16), offset = u32(input + 20);
            assert.ok(object && (layout === 0 || layout === object.layout));
            const descriptor = layouts.get(object.layout);
            assert.ok(descriptor.targets.has(offset));
            const storage = new DataView(object.bytes.buffer);
            if (command === 10) {
                assert.equal(inputBytes, 24); assert.equal(outputBytes, 8);
                put64(storage.getBigUint64(offset, true));
            } else {
                assert.equal(inputBytes, 32); assert.equal(outputBytes, 0);
                const value = u64(input + 24), target = descriptor.targets.get(offset);
                assert.ok(value === 0n || objects.has(value) && (target === 0 || objects.get(value).layout === target));
                storage.setBigUint64(offset, value, true);
            }
        } else if (command === 12) collect();
        else throw new Error('Unsupported heap command ' + command);
        return 1;
    }
    const imports = {};
    for (const entry of WebAssembly.Module.imports(module)) {
        assert.equal(entry.module, 'avidscript');
        const values = imports[entry.module] ??= {};
        if (entry.name === 'avid_managed_heap_v1') values[entry.name] = heap;
        else if (entry.name === 'avid_language_error_report_v1') values[entry.name] = () => { throw new Error('Unhandled language error'); };
        else throw new Error('Unexpected import ' + entry.name);
    }
    instance = new WebAssembly.Instance(module, imports);
    for (let repetition = 0; repetition < 3; repetition++) {
        for (const test of fixture.cases) {
            assert.equal(instance.exports.run(test.input), test.result, fixture.name + ' result');
            assert.equal(instance.exports.trace(), test.trace, fixture.name + ' order');
            collect();
            assert.equal(frames.size, 0, fixture.name + ' frames');
            assert.equal(roots.size, 0, fixture.name + ' roots');
            assert.equal(objects.size, 0, fixture.name + ' objects after collection');
            passed++;
        }
    }
    assert.ok(collections > fixture.cases.length * 3);
}
console.log(`AvidScript.ManagedFixtures.WasmExecution: ${passed}/${fixtures.reduce((total, fixture) => total + fixture.cases.length * 3, 0)} passed`);
