param(
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')
)

$ErrorActionPreference = 'Stop'
$pluginRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$checks = 0

function Assert-EnvironmentSafety {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
    $script:checks++
}

function Get-PathSnapshot {
    $snapshot = [ordered]@{ process = $env:PATH }
    foreach ($target in @('User', 'Machine')) {
        $snapshot[$target] = [Environment]::GetEnvironmentVariable('Path', $target)
    }
    return ($snapshot | ConvertTo-Json -Compress)
}

# Cover each isolated CLI entry point, including fixtures that launch the SDK directly.
$scripts = @(Get-ChildItem -Path (Join-Path $pluginRoot 'Build/*.ps1'),
    (Join-Path $pluginRoot 'Build/Contracts/*.ps1'),
    (Join-Path $pluginRoot 'Tools/AvidScript.CSharpFrontend.Tests/*.ps1') -File)
$guarded = 0
foreach ($script in $scripts) {
    if ($script.FullName -eq $PSCommandPath) { continue }
    $source = [IO.File]::ReadAllText($script.FullName)
    $cliHomeAssignment = [regex]::Match($source, '\$env:DOTNET_CLI_HOME\s*=')
    if (-not $cliHomeAssignment.Success) { continue }
    $guard = [regex]::Match($source, '\$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH\s*=\s*[''"]0[''"]')
    Assert-EnvironmentSafety ($guard.Success -and $guard.Index -lt $cliHomeAssignment.Index) "Unprotected CLI HOME: $($script.Name)"
    Assert-EnvironmentSafety ($source -match '\$env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK\s*=\s*[''"]1[''"]') "Unprotected workload initialization: $($script.Name)"
    Assert-EnvironmentSafety ($source -match '\$env:DOTNET_GENERATE_ASPNET_CERTIFICATE\s*=\s*[''"]0[''"]') "Unexpected development certificate initialization: $($script.Name)"
    $guarded++
}
Assert-EnvironmentSafety ($guarded -ge 33) 'The isolated CLI entry point inventory is incomplete.'

foreach ($relative in @('Build/InvokeAvidScriptUiSaveDemo.ps1', 'Build/ReleaseEngineering/AvidScriptCompatibilityDoctor.ps1')) {
    $source = [IO.File]::ReadAllText((Join-Path $pluginRoot $relative))
    Assert-EnvironmentSafety ($source -match 'DOTNET_ADD_GLOBAL_TOOLS_TO_PATH[^\r\n]*[''"]0[''"]') "Unprotected child environment: $relative"
    Assert-EnvironmentSafety ($source -match 'DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK[^\r\n]*[''"]1[''"]') "Unprotected child workload initialization: $relative"
    Assert-EnvironmentSafety ($source -match 'DOTNET_GENERATE_ASPNET_CERTIFICATE[^\r\n]*[''"]0[''"]') "Unexpected child certificate initialization: $relative"
}
foreach ($relative in @('Build/InvokeCSharpFrontend.ps1', 'Build/InvokeCSharpSemantic.ps1',
    'Build/InvokeCSharpGuestCompiler.ps1', 'Build/BuildCSharpScriptTypes.ps1')) {
    $source = [IO.File]::ReadAllText((Join-Path $pluginRoot $relative))
    Assert-EnvironmentSafety ($source -match '--disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false') "Unbounded build: $relative"
}

