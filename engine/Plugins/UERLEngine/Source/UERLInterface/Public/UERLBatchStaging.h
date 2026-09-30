#pragma once

#include "CoreMinimal.h"
#include "UERLInterfaceTypes.h"

/**
 * Bind one named field to a Worker-side row-major staging column.
 *
 * The Bridge still owns field-major wire placement; Column only identifies
 * where the Worker reads or writes the selected field in its staging batch.
 */
struct FUERLBatchFieldBinding
{
	/** Store the selected field metadata. */
	FUERLFieldDescriptor Field;
	/** Store the first scalar column occupied by the field. */
	int32 Column = INDEX_NONE;
};

/** Store the selected Action/State schema and Worker staging widths. */
struct FUERLBatchSchema
{
	/** Store Action fields in the Task-requested order. */
	TArray<FUERLBatchFieldBinding> ActionFields;
	/** Store State fields in the Task-requested order. */
	TArray<FUERLBatchFieldBinding> StateFields;
	/** Store the total scalar width of the Action staging batch. */
	int32 ActionWidth = 0;
	/** Store the total scalar width of the State staging batch. */
	int32 StateWidth = 0;
	/** Store the fixed scalar width of the explicit Robot reset vector. */
	int32 ResetWidth = 0;

	/** Validate selected fields, uniqueness, and staging widths. */
	UERLINTERFACE_API bool IsValid() const;
};

/** Provide a read-only row-major view over one Worker staging batch. */
struct FUERLConstBatchView
{
	/** Point at the first scalar in the staging batch without taking ownership. */
	const float* Data = nullptr;
	/** Store the number of Slot rows in the view. */
	int32 NumRows = 0;
	/** Store the number of scalar columns in each row. */
	int32 NumColumns = 0;

	/** Return whether the view points at a non-empty staging batch. */
	bool IsValid() const { return Data && NumRows > 0 && NumColumns > 0; }
	/** Return one row-major scalar without bounds checking. */
	float At(int32 Row, int32 Column) const
	{
		return Data[static_cast<int64>(Row) * NumColumns + Column];
	}
};

/** Provide a mutable row-major view over one Worker staging batch. */
struct FUERLMutableBatchView
{
	/** Point at the first writable scalar without taking ownership. */
	float* Data = nullptr;
	/** Store the number of Slot rows in the view. */
	int32 NumRows = 0;
	/** Store the number of scalar columns in each row. */
	int32 NumColumns = 0;

	/** Return whether the view points at a non-empty staging batch. */
	bool IsValid() const { return Data && NumRows > 0 && NumColumns > 0; }
	/** Return one writable row-major scalar without bounds checking. */
	float& At(int32 Row, int32 Column) const
	{
		return Data[static_cast<int64>(Row) * NumColumns + Column];
	}
};