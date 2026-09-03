"""Put a backup onto a device, verified after every write.

THIS IS THE CANONICAL COPY. It reads manifest.json, settings.json and
album.json from its own directory, so it does not run from here - copy it
next to a backup and run it there. The copy inside a backup folder is a
snapshot; fixes belong here, where they are versioned.

It follows the same routes the web UI uses, because those are the ones that keep
the device's own bookkeeping straight: a photo is the file plus an entry in the
album index, and dropping the file in without the entry gives a device that is
holding a picture it will never show.

  py restore.py 192.168.0.1xx --wifi-pass "..."   # settings, photos, radar
  py restore.py 192.168.0.1xx                     # everything except WiFi
  py restore.py 192.168.0.1xx --dry-run           # check the backup, send nothing

Run it inside the two minute boot grace, or sign in first: /file needs a session
and the grace is the only way to have one without typing a password.

The WiFi password is not in this backup - the device never puts it on the wire,
so neither the card nor the saved profiles are ever restored without one. Pass
--wifi-pass to set the one network it belongs to, or leave it out and type them
into the web UI, which is the only place that knows them.
Flash the firmware and the LittleFS image first; see RESTORE.md.
"""
import hashlib
import io
import json
import os
import sys
import urllib.parse
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))

# Read back off the device, so they are never sent back to it.
READ_ONLY = {
    "version", "ap_active", "card_ready", "effective_brightness",
    "night_mode_active", "ssid_live", "wifi_bssid", "wifi_channel",
    "screen_count", "analog_face_count", "ow_key_set", "kma_key_set",
    "pass_set", "web_password_is_default", "screens_on", "fs_total",
    "fs_used", "fs_free",
}

# Never restored, because a backup cannot hold what the device will not send.
#
# /api/config reports the network names but never the passwords - it says
# pass_set: true and lists profiles as {"ssid": ...} with nothing else. Sending
# those back writes networks the device cannot join, and on 2026-09-02 that took
# a device off the air: it tried each name with a blank password, and until the
# firmware was fixed each attempt also overwrote the SDK's own copy of the
# credentials that had been working. The screen said "WiFi 연결 안됨" on a device
# that had been online minutes before.
#
# So WiFi is restored only when a password is supplied, and then only the one
# network that password belongs to. The profiles are left to the web UI, where
# the person typing knows the passwords.
WIFI_KEYS = {"ssid", "pass", "wifi_profiles"}


def post(url, body=None, ctype="application/json", timeout=60):
    data = body.encode("utf-8") if isinstance(body, str) else body
    req = urllib.request.Request(url, data=data or b"", method="POST")
    if data:
        req.add_header("Content-Type", ctype)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace").strip()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace").strip()


def post_file(dev, path, blob, timeout=120):
    bound = "----smalltv" + uuid.uuid4().hex
    pre = ("--%s\r\nContent-Disposition: form-data; name=\"file\"; "
           "filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n"
           % (bound, os.path.basename(path))).encode()
    body = pre + blob + ("\r\n--%s--\r\n" % bound).encode()
    url = dev + "/file?path=" + urllib.parse.quote(path) + "&size=%d" % len(blob)
    return post(url, body, "multipart/form-data; boundary=" + bound, timeout)


def get(dev, path, timeout=40):
    with urllib.request.urlopen(dev + path, timeout=timeout) as r:
        return r.status, r.read()


