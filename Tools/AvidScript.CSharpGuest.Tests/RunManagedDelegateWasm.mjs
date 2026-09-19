import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

assert.ok(process.argv[2], 'Pass managed-delegates.wasm emitted by the Guest tests.');
const bytes = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(bytes));
const { instance } = await WebAssembly.instantiate(bytes, {});
const e = instance.exports;
let cases = 0;
for (let round = 0; round < 3; ++round) {
    for (let n = -5; n <= 64; ++n) {
        for (const which of [-1, 1]) {
            const sum = n > 0 ? n * (n - 1) / 2 + which * 10 * n : 0;
            assert.equal(e.run(n, which), sum + n + 10);
            ++cases;
        }
        assert.equal(e.byref(n), (n + 7) + (n + 4) * 100 + (n + 4) * 2000);
        assert.equal(e.local(n), 2 * n);
        assert.equal(e.named(n), (n + 1) * 100 + n + 203);
        assert.equal(e.wide(BigInt(n)), BigInt(n) + 50n);
        assert.equal(e.nullable(n), n > 0 ? n + 10 : -7);
        assert.equal(e.action(n), n + 3);
        assert.equal(e.recursive(n), n > 0 ? n * (n + 1) / 2 : 0);
        cases += 7;
    }
}
console.log(`AvidScript.ManagedDelegates.WasmExecution: ${cases}/${cases} passed`);
