import sys
sys.path.insert(0, r"C:\wt_power_g2\tools")
from fec_ack_sweep import SimShell
for p in sys.argv[1:]:
    try:
        s = SimShell(p)
        print(p, repr(s.run("ackfault show", timeout=3)[:300]))
        print(p, repr(s.run("ackfault turn", until="TURN-END", timeout=3)[:300]))
        s.close()
    except Exception as e:
        print(p, "ERR", e)
