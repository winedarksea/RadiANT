"""Phase 3: loss per device number against candidate predictors.

On-air address for a power sensor (dtype 0x0B): A6 C5 dnl dnh 0B, each byte
MSb-first (pack_address() rev8()s every byte and the RADIO sends LSb-first).

    python analyze_sweep.py [prefix]   (default p3)
"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
DTYPE = 0x0B


def bits(dn, nbytes=5):
    addr = [0xA6, 0xC5, dn & 0xFF, dn >> 8, DTYPE][:nbytes]
    return "".join(f"{b:08b}" for b in addr)


def longest_alt(s):
    best = cur = 1
    for a, b in zip(s, s[1:]):
        cur = cur + 1 if a != b else 1
        best = max(best, cur)
    return best


def longest_same(s):
    best = cur = 1
    for a, b in zip(s, s[1:]):
        cur = cur + 1 if a == b else 1
        best = max(best, cur)
    return best


def min_shift_hamming(s, shifts=range(1, 9)):
    """Smallest Hamming distance between the address and itself shifted by
    1..8 bits (over the overlap) - how easily a slipped correlator re-matches."""
    out = []
    for k in shifts:
        a, b = s[k:], s[:-k]
        out.append(sum(x != y for x, y in zip(a, b)))
    return min(out)


def preamble_hamming(s, n=8):
    """Best match of the 8-bit preamble 01010101/10101010 anywhere after the
    first address byte."""
    best = n
    for i in range(8, len(s) - n + 1):
        w = s[i:i + n]
        for p in ("01010101", "10101010"):
            best = min(best, sum(x != y for x, y in zip(w, p)))
    return best


def loss_rows(win):
    out = {}
    for js in sorted(win.glob("*.json")):
        if js.name == "window.json":
            continue
        try:
            d = json.loads(js.read_text())
        except ValueError:
            continue
        ch = d["channels"][0]
        fails = d.get("events", {}).get("EVENT_RX_FAIL", 0)
        n = ch["packets"] + fails
        out[js.stem] = (100.0 * fails / n) if n else float("nan")
    return out


def silent_delta(win):
    """rxdiag silent count over the window from the 54L15 log, if present."""
    log = win / "log_COM3.txt"
    if not log.exists():
        return None
    vals = []
    for line in log.read_text(errors="replace").splitlines():
        if "rxdiag" in line and "silent=" in line:
            try:
                vals.append(int(line.split("silent=")[1].split()[0]))
            except (IndexError, ValueError):
                pass
    return (vals[-1] - vals[0]) if len(vals) >= 2 else None


def main(prefix):
    wins = sorted(HERE.glob(f"{prefix}_dn*"), key=lambda p: p.name)
    rx_names = sorted({k for w in wins for k in loss_rows(w)})
    hdr = f"{'dn':>6} {'hex':>6} alt5 alt3 same5 shH5 preH " + " ".join(f"{n:>8}" for n in rx_names) + "  silent"
    print(hdr)
    for w in wins:
        dn = int(w.name.split("_dn")[1])
        s5, s3 = bits(dn, 5), bits(dn, 3)
        lr = loss_rows(w)
        cells = " ".join(f"{lr.get(n, float('nan')):8.2f}" for n in rx_names)
        sd = silent_delta(w)
        print(f"{dn:6d} 0x{dn:04X} {longest_alt(s5):4d} {longest_alt(s3):4d} {longest_same(s5):5d} "
              f"{min_shift_hamming(s5):4d} {preamble_hamming(s5):4d} {cells}  {sd if sd is not None else '-'}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "p3")
