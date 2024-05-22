#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 256

void error(const char *msg) {
    perror(msg);
    exit(0);
}

void send_udp_action(int udp_sockfd, struct sockaddr_in6 *server_addr, int player_id, int seq_num, const char *action) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "%d %d %s", player_id, seq_num, action);
    int n = sendto(udp_sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)server_addr, sizeof(*server_addr));
    if (n < 0) error("ERROR sending UDP action");
}

int sockfd, udp_sockfd, player_id, team, seq_num = 0;
struct sockaddr_in6 server_udp_addr;
pthread_t chat_thread;

// Fonction de thread pour recevoir les messages de chat du serveur
void *receive_chat_messages(void *arg) {
    (void)arg; // Marquer le paramètre comme utilisé pour éviter les avertissements
    char buffer[BUFFER_SIZE];
    while (1) {
        bzero(buffer, BUFFER_SIZE);
        int n = read(sockfd, buffer, BUFFER_SIZE - 1);
        if (n < 0) error("ERROR reading from socket");
        if (strncmp(buffer, "CHAT:", 5) == 0) {
            printf("Message de chat: %s\n", buffer + 5);
        } else if (strncmp(buffer, "DISCONNECT", 10) == 0) {
            printf("Le serveur a terminé la partie. Déconnexion...\n");
            close(sockfd);
            close(udp_sockfd);
            exit(0);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int portno, n;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct ipv6_mreq mreq;

    char buffer[BUFFER_SIZE];
    if (argc < 3) {
        fprintf(stderr, "usage %s hostname port\n", argv[0]);
        exit(0);
    }

    portno = atoi(argv[2]);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    server = gethostbyname(argv[1]);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host\n");
        exit(0);
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);  // Utiliser h_addr_list[0]
    serv_addr.sin_port = htons(portno);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    printf("Entrez votre nom: ");
    bzero(buffer, BUFFER_SIZE);
    fgets(buffer, BUFFER_SIZE - 1, stdin);
    buffer[strcspn(buffer, "\n")] = 0; // Enlever le caractère de nouvelle ligne

    // Envoyer le nom du joueur au serveur
    n = write(sockfd, buffer, strlen(buffer));
    if (n < 0)
        error("ERROR writing player name to socket");

    printf("Entrez le mode de jeu (1 pour 4 joueurs, 2 pour les équipes): ");
    bzero(buffer, BUFFER_SIZE);
    fgets(buffer, BUFFER_SIZE - 1, stdin);

    n = write(sockfd, buffer, strlen(buffer));
    if (n < 0)
        error("ERROR writing to socket");

    bzero(buffer, BUFFER_SIZE);
    n = read(sockfd, buffer, BUFFER_SIZE - 1);
    if (n < 0)
        error("ERROR reading from socket");
    printf("%s\n", buffer);

    // Analyser la réponse du serveur pour les détails multicast
    char multicast_addr[BUFFER_SIZE];
    int multicast_port, udp_port;
    sscanf(buffer, "Player ID: %d\nTeam: %d\nMulticast Address: %s\nMulticast Port: %d\nUDP Port: %d\n",
           &player_id, &team, multicast_addr, &multicast_port, &udp_port);

    // S'abonner à l'adresse multicast
    udp_sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (udp_sockfd < 0)
        error("ERROR opening UDP socket");

    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET6, multicast_addr, &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = 0; // Any interface

    if (setsockopt(udp_sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0)
        error("ERROR joining multicast group");

    // Notifier le serveur que le client est prêt
    strcpy(buffer, "READY");
    n = write(sockfd, buffer, strlen(buffer));
    if (n < 0)
        error("ERROR writing ready signal to socket");

    // Attendre le signal de début de jeu
    while (1) {
        bzero(buffer, BUFFER_SIZE);
        n = read(sockfd, buffer, BUFFER_SIZE - 1);
        if (n < 0)
            error("ERROR reading from socket");
        if (strncmp(buffer, "Game start!", 11) == 0) {
            printf("%s\n", buffer);
            break;
        }
    }

    // Configurer l'adresse du serveur UDP
    memset(&server_udp_addr, 0, sizeof(server_udp_addr));
    server_udp_addr.sin6_family = AF_INET6;
    inet_pton(AF_INET6, multicast_addr, &server_udp_addr.sin6_addr);
    server_udp_addr.sin6_port = htons(udp_port);

    // Démarrer le thread de chat
    if (pthread_create(&chat_thread, NULL, receive_chat_messages, NULL) != 0)
        error("ERROR creating chat thread");

    // Boucle principale du jeu pour envoyer des actions et des messages de chat
    while (1) {
        printf("Entrez une action (N:NORD, S:SUD, E:EST, W:OUEST, B:BOMBE) ou un message de chat (préfixer avec CHAT:): ");
        bzero(buffer, BUFFER_SIZE);
        fgets(buffer, BUFFER_SIZE - 1, stdin);
        buffer[strcspn(buffer, "\n")] = 0; // Enlever le caractère de nouvelle ligne

        if (strncmp(buffer, "CHAT:", 5) == 0) {
            // Envoyer le message de chat au serveur
            n = write(sockfd, buffer, strlen(buffer));
            if (n < 0)
                error("ERROR sending chat message to socket");
        } else {
            // Envoyer une action de jeu
            send_udp_action(udp_sockfd, &server_udp_addr, player_id, ++seq_num, buffer);
        }
    }

    close(sockfd);
    close(udp_sockfd);
    return 0;
}
