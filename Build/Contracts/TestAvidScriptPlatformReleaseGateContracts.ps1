$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$PluginRoot = Split-Path -Parent $BuildRoot
$GateModule = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPlatformReleaseGate.ps1'
$GateCli = Join-Path $BuildRoot 'InvokeAvidScriptPlatformReleaseGate.ps1'
$StepScript = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPlatformReleaseGateStep.ps1'
$PlanSchema = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPlatformReleaseGatePlan.schema.json'
$ReportSchema = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPlatformReleaseGateReport.schema.json'
. $GateModule

$Root = Join-Path 'C:\tmp\AvidScript\P65PlatformGateContracts' (
    "$PID-$([guid]::NewGuid().ToString('N'))")
$Passed = 0
$Total = 8

function Invoke-PlatformGateContract {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    try {
        & $Body
        $script:Passed++
    }
    catch {
        throw "$Name failed: $($_.Exception.Message)"
    }
}

function Write-PlatformGateContractJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    Write-AvidScriptPluginReleaseJson $Path $Value
}

function New-PlatformGateContractPackage {
    param([Parameter(Mandatory = $true)][string]$Name)

    $FixtureRoot = Join-Path $Root $Name
    $PayloadRoot = Join-Path $FixtureRoot 'input'
    $PayloadPlugin = Join-Path $PayloadRoot 'AvidScript'
    $BuildFixture = Join-Path $PayloadPlugin 'Build'
    $ThirdParty = Join-Path $PayloadPlugin 'Source/ThirdParty/Fixture'
    [System.IO.Directory]::CreateDirectory($BuildFixture) | Out-Null
    [System.IO.Directory]::CreateDirectory($ThirdParty) | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $PayloadPlugin 'AvidScript.uplugin'),
        '{"FileVersion":3,"VersionName":"0.1.0"}')
    [System.IO.File]::WriteAllText((Join-Path $PayloadPlugin 'LICENSE'), 'MIT')
    $GeneratedProducer = Join-Path $BuildFixture 'AvidScriptGeneratedTypeCookPackage.ps1'
    $ModuleProducer = Join-Path $BuildFixture 'AvidScriptModuleReleasePackage.ps1'
    [System.IO.File]::Copy(
        (Join-Path $BuildRoot 'AvidScriptGeneratedTypeCookPackage.ps1'),
        $GeneratedProducer)
    [System.IO.File]::Copy(
        (Join-Path $BuildRoot 'AvidScriptModuleReleasePackage.ps1'),
        $ModuleProducer)
    $DependencyPath = Join-Path $ThirdParty 'dependency.lock.json'
    [System.IO.File]::WriteAllText($DependencyPath, '{"version":"fixture"}')
    $Artifacts = @(
        [pscustomobject][ordered]@{
            id = 'generated-type-package'
            producer_path = 'AvidScript/Build/AvidScriptGeneratedTypeCookPackage.ps1'
            producer_sha256 = Get-AvidScriptPluginReleaseSha256 $GeneratedProducer
        },
        [pscustomobject][ordered]@{
            id = 'module-release-package'
            producer_path = 'AvidScript/Build/AvidScriptModuleReleasePackage.ps1'
            producer_sha256 = Get-AvidScriptPluginReleaseSha256 $ModuleProducer
        })
    $Dependencies = @(
        [pscustomobject][ordered]@{
            id = 'fixture-runtime'
            version = '1.0.0'
            mode = 'external'
            identity_path = 'AvidScript/Source/ThirdParty/Fixture/dependency.lock.json'
            identity_sha256 = Get-AvidScriptPluginReleaseSha256 $DependencyPath
        })
    return Publish-AvidScriptPluginReleasePackage `
        -PayloadSourceRoot $PayloadRoot `
        -OutputRoot (Join-Path $FixtureRoot 'release') `
        -Version '0.1.0' `
        -Commit ('a' * 40) `
        -Tree ('b' * 40) `
        -CommittedAtUtc '2026-09-06T00:00:00.0000000+00:00' `
        -ArtifactContracts $Artifacts `
        -Dependencies $Dependencies
}

