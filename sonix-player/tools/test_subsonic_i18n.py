#!/usr/bin/env python3
"""Validate Navidrome catalog coverage and printf arguments on both devices."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "sonix-packer/assets"
FORMAT = re.compile(r"%(?:[-+ #0]*)(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn%]")


def catalog(path):
    entries = {}
    count = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith((";", "#", "[")) or "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if key.startswith("navidrome_"):
            assert key not in entries, (path, "duplicate key", key)
        entries[key] = value
        count += bool(value)
    assert count < 2048, (path, "language table is full", count)
    return entries


def formats(text):
    # Width and precision also affect varargs; keep the complete specifier.
    return [match for match in FORMAT.findall(text) if match != "%%"]


def main():
    english = catalog(ASSETS / "R1/usr/resource/sonix/language/English.ini")
    expected = {key for key in english if key.startswith("navidrome_")}
    used = set()
    for path in (ROOT / "sonix-player/src").rglob("*.c"):
        used.update(re.findall(r'"(navidrome_[a-z0-9_]+)"', path.read_text()))
    assert expected == used, ("missing or unused keys", used - expected, expected - used)
    locales = None
    reference = {}
    for model in ("R1", "R3PII"):
        paths = sorted((ASSETS / model / "usr/resource/sonix/language").glob("*.ini"))
        names = {path.stem for path in paths}
        if locales is None:
            locales = names
        assert names == locales, (model, "language lists differ")
        for path in paths:
            values = catalog(path)
            actual = {key for key in values if key.startswith("navidrome_")}
            assert actual == expected, (path, "missing/extra keys", expected - actual, actual - expected)
            for key in expected:
                assert values[key], (path, key, "empty translation")
                assert formats(values[key]) == formats(english[key]), (path, key, "printf mismatch")
            for key in ("password", "remove", "timeset_not_set", "Scarica"):
                assert values.get(key), (path, "missing reused translation", key)
            translated = {key: values[key] for key in expected}
            if model == "R1":
                reference[path.stem] = translated
            else:
                assert translated == reference[path.stem], (path, "device catalogs differ")
    print(f"Navidrome i18n: {len(expected)} keys, {len(locales)} languages, both device catalogs passed")


if __name__ == "__main__":
    main()
