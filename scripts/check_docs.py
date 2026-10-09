"""Check project Markdown links and parse documented uerl commands without running them."""

from __future__ import annotations

import argparse
import contextlib
import html
import importlib
import io
import re
import shlex
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import cast
from urllib.parse import unquote, urlsplit

REPO_ROOT = Path(__file__).resolve().parents[1]


@dataclass
class Markdown:
    prose: str
    blocks: list[tuple[int, str, list[str]]]
    errors: list[str]


def split_markdown(text: str) -> Markdown:
    """Keep prose line numbers and exclude fenced examples from link checks."""
    prose: list[str] = []
    blocks: list[tuple[int, str, list[str]]] = []
    errors: list[str] = []
    fence = ""
    language = ""
    start = 0
    lines: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        marker = re.match(r"^\s*(`{3,}|~{3,})(.*)$", line)
        if not fence and marker:
            fence, language = marker.group(1), marker.group(2).strip().lower()
            start, lines = number, []
            prose.append("")
        elif fence:
            prose.append("")
            if (
                marker and marker.group(1)[0] == fence[0]
                and len(marker.group(1)) >= len(fence) and not marker.group(2).strip()
            ):
                blocks.append((start, language, lines))
                fence = ""
            else:
                lines.append(line)
        else:
            prose.append(line)
    if fence:
        errors.append(f"line {start}: unclosed code fence")
    return Markdown("\n".join(prose), blocks, errors)


def heading_anchors(prose: str) -> set[str]:
    """Generate GitHub-style anchors, including duplicate heading suffixes."""
    anchors = set(re.findall(r'<(?:a|span)\s+[^>]*(?:id|name)=["\']([^"\']+)["\']', prose))
    lines = prose.splitlines()
    for index, line in enumerate(lines):
        match = re.match(r"^\s{0,3}#{1,6}\s+(.+?)(?:\s+#+)?\s*$", line)
        title = match.group(1) if match else None
        if title is None and index and re.fullmatch(r"\s{0,3}(?:=+|-+)\s*", line) and lines[index - 1].strip():
            title = lines[index - 1].strip()
        if title is None:
            continue
        title = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", title)
        title = html.unescape(re.sub(r"<[^>]*>", "", title)).lower()
        slug = re.sub(r"[^\w\-\s]", "", title).replace(" ", "-")
        candidate = slug
        suffix = 0
        while candidate in anchors:
            suffix += 1
            candidate = f"{slug}-{suffix}"
        anchors.add(candidate)
    return anchors


# The project uses inline and reference links. Angle brackets allow spaces in a destination.
_DESTINATION = r"(<[^>\n]+>|(?:\\.|[^()\s]|\([^()\n]*\))+)"
_INLINE_LINK = re.compile(r"!?\[[^\]\n]*\]\(" + _DESTINATION + r"(?:\s+[\"'][^\"']*[\"'])?\)")
_REFERENCE = re.compile(r"^\s*\[([^\]]+)\]:\s*(<[^>]+>|\S+)")


def check_links(root: Path, source: Path, prose: str) -> list[str]:
    errors: list[str] = []
    definitions: dict[str, str] = {}
    for line in prose.splitlines():
        if match := _REFERENCE.match(line):
            definitions[" ".join(match.group(1).lower().split())] = match.group(2)
    for number, original in enumerate(prose.splitlines(), 1):
        line = re.sub(r"(`+).*?\1", "", original)
        destinations = [match.group(1) for match in _INLINE_LINK.finditer(line)]
        definition = _REFERENCE.match(line)
        if definition:
            destinations.append(definition.group(2))
        for match in re.finditer(r"!?\[([^\]]+)\]\[([^\]]*)\]", line):
            key = " ".join((match.group(2) or match.group(1)).lower().split())
            if key in definitions:
                destinations.append(definitions[key])
            else:
                errors.append(f"line {number}: undefined link reference [{key}]")
        for destination in destinations:
            destination = re.sub(r"\\([()])", r"\1", destination.strip("<>"))
            try:
                parts = urlsplit(destination)
            except ValueError:
                errors.append(f"line {number}: invalid link destination: {destination}")
                continue
            if parts.scheme or parts.netloc:
                continue
            path = unquote(parts.path)
            target = (root / path.lstrip("/") if path.startswith("/") else source.parent / path) if path else source
            target = target.resolve()
            if not target.is_relative_to(root.resolve()):
                errors.append(f"line {number}: link leaves the checkout: {destination}")
            elif not target.exists():
                errors.append(f"line {number}: missing link target: {destination}")
            elif parts.fragment and target.suffix.lower() == ".md" and target.is_file():
                anchors = heading_anchors(split_markdown(target.read_text(encoding="utf-8")).prose)
                if unquote(parts.fragment) not in anchors:
                    errors.append(f"line {number}: missing heading anchor: {destination}")
    return errors


