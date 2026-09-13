#include "As36LeadershipBridge.h"

#include "AvidScriptPerfFixture.h"
#include "AngelscriptManager.h"
#include "ClassGenerator/ASClass.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

namespace
{
    FString DescribeAs36Function(UFunction& Function)
    {
        FString Text = FString::Printf(TEXT(" %s parms=%d struct=%d:"), *Function.GetName(), Function.ParmsSize, Function.GetStructureSize());
        for (TFieldIterator<FProperty> It(&Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_Parm))
            {
                Text += FString::Printf(TEXT(" [%s %s offset=%d size=%d flags=%llx]"),
                    *It->GetName(), *It->GetCPPType(), It->GetOffset_ForInternal(), It->GetSize(),
                    static_cast<uint64>(It->GetPropertyFlags()));
            }
        }
        return Text;
    }

    template <typename PropertyType, typename ValueType>
    ValueType* FindAs36Parameter(UFunction& Function, void* Frame, FName Name, bool bReturn)
    {
        PropertyType* Property = FindFProperty<PropertyType>(&Function, Name);
        // UASFunction::ParmsSize is reset to the end of the last input argument.
        // Its return value can follow those bytes. FStructOnScope allocates the
        // full linked UStruct size, which is the actual return-buffer boundary.
        const int32 FrameBytes = Function.GetStructureSize();
        const int32 Limit = bReturn ? FrameBytes : Function.ParmsSize;
        if (!Property || Property->ArrayDim != 1 ||
            FrameBytes < Function.ParmsSize ||
            !Property->HasAnyPropertyFlags(CPF_Parm) ||
            Property->HasAnyPropertyFlags(CPF_ReturnParm) != bReturn ||
            (!bReturn && Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)) ||
            (bReturn && Property->GetOffset_ForUFunction() != Function.ReturnValueOffset) ||
            Property->GetOffset_ForInternal() < 0 ||
            Property->GetOffset_ForInternal() + Property->GetSize() > Limit)
        {
            return nullptr;
        }
        return Property->template ContainerPtrToValuePtr<ValueType>(Frame);
    }
}

bool FAs36LeadershipBridge::BindEntry(
    FEntry& Entry, FName Name, int32 ExpectedParameterCount, FString& OutError)
{
    UFunction* Function = Actor->FindFunction(Name);
    if (!Function || !Cast<UASFunction>(Function) ||
        Function->GetOuterUClass() != BoundClass.Get() ||
        Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Net | FUNC_Delegate) ||
        Function->NumParms != ExpectedParameterCount)
    {
        OutError = FString::Printf(TEXT("Invalid AngelScript entry %s"), *Name.ToString());
        return false;
    }
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        if (It->HasAnyPropertyFlags(CPF_Parm)) { ++Count; }
    }
    if (Count != ExpectedParameterCount)
    {
        OutError = FString::Printf(TEXT("Unexpected parameter set in %s"), *Name.ToString());
        return false;
    }
    Entry.Function = Function;
    Entry.Frame = MakeUnique<FStructOnScope>(Function);
    return true;
}

bool FAs36LeadershipBridge::CheckLifetime(FString& OutError) const
{
    if (!IsInGameThread() || !Actor.IsValid() || !Fixture.IsValid() ||
        !World.IsValid() || !BoundClass.IsValid() || World->bIsTearingDown ||
        Actor->IsActorBeingDestroyed() || Fixture->IsActorBeingDestroyed() ||
        Actor->GetWorld() != World.Get() || Fixture->GetWorld() != World.Get() ||
        Actor->GetClass() != BoundClass.Get())
    {
        OutError = TEXT("Invalid Game Thread, actor, fixture, class or measured World lifetime");
        return false;
    }
    const UASClass* ScriptClass = Cast<UASClass>(BoundClass.Get());
    if (!ScriptClass || ScriptClass->NewerVersion)
    {
        OutError = TEXT("AngelScript class is missing or has been superseded by reload");
        return false;
    }
    return true;
}

