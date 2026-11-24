// mcla_rage_fatal: say what the game's fatal-error function was asked to print (HOOKS_SPEC B1).
//
// Site 0x821BD618, AFTER its first instruction (`mflr r12`): sub_821BD618 is RAGE's never-returning
// fatal (a Quitf-like printf): r3 = printf format, r4..r6 = the first arguments, r12 = the return
// address. 212 `bl` and 8 `b` sites call it. It loads a handler from 0x8285FEA0 and calls it, then
// spins forever; the game's main points the handler at a routine that ends in the import
// XamShowDirtyDiscErrorUI, which our SDK answers with a generic "disc read error" and exit(1)
// (SDK patch 0205 now flushes the log and says "[xam] dirty-disc error UI requested"). Without this
// hook a RAGE fatal on iOS looks like a silent crash.
//
// The hook formats the guest printf string from r4..r6 (the integer arguments the ABI puts in
// registers; later arguments and floating point arguments are shown as "?"), logs
//     [mcla-fatal] <message> (caller 0x<bl address>)
//     [mcla-fatal] raw: fmt=0x.. r4=0x.. r5=0x.. r6=0x.. lr=0x..
// flushes the log, and lets the function carry on (its callers assume it never returns).
// Every guest pointer is validated (mcla_guest.h): a bad format pointer or a %s argument that is
// not a string is printed as <bad 0x...>; the hook cannot fault.
//
// Off: --mcla_rage_fatal=false, or MCLA_HOOK_RAGE_FATAL=0 at build time.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "nfsmw_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "mcla_guest.h"
#include "mcla_hooks.h"

REXCVAR_DEFINE_BOOL(mcla_rage_fatal, true, "MCLA",
                    "MCLA: log the message of the RAGE fatal-error function (caller, formatted text) "
                    "before the process exits");

namespace {

mcla::hooks::Registration g_registration("fatal", "mcla_rage_fatal", MCLA_HOOK_RAGE_FATAL != 0,
                                         [] { return REXCVAR_GET(mcla_rage_fatal); });

std::atomic<bool> g_fatal_seen{false};

}  // namespace

namespace mcla {
bool FatalSeen() { return g_fatal_seen.load(std::memory_order_relaxed); }
}  // namespace mcla

#if MCLA_HOOK_RAGE_FATAL

