"""Persist generic curriculum state with RSL-RL checkpoints."""

from __future__ import annotations

from collections.abc import Collection, Mapping

from rsl_rl.runners import OnPolicyRunner

from ...core.config import ResolvedRunConfig
from ...core.config.snapshot import CHECKPOINT_CONFIG_KEY, resolved_config_to_yaml
from .time_aware_ppo import PHANTOMX_PHYSICAL_TIME_OBJECTIVE, TimeAwarePPO
from .vecenv import UERLVecEnvWrapper

_CURRICULUM_CHECKPOINT_KEY = "uerl_curriculum"
_DIRECT_ENV_RANDOM_STATE_KEY = "uerl_direct_env_random"
_TRAINING_OBJECTIVE_KEY = "uerl_training_objective"
TRAINING_OPTIONS_KEY = "uerl_training_options"


class UERLOnPolicyRunner(OnPolicyRunner):  # type: ignore[misc]
    """Extend the pinned runner only with curriculum checkpoint ownership."""

    env: UERLVecEnvWrapper
    training_options: Mapping[str, object] | None = None
    resolved_config: ResolvedRunConfig | None = None

    def save(self, path: str, infos: dict[str, object] | None = None) -> None:
        """Save policy state and the environment's named curriculum terms."""

        checkpoint_infos = dict(infos) if infos is not None else {}
        if self.resolved_config is not None:
            checkpoint_infos[CHECKPOINT_CONFIG_KEY] = resolved_config_to_yaml(self.resolved_config)
        if self.training_options is not None:
            checkpoint_infos[TRAINING_OPTIONS_KEY] = dict(self.training_options)
        if isinstance(self.alg, TimeAwarePPO):
            checkpoint_infos[_TRAINING_OBJECTIVE_KEY] = PHANTOMX_PHYSICAL_TIME_OBJECTIVE
        curriculum_state = self.env.direct_env.curriculum_state_dict()
        if curriculum_state:
            checkpoint_infos[_CURRICULUM_CHECKPOINT_KEY] = curriculum_state
        random_state_method = getattr(type(self.env.direct_env), "random_state_dict", None)
        if callable(random_state_method):
            random_state = self.env.direct_env.random_state_dict()
            if random_state:
                checkpoint_infos[_DIRECT_ENV_RANDOM_STATE_KEY] = random_state
        super().save(path, checkpoint_infos or None)

    def load(
        self,
        path: str,
        load_cfg: dict[str, object] | None = None,
        strict: bool = True,
        map_location: str | None = None,
        restore_curriculum: bool = True,
        skip_curriculum_terms: Collection[str] = (),
    ) -> dict[str, object]:
        """Load policy state and optionally restore named curriculum terms.

        ``skip_curriculum_terms`` names checkpoint terms that this Run
        deliberately does not register; every other term must still match.
        """

        infos = super().load(path, load_cfg=load_cfg, strict=strict, map_location=map_location)
        if restore_curriculum and isinstance(infos, Mapping) and _CURRICULUM_CHECKPOINT_KEY in infos:
            state = infos[_CURRICULUM_CHECKPOINT_KEY]
            if not isinstance(state, Mapping):
                raise ValueError("checkpoint curriculum state must be a mapping")
            self.env.direct_env.load_curriculum_state_dict(
                {name: term for name, term in state.items() if name not in skip_curriculum_terms}
            )
        if isinstance(infos, Mapping) and _DIRECT_ENV_RANDOM_STATE_KEY in infos:
            state = infos[_DIRECT_ENV_RANDOM_STATE_KEY]
            if not isinstance(state, Mapping):
                raise ValueError("checkpoint DirectEnv random state must be a mapping")
            load_random_state_method = getattr(type(self.env.direct_env), "load_random_state_dict", None)
            if not callable(load_random_state_method):
                raise ValueError("checkpoint contains DirectEnv random state but the environment cannot restore it")
            self.env.direct_env.load_random_state_dict(state)
        return dict(infos) if isinstance(infos, Mapping) else {}

    def freeze_observation_normalization(self) -> None:
        """Keep loaded actor and critic statistics fixed during warm-start training."""

        self.alg.actor.obs_normalization = False
        self.alg.critic.obs_normalization = False


__all__ = ["UERLOnPolicyRunner"]
