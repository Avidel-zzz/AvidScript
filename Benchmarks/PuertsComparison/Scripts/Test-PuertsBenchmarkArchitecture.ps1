[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$RepositoryRoot = Split-Path -Parent (Split-Path -Parent $BenchmarkRoot)

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53A1000 $Message"
    }
}

function Read-RequiredText {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $Path = Join-Path $BenchmarkRoot $RelativePath
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "required file is missing: $RelativePath"
    return Get-Content -LiteralPath $Path -Raw
}

$ProductionSourceRoot = Join-Path $RepositoryRoot 'Source'
$ForbiddenMatches = @(
    Get-ChildItem -LiteralPath $ProductionSourceRoot -Recurse -File -Include '*.h', '*.cpp', '*.Build.cs' |
        Select-String -Pattern 'Puerts|FJsEnv|JsObject\.h|Binding\.hpp'
)
Assert-True ($ForbiddenMatches.Count -eq 0) 'production AvidScript modules must not depend on Puerts'

$Plugin = Read-RequiredText 'AvidScriptPerfHarness/AvidScriptPerfHarness.uplugin' | ConvertFrom-Json
$PluginNames = @($Plugin.Plugins | ForEach-Object { [string]$_.Name })
Assert-True ($PluginNames -ccontains 'AvidScript') 'benchmark harness must depend on AvidScript'
Assert-True ($PluginNames -ccontains 'Puerts') 'benchmark harness must depend on Puerts'
Assert-True (@($Plugin.Modules).Count -eq 1) 'benchmark harness must stay isolated in one optional module'

$BuildRules = Read-RequiredText 'AvidScriptPerfHarness/Source/AvidScriptPerfHarness/AvidScriptPerfHarness.Build.cs'
Assert-True ($BuildRules.Contains('"JsEnv"')) 'benchmark harness must compile against the official Puerts JsEnv module'
Assert-True ($BuildRules.Contains('"AvidScriptRuntime"')) 'benchmark harness must use the formal AvidScript runtime surface'

$Fixture = Read-RequiredText 'AvidScriptPerfHarness/Source/AvidScriptPerfHarness/Public/AvidScriptPerfFixture.h'
foreach ($Method in @(
    'ReflectNoOp',
    'ReflectAddInt32',
    'ReflectSetScalar',
    'ReflectGetScalar',
    'ReflectVectorValue',
    'ReflectObjectRoundtrip',
    'ReflectBatchAdd',
    'NativeNoOp',
    'NativeAddInt32',
    'NativeVectorValue',
    'NativeObjectRoundtrip',
    'NativeBatchAdd')) {
    Assert-True ($Fixture.Contains($Method)) "shared fixture is missing method: $Method"
}

$StaticBindings = Read-RequiredText 'AvidScriptPerfHarness/Source/AvidScriptPerfHarness/Private/AvidScriptPerfStaticBindings.cpp'
Assert-True ($StaticBindings.Contains('puerts::DefineClass<AvidScriptPerfStatic>()')) 'static lane must use Puerts template binding'
foreach ($Operation in @('NoOp', 'AddInt32', 'SetScalar', 'GetScalar', 'VectorValue', 'ObjectRoundtrip', 'BatchAdd')) {
    Assert-True ($StaticBindings.Contains(".Function(`"$Operation`"")) "static lane is missing operation: $Operation"
}

$ReflectionScript = Read-RequiredText 'AvidScriptPerfHarness/Content/JavaScript/reflection.js'
$StaticScript = Read-RequiredText 'AvidScriptPerfHarness/Content/JavaScript/static.js'
Assert-True ($ReflectionScript.Contains('fixture.ReflectAddInt32')) 'reflection lane must call the reflected UFUNCTION surface'
Assert-True ($StaticScript.Contains('cpp.AvidScriptPerfStatic')) 'static lane must call the template-bound C++ surface'
Assert-True ($ReflectionScript.Contains('RegisterPuertsCallbacks')) 'reflection lane must publish host-callable callbacks'
Assert-True ($StaticScript.Contains('RegisterPuertsCallbacks')) 'static lane must publish host-callable callbacks'

$AvidScriptWorkload = Read-RequiredText 'Workloads/AvidScriptPerfWorkload.cs'
$AvidScriptProfile = Read-RequiredText 'Workloads/AvidScriptPerfWorkload.csharp-profile.json' | ConvertFrom-Json
Assert-True ($AvidScriptWorkload.Contains('EntryPoint = "avid_on_event"')) 'AvidScript lane must use the formal gameplay event export'
Assert-True ($AvidScriptWorkload.Contains('UAvidScriptPerfFixture fixture = UE.Self')) 'AvidScript lane must use the shared fixture as self'
Assert-True ($AvidScriptWorkload.Contains('fixture.ReflectAddInt32')) 'AvidScript lane must use the generated reflected binding'
Assert-True ($AvidScriptProfile.binding_profile.self_class_path -ceq '/Script/AvidScriptPerfHarness.AvidScriptPerfFixture') 'AvidScript profile self class must be the shared fixture'

$Profile = Read-RequiredText 'Config/BenchmarkProfile.json' | ConvertFrom-Json
$ExpectedLanes = @('native_cpp', 'puerts_v8_reflection', 'puerts_v8_static', 'avidscript_wamr')
Assert-True (@($Profile.lanes).Count -eq $ExpectedLanes.Count) 'benchmark profile must contain exactly four lanes'
for ($Index = 0; $Index -lt $ExpectedLanes.Count; ++$Index) {
    Assert-True ([string]$Profile.lanes[$Index] -ceq $ExpectedLanes[$Index]) "benchmark lane order mismatch at index $Index"
}
Assert-True ([int]$Profile.process_runs -ge 5) 'benchmark profile requires at least five process runs'
Assert-True ([int]$Profile.timed_samples -ge 30) 'benchmark profile requires at least thirty timed samples'

$ResultSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkResult.schema.json'
$ResultSchema = Get-Content -LiteralPath $ResultSchemaPath -Raw | ConvertFrom-Json
Assert-True (@($ResultSchema.required) -ccontains 'provenance') 'result schema must require provenance'
Assert-True (@($ResultSchema.required) -ccontains 'samples') 'result schema must require raw samples'

$TrackedText = @(
    Get-ChildItem -LiteralPath $BenchmarkRoot -Recurse -File |
        Where-Object { $_.Extension -in @('.ps1', '.json', '.md', '.h', '.cpp', '.cs', '.js') } |
        Get-Content -Raw
) -join "`n"
$Separator = [System.IO.Path]::DirectorySeparatorChar
$UserProfileToken = 'C:' + $Separator + 'Users' + $Separator
$PrivateKeyToken = @('BEGIN', 'OPENSSH', 'PRIVATE', 'KEY') -join ' '
Assert-True (-not $TrackedText.Contains($UserProfileToken)) 'benchmark sources must not contain a user-profile absolute path'
Assert-True (-not $TrackedText.Contains($PrivateKeyToken)) 'benchmark sources must not contain private key material'

Write-Output 'Puerts benchmark architecture passed: production_isolation=1 lanes=4 shared_fixture=1 provenance=1 privacy=1'
