[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = "$env:USERPROFILE/.dotnet/dotnet.exe"
)

$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$project = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$output = Join-Path $projectRoot 'Saved/AvidScriptBoundedLanguageErrorBuild'
$package = (Resolve-Path -LiteralPath $BindingPackagePath -ErrorAction Stop).Path
$source = Join-Path $pluginRoot 'Fixtures/Phase66/BoundedLanguageErrorsLifecycle.cs'
$fixtureProject = Join-Path $pluginRoot 'Fixtures/Phase66/BoundedLanguageErrorsLifecycle.csproj'
$build = Join-Path $pluginRoot 'Build/BuildCSharpActorLifecycle.ps1'
$stem = 'bounded_language_errors'
& pwsh -NoProfile -File $build -DotNetPath $DotNetPath `
    -SourcePath $source -ProjectPath $fixtureProject -ProjectRoot $projectRoot `
    -BindingPackagePath $package -OutputRoot $output -ArtifactStem $stem `
    -ModuleId $stem -LanguageErrors bounded -CompilerWorkerMode disabled
if ($LASTEXITCODE -ne 0) { throw 'Formal bounded lifecycle build failed.' }
& node (Join-Path $pluginRoot 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs') `
    (Join-Path $output "$stem.wasm") --bounded-lifecycle
if ($LASTEXITCODE -ne 0) { throw 'Formal bounded lifecycle Node execution failed.' }

$buildBat = Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat'
& $buildBat AvidTPSTemplateEditor Win64 Development "-Project=$project" `
    -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -MaxParallelActions=1 -NoUBA
if ($LASTEXITCODE -ne 0) { throw "No-clean Editor build failed: $LASTEXITCODE" }

$editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$log = Join-Path $projectRoot "Saved/Logs/AvidScript_BoundedLanguageErrorBuild_$runId.log"
$test = 'AvidScript.Runtime.BoundedLanguageErrorBuild.FormalManifest'
& $editor $project -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $test;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$log"
if ($LASTEXITCODE -ne 0) { throw "Bounded language-error Automation exited $LASTEXITCODE; log=$log" }
$contents = Get-Content -LiteralPath $log -Raw
$success = [regex]::Matches($contents,
    'Test Completed\. Result=\{Success\} Name=\{FormalManifest\} Path=\{AvidScript\.Runtime\.BoundedLanguageErrorBuild\.FormalManifest\}').Count
$failed = [regex]::Matches($contents, 'Test Completed\. Result=\{Fail\}').Count
$complete = [regex]::Matches($contents, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
if ($success -ne 1 -or $failed -ne 0 -or $complete -ne 1) {
    throw "Bounded language-error Automation evidence incomplete: success=$success failed=$failed complete=$complete log=$log"
}
Write-Host "AvidScript.Runtime.BoundedLanguageErrorBuild: 1/1 passed; log=$log"
