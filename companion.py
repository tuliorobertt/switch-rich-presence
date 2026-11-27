"""Recebe o jogo do Switch por UDP e o publica no Discord Desktop via IPC."""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import struct
import sys
import time
import unicodedata
import uuid
from pathlib import Path

HANDSHAKE, FRAME, CLOSE, PING, PONG = range(5)
MAX_FRAME = 1024 * 1024
RETRY_SECONDS = 5
STALE_SECONDS = 210
BASE_DIR = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
SWITCH_ICON = "https://upload.wikimedia.org/wikipedia/commons/0/04/Nintendo_Switch_logo%2C_square.png"
USER_DIR = Path(os.environ.get("APPDATA", Path.home())) / "SwitchRichPresence"
CUSTOM_FILE = USER_DIR / "custom-games.json"
_BRACKETS = re.compile(r"[\[(（【].*?[\])）】]")
_DEMO = re.compile(r"\b(?:demo|trial)(?: version)?\b|体験版|試玩版|体验版|체험판", re.I)
_EDITION = re.compile(r"\b(?:(?:deluxe|complete|definitive|ultimate|special|digital|standard|goty)\s+)?edition\b", re.I)


def title_key(name: str, loose: bool = False) -> str:
    """Normalize a game name so regional and edition variants can share an image."""
    text = unicodedata.normalize("NFKC", name).casefold()
    text = text.replace("™", "").replace("®", "").replace("©", "")
    if loose:
        text = _DEMO.sub(" ", _BRACKETS.sub(" ", text))
        text = _EDITION.sub(" ", text)
    # Strip accents only from Latin letters; Japanese dakuten change meaning.
    text = "".join(unicodedata.normalize("NFKD", c)[0] if ord(c) < 0x250 else c for c in text)
    return re.sub(r"[\W_]+", "", text)


class IconResolver:
    """Finds a public image for a title: custom > exact ID > same name > loose name."""

    def __init__(self, names: dict[str, str], icons: dict[str, str]):
        self.icons = {str(k).upper(): v for k, v in icons.items() if isinstance(v, str) and v.startswith("https://")}
        self.exact: dict[str, str] = {}
        self.loose: dict[str, str] = {}
        for tid in sorted(self.icons):
            name = names.get(tid)
            if name:
                self.add_name(name, self.icons[tid])

    def add_name(self, name: str, url: str) -> None:
        for index, loose in ((self.exact, False), (self.loose, True)):
            key = title_key(name, loose)
            if len(key) >= 3:
                index.setdefault(key, url)

    def lookup(self, title_id: str, title: str) -> tuple[str | None, str]:
        if title_id in self.icons:
            return self.icons[title_id], "catálogo"
        for index, loose, source in ((self.exact, False, "mesmo nome"), (self.loose, True, "nome semelhante")):
            key = title_key(title, loose) if title else ""
            if len(key) >= 3 and key in index:
                return index[key], source
        return None, "padrão"


class CustomCatalog:
    """User overrides in %APPDATA%, reloaded automatically when the file changes."""

    def __init__(self, path: Path = CUSTOM_FILE):
        self.path = path
        self.entries: dict[str, dict[str, str]] = {}
        self.mtime: float | None = None
        self.error: str | None = None

    def refresh(self) -> bool:
        try:
            mtime = self.path.stat().st_mtime
        except OSError:
            changed = bool(self.entries)
            self.entries, self.mtime = {}, None
            return changed
        if mtime == self.mtime:
            return False
        self.mtime = mtime
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8-sig"))
            if not isinstance(raw, dict):
                raise ValueError("o arquivo precisa ser um objeto JSON")
        except (OSError, ValueError) as exc:
            self.error = f"Personalizações ignoradas ({self.path.name}): {exc}"
            return False
        entries = {}
        for key, value in raw.items():
            tid = str(key).upper()
            if len(tid) != 16 or any(c not in "0123456789ABCDEF" for c in tid) or not isinstance(value, dict):
                continue
            entry = {}
            if isinstance(value.get("name"), str) and value["name"].strip():
                entry["name"] = value["name"].strip()[:128]
            if isinstance(value.get("icon"), str) and value["icon"].strip().startswith("https://"):
                entry["icon"] = value["icon"].strip()[:512]
            if entry:
                entries[tid] = entry
        self.entries, self.error = entries, None
        return True

    def get(self, title_id: str) -> dict[str, str]:
        return self.entries.get(title_id, {})


