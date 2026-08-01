#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptActorBinding.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectOwnership.h"
#include "AvidScriptSceneComponentBinding.h"
#include "Invocation/AvidScriptBindingCodecProgram.h"
#include "Invocation/AvidScriptBindingPreparedInvocation.h"

#include "AvidScriptBindingsTestTypes.h"

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Script.h"
#include "UObject/UObjectGlobals.h"

namespace
{
class FAvidScriptBoundaryGuestMemory final : public IAvidScriptVmGuestMemory
{
public:
	FAvidScriptBoundaryGuestMemory()
	{
		Bytes.SetNumZeroed(512);
	}

	bool ReadBytes(
		uint32 GuestAddress,
		TArrayView<uint8> OutBytes,
		FString& OutError) override
	{
		if (GuestAddress > static_cast<uint32>(Bytes.Num())
			|| OutBytes.Num() > Bytes.Num() - static_cast<int32>(GuestAddress))
		{
			OutError = TEXT("guest read range");
			return false;
		}
		OutError.Reset();
		FMemory::Memcpy(OutBytes.GetData(), Bytes.GetData() + GuestAddress, OutBytes.Num());
		return true;
	}

	bool WriteBytes(
		uint32 GuestAddress,
		TConstArrayView<uint8> InBytes,
		FString& OutError) override
	{
		if (GuestAddress > static_cast<uint32>(this->Bytes.Num())
			|| InBytes.Num() > this->Bytes.Num() - static_cast<int32>(GuestAddress))
		{
			OutError = TEXT("guest write range");
			return false;
		}
		OutError.Reset();
		FMemory::Memcpy(
			this->Bytes.GetData() + GuestAddress,
			InBytes.GetData(),
			InBytes.Num());
		return true;
	}

	bool BorrowReadOnlyBytes(uint32 Address, uint32 Count, uint32 Alignment, TConstArrayView<uint8>& OutBytes, FString& OutError) override
	{
		if (Address > static_cast<uint32>(Bytes.Num()) || Count > static_cast<uint32>(Bytes.Num()) - Address)
		{
			OutError = TEXT("guest borrow range"); return false;
		}
		OutBytes = MakeArrayView(Bytes).Slice(Address, Count); OutError.Reset(); return true;
	}

	bool BorrowMutableBytes(uint32 Address, uint32 Count, uint32 Alignment, TArrayView<uint8>& OutBytes, FString& OutError) override
	{
		if (Address > static_cast<uint32>(Bytes.Num()) || Count > static_cast<uint32>(Bytes.Num()) - Address)
		{
			OutError = TEXT("guest borrow range"); return false;
		}
		OutBytes = MakeArrayView(Bytes).Slice(Address, Count); OutError.Reset(); return true;
	}

	TArray<uint8> Bytes;
};

class FAvidScriptBoundaryOwnership final : public IAvidScriptObjectOwnershipDomain
{
public:
	bool Adopt(FAvidScriptObjectRegistry&, UObject&, const FAvidScriptObjectHandle&, EAvidScriptObjectFactoryKind, FAvidScriptObjectHandleResult&) override { return false; }
	bool Borrow(FAvidScriptObjectRegistry& Registry, UObject& Object, FAvidScriptObjectHandleResult& OutResult) override
	{
		++BorrowCount;
		OutResult.Handle = Registry.AcquireBorrowedObject(&Object, OutResult, false);
		return OutResult.Handle.IsValid();
	}
	bool Release(const FAvidScriptObjectHandle& Handle, FAvidScriptObjectRegistry& Registry, FAvidScriptObjectHandleResult& OutResult) override
	{
		return Registry.ReleaseBorrowedHandle(Handle, OutResult, false);
	}
	bool Owns(const FAvidScriptObjectHandle&, const UObject*) const override { return false; }
	void Cleanup(FAvidScriptObjectRegistry&) override {}

