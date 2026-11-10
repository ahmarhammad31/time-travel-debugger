#pragma once

#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files.

// resolve.bin bookkeeping
struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    std::string targetFuncName;
};

const uint8_t STATUS_OK = 0;
const uint8_t STATUS_ERROR = 1;

struct Crc32
{
    uint32_t state = 0xFFFFFFFF;

    void add(const char *data, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            state ^= (unsigned char)data[i];
            for (int bit = 0; bit < 8; bit++)
            {
                if (state & 1)
                    state = (state >> 1) ^ 0xEDB88320;
                else
                    state = state >> 1;
            }
        }
    }

    uint32_t result() const
    {
        return ~state;
    }
};

inline void crcToBytes(uint32_t crc, char out[4])
{
    out[0] = (char)(crc >> 24);
    out[1] = (char)(crc >> 16);
    out[2] = (char)(crc >> 8);
    out[3] = (char)crc;
}

class TrailerChecker
{
    Crc32 crc;
    char last4[4];
    size_t held = 0;
    bool sawBody = false;

public:
    size_t feed(const char *in, size_t n, char *out)
    {
        memcpy(out, last4, held);
        memcpy(out + held, in, n);
        size_t total = held + n;

        if (total <= 4)
        {
            memcpy(last4, out, total);
            held = total;
            return 0;
        }

        size_t body = total - 4;
        memcpy(last4, out + body, 4);
        held = 4;
        crc.add(out, body);
        sawBody = true;
        return body;
    }

    bool verify() const
    {
        if (!sawBody || held != 4)
            return false;
        char expected[4];
        crcToBytes(crc.result(), expected);
        return memcmp(expected, last4, 4) == 0;
    }

    const char *trailer() const
    {
        return last4;
    }
};

inline bool sendAll(int sock, const char *data, size_t n)
{
    while (n > 0)
    {
        ssize_t sent = send(sock, data, n, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return false;
        data += sent;
        n -= (size_t)sent;
    }
    return true;
}

inline bool sendError(int sock, const std::string &msg)
{
    char status = (char)STATUS_ERROR;
    Crc32 crc;
    crc.add(&status, 1);
    crc.add(msg.data(), msg.size());
    char trailer[4];
    crcToBytes(crc.result(), trailer);

    return sendAll(sock, &status, 1) && sendAll(sock, msg.data(), msg.size()) && sendAll(sock, trailer, 4);
}

inline bool sendFramedFile(int sock, uint8_t status, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    char statusByte = (char)status;
    Crc32 crc;
    crc.add(&statusByte, 1);
    bool ok = sendAll(sock, &statusByte, 1);

    const int BUFFER_SIZE = 64 * 1024;
    char buffer[BUFFER_SIZE];
    while (ok)
    {
        size_t n = fread(buffer, 1, sizeof(buffer), f);
        if (n == 0)
            break;
        crc.add(buffer, n);
        ok = sendAll(sock, buffer, n);
    }
    fclose(f);

    if (!ok)
        return false;
    char trailer[4];
    crcToBytes(crc.result(), trailer);
    return sendAll(sock, trailer, 4);
}