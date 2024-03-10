#include <csignal>
#include <cstdint>
#include <memory>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <array>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cassert>
#include <pthread.h>
#include <atomic>
#include <vector>
#include <sys/poll.h>
#include <atomic>
#include <chrono>

#include "common.hpp"
#include "slotmap/slotmap.hpp"

struct client_t
{
    char username[32] = {'\0'};
    pthread_t listener = -1;
    sockaddr_in address = {static_cast<sa_family_t>(-1)};
    int32_t socket = -1;
};

struct connected_client_t
{
    sockaddr_in address = {static_cast<sa_family_t>(-1)};
    int32_t socket = -1;
};

struct SlotMapClientTraits
{
    static constexpr int64_t IndexBits = 32;
    static constexpr int64_t IdBits = 64 - IndexBits;
    static constexpr int64_t MinFreeKeys = 64;
    static constexpr int64_t AllocationSize = 512;
};

sig_atomic_t shutdown_server = 0;

SlotMap<client_t, SlotMapClientTraits> clients{};
pthread_rwlock_t clients_lock{};

struct stored_message_t
{
    client_message_header_t header = {};
    std::unique_ptr<uint8_t[]> data = nullptr;
    decltype(clients)::KeyHandle client = {};
};

std::vector<stored_message_t> messages{};
pthread_mutex_t messages_mx{};
pthread_cond_t messages_cv{};

void* client_listener_routine(void* user_data);

std::string address2string(sockaddr_in address)
{
    return std::string{inet_ntoa(address.sin_addr)} + ":" + std::to_string(address.sin_port);
}

ssize_t recieve_message(client_message_t* out_message, int32_t client_socket)
{
    ssize_t ret = recieve_bytes(client_socket, &out_message->header, sizeof(out_message->header));
    if(ret == 0 || ret == -1)
    {
        return ret;
    }

    out_message->data = std::make_unique<uint8_t[]>(out_message->header.size);
    ret = recieve_bytes(client_socket, out_message->data.get(), out_message->header.size);
    return ret;
}

ssize_t send_message(int32_t to_socket, server_message_type type, uint16_t size, const void* data)
{
    server_message_header_t header{
            .type = type,
            .size = size
    };

    ssize_t ret = send_bytes(to_socket, &header, sizeof(header), MSG_MORE);
    if(ret == 0 || ret == -1)
    {
        return ret;
    }

    ret = send_bytes(to_socket, data, size);
    return ret;
}

namespace response
{
    void connected(stored_message_t& message)
    {
        auto connected_client = reinterpret_cast<connected_client_t*>(message.data.get());

        pthread_rwlock_wrlock(&clients_lock);

        auto client_handle = clients.Add();
        client_t* new_client = clients[client_handle];

        new_client->address = connected_client->address;
        new_client->socket = connected_client->socket;

        static_assert(sizeof(void*) == sizeof(decltype(client_handle)));

        int32_t ret = pthread_create(&new_client->listener, nullptr, &::client_listener_routine, std::bit_cast<void*>(client_handle));
        if(ret != 0)
        {
            std::printf("failed to launch listener thread: %i\n", ret);
            clients.Remove(client_handle);
        }
        else
        {
            std::printf("added connection from %s\n", address2string(connected_client->address).c_str());
        }

        pthread_rwlock_unlock(&clients_lock);
    }

    void disconnected(stored_message_t& message)
    {
        auto disconnection_reason = reinterpret_cast<ssize_t*>(message.data.get());
        client_t disconnected_client;

        pthread_rwlock_rdlock(&clients_lock);
        disconnected_client = *clients[message.client];
        pthread_rwlock_unlock(&clients_lock);

        if(*disconnection_reason == 0)
        {
            std::printf("client %s disconnected\n", address2string(disconnected_client.address).c_str());
        }
        else
        {
            std::printf("client %s disconnected erronously %s\n", address2string(disconnected_client.address).c_str(), strerror(*disconnection_reason));
        }

        pthread_rwlock_wrlock(&clients_lock);
        clients.Remove(message.client);
        pthread_rwlock_unlock(&clients_lock);

        pthread_join(disconnected_client.listener, nullptr);
    }

