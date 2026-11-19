import json, sys, math, collections
TICK = 1/32768
def load(path):
    d = json.load(open(path))
    ch = d['channels'][0]
    iv = ch['g1_radio_intervals_s']
    return iv, ch
def analyse(path, nb=8):
    iv, ch = load(path)
    P0 = 8070*TICK
    t=[0.0]
    for x in iv: t.append(t[-1]+x)
    k=[round(x/P0) for x in t]
    # refine period by LSQ
    n=len(t); mk=sum(k)/n; mt=sum(t)/n
    P=sum((ki-mk)*(ti-mt) for ki,ti in zip(k,t))/sum((ki-mk)**2 for ki in k)
    a=mt-P*mk
    k=[round((x-a)/P) for x in t]
    got=set(k); K=max(k)
    miss=[i for i in range(K+1) if i not in got]
    # run lengths
    runs=collections.Counter(); r=0
    for i in range(K+1):
        if i in got:
            if r: runs[r]+=1
            r=0
        else: r+=1
    # residual sign: residual = t - (a + kP) in us
    res={ki:(ti-(a+ki*P))*1e6 for ki,ti in zip(k,t)}
    # phase of predicted arrival vs receiver tick grid
    def ph(i): 
        x=(a+i*P)/TICK; return x-math.floor(x)
    bins=[[0,0] for _ in range(nb)]
    for i in range(K+1):
        b=int(ph(i)*nb)%nb
        bins[b][0]+=1
        if i not in got: bins[b][1]+=1
    # minute histogram
    mins=[0]*11
    for i in miss: mins[min(10,int(i*P/60))]+=1
    # residual of packet right after a miss vs others
    after=[res[i] for i in got if (i-1) not in got and i>0]
    other=[res[i] for i in got if (i-1) in got]
    # beat: phase drift per period, in ticks
    drift=(P/TICK)-round(P/TICK)
    print(f"{path.split('/')[-1]}: n={n} slots={K+1} miss={len(miss)} P={P*1e6:.3f}us drift/period={drift*TICK*1e6:+.3f}us beat={abs(1/drift) if drift else 0:.1f} periods")
    print("  runs", dict(sorted(runs.items())))
    print("  phase bins (slots,miss):", " ".join(f"{s}/{m}" for s,m in bins))
    print("  minutes", mins)
    if after: print(f"  resid after miss mean {sum(after)/len(after):+.1f}us  others {sum(other)/len(other):+.1f}us; resid set after: {sorted(collections.Counter(round(x) for x in after).items())[:8]}")
    return miss, P, a
if __name__=='__main__':
    for p in sys.argv[1:]: analyse(p)
