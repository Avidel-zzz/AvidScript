'use strict';

const fs = require('node:fs');

if (process.argv.length !== 3) {
  throw new Error('Usage: node RunThrowProducerWasm.cjs <throw-producer.wasm>');
}

const wasmModule = new WebAssembly.Module(fs.readFileSync(process.argv[2]));
let instance;
let nextToken = 1n;
let allocations = 0;
const layouts = new Map(), objects = new Map(), frames = new Map(), roots = new Map();
let reported = false;
function collect() {
  const live = new Set([...roots.values()].filter(token => token !== 0n));
  for (const token of objects.keys()) {
    if (!live.has(token)) objects.delete(token);
  }
}
function managedHeap(input, inputLength, output, outputLength) {
  const view = new DataView(instance.exports.memory.buffer);
  const u32 = offset => view.getUint32(offset, true);
  const u64 = offset => view.getBigUint64(offset, true);
  const put64 = (offset, value) => view.setBigUint64(offset, value, true);
  const check = (valid, message) => {
    if (!valid) throw new Error(`Managed heap packet: ${message}`);
  };
  check(u32(input) === 0x3150484d, 'magic');
  const command = u32(input + 4);
  if (command === 1) {
    check(outputLength === 0 && inputLength >= 12 && layouts.size === 0, 'configure size');
    const count = u32(input + 8);
    let cursor = input + 12;
    for (let index = 0; index < count; index++) {
      check(cursor + 12 <= input + inputLength, 'layout header');
      const ordinal = u32(cursor), size = u32(cursor + 4), edges = u32(cursor + 8);
      check(ordinal > 0 && size > 0 && size <= 65536 && !layouts.has(ordinal), 'layout');
      cursor += 12 + edges * 8;
      check(cursor <= input + inputLength, 'layout edges');
      layouts.set(ordinal, size);
    }
    check(cursor === input + inputLength, 'configuration tail');
    return 1;
  }
  if (command === 2) {
    check(inputLength === 8 && outputLength === 8, 'push frame size');
    const token = nextToken++;
    frames.set(token, new Set());
    put64(output, token);
    return 1;
  }
  if (command === 3) {
    check(inputLength === 16 && outputLength === 0, 'pop frame size');
    const frame = u64(input + 8);
    check(frames.has(frame), 'unknown frame');
    for (const root of frames.get(frame)) roots.delete(root);
    frames.delete(frame);
    return 1;
  }
  if (command === 4) {
    check(inputLength === 24 && outputLength === 8, 'create root size');
    const frame = u64(input + 8), value = u64(input + 16);
    check(frames.has(frame), 'root frame');
    const token = nextToken++;
    frames.get(frame).add(token);
    roots.set(token, value);
    put64(output, token);
    return 1;
  }
  if (command === 5) {
    check(inputLength === 24 && outputLength === 0, 'set root size');
    const root = u64(input + 8);
    check(roots.has(root), 'unknown root');
    roots.set(root, u64(input + 16));
    return 1;
  }
  if (command === 7) {
    check(inputLength === 20 && outputLength === 8, 'allocate size');
    const ordinal = u32(input + 8), root = u64(input + 12);
    check(layouts.has(ordinal) && roots.has(root), 'allocation identity');
    const token = nextToken++;
    objects.set(token, { ordinal, bytes: new Uint8Array(layouts.get(ordinal)) });
    allocations++;
    roots.set(root, token);
    put64(output, token);
    return 1;
  }
  if (command === 8 || command === 9) {
    check(inputLength >= 28, 'field header');
    const object = objects.get(u64(input + 8));
    const ordinal = u32(input + 16), offset = u32(input + 20), count = u32(input + 24);
    check(object && (ordinal === 0 || object.ordinal === ordinal)
      && offset + count <= object.bytes.length, 'field bounds');
    check(command === 8 ? inputLength === 28 && outputLength === count
      : inputLength === 28 + count && outputLength === 0, 'field size');
    if (command === 8) {
      new Uint8Array(view.buffer, output, count).set(object.bytes.subarray(offset, offset + count));
    } else {
      object.bytes.set(new Uint8Array(view.buffer, input + 28, count), offset);
    }
    return 1;
  }
  if (command === 12) {
    check(inputLength === 8 && outputLength === 0, 'collect size');
    collect();
    return 1;
  }
  throw new Error(`Unexpected managed heap command: ${command}`);
}

