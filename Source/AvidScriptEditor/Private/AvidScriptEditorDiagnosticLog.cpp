#include "AvidScriptEditorDiagnosticLog.h"

#include "AvidScriptEditorCallStack.h"
#include "AvidScriptEditorModule.h"
#include "AvidScriptEditorResultPresentation.h"

#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
const FName AvidScriptMessageLogName(TEXT("AvidScript"));

EMessageSeverity::Type ToAvidScriptMessageSeverity(
	const EAvidScriptEditorPresentationSeverity Severity)
{
	switch (Severity)
	{
	case EAvidScriptEditorPresentationSeverity::Error:
		return EMessageSeverity::Error;
	case EAvidScriptEditorPresentationSeverity::Warning:
		return EMessageSeverity::Warning;
	case EAvidScriptEditorPresentationSeverity::Info:
	default:
		return EMessageSeverity::Info;
	}
}

FString MakeAvidScriptCallStackFrameText(const FAvidScriptEditorCallStackFrame& Frame)
{
	if (Frame.IsSourceNavigable())
	{
		return FString::Printf(
			TEXT("#%d %s (%s:%d:%d)"),
			Frame.Ordinal,
			*Frame.DisplayName,
			*Frame.SourceLocation.File,
			Frame.SourceLocation.Line,
			Frame.SourceLocation.Column);
	}
	return FString::Printf(TEXT("#%d %s"), Frame.Ordinal, *Frame.DisplayName);
}
} // namespace

void FAvidScriptEditorDiagnosticLog::Register()
{
	FMessageLogModule& MessageLogModule =
		FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
	if (!MessageLogModule.IsRegisteredLogListing(AvidScriptMessageLogName))
	{
		FMessageLogInitializationOptions Options;
		Options.bShowPages = true;
		Options.bScrollToBottom = true;
		Options.MaxPageCount = 20;
		MessageLogModule.RegisterLogListing(
			AvidScriptMessageLogName,
			NSLOCTEXT("AvidScriptEditor", "AvidScriptMessageLog", "AvidScript"),
			Options);
	}
}

void FAvidScriptEditorDiagnosticLog::Unregister()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("MessageLog")))
	{
		FMessageLogModule& MessageLogModule =
			FModuleManager::GetModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
		if (MessageLogModule.IsRegisteredLogListing(AvidScriptMessageLogName))
		{
			MessageLogModule.UnregisterLogListing(AvidScriptMessageLogName);
		}
	}
}

void FAvidScriptEditorDiagnosticLog::Publish(
	const FAvidScriptEditorCommandPresentation& Presentation)
{
	if (Presentation.CallStack.IsEmpty())
	{
		return;
	}

	const EMessageSeverity::Type Severity = ToAvidScriptMessageSeverity(Presentation.Severity);
	FMessageLog MessageLog(AvidScriptMessageLogName);
	MessageLog.NewPage(FText::FromString(Presentation.Title));
	MessageLog.Message(
		Severity,
		FText::FromString(FString::Printf(TEXT("%s: %s"), *Presentation.Title, *Presentation.Body)));

	for (const FAvidScriptEditorCallStackFrame& Frame : Presentation.CallStack)
	{
		TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(
			Severity,
			FText::FromString(MakeAvidScriptCallStackFrameText(Frame)));
		if (Frame.IsSourceNavigable())
		{
			Message->AddToken(FActionToken::Create(
				NSLOCTEXT("AvidScriptEditor", "OpenCallStackSource", "Open source"),
				FText::FromString(Frame.SourceLocation.File),
				FOnActionTokenExecuted::CreateLambda([Frame]()
				{
					FAvidScriptEditorDiagnosticNavigationResult Result;
					if (!FAvidScriptEditorCallStack::OpenSource(
							Frame,
							FPaths::ProjectDir(),
							Result))
					{
						UE_LOG(
							LogAvidScriptEditor,
							Warning,
							TEXT("AvidScript call-stack navigation failed: category=%s message=%s"),
							*Result.ErrorCategory,
							*Result.ErrorMessage);
					}
				})));
		}
		MessageLog.AddMessage(Message);
	}
	MessageLog.Notify(
		NSLOCTEXT("AvidScriptEditor", "AvidScriptRuntimeFailure", "AvidScript runtime diagnostic available"),
		Severity,
		true);
}
