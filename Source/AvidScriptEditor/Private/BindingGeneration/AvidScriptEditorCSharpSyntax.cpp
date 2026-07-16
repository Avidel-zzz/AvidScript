#include "BindingGeneration/AvidScriptEditorCSharpSyntax.h"

namespace
{
bool IsCSharpKeyword(const FString& Value)
{
	static const TSet<FString> Keywords = {
		TEXT("abstract"), TEXT("as"), TEXT("base"), TEXT("bool"), TEXT("break"), TEXT("byte"),
		TEXT("case"), TEXT("catch"), TEXT("char"), TEXT("checked"), TEXT("class"), TEXT("const"),
		TEXT("continue"), TEXT("decimal"), TEXT("default"), TEXT("delegate"), TEXT("do"), TEXT("double"),
		TEXT("else"), TEXT("enum"), TEXT("event"), TEXT("explicit"), TEXT("extern"), TEXT("false"),
		TEXT("finally"), TEXT("fixed"), TEXT("float"), TEXT("for"), TEXT("foreach"), TEXT("goto"),
		TEXT("if"), TEXT("implicit"), TEXT("in"), TEXT("int"), TEXT("interface"), TEXT("internal"),
		TEXT("is"), TEXT("lock"), TEXT("long"), TEXT("namespace"), TEXT("new"), TEXT("null"),
		TEXT("object"), TEXT("operator"), TEXT("out"), TEXT("override"), TEXT("params"), TEXT("private"),
		TEXT("protected"), TEXT("public"), TEXT("readonly"), TEXT("ref"), TEXT("return"), TEXT("sbyte"),
		TEXT("sealed"), TEXT("short"), TEXT("sizeof"), TEXT("stackalloc"), TEXT("static"), TEXT("string"),
		TEXT("struct"), TEXT("switch"), TEXT("this"), TEXT("throw"), TEXT("true"), TEXT("try"),
		TEXT("typeof"), TEXT("uint"), TEXT("ulong"), TEXT("unchecked"), TEXT("unsafe"), TEXT("ushort"),
		TEXT("using"), TEXT("virtual"), TEXT("void"), TEXT("volatile"), TEXT("while")
	};
	return Keywords.Contains(Value);
}

} // namespace

FString FAvidScriptEditorCSharpSyntax::MakeIdentifier(const FString& Value)
{
	FString Result;
	Result.Reserve(Value.Len() + 1);
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		const bool bValid = Character == TEXT('_') || FChar::IsAlpha(Character) || (Index > 0 && FChar::IsDigit(Character));
		Result.AppendChar(bValid ? Character : TEXT('_'));
	}
	if (Result.IsEmpty())
	{
		Result = TEXT("_");
	}
	if (FChar::IsDigit(Result[0]))
	{
		Result.InsertAt(0, TEXT('_'));
	}
	if (IsCSharpKeyword(Result))
	{
		Result.InsertAt(0, TEXT('@'));
	}
	return Result;
}
