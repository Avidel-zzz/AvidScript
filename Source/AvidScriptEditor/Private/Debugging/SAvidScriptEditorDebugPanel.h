#pragma once

#include "CoreMinimal.h"

class FAvidScriptEditorDebugTargetController;
class SWidget;

TSharedRef<SWidget> MakeAvidScriptEditorDebugPanel(
	TSharedRef<FAvidScriptEditorDebugTargetController> Controller);
