#include "UERLPolicyNetwork.h"

#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "UObject/Package.h"

namespace
{
	/** Preferred CPU runtime: formal NNE ORT plugin (loads RSL-RL ONNX). */
	constexpr TCHAR GUERLPolicyNneRuntimeName[] = TEXT("NNERuntimeORTCpu");

	// Unique name: unity builds merge TUs and collide on anonymous Fail helpers.
	bool PolicyNetworkFail(FString& OutError, const FString& Message)
	{
		OutError = Message;
		return false;
	}
}

void FUERLPolicyNetwork::Reset()
{
	ModelInstance.Reset();
	InputScratch.Reset();
	OutputScratch.Reset();
	CachedInputWidth = 0;
	CachedOutputWidth = 0;
}

int32 FUERLPolicyNetwork::FeatureWidthFromDesc(const UE::NNE::FTensorDesc& Desc, FString& OutError)
{
	const TConstArrayView<int32> Dims = Desc.GetShape().GetData();
	if (Dims.Num() == 0)
	{
		OutError = TEXT("model tensor descriptor has empty shape");
		return 0;
	}

	// Feature width is the last concrete (>0) dimension — batch may be symbolic (-1).
	for (int32 Index = Dims.Num() - 1; Index >= 0; --Index)
	{
		if (Dims[Index] > 0)
		{
			return Dims[Index];
		}
	}

	OutError = FString::Printf(
		TEXT("model tensor '%s' has no concrete feature dimension"),
		*Desc.GetName());
	return 0;
}

bool FUERLPolicyNetwork::Build(const FUERLPolicyArtifact& Artifact, FString& OutError)
{
	Reset();
	OutError.Reset();

	if (!Artifact.IsLoaded())
	{
		return PolicyNetworkFail(OutError, TEXT("policy artifact is not loaded"));
	}

	const TConstArrayView<uint8> OnnxBytes = Artifact.OnnxBytes();
	if (OnnxBytes.Num() == 0)
	{
		return PolicyNetworkFail(OutError, TEXT("policy artifact ONNX segment is empty"));
	}

	const TWeakInterfacePtr<INNERuntimeCPU> Runtime =
		UE::NNE::GetRuntime<INNERuntimeCPU>(GUERLPolicyNneRuntimeName);
	if (!Runtime.IsValid())
	{
		return PolicyNetworkFail(
			OutError,
			FString::Printf(
				TEXT("NNE CPU runtime '%s' is not registered; enable the NNERuntimeORT plugin"),
				GUERLPolicyNneRuntimeName));
	}

	UNNEModelData* ModelData = NewObject<UNNEModelData>(GetTransientPackageAsObject());
	if (ModelData == nullptr)
	{
		return PolicyNetworkFail(OutError, TEXT("failed to allocate UNNEModelData"));
	}
	ModelData->AddToRoot();
	ModelData->Init(
		TEXT("onnx"),
		TConstArrayView64<uint8>(OnnxBytes.GetData(), static_cast<int64>(OnnxBytes.Num())));

	const FString TargetRuntime(GUERLPolicyNneRuntimeName);
	ModelData->SetTargetRuntimes(MakeArrayView(&TargetRuntime, 1));

	const TSharedPtr<UE::NNE::IModelCPU> Model = Runtime->CreateModelCPU(ModelData);
	ModelData->RemoveFromRoot();
	if (!Model.IsValid())
	{
		return PolicyNetworkFail(
			OutError,
			FString::Printf(
				TEXT("NNE runtime '%s' failed to create a CPU model from ONNX (%d bytes)"),
				GUERLPolicyNneRuntimeName,
				OnnxBytes.Num()));
	}

	ModelInstance = Model->CreateModelInstanceCPU();
	if (!ModelInstance.IsValid())
	{
		Reset();
		return PolicyNetworkFail(OutError, TEXT("NNE failed to create a CPU model instance"));
	}

	const TConstArrayView<UE::NNE::FTensorDesc> InputDescs = ModelInstance->GetInputTensorDescs();
	const TConstArrayView<UE::NNE::FTensorDesc> OutputDescs = ModelInstance->GetOutputTensorDescs();
	if (InputDescs.Num() != 1 || OutputDescs.Num() != 1)
	{
		Reset();
		return PolicyNetworkFail(
			OutError,
			FString::Printf(
				TEXT("policy ONNX must have exactly one input and one output tensor (got %d / %d)"),
				InputDescs.Num(),
				OutputDescs.Num()));
	}

	FString WidthError;
	const int32 InWidth = FeatureWidthFromDesc(InputDescs[0], WidthError);
	if (InWidth <= 0)
	{
		Reset();
		return PolicyNetworkFail(OutError, WidthError.IsEmpty() ? TEXT("could not read input width") : WidthError);
	}
	const int32 OutWidth = FeatureWidthFromDesc(OutputDescs[0], WidthError);
	if (OutWidth <= 0)
	{
		Reset();
		return PolicyNetworkFail(OutError, WidthError.IsEmpty() ? TEXT("could not read output width") : WidthError);
	}

	TArray<UE::NNE::FTensorShape> InputShapes;
	InputShapes.Add(UE::NNE::FTensorShape::Make({1u, static_cast<uint32>(InWidth)}));
	if (ModelInstance->SetInputTensorShapes(InputShapes) != UE::NNE::IModelInstanceCPU::ESetInputTensorShapesStatus::Ok)
	{
		Reset();
		return PolicyNetworkFail(
			OutError,
			FString::Printf(TEXT("SetInputTensorShapes failed for input width %d"), InWidth));
	}

	CachedInputWidth = InWidth;
	CachedOutputWidth = OutWidth;
	InputScratch.SetNumUninitialized(InWidth);
	OutputScratch.SetNumUninitialized(OutWidth);
	return true;
}

bool FUERLPolicyNetwork::Evaluate(
	TConstArrayView<float> Observation,
	TArray<float>& OutAction,
	FString& OutError) const
{
	OutError.Reset();
	OutAction.Reset();

	if (!IsBuilt())
	{
		return PolicyNetworkFail(OutError, TEXT("policy network is not built"));
	}

	if (Observation.Num() != CachedInputWidth)
	{
		return PolicyNetworkFail(
			OutError,
			FString::Printf(
				TEXT("observation width %d does not match model input width %d"),
				Observation.Num(),
				CachedInputWidth));
	}

	FMemory::Memcpy(InputScratch.GetData(), Observation.GetData(), CachedInputWidth * sizeof(float));

	TArray<UE::NNE::FTensorBindingCPU> Inputs;
	TArray<UE::NNE::FTensorBindingCPU> Outputs;
	Inputs.Add({InputScratch.GetData(), static_cast<uint64>(CachedInputWidth * sizeof(float))});
	Outputs.Add({OutputScratch.GetData(), static_cast<uint64>(CachedOutputWidth * sizeof(float))});

	if (ModelInstance->RunSync(Inputs, Outputs) != UE::NNE::IModelInstanceCPU::ERunSyncStatus::Ok)
	{
		return PolicyNetworkFail(OutError, TEXT("NNE RunSync failed"));
	}

	OutAction = OutputScratch;
	return true;
}
