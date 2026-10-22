# 飞书任务 on the X4

Read-only task list for the 阅星曈 X4 (this chinesetype firmware). The device does not talk to Feishu. A Mac companion does OAuth and pushes a compact binary frame over the same USB CDC port the page-turner already uses.

No Feishu app id, secret, or user token is required to build, test, or flash this firmware.

## What the device does

- Home menu entry **飞书任务**. The badge counts unfinished tasks flagged today and overdue (`逾2今1`, `逾期N`, or `今日N`).
- List tabs **今日 / 逾期 / 全部**. Left / right change tabs (full refresh). Up / down move the selection (fast / partial refresh). Confirm opens the task. Back leaves.
- Detail shows the title, due line (UTC), and plain-text notes. Up / down scroll the notes. There is no complete or edit action.
- Empty cache or an empty tab: **暂无任务**.
- Long-press Confirm (about 800 ms) on any task screen writes `TASKSYNC\n` and shows **同步中…** until a frame or `TASKFAIL`.
- `TASKBUSY\n` shows the same syncing screen. `TASKFAIL <reason>\n` shows **同步失败** and keeps the previous cache. A good frame replaces the cache (an empty frame shows 暂无任务).
- Cache file: `/.crosspoint/feishu_tasks.bin` on the SD card. Descriptions stay in that file; RAM keeps titles and flags for at most 50 tasks.
- If the host never answers, the syncing screen becomes **同步失败 / 主机无响应** after 30 seconds.
- Outside this screen, USB bytes `n` / `N` / `p` / `P` still turn pages (250 ms debounce, same inject path as before). Those bytes are not injected while the task screen is open. Bytes inside an `XTASK1` frame or a `TASK…` line are never page turns.

The device does not recompute “today” or “overdue”. It trusts the flag bits in the frame. Due times are shown in UTC because the reader has no timezone.

## Wire format (v1)

Little-endian. One frame, at most 8 KiB.

| Field | Type |
| --- | --- |
| Magic | 8 bytes `XTASK1` + two NUL bytes (`58 54 41 53 4B 31 00 00`) |
| version | uint16 = 1 |
| syncedAtUnix | uint32 UTC seconds |
| taskCount | uint16, max 50 |
| tasks | see below |
| crc | uint16 CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, not reflected) over every byte **after** the magic through the last task field |

Each task:

| Field | Type |
| --- | --- |
| idLen + id | uint8, max 64 bytes |
| titleLen + title | uint8, UTF-8, max 80 bytes |
| dueUnix | uint32 seconds, 0 = no due date |
| flags | uint8: bit0 done, bit1 all-day, bit2 today, bit3 overdue |
| descLen + description | uint16, UTF-8, max 512 bytes, may be 0 |

Host → device lines (ASCII, newline terminated), optional around the frame:

- `TASKBUSY` — show 同步中…
- `TASKPUSH` — a binary frame follows (leading whitespace is skipped)
- `TASKFAIL <short reason>` — show 同步失败, keep the old cache

The frame may also arrive with no `TASKPUSH` line. Device → host: `TASKSYNC\n` on long Confirm.

`n` / `p` remain single-byte page turns and are only honored when the task screen is not in front. A text line that is not one of the commands above is discarded through the next newline, so log text is not a series of page turns. A gap of 3 seconds in the middle of a frame (or 1 second in the middle of a text line) returns the parser to idle.

## Host-side test (no device, no Feishu)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -Isrc/tasks -o /tmp/xtask1_test \
  test/feishu_task/test_xtask1.cpp \
  src/tasks/XTask1Parse.cpp \
  src/tasks/FeishuSerialRx.cpp
/tmp/xtask1_test

python3 tools/feishu_task_push.py --self-test
```

The C++ test parses a fixture frame (including 买牛奶 / a description that contains `n`, `p`, and newlines) and checks that those bytes are not page turns. `n` still turns the page before and after the frame.

## Try it on a device without Feishu

Build and flash like any other firmware. CI must not flash a board. From a computer, after the reader is on the USB CDC port at **115200 8N1**:

```bash
python3 -m pip install pyserial
python3 tools/feishu_task_push.py --port /dev/cu.usbmodemXXXX
```

Open **飞书任务** on the home menu. The sample push is two tasks (买牛奶 today, 还书 overdue). Long-press Confirm and the other direction:

```bash
python3 tools/feishu_task_push.py --port /dev/cu.usbmodemXXXX --listen
```

That waits for `TASKSYNC` and then pushes. macOS port names look like `/dev/cu.usbmodem*`. Firmware logs share this port; the script only reacts to a `TASKSYNC` line.

Page-turn check, with a book open and the task screen closed: send `n`, then `p`. The log line is `[DBG] [USB] Page turn next` or `prev`.

## Mac companion

`x4-page-turner` on the Mac holds the Feishu user token and listens on the open serial port:

1. `FEISHU_APP_ID` and `FEISHU_APP_SECRET` come from the environment or a local config file. Do not commit them.
2. Login uses a localhost loopback redirect. The companion README has the port.
3. It reads Task v2 `my_tasks`, truncates fields to the limits above, sets the flag bits, and writes `TASKBUSY`, `TASKPUSH`, and one `XTASK1` frame at 115200.
4. On `TASKSYNC\n` it syncs again. On failure it writes `TASKFAIL <reason>\n`.

This repository does not contain that app and does not perform Feishu OAuth.

## Flash

Do this from a workstation, not from CI. The image is the PlatformIO `default` env. Steps and offsets are in [FLASH.md](../FLASH.md) and in the project README (web flasher at https://xteink.dve.al/, or `pio run -e default -t upload`).

```bash
pio run -e default
```

USB serial console baud is 115200. The upload baud in `platformio.ini` (921600) is only for esptool.
