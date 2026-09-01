#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptWasmReload.h"
#include "AvidScriptWasmRuntime.h"

#include "AvidScriptRuntimeBackendTestLanes.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
void AppendCSharpU32Leb(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	} while (Value != 0);
}

void AppendCSharpString(TArray<uint8>& Bytes, const char* Text)
{
	const int32 Length = static_cast<int32>(FCStringAnsi::Strlen(Text));
	AppendCSharpU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Text[Index]));
	}
}

void AppendCSharpSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendCSharpU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendCSharpF32(TArray<uint8>& Bytes, float Value)
{
	const uint8* RawBytes = reinterpret_cast<const uint8*>(&Value);
	Bytes.Append(RawBytes, sizeof(Value));
}

void AppendActorSetLocationCall(
	TArray<uint8>& Body,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& TargetLocation)
{
	Body.Add(0x41);
	AppendCSharpU32Leb(Body, ActorHandle.Slot);
	Body.Add(0x41);
	AppendCSharpU32Leb(Body, ActorHandle.Generation);
	Body.Add(0x43);
	AppendCSharpF32(Body, static_cast<float>(TargetLocation.X));
	Body.Add(0x43);
	AppendCSharpF32(Body, static_cast<float>(TargetLocation.Y));
	Body.Add(0x43);
	AppendCSharpF32(Body, static_cast<float>(TargetLocation.Z));
	Body.Add(0x10);
	AppendCSharpU32Leb(Body, 0);
	Body.Add(0x1a);
}

TArray<uint8> BuildCSharpDirectAbiContractFixture(
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& BeginPlayLocation,
	const FVector& TickLocation)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> TypeSection;
	AppendCSharpU32Leb(TypeSection, 3);
	TypeSection.Add(0x60);
	AppendCSharpU32Leb(TypeSection, 5);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	AppendCSharpU32Leb(TypeSection, 1);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x60);
	AppendCSharpU32Leb(TypeSection, 0);
	AppendCSharpU32Leb(TypeSection, 0);
	TypeSection.Add(0x60);
	AppendCSharpU32Leb(TypeSection, 1);
	TypeSection.Add(0x7d);
	AppendCSharpU32Leb(TypeSection, 0);
	AppendCSharpSection(Module, 1, TypeSection);

	TArray<uint8> ImportSection;
	AppendCSharpU32Leb(ImportSection, 1);
	AppendCSharpString(ImportSection, "env");
	AppendCSharpString(ImportSection, "actor_set_location");
	ImportSection.Add(0x00);
	AppendCSharpU32Leb(ImportSection, 0);
	AppendCSharpSection(Module, 2, ImportSection);

	TArray<uint8> FunctionSection;
	AppendCSharpU32Leb(FunctionSection, 2);
	AppendCSharpU32Leb(FunctionSection, 1);
	AppendCSharpU32Leb(FunctionSection, 2);
	AppendCSharpSection(Module, 3, FunctionSection);

	TArray<uint8> ExportSection;
	AppendCSharpU32Leb(ExportSection, 2);
	AppendCSharpString(ExportSection, "avid_on_begin_play");
	ExportSection.Add(0x00);
	AppendCSharpU32Leb(ExportSection, 1);
	AppendCSharpString(ExportSection, "avid_on_tick");
	ExportSection.Add(0x00);
	AppendCSharpU32Leb(ExportSection, 2);
	AppendCSharpSection(Module, 7, ExportSection);

	TArray<uint8> BeginPlayBody;
	AppendCSharpU32Leb(BeginPlayBody, 0);
	AppendActorSetLocationCall(BeginPlayBody, ActorHandle, BeginPlayLocation);
	BeginPlayBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendCSharpU32Leb(TickBody, 0);
	AppendActorSetLocationCall(TickBody, ActorHandle, TickLocation);
	TickBody.Add(0x0b);

	TArray<uint8> CodeSection;
	AppendCSharpU32Leb(CodeSection, 2);
	AppendCSharpU32Leb(CodeSection, static_cast<uint32>(BeginPlayBody.Num()));
	CodeSection.Append(BeginPlayBody);
	AppendCSharpU32Leb(CodeSection, static_cast<uint32>(TickBody.Num()));
	CodeSection.Append(TickBody);
	AppendCSharpSection(Module, 10, CodeSection);

	return Module;
}

FString GetCSharpSampleSourcePath()
{
	return FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript"),
		TEXT("Samples"),
		TEXT("CSharp"),
		TEXT("ActorLifecycle"),
		TEXT("ActorLifecycleScript.cs"));
}


FString GetCSharpToolchainReportPath()
{
	FString ReportPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle"),
		TEXT("actor_lifecycle.csharp.report.json"));
	ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
	FPaths::NormalizeFilename(ReportPath);
	return ReportPath;
}

FString GetCSharpNameStringToolchainReportPath()
{
	FString ReportPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("P57_11B2_NameStringRoundtrip"),
		TEXT("name_string_roundtrip.csharp.report.json"));
	ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
	FPaths::NormalizeFilename(ReportPath);
	return ReportPath;
}

FString GetCSharpNameStringSourcePath()
{
	FString SourcePath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript"),
		TEXT("Source"),
		TEXT("AvidScriptRuntime"),
		TEXT("Private"),
		TEXT("Tests"),
		TEXT("Fixtures"),
		TEXT("P57_11B2_NameStringRoundtrip.cs"));
	SourcePath = FPaths::ConvertRelativePathToFull(SourcePath);
	FPaths::NormalizeFilename(SourcePath);
	return SourcePath;
}

FString GetCSharpArrayToolchainReportPath()
{
	FString ReportPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("P57_11B3_ArrayRoundtrip"),
		TEXT("array_roundtrip.csharp.report.json"));
	ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
	FPaths::NormalizeFilename(ReportPath);
	return ReportPath;
}

bool ReadCSharpIntArrayProperty(
	UObject& Object,
	TArray<int32>& OutValues)
{
	OutValues.Reset();
	const FArrayProperty* const ArrayProperty =
		FindFProperty<FArrayProperty>(Object.GetClass(), TEXT("ReadableIntArray"));
	const FIntProperty* const ElementProperty = ArrayProperty == nullptr
		? nullptr
		: CastField<FIntProperty>(ArrayProperty->Inner);
	if (ArrayProperty == nullptr || ElementProperty == nullptr)
	{
		return false;
	}
	const void* const ArrayValue =
		ArrayProperty->ContainerPtrToValuePtr<void>(&Object);
	FScriptArrayHelper Helper(ArrayProperty, ArrayValue);
	OutValues.Reserve(Helper.Num());
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		OutValues.Add(ElementProperty->GetPropertyValue(Helper.GetRawPtr(Index)));
	}
	return true;
}

bool ReadCSharpNameStringProperties(
	UObject& Object,
	FString& OutName,
	FString& OutString)
{
	const FNameProperty* const NameProperty =
		FindFProperty<FNameProperty>(Object.GetClass(), TEXT("ReadableFName"));
	const FStrProperty* const StringProperty =
		FindFProperty<FStrProperty>(Object.GetClass(), TEXT("ReadableFString"));
	if (NameProperty == nullptr || StringProperty == nullptr)
	{
		return false;
	}

	OutName = NameProperty->GetPropertyValue_InContainer(&Object).ToString();
	OutString = StringProperty->GetPropertyValue_InContainer(&Object);
	return true;
}

bool InvokeCSharpReturnStringOracle(UObject& Object, FString& OutString)
{
	OutString.Reset();
	UFunction* const Function = Object.FindFunction(TEXT("ReturnFString"));
	const FStrProperty* const ReturnProperty = Function == nullptr
		? nullptr
		: CastField<FStrProperty>(Function->GetReturnProperty());
	if (Function == nullptr || ReturnProperty == nullptr)
	{
		return false;
	}

	const int32 Alignment = FMath::Max(1, Function->GetMinAlignment());
	TArray<uint8> Scratch;
	Scratch.SetNumZeroed(Function->GetStructureSize() + Alignment - 1);
	void* const Frame = reinterpret_cast<void*>(Align(
		reinterpret_cast<UPTRINT>(Scratch.GetData()),
		static_cast<UPTRINT>(Alignment)));
	Function->InitializeStruct(Frame);
	Object.ProcessEvent(Function, Frame);
	OutString = ReturnProperty->GetPropertyValue_InContainer(Frame);
	Function->DestroyStruct(Frame);
	return true;
}

FString ResolveCSharpReportArtifactPath(const FString& ArtifactPath)
{
	if (ArtifactPath.IsEmpty())
	{
		return FString();
	}

	FString NormalizedPath = ArtifactPath;
	FPaths::NormalizeFilename(NormalizedPath);

	if (!FPaths::IsRelative(NormalizedPath))
	{
		return FPaths::ConvertRelativePathToFull(NormalizedPath);
	}

	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), NormalizedPath));
}

bool CreateCSharpContractWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptCSharpContractWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutWorld->InitializeActorsForPlay(FURL());
	return true;
}

void DestroyCSharpContractWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}

	World->DestroyWorld(false);
	World = nullptr;
}

