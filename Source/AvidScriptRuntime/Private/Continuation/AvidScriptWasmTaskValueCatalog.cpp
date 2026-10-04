#include "Continuation/AvidScriptWasmTaskValueCatalog.h"
#include "AvidScriptHash.h"
#include "AvidScriptWasmModuleLayout.h"
#include "Containers/StringConv.h"

bool FAvidScriptWasmTaskValueCatalog::ReadFromCanonicalWasm(TConstArrayView<uint8> CanonicalWasm,
    const FString& ExpectedModuleId, TUniquePtr<FAvidScriptWasmTaskValueCatalog>& OutCatalog, FString& OutError)
{
    using namespace AvidScript::TaskResult;
    OutCatalog.Reset(); OutError.Reset();
    TArray<uint8> Payload, Provenance; bool bFound = false, bProvenanceFound = false;
    if (!ReadAvidScriptWasmCustomSection(CanonicalWasm, TEXT("avidscript.task_values"),
        CatalogAbi::MaxCatalogBytes, Payload, bFound, OutError)) return false;
    if (!bFound) return true;
    if (!ReadAvidScriptWasmCustomSection(CanonicalWasm, TEXT("avidscript.provenance"),
        4 * 1024 * 1024, Provenance, bProvenanceFound, OutError)) return false;
    auto Fail = [&OutError](const TCHAR* Message) { OutError = Message; return false; };
    if (!bProvenanceFound || Provenance.IsEmpty() || ExpectedModuleId.IsEmpty())
        return Fail(TEXT("Task value catalog requires canonical module provenance."));
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Provenance.GetData()), Provenance.Num());
    const FTCHARToUTF8 RoundTrip(Text.Get(), Text.Length());
    if (RoundTrip.Length() != Provenance.Num() || FMemory::Memcmp(RoundTrip.Get(), Provenance.GetData(), Provenance.Num()) != 0)
        return Fail(TEXT("Task value provenance UTF-8 is invalid."));
    TArray<FString> Lines; FString(Text.Length(), Text.Get()).ParseIntoArrayLines(Lines, false);
    TMap<FString, FString> Fields;
    for (const FString& Line : Lines)
    {
        int32 Separator = INDEX_NONE;
        if (!Line.FindChar('=', Separator) || Separator <= 0 || Fields.Contains(Line.Left(Separator)))
            return Fail(TEXT("Task value provenance fields are invalid or duplicated."));
        Fields.Add(Line.Left(Separator), Line.Mid(Separator + 1));
    }
    const FString Hash = Fields.FindRef(TEXT("source_sha256"));
    if (Fields.FindRef(TEXT("module_id")) != ExpectedModuleId || Hash.Len() != 64)
        return Fail(TEXT("Task value catalog module/source identity is invalid."));
    std::array<std::uint8_t, 32> Source{};
    auto Hex = [](TCHAR C) -> int32 { return C >= '0' && C <= '9' ? C - '0' : C >= 'a' && C <= 'f' ? C - 'a' + 10 : -1; };
    for (int32 I = 0; I < 32; ++I)
    {
        const int32 High = Hex(Hash[I * 2]), Low = Hex(Hash[I * 2 + 1]);
        if (High < 0 || Low < 0) return Fail(TEXT("Task value catalog source hash is not canonical."));
        Source[I] = static_cast<std::uint8_t>((High << 4) | Low);
    }
    const FTCHARToUTF8 Module(ExpectedModuleId.GetCharArray().GetData(), ExpectedModuleId.Len());
    auto Candidate = MakeUnique<FAvidScriptWasmTaskValueCatalog>();
    if (ReadValueCatalog({Payload.GetData(), static_cast<std::size_t>(Payload.Num())},
        std::string(Module.Get(), Module.Length()), Source, Candidate->Values) != EValueError::Ok)
        return Fail(TEXT("Task value catalog wire data or provenance association is invalid."));
    Candidate->CanonicalSha256 = FAvidScriptHash::Sha256Hex(CanonicalWasm);
    OutCatalog = MoveTemp(Candidate); return true;
}
