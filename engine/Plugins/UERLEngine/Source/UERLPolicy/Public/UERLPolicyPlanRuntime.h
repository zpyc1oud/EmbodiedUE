#pragma once

#include "CoreMinimal.h"
#include "UERLInterfaceTypes.h"
#include "UERLPlan.h"
#include "UERLPolicyOperators.h"

/**
 * Runtime inputs for source operators.
 *
 * ``RawState`` is one env row packed in ``AvailableState`` order (the layout
 * passed to ``Compile``). ``Commands`` / ``PreviousAction`` / ``PolicyAction``
 * feed the ``command`` / ``previous_action`` / ``policy_action`` source ops.
 * ``ControlFrameDtSeconds`` is the just-completed control interval in seconds
 * for the ``control_frame_dt`` source op.
 *
 * Error timing for ``command``:
 * - **Compile**: plan width vs ``AvailableCommands`` (``channels()``) width.
 * - **Execute**: channel absent from ``Commands`` (no silent zero-fill).
 *
 * Error timing for action ``command_fields``:
 * - **Compile**: produced slot width vs ``AvailableCommands`` physical width.
 */
struct FUERLPlanInputs
{
	TConstArrayView<float> RawState;
	TMap<FName, TArray<float>> Commands;
	TConstArrayView<float> PreviousAction;
	TConstArrayView<float> PolicyAction;
	float ControlFrameDtSeconds = 0.0f;
};

/**
 * Compile an observation or action plan once; evaluate against ``FUERLPlanInputs``.
 *
 * Observation: deployment consumes the ``policy`` group only — ``Execute``
 * writes that group (member slots concatenated in declared order) into
 * ``OutValues``.
 *
 * Action: ``Execute`` concatenates ``command_fields`` slots in declared order
 * into ``OutValues`` (same evaluation loop; only the input channel and output
 * slot list differ).
 */
class UERLPOLICY_API FUERLPlanRuntime
{
public:
	/**
	 * Resolve operators, bind slot names to indices, resolve ``select`` field
	 * offsets from ``AvailableState``, check ``command`` widths against
	 * ``AvailableCommands``, and allocate the slot buffer. Failure leaves the
	 * runtime uncompiled (no partial state).
	 */
	bool Compile(
		const FUERLObservationPlan& Plan,
		const TArray<FUERLFieldDescriptor>& AvailableState,
		FString& OutError);

	bool Compile(
		const FUERLObservationPlan& Plan,
		const TArray<FUERLFieldDescriptor>& AvailableState,
		const TMap<FName, int32>& AvailableCommands,
		FString& OutError);

	/**
	 * Compile an action plan. ``AvailableCommands`` is the physical-command
	 * width map keyed by ``command_fields`` names (empty skips the width check).
	 */
	bool Compile(
		const FUERLActionPlan& Plan,
		const TMap<FName, int32>& AvailableCommands,
		FString& OutError);

	bool Execute(const FUERLPlanInputs& Inputs, TArray<float>& OutValues, FString& OutError);
	int32 OutputWidth() const;

	bool IsCompiled() const { return bCompiled; }
	void Reset();

private:
	enum class ESourceKind : uint8
	{
		None = 0,
		Select,
		Command,
		ControlFrameDt,
		PreviousAction,
		PolicyAction,
	};

	struct FCompiledOp
	{
		const FUERLPlanOperator* Operator = nullptr;
		TArray<int32> InputSlots;
		int32 OutputSlot = INDEX_NONE;
		int32 Width = 0;
		FUERLPlanParams Params;
		/** For ``select``: element index into ``RawState``. */
		int32 StateOffset = INDEX_NONE;
		bool bIsSource = false;
		ESourceKind SourceKind = ESourceKind::None;
		FName CommandChannel;
	};

	bool CompileOps(
		const TArray<FUERLPlanOp>& PlanOps,
		const TArray<FUERLFieldDescriptor>* AvailableState,
		const TMap<FName, int32>* AvailableCommandsForCommandOps,
		TMap<FName, int32>& OutSlotByName,
		FString& OutError);

	bool bCompiled = false;
	TArray<FCompiledOp> Ops;
	TArray<float> SlotBuffer;
	TArray<int32> SlotOffsets;
	TArray<int32> SlotWidths;
	TArray<int32> OutputSlots;
	int32 CachedOutputWidth = 0;
	int32 ExpectedRawStateWidth = 0;
};
