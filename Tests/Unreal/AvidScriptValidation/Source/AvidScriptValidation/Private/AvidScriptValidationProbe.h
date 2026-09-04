#pragma once

namespace AvidScript::Validation
{
class IUiSavePackagedProbe
{
public:
	virtual ~IUiSavePackagedProbe() = default;
	virtual void Start() = 0;
};
}
