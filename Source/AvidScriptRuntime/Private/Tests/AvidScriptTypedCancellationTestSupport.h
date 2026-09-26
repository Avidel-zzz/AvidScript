#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "AvidScriptWasmReloadTypes.h"
#include "AvidScriptHash.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Dom/JsonObject.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace AvidScript::Tests::TypedCancellation
{
struct FFixture
{
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytes;
	TMap<FString, int32> Offsets;

	bool Load(FAutomationTestBase& Test, const FString& Path, int32 Schema = 24,
		const TCHAR* EntryOwner = TEXT("CancellationLifecycleEntry"), const TCHAR* Stem = TEXT("cancellation_lifecycle"))
	{
		FAvidScriptWasmReloadManifestLoadResult Result;
		if (!Test.TestTrue(TEXT("Formal manifest verifies the executed bytes"),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(Path, Manifest, Bytes, Result)))
		{ Test.AddError(Result.ErrorMessage); return false; }
		if (!Test.TestTrue(TEXT("Fixture has formal state migration"), Manifest.StateMigration.IsEnabled())
			|| !Test.TestEqual(TEXT("Migration covers the exported owner"), Manifest.StateMigration.OwnerTypeId,
				FString(TEXT("type:global::")) + EntryOwner)
			|| !Test.TestEqual(TEXT("Only entry fields migrate"), Manifest.StateMigration.Slots.Num(), 3)) return false;

		// Helper statics have observable memory slots but are not migration fields.
		// Read the exact IR named by this formal fixture, verified against its manifest.
		TArray<uint8> IrBytes;
		const FString IrPath = FPaths::GetPath(Path) / (FString(Stem) + TEXT(".guestir.json"));
		if (!Test.TestTrue(TEXT("Compiler IR exists"), FFileHelper::LoadFileToArray(IrBytes, *IrPath))
			|| !Test.TestEqual(TEXT("Readback layout matches verified compiler provenance"),
				FAvidScriptHash::Sha256Hex(IrBytes), Manifest.DebugProvenance.GuestIrSha256)) return false;
		FString Json;
		FFileHelper::BufferToString(Json, IrBytes.GetData(), IrBytes.Num());
		TSharedPtr<FJsonObject> Ir;
		if (!Test.TestTrue(TEXT("Compiler IR parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Ir))
			|| !Ir.IsValid()
			|| !Test.TestEqual(TEXT("Compiler cancellation IR version"), Ir->GetIntegerField(TEXT("schema_version")), Schema)
			|| !Test.TestEqual(TEXT("Compiler cancellation IR semantic version"), Ir->GetStringField(TEXT("ir_version")),
				FString(Schema == 25 ? TEXT("1.24") : TEXT("1.23")))
			|| !Test.TestEqual(TEXT("IR and executable share module identity"), Ir->GetStringField(TEXT("module_id")), Manifest.ModuleId)) return false;
		Offsets.Reset();
		for (const auto& Value : Ir->GetObjectField(TEXT("memory_layout"))->GetArrayField(TEXT("state_slots")))
		{
			const auto Slot = Value->AsObject();
			if (Slot->GetStringField(TEXT("type_id")) != TEXT("type:int32")) continue;
			const FString Id = Slot->GetStringField(TEXT("global_id"));
			const int32 Offset = Slot->GetIntegerField(TEXT("offset"));
			if (!Test.TestTrue(TEXT("Readback uses a unique aligned i32 memory slot"), !Offsets.Contains(Id)
				&& Slot->GetIntegerField(TEXT("size")) == 4 && Offset >= 0 && Offset <= 65532 && Offset % 4 == 0)) return false;
			Offsets.Add(Id, Offset);
		}
		for (const TCHAR* Field : {TEXT("BeginCount"), TEXT("ReloadMode"), TEXT("Result")})
		{
			const FString Id = FString::Printf(TEXT("state:type:global::%s:%s"), EntryOwner, Field);
			const auto* Slot = Manifest.StateMigration.Slots.FindByPredicate(
				[&](const FAvidScriptWasmStateSlot& Item) { return Item.StableId == Id; });
			const int32* Offset = Offsets.Find(GlobalId(EntryOwner, Field));
			if (!Test.TestTrue(TEXT("Migrated field matches compiler memory layout"), Slot && Offset
				&& Slot->Offset == static_cast<uint32>(*Offset) && Slot->Size == sizeof(int32))) return false;
		}
		return true;
	}

	static FString GlobalId(const TCHAR* Owner, const TCHAR* Field)
	{ return FString::Printf(TEXT("global:symbol:field:global::%s.%s:int32"), Owner, Field); }

	int32 Read(FAutomationTestBase& Test, const FAvidScriptWasmRuntimeInstance& Runtime,
		const TCHAR* Owner, const TCHAR* Field) const
	{
		const FString Id = GlobalId(Owner, Field);
		const int32* Offset = Offsets.Find(Id);
		int32 Value = MIN_int32;
		FString Error;
		if (!Offset) Test.AddError(TEXT("Missing fixture slot: ") + Id);
		else if (!Runtime.ReadStateBytes(*Offset,
			MakeArrayView(reinterpret_cast<uint8*>(&Value), sizeof(Value)), Error)) Test.AddError(Error);
		return Value;
	}
};

inline FAvidScriptVmBackendSelection Selection(EAvidScriptVmBackendKind Backend)
{
	FAvidScriptVmBackendSelection Result;
	Result.BackendKind = Backend;
	Result.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
		? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
	return Result;
}

inline void CheckHeapReleased(FAutomationTestBase& Test, FAvidScriptWasmRuntimeInstance& Runtime)
{
	auto* Heap = Runtime.GetManagedHeapForTesting();
	Test.TestEqual(TEXT("Persistent roots released"), Heap->GetStats().LiveRoots, uint32(0));
	Test.TestEqual(TEXT("Call frames released"), Heap->GetStats().ActiveFrames, uint32(0));
	Test.TestTrue(TEXT("Retired exception objects can collect"), Heap->Collect() == Managed::EHeapError::Ok);
	Test.TestEqual(TEXT("Exception objects reclaimed"), Heap->GetStats().LiveObjects, uint32(0));
}

inline void CheckEmpty(FAutomationTestBase& Test, FAvidScriptSessionContinuations& Owner, int32 Sources)
{
	Test.TestEqual(TEXT("Task records released"), Owner.GetTaskResultsForTesting().GetCount(), 0);
	Test.TestEqual(TEXT("Task waiters released"), Owner.GetTaskResultsForTesting().GetWaiterCount(), 0);
	Test.TestEqual(TEXT("State frames released"), Owner.GetStateFrameByteCountForTesting(), 0);
	Test.TestEqual(TEXT("Cancellation bindings released"), Owner.GetCancellationBindingCountForTesting(), 0);
	Test.TestEqual(TEXT("Cancellation sources have exact surviving ownership"), Owner.GetCancellationSourceCountForTesting(), Sources);
	Test.TestEqual(TEXT("Active continuations released"), Owner.GetActiveCount(), 0);
	Test.TestEqual(TEXT("Prepared continuations released"), Owner.GetPreparedCount(), 0);
	for (const auto Lane : {EAvidScriptContinuationLane::Active, EAvidScriptContinuationLane::Prepared})
		Test.TestEqual(TEXT("Ready queue empty"), Owner.GetReadyCountForTesting(Lane), 0);
}
}

#endif
