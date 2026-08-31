#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptActorBinding.h"
#include "AvidScriptArrayValueHeap.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectOwnership.h"
#include "AvidScriptSceneComponentBinding.h"
#include "AvidScriptUtf8ValueHeap.h"
#include "Invocation/AvidScriptBindingCodecProgram.h"
#include "Invocation/AvidScriptBindingPreparedInvocation.h"

#include "AvidScriptBindingsTestTypes.h"

#include "Containers/StringConv.h"
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
		if (bRejectWrites)
		{
			OutError = TEXT("guest write rejected");
			return false;
		}
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
		++MutableBorrowCallCount;
		if (MutableBorrowCallCount == FailMutableBorrowCall)
		{
			OutBytes = TArrayView<uint8>();
			OutError = TEXT("guest mutable borrow rejected");
			return false;
		}
		if (Address > static_cast<uint32>(Bytes.Num()) || Count > static_cast<uint32>(Bytes.Num()) - Address)
		{
			OutError = TEXT("guest borrow range"); return false;
		}
		OutBytes = MakeArrayView(Bytes).Slice(Address, Count); OutError.Reset(); return true;
	}

	TArray<uint8> Bytes;
	int32 MutableBorrowCallCount = 0;
	int32 FailMutableBorrowCall = INDEX_NONE;
	bool bRejectWrites = false;
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

FValueCodecProgram MakeUtf8Codec(
	FProperty* Property,
	const EValueCodecDirection Direction,
	const int32 ArgumentOffset,
	const EValueCodecKind Kind)
{
	FValueCodecProgram Program;
	Program.Property = Property;
	Program.Direction = Direction;
	Program.Kind = Kind;
	Program.ArgumentOffset = ArgumentOffset;
	Program.ArgumentWidth = 1;
	Program.GuestStorageSize = sizeof(uint32);
	Program.WireSize = sizeof(uint32);
	Program.WireAlignment = alignof(uint32);
	Program.Name = Property == nullptr ? TEXT("ReturnValue") : Property->GetName();
	return Program;
}

FValueCodecProgram MakeIntArrayCodec(
	FProperty* Property,
	const EValueCodecDirection Direction,
	const int32 ArgumentOffset)
{
	FValueCodecProgram Program;
	Program.Property = Property;
	Program.Direction = Direction;
	Program.Kind = EValueCodecKind::Array;
	Program.TypeId = FString::ChrN(64, TEXT('a'));
	Program.ArgumentOffset = ArgumentOffset;
	Program.ArgumentWidth = 1;
	Program.GuestStorageSize = sizeof(uint32);
	Program.WireSize = sizeof(uint32);
	Program.WireAlignment = alignof(uint32);
	Program.Name = Property == nullptr ? TEXT("ReturnValue") : Property->GetName();

	FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(Property);
	FValueCodecProgram& Element = Program.Children.AddDefaulted_GetRef();
	Element.Property = ArrayProperty->Inner;
	Element.Kind = EValueCodecKind::Int32;
	Element.GuestStorageSize = sizeof(int32);
	Element.WireSize = sizeof(int32);
	Element.WireAlignment = alignof(int32);
	Element.Name = TEXT("Element");
	return Program;
}

FValueCodecProgram MakeInt32Codec(
	FProperty* Property,
	const EValueCodecDirection Direction,
	const int32 ArgumentOffset)
{
	FValueCodecProgram Program;
	Program.Property = Property;
	Program.Direction = Direction;
	Program.Kind = EValueCodecKind::Int32;
	Program.ArgumentOffset = ArgumentOffset;
	Program.ArgumentWidth = 1;
	Program.GuestStorageSize = sizeof(int32);
	Program.WireSize = sizeof(int32);
	Program.WireAlignment = alignof(int32);
	Program.Name = Property == nullptr ? TEXT("ReturnValue") : Property->GetName();
	return Program;
}

