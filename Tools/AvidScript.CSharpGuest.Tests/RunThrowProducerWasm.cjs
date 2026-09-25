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
  if (entry.module === 'avidscript' && entry.name === 'avid_ue_receiver_0_require_v1') {
    (imports[entry.module] ??= {})[entry.name] = handle => {
      if (handle !== 123n) throw new Error(`Unexpected generated receiver: ${handle}`);
      return 1;
    };
    continue;
  }
  if (entry.module !== 'avidscript' || entry.name !== 'avid_managed_heap_v1') {
    throw new Error(`Unexpected import ${entry.module}.${entry.name}`);
  }
  (imports[entry.module] ??= {})[entry.name] = managedHeap;
}
instance = new WebAssembly.Instance(wasmModule, imports);
if (typeof instance.exports.avid_void_guard_entry === 'function') {
  const handled = instance.exports.avid_void_guard_entry;
  const uncaught = instance.exports.avid_void_guard_uncaught_entry;
  const beginPlay = instance.exports.avid_on_begin_play;
  if (typeof uncaught !== 'function' || typeof beginPlay !== 'function') {
    throw new Error('Conditional void guard is missing its public entries');
  }
  const normal = handled(5), recovered = handled(-1), direct = uncaught(5);
  beginPlay();
  collect();
  if (normal !== 7 || recovered !== 19 || direct !== undefined || allocations !== 2
    || frames.size !== 0 || roots.size !== 0 || objects.size !== 0) {
    throw new Error(`Conditional void guard results/cleanup = ${normal}/${recovered}/${direct}/${allocations}/${frames.size}/${roots.size}/${objects.size}`);
  }
  try {
    uncaught(-1);
    throw new Error('Conditional void guard returned after an uncaught language error');
  } catch (error) {
    if (error.message !== 'uncaught-language-error' || !reported || allocations !== 3) throw error;
  }
  process.stdout.write('C# conditional void guard WASM: 5/5 passed\n');
  process.exit(0);
}
if (typeof instance.exports.avid_void_handled_entry === 'function') {
  const handled = instance.exports.avid_void_handled_entry;
  const uncaught = instance.exports.avid_void_uncaught_entry;
  const beginPlay = instance.exports.avid_on_begin_play;
  if (typeof uncaught !== 'function' || typeof beginPlay !== 'function') {
    throw new Error('Void throw producer is missing its public entries');
  }
  const normal = handled(5), recovered = handled(-1);
  beginPlay();
  collect();
  if (normal !== 7 || recovered !== 19 || allocations !== 2
    || frames.size !== 0 || roots.size !== 0 || objects.size !== 0) {
    throw new Error(`Void throw producer results/cleanup = ${normal}/${recovered}/${allocations}/${frames.size}/${roots.size}/${objects.size}`);
  }
  try {
    uncaught();
    throw new Error('Void throw producer returned after an uncaught language error');
  } catch (error) {
    if (error.message !== 'uncaught-language-error' || !reported || allocations !== 3) throw error;
  }
  process.stdout.write('C# void throw producer WASM: 5/5 passed\n');
  process.exit(0);
}
if (typeof instance.exports.avid_guarded_entry === 'function') {
  const handled = instance.exports.avid_guarded_entry;
  const uncaught = instance.exports.avid_guarded_uncaught_entry;
  const beginPlay = instance.exports.avid_on_begin_play;
  if (typeof uncaught !== 'function' || typeof beginPlay !== 'function') {
    throw new Error('Conditional guard is missing its public entries');
  }
  const normal = handled(5), recovered = handled(-1), direct = uncaught(5);
  beginPlay();
  collect();
  if (normal !== 7 || recovered !== 19 || direct !== 7
    || frames.size !== 0 || roots.size !== 0 || objects.size !== 0 || allocations !== 2) {
    throw new Error(`Conditional guard results/cleanup = ${normal}/${recovered}/${direct}/${frames.size}/${roots.size}/${objects.size}/${allocations}`);
  }
  try {
    uncaught(-1);
    throw new Error('Conditional guard returned after an uncaught language error');
  } catch (error) {
    if (error.message !== 'uncaught-language-error' || !reported || allocations !== 3) throw error;
  }
  process.stdout.write('C# parameterized conditional guard WASM: 5/5 passed\n');
  process.exit(0);
}
const generatedFunctions = Object.keys(instance.exports).filter(name => name.startsWith('avid_ue_'));
if (generatedFunctions.length !== 0) {
  if (generatedFunctions.length !== 1) {
    throw new Error(`Expected one generated UFunction export, got ${generatedFunctions.length}`);
  }
  const entry = instance.exports[generatedFunctions[0]];
  const value = entry(123n, 5);
  if (value !== 7 || frames.size !== 0 || roots.size !== 0 || objects.size !== 0) {
    throw new Error(`Generated UFunction result/cleanup = ${value}/${frames.size}/${roots.size}/${objects.size}; expected 7/0/0/0`);
  }
  try {
    entry(123n, -1);
    throw new Error('Generated UFunction returned after an uncaught language error');
  } catch (error) {
    if (error.message !== 'uncaught-language-error' || !reported || allocations !== 1) throw error;
  }
  process.stdout.write('C# source generated UFunction language boundary WASM: 4/4 passed\n');
  process.exit(0);
}
if (typeof instance.exports.avid_language_value_entry === 'function') {
  const value = instance.exports.avid_language_value_entry(5);
  if (value !== 7 || frames.size !== 0 || roots.size !== 0 || objects.size !== 0) {
    throw new Error(`Value entry result/cleanup = ${value}/${frames.size}/${roots.size}/${objects.size}; expected 7/0/0/0`);
  }
  try {
    instance.exports.avid_language_value_entry(-1);
    throw new Error('Value entry returned after an uncaught language error');
  } catch (error) {
    if (error.message !== 'uncaught-language-error' || !reported || allocations !== 1) throw error;
  }
  process.stdout.write('C# source return-value UE boundary WASM: 4/4 passed\n');
  process.exit(0);
}
const multipleCatches = typeof instance.exports.catch_source_probe_a === 'function';
const typedCatches = typeof instance.exports.typed_invalid_probe === 'function';
const finallyCatches = typeof instance.exports.finally_source_probe === 'function';
const nestedFinallyCatches = typeof instance.exports.nested_finally_source_probe === 'function';
const throwFinallyCatches = typeof instance.exports.throw_finally_source_probe === 'function';
const nestedLocalThrowCatches = typeof instance.exports.nested_local_throw_catch_probe === 'function';
const multiLocalThrowCatches = typeof instance.exports.multi_local_cleanup_first_probe === 'function';
const sideEffectCatches = typeof instance.exports.side_effect_first_probe === 'function';
const mixedCleanupCatches = typeof instance.exports.mixed_cleanup_normal_first_probe === 'function';
const mixedBranchCatches = typeof instance.exports.mixed_branch_normal_first_probe === 'function';
const calledReturnCatches = typeof instance.exports.called_return_normal_probe === 'function';
const calledBranchCatches = typeof instance.exports.called_branch_normal_probe === 'function';
const catchFinallyCatches = typeof instance.exports.catch_finally_normal_probe === 'function';
const catchBranchCatches = typeof instance.exports.catch_branch_normal_first_probe === 'function';
const rethrowFinallyCatches = typeof instance.exports.rethrow_finally_normal_probe === 'function';
const rethrowBranchCatches = typeof instance.exports.rethrow_branch_normal_first_probe === 'function';
const localRethrowFinallyCatches = typeof instance.exports.local_rethrow_finally_catch_probe === 'function';
const localRethrowBranchCatches = typeof instance.exports.local_rethrow_branch_first_probe === 'function';
const replacementCatches = typeof instance.exports.replacement_catch_probe === 'function';
const nestedReplacementCatches = typeof instance.exports.nested_replacement_catch_probe === 'function';
const outerNestedReplacementCatches = typeof instance.exports.outer_nested_replacement_catch_probe === 'function';
const outermostReplacementCatches = typeof instance.exports.outermost_replacement_catch_probe === 'function';
const consecutiveReplacementCatches = typeof instance.exports.consecutive_replacement_catch_probe === 'function';
const branchCleanupCatches = typeof instance.exports.branch_cleanup_first_probe === 'function';
const rethrowCatches = typeof instance.exports.rethrow_local_source_probe === 'function';
const nestedRethrowCatches = typeof instance.exports.nested_rethrow_source_probe === 'function';
const nestedCatchCleanupCatches = typeof instance.exports.nested_catch_cleanup_normal_first_probe === 'function';
const catchVariableCatches = typeof instance.exports.catch_variable_call_probe === 'function';
const catches = typedCatches || multipleCatches || finallyCatches || nestedFinallyCatches || throwFinallyCatches
  || nestedLocalThrowCatches || multiLocalThrowCatches || sideEffectCatches
  || mixedCleanupCatches || mixedBranchCatches || calledReturnCatches
  || calledBranchCatches
  || catchFinallyCatches || catchBranchCatches || rethrowFinallyCatches
  || rethrowBranchCatches || localRethrowFinallyCatches || localRethrowBranchCatches
  || replacementCatches || nestedReplacementCatches || outerNestedReplacementCatches
  || outermostReplacementCatches || consecutiveReplacementCatches
  || branchCleanupCatches || rethrowCatches
  || nestedRethrowCatches || nestedCatchCleanupCatches || catchVariableCatches
  || typeof instance.exports.catch_source_probe === 'function';
