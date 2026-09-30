#pragma once

#include "CoreMinimal.h"
#include "UERLPlan.h"

/**
 * Closed plan-operator registry for observation/action assembly.
 *
 * Source vs transform (mirrors Python ``operators.py``):
 * - Source ops (``select``, ``command``, ``control_frame_dt``,
 *   ``previous_action``, ``policy_action``)
 *   have ``Evaluate == nullptr``. ``FUERLPlanRuntime`` specializes them against
 *   ``FUERLPlanInputs``.
 * - Transform ops (``concat``, ``rotate_inverse``, ``projected_gravity``, …)
 *   are pure: ``Evaluate`` reads input spans and writes the output span.
 *
 * Plan quaternions are **xyzw** (see Docs/Operators.md). C++ builds
 * ``FQuat(Q[0], Q[1], Q[2], Q[3])`` to match.
 *
 * Operator admission: a new operator must land with (1) a Python registry entry
 * and executor wiring, (2) a C++ ``RegisterPlanOperator`` entry, (3) at least
 * one parity corpus case under ``tests/parity/cases/<op>/``, and (4) a recorded
 * negative self-check that a deliberate C++ bug fails the corpus. Missing any
 * of the four blocks merge.
 */

struct FUERLPlanOpContext
{
	TArray<TConstArrayView<float>> Inputs;
	TArrayView<float> Output;
	const FUERLPlanParams* Params = nullptr;
};

struct FUERLPlanOperator
{
	FName Name;
	/** Fixed arity, or -1 for variadic (non-negative input count). */
	int32 Arity = 0;
	int32 (*OutputWidth)(TConstArrayView<int32> InputWidths, const FUERLPlanParams& Params) = nullptr;
	/** Null for source operators specialized by the runtime. */
	void (*Evaluate)(const FUERLPlanOpContext& Context) = nullptr;
};

UERLPOLICY_API void RegisterPlanOperator(const FUERLPlanOperator& Operator);
UERLPOLICY_API const FUERLPlanOperator* FindPlanOperator(FName Name);

/** Register the closed built-in set (sources + transforms). Idempotent. */
UERLPOLICY_API void RegisterBuiltinPlanOperators();
