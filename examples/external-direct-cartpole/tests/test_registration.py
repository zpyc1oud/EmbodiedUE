from example_direct_cartpole import TASK_ID, create_registration

from uerl.core.direct.capabilities import CapabilityStatus
from uerl.core.direct.task import DirectTask


def test_registration_builds_a_direct_task_and_reports_capabilities() -> None:
    registration = create_registration()
    config = registration.task_config_factory()
    task = registration.task_factory(config)

    assert registration.task_id == "UERL-DirectCartPole-v0"
    assert isinstance(task, DirectTask)
    assert task.capabilities.train.status is CapabilityStatus.SUPPORTED
    assert task.capabilities.evaluate.status is CapabilityStatus.SUPPORTED
    assert task.capabilities.export.status is CapabilityStatus.UNSUPPORTED
    assert task.capabilities.export.reason is not None
    assert "Manager-generated" in task.capabilities.export.reason
    assert TASK_ID == registration.task_id