void AdvanceCSharpContractWorld(UWorld* World, const float ElapsedSeconds)
{
	World->Tick(LEVELTICK_All, 0.0f);
	++GFrameCounter;
	World->Tick(LEVELTICK_All, ElapsedSeconds);
	++GFrameCounter;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCSharpSampleShapeSmokeTest,
	"AvidScript.Guest.CSharp.SampleShapeSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpSampleShapeSmokeTest::RunTest(const FString& Parameters)
{
	FString SourceText;
	const FString SourcePath = GetCSharpSampleSourcePath();
	if (!FFileHelper::LoadFileToString(SourceText, *SourcePath))
	{
		AddError(FString::Printf(TEXT("Failed to load C# sample source: %s"), *SourcePath));
		return true;
	}

	TestTrue(TEXT("Sample exports BeginPlay"), SourceText.Contains(TEXT("avid_on_begin_play")));
	TestTrue(TEXT("Sample exports Tick"), SourceText.Contains(TEXT("avid_on_tick")));
	TestTrue(TEXT("Sample exports EndPlay"), SourceText.Contains(TEXT("avid_on_end_play")));
	TestTrue(TEXT("Sample exports Timer callback"), SourceText.Contains(TEXT("avid_on_timer")));
	TestTrue(TEXT("Sample exports gameplay event callback"), SourceText.Contains(TEXT("avid_on_event")));
	TestFalse(TEXT("Sample leaves continuation dispatcher to the compiler"), SourceText.Contains(TEXT("avid_on_continuation")));
	TestTrue(TEXT("Sample schedules async object loading"), SourceText.Contains(TEXT("AvidAssets.LoadObjectAsync")));
	TestTrue(TEXT("Sample declares an object-result continuation handler"),
		SourceText.Contains(TEXT("AvidContinuationStatus status"))
		&& SourceText.Contains(TEXT("AvidLoadedObject loadedObject")));
	TestFalse(TEXT("Sample leaves typed gameplay event dispatcher to the compiler"), SourceText.Contains(TEXT("avid_on_gameplay_event")));
	TestTrue(TEXT("Sample declares EndPlay method"), SourceText.Contains(TEXT("public static void EndPlay")));
	TestTrue(TEXT("Sample declares OnTimer method"), SourceText.Contains(TEXT("public static void OnTimer(int callbackId, int timerHandle)")));
	TestTrue(TEXT("Sample declares OnEvent method"), SourceText.Contains(TEXT("public static void OnEvent(int eventId, float value)")));
	TestTrue(TEXT("Sample declares a continuation handler"), SourceText.Contains(TEXT("public static void OnDeferredBeginPlay()")));
	TestTrue(TEXT("Sample declares typed begin overlap callback"), SourceText.Contains(TEXT("public static void OnBeginOverlap(AActor otherActor, FVector location)")));
	TestTrue(TEXT("Sample declares typed end overlap callback"), SourceText.Contains(TEXT("public static void OnEndOverlap(AActor otherActor, FVector location)")));
	TestTrue(TEXT("Sample declares typed hit callback"), SourceText.Contains(TEXT("public static void OnHit(AActor otherActor, FVector normalImpulse)")));
	TestTrue(TEXT("Sample declares typed input callback"), SourceText.Contains(TEXT("public static void OnInput(InputEvent input)")));
	TestTrue(TEXT("Sample declares InputEvent value type"), SourceText.Contains(TEXT("readonly struct InputEvent")));
	TestTrue(TEXT("Sample exposes all InputEvent fields"), SourceText.Contains(TEXT("readonly int ActionId")) && SourceText.Contains(TEXT("readonly int TriggerEvent")) && SourceText.Contains(TEXT("readonly FVector Value")));
	TestTrue(TEXT("Sample uses UnmanagedCallersOnly"), SourceText.Contains(TEXT("UnmanagedCallersOnly")));
	TestTrue(TEXT("Sample imports env actor_set_location"), SourceText.Contains(TEXT("DllImport(\"env\"")) && SourceText.Contains(TEXT("actor_set_location")));
	TestTrue(TEXT("Sample imports env actor_add_location_offset"), SourceText.Contains(TEXT("DllImport(\"env\"")) && SourceText.Contains(TEXT("actor_add_location_offset")));
	TestTrue(TEXT("Sample imports env owner_get_slot"), SourceText.Contains(TEXT("owner_get_slot")));
	TestTrue(TEXT("Sample imports env owner_get_generation"), SourceText.Contains(TEXT("owner_get_generation")));
	TestTrue(TEXT("Sample imports env timer_set_once"), SourceText.Contains(TEXT("timer_set_once")));
	TestTrue(TEXT("Sample imports env timer_cancel"), SourceText.Contains(TEXT("timer_cancel")));
	TestTrue(TEXT("Sample imports env continuation_delay"), SourceText.Contains(TEXT("continuation_delay")));
	TestTrue(TEXT("Sample imports env continuation_cancel"), SourceText.Contains(TEXT("continuation_cancel")));
	TestTrue(TEXT("Sample declares sequential FVector"), SourceText.Contains(TEXT("[StructLayout(LayoutKind.Sequential)]")) && SourceText.Contains(TEXT("public readonly struct FVector")));
	TestTrue(TEXT("Sample declares FVector addition"), SourceText.Contains(TEXT("public static FVector operator +")));
	TestTrue(TEXT("Sample declares sequential FRotator"), SourceText.Contains(TEXT("public readonly struct FRotator")));
	TestTrue(TEXT("Sample declares FRotator addition"), SourceText.Contains(TEXT("public static FRotator operator +")));
	TestTrue(TEXT("Sample declares handle-backed AActor"), SourceText.Contains(TEXT("public readonly struct AActor")));
	TestTrue(TEXT("Sample presents typed GetActorLocation"), SourceText.Contains(TEXT("public FVector GetActorLocation()")) && SourceText.Contains(TEXT("actor_get_location")));
	TestTrue(TEXT("Sample presents typed GetActorRotation"), SourceText.Contains(TEXT("public FRotator GetActorRotation()")) && SourceText.Contains(TEXT("actor_get_rotation")));
	TestTrue(TEXT("Sample presents typed SetActorRotation"), SourceText.Contains(TEXT("public bool SetActorRotation(FRotator rotation)")) && SourceText.Contains(TEXT("actor_set_rotation")));
	TestTrue(TEXT("Sample presents typed GetActorScale3D"), SourceText.Contains(TEXT("public FVector GetActorScale3D()")) && SourceText.Contains(TEXT("actor_get_scale")));
	TestTrue(TEXT("Sample presents typed SetActorScale3D"), SourceText.Contains(TEXT("public bool SetActorScale3D(FVector scale)")) && SourceText.Contains(TEXT("actor_set_scale")));
	TestTrue(TEXT("Sample declares FTransform snapshot"), SourceText.Contains(TEXT("public readonly struct FTransform")) && SourceText.Contains(TEXT("public FTransform GetActorTransform()")));
	TestTrue(TEXT("Sample declares handle-backed USceneComponent"), SourceText.Contains(TEXT("public readonly struct USceneComponent")));
	TestTrue(TEXT("Sample presents typed GetRootComponent"), SourceText.Contains(TEXT("public USceneComponent GetRootComponent()")) && SourceText.Contains(TEXT("actor_get_root_component")));
	TestTrue(TEXT("Sample presents component world location read"), SourceText.Contains(TEXT("public FVector GetWorldLocation()")) && SourceText.Contains(TEXT("scene_component_get_world_location")));
	TestTrue(TEXT("Sample presents component world location write"), SourceText.Contains(TEXT("public bool SetWorldLocation(FVector location)")) && SourceText.Contains(TEXT("scene_component_set_world_location")));
	TestTrue(TEXT("Sample uses root component chain"), SourceText.Contains(TEXT("UE.Self.GetRootComponent().GetWorldLocation()")) && SourceText.Contains(TEXT("UE.Self.GetRootComponent().SetWorldLocation(")));
	TestTrue(TEXT("Sample declares UE.Self"), SourceText.Contains(TEXT("public static class UE")) && SourceText.Contains(TEXT("public static AActor Self")));
	TestTrue(TEXT("Sample presents UE.SetTimer"), SourceText.Contains(TEXT("public static int SetTimer(float delaySeconds, int callbackId)")));
	TestTrue(TEXT("Sample presents UE.CancelTimer"), SourceText.Contains(TEXT("public static bool CancelTimer(int timerHandle)")));
	TestTrue(TEXT("Sample schedules Timer in BeginPlay"), SourceText.Contains(TEXT("UE.SetTimer(0.05f, 7)")));
	TestTrue(TEXT("Sample schedules a UE continuation in BeginPlay"), SourceText.Contains(TEXT("AvidContinuations.Delay(0.04f, DeferredBeginPlay)")));
	TestTrue(TEXT("Sample timer callback changes Actor"), SourceText.Contains(TEXT("UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 50.0f))")));
	TestTrue(TEXT("Sample event callback uses payload value"), SourceText.Contains(TEXT("UE.Self.AddActorWorldOffset(new FVector(0.0f, value, 0.0f))")));
	TestTrue(TEXT("Sample uses typed SetActorLocation"), SourceText.Contains(TEXT("UE.Self.SetActorLocation(new FVector")));
	TestTrue(TEXT("Sample uses typed AddActorWorldOffset"), SourceText.Contains(TEXT("UE.Self.AddActorWorldOffset(new FVector")));
	TestTrue(TEXT("Sample uses FVector.Zero"), SourceText.Contains(TEXT("UE.Self.SetActorLocation(FVector.Zero)")));
	TestTrue(TEXT("Sample presents Actor.SetLocation facade"), SourceText.Contains(TEXT("public static class Actor")) && SourceText.Contains(TEXT("public static bool SetLocation")));
	TestTrue(TEXT("Sample presents Actor.AddLocationOffset facade"), SourceText.Contains(TEXT("public static bool AddLocationOffset")));
	TestTrue(TEXT("Sample declares elapsed seconds state"), SourceText.Contains(TEXT("private static float ElapsedSeconds")));
	TestTrue(TEXT("Sample records the active overlap actor handle"), SourceText.Contains(TEXT("private static AActor ActiveOverlapActor")));
	TestTrue(TEXT("Sample records whether an input has already arrived"), SourceText.Contains(TEXT("private static bool HasPreviousInput")));
	TestTrue(TEXT("Sample records the input trigger event"), SourceText.Contains(TEXT("private static int LastInputTriggerEvent")));
	TestTrue(TEXT("Sample compares active overlap handles"), SourceText.Contains(TEXT("ActiveOverlapActor.Matches(otherActor)")));
	TestTrue(TEXT("Sample only suppresses an exact duplicate input"), SourceText.Contains(TEXT("HasPreviousInput && input.ActionId == LastInputActionId && input.TriggerEvent == LastInputTriggerEvent")));
	TestFalse(TEXT("Sample leaves elapsed seconds available for reload migration"), SourceText.Contains(TEXT("ElapsedSeconds = 0.0f")));
	TestTrue(TEXT("Sample accumulates elapsed seconds in Tick"), SourceText.Contains(TEXT("ElapsedSeconds += deltaSeconds")));
	TestTrue(TEXT("Sample reads location into a local FVector"), SourceText.Contains(TEXT("FVector currentLocation = UE.Self.GetActorLocation()")));
	TestTrue(TEXT("Sample uses FVector addition"), SourceText.Contains(TEXT("currentLocation + new FVector(120.0f * deltaSeconds, 0.0f, 0.0f)")));
	TestTrue(TEXT("Sample reads rotation into a local FRotator"), SourceText.Contains(TEXT("FRotator currentRotation = UE.Self.GetActorRotation()")));
	TestTrue(TEXT("Sample uses FRotator addition"), SourceText.Contains(TEXT("currentRotation + new FRotator(0.0f, 90.0f * deltaSeconds, 0.0f)")));
	TestTrue(TEXT("Sample reads scale into a local FVector"), SourceText.Contains(TEXT("FVector currentScale = UE.Self.GetActorScale3D()")));
	TestTrue(TEXT("Sample uses scale FVector addition"), SourceText.Contains(TEXT("currentScale + new FVector(0.0f, 0.0f, 0.6f * deltaSeconds)")));
	TestTrue(TEXT("Sample resets actor in typed EndPlay"), SourceText.Contains(TEXT("UE.Self.SetActorLocation(FVector.Zero)")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCSharpDirectAbiContractLifecycleSmokeTest,
	"AvidScript.Guest.CSharp.DirectAbiContractLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpDirectAbiContractLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateCSharpContractWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript C# direct ABI contract world."));
		DestroyCSharpContractWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyCSharpContractWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);

	const FVector BeginPlayLocation(130.0, 230.0, 330.0);
	const FVector TickLocation(131.0, 231.0, 331.0);
	const TArray<uint8> WasmBytes = BuildCSharpDirectAbiContractFixture(ActorHandle, BeginPlayLocation, TickLocation);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = TEXT("csharp_direct_abi_contract");
	Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
	Manifest.Language = TEXT("csharp");
	Manifest.RequiredExports = {
		TEXT("avid_on_begin_play"),
		TEXT("avid_on_tick")
	};
	Manifest.RequiredImports = {
		FAvidScriptWasmRequiredImport{ TEXT("env"), TEXT("actor_set_location") }
	};

	FAvidScriptWasmReloadSession Session;
	Session.SetHostContext(HostContext);

	FAvidScriptWasmReloadResult ReloadResult;
	if (!Session.LoadInitialModule(WasmBytes.GetData(), WasmBytes.Num(), Manifest, ReloadResult))
	{
		AddError(ReloadResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestTrue(TEXT("C# direct ABI contract module loads"), ReloadResult.bSucceeded);
	TestEqual(TEXT("BeginPlay moves actor through C# ABI contract"), Actor->GetActorLocation(), BeginPlayLocation);

	FAvidScriptWasmSmokeResult TickResult;
	if (!Session.TickLive(1.0f / 60.0f, TickResult))
	{
		AddError(TickResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestEqual(TEXT("Tick moves actor through C# ABI contract"), Actor->GetActorLocation(), TickLocation);
	TestEqual(TEXT("Tick count increments"), Session.GetLiveTickCallCount(), 1);

	DestroyCSharpContractWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCSharpSourceAdapterArtifactLifecycleSmokeTest,
	"AvidScript.Guest.CSharp.SourceAdapterArtifactLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpSourceAdapterArtifactLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetCSharpToolchainReportPath();
	if (!FPaths::FileExists(ReportPath))
	{
		AddError(FString::Printf(
			TEXT("C# source adapter report is missing; run BuildCSharpActorLifecycle.ps1 before this lifecycle smoke. report=%s"),
			*ReportPath));
		return true;
	}

	FString ReportJson;
	if (!FFileHelper::LoadFileToString(ReportJson, *ReportPath))
	{
		AddError(FString::Printf(TEXT("Failed to read C# source adapter report: %s"), *ReportPath));
		return true;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReportJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		AddError(FString::Printf(TEXT("C# source adapter report is not valid JSON: %s"), *ReportPath));
		return true;
	}

	FString Result;
	if (!RootObject->TryGetStringField(TEXT("result"), Result))
	{
		AddError(TEXT("C# source adapter report does not declare a result."));
		return true;
	}

	if (!TestEqual(TEXT("C# source adapter builds a direct ABI artifact"), Result, FString(TEXT("direct_abi_built"))))
	{
		return true;
	}

	bool bDirectAbiSupported = false;
	TestTrue(TEXT("C# source adapter reports direct ABI support"), RootObject->TryGetBoolField(TEXT("direct_abi_supported"), bDirectAbiSupported));
	if (!TestTrue(TEXT("C# source adapter direct ABI support is true"), bDirectAbiSupported))
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* ArtifactsObjectPtr = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("artifacts"), ArtifactsObjectPtr) || ArtifactsObjectPtr == nullptr || !ArtifactsObjectPtr->IsValid())
	{
		AddError(TEXT("C# source adapter report does not include artifacts."));
		return true;
	}

	FString ManifestArtifactPath;
	FString GuestIrArtifactPath;
	FString WasmArtifactPath;
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("manifest_file"), ManifestArtifactPath);
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("guest_ir_file"), GuestIrArtifactPath);
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("wasm_file"), WasmArtifactPath);

	const FString ManifestPath = ResolveCSharpReportArtifactPath(ManifestArtifactPath);
	const FString GuestIrPath = ResolveCSharpReportArtifactPath(GuestIrArtifactPath);
	const FString WasmPath = ResolveCSharpReportArtifactPath(WasmArtifactPath);
	if (!TestTrue(TEXT("Formal C# compiler writes a manifest artifact"), !ManifestPath.IsEmpty() && FPaths::FileExists(ManifestPath)) ||
		!TestTrue(TEXT("Formal C# compiler writes a Guest IR artifact"), !GuestIrPath.IsEmpty() && FPaths::FileExists(GuestIrPath)) ||
		!TestTrue(TEXT("Formal C# compiler writes a WASM artifact"), !WasmPath.IsEmpty() && FPaths::FileExists(WasmPath)))
	{
		return true;
	}

	FString GuestIrJson;
	if (!FFileHelper::LoadFileToString(GuestIrJson, *GuestIrPath))
	{
		AddError(FString::Printf(TEXT("Failed to read formal C# Guest IR JSON: %s"), *GuestIrPath));
		return true;
	}
	TSharedPtr<FJsonObject> GuestIrRoot;
	const TSharedRef<TJsonReader<>> GuestIrReader = TJsonReaderFactory<>::Create(GuestIrJson);
	if (!FJsonSerializer::Deserialize(GuestIrReader, GuestIrRoot) || !GuestIrRoot.IsValid())
	{
		AddError(FString::Printf(TEXT("Formal C# Guest IR is not valid JSON: %s"), *GuestIrPath));
		return true;
	}
	TestEqual(TEXT("Guest IR schema version"), GuestIrRoot->GetIntegerField(TEXT("schema_version")), 2);
	TestEqual(TEXT("Guest IR version"), GuestIrRoot->GetStringField(TEXT("ir_version")), FString(TEXT("1.1")));
	TestEqual(TEXT("Guest IR language"), GuestIrRoot->GetStringField(TEXT("language")), FString(TEXT("csharp")));
	TestTrue(TEXT("Guest IR lowering succeeded"), GuestIrRoot->GetBoolField(TEXT("succeeded")));

	const TSharedPtr<FJsonObject>* ReportGuestIrPtr = nullptr;
	if (!RootObject->TryGetObjectField(TEXT("guest_ir"), ReportGuestIrPtr) || ReportGuestIrPtr == nullptr || !ReportGuestIrPtr->IsValid())
	{
		AddError(TEXT("Formal C# report does not include Guest IR metadata."));
		return true;
	}
	const FString ReportGuestIrSha = (*ReportGuestIrPtr)->GetStringField(TEXT("sha256"));
	const FString ReportSemanticSha = (*ReportGuestIrPtr)->GetStringField(TEXT("semantic_sha256"));
	TestEqual(TEXT("Report Guest IR SHA-256 shape"), ReportGuestIrSha.Len(), 64);

	const TSharedPtr<FJsonObject>* GuestProvenancePtr = nullptr;
	if (!GuestIrRoot->TryGetObjectField(TEXT("provenance"), GuestProvenancePtr) || GuestProvenancePtr == nullptr || !GuestProvenancePtr->IsValid())
	{
		AddError(TEXT("Formal C# Guest IR does not include provenance."));
		return true;
	}
	TestEqual(
		TEXT("Guest IR semantic provenance matches the build report"),
		(*GuestProvenancePtr)->GetStringField(TEXT("semantic_sha256")),
		ReportSemanticSha);

	FString ManifestJson;
	if (!FFileHelper::LoadFileToString(ManifestJson, *ManifestPath))
	{
		AddError(FString::Printf(TEXT("Failed to read formal C# manifest JSON: %s"), *ManifestPath));
		return true;
	}
	TSharedPtr<FJsonObject> ManifestRoot;
	const TSharedRef<TJsonReader<>> ManifestReader = TJsonReaderFactory<>::Create(ManifestJson);
	if (!FJsonSerializer::Deserialize(ManifestReader, ManifestRoot) || !ManifestRoot.IsValid())
	{
		AddError(FString::Printf(TEXT("Formal C# manifest is not valid JSON: %s"), *ManifestPath));
		return true;
	}
	const TSharedPtr<FJsonObject>* ManifestGuestIrPtr = nullptr;
	const TSharedPtr<FJsonObject>* ManifestStateMigrationPtr = nullptr;
	const TSharedPtr<FJsonObject>* ToolchainPtr = nullptr;
	if (!ManifestRoot->TryGetObjectField(TEXT("guest_ir"), ManifestGuestIrPtr) || ManifestGuestIrPtr == nullptr || !ManifestGuestIrPtr->IsValid() ||
		!ManifestRoot->TryGetObjectField(TEXT("state_migration"), ManifestStateMigrationPtr) || ManifestStateMigrationPtr == nullptr || !ManifestStateMigrationPtr->IsValid() ||
		!ManifestRoot->TryGetObjectField(TEXT("toolchain"), ToolchainPtr) || ToolchainPtr == nullptr || !ToolchainPtr->IsValid())
	{
		AddError(TEXT("Formal C# manifest is missing Guest IR, state migration, or toolchain metadata."));
		return true;
	}
	TestEqual(
		TEXT("Manifest identifies the formal compiler chain"),
		(*ToolchainPtr)->GetStringField(TEXT("compiler")),
		FString(TEXT("avidscript-csharp-guest-wasm")));
	TestEqual(
		TEXT("Manifest Guest IR hash matches the build report"),
		(*ManifestGuestIrPtr)->GetStringField(TEXT("sha256")),
		ReportGuestIrSha);
	TestEqual(TEXT("Manifest state migration strategy"),
		(*ManifestStateMigrationPtr)->GetStringField(TEXT("strategy")),
		FString(TEXT("host_snapshot")));

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(ManifestPath, Manifest, Bytecode, ManifestLoadResult))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return true;
	}
	TestTrue(TEXT("Loaded C# manifest enables host snapshot migration"), Manifest.StateMigration.IsEnabled());
	TestEqual(TEXT("Loaded C# manifest has six mutable sample state slots"), Manifest.StateMigration.Slots.Num(), 6);

	const TArray<FString> ExpectedLifecycleExports = {
		TEXT("avid_on_begin_play"),
		TEXT("avid_on_continuation_v2"),
		TEXT("avid_on_end_play"),
		TEXT("avid_on_event"),
		TEXT("avid_on_gameplay_event"),
		TEXT("avid_on_tick"),
		TEXT("avid_on_timer")
	};
	for (const FString& ExpectedExport : ExpectedLifecycleExports)
	{
		TestTrue(
			*FString::Printf(TEXT("Formal C# manifest requires export %s"), *ExpectedExport),
			Manifest.RequiredExports.Contains(ExpectedExport));
	}

	const bool bRequiresAddLocationOffset = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("actor_add_location_offset");
		});
	TestTrue(TEXT("C# source adapter manifest requires add location offset import"), bRequiresAddLocationOffset);
	const bool bRequiresGetLocation = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("actor_get_location");
		});
	TestTrue(TEXT("C# source adapter manifest requires get location import"), bRequiresGetLocation);
	const bool bRequiresGetRotation = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("actor_get_rotation");
		});
	const bool bRequiresSetRotation = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("actor_set_rotation");
		});
	TestTrue(TEXT("C# source adapter manifest requires get rotation import"), bRequiresGetRotation);
	TestTrue(TEXT("C# source adapter manifest requires set rotation import"), bRequiresSetRotation);
	const bool bRequiresGetScale = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("actor_get_scale");
		});
	const bool bRequiresSetScale = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("actor_set_scale");
		});
	TestTrue(TEXT("C# source adapter manifest requires get scale import"), bRequiresGetScale);
	TestTrue(TEXT("C# source adapter manifest requires set scale import"), bRequiresSetScale);
	const bool bRequiresOwnerSlot = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("owner_get_slot");
		});
	const bool bRequiresOwnerGeneration = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("owner_get_generation");
		});
	TestTrue(TEXT("C# source adapter manifest requires owner slot import"), bRequiresOwnerSlot);
	TestTrue(TEXT("C# source adapter manifest requires owner generation import"), bRequiresOwnerGeneration);
	const bool bRequiresGetRootComponent = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("actor_get_root_component");
		});
	const bool bRequiresComponentGetWorldLocation = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("scene_component_get_world_location");
		});
	const bool bRequiresComponentSetWorldLocation = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("scene_component_set_world_location");
		});
	TestTrue(TEXT("C# source adapter manifest requires root component import"), bRequiresGetRootComponent);
	TestTrue(TEXT("C# source adapter manifest requires component get world location import"), bRequiresComponentGetWorldLocation);
	TestTrue(TEXT("C# source adapter manifest requires component set world location import"), bRequiresComponentSetWorldLocation);
	const bool bRequiresTimerSetOnce = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("timer_set_once");
		});
	const bool bRequiresTimerCancel = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env") && RequiredImport.ImportName == TEXT("timer_cancel");
		});
	TestTrue(TEXT("C# source adapter manifest requires set-once Timer import"), bRequiresTimerSetOnce);
	TestFalse(TEXT("C# source adapter manifest omits unreachable Timer cancel import"), bRequiresTimerCancel);
	const bool bRequiresContinuationDelay = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env")
				&& RequiredImport.ImportName == TEXT("continuation_delay");
		});
	const bool bRequiresContinuationCancel = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env")
				&& RequiredImport.ImportName == TEXT("continuation_cancel");
		});
	TestTrue(TEXT("C# source adapter manifest requires continuation delay"), bRequiresContinuationDelay);
	TestTrue(TEXT("C# source adapter manifest retains continuation cancel for state rollback"), bRequiresContinuationCancel);
	const bool bRequiresContinuationStateStore = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env")
				&& RequiredImport.ImportName == TEXT("continuation_state_store");
		});
	const bool bRequiresContinuationStateRead = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env")
				&& RequiredImport.ImportName == TEXT("continuation_state_read");
		});
	TestTrue(TEXT("C# source adapter manifest requires continuation state store"), bRequiresContinuationStateStore);
	TestTrue(TEXT("C# source adapter manifest requires continuation state read"), bRequiresContinuationStateRead);
	const bool bRequiresContinuationLoadObject = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& RequiredImport)
		{
			return RequiredImport.ModuleName == TEXT("env")
				&& RequiredImport.ImportName == TEXT("continuation_load_object");
		});
	TestTrue(TEXT("C# source adapter manifest requires async object loading"), bRequiresContinuationLoadObject);

	IConsoleVariable* const StreamableDelegateDelay =
		IConsoleManager::Get().FindConsoleVariable(
			TEXT("s.StreamableDelegateDelayFrames"));
	if (!TestNotNull(
			TEXT("Streamable delegate delay console variable is available"),
			StreamableDelegateDelay))
	{
		return true;
	}
	const int32 PreviousStreamableDelegateDelay =
		StreamableDelegateDelay->GetInt();
	StreamableDelegateDelay->Set(0, ECVF_SetByCode);
	ON_SCOPE_EXIT
	{
		StreamableDelegateDelay->Set(
			PreviousStreamableDelegateDelay,
			ECVF_SetByCode);
	};
	UObject* const PreloadedAsyncObject = LoadObject<UObject>(
		nullptr,
		TEXT("/Engine/EngineMeshes/Cube.Cube"));
	if (!TestNotNull(
			TEXT("Async object lifecycle fixture preloads the Engine Cube"),
			PreloadedAsyncObject))
	{
		return true;
	}

	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
	AddInfo(AvidScriptRuntimeLaneLabel(Lane, TEXT("running shared C# source adapter lifecycle oracle")));
	UWorld* World = nullptr;
	if (!CreateCSharpContractWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript C# source adapter lifecycle world."));
		DestroyCSharpContractWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyCSharpContractWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));
	Actor->SetActorRotation(FRotator(5.0, 10.0, 15.0));
	Actor->SetActorScale3D(FVector(2.0, 2.0, 2.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult DummyRegisterResult;
	const FAvidScriptObjectHandle DummyHandle = Registry.RegisterObject(World, DummyRegisterResult);
	TestTrue(TEXT("Dummy UObject registers before source adapter actor"), DummyRegisterResult.bSucceeded);

	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers for source adapter artifact"), RegisterResult.bSucceeded);
	if (!RegisterResult.bSucceeded)
	{
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestEqual(TEXT("Dummy UObject occupies slot one"), DummyHandle.Slot, static_cast<uint32>(1));
	TestNotEqual(TEXT("Source adapter owner Actor does not use slot one"), ActorHandle.Slot, static_cast<uint32>(1));

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	HostContext.OwnerHandle = ActorHandle;

	FAvidScriptWasmReloadSession Session;
	Session.SetBackendSelectionForTesting(Lane.Selection);
	Session.SetHostContext(HostContext);

	FAvidScriptWasmReloadResult ReloadResult;
	if (!Session.LoadInitialModule(Bytecode.GetData(), Bytecode.Num(), Manifest, ReloadResult))
	{
		AddError(ReloadResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestTrue(TEXT("C# source adapter artifact loads"), ReloadResult.bSucceeded);
	TestEqual(TEXT("C# BeginPlay registers one live Timer"), Session.GetLivePendingTimerCount(), 1);
	TestEqual(TEXT("C# BeginPlay registers callback and async continuations"), Session.GetLivePendingContinuationCount(), 3);

	FAvidScriptWasmReloadManifest RejectedManifest = Manifest;
	RejectedManifest.ModuleId = TEXT("csharp_timer_rejected_reload");
	RejectedManifest.RequiredExports.Add(TEXT("avid_missing_reload_export"));
	TestFalse(TEXT("Reload missing an export is rejected"), Session.ReloadModule(Bytecode.GetData(), Bytecode.Num(), RejectedManifest, ReloadResult));
	TestTrue(TEXT("Rejected reload preserves live runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Rejected reload preserves old Timer"), Session.GetLivePendingTimerCount(), 1);
	TestEqual(TEXT("Rejected reload preserves all active continuations"), Session.GetLivePendingContinuationCount(), 3);
	TestTrue(TEXT("C# BeginPlay source moves actor"), Actor->GetActorLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("C# BeginPlay source resets actor rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# BeginPlay source resets actor scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));

	AdvanceCSharpContractWorld(World, 0.05f);
	FAvidScriptWasmSmokeResult TickResult;
	if (!Session.TickLive(1.0f / 60.0f, TickResult))
	{
		AddError(TickResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}
	TestAvidScriptRuntimeLaneIdentity(*this, Lane, TickResult);

	TestTrue(TEXT("C# first Tick resumes the UE continuation after normal Tick"), Actor->GetActorLocation().Equals(FVector(102.0, 240.0, 300.0), 0.01));
	TestEqual(TEXT("The first safe pump leaves object-load and async NextTick continuations"), Session.GetLivePendingContinuationCount(), 2);
	TestTrue(TEXT("C# first Tick source rotates actor"), Actor->GetActorRotation().Equals(FRotator(0.0, 1.5, 0.0), 0.01));
	TestTrue(TEXT("C# first Tick source scales actor"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.01), 0.01));

	FAvidScriptWasmSmokeResult SecondTickResult;
	if (!Session.TickLive(1.0f / 60.0f, SecondTickResult))
	{
		AddError(SecondTickResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestTrue(TEXT("C# second Tick receives the loaded-object result"), Actor->GetActorLocation().Equals(FVector(104.0, 240.0, 310.0), 0.01));
	TestEqual(TEXT("Delivered callback object result leaves the async NextTick continuation"), Session.GetLivePendingContinuationCount(), 1);
	TestTrue(TEXT("C# second Tick source preserves rotation state"), Actor->GetActorRotation().Equals(FRotator(0.0, 3.0, 0.0), 0.01));
	TestTrue(TEXT("C# second Tick source preserves scale state"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.02), 0.01));
	TestEqual(TEXT("C# source adapter tick count increments"), Session.GetLiveTickCallCount(), 2);

	FAvidScriptWasmSmokeResult ThirdTickResult;
	if (!Session.TickLive(1.0f / 60.0f, ThirdTickResult))
	{
		AddError(ThirdTickResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}
	TestTrue(TEXT("C# third Tick preserves the object result and fires Timer callback"), Actor->GetActorLocation().Equals(FVector(106.0, 240.0, 360.0), 0.01));
	TestEqual(TEXT("C# Timer callback count"), ThirdTickResult.TimerCallbackCount, 1);
	TestEqual(TEXT("C# Timer callback id"), ThirdTickResult.LastTimerCallbackId, 7);
	TestTrue(TEXT("C# Timer callback reports a handle"), ThirdTickResult.LastTimerHandle > 0);
	TestEqual(TEXT("Fired one-shot Timer leaves no pending Timer"), Session.GetLivePendingTimerCount(), 0);
	TestEqual(TEXT("Async NextTick resumes and schedules its object await"), Session.GetLivePendingContinuationCount(), 1);

	FAvidScriptWasmSmokeResult EventResult;
	TestTrue(TEXT("C# gameplay event dispatch succeeds"), Session.DispatchEventLive(3, 25.0f, EventResult));
	TestTrue(TEXT("C# gameplay event payload moves actor"), Actor->GetActorLocation().Equals(FVector(106.0, 265.0, 360.0), 0.01));
	TestEqual(TEXT("C# gameplay event callback count"), EventResult.EventCallbackCount, 1);
	TestEqual(TEXT("C# gameplay event id"), EventResult.LastEventId, 3);
	TestEqual(TEXT("C# gameplay event value"), EventResult.LastEventValue, 25.0f);

	AActor* OtherActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("C# typed callback first other Actor spawns"), OtherActor);
	FAvidScriptObjectHandleResult OtherRegisterResult;
	const FAvidScriptObjectHandle OtherHandle = Registry.RegisterObject(OtherActor, OtherRegisterResult);
	TestTrue(TEXT("C# typed callback first other Actor registers"), OtherRegisterResult.bSucceeded);
	if (OtherActor == nullptr || !OtherRegisterResult.bSucceeded)
	{
		DestroyCSharpContractWorld(World);
		return true;
	}
	AActor* SecondOtherActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("C# typed callback second other Actor spawns"), SecondOtherActor);
	FAvidScriptObjectHandleResult SecondOtherRegisterResult;
	const FAvidScriptObjectHandle SecondOtherHandle = Registry.RegisterObject(SecondOtherActor, SecondOtherRegisterResult);
	TestTrue(TEXT("C# typed callback second other Actor registers"), SecondOtherRegisterResult.bSucceeded);
	if (SecondOtherActor == nullptr || !SecondOtherRegisterResult.bSucceeded)
	{
		DestroyCSharpContractWorld(World);
		return true;
	}

	OtherActor->SetActorLocation(FVector(10.0, 20.0, 30.0));
	SecondOtherActor->SetActorLocation(FVector(40.0, 50.0, 60.0));
	FAvidScriptGameplayEvent TypedEvent;
	TypedEvent.Type = EAvidScriptGameplayEventType::BeginOverlap;
	TypedEvent.ObjectHandle = OtherHandle;
	TypedEvent.VectorValue = FVector3f(10.0f, 20.0f, 30.0f);
	TestTrue(TEXT("C# begin overlap dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# begin overlap maps AActor and FVector parameters"), OtherActor->GetActorLocation().Equals(FVector(10.0, 30.0, 30.0), 0.01));
	TypedEvent.ObjectHandle = SecondOtherHandle;
	TypedEvent.VectorValue = FVector3f(40.0f, 50.0f, 60.0f);
	TestTrue(TEXT("C# distinct Actor begin overlap dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# distinct Actor begin overlap is not suppressed"), SecondOtherActor->GetActorLocation().Equals(FVector(40.0, 60.0, 60.0), 0.01));
	TypedEvent.Type = EAvidScriptGameplayEventType::EndOverlap;
	TypedEvent.ObjectHandle = OtherHandle;
	TypedEvent.VectorValue = FVector3f(10.0f, 30.0f, 30.0f);
	TestTrue(TEXT("C# stale Actor end overlap dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# stale Actor end overlap does not end a different Actor"), OtherActor->GetActorLocation().Equals(FVector(10.0, 30.0, 30.0), 0.01));
	TypedEvent.Type = EAvidScriptGameplayEventType::BeginOverlap;
	TypedEvent.ObjectHandle = SecondOtherHandle;
	TypedEvent.VectorValue = FVector3f(100.0f, 200.0f, 300.0f);
	TestTrue(TEXT("C# duplicate begin overlap dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# gameplay state suppresses same Actor begin overlap"), SecondOtherActor->GetActorLocation().Equals(FVector(40.0, 60.0, 60.0), 0.01));

	TypedEvent.Type = EAvidScriptGameplayEventType::Hit;
	TypedEvent.VectorValue = FVector3f(1.0f, 2.0f, 3.0f);
	TestTrue(TEXT("C# hit dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# hit forwards normal impulse"), SecondOtherActor->GetActorLocation().Equals(FVector(41.0, 62.0, 63.0), 0.01));
	TestEqual(TEXT("legacy and typed callbacks share event accounting"), EventResult.EventCallbackCount, 6);

	TypedEvent = FAvidScriptGameplayEvent();
	TypedEvent.Type = EAvidScriptGameplayEventType::Input;
	TypedEvent.PrimaryId = 0;
	TypedEvent.SecondaryId = 1;
	TypedEvent.VectorValue = FVector3f(1.0f, 2.0f, 3.0f);
	TestTrue(TEXT("C# zero action input dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# first zero action input is not mistaken for a duplicate"), Actor->GetActorLocation().Equals(FVector(1.0, 3.0, 3.0), 0.01));
	TypedEvent.PrimaryId = 5;
	TypedEvent.SecondaryId = 2;
	TypedEvent.VectorValue = FVector3f(1.0f, 2.0f, 3.0f);
	TestTrue(TEXT("C# input dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# InputEvent maps ActionId TriggerEvent and Value"), Actor->GetActorLocation().Equals(FVector(6.0, 4.0, 3.0), 0.01));
	TestEqual(TEXT("input shares generic event accounting"), EventResult.EventCallbackCount, 8);
	TypedEvent.SecondaryId = 3;
	TypedEvent.VectorValue = FVector3f(4.0f, 5.0f, 6.0f);
	TestTrue(TEXT("C# distinct trigger input dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# same action with a different trigger is not suppressed"), Actor->GetActorLocation().Equals(FVector(9.0, 8.0, 6.0), 0.01));
	TypedEvent.VectorValue = FVector3f(100.0f, 200.0f, 300.0f);
	TestTrue(TEXT("C# duplicate input dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# gameplay state suppresses an exact duplicate input"), Actor->GetActorLocation().Equals(FVector(9.0, 8.0, 6.0), 0.01));
	TestEqual(TEXT("duplicate input still participates in runtime event accounting"), EventResult.EventCallbackCount, 10);

	FAvidScriptWasmRuntimeInstance* const RuntimeBeforeReload = Session.GetLiveRuntimeForTesting();
	TestNotNull(TEXT("C# source adapter exposes the live runtime for state verification"), RuntimeBeforeReload);
	TMap<FString, TArray<uint8>> StateBytesBeforeReload;
	FString StateReadError;
	if (RuntimeBeforeReload == nullptr)
	{
		DestroyCSharpContractWorld(World);
		return true;
	}
	for (const FAvidScriptWasmStateSlot& StateSlot : Manifest.StateMigration.Slots)
	{
		TArray<uint8> StateBytes;
		StateBytes.SetNumZeroed(StateSlot.Size);
		TestTrue(FString::Printf(TEXT("C# source adapter reads non-default state slot before reload: %s"), *StateSlot.StableId),
			RuntimeBeforeReload->ReadStateBytes(StateSlot.Offset, StateBytes, StateReadError));
		TestTrue(FString::Printf(TEXT("C# source adapter writes a non-default state slot before reload: %s"), *StateSlot.StableId),
			StateBytes.ContainsByPredicate([](uint8 Value) { return Value != 0; }));
		StateBytesBeforeReload.Add(StateSlot.StableId, MoveTemp(StateBytes));
	}

	FAvidScriptWasmReloadManifest ReloadedManifest = Manifest;
	TestTrue(TEXT("Compatible C# Timer reload applies"), Session.ReloadModule(Bytecode.GetData(), Bytecode.Num(), ReloadedManifest, ReloadResult));
	TestTrue(TEXT("Compatible C# reload attempts state migration"), ReloadResult.bStateMigrationAttempted);
	TestTrue(TEXT("Compatible C# reload applies state migration"), ReloadResult.bStateMigrationApplied);
	TestEqual(TEXT("Compatible C# reload migrates gameplay state slots"), ReloadResult.StateMigrationMigratedSlotCount, 6);
	TestEqual(TEXT("Compatible C# reload migrates gameplay state bytes"), ReloadResult.StateMigrationMigratedByteCount, 22);
	FAvidScriptWasmRuntimeInstance* const RuntimeAfterReload = Session.GetLiveRuntimeForTesting();
	TestNotNull(TEXT("C# source adapter exposes the reloaded runtime for state verification"), RuntimeAfterReload);
	if (RuntimeAfterReload == nullptr)
	{
		DestroyCSharpContractWorld(World);
		return true;
	}
	for (const FAvidScriptWasmStateSlot& StateSlot : ReloadedManifest.StateMigration.Slots)
	{
		TArray<uint8> StateBytesAfterReload;
		StateBytesAfterReload.SetNumZeroed(StateSlot.Size);
		TestTrue(FString::Printf(TEXT("C# source adapter reads migrated state slot after reload: %s"), *StateSlot.StableId),
			RuntimeAfterReload->ReadStateBytes(StateSlot.Offset, StateBytesAfterReload, StateReadError));
		const TArray<uint8>* const ExpectedStateBytes = StateBytesBeforeReload.Find(StateSlot.StableId);
		TestNotNull(FString::Printf(TEXT("C# source adapter retains a pre-reload value for state slot: %s"), *StateSlot.StableId), ExpectedStateBytes);
		if (ExpectedStateBytes != nullptr)
		{
			TestEqual(FString::Printf(TEXT("C# source adapter preserves exact state bytes across reload: %s"), *StateSlot.StableId), StateBytesAfterReload, *ExpectedStateBytes);
		}
	}
	TestEqual(TEXT("Reloaded runtime owns one fresh Timer"), Session.GetLivePendingTimerCount(), 1);
	TestEqual(TEXT("Reloaded runtime owns fresh callback and async continuations"), Session.GetLivePendingContinuationCount(), 3);
	TestEqual(TEXT("Reloaded runtime callback count starts fresh"), Session.GetLiveTimerCallbackCount(), 0);
	TestEqual(TEXT("Reloaded runtime event count starts fresh"), Session.GetLiveEventCallbackCount(), 0);
	TypedEvent.Type = EAvidScriptGameplayEventType::BeginOverlap;
	TypedEvent.ObjectHandle = SecondOtherHandle;
	TypedEvent.VectorValue = FVector3f(500.0f, 600.0f, 700.0f);
	TestTrue(TEXT("C# reloaded duplicate begin overlap dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# reloaded active overlap suppresses the same Actor"), SecondOtherActor->GetActorLocation().Equals(FVector(41.0, 62.0, 63.0), 0.01));
	TypedEvent.Type = EAvidScriptGameplayEventType::EndOverlap;
	TypedEvent.ObjectHandle = OtherHandle;
	TypedEvent.VectorValue = FVector3f(10.0f, 30.0f, 30.0f);
	TestTrue(TEXT("C# reloaded stale Actor end overlap dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# reloaded stale Actor end does not end the tracked Actor"), OtherActor->GetActorLocation().Equals(FVector(10.0, 30.0, 30.0), 0.01));
	TypedEvent.ObjectHandle = SecondOtherHandle;
	TypedEvent.VectorValue = FVector3f(40.0f, 60.0f, 60.0f);
	TestTrue(TEXT("C# reloaded tracked Actor end overlap dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# reloaded tracked Actor end overlap retains its handle identity"), SecondOtherActor->GetActorLocation().Equals(FVector(40.0, 60.0, 65.0), 0.01));
	TypedEvent = FAvidScriptGameplayEvent();
	TypedEvent.Type = EAvidScriptGameplayEventType::Input;
	TypedEvent.PrimaryId = 5;
	TypedEvent.SecondaryId = 3;
	TypedEvent.VectorValue = FVector3f(500.0f, 600.0f, 700.0f);
	TestTrue(TEXT("C# reloaded duplicate input dispatch succeeds"), Session.DispatchGameplayEventLive(TypedEvent, EventResult));
	TestTrue(TEXT("C# reloaded exact input pair remains suppressed after BeginPlay reset"), Actor->GetActorLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
	FAvidScriptWasmSmokeResult ReloadTickResult;
	for (int32 TickIndex = 0; TickIndex < 3; ++TickIndex)
	{
		if (!Session.TickLive(1.0f / 60.0f, ReloadTickResult))
		{
			AddError(ReloadTickResult.ErrorMessage);
			DestroyCSharpContractWorld(World);
			return true;
		}
	}
	TestEqual(TEXT("Successful reload fires only the fresh Timer"), Session.GetLiveTimerCallbackCount(), 1);
	TestEqual(TEXT("Successful reload does not retain old pending Timer"), Session.GetLivePendingTimerCount(), 0);
	TestTrue(TEXT("Reloaded object and Timer callbacks each have one movement effect"), Actor->GetActorLocation().Equals(FVector(106.0, 200.0, 360.0), 0.01));
	TestEqual(TEXT("Session Tick does not advance delay or async NextTick TimerManager producers"), Session.GetLivePendingContinuationCount(), 2);

	if (Manifest.RequiredExports.Contains(TEXT("avid_on_debug_resume")))
	{
		TestTrue(
			TEXT("Debug artifact attaches an empty-breakpoint Session debugger"),
			Session.AttachDebugger(TConstArrayView<uint64>()));
		TestTrue(TEXT("Debug artifact accepts pause-next"), Session.RequestDebugPause());
		const FVector LocationBeforePause = Actor->GetActorLocation();
		FAvidScriptWasmSmokeResult PausedTickResult;
		TestTrue(
			TEXT("Instrumented Tick cooperatively returns after committing a frame"),
			Session.TickLive(1.0f / 60.0f, PausedTickResult));
		const FAvidScriptDebugSessionSnapshot PausedSnapshot =
			Session.GetDebugSnapshot();
		TestEqual(
			TEXT("Instrumented Tick enters Paused"),
			PausedSnapshot.State,
			EAvidScriptDebugSessionState::Paused);
		TestTrue(
			TEXT("Paused Tick owns a bounded suspension frame"),
			PausedSnapshot.SuspensionToken > 0
				&& PausedSnapshot.ResumeRoute > 0
				&& PausedSnapshot.FrameByteCount > 0
				&& PausedSnapshot.FrameByteCount <= 4096);

		FAvidScriptWasmSmokeResult BlockedTickResult;
		TestFalse(
			TEXT("Paused Session rejects a second Tick entry"),
			Session.TickLive(1.0f / 60.0f, BlockedTickResult));
		TestEqual(
			TEXT("Paused entry rejection is explicit"),
			BlockedTickResult.ErrorCategory,
			FString(TEXT("debug_execution_suspended")));

		FAvidScriptWasmSmokeResult StepResult;
		TestTrue(
			TEXT("StepInto resumes through avid_on_debug_resume"),
			Session.StepIntoDebugExecution(StepResult));
		const FAvidScriptDebugSessionSnapshot SteppedSnapshot =
			Session.GetDebugSnapshot();
		TestEqual(
			TEXT("StepInto pauses at a later sequence point"),
			SteppedSnapshot.State,
			EAvidScriptDebugSessionState::Paused);
		TestTrue(
			TEXT("StepInto replaces the consumed suspension token"),
			SteppedSnapshot.PauseSequence > PausedSnapshot.PauseSequence
				&& SteppedSnapshot.SuspensionToken > 0
				&& SteppedSnapshot.SuspensionToken != PausedSnapshot.SuspensionToken);

		FAvidScriptWasmSmokeResult ContinueResult;
		TestTrue(
			TEXT("Continue completes the suspended Tick body"),
			Session.ContinueDebugExecution(ContinueResult));
		TestEqual(
			TEXT("Continue returns the Session to Running"),
			Session.GetDebugSnapshot().State,
			EAvidScriptDebugSessionState::Running);
		TestTrue(
			TEXT("Resumed Tick commits gameplay work after the pause"),
			!Actor->GetActorLocation().Equals(LocationBeforePause, 0.01));
		TestTrue(TEXT("Running debugger detaches cleanly"), Session.DetachDebugger());
	}

	FAvidScriptWasmSmokeResult EndPlayResult;
	if (!Session.EndPlayLive(EndPlayResult))
	{
		AddError(EndPlayResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestEqual(TEXT("EndPlay result preserves Timer callback count"), EndPlayResult.TimerCallbackCount, 1);
	TestEqual(TEXT("EndPlay cancels the outstanding continuations"), Session.GetLivePendingContinuationCount(), 0);
	TestTrue(TEXT("C# EndPlay source moves actor"), Actor->GetActorLocation().Equals(FVector::ZeroVector, 0.01));
	TestTrue(TEXT("C# EndPlay source resets actor rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# EndPlay source resets actor scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));
	TestTrue(TEXT("C# EndPlay source calls export"), EndPlayResult.bEndPlayCalled);

	DestroyCSharpContractWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCSharpArrayUFunctionEndToEndTest,
	"AvidScript.Guest.CSharp.ArrayUFunctionEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpArrayUFunctionEndToEndTest::RunTest(
	const FString& Parameters)
{
	const FString ReportPath = GetCSharpArrayToolchainReportPath();
	if (!FPaths::FileExists(ReportPath))
	{
		AddError(FString::Printf(
			TEXT("P57.11B3 C# array report is missing; run the Editor array emitter test first. report=%s"),
			*ReportPath));
		return true;
	}

	FString ReportJson;
	TSharedPtr<FJsonObject> Report;
	if (!FFileHelper::LoadFileToString(ReportJson, *ReportPath)
		|| !FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(ReportJson),
			Report)
		|| !Report.IsValid())
	{
		AddError(TEXT("P57.11B3 C# array report is unreadable or invalid."));
		return true;
	}
	if (!TestEqual(
			TEXT("P57.11B3 array source builds through the formal toolchain"),
			Report->GetStringField(TEXT("result")),
			FString(TEXT("direct_abi_built"))))
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* Artifacts = nullptr;
	if (!Report->TryGetObjectField(TEXT("artifacts"), Artifacts)
		|| Artifacts == nullptr
		|| !Artifacts->IsValid())
	{
		AddError(TEXT("P57.11B3 C# array report has no artifact metadata."));
		return true;
	}
	const FString ManifestPath = ResolveCSharpReportArtifactPath(
		(*Artifacts)->GetStringField(TEXT("manifest_file")));
	const FString GuestIrPath = ResolveCSharpReportArtifactPath(
		(*Artifacts)->GetStringField(TEXT("guest_ir_file")));
	if (!TestTrue(TEXT("P57.11B3 array manifest exists"), FPaths::FileExists(ManifestPath))
		|| !TestTrue(TEXT("P57.11B3 array Guest IR exists"), FPaths::FileExists(GuestIrPath)))
	{
		return true;
	}

	FString GuestIrJson;
	TSharedPtr<FJsonObject> GuestIr;
	if (!FFileHelper::LoadFileToString(GuestIrJson, *GuestIrPath)
		|| !FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(GuestIrJson),
			GuestIr)
		|| !GuestIr.IsValid())
	{
		AddError(TEXT("P57.11B3 array Guest IR is unreadable or invalid."));
		return true;
	}
	TSet<FString> ImportNames;
	for (const TSharedPtr<FJsonValue>& ImportValue :
		GuestIr->GetArrayField(TEXT("imports")))
	{
		const TSharedPtr<FJsonObject> Import = ImportValue->AsObject();
		if (Import.IsValid()
			&& Import->GetStringField(TEXT("module")) == TEXT("avidscript"))
		{
			ImportNames.Add(Import->GetStringField(TEXT("name")));
		}
	}
	for (const TCHAR* RequiredImport : {
		TEXT("avid_value_array_length"),
		TEXT("avid_value_array_read_range"),
		TEXT("avid_value_array_write_range"),
		TEXT("avid_value_release") })
	{
		const FString RequiredImportName(RequiredImport);
		TestTrue(
			TEXT("P57.11B3 Guest IR contains ") + RequiredImportName,
			ImportNames.Contains(RequiredImportName));
	}
	TestFalse(
		TEXT("P57.11D Guest IR removes the element load import"),
		ImportNames.Contains(TEXT("avid_value_array_load")));
	TestFalse(
		TEXT("P57.11D Guest IR removes the element store import"),
		ImportNames.Contains(TEXT("avid_value_array_store")));
	TestTrue(
		TEXT("P57.11D Guest IR contains compiler-managed array loads"),
		GuestIrJson.Contains(TEXT("\"op\": \"array_region_load\"")));
	TestTrue(
		TEXT("P57.11D Guest IR contains compiler-managed array stores"),
		GuestIrJson.Contains(TEXT("\"op\": \"array_region_store\"")));

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			ManifestPath,
			Manifest,
			Bytecode,
			ManifestLoadResult))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return true;
	}

	FModuleManager::LoadModulePtr<IModuleInterface>(TEXT("AvidScriptEditor"));
	UClass* const FixtureClass = FindObject<UClass>(
		nullptr,
		TEXT("/Script/AvidScriptEditor.AvidScriptCSharpNameStringTestActor"));
	if (!TestNotNull(TEXT("P57.11B3 reflected array fixture class loads"), FixtureClass))
	{
		return true;
	}
	if (!TestTrue(
			TEXT("P57.11B3 manifest carries a verified binding package"),
			Manifest.BindingPackage.IsValid()))
	{
		return true;
	}
	TestEqual(
		TEXT("P57.11B3 package binds the exact array fixture owner"),
		Manifest.BindingPackage->GetExpectedSelfClass(),
		FixtureClass);

	TArray<FAvidScriptRuntimeBackendTestLane> Lanes =
		GetAvidScriptRuntimeBackendTestLanes();
	if (!TestEqual(TEXT("P57.11B3 array gate requires WAMR and Wasmtime"), Lanes.Num(), 2))
	{
		return true;
	}
	for (const FAvidScriptRuntimeBackendTestLane& Lane : Lanes)
	{
		UWorld* FixtureWorld = nullptr;
		if (!TestTrue(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("array fixture world creates")),
				CreateCSharpContractWorld(FixtureWorld)))
		{
			continue;
		}
		ON_SCOPE_EXIT
		{
			DestroyCSharpContractWorld(FixtureWorld);
		};
		FActorSpawnParameters SpawnParameters;
		UObject* const FixtureObject = FixtureWorld->SpawnActor<AActor>(
			FixtureClass,
			FTransform::Identity,
			SpawnParameters);
		if (!TestNotNull(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("array fixture instance creates")),
				FixtureObject))
		{
			continue;
		}
		FAvidScriptObjectRegistry Registry;
		FAvidScriptObjectHandleResult RegisterResult;
		const FAvidScriptObjectHandle OwnerHandle =
			Registry.RegisterObject(FixtureObject, RegisterResult);
		if (!TestTrue(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("array fixture registers")),
				RegisterResult.bSucceeded))
		{
			continue;
		}

		FAvidScriptWasmHostContext HostContext;
		HostContext.ObjectRegistry = &Registry;
		HostContext.OwnerHandle = OwnerHandle;
		HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
		FAvidScriptWasmReloadSession Session;
		Session.SetBackendSelectionForTesting(Lane.Selection);
		Session.SetHostContext(HostContext);
		FAvidScriptWasmReloadResult ReloadResult;
		if (!Session.LoadInitialModule(
				Bytecode.GetData(),
				Bytecode.Num(),
				Manifest,
				ReloadResult))
		{
			AddError(AvidScriptRuntimeLaneLabel(Lane, *ReloadResult.ErrorMessage));
			continue;
		}

		TArray<int32> Values;
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay array property reads")),
			ReadCSharpIntArrayProperty(*FixtureObject, Values));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay array property count")),
			Values.Num(),
			0);

		FAvidScriptWasmSmokeResult TickResult;
		if (!Session.TickLive(1.0f / 60.0f, TickResult))
		{
			AddError(AvidScriptRuntimeLaneLabel(Lane, *TickResult.ErrorMessage));
			continue;
		}
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("compiler-managed array region host crossings")),
			TickResult.HostImportCallCount,
			9);
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("Tick array property reads")),
			ReadCSharpIntArrayProperty(*FixtureObject, Values));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("Tick array property count")),
			Values.Num(),
			3);
		if (Values.Num() == 3)
		{
			TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Tick array element 0")), Values[0], 11);
			TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Tick array element 1")), Values[1], 1);
			TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Tick array element 2")), Values[2], 2);
		}
		FAvidScriptWasmRuntimeInstance* const Runtime =
			Session.GetLiveRuntimeForTesting();
		if (TestNotNull(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("array runtime remains live")),
				Runtime))
		{
			TestEqual(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("explicit release drains array capabilities")),
				Runtime->GetArrayValueHeapForTesting().GetStats().LiveValues,
				0);
		}
		FAvidScriptWasmSmokeResult EndPlayResult;
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("array EndPlay succeeds")),
			Session.EndPlayLive(EndPlayResult));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCSharpNameStringUFunctionEndToEndTest,
	"AvidScript.Guest.CSharp.NameStringUFunctionEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpNameStringUFunctionEndToEndTest::RunTest(
	const FString& Parameters)
{
	const FString ReportPath = GetCSharpNameStringToolchainReportPath();
	if (!FPaths::FileExists(ReportPath))
	{
		AddError(FString::Printf(
			TEXT("Phase 57.11B2 C# report is missing; stage the generated binding package and run BuildCSharpActorLifecycle.ps1 before Automation. report=%s"),
			*ReportPath));
		return true;
	}

	FString ReportJson;
	if (!FFileHelper::LoadFileToString(ReportJson, *ReportPath))
	{
		AddError(FString::Printf(TEXT("Failed to read Phase 57.11B2 C# report: %s"), *ReportPath));
		return true;
	}

	TSharedPtr<FJsonObject> Report;
	const TSharedRef<TJsonReader<>> ReportReader =
		TJsonReaderFactory<>::Create(ReportJson);
	if (!FJsonSerializer::Deserialize(ReportReader, Report) || !Report.IsValid())
	{
		AddError(FString::Printf(TEXT("Phase 57.11B2 C# report is invalid JSON: %s"), *ReportPath));
		return true;
	}

	if (!TestEqual(
			TEXT("Phase 57.11B2 source builds through the formal direct ABI toolchain"),
			Report->GetStringField(TEXT("result")),
			FString(TEXT("direct_abi_built"))))
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* SourceObjectPtr = nullptr;
	const TSharedPtr<FJsonObject>* BindingPackageObjectPtr = nullptr;
	const TSharedPtr<FJsonObject>* ArtifactsObjectPtr = nullptr;
	if (!Report->TryGetObjectField(TEXT("source"), SourceObjectPtr)
		|| SourceObjectPtr == nullptr || !SourceObjectPtr->IsValid()
		|| !Report->TryGetObjectField(TEXT("binding_package"), BindingPackageObjectPtr)
		|| BindingPackageObjectPtr == nullptr || !BindingPackageObjectPtr->IsValid()
		|| !Report->TryGetObjectField(TEXT("artifacts"), ArtifactsObjectPtr)
		|| ArtifactsObjectPtr == nullptr || !ArtifactsObjectPtr->IsValid())
	{
		AddError(TEXT("Phase 57.11B2 report is missing source, binding package, or artifact metadata."));
		return true;
	}

	const FString ExpectedSourcePath = GetCSharpNameStringSourcePath();
	const FString ReportSourcePath = ResolveCSharpReportArtifactPath(
		(*SourceObjectPtr)->GetStringField(TEXT("file")));
	TestEqual(
		TEXT("Formal Roslyn input is the Phase 57.11B2 C# fixture"),
		ReportSourcePath,
		ExpectedSourcePath);

	FString ReferenceSourceArtifactPath;
	(*BindingPackageObjectPtr)->TryGetStringField(
		TEXT("reference_source_file"),
		ReferenceSourceArtifactPath);
	const FString ReferenceSourcePath =
		ResolveCSharpReportArtifactPath(ReferenceSourceArtifactPath);
	if (!TestTrue(
			TEXT("Build report retains the generated C# facade source"),
			!ReferenceSourcePath.IsEmpty() && FPaths::FileExists(ReferenceSourcePath)))
	{
		return true;
	}

	FString GeneratedFacade;
	if (!FFileHelper::LoadFileToString(GeneratedFacade, *ReferenceSourcePath))
	{
		AddError(FString::Printf(TEXT("Failed to read generated C# facade: %s"), *ReferenceSourcePath));
		return true;
	}
	TestTrue(TEXT("Generated facade exposes FName input"),
		GeneratedFacade.Contains(TEXT("ConstRefFName(string InName)")));
	TestTrue(TEXT("Generated facade exposes FString input"),
		GeneratedFacade.Contains(TEXT("ConstRefFString(string InString)")));
	TestTrue(TEXT("Generated facade exposes FName ref/out/return"),
		GeneratedFacade.Contains(TEXT("RefFName(ref string InOutName)"))
		&& GeneratedFacade.Contains(TEXT("OutFName(out string OutName)"))
		&& GeneratedFacade.Contains(TEXT("public string ReturnFName()")));
	TestTrue(TEXT("Generated facade exposes FString ref/out/return"),
		GeneratedFacade.Contains(TEXT("RefFString(ref string InOutString)"))
		&& GeneratedFacade.Contains(TEXT("OutFString(out string OutString)"))
		&& GeneratedFacade.Contains(TEXT("public string ReturnFString()")));
	TestTrue(TEXT("Generated facade exposes FName and FString properties"),
		GeneratedFacade.Contains(TEXT("public string ReadableFName"))
		&& GeneratedFacade.Contains(TEXT("public string ReadableFString")));

	int32 UsedBindingImportCount = 0;
	TestTrue(
		TEXT("Build report records the generated binding import slice"),
		(*BindingPackageObjectPtr)->TryGetNumberField(
			TEXT("used_import_count"),
			UsedBindingImportCount));
	TestEqual(
		TEXT("C# reachability retains twelve bindings plus the owner capability import"),
		UsedBindingImportCount,
		13);

	FString ManifestArtifactPath;
	FString GuestIrArtifactPath;
	FString WasmArtifactPath;
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("manifest_file"), ManifestArtifactPath);
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("guest_ir_file"), GuestIrArtifactPath);
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("wasm_file"), WasmArtifactPath);
	const FString ManifestPath = ResolveCSharpReportArtifactPath(ManifestArtifactPath);
	const FString GuestIrPath = ResolveCSharpReportArtifactPath(GuestIrArtifactPath);
	const FString WasmPath = ResolveCSharpReportArtifactPath(WasmArtifactPath);
	if (!TestTrue(TEXT("Formal Roslyn/Guest compiler writes Guest IR"),
			!GuestIrPath.IsEmpty() && FPaths::FileExists(GuestIrPath))
		|| !TestTrue(TEXT("Formal Guest IR backend writes Wasm"),
			!WasmPath.IsEmpty() && FPaths::FileExists(WasmPath))
		|| !TestTrue(TEXT("Formal build writes the runtime manifest"),
			!ManifestPath.IsEmpty() && FPaths::FileExists(ManifestPath)))
	{
		return true;
	}

	FString GuestIrJson;
	if (!FFileHelper::LoadFileToString(GuestIrJson, *GuestIrPath))
	{
		AddError(FString::Printf(TEXT("Failed to read Phase 57.11B2 Guest IR: %s"), *GuestIrPath));
		return true;
	}
	TSharedPtr<FJsonObject> GuestIr;
	const TSharedRef<TJsonReader<>> GuestIrReader =
		TJsonReaderFactory<>::Create(GuestIrJson);
	if (!FJsonSerializer::Deserialize(
			GuestIrReader,
			GuestIr)
		|| !GuestIr.IsValid())
	{
		AddError(FString::Printf(TEXT("Phase 57.11B2 Guest IR is invalid: %s"), *GuestIrPath));
		return true;
	}
	TestEqual(TEXT("Phase 57.11B2 Guest IR schema"),
		GuestIr->GetIntegerField(TEXT("schema_version")), 2);
	TestEqual(TEXT("Phase 57.11B2 Guest IR version"),
		GuestIr->GetStringField(TEXT("ir_version")), FString(TEXT("1.1")));
	TestTrue(TEXT("Phase 57.11B2 Guest IR lowering succeeds"),
		GuestIr->GetBoolField(TEXT("succeeded")));

	const TArray<TSharedPtr<FJsonValue>>* GuestImports = nullptr;
	int32 DynamicBindingImportCount = 0;
	if (GuestIr->TryGetArrayField(TEXT("imports"), GuestImports)
		&& GuestImports != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& ImportValue : *GuestImports)
		{
			const TSharedPtr<FJsonObject> Import = ImportValue.IsValid()
				? ImportValue->AsObject()
				: nullptr;
			if (Import.IsValid()
				&& Import->GetStringField(TEXT("module")) == TEXT("avidscript")
				&& Import->GetStringField(TEXT("name")).StartsWith(TEXT("avid_ue_")))
			{
				++DynamicBindingImportCount;
			}
		}
	}
	TestEqual(
		TEXT("Guest IR directly imports every exercised generated binding"),
		DynamicBindingImportCount,
		12);

	FModuleManager::LoadModulePtr<IModuleInterface>(TEXT("AvidScriptEditor"));
	UClass* const FixtureClass = FindObject<UClass>(
		nullptr,
		TEXT("/Script/AvidScriptEditor.AvidScriptCSharpNameStringTestActor"));
	if (!TestNotNull(TEXT("Phase 57.11B2 reflected UFUNCTION fixture class loads"), FixtureClass))
	{
		return true;
	}

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			ManifestPath,
			Manifest,
			Bytecode,
			ManifestLoadResult))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return true;
	}
	if (!TestTrue(
			TEXT("Runtime manifest loads a verified generated binding package"),
			Manifest.BindingPackage.IsValid()))
	{
		return true;
	}

	TestEqual(
		TEXT("Generated package binds the exact UFUNCTION fixture owner"),
		Manifest.BindingPackage->GetExpectedSelfClass(),
		FixtureClass);

	TArray<FAvidScriptRuntimeBackendTestLane> Lanes =
		GetAvidScriptRuntimeBackendTestLanes();
	if (!TestEqual(
			TEXT("Phase 57.11B2 end-to-end gate requires WAMR and Wasmtime"),
			Lanes.Num(),
			2))
	{
		return true;
	}

	FString InitialName(TEXT("Input_Name_"));
	InitialName.AppendChar(static_cast<TCHAR>(0x540d));
	FString InitialString(TEXT("Input_String_"));
	InitialString.AppendChar(static_cast<TCHAR>(0x503c));
	const FString NoneName = FName(NAME_None).ToString();
	UWorld* FixtureWorld = nullptr;
	if (!TestTrue(
			TEXT("Phase 57.11B2 UFUNCTION fixture world creates"),
			CreateCSharpContractWorld(FixtureWorld)))
	{
		return true;
	}
	ON_SCOPE_EXIT
	{
		DestroyCSharpContractWorld(FixtureWorld);
	};

	for (const FAvidScriptRuntimeBackendTestLane& Lane : Lanes)
	{
		AddInfo(AvidScriptRuntimeLaneLabel(
			Lane,
			TEXT("running generated C# FName/FString UFUNCTION oracle")));
		FActorSpawnParameters SpawnParameters;
		AActor* const FixtureObject = FixtureWorld->SpawnActor<AActor>(
			FixtureClass,
			FTransform::Identity,
			SpawnParameters);
		if (!TestNotNull(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("UFUNCTION fixture instance creates")),
				FixtureObject))
		{
			continue;
		}
		FString DirectReturnString;
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("direct ProcessEvent string oracle invokes")),
			InvokeCSharpReturnStringOracle(*FixtureObject, DirectReturnString));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("direct ProcessEvent string oracle returns value")),
			DirectReturnString,
			FString(TEXT("Avid")));

		FAvidScriptObjectRegistry Registry;
		FAvidScriptObjectHandleResult RegisterResult;
		const FAvidScriptObjectHandle OwnerHandle =
			Registry.RegisterObject(FixtureObject, RegisterResult);
		if (!TestTrue(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("UFUNCTION fixture registers")),
				RegisterResult.bSucceeded))
		{
			continue;
		}

		FAvidScriptWasmHostContext HostContext;
		HostContext.ObjectRegistry = &Registry;
		HostContext.OwnerHandle = OwnerHandle;
		HostContext.ActorWritePolicy =
			EAvidScriptActorWritePolicy::AllowWrites;

		FAvidScriptWasmReloadSession Session;
		Session.SetBackendSelectionForTesting(Lane.Selection);
		Session.SetHostContext(HostContext);
		FAvidScriptWasmReloadResult ReloadResult;
		if (!Session.LoadInitialModule(
				Bytecode.GetData(),
				Bytecode.Num(),
				Manifest,
				ReloadResult))
		{
			AddError(AvidScriptRuntimeLaneLabel(Lane, *ReloadResult.ErrorMessage));
			continue;
		}
		auto AddHeapInfo = [this, &Lane, &Session](const TCHAR* Stage)
		{
			FAvidScriptWasmRuntimeInstance* const Runtime =
				Session.GetLiveRuntimeForTesting();
			if (Runtime == nullptr)
			{
				AddError(AvidScriptRuntimeLaneLabel(
					Lane,
					TEXT("UTF-8 heap diagnostics require a live runtime")));
				return;
			}
			const FAvidScriptUtf8ValueHeap& Heap =
				Runtime->GetUtf8ValueHeapForTesting();
			AddInfo(AvidScriptRuntimeLaneLabel(
				Lane,
				*FString::Printf(
					TEXT("%s UTF-8 heap live=%d reserved=%d"),
					Stage,
					Heap.GetLiveValueCount(),
					Heap.GetReservedValueCount())));
		};
		AddHeapInfo(TEXT("begin play"));

		auto TestProperties = [this, &Lane, FixtureObject](
			const TCHAR* Stage,
			const FString& ExpectedName,
			const FString& ExpectedString)
		{
			FString ActualName;
			FString ActualString;
			const bool bRead = ReadCSharpNameStringProperties(
				*FixtureObject,
				ActualName,
				ActualString);
			TestTrue(
				*AvidScriptRuntimeLaneLabel(Lane, TEXT("reflected string properties read")),
				bRead);
			if (bRead)
			{
				TestEqual(
					*AvidScriptRuntimeLaneLabel(Lane, *FString::Printf(TEXT("%s FName"), Stage)),
					ActualName,
					ExpectedName);
				TestEqual(
					*AvidScriptRuntimeLaneLabel(Lane, *FString::Printf(TEXT("%s FString"), Stage)),
					ActualString,
					ExpectedString);
			}
		};

		TestProperties(TEXT("property set"), InitialName, InitialString);
		FAvidScriptWasmSmokeResult TickResult;
		if (!Session.TickLive(1.0f / 60.0f, TickResult))
		{
			AddError(AvidScriptRuntimeLaneLabel(Lane, *TickResult.ErrorMessage));
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, TickResult);
		AddHeapInfo(TEXT("property get"));
		TestProperties(TEXT("property get and const-ref input"), InitialName, InitialString);

		if (!Session.TickLive(1.0f / 60.0f, TickResult))
		{
			AddError(AvidScriptRuntimeLaneLabel(Lane, *TickResult.ErrorMessage));
			continue;
		}
		AddHeapInfo(TEXT("ref output"));
		TestProperties(
			TEXT("ref output"),
			NoneName,
			InitialString + TEXT("Avid"));

		if (!Session.TickLive(1.0f / 60.0f, TickResult))
		{
			AddError(AvidScriptRuntimeLaneLabel(Lane, *TickResult.ErrorMessage));
			continue;
		}
		AddHeapInfo(TEXT("out output"));
		TestProperties(TEXT("out output"), NoneName, TEXT("Avid"));

		if (!Session.TickLive(1.0f / 60.0f, TickResult))
		{
			AddError(AvidScriptRuntimeLaneLabel(Lane, *TickResult.ErrorMessage));
			continue;
		}
		AddHeapInfo(TEXT("return output"));
		TestProperties(TEXT("return output"), NoneName, TEXT("Avid"));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("all four scripted stages tick")),
			Session.GetLiveTickCallCount(),
			4);

		FAvidScriptWasmSmokeResult EndPlayResult;
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("C# EndPlay completes")),
			Session.EndPlayLive(EndPlayResult));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCSharpToolchainReportSmokeTest,
	"AvidScript.Guest.CSharp.ToolchainReportSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCSharpToolchainReportSmokeTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetCSharpToolchainReportPath();
	if (!FPaths::FileExists(ReportPath))
	{
		AddWarning(FString::Printf(
			TEXT("C# toolchain report is missing; run BuildCSharpActorLifecycle.ps1 before using this as a true toolchain smoke. report=%s"),
			*ReportPath));
		return true;
	}

	FString ReportJson;
	if (!FFileHelper::LoadFileToString(ReportJson, *ReportPath))
	{
		AddError(FString::Printf(TEXT("Failed to read C# toolchain report: %s"), *ReportPath));
		return true;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReportJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		AddError(FString::Printf(TEXT("C# toolchain report is not valid JSON: %s"), *ReportPath));
		return true;
	}

	FString Language;
	TestTrue(TEXT("Report declares language"), RootObject->TryGetStringField(TEXT("language"), Language));
	TestEqual(TEXT("Report language"), Language, FString(TEXT("csharp")));

	FString Result;
	TestTrue(TEXT("Report declares result"), RootObject->TryGetStringField(TEXT("result"), Result));
	TestTrue(
		TEXT("Report result is recognized"),
		Result == TEXT("direct_abi_built") ||
		Result == TEXT("direct_abi_unsupported") ||
		Result == TEXT("missing_toolchain") ||
		Result == TEXT("missing_workload") ||
		Result == TEXT("publish_failed"));

	bool bDirectAbiSupported = false;
	TestTrue(TEXT("Report declares direct ABI support"), RootObject->TryGetBoolField(TEXT("direct_abi_supported"), bDirectAbiSupported));
	if (Result == TEXT("direct_abi_built"))
	{
		TestTrue(TEXT("Built result reports direct ABI support"), bDirectAbiSupported);
	}
	else
	{
		TestFalse(TEXT("Non-built result reports no direct ABI support"), bDirectAbiSupported);
	}

	return true;
}

#endif
