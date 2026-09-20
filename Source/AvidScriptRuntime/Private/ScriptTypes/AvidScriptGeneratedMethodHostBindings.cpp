#include "ScriptTypes/AvidScriptGeneratedTypeHostBindings.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeAuthority.h"
#include "AvidScriptWasmModuleLayout.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AvidScriptGeneratedMethodBindings
{
constexpr uint32 FrameMagic = 0x31464341, FrameVersion = 1, HeaderBytes = 64, MaxFrameBytes = 65536;
constexpr TCHAR ImportPrefix[] = TEXT("avid_ue_method_");
constexpr TCHAR DispatchPrefix[] = TEXT("avid_ue_dispatch_");

bool UInt(const TSharedPtr<FJsonValue>& Value, uint32 Maximum, uint32& Out)
{
	double Number = 0;
	if (!Value || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number) || Number < 0 || Number > Maximum) return false;
	Out = static_cast<uint32>(Number); return static_cast<double>(Out) == Number;
}
bool UInt(const FJsonObject& Object, const TCHAR* Name, uint32 Maximum, uint32& Out)
{
	const auto* Value = Object.Values.Find(Name); return Value && UInt(*Value, Maximum, Out);
}
bool Offsets(const FJsonObject& Object, const TCHAR* Name, uint32 FrameBytes, uint32 Limit, TArray<uint32>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.TryGetArrayField(Name, Values) || static_cast<uint32>(Values->Num()) > Limit) return false;
	for (const auto& Value : *Values)
	{
		uint32 Offset = 0;
		if (!UInt(Value, FrameBytes - 1, Offset) || Offset < HeaderBytes || (!Out.IsEmpty() && Offset <= Out.Last())) return false;
		Out.Add(Offset);
	}
	return true;
}
bool Hash(const FString& Text, uint8* OutBytes)
{
	if (Text.Len() != 64) return false;
	for (int32 I = 0; I < 64; ++I)
	{
		const TCHAR C = Text[I];
		if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return false;
		if (OutBytes)
		{
			const uint8 Nibble = C <= '9' ? C - '0' : C - 'a' + 10;
			if ((I & 1) == 0) OutBytes[I / 2] = Nibble << 4; else OutBytes[I / 2] |= Nibble;
		}
	}
	return true;
}
uint64 Read(TConstArrayView<uint8> Bytes, uint32 Offset, uint32 Count)
{
	uint64 Value = 0;
	for (uint32 I = 0; I < Count; ++I) Value |= uint64(Bytes[Offset + I]) << (8 * I);
	return Value;
}
EAvidScriptVmTypedHostStatus Invoke(void* Opaque, int32 Address, int32 Size, int32& OutValue)
{
	OutValue = 0;
	auto* Context = static_cast<FAvidScriptGeneratedMethodHostContext*>(Opaque);
	if (!Context || !Context->Runtime || !Context->Runtime->InvokeGeneratedMethodFrame(Context->RouteIndex, Address, Size))
		return EAvidScriptVmTypedHostStatus::Rejected;
	OutValue = 1; return EAvidScriptVmTypedHostStatus::Succeeded;
}
}

