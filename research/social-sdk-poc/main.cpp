// Experimental desktop proof of concept using the official Discord Social SDK.
// No OAuth codes or tokens are printed or persisted by this application.
#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

constexpr uint64_t APP_ID = 1565506238114959380ULL;
static std::atomic<bool> running{true};

static void stop(int) { running = false; }

int main() {
    std::signal(SIGINT, stop);
    auto client = std::make_shared<discordpp::Client>();
    client->SetStatusChangedCallback([client](discordpp::Client::Status status,
                                              discordpp::Client::Error error, int32_t detail) {
        std::cout << "Estado SDK: " << discordpp::Client::StatusToString(status) << std::endl;
        if (status == discordpp::Client::Status::Ready) {
            discordpp::Activity activity;
            activity.SetType(discordpp::ActivityTypes::Playing);
            activity.SetDetails("Teste de presenca independente");
            activity.SetState("Switch Rich Presence");
            client->UpdateRichPresence(activity, [](discordpp::ClientResult result) {
                if (result.Successful()) {
                    std::cout << "Atividade enviada. Confira seu perfil no Discord.\n";
                } else {
                    std::cerr << "Falha ao atualizar atividade: " << result.Error() << std::endl;
                }
            });
        } else if (error != discordpp::Client::Error::None) {
            std::cerr << "Erro de conexao: " << discordpp::Client::ErrorToString(error)
                      << " (detalhe " << detail << ")\n";
        }
    });

    auto verifier = client->CreateAuthorizationCodeVerifier();
    discordpp::AuthorizationArgs args{};
    args.SetClientId(APP_ID);
    args.SetScopes(discordpp::Client::GetDefaultPresenceScopes());
    args.SetCodeChallenge(verifier.Challenge());
    std::cout << "Autorize o aplicativo na pagina oficial do Discord.\n";
    std::cout << "Depois, feche o Discord Desktop e confira o perfil no celular.\n";
    std::cout << "Mantenha esta janela aberta; Ctrl+C encerra o teste.\n";

    client->Authorize(args, [client, verifier](auto result, auto code, auto redirect) {
        if (!result.Successful()) {
            std::cerr << "Falha na autorizacao: " << result.Error() << std::endl;
            running = false;
            return;
        }
        client->GetToken(APP_ID, code, verifier.Verifier(), redirect,
            [client](discordpp::ClientResult result, std::string access_token,
                     std::string, discordpp::AuthorizationTokenType token_type,
                     int32_t, std::string) {
                if (!result.Successful()) {
                    std::cerr << "Falha ao obter autorizacao: " << result.Error() << std::endl;
                    running = false;
                    return;
                }
                client->UpdateToken(token_type, access_token, [client](discordpp::ClientResult update) {
                    if (!update.Successful()) {
                        std::cerr << "Falha ao iniciar sessao: " << update.Error() << std::endl;
                        running = false;
                        return;
                    }
                    client->Connect();
                });
            });
    });
    while (running) {
        discordpp::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}