FValueCodecProgram MakeInterfaceCodec(
	FProperty* Property,
	const EValueCodecDirection Direction,
	const int32 ArgumentOffset)
{
	FValueCodecProgram Program;
	Program.Property = Property;
	Program.ObjectClass = UAvidScriptBindingsCallableInterface::StaticClass();
	Program.Direction = Direction;
	Program.Kind = EValueCodecKind::Interface;
	Program.ArgumentOffset = ArgumentOffset;
	Program.ArgumentWidth = Direction == EValueCodecDirection::Return ? 1 : 2;
	Program.GuestStorageSize = sizeof(FAvidScriptObjectHandle);
	Program.WireSize = sizeof(FAvidScriptObjectHandle);
	Program.WireAlignment = alignof(uint32);
	Program.Name = Property == nullptr ? TEXT("ReturnValue") : Property->GetName();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptArrayValueBindingBoundaryTest,
	"AvidScript.Bindings.ArrayValueHeap.CrossInvocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptArrayValueBindingBoundaryTest::RunTest(
	const FString& Parameters)
{
	UClass* ExpectedClass = UAvidScriptBindingsTestObject::StaticClass();
	UFunction* Function = ExpectedClass->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(
			UAvidScriptBindingsTestObject,
			IntArrayRoundtrip));
	if (!TestNotNull(TEXT("Array fixture function reflects"), Function))
	{
		return false;
	}
	FProperty* InputProperty = FindFProperty<FProperty>(Function, TEXT("Input"));
	FProperty* InOutProperty = FindFProperty<FProperty>(Function, TEXT("InOut"));
	FProperty* OutProperty = FindFProperty<FProperty>(Function, TEXT("OutValue"));
	FProperty* ReturnProperty = Function->GetReturnProperty();
	if (!TestNotNull(TEXT("Array input reflects"), InputProperty)
		|| !TestNotNull(TEXT("Array ref input reflects"), InOutProperty)
		|| !TestNotNull(TEXT("Array output reflects"), OutProperty)
		|| !TestNotNull(TEXT("Array return reflects"), ReturnProperty))
	{
		return false;
	}

	FInvocationCodecProgram Program;
	Program.OwnerClass = ExpectedClass;
	Program.Function = Function;
	Program.DebugPath = Function->GetPathName();
	Program.FrameSize = Function->GetStructureSize();
	Program.FrameAlignment = FMath::Max(1, Function->GetMinAlignment());
	Program.RequiredScratchSize = Program.FrameSize + Program.FrameAlignment - 1;
	Program.ExpectedArgumentCount = 6;
	Program.bRequiresGuestMemory = true;
	Program.Parameters.Add(MakeIntArrayCodec(InputProperty, EValueCodecDirection::ConstRef, 2));
	Program.Parameters.Add(MakeIntArrayCodec(InOutProperty, EValueCodecDirection::Ref, 3));
	Program.Parameters.Add(MakeIntArrayCodec(OutProperty, EValueCodecDirection::Out, 4));
	Program.ReturnValue = MakeIntArrayCodec(ReturnProperty, EValueCodecDirection::Return, 5);
	FPreparedDynamicInvocationCell Cell{ &Program, 0 };

	FAvidScriptBoundaryGuestMemory GuestMemory;
	const auto StoreLinearArray = [&GuestMemory](
		const uint32 Address,
		const TConstArrayView<int32> Values)
	{
		const int32 Count = Values.Num();
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address, &Count, sizeof(Count));
		if (!Values.IsEmpty())
		{
			FMemory::Memcpy(
				GuestMemory.Bytes.GetData() + Address + sizeof(Count),
				Values.GetData(),
				Values.Num() * sizeof(int32));
		}
	};
	const auto StoreValueReference = [&GuestMemory](
		const uint32 Address,
		const uint32 ValueReference)
	{
		FMemory::Memcpy(
			GuestMemory.Bytes.GetData() + Address,
			&ValueReference,
			sizeof(ValueReference));
	};
	const int32 InputValues[] = { 1, 2 };
	const int32 RefValues[] = { 10 };
	StoreLinearArray(32, MakeArrayView(InputValues));
	StoreLinearArray(96, MakeArrayView(RefValues));
	StoreValueReference(320, 96);

	FAvidScriptArrayValueHeap Heap;
	FAvidScriptBindingInvocationContext Context;
	Context.ArrayValueHeap = &Heap;
	TArray<uint8> Scratch;
	Scratch.SetNumZeroed(Program.RequiredScratchSize);
	UAvidScriptBindingsTestObject* Receiver =
		NewObject<UAvidScriptBindingsTestObject>();
	FAvidScriptDynamicHostCallResult Result;
	const uint64 Arguments[] = { 0, 0, 32, 320, 324, 328 };
	if (!TestTrue(
			TEXT("Array function accepts linear values and publishes capabilities"),
			InvokePreparedDynamicReflection(
				&Cell,
				*Receiver,
				Arguments,
				&GuestMemory,
				Context,
				Scratch,
				Result)))
	{
		AddError(Result.Details);
		return false;
	}

	uint32 InOutToken = 0;
	uint32 OutToken = 0;
	uint32 ReturnToken = 0;
	FMemory::Memcpy(&InOutToken, GuestMemory.Bytes.GetData() + 320, sizeof(uint32));
	FMemory::Memcpy(&OutToken, GuestMemory.Bytes.GetData() + 324, sizeof(uint32));
	FMemory::Memcpy(&ReturnToken, GuestMemory.Bytes.GetData() + 328, sizeof(uint32));
	TestTrue(TEXT("Ref array returns a capability"), InOutToken > MAX_int32);
	TestTrue(TEXT("Out array returns a capability"), OutToken > MAX_int32);
	TestTrue(TEXT("Return array returns a capability"), ReturnToken > MAX_int32);
	TestEqual(TEXT("Three array outputs remain live for cross-invocation use"), Heap.GetStats().LiveValues, 3);

	const auto ResolveIntArray = [&Heap](
		const uint32 Token,
		TArray<int32>& OutValues)
	{
		FAvidScriptArrayValueView View;
		FString Error;
		if (!Heap.Resolve(Token, FString::ChrN(64, TEXT('a')), View, Error)
			|| View.ElementStride != sizeof(int32))
		{
			return false;
		}
		OutValues.SetNumUninitialized(View.ElementCount);
		if (!OutValues.IsEmpty())
		{
			FMemory::Memcpy(
				OutValues.GetData(),
				View.Bytes.GetData(),
				View.Bytes.Num());
		}
		return true;
	};
	TArray<int32> Resolved;
	TestTrue(TEXT("Ref array capability resolves"), ResolveIntArray(InOutToken, Resolved));
	TestEqual(TEXT("Ref array preserves appended value count"), Resolved.Num(), 3);
	if (!Resolved.IsEmpty())
	{
		TestEqual(TEXT("Ref array preserves first value"), Resolved[0], 10);
	}
	TestTrue(TEXT("Out array capability resolves"), ResolveIntArray(OutToken, Resolved));
	TestEqual(TEXT("Out array preserves input count"), Resolved.Num(), 2);
	TestTrue(TEXT("Return array capability resolves"), ResolveIntArray(ReturnToken, Resolved));
	TestEqual(TEXT("Return array preserves appended value count"), Resolved.Num(), 3);

	FProperty* ArrayProperty = FindFProperty<FProperty>(ExpectedClass, TEXT("IntArrayProperty"));
	if (!TestNotNull(TEXT("Array property reflects"), ArrayProperty))
	{
		return false;
	}
	FInvocationCodecProgram PropertyWriteProgram;
	PropertyWriteProgram.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
	PropertyWriteProgram.OwnerClass = ExpectedClass;
	PropertyWriteProgram.ReflectedProperty = ArrayProperty;
	PropertyWriteProgram.DebugPath = ArrayProperty->GetPathName();
	PropertyWriteProgram.ExpectedArgumentCount = 3;
	PropertyWriteProgram.bRequiresGuestMemory = true;
	PropertyWriteProgram.Parameters.Add(
		MakeIntArrayCodec(ArrayProperty, EValueCodecDirection::Value, 2));
	FPreparedDynamicInvocationCell PropertyWriteCell{ &PropertyWriteProgram, 0 };
	const uint64 TokenWriteArguments[] = { 0, 0, ReturnToken };
	TestTrue(
		TEXT("A later invocation accepts the returned array capability"),
		InvokePreparedDynamicReflection(
			&PropertyWriteCell,
			*Receiver,
			TokenWriteArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(TEXT("Capability input reaches the reflected array property"), Receiver->IntArrayProperty.Num(), 3);

	FString ReleaseError;
	TestTrue(TEXT("Ref array capability releases explicitly"), Heap.ReleaseValue(InOutToken, ReleaseError));
	TestTrue(TEXT("Out array capability releases explicitly"), Heap.ReleaseValue(OutToken, ReleaseError));
	TestTrue(TEXT("Return array capability releases explicitly"), Heap.ReleaseValue(ReturnToken, ReleaseError));
	TestEqual(TEXT("Explicit release leaves no live array capabilities"), Heap.GetStats().LiveValues, 0);
	FAvidScriptArrayValueView StaleView;
	TestFalse(
		TEXT("Released array capability becomes stale"),
		Heap.Resolve(ReturnToken, FString(), StaleView, ReleaseError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptInterfaceInteropBoundaryTest,
	"AvidScript.Bindings.PreparedDynamic.InterfaceInterop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptInterfaceInteropBoundaryTest::RunTest(const FString& Parameters)
{
	UClass* InterfaceClass = UAvidScriptBindingsCallableInterface::StaticClass();
	UAvidScriptBindingsInterfaceImplementer* Implementer =
		NewObject<UAvidScriptBindingsInterfaceImplementer>();
	UAvidScriptBindingsTestObject* PlainObject =
		NewObject<UAvidScriptBindingsTestObject>();
	if (!TestTrue(
			TEXT("Fixture implements the reflected interface"),
			Implementer->GetClass()->ImplementsInterface(InterfaceClass)))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptBoundaryOwnership Ownership;
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle ImplementerHandle =
		Registry.RegisterObject(Implementer, HandleResult, false);
	const FAvidScriptObjectHandle PlainHandle =
		Registry.RegisterObject(PlainObject, HandleResult, false);
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.ObjectOwnership = &Ownership;

	UObject* Resolved = nullptr;
	FString Details;
	TestTrue(
		TEXT("Interface-typed handle accepts an implementation object"),
		ResolveObjectHandle(
			ImplementerHandle.Slot,
			ImplementerHandle.Generation,
			InterfaceClass,
			Context,
			false,
			Resolved,
			Details));
	TestTrue(
		TEXT("Resolved interface keeps UObject identity"),
		Resolved == Implementer);
	TestFalse(
		TEXT("Interface-typed handle rejects a non-implementer"),
		ResolveObjectHandle(
			PlainHandle.Slot,
			PlainHandle.Generation,
			InterfaceClass,
			Context,
			false,
			Resolved,
			Details));

	FProperty* InterfaceProperty = FindFProperty<FProperty>(
		UAvidScriptBindingsTestObject::StaticClass(),
		TEXT("InterfaceProperty"));
	if (!TestTrue(
			TEXT("Interface property reflects as FInterfaceProperty"),
			InterfaceProperty != nullptr && InterfaceProperty->IsA<FInterfaceProperty>()))
	{
		return false;
	}

	FInvocationCodecProgram PropertyWriteProgram;
	PropertyWriteProgram.Kind =
		EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
	PropertyWriteProgram.OwnerClass = UAvidScriptBindingsTestObject::StaticClass();
	PropertyWriteProgram.ReflectedProperty = InterfaceProperty;
	PropertyWriteProgram.DebugPath = InterfaceProperty->GetPathName();
	PropertyWriteProgram.ExpectedArgumentCount = 4;
	PropertyWriteProgram.Parameters.Add(MakeInterfaceCodec(
		InterfaceProperty,
		EValueCodecDirection::Value,
		2));
	FPreparedDynamicInvocationCell PropertyWriteCell{ &PropertyWriteProgram, 0 };
	TArray<uint8> EmptyScratch;
	FAvidScriptDynamicHostCallResult Result;
	const uint64 PropertyWriteArguments[] = {
		0,
		0,
		ImplementerHandle.Slot,
		ImplementerHandle.Generation
	};
	if (!TestTrue(
			TEXT("Prepared interface property write accepts the capability"),
			InvokePreparedDynamicReflection(
				&PropertyWriteCell,
				*PlainObject,
				PropertyWriteArguments,
				nullptr,
				Context,
				EmptyScratch,
				Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestTrue(
		TEXT("Interface property stores the implementation UObject"),
		PlainObject->InterfaceProperty.GetObject() == Implementer);

	FInvocationCodecProgram PropertyReadProgram;
	PropertyReadProgram.Kind =
		EAvidScriptBindingInvocationKind::ReflectedPropertyRead;
	PropertyReadProgram.OwnerClass = UAvidScriptBindingsTestObject::StaticClass();
	PropertyReadProgram.ReflectedProperty = InterfaceProperty;
	PropertyReadProgram.DebugPath = InterfaceProperty->GetPathName();
	PropertyReadProgram.ExpectedArgumentCount = 3;
	PropertyReadProgram.bRequiresGuestMemory = true;
	PropertyReadProgram.ReturnValue = MakeInterfaceCodec(
		InterfaceProperty,
		EValueCodecDirection::Return,
		2);
	FPreparedDynamicInvocationCell PropertyReadCell{ &PropertyReadProgram, 0 };
	FAvidScriptBoundaryGuestMemory GuestMemory;
	const uint64 PropertyReadArguments[] = { 0, 0, 32 };
	if (!TestTrue(
			TEXT("Prepared interface property read publishes a capability"),
			InvokePreparedDynamicReflection(
				&PropertyReadCell,
				*PlainObject,
				PropertyReadArguments,
				&GuestMemory,
				Context,
				EmptyScratch,
				Result)))
	{
		AddError(Result.Details);
		return false;
	}
	FAvidScriptObjectHandle EncodedHandle;
	FMemory::Memcpy(
		&EncodedHandle,
		GuestMemory.Bytes.GetData() + 32,
		sizeof(EncodedHandle));
	TestEqual(TEXT("Interface output preserves the object slot"), EncodedHandle.Slot, ImplementerHandle.Slot);
	TestEqual(
		TEXT("Interface output preserves the object generation"),
		EncodedHandle.Generation,
		ImplementerHandle.Generation);

	UFunction* InterfaceFunction = InterfaceClass->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(
			IAvidScriptBindingsCallableInterface,
			TransformInterfaceValue));
	if (!TestNotNull(TEXT("Interface function reflects"), InterfaceFunction))
	{
		return false;
	}
	FProperty* InputProperty = FindFProperty<FProperty>(InterfaceFunction, TEXT("Value"));
	FProperty* ReturnProperty = InterfaceFunction->GetReturnProperty();
	if (!TestNotNull(TEXT("Interface input reflects"), InputProperty)
		|| !TestNotNull(TEXT("Interface return reflects"), ReturnProperty))
	{
		return false;
	}

	FInvocationCodecProgram FunctionProgram;
	FunctionProgram.OwnerClass = InterfaceClass;
	FunctionProgram.Function = InterfaceFunction;
	FunctionProgram.DebugPath = InterfaceFunction->GetPathName();
	FunctionProgram.FrameSize = InterfaceFunction->GetStructureSize();
	FunctionProgram.FrameAlignment = FMath::Max(
		1,
		InterfaceFunction->GetMinAlignment());
	FunctionProgram.RequiredScratchSize =
		FunctionProgram.FrameSize + FunctionProgram.FrameAlignment - 1;
	FunctionProgram.ExpectedArgumentCount = 4;
	FunctionProgram.bRequiresGuestMemory = true;
	FunctionProgram.Parameters.Add(MakeInt32Codec(
		InputProperty,
		EValueCodecDirection::Value,
		2));
	FunctionProgram.ReturnValue = MakeInt32Codec(
		ReturnProperty,
		EValueCodecDirection::Return,
		3);
	FPreparedDynamicInvocationCell FunctionCell{ &FunctionProgram, 0 };
	TArray<uint8> FunctionScratch;
	FunctionScratch.SetNumZeroed(FunctionProgram.RequiredScratchSize);
	const uint64 FunctionArguments[] = { 0, 0, 5, 64 };
	if (!TestTrue(
			TEXT("Prepared interface call reaches the concrete implementation"),
			InvokePreparedDynamicReflection(
				&FunctionCell,
				*Implementer,
				FunctionArguments,
				&GuestMemory,
				Context,
				FunctionScratch,
				Result)))
	{
		AddError(Result.Details);
		return false;
	}
	int32 ReturnValue = 0;
	FMemory::Memcpy(
		&ReturnValue,
		GuestMemory.Bytes.GetData() + 64,
		sizeof(ReturnValue));
	TestEqual(TEXT("Interface return value reaches guest memory"), ReturnValue, 16);
	TestEqual(TEXT("Interface implementation runs once"), Implementer->InvocationCount, 1);
	TestEqual(
		TEXT("Interface dispatch caches the concrete receiver class"),
		FunctionProgram.CachedInterfaceReceiverClass,
		Implementer->GetClass());
	TestNotNull(
		TEXT("Interface dispatch caches the resolved function"),
		FunctionProgram.CachedInterfaceFunction);

	const uint64 CachedArguments[] = { 0, 0, 7, 68 };
	TestTrue(
		TEXT("A repeated interface call reuses the prepared implementation"),
		InvokePreparedDynamicReflection(
			&FunctionCell,
			*Implementer,
			CachedArguments,
			&GuestMemory,
			Context,
			FunctionScratch,
			Result));
	FMemory::Memcpy(
		&ReturnValue,
		GuestMemory.Bytes.GetData() + 68,
		sizeof(ReturnValue));
	TestEqual(TEXT("Cached interface call preserves behavior"), ReturnValue, 22);
	TestEqual(TEXT("Cached interface call runs once more"), Implementer->InvocationCount, 2);

	TestFalse(
		TEXT("Prepared interface call rejects a non-implementer"),
		InvokePreparedDynamicReflection(
			&FunctionCell,
			*PlainObject,
			FunctionArguments,
			&GuestMemory,
			Context,
			FunctionScratch,
			Result));
	TestTrue(
		TEXT("Non-implementer reports the receiver contract"),
		Result.Details.Contains(TEXT("binding_target_invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptUtf8ValueBindingBoundaryTest,
	"AvidScript.Bindings.Utf8ValueHeap.CrossInvocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptUtf8ValueBindingBoundaryTest::RunTest(
	const FString& Parameters)
{
	UClass* ExpectedClass = UAvidScriptBindingsTestObject::StaticClass();
	UFunction* Function = ExpectedClass->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(
			UAvidScriptBindingsTestObject,
			Utf8Roundtrip));
	if (!TestNotNull(TEXT("UTF-8 fixture function reflects"), Function))
	{
		return false;
	}
	FProperty* InputNameProperty = FindFProperty<FProperty>(Function, TEXT("InputName"));
	FProperty* InputStringProperty = FindFProperty<FProperty>(Function, TEXT("InputString"));
	FProperty* InOutNameProperty = FindFProperty<FProperty>(Function, TEXT("InOutName"));
	FProperty* InOutStringProperty = FindFProperty<FProperty>(Function, TEXT("InOutString"));
	FProperty* OutNameProperty = FindFProperty<FProperty>(Function, TEXT("OutName"));
	FProperty* ReturnProperty = Function->GetReturnProperty();
	if (!TestNotNull(TEXT("UTF-8 input FName reflects"), InputNameProperty)
		|| !TestNotNull(TEXT("UTF-8 input FString reflects"), InputStringProperty)
		|| !TestNotNull(TEXT("UTF-8 ref FName reflects"), InOutNameProperty)
		|| !TestNotNull(TEXT("UTF-8 ref FString reflects"), InOutStringProperty)
		|| !TestNotNull(TEXT("UTF-8 out FName reflects"), OutNameProperty)
		|| !TestNotNull(TEXT("UTF-8 FString return reflects"), ReturnProperty))
	{
		return false;
	}

	FInvocationCodecProgram Program;
	Program.OwnerClass = ExpectedClass;
	Program.Function = Function;
	Program.DebugPath = Function->GetPathName();
	Program.FrameSize = Function->GetStructureSize();
	Program.FrameAlignment = FMath::Max(1, Function->GetMinAlignment());
	Program.RequiredScratchSize = Program.FrameSize + Program.FrameAlignment - 1;
	Program.ExpectedArgumentCount = 8;
	Program.bRequiresGuestMemory = true;
	Program.Parameters.Add(MakeUtf8Codec(InputNameProperty, EValueCodecDirection::ConstRef, 2, EValueCodecKind::Name));
	Program.Parameters.Add(MakeUtf8Codec(InputStringProperty, EValueCodecDirection::ConstRef, 3, EValueCodecKind::String));
	Program.Parameters.Add(MakeUtf8Codec(InOutNameProperty, EValueCodecDirection::Ref, 4, EValueCodecKind::Name));
	Program.Parameters.Add(MakeUtf8Codec(InOutStringProperty, EValueCodecDirection::Ref, 5, EValueCodecKind::String));
	Program.Parameters.Add(MakeUtf8Codec(OutNameProperty, EValueCodecDirection::Out, 6, EValueCodecKind::Name));
	Program.ReturnValue = MakeUtf8Codec(ReturnProperty, EValueCodecDirection::Return, 7, EValueCodecKind::String);
	FPreparedDynamicInvocationCell Cell{ &Program, 0 };

	FAvidScriptBoundaryGuestMemory GuestMemory;
	const auto StoreLinearUtf8 = [&GuestMemory](
		const uint32 Address,
		const TConstArrayView<uint8> Bytes)
	{
		const uint32 Length = static_cast<uint32>(Bytes.Num());
		FMemory::Memcpy(GuestMemory.Bytes.GetData() + Address, &Length, sizeof(Length));
		if (!Bytes.IsEmpty())
		{
			FMemory::Memcpy(
				GuestMemory.Bytes.GetData() + Address + sizeof(Length),
				Bytes.GetData(),
				Bytes.Num());
		}
		GuestMemory.Bytes[Address + sizeof(Length) + Bytes.Num()] = 0;
	};
	const auto StoreValueReference = [&GuestMemory](
		const uint32 Address,
		const uint32 ValueReference)
	{
		FMemory::Memcpy(
			GuestMemory.Bytes.GetData() + Address,
			&ValueReference,
			sizeof(ValueReference));
	};
	const uint8 InputNameUtf8[] = { 'P', 'l', 'a', 'y', 'e', 'r' };
	const uint8 InputStringUtf8[] = {
		'h', 'e', 'l', 'l', 'o',
		0xe4, 0xb8, 0x96, 0xe7, 0x95, 0x8c
	};
	const uint8 RefNameUtf8[] = { 'S', 'e', 'e', 'd' };
	const uint8 RefStringUtf8[] = { 'b', 'a', 's', 'e' };
	StoreLinearUtf8(16, MakeArrayView(InputNameUtf8));
	StoreLinearUtf8(64, MakeArrayView(InputStringUtf8));
	StoreLinearUtf8(128, MakeArrayView(RefNameUtf8));
	StoreLinearUtf8(192, MakeArrayView(RefStringUtf8));
	StoreValueReference(320, 128);
	StoreValueReference(324, 192);

	FAvidScriptUtf8ValueHeap Heap;
	FAvidScriptBindingInvocationContext Context;
	Context.Utf8ValueHeap = &Heap;
	TArray<uint8> Scratch;
	Scratch.SetNumZeroed(Program.RequiredScratchSize);
	UAvidScriptBindingsTestObject* Receiver = NewObject<UAvidScriptBindingsTestObject>();
	FAvidScriptDynamicHostCallResult Result;
	const uint64 OverlapArguments[] = { 0, 0, 16, 64, 320, 324, 328, 328 };
	TestFalse(
		TEXT("Overlapping UTF-8 outputs fail before ProcessEvent"),
		InvokePreparedDynamicReflection(
			&Cell,
			*Receiver,
			OverlapArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(TEXT("Overlapping UTF-8 outputs do not invoke game logic"), Receiver->Utf8InvocationCount, 0);
	TestEqual(TEXT("Overlap rollback releases UTF-8 reservations"), Heap.GetReservedValueCount(), 0);
	TestEqual(TEXT("Overlap rollback publishes no UTF-8 values"), Heap.GetLiveValueCount(), 0);

	const uint64 Arguments[] = { 0, 0, 16, 64, 320, 324, 328, 332 };
	if (!TestTrue(
			TEXT("UTF-8 function accepts linear inputs and returns heap tokens"),
			InvokePreparedDynamicReflection(
				&Cell,
				*Receiver,
				Arguments,
				&GuestMemory,
				Context,
				Scratch,
				Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestEqual(TEXT("Successful UTF-8 call invokes game logic once"), Receiver->Utf8InvocationCount, 1);

	uint32 InOutNameToken = 0;
	uint32 InOutStringToken = 0;
	uint32 OutNameToken = 0;
	uint32 ReturnToken = 0;
	FMemory::Memcpy(&InOutNameToken, GuestMemory.Bytes.GetData() + 320, sizeof(uint32));
	FMemory::Memcpy(&InOutStringToken, GuestMemory.Bytes.GetData() + 324, sizeof(uint32));
	FMemory::Memcpy(&OutNameToken, GuestMemory.Bytes.GetData() + 328, sizeof(uint32));
	FMemory::Memcpy(&ReturnToken, GuestMemory.Bytes.GetData() + 332, sizeof(uint32));
	TestTrue(TEXT("Ref FName output is a heap token"), FAvidScriptUtf8ValueHeap::IsHeapToken(InOutNameToken));
	TestTrue(TEXT("Ref FString output is a heap token"), FAvidScriptUtf8ValueHeap::IsHeapToken(InOutStringToken));
	TestTrue(TEXT("Out FName output is a heap token"), FAvidScriptUtf8ValueHeap::IsHeapToken(OutNameToken));
	TestTrue(TEXT("FString return is a heap token"), FAvidScriptUtf8ValueHeap::IsHeapToken(ReturnToken));

	const auto ResolveTokenString = [&Heap](const uint32 Token, FString& OutValue)
	{
		TConstArrayView<uint8> Bytes;
		FString Error;
		if (!Heap.Resolve(Token, Bytes, Error))
		{
			return false;
		}
		static constexpr ANSICHAR Empty[] = "";
		const ANSICHAR* Utf8 = Bytes.IsEmpty()
			? Empty
			: reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
		const FUTF8ToTCHAR Converted(Utf8, Bytes.Num());
		OutValue = FString(Converted.Length(), Converted.Get());
		return true;
	};
	const FUTF8ToTCHAR InputStringConverted(
		reinterpret_cast<const ANSICHAR*>(InputStringUtf8),
		UE_ARRAY_COUNT(InputStringUtf8));
	const FString InputStringValue(
		InputStringConverted.Length(),
		InputStringConverted.Get());
	FString Resolved;
	TestTrue(TEXT("Ref FName token resolves"), ResolveTokenString(InOutNameToken, Resolved));
	TestEqual(TEXT("Ref FName token preserves mutation"), Resolved, FString(TEXT("Seed_Touched")));
	TestTrue(TEXT("Ref FString token resolves"), ResolveTokenString(InOutStringToken, Resolved));
	TestEqual(TEXT("Ref FString token preserves mutation"), Resolved, FString(TEXT("base|")) + InputStringValue);
	TestTrue(TEXT("Out FName token resolves"), ResolveTokenString(OutNameToken, Resolved));
	TestEqual(TEXT("Out FName token preserves value"), Resolved, FString(TEXT("Player")));
	TestTrue(TEXT("FString return token resolves"), ResolveTokenString(ReturnToken, Resolved));
	TestEqual(TEXT("FString return token preserves value"), Resolved, FString(TEXT("Player:")) + InputStringValue);

	FProperty* StringProperty = FindFProperty<FProperty>(ExpectedClass, TEXT("Utf8StringProperty"));
	if (!TestNotNull(TEXT("UTF-8 FString property reflects"), StringProperty))
	{
		return false;
	}
	FInvocationCodecProgram StringWriteProgram;
	StringWriteProgram.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
	StringWriteProgram.OwnerClass = ExpectedClass;
	StringWriteProgram.ReflectedProperty = StringProperty;
	StringWriteProgram.DebugPath = StringProperty->GetPathName();
	StringWriteProgram.ExpectedArgumentCount = 3;
	StringWriteProgram.bRequiresGuestMemory = true;
	StringWriteProgram.Parameters.Add(MakeUtf8Codec(
		StringProperty,
		EValueCodecDirection::Value,
		2,
		EValueCodecKind::String));
	FPreparedDynamicInvocationCell StringWriteCell{ &StringWriteProgram, 0 };
	const uint64 TokenWriteArguments[] = { 0, 0, ReturnToken };
	TestTrue(
		TEXT("A second invocation accepts a FString heap token"),
		InvokePreparedDynamicReflection(
			&StringWriteCell,
			*Receiver,
			TokenWriteArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(
		TEXT("Token input reaches the reflected FString property"),
		Receiver->Utf8StringProperty,
		FString(TEXT("Player:")) + InputStringValue);

	Receiver->Utf8StringProperty = TEXT("unchanged");
	FAvidScriptBindingInvocationContext MissingHeapContext = Context;
	MissingHeapContext.Utf8ValueHeap = nullptr;
	TestFalse(
		TEXT("Heap token input rejects a missing session heap"),
		InvokePreparedDynamicReflection(
			&StringWriteCell,
			*Receiver,
			TokenWriteArguments,
			&GuestMemory,
			MissingHeapContext,
			Scratch,
			Result));
	TestEqual(TEXT("Missing heap leaves FString unchanged"), Receiver->Utf8StringProperty, FString(TEXT("unchanged")));
	const uint64 ForgedTokenArguments[] = { 0, 0, MAX_uint32 };
	TestFalse(
		TEXT("Forged UTF-8 heap token is rejected"),
		InvokePreparedDynamicReflection(
			&StringWriteCell,
			*Receiver,
			ForgedTokenArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	Heap.Reset();
	TestFalse(
		TEXT("Token from a reset session is stale"),
		InvokePreparedDynamicReflection(
			&StringWriteCell,
			*Receiver,
			TokenWriteArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));

	const uint8 EmbeddedNulUtf8[] = { 'a', 0, 'b' };
	StoreLinearUtf8(400, MakeArrayView(EmbeddedNulUtf8));
	const uint64 LinearStringArguments[] = { 0, 0, 400 };
	TestTrue(
		TEXT("FString linear input permits an embedded NUL"),
		InvokePreparedDynamicReflection(
			&StringWriteCell,
			*Receiver,
			LinearStringArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(TEXT("Embedded NUL FString preserves its explicit length"), Receiver->Utf8StringProperty.Len(), 3);
	TestEqual(TEXT("Embedded NUL FString preserves trailing data"), Receiver->Utf8StringProperty[2], TCHAR('b'));

	FInvocationCodecProgram StringReadProgram;
	StringReadProgram.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyRead;
	StringReadProgram.OwnerClass = ExpectedClass;
	StringReadProgram.ReflectedProperty = StringProperty;
	StringReadProgram.DebugPath = StringProperty->GetPathName();
	StringReadProgram.ExpectedArgumentCount = 3;
	StringReadProgram.bRequiresGuestMemory = true;
	StringReadProgram.ReturnValue = MakeUtf8Codec(
		StringProperty,
		EValueCodecDirection::Return,
		2,
		EValueCodecKind::String);
	FPreparedDynamicInvocationCell StringReadCell{ &StringReadProgram, 0 };
	FAvidScriptBoundaryGuestMemory FailingGuestMemory;
	FailingGuestMemory.FailMutableBorrowCall = 1;
	const uint64 FailedReadArguments[] = { 0, 0, 64 };
	TestFalse(
		TEXT("Unavailable mutable output range fails before encoding"),
		InvokePreparedDynamicReflection(
			&StringReadCell,
			*Receiver,
			FailedReadArguments,
			&FailingGuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(TEXT("Failed preflight leaves no live UTF-8 values"), Heap.GetLiveValueCount(), 0);
	TestEqual(TEXT("Failed preflight creates no UTF-8 reservation"), Heap.GetReservedValueCount(), 0);

	TArray<uint8> LargeInput;
	TArray<uint8> LargeRef;
	LargeInput.Init(static_cast<uint8>('a'), FAvidScriptUtf8ValueHeap::MaxValueBytes / 2u);
	LargeRef.Init(static_cast<uint8>('b'), FAvidScriptUtf8ValueHeap::MaxValueBytes / 2u);
	const auto InternLargeValue = [this, &Heap](
		const TConstArrayView<uint8> Bytes,
		uint32& OutToken)
	{
		FString InternError;
		FAvidScriptUtf8ValueReservation Reservation;
		bool bCreatedValue = false;
		return TestTrue(
			TEXT("Large atomic-output input reserves a capability"),
			Heap.Reserve(Reservation, InternError))
			&& TestTrue(
				TEXT("Large atomic-output input interns"),
				Heap.InternReserved(
					Reservation,
					Bytes,
					OutToken,
					bCreatedValue,
					InternError));
	};
	uint32 LargeInputToken = 0;
	uint32 LargeRefToken = 0;
	if (!InternLargeValue(LargeInput, LargeInputToken)
		|| !InternLargeValue(LargeRef, LargeRefToken))
	{
		return false;
	}
	StoreValueReference(320, 128);
	StoreValueReference(324, LargeRefToken);
	StoreValueReference(328, 0x11223344u);
	StoreValueReference(332, 0x55667788u);
	TArray<uint8> OutputSnapshot;
	OutputSnapshot.Append(GuestMemory.Bytes.GetData() + 320, 16);
	const uint64 AtomicFailureArguments[] = {
		0, 0, 16, LargeInputToken, 320, 324, 328, 332
	};
	TestFalse(
		TEXT("A later oversized UTF-8 output rejects the whole publication"),
		InvokePreparedDynamicReflection(
			&Cell,
			*Receiver,
			AtomicFailureArguments,
			&GuestMemory,
			Context,
			Scratch,
			Result));
	TestEqual(TEXT("Atomic encode failure occurs after game logic"), Receiver->Utf8InvocationCount, 2);
	TestTrue(
		TEXT("Atomic encode failure leaves every guest output byte unchanged"),
		FMemory::Memcmp(
			GuestMemory.Bytes.GetData() + 320,
			OutputSnapshot.GetData(),
			OutputSnapshot.Num()) == 0);
	TestEqual(TEXT("Atomic rollback retains only the two input values"), Heap.GetLiveValueCount(), 2);
	TestEqual(TEXT("Atomic rollback releases every output reservation"), Heap.GetReservedValueCount(), 0);
	Heap.Reset();
	return true;
}

#endif