bool FAs36LeadershipBridge::Bind(
    AActor& InActor, AAvidScriptPerfFixture& InFixture, FString& OutError)
{
    bBound = false;
    PreparedActor = nullptr;
    bExecuted = false;
    Actor = &InActor;
    Fixture = &InFixture;
    BoundClass = InActor.GetClass();
    World = InActor.GetWorld();
    if (!CheckLifetime(OutError)) { return false; }
    const TSharedPtr<FAngelscriptClassDesc> Descriptor =
        FAngelscriptManager::Get().GetClass(TEXT("AAs36LeadershipDriver"));
    if (!Descriptor.IsValid() || Descriptor->Class != BoundClass.Get())
    {
        OutError = TEXT("Actor is not the active AAs36LeadershipDriver script class");
        return false;
    }
    if (!BindEntry(Initialize, TEXT("Initialize"), 1, OutError) ||
        !BindEntry(ValidateTypes, TEXT("ValidateNativeTypes"), 1, OutError) ||
        !BindEntry(Run, TEXT("RunWorkload"), 4, OutError) ||
        !BindEntry(Reset, TEXT("ResetCallback"), 1, OutError) ||
        !BindEntry(Empty, TEXT("EmptyCallback"), 1, OutError) ||
        !BindEntry(Tick, TEXT("ReceiveTick"), 1, OutError) ||
        !BindEntry(Read, TEXT("GetCallbackChecksum"), 1, OutError))
    {
        return false;
    }
    FObjectProperty* FixtureParameter = FindFProperty<FObjectProperty>(Initialize.Function, TEXT("InFixture"));
    if (!FixtureParameter || FixtureParameter->PropertyClass != AAvidScriptPerfFixture::StaticClass() ||
        !FindAs36Parameter<FObjectProperty, UObject*>(*Initialize.Function, Initialize.Memory(), TEXT("InFixture"), false))
    {
        OutError = TEXT("Invalid fixture parameter type or layout");
        return false;
    }
    RunWorkload = FindAs36Parameter<FIntProperty, int32>(*Run.Function, Run.Memory(), TEXT("Workload"), false);
    RunIterations = FindAs36Parameter<FIntProperty, int32>(*Run.Function, Run.Memory(), TEXT("Iterations"), false);
    RunSeed = FindAs36Parameter<FIntProperty, int32>(*Run.Function, Run.Memory(), TEXT("Seed"), false);
    RunResult = FindAs36Parameter<FIntProperty, int32>(*Run.Function, Run.Memory(), TEXT("ReturnValue"), true);
    ResetSeed = FindAs36Parameter<FIntProperty, int32>(*Reset.Function, Reset.Memory(), TEXT("Seed"), false);
    EmptyValue = FindAs36Parameter<FIntProperty, int32>(*Empty.Function, Empty.Memory(), TEXT("Value"), false);
    TickDelta = FindAs36Parameter<FFloatProperty, float>(*Tick.Function, Tick.Memory(), TEXT("DeltaSeconds"), false);
    ReadResult = FindAs36Parameter<FIntProperty, int32>(*Read.Function, Read.Memory(), TEXT("ReturnValue"), true);
    if (!RunWorkload || !RunIterations || !RunSeed || !RunResult ||
        !ResetSeed || !EmptyValue || !TickDelta || !ReadResult)
    {
        OutError = FString::Printf(TEXT("Unexpected parameter layout: workload=%d iterations=%d seed=%d run_result=%d reset=%d empty=%d tick=%d read_result=%d."),
            RunWorkload != nullptr, RunIterations != nullptr, RunSeed != nullptr, RunResult != nullptr,
            ResetSeed != nullptr, EmptyValue != nullptr, TickDelta != nullptr, ReadResult != nullptr);
        OutError += DescribeAs36Function(*Run.Function) + DescribeAs36Function(*Reset.Function)
            + DescribeAs36Function(*Empty.Function) + DescribeAs36Function(*Tick.Function) + DescribeAs36Function(*Read.Function);
        return false;
    }
    FixtureParameter->SetObjectPropertyValue_InContainer(Initialize.Memory(), &InFixture);
    InActor.ProcessEvent(Initialize.Function, Initialize.Memory());
    FObjectProperty* StoredFixture = FindFProperty<FObjectProperty>(BoundClass.Get(), TEXT("Fixture"));
    if (!StoredFixture || StoredFixture->GetObjectPropertyValue_InContainer(&InActor) != &InFixture)
    {
        OutError = TEXT("Script Initialize did not preserve the fixture identity");
        return false;
    }
    FBoolProperty* TypeResult = CastField<FBoolProperty>(ValidateTypes.Function->GetReturnProperty());
    if (!TypeResult || TypeResult->ArrayDim != 1 ||
        TypeResult->GetOffset_ForInternal() < 0 ||
        TypeResult->GetOffset_ForInternal() + TypeResult->GetSize() > ValidateTypes.Function->ParmsSize)
    {
        OutError = TEXT("Invalid native-type validation return layout");
        return false;
    }
    InActor.ProcessEvent(ValidateTypes.Function, ValidateTypes.Memory());
    if (!TypeResult->GetPropertyValue_InContainer(ValidateTypes.Memory()))
    {
        OutError = TEXT("Float64 FVector, ref/out or UObject identity validation failed");
        return false;
    }
    bBound = true;
    return true;
}

