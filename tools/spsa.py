#!/usr/bin/env python3
"""Tune the parameters in src/tune.h with SPSA, playing games through fastchess.

    clang -O3 -flto -march=x86-64 -DTUNE -DBUILD='"tune"' src/*.c -o cwtch_tune -lm -lpthread -lzstd
    tools/spsa.py --engine ./cwtch_tune --book books/UHO_Lichess_4852_v1.epd --iterations 2000

Progress is written to --out after every iteration and resumed from it, so a run
can be stopped and restarted at any time. Results are in the file's "params".
"""
import argparse
import json
import math
import os
import random
import re
import subprocess
import tempfile


def read_tunables(path):
    text = open(path).read()
    found = re.findall(r"X\((\w+), (-?\d+), (-?\d+), (-?\d+), (\d+)\)", text)
    return [dict(name=n, value=float(d), low=int(lo), high=int(hi), step=int(s)) for n, d, lo, hi, s in found]


def play(args, plus, minus, workdir):
    def engine(name, values):
        options = [f"option.{k}={v}" for k, v in values.items()]
        return ["-engine", f"cmd={args.engine}", "proto=uci", f"tc={args.tc}", "timemargin=250",
                f"option.Hash={args.hash}", f"name={name}"] + options

    command = [args.fastchess, "-concurrency", str(args.concurrency), "-rounds", str(args.pairs),
               "-repeat", "-resign", "movecount=5", "score=600", "-draw", "movenumber=40", "movecount=8", "score=5",
               *engine("plus", plus), *engine("minus", minus),
               "-openings", f"file={args.book}", "format=epd", "order=random", "-srand", str(random.randrange(1 << 30))]
    out = subprocess.run(command, cwd=workdir, capture_output=True, text=True, errors="ignore").stdout

    results = re.findall(r"Games: (\d+), Wins: (\d+), Losses: (\d+), Draws: (\d+)", out)
    if not results:
        raise RuntimeError("no result from fastchess:\n" + out[-2000:])
    games, wins, losses, draws = map(int, results[-1])
    return games, wins, losses, draws


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--book", required=True)
    parser.add_argument("--fastchess", default=os.path.join(root, "bin", "fastchess"))
    parser.add_argument("--tunables", default=os.path.join(root, "src", "tune.h"))
    parser.add_argument("--out", default="spsa.json")
    parser.add_argument("--iterations", type=int, default=2000)
    parser.add_argument("--tc", default="3+0.03")
    parser.add_argument("--hash", type=int, default=8)
    parser.add_argument("--concurrency", type=int, default=13)
    parser.add_argument("--pairs", type=int, default=8, help="game pairs per iteration")
    parser.add_argument("--alpha", type=float, default=0.602)
    parser.add_argument("--gamma", type=float, default=0.101)
    parser.add_argument("--A", type=float, default=None, help="default: 10%% of iterations")
    parser.add_argument("--a-end", type=float, default=2.0, help="final move per unit of gradient, in steps")
    args = parser.parse_args()

    args.engine = os.path.abspath(args.engine)
    args.book = os.path.abspath(args.book)
    args.fastchess = os.path.abspath(args.fastchess)
    args.out = os.path.abspath(args.out)

    params = read_tunables(args.tunables)
    state = dict(iteration=0, params={p["name"]: p["value"] for p in params}, games=0, wins=0, losses=0, draws=0)
    if os.path.exists(args.out):
        state = json.load(open(args.out))

    big_a = args.A if args.A is not None else args.iterations * 0.1

    with tempfile.TemporaryDirectory() as workdir:
        while state["iteration"] < args.iterations:
            k = state["iteration"] + 1

            # c shrinks slowly, a faster: standard SPSA gains, scaled per parameter by its step
            c_k = (args.iterations / k) ** args.gamma
            a_k = args.a_end * ((big_a + args.iterations) / (big_a + k)) ** args.alpha

            deltas, plus, minus = {}, {}, {}
            for p in params:
                name = p["name"]
                deltas[name] = random.choice((-1, 1))
                shift = p["step"] * c_k * deltas[name]
                plus[name] = round(min(p["high"], max(p["low"], state["params"][name] + shift)))
                minus[name] = round(min(p["high"], max(p["low"], state["params"][name] - shift)))

            games, wins, losses, draws = play(args, plus, minus, workdir)
            score = (wins - losses) / max(games, 1)

            for p in params:
                name = p["name"]
                # gradient step in units of the parameter's own step size
                move = a_k * score * deltas[name] / c_k * p["step"]
                state["params"][name] = min(p["high"], max(p["low"], state["params"][name] + move))

            state["iteration"] = k
            state["games"] += games
            state["wins"] += wins
            state["losses"] += losses
            state["draws"] += draws

            with open(args.out + ".tmp", "w") as f:
                json.dump(state, f, indent=1)
            os.replace(args.out + ".tmp", args.out)

            if k % 10 == 0 or k == 1:
                print(f"iter {k}/{args.iterations} games {state['games']} a {a_k:.2f} c {c_k:.2f}", flush=True)


if __name__ == "__main__":
    main()
