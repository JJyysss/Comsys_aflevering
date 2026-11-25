#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>

#ifdef __APPLE__
#include "./endian.h"
#else
#include <endian.h>
#endif

#include "./peer.h"


// Global variables to be used by both the server and client side of the peer.
Address_t *my_address;

Address_t** network = NULL;
uint32_t peer_count = 0;
pthread_mutex_t network_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

#define BLOCK_PAYLOAD (MAX_MSG_LEN - REPLY_HEADER_LEN)

void network_signature(const hashdata_t signature,
                       const char *salt, hashdata_t result)
{
    char buf[SHA256_HASH_SIZE + SALT_LEN];
    memcpy(buf, signature, SHA256_HASH_SIZE);
    memcpy(buf + SHA256_HASH_SIZE, salt, SALT_LEN);
    get_data_sha(buf, result,
                 SHA256_HASH_SIZE + SALT_LEN,
                 SHA256_HASH_SIZE);
}

/* a help function to check/find a peer if it is already in the network. */
Address_t* check_peer_exist(char *ip, uint32_t port)
{
    for (uint32_t i = 0; i < peer_count; i++) {
        if (strncmp(network[i]->ip, ip, IP_LEN) == 0
                            && network[i]->port == port) {
            return network[i];
        }
    }
    return NULL;
}

int add_peer(Address_t *new_peer)
{
    pthread_mutex_lock(&network_mutex);
    if (check_peer_exist(new_peer->ip, new_peer->port) != NULL) {
        free(new_peer);
        pthread_mutex_unlock(&network_mutex);
        return 0;
    }

    Address_t **tmp = realloc(
        network, sizeof(Address_t*) * (peer_count + 1));
    
        if (tmp == NULL) {
        free(new_peer);
        pthread_mutex_unlock(&network_mutex);
        return -1;
    }

    network = tmp;
    network[peer_count] = new_peer;
    peer_count++;
    printf("Added new peer to network: %s %d\n",
           new_peer->ip, new_peer->port);
    pthread_mutex_unlock(&network_mutex);

    return 0;
}

/* a helper function that can read and parsing the 80 bytes of Reply_Header_t */
int read_reply_header(compsys_helper_state_t *state,
                      ReplyHeader_t *reply_header)
{
    ssize_t read_bytes = compsys_helper_readnb(state, reply_header, sizeof(ReplyHeader_t));

    if (read_bytes != REPLY_HEADER_LEN) {
        return -1;
    }
    
    reply_header->length = ntohl(reply_header->length);
    reply_header->status = ntohl(reply_header->status);
    reply_header->this_block  = ntohl (reply_header->this_block);
    reply_header->block_count = ntohl (reply_header->block_count);

    return 0;
}

/* a help function to read the body from reply and request*/
int read_body (compsys_helper_state_t *state, char *reply_body,
               uint32_t body_length)
{
    ssize_t read_bytes = compsys_helper_readnb(state, reply_body, body_length);

    if (read_bytes != body_length) {
        return -1;
    }

    return 0;
}

/* send reply back to the peer*/
int send_reply(int peer_socket, uint32_t status,
               char *body, uint32_t body_length)
{
    uint32_t block_count = body_length / BLOCK_PAYLOAD;

    if (body_length % BLOCK_PAYLOAD != 0 || body_length == 0) {
        block_count++;
    }
    
    hashdata_t total_hash;
    get_data_sha(body_length ? body : "", total_hash,
                 body_length, SHA256_HASH_SIZE);
    
    for (uint32_t i = 0; i < block_count; i++) {
        uint32_t offset = i * BLOCK_PAYLOAD;
        uint32_t block_length = body_length - offset;

        if (block_length > BLOCK_PAYLOAD) {
            block_length = BLOCK_PAYLOAD;
        }

        char *block = body_length ? body + offset : "";
        ReplyHeader_t reply_header;
        reply_header.length = htonl(block_length);
        reply_header.status = htonl(status);
        reply_header.this_block = htonl(i);
        reply_header.block_count = htonl(block_count);
        hashdata_t block_hash;
        get_data_sha(block, block_hash, block_length, SHA256_HASH_SIZE);
        memcpy(reply_header.block_hash, block_hash, SHA256_HASH_SIZE);
        memcpy(reply_header.total_hash, total_hash, SHA256_HASH_SIZE);
        
        if (compsys_helper_writen(peer_socket, &reply_header,
                REPLY_HEADER_LEN) != REPLY_HEADER_LEN) {
            return -1;
        }

        if (block_length > 0) {
            if (compsys_helper_writen(
                    peer_socket, block, block_length) != block_length) {
                return -1;
            }
        }
    }
    return 0;
}

