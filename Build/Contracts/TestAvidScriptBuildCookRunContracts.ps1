Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$RunnerPath = Join-Path $BuildRoot 'InvokeAvidScriptBuildCookRun.ps1'
$FixtureRoot = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ("AvidScriptBuildCookRunContract_$PID`_$([Guid]::NewGuid().ToString('N'))")
$Utf8 = [System.Text.UTF8Encoding]::new($false)
$Passed = 0
$Total = 0
$Failures = [System.Collections.Generic.List[string]]::new()

function Assert-BuildCookRunContract {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-BuildCookRunContractCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    ++$script:Total
    try {
        & $Body
        ++$script:Passed
    }
    catch {
        $script:Failures.Add("$Name`: $($_.Exception.Message)")
    }
}

function Get-BuildCookRunFunctionAst {
    param(
        [Parameter(Mandatory = $true)]$Ast,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $Functions = @($Ast.FindAll({
                param($Node)
                $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                    $Node.Name -ceq $Name
            }, $true))
    if ($Functions.Count -ne 1) {
        throw "Expected exactly one function named $Name."
    }
    return $Functions[0]
}

function Write-BuildCookRunReceiptFixture {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string]$TargetName,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][datetime]$LastWriteTimeUtc
    )

    $ReceiptRoot = Join-Path $ProjectRoot 'Binaries/Win64'
    [void][System.IO.Directory]::CreateDirectory($ReceiptRoot)
    $ReceiptPath = Join-Path $ReceiptRoot $FileName
    $ExecutableName = if ($Configuration -ceq 'Shipping') {
        "$TargetName-Win64-Shipping.exe"
    }
    else {
        "$TargetName.exe"
    }
    [System.IO.File]::WriteAllBytes(
        (Join-Path $ReceiptRoot $ExecutableName),
        [byte[]](0x4d, 0x5a))
    $Receipt = [pscustomobject][ordered]@{
        TargetName = $TargetName
        Platform = 'Win64'
        Configuration = $Configuration
        TargetType = 'Game'
        BuildProducts = @(
            [pscustomobject][ordered]@{
                Path = "`$(ProjectDir)/Binaries/Win64/$ExecutableName"
                Type = 'Executable'
            })
        RuntimeDependencies = @()
    }
    [System.IO.File]::WriteAllText(
        $ReceiptPath,
        ($Receipt | ConvertTo-Json -Depth 4 -Compress),
        $Utf8)
    (Get-Item -LiteralPath $ReceiptPath).LastWriteTimeUtc = $LastWriteTimeUtc
    return $ReceiptPath
}

