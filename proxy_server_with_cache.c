#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define MAX_CLIENTS 1000
typedef struct cache_entry cache_entry;

struct cache_entry{
    char* data;
    int len;
    char* url;
    time_t timestamp;
    cache_entry* next;
};

// Cache functions Declarations
cache_entry* find(char* url);
int add_cache_entry(char* url, char* data, int len);
void remove_cache_entry();

int port_number = 8080;
int proxy_socket_id;

pthread_t tid[MAX_CLIENTS];

sem_t semaphore;

pthread_mutex_t lock;

cache_entry* head;

int cache_size = 0;


int main(int argc, char *argv[]) {

    int client_socket_id, client_length;
    struct sockaddr_in server_addr, client_addr;
    sem_init(&semaphore, 0, MAX_CLIENTS);

    pthread_mutex_init(&lock, NULL);

    if(argv == 2){
        port_number = atoi(argv[1]);
    }else{
        printf("Too few arguements\n");
    }

    printf("Proxy server started on port %d\n", port_number);

    proxy_socket_id = socket(AF_INET, SOCK_STREAM, 0);

    if(proxy_socket_id < 0){
        printf("Error creating socket\n");
        return 1;
    }

    int reuse = 1;

    if(setsockopt(proxy_socket_id, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0){
        printf("Error setting socket options\n");
        return 1;
    }

    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(proxy_socket_id, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
        printf("Error binding socket\n");
        return 1;
    }

    printf("Binding on the post %d\n", port_number);

    int listent_status = listen(proxy_socket_id, MAX_CLIENTS);

    if(listent_status<0){
        printf("Error in listening");
        exit(1);
    }

    int i = 0;
    int Connceted_Socket_Id[MAX_CLIENTS];


    while(1){
        bzero((char *)&client_addr, sizeof(client_addr));
        client_length = sizeof(client_addr);

        client_socket_id = accept(proxy_socket_id, (struct sockaddr_in*)&client_addr, (socklen_t*)&client_length);

        if(client_socket_id<0){
            printf("Unable to connect");
            exit(1);
        }else{
            Connceted_Socket_Id[i] = client_socket_id;
        }

        struct sockaddr_in *client_pt = (struct sockeradddr_in *)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip_addr, str, INET6_ADDRSTRLEN);


    }


    return 0;
}




