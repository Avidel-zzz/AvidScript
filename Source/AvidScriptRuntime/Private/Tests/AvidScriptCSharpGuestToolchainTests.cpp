#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptWasmReload.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

FString GetCSharpSourceAdapterPath()
{
	return FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript"),
		TEXT("Build"),
		TEXT("BuildCSharpActorLifecycle.ps1"));
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

	FString AdapterText;
	const FString AdapterPath = GetCSharpSourceAdapterPath();
	if (!FFileHelper::LoadFileToString(AdapterText, *AdapterPath))
	{
		AddError(FString::Printf(TEXT("Failed to load C# source adapter: %s"), *AdapterPath));
		return true;
	}
	TestTrue(
		TEXT("Source adapter accepts an explicitly empty EndPlay body"),
		AdapterText.Contains(TEXT("[AllowEmptyString()]")));
	TestTrue(TEXT("Sample exports BeginPlay"), SourceText.Contains(TEXT("avid_on_begin_play")));
	TestTrue(TEXT("Sample exports Tick"), SourceText.Contains(TEXT("avid_on_tick")));
	TestTrue(TEXT("Sample exports EndPlay"), SourceText.Contains(TEXT("avid_on_end_play")));
	TestTrue(TEXT("Sample exports Timer callback"), SourceText.Contains(TEXT("avid_on_timer")));
	TestTrue(TEXT("Sample declares EndPlay method"), SourceText.Contains(TEXT("public static void EndPlay")));
	TestTrue(TEXT("Sample declares OnTimer method"), SourceText.Contains(TEXT("public static void OnTimer(int callbackId, int timerHandle)")));
	TestTrue(TEXT("Sample uses UnmanagedCallersOnly"), SourceText.Contains(TEXT("UnmanagedCallersOnly")));
	TestTrue(TEXT("Sample imports env actor_set_location"), SourceText.Contains(TEXT("DllImport(\"env\"")) && SourceText.Contains(TEXT("actor_set_location")));
	TestTrue(TEXT("Sample imports env actor_add_location_offset"), SourceText.Contains(TEXT("DllImport(\"env\"")) && SourceText.Contains(TEXT("actor_add_location_offset")));
	TestTrue(TEXT("Sample imports env owner_get_slot"), SourceText.Contains(TEXT("owner_get_slot")));
	TestTrue(TEXT("Sample imports env owner_get_generation"), SourceText.Contains(TEXT("owner_get_generation")));
	TestTrue(TEXT("Sample imports env timer_set_once"), SourceText.Contains(TEXT("timer_set_once")));
	TestTrue(TEXT("Sample imports env timer_cancel"), SourceText.Contains(TEXT("timer_cancel")));
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
	TestTrue(TEXT("Sample uses root component chain"), SourceText.Contains(TEXT("UE.Self.GetRootComponent().GetWorldLocation()")) && SourceText.Contains(TEXT("UE.Self.GetRootComponent().SetWorldLocation(rootLocation)")));
	TestTrue(TEXT("Sample declares UE.Self"), SourceText.Contains(TEXT("public static class UE")) && SourceText.Contains(TEXT("public static AActor Self")));
	TestTrue(TEXT("Sample presents UE.SetTimer"), SourceText.Contains(TEXT("public static int SetTimer(float delaySeconds, int callbackId)")));
	TestTrue(TEXT("Sample presents UE.CancelTimer"), SourceText.Contains(TEXT("public static bool CancelTimer(int timerHandle)")));
	TestTrue(TEXT("Sample schedules Timer in BeginPlay"), SourceText.Contains(TEXT("UE.SetTimer(0.05f, 7)")));
	TestTrue(TEXT("Sample timer callback changes Actor"), SourceText.Contains(TEXT("UE.Self.AddActorWorldOffset(new FVector(0.0f, 0.0f, 50.0f))")));
	TestTrue(TEXT("Sample uses typed SetActorLocation"), SourceText.Contains(TEXT("UE.Self.SetActorLocation(new FVector")));
	TestTrue(TEXT("Sample uses typed AddActorWorldOffset"), SourceText.Contains(TEXT("UE.Self.AddActorWorldOffset(new FVector")));
	TestTrue(TEXT("Sample uses FVector.Zero"), SourceText.Contains(TEXT("UE.Self.SetActorLocation(FVector.Zero)")));
	TestTrue(TEXT("Sample presents Actor.SetLocation facade"), SourceText.Contains(TEXT("public static class Actor")) && SourceText.Contains(TEXT("public static bool SetLocation")));
	TestTrue(TEXT("Sample presents Actor.AddLocationOffset facade"), SourceText.Contains(TEXT("public static bool AddLocationOffset")));
	TestTrue(TEXT("Sample declares elapsed seconds state"), SourceText.Contains(TEXT("private static float ElapsedSeconds")));
	TestTrue(TEXT("Sample resets elapsed seconds in BeginPlay"), SourceText.Contains(TEXT("ElapsedSeconds = 0.0f")));
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
	FString WasmArtifactPath;
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("manifest_file"), ManifestArtifactPath);
	(*ArtifactsObjectPtr)->TryGetStringField(TEXT("wasm_file"), WasmArtifactPath);

	const FString ManifestPath = ResolveCSharpReportArtifactPath(ManifestArtifactPath);
	const FString WasmPath = ResolveCSharpReportArtifactPath(WasmArtifactPath);
	if (!TestTrue(TEXT("C# source adapter writes a manifest artifact"), !ManifestPath.IsEmpty() && FPaths::FileExists(ManifestPath)) ||
		!TestTrue(TEXT("C# source adapter writes a wasm artifact"), !WasmPath.IsEmpty() && FPaths::FileExists(WasmPath)))
	{
		return true;
	}

	FString ManifestJson;
	if (!FFileHelper::LoadFileToString(ManifestJson, *ManifestPath))
	{
		AddError(FString::Printf(TEXT("Failed to read C# source adapter manifest JSON: %s"), *ManifestPath));
		return true;
	}
	TestTrue(TEXT("C# source adapter manifest declares actor lifecycle v10 subset"), ManifestJson.Contains(TEXT("actor_lifecycle_v10")));
	TestTrue(TEXT("C# source adapter manifest declares USceneComponent"), ManifestJson.Contains(TEXT("USceneComponent")));
	TestTrue(TEXT("C# source adapter manifest declares GetRootComponent"), ManifestJson.Contains(TEXT("GetRootComponent")));
	TestTrue(TEXT("C# source adapter manifest declares FVector"), ManifestJson.Contains(TEXT("FVector")));
	TestTrue(TEXT("C# source adapter manifest declares AActor"), ManifestJson.Contains(TEXT("AActor")));
	TestTrue(TEXT("C# source adapter manifest declares UE.Self"), ManifestJson.Contains(TEXT("UE.Self")));
	TestTrue(TEXT("C# source adapter manifest declares GetActorLocation"), ManifestJson.Contains(TEXT("GetActorLocation")));
	TestTrue(TEXT("C# source adapter manifest declares FRotator"), ManifestJson.Contains(TEXT("FRotator")));
	TestTrue(TEXT("C# source adapter manifest declares GetActorRotation"), ManifestJson.Contains(TEXT("GetActorRotation")));
	TestTrue(TEXT("C# source adapter manifest declares SetActorRotation"), ManifestJson.Contains(TEXT("SetActorRotation")));
	TestTrue(TEXT("C# source adapter manifest declares GetActorScale3D"), ManifestJson.Contains(TEXT("GetActorScale3D")));
	TestTrue(TEXT("C# source adapter manifest declares SetActorScale3D"), ManifestJson.Contains(TEXT("SetActorScale3D")));
	TestTrue(TEXT("C# source adapter manifest declares FTransform"), ManifestJson.Contains(TEXT("FTransform")));
	TestTrue(TEXT("C# source adapter manifest requires EndPlay export"), ManifestJson.Contains(TEXT("avid_on_end_play")));
	TestTrue(TEXT("C# source adapter manifest requires Timer export"), ManifestJson.Contains(TEXT("avid_on_timer")));
	TestTrue(TEXT("C# source adapter manifest declares UE.SetTimer"), ManifestJson.Contains(TEXT("UE.SetTimer(float delaySeconds, int callbackId)")));
	TestTrue(TEXT("C# source adapter manifest declares static float state support"), ManifestJson.Contains(TEXT("private static float")));
	TestTrue(TEXT("C# source adapter manifest declares field accumulation support"), ManifestJson.Contains(TEXT("Field += expression")));

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(ManifestPath, Manifest, Bytecode, ManifestLoadResult))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return true;
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
	TestTrue(TEXT("C# source adapter manifest requires Timer cancel import"), bRequiresTimerCancel);

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

	FAvidScriptWasmReloadManifest RejectedManifest = Manifest;
	RejectedManifest.ModuleId = TEXT("csharp_timer_rejected_reload");
	RejectedManifest.RequiredExports.Add(TEXT("avid_missing_reload_export"));
	TestFalse(TEXT("Reload missing an export is rejected"), Session.ReloadModule(Bytecode.GetData(), Bytecode.Num(), RejectedManifest, ReloadResult));
	TestTrue(TEXT("Rejected reload preserves live runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Rejected reload preserves old Timer"), Session.GetLivePendingTimerCount(), 1);
	TestTrue(TEXT("C# BeginPlay source moves actor"), Actor->GetActorLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("C# BeginPlay source resets actor rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# BeginPlay source resets actor scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));

	FAvidScriptWasmSmokeResult TickResult;
	if (!Session.TickLive(1.0f / 60.0f, TickResult))
	{
		AddError(TickResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestTrue(TEXT("C# first Tick source applies stateful elapsed expression"), Actor->GetActorLocation().Equals(FVector(102.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("C# first Tick source rotates actor"), Actor->GetActorRotation().Equals(FRotator(0.0, 1.5, 0.0), 0.01));
	TestTrue(TEXT("C# first Tick source scales actor"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.01), 0.01));

	FAvidScriptWasmSmokeResult SecondTickResult;
	if (!Session.TickLive(1.0f / 60.0f, SecondTickResult))
	{
		AddError(SecondTickResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestTrue(TEXT("C# second Tick source preserves elapsed state"), Actor->GetActorLocation().Equals(FVector(104.0, 200.0, 300.0), 0.01));
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
	TestTrue(TEXT("C# third Tick source preserves movement and fires Timer callback"), Actor->GetActorLocation().Equals(FVector(106.0, 200.0, 350.0), 0.01));
	TestEqual(TEXT("C# Timer callback count"), ThirdTickResult.TimerCallbackCount, 1);
	TestEqual(TEXT("C# Timer callback id"), ThirdTickResult.LastTimerCallbackId, 7);
	TestTrue(TEXT("C# Timer callback reports a handle"), ThirdTickResult.LastTimerHandle > 0);
	TestEqual(TEXT("Fired one-shot Timer leaves no pending Timer"), Session.GetLivePendingTimerCount(), 0);

	FAvidScriptWasmReloadManifest ReloadedManifest = Manifest;
	ReloadedManifest.ModuleId = TEXT("csharp_timer_successful_reload");
	TestTrue(TEXT("Compatible C# Timer reload applies"), Session.ReloadModule(Bytecode.GetData(), Bytecode.Num(), ReloadedManifest, ReloadResult));
	TestEqual(TEXT("Reloaded runtime owns one fresh Timer"), Session.GetLivePendingTimerCount(), 1);
	TestEqual(TEXT("Reloaded runtime callback count starts fresh"), Session.GetLiveTimerCallbackCount(), 0);
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
	TestTrue(TEXT("Reloaded Timer callback has one movement effect"), Actor->GetActorLocation().Equals(FVector(106.0, 200.0, 350.0), 0.01));

	FAvidScriptWasmSmokeResult EndPlayResult;
	if (!Session.EndPlayLive(EndPlayResult))
	{
		AddError(EndPlayResult.ErrorMessage);
		DestroyCSharpContractWorld(World);
		return true;
	}

	TestEqual(TEXT("EndPlay result preserves Timer callback count"), EndPlayResult.TimerCallbackCount, 1);
	TestTrue(TEXT("C# EndPlay source moves actor"), Actor->GetActorLocation().Equals(FVector::ZeroVector, 0.01));
	TestTrue(TEXT("C# EndPlay source resets actor rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# EndPlay source resets actor scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));
	TestTrue(TEXT("C# EndPlay source calls export"), EndPlayResult.bEndPlayCalled);

	DestroyCSharpContractWorld(World);
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
