import json
import os
import queue
import sys
import threading
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import messagebox
import winreg

from PIL import Image
import pystray
import companion

APP_NAME = 'Switch Rich Presence'
ROOT = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent))
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'SwitchRichPresence'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
RUN_KEY = r'Software\Microsoft\Windows\CurrentVersion\Run'
BG, PANEL, BORDER = '#0d111b', '#171d2b', '#273047'
TEXT, MUTED, ACCENT, GREEN = '#f2f4fb', '#9ba8be', '#826aff', '#5ee0ad'


def read_settings():
    defaults = {'port': 39421, 'client_id': '1565506238114959380', 'minimize_to_tray': False}
    try:
        data = json.loads(CONFIG_FILE.read_text(encoding='utf-8'))
        if isinstance(data, dict):
            defaults.update({k: v for k, v in data.items() if k in defaults})
    except (OSError, ValueError):
        pass
    return defaults


def startup_command():
    if getattr(sys, 'frozen', False):
        return f'"{sys.executable}" --startup'
    pythonw = Path(sys.executable).with_name('pythonw.exe')
    return f'"{pythonw if pythonw.exists() else sys.executable}" "{Path(__file__).resolve()}" --startup'


def is_autostart():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            return bool(winreg.QueryValueEx(key, APP_NAME)[0])
    except OSError:
        return False


def set_autostart(enabled):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
        if enabled:
            winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, startup_command())
        else:
            try:
                winreg.DeleteValue(key, APP_NAME)
            except FileNotFoundError:
                pass


