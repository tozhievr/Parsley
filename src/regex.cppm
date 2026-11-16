module;
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
export module parsley.regex;
export namespace parsley::regex {
struct CharMatcher {
  bool negated = false;
  bool match_any_non_nl = false;
  std::vector<std::pair<unsigned char, unsigned char>> ranges;
  std::vector<std::string> posix_classes;

  bool matches(unsigned char c) const noexcept {
    if (match_any_non_nl) {
      return c != '\n' && c != '\r';
    }
    bool hit = false;
    for (const auto& [lo, hi] : ranges) {
      if (c >= lo && c <= hi) {
        hit = true;
        break;
      }
    }
    if (!hit && c < 128) {
      for (const auto& cls : posix_classes) {
        if (cls == "space" && std::isspace(c)) {
          hit = true;
        } else if (cls == "alpha" && std::isalpha(c)) {
          hit = true;
        } else if (cls == "alnum" && std::isalnum(c)) {
          hit = true;
        } else if (cls == "digit" && std::isdigit(c)) {
          hit = true;
        } else if (cls == "upper" && std::isupper(c)) {
          hit = true;
        } else if (cls == "lower" && std::islower(c)) {
          hit = true;
        } else if (cls == "xdigit" && std::isxdigit(c)) {
          hit = true;
        } else if (cls == "blank" && (c == ' ' || c == '\t')) {
          hit = true;
        } else if (cls == "punct" && std::ispunct(c)) {
          hit = true;
        } else if (cls == "print" && std::isprint(c)) {
          hit = true;
        } else if (cls == "cntrl" && std::iscntrl(c)) {
          hit = true;
        }
        if (hit) {
          break;
        }
      }
    }
    return negated ? !hit : hit;
  }

