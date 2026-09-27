// Execute the hand-authored IR value fixture emitted by WasmBackend.Tests.
// No UE Host is supplied; exception-reader lifecycle is a separate native test.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

assert.equal(process.argv.length, 3, 'Usage: node VerifyCancellationTokenValues.mjs <token-values.wasm>');
const bytes = await readFile(process.argv[2]);
let passed = 0;
function equal(actual, expected, label) {
  assert.equal(actual, expected, label);
  passed++;
}

equal(WebAssembly.validate(bytes), true, 'Production emitter produces valid WASM');
const module = await WebAssembly.compile(bytes);
equal(WebAssembly.Module.imports(module).length, 0, 'Token values do not require a Host');
const first = (await WebAssembly.instantiate(module)).exports;
const second = (await WebAssembly.instantiate(module)).exports;
const values = [0n, 1n, -1n, 0x7fffffffn, 0x80000000n, 0xffffffffn,
  0x100000000n, 0x100000001n, 0x7fffffffffffffffn, -0x8000000000000000n];

for (const [index, exports] of [first, second].entries()) {
  equal(exports.none(), 0n, `Instance ${index}: None`);
  for (const value of values) {
    equal(exports.roundtrip(value), value, `Instance ${index}: aggregate parameter/return ${value}`);
    for (const other of values) {
      equal(exports.compare(value, other), value === other ? 1 : 0,
        `Instance ${index}: compare ${value} and ${other}`);
    }
  }
}

// Interleave calls to exercise stack-frame restoration and instance isolation.
for (let index = 0; index < 1024; index++) {
  const value = (BigInt(index) << 32n) | 1n;
  equal(first.roundtrip(value), value, 'First instance retains upper bits');
  equal(second.roundtrip(-value), -value, 'Second instance retains sign');
}
equal(first.none(), 0n, 'None after repeated aggregate calls');
equal(second.none(), 0n, 'Other instance None after repeated calls');
console.log(`Cancellation token WASM value observations: ${passed}/${passed} passed`);
