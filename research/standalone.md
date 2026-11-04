# Pesquisa: presença sem o computador

Esta é uma pesquisa na branch `standalone-version`, não uma implementação funcional de Rich Presence independente. A versão existente continua usando UDP e Discord Desktop; o experimento abaixo não altera essa configuração nem o sysmodule.

## Resultado em hardware

Em 3 de novembro de 2026, o usuário executou o `https-probe` no Switch e relatou DNS resolvido, TCP conectado, validação TLS bem-sucedida e `HTTP/1.1 200 OK` para o endpoint público do Discord. Isso confirma a conexão desse **NRO em primeiro plano** naquela rede, sem credenciais; não comprova que o sysmodule em segundo plano consiga usar os mesmos serviços, nem que exista uma forma suportada de publicar presença diretamente.

Na prova de conceito para Windows, o usuário relatou os estados `Connecting`, `Connected` e `Ready`, seguidos da confirmação de envio da atividade pelo Social SDK. O Discord Desktop já estava fechado quando o teste iniciou, e a atividade foi vista no Discord de outra pessoa, em um celular. Isso confirma a publicação pelo SDK em execução no PC sem o Discord Desktop; o teste não executa o SDK no Switch.

## O que está confirmado

- O sysmodule já identifica o processo do jogo e envia eventos de mudança e heartbeat por UDP. O companion no PC resolve nomes e imagens e publica via IPC local do Discord Desktop.
- O [Discord Social SDK](https://discord.com/developers/docs/social-sdk/index.html) oferece autenticação OAuth e Rich Presence, inclusive uma sessão conectada aos serviços do Discord. O escopo de presença é documentado no [guia oficial de autenticação](https://github.com/discord/discord-api-docs/blob/main/developers/discord-social-sdk/development-guides/account-linking-with-discord.mdx).
- Para consoles suportados, o [SDK documenta autorização de dispositivo](https://discord.com/developers/docs/slayer-sdk/authentication.html): o usuário autoriza em outro dispositivo usando código ou QR, e o SDK para cliente público obtém as credenciais. Isso descreve um fluxo oficial para plataformas suportadas; não comprova que o SDK possa ser executado no homebrew do Switch nem que o fluxo possa ser reproduzido independentemente dele.
- A [lista de plataformas para consoles](https://support-dev.discord.com/hc/en-us/articles/30127085446039-Introducing-the-Discord-Social-SDK) cita Xbox e PlayStation e exige aprovação de middleware; não apresenta um SDK para homebrew no Switch.
- A [versão 1.10 do SDK](https://discord.com/developers/docs/social-sdk/release_notes.html) também descreve publicação por RPC no cliente Discord para Android. Um aplicativo de celular com o SDK poderia ser pesquisado como alternativa sem PC, mas ainda exige outro dispositivo e não foi testado neste projeto.
- O [libnx](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/ssl.h) expõe sockets e serviço TLS de cliente. Isso não demonstra que exista uma API pública compatível para publicar presença diretamente do Switch.

## Caminhos e lacunas

| Caminho | Situação | Próximo teste |
|---|---|---|
| Switch → Discord, sem outros dispositivos | Não há suporte oficial identificado para o Social SDK no ambiente homebrew. OAuth, sozinho, não comprova que um cliente possa publicar presença. | Confirmar com a documentação ou suporte do Discord se há integração e plataforma autorizadas para esse ambiente. |
| Switch → serviço próprio → Discord | O SDK tem suporte a Linux e autenticação, mas suporte técnico e autorização para hospedar sessões dessa maneira **não foram confirmados**. | Verificar termos e suporte do Discord; só então testar o SDK oficial conectado, com uma conta de teste. |
| Switch → Android → Discord | A documentação do SDK menciona RPC ao cliente Android; exige um aplicativo no celular e execução em segundo plano. | Confirmar restrições de execução, integração e requisitos de instalação antes de implementar. |

Não usar token pessoal, endpoint privado, engenharia reversa do SDK ou autenticação de bot para simular a presença de uma conta de usuário. A [política de contas automatizadas](https://support.discord.com/hc/en-us/articles/115002192352-Automated-User-Accounts-Self-Bots) e os [termos do Social SDK](https://support-dev.discord.com/hc/en-us/articles/30225844245271-Discord-Social-SDK-Terms) devem orientar a escolha da integração.

Se uma opção autorizada de publicação for confirmada, o desenho precisa prever consentimento OAuth, vínculo único com o console, expiração e renovação de credenciais, TLS com validação de certificado, desconexão, remoção de atividade após perda de comunicação e mensagens de erro. O IP local não identifica uma conta; um eventual intermediário precisará autenticar o console e associá-lo à conta vinculada.

## Primeiro experimento: HTTPS a partir do Switch

O NRO `https-probe` é um diagnóstico isolado: resolve `discord.com`, abre TCP, negocia TLS usando a verificação padrão de certificado e hostname do serviço SSL do Switch e consulta o endpoint público `/api/v10/gateway`. Não usa OAuth, não solicita tokens, não publica atividade e não altera arquivos do SD.

Compile com o toolchain do projeto:

```sh
make -C research/https-probe
```

Copie `switch-https-probe.nro` para `switch/switch-https-probe/` no SD. No Homebrew Menu, abra **Switch HTTPS Probe** e pressione **A**. Anote a última etapa, o resultado hexadecimal ou a linha HTTP; **−** fecha o app depois que o teste retorna. O teste usa timeouts nos sockets, mas a resolução DNS ou serviços do sistema podem demorar; não representa um teste de funcionamento em segundo plano.

Resultado HTTP, inclusive erro HTTP, comprova apenas que uma resposta HTTPS chegou após a negociação; não comprova autenticação, Rich Presence ou suporte oficial. Erros podem envolver DNS, acesso à internet, bloqueios de rede, relógio ou certificados. O teste em hardware acima foi relatado pelo usuário.

## Critérios para avançar

1. **Concluído:** confirmar HTTPS no NRO em primeiro plano no console usado pelo usuário.
2. Validar um caminho de publicação suportado e os termos aplicáveis; não assumir que conectar por HTTPS permita enviar presença de usuário.
3. **Concluído no Windows:** o SDK autorizou a conta e publicou uma atividade que foi vista por outra pessoa enquanto o Discord Desktop estava fechado. O programa de teste ainda rodava no PC.
4. Apenas após a prova, verificar serviços em segundo plano e projetar a integração com o sysmodule, sem expor tokens em logs ou configurações públicas.

O código inicial da prova de conceito para Windows está em [`social-sdk-poc`](social-sdk-poc/README.md). Foi compilado com o SDK oficial; o usuário confirmou a atividade visível em outro dispositivo com o Discord Desktop fechado. Ele não demonstra suporte ao Switch.
