#!/usr/bin/env python3
"""Bounded HTTP/1.1 endurance client for an already-running nginx endpoint."""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import http.client
import json
import os
import random
import shlex
import socket
import ssl
import struct
import subprocess
import sys
import time
import urllib.parse
from typing import Dict, List, Optional, Sequence, Set, Tuple


DEFAULT_SEED = 0x5EEDC0DE12345678
USER_AGENT = "wepoll-ex-nginx-endurance/1"

PROFILES = {
    "default": {
        "batches": 3,
        "normal": 24,
        "keepalive_connections": 2,
        "keepalive_requests": 8,
        "slow": 3,
        "resets": 6,
        "half_close": 3,
        "backpressure": 0,
        "workers": 8,
        "pause_ms": 50,
    },
    "long": {
        "batches": 30,
        "normal": 100,
        "keepalive_connections": 8,
        "keepalive_requests": 25,
        "slow": 12,
        "resets": 24,
        "half_close": 12,
        "backpressure": 0,
        "workers": 16,
        "pause_ms": 100,
    },
    "production": {
        "batches": 120,
        "normal": 250,
        "keepalive_connections": 16,
        "keepalive_requests": 50,
        "slow": 24,
        "resets": 64,
        "half_close": 24,
        "backpressure": 0,
        "workers": 32,
        "pause_ms": 250,
    },
}


@dataclasses.dataclass(frozen=True)
class Target:
    scheme: str
    host: str
    port: int
    request_target: str
    host_header: str


@dataclasses.dataclass(frozen=True)
class Config:
    target: Target
    seed: int
    batches: int
    normal: int
    keepalive_connections: int
    keepalive_requests: int
    slow: int
    resets: int
    half_close: int
    backpressure: int
    workers: int
    timeout: float
    slow_delay_ms: float
    backpressure_pause_ms: float
    backpressure_receive_buffer: int
    backpressure_min_body_bytes: int
    pause_ms: int
    max_body_bytes: int
    accepted_statuses: Set[int]
    reload_command: Optional[Sequence[str]]
    reload_every: int
    reload_timeout: float
    reload_grace: float
    reload_settle: float
    ssl_context: Optional[ssl.SSLContext]


@dataclasses.dataclass(frozen=True)
class Task:
    kind: str
    index: int
    seed: int


@dataclasses.dataclass(frozen=True)
class Outcome:
    kind: str
    operations: int


class EnduranceError(RuntimeError):
    pass


def positive_int(text: str) -> int:
    value = int(text, 0)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative_int(text: str) -> int:
    value = int(text, 0)
    if value < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return value


