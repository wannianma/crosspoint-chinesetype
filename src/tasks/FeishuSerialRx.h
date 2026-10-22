#pragma once

#include <stddef.h>
#include <stdint.h>

#include "XTask1Parse.h"

// Byte stream in front of the USB CDC page-turn reader.
// Idle n/N/p/P are page turns. TASKBUSY / TASKFAIL / TASKPUSH lines and XTASK1
// frames are consumed whole, so bytes inside them are never page turns.
// Unknown text is drained through the next newline (same as the previous
// page-turn reader). A gap with no newline or no completed frame returns to idle.

enum class FeishuRxEvent : uint8_t {
  None = 0,
  PageNext,
  PagePrev,
  Busy,
  Fail,
  FrameOk,
  FrameBad,
};

struct FeishuRxResult {
  FeishuRxEvent event;
  char message[48];
};

class FeishuSerialRx {
 public:
  FeishuSerialRx();
  FeishuSerialRx(const FeishuSerialRx&) = delete;
  FeishuSerialRx& operator=(const FeishuSerialRx&) = delete;

  static FeishuSerialRx& instance();

  void reset();
  void setPageTurnEnabled(bool enabled);

  // `nowMs` is a free-running millisecond clock (wraps like millis()).
  FeishuRxResult feed(uint8_t byte, uint32_t nowMs);
  FeishuRxResult tick(uint32_t nowMs);

  // Valid until the next feed() that starts another frame. Aliases the internal buffer.
  const XTask1View* tasks() const { return views_; }
  int taskCount() const { return taskCount_; }
  uint32_t syncedAtUnix() const { return syncedAt_; }

 private:
  enum class Mode : uint8_t { Idle, Line, Frame, Drain };

  FeishuRxResult make(FeishuRxEvent event, const char* message);
  bool linePrefixOk() const;
  FeishuRxResult finishLine();
  void enterDrain(uint8_t byte);
  FeishuRxResult failFrame(const char* message);

  Mode mode_;
  bool pageTurnEnabled_;
  bool magicOk_;
  uint32_t lastByteMs_;
  char line_[96];
  size_t lineLen_;
  uint8_t frame_[kMaxXTask1Frame];
  size_t frameLen_;
  XTask1View views_[kMaxXTask1Tasks];
  int taskCount_;
  uint32_t syncedAt_;
};
