const puerts = require("puerts");
const fixture = puerts.argv.getByName("Fixture");

function decodeBase64(input) {
    const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const outputLength = Math.floor(input.length * 3 / 4) -
        (input.endsWith("==") ? 2 : input.endsWith("=") ? 1 : 0);
    const output = new Uint8Array(outputLength);
    let accumulator = 0;
    let bitCount = 0;
    let outputIndex = 0;

    for (let index = 0; index < input.length; ++index) {
        const character = input[index];
        if (character === "=") {
            break;
        }
        const value = alphabet.indexOf(character);
        if (value < 0) {
            continue;
        }
        accumulator = (accumulator << 6) | value;
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            output[outputIndex++] = (accumulator >>> bitCount) & 0xff;
        }
    }
    return output;
}

const kernelBytes = decodeBase64(fixture.GetControlledWasmBase64());
const kernelWasmSha256 = fixture.GetControlledWasmSha256();
const kernelModule = new WebAssembly.Module(kernelBytes);
const kernelInstance = new WebAssembly.Instance(kernelModule, {});
const run = kernelInstance.exports.run;
if (typeof run !== "function") {
    throw new Error("controlled runtime kernel is missing run(iterations, seed)");
}

fixture.RegisterControlledWasmRunner(
    (iterations, seed) => run(iterations | 0, seed | 0) | 0,
    "webassembly.module_instance.cached_export.v1",
    kernelWasmSha256,
    kernelWasmSha256);
