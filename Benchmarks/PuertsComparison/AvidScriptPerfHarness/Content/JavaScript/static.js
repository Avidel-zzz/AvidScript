import * as UE from "ue";
import * as cpp from "cpp";
import { argv } from "puerts";

const fixture = argv.getByName("Fixture");
const bridge = cpp.AvidScriptPerfStatic;
const MIX_MULTIPLIER = 1664525;
const MIX_INCREMENT = 1013904223;

function mix(value) {
    return (Math.imul(value | 0, MIX_MULTIPLIER) + MIX_INCREMENT) | 0;
}

function runWorkload(workload, iterations, seed) {
    let accumulator = seed | 0;
    for (let index = 0; index < iterations; ++index) {
        switch (workload) {
            case 0:
                accumulator = mix(accumulator ^ index);
                break;
            case 1:
                accumulator = mix(bridge.NoOp(fixture, accumulator) ^ index);
                break;
            case 2:
                accumulator = mix(bridge.AddInt32(fixture, accumulator, index));
                break;
            case 3:
                bridge.SetScalar(fixture, accumulator ^ index);
                accumulator = mix(bridge.GetScalar(fixture));
                break;
            case 4: {
                const value = new UE.Vector(index & 31, (index * 3) & 31, (index * 7) & 31);
                const result = bridge.VectorValue(fixture, value);
                const packed = (result.X | 0) ^ ((result.Y | 0) << 8) ^ ((result.Z | 0) << 16);
                accumulator = mix(accumulator ^ packed);
                break;
            }
            case 5:
                accumulator = mix(accumulator ^ (bridge.ObjectRoundtrip(fixture, fixture) === fixture ? index : -1));
                break;
            case 6:
                accumulator = mix(bridge.BatchAdd(fixture, accumulator, 8));
                break;
            default:
                throw new Error(`unknown static workload ${workload}`);
        }
    }
    return accumulator | 0;
}

function emptyCallback(seed) {
    return mix(seed);
}

fixture.RegisterPuertsCallbacks(runWorkload, emptyCallback);
