param(
    [string]$DotNetPath = "",
    [string]$OutputRoot = "",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

function New-ToolCandidate {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Path
    )

    return [PSCustomObject]@{
        Source = $Source
        Path = $Path
    }
}

function Resolve-ExistingFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Path).Path
    }

    return $null
}

function Resolve-DotNetTool {
    $Candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($DotNetPath)) {
        $Candidates += New-ToolCandidate -Source "parameter" -Path $DotNetPath
    }

    if (-not [string]::IsNullOrWhiteSpace($env:AVIDSCRIPT_DOTNET)) {
        $Candidates += New-ToolCandidate -Source "env:AVIDSCRIPT_DOTNET" -Path $env:AVIDSCRIPT_DOTNET
    }

    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $Candidates += New-ToolCandidate -Source "user_profile_dotnet" -Path (Join-Path $env:USERPROFILE ".dotnet\dotnet.exe")
    }

    $PathCommand = Get-Command "dotnet" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $PathCommand) {
        $Candidates += New-ToolCandidate -Source "PATH" -Path $PathCommand.Source
    }

    $Checked = @()
    foreach ($Candidate in $Candidates) {
        $Checked += "$($Candidate.Source):$($Candidate.Path)"
        $ResolvedPath = Resolve-ExistingFile -Path $Candidate.Path
        if ($null -ne $ResolvedPath) {
            return [PSCustomObject]@{
                Found = $true
                Source = $Candidate.Source
                Path = $ResolvedPath
                Checked = $Checked
            }
        }
    }

    return [PSCustomObject]@{
        Found = $false
        Source = ""
        Path = ""
        Checked = $Checked
    }
}

function Convert-ToProjectRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $RootPath = [System.IO.Path]::GetFullPath($ProjectRoot)
    $PathSeparators = @([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $RootPath = $RootPath.TrimEnd($PathSeparators)

    if ($FullPath.StartsWith($RootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        $RelativePath = $FullPath.Substring($RootPath.Length)
        $RelativePath = $RelativePath.TrimStart($PathSeparators)
        return $RelativePath.Replace("\", "/")
    }

    return $FullPath
}

function Read-U32Leb {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][ref]$Index
    )

    $Result = 0
    $Shift = 0
    while ($true) {
        if ($Index.Value -ge $Bytes.Length) {
            throw "Unexpected end of WASM while reading u32 LEB."
        }

        $Byte = [int]$Bytes[$Index.Value]
        $Index.Value++
        $Result = $Result -bor (($Byte -band 0x7f) -shl $Shift)
        if (($Byte -band 0x80) -eq 0) {
            return $Result
        }

        $Shift += 7
    }
}

function Read-WasmName {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][ref]$Index
    )

    $Length = Read-U32Leb -Bytes $Bytes -Index $Index
    if (($Index.Value + $Length) -gt $Bytes.Length) {
        throw "Unexpected end of WASM while reading a name."
    }

    $Text = [System.Text.Encoding]::UTF8.GetString($Bytes, $Index.Value, $Length)
    $Index.Value += $Length
    return $Text
}

function Get-WasmExports {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($Bytes.Length -lt 8 -or $Bytes[0] -ne 0x00 -or $Bytes[1] -ne 0x61 -or $Bytes[2] -ne 0x73 -or $Bytes[3] -ne 0x6d) {
        throw "File is not a WebAssembly module: $Path"
    }

    $Index = 8
    $Exports = @()
    while ($Index -lt $Bytes.Length) {
        $SectionId = [int]$Bytes[$Index]
        $Index++
        $IndexRef = [ref]$Index
        $SectionLength = Read-U32Leb -Bytes $Bytes -Index $IndexRef
        $Index = $IndexRef.Value
        $SectionEnd = $Index + $SectionLength

        if ($SectionId -eq 7) {
            $IndexRef = [ref]$Index
            $Count = Read-U32Leb -Bytes $Bytes -Index $IndexRef
            $Index = $IndexRef.Value
            for ($ExportIndex = 0; $ExportIndex -lt $Count; ++$ExportIndex) {
                $IndexRef = [ref]$Index
                $Name = Read-WasmName -Bytes $Bytes -Index $IndexRef
                $Index = $IndexRef.Value
                $Kind = [int]$Bytes[$Index]
                $Index++
                $IndexRef = [ref]$Index
                $ItemIndex = Read-U32Leb -Bytes $Bytes -Index $IndexRef
                $Index = $IndexRef.Value
                $Exports += [PSCustomObject]@{
                    name = $Name
                    kind = $Kind
                    index = $ItemIndex
                }
            }
            break
        }

        $Index = $SectionEnd
    }

    return $Exports
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Bytes = [System.IO.File]::ReadAllBytes($Path)
    $Sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $Hash = $Sha.ComputeHash($Bytes)
        return -join ($Hash | ForEach-Object { $_.ToString("x2") })
    }
    finally {
        $Sha.Dispose()
    }
}

