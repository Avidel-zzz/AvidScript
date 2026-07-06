param(
    [string]$DotNetPath = "",
    [string]$OutputRoot = "",
    [string]$Configuration = "Release",
    [string]$SourcePath = "",
    [string]$ProjectPath = "",
    [string]$ModuleId = "",
    [string]$ArtifactStem = "",
    [string]$ReportPath = "",
    [string]$ManifestPath = ""
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

function New-WasmByteList {
    $List = [System.Collections.Generic.List[byte]]::new()
    return ,$List
}

function Add-WasmByte {
    param(
        [Parameter(Mandatory = $true)]$Bytes,
        [Parameter(Mandatory = $true)][int]$Value
    )

    [void]$Bytes.Add([byte]($Value -band 0xff))
}

function Add-WasmBytes {
    param(
        [Parameter(Mandatory = $true)]$Bytes,
        [Parameter(Mandatory = $true)]$Values
    )

    foreach ($Value in $Values) {
        Add-WasmByte -Bytes $Bytes -Value ([int]$Value)
    }
}

function Add-WasmU32Leb {
    param(
        [Parameter(Mandatory = $true)]$Bytes,
        [Parameter(Mandatory = $true)][uint32]$Value
    )

    $Remaining = $Value
    do {
        $Byte = [int]($Remaining -band 0x7f)
        $Remaining = $Remaining -shr 7
        if ($Remaining -ne 0) {
            $Byte = $Byte -bor 0x80
        }
        Add-WasmByte -Bytes $Bytes -Value $Byte
    } while ($Remaining -ne 0)
}

function Add-WasmString {
    param(
        [Parameter(Mandatory = $true)]$Bytes,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $Encoded = [System.Text.Encoding]::UTF8.GetBytes($Text)
    Add-WasmU32Leb -Bytes $Bytes -Value ([uint32]$Encoded.Length)
    Add-WasmBytes -Bytes $Bytes -Values $Encoded
}

function Add-WasmSection {
    param(
        [Parameter(Mandatory = $true)]$Module,
        [Parameter(Mandatory = $true)][int]$SectionId,
        [Parameter(Mandatory = $true)]$Payload
    )

    Add-WasmByte -Bytes $Module -Value $SectionId
    Add-WasmU32Leb -Bytes $Module -Value ([uint32]$Payload.Count)
    Add-WasmBytes -Bytes $Module -Values $Payload.ToArray()
}

function Add-WasmF32 {
    param(
        [Parameter(Mandatory = $true)]$Bytes,
        [Parameter(Mandatory = $true)][single]$Value
    )

    Add-WasmBytes -Bytes $Bytes -Values ([System.BitConverter]::GetBytes($Value))
}

function Convert-WasmByteListToArray {
    param([Parameter(Mandatory = $true)]$Bytes)

    $Array = [byte[]]::new($Bytes.Count)
    $Bytes.CopyTo($Array)
    return $Array
}

function Get-CSharpMethodBody {
    param(
        [Parameter(Mandatory = $true)][string]$SourceText,
        [Parameter(Mandatory = $true)][string]$MethodName
    )

    $Pattern = "(?s)\b$([regex]::Escape($MethodName))\s*\([^)]*\)\s*\{"
    $Match = [regex]::Match($SourceText, $Pattern)
    if (-not $Match.Success) {
        throw "C# source adapter could not find method '$MethodName'."
    }

    $OpenBraceIndex = $SourceText.IndexOf('{', $Match.Index)
    if ($OpenBraceIndex -lt 0) {
        throw "C# source adapter could not find method body for '$MethodName'."
    }

    $Depth = 0
    for ($Index = $OpenBraceIndex; $Index -lt $SourceText.Length; ++$Index) {
        $Char = $SourceText[$Index]
        if ($Char -eq '{') {
            ++$Depth
        }
        elseif ($Char -eq '}') {
            --$Depth
            if ($Depth -eq 0) {
                $Start = $OpenBraceIndex + 1
                return $SourceText.Substring($Start, $Index - $Start)
            }
        }
    }

    throw "C# source adapter found an unterminated method body for '$MethodName'."
}

function Convert-CSharpNumericLiteral {
    param([Parameter(Mandatory = $true)][string]$Expression)

    $Normalized = $Expression.Trim()
    $Normalized = [regex]::Replace($Normalized, '(?<=[0-9\.])[fFdDmM]$', '')
    $Value = [single]0
    if ([single]::TryParse(
        $Normalized,
        [System.Globalization.NumberStyles]::Float,
        [System.Globalization.CultureInfo]::InvariantCulture,
        [ref]$Value)) {
        return $Value
    }

    throw "Unsupported C# numeric expression '$Expression'."
}

function Convert-CSharpFloatExpression {
    param([Parameter(Mandatory = $true)][string]$Expression)

    $Trimmed = $Expression.Trim()
    $AddMatch = [regex]::Match($Trimmed, '^(?<left>.+?)\s*\+\s*(?<right>.+)$')
    if ($AddMatch.Success) {
        return [PSCustomObject]@{
            Kind = 'add'
            Left = Convert-CSharpFloatExpression -Expression $AddMatch.Groups['left'].Value
            Right = Convert-CSharpFloatExpression -Expression $AddMatch.Groups['right'].Value
        }
    }

    if ($Trimmed -eq 'deltaSeconds') {
        return [PSCustomObject]@{
            Kind = 'delta'
            Value = [single]0
        }
    }

    $MultiplyMatch = [regex]::Match($Trimmed, '^(?<left>.+?)\s*\*\s*(?<right>.+)$')
    if ($MultiplyMatch.Success) {
        $Left = $MultiplyMatch.Groups['left'].Value.Trim()
        $Right = $MultiplyMatch.Groups['right'].Value.Trim()
        if ($Left -eq 'deltaSeconds') {
            return [PSCustomObject]@{
                Kind = 'delta_mul'
                Value = Convert-CSharpNumericLiteral -Expression $Right
            }
        }
        if ($Right -eq 'deltaSeconds') {
            return [PSCustomObject]@{
                Kind = 'delta_mul'
                Value = Convert-CSharpNumericLiteral -Expression $Left
            }
        }
    }

    return [PSCustomObject]@{
        Kind = 'constant'
        Value = Convert-CSharpNumericLiteral -Expression $Trimmed
    }
}

function Get-CSharpActorCalls {
    param(
        [Parameter(Mandatory = $true)][string]$MethodBody,
        [Parameter(Mandatory = $true)][string]$MethodName
    )

    $Calls = @()
    $Pattern = 'Actor\s*\.\s*(?<method>SetLocation|AddLocationOffset)\s*\(\s*(?<x>[^,]+?)\s*,\s*(?<y>[^,]+?)\s*,\s*(?<z>[^\)]+?)\s*\)\s*;'
    foreach ($Match in [regex]::Matches($MethodBody, $Pattern)) {
        $Method = $Match.Groups['method'].Value
        $Calls += [PSCustomObject]@{
            Kind = if ($Method -eq 'SetLocation') { 'set_location' } else { 'add_location_offset' }
            X = Convert-CSharpFloatExpression -Expression $Match.Groups['x'].Value
            Y = Convert-CSharpFloatExpression -Expression $Match.Groups['y'].Value
            Z = Convert-CSharpFloatExpression -Expression $Match.Groups['z'].Value
        }
    }

    if ($Calls.Count -eq 0) {
        throw "C# source adapter found no supported Actor calls in '$MethodName'. Supported calls: Actor.SetLocation and Actor.AddLocationOffset."
    }

    return $Calls
}

function Add-CSharpFloatExpressionCode {
    param(
        [Parameter(Mandatory = $true)]$Body,
        [Parameter(Mandatory = $true)]$Expression,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds
    )

    if ($Expression.Kind -eq 'constant') {
        Add-WasmByte -Bytes $Body -Value 0x43
        Add-WasmF32 -Bytes $Body -Value ([single]$Expression.Value)
        return
    }

    if ($Expression.Kind -eq 'add') {
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Expression.Left -AllowDeltaSeconds $AllowDeltaSeconds
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Expression.Right -AllowDeltaSeconds $AllowDeltaSeconds
        Add-WasmByte -Bytes $Body -Value 0x92
        return
    }

    if (-not $AllowDeltaSeconds) {
        throw 'deltaSeconds is only available inside Tick(float deltaSeconds).'
    }

    if ($Expression.Kind -eq 'delta') {
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value 0
        return
    }

    if ($Expression.Kind -eq 'delta_mul') {
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value 0
        Add-WasmByte -Bytes $Body -Value 0x43
        Add-WasmF32 -Bytes $Body -Value ([single]$Expression.Value)
        Add-WasmByte -Bytes $Body -Value 0x94
        return
    }

    throw "Unsupported C# expression kind '$($Expression.Kind)'."
}

function Add-CSharpActorCall {
    param(
        [Parameter(Mandatory = $true)]$Body,
        [Parameter(Mandatory = $true)]$Call,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds
    )

    $FunctionIndex = 0
    if ($Call.Kind -eq 'set_location') {
        $FunctionIndex = 0
    }
    elseif ($Call.Kind -eq 'add_location_offset') {
        $FunctionIndex = 1
    }
    else {
        throw "Unsupported C# actor call kind '$($Call.Kind)'."
    }

    Add-WasmByte -Bytes $Body -Value 0x41
    Add-WasmU32Leb -Bytes $Body -Value 1
    Add-WasmByte -Bytes $Body -Value 0x41
    Add-WasmU32Leb -Bytes $Body -Value 1
    Add-CSharpFloatExpressionCode -Body $Body -Expression $Call.X -AllowDeltaSeconds $AllowDeltaSeconds
    Add-CSharpFloatExpressionCode -Body $Body -Expression $Call.Y -AllowDeltaSeconds $AllowDeltaSeconds
    Add-CSharpFloatExpressionCode -Body $Body -Expression $Call.Z -AllowDeltaSeconds $AllowDeltaSeconds
    Add-WasmByte -Bytes $Body -Value 0x10
    Add-WasmU32Leb -Bytes $Body -Value $FunctionIndex
    Add-WasmByte -Bytes $Body -Value 0x1a
}

function New-CSharpLifecycleFunctionBody {
    param(
        [Parameter(Mandatory = $true)]$Calls,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds
    )

    $Body = New-WasmByteList
    Add-WasmU32Leb -Bytes $Body -Value 0
    foreach ($Call in $Calls) {
        Add-CSharpActorCall -Body $Body -Call $Call -AllowDeltaSeconds $AllowDeltaSeconds
    }
    Add-WasmByte -Bytes $Body -Value 0x0b
    return ,$Body
}

function New-CSharpDirectAbiWasmModule {
    param(
        [Parameter(Mandatory = $true)]$BeginPlayCalls,
        [Parameter(Mandatory = $true)]$TickCalls
    )

    $Module = New-WasmByteList
    Add-WasmBytes -Bytes $Module -Values ([byte[]](0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00))

    $TypeSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $TypeSection -Value 3
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 5
    Add-WasmBytes -Bytes $TypeSection -Values ([byte[]](0x7f, 0x7f, 0x7d, 0x7d, 0x7d))
    Add-WasmU32Leb -Bytes $TypeSection -Value 1
    Add-WasmByte -Bytes $TypeSection -Value 0x7f
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 0
    Add-WasmU32Leb -Bytes $TypeSection -Value 0
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 1
    Add-WasmByte -Bytes $TypeSection -Value 0x7d
    Add-WasmU32Leb -Bytes $TypeSection -Value 0
    Add-WasmSection -Module $Module -SectionId 1 -Payload $TypeSection

    $ImportSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $ImportSection -Value 2
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_set_location'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 0
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_add_location_offset'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 0
    Add-WasmSection -Module $Module -SectionId 2 -Payload $ImportSection

    $FunctionSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $FunctionSection -Value 2
    Add-WasmU32Leb -Bytes $FunctionSection -Value 1
    Add-WasmU32Leb -Bytes $FunctionSection -Value 2
    Add-WasmSection -Module $Module -SectionId 3 -Payload $FunctionSection

    $ExportSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $ExportSection -Value 2
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_begin_play'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 2
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_tick'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 3
    Add-WasmSection -Module $Module -SectionId 7 -Payload $ExportSection

    $BeginPlayBody = New-CSharpLifecycleFunctionBody -Calls $BeginPlayCalls -AllowDeltaSeconds $false
    $TickBody = New-CSharpLifecycleFunctionBody -Calls $TickCalls -AllowDeltaSeconds $true

    $CodeSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $CodeSection -Value 2
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$BeginPlayBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $BeginPlayBody.ToArray()
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$TickBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $TickBody.ToArray()
    Add-WasmSection -Module $Module -SectionId 10 -Payload $CodeSection

    return ,(Convert-WasmByteListToArray -Bytes $Module)
}

function Invoke-CSharpSourceAdapter {
    param([object[]]$Diagnostics = @())

    $AdapterDiagnostics = @($Diagnostics)
    $AdapterWasmPath = Join-Path $OutputRoot "$ArtifactStem.csharp_adapter.wasm"

    try {
        if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
            throw "C# source file is missing: $SourcePath"
        }

        $SourceText = [System.IO.File]::ReadAllText($SourcePath)
        $BeginPlayBody = Get-CSharpMethodBody -SourceText $SourceText -MethodName 'BeginPlay'
        $TickBody = Get-CSharpMethodBody -SourceText $SourceText -MethodName 'Tick'
        $BeginPlayCalls = @(Get-CSharpActorCalls -MethodBody $BeginPlayBody -MethodName 'BeginPlay')
        $TickCalls = @(Get-CSharpActorCalls -MethodBody $TickBody -MethodName 'Tick')
        $WasmBytes = New-CSharpDirectAbiWasmModule -BeginPlayCalls $BeginPlayCalls -TickCalls $TickCalls

        [System.IO.File]::WriteAllBytes($AdapterWasmPath, $WasmBytes)
        $ObservedExports = @(Get-WasmExports -Path $AdapterWasmPath)
        $ObservedNames = @($ObservedExports | ForEach-Object { $_.name })
        $RequiredExports = @('avid_on_begin_play', 'avid_on_tick')
        $MissingExports = @($RequiredExports | Where-Object { $ObservedNames -notcontains $_ })
        if ($MissingExports.Count -gt 0) {
            throw "C# source adapter produced a WASM file without required exports: $($MissingExports -join ',')"
        }

        $ArtifactHash = Get-Sha256Hex -Path $AdapterWasmPath
        $Manifest = [ordered]@{
            schema_version = 1
            module_id = $ModuleId
            abi_version = 1
            language = 'csharp'
            source = [ordered]@{
                file = Convert-ToProjectRelativePath -Path $SourcePath
                compiler = 'avidscript-csharp-source-adapter'
                subset = 'actor_lifecycle_v2'
            }
            wasm = [ordered]@{
                file = Convert-ToProjectRelativePath -Path $AdapterWasmPath
                sha256 = $ArtifactHash
            }
            required_exports = $RequiredExports
            required_imports = @(
                [ordered]@{
                    module = 'env'
                    name = 'actor_set_location'
                },
                [ordered]@{
                    module = 'env'
                    name = 'actor_add_location_offset'
                }
            )
            toolchain = [ordered]@{
                compiler = 'avidscript-csharp-source-adapter'
                target = 'wasm32-direct-abi'
                direct_abi = $true
            }
            adapter_contract = [ordered]@{
                self_slot = 1
                self_generation = 1
                supported_calls = @('Actor.SetLocation(float x, float y, float z)', 'Actor.AddLocationOffset(float x, float y, float z)')
                supported_tick_expressions = @('numeric literal', 'deltaSeconds', 'deltaSeconds * numeric literal', 'addition of supported expressions')
            }
        }

        $ManifestJson = $Manifest | ConvertTo-Json -Depth 10
        [System.IO.File]::WriteAllText($ManifestPath, $ManifestJson + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

        $AdapterDiagnostics += [ordered]@{
            code = 'source_adapter_used'
            message = 'Built direct ABI WASM from the limited C# ActorLifecycle source subset with Actor.SetLocation and Actor.AddLocationOffset support.'
        }

        return [PSCustomObject]@{
            Succeeded = $true
            WasmPath = $AdapterWasmPath
            ManifestPath = $ManifestPath
            ObservedExports = @($ObservedExports)
            Diagnostics = @($AdapterDiagnostics)
            Sha256 = $ArtifactHash
        }
    }
    catch {
        $AdapterDiagnostics += [ordered]@{
            code = 'source_adapter_failed'
            message = $_.Exception.Message
            stack = $_.ScriptStackTrace
        }

        return [PSCustomObject]@{
            Succeeded = $false
            WasmPath = ''
            ManifestPath = ''
            ObservedExports = @()
            Diagnostics = @($AdapterDiagnostics)
            Sha256 = ''
        }
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
        [object[]]$WorkloadList = @(),
        [string]$Compiler = "dotnet-wasi"
    )

    $ReportDirectory = Split-Path -Parent $ReportPath
    New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null

    $Report = [ordered]@{
        schema_version = 1
        language = "csharp"
        module_id = $ModuleId
        result = $Result
        direct_abi_supported = $DirectAbiSupported
        source = [ordered]@{
            project = Convert-ToProjectRelativePath -Path $ProjectPath
            file = Convert-ToProjectRelativePath -Path $SourcePath
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
            },
            [ordered]@{
                module = "env"
                name = "actor_add_location_offset"
            }
        )
        observed_exports = @($ObservedExports | ForEach-Object { $_.name })
        artifacts = [ordered]@{
            wasm_file = if ([string]::IsNullOrWhiteSpace($WasmPath)) { "" } else { Convert-ToProjectRelativePath -Path $WasmPath }
            manifest_file = if ([string]::IsNullOrWhiteSpace($ManifestPath)) { "" } else { Convert-ToProjectRelativePath -Path $ManifestPath }
            report_file = Convert-ToProjectRelativePath -Path $ReportPath
        }
        toolchain = [ordered]@{
            compiler = $Compiler
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
$DefaultProjectPath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\AvidScript.ActorLifecycle.csproj"
$DefaultSourcePath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"

if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    $ProjectPath = $DefaultProjectPath
}
if ([string]::IsNullOrWhiteSpace($SourcePath)) {
    $SourcePath = $DefaultSourcePath
}
if ([string]::IsNullOrWhiteSpace($ModuleId)) {
    $ModuleId = "csharp_actor_lifecycle"
}
if ([string]::IsNullOrWhiteSpace($ArtifactStem)) {
    $ArtifactStem = "actor_lifecycle"
}
if ($ArtifactStem.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
    throw "ArtifactStem contains invalid file name characters: $ArtifactStem"
}

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $ProjectRoot "Saved\AvidScriptCSharpGuest\ActorLifecycle"
}

if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $OutputRoot "$ArtifactStem.csharp.report.json"
}
if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $OutputRoot "$ArtifactStem.avidscript.json"
}
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
if ($DotNet.Found) {
    Write-Output "[AvidScript][CSharp][Toolchain] dotnet=FOUND source=$($DotNet.Source) path=$($DotNet.Path)"
}
else {
    $Diagnostics += [ordered]@{
        code = "dotnet_missing"
        message = "dotnet was not found"
        checked = @($DotNet.Checked)
    }
}

