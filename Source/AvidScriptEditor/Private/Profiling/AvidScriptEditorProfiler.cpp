#include "Profiling/AvidScriptEditorProfiler.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
struct FAvidScriptProfilerAggregateKey
{
	EAvidScriptProfilerEventKind Kind = EAvidScriptProfilerEventKind::GuestCall;
	uint32 OperationId = 0;
	uint64 ProbeId = 0;

	bool operator==(const FAvidScriptProfilerAggregateKey& Other) const
	{
		return Kind == Other.Kind
			&& OperationId == Other.OperationId
			&& ProbeId == Other.ProbeId;
	}
};

uint32 GetTypeHash(const FAvidScriptProfilerAggregateKey& Key)
{
	uint32 Hash = HashCombine(
		::GetTypeHash(static_cast<uint8>(Key.Kind)),
		::GetTypeHash(Key.OperationId));
	return HashCombine(Hash, ::GetTypeHash(Key.ProbeId));
}

FString GetProfilerKindLabel(const EAvidScriptProfilerEventKind Kind)
{
	switch (Kind)
	{
	case EAvidScriptProfilerEventKind::RuntimeLoad:
		return TEXT("Runtime Load");
	case EAvidScriptProfilerEventKind::GuestCall:
		return TEXT("Guest Call");
	case EAvidScriptProfilerEventKind::HostCall:
		return TEXT("UE Crossing");
	case EAvidScriptProfilerEventKind::Continuation:
		return TEXT("Continuation");
	case EAvidScriptProfilerEventKind::Reload:
		return TEXT("Reload");
	case EAvidScriptProfilerEventKind::Compile:
		return TEXT("Compile");
	case EAvidScriptProfilerEventKind::Cache:
		return TEXT("Cache");
	default:
		return TEXT("Unknown");
	}
}

bool MatchesSearch(
	const FAvidScriptEditorProfilerEventRow& Row,
	const FString& SearchText)
{
	if (SearchText.IsEmpty())
	{
		return true;
	}
	return Row.KindLabel.Contains(SearchText, ESearchCase::IgnoreCase)
		|| Row.SourceFile.Contains(SearchText, ESearchCase::IgnoreCase)
		|| Row.FunctionName.Contains(SearchText, ESearchCase::IgnoreCase)
		|| FString::Printf(TEXT("%u"), Row.OperationId).Contains(
			SearchText,
			ESearchCase::IgnoreCase);
}

TSharedRef<FJsonObject> MakeEventJson(const FAvidScriptEditorProfilerEventRow& Row)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("sequence"), FString::Printf(TEXT("%llu"), Row.Sequence));
	Json->SetStringField(TEXT("kind"), Row.KindLabel);
	Json->SetNumberField(TEXT("operation_id"), Row.OperationId);
	Json->SetNumberField(TEXT("duration_microseconds"), Row.DurationMicroseconds);
	Json->SetStringField(TEXT("epoch"), FString::Printf(TEXT("%llu"), Row.Epoch));
	Json->SetStringField(TEXT("probe_id"), FString::Printf(TEXT("%llu"), Row.ProbeId));
	Json->SetStringField(TEXT("correlation_id"), FString::Printf(TEXT("%llu"), Row.CorrelationId));
	Json->SetStringField(TEXT("value"), FString::Printf(TEXT("%lld"), Row.Value));
	Json->SetBoolField(TEXT("succeeded"), Row.bSucceeded);
	Json->SetStringField(TEXT("source_file"), Row.SourceFile);
	Json->SetNumberField(TEXT("source_line"), Row.SourceLine);
	Json->SetStringField(TEXT("function"), Row.FunctionName);
	return Json;
}

