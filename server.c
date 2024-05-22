#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>   
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>
#include <time.h>
#include <pthread.h>


// Définir les constantes pour le jeu
#define MAX_CLIENTS 4
#define BUFFER_SIZE 256
#define MULTICAST_ADDR "ff02::1" // Exemple d'adresse multicast IPv6
#define MULTICAST_PORT 12345 // Port pour la communication multicast
#define UDP_PORT 54321 // Port UDP pour recevoir les messages des joueurs
#define FREQ 100 // Fréquence des mises à jour différentielles en millisecondes
#define BOMB_TIMER 3 // Minuteur d'explosion des bombes en secondes

// Définir la structure Player
typedef struct {
    int socket_fd;
    int connected;
    int ready;
    int y_pos;
    int x_pos;
    char symbol[BUFFER_SIZE];
    int team;
    int last_seq_num;
    char name[BUFFER_SIZE]; // Ajouter ce champ pour le nom du joueur
} Player;

// Définir la structure GameSession
typedef struct {
    int game_mode;
    int num_players;
    Player players[MAX_CLIENTS];
} GameSession;

// Définir la structure Bomb
typedef struct {
    int y_pos;
    int x_pos;
    time_t placed_time;
    int active; // Indiquer si la bombe est active
} Bomb;

// Variables globales pour l'état du jeu
Player players[MAX_CLIENTS];
Bomb bombs[MAX_CLIENTS]; // Supposons une bombe par joueur pour simplifier
GameSession game_session;
int grid_height, grid_width;
int num_ready_players = 0;

int udp_sockfd;
struct sockaddr_in6 udp_addr;
pthread_mutex_t lock;

// Fonction de gestion des erreurs
void error(const char *msg) {
    perror(msg);
    endwin(); // S'assurer que le mode ncurses est arrêté
    exit(1);
}

// Configurer la fenêtre du jeu
void setup_window() {
    initscr();
    getmaxyx(stdscr, grid_height, grid_width); // Obtenir les dimensions du terminal
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0); // Masquer le curseur
    start_color();

    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);
    init_pair(4, COLOR_BLUE, COLOR_BLACK);
    init_pair(5, COLOR_WHITE, COLOR_BLACK); // Pour les murs
}

// Initialiser la grille du jeu avec des murs et des espaces vides
void initialize_grid() {
    for (int i = 0; i < grid_height; i++) {
        for (int j = 0; j < grid_width; j++) {
            if ((i % 2 == 0) && (j % 5 == 0))
                mvaddch(i, j, 'X'); // Murs indestructibles
            else if ((i % 3 == 0) && (j % 4 == 0))
                mvaddch(i, j, '#'); // Murs destructibles
            else
                mvaddch(i, j, ' '); // Espace vide
        }
    }
}

// Initialiser les positions des joueurs et les bombes
void initialize_players() {
    players[0] = (Player){-1, 0, 0, 1, 1, "", -1, -1, ""};
    strcpy(players[0].symbol, "P0");
    players[1] = (Player){-1, 0, 0, 1, grid_width - 2, "", -1, -1, ""};
    strcpy(players[1].symbol, "P1");
    players[2] = (Player){-1, 0, 0, grid_height - 2, 1, "", -1, -1, ""};
    strcpy(players[2].symbol, "P2");
    players[3] = (Player){-1, 0, 0, grid_height - 2, grid_width - 2, "", -1, -1, ""};
    strcpy(players[3].symbol, "P3");
    memset(bombs, 0, sizeof(bombs)); // Initialiser les bombes
}

// Dessiner la grille du jeu à l'écran
void draw_grid() {
    for (int i = 0; i < grid_height; i++) {
        for (int j = 0; j < grid_width; j++) {
            if ((i % 2 == 0) && (j % 5 == 0))
                mvaddch(i, j, 'X'); // Murs indestructibles
            else if ((i % 3 == 0) && (j % 4 == 0))
                mvaddch(i, j, '#'); // Murs destructibles
            else
                mvaddch(i, j, ' '); // Espace vide
        }
    }
    refresh();
}

