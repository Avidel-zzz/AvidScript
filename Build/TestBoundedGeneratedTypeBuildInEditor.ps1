[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackageManifestPath,
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = "$env:USERPROFILE/.dotnet/dotnet.exe"
)

$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$project = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$root = Join-Path $projectRoot 'Saved/AvidScriptBoundedGeneratedTypeBuild'
$source = Join-Path $pluginRoot 'Fixtures/Phase66/BoundedGeneratedType.cs'
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$native = Join-Path $root 'Native'
$artifacts = Join-Path $root 'Artifacts'
$runtime = Join-Path $artifacts "$sourceHash/Runtime"
$package = (Resolve-Path -LiteralPath $BindingPackageManifestPath -ErrorAction Stop).Path

& pwsh -NoProfile -File (Join-Path $pluginRoot 'Build/BuildCSharpScriptTypes.ps1') `
    -DotNetPath $DotNetPath -SourcePath $source `
    -SourceId 'Fixtures/Phase66/BoundedGeneratedType.cs' `
    -ProjectPath (Join-Path $pluginRoot 'Fixtures/Phase66/BoundedGeneratedType.csproj') `
    -BindingPackageManifestPath $package -OutputRoot $native -ArtifactRoot $artifacts `
    -CookOutputRoot (Join-Path $root 'Cook') `
    -RuntimeModuleId 'bounded_generated_type_build' -LanguageErrors bounded
if ($LASTEXITCODE -ne 0) { throw 'Formal bounded generated type build failed.' }

& node (Join-Path $pluginRoot 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs') `
    (Join-Path $runtime 'generated_types.wasm') --bounded-generated-type `
    (Join-Path $native 'AvidScriptGeneratedManifest.json')
if ($LASTEXITCODE -ne 0) { throw 'Bounded generated UFunction Node execution failed.' }

foreach ($name in @('generated_types.wasm', 'generated_types.avidscript.json')) {
    Copy-Item -LiteralPath (Join-Path $runtime $name) -Destination (Join-Path $root $name) -Force
    if ((Get-FileHash -LiteralPath (Join-Path $runtime $name) -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath (Join-Path $root $name) -Algorithm SHA256).Hash) {
        throw "Copied formal Runtime artifact changed: $name"
    }
}
Copy-Item -LiteralPath (Join-Path $native 'AvidScriptGeneratedManifest.json') `
    -Destination (Join-Path $root 'AvidScriptGeneratedManifest.json') -Force

$buildBat = Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat'
& $buildBat AvidTPSTemplateEditor Win64 Development "-Project=$project" `
    -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -gather -MaxParallelActions=1 -NoUBA
if ($LASTEXITCODE -ne 0) { throw "No-clean Editor build failed: $LASTEXITCODE" }

$editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$log = Join-Path $projectRoot "Saved/Logs/AvidScript_BoundedGeneratedTypeBuild_$runId.log"
$test = 'AvidScript.Runtime.BoundedGeneratedTypeBuild.FormalPackage'
& $editor $project -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $test;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$log"
if ($LASTEXITCODE -ne 0) { throw "Bounded generated type Automation exited $LASTEXITCODE; log=$log" }
$contents = Get-Content -LiteralPath $log -Raw
$success = [regex]::Matches($contents,
    'Test Completed\. Result=\{Success\} Name=\{FormalPackage\} Path=\{AvidScript\.Runtime\.BoundedGeneratedTypeBuild\.FormalPackage\}').Count
$failed = [regex]::Matches($contents, 'Test Completed\. Result=\{Fail\}').Count
$complete = [regex]::Matches($contents, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
if ($success -ne 1 -or $failed -ne 0 -or $complete -ne 1) {
    throw "Bounded generated type Automation evidence incomplete: success=$success failed=$failed complete=$complete log=$log"
}
Write-Host "AvidScript.Runtime.BoundedGeneratedTypeBuild: 1/1 passed; log=$log"
