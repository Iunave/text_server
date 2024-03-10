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
#include <iostream>

#include "common.hpp"

ssize_t send_message(int32_t to_socket, client_message_type_t type, uint16_t size, const void* data)
{
    client_message_header_t header{
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

ssize_t recieve_message(server_message_t* out_message, int32_t server_socket)
{
    ssize_t ret = recieve_bytes(server_socket, &out_message->header, sizeof(out_message->header));
    if(ret == 0 || ret == -1)
    {
        return ret;
    }

    out_message->data = std::make_unique<uint8_t[]>(out_message->header.size);
    ret = recieve_bytes(server_socket, out_message->data.get(), out_message->header.size);
    return ret;
}

std::string prompt_username()
{
    auto valid_username = [](std::string_view username)
    {
        for(char letter : username)
        {
            if(username.length() >= 32)
            {
                return false;
            }

            if(!(letter >= '!' && letter <= '~')) //space not included
            {
                return false;
            }
        }
        return true;
    };

    while(true)
    {
        std::printf("login username: ");

        std::string username{};
        getline(std::cin, username);

        if(valid_username(username))
        {
            return username;
        }
        else
        {
            std::printf("invalid username\n");
        }
    }
}

std::string prompt_text()
{
    auto valid_text = [](std::string_view text)
    {
        for(char letter : text)
        {
            if(!(letter >= ' ' && letter <= '~'))
            {
                return false;
            }
        }
        return true;
    };

    while(true)
    {
        std::string text{};
        getline(std::cin, text);

        if(valid_text(text))
        {
            return text;
        }
        else
        {
            std::printf("invalid text message\n");
        }
    }
}

inline int32_t server_socket = -1;
std::atomic<bool> has_connection = false;

void* server_listener(void* user_data)
{
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);

    while(true)
    {
        server_message_t message{};
        ssize_t ret = recieve_message(&message, server_socket);

        if(ret <= 0)
        {
            std::printf("lost connection to server %zi\n", ret);
            has_connection.store(false, std::memory_order_relaxed);
            pthread_exit(nullptr);
        }

        if(message.header.type == server_message_type::text)
        {
            std::printf("%s\n", reinterpret_cast<char*>(message.data.get()));
        }
    }
}

void signal_handler(int signal, siginfo_t* info, void* user_data)
{
    has_connection.store(false, std::memory_order_relaxed);
}

int32_t main(int32_t argc, const char** argv)
{
    struct sigaction signal_action{};
    signal_action.sa_sigaction = signal_handler;
    sigaction(SIGTERM | SIGINT, &signal_action, nullptr);

    if(argc != 3)
    {
        std::printf("specify the IP and PORT to connect to\n");
        return EXIT_FAILURE;
    }

    const in_addr_t server_ip = inet_addr(argv[1]);
    if(errno != 0)
    {
        perror("error parsing IP");
        return EXIT_FAILURE;
    }

    const uint16_t server_port = std::strtoul(argv[2], nullptr, 10);
    if(errno != 0)
    {
        perror("error parsing PORT");
        return EXIT_FAILURE;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(server_socket == -1)
    {
        perror("failed to create tcp socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_address{
            .sin_family = AF_INET,
            .sin_port = htons(server_port),
            .sin_addr = {server_ip},
    };

    if(connect(server_socket, reinterpret_cast<const sockaddr*>(&server_address), sizeof(server_address)) == -1)
    {
        perror("failed to connect to server");
        return EXIT_FAILURE;
    }

    std::printf("connected to server %s:%s\n", argv[1], argv[2]);

    has_connection.store(true, std::memory_order_relaxed);

    while(true)
    {
        std::string username = prompt_username();
        ssize_t ret = send_message(server_socket, client_message_type_t::login, username.length() + 1, username.c_str());

        if(ret == 0)
        {
            std::printf("lost connection to server\n");
            return EXIT_FAILURE;
        }
        else if(ret == -1)
        {
            perror("error sending login request");
            return EXIT_FAILURE;
        }

        server_message_t server_response{};
        ret = recieve_message(&server_response, server_socket);

        if(ret == 0)
        {
            std::printf("lost connection to server\n");
            return EXIT_FAILURE;
        }
        else if(ret == -1)
        {
            perror("error receiving login response");
            return EXIT_FAILURE;
        }

        const char* response_text = reinterpret_cast<char*>(server_response.data.get());
        if(std::strcmp(response_text, "success") != 0)
        {
            std::printf("login failure: %s\n", response_text);
        }
        else
        {
            std::printf("successfully logged in as \"%s\"\n", username.c_str());
            break;
        }
    }

    pthread_t server_listener_thread{};
    if(pthread_create(&server_listener_thread, nullptr, &::server_listener, nullptr) != 0)
    {
        std::printf("error launching server listener\n");
        return EXIT_FAILURE;
    }

    while(has_connection.load(std::memory_order_relaxed))
    {
        std::string text = prompt_text();
        ssize_t send_text_result = send_message(server_socket, client_message_type_t::text, text.length() + 1, text.c_str());

        if(send_text_result == 0)
        {
            std::printf("lost connection to server\n");
            break;
        }
        else if(send_text_result == -1)
        {
            perror("error sending text");
            break;
        }
    }

    (void)pthread_join(server_listener_thread, nullptr);

    if(close(server_socket) == -1)
    {
        perror("failed to close server socket");
    }

    return EXIT_SUCCESS;
}