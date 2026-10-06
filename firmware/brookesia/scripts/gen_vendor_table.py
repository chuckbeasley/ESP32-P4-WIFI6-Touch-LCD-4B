#!/usr/bin/env python3
"""Build the vendor identification database for the firmware.

Downloads the IEEE OUI registry and Bluetooth SIG assigned-number lists (the same
sources Fieldwatch compiles into its radiodb.bin), adds the Fast Pair model list, and
packs them into a single binary the firmware loads at runtime from the SD card
(/sdcard/vendor_db.bin). Updating the database is then a matter of re-running this
script and copying the file onto the card -- no firmware rebuild.

Binary layout (all little-endian):
    u32 magic        "VLDB"
    u16 version      1
    u16 sections     5
    then five sections in this fixed order: OUI(24), Company(16), Service(16),
    Appearance(16), FastPair(24). Each section is:
        u32 count
        count * { u32 key, u32 name_off }   # sorted ascending by key
        u32 pool_size
        pool_size bytes of NUL-terminated names
"""
from __future__ import annotations

import csv
import io
import re
import struct
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_INC = ROOT / "components" / "VendorLookup" / "vendor_table.inc"
OUT_BIN = ROOT / "components" / "VendorLookup" / "vendor_db.bin"

OUI_URL = "https://standards-oui.ieee.org/oui/oui.csv"
CO_URL = ("https://bitbucket.org/bluetooth-SIG/public/raw/main/"
          "assigned_numbers/company_identifiers/company_identifiers.yaml")
SVC_URL = ("https://bitbucket.org/bluetooth-SIG/public/raw/main/"
           "assigned_numbers/uuids/service_uuids.yaml")
APPEAR_URL = ("https://bitbucket.org/bluetooth-SIG/public/raw/main/"
              "assigned_numbers/core/appearance_values.yaml")

MAGIC = b"VLDB"
VERSION = 1
SECTIONS = 6

# Google Fast Pair 24-bit model IDs (Fieldwatch's FastPairModels.kt, public listings).
FASTPAIR = [
    (0x000006, "Google Pixel Buds"), (0x000007, "Android Auto"),
    (0x00000B, "Google Gphones"), (0x00000C, "Google Set Up Device"),
    (0x00000F, "Google Pixel Buds A-Series"), (0x000035, "Fast Pair test device"),
    (0x000047, "Arduino 101"), (0x000048, "Fast Pair Headphones"),
    (0x000049, "Fast Pair Headphones"), (0x0000F0, "Bose QuietComfort 35 II"),
    (0x0001F0, "Bisto CSR8670"), (0x0002F0, "JBL Everest 110GA"),
    (0x0003F0, "LG HBS-835S"), (0x001000, "LG HBS1110"),
    (0x002000, "AIAIAI TMA-2"), (0x003000, "Libratone Q Adapt On-Ear"),
    (0x003001, "Libratone Q Adapt On-Ear"), (0x003B41, "M&D MW65"),
    (0x003D8A, "Cleer FLOW II"), (0x005BC3, "Panasonic RP-HD610N"),
    (0x008F7D, "soundcore Glow Mini"), (0x00A168, "boAt Airdopes 621"),
    (0x00AA48, "Jabra Elite 2"), (0x00AA91, "Beoplay E8 2.0"),
    (0x00C95C, "Sony WF-1000X"), (0x00FA72, "Pioneer SE-MS9BN"),
    (0x0100F0, "Bose QuietComfort 35 II"), (0x011242, "Nirvana Ion"),
    (0x013D8A, "Cleer EDGE Voice"), (0x01AA91, "Beoplay H9 3rd gen"),
    (0x01C95C, "Sony WF-1000X"), (0x01EEB4, "Sony WH-1000XM4"),
    (0x02AA91, "B&O Earset"), (0x02C95C, "Sony WH-1000XM2"),
    (0x02D815, "Audio-Technica ATH-CK1TW"), (0x02D886, "JBL Reflect Mini NC"),
    (0x02DD4F, "JBL Tune 770NC"), (0x02E2A9, "TCL MOVEAUDIO S200"),
    (0x02F637, "JBL Live Flex"), (0x035754, "Plantronics PLT K2"),
    (0x035764, "Plantronics V8200"), (0x038B91, "DENON AH-C830NCW"),
    (0x038CC7, "JBL Tune 760NC"), (0x038F16, "Beats Studio Buds"),
    (0x03AA91, "B&O Beoplay H8i"), (0x03C95C, "Sony WH-1000XM2"),
    (0x03C99C, "Moto Buds 135"), (0x045754, "Plantronics PLT K2"),
    (0x04AA91, "Beoplay H4"), (0x04ACFC, "JBL Wave Beam"),
    (0x04AFB8, "JBL Tune 720BT"), (0x04C95C, "Sony WI-1000X"),
    (0x050F0C, "Marshall Major III Voice"), (0x052CC7, "Marshall Minor III"),
    (0x054B2D, "JBL Tune 125TWS"), (0x0577B1, "Galaxy S23 Ultra"),
    (0x057802, "TicWatch Pro 5"), (0x0582FD, "Google Pixel Buds"),
    (0x058D08, "Sony WH-1000XM4"), (0x05A963, "Wonderboom 3"),
    (0x05A9BC, "Galaxy S20+"), (0x05AA91, "B&O Beoplay E6"),
    (0x05C452, "JBL Live 220BT"), (0x05C95C, "Sony WI-1000X"),
    (0x060000, "Google Pixel Buds"), (0x0660D7, "JBL Live 770NC"),
    (0x06AE20, "Galaxy S21 5G"), (0x06C197, "OPPO Enco Air3 Pro"),
    (0x06C95C, "Sony WH-1000XM2"), (0x06D8FC, "soundcore Liberty 4 NC"),
    (0x0744B6, "Technics EAH-AZ60M2"), (0x07A41C, "Sony WF-C700N"),
    (0x07C95C, "Sony WH-1000XM2"), (0x07F426, "Nest Hub Max"),
    (0x0E30C3, "Razer Hammerhead TWS"), (0x2D7A23, "Sony WF-1000XM4"),
    (0x30018E, "Google Pixel Buds Pro 2"), (0x718FA4, "JBL Live 300TWS"),
    (0x72EF8D, "Razer Hammerhead TWS X"), (0x72FB00, "soundcore Spirit Pro"),
    (0x821F66, "JBL Flip 6"), (0x92BBBD, "Google Pixel Buds"),
    (0x9D3F8A, "soundcore Liberty 4"), (0xAE3989, "Xiaomi Redmi Buds 5 Pro"),
    (0xCD8256, "Bose Noise Cancelling 700"), (0xD0A72C, "Nothing Ear (a)"),
    (0xD446A7, "Sony WH-1000XM5"), (0xD446F9, "Jabra Elite 8 Active"),
    (0xD5BC6B, "Sony WH-1000XM6"), (0xD97EBA, "OnePlus Nord Buds 3 Pro"),
    (0xF00000, "Bose QuietComfort 35 II"), (0xF00002, "Bose QuietComfort Earbuds II"),
    (0xF00200, "JBL Everest 110GA"), (0xF00203, "JBL Everest 310GA"),
    (0xF00207, "JBL Everest 710GA"), (0xF00209, "JBL Live 400BT"),
    (0xF0020E, "JBL Live 500BT"), (0xF00213, "JBL Live 650BTNC"),
    (0xF00300, "LG HBS-835S"), (0xF00301, "LG HBS-835"),
    (0xF00302, "LG HBS-830"), (0xF00303, "LG HBS-930"),
    (0xF00304, "LG HBS-1010"), (0xF00305, "LG HBS-1500"),
    (0xF00306, "LG HBS-1700"), (0xF00307, "LG HBS-1120"),
    (0xF00308, "LG HBS-1125"), (0xF00309, "LG HBS-2000"),
    (0xF0B77F, "soundcore Liberty 4 NC"), (0xF52494, "JBL Buds Pro"),
]

