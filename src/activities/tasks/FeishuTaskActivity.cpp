#include "FeishuTaskActivity.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr unsigned long kSyncHoldMs = 800;
constexpr int kMaxWrapLines = 80;

struct WrappedLine {
  uint16_t off;
  uint16_t len;
};

const char* kTabLabels[] = {"今日", "逾期", "全部"};

int wrapText(const GfxRenderer& renderer, const char* text, int maxWidth, WrappedLine* lines, int maxLines) {
  if (text == nullptr || lines == nullptr || maxLines <= 0 || maxWidth <= 0) {
    return 0;
  }
  const int total = static_cast<int>(strlen(text));
  int index = 0;
  int count = 0;
  int guard = 0;
  while (index < total && count < maxLines && guard++ < kMaxXTask1Desc + 4) {
    if (text[index] == '\n') {
      lines[count].off = static_cast<uint16_t>(index);
      lines[count].len = 0;
      count++;
      index++;
      continue;
    }

    int last = index;
    int cursor = index;
    while (cursor < total && text[cursor] != '\n') {
      const unsigned char* next = reinterpret_cast<const unsigned char*>(text + cursor);
      const unsigned char* start = next;
      utf8NextCodepoint(&next);
      int cpLen = static_cast<int>(next - start);
      if (cpLen <= 0) {
        cpLen = 1;
      }
      const int candidate = cursor + cpLen;
      if (candidate - index >= 180) {
        break;
      }
      char tmp[192];
      const int len = candidate - index;
      memcpy(tmp, text + index, static_cast<size_t>(len));
      tmp[len] = '\0';
      if (renderer.getTextWidth(UI_12_FONT_ID, tmp) > maxWidth) {
        break;
      }
      last = candidate;
      cursor = candidate;
    }

    if (last == index) {
      const unsigned char* next = reinterpret_cast<const unsigned char*>(text + index);
      const unsigned char* start = next;
      utf8NextCodepoint(&next);
      int cpLen = static_cast<int>(next - start);
      if (cpLen <= 0) {
        cpLen = 1;
      }
      last = index + cpLen;
      if (last > total) {
        last = total;
      }
    }

    lines[count].off = static_cast<uint16_t>(index);
    int len = last - index;
    if (count == maxLines - 1 && last < total) {
      len = total - index;
    }
    if (len > 500) {
      len = 500;
    }
    lines[count].len = static_cast<uint16_t>(len);
    count++;
    index = last;
    if (index < total && text[index] == '\n') {
      index++;
    }
    if (count == maxLines) {
      break;
    }
  }
  return count;
}

void drawWrapped(const GfxRenderer& renderer, const char* text, const WrappedLine& line, int x, int y, int maxWidth) {
  char tmp[192];
  int len = line.len;
  if (len <= 0 || text == nullptr) {
    return;
  }
  if (len > 180) {
    len = 180;
  }
  memcpy(tmp, text + line.off, static_cast<size_t>(len));
  tmp[len] = '\0';
  const std::string shown = renderer.truncatedText(UI_12_FONT_ID, tmp, maxWidth);
  renderer.drawText(UI_12_FONT_ID, x, y, shown.c_str());
}

void formatSubtitle(const FeishuTaskMeta& meta, char* out, size_t cap) {
  if (out == nullptr || cap == 0) {
    return;
  }
  char due[40];
  due[0] = '\0';
  formatXTask1Due(meta.dueUnix, (meta.flags & kXTask1DueAllDay) != 0, due, sizeof(due));
  const char* tag = "";
  if ((meta.flags & kXTask1Done) != 0) {
    tag = "已完成";
  } else if ((meta.flags & kXTask1Overdue) != 0) {
    tag = "逾期";
  } else if ((meta.flags & kXTask1Today) != 0) {
    tag = "今日";
  }
  if (tag[0] != '\0' && due[0] != '\0') {
    snprintf(out, cap, "%s %s", tag, due);
  } else if (tag[0] != '\0') {
    snprintf(out, cap, "%s", tag);
  } else {
    snprintf(out, cap, "%s", due);
  }
}

}  // namespace

void FeishuTaskActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FeishuTaskActivity*>(param);
  self->displayTaskLoop();
}

void FeishuTaskActivity::rebuildFilter(bool resetSelection) {
  const bool all = tab_ == 2;
  const uint8_t flag = tab_ == 0 ? kXTask1Today : kXTask1Overdue;
  filteredCount_ = FeishuTaskStore::instance().collect(flag, all, filtered_, kMaxXTask1Tasks);
  if (resetSelection || filteredCount_ <= 0) {
    selected_ = 0;
  } else if (selected_ >= filteredCount_) {
    selected_ = filteredCount_ - 1;
  }
  if (selected_ < 0) {
    selected_ = 0;
  }
}

