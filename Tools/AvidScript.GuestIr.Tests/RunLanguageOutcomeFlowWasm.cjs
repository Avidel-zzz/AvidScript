'use strict';

const fs = require('node:fs');

if (process.argv.length !== 3) {
  throw new Error('Usage: node RunLanguageOutcomeFlowWasm.cjs <module.wasm>');
}

const wasmModule = new WebAssembly.Module(fs.readFileSync(process.argv[2]));
for (const entry of WebAssembly.Module.imports(wasmModule)) {
  if (entry.module !== 'avidscript' || entry.name !== 'avid_managed_heap_v1') {
    throw new Error(`Unexpected import ${entry.module}.${entry.name}`);
  }
}
let instance;
let nextToken = 0n;
const imports = {
  avidscript: { avid_managed_heap_v1: (packet, _inputBytes, output, outputBytes) => {
    const bytes = new DataView(instance.exports.memory.buffer);
    const command = bytes.getInt32(packet + 4, true);
    if (command === 7) throw new Error('Fixture unexpectedly allocated a managed object');
    if (outputBytes === 8) bytes.setBigUint64(output, ++nextToken, true);
    return 1;
  } },
};
instance = new WebAssembly.Instance(wasmModule, imports);
for (const [name, input, expected] of [
  ['checked_outcome', 0, 42], ['checked_outcome', 1, 2],
  ['propagated_outcome', 0, 43], ['propagated_outcome', 1, 2],
]) {
  const call = instance.exports[name];
  if (typeof call !== 'function') throw new Error(`Missing ${name} export`);
  const actual = call(input);
  if (actual !== expected) {
    throw new Error(`${name}(${input}) = ${actual}; expected ${expected}`);
  }
}
process.stdout.write('Language outcome flow WASM: 4/4 passed\n');
