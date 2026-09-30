#pragma once

#include "CoreMinimal.h"

namespace UERLRobot
{
	/** Configure one scalar actuator in SI units for the unified control law. */
	struct FUERLActuatorLawConfig
	{
		/** Position stiffness gain in effort per position unit. */
		double Stiffness = 0.0;
		/** Velocity damping gain in effort per velocity unit. */
		double Damping = 0.0;
		/** Absolute effort clamp. */
		double EffortLimit = 0.0;
	};

	/**
	 * Compute one clamped effort from one indexed target and measured joint state.
	 *
	 * Positive stiffness interprets Target as a position target. With zero
	 * stiffness and positive damping, Target is the effort feed-forward value;
	 * with both gains zero, the actuator is passive. All three cases share this
	 * one calculation and one effort limit.
	 */
	UERLROBOT_API double ComputeUnifiedActuatorEffort(
		const FUERLActuatorLawConfig& Config,
		double Target,
		double Position,
		double Velocity);
}
