# python3 -B -c <script> <config file>: runs the config, prints records:
#   V <b|i|f|s> <name> <value> | K/B <mods> <key|button> <action> <arg> | E <msg>
import sys, os

def esc(s):
    return (s.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\n", "\\n").replace("\r", "\\r"))

out = []

def emit(*fields):
    out.append("\t".join(esc(str(f)) for f in fields))

def finish(code):
    data = ("\n".join(out) + "\n").encode("utf-8", "surrogateescape")
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()
    sys.exit(code)

path = sys.argv[1]
ns = {"__name__": "wate_config"}
try:
    import __future__
    with open(path, "rb") as fh:
        src = fh.read()
    apath = os.path.abspath(path)
    ns["__file__"] = apath
    sys.path.insert(0, os.path.dirname(apath))
    code = compile(src, path, "exec",
                   flags=__future__.annotations.compiler_flag,
                   dont_inherit=True)
    exec(code, ns)
except BaseException as e:
    line = getattr(e, "lineno", None)
    if line is None:
        import traceback
        for fr in traceback.extract_tb(e.__traceback__):
            if fr.filename == path:
                line = fr.lineno
    msg = type(e).__name__
    detail = getattr(e, "msg", None) or str(e)
    if detail:
        msg += ": " + str(detail)
    if line:
        msg += " (line %s)" % line
    emit("E", msg)
    finish(1)

SIMPLE = (str, int, float, bool)

def tname(v):
    if isinstance(v, bool):
        return "b"
    if isinstance(v, int):
        return "i"
    if isinstance(v, float):
        return "f"
    return "s"

def val(v):
    if isinstance(v, bool):
        return "1" if v else "0"
    return v

for name, v in list(ns.items()):
    if name.startswith("_") or name in ("keys", "buttons"):
        continue
    if isinstance(v, SIMPLE):
        emit("V", tname(v), name, val(v))
    elif isinstance(v, type) and v.__module__ == "wate_config":
        for a, av in list(vars(v).items()):
            if not a.startswith("_") and isinstance(av, SIMPLE):
                emit("V", tname(av), name + "." + a, val(av))

def binding_list(name, tag, what):
    items = ns.get(name)
    if items is None:
        return
    if not isinstance(items, (list, tuple)):
        emit("E", "%s must be a list of [mods, %s, action, arg] entries" % (name, what))
        return
    for i, k in enumerate(items):
        ok = isinstance(k, (list, tuple)) and 3 <= len(k) <= 4
        if ok:
            mods, key, action = k[0], k[1], k[2]
            if isinstance(mods, (list, tuple)):
                ok = all(isinstance(m, str) for m in mods)
                mods = "+".join(mods) if ok else ""
            ok = ok and isinstance(mods, str) and isinstance(key, str) \
                and isinstance(action, str)
        if not ok:
            emit("E", "%s[%d] must be [mods, %s, action] or "
                      "[mods, %s, action, arg]" % (name, i, what, what))
            continue
        emit(tag, mods, key, action, k[3] if len(k) == 4 else "")

binding_list("keys", "K", "key")
binding_list("buttons", "B", "button")

finish(0)
