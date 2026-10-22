#include "FeishuSerialRx.h"

#include <string.h>

namespace {

constexpr uint32_t kLineGapMs = 1000;
constexpr uint32_t kFrameGapMs = 3000;
constexpr uint8_t kMagic[8] = {'X', 'T', 'A', 'S', 'K', '1', 0, 0};

void copyMessage(char* dst, size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  const size_t full = strlen(src);
  const bool truncated = full >= cap;
  size_t n = truncated ? cap - 1 : full;
  memcpy(dst, src, n);
  dst[n] = '\0';
  if (!truncated) {
    return;
  }
  while (n > 0 && (static_cast<unsigned char>(dst[n - 1]) & 0xC0) == 0x80) {
    dst[--n] = '\0';
  }
  if (n > 0) {
    const unsigned char lead = static_cast<unsigned char>(dst[n - 1]);
    if ((lead & 0xE0) == 0xC0 || (lead & 0xF0) == 0xE0 || (lead & 0xF8) == 0xF0) {
      dst[--n] = '\0';
    }
  }
}

bool startsWith(const char* text, size_t len, const char* prefix) {
  const size_t prefixLen = strlen(prefix);
  if (len < prefixLen) {
    return memcmp(text, prefix, len) == 0;
  }
  return memcmp(text, prefix, prefixLen) == 0;
}

}  // namespace

FeishuSerialRx::FeishuSerialRx() { reset(); }

FeishuSerialRx& FeishuSerialRx::instance() {
  static FeishuSerialRx rx;
  return rx;
}

void FeishuSerialRx::reset() {
  mode_ = Mode::Idle;
  pageTurnEnabled_ = true;
  magicOk_ = false;
  lastByteMs_ = 0;
  lineLen_ = 0;
  line_[0] = '\0';
  frameLen_ = 0;
  taskCount_ = 0;
  syncedAt_ = 0;
  memset(views_, 0, sizeof(views_));
}

void FeishuSerialRx::setPageTurnEnabled(bool enabled) { pageTurnEnabled_ = enabled; }

FeishuRxResult FeishuSerialRx::make(FeishuRxEvent event, const char* message) {
  FeishuRxResult result;
  result.event = event;
  copyMessage(result.message, sizeof(result.message), message);
  return result;
}

bool FeishuSerialRx::linePrefixOk() const {
  if (lineLen_ >= 8 && memcmp(line_, "TASKFAIL", 8) == 0) {
    return true;
  }
  return startsWith(line_, lineLen_, "TASKBUSY") || startsWith(line_, lineLen_, "TASKFAIL") ||
         startsWith(line_, lineLen_, "TASKPUSH");
}

void FeishuSerialRx::enterDrain(uint8_t byte) {
  mode_ = Mode::Drain;
  if (byte == '\n') {
    mode_ = Mode::Idle;
  }
}

FeishuRxResult FeishuSerialRx::failFrame(const char* message) {
  mode_ = Mode::Idle;
  magicOk_ = false;
  frameLen_ = 0;
  taskCount_ = 0;
  return make(FeishuRxEvent::FrameBad, message);
}

FeishuRxResult FeishuSerialRx::finishLine() {
  line_[lineLen_] = '\0';
  mode_ = Mode::Idle;
  if (strcmp(line_, "TASKBUSY") == 0) {
    return make(FeishuRxEvent::Busy, nullptr);
  }
  if (strcmp(line_, "TASKPUSH") == 0) {
    mode_ = Mode::Frame;
    magicOk_ = false;
    frameLen_ = 0;
    return make(FeishuRxEvent::None, nullptr);
  }
  if (strcmp(line_, "TASKFAIL") == 0) {
    return make(FeishuRxEvent::Fail, nullptr);
  }
  if (lineLen_ >= 9 && memcmp(line_, "TASKFAIL ", 9) == 0) {
    return make(FeishuRxEvent::Fail, line_ + 9);
  }
  return make(FeishuRxEvent::None, nullptr);
}