typedef struct {
    char *data;
    uint32_t length;
} ReceivedBlock;

void free_blocks(ReceivedBlock *blocks, uint32_t block_count)
{
    for (uint32_t i = 0; i < block_count; i++) {
        free(blocks[i].data);
    }

    free(blocks);
}

int receive_reply(compsys_helper_state_t *state, char **reply_body,
                  uint32_t *body_length, uint32_t *status)
{
    ReplyHeader_t reply_header;

    if (read_reply_header(state, &reply_header) < 0) {
        return -1;
    }

    uint32_t block_count = reply_header.block_count;

    if (block_count == 0) {
        return -1;
    }

    *status = reply_header.status;
    hashdata_t total_hash;
    memcpy(total_hash, reply_header.total_hash, SHA256_HASH_SIZE);
    ReceivedBlock *blocks = calloc(block_count, sizeof(ReceivedBlock));

    if (blocks == NULL) {
        return -1;
    }

    *body_length = 0;

    for (uint32_t i = 0; i < block_count; i++) {
        // The first header has already been read.
        if (i > 0) {
            if (read_reply_header(state, &reply_header) < 0) {
                free_blocks(blocks, block_count);
                return -1;
            }
        }

        uint32_t index = reply_header.this_block;
        uint32_t length = reply_header.length;

        // Validate the block header.
        if (index >= block_count || length > BLOCK_PAYLOAD ||
            reply_header.block_count != block_count ||
            reply_header.status != *status ||
            memcmp(total_hash, reply_header.total_hash,
                   SHA256_HASH_SIZE) != 0) {
            free_blocks(blocks, block_count);
            return -1;
        }

        // Reject duplicate blocks and length overflow.
        if (blocks[index].data != NULL || length > UINT32_MAX - *body_length) {
            free_blocks(blocks, block_count);
            return -1;
        }

        // Allocate memory for the block.
        blocks[index].data = malloc(length ? length : 1);
        blocks[index].length = length;

        if (blocks[index].data == NULL) {
            free_blocks(blocks, block_count);
            return -1;
        }

        // Read the block body.
        if (read_body(state, blocks[index].data, length) < 0) {
            free_blocks(blocks, block_count);
            return -1;
        }

        // Verify the block hash.
        hashdata_t block_hash;

        get_data_sha(blocks[index].data, block_hash, length,
                     SHA256_HASH_SIZE);

        if (memcmp(block_hash, reply_header.block_hash,
                   SHA256_HASH_SIZE) != 0) {
            free_blocks(blocks, block_count);
            return -1;
        }

        *body_length += length;
    }

    // Allocate memory for the complete response.
    *reply_body = malloc(*body_length ? *body_length : 1);

    if (*reply_body == NULL) {
        free_blocks(blocks, block_count);
        return -1;
    }

    // Assemble the blocks in numerical order.
    uint32_t offset = 0;

    for (uint32_t i = 0; i < block_count; i++) {
        memcpy(*reply_body + offset,
               blocks[i].data, blocks[i].length);
        offset += blocks[i].length;
    }

    free_blocks(blocks, block_count);

    // Verify the hash of the complete response.
    hashdata_t calculated_hash;

    get_data_sha(*reply_body, calculated_hash,
                 *body_length, SHA256_HASH_SIZE);

    if (memcmp(calculated_hash, total_hash, SHA256_HASH_SIZE) != 0) {
        free(*reply_body);
        *reply_body = NULL;
        return -1;
    }

    return 0;
}

