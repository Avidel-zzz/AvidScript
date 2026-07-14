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

function Get-CSharpFrontendScriptTypes {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Declarations,
        [object[]]$Results = @()
    )

    $Collected = @($Results)
    foreach ($Declaration in @($Declarations)) {
        if ($Declaration.kind -in @('ClassDeclaration', 'StructDeclaration', 'RecordDeclaration', 'RecordStructDeclaration')) {
            $Collected += $Declaration
        }
        if ($null -ne $Declaration.members) {
            $Collected = @(Get-CSharpFrontendScriptTypes -Declarations @($Declaration.members) -Results $Collected)
        }
    }

    return $Collected
}

function Find-CSharpFrontendScriptType {
    param(
        [Parameter(Mandatory = $true)]$FrontendModel,
        [Parameter(Mandatory = $true)][string]$SourcePath
    )

    $Types = @(Get-CSharpFrontendScriptTypes -Declarations @($FrontendModel.syntax.declarations))
    $ExpectedName = [System.IO.Path]::GetFileNameWithoutExtension($SourcePath)
    $NamedMatches = @($Types | Where-Object { $_.name -eq $ExpectedName })
    if ($NamedMatches.Count -eq 1) {
        return $NamedMatches[0]
    }
    if ($NamedMatches.Count -gt 1) {
        throw "C# frontend found multiple script types named '$ExpectedName'."
    }

    $LifecycleMatches = @($Types | Where-Object {
        $MethodNames = @($_.members | Where-Object kind -eq 'MethodDeclaration' | ForEach-Object name)
        $MethodNames -contains 'BeginPlay' -and $MethodNames -contains 'Tick'
    })
    if ($LifecycleMatches.Count -eq 1) {
        return $LifecycleMatches[0]
    }

    throw "C# frontend could not select one script type for '$SourcePath'; expected a type named '$ExpectedName' or one unique type containing BeginPlay and Tick."
}

function Get-CSharpFrontendMethod {
    param(
        [Parameter(Mandatory = $true)]$ScriptType,
        [Parameter(Mandatory = $true)][string]$MethodName,
        [switch]$Optional
    )

    $Matches = @($ScriptType.members | Where-Object { $_.kind -eq 'MethodDeclaration' -and $_.name -eq $MethodName })
    if ($Matches.Count -eq 0 -and $Optional) {
        return $null
    }
    if ($Matches.Count -ne 1) {
        throw "C# frontend expected exactly one method '$MethodName' on '$($ScriptType.name)', found $($Matches.Count)."
    }

    return $Matches[0]
}

