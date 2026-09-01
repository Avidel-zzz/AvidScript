#include "Debugging/SAvidScriptEditorDebugPanel.h"

#include "AvidScriptEditorDiagnosticNavigation.h"
#include "Debugging/AvidScriptEditorDebugTargetController.h"
#include "Profiling/SAvidScriptEditorProfilerPanel.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "AvidScriptEditorDebugPanel"

namespace
{
struct FAvidScriptDebugTargetOption
{
	FString TargetId;
	FString Label;
};

FText GetAvidScriptDebugStateText(const EAvidScriptDebugSessionState State)
{
	switch (State)
	{
	case EAvidScriptDebugSessionState::Detached:
		return LOCTEXT("DebugStateDetached", "Detached");
	case EAvidScriptDebugSessionState::Running:
		return LOCTEXT("DebugStateRunning", "Running");
	case EAvidScriptDebugSessionState::Suspending:
		return LOCTEXT("DebugStateSuspending", "Pause requested");
	case EAvidScriptDebugSessionState::Paused:
		return LOCTEXT("DebugStatePaused", "Paused");
	case EAvidScriptDebugSessionState::Resuming:
		return LOCTEXT("DebugStateResuming", "Resuming");
	default:
		return LOCTEXT("DebugStateUnknown", "Unknown");
	}
}

TSharedRef<SWidget> MakeAvidScriptDebugIconButton(
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
			SNew(SImage)
			.Image(FAppStyle::GetBrush(IconName))
		];
}

class SAvidScriptEditorDebugPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAvidScriptEditorDebugPanel) {}
	SLATE_END_ARGS()

	void Construct(
		const FArguments& InArgs,
		TSharedRef<FAvidScriptEditorDebugTargetController> InController)
	{
		Controller = InController;
		StatusText = LOCTEXT("WaitingForPIE", "Start PIE to discover an AvidScript target.");
		ChildSlot
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 7.0f, 8.0f, 4.0f)
			[
				BuildToolbar()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 2.0f, 8.0f, 3.0f)
			[
				BuildPageSelector()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(8.0f, 3.0f)
			[
				SAssignNew(WorkspaceSwitcher, SWidgetSwitcher)
				.WidgetIndex(CurrentWorkspace)
				+ SWidgetSwitcher::Slot()
				[
					BuildDebuggerWorkspace()
				]
				+ SWidgetSwitcher::Slot()
				[
					MakeAvidScriptEditorProfilerPanel(Controller.ToSharedRef())
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(8.0f, 3.0f, 8.0f, 7.0f)
			[
				SNew(STextBlock)
				.Text(this, &SAvidScriptEditorDebugPanel::GetStatusText)
				.AutoWrapText(true)
				.Visibility_Lambda([this]()
				{
					return CurrentWorkspace == 0
						? EVisibility::Visible
						: EVisibility::Collapsed;
				})
			]
		];

		RefreshPanel();
		RegisterActiveTimer(
			0.2f,
			FWidgetActiveTimerDelegate::CreateSP(
				this,
				&SAvidScriptEditorDebugPanel::HandlePanelRefresh));
	}

private:
	TSharedRef<SWidget> BuildToolbar()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.MaxWidth(420.0f)
			.VAlign(VAlign_Center)
			[
				SAssignNew(TargetCombo, SComboBox<TSharedPtr<FAvidScriptDebugTargetOption>>)
				.OptionsSource(&TargetOptions)
				.OnGenerateWidget(this, &SAvidScriptEditorDebugPanel::GenerateTargetWidget)
				.OnSelectionChanged(this, &SAvidScriptEditorDebugPanel::HandleTargetSelectionChanged)
				[
					SNew(STextBlock)
					.Text(this, &SAvidScriptEditorDebugPanel::GetSelectedTargetText)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeAvidScriptDebugIconButton(
					TEXT("Icons.Link"),
					LOCTEXT("AttachDebuggerToolTip", "Attach the AvidScript debugger."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorDebugPanel::HandleAttach),
					TAttribute<bool>::CreateSP(this, &SAvidScriptEditorDebugPanel::CanAttach))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeAvidScriptDebugIconButton(
					TEXT("Icons.Unlink"),
					LOCTEXT("DetachDebuggerToolTip", "Detach the AvidScript debugger."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorDebugPanel::HandleDetach),
					TAttribute<bool>::CreateSP(this, &SAvidScriptEditorDebugPanel::CanDetach))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeAvidScriptDebugIconButton(
					TEXT("BTEditor.Debugger.PausePlaySession.Small"),
					LOCTEXT("PauseDebuggerToolTip", "Pause at the next instrumented C# sequence point."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorDebugPanel::HandlePause),
					TAttribute<bool>::CreateSP(this, &SAvidScriptEditorDebugPanel::CanPause))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeAvidScriptDebugIconButton(
					TEXT("PlayWorld.ContinueExecution.Small"),
					LOCTEXT("ContinueDebuggerToolTip", "Continue C# execution."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorDebugPanel::HandleContinue),
					TAttribute<bool>::CreateSP(this, &SAvidScriptEditorDebugPanel::CanResume))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeAvidScriptDebugIconButton(
					TEXT("PlayWorld.StepInto.Small"),
					LOCTEXT("StepIntoDebuggerToolTip", "Step into the next C# sequence point."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorDebugPanel::HandleStepInto),
					TAttribute<bool>::CreateSP(this, &SAvidScriptEditorDebugPanel::CanResume))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeAvidScriptDebugIconButton(
					TEXT("Icons.Refresh"),
					LOCTEXT("RefreshDebuggerToolTip", "Refresh PIE targets and debugger state."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorDebugPanel::HandleRefresh),
					TAttribute<bool>(true))
			];
	}

	TSharedRef<SWidget> BuildPageSelector()
	{
		return SNew(SUniformGridPanel)
			.SlotPadding(FMargin(1.0f))
			+ SUniformGridPanel::Slot(0, 0)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.IsChecked_Lambda([this]()
				{
					return CurrentWorkspace == 0
						? ECheckBoxState::Checked
						: ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
				{
					if (State == ECheckBoxState::Checked)
					{
						SetWorkspace(0);
					}
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("DebuggerWorkspaceTab", "Debugger"))
					.Justification(ETextJustify::Center)
				]
			]
			+ SUniformGridPanel::Slot(1, 0)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.IsChecked_Lambda([this]()
				{
					return CurrentWorkspace == 1
						? ECheckBoxState::Checked
						: ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
				{
					if (State == ECheckBoxState::Checked)
					{
						SetWorkspace(1);
					}
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ProfilerWorkspaceTab", "Profiler"))
					.Justification(ETextJustify::Center)
				]
			];
	}

	TSharedRef<SWidget> BuildDebuggerWorkspace()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 3.0f)
			[
				BuildBreakpointInput()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 3.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(4.0f, 2.0f))
				.ToolTipText(LOCTEXT("OpenPausedSourceToolTip", "Open the active paused C# source location."))
				.OnClicked(this, &SAvidScriptEditorDebugPanel::HandleOpenPausedSource)
				.IsEnabled(this, &SAvidScriptEditorDebugPanel::CanOpenPausedSource)
				[
					SNew(STextBlock)
					.Text(this, &SAvidScriptEditorDebugPanel::GetPausedSourceText)
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Vertical)
				+ SSplitter::Slot()
				.Value(0.5f)
				[
					BuildBreakpointList()
				]
				+ SSplitter::Slot()
				.Value(0.5f)
				[
					BuildVariableList()
				]
			];
	}

	TSharedRef<SWidget> BuildBreakpointInput()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(SourceInput, SEditableTextBox)
				.HintText(LOCTEXT("BreakpointSourceHint", "Scripts/AvidScript/Game.cs"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(96.0f)
				[
					SNew(SNumericEntryBox<int32>)
					.MinValue(1)
					.MaxValue(MAX_int32)
					.Value_Lambda([this]() { return TOptional<int32>(BreakpointLine); })
					.OnValueChanged_Lambda([this](const int32 Value) { BreakpointLine = FMath::Max(1, Value); })
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeAvidScriptDebugIconButton(
					TEXT("Icons.Plus"),
					LOCTEXT("AddBreakpointToolTip", "Add or enable a source breakpoint."),
					FOnClicked::CreateSP(this, &SAvidScriptEditorDebugPanel::HandleAddBreakpoint),
					TAttribute<bool>(true))
			];
	}

	TSharedRef<SWidget> BuildBreakpointList()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BreakpointsHeading", "Breakpoints"))
				.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(BreakpointRows, SVerticalBox)
				]
			];
	}

	TSharedRef<SWidget> BuildVariableList()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 5.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("VariablesHeading", "Variables"))
				.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(VariableRows, SVerticalBox)
				]
			];
	}

	EActiveTimerReturnType HandlePanelRefresh(double, float)
	{
		RefreshPanel();
		return EActiveTimerReturnType::Continue;
	}

	void RefreshPanel()
	{
		RefreshTargetOptions();
		RefreshBreakpointRows();
		RefreshVariableRows();
		const FAvidScriptEditorDebugSessionView& View = Controller->GetSessionModel().GetView();
		if (!View.LastError.IsEmpty())
		{
			StatusText = FText::FromString(View.LastError);
		}
		else if (LastCommandError.IsEmpty())
		{
			StatusText = View.bRuntimeBound
				? FText::Format(
					LOCTEXT("BoundStatusFormat", "{0} | epoch {1} | {2} breakpoint(s)"),
					GetAvidScriptDebugStateText(View.Runtime.State),
					FText::AsNumber(View.Runtime.Epoch),
					FText::AsNumber(View.Runtime.BreakpointCount))
				: LOCTEXT("NoTargetStatus", "No live PIE AvidScript target.");
		}
	}

	void RefreshTargetOptions()
	{
		FString Signature;
		for (const FAvidScriptEditorDebugTarget& Target : Controller->GetTargets())
		{
			Signature += Target.TargetId;
			Signature += TEXT("\n");
		}
		if (Signature == TargetSignature)
		{
			return;
		}

		TargetSignature = MoveTemp(Signature);
		TargetOptions.Reset();
		for (const FAvidScriptEditorDebugTarget& Target : Controller->GetTargets())
		{
			TSharedPtr<FAvidScriptDebugTargetOption> Option = MakeShared<FAvidScriptDebugTargetOption>();
			Option->TargetId = Target.TargetId;
			Option->Label = FString::Printf(
				TEXT("%s  [%s]"),
				*Target.DisplayName,
				*Target.WorldName);
			TargetOptions.Add(MoveTemp(Option));
		}
		TargetCombo->RefreshOptions();

		bRefreshingTargets = true;
		TSharedPtr<FAvidScriptDebugTargetOption> Selected;
		for (const TSharedPtr<FAvidScriptDebugTargetOption>& Option : TargetOptions)
		{
			if (Option.IsValid() && Option->TargetId == Controller->GetSelectedTargetId())
			{
				Selected = Option;
				break;
			}
		}
		TargetCombo->SetSelectedItem(Selected);
		bRefreshingTargets = false;
	}

	void RefreshBreakpointRows()
	{
		BreakpointRows->ClearChildren();
		const TArray<FAvidScriptEditorSourceBreakpoint>& Breakpoints =
			Controller->GetSessionModel().GetView().Breakpoints;
		if (Breakpoints.IsEmpty())
		{
			BreakpointRows->AddSlot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NoBreakpoints", "No source breakpoints."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
			return;
		}

		for (const FAvidScriptEditorSourceBreakpoint& Breakpoint : Breakpoints)
		{
			const FString SourceFile = Breakpoint.SourceFile;
			const FString SourceSha256 = Breakpoint.SourceSha256;
			const int32 RequestedLine = Breakpoint.RequestedLine;
			const int32 ResolvedLine = Breakpoint.ResolvedLine;
			const int32 ResolvedColumn = Breakpoint.ResolvedColumn;
			BreakpointRows->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked(Breakpoint.bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged_Lambda([this, SourceFile, RequestedLine](const ECheckBoxState State)
					{
						FString Error;
						SetCommandResult(Controller->SetSourceBreakpoint(
							SourceFile,
							RequestedLine,
							State == ECheckBoxState::Checked,
							Error), Error);
					})
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.45f)
				.Padding(6.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s:%d"), *SourceFile, RequestedLine)))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.35f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Breakpoint.FunctionName))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.2f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Breakpoint.IsBound()
						? LOCTEXT("BreakpointBound", "Bound")
						: LOCTEXT("BreakpointUnresolved", "Unresolved"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					MakeAvidScriptDebugIconButton(
						TEXT("Icons.Browse"),
						LOCTEXT("OpenBreakpointToolTip", "Open this C# breakpoint location."),
						FOnClicked::CreateLambda([this, SourceFile, SourceSha256, RequestedLine, ResolvedLine, ResolvedColumn]()
						{
							return OpenSource(
								SourceFile,
								SourceSha256,
								ResolvedLine > 0 ? ResolvedLine : RequestedLine,
								ResolvedColumn > 0 ? ResolvedColumn : 1);
						}),
						TAttribute<bool>(true))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					MakeAvidScriptDebugIconButton(
						TEXT("Icons.Delete"),
						LOCTEXT("RemoveBreakpointToolTip", "Remove this source breakpoint."),
						FOnClicked::CreateLambda([this, SourceFile, RequestedLine]()
						{
							FString Error;
							const bool bSucceeded = Controller->RemoveSourceBreakpoint(
								SourceFile,
								RequestedLine,
								Error);
							SetCommandResult(bSucceeded, Error);
							return FReply::Handled();
						}),
						TAttribute<bool>(true))
				]
			];
		}
	}

	void RefreshVariableRows()
	{
		VariableRows->ClearChildren();
		const FAvidScriptDebugVariablesSnapshot& Variables =
			Controller->GetSessionModel().GetView().Variables;
		if (Variables.Variables.IsEmpty())
		{
			VariableRows->AddSlot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NoVariables", "Variables are available while paused."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
			return;
		}

		for (const FAvidScriptDebugVariableSnapshot& Variable : Variables.Variables)
		{
			VariableRows->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(0.25f)
				[
					SNew(STextBlock).Text(FText::FromString(Variable.Name))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.3f)
				.Padding(6.0f, 0.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Variable.TypeId))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(0.45f)
				[
					SNew(STextBlock).Text(FText::FromString(Variable.Value))
				]
			];
		}
	}

	void SetWorkspace(const int32 Workspace)
	{
		CurrentWorkspace = FMath::Clamp(Workspace, 0, 1);
		LastCommandError.Reset();
		if (WorkspaceSwitcher.IsValid())
		{
			WorkspaceSwitcher->SetActiveWidgetIndex(CurrentWorkspace);
		}
		RefreshPanel();
	}

	TSharedRef<SWidget> GenerateTargetWidget(
		TSharedPtr<FAvidScriptDebugTargetOption> Option) const
	{
		return SNew(STextBlock)
			.Text(Option.IsValid() ? FText::FromString(Option->Label) : FText::GetEmpty());
	}

	void HandleTargetSelectionChanged(
		TSharedPtr<FAvidScriptDebugTargetOption> Option,
		ESelectInfo::Type)
	{
		if (bRefreshingTargets || !Option.IsValid())
		{
			return;
		}
		FString Error;
		SetCommandResult(Controller->SelectTarget(Option->TargetId, Error), Error);
	}

	FReply HandleAttach() { return RunCommand(&FAvidScriptEditorDebugTargetController::AttachDebugger); }
	FReply HandleDetach() { return RunCommand(&FAvidScriptEditorDebugTargetController::DetachDebugger); }
	FReply HandlePause() { return RunCommand(&FAvidScriptEditorDebugTargetController::RequestPause); }
	FReply HandleContinue() { return RunCommand(&FAvidScriptEditorDebugTargetController::ContinueExecution); }
	FReply HandleStepInto() { return RunCommand(&FAvidScriptEditorDebugTargetController::StepInto); }

	FReply HandleRefresh()
	{
		FString Error;
		SetCommandResult(Controller->Tick(Error), Error);
		RefreshPanel();
		return FReply::Handled();
	}

	FReply HandleAddBreakpoint()
	{
		FString Error;
		SetCommandResult(Controller->SetSourceBreakpoint(
			SourceInput->GetText().ToString(),
			BreakpointLine,
			true,
			Error), Error);
		RefreshPanel();
		return FReply::Handled();
	}

	FReply HandleOpenPausedSource()
	{
		const FAvidScriptDebugVariablesSnapshot& Variables =
			Controller->GetSessionModel().GetView().Variables;
		return OpenSource(
			Variables.SourceFile,
			Variables.SourceSha256,
			Variables.Line,
			Variables.Column);
	}

	FReply OpenSource(
		const FString& SourceFile,
		const FString& SourceSha256,
		const int32 Line,
		const int32 Column)
	{
		FAvidScriptEditorSourceLocation Location;
		Location.File = SourceFile;
		Location.SourceSha256 = SourceSha256;
		Location.Line = Line;
		Location.Column = Column;
		FAvidScriptEditorDiagnosticNavigationResult Result;
		const bool bSucceeded = FAvidScriptEditorDiagnosticNavigation::Open(
			Location,
			FPaths::ProjectDir(),
			Result);
		SetCommandResult(bSucceeded, Result.ErrorMessage);
		return FReply::Handled();
	}

	using FControllerCommand = bool (FAvidScriptEditorDebugTargetController::*)(FString&);
	FReply RunCommand(const FControllerCommand Command)
	{
		FString Error;
		SetCommandResult((Controller.Get()->*Command)(Error), Error);
		RefreshPanel();
		return FReply::Handled();
	}

	void SetCommandResult(const bool bSucceeded, const FString& Error)
	{
		LastCommandError = bSucceeded ? FString() : Error;
		if (!LastCommandError.IsEmpty())
		{
			StatusText = FText::FromString(LastCommandError);
		}
	}

	bool CanAttach() const
	{
		const FAvidScriptEditorDebugSessionView& View = Controller->GetSessionModel().GetView();
		return View.bRuntimeBound && View.Runtime.State == EAvidScriptDebugSessionState::Detached;
	}

	bool CanDetach() const
	{
		const FAvidScriptEditorDebugSessionView& View = Controller->GetSessionModel().GetView();
		return View.bRuntimeBound
			&& View.Runtime.State != EAvidScriptDebugSessionState::Detached
			&& View.Runtime.State != EAvidScriptDebugSessionState::Paused
			&& View.Runtime.State != EAvidScriptDebugSessionState::Resuming;
	}

	bool CanPause() const
	{
		const FAvidScriptEditorDebugSessionView& View = Controller->GetSessionModel().GetView();
		return View.bRuntimeBound && View.Runtime.State == EAvidScriptDebugSessionState::Running;
	}

	bool CanResume() const
	{
		const FAvidScriptEditorDebugSessionView& View = Controller->GetSessionModel().GetView();
		return View.bRuntimeBound && View.Runtime.State == EAvidScriptDebugSessionState::Paused;
	}

	bool CanOpenPausedSource() const
	{
		const FAvidScriptDebugVariablesSnapshot& Variables =
			Controller->GetSessionModel().GetView().Variables;
		return !Variables.SourceFile.IsEmpty() && Variables.Line > 0 && Variables.Column > 0;
	}

	FText GetSelectedTargetText() const
	{
		const TSharedPtr<FAvidScriptDebugTargetOption> Selected = TargetCombo->GetSelectedItem();
		return Selected.IsValid()
			? FText::FromString(Selected->Label)
			: LOCTEXT("NoSelectedTarget", "No PIE target");
	}

	FText GetPausedSourceText() const
	{
		const FAvidScriptDebugVariablesSnapshot& Variables =
			Controller->GetSessionModel().GetView().Variables;
		return CanOpenPausedSource()
			? FText::FromString(FString::Printf(
				TEXT("%s:%d  %s"),
				*Variables.SourceFile,
				Variables.Line,
				*Variables.FunctionName))
			: LOCTEXT("NoPausedSource", "No active paused source location");
	}

	FText GetStatusText() const { return StatusText; }

	TSharedPtr<FAvidScriptEditorDebugTargetController> Controller;
	TArray<TSharedPtr<FAvidScriptDebugTargetOption>> TargetOptions;
	TSharedPtr<SComboBox<TSharedPtr<FAvidScriptDebugTargetOption>>> TargetCombo;
	TSharedPtr<SWidgetSwitcher> WorkspaceSwitcher;
	TSharedPtr<SEditableTextBox> SourceInput;
	TSharedPtr<SVerticalBox> BreakpointRows;
	TSharedPtr<SVerticalBox> VariableRows;
	FString TargetSignature;
	FString LastCommandError;
	FText StatusText;
	int32 BreakpointLine = 1;
	int32 CurrentWorkspace = 0;
	bool bRefreshingTargets = false;
};
} // namespace

TSharedRef<SWidget> MakeAvidScriptEditorDebugPanel(
	TSharedRef<FAvidScriptEditorDebugTargetController> Controller)
{
	return SNew(SAvidScriptEditorDebugPanel, Controller);
}

#undef LOCTEXT_NAMESPACE