/* A help function to handle the reply body. */
int handle_reply(compsys_helper_state_t *state,
                 int command, const char *filename)
{
    char *reply_body = NULL;
    uint32_t body_length;
    uint32_t status;

    if (receive_reply(state, &reply_body, &body_length, &status) < 0) {
        fprintf(stderr, "Error receiving reply\n");
        return -1;
    }

    if (status != STATUS_OK && !(command == COMMAND_REGISTER &&
          status == STATUS_PEER_EXISTS)) {
        fprintf(stderr, "Error, reply status: %u\n", status);
        free(reply_body);
        return -1;
    }

    if (command == COMMAND_REGISTER) {
        if (body_length % PEER_ADDR_LEN != 0) {
            free(reply_body);
            return -1;
        }

        for (uint32_t i = 0; i < body_length; i += PEER_ADDR_LEN) {
            Address_t *new_peer = malloc(sizeof(Address_t));

            if (new_peer == NULL) {
                free(reply_body);
                return -1;
            }

            memcpy(new_peer, reply_body + i, PEER_ADDR_LEN);
            new_peer->port = ntohl(new_peer->port);

            if (memchr(new_peer->ip, '\0', IP_LEN) == NULL ||
                !is_valid_ip(new_peer->ip) || new_peer->port == 0 ||
                new_peer->port > 65535) {
                free(new_peer);
                free(reply_body);
                return -1;
            }

            printf("Peer is replied: %s %u\n", new_peer->ip, new_peer->port);

            // Update our own network credentials.
            if (strncmp(new_peer->ip, my_address->ip, IP_LEN) == 0 &&
                new_peer->port == my_address->port) {
                pthread_mutex_lock(&network_mutex);
                *network[0] = *new_peer;
                pthread_mutex_unlock(&network_mutex);
                free(new_peer);
            } else {

                if (add_peer(new_peer) < 0) {
                    free(reply_body);
                    return -1;
                }
            }
        }
    }

    else if (command == COMMAND_RETREIVE) {
        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(filename, "wb");
        if (fp == NULL) {
            pthread_mutex_unlock(&file_mutex);
            free(reply_body);
            return -1;
        }

        size_t written = fwrite(reply_body, 1, body_length, fp);
        int result = fclose(fp);
        pthread_mutex_unlock(&file_mutex);

        if (written != body_length || result != 0) {
            fprintf(stderr, "Error writing file\n");
            free(reply_body);
            return -1;
        }

        printf("File retrieved: %s\n", filename);
    }

    free(reply_body);

    return 0;
}

/*
 * Send a request message to another peer on the network. Unless this is 
 * specifically an 'inform' message as described in the assignment handout, a 
 * reply will always be expected.
 */
int send_message(Address_t peer_address, int command,
                 char *request_body, int body_length)
{
    fprintf(stdout, "Connecting to server at %s:%u to run command %d\n",
            peer_address.ip, peer_address.port, command);
    compsys_helper_state_t state;
    char msg_buf[MAX_MSG_LEN];

    // Validate the request body length.
    if (body_length < 0 || body_length > MAX_MSG_LEN - REQUEST_HEADER_LEN) {
        return -1;
    }

    // Set up the connection.
    char port_char[6];

    snprintf(port_char, sizeof(port_char), "%u", peer_address.port);
    int peer_socket = compsys_helper_open_clientfd(peer_address.ip, port_char);

    if (peer_socket < 0) {
        fprintf(stderr, "Error connecting to peer\n");
        return -1;
    }

    compsys_helper_readinitb(&state, peer_socket);

    // Construct the request message.
    RequestHeader_t request_header;

    strncpy(request_header.ip, my_address->ip, IP_LEN);
    memcpy(request_header.signature, my_address->signature, SHA256_HASH_SIZE);
    request_header.port = htonl(my_address->port);
    request_header.command = htonl(command);
    request_header.length = htonl(body_length);

    // Copy the header and body into the message buffer.
    memcpy(msg_buf, &request_header, REQUEST_HEADER_LEN);

    if (body_length > 0) {
        memcpy(msg_buf + REQUEST_HEADER_LEN, request_body, body_length);
    }

    // Send the complete request.
    int message_length = REQUEST_HEADER_LEN + body_length;

    if (compsys_helper_writen(peer_socket, msg_buf,
                              message_length) != message_length) {
        close(peer_socket);
        return -1;
    }

    int result = 0;

    if (command != COMMAND_INFORM) {
        result = handle_reply(&state, command, request_body);
    }

    close(peer_socket);

    return result;
}

