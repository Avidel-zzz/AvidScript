#include "AvidScriptEditorModule.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogAvidScriptEditor);

void FAvidScriptEditorModule::StartupModule()
{
	UE_LOG(LogAvidScriptEditor, Display, TEXT("AvidScriptEditor module started."));
}

void FAvidScriptEditorModule::ShutdownModule()
{
	UE_LOG(LogAvidScriptEditor, Display, TEXT("AvidScriptEditor module stopped."));
}

IMPLEMENT_MODULE(FAvidScriptEditorModule, AvidScriptEditor)