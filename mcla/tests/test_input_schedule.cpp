// Unit test of the schedule steps of the host's input script (src/host/input_script.h): `set <cvar>=<value>`,
// `burn <n>`, `burnmem <n>`, `burngpu <pct>` and `mark <label>`, parsed from the same script as the pad steps. Valid and invalid
// spellings, that the pad steps vector never sees a schedule step (StateAt / PressesBy), the stable time order, the
// file-text comments, and the canonical-state label of a segment (SchedState). Standalone: header only, no SDK, no
// game.
//
// Not wired into CMake (mcla/tests/CMakeLists.txt). By hand:
//   clang++ -std=c++23 -O1 -Wall -Wextra -I src/host mcla/tests/test_input_schedule.cpp \
//       -o <scratch>/test_input_schedule && <scratch>/test_input_schedule
#include <cstdio>
#include <string>
#include <vector>

#include "input_script.h"

namespace m = mcla_host;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      ++g_failures;                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

#define CHECK_STR(got, want)                                                                           \
  do {                                                                                                 \
    const std::string got_ = (got), want_ = (want);                                                    \
    ++g_checks;                                                                                        \
    if (got_ != want_) {                                                                               \
      ++g_failures;                                                                                    \
      std::printf("FAIL %s:%d: %s: got '%s', want '%s'\n", __FILE__, __LINE__, #got, got_.c_str(),     \
                  want_.c_str());                                                                      \
    }                                                                                                  \
  } while (0)

struct Parsed {
  bool ok = false;
  std::string err;
  std::vector<m::Step> pad;
  std::vector<m::SchedStep> sched;
};

Parsed Parse(const std::string& script) {
  Parsed p;
  p.ok = m::ParseScript(script, p.pad, p.err, &p.sched);
  return p;
}

// One schedule step that must parse, and nothing else came out of it.
m::SchedStep One(const std::string& step) {
  const Parsed p = Parse(step);
  CHECK(p.ok);
  CHECK(p.pad.empty());
  CHECK(p.sched.size() == 1);
  if (!p.ok || p.sched.size() != 1) {
    std::printf("  (step '%s': %s)\n", step.c_str(), p.err.c_str());
    return {};
  }
  return p.sched[0];
}

void Bad(const std::string& script) {
  const Parsed p = Parse(script);
  if (p.ok) std::printf("  (accepted: '%s')\n", script.c_str());
  CHECK(!p.ok);
  CHECK(!p.err.empty());
}

void TestSet() {
  const m::SchedStep s = One("12.5:set mcla_gfx_preset=perf");
  CHECK(s.kind == m::SchedStep::kSet);
  CHECK(s.at_s == 12.5);
  CHECK_STR(s.name, "mcla_gfx_preset");
  CHECK_STR(s.value, "perf");
  CHECK_STR(s.text, "12.5:set mcla_gfx_preset=perf");

  // Keyword in any case, any whitespace after it; the name keeps its case, the value is verbatim.
  CHECK_STR(One("5:SET Foo_1=Bar").name, "Foo_1");
  CHECK_STR(One("5:SeT Foo_1=Bar").value, "Bar");
  CHECK_STR(One("5:set\tx=1").name, "x");
  CHECK_STR(One("  5 :  set   x=1  ").value, "1");
  CHECK_STR(One("5:set x=0.25").value, "0.25");
  CHECK_STR(One("5:set x=-3").value, "-3");
  CHECK_STR(One("5:set x=a=b").value, "a=b");        // only the first '=' splits
  CHECK_STR(One("5:set x=1/500").value, "1/500");    // no /hold on these steps: it is just part of the value
  CHECK_STR(One("5:set x=1+2").value, "1+2");

  Bad("5:set");
  Bad("5:set x");
  Bad("5:set x=");
  Bad("5:set =1");
  Bad("5:set x y=1");
  Bad("5:set x=1 y=2");
  Bad("5:set x = 1");
  Bad("5:set x= 1");
  Bad("5:set x=1#c");
  Bad("5:set x-y=1");
  Bad("5:set x.y=1");
  Bad("5:set burn=1");  // keys of the segment label
  Bad("5:set burnmem=1");
  Bad("5:set burngpu=1");
  Bad("5:set mark=a");
  Bad("5:setx=1");  // not the keyword: a pad step with an unknown button
}

void TestBurn() {
  m::SchedStep s = One("30:burn 4");
  CHECK(s.kind == m::SchedStep::kBurn);
  CHECK(s.n == 4);
  CHECK(s.at_s == 30);
  s = One("30:burnmem 0");
  CHECK(s.kind == m::SchedStep::kBurnMem);
  CHECK(s.n == 0);
  CHECK(One("1:burn 32").n == 32);
  CHECK(One("1:BURN 07").n == 7);
  CHECK(One("1:BurnMem 12").n == 12);

  Bad("1:burn");
  Bad("1:burn x");
  Bad("1:burn -1");
  Bad("1:burn +4");
  Bad("1:burn 33");
  Bad("1:burn 100");
  Bad("1:burn 4 5");
  Bad("1:burn 4/200");
  Bad("1:burn 4+1");
  Bad("1:burn 1.5");
  Bad("1:burnmem 33");
  Bad("1:burnmem");
  Bad("1:burn4");
}

