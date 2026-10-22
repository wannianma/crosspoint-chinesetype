#pragma once

#include <stddef.h>
#include <stdint.h>

#include "XTask1Parse.h"

enum class FeishuSyncState : uint8_t { Idle, Syncing, Failed, Ready };

struct FeishuTaskMeta {
  char id[kMaxXTask1Id + 1];
  char title[kMaxXTask1Title + 1];
  uint32_t dueUnix;
  uint16_t descLen;
  uint8_t flags;
};

// Cached Feishu tasks. The wire frame is parsed elsewhere; this keeps the
// last good snapshot and the SD copy at /.crosspoint/feishu_tasks.bin.
class FeishuTaskStore {
 public:
  static FeishuTaskStore& instance();

  void loadFromFile();

  // Copies views (which alias the serial buffer) into RAM and the SD cache.
  // On failure the previous cache is left in place.
  bool replaceFromViews(const XTask1View* views, int count, uint32_t syncedAtUnix);

  void markSyncing();
  void markFailed(const char* reason);
  void requestSync();

  bool syncTimedOut(uint32_t nowMs) const;
  FeishuSyncState state() const;
  uint32_t revision() const;
  uint32_t syncedAtUnix() const;
  int count() const;

  void formatMenuLabel(char* out, size_t cap) const;
  void copyFailReason(char* out, size_t cap) const;
  bool copyMeta(int index, FeishuTaskMeta& out) const;
  bool readDescription(int index, char* buf, size_t cap) const;

  // `all` ignores needFlags. Otherwise a task matches when (flags & needFlags) != 0.
  int collect(uint8_t needFlags, bool all, int* indices, int cap) const;

 private:
  FeishuTaskStore() = default;

  struct Slot {
    char id[kMaxXTask1Id + 1];
    char title[kMaxXTask1Title + 1];
    uint32_t dueUnix;
    uint32_t descOffset;
    uint16_t descLen;
    uint8_t flags;
  };

  void lock() const;
  void unlock() const;
  bool loadLocked();
  bool writeCache(const XTask1View* views, int count, uint32_t syncedAtUnix, uint32_t* descOffsets) const;
  static void fillSlot(Slot& slot, const XTask1View& view, uint32_t descOffset);

  Slot slots_[kMaxXTask1Tasks];
  int count_ = 0;
  uint32_t syncedAt_ = 0;
  uint32_t revision_ = 0;
  uint32_t syncStartMs_ = 0;
  FeishuSyncState state_ = FeishuSyncState::Idle;
  char failReason_[48] = {};
};
