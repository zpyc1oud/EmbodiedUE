#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "UERLProvider.h"

class USkeletalMeshComponent;

/** Persist policy-semantic settings, not concrete map object references. */
struct FUERLInputSpec
{
	FString Name;
	FString ProviderId;
	int32 ProviderVersion = 1;
	/** Owned JSON parameter object text; the factory validates its typed contents. */
	FString ParametersJson = TEXT("{}");
	FString Attachment;
	TArray<FString> SceneBindings;
	FString Sampling = TEXT("completed_window");
	FString MissingData = TEXT("fault");
};

/** Bind logical scene names to host-owned actors for one stable Slot. */
struct FUERLInputSlotBinding
{
	FUERLSlotContext Slot;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<USkeletalMeshComponent> Robot;
	/** Host-owned read-only attachment transforms, in UE world centimetres. */
	TMap<FString, TFunction<bool(FTransform&, FString&)>> TransformReaders;
	TMap<FString, TArray<TWeakObjectPtr<AActor>>> SceneBindings;
};

/** Identify one host-completed sample without giving a provider clock ownership. */
struct FUERLInputSampleStamp
{
	int64 Sequence = 0;
	int64 ResetGeneration = 0;
	double SolverTimeSeconds = 0.0;
};

/** Produce one input's numeric fields. Implementations never advance physics. */
class UERLPROVIDER_API IUERLInputProvider
{
public:
	virtual ~IUERLInputProvider() = default;
	/** Copy required weak bindings; the array view expires after this call. */
	virtual bool Bind(TConstArrayView<FUERLInputSlotBinding> Slots, FString& OutError) = 0;
	/** Write every output scalar in descriptor order for the requested Slot. */
	virtual bool Sample(int32 SlotId, const FUERLInputSampleStamp& Stamp,
		TArrayView<float> OutValues, FString& OutError) = 0;
	virtual bool Reset(TConstArrayView<int32> SlotIds, FString& OutError) = 0;
	/** Release only owned resources; safe after a partial Bind and repeated calls. */
	virtual void Release() = 0;
};

/** Validate effective settings before allocating provider instances. */
class UERLPROVIDER_API IUERLInputFactory
{
public:
	virtual ~IUERLInputFactory() = default;
	virtual FString GetId() const = 0;
	virtual int32 GetVersion() const = 0;
	virtual bool Validate(const FUERLInputSpec& Spec, FUERLInputSpec& OutEffectiveSpec,
		TArray<FUERLFieldDescriptor>& OutFields, FString& OutError) const = 0;
	virtual TUniquePtr<IUERLInputProvider> Create(const FUERLInputSpec& EffectiveSpec) const = 0;
};

/** Explicit native registration, independent of Task identifiers. */
class UERLPROVIDER_API FUERLInputRegistry
{
public:
	static FUERLInputRegistry& Get();
	bool RegisterFactory(const TSharedRef<IUERLInputFactory>& Factory, FString& OutError);
	/** Call only after compiled sets and instances using this module are released. */
	void UnregisterFactory(const FString& Id, int32 Version);
	TSharedPtr<IUERLInputFactory> Resolve(const FString& Id, int32 Version, FString& OutError) const;
private:
	mutable FRWLock Lock;
	TMap<FString, TMap<int32, TSharedPtr<IUERLInputFactory>>> Factories;
};

/** Own compiled providers and publish one immutable sample per Slot/boundary. */
class UERLPROVIDER_API FUERLCompiledInputSet
{
public:
	FUERLCompiledInputSet() = default;
	FUERLCompiledInputSet(const FUERLCompiledInputSet&) = delete;
	FUERLCompiledInputSet& operator=(const FUERLCompiledInputSet&) = delete;
	~FUERLCompiledInputSet();
	bool Bind(const FUERLInputRegistry& Registry, TConstArrayView<FUERLInputSpec> Specs,
		TConstArrayView<FUERLInputSlotBinding> Slots, FString& OutError);
	/** Returned view remains valid until this Slot is sampled/reset or the set is released. */
	bool Sample(int32 SlotId, const FUERLInputSampleStamp& Stamp,
		TConstArrayView<float>& OutValues, FString& OutError);
	bool Reset(TConstArrayView<int32> SlotIds, FString& OutError);
	int64 GetResetGeneration(int32 SlotId) const;
	const TArray<FUERLFieldDescriptor>& GetFields() const { return Fields; }
	TArray<FUERLInputSpec> GetEffectiveSpecs() const;
	void Release();
private:
	struct FCompiledProvider
	{
		FUERLInputSpec Spec;
		TSharedPtr<IUERLInputFactory> Factory;
		TUniquePtr<IUERLInputProvider> Instance;
		int32 Offset = 0;
		int32 Width = 0;
	};
	struct FSlotCache
	{
		TArray<float> Values;
		FUERLInputSampleStamp Stamp;
		int64 Generation = 0;
		bool bPublished = false;
		bool bFaulted = false;
	};
	TArray<FCompiledProvider> Providers;
	TArray<FUERLFieldDescriptor> Fields;
	TMap<int32, FSlotCache> Slots;
	bool bBound = false;
};