def positive_float(text: str) -> float:
    value = float(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative_float(text: str) -> float:
    value = float(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return value


def parse_statuses(specification: str) -> Set[int]:
    statuses: Set[int] = set()
    for item in specification.split(","):
        item = item.strip()
        if not item:
            raise argparse.ArgumentTypeError("empty status item")
        if "-" in item:
            start_text, end_text = item.split("-", 1)
            start = int(start_text)
            end = int(end_text)
            if start > end:
                raise argparse.ArgumentTypeError("reversed status range")
            statuses.update(range(start, end + 1))
        else:
            statuses.add(int(item))
    if not statuses or min(statuses) < 100 or max(statuses) > 599:
        raise argparse.ArgumentTypeError("statuses must be between 100 and 599")
    return statuses


def parse_target(url: str) -> Target:
    parsed = urllib.parse.urlsplit(url)
    scheme = parsed.scheme.lower()
    if scheme not in ("http", "https"):
        raise argparse.ArgumentTypeError("only http:// and https:// endpoints are supported")
    if parsed.username is not None or parsed.password is not None:
        raise argparse.ArgumentTypeError("URL user information is unsupported")
    if parsed.fragment:
        raise argparse.ArgumentTypeError("URL fragments are unsupported")
    if parsed.hostname is None:
        raise argparse.ArgumentTypeError("URL must include a host")
    try:
        default_port = 443 if scheme == "https" else 80
        port = parsed.port or default_port
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    request_target = parsed.path or "/"
    if parsed.query:
        request_target += "?" + parsed.query
    display_host = parsed.hostname
    if ":" in display_host and not display_host.startswith("["):
        display_host = "[" + display_host + "]"
    host_header = (
        display_host if port == default_port else f"{display_host}:{port}"
    )
    return Target(scheme, parsed.hostname, port, request_target, host_header)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Exercise normal, persistent, slow, reset, and half-close HTTP "
            "client behavior plus opt-in response backpressure against an "
            "already-running nginx endpoint."
        )
    )
    parser.add_argument("url", type=parse_target, help="HTTP or HTTPS endpoint URL")
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="disable HTTPS certificate and hostname verification",
    )
    profile = parser.add_mutually_exclusive_group()
    profile.add_argument("--long", action="store_true",
                         help="bounded extended qualification profile")
    profile.add_argument("--production", action="store_true",
                         help="bounded production soak profile")
    parser.add_argument("--seed", type=lambda text: int(text, 0),
                        default=DEFAULT_SEED)
    parser.add_argument("--batches", type=positive_int)
    parser.add_argument("--normal-per-batch", type=nonnegative_int)
    parser.add_argument("--keepalive-connections", type=nonnegative_int)
    parser.add_argument("--keepalive-requests", type=positive_int)
    parser.add_argument("--slow-per-batch", type=nonnegative_int)
    parser.add_argument("--resets-per-batch", type=nonnegative_int)
    parser.add_argument("--half-close-per-batch", type=nonnegative_int)
    parser.add_argument("--backpressure-per-batch", type=nonnegative_int)
    parser.add_argument("--workers", type=positive_int)
    parser.add_argument("--timeout", type=positive_float, default=5.0)
    parser.add_argument("--slow-delay-ms", type=nonnegative_float, default=5.0)
    parser.add_argument("--backpressure-pause-ms", type=nonnegative_float,
                        default=250.0)
    parser.add_argument("--backpressure-receive-buffer", type=positive_int,
                        default=4096)
    parser.add_argument("--backpressure-min-body-bytes", type=positive_int,
                        default=1)
    parser.add_argument("--pause-ms", type=nonnegative_int)
    parser.add_argument("--max-body-bytes", type=positive_int,
                        default=1024 * 1024)
    parser.add_argument("--expect-status", type=parse_statuses,
                        default=parse_statuses("200-399"))

    reload_group = parser.add_mutually_exclusive_group()
    reload_group.add_argument(
        "--reload-command",
        help="reload command parsed into arguments and run without a shell",
    )
    reload_group.add_argument(
        "--nginx-executable",
        help="nginx executable used as: EXECUTABLE [-p PREFIX] -s reload",
    )
    parser.add_argument("--nginx-prefix",
                        help="optional prefix used with --nginx-executable")
    parser.add_argument("--reload-every", type=positive_int, default=1)
    parser.add_argument("--reload-timeout", type=positive_float, default=15.0)
    parser.add_argument("--reload-grace", type=positive_float, default=10.0)
    parser.add_argument(
        "--reload-settle",
        type=nonnegative_float,
        default=0.0,
        help=(
            "seconds to wait after signaling reload before the health check; "
            "stock Win32 nginx may need at least 0.5 seconds"
        ),
    )
    return parser


def command_from_arguments(arguments: argparse.Namespace) -> Optional[List[str]]:
    if arguments.nginx_prefix and not arguments.nginx_executable:
        raise EnduranceError("--nginx-prefix requires --nginx-executable")
    if arguments.reload_command:
        command = shlex.split(arguments.reload_command, posix=os.name != "nt")
        if not command:
            raise EnduranceError("--reload-command must not be empty")
        return command
    if arguments.nginx_executable:
        command = [arguments.nginx_executable]
        if arguments.nginx_prefix:
            command.extend(["-p", arguments.nginx_prefix])
        command.extend(["-s", "reload"])
        return command
    return None


def config_from_arguments(arguments: argparse.Namespace) -> Config:
    profile_name = (
        "production" if arguments.production
        else "long" if arguments.long
        else "default"
    )
    profile = PROFILES[profile_name]

    if arguments.insecure and arguments.url.scheme != "https":
        raise EnduranceError("--insecure requires an https:// endpoint")
    ssl_context: Optional[ssl.SSLContext] = None
    if arguments.url.scheme == "https":
        ssl_context = ssl.create_default_context()
        if arguments.insecure:
            ssl_context.check_hostname = False
            ssl_context.verify_mode = ssl.CERT_NONE

    def selected(name: str, argument_name: str) -> int:
        value = getattr(arguments, argument_name)
        return profile[name] if value is None else value

    return Config(
        target=arguments.url,
        seed=arguments.seed,
        batches=selected("batches", "batches"),
        normal=selected("normal", "normal_per_batch"),
        keepalive_connections=selected(
            "keepalive_connections", "keepalive_connections"
        ),
        keepalive_requests=selected(
            "keepalive_requests", "keepalive_requests"
        ),
        slow=selected("slow", "slow_per_batch"),
        resets=selected("resets", "resets_per_batch"),
        half_close=selected("half_close", "half_close_per_batch"),
        backpressure=selected("backpressure", "backpressure_per_batch"),
        workers=selected("workers", "workers"),
        timeout=arguments.timeout,
        slow_delay_ms=arguments.slow_delay_ms,
        backpressure_pause_ms=arguments.backpressure_pause_ms,
        backpressure_receive_buffer=arguments.backpressure_receive_buffer,
        backpressure_min_body_bytes=arguments.backpressure_min_body_bytes,
        pause_ms=selected("pause_ms", "pause_ms"),
        max_body_bytes=arguments.max_body_bytes,
        accepted_statuses=arguments.expect_status,
        reload_command=command_from_arguments(arguments),
        reload_every=arguments.reload_every,
        reload_timeout=arguments.reload_timeout,
        reload_grace=arguments.reload_grace,
        reload_settle=arguments.reload_settle,
        ssl_context=ssl_context,
    )


