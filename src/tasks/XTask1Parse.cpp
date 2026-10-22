#include "XTask1Parse.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr uint8_t kMagic[8] = {'X', 'T', 'A', 'S', 'K', '1', 0, 0};

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void civilFromDays(int z, int* year, unsigned* month, unsigned* day) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  *year = y + (m <= 2);
  *month = m;
  *day = d;
}

}  // namespace

uint16_t xTask1Crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 0x8000) != 0) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

const char* xTask1ErrorText(XTask1Error err) {
  switch (err) {
    case XTask1Error::Ok:
      return "";
    case XTask1Error::NeedMore:
      return "";
    case XTask1Error::BadMagic:
      return "帧头错误";
    case XTask1Error::BadVersion:
      return "版本不支持";
    case XTask1Error::TooManyTasks:
      return "任务过多";
    case XTask1Error::FieldTooLong:
      return "字段过长";
    case XTask1Error::BadCrc:
      return "校验失败";
    case XTask1Error::TooLarge:
      return "数据过大";
    case XTask1Error::Truncated:
      return "同步超时";
  }
  return "同步失败";
}

XTask1Error parseXTask1(const uint8_t* data, size_t len, XTask1View* out, int cap, int* count, uint32_t* syncedAtUnix,
                        size_t* consumed) {
  if (count) {
    *count = 0;
  }
  if (consumed) {
    *consumed = 0;
  }
  if (data == nullptr) {
    return XTask1Error::Truncated;
  }
  if (len < 8) {
    return XTask1Error::NeedMore;
  }
  if (memcmp(data, kMagic, 8) != 0) {
    return XTask1Error::BadMagic;
  }
  if (len < 16) {
    return XTask1Error::NeedMore;
  }

  const uint16_t version = readU16(data + 8);
  if (version != kXTask1Version) {
    return XTask1Error::BadVersion;
  }
  const uint16_t taskCount = readU16(data + 14);
  if (taskCount > kMaxXTask1Tasks || taskCount > cap || (taskCount > 0 && out == nullptr)) {
    return XTask1Error::TooManyTasks;
  }

  size_t off = 16;
  for (uint16_t i = 0; i < taskCount; ++i) {
    if (off >= len) {
      return XTask1Error::NeedMore;
    }
    const uint8_t idLen = data[off];
    if (idLen > kMaxXTask1Id) {
      return XTask1Error::FieldTooLong;
    }
    off += 1u + idLen;
    if (off > kMaxXTask1Frame) {
      return XTask1Error::TooLarge;
    }
    if (off >= len) {
      return XTask1Error::NeedMore;
    }

    const uint8_t titleLen = data[off];
    if (titleLen > kMaxXTask1Title) {
      return XTask1Error::FieldTooLong;
    }
    off += 1u + titleLen;
    if (off > kMaxXTask1Frame) {
      return XTask1Error::TooLarge;
    }
    if (off >= len) {
      return XTask1Error::NeedMore;
    }

    if (off + 7 > kMaxXTask1Frame) {
      return XTask1Error::TooLarge;
    }
    if (off + 7 > len) {
      return XTask1Error::NeedMore;
    }
    const uint16_t descLen = readU16(data + off + 5);
    if (descLen > kMaxXTask1Desc) {
      return XTask1Error::FieldTooLong;
    }
    const size_t taskEnd = off + 7u + descLen;
    if (taskEnd > kMaxXTask1Frame) {
      return XTask1Error::TooLarge;
    }
    if (taskEnd > len) {
      return XTask1Error::NeedMore;
    }
    off = taskEnd;
  }

  if (off + 2 > kMaxXTask1Frame) {
    return XTask1Error::TooLarge;
  }
  if (len < off + 2) {
    return XTask1Error::NeedMore;
  }

  const uint16_t got = readU16(data + off);
  const uint16_t expect = xTask1Crc16(data + 8, off - 8);
  if (got != expect) {
    return XTask1Error::BadCrc;
  }

  size_t cursor = 16;
  for (uint16_t i = 0; i < taskCount; ++i) {
    XTask1View& view = out[i];
    view.idLen = data[cursor++];
    view.id = data + cursor;
    cursor += view.idLen;
    view.titleLen = data[cursor++];
    view.title = data + cursor;
    cursor += view.titleLen;
    view.dueUnix = readU32(data + cursor);
    cursor += 4;
    view.flags = data[cursor++];
    view.descLen = readU16(data + cursor);
    cursor += 2;
    view.description = data + cursor;
    cursor += view.descLen;
  }

  if (syncedAtUnix) {
    *syncedAtUnix = readU32(data + 10);
  }
  if (count) {
    *count = taskCount;
  }
  if (consumed) {
    *consumed = off + 2;
  }
  return XTask1Error::Ok;
}

bool formatXTask1Due(uint32_t dueUnix, bool allDay, char* out, size_t cap) {
  if (out == nullptr || cap == 0) {
    return false;
  }
  out[0] = '\0';
  if (dueUnix == 0) {
    return false;
  }
  const uint32_t days = dueUnix / 86400u;
  const uint32_t sod = dueUnix % 86400u;
  int year = 1970;
  unsigned month = 1;
  unsigned day = 1;
  civilFromDays(static_cast<int>(days), &year, &month, &day);
  if (allDay) {
    snprintf(out, cap, "%04d-%02u-%02u 全天", year, month, day);
  } else {
    const unsigned hh = sod / 3600u;
    const unsigned mm = (sod % 3600u) / 60u;
    snprintf(out, cap, "%04d-%02u-%02u %02u:%02u", year, month, day, hh, mm);
  }
  return true;
}
