// Run the ordinary C# array-foreach fixtures as real WASM. Local arrays must
// stay in guest memory, so an unexpected UE host array call fails the probe.
const fs = require('node:fs');
const path = require('node:path');

const fixtureDirectory = process.argv[2];
if (!fixtureDirectory) {
    throw new Error('Usage: node RunArrayForeachWasm.js <fixture-directory>');
}

const cases = [
    ['array-foreach.wasm', 'array_foreach', 16],
    ['array-foreach-branches.wasm', 'array_foreach_branches', 107],
    ['array-foreach-nested-return.wasm', 'array_foreach_nested_return', 117],
    ['array-foreach-reference.wasm', 'array_foreach_reference', 6],
];

for (const [fileName, exportName, expected] of cases) {
    const module = new WebAssembly.Module(
        fs.readFileSync(path.join(fixtureDirectory, fileName)));
    const imports = {};
    for (const item of WebAssembly.Module.imports(module)) {
        (imports[item.module] ??= {})[item.name] = () => {
            throw new Error(`Unexpected host call: ${item.module}.${item.name}`);
        };
    }
    const instance = new WebAssembly.Instance(module, imports);
    const actual = instance.exports[exportName]();
    if (actual !== expected) {
        throw new Error(`${exportName}: expected ${expected}, received ${actual}`);
    }
    console.log(`${exportName}: ${actual}`);
}

console.log(`ArrayForeachWasm: ${cases.length}/${cases.length} passed`);
