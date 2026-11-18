import json, os, re, sys
base = r'C:\wt_power_ta\archive\benchmarks\2026-11-18-power-ga'
arms = ['armA_default_quiet', 'armB_lowpower_quiet', 'armC_lowpower_oneway_quiet', 'armD_default_log']
M = 2 ** 32
def d(a, b, k):
    return (a[k] - b[k]) % M
rows = []
for arm in arms:
    p = json.load(open(os.path.join(base, arm, 'peek.json')))
    snaps = [s for s in p['diag']['snapshots'] if s['ok']]
    b, a = snaps[0]['fields'], snaps[-1]['fields']
    dt_ms = d(a, b, 'uptime_ms'); dt = dt_ms / 1000.0
    s = p['samples']
    e = json.load(open(os.path.join(base, arm, 'est.json')))
    v = json.load(open(os.path.join(base, arm, 'verify.json')))
    r = dict(arm=arm, window_s=dt, first_uptime_s=b['uptime_ms'] / 1000.0,
             hfxo=f"{s['hfxo_running']}/{s['valid']}", hfxo_ci=[round(x * 100, 1) for x in s['hfxo_ci95']],
             constlat=f"{s['constlat_on']}/{s['constlat_valid']}", constlat_ci=[round(x * 100, 1) for x in s['constlat_ci95']],
             hfxo_req_pct=round(100.0 * d(a, b, 'hfxo_req_us') / (dt_ms * 1000.0), 3),
             tx_us_s=round(d(a, b, 'radio_tx_us') / dt, 1), rx_us_s=round(d(a, b, 'radio_rx_us') / dt, 1),
             tx_ops_s=round(d(a, b, 'radio_tx_ops') / dt, 3), rx_ops_s=round(d(a, b, 'radio_rx_ops') / dt, 3),
             tx_us_per_op=round(d(a, b, 'radio_tx_us') / max(1, d(a, b, 'radio_tx_ops')), 1),
             rx_us_per_op=round(d(a, b, 'radio_rx_us') / max(1, d(a, b, 'radio_rx_ops')), 1),
             cpu_pm=round(1000.0 * d(a, b, 'cpu_active_us') / (dt_ms * 1000.0), 2),
             idle_s=round(d(a, b, 'idle_entries') / dt, 2),
             flags=hex(a['flags']), constlat_stat=a['constlat_stat'])
    r['est_keys'] = list(e.keys())
    r['est'] = e
    r['verify_keys'] = list(v.keys())
    rows.append(r)
for r in rows:
    est = r.pop('est')
    print(json.dumps(r))
    print('  est:', json.dumps(est)[:1500])
out = []
for r in rows:
    for k in ('est_keys', 'verify_keys'):
        r.pop(k, None)
    arm = r['arm']
    v = json.load(open(os.path.join(base, arm, 'verify.json')))
    r['est_total_ua_itimer3'] = json.load(open(os.path.join(base, arm, 'est.json')))['total_ua_known']
    out.append(r)
step1 = json.load(open(os.path.join(base, 'step1_mpsl_ackps', 'peek_constlat.json')))
s = step1['samples']
summ = {'label': 'ESTIMATE inputs - software proxies, no current meter', 'date': '2026-11-18',
        'step1_mpsl_ackps': {'hfxo': f"{s['hfxo_running']}/{s['valid']}", 'hfxo_ci95': s['hfxo_ci95'],
                             'constlat': f"{s['constlat_on']}/{s['constlat_valid']}", 'constlat_ci95': s['constlat_ci95'],
                             'liveness': step1['liveness']['verdict'], 'dhcsr': step1['liveness']['dhcsr']},
        'arms': out}
json.dump(summ, open(os.path.join(base, 'power_ga_summary.json'), 'w'), indent=2)
