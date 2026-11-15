module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module parsley.analysis;

import parsley.ast;
import parsley.diagnostic;
import parsley.ir;

export namespace parsley::analysis {

enum class Strategy {
  LL1,
  LLk,
  LALR1,
  PEG,
  GLL,
};

inline std::string_view strategy_name(Strategy s) noexcept {
  switch (s) {
    case Strategy::LL1:
      return "LL(1)";
    case Strategy::LLk:
      return "LL(k)";
    case Strategy::LALR1:
      return "LALR(1)";
    case Strategy::PEG:
      return "PEG";
    case Strategy::GLL:
      return "GLL";
  }
  return "LL(1)";
}

struct DerivationNode {
  std::string label;
  std::vector<DerivationNode> children;
};

struct Counterexample {
  std::string rule_id;
  std::string input_sample;
  std::vector<std::string> token_sequence;
  DerivationNode tree_a;
  DerivationNode tree_b;
  std::string description;
  std::string fix_recommendation;
  Span span;
};

struct LeftFactoringRecord {
  std::string rule_id;
  std::vector<std::string> common_prefix_tokens;
  std::size_t original_alt_count = 0;
  std::size_t factored_alt_count = 0;
  Span span;
};

struct LexerConflict {
  std::string mode_name;
  std::string token_a;
  std::string token_b;
  std::string pattern;
  Span span_a;
  Span span_b;
};

struct RuleAnalysis {
  std::string rule_id;
  bool nullable = false;
  std::set<std::string> first;
  std::set<std::string> follow;
  bool reachable = false;
  bool left_recursive = false;
  bool direct_left_recursive = false;
  bool uses_context = false;
  bool has_ll1_conflict = false;
  int required_k = 1;
  Span span;
};

struct AnalysisResult {
  Strategy selected_strategy = Strategy::LL1;
  std::string strategy_reason;
  bool has_left_recursion = false;
  bool has_context_rules = false;
  bool has_ambiguity = false;
  std::map<std::string, RuleAnalysis> rules;
  std::vector<std::string> dead_rules;
  std::vector<LeftFactoringRecord> factored_rules;
  std::vector<Counterexample> counterexamples;
  std::vector<LexerConflict> lexer_conflicts;
};

inline std::string format_derivation_tree(const DerivationNode& node, int indent = 0) {
  std::ostringstream out;
  for (int i = 0; i < indent; ++i) {
    out << "  ";
  }
  out << node.label << '\n';
  for (const auto& ch : node.children) {
    out << format_derivation_tree(ch, indent + 1);
  }
  return out.str();
}

inline std::string format_counterexample(const Counterexample& cx) {
  std::ostringstream out;
  out << "Conflict in rule `" << cx.rule_id << "`: " << cx.description << "\n";
  out << "  Counterexample input: \"" << cx.input_sample << "\"\n";
  out << "  Derivation 1:\n" << format_derivation_tree(cx.tree_a, 2);
  out << "  Derivation 2:\n" << format_derivation_tree(cx.tree_b, 2);
  if (!cx.fix_recommendation.empty()) {
    out << "  Suggested fix: " << cx.fix_recommendation << "\n";
  }
  return out.str();
}

} // namespace parsley::analysis