/*
 * Function to act as thread for all required client interactions. This thread 
 * will be run concurrently with the server_thread. It will start by requesting
 * the IP and port for another peer to connect to. Once both have been provided
 * the thread will register with that peer and expect a response outlining the
 * complete network. The user will then be prompted to provide a file path to
 * retrieve. This file request will be sent to a random peer on the network.
 * This request/retrieve interaction is then repeated forever.
 */ 
void *client_thread(void *vargp)
{
    (void)vargp;
    char peer_ip[IP_LEN] = {0};
    fprintf(stdout, "Enter peer IP to connect to (0 if first peer): ");

    if (scanf("%15s", peer_ip) != 1) {
        return NULL;
    }

    // Skip registration if this is the first peer.
    if (strcmp(peer_ip, "0") != 0) {
        char peer_port[PORT_STR_LEN];
        fprintf(stdout, "Enter peer port to connect to: ");

        if (scanf("%7s", peer_port) != 1) {
            return NULL;
        }

        if (!is_valid_ip(peer_ip) || !is_valid_port_str(peer_port) ||
            atoi(peer_port) == 0) {
            fprintf(stderr, "Invalid peer address\n");
            return NULL;
        }

        Address_t peer_address = {0};
        memcpy(peer_address.ip, peer_ip, IP_LEN);
        peer_address.port = atoi(peer_port);

        // Send the first registration message.
        if (send_message(peer_address,
                         COMMAND_REGISTER, NULL, 0) < 0) {
            fprintf(stderr, "Registration failed\n");
            return NULL;
        }
    }

    // Repeatedly request files from random peers.
    while (1) {
        char filename[PATH_LEN];
        printf("Enter file to retrieve: ");

        if (scanf("%127s", filename) != 1) {
            return NULL;
        }

        // Only allow relative paths without parent traversal.
        if (filename[0] == '/' || strstr(filename, "..") != NULL) {
            fprintf(stderr, "Invalid file path\n");
            continue;
        }

        pthread_mutex_lock(&network_mutex);

        if (peer_count <= 1) {
            pthread_mutex_unlock(&network_mutex);
            printf("No other peers in the network\n");
            continue;
        }

        uint32_t index = 1 + random() % (peer_count - 1);
        Address_t target_peer = *network[index];
        pthread_mutex_unlock(&network_mutex);
        send_message(target_peer, COMMAND_RETREIVE, filename,
                     strlen(filename));
    }

    return NULL;
}

int authenticate(RequestHeader_t *request_header)
{
    pthread_mutex_lock(&network_mutex);
    Address_t *peer = check_peer_exist(request_header->ip,
                                       request_header->port);

    if (peer == NULL) {
        pthread_mutex_unlock(&network_mutex);
        return STATUS_PEER_MISSING;
    }

    hashdata_t signature;
    network_signature(request_header->signature, peer->salt, signature);
    int match = memcmp(signature, peer->signature, SHA256_HASH_SIZE) == 0;
    pthread_mutex_unlock(&network_mutex);

    if (!match) {
        return STATUS_BAD_PASSWORD;
    }

    return STATUS_OK;
}

