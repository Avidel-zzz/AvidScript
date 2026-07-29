#pragma once

#include "AvidScriptWasmRuntime.h"

namespace AvidScriptWasmRuntimePrivate
{
bool CacheResolvedVmExport(
	IAvidScriptVmBackend& Backend,
	const FAvidScriptVmExportHandle& Handle,
	uint32 ExpectedParameterCellCount,
	FAvidScriptCachedVmExport& OutExport,
	FAvidScriptVmError& OutError);
}