namespace {

constexpr size_t kMaxMessage = 512;
constexpr size_t kMaxFormat = 256;
constexpr size_t kMaxString = 120;
constexpr int kMaxLogged = 16;

// printf, from the first three integer arguments only. Supports flags -0+ #, a width and precision
// of up to two digits, the length modifiers h l ll z I64, and d i u x X o c s p %. A conversion
// that needs a floating point register (f e g a) prints "<float>" and every later argument is
// shown as "?" (the register slots of mixed arguments are not known). `*` widths stop the format.
std::string FormatGuestPrintf(const char* format, const uint64_t args[3]) {
  std::string out;
  size_t next_arg = 0;
  bool args_known = true;

  auto take = [&](bool* known) -> uint64_t {
    if (!args_known || next_arg >= 3) {
      *known = false;
      return 0;
    }
    *known = true;
    return args[next_arg++];
  };

  for (const char* p = format; *p && out.size() < kMaxMessage; ++p) {
    if (*p != '%') {
      out.push_back(*p);
      continue;
    }
    ++p;
    if (*p == '%') {
      out.push_back('%');
      continue;
    }
    std::string spec = "%";
    for (; *p && std::string_view("-+ #0").find(*p) != std::string_view::npos; ++p) {
      spec.push_back(*p);
    }
    for (int digits = 0; *p >= '0' && *p <= '9'; ++p) {
      if (++digits <= 2) spec.push_back(*p);
    }
    if (*p == '.') {
      spec.push_back(*p++);
      for (int digits = 0; *p >= '0' && *p <= '9'; ++p) {
        if (++digits <= 2) spec.push_back(*p);
      }
    }
    if (*p == '*') {
      out += "<*>";
      break;
    }
    bool wide = false;
    for (;;) {
      if (*p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'L') {
        ++p;
      } else if (*p == 'l') {
        ++p;
        if (*p == 'l') {
          wide = true;
          ++p;
        }
      } else if (*p == 'I' && p[1] == '6' && p[2] == '4') {
        wide = true;
        p += 3;
      } else if (*p == 'I' && p[1] == '3' && p[2] == '2') {
        p += 3;
      } else {
        break;
      }
    }
    const char conversion = *p;
    if (conversion == 0) {
      out += spec;
      break;
    }
    char piece[160];
    bool known = false;
    switch (conversion) {
      case 'd':
      case 'i': {
        const uint64_t value = take(&known);
        if (!known) {
          out += "?";
          break;
        }
        spec += wide ? "lld" : "d";
        if (wide) {
          std::snprintf(piece, sizeof(piece), spec.c_str(), static_cast<long long>(value));
        } else {
          std::snprintf(piece, sizeof(piece), spec.c_str(), static_cast<int>(static_cast<int32_t>(value)));
        }
        out += piece;
        break;
      }
      case 'u':
      case 'x':
      case 'X':
      case 'o': {
        const uint64_t value = take(&known);
        if (!known) {
          out += "?";
          break;
        }
        if (wide) {
          spec += "ll";
          spec.push_back(conversion);
          std::snprintf(piece, sizeof(piece), spec.c_str(), static_cast<unsigned long long>(value));
        } else {
          spec.push_back(conversion);
          std::snprintf(piece, sizeof(piece), spec.c_str(), static_cast<unsigned>(static_cast<uint32_t>(value)));
        }
        out += piece;
        break;
      }
      case 'p': {
        const uint64_t value = take(&known);
        if (!known) {
          out += "?";
          break;
        }
        std::snprintf(piece, sizeof(piece), "0x%08X", static_cast<unsigned>(static_cast<uint32_t>(value)));
        out += piece;
        break;
      }
      case 'c': {
        const uint64_t value = take(&known);
        const uint8_t c = static_cast<uint8_t>(value);
        out.push_back(known && c >= 0x20 && c < 0x7F ? char(c) : '?');
        break;
      }
      case 's': {
        const uint32_t pointer = static_cast<uint32_t>(take(&known));
        if (!known) {
          out += "?";
        } else if (pointer == 0) {
          out += "(null)";
        } else {
          char text[kMaxString + 1];
          bool complete = false;
          const int length = mcla::guest::CString(pointer, text, sizeof(text), &complete);
          if (length < 0) {
            std::snprintf(piece, sizeof(piece), "<bad 0x%08X>", pointer);
            out += piece;
          } else {
            out += text;
            if (!complete) out += "...";
          }
        }
        break;
      }
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
      case 'a':
      case 'A':
        out += "<float>";
        args_known = false;
        break;
      default:  // not a conversion we know: keep the text as it is and read on
        out += spec;
        out.push_back(conversion);
        break;
    }
  }
  if (out.size() > kMaxMessage) {
    out.resize(kMaxMessage);
    out += "...";
  }
  return out;
}

}  // namespace

void mcla_hook_rage_fatal(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,
                          PPCRegister& r12) {
  MCLA_HOOK_ENTER();
  if (!REXCVAR_GET(mcla_rage_fatal)) {
    return;
  }
  g_fatal_seen.store(true, std::memory_order_relaxed);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) >= kMaxLogged) {
    return;
  }
  const uint32_t format_address = r3.u32;
  const uint64_t args[3] = {r4.u64, r5.u64, r6.u64};
  // r12 is the return address: the call site is the `bl` before it. (For the 8 tail-call sites the
  // value is the caller's caller.)
  const uint32_t caller = r12.u32 - 4;

  char format[kMaxFormat + 1];
  bool complete = false;
  const int length = mcla::guest::CString(format_address, format, sizeof(format), &complete);
  if (length < 0) {
    REXLOG_ERROR("[mcla-fatal] <unreadable format at 0x{:08X}> (caller 0x{:08X})", format_address, caller);
  } else {
    const std::string message = FormatGuestPrintf(format, args);
    REXLOG_ERROR("[mcla-fatal] {} (caller 0x{:08X})", message, caller);
  }
  REXLOG_ERROR("[mcla-fatal] raw: fmt=0x{:08X} r4=0x{:08X} r5=0x{:08X} r6=0x{:08X} lr=0x{:08X}",
               format_address, uint32_t(r4.u32), uint32_t(r5.u32), uint32_t(r6.u32), r12.u32);
  rex::FlushLogging();
}

#else  // MCLA_HOOK_RAGE_FATAL == 0: keeps hooks.toml linking, does nothing.

void mcla_hook_rage_fatal(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}

#endif
