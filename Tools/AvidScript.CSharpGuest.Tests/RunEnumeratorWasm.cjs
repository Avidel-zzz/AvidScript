// Execute a source-defined IDisposable enumerator as real WASM.
const fs = require('node:fs');
const path = require('node:path');

const fixtureDirectory = process.argv[2];
if (!fixtureDirectory) {
    throw new Error('Usage: node RunEnumeratorWasm.cjs <fixture-directory>');
}

const wasmModule = new WebAssembly.Module(
    fs.readFileSync(path.join(fixtureDirectory, 'enumerator-cleanup.wasm')));
let instance;
let nextToken = 1n;
const layouts = new Map();
const objects = new Map();
const frames = new Map();
const roots = new Map();
function managedHeap(input, inputLength, output, outputLength) {
    const view = new DataView(instance.exports.memory.buffer);
    const u32 = at => view.getUint32(at, true);
    const u64 = at => view.getBigUint64(at, true);
    const put64 = (at, value) => view.setBigUint64(at, value, true);
    const requirePacket = (condition, reason) => {
        if (!condition) throw new Error(`Managed heap packet: ${reason}`);
    };
    requirePacket(u32(input) === 0x3150484d, 'wrong magic');
    const command = u32(input + 4);
    if (command === 1) {
        requirePacket(outputLength === 0 && inputLength >= 12 && layouts.size === 0,
            'invalid configuration');
        const count = u32(input + 8);
        let offset = input + 12;
        for (let i = 0; i < count; ++i) {
            requirePacket(offset + 12 <= input + inputLength, 'truncated layout');
            const ordinal = u32(offset), size = u32(offset + 4), edges = u32(offset + 8);
            requirePacket(ordinal > 0 && size > 0 && size <= 65536 && !layouts.has(ordinal),
                'invalid layout');
            offset += 12 + edges * 8;
            requirePacket(offset <= input + inputLength, 'truncated reference edges');
            layouts.set(ordinal, size);
        }
        requirePacket(offset === input + inputLength, 'trailing configuration bytes');
        return 1;
    }
    if (command === 2) {
        requirePacket(inputLength === 8 && outputLength === 8, 'push frame size');
        const token = nextToken++;
        frames.set(token, new Set());
        put64(output, token);
        return 1;
    }
    if (command === 3) {
        requirePacket(inputLength === 16 && outputLength === 0, 'pop frame size');
        const frame = u64(input + 8);
        requirePacket(frames.has(frame), 'unknown frame');
        for (const root of frames.get(frame)) roots.delete(root);
        frames.delete(frame);
        return 1;
    }
    if (command === 4) {
        requirePacket(inputLength === 24 && outputLength === 8, 'create root size');
        const frame = u64(input + 8), value = u64(input + 16);
        requirePacket(frames.has(frame), 'unknown root frame');
        const token = nextToken++;
        frames.get(frame).add(token);
        roots.set(token, value);
        put64(output, token);
        return 1;
    }
    if (command === 5) {
        requirePacket(inputLength === 24 && outputLength === 0, 'set root size');
        const root = u64(input + 8);
        requirePacket(roots.has(root), 'unknown root');
        roots.set(root, u64(input + 16));
        return 1;
    }
    if (command === 7) {
        requirePacket(inputLength === 20 && outputLength === 8, 'allocate size');
        const ordinal = u32(input + 8), root = u64(input + 12);
        requirePacket(layouts.has(ordinal) && roots.has(root), 'invalid allocation');
        const token = nextToken++;
        objects.set(token, { ordinal, bytes: new Uint8Array(layouts.get(ordinal)) });
        roots.set(root, token);
        put64(output, token);
        return 1;
    }
    if (command === 8 || command === 9) {
        requirePacket(inputLength >= 28, 'field packet size');
        const object = objects.get(u64(input + 8));
        const ordinal = u32(input + 16), offset = u32(input + 20), count = u32(input + 24);
        requirePacket(object && (ordinal === 0 || object.ordinal === ordinal)
            && offset + count <= object.bytes.length,
            `field bounds or type: token=${u64(input + 8)} ordinal=${ordinal} offset=${offset} count=${count} object=${object?.ordinal}/${object?.bytes.length}`);
        requirePacket(command === 8 ? inputLength === 28 && outputLength === count
            : inputLength === 28 + count && outputLength === 0, 'field byte count');
        if (command === 8)
            new Uint8Array(view.buffer, output, count).set(object.bytes.subarray(offset, offset + count));
        else
            object.bytes.set(new Uint8Array(view.buffer, input + 28, count), offset);
        return 1;
    }
    throw new Error(`Unexpected managed heap command: ${command}`);
}
const imports = {};
for (const item of WebAssembly.Module.imports(wasmModule)) {
    (imports[item.module] ??= {})[item.name] = item.module === 'avidscript'
        && item.name === 'avid_managed_heap_v1' ? managedHeap : () => {
            throw new Error(`Unexpected host call: ${item.module}.${item.name}`);
        };
}
instance = new WebAssembly.Instance(wasmModule, imports);
const cases = [
    ['enumerator_normal', 12],
    ['enumerator_break_continue', 2],
    ['enumerator_early_return', 22],
    ['enumerator_counts', 33],
];
for (const [name, expected] of cases) {
    const actual = instance.exports[name]();
    if (actual !== expected) {
        throw new Error(`${name}: expected ${expected}, received ${actual}`);
    }
    if (frames.size !== 0 || roots.size !== 0) {
        throw new Error(`${name}: managed heap frame or root leaked across the export`);
    }
    console.log(`${name}: ${actual}`);
}
instance.exports.avid_on_begin_play();
const state = new DataView(instance.exports.memory.buffer);
const stateValues = Array.from({ length: 6 }, (_, index) => state.getInt32(16 + index * 4, true));
const expectedState = [6, 6, 2, 66, 52, 12];
if (stateValues.some((value, index) => value !== expectedState[index])) {
    throw new Error(`avid_on_begin_play: expected ${expectedState}, received ${stateValues}`);
}
if (frames.size !== 0 || roots.size !== 0) {
    throw new Error('avid_on_begin_play leaked managed heap frames or roots');
}
if (layouts.size !== 2 || objects.size < 6) {
    throw new Error('Enumerator class allocations did not exercise two heap layouts');
}
console.log(`EnumeratorCleanupWasm: ${cases.length}/${cases.length} passed`);
