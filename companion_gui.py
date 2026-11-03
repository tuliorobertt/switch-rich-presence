import json
import os
import queue
import sys
import threading
import tkinter as tk
import customtkinter as ctk
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
BG, PANEL, BORDER = '#0b0d14', '#171b27', '#303647'
TEXT, MUTED, ACCENT, GREEN = '#f3f4fa', '#969db0', '#816bff', '#63dfb0'


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
        ctk.set_appearance_mode('dark')
        self.root = ctk.CTk()
        self.root.title(APP_NAME)
        self.root.geometry('1080x720')
        self.root.minsize(900, 620)
        self.root.configure(fg_color=BG)
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
        self.addresses = companion.local_addresses()
        self.ip = tk.StringVar(value=self.addresses[0])
        self.build()
        self.root.protocol('WM_DELETE_WINDOW', self.close)
        self.root.bind('<Unmap>', self.on_minimize)
        self.worker = threading.Thread(target=self.run_worker, daemon=True)
        self.worker.start()
        self.root.after(200, self.poll)
        if '--startup' in sys.argv and self.tray_enabled.get():
            self.root.after(150, self.hide)

    def label(self, parent, text='', size=13, color=TEXT, bold=False, **kwargs):
        return ctk.CTkLabel(parent, text=text, text_color=color,
                            font=ctk.CTkFont(family='Segoe UI', size=size, weight='bold' if bold else 'normal'), **kwargs)

    def button(self, parent, text, command, primary=False, **kwargs):
        return ctk.CTkButton(parent, text=text, command=command, height=38, corner_radius=10,
                             fg_color=ACCENT if primary else '#252a3a', hover_color='#6e58e8' if primary else '#333a4d',
                             text_color='white', font=ctk.CTkFont(family='Segoe UI', size=13, weight='bold'), **kwargs)

    def build(self):
        self.root.grid_columnconfigure(1, weight=1)
        self.root.grid_rowconfigure(0, weight=1)
        side = ctk.CTkFrame(self.root, width=220, corner_radius=0, fg_color='#11141d')
        side.grid(row=0, column=0, sticky='nsew'); side.grid_propagate(False)
        brand = ctk.CTkFrame(side, fg_color='transparent')
        brand.pack(fill='x', padx=20, pady=(27, 34))
        logo = ctk.CTkImage(Image.open(ROOT / 'icon.jpg'), size=(38, 38))
        self.logo_image = logo
        ctk.CTkLabel(brand, text='', image=logo).pack(side='left', padx=(0, 10))
        brand_text = ctk.CTkFrame(brand, fg_color='transparent'); brand_text.pack(side='left')
        self.label(brand_text, 'SWITCH', 15, TEXT, True, anchor='w').pack(anchor='w')
        self.label(brand_text, 'RICH PRESENCE', 10, MUTED, True, anchor='w').pack(anchor='w')
        self.label(side, 'MENU', 10, '#70798d', True).pack(anchor='w', padx=25, pady=(0, 10))
        self.nav = {}
        for title, name in [('Visão geral', 'home'), ('Configurações', 'settings'), ('Atividade', 'logs')]:
            btn = ctk.CTkButton(side, text=title, anchor='w', height=42, corner_radius=9,
                                fg_color='transparent', hover_color='#252a3a', text_color=MUTED,
                                font=ctk.CTkFont(family='Segoe UI', size=13, weight='bold'),
                                command=lambda n=name: self.show(n))
            btn.pack(fill='x', padx=14, pady=3); self.nav[name] = btn
        bottom = ctk.CTkFrame(side, fg_color='#1b1f2b', corner_radius=12)
        bottom.pack(side='bottom', fill='x', padx=15, pady=18)
        self.label(bottom, '●  COMPANION', 10, GREEN, True, anchor='w').pack(anchor='w', padx=13, pady=(12, 2))
        self.label(bottom, 'Nintendo Switch  /  PC', 11, MUTED, anchor='w').pack(anchor='w', padx=13, pady=(0, 12))
        self.content = ctk.CTkFrame(self.root, fg_color=BG, corner_radius=0)
        self.content.grid(row=0, column=1, sticky='nsew', padx=34, pady=28)
        self.pages = {}
        for name in ('home', 'settings', 'logs'):
            page = ctk.CTkFrame(self.content, fg_color='transparent')
            self.pages[name] = page; page.place(relx=0, rely=0, relwidth=1, relheight=1)
        self.home_page(); self.settings_page(); self.logs_page(); self.show('home')

    def heading(self, page, kicker, title, subtitle):
        self.label(page, kicker, 10, ACCENT, True).pack(anchor='w', pady=(0, 5))
        self.label(page, title, 29, TEXT, True).pack(anchor='w')
        self.label(page, subtitle, 13, MUTED).pack(anchor='w', pady=(2, 20))

    def card(self, page, color=PANEL):
        frame = ctk.CTkFrame(page, fg_color=color, corner_radius=16, border_width=1, border_color=BORDER)
        frame.pack(fill='x', pady=(0, 14))
        return frame

    def home_page(self):
        p = self.pages['home']
        self.heading(p, 'VISÃO GERAL', 'Sua jogatina, conectada.', 'Acompanhe o que seu Switch está compartilhando no Discord.')
        hero = self.card(p, '#1c1b30')
        top = ctk.CTkFrame(hero, fg_color='transparent'); top.pack(fill='x', padx=22, pady=(19, 10))
        self.label(top, 'ATIVIDADE ATUAL', 11, '#a99afc', True).pack(side='left')
        self.label(top, '●  NINTENDO SWITCH', 10, GREEN, True).pack(side='right')
        tk_label = ctk.CTkLabel(hero, textvariable=self.game, text_color=TEXT, anchor='w',
                               font=ctk.CTkFont(family='Segoe UI', size=23, weight='bold'), wraplength=600)
        tk_label.pack(fill='x', padx=22, pady=(0, 2))
        ctk.CTkLabel(hero, textvariable=self.status, anchor='w', text_color=MUTED,
                     font=ctk.CTkFont(family='Segoe UI', size=12), wraplength=600).pack(fill='x', padx=22, pady=(0, 20))
        row = ctk.CTkFrame(p, fg_color='transparent'); row.pack(fill='x', pady=(0, 14))
        row.grid_columnconfigure((0, 1), weight=1, uniform='info')
        c1 = ctk.CTkFrame(row, fg_color=PANEL, corner_radius=15, border_width=1, border_color=BORDER)
        c1.grid(row=0, column=0, sticky='nsew', padx=(0, 7))
        self.label(c1, 'CONEXÃO LOCAL', 10, MUTED, True).pack(anchor='w', padx=18, pady=(17, 7))
        ctk.CTkOptionMenu(c1, variable=self.ip, values=self.addresses, fg_color='#252a3a',
                          button_color='#343b50', button_hover_color='#414a61', height=35,
                          font=ctk.CTkFont(family='Segoe UI', size=13, weight='bold')).pack(fill='x', padx=18)
        self.label(c1, 'Configure este IP no Switch.', 11, MUTED).pack(anchor='w', padx=18, pady=(1, 12))
        self.button(c1, 'Copiar IP', self.copy_ip, width=105).pack(anchor='w', padx=18, pady=(0, 17))
        c2 = ctk.CTkFrame(row, fg_color=PANEL, corner_radius=15, border_width=1, border_color=BORDER)
        c2.grid(row=0, column=1, sticky='nsew', padx=(7, 0))
        self.label(c2, 'DISCORD', 10, MUTED, True).pack(anchor='w', padx=18, pady=(17, 7))
        ctk.CTkLabel(c2, textvariable=self.discord, text_color=TEXT, anchor='w', wraplength=270,
                     font=ctk.CTkFont(family='Segoe UI', size=13, weight='bold')).pack(fill='x', padx=18)
        self.label(c2, 'Mantenha o Discord Desktop aberto.', 11, MUTED, wraplength=260, justify='left').pack(anchor='w', padx=18, pady=(5, 18))
        tip = self.card(p, '#141b24')
        self.label(tip, 'PRIMEIROS PASSOS', 10, GREEN, True).pack(anchor='w', padx=18, pady=(14, 4))
        self.label(tip, 'No Switch, abra o app, informe o IP e teste a conexão.', 12, MUTED, wraplength=600, justify='left').pack(anchor='w', padx=18, pady=(0, 15))

    def switch_row(self, page, variable, title, subtitle):
        card = self.card(page)
        row = ctk.CTkFrame(card, fg_color='transparent'); row.pack(fill='x', padx=18, pady=14)
        text = ctk.CTkFrame(row, fg_color='transparent'); text.pack(side='left', fill='x', expand=True)
        self.label(text, title, 14, TEXT, True, anchor='w').pack(anchor='w')
        self.label(text, subtitle, 11, MUTED, anchor='w', wraplength=470, justify='left').pack(anchor='w', pady=(3, 0))
        ctk.CTkSwitch(row, text='', variable=variable, width=42, progress_color=ACCENT,
                      button_color='white', button_hover_color='#e3defd').pack(side='right', padx=(12, 0))

    def settings_page(self):
        p = self.pages['settings']
        self.heading(p, 'PREFERÊNCIAS', 'Do seu jeito.', 'Ajuste a inicialização e a conexão do companion.')
        self.switch_row(p, self.autostart, 'Iniciar com o Windows', 'Abra o companion automaticamente ao entrar no Windows.')
        self.switch_row(p, self.tray_enabled, 'Minimizar para a bandeja', 'O aplicativo continua ativo ao minimizar e inicia oculto quando aberto pelo Windows.')
        card = self.card(p)
        self.label(card, 'CONEXÃO', 10, MUTED, True).pack(anchor='w', padx=18, pady=(14, 4))
        row = ctk.CTkFrame(card, fg_color='transparent'); row.pack(fill='x', padx=18, pady=(0, 16))
        for title, var, width in [('Porta UDP', self.port, 120), ('Discord Application ID', self.client_id, 260)]:
            col = ctk.CTkFrame(row, fg_color='transparent'); col.pack(side='left', padx=(0, 20))
            self.label(col, title, 11, MUTED).pack(anchor='w', pady=(0, 3))
            ctk.CTkEntry(col, textvariable=var, width=width, height=38, corner_radius=8,
                         fg_color='#10131d', border_color=BORDER, text_color=TEXT).pack(anchor='w')
        bar = ctk.CTkFrame(p, fg_color='transparent'); bar.pack(fill='x', pady=2)
        self.button(bar, 'Salvar configurações', self.save, True, width=180).pack(side='left')
        self.label(bar, 'A porta deve ser igual à do Switch.', 11, MUTED).pack(side='left', padx=16)

    def logs_page(self):
        p = self.pages['logs']
        self.heading(p, 'DIAGNÓSTICO', 'Atividade recente', 'Acompanhe testes de conexão, jogos e avisos do Discord.')
        self.logbox = ctk.CTkTextbox(p, fg_color=PANEL, text_color='#cbd4e5', corner_radius=14,
                                      border_width=1, border_color=BORDER, font=ctk.CTkFont(family='Consolas', size=12),
                                      state='disabled', wrap='word')
        self.logbox.pack(fill='both', expand=True)
        self.button(p, 'Limpar histórico', self.clear_log, width=145).pack(anchor='w', pady=12)

    def show(self, name):
        self.pages[name].tkraise()
        for key, button in self.nav.items():
            button.configure(fg_color='#292340' if key == name else 'transparent',
                             text_color='#c3b8ff' if key == name else MUTED)

    def copy_ip(self):
        address = self.ip.get()
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
