const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

async function main() {
  const file = path.join(process.argv[2] || "", "outcome-slots.wasm");
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(file), {});
  const memory = instance.exports.memory;
  const call = instance.exports.outcome_call;
  assert.ok(memory instanceof WebAssembly.Memory);
  assert.equal(typeof call, "function");

  const output = 4096;
  const cases = [
    [2, 0, [0, 0, 0, 23, 1]],
    [1, 0, [1, 7, 31, 0, 1]],
    [0, 0, [1, 7, 31, 0, 1]],
    [2, 1, [1, 9, 44, 0, 1]],
    [1, 1, [1, 9, 44, 0, 1]],
  ];
  for (const [input, cleanupThrows, expected] of cases) {
    new Uint8Array(memory.buffer, output, 20).fill(0xff);
    call(output, input, cleanupThrows);
    const view = new DataView(memory.buffer, output, 20);
    const actual = Array.from({ length: 5 }, (_, i) => view.getInt32(i * 4, true));
    assert.deepEqual(actual, expected, "input=" + input + ", cleanupThrows=" + cleanupThrows);
  }
  console.log("OutcomeSlotWasm: " + cases.length + "/" + cases.length + " passed");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
