"""Instrument an isolated tool copy; never changes a project under test."""
from pathlib import Path
import sys

source = Path(sys.argv[1]) / "src/exec.cc"
text = source.read_text()
needle = "      commands = ce_.Eval(n);"
replacement = '''      if (n.output.str().find("concat-deps") != std::string_view::npos) {
        fprintf(stderr, "DEBUG target %s inputs %zu vars %zu\\n", n.output.c_str(), n.actual_inputs.size(), n.rule_vars ? n.rule_vars->size() : 0);
        for (const auto& dep : n.deps) fprintf(stderr, "DEBUG dep %s\\n", dep.first.c_str());
        for (const auto& dep : n.order_onlys) fprintf(stderr, "DEBUG order %s\\n", dep.first.c_str());
      }
''' + needle
assert needle in text
source.write_text(text.replace(needle, replacement))

source = Path(sys.argv[1]) / "src/eval.cc"
text = source.read_text()
needle = "  rules_.push_back(rule);"
replacement = '''  if (before_term.find("concat-deps") != std::string::npos)
    fprintf(stderr, "DEBUG rule at %s:%d %s\\n", loc_.filename, loc_.lineno, rule->DebugString().c_str());
''' + needle
assert needle in text
source.write_text(text.replace(needle, replacement))
