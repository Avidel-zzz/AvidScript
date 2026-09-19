import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

assert.ok(process.argv[2], 'Pass lexical-captures.wasm emitted by the Guest tests.');
const bytes = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(bytes));
const { instance } = await WebAssembly.instantiate(bytes, {});
let cases = 0;
for (let round = 0; round < 3; ++round) {
    for (let n = -10; n <= 64; ++n) {
        assert.equal(instance.exports.run(n), (2 * n + 6) * 1000 + 2 * n + 17);
        assert.equal(instance.exports.parameter(n), 2 * n + 10);
        assert.equal(instance.exports.loop(n), n > 0 ? n * (n - 1) / 2 : 0);
        assert.equal(instance.exports.constant(), 37);
        cases += 4;
    }
}
console.log(`AvidScript.LexicalCaptures.WasmExecution: ${cases}/${cases} passed`);