$SourceAdapterResult = Invoke-CSharpSourceAdapter -Diagnostics $Diagnostics
if ($SourceAdapterResult.Succeeded) {
    Write-Report -Result "direct_abi_built" -DirectAbiSupported $true -Diagnostics $SourceAdapterResult.Diagnostics -ObservedExports $SourceAdapterResult.ObservedExports -WasmPath $SourceAdapterResult.WasmPath -ManifestPath $SourceAdapterResult.ManifestPath -Compiler "avidscript-csharp-source-adapter"
    Write-Output "[AvidScript][CSharp][Build] result=direct_abi_built compiler=avidscript-csharp-source-adapter manifest=$($SourceAdapterResult.ManifestPath) wasm=$($SourceAdapterResult.WasmPath) sha256=$($SourceAdapterResult.Sha256)"
    exit 0
}

$Diagnostics = @($SourceAdapterResult.Diagnostics)
if (-not $DotNet.Found) {
    Write-Report -Result "missing_toolchain" -DirectAbiSupported $false -Diagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Build] result=missing_toolchain missing=dotnet report=$ReportPath"
    exit 0
}

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
    $PublishOutput = @(& $DotNet.Path publish $ProjectPath -c $Configuration -v:minimal --configfile $NuGetConfigPath --ignore-failed-sources -o $PublishRoot "-p:BaseOutputPath=$BinaryRootForMsBuild" "-p:BaseIntermediateOutputPath=$IntermediateRootForMsBuild" "-p:RestoreIgnoreFailedSources=true" 2>&1)
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

$CopiedWasmPath = Join-Path $OutputRoot "$ArtifactStem.dotnet.wasm"
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
    module_id = $ModuleId
    abi_version = 1
    language = "csharp"
    source = [ordered]@{
        file = Convert-ToProjectRelativePath -Path $SourcePath
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
        },
        [ordered]@{
            module = "env"
            name = "actor_add_location_offset"
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
