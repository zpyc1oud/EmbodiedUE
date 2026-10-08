# PhantomX variant reference values

These cases use the tracking-time v2 objective from Issue #40.
Observation, action, and termination values keep their earlier reviewed meaning.
The change removes the stationary reference subtraction from tracking rewards.

The root rotation is identity. Forward velocity is 0.35 m/s and yaw rate is zero.
Walk and terrain commands request 0.4 m/s. Pursuit requests 0.45 m/s.
For the 20 ms control interval, the reward rate multiplier is one.

The independent scalar calculations are:

- Walk and terrain linear tracking: 1.5 × exp(-(0.05 / 0.2)^2).
- Pursuit linear tracking: 1.5 × exp(-(0.10 / 0.2)^2).
- Yaw tracking: 0.75, because both commanded and measured yaw rates are zero.
- Linear progress: 0.35 / 0.4 for walk/terrain; 0.35 / 0.45 for pursuit.
- Other terms: zero for these input states.

The total is the sum of these contributions. The stored constants were calculated
with scalar arithmetic, without importing or executing the product reward code.
The existing 1e-6 absolute tolerance covers the fixture's float32 rounding.
The tolerance was not increased for the new objective.

These fixtures establish task mathematics. They do not establish learning quality
or contact behavior in Chaos. New stationary and drifting rows are also asserted
through the public Task interface in the Python unit tests.
