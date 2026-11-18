"""Print the optimisation flags of the hot files from each TI build's compile_commands.json."""
import json
import sys

for build in sys.argv[1:]:
    print(f"== {build}")
    cc = json.load(open(build + "/dongle_ti/compile_commands.json"))
    for e in cc:
        f = e["file"].replace("\\", "/")
        if any(f.endswith(x) for x in ("radiant_sched.c", "ant_radio_radiant.c",
                                         "radiant_radio_cc26xx.c", "radiant_search.c")):
            c = e.get("command") or " ".join(e["arguments"])
            opts = [t for t in c.split() if t in ("-Os", "-O2", "-O3", "-O0", "-Og")]
            print(f"  {f.split('/')[-1]}: {' '.join(opts)}")
