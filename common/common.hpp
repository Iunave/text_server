#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

enum class client_message_type : uint16_t
{
    login,
    text,
    MAX
};

struct client_message_header_t
{
    client_message_type type;
    uint16_t size;
};

struct client_message_t
{
    client_message_header_t header;
    std::unique_ptr<uint8_t[]> data;
};

enum class server_message_type : uint16_t
{
    login_response,
    MAX
};

struct server_message_header_t
{
    server_message_type type;
    uint16_t size;
};

struct server_message_t
{
    server_message_header_t header;
    std::unique_ptr<uint8_t[]> data;
};

ssize_t recieve_bytes(int32_t from, void* buffer, size_t size, int flags = 0)
{
    uint8_t* current = static_cast<uint8_t*>(buffer);
    uint8_t* end = static_cast<uint8_t*>(buffer) + size;

    while(current < end)
    {
        size_t remaining = std::distance(current, end);
        ssize_t nread = recv(from, current, remaining, flags);

        if(nread == 0 || nread == -1)
        {
            return nread;
        }

        current += nread;
    }

    return 1;
}

ssize_t send_bytes(int32_t to, void* data, size_t size, int flags = 0)
{
    uint8_t* current = static_cast<uint8_t*>(data);
    uint8_t* end = static_cast<uint8_t*>(data) + size;

    while(current < end)
    {
        size_t remaining = std::distance(current, end);
        ssize_t nsent = send(to, current, remaining, flags);

        if(nsent == 0 || nsent == -1)
        {
            return nsent;
        }

        current += nsent;
    }

    return 1;
}