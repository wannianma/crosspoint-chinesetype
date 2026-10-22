#include "FeishuTaskStore.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr char kCachePath[] = "/.crosspoint/feishu_tasks.bin";
constexpr char kTmpPath[] = "/.crosspoint/feishu_tasks.tmp";
constexpr uint8_t kCacheVersion = 1;
constexpr uint32_t kSyncWaitMs = 30000;

SemaphoreHandle_t cacheMutex = nullptr;

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

void copyField(char* dst, size_t cap, const uint8_t* src, size_t len) {
  if (cap == 0) {
    return;
  }
  if (len >= cap) {
    len = cap - 1;
  }
  if (len > 0 && src != nullptr) {
    memcpy(dst, src, len);
    for (size_t i = 0; i < len; ++i) {
      if (dst[i] == '\0') {
        dst[i] = ' ';
      }
    }
  }
  dst[len] = '\0';
}

void copyReason(char* dst, size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= cap) {
    n = cap - 1;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

bool readAll(FsFile& file, void* dst, size_t len) {
  if (len == 0) {
    return true;
  }
  const int got = file.read(dst, len);
  return got == static_cast<int>(len);
}

bool writeAll(FsFile& file, const void* src, size_t len) {
  if (len == 0) {
    return true;
  }
  const int wrote = file.write(reinterpret_cast<const uint8_t*>(src), len);
  return wrote == static_cast<int>(len);
}

bool discard(FsFile& file, uint16_t len) {
  uint8_t trash[64];
  while (len > 0) {
    const uint16_t chunk = len > sizeof(trash) ? static_cast<uint16_t>(sizeof(trash)) : len;
    if (!readAll(file, trash, chunk)) {
      return false;
    }
    len = static_cast<uint16_t>(len - chunk);
  }
  return true;
}

}  // namespace

FeishuTaskStore& FeishuTaskStore::instance() {
  static FeishuTaskStore store;
  return store;
}

void FeishuTaskStore::lock() const {
  if (cacheMutex == nullptr) {
    cacheMutex = xSemaphoreCreateMutex();
  }
  if (cacheMutex != nullptr) {
    xSemaphoreTake(cacheMutex, portMAX_DELAY);
  }
}

void FeishuTaskStore::unlock() const {
  if (cacheMutex != nullptr) {
    xSemaphoreGive(cacheMutex);
  }
}

void FeishuTaskStore::fillSlot(Slot& slot, const XTask1View& view, uint32_t descOffset) {
  memset(&slot, 0, sizeof(slot));
  copyField(slot.id, sizeof(slot.id), view.id, view.idLen);
  copyField(slot.title, sizeof(slot.title), view.title, view.titleLen);
  slot.dueUnix = view.dueUnix;
  slot.descOffset = descOffset;
  slot.descLen = view.descLen;
  slot.flags = view.flags;
}

bool FeishuTaskStore::writeCache(const XTask1View* views, int count, uint32_t syncedAtUnix,
                                 uint32_t* descOffsets) const {
  if (count < 0 || count > kMaxXTask1Tasks) {
    return false;
  }
  if (!SdMan.ready()) {
    return false;
  }
  SdMan.mkdir("/.crosspoint");

  FsFile file;
  if (!SdMan.openFileForWrite("FTSK", kTmpPath, file)) {
    return false;
  }

  uint8_t header[11];
  header[0] = 'F';
  header[1] = 'T';
  header[2] = 'S';
  header[3] = '1';
  header[4] = kCacheVersion;
  writeU32(header + 5, syncedAtUnix);
  writeU16(header + 9, static_cast<uint16_t>(count));
  if (!writeAll(file, header, sizeof(header))) {
    file.close();
    SdMan.remove(kTmpPath);
    return false;
  }

  uint32_t cursor = sizeof(header);
  for (int i = 0; i < count; ++i) {
    const XTask1View& view = views[i];
    const uint8_t idLen = view.idLen;
    const uint8_t titleLen = view.titleLen;
    uint8_t due[4];
    uint8_t descLenBytes[2];
    writeU32(due, view.dueUnix);
    writeU16(descLenBytes, view.descLen);
    const bool fieldsOk = writeAll(file, &idLen, 1) && (idLen == 0 || writeAll(file, view.id, idLen)) &&
                          writeAll(file, &titleLen, 1) && (titleLen == 0 || writeAll(file, view.title, titleLen)) &&
                          writeAll(file, due, sizeof(due)) && writeAll(file, &view.flags, 1) &&
                          writeAll(file, descLenBytes, sizeof(descLenBytes));
    if (!fieldsOk) {
      file.close();
      SdMan.remove(kTmpPath);
      return false;
    }
    cursor += 1u + idLen + 1u + titleLen + 4u + 1u + 2u;
    descOffsets[i] = cursor;
    if (view.descLen > 0 && !writeAll(file, view.description, view.descLen)) {
      file.close();
      SdMan.remove(kTmpPath);
      return false;
    }
    cursor += view.descLen;
  }

  file.close();
  SdMan.remove(kCachePath);
  if (!SdMan.rename(kTmpPath, kCachePath)) {
    Serial.printf("[%lu] [FTSK] rename cache failed\n", millis());
    return false;
  }
  return true;
}

