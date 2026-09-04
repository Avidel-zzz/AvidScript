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

function Invoke-BuildCookRunSelectionFixture {
    param([hashtable]$Selection = @{})

    $FakeRoot = Join-Path $FixtureRoot ([Guid]::NewGuid().ToString('N'))
    $Observed = [pscustomobject]@{
        Calls = [System.Collections.Generic.List[string]]::new()
        Arguments = @()
        Summary = $null
        ErrorCategory = ''
        ErrorMessage = ''
        ErrorStep = ''
    }
    function Get-AvidScriptBuildCookRunProjectContext {
        $Observed.Calls.Add('context')
        return [pscustomobject]@{
            ProjectRoot = $FakeRoot
            ProjectFile = Join-Path $FakeRoot 'Game.uproject'
            PluginRoot = Join-Path $FakeRoot 'Plugins/AvidScript'
            TargetName = 'Game'
        }
    }
    function Get-AvidScriptBuildCookRunEngineContext {
        return [pscustomobject]@{ RunUatPath = 'fixture-RunUAT.bat' }
    }
    function Invoke-AvidScriptBuildCookRunReleaseStep {
        $Observed.Calls.Add('release')
        return [pscustomobject]@{ status = 'ok' }
    }
    # Keep the real UAT step and argument builder; intercept only the external process.
    function Invoke-AvidScriptBuildCookRunProcess {
        param($Executable, $Arguments, $WorkingDirectory, $OutputLogPath)
        $Observed.Calls.Add('uat')
        $Observed.Arguments = @($Arguments)
        return [pscustomobject]@{ ExitCode = 0; Stdout = ''; Stderr = '' }
    }
    function Get-AvidScriptBuildCookRunGameReceipt {
        return [pscustomobject]@{ Path = 'fixture.target'; Freshness = 'fresh' }
    }
    function Invoke-AvidScriptBuildCookRunReceiptStep {
        $Observed.Calls.Add('receipt')
        return [pscustomobject]@{ status = 'ok' }
    }
    try {
        $Observed.Summary = Invoke-AvidScriptBuildCookRun @Selection `
            -SourcePath fixture -CSharpProjectPath fixture -ModuleId fixture `
            -ArtifactStem fixture -OutputRoot fixture -DotNetPath fixture `
            -Configuration Development -ArchiveRoot (Join-Path $FakeRoot 'Saved/Archive') `
            -PackagedOracleTimeoutSeconds 120 -PackagedOracleMode None -EngineRoot fixture
    }
    catch {
        $Observed.ErrorCategory = [string]$_.Exception.Data['category']
        $Observed.ErrorMessage = $_.Exception.Message
        $Observed.ErrorStep = $script:AvidScriptBuildCookRunStep
    }
    return $Observed
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
            'CookMaps',
            'CSharpProjectPath',
            'DotNetPath',
            'EnablePlugins',
            'EngineRoot',
            'GeneratedTypeManifestPath',
            'ModuleId',
            'OutputRoot',
            'PackagedOracleMode',
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
                "[string]`$PackagedOracleMode = 'Legacy'",
                "[string]`$GeneratedTypeManifestPath = ''",
                '[string[]]$CookMaps = @()',
                '[string[]]$EnablePlugins = @()',
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

    Invoke-BuildCookRunContractCase 'empty cook selections preserve arguments and JSON arrays' {
        $Base = @{ ProjectFile = 'C:\Project\Game.uproject'; TargetName = 'Game';
            Configuration = 'Development'; ArchiveRoot = 'C:\Project\Saved\Archive' }
        $DefaultArguments = @(New-AvidScriptBuildCookRunUatArguments @Base)
        $EmptyArguments = @(New-AvidScriptBuildCookRunUatArguments @Base -CookMaps @() -EnablePlugins @())
        Assert-BuildCookRunContract (($DefaultArguments -join '|') -ceq ($EmptyArguments -join '|')) `
            'Explicit empty selections changed the default UAT arguments.'
        Assert-BuildCookRunContract ($DefaultArguments.Count -eq 16 -and
            @($DefaultArguments | Where-Object { $_ -match 'map=|ubtargs=|EnablePlugin|WaitMutex|NoHotReloadFromIDE' }).Count -eq 0) `
            'Default arguments acquired opt-in map/plugin switches.'
        foreach ($Selection in @(@{}, @{ CookMaps = @(); EnablePlugins = @() })) {
            $Observed = Invoke-BuildCookRunSelectionFixture -Selection $Selection
            Assert-BuildCookRunContract ($Observed.ErrorMessage -ceq '') "Fixture failed: $($Observed.ErrorMessage)"
            $Json = $Observed.Summary | ConvertTo-Json -Depth 8 -Compress | ConvertFrom-Json
            Assert-BuildCookRunContract ($Json.cook_maps -is [array] -and $Json.cook_maps.Count -eq 0 -and
                $Json.enable_plugins -is [array] -and $Json.enable_plugins.Count -eq 0) `
                'Default result selections must serialize as empty arrays.'
        }
    }

    Invoke-BuildCookRunContractCase 'map and plugin selections reach UAT and final result' {
        $Maps = @('/AvidScript/Demos/UiSave/L_UiSave', '/Game/Maps/Test_2')
        $Plugins = @('AvidScriptValidation', 'Second_Plugin2')
        $Observed = Invoke-BuildCookRunSelectionFixture -Selection @{ CookMaps = $Maps; EnablePlugins = $Plugins }
        Assert-BuildCookRunContract ($Observed.ErrorMessage -ceq '') "Fixture failed: $($Observed.ErrorMessage)"
        Assert-BuildCookRunContract (($Observed.Calls -join '|') -ceq 'context|release|uat|receipt') `
            'Selections changed the Release/UAT/receipt sequence.'
        foreach ($Expected in @(
                '-map=/AvidScript/Demos/UiSave/L_UiSave+/Game/Maps/Test_2',
                '-ubtargs=-EnablePlugin=AvidScriptValidation+Second_Plugin2 -WaitMutex -NoHotReloadFromIDE',
                '-AdditionalCookerOptions=-SkipZenStore -AvidScriptSuppressGeneratedTypeExecution -EnablePlugins=AvidScriptValidation,Second_Plugin2',
                '-skipbuildeditor')) {
            Assert-BuildCookRunContract ($Observed.Arguments -ccontains $Expected) "Missing UAT argument: $Expected"
        }
        foreach ($Prefix in @('-map=', '-ubtargs=', '-AdditionalCookerOptions=')) {
            Assert-BuildCookRunContract (@($Observed.Arguments | Where-Object { $_.StartsWith($Prefix) }).Count -eq 1) `
                "Expected exactly one $Prefix argument."
        }
        Assert-BuildCookRunContract (@($Observed.Arguments | Where-Object {
                $_ -match '^-(?:EnablePlugins?|Plugin|ForeignPlugin|WaitMutex|NoHotReloadFromIDE)(?:=|$)|UniqueBuildEnvironment'
            }).Count -eq 0) 'Plugin switches leaked to UAT top level or changed the build environment.'
        $Json = $Observed.Summary | ConvertTo-Json -Depth 8 -Compress | ConvertFrom-Json
        Assert-BuildCookRunContract ($Json.cook_maps -is [array] -and $Json.enable_plugins -is [array] -and
            ($Json.cook_maps -join '|') -ceq ($Maps -join '|') -and
            ($Json.enable_plugins -join '|') -ceq ($Plugins -join '|')) `
            'Final JSON lost the selected map/plugin arrays, order, or case.'
        $Commands = @($RunnerAst.FindAll({ param($Node)
                $Node -is [System.Management.Automation.Language.CommandAst] -and
                    $Node.GetCommandName() -ceq 'Invoke-AvidScriptBuildCookRun'
            }, $true))
        Assert-BuildCookRunContract ($Commands.Count -eq 1) 'Expected one top-level runner invocation.'
        foreach ($Name in @('CookMaps', 'EnablePlugins')) {
            Assert-BuildCookRunContract ($Commands[0].Extent.Text.Contains("-$Name `$$Name")) `
                "Top-level script does not forward $Name."
        }
    }

    Invoke-BuildCookRunContractCase 'map-only and plugin-only switches stay independent' {
        $Base = @{ ProjectFile = 'C:\Project\Game.uproject'; TargetName = 'Game';
            Configuration = 'Shipping'; ArchiveRoot = 'C:\Project\Saved\Archive' }
        $MapOnly = @(New-AvidScriptBuildCookRunUatArguments @Base -CookMaps '/Game/Test')
        Assert-BuildCookRunContract ($MapOnly -ccontains '-map=/Game/Test' -and
            $MapOnly -ccontains '-AdditionalCookerOptions=-SkipZenStore -AvidScriptSuppressGeneratedTypeExecution' -and
            @($MapOnly | Where-Object { $_ -match 'ubtargs|EnablePlugin|WaitMutex|NoHotReloadFromIDE' }).Count -eq 0) `
            'Map-only selection added plugin/build flags.'
        $PluginOnly = @(New-AvidScriptBuildCookRunUatArguments @Base -EnablePlugins 'AvidScriptValidation')
        Assert-BuildCookRunContract (@($PluginOnly | Where-Object { $_ -match '^-map=' }).Count -eq 0 -and
            $PluginOnly -ccontains '-ubtargs=-EnablePlugin=AvidScriptValidation -WaitMutex -NoHotReloadFromIDE') `
            'Plugin-only selection added a map or lost UBT flags.'
    }

    foreach ($InvalidGroup in @(
            @{ Name = 'null cook selection arrays'; Option = 'CookMaps'; Values = @($null); Category = 'cook_maps_invalid' },
            @{ Name = 'null plugin selection arrays'; Option = 'EnablePlugins'; Values = @($null); Category = 'enable_plugins_invalid' },
            @{ Name = 'empty and malformed maps'; Option = 'CookMaps';
                Values = @('', ' ', '/Game', 'Game/Map', '/Game/Map/', '/Game//Map', '/Game/../Map', '/Game/Map.umap', '/Game\Map');
                Category = 'cook_maps_invalid' },
            @{ Name = 'empty and malformed plugins'; Option = 'EnablePlugins';
                Values = @('', ' ', '1Plugin', '_Plugin', 'A-B', 'A/B'); Category = 'enable_plugins_invalid' },
            @{ Name = 'map delimiter and command injection'; Option = 'CookMaps';
                Values = @('/Game/A+/Game/B', '/Game/A,/Game/B', '/Game/A -clean', '/Game/A"', '/Game/A;echo', '/Game/A&echo', '/Game/A|echo', "/Game/A`n");
                Category = 'cook_maps_invalid' },
            @{ Name = 'plugin delimiter and command injection'; Option = 'EnablePlugins';
                Values = @('A+B', 'A,B', 'A -clean', 'A"', 'A;echo', 'A&echo', 'A|echo', "A`n"); Category = 'enable_plugins_invalid' },
            @{ Name = 'duplicate and null map entries'; Option = 'CookMaps';
                Values = @(@('/Game/A', '/game/a'), @('/Game/A', $null)); Category = 'cook_maps_invalid' },
            @{ Name = 'duplicate and null plugin entries'; Option = 'EnablePlugins';
                Values = @(@('Plugin', 'plugin'), @('Plugin', $null)); Category = 'enable_plugins_invalid' })) {
        Invoke-BuildCookRunContractCase $InvalidGroup.Name {
            foreach ($InvalidValue in $InvalidGroup.Values) {
                $Selection = @{ ($InvalidGroup.Option) = $InvalidValue }
                $Observed = Invoke-BuildCookRunSelectionFixture -Selection $Selection
                Assert-BuildCookRunContract ($Observed.ErrorCategory -ceq $InvalidGroup.Category -and
                    $Observed.ErrorStep -ceq 'validation' -and $Observed.Calls.Count -eq 0) `
                    "Invalid $($InvalidGroup.Option) reached context/Release/UAT: $($Observed.ErrorMessage)"
            }
        }
    }

    Invoke-BuildCookRunContractCase 'direct UAT entry points reject invalid selections before side effects' {
        $UnusedRoot = Join-Path $FixtureRoot 'InvalidDirectUat'
        foreach ($Option in @('CookMaps', 'EnablePlugins')) {
            $Selection = @{ $Option = 'invalid;injection' }
            $ExpectedCategory = if ($Option -ceq 'CookMaps') { 'cook_maps_invalid' } else { 'enable_plugins_invalid' }
            foreach ($Entry in @('arguments', 'step')) {
                $Rejected = $false
                try {
                    if ($Entry -ceq 'arguments') {
                        New-AvidScriptBuildCookRunUatArguments @Selection -ProjectFile fixture -TargetName Game `
                            -Configuration Development -ArchiveRoot fixture | Out-Null
                    }
                    else {
                        Invoke-AvidScriptBuildCookRunUatStep @Selection -ProjectContext ([pscustomobject]@{ ProjectRoot = $UnusedRoot }) `
                            -EngineContext ([pscustomobject]@{}) -Configuration Development -ArchiveRoot fixture | Out-Null
                    }
                }
                catch { $Rejected = $_.Exception.Data['category'] -ceq $ExpectedCategory }
                Assert-BuildCookRunContract $Rejected "Direct UAT $Entry accepted invalid $Option."
                Assert-BuildCookRunContract (-not (Test-Path -LiteralPath $UnusedRoot)) 'Invalid UAT input created directories/logs.'
            }
        }
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

    Invoke-BuildCookRunContractCase 'packaged oracle selects the real configuration binary' {
        $OracleFixtureRoot = Join-Path $FixtureRoot 'PackagedOracleExecutable'
        $PackageRoot = Join-Path $OracleFixtureRoot 'Windows'
        $BinaryRoot = Join-Path $PackageRoot 'Game/Binaries/Win64'
        [void][System.IO.Directory]::CreateDirectory($BinaryRoot)
        $LauncherPath = Join-Path $PackageRoot 'Game.exe'
        $DevelopmentPath = Join-Path $BinaryRoot 'Game.exe'
        $ShippingPath = Join-Path $BinaryRoot 'Game-Win64-Shipping.exe'
        foreach ($Path in @($LauncherPath, $DevelopmentPath, $ShippingPath)) {
            [System.IO.File]::WriteAllBytes($Path, [byte[]](0x4d, 0x5a))
        }
        . $PackagedOraclePath `
            -ArchiveRoot $OracleFixtureRoot `
            -TargetName Game `
            -ModuleId test.module `
            -Configuration Development `
            -TimeoutSeconds 10
        $ResolvedDevelopment = Resolve-AvidScriptPackagedOracleExecutable `
            -ResolvedArchiveRoot $OracleFixtureRoot `
            -ResolvedTargetName Game `
            -ResolvedConfiguration Development
        $ResolvedShipping = Resolve-AvidScriptPackagedOracleExecutable `
            -ResolvedArchiveRoot $OracleFixtureRoot `
            -ResolvedTargetName Game `
            -ResolvedConfiguration Shipping
        Assert-BuildCookRunContract `
            ($ResolvedDevelopment -ceq [System.IO.Path]::GetFullPath($DevelopmentPath)) `
            'Development resolved the launcher instead of the real game binary.'
        Assert-BuildCookRunContract `
            ($ResolvedShipping -ceq [System.IO.Path]::GetFullPath($ShippingPath)) `
            'Shipping resolved the launcher instead of the configuration binary.'
    }

    Invoke-BuildCookRunContractCase 'packaged oracle process and report contract' {
        foreach ($RequiredToken in @(
                'Resolve-AvidScriptPackagedOracleExecutable',
                'System.Diagnostics.ProcessStartInfo',
                'SpecialFolder]::LocalApplicationData',
                '$Arguments.Add($TargetName)',
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

    Invoke-BuildCookRunContractCase 'PickupRush prepares the matching Generated Type with pinned SDK cwd' {
        $PickupRushPath = Join-Path $BuildRoot 'InvokeAvidScriptPickupRush.ps1'
        $Errors = $null
        $PickupRushAst = [System.Management.Automation.Language.Parser]::ParseFile(
            $PickupRushPath, [ref]$null, [ref]$Errors)
        Assert-BuildCookRunContract ($Errors.Count -eq 0) 'PickupRush runner has parse errors.'
        foreach ($Name in @('Throw-AvidScriptPickupRushError', 'Publish-AvidScriptPickupRushGeneratedTypes')) {
            $Function = Get-BuildCookRunFunctionAst -Ast $PickupRushAst -Name $Name
            . ([scriptblock]::Create($Function.Extent.Text))
        }
        $ProjectRoot = Join-Path $FixtureRoot 'PickupRushProject'
        $PluginRoot = Join-Path $ProjectRoot 'Plugins/AvidScript'
        $CurrentRoot = Join-Path $PluginRoot 'Content/AvidScriptGenerated'
        $CatalogRoot = Join-Path $ProjectRoot 'Content/AvidScript/Modules'
        [void][System.IO.Directory]::CreateDirectory($CurrentRoot)
        [void][System.IO.Directory]::CreateDirectory($CatalogRoot)
        [System.IO.File]::WriteAllText(
            (Join-Path $CurrentRoot 'current.json'),
            '{"module_id":"avidscript_generated","package_id":"fixture-package"}', $Utf8)
        [System.IO.File]::WriteAllText(
            (Join-Path $CatalogRoot 'catalog.json'),
            '{"modules":[{"module_id":"avidscript_generated","variants":[{"platform":"win64","architecture":"x86_64","configuration":"shipping","package_id":"fixture-package"}]}]}',
            $Utf8)
        $Configuration = 'Shipping'
        $DotNetPath = 'fixture-dotnet'
        $TimeoutSeconds = 30
        function Invoke-AvidScriptPickupRushProcess {
            param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds)
            Assert-BuildCookRunContract ($WorkingDirectory -ceq $PluginRoot) 'Generator bypassed plugin global.json.'
            $ConfigurationIndex = [Array]::IndexOf($Arguments, '-PackageConfiguration')
            Assert-BuildCookRunContract `
                ($ConfigurationIndex -ge 0 -and $Arguments[$ConfigurationIndex + 1] -ceq $Configuration) `
                'Generator did not receive the package configuration.'
            Assert-BuildCookRunContract ($Arguments -contains '-HeadlessRelease') 'Generator did not request precompiled release.'
            return [pscustomobject]@{ ExitCode = 0; Stdout = ''; Stderr = '' }
        }
        $Published = Publish-AvidScriptPickupRushGeneratedTypes -ResolvedBindingPackagePath 'fixture-binding'
        Assert-BuildCookRunContract ($Published.package_id -ceq 'fixture-package') 'Published package identity was lost.'
        $Configuration = 'Development'
        $Rejected = $false
        try {
            Publish-AvidScriptPickupRushGeneratedTypes -ResolvedBindingPackagePath 'fixture-binding' | Out-Null
        }
        catch {
            $Rejected = $_.Exception.Data['category'] -ceq 'generated_type_publication_invalid'
        }
        Assert-BuildCookRunContract $Rejected 'A mismatched package configuration was accepted.'
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
            'cook_selection_defaults_and_forwarding',
            'nested_ubt_and_cooker_plugin_options',
            'cook_selection_json_arrays',
            'cook_selection_fail_closed_before_release_uat',
            'argument_list',
            'single_line_json',
            'archive_safety',
            'receipt_exact_stale_ambiguous',
            'receipt_validator_handoff',
            'packaged_oracle_configuration_binary',
            'packaged_oracle_process_report',
            'pickup_rush_generated_type_configuration_sdk_cwd'
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
