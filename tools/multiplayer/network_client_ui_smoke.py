#!/usr/bin/env python3
"""Drive the production network-client UI through a PTY and a disconnecting TCP proxy."""

from __future__ import annotations

import argparse
import fcntl
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path


MAXIMUM_TRANSPORT_FRAME_BYTES = 1024 * 1024
MAXIMUM_RELAY_BUFFER_BYTES = 2 * 1024 * 1024
PLAYER_COMMAND_MESSAGE_TYPE = 10
COMMAND_RESULT_MESSAGE_TYPE = 11


def append_bounded(buffer: bytearray, payload: bytes, label: str) -> None:
    if len(payload) > MAXIMUM_RELAY_BUFFER_BYTES - len(buffer):
        raise RuntimeError(f"{label} exceeded the bounded relay budget")
    buffer.extend(payload)


def consume_protocol_message_types(buffer: bytearray) -> list[int]:
    message_types: list[int] = []
    while len(buffer) >= 4:
        length = int.from_bytes(buffer[:4], "big")
        if length > MAXIMUM_TRANSPORT_FRAME_BYTES:
            raise RuntimeError("proxy observed an oversized transport frame")
        encoded_size = 4 + length
        if len(buffer) < encoded_size:
            break
        frame = bytes(buffer[4:encoded_size])
        del buffer[:encoded_size]
        if len(frame) < 48 or frame[:4] != b"CDMP":
            raise RuntimeError("proxy observed an invalid protocol envelope")
        message_types.append(int.from_bytes(frame[10:12], "big"))
    return message_types


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--backend-host", default="127.0.0.1")
    parser.add_argument("--backend-port", required=True, type=int)
    parser.add_argument("--token-file", required=True, type=Path)
    parser.add_argument("--user-dir", required=True, type=Path)
    parser.add_argument("--transcript", required=True, type=Path)
    parser.add_argument("--event-log", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=float, default=180.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(4)
    listener.setblocking(False)
    proxy_port = listener.getsockname()[1]

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 45, 140, 0, 0))
    environment = os.environ.copy()
    environment.setdefault("TERM", "xterm-256color")
    command = [
        str(args.client.resolve()),
        "--userdir",
        str(args.user_dir.resolve()) + os.sep,
        "--connect",
        f"127.0.0.1:{proxy_port}",
        "--connect-token-file",
        str(args.token_file.resolve()),
    ]
    process = subprocess.Popen(
        command,
        cwd=args.client.resolve().parent,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=environment,
        close_fds=True,
        start_new_session=True,
    )
    os.close(slave)

    transcript = bytearray()
    events: list[str] = []
    client_socket: socket.socket | None = None
    backend_socket: socket.socket | None = None
    to_client = bytearray()
    to_backend = bytearray()
    forwarded_client_frames = bytearray()
    observed_server_frames = bytearray()
    generation = 0
    sent_wait = False
    wait_frame_forwarded = False
    dropped = False
    sent_reconnect = False
    sent_move = False
    sent_quit = False
    second_connected_at: float | None = None
    move_sent_at: float | None = None
    started_at = time.monotonic()

    def note(message: str) -> None:
        line = f"{time.monotonic() - started_at:.3f} {message}"
        events.append(line)
        print(message, flush=True)

    def close_pair() -> None:
        nonlocal client_socket, backend_socket, to_client, to_backend
        nonlocal forwarded_client_frames, observed_server_frames
        for active_socket in (client_socket, backend_socket):
            if active_socket is not None:
                try:
                    active_socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                active_socket.close()
        client_socket = None
        backend_socket = None
        to_client.clear()
        to_backend.clear()
        forwarded_client_frames.clear()
        observed_server_frames.clear()

    try:
        deadline = started_at + args.timeout_seconds
        while time.monotonic() < deadline:
            read_watch: list[object] = [master, listener]
            write_watch: list[socket.socket] = []
            if client_socket is not None:
                read_watch.append(client_socket)
                if to_client:
                    write_watch.append(client_socket)
            if backend_socket is not None:
                read_watch.append(backend_socket)
                if to_backend:
                    write_watch.append(backend_socket)
            try:
                readable, writable, _ = select.select(read_watch, write_watch, [], 0.05)
            except (OSError, ValueError):
                readable, writable = [], []

            if master in readable:
                try:
                    output = os.read(master, 65536)
                except OSError:
                    output = b""
                if output:
                    transcript.extend(output)

            if listener in readable:
                accepted, _ = listener.accept()
                accepted.setblocking(False)
                backend = socket.create_connection(
                    (args.backend_host, args.backend_port), timeout=5.0
                )
                backend.setblocking(False)
                close_pair()
                client_socket = accepted
                backend_socket = backend
                generation += 1
                if generation == 2:
                    second_connected_at = time.monotonic()
                note(f"proxy accepted connection generation {generation}")

            if client_socket is not None and client_socket in readable:
                try:
                    payload = client_socket.recv(65536)
                except BlockingIOError:
                    payload = None
                if payload is None:
                    pass
                elif not payload:
                    close_pair()
                    note("client side closed")
                else:
                    append_bounded(to_backend, payload, "client-to-server relay")

            if backend_socket is not None and backend_socket in readable:
                try:
                    payload = backend_socket.recv(65536)
                except BlockingIOError:
                    payload = None
                if payload is None:
                    pass
                elif not payload:
                    backend_socket.close()
                    backend_socket = None
                    note("server side closed")
                    if client_socket is not None and not to_client:
                        client_socket.close()
                        client_socket = None
                else:
                    append_bounded(
                        observed_server_frames, payload, "server frame inspection"
                    )
                    message_types = consume_protocol_message_types(
                        observed_server_frames
                    )
                    if (
                        generation == 1
                        and wait_frame_forwarded
                        and COMMAND_RESULT_MESSAGE_TYPE in message_types
                        and not dropped
                    ):
                        dropped = True
                        close_pair()
                        note(
                            "dropped first connection after the wait result was produced"
                        )
                    else:
                        append_bounded(to_client, payload, "server-to-client relay")

            if backend_socket is not None and backend_socket in writable and to_backend:
                try:
                    sent = backend_socket.send(to_backend)
                except BlockingIOError:
                    sent = 0
                if sent:
                    forwarded = bytes(to_backend[:sent])
                    del to_backend[:sent]
                    append_bounded(
                        forwarded_client_frames, forwarded, "client frame inspection"
                    )
                    if (
                        sent_wait
                        and generation == 1
                        and PLAYER_COMMAND_MESSAGE_TYPE
                        in consume_protocol_message_types(forwarded_client_frames)
                    ):
                        wait_frame_forwarded = True

            if client_socket is not None and client_socket in writable and to_client:
                try:
                    sent = client_socket.send(to_client)
                except BlockingIOError:
                    sent = 0
                if sent:
                    del to_client[:sent]
                    if not to_client and backend_socket is None:
                        client_socket.close()
                        client_socket = None

            scene_count = transcript.count(b"Authoritative scene synchronized.")
            if scene_count >= 1 and not sent_wait:
                os.write(master, b".")
                sent_wait = True
                note("sent local wait input")
            if dropped and b"Press confirm to reconnect" in transcript and not sent_reconnect:
                os.write(master, b"\r")
                sent_reconnect = True
                note("sent local reconnect confirmation")
            if (
                sent_reconnect
                and generation >= 2
                and scene_count >= 2
                and second_connected_at is not None
                and time.monotonic() - second_connected_at >= 0.5
                and not sent_move
            ):
                os.write(master, b"l")
                sent_move = True
                move_sent_at = time.monotonic()
                note("sent local east movement input after resume replay")
            if (
                sent_move
                and transcript.count(b"sent; awaiting server result.") >= 2
                and move_sent_at is not None
                and time.monotonic() - move_sent_at >= 0.5
                and not sent_quit
            ):
                os.write(master, b"\x1b")
                sent_quit = True
                note("sent local quit input")

            return_code = process.poll()
            if return_code is not None:
                note(f"client exited with status {return_code}")
                break
        else:
            note("client smoke timed out")
    finally:
        close_pair()
        listener.close()
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
        try:
            while True:
                transcript.extend(os.read(master, 65536))
        except OSError:
            pass
        os.close(master)
        args.transcript.parent.mkdir(parents=True, exist_ok=True)
        args.event_log.parent.mkdir(parents=True, exist_ok=True)
        args.transcript.write_bytes(transcript)
        args.event_log.write_text("\n".join(events) + "\n", encoding="utf-8")

    scene_count = transcript.count(b"Authoritative scene synchronized.")
    healthy = (
        sent_wait
        and dropped
        and sent_reconnect
        and generation >= 2
        and sent_move
        and sent_quit
        and transcript.count(b"sent; awaiting server result.") >= 2
        and process.returncode == 0
        and b"ERROR: AddressSanitizer" not in transcript
        and b"runtime error:" not in transcript
    )
    print(f"scene_sync_events={scene_count} transcript_bytes={len(transcript)}")
    return 0 if healthy else 1


if __name__ == "__main__":
    sys.exit(main())
