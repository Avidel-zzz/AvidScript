const UE = require("ue");
const puerts = require("puerts");

const fixture = puerts.argv.getByName("Fixture");
const MIX_MULTIPLIER = 1664525;
const MIX_INCREMENT = 1013904223;
let moduleChecksum = 0;

function mix(value) {
    return (Math.imul(value | 0, MIX_MULTIPLIER) + MIX_INCREMENT) | 0;
}

function makeGameplayToken(seed, frame, entity, operation) {
    return mix((seed ^ ((frame * 131 + entity * 17 + operation) | 0)) | 0);
}

function runGameplayScalar(fixture, accumulator, token) {
    return mix(fixture.ReflectAddInt32(accumulator, token));
}

function runGameplayPropertyBatch4(fixture, accumulator, token0, token1, token2, token3) {
    const value0 = (accumulator ^ token0) | 0;
    const accumulator0 = mix((value0 ^ token0) | 0);
    const value1 = (accumulator0 ^ token1) | 0;
    const accumulator1 = mix((value1 ^ token1) | 0);
    const value2 = (accumulator1 ^ token2) | 0;
    const accumulator2 = mix((value2 ^ token2) | 0);
    const value3 = (accumulator2 ^ token3) | 0;
    const accumulator3 = mix((value3 ^ token3) | 0);
    fixture.ScalarValue = value0;
    fixture.ScalarValue = value1;
    fixture.ScalarValue = value2;
    fixture.ScalarValue = value3;
    return accumulator3;
}

function runGameplayVector(fixture, accumulator, token) {
    const value = new UE.Vector(token & 31, (token >>> 5) & 31, (token >>> 10) & 31);
    const result = fixture.ReflectVectorValue(value);
    const packed = (result.X | 0) ^ ((result.Y | 0) << 8) ^ ((result.Z | 0) << 16);
    return mix((accumulator ^ packed) | 0);
}

function runGameplayObject(fixture, accumulator, token) {
    const result = fixture.ReflectObjectRoundtrip(fixture) === fixture ? token : ~token;
    return mix((accumulator ^ result) | 0);
}

function runGameplayEvent(fixture, accumulator, token) {
    return mix(fixture.ReflectEventStep(accumulator, token));
}

function runGameplayFrame(fixture, workload, frames, seed) {
    let accumulator = seed | 0;
    for (let frame = 0; frame < frames; ++frame) {
        if (workload === 10) {
            let operation = 0;
            for (let index = 0; index < 32; ++index) {
                const token = makeGameplayToken(seed, frame, 0, operation++);
                accumulator = runGameplayScalar(fixture, accumulator, token);
            }
            for (let batch = 0; batch < 8; ++batch) {
                const token0 = makeGameplayToken(seed, frame, 0, operation++);
                const token1 = makeGameplayToken(seed, frame, 0, operation++);
                const token2 = makeGameplayToken(seed, frame, 0, operation++);
                const token3 = makeGameplayToken(seed, frame, 0, operation++);
                accumulator = runGameplayPropertyBatch4(
                    fixture, accumulator, token0, token1, token2, token3);
            }
            for (let index = 0; index < 8; ++index) {
                const token = makeGameplayToken(seed, frame, 0, operation++);
                accumulator = runGameplayVector(fixture, accumulator, token);
            }
            for (let index = 0; index < 4; ++index) {
                const token = makeGameplayToken(seed, frame, 0, operation++);
                accumulator = runGameplayObject(fixture, accumulator, token);
            }
            for (let index = 0; index < 2; ++index) {
                const token = makeGameplayToken(seed, frame, 0, operation++);
                accumulator = runGameplayEvent(fixture, accumulator, token);
            }
            continue;
        }

        for (let entity = 0; entity < 1024; ++entity) {
            let operation = 0;
            for (let index = 0; index < 4; ++index) {
                const token = makeGameplayToken(seed, frame, entity, operation++);
                accumulator = runGameplayScalar(fixture, accumulator, token);
            }
            const token0 = makeGameplayToken(seed, frame, entity, operation++);
            const token1 = makeGameplayToken(seed, frame, entity, operation++);
            const token2 = makeGameplayToken(seed, frame, entity, operation++);
            const token3 = makeGameplayToken(seed, frame, entity, operation++);
            accumulator = runGameplayPropertyBatch4(
                fixture, accumulator, token0, token1, token2, token3);
            for (let index = 0; index < 2; ++index) {
                const token = makeGameplayToken(seed, frame, entity, operation++);
                accumulator = runGameplayVector(fixture, accumulator, token);
            }
            const token = makeGameplayToken(seed, frame, entity, operation);
            accumulator = (entity & 1) === 0
                ? runGameplayObject(fixture, accumulator, token)
                : runGameplayEvent(fixture, accumulator, token);
        }
    }
    return accumulator | 0;
}

function runWorkload(fixture, workload, iterations, seed) {
    if (workload === 10 || workload === 11) {
        moduleChecksum = runGameplayFrame(fixture, workload, iterations, seed);
        return;
    }
    let accumulator = seed | 0;
    const inOutRef = puerts.$ref(new UE.Vector(0, 0, 0));
    const outRef = puerts.$ref(new UE.Vector(0, 0, 0));
    for (let index = 0; index < iterations; ++index) {
        switch (workload) {
            case 0:
                accumulator = mix(accumulator ^ index);
                break;
            case 1:
                accumulator = mix(fixture.ReflectNoOp(accumulator) ^ index);
                break;
            case 2:
                accumulator = mix(fixture.ReflectAddInt32(accumulator, index));
                break;
            case 3:
                fixture.ScalarValue = accumulator ^ index;
                accumulator = mix(fixture.ScalarValue);
                break;
            case 4: {
                const value = new UE.Vector(index & 31, (index * 3) & 31, (index * 7) & 31);
                const result = fixture.ReflectVectorValue(value);
                const packed = (result.X | 0) ^ ((result.Y | 0) << 8) ^ ((result.Z | 0) << 16);
                accumulator = mix(accumulator ^ packed);
                break;
            }
            case 5:
                accumulator = mix(accumulator ^ (fixture.ReflectObjectRoundtrip(fixture) === fixture ? index : -1));
                break;
            case 6:
                accumulator = mix(fixture.ReflectBatchAdd(accumulator, 8));
                break;
            case 9: {
                puerts.$set(
                    inOutRef,
                    new UE.Vector(index & 31, (index * 3) & 31, (index * 7) & 31));
                fixture.ReflectVectorRefOut(inOutRef, outRef);
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
                throw new Error(`unknown reflection workload ${workload}`);
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
    1,
    runWorkload,
    resetCallback,
    emptyCallback,
    tickCallback,
    getModuleChecksum);
