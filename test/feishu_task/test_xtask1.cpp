#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "FeishuSerialRx.h"
#include "XTask1Parse.h"

namespace {

int g_fails = 0;

void check(bool cond, const char* expr, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d %s\n", "test_xtask1.cpp", line, expr);
    g_fails++;
  }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

struct Sample {
  std::string id;
  std::string title;
  std::string description;
  uint32_t dueUnix;
  uint8_t flags;
};

std::vector<uint8_t> buildFrame(uint32_t syncedAt, const std::vector<Sample>& tasks, uint16_t version = 1) {
  std::vector<uint8_t> out = {'X', 'T', 'A', 'S', 'K', '1', 0, 0};
  appendU16(out, version);
  appendU32(out, syncedAt);
  appendU16(out, static_cast<uint16_t>(tasks.size()));
  for (const Sample& task : tasks) {
    out.push_back(static_cast<uint8_t>(task.id.size()));
    out.insert(out.end(), task.id.begin(), task.id.end());
    out.push_back(static_cast<uint8_t>(task.title.size()));
    out.insert(out.end(), task.title.begin(), task.title.end());
    appendU32(out, task.dueUnix);
    out.push_back(task.flags);
    appendU16(out, static_cast<uint16_t>(task.description.size()));
    out.insert(out.end(), task.description.begin(), task.description.end());
  }
  const uint16_t crc = xTask1Crc16(out.data() + 8, out.size() - 8);
  appendU16(out, crc);
  return out;
}

bool sameBytes(const uint8_t* data, size_t len, const std::string& text) {
  return len == text.size() && (len == 0 || std::memcmp(data, text.data(), len) == 0);
}

std::vector<FeishuRxEvent> feedAll(FeishuSerialRx& rx, const uint8_t* data, size_t len, uint32_t startMs = 1000) {
  std::vector<FeishuRxEvent> events;
  for (size_t i = 0; i < len; ++i) {
    const FeishuRxResult result = rx.feed(data[i], startMs + static_cast<uint32_t>(i));
    if (result.event != FeishuRxEvent::None) {
      events.push_back(result.event);
    }
  }
  return events;
}

std::vector<FeishuRxEvent> feedStr(FeishuSerialRx& rx, const char* text, uint32_t startMs = 1000) {
  return feedAll(rx, reinterpret_cast<const uint8_t*>(text), std::strlen(text), startMs);
}

void testCrcAndDue() {
  const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(xTask1Crc16(digits, sizeof(digits)) == 0x29B1);

  char due[40];
  CHECK(!formatXTask1Due(0, false, due, sizeof(due)));
  CHECK(due[0] == '\0');
  CHECK(formatXTask1Due(1700000000u, false, due, sizeof(due)));
  CHECK(std::strcmp(due, "2023-11-14 22:13") == 0);
  CHECK(formatXTask1Due(1700000000u, true, due, sizeof(due)));
  CHECK(std::strcmp(due, "2023-11-14 全天") == 0);
  CHECK(formatXTask1Due(0, false, due, sizeof(due)) == false);
}

void testRoundTrip() {
  const std::string milk = "\xE4\xB9\xB0\xE7\x89\x9B\xE5\xA5\xB6";  // 买牛奶
  const std::string note = "\xE4\xBD\x8E\xE8\x84\x82\xE4\xB8\xA4\xE7\x9B\x92\n" + std::string("n\np\nline");
  Sample first{std::string("g-") + std::string(60, 'a'), milk, note, 1700000000u, kXTask1Today | kXTask1DueAllDay};
  Sample second{"id2", "还书", std::string(1, '\0') + "p", 0, kXTask1Overdue | kXTask1Done};
  // "还书" as UTF-8 written directly would depend on the source charset. Build it explicitly.
  second.title = "\xE8\xBF\x98\xE4\xB9\xA6";

  const auto frame = buildFrame(0x12345678u, {first, second});
  CHECK(frame.size() < static_cast<size_t>(kMaxXTask1Frame));
  CHECK(frame.size() >= 8);
  CHECK(frame[0] == 'X' && frame[6] == 0 && frame[7] == 0);

  XTask1View views[kMaxXTask1Tasks];
  int count = -1;
  uint32_t synced = 0;
  size_t consumed = 0;
  const auto prefix = parseXTask1(frame.data(), frame.size() / 2, views, kMaxXTask1Tasks, &count, &synced, &consumed);
  CHECK(prefix == XTask1Error::NeedMore);
  CHECK(consumed == 0);

  std::vector<uint8_t> padded = frame;
  padded.push_back('n');
  padded.push_back('p');
  const auto ok = parseXTask1(padded.data(), padded.size(), views, kMaxXTask1Tasks, &count, &synced, &consumed);
  CHECK(ok == XTask1Error::Ok);
  CHECK(consumed == frame.size());
  CHECK(count == 2);
  CHECK(synced == 0x12345678u);
  CHECK(sameBytes(views[0].id, views[0].idLen, first.id));
  CHECK(sameBytes(views[0].title, views[0].titleLen, milk));
  CHECK(sameBytes(views[0].description, views[0].descLen, note));
  CHECK(views[0].dueUnix == 1700000000u);
  CHECK(views[0].flags == (kXTask1Today | kXTask1DueAllDay));
  CHECK(sameBytes(views[1].title, views[1].titleLen, second.title));
  CHECK(views[1].descLen == 2);
  CHECK(views[1].description[0] == 0);
  CHECK(views[1].description[1] == 'p');
  CHECK((views[1].flags & kXTask1Overdue) != 0);
  CHECK((views[1].flags & kXTask1Done) != 0);
}

void testRejects() {
  XTask1View views[kMaxXTask1Tasks];
  int count = 0;
  uint32_t synced = 0;
  size_t consumed = 0;

  auto empty = buildFrame(42, {});
  CHECK(parseXTask1(empty.data(), empty.size(), nullptr, 0, &count, &synced, &consumed) == XTask1Error::Ok);
  CHECK(count == 0);
  CHECK(synced == 42);

  auto badCrc = empty;
  badCrc.back() ^= 0xFF;
  CHECK(parseXTask1(badCrc.data(), badCrc.size(), views, kMaxXTask1Tasks, &count, &synced, &consumed) ==
        XTask1Error::BadCrc);

  auto badVer = buildFrame(1, {}, 2);
  CHECK(parseXTask1(badVer.data(), badVer.size(), views, kMaxXTask1Tasks, &count, &synced, &consumed) ==
        XTask1Error::BadVersion);

  std::vector<Sample> tooMany(51);
  auto many = buildFrame(1, tooMany);
  CHECK(parseXTask1(many.data(), many.size(), views, kMaxXTask1Tasks, &count, &synced, &consumed) ==
        XTask1Error::TooManyTasks);

  Sample longTitle{"i", std::string(81, 't'), "", 0, 0};
  auto titleFrame = buildFrame(1, {longTitle});
  CHECK(parseXTask1(titleFrame.data(), titleFrame.size(), views, kMaxXTask1Tasks, &count, &synced, &consumed) ==
        XTask1Error::FieldTooLong);

  Sample maxDesc{"i", "t", std::string(kMaxXTask1Desc, 'd'), 0, kXTask1Today};
  auto maxFrame = buildFrame(7, {maxDesc});
  CHECK(parseXTask1(maxFrame.data(), maxFrame.size(), views, kMaxXTask1Tasks, &count, &synced, &consumed) ==
        XTask1Error::Ok);
  CHECK(views[0].descLen == kMaxXTask1Desc);

  Sample overDesc{"i", "t", std::string(kMaxXTask1Desc + 1, 'd'), 0, 0};
  auto overFrame = buildFrame(7, {overDesc});
  CHECK(parseXTask1(overFrame.data(), overFrame.size(), views, kMaxXTask1Tasks, &count, &synced, &consumed) ==
        XTask1Error::FieldTooLong);

  const uint8_t junk[] = {'X', 'T', 'A', 'S', 'K', '1', 0, 1};
  CHECK(parseXTask1(junk, sizeof(junk), views, kMaxXTask1Tasks, &count, &synced, &consumed) == XTask1Error::BadMagic);
}

void testSerialFraming() {
  FeishuSerialRx rx;
  const std::string note = std::string("pay\nnext\np") + "\n" + std::string(40, 'n');
  const auto frame = buildFrame(99, {{"ab", "\xE4\xB9\xB0\xE7\x89\x9B\xE5\xA5\xB6", note, 0, kXTask1Today}});

  auto turns = feedStr(rx, "n\n");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::PageNext);
  turns = feedStr(rx, "P");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::PagePrev);

  turns = feedStr(rx, "hello\n");
  CHECK(turns.empty());
  turns = feedStr(rx, "n");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::PageNext);

  // A line that merely starts with p is still a page turn, matching the reader.
  turns = feedStr(rx, "plan\n");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::PagePrev);

  std::string buried(80, 'x');
  buried += "nNp\n";
  turns = feedStr(rx, buried.c_str());
  CHECK(turns.empty());

  turns = feedStr(rx, "TASKBUSY\n");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::Busy);

  FeishuRxResult fail = rx.feed('T', 5000);
  const char* failLine = "ASKFAIL disk full\n";
  for (size_t i = 0; failLine[i] != '\0'; ++i) {
    fail = rx.feed(static_cast<uint8_t>(failLine[i]), 5000 + static_cast<uint32_t>(i));
  }
  CHECK(fail.event == FeishuRxEvent::Fail);
  CHECK(std::strcmp(fail.message, "disk full") == 0);

  std::vector<uint8_t> pushed;
  const char* push = "TASKPUSH\n\n";
  pushed.insert(pushed.end(), push, push + std::strlen(push));
  pushed.insert(pushed.end(), frame.begin(), frame.end());
  int frameOk = 0;
  int pages = 0;
  FeishuRxResult last{};
  for (size_t i = 0; i < pushed.size(); ++i) {
    last = rx.feed(pushed[i], 8000 + static_cast<uint32_t>(i));
    if (last.event == FeishuRxEvent::FrameOk) {
      frameOk++;
    }
    if (last.event == FeishuRxEvent::PageNext || last.event == FeishuRxEvent::PagePrev) {
      pages++;
    }
  }
  CHECK(frameOk == 1);
  CHECK(pages == 0);
  CHECK(last.event == FeishuRxEvent::FrameOk);
  CHECK(rx.taskCount() == 1);
  CHECK(rx.syncedAtUnix() == 99);
  CHECK(rx.tasks()[0].descLen == note.size());
  CHECK(std::memcmp(rx.tasks()[0].description, note.data(), note.size()) == 0);

  turns = feedStr(rx, "N");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::PageNext);

  rx.setPageTurnEnabled(false);
  turns = feedStr(rx, "np");
  CHECK(turns.empty());
  rx.setPageTurnEnabled(true);
  turns = feedStr(rx, "p");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::PagePrev);
}

