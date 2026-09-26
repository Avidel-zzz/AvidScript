[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$GuestIrPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$buildPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'BuildCSharpActorLifecycle.ps1'
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($buildPath, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw ($errors | Out-String) }
$definitions = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Test-CompilerInjectedBindingImport'
}, $false))
if ($definitions.Count -ne 1) { throw 'Compiler import guard must have exactly one definition.' }
# Load the real guard without running the build entrypoint.
. ([scriptblock]::Create($definitions[0].Extent.Text))
$ir = Get-Content -LiteralPath $GuestIrPath -Raw | ConvertFrom-Json
$passed = 0
foreach ($bounded in @($false, $true)) {
    $options = @{
        AllowDataLaneImports = $false
        AllowGeneratedTypeImports = $false
        AllowDebugImports = $false
        AllowBoundedLanguageErrors = $bounded
        AllowDirectAwaitCleanup = $bounded
    }
    foreach ($name in @('avid_managed_heap_v1', 'avid_continuation_state_store_v1', 'avid_continuation_state_read_v1')) {
        $imports = @($ir.imports | Where-Object name -CEQ $name)
        if ($imports.Count -ne 1) { throw "Expected one compiled managed import: $name" }
        $original = $imports[0]
        if (-not (Test-CompilerInjectedBindingImport -Import $original @options)) {
            throw "Compiler-owned import rejected: $name bounded=$bounded"
        }
        $passed++
        foreach ($field in @('id', 'name', 'module', 'dispatch_class', 'optimization_class', 'binding_ordinal', 'parameter_type_ids', 'return_type_id')) {
            $mutated = $original | ConvertTo-Json -Depth 20 | ConvertFrom-Json
            switch ($field) {
                'id' { $mutated.id = 'import:symbol:method:global::User.Native.Call():int32' }
                'name' { $mutated.name += '_spoof' }
                'module' { $mutated.module = 'env' }
                'dispatch_class' { $mutated.dispatch_class = 'dynamic' }
                'optimization_class' { $mutated.optimization_class = 'unexpected' }
                'binding_ordinal' { $mutated.binding_ordinal = 0 }
                'parameter_type_ids' { $mutated.parameter_type_ids[0] = 'type:float32' }
                'return_type_id' { $mutated.return_type_id = 'type:void' }
            }
            if (Test-CompilerInjectedBindingImport -Import $mutated @options) {
                throw "Malformed compiler import accepted: $name field=$field bounded=$bounded"
            }
            $passed++
        }
    }
}
if ($passed -ne 54) { throw "Managed import coverage changed: $passed" }
Write-Output 'CompilerManagedImportContracts: 54/54 passed'