namespace parsley::analysis::detail {

inline std::string format_token_name(const ir::Grammar& g, const std::string& token_id) {
  if (token_id == "eof") {
    return "eof";
  }
  if (const auto* td = ir::find_token(g, token_id)) {
    if (!td->is_regex && (td->is_anonymous || td->is_keyword)) {
      return "\"" + td->pattern + "\"";
    }
  }
  return token_id;
}

inline std::string describe_prod(const ir::Grammar& g, const ir::ProdPtr& p) {
  if (!p) {
    return "eps";
  }
  switch (p->kind) {
    case ir::Prod::Kind::Tok:
      return format_token_name(g, p->token_id);
    case ir::Prod::Kind::Ref:
      return p->rule_id;
    case ir::Prod::Kind::Seq: {
      std::string out;
      for (std::size_t i = 0; i < p->items.size(); ++i) {
        if (i > 0) {
          out += " ";
        }
        out += describe_prod(g, p->items[i]);
      }
      return out.empty() ? "eps" : out;
    }
    case ir::Prod::Kind::Alt: {
      std::string out = "(";
      for (std::size_t i = 0; i < p->items.size(); ++i) {
        if (i > 0) {
          out += " | ";
        }
        out += describe_prod(g, p->items[i]);
      }
      out += ")";
      return out;
    }
    case ir::Prod::Kind::Rep:
      return "rep(" + describe_prod(g, p->inner) + ")";
    case ir::Prod::Kind::Look:
      return std::string(p->polarity == ir::Polarity::Positive ? "&" : "!") +
             describe_prod(g, p->inner);
    case ir::Prod::Kind::Act:
      if (!p->items.empty()) {
        std::string out;
        for (std::size_t i = 0; i < p->items.size(); ++i) {
          if (i > 0) {
            out += " ";
          }
          out += describe_prod(g, p->items[i]);
        }
        return out;
      }
      return "pure";
    case ir::Prod::Kind::PrecTable:
      return "operators(" + describe_prod(g, p->inner) + ")";
    case ir::Prod::Kind::Ctx:
      return p->ctx_var.empty() ? "context do" : ("exactly " + p->ctx_var);
    case ir::Prod::Kind::Label:
      return describe_prod(g, p->inner);
    case ir::Prod::Kind::Recover:
      return describe_prod(g, p->inner);
    case ir::Prod::Kind::Eps:
      return "eps";
  }
  return "eps";
}

inline bool prod_equiv(const ir::ProdPtr& a, const ir::ProdPtr& b) {
  if (!a || !b) {
    return (!a || a->kind == ir::Prod::Kind::Eps) && (!b || b->kind == ir::Prod::Kind::Eps);
  }
  if (a->kind != b->kind) {
    return false;
  }
  switch (a->kind) {
    case ir::Prod::Kind::Tok:
      return a->token_id == b->token_id;
    case ir::Prod::Kind::Ref:
      return a->rule_id == b->rule_id;
    case ir::Prod::Kind::Eps:
      return true;
    case ir::Prod::Kind::Rep:
      return a->min_rep == b->min_rep && a->max_rep == b->max_rep &&
             prod_equiv(a->inner, b->inner);
    case ir::Prod::Kind::Look:
      return a->polarity == b->polarity && prod_equiv(a->inner, b->inner);
    case ir::Prod::Kind::Label:
      return a->label == b->label && prod_equiv(a->inner, b->inner);
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Alt:
      if (a->items.size() != b->items.size()) {
        return false;
      }
      for (std::size_t i = 0; i < a->items.size(); ++i) {
        if (!prod_equiv(a->items[i], b->items[i])) {
          return false;
        }
      }
      return true;
    case ir::Prod::Kind::Act:
    case ir::Prod::Kind::PrecTable:
    case ir::Prod::Kind::Ctx:
    case ir::Prod::Kind::Recover:
      return false;
  }
  return false;
}

inline ir::ActionExpr shift_action_args(const ir::ActionExpr& act, int prefix_len) {
  ir::ActionExpr copy = act;
  if (copy.kind == ir::ActionExpr::Kind::Arg || copy.kind == ir::ActionExpr::Kind::Identity) {
    if (copy.arg_index < 0) {
      copy.arg_index -= prefix_len;
    } else if (copy.arg_index < prefix_len) {
      copy.arg_index = -prefix_len + copy.arg_index;
    } else {
      copy.arg_index -= prefix_len;
    }
  }
  for (auto& sub : copy.args) {
    sub = shift_action_args(sub, prefix_len);
  }
  return copy;
}

struct FlattenedAlt {
  std::vector<ir::ProdPtr> seq;
  bool has_action = false;
  ir::ActionExpr action;
  Span span;
  ir::ProdPtr original;
};

inline FlattenedAlt flatten_alt_item(const ir::ProdPtr& p) {
  FlattenedAlt out;
  out.original = p;
  if (!p) {
    return out;
  }
  out.span = p->span;
  if (p->kind == ir::Prod::Kind::Act && !p->items.empty()) {
    out.seq = p->items;
    out.has_action = true;
    out.action = p->action;
    return out;
  }
  if (p->kind == ir::Prod::Kind::Seq && !p->items.empty()) {
    out.seq = p->items;
    out.has_action = false;
    return out;
  }
  out.seq = {p};
  out.has_action = false;
  return out;
}

inline ir::ProdPtr build_suffix_prod(const FlattenedAlt& alt, std::size_t prefix_len) {
  std::vector<ir::ProdPtr> rem;
  if (prefix_len < alt.seq.size()) {
    rem.insert(rem.end(),
               alt.seq.begin() + static_cast<std::ptrdiff_t>(prefix_len),
               alt.seq.end());
  }
  if (alt.has_action) {
    return ir::make_act(shift_action_args(alt.action, static_cast<int>(prefix_len)),
                        std::move(rem),
                        alt.span);
  }
  if (rem.empty()) {
    return ir::make_eps(alt.span);
  }
  if (rem.size() == 1) {
    return rem[0];
  }
  return ir::make_seq(std::move(rem), alt.span);
}

// Forward declarations for sample/counterexample synthesis
inline std::string sample_token_lexeme(const ir::Grammar& g, const std::string& token_id) {
  if (token_id == "eof") {
    return "";
  }
  if (const auto* td = ir::find_token(g, token_id)) {
    if (!td->is_regex) {
      return td->pattern;
    }
    const std::string& pat = td->pattern;
    if (pat.find("0-9") != std::string::npos) {
      return "1";
    }
    if (pat.find("a-z") != std::string::npos || pat.find("A-Z") != std::string::npos) {
      return "x";
    }
    if (pat.find("[^\"]") != std::string::npos) {
      return "abc";
    }
    if (pat.find("[^<]") != std::string::npos) {
      return "text";
    }
    return "a";
  }
  return token_id;
}

inline std::vector<std::string> sample_tokens_for_prod(
    const ir::Grammar& g,
    const ir::ProdPtr& p,
    std::unordered_set<std::string>& visiting,
    int depth = 0) {
  if (!p || depth > 8) {
    return {};
  }
  switch (p->kind) {
    case ir::Prod::Kind::Eps:
      return {};
    case ir::Prod::Kind::Tok:
      if (p->token_id == "eof") {
        return {};
      }
      return {p->token_id};
    case ir::Prod::Kind::Ref: {
      if (visiting.contains(p->rule_id)) {
        return {};
      }
      auto it = g.gRules.find(p->rule_id);
      if (it == g.gRules.end()) {
        return {};
      }
      visiting.insert(p->rule_id);
      auto res = sample_tokens_for_prod(g, it->second.rBody, visiting, depth + 1);
      visiting.erase(p->rule_id);
      return res;
    }
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Act: {
      std::vector<std::string> out;
      for (const auto& item : p->items) {
        auto sub = sample_tokens_for_prod(g, item, visiting, depth + 1);
        out.insert(out.end(), sub.begin(), sub.end());
      }
      return out;
    }
    case ir::Prod::Kind::Alt: {
      std::vector<std::string> best;
      bool has_best = false;
      for (const auto& item : p->items) {
        auto sub = sample_tokens_for_prod(g, item, visiting, depth + 1);
        if (!has_best || sub.size() < best.size()) {
          best = std::move(sub);
          has_best = true;
        }
      }
      return best;
    }
    case ir::Prod::Kind::Rep: {
      if (p->min_rep == 0) {
        return {};
      }
      std::vector<std::string> out;
      for (int i = 0; i < p->min_rep; ++i) {
        auto sub = sample_tokens_for_prod(g, p->inner, visiting, depth + 1);
        out.insert(out.end(), sub.begin(), sub.end());
      }
      return out;
    }
    case ir::Prod::Kind::Look:
      return {};
    case ir::Prod::Kind::PrecTable:
      return sample_tokens_for_prod(g, p->inner, visiting, depth + 1);
    case ir::Prod::Kind::Ctx: {
      if (!p->ctx_var.empty()) {
        return {"ident"};
      }
      std::vector<std::string> out;
      for (const auto& st : p->ctx_steps) {
        auto sub = sample_tokens_for_prod(g, st.prod, visiting, depth + 1);
        out.insert(out.end(), sub.begin(), sub.end());
      }
      return out;
    }
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::Recover:
      return sample_tokens_for_prod(g, p->inner, visiting, depth + 1);
  }
  return {};
}

inline std::string join_token_samples(const ir::Grammar& g, const std::vector<std::string>& toks) {
  std::string out;
  for (std::size_t i = 0; i < toks.size(); ++i) {
    if (i > 0) {
      out.push_back(' ');
    }
    out += sample_token_lexeme(g, toks[i]);
  }
  return out;
}

inline DerivationNode build_derivation_for_prod(const ir::Grammar& g,
                                                 const ir::ProdPtr& p,
                                                 int depth = 0) {
  if (!p || depth > 4) {
    return DerivationNode{"...", {}};
  }
  switch (p->kind) {
    case ir::Prod::Kind::Tok:
      return DerivationNode{format_token_name(g, p->token_id), {}};
    case ir::Prod::Kind::Ref:
      return DerivationNode{p->rule_id, {}};
    case ir::Prod::Kind::Eps:
      return DerivationNode{"eps", {}};
    case ir::Prod::Kind::Seq: {
      DerivationNode n{"seq", {}};
      for (const auto& it : p->items) {
        n.children.push_back(build_derivation_for_prod(g, it, depth + 1));
      }
      return n;
    }
    case ir::Prod::Kind::Act: {
      std::string lbl = p->action.kind == ir::ActionExpr::Kind::Ctor ? p->action.name : "act";
      DerivationNode n{lbl, {}};
      for (const auto& it : p->items) {
        n.children.push_back(build_derivation_for_prod(g, it, depth + 1));
      }
      return n;
    }
    case ir::Prod::Kind::Alt:
      if (!p->items.empty()) {
        return build_derivation_for_prod(g, p->items[0], depth + 1);
      }
      return DerivationNode{"alt", {}};
    case ir::Prod::Kind::Rep:
      return DerivationNode{"rep", {build_derivation_for_prod(g, p->inner, depth + 1)}};
    case ir::Prod::Kind::Look:
      return DerivationNode{"look", {build_derivation_for_prod(g, p->inner, depth + 1)}};
    case ir::Prod::Kind::PrecTable:
      return DerivationNode{"prec", {build_derivation_for_prod(g, p->inner, depth + 1)}};
    case ir::Prod::Kind::Ctx:
      return DerivationNode{p->ctx_var.empty() ? "ctx" : ("exactly " + p->ctx_var), {}};
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::Recover:
      return build_derivation_for_prod(g, p->inner, depth + 1);
  }
  return DerivationNode{"?", {}};
}

inline ir::ProdPtr factor_prod(const ir::Grammar& g,
                               const std::string& rule_id,
                               const ir::ProdPtr& p,
                               DiagnosticBag& diags,
                               AnalysisResult& result) {
  if (!p) {
    return p;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Act: {
      for (auto& it : p->items) {
        it = factor_prod(g, rule_id, it, diags, result);
      }
      return p;
    }
    case ir::Prod::Kind::Rep:
    case ir::Prod::Kind::Look:
    case ir::Prod::Kind::Label:
      p->inner = factor_prod(g, rule_id, p->inner, diags, result);
      return p;
    case ir::Prod::Kind::Recover:
      p->inner = factor_prod(g, rule_id, p->inner, diags, result);
      p->sync = factor_prod(g, rule_id, p->sync, diags, result);
      return p;
    case ir::Prod::Kind::PrecTable:
      p->inner = factor_prod(g, rule_id, p->inner, diags, result);
      return p;
    case ir::Prod::Kind::Ctx:
      for (auto& st : p->ctx_steps) {
        st.prod = factor_prod(g, rule_id, st.prod, diags, result);
      }
      return p;
    case ir::Prod::Kind::Alt:
      break;
    case ir::Prod::Kind::Tok:
    case ir::Prod::Kind::Ref:
    case ir::Prod::Kind::Eps:
      return p;
  }

  // First factor children recursively
  for (auto& it : p->items) {
    it = factor_prod(g, rule_id, it, diags, result);
  }
  if (p->items.size() < 2) {
    return p;
  }

  std::vector<FlattenedAlt> alts;
  alts.reserve(p->items.size());
  for (const auto& it : p->items) {
    alts.push_back(flatten_alt_item(it));
  }

  // Group alternatives sharing the same leading item (preserving order of first appearance)
  std::vector<bool> grouped(alts.size(), false);
  std::vector<ir::ProdPtr> new_items;
  bool any_factored = false;

  for (std::size_t i = 0; i < alts.size(); ++i) {
    if (grouped[i]) {
      continue;
    }
    if (alts[i].seq.empty()) {
      new_items.push_back(alts[i].original);
      grouped[i] = true;
      continue;
    }
    std::vector<std::size_t> members = {i};
    for (std::size_t j = i + 1; j < alts.size(); ++j) {
      if (!grouped[j] && !alts[j].seq.empty() &&
          prod_equiv(alts[i].seq[0], alts[j].seq[0])) {
        members.push_back(j);
      }
    }
    if (members.size() == 1) {
      new_items.push_back(alts[i].original);
      grouped[i] = true;
      continue;
    }

    // Compute longest common prefix across all members
    std::size_t prefix_len = 1;
    while (true) {
      bool all_have = true;
      for (std::size_t idx : members) {
        if (alts[idx].seq.size() <= prefix_len ||
            !prod_equiv(alts[members[0]].seq[prefix_len], alts[idx].seq[prefix_len])) {
          all_have = false;
          break;
        }
      }
      if (!all_have) {
        break;
      }
      ++prefix_len;
    }

    std::vector<std::string> prefix_names;
    for (std::size_t k = 0; k < prefix_len; ++k) {
      prefix_names.push_back(describe_prod(g, alts[members[0]].seq[k]));
    }
    std::string prefix_desc;
    for (std::size_t k = 0; k < prefix_names.size(); ++k) {
      if (k > 0) {
        prefix_desc += " ";
      }
      prefix_desc += prefix_names[k];
    }

    Span conflict_span = alts[members[0]].span;
    for (std::size_t idx : members) {
      if (alts[idx].span.is_valid()) {
        conflict_span = Span::merge(conflict_span, alts[idx].span);
      }
    }

    if (g.pragmas.no_factor) {
      // Emit E0214 as Error with concrete counterexample
      std::unordered_set<std::string> vis;
      auto toks = sample_tokens_for_prod(g, alts[members[0]].original, vis);
      Counterexample cx;
      cx.rule_id = rule_id;
      cx.token_sequence = toks;
      cx.input_sample = join_token_samples(g, toks);
      cx.tree_a = DerivationNode{rule_id, {build_derivation_for_prod(g, alts[members[0]].original)}};
      cx.tree_b = DerivationNode{rule_id, {build_derivation_for_prod(g, alts[members[1]].original)}};
      cx.description = "alternatives share common prefix `" + prefix_desc + "`";
      cx.fix_recommendation =
          "remove {-# NOFACTOR #-} to enable automatic left-factoring, or factor `" +
          prefix_desc + "` manually";
      cx.span = conflict_span;

      Diagnostic d;
      d.code = "E0214";
      d.severity = Severity::Error;
      d.message = "ambiguous alternatives in rule `" + rule_id + "`";
      d.span = conflict_span;
      d.label = "both start with `" + prefix_desc + "`";
      if (alts[members[1]].span.is_valid()) {
        d.related.push_back(RelatedSpan{alts[members[1]].span, "conflicting alternative here"});
      }
      d.notes.push_back("counterexample input: \"" + cx.input_sample + "\"");
      d.helps.push_back(cx.fix_recommendation);
      diags.push(std::move(d));

      result.counterexamples.push_back(std::move(cx));
      result.has_ambiguity = true;
      for (std::size_t idx : members) {
        grouped[idx] = true;
        new_items.push_back(alts[idx].original);
      }
      continue;
    }

    // Automatic left-factoring
    any_factored = true;
    std::vector<ir::ProdPtr> suffixes;
    suffixes.reserve(members.size());
    for (std::size_t idx : members) {
      grouped[idx] = true;
      suffixes.push_back(build_suffix_prod(alts[idx], prefix_len));
    }

    auto tail_alt = ir::make_alt(std::move(suffixes), conflict_span);
    tail_alt = factor_prod(g, rule_id, tail_alt, diags, result);

    std::vector<ir::ProdPtr> seq_items;
    seq_items.reserve(prefix_len + 1);
    for (std::size_t k = 0; k < prefix_len; ++k) {
      seq_items.push_back(alts[members[0]].seq[k]);
    }
    seq_items.push_back(std::move(tail_alt));
    new_items.push_back(ir::make_seq(std::move(seq_items), conflict_span));

    LeftFactoringRecord rec;
    rec.rule_id = rule_id;
    rec.common_prefix_tokens = std::move(prefix_names);
    rec.original_alt_count = p->items.size();
    rec.span = conflict_span;
    result.factored_rules.push_back(std::move(rec));

    Diagnostic d;
    d.code = "E0214";
    d.severity = Severity::Note;
    d.message = "ambiguous alternatives in rule `" + rule_id + "`";
    d.span = conflict_span;
    d.label = "both start with `" + prefix_desc + "`";
    d.notes.push_back("this is resolved by left-factoring (automatic)");
    d.helps.push_back("add {-# NOFACTOR #-} to disable and see the conflict");
    diags.push(std::move(d));
  }

  if (any_factored) {
    if (!result.factored_rules.empty()) {
      result.factored_rules.back().factored_alt_count = new_items.size();
    }
    if (new_items.size() == 1) {
      return new_items[0];
    }
    p->items = std::move(new_items);
  }
  return p;
}

inline bool is_prod_nullable(const ir::ProdPtr& p,
                             const std::map<std::string, RuleAnalysis>& rules) {
  if (!p) {
    return true;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Eps:
      return true;
    case ir::Prod::Kind::Tok:
      return false;
    case ir::Prod::Kind::Ref: {
      auto it = rules.find(p->rule_id);
      return it != rules.end() && it->second.nullable;
    }
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Act:
      for (const auto& item : p->items) {
        if (!is_prod_nullable(item, rules)) {
          return false;
        }
      }
      return true;
    case ir::Prod::Kind::Alt:
      for (const auto& item : p->items) {
        if (is_prod_nullable(item, rules)) {
          return true;
        }
      }
      return false;
    case ir::Prod::Kind::Rep:
      return p->min_rep == 0 || is_prod_nullable(p->inner, rules);
    case ir::Prod::Kind::Look:
      return true;
    case ir::Prod::Kind::PrecTable:
      return is_prod_nullable(p->inner, rules);
    case ir::Prod::Kind::Ctx:
      if (!p->ctx_var.empty()) {
        return false;
      }
      for (const auto& st : p->ctx_steps) {
        if (!is_prod_nullable(st.prod, rules)) {
          return false;
        }
      }
      return true;
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::Recover:
      return is_prod_nullable(p->inner, rules);
  }
  return false;
}

inline std::set<std::string> first_of_prod(const ir::ProdPtr& p,
                                           const std::map<std::string, RuleAnalysis>& rules) {
  std::set<std::string> out;
  if (!p) {
    return out;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Eps:
      return out;
    case ir::Prod::Kind::Tok:
      out.insert(p->token_id);
      return out;
    case ir::Prod::Kind::Ref: {
      auto it = rules.find(p->rule_id);
      if (it != rules.end()) {
        out.insert(it->second.first.begin(), it->second.first.end());
      }
      return out;
    }
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Act:
      for (const auto& item : p->items) {
        auto f = first_of_prod(item, rules);
        out.insert(f.begin(), f.end());
        if (!is_prod_nullable(item, rules)) {
          break;
        }
      }
      return out;
    case ir::Prod::Kind::Alt:
      for (const auto& item : p->items) {
        auto f = first_of_prod(item, rules);
        out.insert(f.begin(), f.end());
      }
      return out;
    case ir::Prod::Kind::Rep:
      return first_of_prod(p->inner, rules);
    case ir::Prod::Kind::Look:
      if (p->polarity == ir::Polarity::Positive) {
        return first_of_prod(p->inner, rules);
      }
      return out;
    case ir::Prod::Kind::PrecTable: {
      auto f = first_of_prod(p->inner, rules);
      out.insert(f.begin(), f.end());
      for (const auto& lvl : p->levels) {
        if (lvl.assoc == ast::AssocKind::Prefix) {
          for (const auto& op : lvl.ops) {
            if (!op.pattern.empty()) {
              auto pf = first_of_prod(op.pattern[0], rules);
              out.insert(pf.begin(), pf.end());
            }
          }
        }
      }
      return out;
    }
    case ir::Prod::Kind::Ctx:
      if (!p->ctx_var.empty()) {
        out.insert("<ctx:" + p->ctx_var + ">");
        return out;
      }
      for (const auto& st : p->ctx_steps) {
        auto f = first_of_prod(st.prod, rules);
        out.insert(f.begin(), f.end());
        if (!is_prod_nullable(st.prod, rules)) {
          break;
        }
      }
      return out;
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::Recover:
      return first_of_prod(p->inner, rules);
  }
  return out;
}

inline bool propagate_follow(const ir::ProdPtr& p,
                             const std::set<std::string>& tail_follow,
                             std::map<std::string, RuleAnalysis>& rules) {
  if (!p) {
    return false;
  }
  bool changed = false;
  switch (p->kind) {
    case ir::Prod::Kind::Eps:
    case ir::Prod::Kind::Tok:
      return false;
    case ir::Prod::Kind::Ref: {
      auto it = rules.find(p->rule_id);
      if (it != rules.end()) {
        const std::size_t before = it->second.follow.size();
        it->second.follow.insert(tail_follow.begin(), tail_follow.end());
        if (it->second.follow.size() > before) {
          changed = true;
        }
      }
      return changed;
    }
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Act: {
      std::set<std::string> cur_follow = tail_follow;
      for (std::size_t idx = p->items.size(); idx > 0; --idx) {
        const auto& item = p->items[idx - 1];
        if (propagate_follow(item, cur_follow, rules)) {
          changed = true;
        }
        auto f = first_of_prod(item, rules);
        if (is_prod_nullable(item, rules)) {
          cur_follow.insert(f.begin(), f.end());
        } else {
          cur_follow = std::move(f);
        }
      }
      return changed;
    }
    case ir::Prod::Kind::Alt:
      for (const auto& item : p->items) {
        if (propagate_follow(item, tail_follow, rules)) {
          changed = true;
        }
      }
      return changed;
    case ir::Prod::Kind::Rep: {
      std::set<std::string> inner_follow = tail_follow;
      if (!p->max_rep || *p->max_rep > 1) {
        auto f = first_of_prod(p->inner, rules);
        inner_follow.insert(f.begin(), f.end());
      }
      return propagate_follow(p->inner, inner_follow, rules);
    }
    case ir::Prod::Kind::Look:
      return propagate_follow(p->inner, tail_follow, rules);
    case ir::Prod::Kind::PrecTable: {
      std::set<std::string> base_follow = tail_follow;
      for (const auto& lvl : p->levels) {
        for (const auto& op : lvl.ops) {
          if (!op.pattern.empty()) {
            auto pf = first_of_prod(op.pattern[0], rules);
            base_follow.insert(pf.begin(), pf.end());
          }
          std::set<std::string> pat_follow = tail_follow;
          for (std::size_t idx = op.pattern.size(); idx > 0; --idx) {
            if (propagate_follow(op.pattern[idx - 1], pat_follow, rules)) {
              changed = true;
            }
            auto pf = first_of_prod(op.pattern[idx - 1], rules);
            if (is_prod_nullable(op.pattern[idx - 1], rules)) {
              pat_follow.insert(pf.begin(), pf.end());
            } else {
              pat_follow = std::move(pf);
            }
          }
        }
      }
      if (propagate_follow(p->inner, base_follow, rules)) {
        changed = true;
      }
      return changed;
    }
    case ir::Prod::Kind::Ctx: {
      std::set<std::string> cur_follow = tail_follow;
      for (std::size_t idx = p->ctx_steps.size(); idx > 0; --idx) {
        const auto& st = p->ctx_steps[idx - 1];
        if (propagate_follow(st.prod, cur_follow, rules)) {
          changed = true;
        }
        auto f = first_of_prod(st.prod, rules);
        if (is_prod_nullable(st.prod, rules)) {
          cur_follow.insert(f.begin(), f.end());
        } else {
          cur_follow = std::move(f);
        }
      }
      return changed;
    }
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::Recover:
      if (propagate_follow(p->inner, tail_follow, rules)) {
        changed = true;
      }
      if (p->sync && propagate_follow(p->sync, tail_follow, rules)) {
        changed = true;
      }
      return changed;
  }
  return changed;
}

inline void collect_all_refs(const ir::ProdPtr& p, std::vector<std::string>& out) {
  if (!p) {
    return;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Ref:
      out.push_back(p->rule_id);
      return;
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Alt:
    case ir::Prod::Kind::Act:
      for (const auto& it : p->items) {
        collect_all_refs(it, out);
      }
      return;
    case ir::Prod::Kind::Rep:
    case ir::Prod::Kind::Look:
    case ir::Prod::Kind::Label:
      collect_all_refs(p->inner, out);
      return;
    case ir::Prod::Kind::Recover:
      collect_all_refs(p->inner, out);
      collect_all_refs(p->sync, out);
      return;
    case ir::Prod::Kind::PrecTable:
      collect_all_refs(p->inner, out);
      for (const auto& lvl : p->levels) {
        for (const auto& op : lvl.ops) {
          for (const auto& pat : op.pattern) {
            collect_all_refs(pat, out);
          }
        }
      }
      return;
    case ir::Prod::Kind::Ctx:
      for (const auto& st : p->ctx_steps) {
        collect_all_refs(st.prod, out);
      }
      return;
    case ir::Prod::Kind::Tok:
    case ir::Prod::Kind::Eps:
      return;
  }
}

inline void collect_left_refs(const ir::ProdPtr& p,
                              const std::map<std::string, RuleAnalysis>& rules,
                              std::vector<std::string>& out) {
  if (!p) {
    return;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Ref:
      out.push_back(p->rule_id);
      return;
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Act:
      for (const auto& it : p->items) {
        collect_left_refs(it, rules, out);
        if (!is_prod_nullable(it, rules)) {
          break;
        }
      }
      return;
    case ir::Prod::Kind::Alt:
      for (const auto& it : p->items) {
        collect_left_refs(it, rules, out);
      }
      return;
    case ir::Prod::Kind::Rep:
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::Recover:
    case ir::Prod::Kind::PrecTable:
      collect_left_refs(p->inner, rules, out);
      return;
    case ir::Prod::Kind::Look:
      if (p->polarity == ir::Polarity::Positive) {
        collect_left_refs(p->inner, rules, out);
      }
      return;
    case ir::Prod::Kind::Ctx:
      for (const auto& st : p->ctx_steps) {
        collect_left_refs(st.prod, rules, out);
        if (!is_prod_nullable(st.prod, rules)) {
          break;
        }
      }
      return;
    case ir::Prod::Kind::Tok:
    case ir::Prod::Kind::Eps:
      return;
  }
}

inline bool prod_uses_context(const ir::ProdPtr& p) {
  if (!p) {
    return false;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Ctx:
      return true;
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Alt:
    case ir::Prod::Kind::Act:
      for (const auto& it : p->items) {
        if (prod_uses_context(it)) {
          return true;
        }
      }
      return false;
    case ir::Prod::Kind::Rep:
    case ir::Prod::Kind::Look:
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::PrecTable:
      return prod_uses_context(p->inner);
    case ir::Prod::Kind::Recover:
      return prod_uses_context(p->inner) || prod_uses_context(p->sync);
    case ir::Prod::Kind::Tok:
    case ir::Prod::Kind::Ref:
    case ir::Prod::Kind::Eps:
      return false;
  }
  return false;
}

inline void check_alt_conflicts(const ir::Grammar& g,
                                const std::string& rule_id,
                                const ir::ProdPtr& p,
                                const std::set<std::string>& local_follow,
                                std::map<std::string, RuleAnalysis>& rules,
                                AnalysisResult& result) {
  if (!p) {
    return;
  }
  switch (p->kind) {
    case ir::Prod::Kind::Alt: {
      std::vector<std::set<std::string>> preds;
      preds.reserve(p->items.size());
      for (const auto& it : p->items) {
        auto pred = first_of_prod(it, rules);
        if (is_prod_nullable(it, rules)) {
          pred.insert(local_follow.begin(), local_follow.end());
        }
        preds.push_back(std::move(pred));
      }
      for (std::size_t i = 0; i < p->items.size(); ++i) {
        for (std::size_t j = i + 1; j < p->items.size(); ++j) {
          std::vector<std::string> overlap;
          for (const auto& tok : preds[i]) {
            if (preds[j].contains(tok)) {
              overlap.push_back(tok);
            }
          }
          if (!overlap.empty()) {
            rules[rule_id].has_ll1_conflict = true;
            rules[rule_id].required_k = std::max(rules[rule_id].required_k, 2);
            result.has_ambiguity = true;

            // Avoid duplicating counterexamples already emitted during NOFACTOR pass
            bool already_recorded = false;
            for (const auto& existing : result.counterexamples) {
              if (existing.rule_id == rule_id && existing.span == p->span) {
                already_recorded = true;
                break;
              }
            }
            if (!already_recorded) {
              std::unordered_set<std::string> vis;
              auto toks_a = sample_tokens_for_prod(g, p->items[i], vis);
              if (toks_a.empty() && !overlap.empty() && overlap[0] != "eof") {
                toks_a.push_back(overlap[0]);
              }
              Counterexample cx;
              cx.rule_id = rule_id;
              cx.token_sequence = toks_a;
              cx.input_sample = join_token_samples(g, toks_a);
              cx.tree_a = DerivationNode{rule_id, {build_derivation_for_prod(g, p->items[i])}};
              cx.tree_b = DerivationNode{rule_id, {build_derivation_for_prod(g, p->items[j])}};
              cx.description = "FIRST/FOLLOW conflict on token `" +
                               format_token_name(g, overlap[0]) + "` between alternatives " +
                               std::to_string(i + 1) + " and " + std::to_string(j + 1);
              cx.fix_recommendation =
                  "refactor alternatives to have disjoint prefixes or switch to {-# MODE PEG #-} / {-# MODE GLL #-}";
              cx.span = p->span;
              result.counterexamples.push_back(std::move(cx));
            }
          }
        }
      }
      for (const auto& it : p->items) {
        check_alt_conflicts(g, rule_id, it, local_follow, rules, result);
      }
      return;
    }
    case ir::Prod::Kind::Seq:
    case ir::Prod::Kind::Act: {
      std::set<std::string> cur_follow = local_follow;
      for (std::size_t idx = p->items.size(); idx > 0; --idx) {
        const auto& item = p->items[idx - 1];
        check_alt_conflicts(g, rule_id, item, cur_follow, rules, result);
        auto f = first_of_prod(item, rules);
        if (is_prod_nullable(item, rules)) {
          cur_follow.insert(f.begin(), f.end());
        } else {
          cur_follow = std::move(f);
        }
      }
      return;
    }
    case ir::Prod::Kind::Rep: {
      std::set<std::string> inner_follow = local_follow;
      if (!p->max_rep || *p->max_rep > 1) {
        auto f = first_of_prod(p->inner, rules);
        inner_follow.insert(f.begin(), f.end());
      }
      check_alt_conflicts(g, rule_id, p->inner, inner_follow, rules, result);
      return;
    }
    case ir::Prod::Kind::Look:
    case ir::Prod::Kind::Label:
    case ir::Prod::Kind::Recover:
    case ir::Prod::Kind::PrecTable:
      check_alt_conflicts(g, rule_id, p->inner, local_follow, rules, result);
      return;
    case ir::Prod::Kind::Ctx:
      for (const auto& st : p->ctx_steps) {
        check_alt_conflicts(g, rule_id, st.prod, local_follow, rules, result);
      }
      return;
    case ir::Prod::Kind::Tok:
    case ir::Prod::Kind::Ref:
    case ir::Prod::Kind::Eps:
      return;
  }
}

inline void check_lexer_conflicts_in_list(const std::string& mode_name,
                                          const std::vector<ir::TokenDef>& tokens,
                                          const ir::GrammarPragmas& pragmas,
                                          DiagnosticBag& diags,
                                          AnalysisResult& result) {
  auto has_explicit_prec = [&](const std::string& a, const std::string& b) {
    for (const auto& [lo, hi] : pragmas.token_precedence) {
      if ((lo == a && hi == b) || (lo == b && hi == a)) {
        return true;
      }
    }
    return false;
  };

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    for (std::size_t j = i + 1; j < tokens.size(); ++j) {
      const auto& a = tokens[i];
      const auto& b = tokens[j];
      if (a.pattern == b.pattern && a.is_regex == b.is_regex && a.priority == b.priority &&
          !has_explicit_prec(a.id, b.id)) {
        LexerConflict lc;
        lc.mode_name = mode_name;
        lc.token_a = a.id;
        lc.token_b = b.id;
        lc.pattern = a.pattern;
        lc.span_a = a.span;
        lc.span_b = b.span;
        result.lexer_conflicts.push_back(std::move(lc));

        Diagnostic d;
        d.code = "E0002";
        d.severity = Severity::Error;
        d.message = "unresolvable lexer token conflict between `" + a.id + "` and `" + b.id +
                    "` in mode `" + mode_name + "`";
        d.span = b.span;
        d.label = "identical pattern `" + a.pattern + "` with same priority";
        if (a.span.is_valid()) {
          d.related.push_back(RelatedSpan{a.span, "conflicting token `" + a.id + "` defined here"});
        }
        d.helps.push_back("use distinct patterns or specify {-# PREC " + a.id + " < " + b.id + " #-}");
        diags.push(std::move(d));
      }
    }
  }
}

} // namespace parsley::analysis::detail

