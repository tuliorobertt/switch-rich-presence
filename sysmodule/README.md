# Sysmodule

O código em `source/main.c` consulta o processo de aplicação com `pm:shell`, obtém o Program ID com `pm:info`, lê o nome do jogo no control.nacp via `ns:am2` e envia atualizações UDP. A inicialização envia um pedido NIFM, aguarda o estado disponível e só então registra o socket. A configuração é relida durante a execução e o último estado é gravado em `sdmc:/config/switch-richpresence.status`.

O alvo `make -C sysmodule` gera `switch-richpresence-sysmodule.nsp`. O NPDM e o caminho de instalação usam o ID `4200000000075250`. A detecção e envio foram testados em um Switch com Atmosphère 1.11.2 CNX; outras configurações precisam ser verificadas.

O pacote Switch inclui `exefs.nsp`, `flags/boot2.flag`, o NRO e `config/switch-richpresence.ini` (com `host=auto`, busca automática). A instalação e a recuperação estão descritas no README principal.

O NRO edita IP, porta e estado da presença e os salva no SD. Ele também permite testar a conexão com o companion e alterar o marcador de início automático. O teste apenas verifica que o companion respondeu; a conexão do companion com Discord é independente. O status exibido pelo NRO pode ser antigo se o sysmodule não estiver em execução.

O script `package.ps1` cria o marcador de boot quando recebe `-EnableAutostart`. `-UpdateOnly` gera um pacote sem configuração e sem marcador de boot, para preservar os arquivos existentes no SD. Alterações de início automático precisam de reinício do console.