void FeishuTaskActivity::onStoreChanged() {
  const FeishuSyncState state = FeishuTaskStore::instance().state();
  if (state == FeishuSyncState::Syncing) {
    screen_ = Screen::Syncing;
  } else if (state == FeishuSyncState::Failed) {
    screen_ = Screen::Fail;
  } else {
    screen_ = Screen::List;
    rebuildFilter(false);
  }
  fullRefresh_ = true;
  updateRequired = true;
}

void FeishuTaskActivity::beginSync() {
  FeishuTaskStore::instance().requestSync();
  seenRev_ = FeishuTaskStore::instance().revision();
  screen_ = Screen::Syncing;
  fullRefresh_ = true;
  updateRequired = true;
}

void FeishuTaskActivity::openDetail() {
  if (selected_ < 0 || selected_ >= filteredCount_) {
    return;
  }
  const int index = filtered_[selected_];
  if (!FeishuTaskStore::instance().copyMeta(index, detail_)) {
    return;
  }
  if (!FeishuTaskStore::instance().readDescription(index, notes_, sizeof(notes_))) {
    notes_[0] = '\0';
  }
  scroll_ = 0;
  noteLineCount_ = 0;
  screen_ = Screen::Detail;
  fullRefresh_ = true;
  updateRequired = true;
}

void FeishuTaskActivity::onBack() {
  if (screen_ == Screen::Detail || screen_ == Screen::Fail) {
    screen_ = Screen::List;
    rebuildFilter(false);
    fullRefresh_ = true;
    updateRequired = true;
    return;
  }
  onGoHome();
}

void FeishuTaskActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderingMutex = xSemaphoreCreateMutex();

  auto& store = FeishuTaskStore::instance();
  seenRev_ = store.revision();
  tab_ = 0;
  selected_ = 0;
  scroll_ = 0;
  longFired_ = false;
  switch (store.state()) {
    case FeishuSyncState::Syncing:
      screen_ = Screen::Syncing;
      break;
    case FeishuSyncState::Failed:
      screen_ = Screen::Fail;
      break;
    case FeishuSyncState::Idle:
    case FeishuSyncState::Ready:
      screen_ = Screen::List;
      break;
  }
  rebuildFilter(true);
  fullRefresh_ = true;
  updateRequired = true;

  xTaskCreate(&FeishuTaskActivity::taskTrampoline, "FeishuTaskActivity", 6144, this, 1, &displayTaskHandle);
}

void FeishuTaskActivity::onExit() {
  Activity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

bool FeishuTaskActivity::preventAutoSleep() {
  return screen_ == Screen::Syncing || FeishuTaskStore::instance().state() == FeishuSyncState::Syncing;
}

void FeishuTaskActivity::loop() {
  auto& store = FeishuTaskStore::instance();
  const uint32_t rev = store.revision();
  if (rev != seenRev_) {
    seenRev_ = rev;
    onStoreChanged();
  }

  const bool confirmDown = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmReleased) {
    const bool wasLong = longFired_ || mappedInput.getHeldTime() >= kSyncHoldMs;
    if (!longFired_ && wasLong) {
      beginSync();
    }
    longFired_ = false;
    if (!wasLong && screen_ == Screen::List) {
      openDetail();
    }
  } else if (confirmDown && !longFired_ && mappedInput.getHeldTime() >= kSyncHoldMs) {
    longFired_ = true;
    beginSync();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (screen_ == Screen::List) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      tab_ = (tab_ + 2) % 3;
      rebuildFilter(true);
      fullRefresh_ = true;
      updateRequired = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      tab_ = (tab_ + 1) % 3;
      rebuildFilter(true);
      fullRefresh_ = true;
      updateRequired = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (selected_ > 0) {
        selected_--;
        fullRefresh_ = false;
        updateRequired = true;
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (selected_ + 1 < filteredCount_) {
        selected_++;
        fullRefresh_ = false;
        updateRequired = true;
      }
    }
  } else if (screen_ == Screen::Detail) {
    const int maxStart = noteLineCount_ > noteVisible_ ? noteLineCount_ - noteVisible_ : 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) && scroll_ > 0) {
      scroll_--;
      fullRefresh_ = false;
      updateRequired = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) && scroll_ < maxStart) {
      scroll_++;
      fullRefresh_ = false;
      updateRequired = true;
    }
  }
}

void FeishuTaskActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void FeishuTaskActivity::render() {
  const bool full = fullRefresh_;
  fullRefresh_ = false;
  renderer.clearScreen();
  switch (screen_) {
    case Screen::Detail:
      renderDetail();
      break;
    case Screen::Syncing:
      renderStatus("同步中…", "等待主机");
      break;
    case Screen::Fail: {
      char reason[48];
      FeishuTaskStore::instance().copyFailReason(reason, sizeof(reason));
      renderStatus("同步失败", reason);
      break;
    }
    case Screen::List:
      renderList();
      break;
  }

  renderer.displayBuffer(full ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}

void FeishuTaskActivity::renderStatus(const char* title, const char* detail) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "飞书任务");
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, title, true, EpdFontFamily::BOLD);
  if (detail != nullptr && detail[0] != '\0') {
    const std::string line = renderer.truncatedText(UI_12_FONT_ID, detail, pageWidth - 48);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 + 4, line.c_str());
  }
  const char* confirm = screen_ == Screen::Fail ? "长按同步" : "";
  const auto labels = mappedInput.mapLabels("返回", confirm, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FeishuTaskActivity::renderList() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "飞书任务");

  std::vector<TabInfo> tabs;
  tabs.reserve(3);
  for (int i = 0; i < 3; ++i) {
    tabs.push_back(TabInfo{kTabLabels[i], i == tab_});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 false);

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (filteredCount_ <= 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, listTop + listHeight / 2 - 20, "暂无任务", true, EpdFontFamily::BOLD);
  } else {
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listHeight}, filteredCount_, selected_,
        [this](int index) {
          FeishuTaskMeta meta;
          if (index < 0 || index >= filteredCount_ || !FeishuTaskStore::instance().copyMeta(filtered_[index], meta)) {
            return std::string();
          }
          return std::string(meta.title);
        },
        [this](int index) {
          FeishuTaskMeta meta;
          char line[64];
          line[0] = '\0';
          if (index >= 0 && index < filteredCount_ && FeishuTaskStore::instance().copyMeta(filtered_[index], meta)) {
            formatSubtitle(meta, line, sizeof(line));
          }
          return std::string(line);
        },
        nullptr, nullptr);
  }

  const auto labels = mappedInput.mapLabels("返回", "查看", "上一类", "下一类");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "向上", "向下");
}

void FeishuTaskActivity::renderDetail() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  const int pad = metrics.contentSidePadding;
  const int maxWidth = pageWidth - pad * 2;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + 4;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "飞书任务");

  WrappedLine titleLines[4];
  const int titleCount = wrapText(renderer, detail_.title, maxWidth, titleLines, 4);
  for (int i = 0; i < titleCount; ++i) {
    drawWrapped(renderer, detail_.title, titleLines[i], pad, y, maxWidth);
    y += lineHeight;
  }

  char subtitle[64];
  formatSubtitle(detail_, subtitle, sizeof(subtitle));
  if (subtitle[0] != '\0') {
    renderer.drawText(UI_10_FONT_ID, pad, y, subtitle);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  y += 6;
  renderer.drawLine(pad, y, pageWidth - pad, y);
  y += 8;

  const int areaBottom = pageHeight - metrics.buttonHintsHeight - 6;
  int visible = lineHeight > 0 ? (areaBottom - y) / lineHeight : 1;
  if (visible < 1) {
    visible = 1;
  }
  noteVisible_ = visible;

  if (notes_[0] == '\0') {
    noteLineCount_ = 0;
    scroll_ = 0;
    renderer.drawText(UI_12_FONT_ID, pad, y, "无备注");
  } else {
    WrappedLine noteLines[kMaxWrapLines];
    const int noteCount = wrapText(renderer, notes_, maxWidth, noteLines, kMaxWrapLines);
    noteLineCount_ = noteCount;
    const int maxStart = noteCount > visible ? noteCount - visible : 0;
    if (scroll_ > maxStart) {
      scroll_ = maxStart;
    }
    if (scroll_ < 0) {
      scroll_ = 0;
    }
    for (int i = 0; i < visible && scroll_ + i < noteCount; ++i) {
      drawWrapped(renderer, notes_, noteLines[scroll_ + i], pad, y + i * lineHeight, maxWidth);
    }
    if (noteCount > visible && maxWidth > 8) {
      const int barX = pageWidth - pad;
      const int barH = visible * lineHeight;
      const int thumbH = barH * visible / noteCount;
      const int travel = barH - (thumbH > 0 ? thumbH : 1);
      const int thumbY = y + (maxStart > 0 ? travel * scroll_ / maxStart : 0);
      renderer.drawRect(barX - 3, y, 3, barH);
      renderer.fillRect(barX - 3, thumbY, 3, thumbH > 2 ? thumbH : 2);
    }
  }

  const auto labels = mappedInput.mapLabels("返回", "长按同步", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "向上", "向下");
}