# Use a fresh first-run home to exercise SDK initialization without changing the caller's environment.
Assert-EnvironmentSafety (Test-Path -LiteralPath $DotNetPath -PathType Leaf) 'The installed dotnet host is missing.'
$before = Get-PathSnapshot
$probeParent = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptDotNetEnvironmentSafety'))
$probeRoot = [IO.Path]::GetFullPath((Join-Path $probeParent ([Guid]::NewGuid().ToString('N'))))
Assert-EnvironmentSafety ($probeRoot.StartsWith($probeParent + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) 'Probe directory escaped its owner root.'
if (Test-Path -LiteralPath $probeParent) {
    Assert-EnvironmentSafety (((Get-Item -LiteralPath $probeParent).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Probe parent is a reparse point.'
}
$process = [Diagnostics.Process]::new()
try {
    [void][IO.Directory]::CreateDirectory($probeRoot)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = [IO.Path]::GetFullPath($DotNetPath)
    $start.WorkingDirectory = $pluginRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment['DOTNET_CLI_HOME'] = $probeRoot
    $start.Environment['DOTNET_ADD_GLOBAL_TOOLS_TO_PATH'] = '0'
    $start.Environment['DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK'] = '1'
    $start.Environment['DOTNET_GENERATE_ASPNET_CERTIFICATE'] = '0'
    $start.Environment['DOTNET_NOLOGO'] = '1'
    $start.Environment['DOTNET_CLI_TELEMETRY_OPTOUT'] = '1'
    $start.Environment['DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE'] = 'true'
    $probeProject = Join-Path $probeRoot 'Probe.csproj'
    $probeConfig = Join-Path $probeRoot 'NuGet.Config'
    [IO.File]::WriteAllText($probeProject, '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>')
    [IO.File]::WriteAllText($probeConfig, '<configuration><packageSources><clear /></packageSources></configuration>')
    foreach ($argument in @('build', $probeProject, '--disable-build-servers', '-m:1', '-nodeReuse:false',
        '-p:UseSharedCompilation=false', "-p:RestoreConfigFile=$probeConfig", '--nologo', '-v', 'quiet')) {
        [void]$start.ArgumentList.Add($argument)
    }
    $process.StartInfo = $start
    Assert-EnvironmentSafety ($process.Start()) 'SDK probe failed to start.'
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw 'The owned SDK probe timed out.'
    }
    Assert-EnvironmentSafety ($process.ExitCode -eq 0) ('SDK build probe failed: ' + $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult())
    Assert-EnvironmentSafety (Test-Path -LiteralPath (Join-Path $probeRoot 'bin/Debug/net8.0/Probe.dll')) 'SDK build probe produced no assembly.'
    Assert-EnvironmentSafety (-not ($stdout.GetAwaiter().GetResult() -match 'Microsoft.NET.Runtime.WebAssembly|MonoAOTCompiler|workload pack|工作负载包')) 'First-run probe performed optional workload maintenance.'
    $start.ArgumentList.Clear()
    [void]$start.ArgumentList.Add('--version')
    $process.Dispose()
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    Assert-EnvironmentSafety ($process.Start()) 'SDK version probe failed to start.'
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw 'The owned SDK version probe timed out.'
    }
    Assert-EnvironmentSafety ($process.ExitCode -eq 0) ('SDK version probe failed: ' + $stderr.GetAwaiter().GetResult())
    $expected = (Get-Content -Raw -LiteralPath (Join-Path $pluginRoot 'global.json') | ConvertFrom-Json).sdk.version
    Assert-EnvironmentSafety ($stdout.GetAwaiter().GetResult().Trim() -ceq $expected) 'SDK probe selected a different version.'
    Assert-EnvironmentSafety ((Get-PathSnapshot) -ceq $before) 'SDK initialization changed PATH.'
}
finally {
    $process.Dispose()
    if (Test-Path -LiteralPath $probeRoot) {
        $resolved = (Resolve-Path -LiteralPath $probeRoot).Path
        $probeItems = @(Get-Item -LiteralPath $probeRoot) + @(Get-ChildItem -LiteralPath $probeRoot -Recurse -Force)
        $reparse = @($probeItems |
            Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 })
        if ($resolved -cne $probeRoot -or $reparse.Count -ne 0) { throw 'Unsafe probe cleanup target; files retained.' }
        Remove-Item -LiteralPath $probeRoot -Recurse -Force
    }
}
Assert-EnvironmentSafety (-not (Test-Path -LiteralPath $probeRoot)) 'Probe directory was retained.'
Write-Host "AvidScript.DotNetEnvironmentSafety: $checks/$checks passed; isolated entry points: $guarded; SDK first-run PATH unchanged; probe cleaned."
