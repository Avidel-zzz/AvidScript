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
const throwFinallyCatches = typeof instance.exports.throw_finally_source_probe === 'function';
const nestedLocalThrowCatches = typeof instance.exports.nested_local_throw_catch_probe === 'function';
const multiLocalThrowCatches = typeof instance.exports.multi_local_cleanup_first_probe === 'function';
const mixedCleanupCatches = typeof instance.exports.mixed_cleanup_normal_first_probe === 'function';
const calledReturnCatches = typeof instance.exports.called_return_normal_probe === 'function';
const catchFinallyCatches = typeof instance.exports.catch_finally_normal_probe === 'function';
const replacementCatches = typeof instance.exports.replacement_catch_probe === 'function';
const nestedReplacementCatches = typeof instance.exports.nested_replacement_catch_probe === 'function';
const branchCleanupCatches = typeof instance.exports.branch_cleanup_first_probe === 'function';
const rethrowCatches = typeof instance.exports.rethrow_local_source_probe === 'function';
const nestedRethrowCatches = typeof instance.exports.nested_rethrow_source_probe === 'function';
const catchVariableCatches = typeof instance.exports.catch_variable_call_probe === 'function';
const catches = multipleCatches || finallyCatches || nestedFinallyCatches || throwFinallyCatches
  || nestedLocalThrowCatches || multiLocalThrowCatches || mixedCleanupCatches || calledReturnCatches
  || catchFinallyCatches
  || replacementCatches || nestedReplacementCatches || branchCleanupCatches || rethrowCatches
  || nestedRethrowCatches || catchVariableCatches
  || typeof instance.exports.catch_source_probe === 'function';
