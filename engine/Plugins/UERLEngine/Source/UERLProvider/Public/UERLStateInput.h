#pragma once

#include "UERLInputProvider.h"

/** Adapt an existing host catalog without changing names, units, frames or values. */
class UERLPROVIDER_API FUERLStateInputFactory final : public IUERLInputFactory
{
public:
	/** Register in a host-local registry with the matching immutable state catalog. */
	explicit FUERLStateInputFactory(TArray<FUERLFieldDescriptor> InFields);
	FString GetId() const override { return TEXT("uerl.state"); }
	int32 GetVersion() const override { return 1; }
	bool Validate(const FUERLInputSpec& Spec, FUERLInputSpec& OutEffectiveSpec,
		TArray<FUERLFieldDescriptor>& OutFields, FString& OutError) const override;
	TUniquePtr<IUERLInputProvider> Create(const FUERLInputSpec& EffectiveSpec) const override;
private:
	TArray<FUERLFieldDescriptor> Fields;
};