const imports = {};
for (const entry of WebAssembly.Module.imports(wasmModule)) {
  if (entry.module === 'avidscript' && entry.name === 'avid_language_error_report_v1') {
    (imports[entry.module] ??= {})[entry.name] = (type, source, root) => {
      const activeRoots = [...frames.values()].at(-1);
      if (type !== 1 || source !== 1 || !objects.has(root)
        || !activeRoots || ![...activeRoots].some(token => roots.get(token) === root)) {
        throw new Error(`Invalid uncaught language-error report: type=${type} source=${source} object=${root}`);
      }
      reported = true;
      throw new Error('uncaught-language-error');
    };
    continue;
  }
  if (entry.module !== 'avidscript' || entry.name !== 'avid_managed_heap_v1') {
    throw new Error(`Unexpected import ${entry.module}.${entry.name}`);
  }
  (imports[entry.module] ??= {})[entry.name] = managedHeap;
}
instance = new WebAssembly.Instance(wasmModule, imports);
const multipleCatches = typeof instance.exports.catch_source_probe_a === 'function';
const finallyCatches = typeof instance.exports.finally_source_probe === 'function';
const nestedFinallyCatches = typeof instance.exports.nested_finally_source_probe === 'function';
const catches = multipleCatches || finallyCatches || nestedFinallyCatches
  || typeof instance.exports.catch_source_probe === 'function';
if (multipleCatches) {
  const first = instance.exports.catch_source_probe_a();
  const second = instance.exports.catch_source_probe_b();
  if (first !== 11 || second !== 22) {
    throw new Error(`Multi-producer catch results = ${first}, ${second}; expected 11, 22`);
  }
} else {
  const actual = nestedFinallyCatches
    ? instance.exports.nested_finally_source_probe()
    : finallyCatches ? instance.exports.finally_source_probe()
    : catches ? instance.exports.catch_source_probe() : instance.exports.throw_source_probe();
  const expected = nestedFinallyCatches ? 11 : finallyCatches ? 1 : catches ? 7 : 3;
  if (actual !== expected) throw new Error(`source probe() = ${actual}; expected ${expected}`);
}
if (allocations !== (multipleCatches ? 2 : 1) || frames.size !== 0 || roots.size !== 0) {
  throw new Error(`Root teardown mismatch: allocations=${allocations}, frames=${frames.size}, roots=${roots.size}`);
}
collect();
if (objects.size !== 0) throw new Error('Unrooted error object survived collection');
if (catches) {
  instance.exports.avid_on_begin_play();
  if (reported || frames.size !== 0 || roots.size !== 0) {
    throw new Error('A handled language error escaped or retained a frame root');
  }
  collect();
  if (objects.size !== 0) throw new Error('Handled error object survived collection');
  process.stdout.write(multipleCatches
    ? 'C# source multi-catch WASM: 3/3 passed\n'
    : nestedFinallyCatches ? 'C# source nested-finally-catch WASM: 2/2 passed\n'
    : finallyCatches ? 'C# source finally-catch WASM: 2/2 passed\n'
      : 'C# source catch WASM: 2/2 passed\n');
  process.exit(0);
}
if (instance.exports.avid_on_begin_play) {
  try {
    instance.exports.avid_on_begin_play();
    throw new Error('UE entry returned after an uncaught language error');
  } catch (error) {
    if (error.message !== 'uncaught-language-error' || !reported) throw error;
  }
  process.stdout.write('C# source throw producer WASM: 2/2 passed\n');
} else {
  process.stdout.write('C# source throw producer WASM: 1/1 passed\n');
}