def request_headers(config: Config, request_id: str,
                    connection: str) -> Dict[str, str]:
    return {
        "Host": config.target.host_header,
        "User-Agent": USER_AGENT,
        "Connection": connection,
        "X-Wepoll-Endurance": request_id,
    }


def make_http_connection(config: Config) -> http.client.HTTPConnection:
    if config.target.scheme == "https":
        assert config.ssl_context is not None
        return http.client.HTTPSConnection(
            config.target.host,
            config.target.port,
            timeout=config.timeout,
            context=config.ssl_context,
        )
    return http.client.HTTPConnection(
        config.target.host, config.target.port, timeout=config.timeout
    )


def open_raw_connection(config: Config,
                        receive_buffer: Optional[int] = None) -> socket.socket:
    client = socket.create_connection(
        (config.target.host, config.target.port), timeout=config.timeout
    )
    client.settimeout(config.timeout)
    if receive_buffer is not None:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
    if config.target.scheme == "https":
        assert config.ssl_context is not None
        try:
            client = config.ssl_context.wrap_socket(
                client, server_hostname=config.target.host
            )
        except Exception:
            client.close()
            raise
        client.settimeout(config.timeout)
    return client


def validate_response(config: Config, response: http.client.HTTPResponse,
                      request_id: str) -> int:
    if response.status not in config.accepted_statuses:
        raise EnduranceError(
            f"{request_id}: unexpected HTTP status {response.status}"
        )
    length = response.getheader("Content-Length")
    if length is not None:
        try:
            declared_length = int(length)
        except ValueError as error:
            raise EnduranceError(
                f"{request_id}: invalid Content-Length {length!r}"
            ) from error
        if declared_length > config.max_body_bytes:
            raise EnduranceError(
                f"{request_id}: body length {declared_length} exceeds "
                f"{config.max_body_bytes}"
            )
    body = response.read(config.max_body_bytes + 1)
    if len(body) > config.max_body_bytes:
        raise EnduranceError(
            f"{request_id}: body exceeds {config.max_body_bytes} bytes"
        )
    return len(body)


def normal_request(config: Config, request_id: str) -> Outcome:
    connection = make_http_connection(config)
    try:
        connection.request(
            "GET",
            config.target.request_target,
            headers=request_headers(config, request_id, "close"),
        )
        response = connection.getresponse()
        validate_response(config, response, request_id)
        response.close()
    finally:
        connection.close()
    return Outcome("normal", 1)


def keepalive_requests(config: Config, request_id: str) -> Outcome:
    connection = make_http_connection(config)
    original_fileno: Optional[int] = None
    try:
        for index in range(config.keepalive_requests):
            item_id = f"{request_id}-{index}"
            connection.request(
                "GET",
                config.target.request_target,
                headers=request_headers(config, item_id, "keep-alive"),
            )
            if connection.sock is None:
                raise EnduranceError(f"{item_id}: connection was not opened")
            current_fileno = connection.sock.fileno()
            if original_fileno is None:
                original_fileno = current_fileno
            elif current_fileno != original_fileno:
                raise EnduranceError(f"{item_id}: keep-alive connection changed")
            response = connection.getresponse()
            validate_response(config, response, item_id)
            if index + 1 < config.keepalive_requests and response.will_close:
                raise EnduranceError(f"{item_id}: server closed keep-alive early")
            response.close()
    finally:
        connection.close()
    return Outcome("keepalive", config.keepalive_requests)


