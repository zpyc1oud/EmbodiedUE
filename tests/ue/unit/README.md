# UE unit tests

Native UE Automation tests compile with their UBT modules. Their sources are located in:

- `engine/Plugins/UERLEngine/Source/UERLInterface/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTransport/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTerrain/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLWorker/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLRobot/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLPolicy/Private/Tests/Unit/`

`UERLRobot` cases cover topology reflection, observation plans, kinematics, contact, provider registration/configuration rejection, and real CartPole content smoke tests. The content smoke requires the repository Skeletal Mesh and PhysicsAsset and must not silently skip missing assets.

This directory indexes the tests; it does not duplicate C++ sources outside their compilable modules. See [the test guide](../../README.md) for commands.
