import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

assert.ok(process.argv[2], 'Pass lambdas.wasm emitted by the Guest tests.');
const bytes = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(bytes));
const { instance } = await WebAssembly.instantiate(bytes, {});
let cases = 0;
for (let round = 0; round < 3; ++round) {
    for (let n = -5; n <= 64; ++n) {
        assert.equal(instance.exports.run(n), n * n + (n > 0 ? n + 2 : 7) + (2 * n + 1) + (n - 3) + (n > 0 ? n + 11 : n - 11));
        assert.equal(instance.exports.local(n), n * 4 + 8);
        assert.equal(instance.exports.byref(n), (n + 2) * 1000 + (n + 2) * 30 + n + 3);
        assert.equal(instance.exports.recursive(n), n > 0 ? n * (n + 1) / 2 : 0);
        assert.equal(instance.exports.property(n), n + 17);
        cases += 5;
    }
}
console.log(`AvidScript.Lambdas.WasmExecution: ${cases}/${cases} passed`);
