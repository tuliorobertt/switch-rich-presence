# Switch Rich Presence

Sysmodule para Nintendo Switch com Atmosphère que detecta o jogo aberto e envia o estado ao companion para Windows. O companion publica a atividade no Discord Desktop. O sysmodule e a comunicação com o PC foram testados em um Switch com firmware 22.5.0 e Atmosphère 1.11.2 CNX; outros ambientes podem exigir ajustes.

## Instalação

1. No PC, extraia o pacote Windows e abra `SwitchRichPresence.exe`. Mantenha a pasta `_internal` junto ao executável. Abra também o Discord Desktop e permita o compartilhamento de atividade nas configurações do Discord.
2. Extraia o pacote Switch na raiz do cartão SD, mesclando as pastas `atmosphere`, `config` e `switch`. Reinicie o console no Atmosphère.
3. No NRO Switch Rich Presence, configure o IP do PC mostrado na tela inicial do companion, confirme a porta UDP `39421` e pressione **+** para salvar. Switch e PC devem estar na mesma rede. Se solicitado, permita o aplicativo no Firewall do Windows para redes privadas.
4. Selecione **Testar conexão**. A interface do PC registra o teste. Abra um jogo e verifique o Discord.

A porta do roteador não precisa ser encaminhada para uso na rede local. O protocolo UDP não autentica dispositivos; use-o em uma rede local confiável. O teste do NRO verifica apenas a conexão com o companion, não com o Discord.

O sysmodule inicia automaticamente via `boot2.flag`. Se houver falha ao iniciar o Atmosphère, remova do SD a pasta `atmosphere/contents/4200000000075250`. Para desativar somente o início automático, remova o arquivo `flags/boot2.flag` dessa pasta. O NRO também pode alterar essa opção; reinicie para aplicar.

## Companion

A tela inicial mostra o IP do PC, a atividade recebida e o estado do Discord. Em **Atividade**, veja o histórico e os erros. Em **Configurações**, escolha iniciar com o Windows e minimizar para a bandeja. Quando ambas estiverem ativas, o aplicativo inicia oculto na bandeja; clique no ícone para abri-lo ou sair. Ao minimizar, a janela também vai para a bandeja. Fechar a janela encerra o aplicativo.

As configurações ficam em `%APPDATA%\SwitchRichPresence\settings.json`; o início automático usa a chave Run do usuário atual. Se mover o executável, desative e ative novamente o início automático. Alterar a porta no PC exige alterar também no NRO. O Application ID padrão pode ser trocado nas configurações. Nunca coloque tokens ou Client Secret no projeto.

Após 210 segundos sem eventos do Switch, a atividade é removida. Com o jogo fechado, o módulo informa “No Menu”; pausar a presença no NRO remove a atividade. Uma aplicação suspensa no menu HOME ainda pode ser detectada como jogo aberto. O nome exibido no topo da atividade depende do Discord e de sua aplicação.

## Fontes de nomes e imagens

`games.json` combina dados de [Switch Games JSON](https://github.com/Producdevity/switch-games-json), [switch-cheat](https://github.com/ibnux/switch-cheat/blob/master/GAMES.md) e [Eden](https://github.com/liugh-dev/eden-emu/blob/master/dist/compatibility_list/compatibility_list.json). `game-icons.json` relaciona IDs a URLs públicas de ícones do [TitleDB](https://github.com/blawar/titledb). Jogos ausentes usam Title ID ou logo padrão. A disponibilidade das imagens depende dos servidores externos e do Discord.

## Desenvolvimento e compilação

Requer Python 3 no Windows. Crie um ambiente virtual e instale `requirements.txt`; execute `python companion_gui.py`. Para compilar o companion, use `python -m PyInstaller --noconfirm SwitchRichPresence.spec`. O executável e os arquivos necessários ficam em `dist/SwitchRichPresence`.

Para compilar o Switch com devkitPro/libnx:

```sh
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
make
make -C sysmodule
```

`sysmodule/package.ps1` gera pacotes com `-EnableAutostart` e pode preservar configuração com `-UpdateOnly`. O pacote de atualização deve ser extraído na raiz do SD; reinicie o Switch para carregar o módulo atualizado.

## Protocolo

O sysmodule envia JSON UTF-8 por UDP: `{"title_id":"0100152000022000","state":"playing"}`. `idle` representa o menu e `paused` remove a presença. Heartbeats são enviados periodicamente. O projeto usa Discord Desktop local; não é necessário token pessoal.