void handle_register(int connfd, RequestHeader_t *request_header)
{
    pthread_mutex_lock(&network_mutex);
    uint32_t status = STATUS_OK;
    Address_t *existing_peer = check_peer_exist(request_header->ip,
                                                request_header->port);

    if (existing_peer != NULL) {
        status = STATUS_PEER_EXISTS;
    } else {
        Address_t *new_peer = malloc(sizeof(Address_t));

        if (new_peer == NULL) {
            pthread_mutex_unlock(&network_mutex);
            send_reply(connfd, STATUS_OTHER, "Allocation failed",
                       strlen("Allocation failed"));
            return;
        }

        memcpy(new_peer->ip, request_header->ip, IP_LEN);
        new_peer->port = request_header->port;
        generate_random_salt(new_peer->salt);
        network_signature(request_header->signature, new_peer->salt,
                          new_peer->signature);
        Address_t **tmp = realloc(
            network, sizeof(Address_t*) * (peer_count + 1));

        if (tmp == NULL) {
            free(new_peer);
            pthread_mutex_unlock(&network_mutex);
            send_reply(connfd, STATUS_OTHER, "Allocation failed",
                       strlen("Allocation failed"));
            return;
        }

        network = tmp;
        network[peer_count] = new_peer;
        peer_count++;
    }

    // Construct the complete peer list.
    if (peer_count > UINT32_MAX / PEER_ADDR_LEN) {
        pthread_mutex_unlock(&network_mutex);
        send_reply(connfd, STATUS_OTHER, "Peer list too large",
                   strlen("Peer list too large"));
        return;
    }

    uint32_t peer_list_length = peer_count * PEER_ADDR_LEN;
    char *peer_list = malloc(peer_list_length);

    if (peer_list == NULL) {
        pthread_mutex_unlock(&network_mutex);
        send_reply(connfd, STATUS_OTHER, "Allocation failed",
                   strlen("Allocation failed"));
        return;
    }

    // Copy the network while holding the mutex.
    uint32_t count = peer_count;

    for (uint32_t i = 0; i < count; i++) {
        Address_t network_1 = *network[i];
        network_1.port = htonl(network_1.port);
        memcpy(peer_list + i * PEER_ADDR_LEN, &network_1, PEER_ADDR_LEN);
    }

    pthread_mutex_unlock(&network_mutex);

    // Send the complete network to the registering peer.
    send_reply(connfd, status, peer_list, peer_list_length);

    // Inform existing peers about the newly registered peer.
    if (status == STATUS_OK) {
        Address_t new_join_peer;
        memcpy(&new_join_peer, peer_list + (count - 1) * PEER_ADDR_LEN,
               PEER_ADDR_LEN);

        for (uint32_t i = 1; i < count - 1; i++) {
            Address_t target_peer;
            memcpy(&target_peer, peer_list + i * PEER_ADDR_LEN, PEER_ADDR_LEN);
            target_peer.port = ntohl(target_peer.port);
            send_message(target_peer, COMMAND_INFORM, (char *)&new_join_peer,
                         PEER_ADDR_LEN);
        }
    }

    free(peer_list);
}

void serve_file(int connfd, const char *filename)
{
    pthread_mutex_lock(&file_mutex);
    FILE *fp = fopen(filename, "rb");

    if (fp == NULL) {
        pthread_mutex_unlock(&file_mutex);
        send_reply(connfd, STATUS_BAD_REQUEST, "File not found",
                   strlen("File not found"));
        return;
    }

    // Determine the file size.
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        pthread_mutex_unlock(&file_mutex);
        send_reply(connfd, STATUS_OTHER, "Error reading file",
                   strlen("Error reading file"));
        return;
    }

    long file_size = ftell(fp);

    if (file_size < 0 || (uint64_t)file_size > UINT32_MAX ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        pthread_mutex_unlock(&file_mutex);
        send_reply(connfd, STATUS_OTHER, "Invalid file size",
                   strlen("Invalid file size"));
        return;
    }

    // Allocate memory for the file.
    char *body = malloc(file_size ? (size_t)file_size : 1);

    if (body == NULL) {
        fclose(fp);
        pthread_mutex_unlock(&file_mutex);
        send_reply(connfd, STATUS_OTHER, "Allocation failed",
                   strlen("Allocation failed"));
        return;
    }

    // Read the file into memory.
    size_t read_bytes = fread(body, 1, file_size, fp);

    fclose(fp);
    pthread_mutex_unlock(&file_mutex);

    if (read_bytes != (size_t)file_size) {
        free(body);
        send_reply(connfd, STATUS_OTHER, "Error reading file",
                   strlen("Error reading file"));
        return;
    }

    // Send the file using the existing reply protocol.
    send_reply(connfd, STATUS_OK, body, (uint32_t)file_size);

    free(body);
}

