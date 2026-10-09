"""Unit tests for the project scope parser.

The tests run against the real documentation files in the repository. They verify that the
collector discovers the expected markdown files, that heading normalization works, and that the
handoff documents -- the ones that actually describe this fork -- yield scope content.
"""

import sys
from pathlib import Path

# Ensure the ``tools`` package (which contains ``parse_project_scope.py``) is on the import path.
# The test file lives in ``tests/``; the repository root is one level up.
repo_root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo_root / "tools"))

from parse_project_scope import (  # noqa: E402
    SCOPE_DOCS,
    collect_md_files,
    collect_named_files,
    process_file,
)

SCOPE_FILES = ["README.md", "handoff/project.md", "handoff/readme.md"]


def test_repo_root_is_the_repository() -> None:
    # Guards the path arithmetic the whole module depends on.
    assert (repo_root / "platformio.ini").is_file()
    assert (repo_root / "handoff").is_dir()


def test_collect_md_files() -> None:
    files = collect_md_files(repo_root)
    rel = {p.relative_to(repo_root).as_posix() for p in files}

    # The scope documents the request named.
    for name in SCOPE_DOCS:
        assert name in rel, f"Expected scope document {name} to be discovered"

    # The wider documentation sweep must not regress.
    expected = {
        "Documentation/Hardware_Architecture.md",
        "Documentation/Display_Touch_Architecture.md",
        "Documentation/App_Development_Guide.md",
        "Documentation/Flash_and_Persistence.md",
        "Documentation/Kryon3D_Engine_Guide.md",
        "Documentation/JS_API_Guide.md",
    }
    for e in expected:
        assert e in rel, f"Expected documentation file {e} to be discovered"


def test_collect_named_files_restricts_and_reports_missing() -> None:
    found, missing = collect_named_files(repo_root, SCOPE_FILES + ["does/not/exist.md"])
    got = {p.relative_to(repo_root).as_posix() for p in found}
    assert got == set(SCOPE_FILES)
    assert missing == ["does/not/exist.md"]


def test_collect_named_files_rejects_ignored_paths() -> None:
    found, missing = collect_named_files(repo_root, [".pio/does-not-matter.md"])
    assert found == []
    assert missing == [".pio/does-not-matter.md"]


def test_all_scope_files_yield_content() -> None:
    for name in SCOPE_FILES:
        sections = process_file(repo_root / name)
        assert sections, f"{name} produced no sections"
        assert any(v.strip() for v in sections.values()), f"{name} produced only empty sections"


def test_handoff_project_scope_names_the_port_target() -> None:
    sections = process_file(repo_root / "handoff" / "project.md")
    scope = sections.get("scope", "")
    assert scope, "handoff/project.md should yield a 'scope' section from 'What this project is'"
    assert "ESP32-S31-Korvo-1" in scope
    assert "ESP-IDF v6.1.0" in scope


def test_handoff_project_constraints_and_open_items() -> None:
    sections = process_file(repo_root / "handoff" / "project.md")
    assert "constraints" in sections, "Standing rules heading should map to 'constraints'"
    assert "open_items" in sections, "Open items heading should map to 'open_items'"


def test_handoff_readme_goal() -> None:
    sections = process_file(repo_root / "handoff" / "readme.md")
    goal = sections.get("goals", "")
    assert goal, "handoff/readme.md should yield 'goals' from 'The goal this serves'"
    assert "Korvo-1" in goal
    assert "constraints" in sections, "'Constraints to carry into the new session' should map"


def test_readme_yields_prose_not_markup() -> None:
    sections = process_file(repo_root / "README.md")
    assert "overview" in sections, "README.md should fall back to its first substantial prose"
    overview = sections["overview"]
    assert overview.strip()
    assert "<div" not in overview, "Raw markup leaked into the overview"
    assert "ESP32" in overview


def test_process_file_overview_on_documentation() -> None:
    path = repo_root / "Documentation" / "Hardware_Architecture.md"
    sections = process_file(path)
    assert isinstance(sections, dict)
    assert "overview" in sections, "Fallback overview missing"
    assert sections["overview"].strip(), "Overview content should not be empty"