function New-PlatformGateContractProject {
    param([Parameter(Mandatory = $true)][string]$Name)

    $ProjectRoot = Join-Path $Root $Name
    [System.IO.Directory]::CreateDirectory($ProjectRoot) | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $ProjectRoot "$Name.uproject"), '{}')
    return $ProjectRoot
}

function New-BlockedAndroidToolchainScript {
    $Path = Join-Path $Root 'BlockedAndroidToolchain.ps1'
    $Json = @{
        schema_version = 1
        status = 'blocked'
        ready = $false
        result = 'avidscript_android_toolchain_blocked'
        checks = @(
            @{ id = 'engine'; status = 'ok' },
            @{ id = 'sdk_root'; status = 'blocked' })
        toolchain = @{ sdk_root = ''; ndk_root = ''; java_home = ''; adb_path = '' }
        requirements = @{ platform = 'android-34' }
    } | ConvertTo-Json -Depth 8 -Compress
    [System.IO.File]::WriteAllText(
        $Path,
        "[Console]::Out.WriteLine('$($Json.Replace("'", "''"))')`nexit 2`n",
        [System.Text.UTF8Encoding]::new($false))
    return $Path
}

[System.IO.Directory]::CreateDirectory($Root) | Out-Null
try {
    Invoke-PlatformGateContract 'required scripts parse' {
        foreach ($Path in @($GateModule, $GateCli, $StepScript)) {
            $Tokens = $null
            $Errors = $null
            [void][System.Management.Automation.Language.Parser]::ParseFile(
                $Path,
                [ref]$Tokens,
                [ref]$Errors)
            if ($Errors.Count -ne 0) {
                throw "$Path contains PowerShell parse errors."
            }
        }
    }

    Invoke-PlatformGateContract 'disabled plan schema' {
        $PlanPath = Join-Path $Root 'disabled-plan.json'
        Write-PlatformGateContractJson $PlanPath ([ordered]@{
                schema_version = 1
                win64 = [ordered]@{ enabled = $false }
                android = [ordered]@{
                    enabled = $false
                    device = [ordered]@{ enabled = $false }
                }
            })
        if (-not ((Get-Content -LiteralPath $PlanPath -Raw) |
                Test-Json -SchemaFile $PlanSchema)) {
            throw 'disabled plan did not satisfy the public schema.'
        }
        $Plan = Get-AvidScriptPlatformReleasePlan $PlanPath
        if ($Plan.win64.enabled -or $Plan.android.enabled) {
            throw 'disabled plan changed execution intent.'
        }
    }

    Invoke-PlatformGateContract 'enabled plan semantic rejection' {
        $PlanPath = Join-Path $Root 'incomplete-plan.json'
        Write-PlatformGateContractJson $PlanPath ([ordered]@{
                schema_version = 1
                win64 = [ordered]@{ enabled = $true }
                android = [ordered]@{
                    enabled = $false
                    device = [ordered]@{ enabled = $false }
                }
            })
        $Rejected = $false
        try {
            Get-AvidScriptPlatformReleasePlan $PlanPath | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'plan_invalid'
        }
        if (-not $Rejected) {
            throw 'enabled plan accepted missing build inputs.'
        }
    }

    Invoke-PlatformGateContract 'strict child output' {
        $Rejected = $false
        try {
            ConvertFrom-AvidScriptPlatformReleaseChildJson "{}`n{}" 'fixture' | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'child_output_invalid'
        }
        if (-not $Rejected) {
            throw 'multiple JSON lines were accepted from a child process.'
        }
    }

    Invoke-PlatformGateContract 'empty optional paths and child stderr diagnostics' {
        $StepSource = [System.IO.File]::ReadAllText($StepScript)
        $GateSource = [System.IO.File]::ReadAllText($GateModule)
        foreach ($RequiredToken in @(
                '$StepInput = Get-Content',
                "if (-not [string]::IsNullOrWhiteSpace(`$OptionalPath.Value))",
                "`$Parameters[`$OptionalPath.Name] = `$OptionalPath.Value")) {
            if (-not $StepSource.Contains($RequiredToken)) {
                throw "Win64 step does not omit empty optional paths: $RequiredToken"
            }
        }
        if ($StepSource.Contains('$Input = Get-Content')) {
            throw 'Step input shadows the PowerShell automatic $input enumerator.'
        }
        if ($GateSource.Contains('[Parameter(Mandatory = $true)]$Input')) {
            throw 'Gate step parameter shadows the PowerShell automatic $input enumerator.'
        }
        $ChildPath = Join-Path $Root 'stderr-only-child.ps1'
        [System.IO.File]::WriteAllText(
            $ChildPath,
            "[Console]::Error.WriteLine('fixture-child-error')`nexit 1`n",
            [System.Text.UTF8Encoding]::new($false))
        $Rejected = $false
        try {
            Invoke-AvidScriptPlatformReleaseScriptJson `
                -ScriptPath $ChildPath `
                -WorkingDirectory $Root `
                -TimeoutSeconds 10 | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'child_output_invalid' -and
                $_.Exception.Message.Contains('fixture-child-error')
        }
        if (-not $Rejected) {
            throw 'stderr-only child failure was not preserved by the Gate.'
        }
    }

    Invoke-PlatformGateContract 'inspect report and honest not-run layers' {
        $Package = New-PlatformGateContractPackage 'InspectPackage'
        $Project = New-PlatformGateContractProject 'InspectProject'
        $ToolchainScript = New-BlockedAndroidToolchainScript
        $Report = Invoke-AvidScriptPlatformReleaseGate `
            -PackageRoot $Package.Root `
            -Mode Inspect `
            -ProjectRoot $Project `
            -EngineRoot 'C:\UnrealEngine' `
            -DotNetPath 'dotnet' `
            -ToolchainScriptPath $ToolchainScript
        $ReportPath = Join-Path $Project $Report.report_file
        $SchemaValid = (Get-Content -LiteralPath $ReportPath -Raw) |
            Test-Json -SchemaFile $ReportSchema
        $ExpectedLayerIds = @(
            'release-package', 'project-artifacts', 'win64-shipping',
            'android-toolchain', 'android-arm64', 'android-package',
            'android-device', 'shipping-manual')
        if (-not $SchemaValid -or $Report.result -cne 'partial' -or
            [string]::Join('|', @($Report.layers.id)) -cne
            [string]::Join('|', $ExpectedLayerIds) -or
            $Report.summary.passed -ne 1 -or $Report.summary.blocked -ne 1 -or
            $Report.summary.not_run -ne 6) {
            throw 'Inspect report did not preserve the expected layered state.'
        }
    }

    Invoke-PlatformGateContract 'tampered release fails closed' {
        $Package = New-PlatformGateContractPackage 'TamperPackage'
        $Project = New-PlatformGateContractProject 'TamperProject'
        [System.IO.File]::AppendAllText(
            (Join-Path $Package.PluginRoot 'Build/AvidScriptModuleReleasePackage.ps1'),
            '# tampered')
        $Report = Invoke-AvidScriptPlatformReleaseGate `
            -PackageRoot $Package.Root `
            -Mode Inspect `
            -ProjectRoot $Project `
            -EngineRoot 'C:\UnrealEngine' `
            -DotNetPath 'dotnet' `
            -ToolchainScriptPath (New-BlockedAndroidToolchainScript)
        if ($Report.result -cne 'failed' -or
            $Report.layers[0].status -cne 'failed' -or
            $Report.summary.failed -ne 1) {
            throw 'tampered release did not fail the release layer.'
        }
    }

    Invoke-PlatformGateContract 'report path escape rejection' {
        $Rejected = $false
        try {
            $Project = New-PlatformGateContractProject 'EscapeProject'
            Write-AvidScriptPlatformReleaseGateReport `
                -Report ([pscustomobject]@{}) `
                -ReportPath (Join-Path $Root 'escaped.json') `
                -ProjectRoot $Project
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'report_path_invalid'
        }
        if (-not $Rejected) {
            throw 'report path escaped the controlled project evidence root.'
        }
    }
}
finally {
    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $AllowedRoot = [System.IO.Path]::GetFullPath(
        'C:\tmp\AvidScript\P65PlatformGateContracts').TrimEnd('\') + '\'
    if ($ResolvedRoot.StartsWith(
            $AllowedRoot,
            [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $ResolvedRoot)) {
        Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
    }
}

if ($Passed -ne $Total) {
    throw "AvidScript platform release gate contracts: $Passed/$Total passed"
}
Write-Output "AvidScript platform release gate contracts: $Passed/$Total passed"
