#include "AvidScriptPerfFixture.h"

#include "Binding.hpp"
#include "UEDataBinding.hpp"

UsingUClass(UObject);
UsingUClass(AAvidScriptPerfFixture);
UsingUStruct(FVector);

namespace
{
	struct FAvidScriptPerfStaticBindingRegistration
	{
		FAvidScriptPerfStaticBindingRegistration()
		{
			puerts::DefineClass<AAvidScriptPerfFixture>()
				.Method("StaticNoOp", MakeFunction(&AAvidScriptPerfFixture::NativeNoOp))
				.Method("StaticAddInt32", MakeFunction(&AAvidScriptPerfFixture::NativeAddInt32))
				.Property("StaticScalarValue", MakeProperty(&AAvidScriptPerfFixture::ScalarValue))
				.Method("StaticVectorValue", MakeFunction(&AAvidScriptPerfFixture::NativeVectorValue))
				.Method("StaticVectorRefOut", MakeFunction(&AAvidScriptPerfFixture::NativeVectorRefOut))
				.Method("StaticObjectRoundtrip", MakeFunction(&AAvidScriptPerfFixture::NativeObjectRoundtrip))
				.Method("StaticBatchAdd", MakeFunction(&AAvidScriptPerfFixture::NativeBatchAdd))
				.Register();
		}
	};

	FAvidScriptPerfStaticBindingRegistration StaticBindingRegistration;
}
