#!/usr/bin/env python3
"""Push an XTASK1 v1 frame to an X4 over USB CDC, without Feishu credentials.

The device speaks the same framing as the Mac companion (x4-page-turner):

  TASKBUSY\\n
  TASKPUSH\\n
  <binary XTASK1 frame>
  TASKFAIL <reason>\\n

Magic is 8 bytes: ASCII XTASK1 followed by two NUL bytes (58 54 41 53 4B 31 00 00).
CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) covers every byte after the magic
through the last task field. Version is 1. At most 50 tasks, 8 KiB total.

The device answers a long Confirm press with TASKSYNC\\n. Page-turn bytes n/p
are a different command and only apply outside the task screen.

  python3 tools/feishu_task_push.py --self-test
  python3 tools/feishu_task_push.py --port /dev/cu.usbmodemXXXX
  python3 tools/feishu_task_push.py --port /dev/cu.usbmodemXXXX --listen

--listen waits for TASKSYNC before pushing. pyserial is only needed to open a port.
"""

from __future__ import annotations

import argparse
import sys
import time

MAGIC = b"XTASK1\x00\x00"
MAX_TASKS = 50
MAX_FRAME = 8192


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def _u16(value: int) -> bytes:
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def _u32(value: int) -> bytes:
    return bytes(
        (
            value & 0xFF,
            (value >> 8) & 0xFF,
            (value >> 16) & 0xFF,
            (value >> 24) & 0xFF,
        )
    )


def encode_frame(tasks: list[dict], synced_at: int) -> bytes:
    if len(tasks) > MAX_TASKS:
        raise ValueError("task count > 50")
    body = bytearray()
    body += _u16(1)
    body += _u32(synced_at & 0xFFFFFFFF)
    body += _u16(len(tasks))
    for task in tasks:
        task_id = task.get("id", "").encode("utf-8")[:64]
        title = task.get("title", "").encode("utf-8")[:80]
        description = task.get("description", "").encode("utf-8")[:512]
        due = int(task.get("due_unix", 0)) & 0xFFFFFFFF
        flags = int(task.get("flags", 0)) & 0xFF
        body.append(len(task_id))
        body += task_id
        body.append(len(title))
        body += title
        body += _u32(due)
        body.append(flags)
        body += _u16(len(description))
        body += description
    frame = MAGIC + bytes(body) + _u16(crc16_ccitt(body))
    if len(frame) > MAX_FRAME:
        raise ValueError("frame larger than 8 KiB")
    return frame


def sample_tasks() -> list[dict]:
    # Flags: bit0 done, bit1 all-day, bit2 today, bit3 overdue.
    return [
        {
            "id": "sample-milk",
            "title": "买牛奶",
            "description": "低脂两盒\n记得小票",
            "due_unix": 1700000000,
            "flags": (1 << 1) | (1 << 2),
        },
        {
            "id": "sample-book",
            "title": "还书",
            "description": "图书馆三楼",
            "due_unix": 1700000000,
            "flags": 1 << 3,
        },
    ]


def push_bytes(port: str, payload: bytes, baud: int) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("pyserial is required to open a port: python3 -m pip install pyserial") from exc
    with serial.Serial(port, baud, timeout=0.2) as ser:
        ser.write(payload)
        ser.flush()


def listen_for_sync(port: str, baud: int) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("pyserial is required to open a port: python3 -m pip install pyserial") from exc
    print(f"listening on {port} for TASKSYNC", flush=True)
    with serial.Serial(port, baud, timeout=0.5) as ser:
        pending = b""
        while True:
            chunk = ser.read(256)
            if not chunk:
                continue
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if text == "TASKSYNC":
                    frame = encode_frame(sample_tasks(), int(time.time()))
                    packet = b"TASKBUSY\nTASKPUSH\n" + frame
                    ser.write(packet)
                    ser.flush()
                    print(f"pushed {len(sample_tasks())} tasks ({len(frame)} byte frame)", flush=True)


def self_test() -> None:
    digits = b"123456789"
    if crc16_ccitt(digits) != 0x29B1:
        raise SystemExit(f"crc vector mismatch: {crc16_ccitt(digits):#x}")
    frame = encode_frame(sample_tasks(), 0x12345678)
    if not frame.startswith(MAGIC):
        raise SystemExit("magic mismatch")
    if len(frame) > MAX_FRAME:
        raise SystemExit("frame too large")
    # Re-read the two sample titles back out of the body to catch a packing mistake.
    body = frame[8:-2]
    expect = crc16_ccitt(body)
    got = frame[-2] | (frame[-1] << 8)
    if got != expect:
        raise SystemExit("crc not stored little-endian")
    count = body[6] | (body[7] << 8)
    if count != 2:
        raise SystemExit(f"task count {count}")
    if "买牛奶".encode("utf-8") not in frame or "还书".encode("utf-8") not in frame:
        raise SystemExit("sample titles missing")
    print(f"feishu_task_push self-test ok ({len(frame)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description="Push sample Feishu tasks to an X4")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--port", help="USB CDC serial device, 115200 8N1")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--listen", action="store_true", help="Wait for TASKSYNC, then push")
    parser.add_argument("--stdout", action="store_true", help="Write TASKBUSY/TASKPUSH/frame to stdout")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return

    frame = encode_frame(sample_tasks(), int(time.time()))
    packet = b"TASKBUSY\nTASKPUSH\n" + frame
    if args.listen:
        if not args.port:
            raise SystemExit("--listen needs --port")
        listen_for_sync(args.port, args.baud)
        return
    if args.stdout or not args.port:
        sys.stdout.buffer.write(packet)
        return
    push_bytes(args.port, packet, args.baud)
    print(f"pushed {len(sample_tasks())} tasks to {args.port}", flush=True)


if __name__ == "__main__":
    main()