def load(rel, sha):
    blob = open(os.path.join(HERE, rel.replace("/", os.sep)), "rb").read()
    if hashlib.sha256(blob).hexdigest() != sha:
        raise SystemExit("!! %s does not match the manifest - stopping" % rel)
    return blob


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    dev = host if host.startswith("http") else "http://" + host
    dry = "--dry-run" in sys.argv
    wifi_pass = None
    if "--wifi-pass" in sys.argv:
        wifi_pass = sys.argv[sys.argv.index("--wifi-pass") + 1]

    man = json.load(io.open(os.path.join(HERE, "manifest.json"), encoding="utf-8"))
    settings = json.load(io.open(os.path.join(HERE, "settings.json"), encoding="utf-8"))
    album = json.load(io.open(os.path.join(HERE, "album.json"), encoding="utf-8"))

    print("backup of %s taken %s, firmware %s"
          % (man["device"], man["taken"], man["firmware"]))
    print("target %s%s" % (dev, "  (dry run)" if dry else ""))
    print()

    # Everything is checked against the manifest before anything is sent, so a
    # backup that rotted on disk fails here and not halfway through a device.
    blobs = {}
    for entry in man["photos"] + man["radar"]:
        blobs[entry["file"]] = load(entry["file"], entry["sha256"])
    print("all %d photos and backgrounds match the manifest"
          % (len(man["photos"]) + len(man["radar"])))
    if dry:
        print("dry run - nothing sent")
        return 0

    print()
    print("1. radar backgrounds")
    for entry in man["radar"]:
        blob = blobs[entry["file"]]
        status, body = post_file(dev, "/radar/%s.rgb" % entry["id"], blob)
        ok = status == 200 and "fail" not in body.lower()
        print("   %-34s %7d %s" % (entry["id"], len(blob), "ok" if ok else body))
        if not ok:
            return 1

    print()
    print("2. photos")
    for entry in man["photos"]:
        blob = blobs[entry["file"]]
        ext = ".jpg" if entry.get("fmt") == "jpg" else ".rgb"
        status, body = post_file(dev, "/album/%s%s" % (entry["id"], ext), blob)
        ok = status == 200 and "fail" not in body.lower()
        print("   %-34s %7d %s" % (entry["id"], len(blob), "ok" if ok else body))
        if not ok:
            return 1

    # The index, or the device is holding pictures it will never show.
    status, body = post(dev + "/api/album", json.dumps({
        "interval_seconds": album.get("interval_seconds", 10),
        "photos": [{"id": p["id"], "name": p.get("name"), "on": p.get("on", True)}
                   for p in man["photos"]]}))
    print("   album index: %s %s" % (status, body))
    if status != 200:
        return 1

    print()
    print("3. settings")
    body = {k: v for k, v in settings.items()
            if k not in READ_ONLY and k not in WIFI_KEYS}
    if wifi_pass is not None:
        # The name comes from the backup, the password from the command line.
        # Only this one network, and only because a password was given for it.
        body["ssid"] = settings.get("ssid", "")
        body["pass"] = wifi_pass
    status, txt = post(dev + "/api/config", json.dumps(body, ensure_ascii=False))
    print("   %s %s" % (status, txt))
    if status != 200:
        return 1
    if wifi_pass is None:
        print("   WiFi left untouched - no password to restore it with.")
        print("   The backup has the network names but not their passwords, and")
        print("   writing names without passwords is what takes a device off the")
        print("   air. Enter them in the web UI, or re-run with --wifi-pass.")
        print("   Names in the backup, for reference: %s"
              % ", ".join([settings.get("ssid") or "-"]
                          + [p.get("ssid", "?") for p in (settings.get("wifi_profiles") or [])]))

    print()
    print("4. verify")
    bad = 0
    for entry in man["radar"]:
        status, blob = get(dev, "/api/radar/bg?id=" + urllib.parse.quote(entry["id"]))
        if hashlib.sha256(blob).hexdigest() != entry["sha256"]:
            print("   radar %s reads back wrong" % entry["id"])
            bad += 1
    for entry in man["photos"]:
        status, blob = get(dev, "/api/album/photo?id=" + urllib.parse.quote(entry["id"]))
        if hashlib.sha256(blob).hexdigest() != entry["sha256"]:
            print("   photo %s reads back wrong" % entry["id"])
            bad += 1
    live = json.loads(get(dev, "/api/config")[1].decode("utf-8"))
    for k in ("ow_key", "kma_key", "screens", "radar_bg", "timezone_offset_minutes"):
        if live.get(k) != settings.get(k):
            print("   setting %s reads back as %r, expected %r"
                  % (k, live.get(k), settings.get(k)))
            bad += 1
    n = len(man["photos"]) + len(man["radar"])
    print("   %d of %d files read back byte-identical, settings checked" % (n - bad, n))

    print()
    print("Restart the device so it loads the restored config.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