try {
    if (-not (Test-Path -LiteralPath $RunnerPath -PathType Leaf)) {
        throw "BuildCookRun runner is missing: $RunnerPath"
    }
    [void][System.IO.Directory]::CreateDirectory($FixtureRoot)
    $Tokens = $null
    $ParseErrors = $null
    $RunnerAst = [System.Management.Automation.Language.Parser]::ParseFile(
        $RunnerPath,
        [ref]$Tokens,
        [ref]$ParseErrors)
    if ($ParseErrors.Count -ne 0) {
        throw "Runner contains PowerShell parse errors: $($ParseErrors.Message -join '; ')"
    }
    $RunnerSource = [System.IO.File]::ReadAllText($RunnerPath)
    $PackagedOraclePath = Join-Path $BuildRoot 'InvokeAvidScriptPackagedOracle.ps1'
    if (-not (Test-Path -LiteralPath $PackagedOraclePath -PathType Leaf)) {
        throw "Packaged oracle runner is missing: $PackagedOraclePath"
    }
    $PackagedOracleSource = [System.IO.File]::ReadAllText($PackagedOraclePath)
    . $RunnerPath `
        -SourcePath 'contract-source' `
        -CSharpProjectPath 'contract-project' `
        -ModuleId 'contract.module' `
        -ArtifactStem 'contract_artifact' `
        -OutputRoot 'contract-output' `
        -DotNetPath 'contract-dotnet' `
        -ArchiveRoot 'contract-archive'

    Invoke-BuildCookRunContractCase 'parameter schema' {
        $ExpectedParameters = @(
            'ArchiveRoot',
            'ArtifactStem',
            'BindingPackagePath',
            'Configuration',
            'CSharpProjectPath',
            'DotNetPath',
            'EngineRoot',
            'GeneratedTypeManifestPath',
            'ModuleId',
            'OutputRoot',
            'PackagedOracleTimeoutSeconds',
            'RuntimeBindingPackagePath',
            'SourcePath')
        $ActualParameters = @($RunnerAst.ParamBlock.Parameters |
                ForEach-Object { $_.Name.VariablePath.UserPath } |
                Sort-Object)
        Assert-BuildCookRunContract `
            ([string]::Join('|', $ActualParameters) -ceq
                [string]::Join('|', $ExpectedParameters)) `
            'Runner exposes an unexpected parameter set.'
        foreach ($RequiredToken in @(
                "[ValidateSet('Development', 'Shipping')]",
                "[string]`$Configuration = 'Development'",
                "[string]`$EngineRoot = 'C:\UnrealEngine'",
                "[string]`$GeneratedTypeManifestPath = ''",
                '[Parameter(Mandatory = $true)][string]$ArchiveRoot')) {
            Assert-BuildCookRunContract `
                ($RunnerSource.Contains($RequiredToken)) `
                "Parameter schema token is missing: $RequiredToken"
        }
    }

    Invoke-BuildCookRunContractCase 'fixed UE5.8 source engine' {
        $EngineFunction = Get-BuildCookRunFunctionAst `
            -Ast $RunnerAst `
            -Name 'Get-AvidScriptBuildCookRunEngineContext'
        $EngineText = $EngineFunction.Extent.Text
        foreach ($RequiredToken in @(
                "[System.IO.Path]::GetFullPath('C:\UnrealEngine')",
                "'Engine/Build/Build.version'",
                "'Engine/Source'",
                "'Engine/Build/BatchFiles/RunUAT.bat'",
                '$MajorVersion -ne 5',
                '$MinorVersion -ne 8')) {
            Assert-BuildCookRunContract `
                ($EngineText.Contains($RequiredToken)) `
                "Engine validation token is missing: $RequiredToken"
        }
    }

    Invoke-BuildCookRunContractCase 'release before UAT before receipt before oracle' {
        $InvokeFunction = Get-BuildCookRunFunctionAst `
            -Ast $RunnerAst `
            -Name 'Invoke-AvidScriptBuildCookRun'
        $InvokeText = $InvokeFunction.Extent.Text
        $ReleaseIndex = $InvokeText.IndexOf(
            'Invoke-AvidScriptBuildCookRunReleaseStep',
            [System.StringComparison]::Ordinal)
        $UatIndex = $InvokeText.IndexOf(
            'Invoke-AvidScriptBuildCookRunUatStep',
            [System.StringComparison]::Ordinal)
        $ReceiptIndex = $InvokeText.IndexOf(
            'Invoke-AvidScriptBuildCookRunReceiptStep',
            [System.StringComparison]::Ordinal)
        $OracleIndex = $InvokeText.IndexOf(
            'Invoke-AvidScriptBuildCookRunOracleStep',
            [System.StringComparison]::Ordinal)
        Assert-BuildCookRunContract `
            ($ReleaseIndex -ge 0 -and
                $UatIndex -gt $ReleaseIndex -and
                $ReceiptIndex -gt $UatIndex -and
                $OracleIndex -gt $ReceiptIndex) `
            'Orchestration order is not release -> UAT -> receipt validator -> packaged oracle.'
    }

    Invoke-BuildCookRunContractCase 'Development UAT no-clean Zen contract' {
        $Arguments = @(New-AvidScriptBuildCookRunUatArguments `
                -ProjectFile 'C:\Project With Space\Game.uproject' `
                -TargetName 'Game' `
                -Configuration Development `
                -ArchiveRoot 'C:\Project With Space\Saved\Archive')
        foreach ($Expected in @(
                'BuildCookRun',
                '-target=Game',
                '-targetplatform=Win64',
                '-clientconfig=Development',
                '-build',
                '-skipbuildeditor',
                '-cook',
                '-stage',
                '-pak',
                '-archive',
                '-AdditionalCookerOptions=-SkipZenStore -AvidScriptSuppressGeneratedTypeExecution')) {
            Assert-BuildCookRunContract `
                ($Arguments -ccontains $Expected) `
                "Development UAT argument is missing: $Expected"
        }
        Assert-BuildCookRunContract `
            (@($Arguments | Where-Object { $_ -imatch '^-clean(?:$|=)' }).Count -eq 0) `
            'UAT arguments contain a forbidden clean switch.'
        Assert-BuildCookRunContract `
            (@($Arguments | Where-Object {
                        $_ -imatch '^-target=.*Editor' -or
                            $_ -imatch '^-clean(?:$|=)'
                    }).Count -eq 0) `
            'UAT arguments select, clean, or clear an Editor target.'
    }

    Invoke-BuildCookRunContractCase 'Shipping UAT contract' {
        $Arguments = @(New-AvidScriptBuildCookRunUatArguments `
                -ProjectFile 'C:\Project\Game.uproject' `
                -TargetName 'Game' `
                -Configuration Shipping `
                -ArchiveRoot 'C:\Project\Saved\ShippingArchive')
        Assert-BuildCookRunContract `
            ($Arguments -ccontains '-clientconfig=Shipping') `
            'Shipping is not forwarded to BuildCookRun.'
        Assert-BuildCookRunContract `
            ($Arguments -ccontains '-AdditionalCookerOptions=-SkipZenStore -AvidScriptSuppressGeneratedTypeExecution') `
            'Shipping does not retain the Zen workaround.'
        Assert-BuildCookRunContract `
            ($Arguments -ccontains '-skipbuildeditor') `
            'Shipping unexpectedly rebuilds the Editor target.'
    }

    Invoke-BuildCookRunContractCase 'ProcessStartInfo and JSON output' {
        foreach ($RequiredToken in @(
                '[System.Diagnostics.ProcessStartInfo]::new()',
                'ArgumentList.Add($Argument)',
                "[Guid]::NewGuid().ToString('N')",
                'OutputLogPath',
                'avidscript_build_cook_run_succeeded',
                'avidscript_build_cook_run_failed',
                'ConvertTo-Json -Depth 32 -Compress',
                '[Console]::Out.WriteLine')) {
            Assert-BuildCookRunContract `
                ($RunnerSource.Contains($RequiredToken)) `
                "Runner output/process token is missing: $RequiredToken"
        }
        Assert-BuildCookRunContract `
            (-not $RunnerSource.Contains('Invoke-Expression')) `
            'Runner uses Invoke-Expression.'
        $Payload = ConvertFrom-AvidScriptBuildCookRunJsonText `
            -Text '{"schema_version":1,"result":"ok"}' `
            -Label 'contract JSON' `
            -RequireSingleLine
        Assert-BuildCookRunContract `
            ([long]$Payload.schema_version -eq 1) `
            'Single-line JSON was not parsed.'
        foreach ($InvalidJson in @(
                "{`n`"result`":`"pretty`"`n}",
                '{"result":"one","result":"two"}')) {
            $Rejected = $false
            try {
                ConvertFrom-AvidScriptBuildCookRunJsonText `
                    -Text $InvalidJson `
                    -Label 'invalid contract JSON' `
                    -RequireSingleLine | Out-Null
            }
            catch {
                $Rejected = $true
            }
            Assert-BuildCookRunContract $Rejected 'Non-strict JSON output was accepted.'
        }
    }

    Invoke-BuildCookRunContractCase 'ArchiveRoot containment and emptiness' {
        $ProjectRoot = Join-Path $FixtureRoot 'ArchiveProject'
        $EmptyArchive = Join-Path $ProjectRoot 'Saved/EmptyArchive'
        $MissingArchive = Join-Path $ProjectRoot 'Saved/MissingArchive'
        $NonEmptyArchive = Join-Path $ProjectRoot 'Saved/NonEmptyArchive'
        $OutsideArchive = Join-Path $FixtureRoot 'OutsideArchive'
        foreach ($Directory in @($ProjectRoot, $EmptyArchive, $NonEmptyArchive)) {
            [void][System.IO.Directory]::CreateDirectory($Directory)
        }
        [System.IO.File]::WriteAllText(
            (Join-Path $NonEmptyArchive 'keep.txt'),
            'owned',
            $Utf8)
        Assert-BuildCookRunContract `
            ((Resolve-AvidScriptBuildCookRunArchiveRoot `
                    -Path $EmptyArchive `
                    -ProjectRoot $ProjectRoot) -ceq
                [System.IO.Path]::GetFullPath($EmptyArchive)) `
            'Empty ArchiveRoot was rejected.'
        Assert-BuildCookRunContract `
            ((Resolve-AvidScriptBuildCookRunArchiveRoot `
                    -Path $MissingArchive `
                    -ProjectRoot $ProjectRoot) -ceq
                [System.IO.Path]::GetFullPath($MissingArchive)) `
            'Missing ArchiveRoot was rejected.'
        foreach ($RejectedPath in @($NonEmptyArchive, $OutsideArchive, $ProjectRoot)) {
            $Rejected = $false
            try {
                Resolve-AvidScriptBuildCookRunArchiveRoot `
                    -Path $RejectedPath `
                    -ProjectRoot $ProjectRoot | Out-Null
            }
            catch {
                $Rejected = $true
            }
            Assert-BuildCookRunContract $Rejected "Unsafe ArchiveRoot was accepted: $RejectedPath"
        }
        Assert-BuildCookRunContract `
            (-not $RunnerSource.Contains('Remove-Item')) `
            'Runner contains a destructive removal path.'
    }

    Invoke-BuildCookRunContractCase 'exact Game receipt selection' {
        $ProjectRoot = Join-Path $FixtureRoot 'ExactReceiptProject'
        $Cutoff = [System.DateTime]::UtcNow.AddMinutes(-1)
        $ExpectedPath = Write-BuildCookRunReceiptFixture `
            -ProjectRoot $ProjectRoot `
            -FileName 'Game.target' `
            -TargetName 'Game' `
            -Configuration Development `
            -LastWriteTimeUtc ([System.DateTime]::UtcNow)
        [void](Write-BuildCookRunReceiptFixture `
                -ProjectRoot $ProjectRoot `
                -FileName 'GameEditor.target' `
                -TargetName 'GameEditor' `
                -Configuration Development `
                -LastWriteTimeUtc ([System.DateTime]::UtcNow))
        $Selected = Get-AvidScriptBuildCookRunGameReceipt `
            -ProjectRoot $ProjectRoot `
            -TargetName 'Game' `
            -Configuration Development `
            -NotBeforeUtc $Cutoff
        Assert-BuildCookRunContract `
            ($Selected.Path -ceq $ExpectedPath) `
            'Receipt selector did not choose the exact Game target.'
        Assert-BuildCookRunContract `
            ($Selected.Freshness -ceq 'fresh') `
            'A fresh receipt received an unexpected freshness classification.'
    }

    Invoke-BuildCookRunContractCase 'stale receipt rejection' {
        $ProjectRoot = Join-Path $FixtureRoot 'StaleReceiptProject'
        $Cutoff = [System.DateTime]::UtcNow
        [void](Write-BuildCookRunReceiptFixture `
                -ProjectRoot $ProjectRoot `
                -FileName 'Game.target' `
                -TargetName 'Game' `
                -Configuration Shipping `
                -LastWriteTimeUtc $Cutoff.AddMinutes(-5))
        $Rejected = $false
        try {
            Get-AvidScriptBuildCookRunGameReceipt `
                -ProjectRoot $ProjectRoot `
                -TargetName 'Game' `
                -Configuration Shipping `
                -NotBeforeUtc $Cutoff | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'receipt_stale'
        }
        Assert-BuildCookRunContract $Rejected 'A stale exact receipt was accepted.'

        $UatLogPath = Join-Path $ProjectRoot 'Saved/BuildCookRun.log'
        [void][System.IO.Directory]::CreateDirectory(
            [System.IO.Path]::GetDirectoryName($UatLogPath))
        $ExpectedBinaryPath = [System.IO.Path]::GetFullPath(
            (Join-Path $ProjectRoot 'Binaries/Win64/Game-Win64-Shipping.exe'))
        [System.IO.File]::WriteAllText(
            $UatLogPath,
            "Output binary: $ExpectedBinaryPath",
            $Utf8)
        $ValidatedExisting = Get-AvidScriptBuildCookRunGameReceipt `
            -ProjectRoot $ProjectRoot `
            -TargetName 'Game' `
            -Configuration Shipping `
            -NotBeforeUtc $Cutoff `
            -UatLogPath $UatLogPath
        Assert-BuildCookRunContract `
            ($ValidatedExisting.Freshness -ceq 'uat_validated_existing') `
            'An exact UAT-validated existing receipt was not classified explicitly.'
    }

    Invoke-BuildCookRunContractCase 'ambiguous receipt rejection' {
        $ProjectRoot = Join-Path $FixtureRoot 'AmbiguousReceiptProject'
        $Now = [System.DateTime]::UtcNow
        foreach ($FileName in @('Game.target', 'Game-Win64-Development.target')) {
            [void](Write-BuildCookRunReceiptFixture `
                    -ProjectRoot $ProjectRoot `
                    -FileName $FileName `
                    -TargetName 'Game' `
                    -Configuration Development `
                    -LastWriteTimeUtc $Now)
        }
        $Rejected = $false
        try {
            Get-AvidScriptBuildCookRunGameReceipt `
                -ProjectRoot $ProjectRoot `
                -TargetName 'Game' `
                -Configuration Development `
                -NotBeforeUtc $Now.AddMinutes(-1) | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'receipt_ambiguous'
        }
        Assert-BuildCookRunContract $Rejected 'Ambiguous exact receipts were accepted.'
    }

    Invoke-BuildCookRunContractCase 'receipt validator handoff' {
        $ReceiptFunction = Get-BuildCookRunFunctionAst `
            -Ast $RunnerAst `
            -Name 'Invoke-AvidScriptBuildCookRunReceiptStep'
        $ReceiptText = $ReceiptFunction.Extent.Text
        foreach ($RequiredToken in @(
                'TestAvidScriptPackageReceipt.ps1',
                "'-ReceiptPath'",
                '$ReceiptPath',
                "'-ProjectRoot'",
                '$ProjectContext.ProjectRoot',
                "'-PluginRoot'",
                '$ProjectContext.PluginRoot',
                "'-Configuration'",
                '$Configuration',
                'avidscript_package_receipt_valid')) {
            Assert-BuildCookRunContract `
                ($ReceiptText.Contains($RequiredToken)) `
                "Receipt handoff token is missing: $RequiredToken"
        }
    }

    Invoke-BuildCookRunContractCase 'packaged oracle process and report contract' {
        foreach ($RequiredToken in @(
                'Resolve-AvidScriptPackagedOracleExecutable',
                'System.Diagnostics.ProcessStartInfo',
                'ArgumentList.Add($Argument)',
                '-AvidScriptPackagedOracle=',
                '-AvidScriptPackagedOracleReport=',
                "Environment['AVIDSCRIPT_PACKAGED_ORACLE_MODULE']",
                "Environment['AVIDSCRIPT_PACKAGED_ORACLE_REPORT']",
                'WaitForExit($TimeoutSeconds * 1000)',
                'Process.Kill($true)',
                'avidscript_packaged_oracle_process_passed',
                'continuation_observed',
                'world_continued',
                'resolved_from_package')) {
            Assert-BuildCookRunContract `
                ($PackagedOracleSource.Contains($RequiredToken)) `
                "Packaged oracle contract token is missing: $RequiredToken"
        }
        Assert-BuildCookRunContract `
            (-not $PackagedOracleSource.Contains('Invoke-Expression')) `
            'Packaged oracle uses Invoke-Expression.'
    }

    if ($Failures.Count -ne 0) {
        throw "BuildCookRun contracts failed ($Passed/$Total): $($Failures -join ' | ')"
    }

    [pscustomobject][ordered]@{
        result = 'avidscript_build_cook_run_contracts_passed'
        status = 'ok'
        passed = $Passed
        total = $Total
        coverage = @(
            'parameter_schema',
            'ue58_source_engine',
            'release_before_uat',
            'no_clean',
            'zen_workaround',
            'development_shipping',
            'argument_list',
            'single_line_json',
            'archive_safety',
            'receipt_exact_stale_ambiguous',
            'receipt_validator_handoff',
            'packaged_oracle_process_report'
        )
    } | ConvertTo-Json -Depth 4 -Compress
}
catch {
    $Failure = [pscustomobject][ordered]@{
        result = 'avidscript_build_cook_run_contracts_failed'
        status = 'error'
        passed = $Passed
        total = $Total
        message = $_.Exception.Message
    }
    [Console]::Out.WriteLine(($Failure | ConvertTo-Json -Depth 4 -Compress))
    exit 1
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot -PathType Container) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

exit 0
