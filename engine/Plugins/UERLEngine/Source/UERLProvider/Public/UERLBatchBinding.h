#pragma once

#include "CoreMinimal.h"
#include "UERLBatchStaging.h"

/**
 * Bind a Bridge-selected schema to Worker-side row-major staging columns.
 *
 * The binding contains no wire offsets or hashes; those remain owned by the
 * Transport layout compiler.
 */
class UERLPROVIDER_API FUERLBatchBinding
{
public:
	/**
	 * Compile selected fields against the provider-published descriptors.
	 *
	 * @param Schema Supply the selected Action/State field order.
	 * @param AvailableActions Supply fields published by the Robot.
	 * @param AvailableStates Supply fields published by the Environment and Robot.
	 * @param OutError Receive a diagnostic when metadata does not match.
	 * @return true when all selected fields have valid staging bindings.
	 */
	bool Compile(
		const FUERLBatchSchema& Schema,
		const TArray<FUERLFieldDescriptor>& AvailableActions,
		const TArray<FUERLFieldDescriptor>& AvailableStates,
		FString& OutError);
	/** Clear compiled bindings and return to the uncompiled state. */
	void Reset();

	/** Find one selected Action field by its stable name. */
	const FUERLBatchFieldBinding* FindAction(FName Name) const { return Actions.Find(Name); }
	/** Find one selected State field by its stable name. */
	const FUERLBatchFieldBinding* FindState(FName Name) const { return States.Find(Name); }
	/** Return the compiled Action staging width. */
	int32 ActionWidth() const { return CompiledActionWidth; }
	/** Return the compiled State staging width. */
	int32 StateWidth() const { return CompiledStateWidth; }
	/** Return whether Compile completed successfully. */
	bool IsCompiled() const { return bCompiled; }

private:
	TMap<FName, FUERLBatchFieldBinding> Actions;
	TMap<FName, FUERLBatchFieldBinding> States;
	int32 CompiledActionWidth = 0;
	int32 CompiledStateWidth = 0;
	bool bCompiled = false;
};

class UERLPROVIDER_API FUERLNamedActionReader
{
public:
	/** Create a named reader over one compiled Action staging view. */
	FUERLNamedActionReader(const FUERLBatchBinding& InBinding, FUERLConstBatchView InView)
		: Binding(InBinding), View(InView) {}

	/** Return the number of Slot rows in the Action view. */
	int32 NumRows() const { return View.NumRows; }
	/** Read one selected field's contiguous scalar span from a Slot row. */
	bool ReadVector(int32 Row, FName Field, TArrayView<float> OutValues) const;
	/** Read one scalar field value from a Slot row. */
	bool ReadScalar(int32 Row, FName Field, float& OutValue) const;

private:
	const FUERLBatchBinding& Binding;
	FUERLConstBatchView View;
};

class UERLPROVIDER_API FUERLNamedStateWriter
{
public:
	/** Create a named writer over one compiled State staging view. */
	FUERLNamedStateWriter(const FUERLBatchBinding& InBinding, FUERLMutableBatchView InView)
		: Binding(InBinding), View(InView) {}

	/** Return the number of Slot rows in the State view. */
	int32 NumRows() const { return View.NumRows; }
	/** Return the number of scalar columns in the State view. */
	int32 NumColumns() const { return View.NumColumns; }
	/** Write one selected field's contiguous scalar span to a Slot row. */
	bool WriteVector(int32 Row, FName Field, TConstArrayView<float> Values);
	/** Write one scalar field value to a Slot row. */
	bool WriteScalar(int32 Row, FName Field, float Value);
	/** Fill selected Slot rows with one scalar value. */
	void FillRows(const TArray<int32>& Rows, float Value);
	/** Fill every State scalar with one value. */
	void FillAll(float Value);

private:
	const FUERLBatchBinding& Binding;
	FUERLMutableBatchView View;
};