// Dessiner les joueurs sur la grille
void draw_players() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (players[i].connected) {
            attron(COLOR_PAIR(i + 1));
            mvprintw(players[i].y_pos, players[i].x_pos, "%s", players[i].symbol); // Utiliser %s pour les chaînes
            attroff(COLOR_PAIR(i + 1));
        }
    }
    refresh();
}

// Dessiner les bombes sur la grille
void draw_bombs() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (bombs[i].active) {
            mvprintw(bombs[i].y_pos, bombs[i].x_pos, "B");
        }
    }
    refresh();
}

// Initialiser les variables de la session de jeu
void initialize_game_session() {
    game_session.game_mode = 0; // Aucun mode de jeu attribué pour le moment
    game_session.num_players = 0;
}

// Envoyer le message de début de jeu à tous les joueurs connectés
void send_game_start() {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "Game start!\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (players[i].connected) {
            write(players[i].socket_fd, buffer, strlen(buffer));
        }
    }

    // Optionnellement, envoyer la grille de jeu initiale ici
    snprintf(buffer, BUFFER_SIZE, "Initial grid\n");
    for (int i = 0; i < grid_height; i++) {
        for (int j = 0; j < grid_width; j++) {
            buffer[j] = mvinch(i, j);
        }
        buffer[grid_width] = '\n';
        for (int k = 0; k < MAX_CLIENTS; k++) {
            if (players[k].connected) {
                write(players[k].socket_fd, buffer, grid_width + 1);
            }
        }
    }
}

// Diffuser un message de chat à tous les clients connectés
void broadcast_chat_message(int sender_id, const char *message) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, "CHAT: %.50s: %.180s\n", players[sender_id].name, message);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (players[i].connected && players[i].socket_fd != players[sender_id].socket_fd) {
            write(players[i].socket_fd, buffer, strlen(buffer));
        }
    }
}

// Gérer la communication avec un client
void *handle_client(void *arg) {
    int newsockfd = *(int *)arg;
    char buffer[BUFFER_SIZE];
    int player_id = -1;

    // Lire le mode de jeu
    int n = read(newsockfd, buffer, BUFFER_SIZE - 1);
    if (n < 0) error("ERROR reading from socket");
    buffer[n] = '\0';

    int game_mode = atoi(buffer);
    game_session.game_mode = game_mode;

    pthread_mutex_lock(&lock);
    player_id = game_session.num_players;
    players[player_id].socket_fd = newsockfd;
    players[player_id].connected = 1;

    // Lire le nom du joueur
    bzero(buffer, BUFFER_SIZE);
    n = read(newsockfd, buffer, BUFFER_SIZE - 1);
    if (n < 0) error("ERROR reading player name from socket");
    buffer[n] = '\0';
    strncpy(players[player_id].name, buffer, BUFFER_SIZE - 1);

    // Convertir player_id en une chaîne de caractères pour le symbole
    snprintf(players[player_id].symbol, BUFFER_SIZE, "%d", player_id);

    // Assigner les équipes si en mode équipe
    if (game_mode == 2) {
        players[player_id].team = player_id % 2;
    } else {
        players[player_id].team = -1; // Pas d'équipe
    }

    // Ajouter le joueur à la session de jeu
    game_session.players[game_session.num_players] = players[player_id];
    game_session.num_players++;
    pthread_mutex_unlock(&lock);

    // Répondre au client avec les détails du jeu
    char response[BUFFER_SIZE];
    snprintf(response, BUFFER_SIZE, "Player ID: %d\nTeam: %d\nMulticast Address: %s\nMulticast Port: %d\nUDP Port: %d\n",
             player_id, players[player_id].team, MULTICAST_ADDR, MULTICAST_PORT, UDP_PORT);

    n = write(newsockfd, response, strlen(response));
    if (n < 0) error("ERROR writing to socket");

    // Mettre à jour l'affichage des joueurs
    draw_grid(); // S'assurer que la grille est redessinée
    draw_players();

    // Attendre que le client signale qu'il est prêt
    bzero(buffer, BUFFER_SIZE);
    n = read(newsockfd, buffer, BUFFER_SIZE - 1);
    if (n < 0) error("ERROR reading ready signal from socket");

    if (strncmp(buffer, "READY", 5) == 0) {
        players[player_id].ready = 1;
        num_ready_players++;
    }

    // Commencer le jeu si tous les joueurs sont prêts
    if (num_ready_players == MAX_CLIENTS) {
        send_game_start();
    }

    // Gérer les messages des clients en boucle
    while (1) {
        bzero(buffer, BUFFER_SIZE);
        n = read(newsockfd, buffer, BUFFER_SIZE - 1);
        if (n < 0) error("ERROR reading from socket");

        if (strncmp(buffer, "CHAT:", 5) == 0) {
            // Gérer le message de chat
            broadcast_chat_message(player_id, buffer + 5);
        } else {
            // Gérer d'autres actions de jeu (le cas échéant)
        }
    }
    return NULL;
}

