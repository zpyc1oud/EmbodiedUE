# Product direction

## Purpose

EmbodiedUE is a robot training and deployment platform for Unreal Engine game developers.
Its goal is to make physics-driven robots useful as interactive game entities.
Developers train control policies in Chaos and deploy them in UE games.

Adaptive behavior means that a trained controller changes its actions in response to observations and commands.
It does not imply that the deployed game trains new weights during play.
The response depends on the robot, observations, training conditions, and learned policy.

Physical motion comes from simulated bodies, joints, actuators, and contacts.
This gives the game a physically grounded control path.
It does not establish real-world accuracy for an unvalidated asset or solver configuration.

## Who the platform serves

The primary users are game developers who can build a UE project and configure a Python training task.
They need a clear path from a robot asset to a controller that works in their game.

- Technical designers define the behavior, commands, and success conditions.
- Technical artists prepare the robot mesh, bodies, joints, and collision geometry.
- Programmers connect gameplay logic to the runtime and add reusable platform features.

A project can combine these roles in one person.
The platform should reduce the need to change framework internals for each robot or task.

## Gameplay and physical control

Game logic selects goals and supplies commands.
The policy converts supported observations and commands into robot actions.
Chaos advances the physical state.
The game reads that state and continues its own logic.

Task-level logic can include navigation or an existing game AI system.
Its integration must use the selected controller's command contract.
The platform's main responsibility is the training-to-game physical control path.

Training and gameplay can use different maps.
A shared physics backend and exported plans help preserve control semantics.
They do not make arbitrary maps equivalent.
Geometry, input sources, units, coordinate frames, timing, and actuator behavior must satisfy the supported deployment contract.

## Current foundation

The repository contains:

- CartPole and PhantomX reference robot integrations.
- Python Task definitions, reward composition, and PPO training through rsl-rl.
- Batched Chaos execution and saved training runs.
- Evaluation, recording, policy export, and UE runtime integration.
- Observation and action plans packaged with an ONNX policy.
- Software tests and Windows/UE validation procedures.

These are infrastructure capabilities.
Policy quality and game-scene behavior require separate measurements.
Current examples do not establish reliable adaptation to every terrain or disturbance.
See the [README](../README.md) for commands and the [release roadmap](roadmap/next-release.md) for required outcomes.

## Design priorities

1. **Start with the game use case.** Define the commands, observed state, physical response, and success criteria that a game needs.
2. **Keep training and deployment aligned.** Preserve the trained observation and action semantics in the exported artifact and runtime.
3. **Make robot integrations reusable.** Put robot-specific structure in assets and declarations. Extend the common runtime when a new physical capability needs it.
4. **Support new inputs through shared contracts.** Future terrain and visual inputs should serve both training and gameplay. This is an extension goal, not a claim of complete visual support.
5. **Measure useful behavior.** Check task success, tracking error, stability, latency, throughput, and memory within a declared scene and configuration.
6. **Keep the first-use path clear.** Give developers a small working example, useful diagnostics, and a documented route into their own project.

The [architecture guide](architecture.md) defines current system boundaries.
Design proposals remain subject to review and runtime validation.

## Relationship to Isaac Lab

Isaac Lab is a design reference for task configuration, training workflows, and tests.
EmbodiedUE uses those ideas where they help game developers.
Its product goal is physical robot control inside Unreal games, rather than replacing Isaac Lab's robotics simulation and research workflows.
Isaac Sim and Isaac Lab are not runtime dependencies.

## Release and showcase

A public showcase should demonstrate the complete path: prepare a robot, train a controller, evaluate it, and run it in a game.
The scene should show the controller's physical response clearly.
A reusable platform must also let another developer follow that path in the supported setup.

Keep proposed gameplay features separate from release claims.
Publish measured behavior, supported conditions, and known limitations with each release.
The [release readiness guide](release-readiness.md) also covers licensing, content, and packaging.
