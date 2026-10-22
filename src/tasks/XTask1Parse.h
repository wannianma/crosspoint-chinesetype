#pragma once

#include <stddef.h>
#include <stdint.h>

// XTASK1 v1 frame (little-endian), shared with the Mac companion.
//
// Magic is the 8 bytes of the C string literal "XTASK1\0":
//   58 54 41 53 4B 31 00 00
// (ASCII XTASK1, then two NUL bytes). The spec writes this as `XTASK1\0` (8 bytes).
//
// After magic:
//   uint16 version = 1
//   uint32 syncedAtUnix
//   uint16 taskCount (max 50)
//   repeated task:
//     uint8 idLen + id bytes (max 64)
//     uint8 titleLen + title bytes (max 80)
//     uint32 dueUnix (0 = none)
//     uint8 flags
//     uint16 descLen + description bytes (max 512)
//   uint16 crc16-ccitt (poly 0x1021, init 0xFFFF, not reflected)
//     over every byte AFTER magic through the last task field.
//
// The device does not recompute today/overdue; it trusts the host flags.

enum XTask1Flag : uint8_t {
  kXTask1Done = 1u << 0,
  kXTask1DueAllDay = 1u << 1,
  kXTask1Today = 1u << 2,
  kXTask1Overdue = 1u << 3,
};

constexpr int kMaxXTask1Tasks = 50;
constexpr int kMaxXTask1Id = 64;
constexpr int kMaxXTask1Title = 80;
constexpr int kMaxXTask1Desc = 512;
constexpr int kMaxXTask1Frame = 8192;
constexpr uint16_t kXTask1Version = 1;

enum class XTask1Error : uint8_t {
  Ok = 0,
  NeedMore,
  BadMagic,
  BadVersion,
  TooManyTasks,
  FieldTooLong,
  BadCrc,
  TooLarge,
  Truncated,
};

// Pointers alias `data` and are valid only as long as that buffer is.
struct XTask1View {
  const uint8_t* id;
  uint8_t idLen;
  const uint8_t* title;
  uint8_t titleLen;
  const uint8_t* description;
  uint16_t descLen;
  uint32_t dueUnix;
  uint8_t flags;
};

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, xorout 0, not reflected).
uint16_t xTask1Crc16(const uint8_t* data, size_t len);

// One-shot parse. On Ok, `*consumed` is the frame size (there may be bytes after it).
// On NeedMore, the prefix is a valid start of a frame but incomplete.
// `out` may be null only when the frame's taskCount is 0.
XTask1Error parseXTask1(const uint8_t* data, size_t len, XTask1View* out, int cap, int* count, uint32_t* syncedAtUnix,
                        size_t* consumed);

const char* xTask1ErrorText(XTask1Error err);

// UTC. Returns false when dueUnix is 0 (no due date). `out` is always NUL-terminated when cap > 0.
bool formatXTask1Due(uint32_t dueUnix, bool allDay, char* out, size_t cap);