function Get-CSharpFrontendMethodBody {
    param(
        [Parameter(Mandatory = $true)][string]$SourceText,
        [Parameter(Mandatory = $true)]$Method
    )

    if ($null -eq $Method.body -or $Method.body.kind -ne 'Block') {
        throw "C# frontend method '$($Method.name)' must use a block body in the current Guest IR subset."
    }

    $Start = [int]$Method.body.span.start
    $Length = [int]$Method.body.span.length
    if ($Length -lt 2 -or $Start -lt 0 -or ($Start + $Length) -gt $SourceText.Length) {
        throw "C# frontend method '$($Method.name)' reported an invalid body span."
    }

    return $SourceText.Substring($Start + 1, $Length - 2)
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

function Convert-CSharpFrontendFloatInitializer {
    param($Initializer)

    if ($null -eq $Initializer) {
        return [single]0
    }
    if ($Initializer.kind -eq 'NumericLiteralExpression') {
        return Convert-CSharpNumericLiteral -Expression ([string]$Initializer.text)
    }
    if ($Initializer.kind -eq 'ParenthesizedExpression' -and @($Initializer.children).Count -eq 1) {
        return Convert-CSharpFrontendFloatInitializer -Initializer $Initializer.children[0]
    }
    if ($Initializer.kind -eq 'UnaryMinusExpression' -and $Initializer.operator -eq '-' -and @($Initializer.children).Count -eq 1) {
        return -1.0f * (Convert-CSharpFrontendFloatInitializer -Initializer $Initializer.children[0])
    }

    throw "Unsupported static float initializer syntax '$($Initializer.kind)'."
}

function Get-CSharpFrontendStaticFloatFields {
    param([Parameter(Mandatory = $true)]$ScriptType)

    $Fields = @()
    foreach ($Declaration in @($ScriptType.members | Where-Object {
        $_.kind -eq 'FieldDeclaration' -and $_.type -eq 'float' -and
        @($_.modifiers) -contains 'private' -and @($_.modifiers) -contains 'static'
    })) {
        $Name = [string]$Declaration.name
        foreach ($ExistingField in $Fields) {
            if ($ExistingField.Name -eq $Name) {
                throw "C# frontend found duplicate static float field '$Name'."
            }
        }

        $Fields += [PSCustomObject]@{
            Name = $Name
            InitialValue = Convert-CSharpFrontendFloatInitializer -Initializer $Declaration.initializer
            Index = $Fields.Count
        }
    }

    return $Fields
}

function Find-CSharpStaticFloatField {
    param(
        [object[]]$Fields = @(),
        [Parameter(Mandatory = $true)][string]$Name
    )

    foreach ($Field in @($Fields)) {
        if ($Field.Name -eq $Name) {
            return $Field
        }
    }

    return $null
}

function Convert-CSharpFloatExpression {
    param(
        [Parameter(Mandatory = $true)][string]$Expression,
        [object[]]$Fields = @(),
        [string]$CallbackInputParameterName = ''
    )

    $Trimmed = $Expression.Trim()
    if (-not [string]::IsNullOrWhiteSpace($CallbackInputParameterName)) {
        $InputMemberPattern = '^' + [regex]::Escape($CallbackInputParameterName) + '\s*\.\s*(?<member>ActionId|TriggerEvent)$'
        $InputMemberMatch = [regex]::Match($Trimmed, $InputMemberPattern)
        if ($InputMemberMatch.Success) {
            $Kind = if ($InputMemberMatch.Groups['member'].Value -eq 'ActionId') { 'gameplay_primary_id' } else { 'gameplay_secondary_id' }
            return [PSCustomObject]@{ Kind = $Kind; Value = [single]0 }
        }
    }

    $AddMatch = [regex]::Match($Trimmed, '^(?<left>.+?)\s*\+\s*(?<right>.+)$')
    if ($AddMatch.Success) {
        return [PSCustomObject]@{
            Kind = 'add'
            Left = Convert-CSharpFloatExpression -Expression $AddMatch.Groups['left'].Value -Fields $Fields -CallbackInputParameterName $CallbackInputParameterName
            Right = Convert-CSharpFloatExpression -Expression $AddMatch.Groups['right'].Value -Fields $Fields -CallbackInputParameterName $CallbackInputParameterName
        }
    }

    $MultiplyMatch = [regex]::Match($Trimmed, '^(?<left>.+?)\s*\*\s*(?<right>.+)$')
    if ($MultiplyMatch.Success) {
        return [PSCustomObject]@{
            Kind = 'mul'
            Left = Convert-CSharpFloatExpression -Expression $MultiplyMatch.Groups['left'].Value -Fields $Fields -CallbackInputParameterName $CallbackInputParameterName
            Right = Convert-CSharpFloatExpression -Expression $MultiplyMatch.Groups['right'].Value -Fields $Fields -CallbackInputParameterName $CallbackInputParameterName
        }
    }

    if ($Trimmed -eq 'deltaSeconds') {
        return [PSCustomObject]@{
            Kind = 'delta'
            Value = [single]0
        }
    }

    if ($Trimmed -eq 'value') {
        return [PSCustomObject]@{
            Kind = 'event_value'
            Value = [single]0
        }
    }

    if ($Trimmed -match '^[A-Za-z_][A-Za-z0-9_]*$') {
        $Field = Find-CSharpStaticFloatField -Fields $Fields -Name $Trimmed
        if ($null -ne $Field) {
            return [PSCustomObject]@{
                Kind = 'field'
                Name = $Field.Name
                Index = $Field.Index
            }
        }

        throw "Unsupported C# float identifier '$Trimmed'. Supported identifiers are deltaSeconds and private static float fields."
    }

    return [PSCustomObject]@{
        Kind = 'constant'
        Value = Convert-CSharpNumericLiteral -Expression $Trimmed
    }
}

function Find-CSharpValueLocal {
    param(
        [object[]]$ValueLocals = @(),
        [Parameter(Mandatory = $true)][string]$Name
    )

    foreach ($Local in @($ValueLocals)) {
        if ($Local.Name -eq $Name) {
            return $Local
        }
    }

    return $null
}

function Convert-CSharpValueExpression {
    param(
        [Parameter(Mandatory = $true)][string]$Expression,
        [Parameter(Mandatory = $true)][ValidateSet('FVector', 'FRotator')][string]$ValueType,
        [object[]]$Fields = @(),
        [object[]]$ValueLocals = @(),
        [string]$CallbackVectorParameterName = '',
        [string]$CallbackInputParameterName = ''
    )

    $Trimmed = $Expression.Trim()
    $AddMatch = [regex]::Match($Trimmed, '^(?<left>[A-Za-z_][A-Za-z0-9_]*(?:\s*\.\s*Value)?)\s*\+\s*(?<right>.+)$')
    if ($AddMatch.Success) {
        return [PSCustomObject]@{
            Kind = 'value_add'
            ValueType = $ValueType
            Left = Convert-CSharpValueExpression -Expression $AddMatch.Groups['left'].Value -ValueType $ValueType -Fields $Fields -ValueLocals $ValueLocals -CallbackVectorParameterName $CallbackVectorParameterName -CallbackInputParameterName $CallbackInputParameterName
            Right = Convert-CSharpValueExpression -Expression $AddMatch.Groups['right'].Value -ValueType $ValueType -Fields $Fields -ValueLocals $ValueLocals -CallbackVectorParameterName $CallbackVectorParameterName -CallbackInputParameterName $CallbackInputParameterName
        }
    }

    if ($Trimmed -match ('^' + [regex]::Escape($ValueType) + '\s*\.\s*Zero$')) {
        $Zero = [PSCustomObject]@{ Kind = 'constant'; Value = [single]0 }
        return [PSCustomObject]@{ Kind = 'value_construct'; ValueType = $ValueType; A = $Zero; B = $Zero; C = $Zero }
    }

    $ConstructorPattern = '^new\s+' + [regex]::Escape($ValueType) + '\s*\(\s*(?<a>[^,]+?)\s*,\s*(?<b>[^,]+?)\s*,\s*(?<c>.+?)\s*\)$'
    $ConstructorMatch = [regex]::Match($Trimmed, $ConstructorPattern)
    if ($ConstructorMatch.Success) {
        return [PSCustomObject]@{
            Kind = 'value_construct'
            ValueType = $ValueType
            A = Convert-CSharpFloatExpression -Expression $ConstructorMatch.Groups['a'].Value -Fields $Fields -CallbackInputParameterName $CallbackInputParameterName
            B = Convert-CSharpFloatExpression -Expression $ConstructorMatch.Groups['b'].Value -Fields $Fields -CallbackInputParameterName $CallbackInputParameterName
            C = Convert-CSharpFloatExpression -Expression $ConstructorMatch.Groups['c'].Value -Fields $Fields -CallbackInputParameterName $CallbackInputParameterName
        }
    }

    if ($ValueType -eq 'FVector' -and $Trimmed -match '^UE\s*\.\s*Self\s*\.\s*GetActorLocation\s*\(\s*\)$') {
        return [PSCustomObject]@{ Kind = 'get_actor_location'; ValueType = $ValueType }
    }

    if ($ValueType -eq 'FRotator' -and $Trimmed -match '^UE\s*\.\s*Self\s*\.\s*GetActorRotation\s*\(\s*\)$') {
        return [PSCustomObject]@{ Kind = 'get_actor_rotation'; ValueType = $ValueType }
    }

    if ($ValueType -eq 'FVector' -and $Trimmed -match '^UE\s*\.\s*Self\s*\.\s*GetActorScale3D\s*\(\s*\)$') {
        return [PSCustomObject]@{ Kind = 'get_actor_scale'; ValueType = $ValueType }
    }
    if ($ValueType -eq 'FVector' -and $Trimmed -match '^UE\s*\.\s*Self\s*\.\s*GetRootComponent\s*\(\s*\)\s*\.\s*GetWorldLocation\s*\(\s*\)$') {
        return [PSCustomObject]@{ Kind = 'get_root_component_world_location'; ValueType = $ValueType }
    }

    if ($ValueType -eq 'FVector' -and -not [string]::IsNullOrWhiteSpace($CallbackVectorParameterName) -and $Trimmed -eq $CallbackVectorParameterName) {
        return [PSCustomObject]@{ Kind = 'callback_vector'; ValueType = $ValueType }
    }

    if ($Trimmed -match '^[A-Za-z_][A-Za-z0-9_]*$') {
        $Local = Find-CSharpValueLocal -ValueLocals $ValueLocals -Name $Trimmed
        if ($null -ne $Local) {
            if ($Local.ValueType -ne $ValueType) {
                throw "C# value local '$Trimmed' is $($Local.ValueType), not $ValueType."
            }

            return [PSCustomObject]@{ Kind = 'value_local'; ValueType = $ValueType; Name = $Local.Name; Ordinal = $Local.Ordinal }
        }
    }

    throw "Unsupported C# $ValueType expression '$Trimmed'."
}

function Get-CSharpLifecycleStatements {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$MethodBody,
        [Parameter(Mandatory = $true)][string]$MethodName,
        [object[]]$Fields = @(),
        [bool]$AllowEmpty = $false,
        [string]$CallbackActorParameterName = '',
        [string]$CallbackVectorParameterName = '',
        [string]$CallbackInputParameterName = ''
    )

    if ($MethodName -ne 'OnEvent' -and [regex]::IsMatch($MethodBody, '\bvalue\b')) {
        throw "The C# event payload identifier 'value' is only available inside OnEvent(int eventId, float value)."
    }

    $Statements = @()
    $ValueLocals = @()
    foreach ($Match in [regex]::Matches($MethodBody, '(?s)(?<statement>[^;]+);')) {
        $StatementText = $Match.Groups['statement'].Value.Trim()
        if ([string]::IsNullOrWhiteSpace($StatementText)) {
            continue
        }

        $ValueDeclarationMatch = [regex]::Match($StatementText, '^(?<type>FVector|FRotator)\s+(?<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?<expr>.+)$')
        if ($ValueDeclarationMatch.Success) {
            $ValueType = $ValueDeclarationMatch.Groups['type'].Value
            $LocalName = $ValueDeclarationMatch.Groups['name'].Value
            if ($null -ne (Find-CSharpValueLocal -ValueLocals $ValueLocals -Name $LocalName)) {
                throw "Duplicate C# value local '$LocalName' in '$MethodName'."
            }

            $Expression = Convert-CSharpValueExpression -Expression $ValueDeclarationMatch.Groups['expr'].Value -ValueType $ValueType -Fields $Fields -ValueLocals $ValueLocals -CallbackVectorParameterName $CallbackVectorParameterName -CallbackInputParameterName $CallbackInputParameterName
            $Local = [PSCustomObject]@{ Name = $LocalName; ValueType = $ValueType; Ordinal = $ValueLocals.Count }
            $ValueLocals += $Local
            $Statements += [PSCustomObject]@{
                Kind = 'value_local_assign'
                ValueType = $ValueType
                LocalOrdinal = $Local.Ordinal
                Expression = $Expression
            }
            continue
        }

        $TimerSetMatch = [regex]::Match($StatementText, '^UE\s*\.\s*SetTimer\s*\(\s*(?<delay>[^,]+?)\s*,\s*(?<callback>[0-9]+)\s*\)$')
        if ($TimerSetMatch.Success) {
            $Statements += [PSCustomObject]@{
                Kind = 'timer_set_once'
                Delay = Convert-CSharpFloatExpression -Expression $TimerSetMatch.Groups['delay'].Value -Fields $Fields
                CallbackId = [int]::Parse($TimerSetMatch.Groups['callback'].Value, [System.Globalization.CultureInfo]::InvariantCulture)
            }
            continue
        }

        if (-not [string]::IsNullOrWhiteSpace($CallbackActorParameterName)) {
            $CallbackActorPattern = '^' + [regex]::Escape($CallbackActorParameterName) + '\s*\.\s*(?<method>SetActorLocation|AddActorWorldOffset|SetActorRotation|SetActorScale3D)\s*\(\s*(?<expr>.+)\s*\)$'
            $CallbackActorCall = [regex]::Match($StatementText, $CallbackActorPattern)
            if ($CallbackActorCall.Success) {
                $Method = $CallbackActorCall.Groups['method'].Value
                $ValueType = if ($Method -eq 'SetActorRotation') { 'FRotator' } else { 'FVector' }
                $Kind = switch ($Method) {
                    'SetActorLocation' { 'set_location_value' }
                    'AddActorWorldOffset' { 'add_location_offset_value' }
                    'SetActorRotation' { 'set_rotation_value' }
                    'SetActorScale3D' { 'set_scale_value' }
                }
                $Statements += [PSCustomObject]@{
                    Kind = $Kind
                    ValueType = $ValueType
                    UseGameplayEventHandle = $true
                    ValueExpression = Convert-CSharpValueExpression -Expression $CallbackActorCall.Groups['expr'].Value -ValueType $ValueType -Fields $Fields -ValueLocals $ValueLocals -CallbackVectorParameterName $CallbackVectorParameterName -CallbackInputParameterName $CallbackInputParameterName
                }
                continue
            }
        }

        $TypedValueCall = [regex]::Match($StatementText, '^UE\s*\.\s*Self\s*\.\s*(?<method>SetActorLocation|AddActorWorldOffset|SetActorRotation|SetActorScale3D)\s*\(\s*(?<expr>.+)\s*\)$')
        if ($TypedValueCall.Success) {
            $Method = $TypedValueCall.Groups['method'].Value
            $ValueType = if ($Method -eq 'SetActorRotation') { 'FRotator' } else { 'FVector' }
            $Kind = switch ($Method) {
                'SetActorLocation' { 'set_location_value' }
                'AddActorWorldOffset' { 'add_location_offset_value' }
                'SetActorRotation' { 'set_rotation_value' }
                'SetActorScale3D' { 'set_scale_value' }
            }
            $Statements += [PSCustomObject]@{
                Kind = $Kind
                ValueType = $ValueType
                UseGameplayEventHandle = $false
                ValueExpression = Convert-CSharpValueExpression -Expression $TypedValueCall.Groups['expr'].Value -ValueType $ValueType -Fields $Fields -ValueLocals $ValueLocals -CallbackVectorParameterName $CallbackVectorParameterName -CallbackInputParameterName $CallbackInputParameterName
            }
            continue
        }

        $SceneComponentCall = [regex]::Match($StatementText, '^UE\s*\.\s*Self\s*\.\s*GetRootComponent\s*\(\s*\)\s*\.\s*SetWorldLocation\s*\(\s*(?<expr>.+)\s*\)$')
        if ($SceneComponentCall.Success) {
            $Statements += [PSCustomObject]@{
                Kind = 'set_root_component_world_location'
                ValueType = 'FVector'
                ValueExpression = Convert-CSharpValueExpression -Expression $SceneComponentCall.Groups['expr'].Value -ValueType 'FVector' -Fields $Fields -ValueLocals $ValueLocals -CallbackVectorParameterName $CallbackVectorParameterName -CallbackInputParameterName $CallbackInputParameterName
            }
            continue
        }

        $ActorMatch = [regex]::Match($StatementText, '^Actor\s*\.\s*(?<method>SetLocation|AddLocationOffset)\s*\(\s*(?<x>[^,]+?)\s*,\s*(?<y>[^,]+?)\s*,\s*(?<z>[^\)]+?)\s*\)$')
        if ($ActorMatch.Success) {
            $Method = $ActorMatch.Groups['method'].Value
            $Statements += [PSCustomObject]@{
                Kind = if ($Method -eq 'SetLocation') { 'set_location' } else { 'add_location_offset' }
                UseOwnerHandle = $false
                X = Convert-CSharpFloatExpression -Expression $ActorMatch.Groups['x'].Value -Fields $Fields
                Y = Convert-CSharpFloatExpression -Expression $ActorMatch.Groups['y'].Value -Fields $Fields
                Z = Convert-CSharpFloatExpression -Expression $ActorMatch.Groups['z'].Value -Fields $Fields
            }
            continue
        }

        $AddAssignMatch = [regex]::Match($StatementText, '^(?<field>[A-Za-z_][A-Za-z0-9_]*)\s*\+=\s*(?<expr>.+)$')
        if ($AddAssignMatch.Success) {
            $FieldName = $AddAssignMatch.Groups['field'].Value
            $Field = Find-CSharpStaticFloatField -Fields $Fields -Name $FieldName
            if ($null -eq $Field) {
                throw "C# source adapter found assignment to unsupported field '$FieldName' in '$MethodName'. Declare it as private static float."
            }

            $Statements += [PSCustomObject]@{
                Kind = 'field_add_assign'
                FieldName = $Field.Name
                FieldIndex = $Field.Index
                Expression = Convert-CSharpFloatExpression -Expression $AddAssignMatch.Groups['expr'].Value -Fields $Fields
            }
            continue
        }

        $AssignMatch = [regex]::Match($StatementText, '^(?<field>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?<expr>.+)$')
        if ($AssignMatch.Success) {
            $FieldName = $AssignMatch.Groups['field'].Value
            $Field = Find-CSharpStaticFloatField -Fields $Fields -Name $FieldName
            if ($null -eq $Field) {
                throw "C# source adapter found assignment to unsupported field '$FieldName' in '$MethodName'. Declare it as private static float."
            }

            $Statements += [PSCustomObject]@{
                Kind = 'field_assign'
                FieldName = $Field.Name
                FieldIndex = $Field.Index
                Expression = Convert-CSharpFloatExpression -Expression $AssignMatch.Groups['expr'].Value -Fields $Fields
            }
            continue
        }

        throw "Unsupported C# statement in '$MethodName': $StatementText. Supported statements: private static float field assignment, UE.Self typed Actor calls, Actor.SetLocation, Actor.AddLocationOffset."
    }

    if ($Statements.Count -eq 0 -and -not $AllowEmpty) {
        throw "C# source adapter found no supported statements in '$MethodName'. Supported statements: field assignment, UE.Self typed Actor calls, Actor.SetLocation and Actor.AddLocationOffset."
    }

    return $Statements
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

    if ($Expression.Kind -eq 'field') {
        Add-WasmByte -Bytes $Body -Value 0x23
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]$Expression.Index)
        return
    }

    if ($Expression.Kind -eq 'add') {
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Expression.Left -AllowDeltaSeconds $AllowDeltaSeconds
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Expression.Right -AllowDeltaSeconds $AllowDeltaSeconds
        Add-WasmByte -Bytes $Body -Value 0x92
        return
    }

    if ($Expression.Kind -eq 'mul') {
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Expression.Left -AllowDeltaSeconds $AllowDeltaSeconds
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Expression.Right -AllowDeltaSeconds $AllowDeltaSeconds
        Add-WasmByte -Bytes $Body -Value 0x94
        return
    }

    if ($Expression.Kind -eq 'gameplay_primary_id' -or $Expression.Kind -eq 'gameplay_secondary_id') {
        Add-WasmByte -Bytes $Body -Value 0x20
        $ParameterIndex = if ($Expression.Kind -eq 'gameplay_primary_id') { 1 } else { 2 }
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]$ParameterIndex)
        Add-WasmByte -Bytes $Body -Value 0xb2
        return
    }

    if ($Expression.Kind -eq 'event_value') {
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value 1
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

    throw "Unsupported C# expression kind '$($Expression.Kind)'."
}