FeishuRxResult FeishuSerialRx::tick(uint32_t nowMs) {
  if (mode_ == Mode::Idle) {
    return make(FeishuRxEvent::None, nullptr);
  }
  const uint32_t limit = mode_ == Mode::Frame ? kFrameGapMs : kLineGapMs;
  if (nowMs - lastByteMs_ < limit) {
    return make(FeishuRxEvent::None, nullptr);
  }
  const bool frameStarted = mode_ == Mode::Frame && magicOk_;
  mode_ = Mode::Idle;
  magicOk_ = false;
  lineLen_ = 0;
  frameLen_ = 0;
  if (frameStarted) {
    taskCount_ = 0;
    return make(FeishuRxEvent::FrameBad, "同步超时");
  }
  return make(FeishuRxEvent::None, nullptr);
}

FeishuRxResult FeishuSerialRx::feed(uint8_t byte, uint32_t nowMs) {
  lastByteMs_ = nowMs;

  if (mode_ == Mode::Drain) {
    if (byte == '\n') {
      mode_ = Mode::Idle;
    }
    return make(FeishuRxEvent::None, nullptr);
  }

  if (mode_ == Mode::Line) {
    if (byte == '\r') {
      return make(FeishuRxEvent::None, nullptr);
    }
    if (byte == '\n') {
      return finishLine();
    }
    if (lineLen_ + 1 >= sizeof(line_)) {
      enterDrain(byte);
      return make(FeishuRxEvent::None, nullptr);
    }
    line_[lineLen_++] = static_cast<char>(byte);
    if (!linePrefixOk()) {
      enterDrain(byte);
    }
    return make(FeishuRxEvent::None, nullptr);
  }

  if (mode_ == Mode::Frame) {
    if (!magicOk_ && frameLen_ == 0 && (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n')) {
      return make(FeishuRxEvent::None, nullptr);
    }
    if (frameLen_ >= kMaxXTask1Frame) {
      return failFrame("数据过大");
    }
    frame_[frameLen_++] = byte;
    if (!magicOk_) {
      if (frame_[frameLen_ - 1] != kMagic[frameLen_ - 1]) {
        const uint8_t bad = byte;
        frameLen_ = 0;
        enterDrain(bad);
        return make(FeishuRxEvent::None, nullptr);
      }
      if (frameLen_ == 8) {
        magicOk_ = true;
      }
      return make(FeishuRxEvent::None, nullptr);
    }

    int count = 0;
    uint32_t syncedAt = 0;
    size_t consumed = 0;
    const XTask1Error err = parseXTask1(frame_, frameLen_, views_, kMaxXTask1Tasks, &count, &syncedAt, &consumed);
    if (err == XTask1Error::NeedMore) {
      return make(FeishuRxEvent::None, nullptr);
    }
    mode_ = Mode::Idle;
    magicOk_ = false;
    frameLen_ = 0;
    if (err != XTask1Error::Ok) {
      taskCount_ = 0;
      return make(FeishuRxEvent::FrameBad, xTask1ErrorText(err));
    }
    taskCount_ = count;
    syncedAt_ = syncedAt;
    return make(FeishuRxEvent::FrameOk, nullptr);
  }

  // Idle.
  const bool next = byte == 'n' || byte == 'N';
  const bool prev = byte == 'p' || byte == 'P';
  if (next || prev) {
    if (!pageTurnEnabled_) {
      return make(FeishuRxEvent::None, nullptr);
    }
    return make(next ? FeishuRxEvent::PageNext : FeishuRxEvent::PagePrev, nullptr);
  }
  if (byte == '\r' || byte == '\n') {
    return make(FeishuRxEvent::None, nullptr);
  }
  if (byte == 'T') {
    mode_ = Mode::Line;
    lineLen_ = 0;
    line_[lineLen_++] = 'T';
    return make(FeishuRxEvent::None, nullptr);
  }
  if (byte == 'X') {
    mode_ = Mode::Frame;
    magicOk_ = false;
    frameLen_ = 0;
    frame_[frameLen_++] = byte;
    return make(FeishuRxEvent::None, nullptr);
  }
  enterDrain(byte);
  return make(FeishuRxEvent::None, nullptr);
}
