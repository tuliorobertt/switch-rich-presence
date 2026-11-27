# Switch Rich Presence

Sysmodule para Nintendo Switch com Atmosphère que detecta o jogo aberto e envia o estado ao companion para Windows. O companion publica a atividade no Discord Desktop. O sysmodule e a comunicação com o PC foram testados em um Switch com firmware 22.5.0 e Atmosphère 1.11.2 CNX; outros ambientes podem exigir ajustes.

## Instalação

1. No PC, execute `SwitchRichPresence-Setup-<versão>.exe`. O instalador coloca o companion no PC, pode criar a regra do Firewall do Windows (UDP, redes privadas) e iniciar com o Windows, e guarda os arquivos do Switch na pasta `switch-sd` da instalação. Sem instalador, extraia o pacote Windows portátil e abra `SwitchRichPresence.exe`, mantendo a pasta `_internal` junto ao executável. Abra também o Discord Desktop e permita o compartilhamento de atividade nas configurações do Discord.
2. No companion, abra **Instalar no Switch** e use **Copiar para o cartão SD…** com o cartão do console no PC. Outra opção é extrair o pacote Switch na raiz do cartão SD, mesclando as pastas `atmosphere`, `config` e `switch`. Reinicie o console no Atmosphère. O passo a passo completo está em `installer/GUIA-SWITCH.txt`, que acompanha o instalador e o pacote Switch.
3. No NRO Switch Rich Presence, pressione **X** para ativar e salvar a busca automática do companion. Confirme que a porta UDP é a mesma do PC (por padrão `39421`). Switch e PC devem estar na mesma rede. Se solicitado, permita o aplicativo no Firewall do Windows para redes privadas.
4. Selecione **Testar conexão**. A interface do PC registra o teste. Abra um jogo e verifique o Discord.

A busca continua em segundo plano pelo sysmodule e tenta novamente quando o computador muda de rede ou fica indisponível. Ela envia broadcast e, se necessário, procura endereços individuais na sub-rede local, limitada a até 254 endereços. Pode demorar até cerca de 30 segundos para abandonar um companion que parou de responder e até 60 segundos entre varreduras completas. Redes de convidados, isolamento entre clientes ou firewall ainda podem bloquear a busca; nesse caso configure manualmente o IP do PC no NRO. Se houver mais de um companion, o primeiro que responder será escolhido e mantido enquanto responder. A porta do roteador não precisa ser encaminhada para uso na rede local. O protocolo UDP e a descoberta não autenticam dispositivos; use-os em uma rede local confiável. O teste do NRO verifica apenas a conexão com o companion, não com o Discord.

O sysmodule inicia automaticamente via `boot2.flag`. Se houver falha ao iniciar o Atmosphère, remova do SD a pasta `atmosphere/contents/4200000000075250`. Para desativar somente o início automático, remova o arquivo `flags/boot2.flag` dessa pasta. O NRO também pode alterar essa opção (vale no próximo boot) e iniciar ou parar o sysmodule na hora, sem reiniciar o console.

