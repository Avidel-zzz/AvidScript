[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$GuestIrPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$buildPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'BuildCSharpActorLifecycle.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($buildPath, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw ($errors | Out-String) }
foreach ($name in @('Test-CompilerInjectedBindingImport', 'Test-BindingPackageImports')) {
    $definitions = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
    }, $false))
    if ($definitions.Count -ne 1) { throw "Missing unique import guard: $name" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
}
$ir = Get-Content -LiteralPath $GuestIrPath -Raw | ConvertFrom-Json
if ($ir.schema_version -ne 24 -or $ir.ir_version -cne '1.23') { throw 'Expected compiled IR 24/1.23.' }
$passed = 0
foreach ($name in @('avid_task_cancel_language_error_v1', 'avid_task_terminal_error_meta_v1', 'avid_task_terminal_error_root_v1')) {
    $imports = @($ir.imports | Where-Object name -CEQ $name)
    if ($imports.Count -ne 1) { throw "Expected one cancellation import: $name" }
    $original = $imports[0]
    foreach ($bounded in @($false, $true)) {
        foreach ($cancellation in @($false, $true)) {
            $accepted = Test-CompilerInjectedBindingImport -Import $original -AllowDataLaneImports $false `
                -AllowGeneratedTypeImports $false -AllowDebugImports $false -AllowDirectAwaitCleanup $false `
                -AllowBoundedLanguageErrors $bounded -AllowAsyncCancellationFlow $cancellation
            if ($accepted -ne ($bounded -and $cancellation)) { throw "Incorrect cancellation authorization: $name bounded=$bounded cancellation=$cancellation" }
            $passed++
        }
    }
    foreach ($field in @('id', 'name', 'module', 'dispatch_class', 'optimization_class', 'binding_ordinal', 'parameter_type_ids', 'return_type_id')) {
        $mutated = $original | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        switch ($field) {
            'id' { $mutated.id = 'import:symbol:method:global::User.Native.Call():int32' }
            'name' { $mutated.name += '_spoof' }
            'module' { $mutated.module = 'env' }
            'dispatch_class' { $mutated.dispatch_class = 'dynamic' }
            'optimization_class' { $mutated.optimization_class = 'snapshot_read' }
            'binding_ordinal' { $mutated.binding_ordinal = 0 }
            'parameter_type_ids' { $mutated.parameter_type_ids[0] = 'type:float32' }
            'return_type_id' { $mutated.return_type_id = 'type:void' }
        }
        if (Test-CompilerInjectedBindingImport -Import $mutated -AllowDataLaneImports $false `
            -AllowGeneratedTypeImports $false -AllowDebugImports $false -AllowDirectAwaitCleanup $false `
            -AllowBoundedLanguageErrors $true -AllowAsyncCancellationFlow $true) {
            throw "Malformed cancellation import accepted: $name field=$field"
        }
        $passed++
    }
    # A binding package must not grant access to compiler-reserved cancellation ABI.
    $package = [pscustomobject]@{ RequiredImports = @([pscustomobject]@{
        Module = 'avidscript'; Name = $name; StableId = 'test'; Ordinal = 0; Signature = 'test'
    }) }
    foreach ($bounded in @($false, $true)) {
        $result = Test-BindingPackageImports -PackageInfo $package -GuestImports @($original) `
            -AllowBoundedLanguageErrors $bounded -AllowAsyncCancellationFlow $false
        if (@($result.UnexpectedImports).Count -ne 1) { throw "Binding declaration bypassed cancellation opt-in: $name" }
        $passed++
    }
}
if ($passed -ne 42) { throw "Cancellation import coverage changed: $passed" }
Write-Output 'AsyncCancellationImportContracts: 42/42 passed'