# Classification rules, each 12 bytes: (ad_type, key, pat_off, pat_len, pat, min_len,
# action, arg). ad_type is the AD field type (0xFF manufacturer, 0x16 service data,
# 0x19 appearance); key is the company ID / UUID / appearance (little-endian as on the
# wire, matching body[0..1]); pat is matched at body[2 + pat_off]; min_len is a minimum
# body length; action 0 assigns a kind (arg = kind id), action 1 sets a flag bit
# (arg = bit index). Kind ids match ble_toolbox_adv_kind_t; flags: 0=FindMy, 1=Nearby,
# 2=glasses.
RULES = [
    # Immediate kinds (first match wins).
    (0xFF, 0x004C, 0, 2, b"\x02\x15", 0, 0, 1),    # iBeacon
    (0xFF, 0xFEAA, 2, 1, b"\x00", 0, 0, 2),        # Eddystone UID
    (0xFF, 0xFEAA, 2, 1, b"\x10", 0, 0, 3),        # Eddystone URL
    (0xFF, 0xFEAA, 2, 1, b"\x20", 0, 0, 4),        # Eddystone TLM
    (0xFF, 0x0157, 0, 0, b"", 0, 0, 10),           # Tile
    (0x16, 0xFE2C, 0, 0, b"", 0, 0, 8),            # Fast Pair
    (0x16, 0xFD6F, 0, 0, b"", 0, 0, 9),            # Exposure Notification
    # Deferred flags (resolved after the whole advertisement is walked).
    (0xFF, 0x004C, 0, 1, b"\x12", 0, 1, 0),        # FindMy
    (0xFF, 0x004C, 0, 1, b"\x10", 0, 1, 1),        # Nearby
    (0xFF, 0x004C, 0, 1, b"\x0F", 0, 1, 1),        # Nearby
    (0xFF, 0x004C, 0, 1, b"\x10", 0x19, 1, 2),     # Nearby + long -> glasses
    (0xFF, 0x004C, 0, 1, b"\x0F", 0x19, 1, 2),     # Nearby + long -> glasses
    (0x19, 0x0C80, 0, 0, b"", 0, 1, 2),            # appearance -> glasses
    (0x19, 0x0C81, 0, 0, b"", 0, 1, 2),            # appearance -> glasses
    (0x19, 0x0C82, 0, 0, b"", 0, 1, 2),            # appearance -> glasses
]


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "esp-vendor-lookup/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def sanitize(s: str) -> str:
    s = s.strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "'\"":
        s = s[1:-1].strip()
    s = re.sub(r"\s+", " ", s)
    return s.replace("\\", "\\\\").replace('"', '\\"')


