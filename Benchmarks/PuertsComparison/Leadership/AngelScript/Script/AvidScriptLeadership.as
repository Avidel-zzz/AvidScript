// Same-semantics adapter for the existing AAvidScriptPerfFixture UE contract.
// Editor correctness is verified separately from formal timing and native code.
namespace As36Leadership
{
    uint Mix(uint Value)
    {
        return Value * uint(1664525) + uint(1013904223);
    }

    uint Token(uint Seed, int Frame, int Entity, int Operation)
    {
        return Mix(Seed ^ uint(Frame * 131 + Entity * 17 + Operation));
    }

    uint PackVector(const FVector& Value)
    {
        return uint(Value.X) ^ (uint(Value.Y) << 8) ^ (uint(Value.Z) << 16);
    }

    uint GameplayScalar(AAvidScriptPerfFixture Fixture, uint Accumulator, uint Value)
    {
        return Mix(uint(Fixture.ReflectAddInt32(int(Accumulator), int(Value))));
    }

    uint GameplayProperty(AAvidScriptPerfFixture Fixture, uint Accumulator, uint Value)
    {
        uint Written = Accumulator ^ Value;
        Fixture.ScalarValue = int(Written);
        return Mix(Written ^ Value);
    }

    uint GameplayVector(AAvidScriptPerfFixture Fixture, uint Accumulator, uint Value)
    {
        FVector Input(float64(Value & uint(31)), float64((Value >> 5) & uint(31)), float64((Value >> 10) & uint(31)));
        return Mix(Accumulator ^ PackVector(Fixture.ReflectVectorValue(Input)));
    }

    uint GameplayObject(AAvidScriptPerfFixture Fixture, uint Accumulator, uint Value)
    {
        UObject Result = Fixture.ReflectObjectRoundtrip(Fixture);
        return Mix(Accumulator ^ ((Result == Fixture) ? Value : ~Value));
    }

    uint GameplayEvent(AAvidScriptPerfFixture Fixture, uint Accumulator, uint Value)
    {
        return Mix(uint(Fixture.ReflectEventStep(int(Accumulator), int(Value))));
    }

    uint SmallFrame(AAvidScriptPerfFixture Fixture, int Frame, uint Seed, uint InputAccumulator)
    {
        uint Accumulator = InputAccumulator;
        int Operation = 0;
        for (int Index = 0; Index < 32; ++Index)
            Accumulator = GameplayScalar(Fixture, Accumulator, Token(Seed, Frame, 0, Operation++));
        for (int Index = 0; Index < 32; ++Index)
            Accumulator = GameplayProperty(Fixture, Accumulator, Token(Seed, Frame, 0, Operation++));
        for (int Index = 0; Index < 8; ++Index)
            Accumulator = GameplayVector(Fixture, Accumulator, Token(Seed, Frame, 0, Operation++));
        for (int Index = 0; Index < 4; ++Index)
            Accumulator = GameplayObject(Fixture, Accumulator, Token(Seed, Frame, 0, Operation++));
        for (int Index = 0; Index < 2; ++Index)
            Accumulator = GameplayEvent(Fixture, Accumulator, Token(Seed, Frame, 0, Operation++));
        return Accumulator;
    }

    uint DenseFrame(AAvidScriptPerfFixture Fixture, int Frame, uint Seed, uint InputAccumulator)
    {
        uint Accumulator = InputAccumulator;
        for (int Entity = 0; Entity < 1024; ++Entity)
        {
            int Operation = 0;
            for (int Index = 0; Index < 4; ++Index)
                Accumulator = GameplayScalar(Fixture, Accumulator, Token(Seed, Frame, Entity, Operation++));
            for (int Index = 0; Index < 4; ++Index)
                Accumulator = GameplayProperty(Fixture, Accumulator, Token(Seed, Frame, Entity, Operation++));
            for (int Index = 0; Index < 2; ++Index)
                Accumulator = GameplayVector(Fixture, Accumulator, Token(Seed, Frame, Entity, Operation++));
            uint Value = Token(Seed, Frame, Entity, Operation);
            if ((Entity & 1) == 0)
                Accumulator = GameplayObject(Fixture, Accumulator, Value);
            else
                Accumulator = GameplayEvent(Fixture, Accumulator, Value);
        }
        return Accumulator;
    }

