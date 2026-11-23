#include "UERLSubTerrainGenerator.h"

#include "UERLTerrainInternal.h"

class FUERLPlaneSubTerrainGenerator final : public IUERLSubTerrainGenerator
{
public:
	virtual FName Id() const override { return FName(TEXT("plane")); }

	virtual bool ValidateParams(const FJsonObject& Params, FString& OutError) const override
	{
		return UERLTerrainInternal::ValidatePlaneParams(
			Params, OutError);
	}

	virtual bool Generate(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError) const override
	{
		return UERLTerrainInternal::GeneratePlanePatch(Request, OutPatch, OutError);
	}
};

TSharedRef<IUERLSubTerrainGenerator> MakePlaneSubTerrainGenerator()
{
	return MakeShared<FUERLPlaneSubTerrainGenerator>();
}