def parse_oui(data: bytes) -> list[tuple[int, str]]:
    rows = []
    for line in csv.reader(io.StringIO(data.decode("utf-8", "replace"))):
        if len(line) < 3:
            continue
        reg, assign, org = line[0], line[1], line[2]
        if reg != "MA-L" or not re.fullmatch(r"[0-9A-Fa-f]{6}", assign or ""):
            continue
        name = sanitize(org)
        if name and name not in ("IEEE Registration Authority",):
            rows.append((int(assign, 16), name))
    return rows


def parse_company(data: bytes) -> list[tuple[int, str]]:
    rows = []
    value = None
    for line in data.decode("utf-8", "replace").splitlines():
        s = line.strip()
        if s.startswith("- value:"):
            value = int(s.split(":", 1)[1].strip(), 16)
        elif s.startswith("name:") and value is not None:
            name = sanitize(s.split(":", 1)[1])
            if name:
                rows.append((value, name))
            value = None
    return rows


def parse_service(data: bytes) -> list[tuple[int, str]]:
    rows = []
    uuid = None
    for line in data.decode("utf-8", "replace").splitlines():
        s = line.strip()
        if s.startswith("- uuid:"):
            uuid = int(s.split(":", 1)[1].strip(), 16)
        elif s.startswith("name:") and uuid is not None:
            name = sanitize(s.split(":", 1)[1])
            if name and 0 <= uuid < 0x10000:
                rows.append((uuid, name))
            uuid = None
    return rows


def parse_appearance(data: bytes) -> list[tuple[int, str]]:
    rows = []
    cat = None
    cat_name = None
    pending_sub = None
    in_sub = False
    for line in data.decode("utf-8", "replace").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        s = line.strip()
        if s.startswith("- category:"):
            cat = int(s.split(":", 1)[1].strip(), 16)
            cat_name = None
            in_sub = False
            pending_sub = None
        elif s.startswith("name:") and cat is not None and not in_sub and cat_name is None:
            cat_name = sanitize(s.split(":", 1)[1])
            if cat_name:
                rows.append((cat << 6, cat_name))
        elif s.startswith("subcategory:"):
            in_sub = True
        elif in_sub and s.startswith("- value:"):
            pending_sub = int(s.split(":", 1)[1].strip(), 16)
        elif (in_sub and s.startswith("name:") and cat is not None and cat_name is not None
              and pending_sub is not None):
            sub_name = sanitize(s.split(":", 1)[1])
            if sub_name:
                rows.append(((cat << 6) | pending_sub, f"{cat_name} / {sub_name}"))
    return rows


def pack_section(rows: list[tuple[int, str]]) -> bytes:
    """Pack one section: count, sorted {key, off} records, then the name pool."""
    rows = sorted(set(rows))
    out = bytearray()
    out += struct.pack("<I", len(rows))

    # Lay out the string pool first so offsets are known.
    pool = bytearray()
    records = []
    for key, name in rows:
        name_bytes = name.encode("utf-8") + b"\0"
        off = len(pool)
        pool += name_bytes
        records.append((key, off))

    for key, off in records:
        out += struct.pack("<II", key, off)
    out += struct.pack("<I", len(pool))
    out += pool
    return bytes(out)


def pack_rules(rules: list[tuple]) -> bytes:
    """Pack the classification rules: count, then 12 bytes per rule."""
    out = bytearray()
    out += struct.pack("<I", len(rules))
    for ad_type, key, pat_off, pat_len, pat, min_len, action, arg in rules:
        pat = (pat + b"\x00" * 4)[:4]
        out.append(ad_type)
        out += struct.pack("<H", key)
        out.append(pat_off)
        out.append(pat_len)
        out += pat
        out.append(min_len)
        out.append(action)
        out.append(arg)
    return bytes(out)


def main() -> int:
    sections = [
        parse_oui(fetch(OUI_URL)),
        parse_company(fetch(CO_URL)),
        parse_service(fetch(SVC_URL)),
        parse_appearance(fetch(APPEAR_URL)),
        list(FASTPAIR),
    ]

    blob = bytearray()
    blob += MAGIC
    blob += struct.pack("<HH", VERSION, SECTIONS)
    for rows in sections:
        blob += pack_section(rows)
    blob += pack_rules(RULES)

    OUT_BIN.write_bytes(blob)
    print("wrote %s (%d bytes, %d sections, %d rules)"
          % (OUT_BIN, len(blob), SECTIONS, len(RULES)))
    for name, rows in zip(("OUI", "Company", "Service", "Appearance", "FastPair"), sections):
        print("  %-10s %d entries" % (name, len(rows)))
    print("  %-10s %d entries" % ("Rules", len(RULES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
