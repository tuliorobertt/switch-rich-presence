"""Recebe o jogo do Switch por UDP e o publica no Discord Desktop via IPC."""
from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import time
import uuid
from pathlib import Path

HANDSHAKE, FRAME, CLOSE, PING, PONG = range(5)
MAX_FRAME = 1024 * 1024
RETRY_SECONDS = 5
STALE_SECONDS = 210
BASE_DIR = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
SWITCH_ICON = "https://upload.wikimedia.org/wikipedia/commons/0/04/Nintendo_Switch_logo%2C_square.png"


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


def local_addresses() -> list[str]:
    try:
        addresses = {info[4][0] for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
        return sorted(ip for ip in addresses if not ip.startswith("127.")) or ["127.0.0.1"]
    except OSError:
        return ["127.0.0.1"]


def load_game_names(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    raw = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(raw, dict):
        raise ValueError("o catálogo precisa ser um objeto JSON")
    return {str(key).upper(): value.strip()[:128] for key, value in raw.items()
            if len(str(key)) == 16 and all(c in "0123456789ABCDEF" for c in str(key).upper())
            and isinstance(value, str) and value.strip()}


def parse_event(packet: bytes, names: dict[str, str]) -> tuple[str, str, str] | None:
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
        return "No Menu", "Nintendo Switch", ""
    if not title and title_id:
        title = names.get(title_id, f"Nintendo Switch ({title_id})")
    if not title:
        raise ValueError("evento sem título ou Title ID")
    return title, "Jogando no Nintendo Switch" if state == "playing" else state[:128], title_id


def make_activity(event: tuple[str, str, str] | None, started: int, icons: dict[str, str]) -> dict | None:
    if event is None:
        return None
    title, state, title_id = event
    image = icons.get(title_id, SWITCH_ICON)
    return {"type": 0, "name": title, "details": title, "state": state,
            "assets": {"large_image": image, "large_text": title},
            "timestamps": {"start": started}}


def main(argv=None, stop_event=None, log=None) -> int:
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

    desired: tuple[str, str, str] | None = None
    published: tuple[str, str, str] | None = None
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
                    event = parse_event(packet, names)
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
                        if event and event[2] and event[2] not in names:
                            print(f"Jogo não encontrado no catálogo; Title ID: {event[2]}", flush=True)
            except socket.timeout:
                pass
            now = time.monotonic()
            if desired is not None and now - last_received > STALE_SECONDS:
                desired = None
                next_retry = 0
                print("Switch sem atualizações; limpando atividade.", flush=True)
            if desired != published and now >= next_retry:
                if desired is None and discord.pipe is None:
                    published = desired
                    continue
                activity = make_activity(desired, started, icons)
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
                    discord.set_activity(make_activity(desired, started, icons))
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
