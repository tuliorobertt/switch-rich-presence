"""Merge public regional TitleDB catalogs into the local game and icon maps.

Existing names and images take priority; regional catalogs fill gaps only.
Run from the project root: python scripts/update_catalog.py
"""
import json
import os
from pathlib import Path
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
# Order matters: earlier regions win. Asian and European catalogs cover
# releases (and Title IDs) that never shipped in the Americas.
SOURCES = ("BR.pt.json", "US.en.json", "GB.en.json", "JP.ja.json",
           "HK.zh.json", "KR.ko.json", "DE.de.json", "FR.fr.json")
BASE_URL = "https://raw.githubusercontent.com/blawar/titledb/master/"


def read_map(name):
    with (ROOT / name).open(encoding="utf-8-sig") as f:
        result = json.load(f)
    if not isinstance(result, dict):
        raise ValueError(f"{name} deve conter um objeto JSON")
    return result


def write_map(name, values):
    path = ROOT / name
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=ROOT,
                                     prefix=path.name + ".", suffix=".tmp", delete=False) as f:
        temporary = Path(f.name)
        try:
            json.dump(dict(sorted(values.items())), f, ensure_ascii=False, indent=2)
            f.write("\n")
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    names, icons = read_map("games.json"), read_map("game-icons.json")
    old_names, old_icons = len(names), len(icons)
    for filename in SOURCES:
        print(f"Baixando {filename}...", flush=True)
        request = urllib.request.Request(BASE_URL + filename, headers={"User-Agent": "SwitchRichPresence/1.0"})
        with urllib.request.urlopen(request, timeout=90) as response:
            database = json.load(response)
        if not isinstance(database, dict):
            raise ValueError(f"Formato inesperado em {filename}; nenhum catálogo foi salvo")
        for item in database.values():
            if not isinstance(item, dict):
                continue
            tid, name, icon = item.get("id"), item.get("name"), item.get("iconUrl")
            if not isinstance(tid, str):
                continue
            tid = tid.upper()
            # Program IDs are kept exact; update and add-on IDs are not treated
            # as separate games or blindly mapped to a different title.
            if len(tid) != 16 or not tid.endswith("000") or any(c not in "0123456789ABCDEF" for c in tid):
                continue
            if isinstance(name, str) and name.strip() and "\ufffd" not in name:
                names.setdefault(tid, name.strip())
            if isinstance(icon, str) and icon.startswith("https://img-eshop.cdn.nintendo.net/"):
                icons.setdefault(tid, icon)
        print(f"  {len(names)} nomes; {len(icons)} imagens", flush=True)
    import sys
    sys.path.insert(0, str(ROOT))
    from companion import IconResolver
    resolver = IconResolver(names, icons)
    shared = sum(1 for tid, name in names.items() if tid not in icons and resolver.lookup(tid, name)[0])
    print(f"  {len(names) - len(icons)} jogos sem imagem própria; {shared} usarão a imagem de outra "
          "região ou edição pelo nome (resolvido pelo companion).", flush=True)
    write_map("games.json", names)
    write_map("game-icons.json", icons)
    print(f"Concluído: +{len(names)-old_names} nomes; +{len(icons)-old_icons} imagens.")


if __name__ == "__main__":
    main()
