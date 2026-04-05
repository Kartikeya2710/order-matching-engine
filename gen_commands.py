import argparse
import random
import csv
import os
import sys


def load_instruments(path):
    instruments = []
    with open(path) as f:
        for line in f:
            line = line.split('#')[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 5:
                continue
            instruments.append({
                'id': int(parts[0]),
                'min_price': int(parts[2]),
                'max_price': int(parts[3]),
                'tick_size': int(parts[4]),
                'mid': (int(parts[2]) + int(parts[3])) / 2.0,
            })
    return instruments


def align(price, inst):
    ts = inst['tick_size']
    p = int(round(price / ts) * ts)
    return max(inst['min_price'], min(inst['max_price'], p))


def generate(instruments, n, duration_ms, seed,
             add_ratio=0.60, cancel_ratio=0.25,
             market_ratio=0.08, ioc_ratio=0.05, spread_ticks=5):
    rng = random.Random(seed)
    cmds = []
    open_orders = {}
    next_oid = 1
    dur_ns = duration_ms * 1_000_000

    # Timestamps: uniform arrivals with occasional bursts
    times = []
    t = 0
    while len(times) < n:
        gap = max(100, int(rng.expovariate(n / dur_ns)))
        if rng.random() < 0.05:
            for _ in range(rng.randint(5, 30)):
                if len(times) >= n:
                    break
                times.append(t)
                t += rng.randint(50, 500)
        else:
            t += gap
            times.append(t)
    times.sort()
    if times and times[-1] > 0:
        scale = dur_ns / times[-1]
        times = [int(ts * scale) for ts in times]
    times = times[:n]

    for ts in times:
        inst = rng.choice(instruments)
        inst['mid'] += rng.gauss(0, inst['tick_size'] * 2)
        inst['mid'] = max(inst['min_price'] + spread_ticks * inst['tick_size'],
                          min(inst['max_price'] - spread_ticks * inst['tick_size'],
                              inst['mid']))

        r = rng.random()
        if r < add_ratio or not open_orders:
            oid = next_oid
            next_oid += 1
            cid = rng.randint(1, 100)
            verb = 'Buy' if rng.random() < 0.5 else 'Sell'

            if rng.random() < market_ratio:
                cmds.append({
                    'timestamp_ns': ts, 'type': 'Add', 'order_id': oid,
                    'instrument_id': inst['id'], 'client_id': cid,
                    'tif': 'None', 'order_type': 'Market', 'verb': verb,
                    'limit_price': 0, 'stop_price': 0,
                    'qty': rng.randint(1, 50),
                })
            else:
                offset = abs(rng.gauss(0, spread_ticks * inst['tick_size']))
                if verb == 'Buy':
                    price = inst['mid'] - offset - rng.randint(0, spread_ticks) * inst['tick_size']
                else:
                    price = inst['mid'] + offset + rng.randint(0, spread_ticks) * inst['tick_size']
                lp = align(price, inst)
                tif = 'IOC' if rng.random() < ioc_ratio else 'GTC'
                qty = rng.choice([1, 5, 10, 25, 50, 100, 200, 500])
                cmds.append({
                    'timestamp_ns': ts, 'type': 'Add', 'order_id': oid,
                    'instrument_id': inst['id'], 'client_id': cid,
                    'tif': tif, 'order_type': 'Limit', 'verb': verb,
                    'limit_price': lp, 'stop_price': 0, 'qty': qty,
                })
                if tif == 'GTC':
                    open_orders[oid] = {'iid': inst['id'], 'cid': cid,
                                        'verb': verb, 'price': lp, 'qty': qty}

        elif r < add_ratio + cancel_ratio:
            oid = rng.choice(list(open_orders.keys()))
            o = open_orders.pop(oid)
            cmds.append({
                'timestamp_ns': ts, 'type': 'Cancel', 'order_id': oid,
                'instrument_id': o['iid'], 'client_id': o['cid'],
                'tif': 'GTC', 'order_type': 'Limit', 'verb': o['verb'],
                'limit_price': o['price'], 'stop_price': 0, 'qty': o['qty'],
            })

        else:  # Modify
            oid = rng.choice(list(open_orders.keys()))
            o = open_orders[oid]
            inst_m = next((x for x in instruments if x['id'] == o['iid']), instruments[0])
            if rng.random() < 0.5:
                new_qty = max(1, o['qty'] - rng.randint(1, max(1, o['qty'] // 2)))
                new_price = o['price']
            else:
                new_price = align(o['price'] + rng.choice([-2, -1, 1, 2]) * inst_m['tick_size'], inst_m)
                new_qty = o['qty']
            cmds.append({
                'timestamp_ns': ts, 'type': 'Modify', 'order_id': oid,
                'instrument_id': o['iid'], 'client_id': o['cid'],
                'tif': 'GTC', 'order_type': 'Limit', 'verb': o['verb'],
                'limit_price': new_price, 'stop_price': 0, 'qty': new_qty,
            })
            o['price'], o['qty'] = new_price, new_qty

    return cmds


def write_csv(cmds, path):
    fields = ['timestamp_ns', 'type', 'order_id', 'instrument_id', 'client_id',
              'tif', 'order_type', 'verb', 'limit_price', 'stop_price', 'qty']
    with open(path, 'w', newline='') as f:
        f.write('# ' + ','.join(fields) + '\n')
        w = csv.DictWriter(f, fieldnames=fields)
        for c in cmds:
            w.writerow(c)


def main():
    p = argparse.ArgumentParser(description='Generate commands for sim_runner')
    p.add_argument('--instruments', default='instruments.cfg')
    p.add_argument('-o', '--output', default='commands.csv')
    p.add_argument('-n', '--num-commands', type=int, default=50000)
    p.add_argument('--duration-ms', type=int, default=10000)
    p.add_argument('--seed', type=int, default=42)
    args = p.parse_args()

    if not os.path.exists(args.instruments):
        print(f"error: {args.instruments} not found", file=sys.stderr)
        sys.exit(1)

    instruments = load_instruments(args.instruments)
    print(f"loaded {len(instruments)} instrument(s)")
    print(f"generating {args.num_commands} commands (seed={args.seed})...")
    cmds = generate(instruments, args.num_commands, args.duration_ms, args.seed)
    write_csv(cmds, args.output)

    types = {}
    for c in cmds:
        types[c['type']] = types.get(c['type'], 0) + 1
    print(f"wrote {len(cmds)} commands -> {args.output}")
    for t, cnt in sorted(types.items()):
        print(f"  {t:8s} {cnt:7d}  ({100 * cnt / len(cmds):.1f}%)")
    if cmds:
        dur_ms = cmds[-1]['timestamp_ns'] / 1e6
        print(f"  duration: {dur_ms:.1f} ms, avg rate: {len(cmds) / (dur_ms / 1000):.0f} cmds/sec")


if __name__ == '__main__':
    main()