TSharedRef<FJsonObject> MakeAggregateJson(const FAvidScriptEditorProfilerAggregate& Aggregate)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("kind"), Aggregate.KindLabel);
	Json->SetNumberField(TEXT("operation_id"), Aggregate.OperationId);
	Json->SetStringField(TEXT("probe_id"), FString::Printf(TEXT("%llu"), Aggregate.ProbeId));
	Json->SetNumberField(TEXT("call_count"), Aggregate.CallCount);
	Json->SetNumberField(TEXT("total_microseconds"), Aggregate.TotalMicroseconds);
	Json->SetNumberField(TEXT("average_microseconds"), Aggregate.AverageMicroseconds);
	Json->SetNumberField(TEXT("maximum_microseconds"), Aggregate.MaximumMicroseconds);
	Json->SetStringField(TEXT("source_file"), Aggregate.SourceFile);
	Json->SetNumberField(TEXT("source_line"), Aggregate.SourceLine);
	Json->SetStringField(TEXT("function"), Aggregate.FunctionName);
	return Json;
}
} // namespace

bool FAvidScriptEditorProfilerModel::BindRuntime(
	IAvidScriptEditorProfilerRuntime& InRuntime,
	FString& OutError)
{
	Runtime = &InRuntime;
	View = FAvidScriptEditorProfilerView();
	View.bRuntimeBound = true;
	SourceCatalog.Reset();
	SourceCatalogWarning.Reset();
	return Refresh(OutError);
}

void FAvidScriptEditorProfilerModel::UnbindRuntime()
{
	Runtime = nullptr;
	SourceCatalog.Reset();
	SourceCatalogWarning.Reset();
	View = FAvidScriptEditorProfilerView();
}

void FAvidScriptEditorProfilerModel::NotifyRuntimeDestroyed()
{
	UnbindRuntime();
}

bool FAvidScriptEditorProfilerModel::SetCaptureEnabled(
	const bool bEnabled,
	FString& OutError)
{
	if (Runtime == nullptr)
	{
		return Fail(TEXT("AvidScript profiler has no selected Runtime target."), OutError);
	}
	Runtime->SetProfilerEnabled(bEnabled);
	if (Runtime->IsProfilerEnabled() != bEnabled)
	{
		return Fail(TEXT("AvidScript Runtime rejected the profiler capture state change."), OutError);
	}
	return Refresh(OutError);
}

bool FAvidScriptEditorProfilerModel::ResetCapture(FString& OutError)
{
	if (Runtime == nullptr)
	{
		return Fail(TEXT("AvidScript profiler has no selected Runtime target."), OutError);
	}
	Runtime->ResetProfiler();
	return Refresh(OutError);
}

void FAvidScriptEditorProfilerModel::SetFilter(
	const FAvidScriptEditorProfilerFilter& InFilter)
{
	Filter = InFilter;
	Filter.MinimumDurationMicroseconds = FMath::Max(0.0, Filter.MinimumDurationMicroseconds);
	Filter.MaximumRows = FMath::Clamp(Filter.MaximumRows, 1, 100000);
	if (Runtime != nullptr)
	{
		RebuildView(Runtime->GetProfilerSnapshot());
	}
}

bool FAvidScriptEditorProfilerModel::Refresh(FString& OutError)
{
	if (Runtime == nullptr)
	{
		return Fail(TEXT("AvidScript profiler has no selected Runtime target."), OutError);
	}

	TArray<FAvidScriptDebugBreakpoint> Breakpoints;
	FString CatalogError;
	if (Runtime->GetProfilerSourceCatalog(Breakpoints, CatalogError))
	{
		SourceCatalog.Reset();
		for (const FAvidScriptDebugBreakpoint& Breakpoint : Breakpoints)
		{
			if (Breakpoint.ProbeId != 0 && !SourceCatalog.Contains(Breakpoint.ProbeId))
			{
				SourceCatalog.Add(Breakpoint.ProbeId, Breakpoint);
			}
		}
		SourceCatalogWarning.Reset();
	}
	else
	{
		SourceCatalog.Reset();
		SourceCatalogWarning = CatalogError.IsEmpty()
			? TEXT("Profiler source correlation is unavailable for this Runtime artifact.")
			: CatalogError;
	}

	RebuildView(Runtime->GetProfilerSnapshot());
	Succeed(OutError);
	return true;
}

