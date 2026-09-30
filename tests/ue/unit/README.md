# UE unit tests

UE 原生单元 Automation 测试必须随 UBT module 编译，实际文件位于：

- `engine/Plugins/UERLEngine/Source/UERLInterface/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTransport/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTerrain/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLWorker/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLRobot/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLPolicy/Private/Tests/Unit/`

`UERLRobot` 测试覆盖拓扑反射、观测计划、运动学、contact、provider 注册与配置拒绝，以及真实 CartPole 内容 smoke；真实内容 smoke 依赖仓库内 CartPole SkeletalMesh/PhysicsAsset，不应在缺失时静默跳过。

本目录作为 UE 层测试索引，不复制一份无法被 UBT 编译的 C++ 测试。
