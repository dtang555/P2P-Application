/* index.c - UDP Index Server */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define REGISTER 'R'
#define DOWNLOAD 'D'
#define SEARCH 'S'
#define DEREGISTER 'T'
#define ONLINE 'O'
#define ACKNOWLEDGEMENT 'A'
#define ERROR 'E'
#define QUIT 'Q'

#define MAX_ENTRIES 200
#define NAME_LEN 10

struct register_pdu {
    char type;
    char peerName[NAME_LEN];
    char contentName[NAME_LEN];
    struct sockaddr_in addr;
    char padding[16];
};

struct simple_pdu {
    char type;
    char data[100];
};

typedef struct {
    char peerName[NAME_LEN];
    char contentName[NAME_LEN];
    struct sockaddr_in addr;
    int used_count;
    int active;
} content_entry;

content_entry table[MAX_ENTRIES];
int entry_count = 0;

void send_simple(int sock, struct sockaddr_in *to, socklen_t tolen, char type, const char *msg) {
    struct simple_pdu sp;
    memset(&sp,0,sizeof(sp));
    sp.type = type;
    if (msg) strncpy(sp.data, msg, sizeof(sp.data)-1);
    if (sendto(sock, &sp, sizeof(sp), 0, (struct sockaddr*)to, tolen) < 0)
        perror("sendto");
}

void send_register_pdu(int sock, struct sockaddr_in *to, socklen_t tolen, struct register_pdu *rpdu) {
    if (sendto(sock, rpdu, sizeof(*rpdu), 0, (struct sockaddr*)to, tolen) < 0)
        perror("sendto");
}

int find_least_used(const char *content) {
    int best = -1;
    int best_count = 0x7fffffff;
    for (int i = 0; i < entry_count; ++i) {
        if (!table[i].active) continue;
        if (strncmp(table[i].contentName, content, NAME_LEN) == 0) {
            if (table[i].used_count < best_count) {
                best_count = table[i].used_count;
                best = i;
            }
        }
    }
    return best;
}

int find_exact(const char *peerName, const char *contentName) {
    for (int i = 0; i < entry_count; ++i) {
        if (!table[i].active) continue;
        if (strncmp(table[i].peerName, peerName, NAME_LEN) == 0 &&
            strncmp(table[i].contentName, contentName, NAME_LEN) == 0)
            return i;
    }
    return -1;
}

void handle_register(int sock, struct register_pdu *rpdu, struct sockaddr_in *from, socklen_t fromlen) {
    if (find_exact(rpdu->peerName, rpdu->contentName) != -1) {
        send_simple(sock, from, fromlen, ERROR, "Duplicate registration");
        return;
    }
    if (entry_count >= MAX_ENTRIES) {
        send_simple(sock, from, fromlen, ERROR, "Server storage full");
        return;
    }

    strncpy(table[entry_count].peerName, rpdu->peerName, NAME_LEN-1);
    strncpy(table[entry_count].contentName, rpdu->contentName, NAME_LEN-1);
    table[entry_count].addr = rpdu->addr;
    table[entry_count].used_count = 0;
    table[entry_count].active = 1;
    entry_count++;

    send_simple(sock, from, fromlen, ACKNOWLEDGEMENT, "Registered");
    printf("REGISTER: %s -> %s (port %d)\n", rpdu->peerName, rpdu->contentName, ntohs(rpdu->addr.sin_port));
}

void handle_online(int sock, struct sockaddr_in *from, socklen_t fromlen) {
    char buf[1024] = {0};
    for (int i = 0; i < entry_count; ++i) {
        if (!table[i].active) continue;
        strncat(buf, table[i].contentName, sizeof(buf)-strlen(buf)-2);
        strncat(buf, " (by ", sizeof(buf)-strlen(buf)-2);
        strncat(buf, table[i].peerName, sizeof(buf)-strlen(buf)-2);
        strncat(buf, ")\n", sizeof(buf)-strlen(buf)-2);
    }
    send_simple(sock, from, fromlen, ONLINE, strlen(buf) ? buf : "No content registered");
}

void handle_search(int sock, struct register_pdu *rpdu, struct sockaddr_in *from, socklen_t fromlen) {
    int idx = find_least_used(rpdu->contentName);
    struct register_pdu resp;
    memset(&resp,0,sizeof(resp));

    if (idx == -1) {
        send_simple(sock, from, fromlen, ERROR, "Content not found");
        printf("SEARCH: not found %s\n", rpdu->contentName);
    } else {
        resp.type = SEARCH;
        strncpy(resp.peerName, table[idx].peerName, NAME_LEN-1);
        strncpy(resp.contentName, table[idx].contentName, NAME_LEN-1);
        resp.addr = table[idx].addr;

        send_register_pdu(sock, from, fromlen, &resp);

        table[idx].used_count++;
        printf("SEARCH: %s -> %s:%d (%s), used=%d\n",
               rpdu->contentName,
               inet_ntoa(resp.addr.sin_addr),
               ntohs(resp.addr.sin_port),
               resp.peerName,
               table[idx].used_count);
    }
}

void handle_deregister(int sock, struct register_pdu *rpdu, struct sockaddr_in *from, socklen_t fromlen) {
    int idx = find_exact(rpdu->peerName, rpdu->contentName);
    if (idx == -1) {
        send_simple(sock, from, fromlen, ERROR, "No such registration");
        return;
    }
    table[idx].active = 0;
    send_simple(sock, from, fromlen, ACKNOWLEDGEMENT, "Deregistered");
    printf("DEREGISTER: %s -> %s\n", rpdu->peerName, rpdu->contentName);
}

void handle_quit(int sock, struct register_pdu *rpdu, struct sockaddr_in *from, socklen_t fromlen) {
    int removed = 0;
    for (int i = 0; i < entry_count; ++i) {
        if (table[i].active && strncmp(table[i].peerName, rpdu->peerName, NAME_LEN) == 0) {
            table[i].active = 0;
            removed++;
        }
    }
    send_simple(sock, from, fromlen, ACKNOWLEDGEMENT, "Quit");
    printf("QUIT: %s removed %d entries\n", rpdu->peerName, removed);
}

int main(int argc, char *argv[]) {
    int port = (argc == 2) ? atoi(argv[1]) : 3000;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    struct sockaddr_in sin, from;
    socklen_t fromlen = sizeof(from);

    memset(&sin,0,sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&sin, sizeof(sin)) < 0) { perror("bind"); exit(1); }

    printf("Index server listening on port %d\n", port);

    while (1) {
        struct register_pdu rpdu;
        ssize_t n = recvfrom(sock, &rpdu, sizeof(rpdu), 0, (struct sockaddr*)&from, &fromlen);
        if (n < 1) continue;

        switch (rpdu.type) {
            case REGISTER: handle_register(sock, &rpdu, &from, fromlen); break;
            case ONLINE: handle_online(sock, &from, fromlen); break;
            case SEARCH: handle_search(sock, &rpdu, &from, fromlen); break;
            case DEREGISTER: handle_deregister(sock, &rpdu, &from, fromlen); break;
            case QUIT: handle_quit(sock, &rpdu, &from, fromlen); break;
            default: send_simple(sock, &from, fromlen, ERROR, "Unknown request");
        }
    }

    close(sock);
    return 0;
}