if (typedCatches) {
  const invalid = instance.exports.typed_invalid_probe();
  const argument = instance.exports.typed_argument_probe();
  const base = instance.exports.typed_base_probe();
  if (invalid !== 11 || argument !== 22 || base !== 33) {
    throw new Error(`Typed catch results = ${invalid}, ${argument}, ${base}; expected 11, 22, 33`);
  }
} else if (multipleCatches) {
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
} else if (outerNestedReplacementCatches) {
  const handled = instance.exports.outer_nested_replacement_catch_probe();
  const source = instance.exports.outer_nested_replacement_source_probe();
  if (source !== 4 || handled !== 111) {
    throw new Error(`Outer nested cleanup replacement results = ${source}, ${handled}; expected 4, 111`);
  }
} else if (outermostReplacementCatches) {
  const handled = instance.exports.outermost_replacement_catch_probe();
  const source = instance.exports.outermost_replacement_source_probe();
  if (source !== 4 || handled !== 11) {
    throw new Error(`Outermost cleanup replacement results = ${source}, ${handled}; expected 4, 11`);
  }
} else if (consecutiveReplacementCatches) {
  const handled = instance.exports.consecutive_replacement_catch_probe();
  const source = instance.exports.consecutive_replacement_source_probe();
  if (source !== 5 || handled !== 11) {
    throw new Error(`Consecutive cleanup replacement results = ${source}, ${handled}; expected 5, 11`);
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
} else if (sideEffectCatches) {
  const first = instance.exports.side_effect_first_probe();
  const second = instance.exports.side_effect_second_probe();
  if (first !== 11 || second !== 21) {
    throw new Error(`Side-effecting throw decisions = ${first}, ${second}; expected 11, 21`);
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
} else if (mixedBranchCatches) {
  const values = [
    instance.exports.mixed_branch_normal_first_probe(),
    instance.exports.mixed_branch_error_first_probe(),
    instance.exports.mixed_branch_normal_second_probe(),
    instance.exports.mixed_branch_error_second_probe(),
    instance.exports.mixed_branch_called_error_probe(),
    instance.exports.mixed_branch_source_first_probe(),
    instance.exports.mixed_branch_source_second_probe(),
    instance.exports.mixed_branch_source_called_probe(),
  ];
  const expected = [1107, 21, 1227, 32, 42, 4, 5, 3];
  if (values.some((value, index) => value !== expected[index])) {
    throw new Error(`Mixed branching cleanup results = ${values}; expected ${expected}`);
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
} else if (calledBranchCatches) {
  const values = [
    instance.exports.called_branch_normal_probe(),
    instance.exports.called_branch_first_probe(),
    instance.exports.called_branch_second_probe(),
    instance.exports.called_branch_no_else_taken_probe(),
    instance.exports.called_branch_no_else_skipped_probe(),
    instance.exports.called_branch_nested_normal_probe(),
    instance.exports.called_branch_nested_first_probe(),
    instance.exports.called_branch_nested_second_probe(),
    instance.exports.called_branch_source_probe(),
  ];
  const expected = [1107, 11, 12, 1, 0, 10107, 101, 102, 3];
  if (values.some((value, index) => value !== expected[index])) {
    throw new Error(`Called branching cleanup results = ${values}; expected ${expected}`);
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
} else if (catchBranchCatches) {
  const values = [
    instance.exports.catch_branch_normal_first_probe(),
    instance.exports.catch_branch_normal_second_probe(),
    instance.exports.catch_branch_handled_first_probe(),
    instance.exports.catch_branch_handled_second_probe(),
    instance.exports.catch_branch_escaped_first_probe(),
    instance.exports.catch_branch_escaped_second_probe(),
    instance.exports.catch_branch_source_first_probe(),
    instance.exports.catch_branch_source_second_probe(),
  ];
  const expected = [1105, 1205, 1107, 1207, 31, 32, 4, 4];
  if (values.some((value, index) => value !== expected[index])) {
    throw new Error(`Catch branching cleanup results = ${values}; expected ${expected}`);
  }
} else if (rethrowFinallyCatches) {
  const normal = instance.exports.rethrow_finally_normal_probe();
  const handled = instance.exports.rethrow_finally_error_probe();
  const source = instance.exports.rethrow_finally_source_probe();
  if (normal !== 1007 || handled !== 10 || source !== 3) {
    throw new Error(`Rethrow cleanup results = ${normal}, ${handled}, ${source}; expected 1007, 10, 3`);
  }
} else if (rethrowBranchCatches) {
  const values = [
    instance.exports.rethrow_branch_normal_first_probe(),
    instance.exports.rethrow_branch_normal_second_probe(),
    instance.exports.rethrow_branch_error_first_probe(),
    instance.exports.rethrow_branch_error_second_probe(),
    instance.exports.rethrow_branch_source_probe(),
  ];
  const expected = [1107, 1207, 11, 12, 3];
  if (values.some((value, index) => value !== expected[index])) {
    throw new Error(`Branching rethrow cleanup results = ${values}; expected ${expected}`);
  }
} else if (localRethrowFinallyCatches) {
  const handled = instance.exports.local_rethrow_finally_catch_probe();
  const source = instance.exports.local_rethrow_finally_source_probe();
  if (handled !== 10 || source !== 3) {
    throw new Error(`Local rethrow cleanup results = ${handled}, ${source}; expected 10, 3`);
  }
} else if (localRethrowBranchCatches) {
  const first = instance.exports.local_rethrow_branch_first_probe();
  const second = instance.exports.local_rethrow_branch_second_probe();
  const source = instance.exports.local_rethrow_branch_source_probe();
  if (first !== 11 || second !== 12 || source !== 3) {
    throw new Error(`Local branching rethrow cleanup results = ${first}, ${second}, ${source}; expected 11, 12, 3`);
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
} else if (nestedCatchCleanupCatches) {
  const normalFirst = instance.exports.nested_catch_cleanup_normal_first_probe();
  const normalSecond = instance.exports.nested_catch_cleanup_normal_second_probe();
  const handledFirst = instance.exports.nested_catch_cleanup_handled_first_probe();
  const handledSecond = instance.exports.nested_catch_cleanup_handled_second_probe();
  if (normalFirst !== 1107 || normalSecond !== 1207
      || handledFirst !== 1105 || handledSecond !== 1205) {
    throw new Error(`Nested catch cleanup results = ${normalFirst}, ${normalSecond}, ${handledFirst}, ${handledSecond}; expected 1107, 1207, 1105, 1205`);
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
if (allocations !== (consecutiveReplacementCatches ? 6
  : typedCatches ? 3
  : rethrowCatches || replacementCatches || nestedReplacementCatches
  || outerNestedReplacementCatches || outermostReplacementCatches ? 4
  : branchCleanupCatches || rethrowBranchCatches || localRethrowBranchCatches
    ? 3
  : nestedCatchCleanupCatches ? 2
  : calledBranchCatches ? 7
  : catchFinallyCatches ? 5
  : catchBranchCatches ? 10
  : mixedBranchCatches || multiLocalThrowCatches ? 6
  : nestedRethrowCatches || catchVariableCatches || mixedCleanupCatches || calledReturnCatches ? 4
  : rethrowFinallyCatches || localRethrowFinallyCatches || multipleCatches
    || sideEffectCatches
    || nestedLocalThrowCatches ? 2 : 1)
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
  process.stdout.write(typedCatches ? 'C# source typed-catch WASM: 4/4 passed\n'
    : multipleCatches
    ? 'C# source multi-catch WASM: 3/3 passed\n'
    : rethrowCatches ? 'C# source catch-rethrow WASM: 5/5 passed\n'
    : nestedRethrowCatches ? 'C# source nested-rethrow WASM: 5/5 passed\n'
    : nestedCatchCleanupCatches ? 'C# source nested-catch-branching-finally WASM: 7/7 passed\n'
    : catchVariableCatches ? 'C# source catch-variable WASM: 5/5 passed\n'
    : nestedLocalThrowCatches ? 'C# source nested-local-throw-finally WASM: 3/3 passed\n'
    : multiLocalThrowCatches ? 'C# source multi-local-throw-finally WASM: 7/7 passed\n'
    : sideEffectCatches ? 'C# source side-effect-throw-finally WASM: 2/2 passed\n'
    : mixedCleanupCatches ? 'C# source mixed-local-throw-finally WASM: 7/7 passed\n'
    : mixedBranchCatches ? 'C# source mixed-branching-finally WASM: 9/9 passed\n'
    : calledReturnCatches ? 'C# source called-return-finally WASM: 6/6 passed\n'
    : calledBranchCatches ? 'C# source called-branching-finally WASM: 10/10 passed\n'
    : catchFinallyCatches ? 'C# source catch-finally WASM: 5/5 passed\n'
    : catchBranchCatches ? 'C# source catch-branching-finally WASM: 9/9 passed\n'
    : rethrowFinallyCatches ? 'C# source catch-rethrow-finally WASM: 4/4 passed\n'
    : rethrowBranchCatches ? 'C# source catch-rethrow-branching-finally WASM: 6/6 passed\n'
    : localRethrowFinallyCatches ? 'C# source local-catch-rethrow-finally WASM: 3/3 passed\n'
    : localRethrowBranchCatches ? 'C# source local-catch-rethrow-branching-finally WASM: 4/4 passed\n'
    : replacementCatches ? 'C# source cleanup-replaces-error WASM: 3/3 passed\n'
    : nestedReplacementCatches ? 'C# source nested-cleanup-replaces-error WASM: 3/3 passed\n'
    : outerNestedReplacementCatches ? 'C# source outer-nested-cleanup-replaces-error WASM: 3/3 passed\n'
    : outermostReplacementCatches ? 'C# source outermost-cleanup-replaces-error WASM: 3/3 passed\n'
    : consecutiveReplacementCatches ? 'C# source consecutive-nested-cleanup-replaces-error WASM: 3/3 passed\n'
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