Na interface do NRO, use o direcional ou toque na tela para navegar, **A** (ou tocar de novo) para selecionar, **+** para salvar a configuração, **Y** para a ajuda rápida e **-** para sair. A tela mostra o estado do sysmodule com indicador colorido, o jogo atual, o IP do Switch e o companion em uso. A opção **Sysmodule** indica se o módulo está em execução e o inicia ou para sem reiniciar o console; ao parar, o NRO avisa o companion para remover a atividade. Se a interface gráfica não puder ser inicializada, o aplicativo tenta abrir em modo de texto. O desenho da interface usa as fontes do próprio Switch (incluindo os ícones de botões) e a biblioteca [stb_truetype](https://github.com/nothings/stb), cuja licença acompanha o arquivo-fonte em `source/vendor`.

## Companion

A tela inicial mostra o IP do PC, a atividade recebida e o estado do Discord. Em **Atividade**, veja o histórico e os erros. Em **Configurações**, escolha iniciar com o Windows e minimizar para a bandeja. Quando ambas estiverem ativas, o aplicativo inicia oculto na bandeja; clique no ícone para abri-lo ou sair. Ao minimizar, a janela também vai para a bandeja. Fechar a janela encerra o aplicativo.

A tela inicial também mostra a capa do jogo, o Title ID e de onde veio a imagem. **Personalizar jogo** define nome e imagem (endereço `https://`) para o jogo atual; as escolhas ficam em `%APPDATA%\SwitchRichPresence\custom-games.json` e valem na hora. **Instalar no Switch** mostra os passos de instalação e copia os arquivos do console para o cartão SD, mantendo a configuração e a escolha de início automático já existentes.

As configurações ficam em `%APPDATA%\SwitchRichPresence\settings.json`; o início automático usa a chave Run do usuário atual. Se mover o executável, desative e ative novamente o início automático. Alterar a porta no PC exige alterar também no NRO. O Application ID padrão pode ser trocado nas configurações. Nunca coloque tokens ou Client Secret no projeto.

Após 210 segundos sem eventos do Switch, a atividade é removida. Com o jogo fechado, o módulo informa “No Menu”; pausar a presença no NRO remove a atividade. Uma aplicação suspensa no menu HOME ainda pode ser detectada como jogo aberto. O nome exibido no topo da atividade depende do Discord e de sua aplicação.

## Fontes de nomes e imagens

`games.json` combina dados de [Switch Games JSON](https://github.com/Producdevity/switch-games-json), [switch-cheat](https://github.com/ibnux/switch-cheat/blob/master/GAMES.md), [Eden](https://github.com/liugh-dev/eden-emu/blob/master/dist/compatibility_list/compatibility_list.json) e das bases regionais do [TitleDB](https://github.com/blawar/titledb). `game-icons.json` relaciona IDs a URLs públicas de ícones do TitleDB. Quando um Title ID não tem imagem própria (por exemplo, a edição asiática de um jogo), o companion usa a imagem de outra região ou edição com o mesmo nome, inclusive demos e edições como “Complete Edition”. O sysmodule também envia o nome do jogo lido do próprio console, usado para jogos ausentes dos catálogos. Só sem nenhuma correspondência a logo do Switch é usada; o ID é exibido na tela Atividade e o jogo pode ser personalizado. A disponibilidade das imagens depende dos servidores externos e do Discord.

Para atualizar os catálogos do código-fonte, execute `python scripts/update_catalog.py` com acesso à internet. O script acrescenta nomes e imagens de Brasil, Estados Unidos, Reino Unido, Japão, Hong Kong, Coreia, Alemanha e França sem sobrescrever entradas existentes. Depois, recompile o aplicativo Windows para incluir os catálogos atualizados. A cobertura depende das fontes e não garante identificação de todos os títulos ou edições.

## Desenvolvimento e compilação

Requer Python 3 no Windows. Crie um ambiente virtual e instale `requirements.txt`; execute `python companion_gui.py`.

Para gerar uma release completa, execute:

```powershell
powershell -ExecutionPolicy Bypass -File build-release.ps1
```

O script compila o NRO e o sysmodule (devkitPro local ou, sem ele, Docker com a imagem `devkitpro/devkita64`), monta `dist/switch-sd`, gera `dist/SwitchRichPresence-Switch-<versão>.zip`, compila o companion com PyInstaller e gera `dist/SwitchRichPresence-Windows-<versão>.zip`. Com o [Inno Setup 6](https://jrsoftware.org/isinfo.php) instalado, também gera `dist/SwitchRichPresence-Setup-<versão>.exe` a partir de `installer/SwitchRichPresence.iss`. Use `-SkipSwitchBuild` para reaproveitar binários já compilados. A versão vem de `APP_VERSION` no `Makefile`.

Para compilar só o Switch com devkitPro/libnx:

```sh
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
make
make -C sysmodule
```

`sysmodule/package.ps1` gera pacotes com `-EnableAutostart` e pode preservar configuração com `-UpdateOnly`. O pacote de atualização deve ser extraído na raiz do SD; reinicie o Switch para carregar o módulo atualizado.

## Protocolo

O sysmodule envia JSON UTF-8 por UDP: `{"title_id":"0100152000022000","state":"playing","title":"Mario Kart 8 Deluxe"}`. `title` é opcional e vem do control.nacp do console; o nome do catálogo ou personalizado tem prioridade. `idle` representa o menu e `paused` remove a presença. Heartbeats são enviados periodicamente. O projeto usa Discord Desktop local; não é necessário token pessoal.