void TestBurnGpu() {
  m::SchedStep s = One("30:burngpu 40");
  CHECK(s.kind == m::SchedStep::kBurnGpu);
  CHECK(s.n == 40);
  CHECK(s.at_s == 30);
  CHECK_STR(s.text, "30:burngpu 40");
  CHECK(One("70:burngpu 0").n == 0);  // 0 stops the load
  CHECK(One("1:burngpu 100").n == 100);
  CHECK(One("1:burngpu 1").n == 1);
  CHECK(One("1:BURNGPU 007").n == 7);
  CHECK(One("1:BurnGpu 50").kind == m::SchedStep::kBurnGpu);
  CHECK(One("1:burngpu\t25").n == 25);
  CHECK(One("  2.5 :  burngpu   60  ").n == 60);

  Bad("1:burngpu");
  Bad("1:burngpu x");
  Bad("1:burngpu -1");
  Bad("1:burngpu +40");
  Bad("1:burngpu 101");
  Bad("1:burngpu 999");
  Bad("1:burngpu 1000");
  Bad("1:burngpu 0100");
  Bad("1:burngpu 40%");
  Bad("1:burngpu 40.5");
  Bad("1:burngpu 40 50");
  Bad("1:burngpu 40/200");
  Bad("1:burngpu 40+1");
  Bad("1:burngpu4");
  Bad("1:burngpu=40");

  // The new keyword leaves its neighbours alone: `burn 40` is still a spinner count (0..32), and a longer word is not a keyword.
  CHECK(One("1:burn 32").kind == m::SchedStep::kBurn);
  Bad("1:burn 40");
  Bad("1:burng 40");
  Bad("1:burngpus 40");

  // With no schedule output it is an error, like the other schedule steps.
  std::vector<m::Step> pad;
  std::string err;
  CHECK(!m::ParseScript("10:burngpu 40", pad, err));
  CHECK(pad.empty());
}

void TestMark() {
  const m::SchedStep s = One("45:mark phase-2.a_b");
  CHECK(s.kind == m::SchedStep::kMark);
  CHECK_STR(s.name, "phase-2.a_b");
  CHECK_STR(One("1:MARK Start").name, "Start");
  CHECK_STR(One("1:mark " + std::string(40, 'x')).name, std::string(40, 'x'));

  Bad("1:mark");
  Bad("1:mark a b");
  Bad("1:mark a/b");
  Bad("1:mark a+b");
  Bad("1:mark a=b");
  Bad("1:mark a:b");
  Bad("1:mark a#b");
  Bad("1:mark " + std::string(41, 'x'));
}

void TestBadTimeAndPadSteps() {
  Bad("x:set a=1");
  Bad("-1:mark a");
  Bad("set a=1");  // no <seconds>: prefix

  // A pad step is untouched by the new keywords.
  Parsed p = Parse("10:a+b/300;20:rt=0.5;30:start");
  CHECK(p.ok);
  CHECK(p.pad.size() == 3);
  CHECK(p.sched.empty());
  CHECK(p.pad[0].hold_ms == 300);
  CHECK(p.pad[0].state.buttons == (m::kA | m::kB));
  Bad("10:aa");

  // With no schedule output a schedule step is an error, not a silent drop.
  std::vector<m::Step> pad;
  std::string err;
  CHECK(!m::ParseScript("10:set a=1", pad, err));
  CHECK(pad.empty());
  CHECK(m::ParseScript("10:a", pad, err));
  CHECK(pad.size() == 1);
}

void TestMixedAndOrder() {
  const Parsed p = Parse("1:a;2:set x=1;3:start/300;4:burn 4;2:mark m");
  CHECK(p.ok);
  CHECK(p.pad.size() == 2);
  CHECK(p.sched.size() == 3);

  // The pad driver never sees a schedule step: not at their time, not at any other.
  for (double t = 0; t < 6; t += 0.05) {
    const m::PadState st = m::StateAt(p.pad, t);
    const bool a_on = t >= 1.0 && t < 1.2;
    const bool start_on = t >= 3.0 && t < 3.3;
    CHECK(((st.buttons & m::kA) != 0) == a_on);
    CHECK(((st.buttons & m::kStart) != 0) == start_on);
    CHECK((st.buttons & ~(m::kA | m::kStart)) == 0);
    CHECK(st.lx == 0 && st.ly == 0 && st.rx == 0 && st.ry == 0 && st.lt == 0 && st.rt == 0);
  }
  CHECK(m::StateAt(p.pad, 2.05) == m::PadState());
  CHECK(m::StateAt(p.pad, 4.05) == m::PadState());
  CHECK(m::PressesBy(p.pad[0], 0.5) == 0);
  CHECK(m::PressesBy(p.pad[0], 5.0) == 1);

  // Time order, and file order for equal times (std::stable_sort).
  const Parsed q = Parse("5:set a=1;3:mark m;5:set b=2;5:burn 1;1:burnmem 2;5:burngpu 40;5:mark z;0:set c=3;2:burngpu 0");
  CHECK(q.ok);
  const char* want[] = {"0:set c=3", "1:burnmem 2", "2:burngpu 0",  "3:mark m",  "5:set a=1",
                        "5:set b=2", "5:burn 1",    "5:burngpu 40", "5:mark z"};
  CHECK(q.sched.size() == 9);
  for (size_t i = 0; i < q.sched.size() && i < 9; ++i) CHECK_STR(q.sched[i].text, want[i]);

  // Several ParseScript calls on one vector (the order holds across them too).
  std::vector<m::Step> pad;
  std::vector<m::SchedStep> sched;
  std::string err;
  CHECK(m::ParseScript("9:mark late", pad, err, &sched));
  CHECK(m::ParseScript("2:mark early", pad, err, &sched));
  CHECK(sched.size() == 2 && sched[0].name == "early" && sched[1].name == "late");

  // A schedule-only script: no pad steps at all.
  const Parsed r = Parse("10:mark a;20:burn 2");
  CHECK(r.ok && r.pad.empty() && r.sched.size() == 2);
}

