#include "AvidScriptVMModule.h"

#include "AvidScriptVmBackend.h"
#include "Modules/ModuleManager.h"

IAvidScriptVmGuestMemory::IAvidScriptVmGuestMemory() = default;

IAvidScriptVmGuestMemory::~IAvidScriptVmGuestMemory() = default;

IMPLEMENT_MODULE(FAvidScriptVMModule, AvidScriptVM)