function Add-CSharpValueComponentCode {
    param(
        [Parameter(Mandatory = $true)]$Body,
        [Parameter(Mandatory = $true)]$Expression,
        [Parameter(Mandatory = $true)][int]$ComponentIndex,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds,
        [Parameter(Mandatory = $true)][int]$ValueLocalBase
    )

    if ($Expression.Kind -eq 'value_construct') {
        $Components = @($Expression.A, $Expression.B, $Expression.C)
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Components[$ComponentIndex] -AllowDeltaSeconds $AllowDeltaSeconds
        return
    }

    if ($Expression.Kind -eq 'value_local') {
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]($ValueLocalBase + (3 * $Expression.Ordinal) + $ComponentIndex))
        return
    }

    if ($Expression.Kind -eq 'callback_vector') {
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value ([uint32](5 + $ComponentIndex))
        return
    }

    if ($Expression.Kind -eq 'value_add') {
        Add-CSharpValueComponentCode -Body $Body -Expression $Expression.Left -ComponentIndex $ComponentIndex -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
        Add-CSharpValueComponentCode -Body $Body -Expression $Expression.Right -ComponentIndex $ComponentIndex -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
        Add-WasmByte -Bytes $Body -Value 0x92
        return
    }

    throw "Unsupported $($Expression.ValueType) component expression kind '$($Expression.Kind)'."
}

