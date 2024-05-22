# Compiler
CC = gcc

# Options de compilation
CFLAGS = -Wall -Wextra -std=c11 -pthread
LDFLAGS = -lncurses

# Les fichiers sources
SERVER_SRC = server.c
CLIENT_SRC = client.c

# Les fichiers objets
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Les exécutables
SERVER_EXEC = server
CLIENT_EXEC = client

# Règle par défaut
all: $(SERVER_EXEC) $(CLIENT_EXEC)

# Règle pour le serveur
$(SERVER_EXEC): $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Règle pour le client
$(CLIENT_EXEC): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Règle pour les fichiers objets
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Règle de nettoyage
clean:
	rm -f $(SERVER_OBJ) $(CLIENT_OBJ) $(SERVER_EXEC) $(CLIENT_EXEC)

# Règle pour exécuter le serveur
run_server: $(SERVER_EXEC)
	./$(SERVER_EXEC) 12345

# Règle pour exécuter le client
run_client: $(CLIENT_EXEC)
	./$(CLIENT_EXEC) localhost 12345

.PHONY: all clean run_server run_client