if (multipleCatches) {
  const first = instance.exports.catch_source_probe_a();
  const second = instance.exports.catch_source_probe_b();
  if (first !== 11 || second !== 22) {
    throw new Error(`Multi-producer catch results = ${first}, ${second}; expected 11, 22`);
  }
} else if (replacementCatches) {
  const handled = instance.exports.replacement_catch_probe();
  const source = instance.exports.throw_source_probe();
  if (source !== 4 || handled !== 1) {
    throw new Error(`Cleanup replacement results = ${source}, ${handled}; expected 4, 1`);
  }
} else if (nestedReplacementCatches) {
  const handled = instance.exports.nested_replacement_catch_probe();
  const source = instance.exports.nested_replacement_source_probe();
  if (source !== 4 || handled !== 11) {
    throw new Error(`Nested cleanup replacement results = ${source}, ${handled}; expected 4, 11`);
  }
} else if (branchCleanupCatches) {
  const first = instance.exports.branch_cleanup_first_probe();
  const second = instance.exports.branch_cleanup_second_probe();
  const source = instance.exports.branch_cleanup_source_probe();
  if (first !== 11 || second !== 12 || source !== 3) {
    throw new Error(`Branching cleanup results = ${first}, ${second}, ${source}; expected 11, 12, 3`);
  }
} else if (nestedLocalThrowCatches) {
  const handled = instance.exports.nested_local_throw_catch_probe();
  const source = instance.exports.nested_local_throw_source_probe();
  if (handled !== 11 || source !== 3) {
    throw new Error(`Nested local throw results = ${handled}, ${source}; expected 11, 3`);
  }
} else if (multiLocalThrowCatches) {
  const first = instance.exports.multi_local_cleanup_first_probe();
  const firstSource = instance.exports.multi_local_cleanup_source_probe();
  const second = instance.exports.multi_local_cleanup_second_probe();
  const secondSource = instance.exports.multi_local_cleanup_source_probe();
  const third = instance.exports.multi_local_cleanup_third_probe();
  const thirdSource = instance.exports.multi_local_cleanup_source_probe();
  if (first !== 11 || firstSource !== 3 || second !== 21 || secondSource !== 4
    || third !== 31 || thirdSource !== 5) {
    throw new Error(`Shared cleanup results = ${first}, ${firstSource}, ${second}, ${secondSource}, ${third}, ${thirdSource}; expected 11, 3, 21, 4, 31, 5`);
  }
} else if (mixedCleanupCatches) {
  const values = [
    instance.exports.mixed_cleanup_normal_first_probe(),
    instance.exports.mixed_cleanup_error_first_probe(),
    instance.exports.mixed_cleanup_normal_second_probe(),
    instance.exports.mixed_cleanup_error_second_probe(),
    instance.exports.mixed_cleanup_source_first_probe(),
    instance.exports.mixed_cleanup_source_second_probe(),
  ];
  const expected = [107, 11, 120, 21, 3, 4];
  if (values.some((value, index) => value !== expected[index])) {
    throw new Error(`Mixed cleanup results = ${values}; expected ${expected}`);
  }
} else if (calledReturnCatches) {
  const values = [
    instance.exports.called_return_normal_probe(),
    instance.exports.called_return_called_error_probe(),
    instance.exports.called_return_local_error_probe(),
    instance.exports.called_return_source_called_probe(),
    instance.exports.called_return_source_local_probe(),
  ];
  const expected = [114, 11, 21, 3, 4];
  if (values.some((value, index) => value !== expected[index])) {
    throw new Error(`Called-return cleanup results = ${values}; expected ${expected}`);
  }
} else if (catchFinallyCatches) {
  const values = [
    instance.exports.catch_finally_normal_probe(),
    instance.exports.catch_finally_handled_probe(),
    instance.exports.catch_finally_escaped_probe(),
    instance.exports.catch_finally_source_probe(),
  ];
  const expected = [105, 107, 21, 4];
  if (values.some((value, index) => value !== expected[index])) {
    throw new Error(`Catch-finally results = ${values}; expected ${expected}`);
  }
} else if (rethrowCatches) {
  const localSource = instance.exports.rethrow_local_source_probe();
  const calledSource = instance.exports.rethrow_call_source_probe();
  const localHandled = instance.exports.rethrow_local_catch_probe();
  const calledHandled = instance.exports.rethrow_call_catch_probe();
  if (localSource !== 4 || calledSource !== 3 || localHandled !== 7 || calledHandled !== 9) {
    throw new Error(`Rethrow results = ${localSource}, ${calledSource}, ${localHandled}, ${calledHandled}; expected 4, 3, 7, 9`);
  }
} else if (nestedRethrowCatches) {
  const source = instance.exports.nested_rethrow_source_probe();
  const local = instance.exports.nested_rethrow_local_probe();
  const escaped = instance.exports.nested_rethrow_escape_probe();
  const mismatch = instance.exports.nested_rethrow_mismatch_probe();
  if (source !== 3 || local !== 3 || escaped !== 5 || mismatch !== 6) {
    throw new Error(`Nested rethrow results = ${source}, ${local}, ${escaped}, ${mismatch}; expected 3, 3, 5, 6`);
  }
} else if (catchVariableCatches) {
  const called = instance.exports.catch_variable_call_probe();
  const local = instance.exports.catch_variable_local_probe();
  const unused = instance.exports.catch_variable_unused_probe();
  const rethrown = instance.exports.catch_variable_rethrow_probe();
  if (called !== 7 || local !== 9 || unused !== 11 || rethrown !== 13) {
    throw new Error(`Catch variable results = ${called}, ${local}, ${unused}, ${rethrown}; expected 7, 9, 11, 13`);
  }
} else {
  const actual = nestedFinallyCatches
    ? instance.exports.nested_finally_source_probe()
    : throwFinallyCatches ? instance.exports.throw_finally_source_probe()
    : finallyCatches ? instance.exports.finally_source_probe()
    : catches ? instance.exports.catch_source_probe() : instance.exports.throw_source_probe();
  const expected = nestedFinallyCatches ? 11
    : throwFinallyCatches || finallyCatches ? 1 : catches ? 7 : 3;
  if (actual !== expected) throw new Error(`source probe() = ${actual}; expected ${expected}`);
}
if (allocations !== (rethrowCatches || replacementCatches || nestedReplacementCatches ? 4
  : branchCleanupCatches ? 3
  : catchFinallyCatches ? 5
  : nestedRethrowCatches || catchVariableCatches || mixedCleanupCatches || calledReturnCatches ? 4
  : multiLocalThrowCatches ? 6
  : multipleCatches || nestedLocalThrowCatches ? 2 : 1)
  || frames.size !== 0 || roots.size !== 0) {
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
    : rethrowCatches ? 'C# source catch-rethrow WASM: 5/5 passed\n'
    : nestedRethrowCatches ? 'C# source nested-rethrow WASM: 5/5 passed\n'
    : catchVariableCatches ? 'C# source catch-variable WASM: 5/5 passed\n'
    : nestedLocalThrowCatches ? 'C# source nested-local-throw-finally WASM: 3/3 passed\n'
    : multiLocalThrowCatches ? 'C# source multi-local-throw-finally WASM: 7/7 passed\n'
    : mixedCleanupCatches ? 'C# source mixed-local-throw-finally WASM: 7/7 passed\n'
    : calledReturnCatches ? 'C# source called-return-finally WASM: 6/6 passed\n'
    : catchFinallyCatches ? 'C# source catch-finally WASM: 5/5 passed\n'
    : replacementCatches ? 'C# source cleanup-replaces-error WASM: 3/3 passed\n'
    : nestedReplacementCatches ? 'C# source nested-cleanup-replaces-error WASM: 3/3 passed\n'
    : branchCleanupCatches ? 'C# source branching-cleanup WASM: 3/3 passed\n'
    : nestedFinallyCatches ? 'C# source nested-finally-catch WASM: 2/2 passed\n'
    : throwFinallyCatches ? 'C# source throw-finally-catch WASM: 2/2 passed\n'
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
