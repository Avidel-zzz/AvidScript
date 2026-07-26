const UE = require("ue");
const puerts = require("puerts");

const fixture = puerts.argv.getByName("Fixture");
const MIX_MULTIPLIER = 1664525;
const MIX_INCREMENT = 1013904223;
let moduleChecksum = 0;

function mix(value) {
    return (Math.imul(value | 0, MIX_MULTIPLIER) + MIX_INCREMENT) | 0;
}

function runWorkload(fixture, workload, iterations, seed) {
    let accumulator = seed | 0;
    const inOutRef = puerts.$ref(new UE.Vector(0, 0, 0));
    const outRef = puerts.$ref(new UE.Vector(0, 0, 0));
    for (let index = 0; index < iterations; ++index) {
        switch (workload) {
            case 0:
                accumulator = mix(accumulator ^ index);
                break;
            case 1:
                accumulator = mix(fixture.StaticNoOp(accumulator) ^ index);
                break;
            case 2:
                accumulator = mix(fixture.StaticAddInt32(accumulator, index));
                break;
            case 3:
                fixture.StaticScalarValue = accumulator ^ index;
                accumulator = mix(fixture.StaticScalarValue);
                break;
            case 4: {
                const value = new UE.Vector(index & 31, (index * 3) & 31, (index * 7) & 31);
                const result = fixture.StaticVectorValue(value);
                const packed = (result.X | 0) ^ ((result.Y | 0) << 8) ^ ((result.Z | 0) << 16);
                accumulator = mix(accumulator ^ packed);
                break;
            }
            case 5:
                accumulator = mix(accumulator ^ (fixture.StaticObjectRoundtrip(fixture) === fixture ? index : -1));
                break;
            case 6:
                accumulator = mix(fixture.StaticBatchAdd(accumulator, 8));
                break;
            case 9: {
                puerts.$set(
                    inOutRef,
                    new UE.Vector(index & 31, (index * 3) & 31, (index * 7) & 31));
                fixture.StaticVectorRefOut(inOutRef, outRef);
                const inOutValue = puerts.$unref(inOutRef);
                const outValue = puerts.$unref(outRef);
                const packed = (inOutValue.X | 0) +
                    (inOutValue.Y | 0) * 37 +
                    (inOutValue.Z | 0) * 101 +
                    (outValue.X | 0) * 257 +
                    (outValue.Y | 0) * 521 +
                    (outValue.Z | 0) * 1031;
                accumulator = mix(accumulator ^ packed);
                break;
            }
            default:
                throw new Error(`unknown static workload ${workload}`);
        }
    }
    moduleChecksum = accumulator | 0;
}

function resetCallback(seed) {
    moduleChecksum = seed | 0;
}

function emptyCallback(token) {
    moduleChecksum = mix(moduleChecksum ^ (token | 0));
}

function tickCallback(deltaSeconds) {
    void deltaSeconds;
    moduleChecksum = mix(moduleChecksum ^ 1);
}

function getModuleChecksum() {
    return moduleChecksum | 0;
}

fixture.RegisterPuertsCallbacks(
    2,
    runWorkload,
    resetCallback,
    emptyCallback,
    tickCallback,
    getModuleChecksum);