	int32 BorrowCount = 0;
};

using namespace UE::AvidScript::BindingPrivate;

FValueCodecProgram MakeRecursiveStructCodec(FProperty* Property, EValueCodecDirection Direction, int32 ArgumentOffset)
{
	FValueCodecProgram Program;
	Program.Property = Property;
	Program.StructType = CastFieldChecked<FStructProperty>(Property)->Struct;
	Program.Direction = Direction;
	Program.Kind = EValueCodecKind::StructWire;
	Program.ArgumentOffset = ArgumentOffset;
	Program.ArgumentWidth = 1;
	Program.GuestStorageSize = 36;
	Program.WireSize = 36;
	Program.WireAlignment = 4;
	Program.Name = Property->GetName();
	const auto AddLeaf = [&Program](FProperty* Field, EValueCodecKind Kind, int32 Offset, int32 Size, UClass* ObjectClass = nullptr)
	{
		FValueCodecProgram& Child = Program.Children.AddDefaulted_GetRef();
		Child.Property = Field; Child.Kind = Kind; Child.ObjectClass = ObjectClass;
		Child.WireOffset = Offset; Child.WireSize = Size; Child.WireAlignment = 4; Child.GuestStorageSize = Size; Child.Name = Field->GetName();
	};
	AddLeaf(FindFProperty<FProperty>(Program.StructType, TEXT("bEnabled")), EValueCodecKind::Bool, 0, 4);
	AddLeaf(FindFProperty<FProperty>(Program.StructType, TEXT("Mode")), EValueCodecKind::Enum, 4, 4);
	AddLeaf(FindFProperty<FProperty>(Program.StructType, TEXT("Position")), EValueCodecKind::Vector, 8, 12);
	AddLeaf(FindFProperty<FProperty>(Program.StructType, TEXT("Target")), EValueCodecKind::Object, 20, 8, UObject::StaticClass());
	FProperty* NestedProperty = FindFProperty<FProperty>(Program.StructType, TEXT("Nested"));
	FValueCodecProgram& Nested = Program.Children.AddDefaulted_GetRef();
	Nested.Property = NestedProperty; Nested.StructType = CastFieldChecked<FStructProperty>(NestedProperty)->Struct;
	Nested.Kind = EValueCodecKind::StructWire; Nested.WireOffset = 28; Nested.WireSize = 8; Nested.WireAlignment = 4; Nested.GuestStorageSize = 8; Nested.Name = TEXT("Nested");
	for (const TPair<FString, EValueCodecKind>& Leaf : { TPair<FString, EValueCodecKind>(TEXT("Count"), EValueCodecKind::Int32), TPair<FString, EValueCodecKind>(TEXT("Ratio"), EValueCodecKind::Float) })
	{
		FValueCodecProgram& Child = Nested.Children.AddDefaulted_GetRef();
		Child.Property = FindFProperty<FProperty>(Nested.StructType, *Leaf.Key); Child.Kind = Leaf.Value;
		Child.WireOffset = Nested.Children.Num() == 1 ? 0 : 4; Child.WireSize = 4; Child.WireAlignment = 4; Child.GuestStorageSize = 4; Child.Name = Leaf.Key;
	}
	return Program;
}

FNativeFuncPtr GRecursiveStructOriginalNative = nullptr;
int32 GRecursiveStructNativeInvocationCount = 0;

void CountRecursiveStructNativeInvocation(
	UObject* Context,
	FFrame& Stack,
	RESULT_DECL)
{
	++GRecursiveStructNativeInvocationCount;
	check(GRecursiveStructOriginalNative != nullptr);
	GRecursiveStructOriginalNative(Context, Stack, RESULT_PARAM);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAvidScriptBindingsBoundarySmokeTest,
    "AvidScript.Architecture.Bindings.ModuleBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingsBoundarySmokeTest::RunTest(const FString& Parameters)
{
    FAvidScriptObjectRegistry Registry;
    TestEqual(TEXT("New binding registry has no live handles"), Registry.GetLiveHandleCount(), 0);

    UObject* Object = NewObject<UAvidScriptBindingsTestObject>();
    FAvidScriptObjectHandleResult FirstResult;
    const FAvidScriptObjectHandle FirstHandle = Registry.RegisterObject(Object, FirstResult);
    FAvidScriptObjectHandleResult DuplicateResult;
    const FAvidScriptObjectHandle DuplicateHandle = Registry.RegisterObject(Object, DuplicateResult);
    TestTrue(TEXT("First registration succeeds"), FirstResult.bSucceeded);
    TestTrue(TEXT("Duplicate registration succeeds"), DuplicateResult.bSucceeded);
    TestEqual(TEXT("Duplicate registration reuses slot"), DuplicateHandle.Slot, FirstHandle.Slot);
    TestEqual(TEXT("Duplicate registration reuses generation"), DuplicateHandle.Generation, FirstHandle.Generation);
    TestEqual(TEXT("Duplicate registration keeps one live handle"), Registry.GetLiveHandleCount(), 1);

    FAvidScriptObjectHandleResult ReleaseResult;
    TestTrue(TEXT("Release removes reverse index entry"), Registry.ReleaseHandle(FirstHandle, ReleaseResult));
    TestEqual(TEXT("Release clears live handle count"), Registry.GetLiveHandleCount(), 0);

    FAvidScriptObjectHandleResult ReregisterResult;
    const FAvidScriptObjectHandle ReregisteredHandle = Registry.RegisterObject(Object, ReregisterResult);
    TestEqual(TEXT("Reregister reuses free slot"), ReregisteredHandle.Slot, FirstHandle.Slot);
    TestNotEqual(TEXT("Reregister advances generation"), ReregisteredHandle.Generation, FirstHandle.Generation);
    TestEqual(TEXT("Reregister restores one live handle"), Registry.GetLiveHandleCount(), 1);

    UObject* BorrowedObject = NewObject<UAvidScriptBindingsTestObject>();
    FAvidScriptObjectHandleResult BorrowResult;
    const FAvidScriptObjectHandle FirstBorrow = Registry.AcquireBorrowedObject(
        BorrowedObject,
        BorrowResult,
        false);
    const FAvidScriptObjectHandle SecondBorrow = Registry.AcquireBorrowedObject(
        BorrowedObject,
        BorrowResult,
        false);
    TestEqual(TEXT("Borrowed leases reuse one handle"), SecondBorrow, FirstBorrow);
    TestEqual(TEXT("Borrowed leases add one live slot"), Registry.GetLiveHandleCount(), 2);
    TestTrue(TEXT("First borrowed release preserves the shared slot"),
        Registry.ReleaseBorrowedHandle(FirstBorrow, BorrowResult, false));
    TestEqual(TEXT("One borrowed lease keeps its slot live"), Registry.GetLiveHandleCount(), 2);
    TestTrue(TEXT("Second borrowed release frees an unanchored slot"),
        Registry.ReleaseBorrowedHandle(SecondBorrow, BorrowResult, false));
    TestEqual(TEXT("Borrowed-only slot returns to the free list"), Registry.GetLiveHandleCount(), 1);

    const FAvidScriptObjectHandle AnchoredBorrow = Registry.AcquireBorrowedObject(
        BorrowedObject,
        BorrowResult,
        false);
    FAvidScriptObjectHandleResult AnchorResult;
    TestEqual(TEXT("Registration anchors an existing borrowed slot"),
        Registry.RegisterObject(BorrowedObject, AnchorResult, false),
        AnchoredBorrow);
    TestTrue(TEXT("Borrow release preserves an anchored slot"),
        Registry.ReleaseBorrowedHandle(AnchoredBorrow, BorrowResult, false));
    TestEqual(TEXT("Anchored slot remains live after borrowed cleanup"), Registry.GetLiveHandleCount(), 2);
    TestTrue(TEXT("Ordinary release removes the anchored slot"),
        Registry.ReleaseHandle(AnchoredBorrow, AnchorResult, false));
    TestTrue(TEXT("Original fixture handle remains releasable"),
        Registry.ReleaseHandle(ReregisteredHandle, ReleaseResult, false));
    TestEqual(TEXT("Registry returns to zero live handles"), Registry.GetLiveHandleCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPreparedDynamicBoundaryTest,
	"AvidScript.Bindings.PreparedDynamic.Boundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPreparedDynamicBoundaryTest::RunTest(
	const FString& Parameters)
{
	UClass* ExpectedClass = UAvidScriptBindingsTestObject::StaticClass();
	UFunction* Function = ExpectedClass->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(
			UAvidScriptBindingsTestObject,
			ReflectionFallbackAddFloat));
	const TSharedPtr<const FAvidScriptBindingPackage> Package =
		FAvidScriptBindingPackage::MakePreparedDynamicPlanForTesting(
			TEXT("prepared-dynamic-test"),
			TEXT("avid_prepared_dynamic_test"),
			TEXT("(ii)i"),
			ExpectedClass,
			Function,
			2,
			64,
			true,
			true);
	if (!TestTrue(TEXT("Prepared test package is created"), Package.IsValid()))
	{
		return false;
	}

	TArray<FAvidScriptPreparedDynamicBinding> Bindings;
	FString Error;
	if (!TestTrue(
			TEXT("Prepared cells publish from the package"),
			Package->BuildPreparedDynamicBindings(Bindings, Error))
		|| !TestEqual(TEXT("One prepared cell is published"), Bindings.Num(), 1))
	{
		AddError(Error);
		return false;
	}
	const FAvidScriptPreparedDynamicBinding& Binding = Bindings[0];
	TestEqual(TEXT("Prepared ordinal is stable"), Binding.BindingOrdinal, 0u);
	TestEqual(
		TEXT("Prepared import identity is stable"),
		Binding.ImportName,
		FString(TEXT("avid_prepared_dynamic_test")));
	TestNotNull(
		TEXT("Prepared cell borrows package-owned immutable storage"),
		Binding.ImmutableInvocationCell);
	TestNotNull(TEXT("Prepared cell has an invoke entry"), Binding.Invoke);

	FAvidScriptBoundaryGuestMemory GuestMemory;
	FAvidScriptBindingInvocationContext InvocationContext;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(64);
	FAvidScriptDynamicHostCallResult Result;
	UAvidScriptBindingsTestObject* Receiver =
		NewObject<UAvidScriptBindingsTestObject>();
	const uint64 ValidArguments[] = { 0, 0 };
	const uint64 ShortArguments[] = { 0 };
	TestFalse(
		TEXT("Prepared invocation rejects an argument-count mismatch"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(ShortArguments),
			&GuestMemory,
			InvocationContext,
			Scratch,
			Result));
	TestTrue(
		TEXT("Argument mismatch reports the frame contract"),
		Result.Details.Contains(TEXT("binding_frame_mismatch")));

	TestFalse(
		TEXT("Prepared invocation rejects missing guest memory"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(ValidArguments),
			nullptr,
			InvocationContext,
			Scratch,
			Result));
	TestTrue(
		TEXT("Missing guest memory reports the frame contract"),
		Result.Details.Contains(TEXT("binding_frame_mismatch")));

	TArray<uint8> ShortScratch;
	ShortScratch.SetNumUninitialized(63);
	TestFalse(
		TEXT("Prepared invocation rejects insufficient scratch"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(ValidArguments),
			&GuestMemory,
			InvocationContext,
			ShortScratch,
			Result));
	TestTrue(
		TEXT("Insufficient scratch reports its contract"),
		Result.Details.Contains(TEXT("binding_scratch_too_small")));

	UObject* StaleOwner = UObject::StaticClass();
	TestFalse(
		TEXT("Prepared invocation rejects a stale owner type"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*StaleOwner,
			MakeArrayView(ValidArguments),
			&GuestMemory,
			InvocationContext,
			Scratch,
			Result));
	TestTrue(
		TEXT("Stale owner reports the target contract"),
		Result.Details.Contains(TEXT("binding_target_invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRecursiveStructCodecBoundaryTest,
	"AvidScript.Bindings.PreparedDynamic.RecursiveStructCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRecursiveStructCodecBoundaryTest::RunTest(const FString& Parameters)
{
	UFunction* Function = UAvidScriptBindingsTestObject::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UAvidScriptBindingsTestObject, RecursiveStructRoundtrip));
	if (!TestNotNull(TEXT("Recursive fixture function reflects"), Function))
	{
		return false;
	}
	TArray<FProperty*> ParametersByOrder;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			ParametersByOrder.Add(*It);
		}
	}
	if (!TestEqual(TEXT("Recursive fixture has three parameters and return"), ParametersByOrder.Num(), 4))
	{
		return false;
	}

	FInvocationCodecProgram Program;
	Program.OwnerClass = UAvidScriptBindingsTestObject::StaticClass();
	Program.Function = Function;
	Program.DebugPath = Function->GetPathName();
	Program.FrameSize = Function->GetStructureSize();
	Program.FrameAlignment = FMath::Max(1, Function->GetMinAlignment());
	Program.RequiredScratchSize = Program.FrameSize + Program.FrameAlignment - 1;
	Program.ExpectedArgumentCount = 6;
	Program.bRequiresGuestMemory = true;
	Program.Parameters.Add(MakeRecursiveStructCodec(ParametersByOrder[0], EValueCodecDirection::ConstRef, 2));
	Program.Parameters.Add(MakeRecursiveStructCodec(ParametersByOrder[1], EValueCodecDirection::Ref, 3));
	Program.Parameters.Add(MakeRecursiveStructCodec(ParametersByOrder[2], EValueCodecDirection::Out, 4));
	Program.ReturnValue = MakeRecursiveStructCodec(ParametersByOrder[3], EValueCodecDirection::Return, 5);
	FPreparedDynamicInvocationCell Cell{ &Program, 0 };

	FAvidScriptBoundaryGuestMemory GuestMemory;
	FAvidScriptObjectRegistry Registry;
	FAvidScriptBoundaryOwnership Ownership;
	UObject* Target = NewObject<UAvidScriptBindingsTestObject>();
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle TargetHandle = Registry.RegisterObject(Target, HandleResult, false);
	const auto StoreInput = [&GuestMemory, TargetHandle](const uint32 Address, const int32 Count, const float Ratio, const float X)
	{
		int32 Enabled = 1;
		int32 Mode = static_cast<int32>(EAvidScriptBindingsStructMode::Secondary);
		const float Position[3] = { X, X + 1.0f, X + 2.0f };
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address, &Enabled, 4);
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address + 4, &Mode, 4);
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address + 8, Position, 12);
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address + 20, &TargetHandle.Slot, 4);
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address + 24, &TargetHandle.Generation, 4);
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address + 28, &Count, 4);
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address + 32, &Ratio, 4);
	};
	StoreInput(32, 3, 1.5f, 2.0f);
	StoreInput(96, 4, 2.5f, 10.0f);

	FAvidScriptBindingInvocationInstrumentation Instrumentation;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.ObjectOwnership = &Ownership;
	Context.InvocationInstrumentation = &Instrumentation;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Program.RequiredScratchSize);
	UAvidScriptBindingsTestObject* Receiver = NewObject<UAvidScriptBindingsTestObject>();
	FAvidScriptDynamicHostCallResult Result;

	const FNativeFuncPtr OriginalNative = Function->GetNativeFunc();
	if (!TestTrue(TEXT("Recursive fixture has a native implementation"), OriginalNative != nullptr))
	{
		return false;
	}
	GRecursiveStructOriginalNative = OriginalNative;
	GRecursiveStructNativeInvocationCount = 0;
	{
		Function->SetNativeFunc(&CountRecursiveStructNativeInvocation);
		ON_SCOPE_EXIT
		{
			Function->SetNativeFunc(OriginalNative);
			GRecursiveStructOriginalNative = nullptr;
		};

		const int32 InitialBorrowCount = Ownership.BorrowCount;
		const int32 InitialLiveHandleCount = Registry.GetLiveHandleCount();
		const TArray<uint8> SameAddressGuestSnapshot = GuestMemory.Bytes;
		const uint64 SameAddressArguments[] = { 0, 0, 32, 96, 160, 160 };
		TestFalse(
			TEXT("Identical out and return ranges fail before ProcessEvent"),
			InvokePreparedDynamicReflection(
				&Cell,
				*Receiver,
				SameAddressArguments,
				&GuestMemory,
				Context,
				Scratch,
				Result));
		TestTrue(
			TEXT("Identical output ranges report the overlap contract"),
			Result.Details.Contains(TEXT("binding_guest_output_overlap")));
		TestEqual(
			TEXT("Identical output ranges do not execute the function"),
			GRecursiveStructNativeInvocationCount,
			0);
		TestEqual(
			TEXT("Identical output ranges do not acquire object borrows"),
			Ownership.BorrowCount,
			InitialBorrowCount);
		TestEqual(
			TEXT("Identical output ranges do not create ownership leases"),
			Registry.GetLiveHandleCount(),
			InitialLiveHandleCount);
		TestTrue(
			TEXT("Identical output ranges leave guest memory unchanged"),
			GuestMemory.Bytes == SameAddressGuestSnapshot);

		const TArray<uint8> PartialGuestSnapshot = GuestMemory.Bytes;
		const uint64 PartialArguments[] = { 0, 0, 32, 96, 160, 180 };
		TestFalse(
			TEXT("Partially overlapping out and return ranges fail before ProcessEvent"),
			InvokePreparedDynamicReflection(
				&Cell,
				*Receiver,
				PartialArguments,
				&GuestMemory,
				Context,
				Scratch,
				Result));
		TestTrue(
			TEXT("Partial output overlap reports the overlap contract"),
			Result.Details.Contains(TEXT("binding_guest_output_overlap")));
		TestEqual(
			TEXT("Partial output overlap does not execute the function"),
			GRecursiveStructNativeInvocationCount,
			0);
		TestEqual(
			TEXT("Partial output overlap does not acquire object borrows"),
			Ownership.BorrowCount,
			InitialBorrowCount);
		TestEqual(
			TEXT("Partial output overlap does not create ownership leases"),
			Registry.GetLiveHandleCount(),
			InitialLiveHandleCount);
		TestTrue(
			TEXT("Partial output overlap leaves guest memory unchanged"),
			GuestMemory.Bytes == PartialGuestSnapshot);
	}

	const uint64 Arguments[] = { 0, 0, 32, 96, 132, 168 };
	if (!TestTrue(
		TEXT("Adjacent ref, out and return ranges remain valid"),
		InvokePreparedDynamicReflection(&Cell, *Receiver, Arguments, &GuestMemory, Context, Scratch, Result)))
	{
		AddError(Result.Details);
		return false;
	}
	int32 InOutCount = 0;
	float InOutRatio = 0.0f;
	FMemory::Memcpy(&InOutCount, GuestMemory.Bytes.GetData() + 96 + 28, 4);
	FMemory::Memcpy(&InOutRatio, GuestMemory.Bytes.GetData() + 96 + 32, 4);
	TestEqual(TEXT("Nested ref count encodes"), InOutCount, 7);
	TestEqual(TEXT("Nested ref ratio encodes"), InOutRatio, 4.0f);
	TestEqual(TEXT("Prepared path records one ProcessEvent"), Instrumentation.SemanticProcessEventCount, 1ull);
	uint32 OutputSlot = 0;
	uint32 OutputGeneration = 0;
	FMemory::Memcpy(&OutputSlot, GuestMemory.Bytes.GetData() + 132 + 20, 4);
	FMemory::Memcpy(&OutputGeneration, GuestMemory.Bytes.GetData() + 132 + 24, 4);
	UObject* ResolvedOutput = nullptr;
	FString ResolveDetails;
	TestTrue(TEXT("Nested object output publishes a capability"), ResolveObjectHandle(OutputSlot, OutputGeneration, UObject::StaticClass(), Context, false, ResolvedOutput, ResolveDetails));
	TestEqual(TEXT("Nested object output resolves to the fixture target"), ResolvedOutput, Target);

	const uint64 MalformedArguments[] = { 0, 0, 32, 96, 160, static_cast<uint64>(MAX_uint32) + 1 };
	TestFalse(TEXT("Oversized return address fails before ProcessEvent"), InvokePreparedDynamicReflection(&Cell, *Receiver, MalformedArguments, &GuestMemory, Context, Scratch, Result));
	TestTrue(TEXT("Malformed address reports return preflight"), Result.Details.Contains(TEXT("binding_return_preflight_failed")));
	TestEqual(TEXT("Malformed address does not replay ProcessEvent"), Instrumentation.SemanticProcessEventCount, 1ull);

	FProperty* RecursiveProperty = FindFProperty<FProperty>(
		UAvidScriptBindingsTestObject::StaticClass(),
		TEXT("RecursiveStructProperty"));
	if (!TestNotNull(TEXT("Recursive struct property reflects"), RecursiveProperty))
	{
		return false;
	}
	FInvocationCodecProgram PropertyWriteProgram;
	PropertyWriteProgram.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
	PropertyWriteProgram.OwnerClass = UAvidScriptBindingsTestObject::StaticClass();
	PropertyWriteProgram.ReflectedProperty = RecursiveProperty;
	PropertyWriteProgram.DebugPath = RecursiveProperty->GetPathName();
	PropertyWriteProgram.FrameSize = CastFieldChecked<FStructProperty>(RecursiveProperty)->Struct->GetStructureSize();
	PropertyWriteProgram.FrameAlignment = FMath::Max(
		1,
		CastFieldChecked<FStructProperty>(RecursiveProperty)->Struct->GetMinAlignment());
	PropertyWriteProgram.RequiredScratchSize =
		PropertyWriteProgram.FrameSize + PropertyWriteProgram.FrameAlignment - 1;
	PropertyWriteProgram.ExpectedArgumentCount = 3;
	PropertyWriteProgram.bRequiresGuestMemory = true;
	PropertyWriteProgram.Parameters.Add(
		MakeRecursiveStructCodec(RecursiveProperty, EValueCodecDirection::Value, 2));
	FPreparedDynamicInvocationCell PropertyWriteCell{ &PropertyWriteProgram, 0 };
	const uint64 PropertyWriteArguments[] = { 0, 0, 32 };
	TestTrue(
		TEXT("Recursive direct property write commits from a fully decoded temporary"),
		InvokePreparedDynamicReflection(
			&PropertyWriteCell,
			*Receiver,
			PropertyWriteArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(TEXT("Recursive property write commits nested count"), Receiver->RecursiveStructProperty.Nested.Count, 3);
	TestEqual(TEXT("Recursive property write commits FVector"), Receiver->RecursiveStructProperty.Position.X, 2.0);

	StoreInput(288, 99, 9.0f, 20.0f);
	const uint32 InvalidSlot = MAX_uint32;
	const uint32 InvalidGeneration = MAX_uint32;
	FMemory::Memcpy(GuestMemory.Bytes.GetData() + 288 + 20, &InvalidSlot, 4);
	FMemory::Memcpy(GuestMemory.Bytes.GetData() + 288 + 24, &InvalidGeneration, 4);
	const uint64 InvalidPropertyWriteArguments[] = { 0, 0, 288 };
	TestFalse(
		TEXT("Recursive property write rejects an invalid nested object capability"),
		InvokePreparedDynamicReflection(
			&PropertyWriteCell,
			*Receiver,
			InvalidPropertyWriteArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(
		TEXT("Failed recursive property decode leaves the destination unchanged"),
		Receiver->RecursiveStructProperty.Nested.Count,
		3);

	FInvocationCodecProgram PropertyReadProgram;
	PropertyReadProgram.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyRead;
	PropertyReadProgram.OwnerClass = UAvidScriptBindingsTestObject::StaticClass();
	PropertyReadProgram.ReflectedProperty = RecursiveProperty;
	PropertyReadProgram.DebugPath = RecursiveProperty->GetPathName();
	PropertyReadProgram.ExpectedArgumentCount = 3;
	PropertyReadProgram.bRequiresGuestMemory = true;
	PropertyReadProgram.ReturnValue =
		MakeRecursiveStructCodec(RecursiveProperty, EValueCodecDirection::Return, 2);
	FPreparedDynamicInvocationCell PropertyReadCell{ &PropertyReadProgram, 0 };
	const uint64 PropertyReadArguments[] = { 0, 0, 352 };
	TestTrue(
		TEXT("Recursive direct property read encodes the fixed wire graph"),
		InvokePreparedDynamicReflection(
			&PropertyReadCell,
			*Receiver,
			PropertyReadArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	int32 PropertyReadCount = 0;
	FMemory::Memcpy(&PropertyReadCount, GuestMemory.Bytes.GetData() + 352 + 28, 4);
	TestEqual(TEXT("Recursive property read preserves nested count"), PropertyReadCount, 3);
	return true;
}

#endif