void FAvidScriptEditorProfilerModel::RebuildView(
	const FAvidScriptProfilerSnapshot& Snapshot)
{
	View.bRuntimeBound = Runtime != nullptr;
	View.bCaptureEnabled = Runtime != nullptr && Runtime->IsProfilerEnabled();
	View.Revision = Snapshot.Revision;
	View.DroppedEventCount = Snapshot.DroppedEventCount;
	View.RejectedThreadEventCount = Snapshot.RejectedThreadEventCount;
	View.SourceEventCount = Snapshot.Events.Num();
	View.FilteredEventCount = 0;
	View.Events.Reset();
	View.Hotspots.Reset();
	View.LastError = SourceCatalogWarning;

	TArray<FAvidScriptEditorProfilerEventRow> FilteredRows;
	FilteredRows.Reserve(Snapshot.Events.Num());
	TMap<FAvidScriptProfilerAggregateKey, int32> AggregateIndices;
	for (const FAvidScriptProfilerEvent& Event : Snapshot.Events)
	{
		FAvidScriptEditorProfilerEventRow Row;
		Row.Sequence = Event.Sequence;
		Row.Epoch = Event.Epoch;
		Row.ProbeId = Event.ProbeId;
		Row.CorrelationId = Event.CorrelationId;
		Row.OperationId = Event.OperationId;
		Row.DurationMicroseconds = static_cast<double>(Event.DurationCycles)
			* Snapshot.SecondsPerCycle
			* 1000000.0;
		Row.Value = Event.Value;
		Row.Kind = Event.Kind;
		Row.bSucceeded = Event.bSucceeded;
		Row.KindLabel = GetProfilerKindLabel(Event.Kind);
		if (const FAvidScriptDebugBreakpoint* Source = SourceCatalog.Find(Event.ProbeId))
		{
			Row.SourceFile = Source->SourceFile;
			Row.FunctionName = Source->FunctionName;
			Row.SourceLine = Source->Line;
		}

		if ((Filter.KindMask & AvidScriptEditorProfilerKindBit(Event.Kind)) == 0
			|| Row.DurationMicroseconds < Filter.MinimumDurationMicroseconds
			|| (Filter.bOnlyFailed && Event.bSucceeded)
			|| !MatchesSearch(Row, Filter.SearchText))
		{
			continue;
		}

		FilteredRows.Add(Row);
		const FAvidScriptProfilerAggregateKey Key{Event.Kind, Event.OperationId, Event.ProbeId};
		int32* ExistingIndex = AggregateIndices.Find(Key);
		if (ExistingIndex == nullptr)
		{
			FAvidScriptEditorProfilerAggregate& Aggregate = View.Hotspots.AddDefaulted_GetRef();
			Aggregate.Kind = Row.Kind;
			Aggregate.OperationId = Row.OperationId;
			Aggregate.ProbeId = Row.ProbeId;
			Aggregate.KindLabel = Row.KindLabel;
			Aggregate.SourceFile = Row.SourceFile;
			Aggregate.FunctionName = Row.FunctionName;
			Aggregate.SourceLine = Row.SourceLine;
			ExistingIndex = &AggregateIndices.Add(Key, View.Hotspots.Num() - 1);
		}
		FAvidScriptEditorProfilerAggregate& Aggregate = View.Hotspots[*ExistingIndex];
		++Aggregate.CallCount;
		Aggregate.TotalMicroseconds += Row.DurationMicroseconds;
		Aggregate.MaximumMicroseconds = FMath::Max(
			Aggregate.MaximumMicroseconds,
			Row.DurationMicroseconds);
	}

	View.FilteredEventCount = FilteredRows.Num();
	const int32 FirstRow = FMath::Max(0, FilteredRows.Num() - Filter.MaximumRows);
	if (FirstRow < FilteredRows.Num())
	{
		View.Events.Append(FilteredRows.GetData() + FirstRow, FilteredRows.Num() - FirstRow);
	}
	for (FAvidScriptEditorProfilerAggregate& Aggregate : View.Hotspots)
	{
		Aggregate.AverageMicroseconds = Aggregate.CallCount > 0
			? Aggregate.TotalMicroseconds / static_cast<double>(Aggregate.CallCount)
			: 0.0;
	}
	View.Hotspots.Sort(
		[](const FAvidScriptEditorProfilerAggregate& Left, const FAvidScriptEditorProfilerAggregate& Right)
		{
			if (Left.TotalMicroseconds != Right.TotalMicroseconds)
			{
				return Left.TotalMicroseconds > Right.TotalMicroseconds;
			}
			if (Left.Kind != Right.Kind)
			{
				return static_cast<uint8>(Left.Kind) < static_cast<uint8>(Right.Kind);
			}
			if (Left.OperationId != Right.OperationId)
			{
				return Left.OperationId < Right.OperationId;
			}
			return Left.ProbeId < Right.ProbeId;
		});
}