void TestFileText() {
  const std::string text =
      "# expect: nothing\n"
      "# seconds: 90\n"
      "10:set mcla_gfx_preset=perf   # switch to perf\n"
      "\n"
      "   20:burn 4 ; 30:mark go\r\n"
      "35:burngpu 50   # foreign GPU load\n"
      "40:start\n";
  const Parsed p = Parse(m::ScriptFromFileText(text));
  CHECK(p.ok);
  CHECK(p.pad.size() == 1);
  CHECK(p.sched.size() == 4);
  if (p.sched.size() == 4) {
    CHECK_STR(p.sched[0].value, "perf");
    CHECK(p.sched[1].n == 4);
    CHECK_STR(p.sched[2].name, "go");
    CHECK(p.sched[3].kind == m::SchedStep::kBurnGpu && p.sched[3].n == 50);
  }
}

void TestSchedState() {
  const Parsed p = Parse("1:set mcla_b=2;2:burn 4;3:mark m1;4:set mcla_a=1;5:set mcla_b=3;6:burnmem 1");
  CHECK(p.ok);
  m::SchedState st(p.sched);
  CHECK(st.cvars().size() == 2);
  CHECK_STR(st.cvars()[0], "mcla_a");
  CHECK_STR(st.cvars()[1], "mcla_b");
  st.SetCvar("mcla_a", "x");
  st.SetCvar("mcla_b", "y");
  CHECK_STR(st.Label(), "burn=0 burnmem=0 mark=start mcla_a=x mcla_b=y");  // sorted by name, byte order
  st.SetBurn(4);
  st.SetMark("m1");
  st.SetCvar("mcla_a", "1");
  CHECK_STR(st.Label(), "burn=4 burnmem=0 mark=m1 mcla_a=1 mcla_b=y");

  // Only the keys the schedule uses; the example of the docs.
  const Parsed q = Parse("1:burn 4;2:set mcla_gfx_preset=perf");
  m::SchedState s2(q.sched);
  s2.SetCvar("mcla_gfx_preset", "quality");
  CHECK_STR(s2.Label(), "burn=0 mcla_gfx_preset=quality");
  s2.SetBurn(4);
  s2.SetCvar("mcla_gfx_preset", "perf");
  CHECK_STR(s2.Label(), "burn=4 mcla_gfx_preset=perf");

  // burngpu is its own key, sorted between burn and burnmem ('g' < 'm').
  const Parsed g = Parse("1:burngpu 40;2:burn 4;3:burnmem 1;4:mark a;5:set mcla_x=1");
  CHECK(g.ok);
  m::SchedState s3(g.sched);
  s3.SetCvar("mcla_x", "0");
  CHECK_STR(s3.Label(), "burn=0 burngpu=0 burnmem=0 mark=start mcla_x=0");
  s3.SetBurnGpu(40);
  CHECK_STR(s3.Label(), "burn=0 burngpu=40 burnmem=0 mark=start mcla_x=0");
  s3.SetBurnGpu(0);
  s3.SetBurn(2);
  CHECK_STR(s3.Label(), "burn=2 burngpu=0 burnmem=0 mark=start mcla_x=0");
  const Parsed g2 = Parse("1:burngpu 25");
  m::SchedState s4(g2.sched);
  CHECK_STR(s4.Label(), "burngpu=0");
  s4.SetBurnGpu(25);
  CHECK_STR(s4.Label(), "burngpu=25");

  // No keys, no label.
  CHECK_STR(m::SchedState(std::vector<m::SchedStep>()).Label(), "");
}

}  // namespace

int main() {
  TestSet();
  TestBurn();
  TestBurnGpu();
  TestMark();
  TestBadTimeAndPadSteps();
  TestMixedAndOrder();
  TestFileText();
  TestSchedState();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
