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

#include "common.hpp"

ssize_t send_message(int32_t to_socket, client_message_type type, uint16_t size, void* data)
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

bool valid_username(const char* str)
{
    return true;
}

char* prompt_username()
{
    std::printf("login username: ");

    char* username = nullptr;
    size_t name_len = 0;
    size_t nread = 0;

    do
    {
        nread = getline(&username, &name_len, stdin);
        username[nread - 1] = '\0';
    }
    while(!valid_username(username));

    return username;
}

bool valid_text(const char* str)
{
    return true;
}

char* prompt_text()
{
    std::printf("send text message: ");

    char* text = nullptr;
    size_t text_len = 0;
    size_t nread = 0;

    do
    {
        nread = getline(&text, &text_len, stdin);
        text[nread - 1] = '\0';
    }
    while(!valid_text(text));

    return text;
}

int32_t main(int32_t argc, const char** argv)
{
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

    int32_t server_socket = socket(AF_INET, SOCK_STREAM, 0);
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

    while(true)
    {
        char* username = prompt_username();
        ssize_t ret = send_message(server_socket, client_message_type::login, strlen(username) + 1, username);
        free(username);

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
            break;
        }
    }

    while(true)
    {
        char* text = prompt_text();
        ssize_t send_text_result = send_message(server_socket, client_message_type::text, strlen(text) + 1, text);
        free(text);

        if(send_text_result == 0)
        {
            std::printf("lost connection to server\n");
            return EXIT_FAILURE;
        }
        else if(send_text_result == -1)
        {
            perror("error sending text");
            return EXIT_FAILURE;
        }
    }

    if(close(server_socket) == -1)
    {
        perror("failed to close server socket");
    }

    return EXIT_SUCCESS;
}