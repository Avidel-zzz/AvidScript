#include "AvidScriptPerfFixture.h"

#include "Binding.hpp"
#include "UEDataBinding.hpp"

UsingUClass(UObject);
UsingUClass(UAvidScriptPerfFixture);
UsingUStruct(FVector);

class AvidScriptPerfStatic
{
public:
	static int32 NoOp(const UAvidScriptPerfFixture* Fixture, const int32 Value)
	{
		return Fixture->NativeNoOp(Value);
	}

	static int32 AddInt32(const UAvidScriptPerfFixture* Fixture, const int32 Left, const int32 Right)
	{
		return Fixture->NativeAddInt32(Left, Right);
	}

	static void SetScalar(UAvidScriptPerfFixture* Fixture, const int32 Value)
	{
		Fixture->NativeSetScalar(Value);
	}

	static int32 GetScalar(const UAvidScriptPerfFixture* Fixture)
	{
		return Fixture->NativeGetScalar();
	}

	static FVector VectorValue(const UAvidScriptPerfFixture* Fixture, const FVector& Value)
	{
		return Fixture->NativeVectorValue(Value);
	}

	static UObject* ObjectRoundtrip(const UAvidScriptPerfFixture* Fixture, UObject* Value)
	{
		return Fixture->NativeObjectRoundtrip(Value);
	}

	static int32 BatchAdd(const UAvidScriptPerfFixture* Fixture, const int32 Seed, const int32 Count)
	{
		return Fixture->NativeBatchAdd(Seed, Count);
	}
};

UsingCppType(AvidScriptPerfStatic);

namespace
{
	struct FAvidScriptPerfStaticBindingRegistration
	{
		FAvidScriptPerfStaticBindingRegistration()
		{
			puerts::DefineClass<AvidScriptPerfStatic>()
				.Function("NoOp", MakeFunction(&AvidScriptPerfStatic::NoOp))
				.Function("AddInt32", MakeFunction(&AvidScriptPerfStatic::AddInt32))
				.Function("SetScalar", MakeFunction(&AvidScriptPerfStatic::SetScalar))
				.Function("GetScalar", MakeFunction(&AvidScriptPerfStatic::GetScalar))
				.Function("VectorValue", MakeFunction(&AvidScriptPerfStatic::VectorValue))
				.Function("ObjectRoundtrip", MakeFunction(&AvidScriptPerfStatic::ObjectRoundtrip))
				.Function("BatchAdd", MakeFunction(&AvidScriptPerfStatic::BatchAdd))
				.Register();
		}
	};

	FAvidScriptPerfStaticBindingRegistration StaticBindingRegistration;
}