export namespace parsley::analysis {

inline AnalysisResult analyze_grammar(ir::Grammar& grammar, DiagnosticBag& diags) {
  AnalysisResult result;

  // 1. Lexer conflict analysis (E0002)
  detail::check_lexer_conflicts_in_list("default", grammar.gTokens, grammar.pragmas, diags, result);
  for (const auto& mode : grammar.gModes) {
    detail::check_lexer_conflicts_in_list(mode.name, mode.tokens, grammar.pragmas, diags, result);
  }

  // 2. Automatic left-factoring (E0214)
  for (auto& [rid, rule] : grammar.gRules) {
    rule.rBody = detail::factor_prod(grammar, rid, rule.rBody, diags, result);
  }

  // Initialize rule entries
  for (const auto& [rid, rule] : grammar.gRules) {
    RuleAnalysis ra;
    ra.rule_id = rid;
    ra.span = rule.rSpan;
    ra.uses_context = detail::prod_uses_context(rule.rBody);
    if (ra.uses_context) {
      result.has_context_rules = true;
    }
    result.rules[rid] = std::move(ra);
  }

  // 3. NULLABLE fixed point
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& [rid, rule] : grammar.gRules) {
      const bool n = detail::is_prod_nullable(rule.rBody, result.rules);
      if (n && !result.rules[rid].nullable) {
        result.rules[rid].nullable = true;
        changed = true;
      }
    }
  }

  // 4. FIRST fixed point
  changed = true;
  while (changed) {
    changed = false;
    for (const auto& [rid, rule] : grammar.gRules) {
      auto f = detail::first_of_prod(rule.rBody, result.rules);
      auto& cur = result.rules[rid].first;
      const std::size_t before = cur.size();
      cur.insert(f.begin(), f.end());
      if (cur.size() > before) {
        changed = true;
      }
    }
  }

  // 5. FOLLOW fixed point
  for (const auto& start : grammar.gStarts) {
    if (auto it = result.rules.find(start); it != result.rules.end()) {
      it->second.follow.insert("eof");
    }
  }
  changed = true;
  while (changed) {
    changed = false;
    for (const auto& [rid, rule] : grammar.gRules) {
      if (detail::propagate_follow(rule.rBody, result.rules[rid].follow, result.rules)) {
        changed = true;
      }
    }
  }

  // 6. Reachability / dead rules (E0520)
  std::queue<std::string> q;
  for (const auto& s : grammar.gStarts) {
    if (auto it = result.rules.find(s); it != result.rules.end() && !it->second.reachable) {
      it->second.reachable = true;
      q.push(s);
    }
  }
  while (!q.empty()) {
    std::string cur = std::move(q.front());
    q.pop();
    auto rit = grammar.gRules.find(cur);
    if (rit == grammar.gRules.end()) {
      continue;
    }
    std::vector<std::string> refs;
    detail::collect_all_refs(rit->second.rBody, refs);
    for (const auto& target : refs) {
      if (auto it = result.rules.find(target); it != result.rules.end() && !it->second.reachable) {
        it->second.reachable = true;
        q.push(target);
      }
    }
  }
  for (const auto& [rid, ra] : result.rules) {
    if (!ra.reachable) {
      result.dead_rules.push_back(rid);
      Diagnostic d;
      d.code = "E0520";
      d.severity = Severity::Warning;
      d.message = "unreachable (dead) rule `" + rid + "`";
      d.span = ra.span;
      d.label = "not reachable from any start rule";
      d.helps.push_back("remove `" + rid + "` or reference it from an exported/start rule");
      diags.push(std::move(d));
    }
  }

  // 7. Direct & indirect left-recursion detection
  std::map<std::string, std::set<std::string>> left_adj;
  for (const auto& [rid, rule] : grammar.gRules) {
    std::vector<std::string> lrefs;
    detail::collect_left_refs(rule.rBody, result.rules, lrefs);
    for (const auto& target : lrefs) {
      if (grammar.gRules.contains(target)) {
        left_adj[rid].insert(target);
        if (target == rid) {
          result.rules[rid].direct_left_recursive = true;
          result.rules[rid].left_recursive = true;
          result.has_left_recursion = true;
        }
      }
    }
  }
  for (const auto& [rid, _] : grammar.gRules) {
    // Check if rid can reach itself in >= 1 step in left_adj
    std::unordered_set<std::string> visited;
    std::queue<std::string> bfs;
    for (const auto& next : left_adj[rid]) {
      if (visited.insert(next).second) {
        bfs.push(next);
      }
    }
    while (!bfs.empty()) {
      std::string cur = std::move(bfs.front());
      bfs.pop();
      if (cur == rid) {
        result.rules[rid].left_recursive = true;
        result.has_left_recursion = true;
        break;
      }
      for (const auto& next : left_adj[cur]) {
        if (visited.insert(next).second) {
          bfs.push(next);
        }
      }
    }
  }

  // 8. Check Alt FIRST/FOLLOW conflicts & generate counterexamples
  for (const auto& [rid, rule] : grammar.gRules) {
    detail::check_alt_conflicts(
        grammar, rid, rule.rBody, result.rules[rid].follow, result.rules, result);
  }

  // 9. Strategy selection
  switch (grammar.pragmas.mode) {
    case ir::ParseMode::LL1:
      result.selected_strategy = Strategy::LL1;
      result.strategy_reason = "explicit {-# MODE LL1 #-} pragma";
      break;
    case ir::ParseMode::LLk:
      result.selected_strategy = Strategy::LLk;
      result.strategy_reason = "explicit {-# MODE LLk #-} pragma";
      break;
    case ir::ParseMode::LALR:
      result.selected_strategy = Strategy::LALR1;
      result.strategy_reason = "explicit {-# MODE LALR #-} pragma";
      break;
    case ir::ParseMode::PEG:
      result.selected_strategy = Strategy::PEG;
      result.strategy_reason = "explicit {-# MODE PEG #-} pragma";
      break;
    case ir::ParseMode::GLL:
      result.selected_strategy = Strategy::GLL;
      result.strategy_reason = "explicit {-# MODE GLL #-} pragma";
      break;
    case ir::ParseMode::Auto:
      if (result.has_context_rules) {
        if (result.has_left_recursion) {
          result.selected_strategy = Strategy::GLL;
          result.strategy_reason = "grammar uses `context do` with left recursion";
        } else {
          result.selected_strategy = Strategy::PEG;
          result.strategy_reason = "grammar uses `context do` monadic bindings";
        }
      } else if (result.has_left_recursion) {
        result.selected_strategy = Strategy::LALR1;
        result.strategy_reason = "grammar contains left recursion";
      } else if (result.has_ambiguity) {
        result.selected_strategy = Strategy::PEG;
        result.strategy_reason = "grammar has unresolved lookahead conflicts";
      } else {
        result.selected_strategy = Strategy::LL1;
        result.strategy_reason = "grammar is LL(1) after left-factoring";
      }
      break;
  }

  // 10. Validate strategy constraints (E0500, E0510)
  if ((result.selected_strategy == Strategy::LL1 || result.selected_strategy == Strategy::LLk) &&
      result.has_left_recursion) {
    for (const auto& [rid, ra] : result.rules) {
      if (ra.left_recursive) {
        Diagnostic d;
        d.code = "E0500";
        d.severity = Severity::Error;
        d.message = std::string(ra.direct_left_recursive ? "direct" : "indirect") +
                    " left recursion in rule `" + rid + "` is incompatible with " +
                    std::string(strategy_name(result.selected_strategy)) + " mode";
        d.span = ra.span;
        d.label = "rule `" + rid + "` can derive itself as its leftmost symbol";
        d.helps.push_back("rewrite using `operators` or switch to {-# MODE LALR #-} / {-# MODE GLL #-}");
        diags.push(std::move(d));
      }
    }
  }

  if (result.has_context_rules && result.selected_strategy != Strategy::PEG &&
      result.selected_strategy != Strategy::GLL) {
    for (const auto& [rid, ra] : result.rules) {
      if (ra.uses_context) {
        Diagnostic d;
        d.code = "E0510";
        d.severity = Severity::Error;
        d.message = "context-sensitive rule `" + rid + "` is incompatible with " +
                    std::string(strategy_name(result.selected_strategy)) + " strategy";
        d.span = ra.span;
        d.label = "uses `context do` / `exactly`";
        d.notes.push_back("`context` blocks introduce monadic dependence on parsed values");
        d.helps.push_back("use {-# MODE PEG #-} or {-# MODE GLL #-}");
        diags.push(std::move(d));
      }
    }
  }

  return result;
}

} // namespace parsley::analysis