function Add-CSharpValueLocalAssignmentCode {
    param(
        [Parameter(Mandatory = $true)]$Body,
        [Parameter(Mandatory = $true)]$Statement,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds,
        [Parameter(Mandatory = $true)][int]$ValueLocalBase
    )

    if ($Statement.Expression.Kind -eq 'get_root_component_world_location') {
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 2
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 3
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value 16
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 9
        Add-WasmByte -Bytes $Body -Value 0x1a
        foreach ($HandleAddress in @(16, 20)) {
            Add-WasmByte -Bytes $Body -Value 0x41
            Add-WasmU32Leb -Bytes $Body -Value ([uint32]$HandleAddress)
            Add-WasmByte -Bytes $Body -Value 0x28
            Add-WasmU32Leb -Bytes $Body -Value 2
            Add-WasmU32Leb -Bytes $Body -Value 0
        }
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value 24
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 10
        Add-WasmByte -Bytes $Body -Value 0x1a
        for ($ComponentIndex = 0; $ComponentIndex -lt 3; ++$ComponentIndex) {
            Add-WasmByte -Bytes $Body -Value 0x41
            Add-WasmU32Leb -Bytes $Body -Value ([uint32](24 + (4 * $ComponentIndex)))
            Add-WasmByte -Bytes $Body -Value 0x2a
            Add-WasmU32Leb -Bytes $Body -Value 2
            Add-WasmU32Leb -Bytes $Body -Value 0
            Add-WasmByte -Bytes $Body -Value 0x21
            Add-WasmU32Leb -Bytes $Body -Value ([uint32]($ValueLocalBase + (3 * $Statement.LocalOrdinal) + $ComponentIndex))
        }
        return
    }
    if ($Statement.Expression.Kind -eq 'get_actor_location' -or $Statement.Expression.Kind -eq 'get_actor_rotation' -or $Statement.Expression.Kind -eq 'get_actor_scale') {
        $FunctionIndex = if ($Statement.Expression.Kind -eq 'get_actor_location') { 4 } elseif ($Statement.Expression.Kind -eq 'get_actor_rotation') { 6 } else { 8 }
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 2
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 3
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value 16
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value $FunctionIndex
        Add-WasmByte -Bytes $Body -Value 0x1a

        for ($ComponentIndex = 0; $ComponentIndex -lt 3; ++$ComponentIndex) {
            Add-WasmByte -Bytes $Body -Value 0x41
            Add-WasmU32Leb -Bytes $Body -Value ([uint32](16 + (4 * $ComponentIndex)))
            Add-WasmByte -Bytes $Body -Value 0x2a
            Add-WasmU32Leb -Bytes $Body -Value 2
            Add-WasmU32Leb -Bytes $Body -Value 0
            Add-WasmByte -Bytes $Body -Value 0x21
            Add-WasmU32Leb -Bytes $Body -Value ([uint32]($ValueLocalBase + (3 * $Statement.LocalOrdinal) + $ComponentIndex))
        }
        return
    }

    for ($ComponentIndex = 0; $ComponentIndex -lt 3; ++$ComponentIndex) {
        Add-CSharpValueComponentCode -Body $Body -Expression $Statement.Expression -ComponentIndex $ComponentIndex -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
        Add-WasmByte -Bytes $Body -Value 0x21
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]($ValueLocalBase + (3 * $Statement.LocalOrdinal) + $ComponentIndex))
    }
}

function Add-CSharpTypedActorValueCall {
    param(
        [Parameter(Mandatory = $true)]$Body,
        [Parameter(Mandatory = $true)]$Statement,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds,
        [Parameter(Mandatory = $true)][int]$ValueLocalBase
    )

    $FunctionIndex = switch ($Statement.Kind) {
        'set_location_value' { 0 }
        'add_location_offset_value' { 1 }
        'set_rotation_value' { 5 }
        'set_scale_value' { 7 }
        default { throw "Unsupported typed Actor value call '$($Statement.Kind)'." }
    }
    if ($Statement.UseGameplayEventHandle) {
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value 3
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value 4
    }
    else {
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 2
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 3
    }
    for ($ComponentIndex = 0; $ComponentIndex -lt 3; ++$ComponentIndex) {
        Add-CSharpValueComponentCode -Body $Body -Expression $Statement.ValueExpression -ComponentIndex $ComponentIndex -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
    }
    Add-WasmByte -Bytes $Body -Value 0x10
    Add-WasmU32Leb -Bytes $Body -Value $FunctionIndex
    Add-WasmByte -Bytes $Body -Value 0x1a
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

    if ($Call.UseOwnerHandle) {
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 2
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 3
    }
    else {
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value 1
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value 1
    }
    Add-CSharpFloatExpressionCode -Body $Body -Expression $Call.X -AllowDeltaSeconds $AllowDeltaSeconds
    Add-CSharpFloatExpressionCode -Body $Body -Expression $Call.Y -AllowDeltaSeconds $AllowDeltaSeconds
    Add-CSharpFloatExpressionCode -Body $Body -Expression $Call.Z -AllowDeltaSeconds $AllowDeltaSeconds
    Add-WasmByte -Bytes $Body -Value 0x10
    Add-WasmU32Leb -Bytes $Body -Value $FunctionIndex
    Add-WasmByte -Bytes $Body -Value 0x1a
}

function Add-CSharpLifecycleStatementCode {
    param(
        [Parameter(Mandatory = $true)]$Body,
        [Parameter(Mandatory = $true)]$Statement,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds,
        [Parameter(Mandatory = $true)][int]$ValueLocalBase
    )

    if ($Statement.Kind -eq 'timer_set_once') {
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Statement.Delay -AllowDeltaSeconds $AllowDeltaSeconds
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]$Statement.CallbackId)
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 12
        Add-WasmByte -Bytes $Body -Value 0x1a
        return
    }

    if ($Statement.Kind -eq 'value_local_assign') {
        Add-CSharpValueLocalAssignmentCode -Body $Body -Statement $Statement -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
        return
    }

    if ($Statement.Kind -eq 'set_root_component_world_location') {
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 2
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 3
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value 16
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 9
        Add-WasmByte -Bytes $Body -Value 0x1a
        foreach ($HandleAddress in @(16, 20)) {
            Add-WasmByte -Bytes $Body -Value 0x41
            Add-WasmU32Leb -Bytes $Body -Value ([uint32]$HandleAddress)
            Add-WasmByte -Bytes $Body -Value 0x28
            Add-WasmU32Leb -Bytes $Body -Value 2
            Add-WasmU32Leb -Bytes $Body -Value 0
        }
        for ($ComponentIndex = 0; $ComponentIndex -lt 3; ++$ComponentIndex) {
            Add-CSharpValueComponentCode -Body $Body -Expression $Statement.ValueExpression -ComponentIndex $ComponentIndex -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
        }
        Add-WasmByte -Bytes $Body -Value 0x10
        Add-WasmU32Leb -Bytes $Body -Value 11
        Add-WasmByte -Bytes $Body -Value 0x1a
        return
    }
    if ($Statement.Kind -eq 'set_location_value' -or $Statement.Kind -eq 'add_location_offset_value' -or $Statement.Kind -eq 'set_rotation_value' -or $Statement.Kind -eq 'set_scale_value') {
        Add-CSharpTypedActorValueCall -Body $Body -Statement $Statement -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
        return
    }

    if ($Statement.Kind -eq 'field_assign') {
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Statement.Expression -AllowDeltaSeconds $AllowDeltaSeconds
        Add-WasmByte -Bytes $Body -Value 0x24
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]$Statement.FieldIndex)
        return
    }

    if ($Statement.Kind -eq 'field_add_assign') {
        Add-WasmByte -Bytes $Body -Value 0x23
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]$Statement.FieldIndex)
        Add-CSharpFloatExpressionCode -Body $Body -Expression $Statement.Expression -AllowDeltaSeconds $AllowDeltaSeconds
        Add-WasmByte -Bytes $Body -Value 0x92
        Add-WasmByte -Bytes $Body -Value 0x24
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]$Statement.FieldIndex)
        return
    }

    Add-CSharpActorCall -Body $Body -Call $Statement -AllowDeltaSeconds $AllowDeltaSeconds
}

