#include "Profiling/SAvidScriptEditorProfilerPanel.h"

#include "AvidScriptEditorDiagnosticNavigation.h"
#include "Debugging/AvidScriptEditorDebugTargetController.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "AvidScriptEditorProfilerPanel"

namespace
{
TSharedRef<SWidget> MakeAvidScriptProfilerIconButton(
	const FName IconName,
	const FText& ToolTip,
	const FOnClicked& OnClicked,
	const TAttribute<bool>& IsEnabled)
{
	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ContentPadding(FMargin(5.0f))
		.ToolTipText(ToolTip)
		.OnClicked(OnClicked)
		.IsEnabled(IsEnabled)
		[
			SNew(SImage).Image(FAppStyle::GetBrush(IconName))
		];
}

class SAvidScriptEditorProfilerPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAvidScriptEditorProfilerPanel) {}
	SLATE_END_ARGS()

	void Construct(
		const FArguments& InArgs,
		TSharedRef<FAvidScriptEditorDebugTargetController> InController)
	{
		Controller = InController;
		FAvidScriptEditorProfilerFilter InitialFilter;
		InitialFilter.MaximumRows = 250;
		Controller->GetProfilerModel().SetFilter(InitialFilter);

		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				BuildControlRow()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				BuildKindRow()
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Vertical)
				+ SSplitter::Slot().Value(0.38f)[BuildHotspotList()]
				+ SSplitter::Slot().Value(0.62f)[BuildEventList()]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SAvidScriptEditorProfilerPanel::GetStatusText)
				.AutoWrapText(true)
			]
		];

		RefreshPanel();
		RegisterActiveTimer(
			0.2f,
			FWidgetActiveTimerDelegate::CreateSP(
				this,
				&SAvidScriptEditorProfilerPanel::HandleRefreshTimer));
	}

