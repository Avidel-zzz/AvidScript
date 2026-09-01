#pragma once

struct FAvidScriptEditorCommandPresentation;

class FAvidScriptEditorDiagnosticLog
{
public:
	static void Register();
	static void Unregister();
	static void Publish(const FAvidScriptEditorCommandPresentation& Presentation);
};
