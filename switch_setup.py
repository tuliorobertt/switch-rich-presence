"""Copies the bundled Switch files (sysmodule, NRO and config) to an SD card."""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

MODULE_ID = "4200000000075250"
CONFIG_REL = Path("config") / "switch-richpresence.ini"
BOOT_FLAG_REL = Path("atmosphere") / "contents" / MODULE_ID / "flags" / "boot2.flag"
MODULE_REL = Path("atmosphere") / "contents" / MODULE_ID / "exefs.nsp"
NRO_REL = Path("switch") / "switch-richpresence" / "switch-richpresence.nro"


def bundle_dir() -> Path | None:
    """Folder with the SD layout shipped by the installer (or a local build)."""
    candidates = [Path(sys.executable).resolve().parent / "switch-sd",
                  Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent)) / "switch-sd",
                  Path(__file__).resolve().parent / "dist" / "switch-sd"]
    for path in candidates:
        if (path / MODULE_REL).is_file():
            return path
    return None


def find_sd_cards() -> list[Path]:
    """Removable drives, with drives that already contain Atmosphère first."""
    if os.name != "nt":
        return []
    import ctypes
    kernel32 = ctypes.windll.kernel32
    mask = kernel32.GetLogicalDrives()
    found = []
    for index in range(26):
        if not mask & (1 << index):
            continue
        root = f"{chr(65 + index)}:\\"
        if kernel32.GetDriveTypeW(ctypes.c_wchar_p(root)) != 2:  # DRIVE_REMOVABLE
            continue
        try:
            has_atmosphere = (Path(root) / "atmosphere").is_dir()
        except OSError:
            continue
        found.append((not has_atmosphere, Path(root)))
    return [path for _, path in sorted(found)]


def describe_target(target: Path) -> str | None:
    """Return a warning for a folder that does not look like a Switch SD root."""
    if not target.is_dir():
        return "A pasta escolhida não existe."
    if not (target / "atmosphere").is_dir():
        return ("Não há uma pasta 'atmosphere' aqui. Escolha a raiz do cartão SD do Switch "
                "com Atmosphère já instalado.")
    return None


def copy_to_sd(bundle: Path, target: Path) -> list[str]:
    """Copy every bundled file to target. Returns a human readable report.

    The user's config is never overwritten, and boot2.flag is only created on a
    first install, so an update keeps the autostart choice made on the console.
    """
    first_install = not (target / MODULE_REL).exists()
    report = []
    for source in sorted(p for p in bundle.rglob("*") if p.is_file()):
        relative = source.relative_to(bundle)
        destination = target / relative
        if relative == CONFIG_REL and destination.exists():
            report.append(f"Mantido (sua configuração): {relative.as_posix()}")
            continue
        if relative == BOOT_FLAG_REL and not first_install:
            report.append("Mantida a opção de início automático escolhida no console.")
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + ".tmp")
        shutil.copyfile(source, temporary)
        os.replace(temporary, destination)
        report.append(f"Copiado: {relative.as_posix()}")
    return report