private:
	TSharedRef<SWidget> BuildControlRow()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked(this, &SAvidScriptEditorProfilerPanel::GetCaptureState)
				.OnCheckStateChanged(this, &SAvidScriptEditorProfilerPanel::HandleCaptureChanged)
				.IsEnabled(this, &SAvidScriptEditorProfilerPanel::CanUseProfiler)
				[
					SNew(STextBlock).Text(LOCTEXT("Capture", "Capture"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f)
			[
				MakeAvidScriptProfilerIconButton(
					TEXT("Icons.Delete"),
					LOCTEXT("ResetToolTip", "Reset captured AvidScript profiler events."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorProfilerPanel::HandleReset),
					TAttribute<bool>::CreateSP(this, &SAvidScriptEditorProfilerPanel::CanUseProfiler))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				MakeAvidScriptProfilerIconButton(
					TEXT("Icons.Save"),
					LOCTEXT("ExportToolTip", "Export the filtered AvidScript profiler snapshot as JSON."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorProfilerPanel::HandleExport),
					TAttribute<bool>::CreateSP(this, &SAvidScriptEditorProfilerPanel::CanUseProfiler))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8.0f, 0.0f)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("SearchHint", "Filter source, function, kind or operation"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					SearchText = Text.ToString();
					ApplyFilter();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(130.0f)
				[
					SNew(SNumericEntryBox<double>)
					.MinValue(0.0)
					.MinSliderValue(0.0)
					.MaxSliderValue(1000.0)
					.AllowSpin(true)
					.Value_Lambda([this]() { return TOptional<double>(MinimumDurationUs); })
					.OnValueChanged_Lambda([this](const double Value)
					{
						MinimumDurationUs = FMath::Max(0.0, Value);
						ApplyFilter();
					})
					.ToolTipText(LOCTEXT("DurationToolTip", "Minimum event duration in microseconds."))
				]
			];
	}

	TSharedRef<SWidget> BuildKindRow()
	{
		const uint32 RuntimeKinds =
			AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::RuntimeLoad)
			| AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::Reload);
		const uint32 BuildKinds =
			AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::Compile)
			| AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::Cache);
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[MakeKindToggle(
				LOCTEXT("KindGuest", "Guest"),
				AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::GuestCall))]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f)[MakeKindToggle(
				LOCTEXT("KindHost", "UE"),
				AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::HostCall))]
			+ SHorizontalBox::Slot().AutoWidth()[MakeKindToggle(
				LOCTEXT("KindAsync", "Async"),
				AvidScriptEditorProfilerKindBit(EAvidScriptProfilerEventKind::Continuation))]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f)[MakeKindToggle(
				LOCTEXT("KindRuntime", "Runtime"), RuntimeKinds)]
			+ SHorizontalBox::Slot().AutoWidth()[MakeKindToggle(
				LOCTEXT("KindBuild", "Build"), BuildKinds)]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]()
				{
					return bOnlyFailed ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
				{
					bOnlyFailed = State == ECheckBoxState::Checked;
					ApplyFilter();
				})
				[
					SNew(STextBlock).Text(LOCTEXT("OnlyFailed", "Failures"))
				]
			];
	}

	TSharedRef<SWidget> MakeKindToggle(const FText& Label, const uint32 Mask)
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda([this, Mask]()
			{
				return (KindMask & Mask) == Mask
					? ECheckBoxState::Checked
					: ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, Mask](const ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					KindMask |= Mask;
				}
				else
				{
					KindMask &= ~Mask;
				}
				ApplyFilter();
			})
			[
				SNew(STextBlock).Text(Label)
			];
	}

	TSharedRef<SWidget> BuildHotspotList()
	{
		return BuildListShell(
			LOCTEXT("HotspotsHeading", "Hotspots"),
			SAssignNew(HotspotRows, SVerticalBox));
	}

	TSharedRef<SWidget> BuildEventList()
	{
		return BuildListShell(
			LOCTEXT("EventsHeading", "Events"),
			SAssignNew(EventRows, SVerticalBox));
	}

	TSharedRef<SWidget> BuildListShell(
		const FText& Heading,
		const TSharedRef<SVerticalBox>& Rows)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(Heading)
				.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f)[SNew(SSeparator)]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()[Rows]
			];
	}

	EActiveTimerReturnType HandleRefreshTimer(double, float)
	{
		RefreshPanel();
		return EActiveTimerReturnType::Continue;
	}

	void RefreshPanel()
	{
		const FAvidScriptEditorProfilerView& View = Controller->GetProfilerModel().GetView();
		const FString Signature = FString::Printf(
			TEXT("%d|%llu|%d|%d|%llu|%u|%.6f|%d|%s"),
			View.bRuntimeBound ? 1 : 0,
			View.Revision,
			View.FilteredEventCount,
			View.Hotspots.Num(),
			View.DroppedEventCount,
			KindMask,
			MinimumDurationUs,
			bOnlyFailed ? 1 : 0,
			*SearchText);
		if (Signature != RenderSignature)
		{
			RenderSignature = Signature;
			RebuildRows(View);
		}
		if (!LastCommandError.IsEmpty())
		{
			StatusText = FText::FromString(LastCommandError);
		}
		else if (!View.LastError.IsEmpty())
		{
			StatusText = FText::FromString(View.LastError);
		}
		else if (!LastCommandNotice.IsEmpty())
		{
			StatusText = FText::FromString(LastCommandNotice);
		}
		else if (View.bRuntimeBound)
		{
			StatusText = FText::Format(
				LOCTEXT("StatusFormat", "{0} | {1}/{2} events | {3} hotspot(s) | {4} dropped"),
				View.bCaptureEnabled ? LOCTEXT("Capturing", "Capturing") : LOCTEXT("Idle", "Idle"),
				FText::AsNumber(View.FilteredEventCount),
				FText::AsNumber(View.SourceEventCount),
				FText::AsNumber(View.Hotspots.Num()),
				FText::AsNumber(View.DroppedEventCount));
		}
		else
		{
			StatusText = LOCTEXT("NoTarget", "No live PIE AvidScript profiler target.");
		}
	}

	void RebuildRows(const FAvidScriptEditorProfilerView& View)
	{
		HotspotRows->ClearChildren();
		EventRows->ClearChildren();
		if (View.Hotspots.IsEmpty())
		{
			AddEmptyRow(*HotspotRows, LOCTEXT("NoHotspots", "No matching profiler hotspots."));
		}
		for (int32 Index = 0; Index < FMath::Min(View.Hotspots.Num(), 50); ++Index)
		{
			AddHotspotRow(View.Hotspots[Index]);
		}
		if (View.Events.IsEmpty())
		{
			AddEmptyRow(*EventRows, LOCTEXT("NoEvents", "No matching profiler events."));
		}
		for (int32 Index = View.Events.Num() - 1; Index >= 0; --Index)
		{
			AddEventRow(View.Events[Index]);
		}
	}

	void AddEmptyRow(SVerticalBox& Rows, const FText& Text)
	{
		Rows.AddSlot().AutoHeight().Padding(2.0f)
		[
			SNew(STextBlock)
			.Text(Text)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}

	void AddHotspotRow(const FAvidScriptEditorProfilerAggregate& Hotspot)
	{
		const FString SourceFile = Hotspot.SourceFile;
		const int32 SourceLine = Hotspot.SourceLine;
		HotspotRows->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.25f)[SNew(STextBlock).Text(FText::FromString(
				FString::Printf(TEXT("%s #%u"), *Hotspot.KindLabel, Hotspot.OperationId)))]
			+ SHorizontalBox::Slot().FillWidth(0.12f)[SNew(STextBlock).Text(FText::AsNumber(Hotspot.CallCount))]
			+ SHorizontalBox::Slot().FillWidth(0.18f)[SNew(STextBlock).Text(FText::FromString(
				FString::Printf(TEXT("%.3f us"), Hotspot.TotalMicroseconds)))]
			+ SHorizontalBox::Slot().FillWidth(0.17f)[SNew(STextBlock).Text(FText::FromString(
				FString::Printf(TEXT("avg %.3f"), Hotspot.AverageMicroseconds)))]
			+ SHorizontalBox::Slot().FillWidth(0.28f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Hotspot.FunctionName))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot().AutoWidth()[MakeSourceButton(SourceFile, SourceLine)]
		];
	}

	void AddEventRow(const FAvidScriptEditorProfilerEventRow& Event)
	{
		const FString SourceFile = Event.SourceFile;
		const int32 SourceLine = Event.SourceLine;
		EventRows->AddSlot().AutoHeight().Padding(0.0f, 1.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.25f)[SNew(STextBlock).Text(FText::FromString(
				FString::Printf(TEXT("%s #%u"), *Event.KindLabel, Event.OperationId)))]
			+ SHorizontalBox::Slot().FillWidth(0.16f)[SNew(STextBlock).Text(FText::FromString(
				FString::Printf(TEXT("%.3f us"), Event.DurationMicroseconds)))]
			+ SHorizontalBox::Slot().FillWidth(0.12f)[SNew(STextBlock).Text(Event.bSucceeded
				? LOCTEXT("EventOk", "OK") : LOCTEXT("EventFailed", "Failed"))]
			+ SHorizontalBox::Slot().FillWidth(0.47f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Event.FunctionName.IsEmpty() ? Event.SourceFile : Event.FunctionName))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot().AutoWidth()[MakeSourceButton(SourceFile, SourceLine)]
		];
	}

	TSharedRef<SWidget> MakeSourceButton(const FString& SourceFile, const int32 SourceLine)
	{
		return MakeAvidScriptProfilerIconButton(
			TEXT("Icons.Browse"),
			LOCTEXT("OpenSourceToolTip", "Open this profiler location in C# source."),
			FOnClicked::CreateLambda([this, SourceFile, SourceLine]()
			{
				FAvidScriptEditorSourceLocation Location;
				Location.File = SourceFile;
				Location.Line = SourceLine;
				Location.Column = 1;
				FAvidScriptEditorDiagnosticNavigationResult Result;
				const bool bSucceeded = FAvidScriptEditorDiagnosticNavigation::Open(
					Location,
					FPaths::ProjectDir(),
					Result);
				SetCommandResult(bSucceeded, Result.ErrorMessage);
				RefreshPanel();
				return FReply::Handled();
			}),
			TAttribute<bool>(!SourceFile.IsEmpty() && SourceLine > 0));
	}

	void ApplyFilter()
	{
		FAvidScriptEditorProfilerFilter Filter;
		Filter.KindMask = KindMask;
		Filter.MinimumDurationMicroseconds = MinimumDurationUs;
		Filter.SearchText = SearchText;
		Filter.bOnlyFailed = bOnlyFailed;
		Filter.MaximumRows = 250;
		Controller->GetProfilerModel().SetFilter(Filter);
		RenderSignature.Reset();
		RefreshPanel();
	}

	void HandleCaptureChanged(const ECheckBoxState State)
	{
		FString Error;
		SetCommandResult(
			Controller->SetProfilerCaptureEnabled(State == ECheckBoxState::Checked, Error),
			Error);
		RenderSignature.Reset();
		RefreshPanel();
	}

	FReply HandleReset()
	{
		FString Error;
		SetCommandResult(Controller->ResetProfiler(Error), Error);
		RenderSignature.Reset();
		RefreshPanel();
		return FReply::Handled();
	}

	FReply HandleExport()
	{
		const FString OutputPath = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScript/Profiler/AvidScriptProfiler.json"));
		FString Error;
		const bool bSucceeded = Controller->ExportProfilerJson(OutputPath, Error);
		SetCommandResult(bSucceeded, Error);
		if (bSucceeded)
		{
			LastCommandNotice = FString::Printf(TEXT("Profiler exported: %s"), *OutputPath);
		}
		RefreshPanel();
		return FReply::Handled();
	}

	void SetCommandResult(const bool bSucceeded, const FString& Error)
	{
		LastCommandError = bSucceeded ? FString() : Error;
		LastCommandNotice.Reset();
	}

	bool CanUseProfiler() const
	{
		return Controller->GetProfilerModel().GetView().bRuntimeBound;
	}

	ECheckBoxState GetCaptureState() const
	{
		return Controller->GetProfilerModel().GetView().bCaptureEnabled
			? ECheckBoxState::Checked
			: ECheckBoxState::Unchecked;
	}

	FText GetStatusText() const { return StatusText; }

	TSharedPtr<FAvidScriptEditorDebugTargetController> Controller;
	TSharedPtr<SVerticalBox> HotspotRows;
	TSharedPtr<SVerticalBox> EventRows;
	FString SearchText;
	FString RenderSignature;
	FString LastCommandError;
	FString LastCommandNotice;
	FText StatusText;
	double MinimumDurationUs = 0.0;
	uint32 KindMask = MAX_uint32;
	bool bOnlyFailed = false;
};
} // namespace

TSharedRef<SWidget> MakeAvidScriptEditorProfilerPanel(
	TSharedRef<FAvidScriptEditorDebugTargetController> Controller)
{
	return SNew(SAvidScriptEditorProfilerPanel, Controller);
}

#undef LOCTEXT_NAMESPACE
