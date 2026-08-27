"""Which airports the table cannot name, taken from real routes.

Samples live traffic, asks the route service for each callsign's two ends, and
reports the IATA codes that would still show as three letters on the dial. The
service also hands back the airport's name and municipality, so the gaps come
with enough to name them without guessing.

Caches every lookup to routes_seen.json so a re-run costs nothing and the sample
can be widened over several sittings.
"""
import io
import json
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "data"))
from airports import AIRPORTS

KNOWN = {c for c, _ in AIRPORTS}
UA = "Mozilla/5.0 (SmallTV)"
CACHE = os.path.join(HERE, "routes_seen.json")
LAT, LON = 37.4879894, 127.0109


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=25) as r:
        return json.loads(r.read().decode("utf-8"))


def load():
    if os.path.exists(CACHE):
        return json.load(io.open(CACHE, encoding="utf-8"))
    return {}


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    seen = load()

    callsigns = set()
    for _ in range(rounds):
        try:
            d = fetch("https://opendata.adsb.fi/api/v3/lat/%s/lon/%s/dist/250" % (LAT, LON))
        except Exception as exc:
            print("  feed failed: %s" % exc)
            break
        for a in d.get("ac", []):
            cs = (a.get("flight") or "").strip()
            if len(cs) >= 4 and cs[:3].isalpha() and cs[:3].isupper():
                callsigns.add(cs)
        if rounds > 1:
            time.sleep(5)

    todo = sorted(c for c in callsigns if c not in seen)
    print("  %d callsigns in the sample, %d not yet looked up" % (len(callsigns), len(todo)))
    for i, cs in enumerate(todo):
        try:
            d = fetch("https://api.adsbdb.com/v0/callsign/" + cs)
            fr = (d.get("response") or {}).get("flightroute") or {}
            rec = {}
            for end in ("origin", "destination"):
                e = fr.get(end) or {}
                if e.get("iata_code"):
                    rec[end] = [e["iata_code"], e.get("municipality") or "", e.get("name") or ""]
            seen[cs] = rec
        except Exception:
            seen[cs] = {}
        if (i + 1) % 25 == 0:
            json.dump(seen, io.open(CACHE, "w", encoding="utf-8"), ensure_ascii=False)
            print("    %d/%d" % (i + 1, len(todo)))
        time.sleep(1.1)
    json.dump(seen, io.open(CACHE, "w", encoding="utf-8"), ensure_ascii=False)

    missing = {}
    total = 0
    for rec in seen.values():
        for end in ("origin", "destination"):
            if end not in rec:
                continue
            code, muni, name = rec[end]
            total += 1
            if code not in KNOWN:
                m = missing.setdefault(code, [0, muni, name])
                m[0] += 1

    print()
    print("  %d route ends known, %d distinct airports unnamed" % (total, len(missing)))
    for code in sorted(missing, key=lambda c: -missing[c][0]):
        n, muni, name = missing[code]
        print("    %s  x%-3d %-22s %s" % (code, n, muni, name[:44]))


main()
