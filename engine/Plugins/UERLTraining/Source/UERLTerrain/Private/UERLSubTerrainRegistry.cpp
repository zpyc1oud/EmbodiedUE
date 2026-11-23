#include "UERLSubTerrainGenerator.h"

#include "HAL/CriticalSection.h"

namespace
{
	FCriticalSection GSubTerrainRegistryLock;
	TMap<FName, TSharedPtr<IUERLSubTerrainGenerator>> GSubTerrainGenerators;
}

void RegisterSubTerrainGenerator(TSharedRef<IUERLSubTerrainGenerator> Generator)
{
	FString Error;
	if (!RegisterSubTerrainGeneratorUnique(Generator, Error))
	{
		checkf(false, TEXT("%s"), *Error);
	}
}

bool RegisterSubTerrainGeneratorUnique(
	TSharedRef<IUERLSubTerrainGenerator> Generator,
	FString& OutError)
{
	const FName Id = Generator->Id();
	if (Id.IsNone())
	{
		OutError = TEXT("sub-terrain generator id must be non-none");
		return false;
	}

	FScopeLock Lock(&GSubTerrainRegistryLock);
	if (GSubTerrainGenerators.Contains(Id))
	{
		OutError = FString::Printf(
			TEXT("sub-terrain generator '%s' is already registered"), *Id.ToString());
		return false;
	}
	GSubTerrainGenerators.Add(Id, Generator);
	OutError.Reset();
	return true;
}

const IUERLSubTerrainGenerator* FindSubTerrainGenerator(FName Id)
{
	FScopeLock Lock(&GSubTerrainRegistryLock);
	if (const TSharedPtr<IUERLSubTerrainGenerator>* Found = GSubTerrainGenerators.Find(Id))
	{
		return Found->Get();
	}
	return nullptr;
}
