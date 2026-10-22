#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "activities/Activity.h"
#include "tasks/FeishuTaskStore.h"
#include "tasks/XTask1Parse.h"

class FeishuTaskActivity final : public Activity {
 public:
  explicit FeishuTaskActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::function<void()>& onGoHome)
      : Activity("FeishuTasks", renderer, mappedInput), onGoHome(onGoHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override;
  bool wantsSerialPageTurn() const override { return false; }

 private:
  enum class Screen : uint8_t { List, Detail, Syncing, Fail };

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void renderList();
  void renderDetail();
  void renderStatus(const char* title, const char* detail);
  void rebuildFilter(bool resetSelection);
  void onStoreChanged();
  void beginSync();
  void openDetail();
  void onBack();

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  const std::function<void()> onGoHome;

  Screen screen_ = Screen::List;
  bool updateRequired = false;
  bool fullRefresh_ = true;
  bool longFired_ = false;
  uint32_t seenRev_ = 0;
  int tab_ = 0;
  int selected_ = 0;
  int filtered_[kMaxXTask1Tasks] = {};
  int filteredCount_ = 0;
  int scroll_ = 0;
  int noteLineCount_ = 0;
  int noteVisible_ = 1;
  FeishuTaskMeta detail_{};
  char notes_[kMaxXTask1Desc + 1] = {};
};
