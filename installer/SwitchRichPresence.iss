; Instalador do companion para Windows (Inno Setup 6).
; Gere com: powershell -File build-release.ps1   (ou: ISCC installer\SwitchRichPresence.iss)
; Espera dist\SwitchRichPresence (PyInstaller) já contendo a pasta switch-sd.

#ifndef AppVersion
  #define AppVersion "1.2.0"
#endif
#define AppName "Switch Rich Presence"
#define AppExe "SwitchRichPresence.exe"
#define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"

[Setup]
AppId={{6F1D4C1E-6B0B-4D7B-9F2E-2A6A2C8B7525}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=tuliorobert
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=SwitchRichPresence-Setup-{#AppVersion}
SetupIconFile=..\icon.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Administrador apenas para a regra de firewall e Program Files.
PrivilegesRequired=admin
UsedUserAreasWarning=no
CloseApplications=yes
RestartApplications=no
InfoBeforeFile=antes-de-instalar.txt
InfoAfterFile=GUIA-SWITCH.txt

[Languages]
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "Criar atalho na área de trabalho"; GroupDescription: "Atalhos:"
Name: "autostart"; Description: "Iniciar com o Windows"; GroupDescription: "Inicialização:"
Name: "firewall"; Description: "Permitir o Switch na rede local (Firewall do Windows, redes privadas, UDP)"; GroupDescription: "Rede:"

[Files]
Source: "..\dist\SwitchRichPresence\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "GUIA-SWITCH.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Como instalar no Switch"; Filename: "{app}\GUIA-SWITCH.txt"
Name: "{group}\Arquivos para o cartão SD"; Filename: "{app}\switch-sd"
Name: "{group}\Desinstalar {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Mesmo valor que o aplicativo grava em Configurações > Iniciar com o Windows.
Root: HKCU; Subkey: "{#RunKey}"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"" --startup"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#AppName}"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""{#AppName}"" dir=in action=allow protocol=UDP profile=private program=""{app}\{#AppExe}"""; Flags: runhidden; Tasks: firewall; StatusMsg: "Configurando o Firewall do Windows..."
Filename: "{app}\{#AppExe}"; Parameters: "--page switch"; Description: "Abrir o {#AppName} e preparar o Switch"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""{#AppName}"""; Flags: runhidden; RunOnceId: "DelFirewall"
Filename: "{sys}\reg.exe"; Parameters: "delete ""HKCU\{#RunKey}"" /v ""{#AppName}"" /f"; Flags: runhidden; RunOnceId: "DelAutostart"