function Write-Report {
    param(
        [Parameter(Mandatory = $true)][string]$Result,
        [Parameter(Mandatory = $true)][bool]$DirectAbiSupported,
        [Parameter(Mandatory = $true)][object[]]$Diagnostics,
        [object[]]$ObservedExports = @(),
        [string]$WasmPath = "",
        [string]$ManifestPath = "",
        [object[]]$SdkList = @(),
        [object[]]$WorkloadList = @()
    )

    $ReportDirectory = Split-Path -Parent $ReportPath
    New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null

    $Report = [ordered]@{
        schema_version = 1
        language = "csharp"
        module_id = "csharp_actor_lifecycle"
        result = $Result
        direct_abi_supported = $DirectAbiSupported
        source = [ordered]@{
            project = Convert-ToProjectRelativePath -Path $SampleProjectPath
            file = Convert-ToProjectRelativePath -Path $SampleSourcePath
        }
        output_root = Convert-ToProjectRelativePath -Path $OutputRoot
        required_exports = @(
            "avid_on_begin_play",
            "avid_on_tick"
        )
        required_imports = @(
            [ordered]@{
                module = "env"
                name = "actor_set_location"
            }
        )
        observed_exports = @($ObservedExports | ForEach-Object { $_.name })
        artifacts = [ordered]@{
            wasm_file = if ([string]::IsNullOrWhiteSpace($WasmPath)) { "" } else { Convert-ToProjectRelativePath -Path $WasmPath }
            manifest_file = if ([string]::IsNullOrWhiteSpace($ManifestPath)) { "" } else { Convert-ToProjectRelativePath -Path $ManifestPath }
            report_file = Convert-ToProjectRelativePath -Path $ReportPath
        }
        toolchain = [ordered]@{
            dotnet = if ($DotNet.Found) { $DotNet.Path } else { "" }
            dotnet_source = if ($DotNet.Found) { $DotNet.Source } else { "" }
            target_framework = "net8.0"
            runtime_identifier = "wasi-wasm"
            sdk_list = @($SdkList)
            workload_list = @($WorkloadList)
        }
        diagnostics = @($Diagnostics)
    }

    $Json = $Report | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText($ReportPath, $Json + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$ProjectPluginsDir = Split-Path -Parent $PluginRoot
$ProjectRoot = Split-Path -Parent $ProjectPluginsDir
$SampleProjectPath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\AvidScript.ActorLifecycle.csproj"
$SampleSourcePath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $ProjectRoot "Saved\AvidScriptCSharpGuest\ActorLifecycle"
}

$ReportPath = Join-Path $OutputRoot "actor_lifecycle.csharp.report.json"
$ManifestPath = Join-Path $OutputRoot "actor_lifecycle.avidscript.json"
$PublishRoot = Join-Path $OutputRoot "publish"
$BinaryRoot = Join-Path $OutputRoot "bin"
$IntermediateRoot = Join-Path $OutputRoot "obj"
$DotNetHome = Join-Path $OutputRoot "DotNetHome"
$NuGetPackagesRoot = Join-Path $OutputRoot "NuGetPackages"
$UserNuGetPackagesRoot = if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) { "" } else { Join-Path $env:USERPROFILE ".nuget\packages" }
if (-not [string]::IsNullOrWhiteSpace($UserNuGetPackagesRoot) -and (Test-Path -LiteralPath $UserNuGetPackagesRoot -PathType Container)) {
    $NuGetPackagesRoot = $UserNuGetPackagesRoot
}
$RedirectedAppData = Join-Path $OutputRoot "AppData\Roaming"
$RedirectedLocalAppData = Join-Path $OutputRoot "AppData\Local"
$RedirectedNuGetDirectory = Join-Path $RedirectedAppData "NuGet"
$NuGetConfigPath = Join-Path $RedirectedNuGetDirectory "NuGet.Config"
$Diagnostics = @()

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
New-Item -ItemType Directory -Force -Path $DotNetHome | Out-Null
if (-not (Test-Path -LiteralPath $NuGetPackagesRoot -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $NuGetPackagesRoot | Out-Null
}
New-Item -ItemType Directory -Force -Path $RedirectedNuGetDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $RedirectedLocalAppData | Out-Null
if (Test-Path -LiteralPath $ReportPath) {
    Remove-Item -LiteralPath $ReportPath -Force
}
if (Test-Path -LiteralPath $ManifestPath) {
    Remove-Item -LiteralPath $ManifestPath -Force
}

$NuGetConfig = @"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
  </packageSources>
</configuration>
"@
[System.IO.File]::WriteAllText($NuGetConfigPath, $NuGetConfig, [System.Text.UTF8Encoding]::new($false))
$env:DOTNET_CLI_HOME = $DotNetHome
$env:NUGET_PACKAGES = $NuGetPackagesRoot
$env:APPDATA = $RedirectedAppData
$env:LOCALAPPDATA = $RedirectedLocalAppData
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
$BinaryRootForMsBuild = $BinaryRoot.Replace("\", "/") + "/"
$IntermediateRootForMsBuild = $IntermediateRoot.Replace("\", "/") + "/"

$DotNet = Resolve-DotNetTool
if (-not $DotNet.Found) {
    $Diagnostics += [ordered]@{
        code = "dotnet_missing"
        message = "dotnet was not found"
        checked = @($DotNet.Checked)
    }
    Write-Report -Result "missing_toolchain" -DirectAbiSupported $false -Diagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Build] result=missing_toolchain missing=dotnet report=$ReportPath"
    exit 0
}

Write-Output "[AvidScript][CSharp][Toolchain] dotnet=FOUND source=$($DotNet.Source) path=$($DotNet.Path)"

$SdkList = @()
try {
    $SdkList = @(& $DotNet.Path --list-sdks 2>&1)
}
catch {
    $Diagnostics += [ordered]@{
        code = "dotnet_sdk_list_failed"
        message = $_.Exception.Message
    }
}

$WorkloadList = @()
try {
    $WorkloadList = @(& $DotNet.Path workload list 2>&1)
}
catch {
    $Diagnostics += [ordered]@{
        code = "dotnet_workload_list_failed"
        message = $_.Exception.Message
    }
}

if (($SdkList -join "`n") -notmatch "8\.0\.") {
    $Diagnostics += [ordered]@{
        code = "dotnet8_missing"
        message = ".NET 8 SDK is required for the current wasi-experimental probe"
    }
    Write-Report -Result "missing_toolchain" -DirectAbiSupported $false -Diagnostics $Diagnostics -SdkList $SdkList -WorkloadList $WorkloadList
    Write-Output "[AvidScript][CSharp][Build] result=missing_toolchain missing=dotnet8 report=$ReportPath"
    exit 0
}

if (($WorkloadList -join "`n") -notmatch "wasi-experimental") {
    $Diagnostics += [ordered]@{
        code = "wasi_workload_missing"
        message = "Install with: dotnet workload install wasi-experimental --skip-manifest-update"
    }
    Write-Report -Result "missing_workload" -DirectAbiSupported $false -Diagnostics $Diagnostics -SdkList $SdkList -WorkloadList $WorkloadList
    Write-Output "[AvidScript][CSharp][Build] result=missing_workload missing=wasi-experimental report=$ReportPath"
    exit 0
}

$PublishOutput = @()
$PublishExitCode = 0
try {
    $PublishOutput = @(& $DotNet.Path publish $SampleProjectPath -c $Configuration -v:minimal --configfile $NuGetConfigPath --ignore-failed-sources -o $PublishRoot "-p:BaseOutputPath=$BinaryRootForMsBuild" "-p:BaseIntermediateOutputPath=$IntermediateRootForMsBuild" "-p:RestoreIgnoreFailedSources=true" 2>&1)
    $PublishExitCode = $LASTEXITCODE
}
catch {
    $PublishExitCode = if ($LASTEXITCODE -ne $null) { $LASTEXITCODE } else { 1 }
    $PublishOutput += $_.Exception.Message
}

foreach ($Line in $PublishOutput) {
    Write-Output "[AvidScript][CSharp][Publish] $Line"
}

if ($PublishExitCode -ne 0) {
    $Diagnostics += [ordered]@{
        code = "publish_exit_nonzero"
        message = "dotnet publish exited with code $PublishExitCode"
    }
}

$WasmCandidates = @(
    (Join-Path $PublishRoot "dotnet.wasm"),
    (Join-Path $BinaryRoot "$Configuration\net8.0\wasi-wasm\dotnet.wasm"),
    (Join-Path $BinaryRoot "$Configuration\net8.0\wasi-wasm\publish\dotnet.wasm"),
    (Join-Path $BinaryRoot "$Configuration\net8.0\wasi-wasm\AppBundle\dotnet.wasm")
)

$WasmPath = ""
foreach ($Candidate in $WasmCandidates) {
    if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
        $WasmPath = (Resolve-Path -LiteralPath $Candidate).Path
        break
    }
}

if ([string]::IsNullOrWhiteSpace($WasmPath)) {
    $Diagnostics += [ordered]@{
        code = "wasm_artifact_missing"
        message = "dotnet publish did not produce dotnet.wasm"
    }
    Write-Report -Result "publish_failed" -DirectAbiSupported $false -Diagnostics $Diagnostics -SdkList $SdkList -WorkloadList $WorkloadList
    Write-Output "[AvidScript][CSharp][Build] result=publish_failed missing=dotnet.wasm report=$ReportPath"
    exit 0
}

$CopiedWasmPath = Join-Path $OutputRoot "actor_lifecycle.dotnet.wasm"
Copy-Item -LiteralPath $WasmPath -Destination $CopiedWasmPath -Force

$ObservedExports = @()
try {
    $ObservedExports = @(Get-WasmExports -Path $CopiedWasmPath)
}
catch {
    $Diagnostics += [ordered]@{
        code = "wasm_export_parse_failed"
        message = $_.Exception.Message
    }
}

$ObservedNames = @($ObservedExports | ForEach-Object { $_.name })
$RequiredExports = @("avid_on_begin_play", "avid_on_tick")
$MissingExports = @($RequiredExports | Where-Object { $ObservedNames -notcontains $_ })

if ($MissingExports.Count -gt 0) {
    $Diagnostics += [ordered]@{
        code = "direct_exports_missing"
        message = "Generated WASM does not expose the AvidScript direct ABI exports."
        missing_exports = @($MissingExports)
    }
    Write-Report -Result "direct_abi_unsupported" -DirectAbiSupported $false -Diagnostics $Diagnostics -ObservedExports $ObservedExports -WasmPath $CopiedWasmPath -SdkList $SdkList -WorkloadList $WorkloadList
    Write-Output "[AvidScript][CSharp][Build] result=direct_abi_unsupported missing_exports=$($MissingExports -join ',') observed_exports=$($ObservedNames -join ',') report=$ReportPath"
    exit 0
}

$ArtifactHash = Get-Sha256Hex -Path $CopiedWasmPath
$Manifest = [ordered]@{
    schema_version = 1
    module_id = "csharp_actor_lifecycle"
    abi_version = 1
    language = "csharp"
    source = [ordered]@{
        file = Convert-ToProjectRelativePath -Path $SampleSourcePath
    }
    wasm = [ordered]@{
        file = Convert-ToProjectRelativePath -Path $CopiedWasmPath
        sha256 = $ArtifactHash
    }
    required_exports = $RequiredExports
    required_imports = @(
        [ordered]@{
            module = "env"
            name = "actor_set_location"
        }
    )
    toolchain = [ordered]@{
        compiler = "dotnet"
        target = "wasi-wasm"
        direct_abi = $true
    }
}

$ManifestJson = $Manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($ManifestPath, $ManifestJson + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Report -Result "direct_abi_built" -DirectAbiSupported $true -Diagnostics $Diagnostics -ObservedExports $ObservedExports -WasmPath $CopiedWasmPath -ManifestPath $ManifestPath -SdkList $SdkList -WorkloadList $WorkloadList
Write-Output "[AvidScript][CSharp][Build] result=direct_abi_built manifest=$ManifestPath wasm=$CopiedWasmPath sha256=$ArtifactHash"
exit 0