function New-CSharpLifecycleFunctionBody {
    param(
        [Parameter(Mandatory = $true)]$Statements,
        [Parameter(Mandatory = $true)][bool]$AllowDeltaSeconds,
        [int]$ParameterCount = 0
    )

    $Body = New-WasmByteList
    $ValueLocalCount = @($Statements | Where-Object { $_.Kind -eq 'value_local_assign' }).Count
    $ValueLocalBase = $ParameterCount
    if ($ValueLocalCount -gt 0) {
        Add-WasmU32Leb -Bytes $Body -Value 1
        Add-WasmU32Leb -Bytes $Body -Value ([uint32](3 * $ValueLocalCount))
        Add-WasmByte -Bytes $Body -Value 0x7d
    }
    else {
        Add-WasmU32Leb -Bytes $Body -Value 0
    }
    foreach ($Statement in $Statements) {
        Add-CSharpLifecycleStatementCode -Body $Body -Statement $Statement -AllowDeltaSeconds $AllowDeltaSeconds -ValueLocalBase $ValueLocalBase
    }
    Add-WasmByte -Bytes $Body -Value 0x0b
    return ,$Body
}

function New-CSharpGameplayEventFunctionBody {
    param([object[]]$GameplayCallbacks = @())

    $Body = New-WasmByteList
    $MaxValueLocalCount = 0
    foreach ($Callback in @($GameplayCallbacks)) {
        $ValueLocalCount = @($Callback.Statements | Where-Object { $_.Kind -eq 'value_local_assign' }).Count
        $MaxValueLocalCount = [Math]::Max($MaxValueLocalCount, $ValueLocalCount)
    }

    if ($MaxValueLocalCount -gt 0) {
        Add-WasmU32Leb -Bytes $Body -Value 1
        Add-WasmU32Leb -Bytes $Body -Value ([uint32](3 * $MaxValueLocalCount))
        Add-WasmByte -Bytes $Body -Value 0x7d
    }
    else {
        Add-WasmU32Leb -Bytes $Body -Value 0
    }

    foreach ($Callback in @($GameplayCallbacks)) {
        Add-WasmByte -Bytes $Body -Value 0x20
        Add-WasmU32Leb -Bytes $Body -Value 0
        Add-WasmByte -Bytes $Body -Value 0x41
        Add-WasmU32Leb -Bytes $Body -Value ([uint32]$Callback.EventType)
        Add-WasmByte -Bytes $Body -Value 0x46
        Add-WasmByte -Bytes $Body -Value 0x04
        Add-WasmByte -Bytes $Body -Value 0x40
        foreach ($Statement in @($Callback.Statements)) {
            Add-CSharpLifecycleStatementCode -Body $Body -Statement $Statement -AllowDeltaSeconds $false -ValueLocalBase 8
        }
        Add-WasmByte -Bytes $Body -Value 0x0b
    }

    Add-WasmByte -Bytes $Body -Value 0x0b
    return ,$Body
}