bool FeishuTaskStore::loadLocked() {
  if (!SdMan.ready() || !SdMan.exists(kCachePath)) {
    return false;
  }
  FsFile file;
  if (!SdMan.openFileForRead("FTSK", kCachePath, file)) {
    return false;
  }

  uint8_t header[11];
  if (!readAll(file, header, sizeof(header)) || memcmp(header, "FTS1", 4) != 0 || header[4] != kCacheVersion) {
    file.close();
    Serial.printf("[%lu] [FTSK] cache header rejected\n", millis());
    return false;
  }
  const uint32_t syncedAt = readU32(header + 5);
  const uint16_t count = readU16(header + 9);
  if (count > kMaxXTask1Tasks) {
    file.close();
    return false;
  }

  // Fill slots_ in place and publish count_ only after the file is fully read.
  // This runs at boot while count_ is still 0, so a failed read cannot show a
  // half-loaded list. Descriptions stay on the SD card.
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t idLen = 0;
    uint8_t titleLen = 0;
    uint8_t due[4];
    uint8_t flags = 0;
    uint8_t descLenBytes[2];
    memset(&slots_[i], 0, sizeof(slots_[i]));
    if (!readAll(file, &idLen, 1) || idLen > kMaxXTask1Id || !readAll(file, slots_[i].id, idLen)) {
      file.close();
      return false;
    }
    slots_[i].id[idLen] = '\0';
    if (!readAll(file, &titleLen, 1) || titleLen > kMaxXTask1Title || !readAll(file, slots_[i].title, titleLen)) {
      file.close();
      return false;
    }
    slots_[i].title[titleLen] = '\0';
    if (!readAll(file, due, sizeof(due)) || !readAll(file, &flags, 1) ||
        !readAll(file, descLenBytes, sizeof(descLenBytes))) {
      file.close();
      return false;
    }
    const uint16_t descLen = readU16(descLenBytes);
    if (descLen > kMaxXTask1Desc) {
      file.close();
      return false;
    }
    slots_[i].dueUnix = readU32(due);
    slots_[i].flags = flags;
    slots_[i].descLen = descLen;
    slots_[i].descOffset = static_cast<uint32_t>(file.position());
    if (!discard(file, descLen)) {
      file.close();
      return false;
    }
  }
  file.close();

  count_ = count;
  syncedAt_ = syncedAt;
  return true;
}

void FeishuTaskStore::loadFromFile() {
  lock();
  if (loadLocked()) {
    state_ = FeishuSyncState::Ready;
    failReason_[0] = '\0';
    revision_++;
    Serial.printf("[%lu] [FTSK] loaded %d tasks\n", millis(), count_);
  }
  unlock();
}

bool FeishuTaskStore::replaceFromViews(const XTask1View* views, int count, uint32_t syncedAtUnix) {
  if (count < 0 || count > kMaxXTask1Tasks || (count > 0 && views == nullptr)) {
    return false;
  }
  uint32_t descOffsets[kMaxXTask1Tasks];
  memset(descOffsets, 0, sizeof(descOffsets));

  lock();
  const bool wrote = writeCache(views, count, syncedAtUnix, descOffsets);
  if (!wrote) {
    unlock();
    Serial.printf("[%lu] [FTSK] cache write failed, keeping previous\n", millis());
    return false;
  }
  for (int i = 0; i < count; ++i) {
    fillSlot(slots_[i], views[i], descOffsets[i]);
  }
  count_ = count;
  syncedAt_ = syncedAtUnix;
  state_ = FeishuSyncState::Ready;
  failReason_[0] = '\0';
  revision_++;
  unlock();
  Serial.printf("[%lu] [FTSK] stored %d tasks\n", millis(), count);
  return true;
}

void FeishuTaskStore::markSyncing() {
  lock();
  syncStartMs_ = millis();
  if (state_ != FeishuSyncState::Syncing) {
    state_ = FeishuSyncState::Syncing;
    revision_++;
  }
  unlock();
}

void FeishuTaskStore::markFailed(const char* reason) {
  char copied[sizeof(failReason_)];
  copyReason(copied, sizeof(copied), reason != nullptr && reason[0] != '\0' ? reason : "同步失败");
  lock();
  if (state_ == FeishuSyncState::Failed && strcmp(failReason_, copied) == 0) {
    unlock();
    return;
  }
  state_ = FeishuSyncState::Failed;
  memcpy(failReason_, copied, sizeof(failReason_));
  revision_++;
  unlock();
  Serial.printf("[%lu] [FTSK] sync failed: %s\n", millis(), copied);
}

