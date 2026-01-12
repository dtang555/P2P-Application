/* peer.c
   Peer program for P2P Project
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define REGISTER 'R'
#define DOWNLOAD 'D'
#define SEARCH 'S'
#define DEREGISTER 'T'
#define ONLINE 'O'
#define ACKNOWLEDGEMENT 'A'
#define ERROR 'E'
#define CONTENT 'C'
#define QUIT 'Q'

#define MAX_FILES 100
#define NAME_LEN 10
#define CHUNK_SIZE 1024

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

struct content_pdu {
    char type;
    char data[CHUNK_SIZE];
    ssize_t data_len;
};

ssize_t recv_all(int sock, void *buf, size_t len) {
    size_t total = 0;
    char *p = buf;
    while (total < len) {
        ssize_t r = recv(sock, p + total, len - total, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return 0;
        total += r;
    }
    return total;
}

ssize_t write_all(int sock, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = buf;
    while (total < len) {
        ssize_t w = write(sock, p + total, len - total);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        total += w;
    }
    return total;
}

char local_names[MAX_FILES][NAME_LEN];
int listen_fds[MAX_FILES];
int local_count = 0;
char peerName[NAME_LEN] = {0};

int create_passive_socket(struct sockaddr_in *out_addr) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    sin.sin_port = htons(0);

    if (bind(s, (struct sockaddr*)&sin, sizeof(sin)) < 0) { perror("bind"); close(s); return -1; }
    if (listen(s, 5) < 0) { perror("listen"); close(s); return -1; }

    socklen_t len = sizeof(sin);
    if (getsockname(s, (struct sockaddr*)&sin, &len) < 0) { perror("getsockname"); close(s); return -1; }

    *out_addr = sin;
    return s;
}

int send_register_udp(int udpsock, struct sockaddr_in *index_addr, const char *peer, const char *content, struct sockaddr_in *content_addr) {
    struct register_pdu rp = {0};
    rp.type = REGISTER;
    strncpy(rp.peerName, peer, NAME_LEN-1);
    strncpy(rp.contentName, content, NAME_LEN-1);
    rp.addr = *content_addr;

    if (sendto(udpsock, &rp, sizeof(rp), 0, (struct sockaddr*)index_addr, sizeof(*index_addr)) < 0) { perror("sendto"); return -1; }

    struct simple_pdu resp;
    ssize_t n = recvfrom(udpsock, &resp, sizeof(resp), 0, NULL, NULL);
    if (n < 0) { perror("recvfrom"); return -1; }

    if (resp.type == ACKNOWLEDGEMENT) {
        printf("Server ack: %s\n", resp.data);
        return 0;
    } else {
        printf("Server error: %s\n", resp.data);
        return -1;
    }
}

int send_online_udp(int udpsock, struct sockaddr_in *index_addr) {
    struct register_pdu rp = {0};
    rp.type = ONLINE;
    if (sendto(udpsock, &rp, sizeof(rp), 0, (struct sockaddr*)index_addr, sizeof(*index_addr)) < 0) { perror("sendto"); return -1; }

    struct simple_pdu resp;
    ssize_t n = recvfrom(udpsock, &resp, sizeof(resp), 0, NULL, NULL);
    if (n < 0) { perror("recvfrom"); return -1; }

    if (resp.type == ONLINE) printf("Online list:\n%s\n", resp.data);
    else printf("Error: %s\n", resp.data);
    return 0;
}

int send_search_udp(int udpsock, struct sockaddr_in *index_addr, const char *peer, const char *content, struct register_pdu *resp_pdu) {
    struct register_pdu rp = {0};
    rp.type = SEARCH;
    strncpy(rp.peerName, peer, NAME_LEN-1);
    strncpy(rp.contentName, content, NAME_LEN-1);

    if (sendto(udpsock, &rp, sizeof(rp), 0, (struct sockaddr*)index_addr, sizeof(*index_addr)) < 0) { perror("sendto"); return -1; }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(udpsock, &rfds);
    struct timeval tv = {5,0};
    if (select(udpsock+1, &rfds, NULL, NULL, &tv) <= 0) { printf("No response from index server\n"); return -1; }

    ssize_t n = recvfrom(udpsock, resp_pdu, sizeof(*resp_pdu), 0, NULL, NULL);
    if (n < 0) { perror("recvfrom"); return -1; }

    if (resp_pdu->type == SEARCH) return 0;

    struct simple_pdu *sp = (struct simple_pdu*)resp_pdu;
    if (sp->type == ERROR) { printf("Index server: %s\n", sp->data); return 1; }
    return -1;
}

int send_deregister_udp(int udpsock, struct sockaddr_in *index_addr, const char *peer, const char *content) {
    struct register_pdu rp = {0};
    rp.type = DEREGISTER;
    strncpy(rp.peerName, peer, NAME_LEN-1);
    strncpy(rp.contentName, content, NAME_LEN-1);

    if (sendto(udpsock, &rp, sizeof(rp), 0, (struct sockaddr*)index_addr, sizeof(*index_addr)) < 0) { perror("sendto"); return -1; }

    struct simple_pdu resp;
    ssize_t n = recvfrom(udpsock, &resp, sizeof(resp), 0, NULL, NULL);
    if (n < 0) { perror("recvfrom"); return -1; }

    if (resp.type == ACKNOWLEDGEMENT) { printf("Deregistered: %s\n", resp.data); return 0; }
    else { printf("Deregister error: %s\n", resp.data); return -1; }
}

int send_quit_udp(int udpsock, struct sockaddr_in *index_addr, const char *peer) {
    struct register_pdu rp = {0};
    rp.type = QUIT;
    strncpy(rp.peerName, peer, NAME_LEN-1);

    if (sendto(udpsock, &rp, sizeof(rp), 0, (struct sockaddr*)index_addr, sizeof(*index_addr)) < 0) { perror("sendto"); return -1; }

    struct simple_pdu resp;
    ssize_t n = recvfrom(udpsock, &resp, sizeof(resp), 0, NULL, NULL);
    if (n < 0) { perror("recvfrom"); return -1; }

    if (resp.type == ACKNOWLEDGEMENT) printf("Quit acknowledged\n");
    else printf("Quit error: %s\n", resp.data);
    return 0;
}

void handle_client_connection(int connfd) {
    struct register_pdu dp = {0};
    if (recv_all(connfd, &dp, sizeof(dp)) <= 0) { close(connfd); return; }
    if (dp.type != DOWNLOAD) { close(connfd); return; }

    char fname[NAME_LEN];
    strncpy(fname, dp.contentName, NAME_LEN-1);
    fname[NAME_LEN-1] = '\0';
    FILE *f = fopen(fname, "rb");
    if (!f) { close(connfd); return; }

    char buf[CHUNK_SIZE];
    while (1) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r == 0) {
            uint32_t zero = htonl(0);
            char endhdr[5] = {CONTENT};
            memcpy(&endhdr[1], &zero, 4);
            write_all(connfd, endhdr, sizeof(endhdr));
            break;
        }
        uint32_t len_net = htonl(r);
        char hdr[5] = {CONTENT};
        memcpy(&hdr[1], &len_net, 4);
        if (write_all(connfd, hdr, sizeof(hdr)) < 0) break;
        if (write_all(connfd, buf, r) < 0) break;
    }
    fclose(f);
    close(connfd);
}

int download_from_server(struct sockaddr_in *server_addr, const char *contentName) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }
    if (connect(sock, (struct sockaddr*)server_addr, sizeof(*server_addr)) < 0) { perror("connect"); close(sock); return -1; }

    struct register_pdu dreq = {0};
    dreq.type = DOWNLOAD;
    strncpy(dreq.contentName, contentName, NAME_LEN-1);
    if (write_all(sock, &dreq, sizeof(dreq)) != sizeof(dreq)) { perror("write_all"); close(sock); return -1; }

    FILE *f = fopen(contentName, "wb");
    if (!f) { perror("fopen"); close(sock); return -1; }

    char hdr[5], buf[CHUNK_SIZE];
    while (1) {
        if (recv_all(sock, hdr, sizeof(hdr)) <= 0) { fclose(f); close(sock); return -1; }
        if (hdr[0] != CONTENT) { fclose(f); close(sock); return -1; }
        uint32_t len; memcpy(&len, &hdr[1], 4); len = ntohl(len);
        if (len == 0) break;

        size_t remaining = len;
        while (remaining > 0) {
            size_t toread = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;
            ssize_t got = recv_all(sock, buf, toread);
            if (got <= 0) { fclose(f); close(sock); return -1; }
            fwrite(buf, 1, got, f);
            remaining -= got;
        }
    }
    fclose(f);
    close(sock);
    return 0;
}

void printOptions() {
    printf("[1] Content Listing\n[2] Content Registration\n[3] Content Download\n[4] Content De-Registration\n[5] Quit\n");
}

int main(int argc, char **argv) {
    char *host = "localhost";
    int port = 3000;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    struct hostent *phe;
    struct sockaddr_in indexServer = {0};
    indexServer.sin_family = AF_INET;
    indexServer.sin_port = htons(port);
    if ((phe = gethostbyname(host))) memcpy(&indexServer.sin_addr, phe->h_addr, phe->h_length);
    else if ((indexServer.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) { fprintf(stderr,"Can't get host entry\n"); exit(EXIT_FAILURE); }

    int udpsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpsock < 0) { perror("socket"); exit(EXIT_FAILURE); }
    if (connect(udpsock, (struct sockaddr*)&indexServer, sizeof(indexServer)) < 0) { perror("connect"); close(udpsock); exit(EXIT_FAILURE); }

    for (int i = 0; i < MAX_FILES; ++i) { listen_fds[i] = -1; local_names[i][0] = '\0'; }
    local_count = 0;

    printf("Enter your peer name: ");
    if (!fgets(peerName, sizeof(peerName), stdin)) { fprintf(stderr, "No name\n"); exit(1); }
    peerName[strcspn(peerName, "\n")] = '\0';

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        int maxfd = 0;
        for (int i = 0; i < local_count; ++i) if (listen_fds[i] >= 0) { FD_SET(listen_fds[i], &rfds); if (listen_fds[i] > maxfd) maxfd = listen_fds[i]; }

        if (select(maxfd+1, &rfds, NULL, NULL, NULL) < 0) { perror("select"); break; }

        for (int i = 0; i < local_count; ++i) if (listen_fds[i] >= 0 && FD_ISSET(listen_fds[i], &rfds)) {
            struct sockaddr_in cli; socklen_t l = sizeof(cli);
            int c = accept(listen_fds[i], (struct sockaddr*)&cli, &l);
            if (c >= 0) handle_client_connection(c);
        }

        if (FD_ISSET(0, &rfds)) {
            printOptions();
            printf("Enter your option here: ");
            int choice = 0;
            if (scanf("%d", &choice) != 1) { while (getchar() != '\n'); continue; }
            while (getchar() != '\n');

            if (choice == 1) send_online_udp(udpsock, &indexServer);
            else if (choice == 2) {
                char fname[NAME_LEN]; printf("Enter file name to register: ");
                if (!fgets(fname, sizeof(fname), stdin)) continue;
                fname[strcspn(fname, "\n")] = '\0';
                if (strlen(fname) == 0 || local_count >= MAX_FILES || access(fname, F_OK) != 0) continue;

                struct sockaddr_in content_addr;
                int lfd = create_passive_socket(&content_addr);
                if (lfd < 0) continue;

                if (send_register_udp(udpsock, &indexServer, peerName, fname, &content_addr) == 0) {
                    strncpy(local_names[local_count], fname, NAME_LEN-1);
                    listen_fds[local_count++] = lfd;
                    printf("Registered and listening on %s:%d for %s\n", inet_ntoa(content_addr.sin_addr), ntohs(content_addr.sin_port), fname);
                } else close(lfd);
            }
            else if (choice == 3) {
                char cname[NAME_LEN]; printf("Enter file to download: ");
                if (!fgets(cname, sizeof(cname), stdin)) continue;
                cname[strcspn(cname, "\n")] = '\0';
                if (strlen(cname) == 0) continue;

                struct register_pdu resp;
                int found = send_search_udp(udpsock, &indexServer, peerName, cname, &resp);
                if (found != 0) { if (found == 1) printf("Content not found\n"); continue; }

                printf("Connecting to content server %s:%d (peer: %s)\n", inet_ntoa(resp.addr.sin_addr), ntohs(resp.addr.sin_port), resp.peerName);
                if (download_from_server(&resp.addr, cname) == 0) {
                    printf("Downloaded %s successfully\n", cname);

                    struct sockaddr_in content_addr;
                    int lfd = create_passive_socket(&content_addr);
                    if (lfd >= 0 && send_register_udp(udpsock, &indexServer, peerName, cname, &content_addr) == 0) {
                        if (local_count < MAX_FILES) {
                            strncpy(local_names[local_count], cname, NAME_LEN-1);
                            listen_fds[local_count++] = lfd;
                            printf("Auto-registered downloaded content %s\n", cname);
                        } else close(lfd);
                    }
                } else printf("Download failed\n");
            }
            else if (choice == 4) {
                char cname[NAME_LEN]; printf("Enter content to deregister: ");
                if (!fgets(cname, sizeof(cname), stdin)) continue;
                cname[strcspn(cname, "\n")] = '\0';
                if (strlen(cname) == 0) continue;
                if (send_deregister_udp(udpsock, &indexServer, peerName, cname) == 0) {
                    for (int i = 0; i < local_count; ++i) if (strncmp(local_names[i], cname, NAME_LEN) == 0) {
                        if (listen_fds[i] >= 0) close(listen_fds[i]);
                        for (int j = i; j < local_count-1; ++j) { listen_fds[j] = listen_fds[j+1]; strncpy(local_names[j], local_names[j+1], NAME_LEN); }
                        local_count--; break;
                    }
                }
            }
            else if (choice == 5) {
                for (int i = 0; i < local_count; ++i) { send_deregister_udp(udpsock, &indexServer, peerName, local_names[i]); if (listen_fds[i] >= 0) close(listen_fds[i]); }
                local_count = 0;
                send_quit_udp(udpsock, &indexServer, peerName);
                close(udpsock);
                printf("Exiting.\n");
                exit(0);
            }
        }
    }

    close(udpsock);
    return 0;
}
