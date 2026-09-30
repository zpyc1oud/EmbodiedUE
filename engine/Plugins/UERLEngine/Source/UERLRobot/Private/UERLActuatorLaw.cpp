#include "UERLActuatorLaw.h"

namespace UERLRobot
{
	double ComputeUnifiedActuatorEffort(
		const FUERLActuatorLawConfig& Config,
		double Target,
		double Position,
		double Velocity)
	{
		const double EffortTarget = Config.Stiffness == 0.0 && Config.Damping > 0.0 ? Target : 0.0;
		const double RawEffort = Config.Stiffness * (Target - Position)
			- Config.Damping * Velocity
			+ EffortTarget;
		return FMath::Clamp(RawEffort, -Config.EffortLimit, Config.EffortLimit);
	}
}
