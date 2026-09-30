#include "UERLRegistry.h"

namespace
{
	template <typename FactoryType>
	TSharedPtr<FactoryType> ResolveFactory(
		const TMap<FName, TSharedPtr<FactoryType>>& Factories,
		FName Id,
		FString& OutError)
	{
		if (Id.IsNone())
		{
			OutError = TEXT("an explicit provider id is required");
			return nullptr;
		}
		if (const TSharedPtr<FactoryType>* Found = Factories.Find(Id))
		{
			return *Found;
		}
		OutError = FString::Printf(TEXT("provider '%s' is not registered"), *Id.ToString());
		return nullptr;
	}
}

FUERLEnvironmentRegistry& FUERLEnvironmentRegistry::Get()
{
	static FUERLEnvironmentRegistry Instance;
	return Instance;
}

bool FUERLEnvironmentRegistry::RegisterFactory(const TSharedRef<IUERLEnvironmentFactory>& Factory, FString& OutError)
{
	const FName Id = Factory->Describe().Id;
	if (Id.IsNone())
	{
		OutError = TEXT("environment factory id must not be empty");
		return false;
	}
	FWriteScopeLock Guard(Lock);
	if (Factories.Contains(Id))
	{
		OutError = FString::Printf(TEXT("environment factory '%s' is already registered"), *Id.ToString());
		return false;
	}
	Factories.Add(Id, Factory);
	return true;
}

void FUERLEnvironmentRegistry::UnregisterFactory(FName Id)
{
	FWriteScopeLock Guard(Lock);
	Factories.Remove(Id);
}

TSharedPtr<IUERLEnvironmentFactory> FUERLEnvironmentRegistry::Resolve(FName Id, FString& OutError) const
{
	FReadScopeLock Guard(Lock);
	return ResolveFactory<IUERLEnvironmentFactory>(Factories, Id, OutError);
}

TArray<FUERLEnvironmentDescriptor> FUERLEnvironmentRegistry::ListDescriptors() const
{
	FReadScopeLock Guard(Lock);
	TArray<FUERLEnvironmentDescriptor> Result;
	for (const TPair<FName, TSharedPtr<IUERLEnvironmentFactory>>& Pair : Factories)
	{
		Result.Add(Pair.Value->Describe());
	}
	Result.Sort([](const FUERLEnvironmentDescriptor& A, const FUERLEnvironmentDescriptor& B)
	{
		return A.Id.LexicalLess(B.Id);
	});
	return Result;
}

FUERLRobotRegistry& FUERLRobotRegistry::Get()
{
	static FUERLRobotRegistry Instance;
	return Instance;
}

bool FUERLRobotRegistry::RegisterFactory(const TSharedRef<IUERLRobotFactory>& Factory, FString& OutError)
{
	const FName Id = Factory->Describe().Id;
	if (Id.IsNone())
	{
		OutError = TEXT("robot factory id must not be empty");
		return false;
	}
	FWriteScopeLock Guard(Lock);
	if (Factories.Contains(Id))
	{
		OutError = FString::Printf(TEXT("robot factory '%s' is already registered"), *Id.ToString());
		return false;
	}
	Factories.Add(Id, Factory);
	return true;
}

void FUERLRobotRegistry::UnregisterFactory(FName Id)
{
	FWriteScopeLock Guard(Lock);
	Factories.Remove(Id);
}

TSharedPtr<IUERLRobotFactory> FUERLRobotRegistry::Resolve(FName Id, FString& OutError) const
{
	FReadScopeLock Guard(Lock);
	return ResolveFactory<IUERLRobotFactory>(Factories, Id, OutError);
}

TArray<FUERLRobotDescriptor> FUERLRobotRegistry::ListDescriptors() const
{
	FReadScopeLock Guard(Lock);
	TArray<FUERLRobotDescriptor> Result;
	for (const TPair<FName, TSharedPtr<IUERLRobotFactory>>& Pair : Factories)
	{
		Result.Add(Pair.Value->Describe());
	}
	Result.Sort([](const FUERLRobotDescriptor& A, const FUERLRobotDescriptor& B)
	{
		return A.Id.LexicalLess(B.Id);
	});
	return Result;
}