/*
 * Handler for all server requests. This will call the relevent function based 
 * on the parsed command code
 */
void *handle_server_request_thread(void *vargp)
{
    compsys_helper_state_t state;
    int connfd = *((int *)vargp);
    pthread_detach(pthread_self());
    free(vargp);

    // Read the request header.
    compsys_helper_readinitb(&state, connfd);

    RequestHeader_t request_header;
    ssize_t read_bytes = compsys_helper_readnb(
        &state, &request_header, sizeof(RequestHeader_t));

    if (read_bytes != REQUEST_HEADER_LEN) {
        fprintf(stderr, "Error reading request header\n");
        send_reply(connfd, STATUS_MALFORMED, "Invalid request header",
                   strlen("Invalid request header"));
        close(connfd);
        return NULL;
    }

    // Convert integer fields to host byte order.
    request_header.port = ntohl(request_header.port);
    request_header.command = ntohl(request_header.command);
    request_header.length = ntohl(request_header.length);

    // Validate the request header.
    if (request_header.length > MAX_MSG_LEN - REQUEST_HEADER_LEN ||
        memchr(request_header.ip, '\0', IP_LEN) == NULL ||
        !is_valid_ip(request_header.ip) ||
        request_header.port == 0 ||
        request_header.port > 65535) {

        if (request_header.command != COMMAND_INFORM) {
            send_reply(connfd, STATUS_MALFORMED, "Invalid request header",
                       strlen("Invalid request header"));
        }

        close(connfd);
        return NULL;
    }

    printf("Request header: ip: %s\n"
           "port: %u\ncommand: %u\nlength: %u\n",
           request_header.ip, request_header.port,
           request_header.command, request_header.length);

    // Read the request body.
    char request_body[MAX_MSG_LEN];

    if (request_header.length > 0) {
        if (read_body(&state, request_body, request_header.length) < 0) {
            if (request_header.command != COMMAND_INFORM) {
                send_reply(connfd, STATUS_MALFORMED, "Invalid request body",
                           strlen("Invalid request body"));
            }

            close(connfd);
            return NULL;
        }
    }

    // Handle REGISTER.
    if (request_header.command == COMMAND_REGISTER) {
        if (request_header.length != 0) {
            send_reply(connfd, STATUS_MALFORMED, "Invalid registration body",
                       strlen("Invalid registration body"));
        } else {
            handle_register(connfd, &request_header);
        }
    }

    // Handle INFORM.
    else if (request_header.command == COMMAND_INFORM) {
        if (request_header.length == PEER_ADDR_LEN) {
            Address_t *new_peer = malloc(sizeof(Address_t));

            if (new_peer != NULL) {
                memcpy(new_peer, request_body, PEER_ADDR_LEN);
                new_peer->port = ntohl(new_peer->port);

                if (memchr(new_peer->ip, '\0', IP_LEN) != NULL &&
                    is_valid_ip(new_peer->ip) &&
                    new_peer->port > 0 &&
                    new_peer->port <= 65535) {
                    printf("New peer informed: %s:%u\n",
                           new_peer->ip, new_peer->port);
                    add_peer(new_peer);
                } else {
                    free(new_peer);
                }
            }
        }
    }

    // Handle RETRIEVE.
    else if (request_header.command == COMMAND_RETREIVE) {
        int status = authenticate(&request_header);

        if (status != STATUS_OK) {
            send_reply(connfd, status, "Authentication failed",
                       strlen("Authentication failed"));
        } else if (request_header.length == 0 ||
                   request_header.length >= PATH_LEN ||
                   memchr(request_body, '\0',
                          request_header.length) != NULL) {
            send_reply(connfd, STATUS_BAD_REQUEST, "Invalid filename",
                       strlen("Invalid filename"));
        } else {
            // Terminate the received filename.
            request_body[request_header.length] = '\0';

            if (request_body[0] == '/' ||
                strstr(request_body, "..") != NULL) {
                send_reply(connfd, STATUS_BAD_REQUEST, "Invalid file path",
                           strlen("Invalid file path"));
            } else {
                serve_file(connfd, request_body);
            }
        }
    }

    // Handle unknown commands.
    else {
        send_reply(connfd, STATUS_MALFORMED, "Unknown command",
                   strlen("Unknown command"));
    }

    close(connfd);
    return NULL;
}

