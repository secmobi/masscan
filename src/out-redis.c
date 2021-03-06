#include "output.h"
#include "masscan.h"
#include "pixie-sockets.h"
#include "logger.h"
#include <ctype.h>

/****************************************************************************
 * Receive a full line from the socket
 ****************************************************************************/
static size_t
recv_line(SOCKET fd, void *buf, size_t buf_size)
{
    size_t count = 0;

    while (count < buf_size) {
        size_t bytes_received;

        bytes_received = recv(fd, (char*)buf+count, 1, 0);
        if (bytes_received == 0) {
            LOG(0, "redis: recv_line() failed\n");
            exit(1);
        }
        count++;
        if (((unsigned char*)buf)[count-1] == '\n')
            break;
    }

    return count;
}

/****************************************************************************
 ****************************************************************************/
static int
parse_state_machine(struct Output *out, const unsigned char *px, size_t length)
{
    unsigned state = out->redis.state;
    unsigned i;
    enum {
        START,
        NUMBER,
        P,
        PO,
        PON,
        PONG,
        PONG_CR,
        PONG_CR_LF
    };

    for (i=0; i<length; i++)
    switch (state) {
    case START:
        switch (px[i]) {
        case '+':
            state = P;
            break;
        case ':':
            state = NUMBER;
            break;
        default:
            LOG(0, "redis: unexpected data: %.*s\n", (int)(length-i), px+i);
            exit(1);
            break;
        }
        break;
    case NUMBER:
        if (isdigit(px[i]) || px[i] == '\r')
            ;
        else if (px[i] == '\n') {
            state = 0;
            if (out->redis.outstanding == 0) {
                LOG(0, "redis: out of sync\n");
                exit(1);
            }
            out->redis.outstanding--;
        } else {
            LOG(0, "redis: unexpected data: %.*s\n", (int)(length-i), px+i);
            exit(1);
        }
        break;
    case P:
    case PO:
    case PON:
    case PONG_CR:
    case PONG_CR_LF:
        if ("PONG+\r\n"[i-P] == px[i]) {
            state++;
            if (px[i] == '\n') {
                out->redis.state = 0;
                return 1;
            }
        } else {
            LOG(0, "redis: unexpected data: %.*s\n", (int)(length-i), px+i);
            exit(1);
        }
    default:
        LOG(0, "redis: unexpected state: %u\n", state);
        exit(1);
    }
    out->redis.state = state;
    return 0;
}

/****************************************************************************
 ****************************************************************************/
static int
clean_response_queue(struct Output *out, SOCKET fd)
{
    fd_set readfds;
    struct timeval tv = {0,0};
    int x;
    int nfds;
    unsigned char buf[1024];
    size_t bytes_read;

    FD_ZERO(&readfds);
#ifdef _MSC_VER
#pragma warning(disable:4127)
#endif
    FD_SET(fd, &readfds);
    nfds = (int)fd;

    x = select(nfds, &readfds, 0, 0, &tv);
    if (x == 0)
        return 1;
    if (x < 0) {
        LOG(0, "redis:select() failed\n");
        exit(1);
    }
    if (x != 1) {
        LOG(0, "redis:select() failed\n");
        exit(1);
    }

    /*
     * Data exists, so parse it
     */
    bytes_read = recv(fd, (char*)buf, sizeof(buf), 0);
    if (bytes_read == 0) {
        LOG(0, "redis:recv() failed\n");
        exit(1);
    }

    return parse_state_machine(out, buf, bytes_read);
}


/****************************************************************************
 ****************************************************************************/
static void
redis_out_open(struct Output *out, FILE *fp)
{
    ptrdiff_t fd = (ptrdiff_t)fp;
    size_t count;
    unsigned char line[1024];

    UNUSEDPARM(out);

    count = send((SOCKET)fd, "PING\r\n", 6, 0);
    if (count != 6) {
        LOG(0, "redis: send(ping) failed\n");
        exit(1);
    }

    count = recv_line((SOCKET)fd, line, sizeof(line));
    if (count != 7 && memcmp(line, "+PONG\r\n", 7) != 0) {
        LOG(0, "redis: unexpected response from redis server: %s\n", line);
        exit(1);
    }
}

/****************************************************************************
 ****************************************************************************/
static void
redis_out_close(struct Output *out, FILE *fp)
{
    ptrdiff_t fd = (ptrdiff_t)fp;
    size_t count;
    unsigned char line[1024];

    UNUSEDPARM(out);

    count = send((SOCKET)fd, "QUIT\r\n", 6, 0);
    if (count != 6) {
        LOG(0, "redis: send(quit) failed\n");
        exit(1);
    }

    count = recv_line((SOCKET)fd, line, sizeof(line));
    if ((count != 5 && memcmp(line, "+OK\r\n", 5) != 0) && (count != 4 && memcmp(line, ":0\r\n", 4) != 0)){
        LOG(0, "redis: unexpected response from redis server: %s\n", line);
        exit(1);
    }
}

/****************************************************************************
 ****************************************************************************/
static void
redis_out_status(struct Output *out, FILE *fp, time_t timestamp,
    int status, unsigned ip, unsigned ip_proto, unsigned port, unsigned reason, unsigned ttl)
{
    ptrdiff_t fd = (ptrdiff_t)fp;
    char line[256];
    int line_length;
    char payload[128];
    int payload_length;
    char reason_buffer[64];
    size_t bytes_sent;

    if (status != PortStatus_Open) {
        return;
    }

    /* payload format:
     * ip:port|proto|ts|ttl|reason
     */
    payload_length = sprintf_s(payload, sizeof(payload), "%u.%u.%u.%u,%u,%s,%d,%u,%s",
        (unsigned char)(ip>>24),
        (unsigned char)(ip>>16),
        (unsigned char)(ip>> 8),
        (unsigned char)(ip>> 0),
        port,
        name_from_ip_proto(ip_proto),
        (int) timestamp,
        ttl,
        reason_string(reason, reason_buffer, sizeof(reason_buffer)));

    /* PUBLISH the payload to topic "masscan" */
    line_length = sprintf_s(line, sizeof(line),
        "*3\r\n"
        "$7\r\nPUBLISH\r\n"
        "$7\r\nmasscan\r\n"
        "$%d\r\n%s\r\n"
        ,
        payload_length, payload);

    bytes_sent = send((SOCKET)fd, line, (int)line_length, 0);
    if (bytes_sent != (size_t)line_length) {
        LOG(0, "redis: error sending data\n");
        return;
    }
    out->redis.outstanding++;

    clean_response_queue(out, (SOCKET)fd);
}

/****************************************************************************
 ****************************************************************************/
static void
redis_out_banner(struct Output *out, FILE *fp, time_t timestamp,
        unsigned ip, unsigned ip_proto, unsigned port,
        enum ApplicationProtocol proto, unsigned ttl,
        const unsigned char *px, unsigned length)
{
    UNUSEDPARM(ttl);
    UNUSEDPARM(timestamp);
    UNUSEDPARM(out);
    UNUSEDPARM(fp);
    UNUSEDPARM(ip);
    UNUSEDPARM(ip_proto);
    UNUSEDPARM(port);
    UNUSEDPARM(proto);
    UNUSEDPARM(px);
    UNUSEDPARM(length);

}


/****************************************************************************
 ****************************************************************************/
const struct OutputType redis_output = {
    "redis",
    0,
    redis_out_open,
    redis_out_close,
    redis_out_status,
    redis_out_banner
};



