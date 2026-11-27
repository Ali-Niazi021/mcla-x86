// mcla-x86 host overlay: just enough JSON to update the launcher's settings file without disturbing it.
//
// The launcher keeps its settings in %LOCALAPPDATA%\mcla_x86\launcher.json:
//
//   { "version": 1, "values": { "<setting id>": <value>, ... } }          (launcher/src/core/settings_store.h)
//
// and builds the game's command line from it on every launch. The overlay's "Save" has to change that file for a setting to
// stick (a command-line value outranks the game's own config file), but it must not lose anything it does not understand: other
// top-level keys, values of ids this game version does not know (the launcher itself carries those), the version number.
// So: a small parser into a tree that keeps key order and the text of every number, and a writer that prints it back.
// Pure C++ (no SDK, no Windows): mcla/tests/test_overlay.cpp compiles it alone.
//
//   Parse()                  text -> Value. False and *error on malformed input (nothing is guessed at).
//   Serialize()              Value -> text, two-space indent, UTF-8 passed through, control characters escaped.
//   MergeLauncherValues()    existing file text + updates -> new file text: `values.<id>` replaced or added, everything
//                            else kept exactly (an updated id keeps its place in the file). A text that is not a JSON object,
//                            or whose "values" is not an object, is an error and nothing is produced.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcla_overlay::json {

struct Value {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  std::string text;  // kNumber: the number as written; kString: the decoded string
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;  // in file order

  static Value Null() { return Value{}; }
  static Value Bool(bool v) {
    Value r;
    r.type = Type::kBool;
    r.boolean = v;
    return r;
  }
  // `literal` must be a valid JSON number as text ("120", "0.75", "-1").
  static Value Number(std::string literal) {
    Value r;
    r.type = Type::kNumber;
    r.text = std::move(literal);
    return r;
  }
  static Value String(std::string s) {
    Value r;
    r.type = Type::kString;
    r.text = std::move(s);
    return r;
  }
  static Value Object() {
    Value r;
    r.type = Type::kObject;
    return r;
  }

  Value* Find(std::string_view key) {
    for (auto& entry : object) {
      if (entry.first == key) return &entry.second;
    }
    return nullptr;
  }
  const Value* Find(std::string_view key) const {
    for (const auto& entry : object) {
      if (entry.first == key) return &entry.second;
    }
    return nullptr;
  }
  // Replaces the value of `key`, or appends it.
  void Set(std::string key, Value v) {
    if (Value* existing = Find(key)) {
      *existing = std::move(v);
      return;
    }
    object.emplace_back(std::move(key), std::move(v));
  }
};

namespace detail {

class Parser {
 public:
  Parser(std::string_view text, std::string* error) : s_(text), error_(error) {}

  bool Run(Value* out) {
    SkipSpace();
    if (!ParseValue(out, 0)) return false;
    SkipSpace();
    if (pos_ != s_.size()) return Fail("text after the end of the value");
    return true;
  }

 private:
  static constexpr int kMaxDepth = 64;

