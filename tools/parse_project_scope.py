"""Parse the project's scope documents and extract project scope information.

Discover the documents that state what this project is -- the four paths in ``SCOPE_DOCS``
(the root ``README.md`` and the handoff capture under ``handoff/`` and ``handofffail.md``),
plus ``CONTRIBUTING.md`` and every ``*.md`` under ``Documentation/`` -- pull out the
scope-bearing sections of each, and write a summary in either JSON or plain markdown.

The handoff documents are the ones that actually describe this fork: ``handoff/project.md``
"1. What this project is" and ``handoff/readme.md`` "The goal this serves" carry the binding
scope, while the root ``README.md`` is upstream's description of KryonOS itself.

Usage::

    python tools/parse_project_scope.py [--files PATH ...] [--output json|md] [--out-file <path>]

``--files`` restricts the run to the named repository-relative paths, e.g.::

    python tools/parse_project_scope.py --files README.md handoff/project.md \\
        handoff/readme.md handofffail.md

If ``--output`` is ``json`` (default) the result is written to ``project-scope.json`` in the
repository root. ``md`` produces ``PROJECT_SCOPE.md`` -- a concatenated markdown summary.
"""

from __future__ import annotations

import argparse
import html
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# What to parse, and which headings carry scope
# ---------------------------------------------------------------------------

#: The documents that describe this fork, in the order they should be reported. These are the
#: files that state the project's scope; ``README.md`` is here because the request named it.
SCOPE_DOCS: List[str] = [
    "README.md",
    "handoff/project.md",
    "handoff/readme.md",
    "handofffail.md",
]

#: Canonical section key -> the normalized headings that produce it. Headings are compared after
#: ``_normalize_heading``, so enumerators ("5.1"), emphasis and case do not matter. A heading that
#: matches nothing still ends the section in progress; it simply starts no new one. The document
#: title is deliberately absent from every set, so a leading H1 is skipped rather than swallowing
#: the whole file.
HEADING_ALIASES: Dict[str, set] = {
    "scope": {
        "scope",
        "overview",
        "what this project is",
        "session overview",
    },
    "goals": {
        "goals",
        "the goal this serves",
    },
    "architecture": {"architecture"},
    "design": {"design"},
    "constraints": {
        "standing rules — verbatim from the user, still binding",
        "constraints to carry into the new session",
    },
    "open_items": {
        "open items — none of them blocking",
        "current pending actions",
    },
}

_ALIAS_TO_KEY: Dict[str, str] = {
    alias: key for key, aliases in HEADING_ALIASES.items() for alias in aliases
}

# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------

_IGNORED_DIRS = {".git", ".pio", "build", "node_modules", "__pycache__"}


def _is_ignored(p: Path) -> bool:
    """Return True if the path is inside a known generated or third-party folder."""
    return any(part in _IGNORED_DIRS for part in p.parts)


def collect_md_files(repo_root: Path) -> List[Path]:
    """Collect the default set of markdown files to parse.

    The four ``SCOPE_DOCS``, then ``CONTRIBUTING.md``, then every ``*.md`` under
    ``Documentation/``. Files inside an ignored directory are skipped, and the result is
    de-duplicated while preserving discovery order.
    """
    candidates: List[Path] = []
    for rel in SCOPE_DOCS + ["CONTRIBUTING.md"]:
        p = repo_root / rel
        if p.is_file() and not _is_ignored(p):
            candidates.append(p)

    docs_dir = repo_root / "Documentation"
    if docs_dir.is_dir():
        candidates.extend(sorted(p for p in docs_dir.rglob("*.md") if not _is_ignored(p)))

    seen: set = set()
    unique: List[Path] = []
    for p in candidates:
        key = p.resolve()
        if key not in seen:
            seen.add(key)
            unique.append(p)
    return unique


def collect_named_files(repo_root: Path, names: Sequence[str]) -> Tuple[List[Path], List[str]]:
    """Resolve repository-relative ``names`` to existing files.

    Returns ``(found, missing)``. Named paths are honoured whether or not they are in
    ``SCOPE_DOCS``, so an arbitrary document can be parsed without editing the module.
    """
    found: List[Path] = []
    missing: List[str] = []
    for name in names:
        p = repo_root / name
        if p.is_file() and not _is_ignored(p):
            found.append(p)
        else:
            missing.append(name)
    return found, missing


