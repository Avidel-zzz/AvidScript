const puerts = require("puerts");
const UE = require("ue");

const fixture = puerts.argv.getByName("Fixture");
const MIX_MULTIPLIER = 1664525;
const MIX_INCREMENT = 1013904223;
const FNV_OFFSET = 0x811c9dc5;
const FNV_PRIME = 16777619;
let fullHash = "fnv1a32:00000000";

function mix(value) {
    return (Math.imul(value | 0, MIX_MULTIPLIER) + MIX_INCREMENT) | 0;
}

function formatHash(value) {
    return `fnv1a32:${(value >>> 0).toString(16).padStart(8, "0")}`;
}

function runArrayWorkload(fixtureObject, size, logicalCalls, seed) {
    let values = UE.NewArray(UE.BuiltinInt);
    for (let index = 0; index < size; ++index) {
        values.Add(mix((seed ^ index) | 0));
    }

    let hash = FNV_OFFSET | 0;
    for (let call = 0; call < logicalCalls; ++call) {
        values = fixtureObject.ReflectInt32ArrayRoundtrip(values);
        hash = FNV_OFFSET | 0;
        for (let index = 0; index < values.Num(); ++index) {
            hash = Math.imul((hash ^ (values.Get(index) | 0)) | 0, FNV_PRIME) | 0;
        }
    }
    fullHash = formatHash(hash);
}

function getFullHash() {
    return fullHash;
}

fixture.RegisterPuertsArrayCallbacks(runArrayWorkload, getFullHash);