    int Run(AAvidScriptPerfFixture Fixture, int Workload, int Iterations, int Seed)
    {
        uint Accumulator = uint(Seed);
        if (Workload == 10 || Workload == 11)
        {
            for (int Frame = 0; Frame < Iterations; ++Frame)
            {
                if (Workload == 10)
                    Accumulator = SmallFrame(Fixture, Frame, uint(Seed), Accumulator);
                else
                    Accumulator = DenseFrame(Fixture, Frame, uint(Seed), Accumulator);
            }
            return int(Accumulator);
        }
        // Select the workload before its timed inner loop, matching the C#
        // and Puerts adapters; do not add per-iteration dispatch overhead.
        if (Workload == 0)
        {
            for (int Index = 0; Index < Iterations; ++Index)
                Accumulator = Mix(Accumulator ^ uint(Index));
        }
        else if (Workload == 1)
        {
            for (int Index = 0; Index < Iterations; ++Index)
                Accumulator = Mix(uint(Fixture.ReflectNoOp(int(Accumulator))) ^ uint(Index));
        }
        else if (Workload == 2)
        {
            for (int Index = 0; Index < Iterations; ++Index)
                Accumulator = Mix(uint(Fixture.ReflectAddInt32(int(Accumulator), Index)));
        }
        else if (Workload == 3)
        {
            for (int Index = 0; Index < Iterations; ++Index)
            {
                Fixture.ScalarValue = int(Accumulator ^ uint(Index));
                Accumulator = Mix(uint(Fixture.ScalarValue));
            }
        }
        else if (Workload == 4)
        {
            for (int Index = 0; Index < Iterations; ++Index)
            {
                FVector Value(float64(Index & 31), float64((Index * 3) & 31), float64((Index * 7) & 31));
                Accumulator = Mix(Accumulator ^ PackVector(Fixture.ReflectVectorValue(Value)));
            }
        }
        else if (Workload == 5)
        {
            for (int Index = 0; Index < Iterations; ++Index)
            {
                UObject Result = Fixture.ReflectObjectRoundtrip(Fixture);
                Accumulator = Mix(Accumulator ^ ((Result == Fixture) ? uint(Index) : uint(0xffffffff)));
            }
        }
        else if (Workload == 6)
        {
            for (int Index = 0; Index < Iterations; ++Index)
                Accumulator = Mix(uint(Fixture.ReflectBatchAdd(int(Accumulator), 8)));
        }
        else if (Workload == 9)
        {
            for (int Index = 0; Index < Iterations; ++Index)
            {
                FVector InOut(float64(Index & 31), float64((Index * 3) & 31), float64((Index * 7) & 31));
                FVector Out;
                Fixture.ReflectVectorRefOut(InOut, Out);
                uint Packed = uint(InOut.X) + uint(InOut.Y) * uint(37) + uint(InOut.Z) * uint(101)
                    + uint(Out.X) * uint(257) + uint(Out.Y) * uint(521) + uint(Out.Z) * uint(1031);
                Accumulator = Mix(Accumulator ^ Packed);
            }
        }
        else
            return int(Mix(Accumulator ^ uint(0xffffffff)));
        return int(Accumulator);
    }

}

// The host must use cached production UFunction/ProcessEvent entries on this
// actor, with setup and function lookup outside timing. Raw context execution
// would bypass the actual UE entry route and is not the callback comparison.
class AAs36LeadershipDriver : AActor
{
    UPROPERTY()
    AAvidScriptPerfFixture Fixture;

    uint CallbackChecksum = 0;

    UFUNCTION()
    void Initialize(AAvidScriptPerfFixture InFixture)
    {
        Fixture = InFixture;
    }

    UFUNCTION()
    bool ValidateNativeTypes()
    {
        // These fractional values cannot roundtrip through float32 unchanged.
        FVector Value(float64(16777217) + 0.25, -(float64(16777219) + 0.5), float64(1099511627776) + 0.125);
        FVector Result = Fixture.ReflectVectorValue(Value);
        if (Result.X != Value.X + 1.0 || Result.Y != Value.Y + 2.0 || Result.Z != Value.Z + 3.0)
            return false;
        FVector InOut = Value;
        FVector Out;
        Fixture.ReflectVectorRefOut(InOut, Out);
        if (InOut.X != Value.X + 1.0 || InOut.Y != Value.Y + 2.0 || InOut.Z != Value.Z + 3.0)
            return false;
        if (Out.X != Value.X + 4.0 || Out.Y != Value.Y + 5.0 || Out.Z != Value.Z + 6.0)
            return false;
        return (Fixture.ReflectObjectRoundtrip(Fixture) == Fixture)
            && (Fixture.ReflectObjectRoundtrip(nullptr) == nullptr);
    }

    UFUNCTION()
    int RunWorkload(int Workload, int Iterations, int Seed)
    {
        return As36Leadership::Run(Fixture, Workload, Iterations, Seed);
    }

    UFUNCTION()
    void ResetCallback(int Seed)
    {
        CallbackChecksum = uint(Seed);
    }

    UFUNCTION()
    void EmptyCallback(int Value)
    {
        CallbackChecksum = As36Leadership::Mix(CallbackChecksum ^ uint(Value));
    }

    UFUNCTION(BlueprintOverride)
    void Tick(float32 DeltaSeconds)
    {
        CallbackChecksum = As36Leadership::Mix(CallbackChecksum ^ uint(1));
    }

    UFUNCTION()
    int GetCallbackChecksum()
    {
        return int(CallbackChecksum);
    }
}
