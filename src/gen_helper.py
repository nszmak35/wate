#!/usr/bin/env python3
# Regenerates helper_py.h from helper.py
import os

here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, "helper.py")).read().rstrip("\n").split("\n")
out = ["/* generated from helper.py by gen_helper.py */",
       "#ifndef WATE_HELPER_PY_H", "#define WATE_HELPER_PY_H", "",
       "static const char helper_script[] ="]
for line in src:
    out.append('\t"%s\\n"' % line.replace("\\", "\\\\").replace('"', '\\"'))
out[-1] += ";"
out += ["", "#endif", ""]
open(os.path.join(here, "helper_py.h"), "w").write("\n".join(out))