void FeishuTaskStore::requestSync() {
  markSyncing();
  Serial.print("TASKSYNC\n");
  Serial.flush();
  Serial.printf("[%lu] [FTSK] TASKSYNC\n", millis());
}

bool FeishuTaskStore::syncTimedOut(uint32_t nowMs) const {
  lock();
  const bool timedOut = state_ == FeishuSyncState::Syncing && (nowMs - syncStartMs_) >= kSyncWaitMs;
  unlock();
  return timedOut;
}

FeishuSyncState FeishuTaskStore::state() const {
  lock();
  const FeishuSyncState value = state_;
  unlock();
  return value;
}

uint32_t FeishuTaskStore::revision() const {
  lock();
  const uint32_t value = revision_;
  unlock();
  return value;
}

uint32_t FeishuTaskStore::syncedAtUnix() const {
  lock();
  const uint32_t value = syncedAt_;
  unlock();
  return value;
}

int FeishuTaskStore::count() const {
  lock();
  const int value = count_;
  unlock();
  return value;
}

void FeishuTaskStore::formatMenuLabel(char* out, size_t cap) const {
  if (out == nullptr || cap == 0) {
    return;
  }
  lock();
  int today = 0;
  int overdue = 0;
  for (int i = 0; i < count_; ++i) {
    if ((slots_[i].flags & kXTask1Done) != 0) {
      continue;
    }
    if ((slots_[i].flags & kXTask1Today) != 0) {
      today++;
    }
    if ((slots_[i].flags & kXTask1Overdue) != 0) {
      overdue++;
    }
  }
  unlock();
  if (overdue > 0 && today > 0) {
    snprintf(out, cap, "飞书任务 逾%d今%d", overdue, today);
  } else if (overdue > 0) {
    snprintf(out, cap, "飞书任务 逾期%d", overdue);
  } else if (today > 0) {
    snprintf(out, cap, "飞书任务 今日%d", today);
  } else {
    snprintf(out, cap, "飞书任务");
  }
}

void FeishuTaskStore::copyFailReason(char* out, size_t cap) const {
  if (out == nullptr || cap == 0) {
    return;
  }
  lock();
  copyReason(out, cap, failReason_);
  unlock();
}

bool FeishuTaskStore::copyMeta(int index, FeishuTaskMeta& out) const {
  lock();
  if (index < 0 || index >= count_) {
    unlock();
    return false;
  }
  const Slot& slot = slots_[index];
  memset(&out, 0, sizeof(out));
  memcpy(out.id, slot.id, sizeof(out.id));
  memcpy(out.title, slot.title, sizeof(out.title));
  out.dueUnix = slot.dueUnix;
  out.descLen = slot.descLen;
  out.flags = slot.flags;
  unlock();
  return true;
}

bool FeishuTaskStore::readDescription(int index, char* buf, size_t cap) const {
  if (buf == nullptr || cap == 0) {
    return false;
  }
  buf[0] = '\0';
  lock();
  if (index < 0 || index >= count_) {
    unlock();
    return false;
  }
  const uint16_t descLen = slots_[index].descLen;
  const uint32_t descOffset = slots_[index].descOffset;
  const uint16_t toCopy = static_cast<uint16_t>(descLen >= cap ? cap - 1 : descLen);
  unlock();
  if (toCopy == 0) {
    return true;
  }

  // Re-check under the lock around the SD read so a replace cannot rewrite the file mid-read.
  lock();
  if (index >= count_ || slots_[index].descOffset != descOffset || slots_[index].descLen != descLen) {
    unlock();
    return false;
  }
  FsFile file;
  if (!SdMan.openFileForRead("FTSK", kCachePath, file)) {
    unlock();
    return false;
  }
  if (!file.seek(descOffset)) {
    file.close();
    unlock();
    return false;
  }
  const bool ok = readAll(file, buf, toCopy);
  file.close();
  unlock();
  if (!ok) {
    buf[0] = '\0';
    return false;
  }
  buf[toCopy] = '\0';
  for (uint16_t i = 0; i < toCopy; ++i) {
    if (buf[i] == '\0') {
      buf[i] = ' ';
    }
  }
  return true;
}

int FeishuTaskStore::collect(uint8_t needFlags, bool all, int* indices, int cap) const {
  if (indices == nullptr || cap <= 0) {
    return 0;
  }
  lock();
  int written = 0;
  for (int i = 0; i < count_ && written < cap; ++i) {
    if (all || (slots_[i].flags & needFlags) != 0) {
      indices[written++] = i;
    }
  }
  unlock();
  return written;
}