bool FAs36LeadershipBridge::PrepareSample(
    int32 Workload, int32 Iterations, int32 Seed, FString& OutError)
{
    PreparedActor = nullptr;
    bExecuted = false;
    const int32 Limit = Workload >= 10 ? 1048576 : 10000000;
    if (!bBound || Workload < 0 || Workload >= 12 || Iterations < 1 || Iterations > Limit)
    {
        OutError = TEXT("Unbound bridge or invalid workload/iteration range");
        return false;
    }
    if (!CheckLifetime(OutError)) { return false; }
    SelectedWorkload = Workload;
    SelectedIterations = Iterations;
    *RunWorkload = Workload;
    *RunIterations = Iterations;
    *RunSeed = Seed;
    *RunResult = 0;
    *ReadResult = 0;
    *ResetSeed = Seed;
    *TickDelta = 1.0f / 60.0f;
    Actor->ProcessEvent(Reset.Function, Reset.Memory());
    Fixture->ScalarValue = 0;
    // Count actual native operations by their own IDs, including gameplay
    // categories. Logical property writes are reported separately by the host.
    Fixture->ResetOperationCounts(INDEX_NONE);
    PreparedActor = Actor.Get();
    return true;
}

bool FAs36LeadershipBridge::ExecutePrepared(FString& OutError)
{
    if (!PreparedActor || bExecuted)
    {
        OutError = TEXT("Sample was not prepared or was already executed");
        return false;
    }
    if (!CheckLifetime(OutError)) { PreparedActor = nullptr; return false; }
    if (SelectedWorkload == 7)
    {
        for (int32 Index = 0; Index < SelectedIterations; ++Index)
        {
            *EmptyValue = Index;
            PreparedActor->ProcessEvent(Empty.Function, Empty.Memory());
        }
    }
    else if (SelectedWorkload == 8)
    {
        for (int32 Index = 0; Index < SelectedIterations; ++Index)
        {
            PreparedActor->ProcessEvent(Tick.Function, Tick.Memory());
        }
    }
    else
    {
        PreparedActor->ProcessEvent(Run.Function, Run.Memory());
    }
    bExecuted = true;
    return true;
}

bool FAs36LeadershipBridge::ReadChecksum(uint32& OutChecksum, FString& OutError)
{
    if (!PreparedActor || !bExecuted)
    {
        OutError = TEXT("Sample has not completed or its checksum was already consumed");
        return false;
    }
    if (!CheckLifetime(OutError)) { PreparedActor = nullptr; return false; }
    if (SelectedWorkload == 7 || SelectedWorkload == 8)
    {
        PreparedActor->ProcessEvent(Read.Function, Read.Memory());
        OutChecksum = static_cast<uint32>(*ReadResult);
    }
    else
    {
        OutChecksum = static_cast<uint32>(*RunResult);
    }
    PreparedActor = nullptr;
    return true;
}

FString FAs36LeadershipBridge::GetEntryIdentity() const
{
    return bBound && BoundClass.IsValid() ? BoundClass->GetPathName() + TEXT("; cached UASFunction via AActor::ProcessEvent") : FString();
}