  std::vector<unsigned char> candidate_chars() const {
    std::vector<unsigned char> out;
    for (int c = 32; c < 127; ++c) if (matches(static_cast<unsigned char>(c))) out.push_back(static_cast<unsigned char>(c));
    if (out.empty()) for (int c = 1; c < 256; ++c) if (matches(static_cast<unsigned char>(c))) out.push_back(static_cast<unsigned char>(c));
    return out;
  }
  std::array<std::uint8_t, 32> to_bitmap() const noexcept {
    std::array<std::uint8_t, 32> bits{};
    for (int ci = 0; ci < 256; ++ci) {
      if (matches(static_cast<unsigned char>(ci))) {
        bits[static_cast<std::size_t>(ci >> 3)] |= static_cast<std::uint8_t>(1u << (ci & 7));
      }
    }
    return bits;
  }
};

struct RegexAst {
  enum class Kind { Empty, Char, Seq, Alt, Repeat };
  Kind kind = Kind::Empty;
  CharMatcher matcher;
  std::vector<std::shared_ptr<RegexAst>> items;
  std::shared_ptr<RegexAst> inner;
  int min_rep = 0;
  int max_rep = -1;
};

using RegexAstPtr = std::shared_ptr<RegexAst>;

class RegexParser {
  std::string_view pat_;
  size_t pos_ = 0;
  size_t depth_ = 0;
  [[noreturn]] void fail(const char* message) const { throw std::invalid_argument(std::string(message) + " at regex byte " + std::to_string(pos_)); }
  char peek() const { return pos_ < pat_.size() ? pat_[pos_] : '\0'; }
  char get() { if (pos_ >= pat_.size()) fail("unexpected end of pattern"); return pat_[pos_++]; }
  static RegexAstPtr node(RegexAst::Kind kind) { auto n = std::make_shared<RegexAst>(); n->kind = kind; return n; }
  unsigned char escape(CharMatcher* multi, bool in_class) {
    char c = get();
    switch (c) {
      case 'n': return '\n'; case 'r': return '\r'; case 't': return '\t';
      case 'f': return '\f'; case 'v': return '\v'; case '0': return 0;
      case 'b': if (in_class) return '\b'; fail("word boundaries are unsupported");
      case 'x': {
        unsigned value = 0;
        for (int i = 0; i < 2; ++i) {
          char h = get(); int d = h >= '0' && h <= '9' ? h-'0' : h >= 'a' && h <= 'f' ? h-'a'+10 : h >= 'A' && h <= 'F' ? h-'A'+10 : -1;
          if (d < 0) fail("expected two hexadecimal digits after \\x");
          value = value * 16 + static_cast<unsigned>(d);
        }
        return static_cast<unsigned char>(value);
      }
      case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
        if (!multi) fail("character class escape cannot be a range endpoint");
        for (int i = 0; i < 256; ++i) {
          bool digit = i >= '0' && i <= '9';
          bool word = digit || (i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '_';
          bool space = i == ' ' || (i >= '\t' && i <= '\r');
          bool hit = c == 'd' || c == 'D' ? digit : c == 'w' || c == 'W' ? word : space;
          if (c == 'D' || c == 'W' || c == 'S') hit = !hit;
          if (hit) multi->ranges.emplace_back(static_cast<unsigned char>(i), static_cast<unsigned char>(i));
        }
        return 0;
      }
      default:
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '1' && c <= '9')) fail("unsupported regex escape");
        return static_cast<unsigned char>(c);
    }
  }
  unsigned char class_char() {
    unsigned char c = static_cast<unsigned char>(get());
    if (c >= 128) fail("non-ASCII character classes are unsupported; use UTF-8 literal alternatives");
    return c == '\\' ? escape(nullptr, true) : c;
  }
  RegexAstPtr atom() {
    if (++depth_ > 64) fail("regex nesting exceeds 64");
    auto n = node(RegexAst::Kind::Char);
    char c = get();
    if (c == '(') {
      if (peek() == '?') { ++pos_; if (get() != ':') fail("only noncapturing (?:...) groups are supported"); }
      n = alt(); if (get() != ')') fail("unclosed group");
    } else if (c == '[') {
      if (peek() == '^') { ++pos_; n->matcher.negated = true; }
      bool first = true;
      while (pos_ < pat_.size() && (first || peek() != ']')) {
        first = false;
        if (pat_.substr(pos_,2) == "[:") {
          auto close = pat_.find(":]",pos_+2); if (close == std::string_view::npos) fail("unclosed POSIX class");
          std::string cls(pat_.substr(pos_+2,close-pos_-2));
          const std::string allowed = " space alpha alnum digit upper lower xdigit blank punct print cntrl ";
          if (allowed.find(" " + cls + " ") == std::string::npos) fail("unsupported POSIX class");
          n->matcher.posix_classes.push_back(cls); pos_ = close+2; continue;
        }
        if (peek() == '\\' && pos_+1 < pat_.size() && std::string_view("dDwWsS").find(pat_[pos_+1]) != std::string_view::npos) {
          ++pos_; escape(&n->matcher,true); continue;
        }
        auto lo = class_char();
        if (peek() == '-' && pos_+1 < pat_.size() && pat_[pos_+1] != ']') {
          ++pos_; auto hi = class_char(); if (lo > hi) fail("reversed character range"); n->matcher.ranges.emplace_back(lo,hi);
        } else n->matcher.ranges.emplace_back(lo,lo);
      }
      if (get() != ']') fail("unclosed character class");
    } else if (c == '.') n->matcher.match_any_non_nl = true;
    else if (c == '\\') {
      bool multi = pos_ < pat_.size() && std::string_view("dDwWsS").find(peek()) != std::string_view::npos;
      auto ch = escape(&n->matcher,false); if (!multi) n->matcher.ranges.emplace_back(ch,ch);
    } else {
      if (std::string_view("^$*+?{})").find(c) != std::string_view::npos) fail("unsupported anchor or misplaced metacharacter");
      n->matcher.ranges.emplace_back(static_cast<unsigned char>(c), static_cast<unsigned char>(c));
    }
    --depth_; return n;
  }
  int count() {
    int value = 0; bool any = false;
    while (peek() >= '0' && peek() <= '9') { any = true; value = value * 10 + get()-'0'; if (value > 1024) fail("repeat bound exceeds 1024"); }
    if (!any) fail("expected repeat bound"); return value;
  }
  RegexAstPtr quantified() {
    auto n = atom(); char c = peek();
    if (c == '*' || c == '+' || c == '?' || c == '{') {
      ++pos_; auto r = node(RegexAst::Kind::Repeat); r->inner = n;
      if (c == '*') { r->min_rep = 0; r->max_rep = -1; }
      if (c == '+') { r->min_rep = 1; r->max_rep = -1; }
      if (c == '?') { r->min_rep = 0; r->max_rep = 1; }
      if (c == '{') {
        r->min_rep = count(); r->max_rep = r->min_rep;
        if (peek() == ',') { ++pos_; r->max_rep = peek() == '}' ? -1 : count(); }
        if (get() != '}' || (r->max_rep >= 0 && r->max_rep < r->min_rep)) fail("invalid repeat bounds");
      }
      if (std::string_view("*+?{").find(peek()) != std::string_view::npos) fail("lazy, possessive and repeated quantifiers are unsupported");
      n = r;
    }
    return n;
  }
  RegexAstPtr seq() {
    auto n = node(RegexAst::Kind::Seq);
    while (pos_ < pat_.size() && peek() != ')' && peek() != '|') n->items.push_back(quantified());
    if (n->items.empty()) n->kind = RegexAst::Kind::Empty;
    return n;
  }
  RegexAstPtr alt() {
    auto n = seq();
    if (peek() != '|') return n;
    auto a = node(RegexAst::Kind::Alt); a->items.push_back(n);
    while (peek() == '|') { ++pos_; a->items.push_back(seq()); } return a;
  }
  size_t cost(const RegexAstPtr& n) {
    size_t v = 1;
    for (const auto& item : n->items) { v += cost(item); if (v > 4096) fail("NFA state budget exceeds 4096"); }
    if (n->inner) v += cost(n->inner) * static_cast<size_t>(n->max_rep < 0 ? n->min_rep + 2 : n->max_rep + 1);
    if (v > 4096) fail("NFA state budget exceeds 4096"); return v;
  }
