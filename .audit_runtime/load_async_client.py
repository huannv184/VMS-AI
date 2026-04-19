#!/usr/bin/env python3
import argparse
import asyncio
import json
import multiprocessing
from concurrent.futures import ProcessPoolExecutor
import statistics
import time
from urllib.parse import urlsplit


async def read_response(reader: asyncio.StreamReader) -> int:
    header_block = await reader.readuntil(b"\r\n\r\n")
    if not header_block:
        raise ConnectionError("empty status line")

    header_text = header_block.decode("ascii", errors="replace")
    header_lines = header_text.split("\r\n")
    status_line = header_lines[0]
    parts = status_line.strip().split(" ", 2)
    if len(parts) < 2:
        raise ConnectionError(f"bad status line: {status_line!r}")
    status_code = int(parts[1])

    content_length = 0
    for line in header_lines[1:]:
        if not line:
            continue
        key, _, value = line.partition(":")
        if key.lower() == "content-length":
            content_length = int(value.strip())

    if content_length:
        await reader.readexactly(content_length)
    return status_code


async def worker(host: str, port: int, path: str, deadline: float, results: list[float], errors: list[str]) -> None:
    try:
        reader, writer = await asyncio.open_connection(host, port)
    except Exception as exc:
        errors.append(f"connect:{exc}")
        return

    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "User-Agent: vms-phase4-load\r\n"
        "Accept: application/json\r\n"
        "Connection: keep-alive\r\n\r\n"
    ).encode("ascii")

    try:
        while time.perf_counter() < deadline:
            start = time.perf_counter()
            writer.write(request)
            await writer.drain()
            status = await read_response(reader)
            elapsed_ms = (time.perf_counter() - start) * 1000.0
            if status != 200:
                errors.append(f"status:{status}")
            else:
                results.append(elapsed_ms)
    except Exception as exc:
        errors.append(f"io:{exc}")
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def run_shard_async(host: str, port: int, path: str, connections: int, duration: float) -> tuple[list[float], list[str]]:
    results: list[float] = []
    errors: list[str] = []
    deadline = time.perf_counter() + duration
    await asyncio.gather(*[
        worker(host, port, path, deadline, results, errors)
        for _ in range(max(0, connections))
    ])
    return results, errors


def run_shard(host: str, port: int, path: str, connections: int, duration: float) -> tuple[list[float], list[str]]:
    return asyncio.run(run_shard_async(host, port, path, connections, duration))


async def run_benchmark(host: str, port: int, path: str, connections: int, duration: float, threads: int) -> tuple[list[float], list[str]]:
    if threads <= 1:
        return await run_shard_async(host, port, path, connections, duration)

    shard_count = min(max(1, threads), max(1, connections))
    base = connections // shard_count
    remainder = connections % shard_count
    shard_connections = [base + (1 if i < remainder else 0) for i in range(shard_count)]

    loop = asyncio.get_running_loop()
    ctx = multiprocessing.get_context("spawn")
    results: list[float] = []
    errors: list[str] = []
    with ProcessPoolExecutor(max_workers=shard_count, mp_context=ctx) as pool:
        tasks = [
            loop.run_in_executor(pool, run_shard, host, port, path, shard_conn, duration)
            for shard_conn in shard_connections
            if shard_conn > 0
        ]
        for shard_results, shard_errors in await asyncio.gather(*tasks):
            results.extend(shard_results)
            errors.extend(shard_errors)
    return results, errors


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(round((pct / 100.0) * (len(ordered) - 1)))))
    return ordered[index]


async def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--connections", type=int, default=40)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--threads", type=int, default=1)
    args = parser.parse_args()

    parsed = urlsplit(args.url)
    if parsed.scheme != "http":
        raise SystemExit("only http:// URLs are supported")

    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    path = parsed.path or "/"
    if parsed.query:
        path += f"?{parsed.query}"

    started = time.perf_counter()
    results, errors = await run_benchmark(host, port, path, args.connections, args.duration, args.threads)
    elapsed = max(time.perf_counter() - started, 0.000001)

    summary = {
        "url": args.url,
        "connections": args.connections,
        "duration_s": round(elapsed, 3),
        "requests": len(results),
        "errors": len(errors),
        "rps": round(len(results) / elapsed, 2),
        "avg_ms": round(statistics.fmean(results), 3) if results else 0.0,
        "p95_ms": round(percentile(results, 95.0), 3),
        "max_ms": round(max(results), 3) if results else 0.0,
        "sample_errors": errors[:10],
    }
    print(json.dumps(summary))
    return 0 if results else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