bool FAvidScriptEditorProfilerModel::ExportJson(
	const FString& OutputPath,
	FString& OutError) const
{
	if (!View.bRuntimeBound)
	{
		OutError = TEXT("AvidScript profiler export requires a selected Runtime target.");
		return false;
	}
	if (OutputPath.IsEmpty())
	{
		OutError = TEXT("AvidScript profiler export requires a non-empty output path.");
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetBoolField(TEXT("capture_enabled"), View.bCaptureEnabled);
	Root->SetStringField(TEXT("revision"), FString::Printf(TEXT("%llu"), View.Revision));
	Root->SetStringField(TEXT("dropped_event_count"), FString::Printf(TEXT("%llu"), View.DroppedEventCount));
	Root->SetStringField(TEXT("rejected_thread_event_count"), FString::Printf(TEXT("%llu"), View.RejectedThreadEventCount));
	Root->SetNumberField(TEXT("source_event_count"), View.SourceEventCount);
	Root->SetNumberField(TEXT("filtered_event_count"), View.FilteredEventCount);

	TSharedRef<FJsonObject> FilterJson = MakeShared<FJsonObject>();
	FilterJson->SetNumberField(TEXT("kind_mask"), Filter.KindMask);
	FilterJson->SetNumberField(TEXT("minimum_duration_microseconds"), Filter.MinimumDurationMicroseconds);
	FilterJson->SetStringField(TEXT("search_text"), Filter.SearchText);
	FilterJson->SetBoolField(TEXT("only_failed"), Filter.bOnlyFailed);
	FilterJson->SetNumberField(TEXT("maximum_rows"), Filter.MaximumRows);
	Root->SetObjectField(TEXT("filter"), FilterJson);

	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Reserve(View.Events.Num());
	for (const FAvidScriptEditorProfilerEventRow& Row : View.Events)
	{
		EventValues.Add(MakeShared<FJsonValueObject>(MakeEventJson(Row)));
	}
	Root->SetArrayField(TEXT("events"), EventValues);

	TArray<TSharedPtr<FJsonValue>> HotspotValues;
	HotspotValues.Reserve(View.Hotspots.Num());
	for (const FAvidScriptEditorProfilerAggregate& Aggregate : View.Hotspots)
	{
		HotspotValues.Add(MakeShared<FJsonValueObject>(MakeAggregateJson(Aggregate)));
	}
	Root->SetArrayField(TEXT("hotspots"), HotspotValues);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("AvidScript profiler export could not serialize JSON.");
		return false;
	}

	const FString FullOutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
	const FString Directory = FPaths::GetPath(FullOutputPath);
	if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, true))
	{
		OutError = FString::Printf(TEXT("Could not create profiler export directory: %s"), *Directory);
		return false;
	}
	const FString TemporaryPath = FullOutputPath + TEXT(".tmp");
	IFileManager::Get().Delete(*TemporaryPath, false, true, true);
	if (!FFileHelper::SaveStringToFile(
			Json,
			*TemporaryPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write profiler export: %s"), *TemporaryPath);
		return false;
	}
	if (!IFileManager::Get().Move(*FullOutputPath, *TemporaryPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TemporaryPath, false, true, true);
		OutError = FString::Printf(TEXT("Could not publish profiler export: %s"), *FullOutputPath);
		return false;
	}

	OutError.Reset();
	return true;
}

bool FAvidScriptEditorProfilerModel::Fail(
	const FString& Error,
	FString& OutError)
{
	View.LastError = Error;
	OutError = Error;
	return false;
}

void FAvidScriptEditorProfilerModel::Succeed(FString& OutError)
{
	OutError.Reset();
	View.LastError = SourceCatalogWarning;
}