def parse_markdown(text: str) -> List[Dict]:
    """Parse markdown into an AST using ``mistune``.

    ``mistune`` is lightweight and yields a list of node dicts. mistune 3 exposes the AST
    renderer by name (``'ast'``); mistune 2 exposed it as ``AstRenderer``, so fall back to that.
    """
    import mistune

    try:
        parser = mistune.create_markdown(renderer="ast")
    except (AttributeError, ValueError):
        parser = mistune.create_markdown(renderer=mistune.AstRenderer())
    return parser(text)


def _inline_text(node: Dict) -> str:
    """Return the plain text of an inline node, recursing through its children.

    A node either carries text itself (``text``, ``codespan``) or wraps children (``emphasis``,
    ``strong``, ``link``, ``paragraph``, ``list_item``). Reading only direct children would
    silently drop every bolded, emphasised or linked phrase -- and these documents are
    bold-dense.

    mistune 3 stores leaf text under ``raw``; mistune 2 used ``text``. Both are accepted.
    """
    if not isinstance(node, dict):
        return ""
    ntype = node.get("type")
    if ntype in ("linebreak", "softbreak"):
        return " "
    if ntype == "blank_line":
        return ""
    children = node.get("children")
    if children:
        return "".join(_inline_text(child) for child in children)
    return node.get("raw") or node.get("text") or ""


_ENUMERATOR_RE = re.compile(r"^\d+(?:\.\d+)*\.?\s*")


def _normalize_heading(text: str) -> str:
    """Normalize a heading for alias lookup.

    Lowercases, drops a leading enumerator (``5.1 ``), removes emphasis markers and inline-code
    backticks, collapses whitespace, and trims surrounding punctuation.
    """
    s = text.strip().lower()
    s = _ENUMERATOR_RE.sub("", s)
    s = s.replace("*", "").replace("`", "").replace("_", "")
    s = re.sub(r"\s+", " ", s)
    return s.strip(" \t:.,;!?-–—")


def _para_pseudo_heading(node: Dict) -> Optional[str]:
    """Return the heading text if a paragraph is a bold-lead pseudo-heading, else ``None``.

    ``handofffail.md`` has no ATX headings below its title; its sections are ``**Session
    Overview**`` and ``**Current Pending Actions**``, which mistune renders as a paragraph whose
    only child is a ``strong``. Treating that as a heading is what lets the file contribute real
    sections instead of one fallback blob.
    """
    children = node.get("children") or []
    if not children or children[0].get("type") != "strong":
        return None
    if "".join(_inline_text(c) for c in children[1:]).strip():
        return None
    text = _inline_text(children[0]).strip()
    return text or None


def extract_sections(ast: List[Dict]) -> Dict[str, str]:
    """Walk the AST and pull out content for the alias-matched headings.

    For each matched heading, collect the text of the following paragraphs, lists and
    block quotes until the next heading -- matched or not -- of any level. Bold-lead
    pseudo-headings count as headings too.
    """
    result: Dict[str, str] = {}
    current_key: Optional[str] = None
    buffer: List[str] = []

    def flush() -> None:
        nonlocal buffer
        if current_key and buffer:
            text = "\n".join(line.strip() for line in buffer if line.strip())
            if text.strip():
                result[current_key] = text.strip()
        buffer.clear()

    for node in ast:
        ntype = node.get("type")

        heading: Optional[str] = None
        if ntype == "heading":
            heading = _inline_text(node)
        elif ntype == "paragraph":
            heading = _para_pseudo_heading(node)

        if heading is not None:
            flush()
            current_key = _ALIAS_TO_KEY.get(_normalize_heading(heading))
            continue

        if current_key is None:
            continue

        if ntype == "paragraph":
            buffer.append(_inline_text(node))
        elif ntype == "list":
            for item in node.get("children", []):
                if item.get("type") == "list_item":
                    buffer.append(f"- {_inline_text(item)}")
        elif ntype == "block_quote":
            buffer.append(_inline_text(node))

    flush()
    return result


# ---------------------------------------------------------------------------
# HTML fallback -- README.md and CONTRIBUTING.md use <h1>/<h2>, not '#'
# ---------------------------------------------------------------------------

_HTML_HEADING_RE = re.compile(r"<h([1-6])[^>]*>(.*?)</h\1\s*>", re.IGNORECASE | re.DOTALL)
_TAG_RE = re.compile(r"<[^>]+>")
_PARA_RE = re.compile(r"<p[^>]*>(.*?)</p>", re.IGNORECASE | re.DOTALL)
_SCRIPT_RE = re.compile(r"<(script|style)\b.*?</\1\s*>", re.IGNORECASE | re.DOTALL)