    void login(stored_message_t& message)
    {
        char username[32]{0};
        for(uint64_t idx = 0; message.data.get()[idx] != 0; ++idx)
        {
            username[idx] = reinterpret_cast<char*>(message.data.get())[idx];
        }

        std::string login_response = "success";

        pthread_rwlock_rdlock(&clients_lock);
        const client_t client = *clients[message.client];

        if(client.username[0] != '\0')
        {
            login_response = "client already logged in";
        }
        else
        {
            for(client_t& existing_client : clients)
            {
                if(std::memcmp(&username[0], &existing_client.username[0], 32) == 0)
                {
                    login_response = "username not unique";
                    break;
                }
            }
        }

        pthread_rwlock_unlock(&clients_lock);

        std::printf("recieved login request from %s with username: %s, result: %s\n", address2string(client.address).c_str(), username, login_response.c_str());

        if(login_response == "success")
        {
            pthread_rwlock_wrlock(&clients_lock);
            std::memcpy(&clients[message.client]->username[0], &username[0], 32);
            pthread_rwlock_unlock(&clients_lock);
        }

        (void)send_message(client.socket, server_message_type::login_response, login_response.length() + 1, login_response.c_str());
    }

    void text(stored_message_t& message)
    {
        pthread_rwlock_rdlock(&clients_lock);
        const client_t sender = *clients[message.client];
        pthread_rwlock_unlock(&clients_lock);

        if(sender.username[0] != '\0')
        {
            std::string text_message = std::string{sender.username} + ": " + std::string{reinterpret_cast<char*>(message.data.get())};
            std::printf("%s\n", text_message.c_str());

            pthread_rwlock_rdlock(&clients_lock);

            for(client_t& client : clients)
            {
                if(client.socket != sender.socket)
                {
                    (void)send_message(client.socket, server_message_type::text, text_message.length() + 1, text_message.c_str());
                }
            }

            pthread_rwlock_unlock(&clients_lock);
        }
    }
}

inline constexpr auto client_message_responses = []() constexpr
{
    std::array<void(*)(stored_message_t&), static_cast<size_t>(client_message_type_t::MAX)> responses{};
    responses[static_cast<size_t>(client_message_type_t::connected)] = &response::connected;
    responses[static_cast<size_t>(client_message_type_t::disconnected)] = &response::disconnected;
    responses[static_cast<size_t>(client_message_type_t::login)] = &response::login;
    responses[static_cast<size_t>(client_message_type_t::text)] = &response::text;
    return responses;
}();

void* client_listener_routine(void* user_data)
{
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    auto client_handle = std::bit_cast<decltype(clients)::KeyHandle>(user_data);

    pthread_rwlock_rdlock(&clients_lock);
    const int32_t socket = clients[client_handle]->socket;
    pthread_rwlock_unlock(&clients_lock);

    while(true)
    {
        client_message_t recieved_message{};
        ssize_t ret = recieve_message(&recieved_message, socket);

        stored_message_t stored_message{};

        if(ret == 0 || ret == -1)
        {
            stored_message.header.type = client_message_type_t::disconnected;
            stored_message.header.size = sizeof(uint64_t);
            stored_message.client = client_handle;
            stored_message.data = std::make_unique<uint8_t[]>(sizeof(ssize_t));

            if(ret == 0)
            {
                *reinterpret_cast<ssize_t*>(stored_message.data.get()) = 0;
            }
            else
            {
                *reinterpret_cast<ssize_t*>(stored_message.data.get()) = errno;
            }
        }
        else
        {
            stored_message.header = recieved_message.header;
            stored_message.client = client_handle;
            stored_message.data = std::move(recieved_message.data);
        }

        pthread_mutex_lock(&messages_mx);

        messages.emplace_back(std::move(stored_message));

        pthread_cond_signal(&messages_cv);
        pthread_mutex_unlock(&messages_mx);

        if(ret == 0 || ret == -1)
        {
            pthread_exit(nullptr);
        }
    }
}