def raw_request(config: Config, request_id: str,
                connection_value: str = "close") -> bytes:
    headers = request_headers(config, request_id, connection_value)
    lines = [
        f"GET {config.target.request_target} HTTP/1.1",
        *(f"{name}: {value}" for name, value in headers.items()),
        "",
        "",
    ]
    return "\r\n".join(lines).encode("ascii")


def read_raw_response(config: Config, client: socket.socket,
                      request_id: str) -> int:
    response = http.client.HTTPResponse(client)
    try:
        response.begin()
        return validate_response(config, response, request_id)
    finally:
        response.close()


def slow_request(config: Config, request_id: str, seed: int) -> Outcome:
    rng = random.Random(seed)
    payload = raw_request(config, request_id)
    client = open_raw_connection(config)
    try:
        offset = 0
        while offset < len(payload):
            remaining = len(payload) - offset
            chunk_length = min(remaining, rng.randint(1, 12))
            client.sendall(payload[offset:offset + chunk_length])
            offset += chunk_length
            if offset < len(payload) and config.slow_delay_ms:
                delay_scale = 0.5 + rng.random()
                time.sleep(config.slow_delay_ms * delay_scale / 1000.0)
        read_raw_response(config, client, request_id)
    finally:
        client.close()
    return Outcome("slow", 1)


def set_abortive_close(client: socket.socket) -> None:
    formats = ("ii", "hh") if os.name != "nt" else ("hh", "ii")
    last_error: Optional[OSError] = None
    for linger_format in formats:
        try:
            linger = struct.pack(linger_format, 1, 0)
            client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
            return
        except OSError as error:
            last_error = error
    if last_error is not None:
        raise last_error


def reset_request(config: Config, request_id: str, seed: int) -> Outcome:
    rng = random.Random(seed)
    payload = raw_request(config, request_id)
    maximum = max(1, len(payload) - 4)
    prefix_length = rng.randint(1, maximum)
    client = open_raw_connection(config)
    try:
        client.sendall(payload[:prefix_length])
        set_abortive_close(client)
    finally:
        client.close()
    return Outcome("reset", 1)


def half_close_request(config: Config, request_id: str) -> Outcome:
    payload = raw_request(config, request_id)
    client = open_raw_connection(config)
    try:
        client.sendall(payload)
        if config.target.scheme == "https":
            socket.socket.shutdown(client, socket.SHUT_WR)
        else:
            client.shutdown(socket.SHUT_WR)
        read_raw_response(config, client, request_id)
    finally:
        client.close()
    return Outcome("half_close", 1)


def backpressure_request(config: Config, request_id: str) -> Outcome:
    payload = raw_request(config, request_id)
    client = open_raw_connection(
        config, receive_buffer=config.backpressure_receive_buffer
    )
    try:
        client.sendall(payload)
        if config.backpressure_pause_ms:
            time.sleep(config.backpressure_pause_ms / 1000.0)
        body_bytes = read_raw_response(config, client, request_id)
        if body_bytes < config.backpressure_min_body_bytes:
            raise EnduranceError(
                f"{request_id}: response body {body_bytes} bytes is below "
                f"the backpressure minimum "
                f"{config.backpressure_min_body_bytes}"
            )
    finally:
        client.close()
    return Outcome("backpressure", 1)


def execute_task(config: Config, batch: int, task: Task) -> Outcome:
    request_id = f"b{batch}-i{task.index}-s{task.seed:016x}"
    if task.kind == "normal":
        return normal_request(config, request_id)
    if task.kind == "keepalive":
        return keepalive_requests(config, request_id)
    if task.kind == "slow":
        return slow_request(config, request_id, task.seed)
    if task.kind == "reset":
        return reset_request(config, request_id, task.seed)
    if task.kind == "half_close":
        return half_close_request(config, request_id)
    if task.kind == "backpressure":
        return backpressure_request(config, request_id)
    raise AssertionError(f"unknown task kind {task.kind}")


def make_tasks(config: Config, rng: random.Random) -> List[Task]:
    counts = (
        ("normal", config.normal),
        ("keepalive", config.keepalive_connections),
        ("slow", config.slow),
        ("reset", config.resets),
        ("half_close", config.half_close),
        ("backpressure", config.backpressure),
    )
    tasks: List[Task] = []
    index = 0
    for kind, count in counts:
        for _ in range(count):
            tasks.append(Task(kind, index, rng.getrandbits(64)))
            index += 1
    rng.shuffle(tasks)
    return tasks


