#pragma once

#include "CoreMinimal.h"
#include "NNERuntimeCPU.h"
#include "UERLPolicyArtifact.h"

/**
 * Load an ONNX policy from a UERLPOL2 artifact and evaluate it via NNE.
 *
 * Input/output widths come from the model tensor descriptors — never from
 * config constants. Observation normalization lives inside the ONNX graph;
 * this type must not normalize again.
 */
class UERLPOLICY_API FUERLPolicyNetwork
{
public:
	/**
	 * Create an NNE model instance from the artifact's ONNX bytes and
	 * preallocate input/output buffers from the tensor descriptors.
	 */
	bool Build(const FUERLPolicyArtifact& Artifact, FString& OutError);

	/** Single inference. Observation must already be the plan-produced vector. */
	bool Evaluate(TConstArrayView<float> Observation, TArray<float>& OutAction, FString& OutError) const;

	int32 InputWidth() const { return CachedInputWidth; }
	int32 OutputWidth() const { return CachedOutputWidth; }
	bool IsBuilt() const { return ModelInstance.IsValid() && CachedInputWidth > 0 && CachedOutputWidth > 0; }

	void Reset();

private:
	static int32 FeatureWidthFromDesc(const UE::NNE::FTensorDesc& Desc, FString& OutError);

	TSharedPtr<UE::NNE::IModelInstanceCPU> ModelInstance;
	mutable TArray<float> InputScratch;
	mutable TArray<float> OutputScratch;
	int32 CachedInputWidth = 0;
	int32 CachedOutputWidth = 0;
};
