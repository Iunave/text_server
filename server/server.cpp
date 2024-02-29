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
#include <unordered_map>

#include "common.hpp"

struct client_t
{
    pthread_t listener;
    sockaddr_in address;
    int32_t socket;
};

struct stored_message_t
{
    client_message_header_t header;
    std::unique_ptr<uint8_t[]> data;
    std::shared_ptr<client_t> client;
};

std::string client_address_string(const client_t* client)
{
    return std::string{inet_ntoa(client->address.sin_addr)} + ":" + std::to_string(client->address.sin_port);
}

inline std::atomic<bool> shutdown_server = false;
inline std::vector<client_t> clients{};
inline std::vector<stored_message_t> messages{};
inline pthread_mutex_t messages_mx{};
inline pthread_cond_t messages_cv{};
inline std::unordered_map<client_t*, std::string> usernames{};

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

ssize_t send_message(int32_t to_socket, server_message_type type, uint16_t size, void* data)
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
    void login(stored_message_t& message)
    {
        std::string username{reinterpret_cast<char*>(message.data.get())};
        std::printf("recieved login from %s with username %s\n", client_address_string(message.client.get()).c_str(), username.c_str());

        std::string login_response = "success";

        auto[it, unique] = usernames.insert(std::make_pair(message.client.get(), username));
        if(!unique)
        {
            login_response = "already logged in";
        }
        else
        {
            for(auto user_it = usernames.begin(); user_it != usernames.end(); ++user_it)
            {
                if(it != user_it && user_it->second == username)
                {
                    login_response = "another user with that name already exists";
                    usernames.erase(it);
                    it = usernames.end();
                    break;
                }
            }
        }

        ssize_t ret = send_message(message.client->socket, server_message_type::login_response, login_response.size() + 1, login_response.data());
        if(ret <= 0)
        {
            if(it != usernames.end())
            {
                usernames.erase(it);
            }
        }
    }

    void text(stored_message_t& message)
    {
        std::printf("%s %s\n ", client_address_string(message.client.get()).c_str(), reinterpret_cast<char*>(message.data.get()));
    }
}

inline constexpr auto client_message_responses = []() constexpr
{
    std::array<void(*)(stored_message_t&), static_cast<size_t>(client_message_type::MAX)> responses{};
    responses[static_cast<uint64_t>(client_message_type::login)] = &response::login;
    responses[static_cast<uint64_t>(client_message_type::text)] = &response::text;
    return responses;
}();

void* client_listener_routine(void* user_data)
{
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    std::shared_ptr<client_t> client{static_cast<client_t*>(user_data)};

    while(true)
    {
        client_message_t message{};
        ssize_t ret = recieve_message(&message, client->socket);

        if(ret == 0)
        {
            std::printf("client disconnected %s\n", client_address_string(client.get()).c_str());
            pthread_exit(nullptr);
        }
        else if(ret == -1)
        {
            perror(client_address_string(client.get()).c_str());
            pthread_exit(nullptr);
        }

        pthread_mutex_lock(&messages_mx);

        stored_message_t& stored_message = messages.emplace_back();
        stored_message.header = message.header;
        stored_message.data = std::move(message.data);
        stored_message.client = client;

        pthread_cond_signal(&messages_cv);
        pthread_mutex_unlock(&messages_mx);
    }

    pthread_exit(nullptr);
}

void* message_queue_handler(void* user_data)
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

        for(stored_message_t& message : moved_messages)
        {
            uint64_t message_type = static_cast<uint64_t>(message.header.type);

            if(message_type < client_message_responses.size())
            {
                client_message_responses[message_type](message);
            }
            else
            {
                std::printf("invalid message type %lu. ignoring\n", message_type);
            }
        }

        moved_messages.clear();
    }

    pthread_exit(nullptr);
}

void signal_handler(int signal, siginfo_t* info, void* user_data)
{
    if(signal == SIGTERM)
    {
        shutdown_server.store(true, std::memory_order_relaxed);
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

    while(!shutdown_server.load(std::memory_order_relaxed))
    {
        client_t new_client{};
        socklen_t client_len = sizeof(new_client.address);

        new_client.socket = accept(server_socket, reinterpret_cast<sockaddr*>(&new_client.address), &client_len);

        if(new_client.socket == -1)
        {
            if(errno != EINTR)
            {
                perror("error accepting connection");
            }
            continue;
        }

        std::printf("accepted connection from %s\n", inet_ntoa(new_client.address.sin_addr));

        for(uint64_t idx = 0; idx < clients.size(); ++idx)
        {
            if(pthread_tryjoin_np(clients[idx].listener, nullptr) == 0)
            {
                if(close(clients[idx].socket) == -1)
                {
                    perror("failed to close client socket");
                }

                clients[idx] = clients.back();
                clients.pop_back();
                --idx;
            }
        }

        auto* client_copy = new client_t{new_client};
        if(pthread_create(&new_client.listener, nullptr, &::client_listener_routine, client_copy) != 0)
        {
            perror("failed to launch client listener thread");
            delete client_copy;
            continue;
        }

        clients.emplace_back(new_client);
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