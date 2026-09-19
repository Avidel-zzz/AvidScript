import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

assert.ok(process.argv[2], 'Pass the AVIDSCRIPT_FUNCTION_REFERENCE_WASM_DIR output directory.');
let cases = 0;
for (const cooperative of [false, true]) {
    const bytes = readFileSync(join(process.argv[2], `function-references${cooperative ? '-cooperative' : ''}.wasm`));
    assert.ok(WebAssembly.validate(bytes));
    let polls = 0;
    let cancel = false;
    const cancellation = new Error('cancel requested');
    const { instance } = await WebAssembly.instantiate(bytes, {
        avidscript: { avid_cooperative_safepoint_poll() { ++polls; if (cancel) throw cancellation; } },
    });
    const e = instance.exports;
    assert.equal(Object.values(e).some(value => value instanceof WebAssembly.Table), false);
    for (let round = 0; round < 3; ++round) {
        for (let n = -20; n <= 80; ++n) {
            assert.equal(e.apply(e.identity(e.get_a()), n), n + 10);
            assert.equal(e.apply(e.identity(e.get_minus()), n), n - 10);
            assert.equal(e.pair(n), n);
            assert.equal(e.byref(n), n + 10);
            assert.equal(e.twice(n + 0.125), 2 * (n + 0.125));
            cases += 5;
        }
    }
    assert.equal(e.recurse(50), 1275);
    ++cases;
    // Same erased WASM signature, different nominal contract: must trap.
    for (const bad of [0, -1, 0x7fffffff, e.get_b()]) {
        assert.throws(() => e.apply(bad, 8), WebAssembly.RuntimeError);
        assert.throws(() => e.empty(bad, 8), WebAssembly.RuntimeError);
        cases += 2;
    }
    assert.throws(() => e.null_call(1), WebAssembly.RuntimeError);
    ++cases;
    if (cooperative) {
        assert.ok(polls > 0, 'indirect recursion must poll');
        cancel = true;
        assert.throws(() => e.recurse(50), error => error === cancellation);
        ++cases;
    }
}
console.log(`AvidScript.FunctionReferences.WasmExecution: ${cases}/${cases} passed`);