bool ConfigureAvidScriptGeneratedMethodRoutes(FAvidScriptWasmRuntimeInstance& Runtime,
	TConstArrayView<uint8> CanonicalWasm, FAvidScriptGeneratedTypeHostBindings& State, FString& OutError)
{
	using namespace AvidScriptGeneratedMethodBindings;
	TArray<uint8> Payload; bool bFound = false;
	if (!ReadAvidScriptWasmCustomSection(CanonicalWasm, TEXT("avidscript.host_call_frames"), 4 * 1024 * 1024, Payload, bFound, OutError)) return false;
	FAvidScriptWasmModuleLayout Layout;
	if (!InspectAvidScriptWasmModuleLayout(CanonicalWasm, Layout, OutError)) return false;
	TSet<FString> Required;
	for (const auto& Import : Layout.FunctionImports)
		if (Import.ModuleName == TEXT("avidscript")
			&& (Import.ImportName.StartsWith(ImportPrefix) || Import.ImportName.StartsWith(DispatchPrefix))) Required.Add(Import.ImportName);
	if (!bFound)
	{
		if (Required.IsEmpty()) return true;
		OutError = TEXT("generated method imports require canonical host-call-frame metadata"); return false;
	}
	for (const uint8 Byte : Payload)
		if (Byte != '\n' && Byte != '\r' && Byte != '\t' && (Byte < 0x20 || Byte > 0x7e))
		{ OutError = TEXT("generated method metadata must be canonical JSON text"); return false; }
	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
	TSharedPtr<FJsonObject> Document;
	const TArray<TSharedPtr<FJsonValue>>* Exports = nullptr; uint32 Version = 0;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Document)
		|| !Document || Document->Values.Num() != 2 || !UInt(*Document, TEXT("schema_version"), 2, Version) || Version < 1
		|| !Document->TryGetArrayField(TEXT("exports"), Exports) || Exports->IsEmpty() || Exports->Num() > 4096)
	{ OutError = TEXT("invalid generated method route metadata"); return false; }
	TSet<FString> Names;
	TMap<FString, uint32> Indices;
	TMap<uint32, TArray<TPair<uint32, FString>>> PendingDispatch;
	uint32 TotalTargets = 0;
	for (const auto& Entry : *Exports)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Entry || !Entry->TryGetObject(Object) || !Object || !*Object)
		{ OutError = TEXT("generated method route must be an object"); return false; }
		const auto& Item = **Object;
		auto Context = MakeUnique<FAvidScriptGeneratedMethodHostContext>();
		FString Module, Signature;
		Context->bDynamicDispatch = Item.HasField(TEXT("dispatch_targets"));
		if (Item.Values.Num() != (Context->bDynamicDispatch ? 8 : 7) || (Context->bDynamicDispatch && Version != 2)
			|| !Item.TryGetStringField(TEXT("name"), Context->ExportName)
			|| !Item.TryGetStringField(TEXT("import_module"), Module) || Module != TEXT("avidscript")
			|| !Item.TryGetStringField(TEXT("import_name"), Context->ImportName) || !Required.Remove(Context->ImportName)
			|| !Item.TryGetStringField(TEXT("signature_sha256"), Signature) || !Hash(Signature, Context->Signature)
			|| !UInt(Item, TEXT("frame_bytes"), MaxFrameBytes, Context->FrameBytes) || Context->FrameBytes < 80 || Context->FrameBytes % 16
			|| !Offsets(Item, TEXT("parameter_offsets"), Context->FrameBytes, 256, Context->ParameterOffsets)
			|| Context->ParameterOffsets.IsEmpty() || Context->ParameterOffsets[0] != HeaderBytes
			|| !Offsets(Item, TEXT("root_token_offsets"), Context->FrameBytes, MaxFrameBytes / 8, Context->RootTokenOffsets))
		{ OutError = TEXT("generated method route has an invalid identity, signature or frame layout"); return false; }
		for (const uint32 Offset : Context->RootTokenOffsets)
			if (Offset % 8 || Offset + 8 > Context->FrameBytes || Offset <= Context->ParameterOffsets.Last())
			{ OutError = TEXT("generated method root offsets are invalid"); return false; }
		Context->Runtime = &Runtime; Context->RouteIndex = State.MethodContexts.Num();
		TArray<FString> Parts;
		const TCHAR* Prefix = Context->bDynamicDispatch ? DispatchPrefix : ImportPrefix;
		Context->ImportName.Mid(FCString::Strlen(Prefix)).ParseIntoArray(Parts, TEXT("_"), false);
		const int32 HashIndex = Context->bDynamicDispatch ? 0 : 1;
		if (!Context->ImportName.StartsWith(Prefix) || Parts.Num() != HashIndex + 3
			|| Parts[HashIndex + 1] != TEXT("invoke") || Parts[HashIndex + 2] != TEXT("v1") || !Hash(Parts[HashIndex], nullptr)
			|| Context->ExportName != FString(Prefix) + Parts[HashIndex] + TEXT("_frame_v1") || Names.Contains(Context->ExportName)
			|| !Layout.FunctionExports.ContainsByPredicate([&](const auto& Export) { return Export.Name == Context->ExportName; }))
		{ OutError = TEXT("generated method route names do not bind a unique canonical export"); return false; }
		if (Context->bDynamicDispatch)
		{
			const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
			if (!Item.TryGetArrayField(TEXT("dispatch_targets"), Targets) || Targets->IsEmpty() || Targets->Num() > 4096
				|| (TotalTargets += Targets->Num()) > 65536)
			{ OutError = TEXT("generated dispatch targets exceed their bounded nonempty contract"); return false; }
			auto& Pending = PendingDispatch.Add(Context->RouteIndex);
			for (const auto& Value : *Targets)
			{
				const TSharedPtr<FJsonObject>* Target = nullptr; uint32 Selector = 0; FString Name;
				if (!Value || !Value->TryGetObject(Target) || !Target || !*Target || (*Target)->Values.Num() != 2
					|| !UInt(**Target, TEXT("selector"), MAX_uint32, Selector)
					|| (!Pending.IsEmpty() && Selector <= Pending.Last().Key)
					|| !(*Target)->TryGetStringField(TEXT("export_name"), Name) || Name.IsEmpty())
				{ OutError = TEXT("generated dispatch targets require ordered unique selectors and named exports"); return false; }
				Pending.Emplace(Selector, MoveTemp(Name));
			}
		}
		else
		{
			uint32 Ordinal = 0;
			if (!LexTryParseString(Ordinal, *Parts[0]) || FString::Printf(TEXT("%u"), Ordinal) != Parts[0])
			{ OutError = TEXT("generated method route has a noncanonical type ordinal"); return false; }
			const auto* Type = State.Registry->FindTypeByOrdinal(Ordinal);
			if (!Type || !Type->Class) { OutError = TEXT("generated method route has no registry type"); return false; }
			Context->ExpectedClass = Type->Class;
		}
		Names.Add(Context->ExportName); Indices.Add(Context->ExportName, Context->RouteIndex);
		FAvidScriptVmTypedHostImport Import;
		Import.StableId = Context->ImportName; Import.ModuleName = Module; Import.ImportName = Context->ImportName;
		Import.Signature = TEXT("(ii)i"); Import.Shape = EAvidScriptVmTypedHostShape::I32PairToI32;
		Import.bSupplementalRuntimeAuthority = true; Import.PreparedTarget.Context = Context.Get(); Import.PreparedTarget.I32Pair = &Invoke;
		State.MethodContexts.Add(MoveTemp(Context)); State.HostImports.Add(MoveTemp(Import));
	}
	if (!Required.IsEmpty()) { OutError = TEXT("generated method imports are missing route metadata"); return false; }
	for (const auto& Pending : PendingDispatch)
	{
		auto& Route = *State.MethodContexts[Pending.Key];
		for (const auto& Entry : Pending.Value)
		{
			const uint32* Index = Indices.Find(Entry.Value);
			const auto* Type = State.Registry->FindTypeByOrdinal(Entry.Key);
			if (!Index || !Type || !Type->Class)
			{ OutError = TEXT("generated dispatch target has no registered type or direct export"); return false; }
			const auto& Target = *State.MethodContexts[*Index];
			if (Target.bDynamicDispatch || !Target.ExpectedClass || !Type->Class->IsChildOf(Target.ExpectedClass)
				|| Route.FrameBytes != Target.FrameBytes || Route.ParameterOffsets != Target.ParameterOffsets
				|| Route.RootTokenOffsets != Target.RootTokenOffsets || FMemory::Memcmp(Route.Signature, Target.Signature, 32))
			{ OutError = TEXT("generated dispatch target changes the receiver type or frame contract"); return false; }
			Route.DispatchRoutes.Add(Entry.Key, *Index);
		}
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::PrepareGeneratedMethodHostBindings(FString& OutError)
{
	OutError.Reset();
	if (!GeneratedTypeHostBindings || GeneratedTypeHostBindings->MethodContexts.IsEmpty()) return true;
	if (!IsInGameThread() || !IsLoaded() || ContextInvocationDepth || ManagedHeapInvocationDepth)
	{ OutError = TEXT("generated method preparation requires an idle loaded Runtime"); return false; }
	for (auto& Context : GeneratedTypeHostBindings->MethodContexts)
	{
		FAvidScriptVmExportHandle Handle; FAvidScriptVmError Error;
		FAvidScriptVmAbiSignature Signature; Signature.Parameters = {EAvidScriptVmValueKind::I32, EAvidScriptVmValueKind::I32};
		if (!VmBackend->ResolveExport(Context->ExportName, Handle, Error) || !VmBackend->ValidateExportSignature(Handle, Signature, Error))
		{ OutError = Error.Category + TEXT(": ") + Error.Details; return false; }
		if (!PrepareContextualExportCall(Context->ExportName, Context->Call, OutError)) return false;
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::InvokeGeneratedMethodFrame(uint32 RouteIndex, int32 FrameAddress, int32 FrameBytes)
{
	using namespace AvidScriptGeneratedMethodBindings;
	auto Reject = [&](const TCHAR* Reason)
	{
		FAvidScriptVmError Error; Error.Category = TEXT("generated_method_frame"); Error.Details = Reason;
		if (ContextInvocationDepth) LatchContextInvocationFailure(Error);
		return false;
	};
	if (!IsInGameThread() || !IsLoaded() || !ContextInvocationDepth || !GeneratedTypeHostBindings
		|| !GeneratedTypeHostBindings->MethodContexts.IsValidIndex(RouteIndex) || !HostContext.ObjectRegistry)
		return Reject(TEXT("method frames require an active generated owner and a prepared route"));
	const auto& Route = *GeneratedTypeHostBindings->MethodContexts[RouteIndex];
	if (!Route.Call.IsValid() || FrameBytes != static_cast<int32>(Route.FrameBytes) || static_cast<uint32>(FrameAddress) % 16)
		return Reject(TEXT("method frame size, alignment or code preparation is invalid"));
	uint64 PackedSelf = 0; TArray<uint64, TInlineAllocator<16>> Roots;
	{
		// All borrowed views end before reentry: nested execution may grow memory.
		TConstArrayView<uint8> Bytes; FString Error;
		auto* Memory = VmBackend->GetGuestMemory();
		if (!Memory || !Memory->BorrowReadOnlyBytes(static_cast<uint32>(FrameAddress), Route.FrameBytes, 16, Bytes, Error))
			return Reject(TEXT("method frame is outside guest memory"));
		if (Read(Bytes, 0, 4) != FrameMagic || Read(Bytes, 4, 4) != FrameVersion || Read(Bytes, 8, 4) != Route.FrameBytes
			|| Read(Bytes, 12, 4) != 0 || FMemory::Memcmp(Bytes.GetData() + 16, Route.Signature, 32) != 0)
			return Reject(TEXT("method frame header or signature does not match the prepared route"));
		for (uint32 Offset = 48; Offset < HeaderBytes; ++Offset) if (Bytes[Offset] != 0) return Reject(TEXT("method frame reserved bytes must be zero"));
		PackedSelf = Read(Bytes, HeaderBytes, 8);
		for (const uint32 Offset : Route.RootTokenOffsets) Roots.Add(Read(Bytes, Offset, 8));
	}
	FAvidScriptObjectHandle Target{static_cast<uint32>(PackedSelf), static_cast<uint32>(PackedSelf >> 32)};
	FAvidScriptObjectHandleResult Resolve;
	UObject* Object = HostContext.ObjectRegistry->ResolveObject(Target, Resolve, false);
	if (!Object) return Reject(TEXT("method receiver is not a live registry object"));
	const FAvidScriptGeneratedMethodHostContext* Selected = &Route;
	if (Route.bDynamicDispatch)
	{
		const auto Authority = HostContext.GeneratedTypeAuthority.Pin();
		uint32 Ordinal = 0; FAvidScriptVmError SelectionError;
		if (!Authority || !Authority->ResolveInstanceTypeOrdinal(*this, Target, *GeneratedTypeHostBindings->Registry, Ordinal, SelectionError))
			return Reject(TEXT("method dispatch target has no active registered script type"));
		const uint32* Index = Route.DispatchRoutes.Find(Ordinal);
		if (!Index || !GeneratedTypeHostBindings->MethodContexts.IsValidIndex(*Index))
			return Reject(TEXT("method dispatch has no implementation for the registered script type"));
		Selected = GeneratedTypeHostBindings->MethodContexts[*Index].Get();
	}
	if (!Selected->ExpectedClass || !Object->IsA(Selected->ExpectedClass) || !Selected->Call.IsValid())
		return Reject(TEXT("selected method receiver or prepared code is invalid"));
	FAvidScriptVmCallFrame Frame; Frame.CellCount = 2;
	Frame.Cells[0] = static_cast<uint32>(FrameAddress); Frame.Cells[1] = Route.FrameBytes;
	FAvidScriptVmError Error;
	return InvokeGeneratedInstanceExport(Target, Selected->Call, Frame, Error, nullptr, Roots);
}