def save_custom_game(title_id: str, name: str = "", icon: str = "", path: Path = CUSTOM_FILE) -> None:
    """Create, update or (with empty fields) remove one user override."""
    title_id = title_id.strip().upper()
    if len(title_id) != 16 or any(c not in "0123456789ABCDEF" for c in title_id):
        raise ValueError("Title ID inválido")
    icon = icon.strip()
    if icon and not icon.startswith("https://"):
        raise ValueError("a imagem precisa ser um endereço https:// público")
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        if not isinstance(data, dict):
            data = {}
    except (OSError, ValueError):
        data = {}
    entry = {k: v for k, v in (("name", name.strip()[:128]), ("icon", icon[:512])) if v}
    if entry:
        data[title_id] = entry
    else:
        data.pop(title_id, None)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


class DiscordIPC:
    def __init__(self, client_id: str, timeout: float = 5):
        self.client_id = client_id
        self.timeout = timeout
        self.pipe = None

    def connect(self) -> None:
        if self.pipe is not None:
            return
        last_error = None
        for index in range(10):
            try:
                path = rf"\\.\pipe\discord-ipc-{index}"
                self.pipe = os.fdopen(os.open(path, os.O_RDWR | os.O_BINARY), "r+b", buffering=0)
                self._send(HANDSHAKE, {"v": 1, "client_id": self.client_id})
                deadline = time.monotonic() + self.timeout
                while True:
                    opcode, payload = self._receive(deadline)
                    if opcode == PING:
                        self._send(PONG, payload)
                    elif opcode == FRAME and isinstance(payload, dict) and payload.get("evt") == "READY":
                        return
                    elif opcode == CLOSE:
                        raise OSError(f"Discord encerrou a conexão: {payload}")
            except (OSError, ValueError, TimeoutError) as exc:
                last_error = exc
                self.close()
        raise OSError(f"Discord Desktop indisponível ({last_error})")

    def _send(self, opcode: int, payload: object) -> None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        packet = struct.pack("<II", opcode, len(data)) + data
        view = memoryview(packet)
        while view:
            count = self.pipe.write(view)
            if not count:
                raise OSError("Discord IPC desconectado")
            view = view[count:]

    def _receive(self, deadline: float) -> tuple[int, object]:
        # Windows named pipes do not support socket timeouts. PeekNamedPipe lets
        # us bound reads without leaving a blocked worker thread behind.
        import ctypes
        import msvcrt
        from ctypes import wintypes

        peek = ctypes.WinDLL("kernel32", use_last_error=True).PeekNamedPipe
        peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                         wintypes.LPVOID, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        peek.restype = wintypes.BOOL
        handle = msvcrt.get_osfhandle(self.pipe.fileno())

        def read_exact(length: int) -> bytes:
            result = bytearray()
            while len(result) < length:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Discord não respondeu ao IPC")
                available = wintypes.DWORD()
                if not peek(handle, None, 0, None, ctypes.byref(available), None):
                    raise OSError(ctypes.get_last_error(), "Discord IPC desconectado")
                if available.value:
                    chunk = self.pipe.read(min(length - len(result), available.value))
                    if not chunk:
                        raise OSError("Discord IPC desconectado")
                    result.extend(chunk)
                else:
                    time.sleep(0.02)
            return bytes(result)

        opcode, length = struct.unpack("<II", read_exact(8))
        if length > MAX_FRAME:
            raise OSError("Resposta IPC grande demais")
        body = read_exact(length)
        try:
            return opcode, json.loads(body.decode("utf-8")) if body else None
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise OSError("Resposta IPC inválida") from exc

    def set_activity(self, activity: dict | None) -> None:
        self.connect()
        nonce = str(uuid.uuid4())
        self._send(FRAME, {"cmd": "SET_ACTIVITY", "nonce": nonce,
                           "args": {"pid": os.getpid(), "activity": activity}})
        deadline = time.monotonic() + self.timeout
        while True:
            opcode, payload = self._receive(deadline)
            if opcode == PING:
                self._send(PONG, payload)
            elif opcode == CLOSE:
                raise OSError(f"Discord encerrou a conexão: {payload}")
            elif opcode == FRAME and isinstance(payload, dict) and payload.get("nonce") == nonce:
                if payload.get("evt") == "ERROR":
                    raise OSError(f"Discord recusou a atividade: {payload.get('data')}")
                if payload.get("cmd") != "SET_ACTIVITY":
                    raise OSError("Discord retornou uma resposta IPC inesperada")
                return

    def close(self) -> None:
        if self.pipe is not None:
            try:
                self.pipe.close()
            except OSError:
                pass
            self.pipe = None


