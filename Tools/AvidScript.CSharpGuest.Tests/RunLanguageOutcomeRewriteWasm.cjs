'use strict';

const fs = require('node:fs');

if (process.argv.length !== 4) {
  throw new Error('Usage: node RunLanguageOutcomeRewriteWasm.cjs <normal.wasm> <error.wasm>');
}

for (const [path, expected] of [[process.argv[2], 8], [process.argv[3], 99]]) {
  const wasmModule = new WebAssembly.Module(fs.readFileSync(path));
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
      if (command === 7) throw new Error('Probe unexpectedly allocated a managed object');
      if (outputBytes === 8) bytes.setBigUint64(output, ++nextToken, true);
      return 1;
    } },
  };
  instance = new WebAssembly.Instance(wasmModule, imports);
  const actual = instance.exports.outcome_rewrite_probe();
  if (actual !== expected) throw new Error(`${path}: ${actual}; expected ${expected}`);
}
process.stdout.write('C# outcome rewrite WASM: 2/2 passed\n');