class App:
    def __init__(self):
        self.settings = read_settings()
        self.root = tk.Tk()
        self.root.title(APP_NAME)
        self.root.geometry('980x660')
        self.root.minsize(820, 570)
        self.root.configure(bg=BG)
        self.events = queue.Queue()
        self.stopping = threading.Event()
        self.tray = None
        self.closing = False
        self.was_hidden = False
        self.autostart = tk.BooleanVar(value=is_autostart())
        self.tray_enabled = tk.BooleanVar(value=bool(self.settings['minimize_to_tray']))
        self.port = tk.StringVar(value=str(self.settings['port']))
        self.client_id = tk.StringVar(value=str(self.settings['client_id']))
        self.status = tk.StringVar(value='Aguardando o Switch')
        self.game = tk.StringVar(value='Nenhuma atividade recebida')
        self.discord = tk.StringVar(value='Aguardando atividade')
        self.ip = tk.StringVar(value='  •  '.join(companion.local_addresses()))
        self.build()
        self.root.protocol('WM_DELETE_WINDOW', self.close)
        self.root.bind('<Unmap>', self.on_minimize)
        self.worker = threading.Thread(target=self.run_worker, daemon=True)
        self.worker.start()
        self.root.after(200, self.poll)
        if '--startup' in sys.argv and self.tray_enabled.get():
            self.root.after(150, self.hide)

    def label(self, parent, text='', size=10, color=TEXT, bold=False, **kw):
        return tk.Label(parent, text=text, bg=kw.pop('bg', parent['bg']), fg=color,
                        font=('Segoe UI', size, 'bold' if bold else 'normal'), **kw)

    def button(self, parent, text, command, primary=False):
        return tk.Button(parent, text=text, command=command, relief='flat', bd=0,
                         bg=ACCENT if primary else '#252e42', fg='white', activebackground='#6954dd',
                         activeforeground='white', padx=16, pady=9, cursor='hand2',
                         font=('Segoe UI', 9, 'bold'))

    def build(self):
        side = tk.Frame(self.root, bg='#111724', width=200)
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        self.label(side, '●  SWITCH', 15, ACCENT, True).pack(anchor='w', padx=25, pady=(30, 0))
        self.label(side, 'RICH PRESENCE', 10, MUTED, True).pack(anchor='w', padx=43)
        self.label(side, 'SEU SWITCH, CONECTADO', 8, MUTED, True).pack(anchor='w', padx=25, pady=(42, 15))
        for name, page in [('◈   Visão geral', 'home'), ('⚙   Configurações', 'settings'), ('≡   Atividade', 'logs')]:
            tk.Button(side, text=name, anchor='w', command=lambda p=page: self.show(p),
                      relief='flat', bg='#111724', fg=TEXT, activebackground=PANEL,
                      activeforeground='white', padx=20, pady=12, cursor='hand2',
                      font=('Segoe UI', 10, 'bold')).pack(fill='x', padx=8)
        self.label(side, 'Feito para sua jogatina', 9, MUTED).pack(side='bottom', pady=24)
        self.content = tk.Frame(self.root, bg=BG)
        self.content.pack(side='left', fill='both', expand=True, padx=30, pady=28)
        self.pages = {}
        for name in ('home', 'settings', 'logs'):
            page = tk.Frame(self.content, bg=BG)
            self.pages[name] = page
            page.place(relx=0, rely=0, relwidth=1, relheight=1)
        self.home_page(); self.settings_page(); self.logs_page(); self.show('home')

    def heading(self, page, title, subtitle):
        self.label(page, title, 23, TEXT, True).pack(anchor='w')
        self.label(page, subtitle, 10, MUTED).pack(anchor='w', pady=(3, 24))

    def card(self, page):
        f = tk.Frame(page, bg=PANEL, highlightbackground=BORDER, highlightthickness=1)
        f.pack(fill='x', pady=(0, 14))
        return f

    def home_page(self):
        p = self.pages['home']
        self.heading(p, 'Sua jogatina, no Discord.', 'Acompanhe sua conexão com o Nintendo Switch em tempo real.')
        c = self.card(p)
        self.label(c, '●  ATIVIDADE ATUAL', 9, GREEN, True).pack(anchor='w', padx=20, pady=(18, 8))
        tk.Label(c, textvariable=self.game, bg=PANEL, fg=TEXT, font=('Segoe UI', 18, 'bold'),
                 anchor='w', wraplength=620).pack(fill='x', padx=20)
        tk.Label(c, textvariable=self.status, bg=PANEL, fg=MUTED, font=('Segoe UI', 10)).pack(anchor='w', padx=20, pady=(5, 20))
        c = self.card(p)
        self.label(c, 'CONEXÃO LOCAL', 9, MUTED, True).pack(anchor='w', padx=20, pady=(17, 5))
        tk.Label(c, textvariable=self.ip, bg=PANEL, fg=TEXT, font=('Segoe UI', 13, 'bold')).pack(anchor='w', padx=20)
        self.label(c, 'Informe o IP do seu PC no aplicativo do Switch.', 9, MUTED).pack(anchor='w', padx=20, pady=(3, 12))
        self.button(c, 'Copiar IP', self.copy_ip).pack(anchor='w', padx=20, pady=(0, 18))
        tk.Label(p, textvariable=self.discord, bg=BG, fg=MUTED, font=('Segoe UI', 10)).pack(anchor='w', pady=(4, 0))

    def checkbox(self, p, variable, title, subtitle):
        c = self.card(p)
        tk.Checkbutton(c, text=title, variable=variable, bg=PANEL, fg=TEXT, selectcolor='#303a50',
                       activebackground=PANEL, activeforeground=TEXT, font=('Segoe UI', 11, 'bold'),
                       anchor='w', padx=12, pady=10).pack(fill='x')
        self.label(c, subtitle, 9, MUTED, wraplength=570, justify='left').pack(anchor='w', padx=36, pady=(0, 15))

    def settings_page(self):
        p = self.pages['settings']
        self.heading(p, 'Configurações', 'Personalize como o companion funciona neste computador.')
        self.checkbox(p, self.autostart, 'Iniciar com o Windows', 'Abre o companion ao entrar na sua conta do Windows.')
        self.checkbox(p, self.tray_enabled, 'Minimizar para a bandeja', 'Ao minimizar, mantém o companion na bandeja. Com início automático, abre minimizado.')
        c = self.card(p)
        row = tk.Frame(c, bg=PANEL); row.pack(fill='x', padx=18, pady=16)
        for title, var, width in [('Porta UDP', self.port, 10), ('Discord Application ID', self.client_id, 28)]:
            col = tk.Frame(row, bg=PANEL); col.pack(side='left', padx=(0, 20))
            self.label(col, title, 9, MUTED).pack(anchor='w', pady=(0, 5))
            tk.Entry(col, textvariable=var, width=width, bg='#0e1420', fg=TEXT, insertbackground='white',
                     relief='flat', font=('Segoe UI', 11)).pack(ipady=7)
        self.button(p, 'Salvar configurações', self.save, True).pack(anchor='w', pady=5)
        self.label(p, 'Alterações de porta e Application ID reiniciam a conexão.', 9, MUTED).pack(anchor='w', pady=8)

    def logs_page(self):
        p = self.pages['logs']
        self.heading(p, 'Atividade', 'Eventos de conexão e mensagens do companion.')
        self.logbox = tk.Text(p, bg=PANEL, fg='#cbd4e5', insertbackground=TEXT, relief='flat',
                              font=('Consolas', 9), padx=15, pady=15, state='disabled', wrap='word')
        self.logbox.pack(fill='both', expand=True)
        self.button(p, 'Limpar histórico', self.clear_log).pack(anchor='w', pady=12)

    def show(self, name): self.pages[name].tkraise()

    def copy_ip(self):
        address = companion.local_addresses()[0]
        self.root.clipboard_clear(); self.root.clipboard_append(address)
        self.status.set(f'IP {address} copiado para a área de transferência')

    def clear_log(self):
        self.logbox.configure(state='normal'); self.logbox.delete('1.0', 'end'); self.logbox.configure(state='disabled')

    def append_log(self, line):
        self.logbox.configure(state='normal')
        self.logbox.insert('end', f'{datetime.now():%H:%M:%S}  {line}\n')
        if int(self.logbox.index('end-1c').split('.')[0]) > 500:
            self.logbox.delete('1.0', '101.0')
        self.logbox.see('end'); self.logbox.configure(state='disabled')

    def run_worker(self):
        try:
            result = companion.main(['--client-id', str(self.settings['client_id']), '--port', str(self.settings['port'])], self.stopping, self.events.put)
            if result:
                self.events.put(f'O serviço encerrou com código {result}. Confira a porta UDP.')
        except BaseException as exc:
            self.events.put(f'Erro no serviço: {exc}')

    def poll(self):
        while not self.events.empty():
            line = self.events.get_nowait()
            if line == '__show__':
                self.root.deiconify(); self.root.lift(); self.root.focus_force()
                continue
            if line == '__exit__':
                self.close(); return
            self.append_log(line)
            if line.startswith('Evento do sysmodule de '):
                self.game.set(line.partition(': ')[2]); self.status.set('Switch conectado • atualização recebida')
            elif line.startswith('Teste de conexão recebido'):
                self.status.set('Teste de conexão recebido do Switch')
            elif line.startswith('Atividade atualizada'):
                self.discord.set('●  Atividade enviada ao Discord')
            elif line.startswith('Aguardando o Discord'):
                self.discord.set('●  Aguardando o Discord Desktop'); self.status.set(line)
            elif line.startswith('Switch sem atualizações'):
                self.game.set('Nenhuma atividade recebida'); self.status.set('Aguardando o Switch')
            elif line.startswith(('Não foi possível', 'Erro no serviço', 'O serviço encerrou')):
                self.status.set(line)
        if not self.closing: self.root.after(200, self.poll)

    def save(self):
        try:
            port = int(self.port.get()); client_id = self.client_id.get().strip()
            if not 1 <= port <= 65535 or not client_id.isdecimal():
                raise ValueError('Informe uma porta entre 1 e 65535 e um Application ID numérico.')
            new = {'port': port, 'client_id': client_id, 'minimize_to_tray': self.tray_enabled.get()}
            CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            CONFIG_FILE.write_text(json.dumps(new, indent=2), encoding='utf-8')
            set_autostart(self.autostart.get())
        except (OSError, ValueError) as exc:
            messagebox.showerror(APP_NAME, f'Não foi possível salvar: {exc}', parent=self.root); return
        restart = port != self.settings['port'] or client_id != self.settings['client_id']
        self.settings = new
        if restart:
            self.stopping.set(); self.root.after(100, self.restart_worker)
        self.status.set('Configurações salvas')

    def restart_worker(self):
        if self.worker.is_alive():
            self.root.after(200, self.restart_worker); return
        self.stopping = threading.Event()
        self.worker = threading.Thread(target=self.run_worker, daemon=True); self.worker.start()

    def on_minimize(self, event):
        if event.widget == self.root and self.tray_enabled.get() and self.root.state() == 'iconic':
            self.root.after(100, self.hide)

    def hide(self):
        if self.closing: return
        try:
            if not self.tray:
                icon = Image.open(ROOT / 'icon.jpg').convert('RGBA')
                menu = pystray.Menu(pystray.MenuItem('Abrir', lambda: self.events.put('__show__'), default=True),
                                    pystray.MenuItem('Sair', lambda: self.events.put('__exit__')))
                self.tray = pystray.Icon('switch_rich_presence', icon, APP_NAME, menu)
                self.tray.run_detached()
            self.root.withdraw(); self.was_hidden = True
        except Exception as exc:
            self.status.set(f'Não foi possível abrir a bandeja: {exc}'); self.root.deiconify()

    def close(self):
        self.closing = True; self.stopping.set()
        if self.tray: self.tray.stop(); self.tray = None
        self.wait_close()

    def wait_close(self):
        if self.worker.is_alive():
            self.root.after(100, self.wait_close)
        else:
            self.root.destroy()


if __name__ == '__main__':
    app = App()
    app.root.mainloop()
