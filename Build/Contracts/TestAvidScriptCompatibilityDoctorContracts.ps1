$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ModulePath = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptCompatibilityDoctor.ps1'
$SchemaPath = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptCompatibilityReport.schema.json'
$CliPath = Join-Path $BuildRoot 'InvokeAvidScriptCompatibilityDoctor.ps1'
. $ModulePath

$Root = Join-Path 'C:\tmp\AvidScript\P65DoctorContracts' (
    "$PID-$([guid]::NewGuid().ToString('N'))")
$Passed = 0
$Total = 5

function Invoke-DoctorContract {
    param([string]$Name, [scriptblock]$Body)

    try {
        & $Body
        $script:Passed++
    }
    catch {
        throw "$Name failed: $($_.Exception.Message)"
    }
}

function Write-DoctorFixtureFile {
    param([string]$Path, [string]$Text)

    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function New-DoctorFixture {
    param([string]$Name, [int]$EngineMinor = 8)

    $Project = Join-Path $Root $Name
    $Plugin = Join-Path $Project 'Plugins/AvidScript'
    $Engine = Join-Path $Project 'EngineRoot'
    Write-DoctorFixtureFile (Join-Path $Project "$Name.uproject") '{"FileVersion":3}'
    Write-DoctorFixtureFile (Join-Path $Project "Source/$Name.Target.cs") 'public class GameTarget {}'
    Write-DoctorFixtureFile (Join-Path $Project "Source/$Name`Editor.Target.cs") 'public class EditorTarget {}'
    Write-DoctorFixtureFile (Join-Path $Plugin 'AvidScript.uplugin') @'
{"FileVersion":3,"VersionName":"0.1.0","Modules":[{"Name":"AvidScriptBindings"},{"Name":"AvidScriptCore"},{"Name":"AvidScriptEditor"},{"Name":"AvidScriptGenerated"},{"Name":"AvidScriptRuntime"},{"Name":"AvidScriptVM"}]}
'@
    Write-DoctorFixtureFile (Join-Path $Plugin 'global.json') '{"sdk":{"version":"8.0.416"}}'
    Write-DoctorFixtureFile (Join-Path $Engine 'Engine/Build/Build.version') `
        "{`"MajorVersion`":5,`"MinorVersion`":$EngineMinor,`"PatchVersion`":0}"
    [System.IO.Directory]::CreateDirectory(
        (Join-Path $Engine 'Engine/Source/Programs/UnrealBuildTool')) | Out-Null
    return [pscustomobject]@{ Project = $Project; Plugin = $Plugin; Engine = $Engine }
}

[System.IO.Directory]::CreateDirectory($Root) | Out-Null
try {
    Invoke-DoctorContract 'stable schema and summary' {
        $Fixture = New-DoctorFixture 'Stable'
        $Report = Get-AvidScriptCompatibilityDoctorReport `
            $Fixture.Plugin $Fixture.Project $Fixture.Engine 'C:\missing\dotnet.exe' `
            -SkipExternalTools
        Assert-AvidScriptCompatibilityDoctorReport $Report
        $Path = Join-Path $Root 'stable.json'
        Write-AvidScriptPluginReleaseJson $Path $Report
        if (-not ((Get-Content -LiteralPath $Path -Raw) | Test-Json -SchemaFile $SchemaPath) -or
            $Report.summary.blocked -ne 0 -or $Report.summary.total -ne 22) {
            throw 'valid static fixture did not produce a schema-valid non-blocked report.'
        }
    }

    Invoke-DoctorContract 'engine mismatch blocks' {
        $Fixture = New-DoctorFixture 'EngineMismatch' 7
        $Report = Get-AvidScriptCompatibilityDoctorReport `
            $Fixture.Plugin $Fixture.Project $Fixture.Engine 'C:\missing\dotnet.exe' `
            -SkipExternalTools
        $Check = @($Report.checks | Where-Object id -eq 'host.engine.version')
        if ($Report.result -cne 'blocked' -or $Check.Count -ne 1 -or
            $Check[0].code -cne 'ASCD1003') {
            throw 'UE version mismatch did not produce the stable blocked diagnostic.'
        }
    }

    Invoke-DoctorContract 'external tools are explicit not-run' {
        $Fixture = New-DoctorFixture 'NoExternal'
        $Report = Get-AvidScriptCompatibilityDoctorReport `
            $Fixture.Plugin $Fixture.Project $Fixture.Engine 'C:\missing\dotnet.exe' `
            -SkipExternalTools
        $Ids = @($Report.checks | Where-Object status -eq 'not_run' | ForEach-Object id)
        foreach ($Expected in @(
                'host.dotnet.version', 'host.visual_studio',
                'plugin.wasmtime.performance', 'platform.android.toolchain')) {
            if ($Ids -cnotcontains $Expected) {
                throw "external probe did not preserve not_run: $Expected"
            }
        }
    }

    Invoke-DoctorContract 'managed receipt validates installed files' {
        $Fixture = New-DoctorFixture 'ManagedReceipt'
        $Files = @(Get-AvidScriptInstalledPluginInventory $Fixture.Plugin)
        $InventorySha = Get-AvidScriptPluginReleaseInventorySha256 $Files
        Write-AvidScriptPluginReleaseJson `
            (Join-Path $Fixture.Plugin '.avidscript-install.json') `
            ([pscustomobject][ordered]@{
                schema_version = 1
                format = 'avidscript.plugin.install'
                release_id = 'a' * 64
                version = '0.1.0'
                manifest_sha256 = 'b' * 64
                inventory_sha256 = $InventorySha
                source_commit = 'c' * 40
                source_tree = 'd' * 40
                installed_at_utc = '2026-09-06T00:00:00.0000000+00:00'
                files = $Files
            })
        $Valid = Get-AvidScriptCompatibilityDoctorReport `
            $Fixture.Plugin $Fixture.Project $Fixture.Engine 'C:\missing\dotnet.exe' `
            -SkipExternalTools
        $ValidReceipt = @($Valid.checks | Where-Object id -eq 'plugin.install_receipt')
        if ($ValidReceipt.Count -ne 1 -or $ValidReceipt[0].status -cne 'passed') {
            throw 'managed receipt did not validate its installed tree.'
        }
        [System.IO.File]::AppendAllText((Join-Path $Fixture.Plugin 'global.json'), 'damage')
        $Damaged = Get-AvidScriptCompatibilityDoctorReport `
            $Fixture.Plugin $Fixture.Project $Fixture.Engine 'C:\missing\dotnet.exe' `
            -SkipExternalTools
        $DamagedReceipt = @($Damaged.checks | Where-Object id -eq 'plugin.install_receipt')
        if ($DamagedReceipt.Count -ne 1 -or $DamagedReceipt[0].status -cne 'blocked') {
            throw 'managed receipt accepted installed file drift.'
        }
    }

    Invoke-DoctorContract 'public CLI report' {
        $Fixture = New-DoctorFixture 'PublicCli'
        $ReportPath = Join-Path $Root 'public-cli.json'
        $PowerShellPath = (Get-Process -Id $PID).Path
        $Output = & $PowerShellPath -NoProfile -File $CliPath `
            -ProjectRoot $Fixture.Project `
            -PluginRoot $Fixture.Plugin `
            -EngineRoot $Fixture.Engine `
            -DotNetPath 'C:\missing\dotnet.exe' `
            -ReportPath $ReportPath `
            -SkipExternalTools
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
            throw 'public Doctor CLI did not produce its report.'
        }
        $StdoutReport = $Output | ConvertFrom-Json -Depth 32 -DateKind String
        $FileReport = Read-AvidScriptPluginReleaseJsonObject $ReportPath 'Doctor report'
        if ($StdoutReport.format -cne 'avidscript.compatibility.report' -or
            $FileReport.summary.total -ne 22) {
            throw 'public Doctor CLI output differs from its report contract.'
        }
    }
}
finally {
    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $AllowedRoot = [System.IO.Path]::GetFullPath('C:\tmp\AvidScript\P65DoctorContracts').TrimEnd('\') + '\'
    if ($ResolvedRoot.StartsWith($AllowedRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $ResolvedRoot)) {
        Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
    }
}

if ($Passed -ne $Total) {
    throw "AvidScript Compatibility Doctor contracts: $Passed/$Total passed"
}
Write-Output "AvidScript Compatibility Doctor contracts: $Passed/$Total passed"
