"""Exercise the explicit external Task registration boundary."""

from importlib.metadata import EntryPoint, EntryPoints

import pytest

from uerl.tasks.registry import create_default_registry


def test_only_enabled_entry_point_is_loaded(monkeypatch: pytest.MonkeyPatch) -> None:
    points = EntryPoints([
        EntryPoint(name="cartpole", value="uerl.tasks.cartpole.registration:create_cartpole_registration",
                   group="uerl.tasks"),
        EntryPoint(name="unused", value="missing_package:factory", group="uerl.tasks"),
    ])
    monkeypatch.setattr("importlib.metadata.entry_points", lambda **kwargs: points)
    assert len(create_default_registry(external_tasks=()).list()) == 6
    # Loading the real factory proves discovery reaches existing duplicate validation.
    from uerl.errors import RegistryError
    with pytest.raises(RegistryError, match="cartpole.*already registered") as error:
        create_default_registry(external_tasks=("cartpole",))
    assert error.value.code == "DUPLICATE_TASK_ID"


@pytest.mark.parametrize(
    ("value", "code", "message"),
    [
        ("uerl.tasks.cartpole.config:CARTPOLE_TASK_ID", "INVALID_FACTORY", "callable"),
        ("builtins:dict", "INVALID_REGISTRATION", "TaskRegistration"),
        ("missing_package:factory", "EXTERNAL_TASK_LOAD_FAILED", "missing_package"),
        ("builtins:len", "EXTERNAL_TASK_LOAD_FAILED", "zero-argument"),
    ],
)
def test_invalid_declarations_explain_the_source(
    monkeypatch: pytest.MonkeyPatch, value: str, code: str, message: str,
) -> None:
    from uerl.errors import RegistryError
    points = EntryPoints([EntryPoint(name="broken", value=value, group="uerl.tasks")])
    monkeypatch.setattr("importlib.metadata.entry_points", lambda **kwargs: points)
    with pytest.raises(RegistryError, match=message) as error:
        create_default_registry(external_tasks=("broken",))
    assert error.value.code == code
    assert "broken" in str(error.value)
    assert value in str(error.value)


def test_missing_and_ambiguous_names_fail_before_import(monkeypatch: pytest.MonkeyPatch) -> None:
    from uerl.errors import RegistryError
    point = EntryPoint(name="ambiguous", value="missing:factory", group="uerl.tasks")
    monkeypatch.setattr("importlib.metadata.entry_points", lambda **kwargs: EntryPoints([point, point]))
    with pytest.raises(RegistryError) as error:
        create_default_registry(external_tasks=("missing",))
    assert error.value.code == "EXTERNAL_TASK_NOT_FOUND"
    with pytest.raises(RegistryError) as error:
        create_default_registry(external_tasks=("ambiguous",))
    assert error.value.code == "DUPLICATE_ENTRY_POINT"


def test_cli_reports_duplicate_without_recursive_suggestions(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str],
) -> None:
    from uerl.cli.main import main
    point = EntryPoint(name="duplicate", value="uerl.tasks.cartpole.registration:create_cartpole_registration",
                       group="uerl.tasks")
    monkeypatch.setattr("importlib.metadata.entry_points", lambda **kwargs: EntryPoints([point]))
    monkeypatch.setenv("UERL_TASK_PLUGINS", " duplicate ")
    assert main(["tasks"]) == 1
    assert "already registered" in capsys.readouterr().out
    assert len(create_default_registry(external_tasks=()).list()) == 6
