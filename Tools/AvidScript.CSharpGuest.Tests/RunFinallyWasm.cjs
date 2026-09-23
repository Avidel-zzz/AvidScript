const fs = require('node:fs');
const path = require('node:path');

const directory = process.argv[2];
if (!directory) throw new Error('Usage: node RunFinallyWasm.cjs <fixture-directory>');
const wasmModule = new WebAssembly.Module(
    fs.readFileSync(path.join(directory, 'finally-cleanup.wasm')));
if (WebAssembly.Module.imports(wasmModule).length !== 0) {
    throw new Error('The pure cleanup fixture must not require Host imports');
}
const instance = new WebAssembly.Instance(wasmModule);
const cases = [
    ['finally_normal', 7],
    ['finally_early_return', 11],
    ['finally_nested_return', 13],
    ['finally_cleanup_count', 21],
    ['finally_loop_exits', 42],
];
for (const [name, expected] of cases) {
    const actual = instance.exports[name]();
    if (actual !== expected) {
        throw new Error(`${name}: expected ${expected}, received ${actual}`);
    }
    console.log(`${name}: ${actual}`);
}
const lifecycle = new WebAssembly.Instance(wasmModule);
lifecycle.exports.avid_on_begin_play();
const state = new DataView(lifecycle.exports.memory.buffer);
// Static state is laid out in stable symbol order, not source declaration order.
const stateValues = [21, 21, 11, 42, 13, 7];
for (let index = 0; index < stateValues.length; ++index) {
    const actual = state.getInt32(16 + index * 4, true);
    if (actual !== stateValues[index]) {
        throw new Error(`avid_on_begin_play state ${index}: expected ${stateValues[index]}, received ${actual}`);
    }
}
console.log(`FinallyCleanupWasm: ${cases.length}/${cases.length} passed`);
