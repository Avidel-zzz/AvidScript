// Execute the C# generic-method fixture as real WASM.
const fs = require('node:fs');
const path = require('node:path');

const fixtureDirectory = process.argv[2];
if (!fixtureDirectory) {
    throw new Error('Usage: node RunGenericMethodsWasm.cjs <fixture-directory>');
}

const wasmModule = new WebAssembly.Module(
    fs.readFileSync(path.join(fixtureDirectory, 'generic-methods.wasm')));
const imports = {};
for (const item of WebAssembly.Module.imports(wasmModule)) {
    (imports[item.module] ??= {})[item.name] = () => {
        throw new Error(`Unexpected host call: ${item.module}.${item.name}`);
    };
}
const instance = new WebAssembly.Instance(wasmModule, imports);
const cases = [
    ['generic_int', 7],
    ['generic_float', 2.5],
    ['generic_forward', 3],
    ['generic_bounce', 4],
    ['generic_pair', 12],
    ['generic_ref_out', 57],
    ['generic_array', 5],
];
for (const [name, expected] of cases) {
    const actual = instance.exports[name]();
    if (actual !== expected) {
        throw new Error(`${name}: expected ${expected}, received ${actual}`);
    }
    console.log(`${name}: ${actual}`);
}
console.log(`GenericMethodsWasm: ${cases.length}/${cases.length} passed`);
