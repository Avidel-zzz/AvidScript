#if WITH_DEV_AUTOMATION_TESTS

#include "Profiling/AvidScriptEditorProfiler.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
class FFakeAvidScriptEditorProfilerRuntime final
	: public IAvidScriptEditorProfilerRuntime
{
public:
	virtual void SetProfilerEnabled(const bool bEnabled) override
	{
		bProfilerEnabled = bEnabled;
	}

	virtual bool IsProfilerEnabled() const override
	{
		return bProfilerEnabled;
	}

	virtual void ResetProfiler() override
	{
		Snapshot.Events.Reset();
		++Snapshot.Revision;
	}

	virtual FAvidScriptProfilerSnapshot GetProfilerSnapshot() const override
	{
		return Snapshot;
	}

	virtual bool GetProfilerSourceCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const override
	{
		if (!bCatalogAvailable)
		{
			OutBreakpoints.Reset();
			OutError = TEXT("source catalog unavailable");
			return false;
		}
		OutBreakpoints = Catalog;
		OutError.Reset();
		return true;
	}

	bool bProfilerEnabled = false;
	bool bCatalogAvailable = true;
	FAvidScriptProfilerSnapshot Snapshot;
	TArray<FAvidScriptDebugBreakpoint> Catalog;
};

FAvidScriptProfilerEvent MakeProfilerEvent(
	const uint64 Sequence,
	const EAvidScriptProfilerEventKind Kind,
	const uint32 OperationId,
	const uint64 ProbeId,
	const uint64 DurationCycles,
	const bool bSucceeded)
{
	FAvidScriptProfilerEvent Event;
	Event.Sequence = Sequence;
	Event.Kind = Kind;
	Event.OperationId = OperationId;
	Event.ProbeId = ProbeId;
	Event.DurationCycles = DurationCycles;
	Event.bSucceeded = bSucceeded;
	return Event;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorProfilerModelTest,
	"AvidScript.Editor.Profiling.Model",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorProfilerModelTest::RunTest(const FString& Parameters)
{
	FFakeAvidScriptEditorProfilerRuntime Runtime;
	Runtime.Snapshot.SecondsPerCycle = 1.0e-6;
	Runtime.Snapshot.Revision = 7;
	Runtime.Snapshot.DroppedEventCount = 3;
	Runtime.Snapshot.Events = {
		MakeProfilerEvent(1, EAvidScriptProfilerEventKind::GuestCall, 10, 101, 3, true),
		MakeProfilerEvent(2, EAvidScriptProfilerEventKind::HostCall, 20, 101, 5, false),
		MakeProfilerEvent(3, EAvidScriptProfilerEventKind::HostCall, 21, 0, 1, true),
		MakeProfilerEvent(4, EAvidScriptProfilerEventKind::Continuation, 14, 0, 10, true)};
	FAvidScriptDebugBreakpoint& Source = Runtime.Catalog.AddDefaulted_GetRef();
	Source.ProbeId = 101;
	Source.SourceFile = TEXT("C:/Game/Scripts/Player.cs");
	Source.FunctionName = TEXT("Player.Tick");
	Source.Line = 42;

	FAvidScriptEditorProfilerModel Model;
	FString Error;
	TestTrue(TEXT("Profiler model binds a Runtime"), Model.BindRuntime(Runtime, Error));
	TestTrue(TEXT("Bound profiler view is marked live"), Model.GetView().bRuntimeBound);
	TestEqual(TEXT("All source events are visible"), Model.GetView().SourceEventCount, 4);
	TestEqual(TEXT("Profiler revision is preserved"), Model.GetView().Revision, uint64(7));
	TestTrue(TEXT("Profiler capture can be enabled"), Model.SetCaptureEnabled(true, Error));
	TestTrue(TEXT("Profiler capture state is reflected"), Model.GetView().bCaptureEnabled);

	FAvidScriptEditorProfilerFilter Filter;
	Filter.KindMask = AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::HostCall);
	Filter.MinimumDurationMicroseconds = 2.0;
	Filter.SearchText = TEXT("Player");
	Filter.bOnlyFailed = true;
	Filter.MaximumRows = 10;
	Model.SetFilter(Filter);
	const FAvidScriptEditorProfilerView& FilteredView = Model.GetView();
	TestEqual(TEXT("Combined filters retain one event"), FilteredView.FilteredEventCount, 1);
	TestEqual(TEXT("Filtered row list contains one event"), FilteredView.Events.Num(), 1);
	TestEqual(TEXT("Filtered event correlates source line"), FilteredView.Events[0].SourceLine, 42);
	TestEqual(TEXT("Filtered event correlates source function"), FilteredView.Events[0].FunctionName, FString(TEXT("Player.Tick")));
	TestEqual(TEXT("Filtered event duration uses seconds-per-cycle"), FilteredView.Events[0].DurationMicroseconds, 5.0);
	TestEqual(TEXT("Filtered hotspot count is one"), FilteredView.Hotspots.Num(), 1);
	TestEqual(TEXT("Hotspot aggregates one call"), FilteredView.Hotspots[0].CallCount, 1);
	TestEqual(TEXT("Hotspot total duration is preserved"), FilteredView.Hotspots[0].TotalMicroseconds, 5.0);

	const FString ExportPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation/AvidScript/Profiler/model-export.json"));
	TestTrue(TEXT("Filtered profiler view exports atomically"), Model.ExportJson(ExportPath, Error));
	FString ExportJson;
	TestTrue(TEXT("Profiler export can be read"), FFileHelper::LoadFileToString(ExportJson, *ExportPath));
	TSharedPtr<FJsonObject> ExportObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExportJson);
	TestTrue(TEXT("Profiler export is valid JSON"), FJsonSerializer::Deserialize(Reader, ExportObject));
	TestTrue(TEXT("Profiler export has a root object"), ExportObject.IsValid());
	if (ExportObject.IsValid())
	{
		TestEqual(TEXT("Profiler export schema is versioned"), ExportObject->GetIntegerField(TEXT("schema_version")), 1);
		TestEqual(TEXT("Profiler export contains one filtered event"), ExportObject->GetArrayField(TEXT("events")).Num(), 1);
		TestEqual(TEXT("Profiler export contains one hotspot"), ExportObject->GetArrayField(TEXT("hotspots")).Num(), 1);
	}

	Runtime.bCatalogAvailable = false;
	Filter = FAvidScriptEditorProfilerFilter();
	Model.SetFilter(Filter);
	TestTrue(TEXT("Profiler refresh survives a missing source catalog"), Model.Refresh(Error));
	TestEqual(TEXT("Profiler data remains available without source mapping"), Model.GetView().Events.Num(), 4);
	TestEqual(TEXT("Stale source mapping is discarded"), Model.GetView().Events[0].SourceLine, 0);
	TestEqual(
		TEXT("Source mapping warning is visible"),
		Model.GetView().LastError,
		FString(TEXT("source catalog unavailable")));

	TestTrue(TEXT("Profiler capture can be reset"), Model.ResetCapture(Error));
	TestEqual(TEXT("Reset clears the Runtime snapshot"), Model.GetView().SourceEventCount, 0);
	Model.NotifyRuntimeDestroyed();
	TestFalse(TEXT("Runtime destruction invalidates the profiler view"), Model.GetView().bRuntimeBound);
	IFileManager::Get().Delete(*ExportPath, false, true, true);
	return true;
}

#endif