void* message_queue_handler(void*)
{
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    while(true)
    {
        pthread_mutex_lock(&messages_mx);

        while(messages.empty())
        {
            pthread_cond_wait(&messages_cv, &messages_mx);
        }

        static std::vector<stored_message_t> moved_messages{};
        std::swap(moved_messages, messages);

        pthread_mutex_unlock(&messages_mx);

        for(stored_message_t& message : moved_messages) //process connections and disconnections first
        {
            auto msgtype = message.header.type;
            if(msgtype == client_message_type_t::connected || msgtype == client_message_type_t::disconnected)
            {
                client_message_responses[static_cast<size_t>(msgtype)](message);
            }
        }

        for(stored_message_t& message : moved_messages)
        {
            auto msgtype = message.header.type;
            if(msgtype != client_message_type_t::connected && msgtype != client_message_type_t::disconnected)
            {
                client_message_responses[static_cast<size_t>(msgtype)](message);
            }
        }

        moved_messages.clear();
    }
}

void signal_handler(int signal, siginfo_t* info, void* user_data)
{
    if(signal == SIGTERM)
    {
        shutdown_server = 1;
    }
}

int32_t main(int32_t argc, const char** argv)
{
    if(argc != 2)
    {
        std::printf("specify a port number as argument to this program");
        return EXIT_FAILURE;
    }

    const uint16_t server_port = std::strtoul(argv[1], nullptr, 10);
    if(errno != 0)
    {
        perror("error parsing port number");
        return EXIT_FAILURE;
    }

    struct sigaction signal_action{};
    signal_action.sa_sigaction = signal_handler;
    sigaction(SIGTERM | SIGINT, &signal_action, nullptr);

    pthread_rwlock_init(&clients_lock, nullptr);
    pthread_mutex_init(&messages_mx, nullptr);
    pthread_cond_init(&messages_cv, nullptr);

    std::printf("creating tcp socket\n");

    int32_t server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(server_socket == -1)
    {
        perror("failed to create tcp socket");
        return EXIT_FAILURE;
    }

    const sockaddr_in server_addr{
            .sin_family = AF_INET,
            .sin_port = htons(server_port),
            .sin_addr = {INADDR_ANY}
    };

    if(bind(server_socket, reinterpret_cast<const sockaddr*>(&server_addr), sizeof(server_addr)) == -1)
    {
        perror("failed to bind tcp socket");
        return EXIT_FAILURE;
    }

    std::printf("bound socket to %s port %i\n", inet_ntoa(server_addr.sin_addr), server_port);

    if(listen(server_socket, 64) == -1)
    {
        perror("failed to listen to tcp socket");
        return EXIT_FAILURE;
    }

    pthread_t message_handler_thread{};
    if(pthread_create(&message_handler_thread, nullptr, &::message_queue_handler, nullptr) != 0)
    {
        perror("failed to launch message handler thread");
        return EXIT_FAILURE;
    }

    while(shutdown_server == 0)
    {
        sockaddr_in client_address{};
        socklen_t address_len = sizeof(client_address);

        int32_t client_socket = accept(server_socket, reinterpret_cast<sockaddr*>(&client_address), &address_len);
        if(client_socket == -1)
        {
            if(errno != EINTR)
            {
                perror("error accepting connection");
            }
            continue;
        }

        stored_message_t stored_message{};
        stored_message.header.type = client_message_type_t::connected;
        stored_message.header.size = sizeof(connected_client_t);
        stored_message.client = decltype(clients)::NullHandle;
        stored_message.data = std::make_unique<uint8_t[]>(sizeof(connected_client_t));
        reinterpret_cast<connected_client_t*>(stored_message.data.get())->address = client_address;
        reinterpret_cast<connected_client_t*>(stored_message.data.get())->socket = client_socket;

        pthread_mutex_lock(&messages_mx);

        messages.emplace_back(std::move(stored_message));

        pthread_cond_signal(&messages_cv);
        pthread_mutex_unlock(&messages_mx);
    }

    for(const client_t& client : clients)
    {
        (void)pthread_cancel(client.listener);

        if(close(client.socket) == -1)
        {
            perror("failed to close client socket");
        }
    }

    (void)pthread_cancel(message_handler_thread);

    if(close(server_socket) == -1)
    {
        perror("failed to close server socket");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}