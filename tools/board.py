#!/usr/bin/env python3
"""Summarise the live Speed Demon board: per track, how many teams have runs and
the best K values, then every run of one team.

    python tools/board.py                 # field summary
    python tools/board.py --team manusingh090
"""
import argparse
import json
import urllib.request

FEED = "https://s3.amazonaws.com/raptors.dev/speeddemon.dev/2026/board/board.json"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--team", help="show this team's latest and best run per track (substring match)")
    args = ap.parse_args()
    req = urllib.request.Request(FEED, headers={"User-Agent": "speed-demon-board-summary/1.0"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        board = json.load(resp)
    teams = board["teams"]
    print(f"generated {board['generated_at']}, {len(teams)} teams, queue {board.get('queue')}")
    for t in "abcd":
        tracks = [x["tracks"].get(t, {}) for x in teams]
        tried = sum(1 for x in tracks if (x.get("latest") or {}).get("stage") not in (None, "unchanged"))
        best = sorted((x["best"] for x in tracks if x.get("best")), key=lambda r: -r["K"])
        top = ", ".join(f"K={r['K']:.1f} T={r['T_s']:.2f}s M={r['M_bytes'] >> 20}MB S={r['S']}" for r in best[:3])
        print(f"track {t}: {tried} teams ran, {len(best)} scored. top: {top}")
    if args.team:
        for x in teams:
            if args.team.lower() not in x["team"].lower():
                continue
            print(f"\nteam {x['team']} ({x['slug']})")
            for t, tr in sorted(x["tracks"].items()):
                for kind in ("latest", "best"):
                    r = tr.get(kind)
                    if not r:
                        continue
                    print(f"  {t} {kind:6} {r['run_id']} {r['stage']:9} commit {r['commit'][:7]} "
                          f"K={r.get('K')} T={r.get('T_s')} M={r.get('M_bytes')} S={r.get('S')} "
                          f"D={r.get('D')} points={r.get('points')} failure={r.get('failure')}")


if __name__ == "__main__":
    main()
