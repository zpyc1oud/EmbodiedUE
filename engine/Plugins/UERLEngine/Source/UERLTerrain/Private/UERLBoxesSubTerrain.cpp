#include "UERLSubTerrainGenerator.h"

#include "UERLTerrainInternal.h"

class FUERLBoxesSubTerrainGenerator final : public IUERLSubTerrainGenerator
{
public:
	virtual FName Id() const override { return FName(TEXT("boxes")); }

	virtual bool ValidateParams(const FJsonObject& Params, FString& OutError) const override
	{
		return UERLTerrainInternal::ValidateBoxesParams(
			Params, OutError);
	}

	virtual bool Generate(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError) const override
	{
		return UERLTerrainInternal::GenerateBoxesPatch(Request, OutPatch, OutError);
	}
};

TSharedRef<IUERLSubTerrainGenerator> MakeBoxesSubTerrainGenerator()
{
	return MakeShared<FUERLBoxesSubTerrainGenerator>();
}
