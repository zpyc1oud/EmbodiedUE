# UE unit tests

Native UE Automation tests compile in their UBT modules.
The C++ source directories are:

- `engine/Plugins/UERLEngine/Source/UERLInterface/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTransport/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTerrain/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLWorker/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLRobot/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLPolicy/Private/Tests/Unit/`

`UERLRobot` cases cover topology reflection, observation plans, kinematics, contact, and provider registration.
`UERL.Unit.Robot.GroundQuery.Bindings` checks the extracted ground-query backend
with analytic floor/step heights, explicit owner filtering, ignored actors,
empty ownership, and a disjoint no-hit region.
They also cover rejected configurations and real CartPole content smoke tests.
The content smoke requires the repository Skeletal Mesh and PhysicsAsset.
Report missing assets as a failure or blocker, not a silent skip.

This directory is an index.
Keep each C++ source in its compilable module.
Use the [test guide](../../README.md) for commands.
Use [Write tests](../../../docs/how-to/write-tests.md) for test implementation and review.
