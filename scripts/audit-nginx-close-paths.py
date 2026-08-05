#!/usr/bin/env python3
"""Audit nginx module socket retirement for synchronized wepoll-ex builds.

The synchronized Windows lifetime policy requires EPOLL_CTL_DEL before
closesocket().  nginx's ngx_close_connection() supplies that ordering through
the active event module's del_conn hook.  This audit verifies the exact core
ordering and rejects raw socket retirement in bundled or supplied module
trees.  A module may annotate an intentional pre-registration temporary close
with ``wepoll-close-audit: allow`` on the same or preceding source line.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import sys
from typing import Iterable, List, Optional, Sequence


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
ALLOW_MARKER = "wepoll-close-audit: allow"
RAW_RETIREMENT_PATTERNS = (
    ("raw socket close", re.compile(r"\b(?:ngx_close_socket|closesocket)\s*\(")),
    ("raw connection free", re.compile(r"\bngx_free_connection\s*\(")),
)


@dataclasses.dataclass(frozen=True)
class Finding:
    path: pathlib.Path
    line: int
    kind: str
    text: str


class AuditError(RuntimeError):
    pass


def strip_comments_and_literals(source: str) -> str:
    """Replace comments and C literals with spaces while preserving newlines."""
    output: List[str] = []
    index = 0
    state = "code"

    while index < len(source):
        character = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "code":
            if character == "/" and following == "*":
                output.extend((" ", " "))
                index += 2
                state = "block-comment"
                continue
            if character == "/" and following == "/":
                output.extend((" ", " "))
                index += 2
                state = "line-comment"
                continue
            if character == '"':
                output.append(" ")
                index += 1
                state = "string"
                continue
            if character == "'":
                output.append(" ")
                index += 1
                state = "character"
                continue
            output.append(character)
            index += 1
            continue

        if state == "block-comment":
            if character == "*" and following == "/":
                output.extend((" ", " "))
                index += 2
                state = "code"
            else:
                output.append("\n" if character == "\n" else " ")
                index += 1
            continue

        if state == "line-comment":
            if character == "\n":
                output.append("\n")
                state = "code"
            else:
                output.append(" ")
            index += 1
            continue

        if character == "\\" and following:
            output.extend((" ", "\n" if following == "\n" else " "))
            index += 2
            continue
        if (state == "string" and character == '"') or (
            state == "character" and character == "'"
        ):
            output.append(" ")
            index += 1
            state = "code"
            continue
        output.append("\n" if character == "\n" else " ")
        index += 1

    return "".join(output)


def extract_function(source: str, name: str) -> str:
    sanitized = strip_comments_and_literals(source)
    signature = re.compile(
        rf"(?ms)^{re.escape(name)}\s*\([^;]*?\)\s*\{{"
    )
    match = signature.search(sanitized)
    if match is None:
        raise AuditError(f"required function {name} was not found")

    opening = sanitized.find("{", match.start())
    depth = 0
    for index in range(opening, len(sanitized)):
        if sanitized[index] == "{":
            depth += 1
        elif sanitized[index] == "}":
            depth -= 1
            if depth == 0:
                return sanitized[opening:index + 1]
    raise AuditError(f"required function {name} has an unterminated body")


def require_order(body: str, earlier: str, later: str, description: str) -> None:
    earlier_index = body.find(earlier)
    later_index = body.find(later)
    if earlier_index == -1 or later_index == -1 or earlier_index >= later_index:
        raise AuditError(description)


def nginx_version(nginx_root: pathlib.Path) -> str:
    header = nginx_root / "src/core/nginx.h"
    source = header.read_text(encoding="utf-8")
    match = re.search(r'^#define\s+NGINX_VERSION\s+"([^"]+)"', source,
                      re.MULTILINE)
    if match is None:
        raise AuditError(f"cannot identify nginx version from {header}")
    return match.group(1)


def audit_core_contract(nginx_root: pathlib.Path) -> None:
    connection_source = (nginx_root / "src/core/ngx_connection.c").read_text(
        encoding="utf-8"
    )
    close_connection = extract_function(connection_source,
                                        "ngx_close_connection")
    require_order(
        close_connection,
        "ngx_del_conn",
        "ngx_close_socket",
        "ngx_close_connection() does not invoke del_conn before socket close",
    )
    require_order(
        close_connection,
        "NGX_CLOSE_EVENT",
        "ngx_close_socket",
        "ngx_close_connection() lost its close-event fallback before close",
    )

    close_listeners = extract_function(connection_source,
                                       "ngx_close_listening_sockets")
    require_order(
        close_listeners,
        "NGX_CLOSE_EVENT",
        "ngx_close_socket",
        "ngx_close_listening_sockets() does not delete active events before close",
    )

    accept_source = (nginx_root / "src/event/ngx_event_accept.c").read_text(
        encoding="utf-8"
    )
    accept_connection = extract_function(accept_source, "ngx_event_accept")
    require_order(
        accept_connection,
        "ngx_add_conn",
        "ls->handler",
        "ngx_event_accept() no longer registers before handing off a connection",
    )

    connect_source = (nginx_root / "src/event/ngx_event_connect.c").read_text(
        encoding="utf-8"
    )
    connect_peer = extract_function(connect_source, "ngx_event_connect_peer")
    require_order(
        connect_peer,
        "ngx_add_conn",
        "connect(s",
        "ngx_event_connect_peer() no longer registers before connect/retirement",
    )
    if "ngx_close_connection(c)" not in connect_peer:
        raise AuditError(
            "ngx_event_connect_peer() no longer retires registered failures "
            "through ngx_close_connection()"
        )


def iter_source_files(roots: Iterable[pathlib.Path]) -> Iterable[pathlib.Path]:
    for root in roots:
        if not root.exists():
            raise AuditError(f"module source root does not exist: {root}")
        if root.is_file():
            if root.suffix.lower() in SOURCE_SUFFIXES:
                yield root
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                yield path


def line_is_allowed(lines: Sequence[str], line_number: int) -> bool:
    first = max(0, line_number - 2)
    return any(ALLOW_MARKER in lines[index]
               for index in range(first, line_number))


def audit_module_file(path: pathlib.Path) -> tuple[List[Finding], int, int]:
    source = path.read_text(encoding="utf-8", errors="replace")
    sanitized = strip_comments_and_literals(source)
    original_lines = source.splitlines()
    findings: List[Finding] = []
    close_connection_calls = len(
        re.findall(r"\bngx_close_connection\s*\(", sanitized)
    )
    allowed = 0

    for kind, pattern in RAW_RETIREMENT_PATTERNS:
        for match in pattern.finditer(sanitized):
            line_number = sanitized.count("\n", 0, match.start()) + 1
            if line_is_allowed(original_lines, line_number):
                allowed += 1
                continue
            text = original_lines[line_number - 1].strip()
            findings.append(Finding(path, line_number, kind, text))

    return findings, close_connection_calls, allowed


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Verify nginx core DEL-before-close ordering and reject raw socket "
            "retirement in bundled or supplied module source trees."
        )
    )
    parser.add_argument("nginx_root", type=pathlib.Path,
                        help="extracted nginx source root")
    parser.add_argument(
        "--module-root",
        action="append",
        type=pathlib.Path,
        default=[],
        help="additional addon/module source root to audit (repeatable)",
    )
    parser.add_argument("--expect-version", default="1.31.3")
    parser.add_argument("--json", action="store_true",
                        help="emit the successful summary as JSON")
    return parser


def run(arguments: argparse.Namespace) -> int:
    nginx_root = arguments.nginx_root.resolve()
    version = nginx_version(nginx_root)
    if version != arguments.expect_version:
        raise AuditError(
            f"expected nginx {arguments.expect_version}, found {version}"
        )

    audit_core_contract(nginx_root)
    roots = [
        nginx_root / "src/http",
        nginx_root / "src/mail",
        nginx_root / "src/stream",
        *sorted((nginx_root / "src/event").glob("ngx_event_openssl*.c")),
        *arguments.module_root,
    ]
    files = sorted(set(iter_source_files(roots)))
    findings: List[Finding] = []
    close_connection_calls = 0
    allowed = 0
    for path in files:
        file_findings, file_close_calls, file_allowed = audit_module_file(path)
        findings.extend(file_findings)
        close_connection_calls += file_close_calls
        allowed += file_allowed

    if findings:
        for finding in findings:
            print(
                f"{finding.path}:{finding.line}: {finding.kind}: "
                f"{finding.text}",
                file=sys.stderr,
            )
        raise AuditError(
            f"found {len(findings)} unreviewed raw retirement operation(s)"
        )

    summary = {
        "result": "pass",
        "nginx_version": version,
        "module_roots": [str(path.resolve()) for path in roots],
        "source_files": len(files),
        "ngx_close_connection_calls": close_connection_calls,
        "annotated_raw_retirements": allowed,
        "unreviewed_raw_retirements": 0,
    }
    if arguments.json:
        print(json.dumps(summary, sort_keys=True))
    else:
        print(
            "PASS nginx close-path audit: "
            f"version={version} files={len(files)} "
            f"close_connection_calls={close_connection_calls} "
            f"annotated_raw_retirements={allowed}"
        )
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = make_parser()
    arguments = parser.parse_args(argv)
    try:
        return run(arguments)
    except (AuditError, OSError) as error:
        print(f"FAIL nginx close-path audit: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
