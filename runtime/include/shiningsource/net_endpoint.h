#ifndef SHININGSOURCE_NET_ENDPOINT_H
#define SHININGSOURCE_NET_ENDPOINT_H

#include <stddef.h>

enum {
    SS_NET_DEFAULT_PORT = 47900,
    SS_NET_HOST_CAPACITY = 128
};

static inline int ss_net_endpoint_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/* Parse an IPv4 relay hostname or address, with an optional decimal port. */
static inline int ss_net_parse_endpoint(const char *text, char *host, size_t host_capacity, int *port)
{
    const char *end, *suffix;
    size_t length;
    unsigned value = SS_NET_DEFAULT_PORT;
    if (!text)
        return 0;
    while (ss_net_endpoint_space(*text))
        ++text;
    end = text;
    while (*end)
        ++end;
    while (end > text && ss_net_endpoint_space(end[-1]))
        --end;
    for (suffix = text; suffix < end && *suffix != ':'; ++suffix) {
        char ch = *suffix;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
            return 0;
    }
    length = (size_t)(suffix - text);
    if (!length || length >= host_capacity)
        return 0;
    if (suffix < end) {
        value = 0;
        for (++suffix; suffix < end; ++suffix) {
            if (*suffix < '0' || *suffix > '9')
                return 0;
            value = value * 10u + (unsigned)(*suffix - '0');
            if (value > 65535u)
                return 0;
        }
        if (!value)
            return 0;
    }
    for (size_t i = 0; i < length; ++i)
        host[i] = text[i];
    host[length] = '\0';
    *port = (int)value;
    return 1;
}

#endif