/*
 * Function to act as basis for running the server thread. This thread will be
 * run concurrently with the client thread, but is infinite in nature.
 */
void *server_thread(void *vargp)
{
    int listenfd = *((int *)vargp);
    printf("Starting to listen on %s:%u\n", my_address->ip, my_address->port);

    // Accept incoming connections continuously.
    while (1) {
        int *connfd = malloc(sizeof(int));

        if (connfd == NULL) {
            fprintf(stderr, "Allocation failed\n");
            continue;
        }

        *connfd = accept(listenfd, NULL, NULL);

        if (*connfd < 0) {
            fprintf(stderr, "Error accepting connection\n");
            free(connfd);
            continue;
        }

        pthread_t td;

        if (pthread_create(&td, NULL, handle_server_request_thread,
                           connfd) != 0) {
            fprintf(stderr, "Error creating thread\n");
            close(*connfd);
            free(connfd);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP> <PORT>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Validate the provided address.
    if (strlen(argv[1]) >= IP_LEN || !is_valid_ip(argv[1]) ||
        !is_valid_port_str(argv[2]) || atoi(argv[2]) == 0) {
        fprintf(stderr, "Invalid peer address\n");
        exit(EXIT_FAILURE);
    }

    my_address = malloc(sizeof(Address_t));

    if (my_address == NULL) {
        exit(EXIT_FAILURE);
    }

    memset(my_address->ip, '\0', IP_LEN);
    memcpy(my_address->ip, argv[1], strlen(argv[1]));
    my_address->port = atoi(argv[2]);

    // Obtain the password.
    char password[PASSWORD_LEN + 1];

    fprintf(stdout, "Create a password to proceed: ");

    if (scanf("%16s", password) != 1) {
        exit(EXIT_FAILURE);
    }

    // Use the hardcoded user salt for testing.
    char user_salt[SALT_LEN + 1] = "0123456789ABCDEF";

    memcpy(my_address->salt, user_salt, SALT_LEN);

    // Calculate the user signature.
    size_t password_length = strlen(password);

    char buf[PASSWORD_LEN + SALT_LEN];
    memcpy(buf, password, password_length);
    memcpy(buf + password_length, user_salt, SALT_LEN);
    get_data_sha(buf, my_address->signature, password_length + SALT_LEN,
                 SHA256_HASH_SIZE);

    // Start our record of the network.
    network = malloc(sizeof(Address_t*));

    if (network == NULL) {
        exit(EXIT_FAILURE);
    }

    network[0] = malloc(sizeof(Address_t));

    if (network[0] == NULL) {
        exit(EXIT_FAILURE);
    }

    *network[0] = *my_address;
    generate_random_salt(network[0]->salt);
    network_signature(my_address->signature, network[0]->salt,
                      network[0]->signature);
    peer_count = 1;

    // Prevent failed socket writes from terminating the peer.
    signal(SIGPIPE, SIG_IGN);

    // Open the listening socket before starting the threads.
    char port_char[6];

    snprintf(port_char, sizeof(port_char), "%u", my_address->port);
    int listenfd = compsys_helper_open_listenfd(port_char);

    if (listenfd < 0) {
        fprintf(stderr, "Error opening listening socket\n");
        exit(EXIT_FAILURE);
    }

    // Set up the client and server threads.
    pthread_t client_thread_id;
    pthread_t server_thread_id;

    if (pthread_create(&server_thread_id, NULL,
                       server_thread, &listenfd) != 0) {
        close(listenfd);
        exit(EXIT_FAILURE);
    }

    if (pthread_create(&client_thread_id, NULL, client_thread, NULL) != 0) {
        exit(EXIT_FAILURE);
    }

    // Wait for the threads to complete.
    pthread_join(client_thread_id, NULL);
    pthread_join(server_thread_id, NULL);

    close(listenfd);
    exit(EXIT_SUCCESS);
}