def documented_commands(blocks: list[tuple[int, str, list[str]]]) -> list[tuple[int, list[str]]]:
    """Read direct uerl commands; join PowerShell and POSIX continuation lines."""
    commands: list[tuple[int, list[str]]] = []
    for start, language, lines in blocks:
        if language not in {"powershell", "bash", "sh", "shell", "console"}:
            continue
        pending = ""
        command_line = start + 1
        for offset, line in enumerate(lines, start + 1):
            text = line.strip()
            if not pending:
                command_line = offset
            continuing = text.endswith(("`", "\\"))
            pending += text[:-1] + " " if continuing else text
            if continuing:
                continue
            # Do not run shell fragments, substitutions, or commands from documentation.
            match = re.match(r"^(?:uv run )?uerl(?:\s+(.+))?$", pending)
            if match:
                tokens = shlex.split(match.group(1) or "", comments=True)
                commands.append((command_line, tokens))
            pending = ""
        if pending and re.match(r"^(?:uv run )?uerl\s+", pending):
            raise ValueError(f"line {command_line}: unfinished command continuation")
    return commands


def _placeholder_values(parser: argparse.ArgumentParser, tokens: list[str]) -> list[str]:
    """Use representative values only for shell variables and explicit placeholders."""
    options: dict[str, argparse.Action] = {}

    def collect(current: argparse.ArgumentParser) -> None:
        for action in current._actions:
            for option in action.option_strings:
                options[option] = action
            if isinstance(action, argparse._SubParsersAction):
                for child in action.choices.values():
                    collect(child)

    collect(parser)
    result: list[str] = []
    for index, token in enumerate(tokens):
        if token.startswith("$") or (token.startswith("<") and token.endswith(">")):
            action = options.get(tokens[index - 1]) if index else None
            if action is not None and action.choices:
                token = str(next(iter(action.choices)))
            elif action is not None and action.type in {int, float}:
                token = "0"
        result.append(token)
    return result


def check_command(tokens: list[str]) -> str | None:
    """Check the real argument parser without calling a command's main function."""
    from uerl.cli.main import _COMMANDS

    if tokens in ([], ["-h"], ["--help"]):
        return None
    if tokens[0] not in _COMMANDS:
        return "unknown uerl command: " + " ".join(tokens)
    command, *arguments = tokens
    module = importlib.import_module(f"uerl.cli.{command}")
    factory = cast(Callable[[], argparse.ArgumentParser], module._parser)
    parser = factory()
    arguments = _placeholder_values(parser, arguments)
    diagnostic = io.StringIO()
    try:
        with contextlib.redirect_stderr(diagnostic), contextlib.redirect_stdout(diagnostic):
            if command in {"train", "play", "export", "config"} or (command == "check" and arguments[:1] == ["task"]):
                from uerl.cli.boundary import parse_overrides

                _, remaining = parser.parse_known_args(arguments)
                if remaining and any(token.startswith("--") and "." not in token for token in remaining):
                    return "unknown option: " + " ".join(remaining)
                parse_overrides(parser, remaining)
            else:
                parser.parse_args(arguments)
    except SystemExit as exc:
        if exc.code != 0:
            return diagnostic.getvalue().strip().splitlines()[-1]
    return None


def check_document(root: Path, path: Path) -> tuple[list[str], int]:
    document = split_markdown(path.read_text(encoding="utf-8"))
    errors = document.errors + check_links(root, path, document.prose)
    try:
        commands = documented_commands(document.blocks)
    except ValueError as exc:
        return [*errors, f"invalid shell quoting: {exc}"], 0
    for line, tokens in commands:
        if error := check_command(tokens):
            errors.append(f"line {line}: {error}")
    return errors, len(commands)


def project_documents(root: Path) -> list[Path]:
    output = subprocess.check_output(["git", "ls-files", "-z", "--", "*.md"], cwd=root)
    return [root / name for name in output.decode().split("\0") if name and not name.startswith(".agents/")]


def main() -> int:
    sys.path.insert(0, str(REPO_ROOT / "src"))
    errors: list[str] = []
    commands = 0
    paths = project_documents(REPO_ROOT)
    for path in paths:
        findings, count = check_document(REPO_ROOT, path)
        errors.extend(f"{path.relative_to(REPO_ROOT)}:{finding}" for finding in findings)
        commands += count
    for error in errors:
        print(error)
    print(f"Checked {len(paths)} project Markdown files and {commands} uerl examples; {len(errors)} findings.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
