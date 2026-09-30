#include "UERLSubTerrainGenerator.h"

#include "UERLTerrainInternal.h"

class FUERLHeightfieldSubTerrainGenerator final : public IUERLSubTerrainGenerator
{
public:
	virtual FName Id() const override { return FName(TEXT("heightfield")); }

	virtual bool ValidateParams(const FJsonObject& Params, FString& OutError) const override
	{
		return UERLTerrainInternal::ValidateHeightfieldParams(
			Params, OutError);
	}

	virtual bool Generate(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError) const override
	{
		return UERLTerrainInternal::GenerateHeightfieldPatch(Request, OutPatch, OutError);
	}
};

TSharedRef<IUERLSubTerrainGenerator> MakeHeightfieldSubTerrainGenerator()
{
	return MakeShared<FUERLHeightfieldSubTerrainGenerator>();
}