public:
  explicit RegexParser(std::string_view pattern) : pat_(pattern) {}
  RegexAstPtr parse() {
    if (pat_.size() > 8192) fail("regex exceeds 8192 bytes");
    auto n = alt(); if (pos_ != pat_.size()) fail("unexpected trailing pattern"); cost(n); return n;
  }
};
struct NfaState {
  enum class Kind {
    Match,
    Split,
    Char,
  };
  Kind kind = Kind::Match;
  CharMatcher matcher;
  int out1 = -1;
  int out2 = -1;
};

struct CompiledRegex {
  RegexAstPtr ast;
  std::vector<NfaState> states;
  int start = 0;

  static CompiledRegex compile(std::string_view pattern) {
    CompiledRegex cr;
    RegexParser parser(pattern);
    cr.ast = parser.parse();
    int match_id = cr.add_state(NfaState{NfaState::Kind::Match, {}, -1, -1});
    cr.start = cr.emit_ast(cr.ast, match_id);
    return cr;
  }

  std::optional<std::size_t> match_prefix(std::string_view input, std::size_t offset) const {
    std::vector<int> cur_states;
    std::vector<int> next_states;
    std::vector<int> seen(states.size(), 0);
    int mark = 1;

    auto add_eps = [&](auto& self, int s, std::vector<int>& list, int stamp) -> void {
      if (s < 0 || seen[static_cast<std::size_t>(s)] == stamp) {
        return;
      }
      seen[static_cast<std::size_t>(s)] = stamp;
      const auto& st = states[static_cast<std::size_t>(s)];
      if (st.kind == NfaState::Kind::Split) {
        self(self, st.out1, list, stamp);
        self(self, st.out2, list, stamp);
      } else {
        list.push_back(s);
      }
    };

    add_eps(add_eps, start, cur_states, mark++);

    std::optional<std::size_t> best;
    for (int s : cur_states) {
      if (states[static_cast<std::size_t>(s)].kind == NfaState::Kind::Match) {
        best = 0;
        break;
      }
    }

    for (std::size_t i = offset; i < input.size() && !cur_states.empty(); ++i) {
      unsigned char ch = static_cast<unsigned char>(input[i]);
      next_states.clear();
      int step_mark = mark++;
      for (int s : cur_states) {
        const auto& st = states[static_cast<std::size_t>(s)];
        if (st.kind == NfaState::Kind::Char && st.matcher.matches(ch)) {
          add_eps(add_eps, st.out1, next_states, step_mark);
        }
      }
      cur_states.swap(next_states);
      for (int s : cur_states) {
        if (states[static_cast<std::size_t>(s)].kind == NfaState::Kind::Match) {
          best = (i - offset) + 1;
          break;
        }
      }
    }
    return best;
  }

private:
  int add_state(NfaState st) {
    int id = static_cast<int>(states.size());
    states.push_back(std::move(st));
    return id;
  }

  int emit_ast(const RegexAstPtr& node, int next_state) {
    if (!node) {
      return next_state;
    }
    switch (node->kind) {
      case RegexAst::Kind::Empty:
        return next_state;
      case RegexAst::Kind::Char:
        return add_state(NfaState{NfaState::Kind::Char, node->matcher, next_state, -1});
      case RegexAst::Kind::Seq: {
        int cur = next_state;
        for (std::size_t i = node->items.size(); i > 0; --i) {
          cur = emit_ast(node->items[i - 1], cur);
        }
        return cur;
      }
      case RegexAst::Kind::Alt: {
        if (node->items.empty()) {
          return next_state;
        }
        std::vector<int> starts;
        starts.reserve(node->items.size());
        for (const auto& it : node->items) {
          starts.push_back(emit_ast(it, next_state));
        }
        int head = starts.back();
        for (std::size_t i = starts.size() - 1; i > 0; --i) {
          head = add_state(NfaState{NfaState::Kind::Split, {}, starts[i - 1], head});
        }
        return head;
      }
      case RegexAst::Kind::Repeat: {
        int tail = next_state;
        if (node->max_rep < 0) {
          int split_id = add_state(NfaState{NfaState::Kind::Split, {}, -1, tail});
          int body_start = emit_ast(node->inner, split_id);
          states[static_cast<std::size_t>(split_id)].out1 = body_start;
          tail = split_id;
        } else {
          for (int k = node->max_rep; k > node->min_rep; --k) {
            int body_start = emit_ast(node->inner, tail);
            tail = add_state(NfaState{NfaState::Kind::Split, {}, body_start, tail});
          }
        }
        for (int k = 0; k < node->min_rep; ++k) {
          tail = emit_ast(node->inner, tail);
        }
        return tail;
      }
    }
    return next_state;
  }
};

}