def run_batch(config: Config, batch: int,
              rng: random.Random) -> Tuple[Dict[str, int], List[str]]:
    tasks = make_tasks(config, rng)
    counts: Dict[str, int] = {
        "normal": 0,
        "keepalive": 0,
        "slow": 0,
        "reset": 0,
        "half_close": 0,
        "backpressure": 0,
    }
    failures: List[str] = []
    with concurrent.futures.ThreadPoolExecutor(
        max_workers=config.workers,
        thread_name_prefix="nginx-endurance",
    ) as executor:
        future_tasks = {
            executor.submit(execute_task, config, batch, task): task
            for task in tasks
        }
        for future in concurrent.futures.as_completed(future_tasks):
            task = future_tasks[future]
            try:
                outcome = future.result()
                counts[outcome.kind] += outcome.operations
            except Exception as error:
                failures.append(
                    f"batch={batch} kind={task.kind} index={task.index} "
                    f"seed=0x{task.seed:016x} "
                    f"{type(error).__name__}: {error}"
                )
    return counts, failures


def invoke_reload(config: Config) -> None:
    assert config.reload_command is not None
    try:
        completed = subprocess.run(
            list(config.reload_command),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=config.reload_timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise EnduranceError(f"reload invocation failed: {error}") from error
    if completed.returncode != 0:
        stdout = completed.stdout.strip()
        stderr = completed.stderr.strip()
        raise EnduranceError(
            f"reload exited {completed.returncode}; "
            f"stdout={stdout!r}; stderr={stderr!r}"
        )
    if config.reload_settle:
        time.sleep(config.reload_settle)


def wait_for_health(config: Config, reload_index: int) -> int:
    deadline = time.monotonic() + config.reload_grace
    attempts = 0
    last_error: Optional[Exception] = None
    while time.monotonic() < deadline:
        attempts += 1
        try:
            normal_request(config, f"reload-{reload_index}-health-{attempts}")
            return attempts
        except Exception as error:
            last_error = error
            time.sleep(0.1)
    raise EnduranceError(
        f"endpoint did not recover within {config.reload_grace:.1f}s: "
        f"{last_error}"
    )


def add_counts(destination: Dict[str, int], source: Dict[str, int]) -> None:
    for name, value in source.items():
        destination[name] = destination.get(name, 0) + value


def run(config: Config) -> int:
    rng = random.Random(config.seed)
    totals: Dict[str, int] = {
        "normal": 0,
        "keepalive": 0,
        "slow": 0,
        "reset": 0,
        "half_close": 0,
        "backpressure": 0,
    }
    all_failures: List[str] = []
    reloads = 0
    start = time.monotonic()

    try:
        normal_request(config, "startup-health")
    except Exception as error:
        print(f"startup health check failed: {error}", file=sys.stderr)
        return 1

    for batch in range(1, config.batches + 1):
        batch_start = time.monotonic()
        counts, failures = run_batch(config, batch, rng)
        add_counts(totals, counts)
        all_failures.extend(failures)
        batch_summary = {
            "batch": batch,
            "batches": config.batches,
            "duration_seconds": round(time.monotonic() - batch_start, 3),
            "counts": counts,
            "failures": len(failures),
        }
        print("BATCH " + json.dumps(batch_summary, sort_keys=True), flush=True)
        for failure in failures:
            print("FAIL " + failure, file=sys.stderr, flush=True)

        reload_due = (
            config.reload_command is not None
            and batch < config.batches
            and batch % config.reload_every == 0
        )
        if reload_due:
            try:
                invoke_reload(config)
                reloads += 1
                attempts = wait_for_health(config, reloads)
                print(
                    f"RELOAD index={reloads} health_attempts={attempts}",
                    flush=True,
                )
            except Exception as error:
                message = f"batch={batch} reload failure: {error}"
                all_failures.append(message)
                print("FAIL " + message, file=sys.stderr, flush=True)

        if batch < config.batches and config.pause_ms:
            time.sleep(config.pause_ms / 1000.0)

    summary = {
        "result": "pass" if not all_failures else "fail",
        "seed": f"0x{config.seed:016x}",
        "url": (
            f"{config.target.scheme}://{config.target.host_header}"
            f"{config.target.request_target}"
        ),
        "batches": config.batches,
        "counts": totals,
        "reloads": reloads,
        "failures": len(all_failures),
        "duration_seconds": round(time.monotonic() - start, 3),
    }
    print("SUMMARY " + json.dumps(summary, sort_keys=True), flush=True)
    return 0 if not all_failures else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = make_parser()
    arguments = parser.parse_args(argv)
    try:
        config = config_from_arguments(arguments)
    except (EnduranceError, ValueError) as error:
        parser.error(str(error))
    try:
        return run(config)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
