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
				.Method("StaticNoOp", MakeFunction(&AAvidScriptPerfFixture::ReflectNoOp))
				.Method("StaticAddInt32", MakeFunction(&AAvidScriptPerfFixture::ReflectAddInt32))
				.Property("StaticScalarValue", MakeProperty(&AAvidScriptPerfFixture::ScalarValue))
				.Method("StaticVectorValue", MakeFunction(&AAvidScriptPerfFixture::ReflectVectorValue))
				.Method("StaticVectorRefOut", MakeFunction(&AAvidScriptPerfFixture::ReflectVectorRefOut))
				.Method("StaticObjectRoundtrip", MakeFunction(&AAvidScriptPerfFixture::ReflectObjectRoundtrip))
				.Method("StaticEventStep", MakeFunction(&AAvidScriptPerfFixture::ReflectEventStep))
				.Method("StaticBatchAdd", MakeFunction(&AAvidScriptPerfFixture::ReflectBatchAdd))
				.Register();
		}
	};

	FAvidScriptPerfStaticBindingRegistration StaticBindingRegistration;
}
