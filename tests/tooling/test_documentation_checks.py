"""Check actual documentation and prove that broken links and CLI examples fail."""
from __future__ import annotations

from pathlib import Path
from unittest.mock import Mock

import pytest

from scripts.check_docs import (
    REPO_ROOT,
    check_command,
    check_document,
    documented_commands,
    heading_anchors,
    project_documents,
    split_markdown,
)


def test_project_documentation_matches_links_and_cli() -> None:
    findings: list[str] = []
    checked = 0
    for path in project_documents(REPO_ROOT):
        errors, count = check_document(REPO_ROOT, path)
        findings.extend(f"{path.relative_to(REPO_ROOT)}: {error}" for error in errors)
        checked += count
    assert checked > 0
    assert not findings, "\n".join(findings)


def test_missing_file_and_heading_are_reported(tmp_path: Path) -> None:
    source = tmp_path / "README.md"
    target = tmp_path / "guide.md"
    target.write_text("# Correct heading\n")
    source.write_text("[missing](missing.md)\n[typo](guide.md#wrong-heading)\n[valid](guide.md#correct-heading)\n")
    errors, count = check_document(tmp_path, source)
    assert count == 0
    assert errors == [
        "line 1: missing link target: missing.md",
        "line 2: missing heading anchor: guide.md#wrong-heading",
    ]


def test_duplicate_unicode_and_formatted_headings() -> None:
    assert heading_anchors("# Use `Task`\n# Use `Task`\n# 中文 API\n") == {"use-task", "use-task-1", "中文-api"}


def test_reference_links_and_encoded_paths(tmp_path: Path) -> None:
    (tmp_path / "a file.md").write_text("# Target\n")
    source = tmp_path / "README.md"
    source.write_text('[good][target]\n[broken][absent]\n[target]: a%20file.md#target\n')
    errors, _ = check_document(tmp_path, source)
    assert errors == ["line 2: undefined link reference [absent]"]


def test_code_fences_and_inline_code_do_not_create_links(tmp_path: Path) -> None:
    source = tmp_path / "README.md"
    source.write_text("```text\n[example](missing.md)\n# Fake heading\n```\n`[example](missing.md)`\n")
    assert check_document(tmp_path, source) == ([], 0)
    assert heading_anchors(split_markdown(source.read_text()).prose) == set()


def test_unclosed_fence_is_an_error(tmp_path: Path) -> None:
    source = tmp_path / "README.md"
    source.write_text("# Guide\n\n```python\nprint('example')\n")
    errors, _ = check_document(tmp_path, source)
    assert errors == ["line 3: unclosed code fence"]


def test_multiline_shell_commands_keep_start_line() -> None:
    document = split_markdown(
        "# Guide\n```powershell\nuv run uerl play `\n  --run $runDir --steps 10\n```\n",
    )
    assert documented_commands(document.blocks) == [(3, ["play", "--run", "$runDir", "--steps", "10"])]


@pytest.mark.parametrize("arguments", [
    ["unknown"],
    ["play", "--run", "saved", "--stepz", "10"],
    ["train", "--num-envs", "2"],  # Missing required Task identifier.
    ["play", "--run", "saved", "--presentation", "not-a-mode"],
    ["play", "--run", "saved", "--steps", "ten"],
])
def test_bad_cli_examples_are_rejected(arguments: list[str]) -> None:
    assert check_command(arguments) is not None


@pytest.mark.parametrize("arguments", [
    ["train", "--task", "UERL-CartPole-Direct-v0", "--num-envs", "2"],
    ["play", "--run", "$runDir", "--terrain-level", "$_"],
    ["config", "--task", "<TaskID>", "--runner.max_iterations", "100"],
    ["check", "host", "--project", "E:/host/host.uproject"],
    ["new", "external-cartpole", "example", "--output-dir", "../example"],
])
def test_supported_cli_examples_are_parsed(arguments: list[str]) -> None:
    assert check_command(arguments) is None


def test_cli_check_never_runs_training(monkeypatch: pytest.MonkeyPatch) -> None:
    command = Mock(side_effect=AssertionError("must not execute documentation"))
    monkeypatch.setattr("uerl.cli.train.main", command)
    assert check_command(["train", "--task", "UERL-CartPole-Direct-v0"]) is None
    command.assert_not_called()


def test_external_urls_are_not_requested(tmp_path: Path) -> None:
    source = tmp_path / "README.md"
    source.write_text("[external](https://example.invalid/missing#anchor)\n")
    assert check_document(tmp_path, source) == ([], 0)


def test_source_links_cannot_read_outside_checkout(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    source = root / "README.md"
    source.write_text("[outside](../secret.md#heading)\n")
    errors, _ = check_document(root, source)
    assert errors == ["line 1: link leaves the checkout: ../secret.md#heading"]


def test_unfinished_cli_continuation_is_rejected(tmp_path: Path) -> None:
    source = tmp_path / "README.md"
    source.write_text("```powershell\nuv run uerl play `\n```\n")
    errors, count = check_document(tmp_path, source)
    assert count == 0
    assert errors == ["invalid shell quoting: line 2: unfinished command continuation"]


def test_angle_destination_parentheses_and_explicit_anchor(tmp_path: Path) -> None:
    (tmp_path / "a file (1).md").write_text('<a id="named"></a>\n')
    source = tmp_path / "README.md"
    source.write_text('[valid](<a file (1).md#named>)\n')
    assert check_document(tmp_path, source) == ([], 0)


@pytest.mark.parametrize("tokens", [[], ["--help"], ["play", "--help"]])
def test_help_examples_are_valid(tokens: list[str]) -> None:
    assert check_command(tokens) is None
