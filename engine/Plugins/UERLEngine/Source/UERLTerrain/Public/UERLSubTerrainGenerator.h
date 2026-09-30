#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UERLTerrainTypes.h"

/** One terrain primitive generator. Difficulty is [0,1], resolved by the atlas. */
class UERLTERRAIN_API IUERLSubTerrainGenerator
{
public:
	virtual ~IUERLSubTerrainGenerator() = default;
	virtual FName Id() const = 0;
	virtual bool ValidateParams(const FJsonObject& Params, FString& OutError) const = 0;
	virtual bool Generate(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError) const = 0;
};

UERLTERRAIN_API void RegisterSubTerrainGenerator(TSharedRef<IUERLSubTerrainGenerator> Generator);
UERLTERRAIN_API bool RegisterSubTerrainGeneratorUnique(
	TSharedRef<IUERLSubTerrainGenerator> Generator,
	FString& OutError);
UERLTERRAIN_API const IUERLSubTerrainGenerator* FindSubTerrainGenerator(FName Id);
