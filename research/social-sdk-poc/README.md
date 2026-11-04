# Teste experimental do Discord Social SDK no Windows

Este teste usa temporariamente o PC para verificar se o SDK oficial consegue publicar uma atividade após autenticação OAuth, inclusive com o Discord Desktop fechado. Não é uma versão standalone para Switch. O código não grava nem imprime códigos ou tokens; o SDK não acompanha este repositório.

## Executar o teste compilado neste projeto

O executável de teste foi compilado localmente contra o SDK fornecido. Antes de executá-lo, confira na aplicação `1565506238114959380` do [Discord Developer Portal](https://discord.com/developers/applications) que o Social SDK está configurado, **Public Client** está ativo em OAuth2 e `http://127.0.0.1/callback` consta em Redirects. Consulte o [guia oficial C++](https://docs.discord.com/developers/discord-social-sdk/getting-started/using-c%2B%2B) se as opções não estiverem disponíveis.

Abra o PowerShell na raiz do projeto e execute:

```powershell
& .\research\social-sdk-poc\build\SocialPresenceProbe.exe
```

Autorize na página oficial que abrir. Quando o programa informar que a atividade foi enviada, feche totalmente o Discord Desktop, inclusive na bandeja, mantendo o teste aberto. Confira no celular ou em outra conta se a atividade permanece. Se necessário, reinicie o teste com o Discord Desktop fechado para verificar a conexão sem o aplicativo. `Ctrl+C` encerra o programa.

Compartilhe apenas as mensagens de estado ou erro, sem credenciais. O teste foi compilado e o usuário confirmou uma atividade visível em outro dispositivo com o Discord Desktop fechado. Não envie senha, token, código OAuth ou Client Secret.

## Recompilar

1. No [Discord Developer Portal](https://discord.com/developers/applications), abra a aplicação `1565506238114959380`. Configure o Social SDK, ative **Public Client** em OAuth2 e adicione `http://127.0.0.1/callback` aos Redirects. Essas opções são descritas no [guia oficial C++](https://docs.discord.com/developers/discord-social-sdk/getting-started/using-c%2B%2B) e podem depender das permissões da aplicação.
2. Na seção **Social SDK → Downloads** dessa aplicação, baixe o SDK para C++ e extraia-o em `research/social-sdk-poc/sdk/discord_social_sdk/`. Essa pasta deve conter `include/discordpp.h`, `lib/release/discord_partner_sdk.lib` e `bin/release/discord_partner_sdk.dll`. Se o ZIP tiver uma pasta superior, ajuste o caminho para evitar uma pasta duplicada. O SDK fica fora do Git.
3. Instale Visual Studio 2022 ou Build Tools com **Desktop development with C++**, incluindo CMake para Windows. Abra um PowerShell na pasta `research/social-sdk-poc` e execute:

   ```powershell
   cmake -S . -B build -A x64
   cmake --build build --config Release
   .\build\Release\SocialPresenceProbe.exe
   ```

   Se o SDK estiver em outro local, informe `-DDISCORD_SDK_ROOT="C:/caminho/discord_social_sdk"` no primeiro comando.

## Execução

Autorize a aplicação na página oficial que abrir. **Não envie tokens, códigos ou Client Secret.** Depois de o programa indicar que a atividade foi enviada, feche completamente o Discord Desktop (inclusive a bandeja) e confira seu perfil no celular ou com outra conta. Mantenha o programa aberto durante a observação; `Ctrl+C` encerra.

Anote somente os estados e eventuais mensagens de erro sem credenciais. Se a atividade desaparecer ao fechar o Discord Desktop, isso é resultado relevante. O SDK pode depender da configuração ou elegibilidade da aplicação.
