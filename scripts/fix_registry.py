#!/usr/bin/env python3
"""Fix registry: change auto v1=cond?lit:lit → const char* v1=..."""
import re
from pathlib import Path

t = (Path.home() / "peiking88/czsc-cpp/src/signals/registry.cpp").read_text()

# Fix: auto v1 = cond ? "lit" : "lit"; → const char* v1 = ...
# This way v1.c_str() is valid (v1 is char* not const char*)
# Wait, actually the problem is the opposite: auto with string literals gives const char*,
# and .c_str() on const char* fails. So we have two options:
# A) Change auto→std::string (so .c_str() works)
# B) Remove .c_str() calls

# Option A is safer: change auto to std::string for string-returning ternaries
t = re.sub(
    r'(auto\s+)(\w+)(\s*=\s*)(\(?)([^;]*?\?"[^"]+"\s*:\s*"[^"]+")(\)?;)',
    r'std::string \2\3\4\5\6',
    t
)

# Fix: double p shadowing parameter p
# Pattern: in function body, there's `double p=...` that shadows `const ParamView& p`
t = re.sub(
    r'(const ParamView& p[^)]*\)\s*\{.*?)(double\s+p\s*=)',
    lambda m: m.group(1) + 'double _' + m.group(2)[7:],
    t, flags=re.DOTALL
)

# Actually the above is too aggressive. Use simpler:
t = t.replace("double p=(b.upper.back()-b.lower.back())/b.mid.back()*100;", "double pw=(b.upper.back()-b.lower.back())/b.mid.back()*100;")
t = t.replace("p>5?\"", "pw>5?\"")
t = t.replace("p>2?\"", "pw>2?\"")
t = t.replace("double p=std::abs(mc.macd.back());", "double mp=std::abs(mc.macd.back());")
t = t.replace("mp>0.5?\"", "mp>0.5?\"")
t = t.replace("mp>0.1?\"", "mp>0.1?\"")

# Fix shadowed p in cut functions
t = t.replace("double p=(cl-ll)/(hh-ll)*100;", "double pos=(cl-ll)/(hh-ll)*100;")
t = t.replace("p>80?\"", "pos>80?\"")
t = t.replace("p<20?\"", "pos<20?\"")

(Path.home() / "peiking88/czsc-cpp/src/signals/registry.cpp").write_text(t)
print(f"Fixed: auto→std::string for ternaries, variable shadowing")