def _strip_html(s: str) -> str:
    """Drop tags, unescape entities, and collapse runs of spaces."""
    s = _SCRIPT_RE.sub(" ", s)
    s = _TAG_RE.sub(" ", s)
    s = html.unescape(s)
    s = s.replace("\xa0", " ")
    return re.sub(r"[ \t]+", " ", s).strip()


def extract_html_sections(raw: str) -> Dict[str, str]:
    """Extract alias-matched sections from documents built out of raw HTML headings.

    ``README.md`` and ``CONTRIBUTING.md`` carry no ATX headings at all -- mistune yields their
    markup as ``block_html`` -- so without this pass they contribute nothing but a fallback.
    """
    heads = list(_HTML_HEADING_RE.finditer(raw))
    if not heads:
        return {}

    result: Dict[str, str] = {}
    for i, m in enumerate(heads):
        key = _ALIAS_TO_KEY.get(_normalize_heading(_strip_html(m.group(2))))
        if not key:
            continue
        start = m.end()
        end = heads[i + 1].start() if i + 1 < len(heads) else len(raw)
        body = _strip_html(raw[start:end])
        body = "\n".join(line.strip() for line in body.splitlines() if line.strip())
        if body and key not in result:
            result[key] = body
    return result


def fallback_prose(raw: str, min_len: int = 80) -> str:
    """Return the first substantial ``<p>`` block of prose, with its tags stripped.

    Used for HTML-heavy documents whose headings match no alias. Picking the first *long*
    paragraph skips the short badge and tagline blocks at the top of ``README.md``.
    """
    for m in _PARA_RE.finditer(raw):
        text = _strip_html(m.group(1))
        if len(text) >= min_len:
            return text
    return ""


def fallback_paragraph(text: str) -> str:
    """Return the first non-heading paragraph from the raw markdown.

    This is used when a file has no target headings and no HTML prose to fall back on.
    """
    paragraphs = re.split(r"\n\s*\n", text.strip())
    for para in paragraphs:
        stripped = para.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            continue
        return stripped
    return ""


def process_file(path: Path) -> Dict[str, str]:
    """Read a markdown file, parse it, and return a mapping of section -> content.

    ATX and bold-lead headings are tried first, then HTML headings. If neither yields a matched
    section, the file's first substantial prose paragraph is stored under ``overview``.
    """
    raw = path.read_text(encoding="utf-8")
    sections = extract_sections(parse_markdown(raw))

    for key, value in extract_html_sections(raw).items():
        sections.setdefault(key, value)

    if not sections:
        sections["overview"] = fallback_prose(raw) or fallback_paragraph(raw)
    return sections


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--files",
        nargs="+",
        metavar="PATH",
        help="Only parse these repository-relative markdown paths. Defaults to the full sweep: "
        "the scope documents, CONTRIBUTING.md, and Documentation/**/*.md.",
    )
    parser.add_argument(
        "--output", choices=["json", "md"], default="json", help="Output format (default: json)"
    )
    parser.add_argument(
        "--out-file",
        type=Path,
        help="Explicit output file path. If omitted, defaults to ``project-scope.json`` or "
        "``PROJECT_SCOPE.md`` in the repository root.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]

    if args.files:
        md_files, missing = collect_named_files(repo_root, args.files)
        for name in missing:
            print(f"warning: not found, skipped: {name}", file=sys.stderr)
        if not md_files:
            print("error: none of the requested files exist", file=sys.stderr)
            return 1
    else:
        md_files = collect_md_files(repo_root)

    aggregate: Dict[str, Dict[str, str]] = {}
    try:
        for fp in md_files:
            rel = fp.relative_to(repo_root).as_posix()
            aggregate[rel] = process_file(fp)
    except ImportError as exc:
        print(f"error: {exc}", file=sys.stderr)
        print("hint: python -m pip install -r requirements-dev.txt", file=sys.stderr)
        return 2

    out_path: Path
    if args.out_file:
        out_path = args.out_file
    else:
        out_path = repo_root / ("project-scope.json" if args.output == "json" else "PROJECT_SCOPE.md")

    if args.output == "json":
        out_path.write_text(json.dumps(aggregate, indent=2, ensure_ascii=False), encoding="utf-8")
    else:
        lines: List[str] = []
        for file, secs in aggregate.items():
            for sec, txt in secs.items():
                lines.append(f"## {file} — {sec.capitalize()}")
                lines.append("")
                lines.append(txt)
                lines.append("")
        out_path.write_text("\n".join(lines).strip() + "\n", encoding="utf-8")

    print(f"Wrote {out_path} ({len(aggregate)} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
