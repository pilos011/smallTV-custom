"""Which callsigns over Korea the airline table cannot name.

Guessing what an ICAO designator stands for is how a dial ends up confidently
wrong, so the answer comes from the route service: it returns the operator for
every callsign it knows. Sampled from live traffic rather than imagined, so the
gaps found are the gaps that actually show on the screen.
"""
import glob
import io
import json
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "data"))

# airlines.py chatters on import, and calls sys.stdout.reconfigure while doing
# it, so a StringIO cannot stand in. Swallow the writes instead.
class _Quiet:
    encoding = "utf-8"

    def write(self, _):
        return 0

    def flush(self):
        pass

    def reconfigure(self, **_):
        pass


_real = sys.stdout
sys.stdout = _Quiet()
from airlines import AIRLINES
sys.stdout = _real

KNOWN = {c for c, _ in AIRLINES}
UA = "Mozilla/5.0 (SmallTV)"


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode("utf-8"))


def main():
    example = {}
    count = {}
    for f in sorted(glob.glob(os.path.join(HERE, "sample_*.json"))):
        for a in json.load(io.open(f, encoding="utf-8")).get("ac", []):
            cs = (a.get("flight") or "").strip()
            if len(cs) < 4:
                continue
            p = cs[:3]
            if not (p.isalpha() and p.isupper()):
                continue
            count[p] = count.get(p, 0) + 1
            if p not in KNOWN:
                example.setdefault(p, cs)

    print("  %d distinct prefixes seen, %d of them unnamed" % (len(count), len(example)))
    print()
    for p in sorted(example):
        cs = example[p]
        name = country = "?"
        try:
            d = fetch("https://api.adsbdb.com/v0/callsign/" + cs)
            al = ((d.get("response") or {}).get("flightroute") or {}).get("airline") or {}
            name = al.get("name") or "?"
            country = al.get("country") or ""
        except Exception as exc:
            name = "lookup failed: %s" % exc
        print("  %s  x%-3d %-10s %-34s %s" % (p, count[p], cs, name, country))
        time.sleep(1.2)


main()