function New-CSharpDirectAbiWasmModule {
    param(
        [object[]]$Fields = @(),
        [Parameter(Mandatory = $true)]$BeginPlayStatements,
        [Parameter(Mandatory = $true)]$TickStatements,
        [object[]]$EndPlayStatements = @(),
        [object[]]$TimerStatements = @(),
        [object[]]$EventStatements = @(),
        [object[]]$GameplayCallbacks = @()
    )

    $Module = New-WasmByteList
    Add-WasmBytes -Bytes $Module -Values ([byte[]](0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00))

    $TypeSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $TypeSection -Value 10
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
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 0
    Add-WasmU32Leb -Bytes $TypeSection -Value 1
    Add-WasmByte -Bytes $TypeSection -Value 0x7f
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 3
    Add-WasmBytes -Bytes $TypeSection -Values ([byte[]](0x7f, 0x7f, 0x7f))
    Add-WasmU32Leb -Bytes $TypeSection -Value 1
    Add-WasmByte -Bytes $TypeSection -Value 0x7f
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 2
    Add-WasmBytes -Bytes $TypeSection -Values ([byte[]](0x7d, 0x7f))
    Add-WasmU32Leb -Bytes $TypeSection -Value 1
    Add-WasmByte -Bytes $TypeSection -Value 0x7f
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 1
    Add-WasmByte -Bytes $TypeSection -Value 0x7f
    Add-WasmU32Leb -Bytes $TypeSection -Value 1
    Add-WasmByte -Bytes $TypeSection -Value 0x7f
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 2
    Add-WasmBytes -Bytes $TypeSection -Values ([byte[]](0x7f, 0x7f))
    Add-WasmU32Leb -Bytes $TypeSection -Value 0
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 2
    Add-WasmBytes -Bytes $TypeSection -Values ([byte[]](0x7f, 0x7d))
    Add-WasmU32Leb -Bytes $TypeSection -Value 0
    Add-WasmByte -Bytes $TypeSection -Value 0x60
    Add-WasmU32Leb -Bytes $TypeSection -Value 8
    Add-WasmBytes -Bytes $TypeSection -Values ([byte[]](0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7d, 0x7d, 0x7d))
    Add-WasmU32Leb -Bytes $TypeSection -Value 0
    Add-WasmSection -Module $Module -SectionId 1 -Payload $TypeSection

    $ImportSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $ImportSection -Value 14
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_set_location'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 0
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_add_location_offset'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 0
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'owner_get_slot'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 3
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'owner_get_generation'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 3
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_get_location'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 4
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_set_rotation'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 0
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_get_rotation'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 4
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_set_scale'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 0
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_get_scale'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 4
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'actor_get_root_component'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 4
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'scene_component_get_world_location'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 4
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'scene_component_set_world_location'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 0
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'timer_set_once'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 5
    Add-WasmString -Bytes $ImportSection -Text 'env'
    Add-WasmString -Bytes $ImportSection -Text 'timer_cancel'
    Add-WasmByte -Bytes $ImportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ImportSection -Value 6
    Add-WasmSection -Module $Module -SectionId 2 -Payload $ImportSection

    $FunctionSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $FunctionSection -Value 6
    Add-WasmU32Leb -Bytes $FunctionSection -Value 1
    Add-WasmU32Leb -Bytes $FunctionSection -Value 2
    Add-WasmU32Leb -Bytes $FunctionSection -Value 1
    Add-WasmU32Leb -Bytes $FunctionSection -Value 7
    Add-WasmU32Leb -Bytes $FunctionSection -Value 8
    Add-WasmU32Leb -Bytes $FunctionSection -Value 9
    Add-WasmSection -Module $Module -SectionId 3 -Payload $FunctionSection
    $MemorySection = New-WasmByteList
    Add-WasmU32Leb -Bytes $MemorySection -Value 1
    Add-WasmU32Leb -Bytes $MemorySection -Value 0
    Add-WasmU32Leb -Bytes $MemorySection -Value 1
    Add-WasmSection -Module $Module -SectionId 5 -Payload $MemorySection

    if (@($Fields).Count -gt 0) {
        $GlobalSection = New-WasmByteList
        Add-WasmU32Leb -Bytes $GlobalSection -Value ([uint32]@($Fields).Count)
        foreach ($Field in @($Fields)) {
            Add-WasmByte -Bytes $GlobalSection -Value 0x7d
            Add-WasmByte -Bytes $GlobalSection -Value 0x01
            Add-WasmByte -Bytes $GlobalSection -Value 0x43
            Add-WasmF32 -Bytes $GlobalSection -Value ([single]$Field.InitialValue)
            Add-WasmByte -Bytes $GlobalSection -Value 0x0b
        }
        Add-WasmSection -Module $Module -SectionId 6 -Payload $GlobalSection
    }

    $ExportSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $ExportSection -Value 6
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_begin_play'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 14
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_tick'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 15
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_end_play'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 16
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_timer'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 17
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_event'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 18
    Add-WasmString -Bytes $ExportSection -Text 'avid_on_gameplay_event'
    Add-WasmByte -Bytes $ExportSection -Value 0x00
    Add-WasmU32Leb -Bytes $ExportSection -Value 19
    Add-WasmSection -Module $Module -SectionId 7 -Payload $ExportSection

    $BeginPlayBody = New-CSharpLifecycleFunctionBody -Statements $BeginPlayStatements -AllowDeltaSeconds $false
    $TickBody = New-CSharpLifecycleFunctionBody -Statements $TickStatements -AllowDeltaSeconds $true -ParameterCount 1
    $EndPlayBody = New-CSharpLifecycleFunctionBody -Statements $EndPlayStatements -AllowDeltaSeconds $false
    $TimerBody = New-CSharpLifecycleFunctionBody -Statements $TimerStatements -AllowDeltaSeconds $false -ParameterCount 2
    $EventBody = New-CSharpLifecycleFunctionBody -Statements $EventStatements -AllowDeltaSeconds $false -ParameterCount 2
    $GameplayEventBody = New-CSharpGameplayEventFunctionBody -GameplayCallbacks $GameplayCallbacks

    $CodeSection = New-WasmByteList
    Add-WasmU32Leb -Bytes $CodeSection -Value 6
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$BeginPlayBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $BeginPlayBody.ToArray()
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$TickBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $TickBody.ToArray()
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$EndPlayBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $EndPlayBody.ToArray()
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$TimerBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $TimerBody.ToArray()
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$EventBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $EventBody.ToArray()
    Add-WasmU32Leb -Bytes $CodeSection -Value ([uint32]$GameplayEventBody.Count)
    Add-WasmBytes -Bytes $CodeSection -Values $GameplayEventBody.ToArray()
    Add-WasmSection -Module $Module -SectionId 10 -Payload $CodeSection

    return ,(Convert-WasmByteListToArray -Bytes $Module)
}
function Invoke-CSharpSourceAdapter {
    param(
        [object[]]$Diagnostics = @(),
        [Parameter(Mandatory = $true)]$FrontendModel
    )

    $AdapterDiagnostics = @($Diagnostics)
    $AdapterWasmPath = Join-Path $OutputRoot "$ArtifactStem.csharp_adapter.wasm"

    try {
        if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
            throw "C# source file is missing: $SourcePath"
        }

        $SourceText = [System.IO.File]::ReadAllText($SourcePath)
        $ScriptType = Find-CSharpFrontendScriptType -FrontendModel $FrontendModel -SourcePath $SourcePath
        $script:SelectedScriptTypeName = [string]$ScriptType.name
        $Fields = @(Get-CSharpFrontendStaticFloatFields -ScriptType $ScriptType)
        $BeginPlayBody = Get-CSharpFrontendMethodBody -SourceText $SourceText -Method (Get-CSharpFrontendMethod -ScriptType $ScriptType -MethodName 'BeginPlay')
        $TickBody = Get-CSharpFrontendMethodBody -SourceText $SourceText -Method (Get-CSharpFrontendMethod -ScriptType $ScriptType -MethodName 'Tick')
        $GameplayCallbackSpecs = @(
            [PSCustomObject]@{ MethodName = 'OnBeginOverlap'; EventType = 1; ActorParameterName = 'otherActor'; VectorParameterName = 'location'; InputParameterName = '' },
            [PSCustomObject]@{ MethodName = 'OnEndOverlap'; EventType = 2; ActorParameterName = 'otherActor'; VectorParameterName = 'location'; InputParameterName = '' },
            [PSCustomObject]@{ MethodName = 'OnHit'; EventType = 3; ActorParameterName = 'otherActor'; VectorParameterName = 'normalImpulse'; InputParameterName = '' },
            [PSCustomObject]@{ MethodName = 'OnInput'; EventType = 4; ActorParameterName = ''; VectorParameterName = 'input.Value'; InputParameterName = 'input' }
        )
        $EndPlayMethod = Get-CSharpFrontendMethod -ScriptType $ScriptType -MethodName 'EndPlay' -Optional
        $TimerMethod = Get-CSharpFrontendMethod -ScriptType $ScriptType -MethodName 'OnTimer' -Optional
        $EventMethod = Get-CSharpFrontendMethod -ScriptType $ScriptType -MethodName 'OnEvent' -Optional
        $EndPlayBody = if ($null -eq $EndPlayMethod) { $null } else { Get-CSharpFrontendMethodBody -SourceText $SourceText -Method $EndPlayMethod }
        $TimerBody = if ($null -eq $TimerMethod) { $null } else { Get-CSharpFrontendMethodBody -SourceText $SourceText -Method $TimerMethod }
        $EventBody = if ($null -eq $EventMethod) { $null } else { Get-CSharpFrontendMethodBody -SourceText $SourceText -Method $EventMethod }
        $BeginPlayStatements = @(Get-CSharpLifecycleStatements -MethodBody $BeginPlayBody -MethodName 'BeginPlay' -Fields $Fields)
        $TickStatements = @(Get-CSharpLifecycleStatements -MethodBody $TickBody -MethodName 'Tick' -Fields $Fields)
        $EndPlayStatements = @()
        if ($null -ne $EndPlayBody) {
            $EndPlayStatements = @(Get-CSharpLifecycleStatements -MethodBody $EndPlayBody -MethodName 'EndPlay' -Fields $Fields -AllowEmpty $true)
        }
        $TimerStatements = @()
        if ($null -ne $TimerBody) {
            $TimerStatements = @(Get-CSharpLifecycleStatements -MethodBody $TimerBody -MethodName 'OnTimer' -Fields $Fields -AllowEmpty $true)
        }

        $EventStatements = @()
        if ($null -ne $EventBody) {
            $EventStatements = @(Get-CSharpLifecycleStatements -MethodBody $EventBody -MethodName 'OnEvent' -Fields $Fields -AllowEmpty $true)
        }

        $GameplayCallbacks = @()
        foreach ($Spec in $GameplayCallbackSpecs) {
            $CallbackMethod = Get-CSharpFrontendMethod -ScriptType $ScriptType -MethodName $Spec.MethodName -Optional
            if ($null -eq $CallbackMethod) {
                continue
            }
            $CallbackBody = Get-CSharpFrontendMethodBody -SourceText $SourceText -Method $CallbackMethod

            $CallbackStatements = @(Get-CSharpLifecycleStatements -MethodBody $CallbackBody -MethodName $Spec.MethodName -Fields $Fields -AllowEmpty $true -CallbackActorParameterName $Spec.ActorParameterName -CallbackVectorParameterName $Spec.VectorParameterName -CallbackInputParameterName $Spec.InputParameterName)
            $GameplayCallbacks += [PSCustomObject]@{
                MethodName = $Spec.MethodName
                EventType = $Spec.EventType
                Statements = $CallbackStatements
            }
        }

        $WasmBytes = New-CSharpDirectAbiWasmModule -Fields $Fields -BeginPlayStatements $BeginPlayStatements -TickStatements $TickStatements -EndPlayStatements $EndPlayStatements -TimerStatements $TimerStatements -EventStatements $EventStatements -GameplayCallbacks $GameplayCallbacks

        [System.IO.File]::WriteAllBytes($AdapterWasmPath, $WasmBytes)
        $ObservedExports = @(Get-WasmExports -Path $AdapterWasmPath)
        $ObservedNames = @($ObservedExports | ForEach-Object { $_.name })
        $RequiredExports = @('avid_on_begin_play', 'avid_on_tick', 'avid_on_end_play', 'avid_on_timer', 'avid_on_event', 'avid_on_gameplay_event')
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
                compiler = 'avidscript-csharp-ast-adapter'
                subset = 'actor_lifecycle_v13'
                sha256 = [string]$FrontendModel.source.sha256
                script_type = $SelectedScriptTypeName
                frontend_file = Convert-ToProjectRelativePath -Path $FrontendArtifactPath
                frontend_schema_version = [int]$FrontendModel.schema_version
                semantic_file = Convert-ToProjectRelativePath -Path $SemanticArtifactPath
                semantic_schema_version = [int]$SemanticModel.schema_version
                semantic_version = [string]$SemanticModel.semantic_version
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
                },
                [ordered]@{
                    module = 'env'
                    name = 'actor_get_location'
                },
                [ordered]@{
                    module = 'env'
                    name = 'actor_set_rotation'
                },
                [ordered]@{
                    module = 'env'
                    name = 'actor_get_rotation'
                },
                [ordered]@{
                    module = 'env'
                    name = 'actor_set_scale'
                },
                [ordered]@{
                    module = 'env'
                    name = 'actor_get_scale'
                },
                [ordered]@{
                    module = 'env'
                    name = 'actor_get_root_component'
                },
                [ordered]@{
                    module = 'env'
                    name = 'scene_component_get_world_location'
                },
                [ordered]@{
                    module = 'env'
                    name = 'scene_component_set_world_location'
                },
                [ordered]@{
                    module = 'env'
                    name = 'owner_get_slot'
                },
                [ordered]@{
                    module = 'env'
                    name = 'owner_get_generation'
                },
                [ordered]@{
                    module = 'env'
                    name = 'timer_set_once'
                },
                [ordered]@{
                    module = 'env'
                    name = 'timer_cancel'
                }
            )
            toolchain = [ordered]@{
                compiler = 'avidscript-csharp-ast-adapter'
                target = 'wasm32-direct-abi'
                direct_abi = $true
            }
            adapter_contract = [ordered]@{
                self_binding = 'owner_handle_imports'
                supported_types = @('FVector', 'FRotator', 'FTransform', 'InputEvent', 'AActor', 'USceneComponent', 'UE.Self')
                supported_calls = @('UE.Self.GetActorLocation()', 'UE.Self.SetActorLocation(FVector)', 'UE.Self.AddActorWorldOffset(FVector)', 'UE.Self.GetActorRotation()', 'UE.Self.SetActorRotation(FRotator)', 'UE.Self.GetActorScale3D()', 'UE.Self.SetActorScale3D(FVector)', 'UE.Self.GetRootComponent().GetWorldLocation()', 'UE.Self.GetRootComponent().SetWorldLocation(FVector)', 'AActor.GetActorTransform()', 'Actor.SetLocation(float x, float y, float z)', 'Actor.AddLocationOffset(float x, float y, float z)', 'UE.SetTimer(float delaySeconds, int callbackId)', 'UE.CancelTimer(int timerHandle)')
                supported_state = @('private static float Field', 'Field = expression', 'Field += expression', 'FVector local = UE.Self.GetActorLocation()', 'FVector local + new FVector(...)', 'FRotator local = UE.Self.GetActorRotation()', 'FRotator local + new FRotator(...)', 'FVector local = UE.Self.GetActorScale3D()', 'FVector local = UE.Self.GetRootComponent().GetWorldLocation()')
                generated_gameplay_callbacks = @($GameplayCallbacks | ForEach-Object { $_.MethodName })
                supported_lifecycle_events = @('BeginPlay', 'Tick', 'EndPlay', 'Timer', 'Event', 'BeginOverlap', 'EndOverlap', 'Hit', 'Input')
                supported_input_fields = @('input.ActionId', 'input.TriggerEvent', 'input.Value')
                supported_event_expressions = @('numeric literal', 'value', 'private static float field', 'multiplication of supported expressions', 'addition of supported expressions')
                static_float_fields = @($Fields | Where-Object { $null -ne $_ } | ForEach-Object { $_.Name })
                supported_tick_expressions = @('numeric literal', 'deltaSeconds', 'private static float field', 'multiplication of supported expressions', 'addition of supported expressions')
            }
        }

        $ManifestJson = $Manifest | ConvertTo-Json -Depth 10
        [System.IO.File]::WriteAllText($ManifestPath, $ManifestJson + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

        $AdapterDiagnostics += [ordered]@{
            code = 'source_adapter_used'
            message = 'Built direct ABI WASM from the C# ActorLifecycle v13 AST subset with generated generic gameplay-event dispatch, typed AActor/FVector collision callbacks, InputEvent field mapping, lifecycle and Timer events, static float state and legacy Actor facade support.'
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
function Convert-CSharpFrontendDiagnostics {
    param(
        [Parameter(Mandatory = $true)]$FrontendModel,
        [Parameter(Mandatory = $true)][string]$SourceId
    )

    $Result = @()
    foreach ($Diagnostic in @($FrontendModel.diagnostics)) {
        $Result += [ordered]@{
            code = [string]$Diagnostic.code
            severity = [string]$Diagnostic.severity
            message = [string]$Diagnostic.message
            file = $SourceId
            start = [int]$Diagnostic.span.start
            length = [int]$Diagnostic.span.length
            line = [int]$Diagnostic.span.line
            column = [int]$Diagnostic.span.column
            end_line = [int]$Diagnostic.span.end_line
            end_column = [int]$Diagnostic.span.end_column
        }
    }

    return $Result
}

function Convert-CSharpSemanticDiagnostics {
    param(
        [Parameter(Mandatory = $true)]$SemanticModel,
        [Parameter(Mandatory = $true)][string]$SourceId
    )

    $Result = @()
    foreach ($Diagnostic in @($SemanticModel.diagnostics)) {
        $Result += [ordered]@{
            code = [string]$Diagnostic.code
            severity = [string]$Diagnostic.severity
            message = [string]$Diagnostic.message
            file = $SourceId
            start = [int]$Diagnostic.span.start
            length = [int]$Diagnostic.span.length
            line = [int]$Diagnostic.span.line
            column = [int]$Diagnostic.span.column
            end_line = [int]$Diagnostic.span.end_line
            end_column = [int]$Diagnostic.span.end_column
        }
    }

    return $Result
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
        succeeded = $Result -eq "direct_abi_built"
        direct_abi_supported = $DirectAbiSupported
        source = [ordered]@{
            project = Convert-ToProjectRelativePath -Path $ProjectPath
            file = Convert-ToProjectRelativePath -Path $SourcePath
            sha256 = if ($null -eq $FrontendModel) { "" } else { [string]$FrontendModel.source.sha256 }
            script_type = $SelectedScriptTypeName
        }
        output_root = Convert-ToProjectRelativePath -Path $OutputRoot
        required_exports = @(
            "avid_on_begin_play",
            "avid_on_tick",
            "avid_on_end_play",
            "avid_on_timer",
            "avid_on_event",
            "avid_on_gameplay_event"
        )
        required_imports = @(
            [ordered]@{
                module = "env"
                name = "actor_set_location"
            },
            [ordered]@{
                module = "env"
                name = "actor_add_location_offset"
            },
            [ordered]@{
                module = "env"
                name = "actor_get_location"
            },
            [ordered]@{
                module = "env"
                name = "actor_set_rotation"
            },
            [ordered]@{
                module = "env"
                name = "actor_get_rotation"
            },
            [ordered]@{
                module = "env"
                name = "actor_set_scale"
            },
            [ordered]@{
                module = "env"
                name = "actor_get_scale"
            },
            [ordered]@{
                module = "env"
                name = "actor_get_root_component"
            },
            [ordered]@{
                module = "env"
                name = "scene_component_get_world_location"
            },
            [ordered]@{
                module = "env"
                name = "scene_component_set_world_location"
            },
            [ordered]@{
                module = "env"
                name = "owner_get_slot"
            },
            [ordered]@{
                module = "env"
                name = "owner_get_generation"
            },
            [ordered]@{
                module = "env"
                name = "timer_set_once"
            },
            [ordered]@{
                module = "env"
                name = "timer_cancel"
            }
        )
        observed_exports = @($ObservedExports | ForEach-Object { $_.name })
        artifacts = [ordered]@{
            wasm_file = if ([string]::IsNullOrWhiteSpace($WasmPath)) { "" } else { Convert-ToProjectRelativePath -Path $WasmPath }
            manifest_file = if ([string]::IsNullOrWhiteSpace($ManifestPath)) { "" } else { Convert-ToProjectRelativePath -Path $ManifestPath }
            report_file = Convert-ToProjectRelativePath -Path $ReportPath
            frontend_file = if (Test-Path -LiteralPath $FrontendArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath -Path $FrontendArtifactPath } else { "" }
            semantic_file = if (Test-Path -LiteralPath $SemanticArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath -Path $SemanticArtifactPath } else { "" }
        }
        frontend = [ordered]@{
            schema_version = if ($null -eq $FrontendModel) { 0 } else { [int]$FrontendModel.schema_version }
            version = if ($null -eq $FrontendModel) { "" } else { [string]$FrontendModel.frontend_version }
        }
        semantic = [ordered]@{
            schema_version = if ($null -eq $SemanticModel) { 0 } else { [int]$SemanticModel.schema_version }
            version = if ($null -eq $SemanticModel) { "" } else { [string]$SemanticModel.semantic_version }
            succeeded = if ($null -eq $SemanticModel) { $false } else { [bool]$SemanticModel.succeeded }
            source_sha256 = if ($null -eq $SemanticModel) { "" } else { [string]$SemanticModel.source.sha256 }
            frontend_sha256 = if ($null -eq $SemanticModel) { "" } else { [string]$SemanticModel.source.frontend_sha256 }
            diagnostic_count = if ($null -eq $SemanticModel) { 0 } else { @($SemanticModel.diagnostics).Count }
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
$FrontendArtifactPath = Join-Path $OutputRoot "$ArtifactStem.csharp.frontend.json"
$SemanticArtifactPath = Join-Path $OutputRoot "$ArtifactStem.csharp.semantic.json"
$StaleAdapterWasmPath = Join-Path $OutputRoot "$ArtifactStem.csharp_adapter.wasm"
$StaleDotNetWasmPath = Join-Path $OutputRoot "$ArtifactStem.dotnet.wasm"
$FrontendModel = $null
$SemanticModel = $null
$SelectedScriptTypeName = ""
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
if (Test-Path -LiteralPath $FrontendArtifactPath) {
    Remove-Item -LiteralPath $FrontendArtifactPath -Force
}
if (Test-Path -LiteralPath $SemanticArtifactPath) {
    Remove-Item -LiteralPath $SemanticArtifactPath -Force
}
foreach ($StaleWasmPath in @($StaleAdapterWasmPath, $StaleDotNetWasmPath)) {
    if (Test-Path -LiteralPath $StaleWasmPath -PathType Leaf) {
        Remove-Item -LiteralPath $StaleWasmPath -Force
    }
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

if (-not $DotNet.Found) {
    Write-Report -Result "missing_toolchain" -DirectAbiSupported $false -Diagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Frontend] result=missing_toolchain missing=dotnet report=$ReportPath"
    exit 1
}

$FrontendScriptPath = Join-Path $BuildDir "InvokeCSharpFrontend.ps1"
$FrontendSourceId = Convert-ToProjectRelativePath -Path $SourcePath
$FrontendArguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $FrontendScriptPath,
    "-DotNetPath", $DotNet.Path,
    "-SourcePath", $SourcePath,
    "-SourceId", $FrontendSourceId,
    "-OutputPath", $FrontendArtifactPath,
    "-Configuration", $Configuration
)
$FrontendOutput = @(& powershell.exe @FrontendArguments 2>&1)
$FrontendExitCode = $LASTEXITCODE

if (Test-Path -LiteralPath $FrontendArtifactPath -PathType Leaf) {
    try {
        $FrontendModel = Get-Content -Raw -LiteralPath $FrontendArtifactPath | ConvertFrom-Json
        $Diagnostics += @(Convert-CSharpFrontendDiagnostics -FrontendModel $FrontendModel -SourceId $FrontendSourceId)
    }
    catch {
        $Diagnostics += [ordered]@{
            code = "frontend_artifact_invalid"
            severity = "error"
            message = $_.Exception.Message
            file = $FrontendSourceId
        }
    }
}
else {
    $Diagnostics += [ordered]@{
        code = "frontend_artifact_missing"
        severity = "error"
        message = "C# frontend did not write its artifact."
        file = $FrontendSourceId
        process_exit_code = $FrontendExitCode
        output = @($FrontendOutput)
    }
}

if ($FrontendExitCode -ne 0 -or $null -eq $FrontendModel -or -not $FrontendModel.succeeded) {
    Write-Report -Result "frontend_failed" -DirectAbiSupported $false -Diagnostics $Diagnostics -Compiler "avidscript-csharp-roslyn"
    Write-Output "[AvidScript][CSharp][Frontend] result=frontend_failed exit_code=$FrontendExitCode artifact=$FrontendArtifactPath report=$ReportPath"
    exit 1
}

$SemanticScriptPath = Join-Path $BuildDir "InvokeCSharpSemantic.ps1"
$SemanticArguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $SemanticScriptPath,
    "-DotNetPath", $DotNet.Path,
    "-SourcePath", $SourcePath,
    "-SourceId", $FrontendSourceId,
    "-FrontendPath", $FrontendArtifactPath,
    "-OutputPath", $SemanticArtifactPath,
    "-Configuration", $Configuration
)
$ResolvedSourcePath = [System.IO.Path]::GetFullPath($SourcePath)
$ResolvedDefaultSourcePath = [System.IO.Path]::GetFullPath($DefaultSourcePath)
if (-not $ResolvedSourcePath.Equals($ResolvedDefaultSourcePath, [System.StringComparison]::OrdinalIgnoreCase)) {
    $SemanticArguments += @("-ReferenceSourcePath", $DefaultSourcePath)
}
$SemanticOutput = @(& powershell.exe @SemanticArguments 2>&1)
$SemanticExitCode = $LASTEXITCODE

if (Test-Path -LiteralPath $SemanticArtifactPath -PathType Leaf) {
    try {
        $SemanticModel = Get-Content -Raw -LiteralPath $SemanticArtifactPath | ConvertFrom-Json
        $Diagnostics += @(Convert-CSharpSemanticDiagnostics -SemanticModel $SemanticModel -SourceId $FrontendSourceId)
    }
    catch {
        $Diagnostics += [ordered]@{
            code = "semantic_artifact_invalid"
            severity = "error"
            message = $_.Exception.Message
            file = $FrontendSourceId
        }
    }
}
else {
    $Diagnostics += [ordered]@{
        code = "semantic_artifact_missing"
        severity = "error"
        message = "C# semantic analyzer did not write its artifact."
        file = $FrontendSourceId
        process_exit_code = $SemanticExitCode
        output = @($SemanticOutput)
    }
}

if ($SemanticExitCode -ne 0 -or $null -eq $SemanticModel -or -not $SemanticModel.succeeded) {
    Write-Report -Result "semantic_failed" -DirectAbiSupported $false -Diagnostics $Diagnostics -Compiler "avidscript-csharp-roslyn-semantic"
    Write-Output "[AvidScript][CSharp][Semantic] result=semantic_failed exit_code=$SemanticExitCode artifact=$SemanticArtifactPath report=$ReportPath"
    exit 1
}

$SourceAdapterResult = Invoke-CSharpSourceAdapter -Diagnostics $Diagnostics -FrontendModel $FrontendModel
if ($SourceAdapterResult.Succeeded) {
    Write-Report -Result "direct_abi_built" -DirectAbiSupported $true -Diagnostics $SourceAdapterResult.Diagnostics -ObservedExports $SourceAdapterResult.ObservedExports -WasmPath $SourceAdapterResult.WasmPath -ManifestPath $SourceAdapterResult.ManifestPath -Compiler "avidscript-csharp-ast-adapter"
    Write-Output "[AvidScript][CSharp][Build] result=direct_abi_built compiler=avidscript-csharp-ast-adapter manifest=$($SourceAdapterResult.ManifestPath) wasm=$($SourceAdapterResult.WasmPath) sha256=$($SourceAdapterResult.Sha256)"
    exit 0
}

$Diagnostics = @($SourceAdapterResult.Diagnostics)
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
$RequiredExports = @("avid_on_begin_play", "avid_on_tick", "avid_on_end_play", "avid_on_timer", "avid_on_event", "avid_on_gameplay_event")
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
        sha256 = [string]$FrontendModel.source.sha256
        script_type = $SelectedScriptTypeName
        frontend_file = Convert-ToProjectRelativePath -Path $FrontendArtifactPath
        frontend_schema_version = [int]$FrontendModel.schema_version
        semantic_file = Convert-ToProjectRelativePath -Path $SemanticArtifactPath
        semantic_schema_version = [int]$SemanticModel.schema_version
        semantic_version = [string]$SemanticModel.semantic_version
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
        },
        [ordered]@{
            module = "env"
            name = "actor_get_location"
        },
        [ordered]@{
            module = "env"
            name = "actor_set_rotation"
        },
        [ordered]@{
            module = "env"
            name = "actor_get_rotation"
        },
        [ordered]@{
            module = "env"
            name = "actor_set_scale"
        },
        [ordered]@{
            module = "env"
            name = "actor_get_scale"
        },
        [ordered]@{
            module = "env"
            name = "actor_get_root_component"
        },
        [ordered]@{
            module = "env"
            name = "scene_component_get_world_location"
        },
        [ordered]@{
            module = "env"
            name = "scene_component_set_world_location"
        },
        [ordered]@{
            module = "env"
            name = "owner_get_slot"
        },
        [ordered]@{
            module = "env"
            name = "owner_get_generation"
        },
        [ordered]@{
            module = "env"
            name = "timer_set_once"
        },
        [ordered]@{
            module = "env"
            name = "timer_cancel"
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