void testBadFrameThenPageTurn() {
  FeishuSerialRx rx;
  auto frame = buildFrame(1, {{"i", "t", "n", 0, 0}});
  frame.back() ^= 0x5A;
  auto events = feedAll(rx, frame.data(), frame.size());
  CHECK(events.size() == 1);
  CHECK(events[0] == FeishuRxEvent::FrameBad);
  CHECK(rx.taskCount() == 0);

  auto turns = feedStr(rx, "p");
  CHECK(turns.size() == 1);
  CHECK(turns[0] == FeishuRxEvent::PagePrev);

  rx.reset();
  const uint8_t partialMagic[] = {'X', 'T', 'A', 'S', 'K', '1', 0, 0, 0x01};
  for (uint8_t byte : partialMagic) {
    CHECK(rx.feed(byte, 100).event == FeishuRxEvent::None);
  }
  const FeishuRxResult timed = rx.tick(100 + 3000);
  CHECK(timed.event == FeishuRxEvent::FrameBad);
  CHECK(std::strcmp(timed.message, "同步超时") == 0);
  auto after = feedStr(rx, "n", 9000);
  CHECK(after.size() == 1);
  CHECK(after[0] == FeishuRxEvent::PageNext);

  rx.reset();
  CHECK(rx.feed('X', 10).event == FeishuRxEvent::None);
  CHECK(rx.tick(10 + 5000).event == FeishuRxEvent::None);
  auto recovered = feedStr(rx, "n", 20);
  CHECK(recovered.size() == 1);
  CHECK(recovered[0] == FeishuRxEvent::PageNext);
}

}  // namespace

int main() {
  testCrcAndDue();
  testRoundTrip();
  testRejects();
  testSerialFraming();
  testBadFrameThenPageTurn();
  if (g_fails != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fails);
    return 1;
  }
  std::printf("xtask1 tests passed\n");
  return 0;
}