// Traiter les requêtes UDP entrantes
void process_udp_requests() {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in6 cli_addr;
    socklen_t cli_len = sizeof(cli_addr);

    while (1) {
        int n = recvfrom(udp_sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&cli_addr, &cli_len);
        if (n < 0) error("ERROR receiving from UDP socket");

        buffer[n] = '\0';

        int player_id, seq_num;
        char action[BUFFER_SIZE];
        sscanf(buffer, "%d %d %s", &player_id, &seq_num, action);

        // Traiter la dernière action basée sur le numéro de séquence
        if (seq_num > players[player_id].last_seq_num) {
            players[player_id].last_seq_num = seq_num;

            // Gérer les mouvements et actions des joueurs
            if (strcmp(action, "N") == 0 && players[player_id].y_pos > 0 && mvinch(players[player_id].y_pos - 1, players[player_id].x_pos) == ' ') {
                players[player_id].y_pos--;
            } else if (strcmp(action, "S") == 0 && players[player_id].y_pos < grid_height - 1 && mvinch(players[player_id].y_pos + 1, players[player_id].x_pos) == ' ') {
                players[player_id].y_pos++;
            } else if (strcmp(action, "E") == 0 && players[player_id].x_pos < grid_width - 1 && mvinch(players[player_id].y_pos, players[player_id].x_pos + 1) == ' ') {
                players[player_id].x_pos++;
            } else if (strcmp(action, "W") == 0 && players[player_id].x_pos > 0 && mvinch(players[player_id].y_pos, players[player_id].x_pos - 1) == ' ') {
                players[player_id].x_pos--;
            } else if (strcmp(action, "B") == 0) {
                // Gérer le placement des bombes
                bombs[player_id].y_pos = players[player_id].y_pos;
                bombs[player_id].x_pos = players[player_id].x_pos;
                bombs[player_id].placed_time = time(NULL);
                bombs[player_id].active = 1;
                mvaddch(players[player_id].y_pos, players[player_id].x_pos, 'B');
            }
            draw_grid();
            draw_players();
            draw_bombs();
        }
    }
}

// Fonction de thread pour l'écouteur UDP
void *udp_listener(void *arg) {
    (void)arg; // Marquer le paramètre comme utilisé pour éviter les avertissements
    process_udp_requests();
    return NULL;
}


// Afficher un grand message de victoire
void display_victory_message(const char *message) {
    clear();
    int len = strlen(message);
    int start_col = (grid_width - len) / 2;
    int start_row = grid_height / 2;

    attron(A_BOLD);
    for (int i = 0; i < len; i++) {
        mvaddch(start_row, start_col + i, message[i]);
    }
    attroff(A_BOLD);
    refresh();
    sleep(5); // Afficher le message pendant 5 secondes
}