def primary_address() -> str | None:
    """IPv4 of the interface used for the default route (no packet is sent)."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))  # TEST-NET-1: only selects a route
        address = probe.getsockname()[0]
        return None if address.startswith(("127.", "0.")) else address
    except OSError:
        return None
    finally:
        probe.close()


def local_addresses() -> list[str]:
    """LAN addresses, the default-route one first (virtual adapters such as
    WSL, Docker or VPNs would otherwise often be suggested to the user)."""
    try:
        addresses = {info[4][0] for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
    except OSError:
        addresses = set()
    addresses = {ip for ip in addresses if not ip.startswith("127.")}
    primary = primary_address()
    if primary:
        addresses.discard(primary)
        return [primary] + sorted(addresses)
    return sorted(addresses) or ["127.0.0.1"]


def load_game_names(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    raw = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(raw, dict):
        raise ValueError("o catálogo precisa ser um objeto JSON")
    return {str(key).upper(): value.strip()[:128] for key, value in raw.items()
            if len(str(key)) == 16 and all(c in "0123456789ABCDEF" for c in str(key).upper())
            and isinstance(value, str) and value.strip()}


Event = tuple[str, str, str, str]


def parse_event(packet: bytes, names: dict[str, str], custom: CustomCatalog | None = None) -> Event | None:
    """Return (display title, state, title ID, title reported by the console)."""
    message = json.loads(packet.decode("utf-8"))
    if not isinstance(message, dict):
        raise ValueError("o evento deve ser um objeto JSON")
    title_id = message.get("title_id", "")
    title = message.get("title", "")
    state = message.get("state", "playing")
    if not all(isinstance(value, str) for value in (title_id, title, state)):
        raise ValueError("title_id, title e state devem ser textos")
    title_id, title, state = title_id.strip().upper(), title.strip()[:128], state.strip().lower()
    if title_id and (len(title_id) != 16 or any(c not in "0123456789ABCDEF" for c in title_id)):
        raise ValueError("Title ID inválido")
    if state == "paused":
        return None
    if state == "idle" or title_id == "0000000000000000" or ("title" in message and not title and not title_id):
        return "No Menu", "Nintendo Switch", "", ""
    display = ""
    if title_id:
        display = (custom.get(title_id).get("name", "") if custom else "") or names.get(title_id, "")
    display = display or title or (f"Nintendo Switch ({title_id})" if title_id else "")
    if not display:
        raise ValueError("evento sem título ou Title ID")
    return display, "Jogando no Nintendo Switch" if state == "playing" else state[:128], title_id, title


def resolve_image(event: Event, resolver: IconResolver | None, custom: CustomCatalog | None = None) -> tuple[str, str]:
    """Return (image URL, where it came from). Falls back to the Switch logo."""
    title, _state, title_id, reported = event
    if title_id and custom and custom.get(title_id).get("icon"):
        return custom.get(title_id)["icon"], "personalizada"
    if resolver is not None and title_id:
        for name in dict.fromkeys(n for n in (title, reported) if n):
            image, source = resolver.lookup(title_id, name)
            if image:
                return image, source
    return SWITCH_ICON, "padrão"


def make_activity(event: Event | None, started: int, resolver: IconResolver | None = None,
                  custom: CustomCatalog | None = None) -> dict | None:
    if event is None:
        return None
    title, state, title_id, _reported = event
    image, _source = resolve_image(event, resolver, custom)
    assets = {"large_image": image, "large_text": title}
    if image != SWITCH_ICON:
        assets.update(small_image=SWITCH_ICON, small_text="Nintendo Switch")
    return {"type": 0, "name": title, "details": title, "state": state,
            "assets": assets, "timestamps": {"start": started}}


def main(argv=None, stop_event=None, log=None, on_activity=None) -> int:
    """Run the UDP listener. on_activity(dict | None) receives each new activity."""
    if log is not None:
        def print(*values, **kwargs):
            log(" ".join(str(value) for value in values))
    else:
        from builtins import print
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-id", help="Application ID do Discord (sobrescreve companion.config.json)")
    parser.add_argument("--port", type=int, default=39421, help="porta UDP (padrão: 39421)")
    parser.add_argument("--games", type=Path, default=BASE_DIR / "games.json", help="arquivo JSON com nomes dos jogos")
    args = parser.parse_args(argv)
    if args.client_id is None:
        config_path = BASE_DIR / "companion.config.json"
        try:
            config = json.loads(config_path.read_text(encoding="utf-8-sig"))
            if not isinstance(config, dict) or not isinstance(config.get("client_id"), str):
                raise ValueError("client_id deve ser uma string")
            args.client_id = config["client_id"].strip()
        except (OSError, ValueError) as exc:
            parser.error(f"informe --client-id ou configure {config_path.name}: {exc}")
    if os.name != "nt":
        parser.error("este companion requer Windows e Discord Desktop")
    if not args.client_id.isdecimal() or not 1 <= args.port <= 65535:
        parser.error("informe um Application ID numérico e uma porta entre 1 e 65535")
    try:
        names = load_game_names(args.games)
    except (OSError, ValueError) as exc:
        parser.error(f"não foi possível ler {args.games}: {exc}")
    try:
        icons = json.loads((BASE_DIR / "game-icons.json").read_text(encoding="utf-8"))
        if not isinstance(icons, dict):
            raise ValueError("catálogo de imagens inválido")
    except (OSError, ValueError) as exc:
        print(f"Catálogo de imagens indisponível: {exc}", file=sys.stderr, flush=True)
        icons = {}
    resolver = IconResolver(names, icons)
    custom = CustomCatalog()
    custom.refresh()
    if custom.error:
        print(custom.error, file=sys.stderr, flush=True)

    def notify(event: Event | None) -> None:
        if on_activity is None:
            return
        if event is None:
            on_activity(None)
            return
        image, source = resolve_image(event, resolver, custom)
        on_activity({"title": event[0], "title_id": event[2], "reported_title": event[3],
                     "image": image, "image_source": source, "known": bool(event[2] in names)})

    discord = DiscordIPC(args.client_id)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", args.port))
    except OSError as exc:
        sock.close()
        print(f"Não foi possível abrir a porta UDP {args.port}: {exc}", file=sys.stderr)
        return 1
    sock.settimeout(1)
    print(f"Aguardando o Switch na porta UDP {args.port}.", flush=True)
    print("IP(s) do PC: " + ", ".join(local_addresses()), flush=True)
    print("Deixe o Discord Desktop aberto. Ctrl+C encerra.", flush=True)

    desired: Event | None = None
    published: Event | None = None
    started = 0
    last_received = 0.0
    next_retry = 0.0
    last_error = None
    try:
        while stop_event is None or not stop_event.is_set():
            try:
                packet, source = sock.recvfrom(2048)
                try:
                    probe = json.loads(packet.decode("utf-8"))
                    if isinstance(probe, dict) and probe.get("type") == "ping":
                        nonce = probe.get("nonce")
                        if isinstance(nonce, str) and len(nonce) == 16 and all(c in "0123456789ABCDEF" for c in nonce):
                            try:
                                sock.sendto(f'{{"type":"pong","nonce":"{nonce}"}}'.encode("ascii"), source)
                                if probe.get("discovery") is not True:
                                    print(f"Teste de conexão recebido de {source[0]}:{source[1]}; resposta enviada.", flush=True)
                            except OSError as exc:
                                print(f"Falha ao responder teste de {source[0]}: {exc}", file=sys.stderr, flush=True)
                        continue
                    event = parse_event(packet, names, custom)
                except (ValueError, UnicodeDecodeError) as exc:
                    print(f"Pacote inválido de {source[0]}: {exc}", file=sys.stderr, flush=True)
                else:
                    last_received = time.monotonic()
                    print(f"Evento do sysmodule de {source[0]}: {event[0] if event else 'presença pausada'}", flush=True)
                    if event != desired:
                        desired = event
                        started = int(time.time()) if event else 0
                        next_retry = 0
                        print(f"Switch {source[0]}: {event[0] if event else 'presença pausada'}", flush=True)
                        if event and event[2] and event[2] not in names and not custom.get(event[2]).get("name"):
                            print(f"Jogo não encontrado no catálogo; Title ID: {event[2]}", flush=True)
                        if event and event[2] and resolve_image(event, resolver, custom)[0] == SWITCH_ICON:
                            print(f"Imagem não encontrada para {event[2]}; usando a logo do Switch. "
                                  "Defina uma imagem em Visão geral > Personalizar jogo.", flush=True)
                        notify(event)
            except socket.timeout:
                pass
            if custom.refresh():
                # Re-read edited overrides and republish the current game.
                if desired is not None and desired[2]:
                    name = custom.get(desired[2]).get("name") or names.get(desired[2]) or desired[3] or desired[0]
                    desired = (name, desired[1], desired[2], desired[3])
                    published = None
                    next_retry = 0
                    notify(desired)
                print("Personalizações de jogos recarregadas.", flush=True)
            elif custom.error:
                print(custom.error, file=sys.stderr, flush=True)
                custom.error = None
            now = time.monotonic()
            if desired is not None and now - last_received > STALE_SECONDS:
                desired = None
                next_retry = 0
                notify(None)
                print("Switch sem atualizações; limpando atividade.", flush=True)
            if desired != published and now >= next_retry:
                if desired is None and discord.pipe is None:
                    published = desired
                    continue
                activity = make_activity(desired, started, resolver, custom)
                try:
                    discord.set_activity(activity)
                    published = desired
                    last_error = None
                    print("Atividade atualizada no Discord." if activity else "Atividade removida do Discord.", flush=True)
                except (OSError, ValueError, TimeoutError) as exc:
                    discord.close()
                    published = None
                    next_retry = time.monotonic() + RETRY_SECONDS
                    error = str(exc)
                    if error != last_error:
                        print(f"Aguardando o Discord: {error}", file=sys.stderr, flush=True)
                        last_error = error
            # Re-publish on each heartbeat so a restarted Discord is detected.
            elif desired is not None and desired == published and now - last_received < 1 and now >= next_retry:
                next_retry = now + RETRY_SECONDS
                try:
                    discord.set_activity(make_activity(desired, started, resolver, custom))
                except (OSError, ValueError, TimeoutError):
                    discord.close()
                    published = None
                    next_retry = 0
    except KeyboardInterrupt:
        print("\nEncerrando.", flush=True)
    finally:
        if discord.pipe is not None and published is not None:
            try:
                discord.set_activity(None)
            except (OSError, ValueError, TimeoutError):
                pass
        discord.close()
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
