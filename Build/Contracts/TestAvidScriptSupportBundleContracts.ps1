$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ModulePath = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptSupportBundle.ps1'
$SchemaPath = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptSupportBundle.schema.json'
. $ModulePath

$Root = Join-Path 'C:\tmp\AvidScript\P65SupportContracts' (
    "$PID-$([guid]::NewGuid().ToString('N'))")
$Passed = 0
$Total = 4

function Invoke-SupportContract {
    param([string]$Name, [scriptblock]$Body)
    try { & $Body; $script:Passed++ }
    catch { throw "$Name failed: $($_.Exception.Message)" }
}

function New-SupportCompatibilityFixture {
    $Project = 'C:\Users\FixtureUser\Projects\FixtureGame'
    $Plugin = "$Project\Plugins\AvidScript"
    return [pscustomobject][ordered]@{
        schema_version = 1
        format = 'avidscript.compatibility.report'
        result = 'degraded'
        generated_at_utc = '2026-09-06T00:00:00.0000000+00:00'
        context = [pscustomobject][ordered]@{
            plugin_root = $Plugin
            project_root = $Project
            engine_root = 'C:\UnrealEngine'
        }
        identity = [pscustomobject][ordered]@{
            plugin_version = '0.1.0'
            engine_version = '5.8.0'
            dotnet_sdk = '8.0.416'
            powershell = '7.5.4'
            install_release_id = ''
            binding_package_sha256 = 'a' * 64
            generated_type_package_id = 'b' * 64
            wasmtime_toolchain_id = 'fixture'
        }
        summary = [pscustomobject][ordered]@{
            passed = 0
            warning = 1
            blocked = 0
            not_run = 0
            total = 1
        }
        checks = @([pscustomobject][ordered]@{
                id = 'fixture.warning'
                area = 'project'
                status = 'warning'
                severity = 'warning'
                code = 'ASCD3999'
                category = 'degraded'
                expected = 'safe'
                actual = "$Plugin\Saved\report.json"
                message = 'fixture warning'
                remediation = 'inspect C:\Users\FixtureUser\Projects\FixtureGame'
            })
    }
}

[System.IO.Directory]::CreateDirectory($Root) | Out-Null
try {
    Invoke-SupportContract 'redacted deterministic publication' {
        $Log = Join-Path $Root 'input.log'
        [System.IO.File]::WriteAllText(
            $Log,
            (([string][char]0x4E2D) * 40000) + "`n" +
            "C:\Users\FixtureUser\Projects\FixtureGame\Saved\game.log`n" +
            "Authorization: Bearer abcdefghijklmnopqrstuvwxyz`n" +
            "email=fixture@example.com`n",
            [System.Text.UTF8Encoding]::new($false))
        $Report = New-SupportCompatibilityFixture
        $First = Publish-AvidScriptSupportBundle $Report (Join-Path $Root 'output') @($Log)
        $Second = Publish-AvidScriptSupportBundle $Report (Join-Path $Root 'output') @($Log)
        $SafeText = Get-Content -LiteralPath (
            Join-Path $First.Root 'files/compatibility.json') -Raw
        $LogText = Get-Content -LiteralPath (Join-Path $First.Root 'files/logs/log-01.txt') -Raw
        if ($First.Manifest.bundle_id -cne $Second.Manifest.bundle_id -or
            $SafeText.Contains('FixtureUser') -or $SafeText.Contains('C:\Users\') -or
            $LogText.Contains('abcdefghijklmnopqrstuvwxyz') -or
            $LogText.Contains('fixture@example.com') -or
            ([System.Text.UTF8Encoding]::new($false).GetByteCount($LogText)) -gt 65536 -or
            -not $SafeText.Contains('<PLUGIN>') -or
            -not $LogText.Contains('<REDACTED>')) {
            throw 'support publication was not deterministic and fully redacted.'
        }
    }

    Invoke-SupportContract 'schema inventory readback' {
        $Report = New-SupportCompatibilityFixture
        $Bundle = Publish-AvidScriptSupportBundle $Report (Join-Path $Root 'schema-output')
        $Resolved = Resolve-AvidScriptSupportBundle $Bundle.Root ([pscustomobject]@{
                PluginRoot = $Report.context.plugin_root
                ProjectRoot = $Report.context.project_root
                EngineRoot = $Report.context.engine_root
            })
        if (-not ((Get-Content -LiteralPath $Resolved.ManifestPath -Raw) |
                Test-Json -SchemaFile $SchemaPath) -or
            $Resolved.Manifest.payload.file_count -ne 1) {
            throw 'support bundle schema or inventory readback failed.'
        }
    }

    Invoke-SupportContract 'tamper rejection' {
        $Report = New-SupportCompatibilityFixture
        $Bundle = Publish-AvidScriptSupportBundle $Report (Join-Path $Root 'tamper-output')
        [System.IO.File]::AppendAllText(
            (Join-Path $Bundle.Root 'files/compatibility.json'),
            'tampered')
        $Rejected = $false
        try {
            Resolve-AvidScriptSupportBundle $Bundle.Root ([pscustomobject]@{
                    PluginRoot = $Report.context.plugin_root
                    ProjectRoot = $Report.context.project_root
                    EngineRoot = $Report.context.engine_root
                }) | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'inventory_invalid'
        }
        if (-not $Rejected) { throw 'tampered support payload was accepted.' }
    }

    Invoke-SupportContract 'source file rejection' {
        $Source = Join-Path $Root 'Gameplay.cs'
        [System.IO.File]::WriteAllText($Source, 'public class Secret {}')
        $Rejected = $false
        try {
            Publish-AvidScriptSupportBundle `
                (New-SupportCompatibilityFixture) `
                (Join-Path $Root 'source-output') `
                @($Source) | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'log_invalid'
        }
        if (-not $Rejected) { throw 'source file was accepted as a support log.' }
    }
}
finally {
    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $AllowedRoot = [System.IO.Path]::GetFullPath('C:\tmp\AvidScript\P65SupportContracts').TrimEnd('\') + '\'
    if ($ResolvedRoot.StartsWith($AllowedRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $ResolvedRoot)) {
        Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
    }
}

if ($Passed -ne $Total) {
    throw "AvidScript support bundle contracts: $Passed/$Total passed"
}
Write-Output "AvidScript support bundle contracts: $Passed/$Total passed"