// Vérifier si le jeu est terminé
void check_game_over() {
    int team0_alive = 0;
    int team1_alive = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (players[i].connected) {
            if (players[i].team == 0) {
                team0_alive++;
            } else if (players[i].team == 1) {
                team1_alive++;
            }
        }
    }

    if (team0_alive == 0 || team1_alive == 0) {
        // Notifier tous les joueurs de l'état de fin de jeu
        char buffer[BUFFER_SIZE];
        const char *winning_team_message;

        if (team0_alive > 0) {
            snprintf(buffer, BUFFER_SIZE, "Game over! Team 0 wins!\n");
            winning_team_message = "Team 0 wins!";
        } else if (team1_alive > 0) {
            snprintf(buffer, BUFFER_SIZE, "Game over! Team 1 wins!\n");
            winning_team_message = "Team 1 wins!";
        } else {
            snprintf(buffer, BUFFER_SIZE, "Game over! It's a draw!\n");
            winning_team_message = "It's a draw!";
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (players[i].connected) {
                write(players[i].socket_fd, buffer, strlen(buffer));
            }
        }

        display_victory_message(winning_team_message);

        // Envoyer un signal de déconnexion aux clients
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (players[i].connected) {
                write(players[i].socket_fd, "DISCONNECT\n", strlen("DISCONNECT\n"));
                close(players[i].socket_fd);
                players[i].connected = 0;
            }
        }

        endwin();
        exit(0);
    }
}

// Gérer les explosions de bombes et leurs effets
void handle_bombs() {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (bombs[i].active && difftime(now, bombs[i].placed_time) >= BOMB_TIMER) {
            // Gérer l'explosion de la bombe
            int y = bombs[i].y_pos;
            int x = bombs[i].x_pos;
            mvaddch(y, x, ' '); // Effacer la bombe

            // Logique d'explosion (simplifiée)
            for (int j = -2; j <= 2; j++) {
                if (y + j >= 0 && y + j < grid_height && mvinch(y + j, x) != 'X') {
                    // Vérifier les murs destructibles
                    if (mvinch(y + j, x) == '#') {
                        mvaddch(y + j, x, ' ');
                    }
                    // Vérifier les joueurs
                    for (int k = 0; k < MAX_CLIENTS; k++) {
                        if (players[k].y_pos == y + j && players[k].x_pos == x) {
                            players[k].connected = 0; // Marquer le joueur comme éliminé
                        }
                    }
                }
                if (x + j >= 0 && x + j < grid_width && mvinch(y, x + j) != 'X') {
                    // Vérifier les murs destructibles
                    if (mvinch(y, x + j) == '#') {
                        mvaddch(y, x + j, ' ');
                    }
                    // Vérifier les joueurs
                    for (int k = 0; k < MAX_CLIENTS; k++) {
                        if (players[k].y_pos == y && players[k].x_pos == x + j) {
                            players[k].connected = 0; // Marquer le joueur comme éliminé
                        }
                    }
                }
            }

            bombs[i].active = 0; // Réinitialiser la bombe
            draw_grid();
            draw_players();
            draw_bombs();
            check_game_over(); // Vérifier si le jeu est terminé après l'explosion
        }
    }
}

// Fonction de thread pour le minuteur de bombes
void *bomb_timer(void *arg) {
    (void)arg; // Marque le paramètre comme utilisé pour éviter les avertissements
    while (1) {
        handle_bombs();
        usleep(100000); // Vérifier les bombes toutes les 100ms
    }
    return NULL;
}

