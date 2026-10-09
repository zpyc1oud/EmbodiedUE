"""Host-only entry points must not allocate a training runtime in the parent."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _probe(source: str) -> None:
    env = {**os.environ, "PYTHONPATH": os.pathsep.join((str(REPO / "src"), str(REPO)))}
    result = subprocess.run(
        [sys.executable, "-c", source], cwd=REPO, env=env, capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_host_and_native_runner_import_without_torch() -> None:
    _probe('''
import importlib.abc
import sys
class RejectTorch(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "torch" or fullname.startswith("torch."):
            raise AssertionError("host-only import attempted to load torch")
sys.meta_path.insert(0, RejectTorch())
import uerl
from uerl.host import resolve_host_profile
from scripts.test_runner_support import StageResult, positive_seconds
assert "torch" not in sys.modules
assert positive_seconds("2") == 2.0
assert StageResult("native").status == "not_run"
assert resolve_host_profile is not None
assert "UERLDirectEnv" in dir(uerl)
try:
    uerl.no_such_public_type
except AttributeError:
    pass
else:
    raise AssertionError("unknown attribute did not fail")
''')


def test_public_types_retain_identity_and_cache_after_lazy_resolution() -> None:
    _probe('''
import uerl
from uerl import UERLDirectEnv, RobotConfig, UERLSession, CartPoleTaskConfig, ConfigError
from uerl.core.direct import UERLDirectEnv as DirectType
from uerl.core.config import RobotConfig as RobotType
from uerl.runtime.session import UERLSession as SessionType
from uerl.tasks.cartpole import CartPoleTaskConfig as TaskType
from uerl.errors import ConfigError as ErrorType
assert UERLDirectEnv is DirectType
assert RobotConfig is RobotType
assert UERLSession is SessionType
assert CartPoleTaskConfig is TaskType
assert ConfigError is ErrorType
for name in uerl.__all__:
    value = getattr(uerl, name)
    assert getattr(uerl, name) is value
    assert vars(uerl)[name] is value
''')