  bool Fail(const char* what) {
    if (error_ && error_->empty()) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "JSON error at byte %zu: %s", pos_, what);
      *error_ = buf;
    }
    return false;
  }

  void SkipSpace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }

  bool Literal(std::string_view word) {
    if (s_.compare(pos_, word.size(), word) != 0) return false;
    pos_ += word.size();
    return true;
  }

  bool ParseValue(Value* out, int depth) {
    if (depth > kMaxDepth) return Fail("nested too deep");
    if (pos_ >= s_.size()) return Fail("unexpected end");
    const char c = s_[pos_];
    if (c == '{') return ParseObject(out, depth);
    if (c == '[') return ParseArray(out, depth);
    if (c == '"') {
      out->type = Value::Type::kString;
      return ParseString(&out->text);
    }
    if (c == 't') {
      if (!Literal("true")) return Fail("bad literal");
      *out = Value::Bool(true);
      return true;
    }
    if (c == 'f') {
      if (!Literal("false")) return Fail("bad literal");
      *out = Value::Bool(false);
      return true;
    }
    if (c == 'n') {
      if (!Literal("null")) return Fail("bad literal");
      *out = Value::Null();
      return true;
    }
    return ParseNumber(out);
  }

  bool ParseNumber(Value* out) {
    const size_t start = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    size_t digits = 0;
    while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
      ++pos_;
      ++digits;
    }
    if (digits == 0) return Fail("a number was expected");
    if (pos_ < s_.size() && s_[pos_] == '.') {
      ++pos_;
      size_t fraction = 0;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
        ++pos_;
        ++fraction;
      }
      if (fraction == 0) return Fail("digits expected after the decimal point");
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      size_t exponent = 0;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
        ++pos_;
        ++exponent;
      }
      if (exponent == 0) return Fail("digits expected in the exponent");
    }
    *out = Value::Number(std::string(s_.substr(start, pos_ - start)));
    return true;
  }

  static void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
      out->push_back(char(cp));
    } else if (cp < 0x800) {
      out->push_back(char(0xC0 | (cp >> 6)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(char(0xE0 | (cp >> 12)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(char(0xF0 | (cp >> 18)));
      out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(uint32_t* out) {
    if (pos_ + 4 > s_.size()) return Fail("short \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[pos_ + size_t(i)];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return Fail("bad \\u escape");
    }
    pos_ += 4;
    *out = v;
    return true;
  }

  bool ParseString(std::string* out) {
    out->clear();
    ++pos_;  // the opening quote
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in a string");
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) break;
      const char e = s_[pos_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // a surrogate pair
            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return Fail("lone surrogate");
            pos_ += 2;
            uint32_t low = 0;
            if (!Hex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return Fail("bad surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("lone surrogate");
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("bad escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseArray(Value* out, int depth) {
    out->type = Value::Type::kArray;
    ++pos_;  // [
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      Value element;
      SkipSpace();
      if (!ParseValue(&element, depth + 1)) return false;
      out->array.push_back(std::move(element));
      SkipSpace();
      if (pos_ >= s_.size()) return Fail("unterminated array");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return Fail("',' or ']' expected");
    }
  }

  bool ParseObject(Value* out, int depth) {
    out->type = Value::Type::kObject;
    ++pos_;  // {
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      SkipSpace();
      if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("a key was expected");
      std::string key;
      if (!ParseString(&key)) return false;
      SkipSpace();
      if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("':' expected");
      ++pos_;
      SkipSpace();
      Value member;
      if (!ParseValue(&member, depth + 1)) return false;
      out->object.emplace_back(std::move(key), std::move(member));
      SkipSpace();
      if (pos_ >= s_.size()) return Fail("unterminated object");
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return Fail("',' or '}' expected");
    }
  }

  std::string_view s_;
  size_t pos_ = 0;
  std::string* error_;
};

inline void AppendString(std::string* out, const std::string& s) {
  out->push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
          out->append(buf);
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

inline void Write(std::string* out, const Value& v, int indent, int level) {
  auto newline = [&](int l) {
    out->push_back('\n');
    out->append(size_t(l * indent), ' ');
  };
  switch (v.type) {
    case Value::Type::kNull: out->append("null"); break;
    case Value::Type::kBool: out->append(v.boolean ? "true" : "false"); break;
    case Value::Type::kNumber: out->append(v.text); break;
    case Value::Type::kString: AppendString(out, v.text); break;
    case Value::Type::kArray:
      if (v.array.empty()) {
        out->append("[]");
        break;
      }
      out->push_back('[');
      for (size_t i = 0; i < v.array.size(); ++i) {
        newline(level + 1);
        Write(out, v.array[i], indent, level + 1);
        if (i + 1 < v.array.size()) out->push_back(',');
      }
      newline(level);
      out->push_back(']');
      break;
    case Value::Type::kObject:
      if (v.object.empty()) {
        out->append("{}");
        break;
      }
      out->push_back('{');
      for (size_t i = 0; i < v.object.size(); ++i) {
        newline(level + 1);
        AppendString(out, v.object[i].first);
        out->append(": ");
        Write(out, v.object[i].second, indent, level + 1);
        if (i + 1 < v.object.size()) out->push_back(',');
      }
      newline(level);
      out->push_back('}');
      break;
  }
}

}  // namespace detail

inline bool Parse(std::string_view text, Value* out, std::string* error) {
  if (error) error->clear();
  // A UTF-8 byte order mark is not JSON, but a file edited by hand may have one.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.remove_prefix(3);
  }
  Value result;
  detail::Parser parser(text, error);
  if (!parser.Run(&result)) return false;
  *out = std::move(result);
  return true;
}

inline std::string Serialize(const Value& v, int indent = 2) {
  std::string out;
  detail::Write(&out, v, indent, 0);
  out.push_back('\n');
  return out;
}

// The launcher's file with `updates` (setting id -> value) applied under "values". An empty `existing_text` is NOT accepted (the
// caller creates nothing: the launcher's file belongs to the launcher). On failure `*error` says why and `*out_text` is untouched.
inline bool MergeLauncherValues(std::string_view existing_text, const std::vector<std::pair<std::string, Value>>& updates,
                                std::string* out_text, std::string* error) {
  Value root;
  std::string parse_error;
  if (!Parse(existing_text, &root, &parse_error)) {
    if (error) *error = "launcher.json does not parse: " + parse_error;
    return false;
  }
  if (root.type != Value::Type::kObject) {
    if (error) *error = "launcher.json is not a JSON object";
    return false;
  }
  Value* values = root.Find("values");
  if (!values) {
    root.object.emplace_back("values", Value::Object());
    values = &root.object.back().second;
  }
  if (values->type != Value::Type::kObject) {
    if (error) *error = "launcher.json: \"values\" is not an object";
    return false;
  }
  for (const auto& update : updates) {
    values->Set(update.first, update.second);
  }
  *out_text = Serialize(root);
  return true;
}

}  // namespace mcla_overlay::json