// Mettre à jour la grille par multicast pour tous les clients
void multicast_grid_update() {
    struct sockaddr_in6 multicast_addr;
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin6_family = AF_INET6;
    multicast_addr.sin6_port = htons(MULTICAST_PORT);
    inet_pton(AF_INET6, MULTICAST_ADDR, &multicast_addr.sin6_addr);

    while (1) {
        char buffer[BUFFER_SIZE];

        // Envoyer la grille complète toutes les secondes
        for (int i = 0; i < grid_height; i++) {
            for (int j = 0; j < grid_width; j++) {
                buffer[j] = mvinch(i, j);
            }
            buffer[grid_width] = '\n';
            sendto(udp_sockfd, buffer, grid_width + 1, 0, (struct sockaddr *)&multicast_addr, sizeof(multicast_addr));
        }

        usleep(1000000 / FREQ);

        // Envoyer des mises à jour différentielles toutes les FREQ ms
        for (int i = 0; i < grid_height; i++) {
            for (int j = 0; j < grid_width; j++) {
                buffer[j] = mvinch(i, j);
            }
            buffer[grid_width] = '\n';
            sendto(udp_sockfd, buffer, grid_width + 1, 0, (struct sockaddr *)&multicast_addr, sizeof(multicast_addr));
        }

        usleep(1000000 / FREQ);
    }
}

// Mettre à jour les différentielles par multicast pour tous les clients
void multicast_differential_update() {
    struct sockaddr_in6 multicast_addr;
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin6_family = AF_INET6;
    multicast_addr.sin6_port = htons(MULTICAST_PORT);
    inet_pton(AF_INET6, MULTICAST_ADDR, &multicast_addr.sin6_addr);

    char buffer[BUFFER_SIZE];
    while (1) {
        usleep(1000000 / FREQ);

        // Envoyer des mises à jour différentielles
        // Pour simplifier, cet exemple envoie la grille entière, mais vous devez implémenter une logique différentielle réelle
        for (int i = 0; i < grid_height; i++) {
            for (int j = 0; j < grid_width; j++) {
                buffer[j] = mvinch(i, j);
            }
            buffer[grid_width] = '\n';
            sendto(udp_sockfd, buffer, grid_width + 1, 0, (struct sockaddr *)&multicast_addr, sizeof(multicast_addr));
        }
    }
}

// Fonction principale
int main(int argc, char *argv[]) {
    int sockfd, newsockfd, portno;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;
    pthread_t udp_thread, bomb_thread, multicast_thread;

    if (argc < 2) {
        fprintf(stderr, "ERROR, no port provided\n");
        exit(1);
    }

    // Créer le socket TCP
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    memset(&serv_addr, 0, sizeof(serv_addr));
    portno = atoi(argv[1]);

    // Configurer la structure de l'adresse du serveur
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    // Lier le socket à l'adresse
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    // Écouter les connexions entrantes
    listen(sockfd, MAX_CLIENTS);
    clilen = sizeof(cli_addr);

    // Configurer la fenêtre du jeu et initialiser l'état du jeu
    setup_window();
    initialize_grid();
    initialize_players();
    initialize_game_session();
    pthread_mutex_init(&lock, NULL);

    // Configurer le socket UDP
    udp_sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (udp_sockfd < 0)
        error("ERROR opening UDP socket");

    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin6_family = AF_INET6;
    udp_addr.sin6_addr = in6addr_any;
    udp_addr.sin6_port = htons(UDP_PORT);

    if (bind(udp_sockfd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0)
        error("ERROR on binding UDP socket");

    // Démarrer le thread écouteur UDP
    if (pthread_create(&udp_thread, NULL, udp_listener, NULL) != 0)
        error("ERROR creating UDP listener thread");

    // Démarrer le thread du minuteur de bombes
    if (pthread_create(&bomb_thread, NULL, bomb_timer, NULL) != 0)
        error("ERROR creating bomb timer thread");

    // Démarrer le thread de mise à jour différentielle multicast
    if (pthread_create(&multicast_thread, NULL, (void *)multicast_differential_update, NULL) != 0)
        error("ERROR creating multicast update thread");

    // Boucle principale du serveur pour accepter les clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0)
            error("ERROR on accept");

        pthread_t client_thread;
        if (pthread_create(&client_thread, NULL, handle_client, &newsockfd) != 0)
            error("ERROR creating client thread");
    }

    getch();
    endwin();

    // Fermer les sockets et nettoyer
    for (int i = 0; i < MAX_CLIENTS; i++) {
        close(players[i].socket_fd);
    }
    close(sockfd);
    close(udp_sockfd);
    return 0;
}
