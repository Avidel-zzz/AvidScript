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
Assert-True ($Plugin.EnabledByDefault -eq $false) 'optional competitor harness must not affect ordinary project builds'

$BuildRules = Read-RequiredText 'AvidScriptPerfHarness/Source/AvidScriptPerfHarness/AvidScriptPerfHarness.Build.cs'
Assert-True ($BuildRules.Contains('"JsEnv"')) 'benchmark harness must compile against the official Puerts JsEnv module'
Assert-True ($BuildRules.Contains('"AvidScriptRuntime"')) 'benchmark harness must use the formal AvidScript runtime surface'

$Fixture = Read-RequiredText 'AvidScriptPerfHarness/Source/AvidScriptPerfHarness/Public/AvidScriptPerfFixture.h'
Assert-True ($Fixture.Contains('AAvidScriptPerfFixture final : public AActor')) 'shared fixture must satisfy the AvidScript Actor self contract'
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
Assert-True ($StaticBindings.Contains('puerts::DefineClass<AAvidScriptPerfFixture>()')) 'static lane must bind the shared fixture instance'
foreach ($Operation in @('StaticNoOp', 'StaticAddInt32', 'StaticVectorValue', 'StaticObjectRoundtrip', 'StaticBatchAdd')) {
    Assert-True ($StaticBindings.Contains(".Method(`"$Operation`"")) "static lane is missing operation: $Operation"
}
foreach ($Method in @('ReflectNoOp', 'ReflectAddInt32', 'ReflectVectorValue', 'ReflectObjectRoundtrip', 'ReflectBatchAdd')) {
    Assert-True ($StaticBindings.Contains("AAvidScriptPerfFixture::$Method")) "static lane must bind the reflected implementation: $Method"
}
Assert-True (-not $StaticBindings.Contains('AAvidScriptPerfFixture::Native')) 'static lane must not retain the old native-wrapper advantage'
Assert-True ($StaticBindings.Contains('.Property("StaticScalarValue"')) 'static lane must benchmark a template-bound property'

$ReflectionScript = Read-RequiredText 'AvidScriptPerfHarness/Content/JavaScript/reflection.js'
$StaticScript = Read-RequiredText 'AvidScriptPerfHarness/Content/JavaScript/static.js'
Assert-True ($ReflectionScript.Contains('fixture.ReflectAddInt32')) 'reflection lane must call the reflected UFUNCTION surface'
Assert-True ($StaticScript.Contains('fixture.StaticAddInt32')) 'static lane must call the instance template-bound surface'
Assert-True ($ReflectionScript.Contains('fixture.ScalarValue =')) 'reflection lane must benchmark reflected property access'
Assert-True ($StaticScript.Contains('fixture.StaticScalarValue =')) 'static lane must benchmark template-bound property access'
Assert-True ($ReflectionScript.Contains('RegisterPuertsCallbacks')) 'reflection lane must publish host-callable callbacks'
Assert-True ($StaticScript.Contains('RegisterPuertsCallbacks')) 'static lane must publish host-callable callbacks'

$AvidScriptWorkload = Read-RequiredText 'AvidScriptPerfHarness/Content/CSharp/AvidScriptPerfWorkload.cs'
$AvidScriptProfile = Read-RequiredText 'AvidScriptPerfHarness/Content/CSharp/AvidScriptPerfWorkload.csharp-profile.json' | ConvertFrom-Json
Assert-True ($AvidScriptWorkload.Contains('EntryPoint = "avid_on_event"')) 'AvidScript lane must use the formal gameplay event export'
Assert-True ($AvidScriptWorkload.Contains('AAvidScriptPerfFixture fixture = UE.Self')) 'AvidScript lane must use the shared fixture as self'
Assert-True ($AvidScriptWorkload.Contains('fixture.ReflectAddInt32')) 'AvidScript lane must use the generated reflected binding'
Assert-True ($AvidScriptWorkload.Contains('fixture.ScalarValue =')) 'AvidScript lane must use generated property binding'
Assert-True ($AvidScriptWorkload.Contains('[AvidPersist]')) 'AvidScript lane must publish checksum through guest state'
Assert-True ($AvidScriptProfile.binding_profile.self_class_path -ceq '/Script/AvidScriptPerfHarness.AvidScriptPerfFixture') 'AvidScript profile self class must be the shared fixture'

$Profile = Read-RequiredText 'Config/BenchmarkProfile.json' | ConvertFrom-Json
$ExpectedLanes = @(
    'native_cpp',
    'puerts_v8_reflection',
    'puerts_v8_static',
    'avidscript_wasmtime_semantic',
    'avidscript_wasmtime_native_direct'
)
Assert-True ([int]$Profile.schema_version -eq 2) 'benchmark profile must use schema v2'
Assert-True (@($Profile.lanes).Count -eq $ExpectedLanes.Count) 'benchmark profile must contain exactly five lanes'
for ($Index = 0; $Index -lt $ExpectedLanes.Count; ++$Index) {
    Assert-True ([string]$Profile.lanes[$Index] -ceq $ExpectedLanes[$Index]) "benchmark lane order mismatch at index $Index"
}
Assert-True ([int]$Profile.process_runs -ge 5) 'benchmark profile requires at least five process runs'
Assert-True ([int]$Profile.timed_samples -ge 30) 'benchmark profile requires at least thirty timed samples'
Assert-True ([int]$Profile.seed -ge -16777216 -and [int]$Profile.seed -le 16777216) 'event ABI seed must remain exactly representable by float'
$SemanticLane = @($Profile.lane_catalog | Where-Object { $_.lane_id -ceq 'avidscript_wasmtime_semantic' })[0]
$DirectLane = @($Profile.lane_catalog | Where-Object { $_.lane_id -ceq 'avidscript_wasmtime_native_direct' })[0]
Assert-True ([string]$SemanticLane.backend_id -ceq 'wasmtime.cranelift.jit') 'semantic lane must use Wasmtime Cranelift'
Assert-True ([string]$DirectLane.backend_id -ceq 'wasmtime.cranelift.jit') 'direct lane must use Wasmtime Cranelift'
Assert-True ([string]$SemanticLane.binding_invocation_mode -ceq 'semantic_process_event') 'semantic lane must publish semantic_process_event'
Assert-True ([string]$DirectLane.binding_invocation_mode -ceq 'qualified_native_direct') 'direct lane must publish qualified_native_direct'
foreach ($Property in @(
    'runtime_id',
    'runtime_version',
    'source_wasm_sha256',
    'execution_artifact_sha256',
    'runtime_build_identity',
    'runtime_artifact_sha256',
    'backend_id')) {
    Assert-True ([string]$SemanticLane.$Property -ceq [string]$DirectLane.$Property) "Wasmtime lanes must share $Property"
}

$Runner = Read-RequiredText 'AvidScriptPerfHarness/Source/AvidScriptPerfHarness/Private/AvidScriptPerfRunner.cpp'
Assert-True ($Runner.Contains('EAvidScriptBindingInvocationPolicy::SemanticProcessEvent')) 'runner must explicitly configure the semantic session'
Assert-True ($Runner.Contains('EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect')) 'runner must explicitly configure the direct session'
Assert-True ($Runner.Contains('TryGetInvocationMode')) 'runner must query immutable package invocation plans before timing'
Assert-True ($Runner.Contains('DirectHitCount')) 'runner must record direct-hit evidence'
Assert-True ($Runner.Contains('RequestedDirectFallbackCount')) 'runner must record requested-direct fallback evidence'
Assert-True ($Runner.Contains('FAvidScriptLane WasmtimeSemantic')) 'runner must own an independent semantic Wasmtime session'
Assert-True ($Runner.Contains('FAvidScriptLane WasmtimeNativeDirect')) 'runner must own an independent direct Wasmtime session'

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

Write-Output 'Puerts benchmark architecture passed: production_isolation=1 lanes=5 shared_fixture=1 provenance=1 privacy=1'
