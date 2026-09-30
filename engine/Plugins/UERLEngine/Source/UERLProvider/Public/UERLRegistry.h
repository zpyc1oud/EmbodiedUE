#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "UERLProvider.h"

/** Register and resolve Environment factories by stable ID. */
class UERLPROVIDER_API FUERLEnvironmentRegistry
{
public:
	/** Return the process-wide Environment registry. */
	static FUERLEnvironmentRegistry& Get();

	/** Register one factory, rejecting duplicate Environment IDs. */
	bool RegisterFactory(const TSharedRef<IUERLEnvironmentFactory>& Factory, FString& OutError);
	/** Remove the factory registered under Id. */
	void UnregisterFactory(FName Id);
	/** Resolve one Environment factory or return a diagnostic in OutError. */
	TSharedPtr<IUERLEnvironmentFactory> Resolve(FName Id, FString& OutError) const;
	/** Return stable descriptors for all registered Environments. */
	TArray<FUERLEnvironmentDescriptor> ListDescriptors() const;

private:
	mutable FRWLock Lock;
	TMap<FName, TSharedPtr<IUERLEnvironmentFactory>> Factories;
};

/** Register and resolve Robot factories by stable ID. */
class UERLPROVIDER_API FUERLRobotRegistry
{
public:
	/** Return the process-wide Robot registry. */
	static FUERLRobotRegistry& Get();

	/** Register one factory, rejecting duplicate Robot IDs. */
	bool RegisterFactory(const TSharedRef<IUERLRobotFactory>& Factory, FString& OutError);
	/** Remove the factory registered under Id. */
	void UnregisterFactory(FName Id);
	/** Resolve one Robot factory or return a diagnostic in OutError. */
	TSharedPtr<IUERLRobotFactory> Resolve(FName Id, FString& OutError) const;
	/** Return stable descriptors for all registered Robots. */
	TArray<FUERLRobotDescriptor> ListDescriptors() const;

private:
	mutable FRWLock Lock;
	TMap<FName, TSharedPtr<IUERLRobotFactory>> Factories;
};
