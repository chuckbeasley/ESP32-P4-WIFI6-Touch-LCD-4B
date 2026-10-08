#!/usr/bin/env python3
"""Sync Fieldwatch data into the firmware (no rebuild).

Downloads the signature catalog and the Fast Pair list from Fieldwatch upstream, detects
what changed, copies the signature JSON onto the SD card, and regenerates vendor_db.json
when the Fast Pair list changed. Run on a machine with the SD card mounted as a drive.

Usage:
    python scripts/sync_fieldwatch.py [--drive F:] [--force]

Files touched:
    components/VendorLookup/fieldwatch-signatures-v2.json   (downloaded, copied to SD)
    scripts/fastpair_models.json                            (parsed Fast Pair list)
    components/VendorLookup/vendor_db.json                  (regenerated when needed)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "components" / "VendorLookup"
SCRIPTS = ROOT / "scripts"

SIG_URL = ("https://raw.githubusercontent.com/OffGridPete/Fieldwatch/main/"
           "dist/fieldwatch-signatures-v2.json")
FP_URL = ("https://raw.githubusercontent.com/OffGridPete/Fieldwatch/main/"
          "app/src/main/java/app/fieldwatch/domain/FastPairModels.kt")

SIG_LOCAL = VENDOR / "fieldwatch-signatures-v2.json"
FP_LOCAL = SCRIPTS / "fastpair_models.json"
DB_LOCAL = VENDOR / "vendor_db.json"


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "esp-fieldwatch-sync/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_fastpair(text: str) -> list[list]:
    out = []
    for m in re.finditer(r'0x([0-9A-Fa-f]+)\s+to\s+"([^"]*)"', text):
        out.append([int(m.group(1), 16), m.group(2)])
    return out


def sd_root(drive: str | None) -> Path | None:
    if drive is None or drive == "":
        return None
    d = drive if drive.endswith(":") or drive.endswith(":\\") else drive + ":"
    p = Path(d + "\\")
    return p if p.exists() else None


def copy_to_sd(src: Path, sd: Path, name: str, done: list[str]) -> None:
    try:
        dst = sd / name
        shutil.copyfile(src, dst)
        done.append(str(dst))
    except OSError as e:
        print(f"  !! could not copy to SD: {e}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drive", default="F:",
                    help="SD card drive letter (default F:); pass an empty string to skip the SD copy")
    ap.add_argument("--force", action="store_true",
                    help="re-copy / regenerate even if unchanged")
    args = ap.parse_args()

    print("fetching Fieldwatch upstream ...")
    sig = fetch(SIG_URL)
    fp = parse_fastpair(fetch(FP_URL).decode("utf-8", "replace"))
    print(f"  signatures: {len(sig)} bytes, Fast Pair: {len(fp)} models")

    fp_old = None
    if FP_LOCAL.exists():
        try:
            fp_old = json.loads(FP_LOCAL.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            fp_old = None
    fp_changed = args.force or (fp_old != fp)
    FP_LOCAL.write_text(json.dumps(fp, ensure_ascii=False), encoding="utf-8")

    sig_changed = args.force
    if SIG_LOCAL.exists():
        sig_changed = sig_changed or (sha256(SIG_LOCAL.read_bytes()) != sha256(sig))
    SIG_LOCAL.write_bytes(sig)

    if not sig_changed and not fp_changed:
        print("no changes (already up to date)")
        return 0

    sd = sd_root(args.drive if args.drive else None)
    done: list[str] = []

    if sig_changed:
        print("signature catalog changed -> updating")
        if sd is not None:
            copy_to_sd(SIG_LOCAL, sd, "fieldwatch-signatures-v2.json", done)
        else:
            print(f"  (SD drive not mounted; saved locally at {SIG_LOCAL})")

    if fp_changed:
        print("Fast Pair list changed -> regenerating vendor_db.json")
        subprocess.run([sys.executable, str(SCRIPTS / "gen_vendor_table.py")], check=True)
        if sd is not None:
            copy_to_sd(DB_LOCAL, sd, "vendor_db.json", done)
        else:
            print(f"  (SD drive not mounted; saved locally at {DB_LOCAL})")

    if done:
        print("copied to SD card:")
        for p in done:
            print(f"  {p}")
    print("reboot the device to reload")
    return 0


if __name__ == "__main__":
    sys.exit(main())
