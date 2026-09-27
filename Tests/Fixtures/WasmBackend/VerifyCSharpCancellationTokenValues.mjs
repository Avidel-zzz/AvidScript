// Run real C# fixtures emitted by CSharpGuest.Tests --cancellation-token-values.
// Pure values need no Host. Exception and async execution belong to UE tests.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';

assert.equal(process.argv.length, 3, 'Usage: node VerifyCSharpCancellationTokenValues.mjs <fixture-directory>');
let passed = 0;
function equal(actual, expected, label) {
  assert.equal(actual, expected, label);
  passed++;
}

const names = ['none', 'copy', 'convert', 'value-call', 'parameter-values', 'evaluation-order', 'comparison-snapshot'];
const values = [0n, 1n, -1n, 0x7fffffffn, 0x80000000n, 0xffffffffn,
  0x100000000n, 0x100000001n, 0x7fffffffffffffffn, -0x8000000000000000n];
for (const name of names) {
  const bytes = await readFile(join(process.argv[2], `${name}.wasm`));
  equal(WebAssembly.validate(bytes), true, `${name}: valid WASM`);
  const module = await WebAssembly.compile(bytes);
  equal(WebAssembly.Module.imports(module).length, 0, `${name}: no Host imports`);
  const first = (await WebAssembly.instantiate(module)).exports;
  const second = (await WebAssembly.instantiate(module)).exports;
  for (let index = 0; index < 64; index++) {
    equal(first.token_main(), 1, `${name}: first instance invocation ${index}`);
    equal(second.token_main(), 1, `${name}: second instance invocation ${index}`);
  }
  if (name !== 'parameter-values') continue;
  for (const instance of [first, second])
    for (const left of values)
      for (const right of values)
        equal(instance.token_same(left, right), left === right ? 1 : 0, `C# copy/compare: ${left}, ${right}`);
  for (let index = 0; index < 1024; index++) {
    const value = (BigInt(index) << 32n) | 1n;
    equal(first.token_same(value, value), 1, 'C# parameter/return retains high bits');
    equal(second.token_same(-value, value), 0, 'C# parameter/return retains sign');
  }
}
console.log(`C# cancellation token WASM observations: ${passed}/${passed} passed`);
