#include<stdio.h>;
#include"proxy_parse.h";
#include<string.h>;
#include<time.h>;
#include<Mspthrd.h>;


// Port
const int port_number = 8080;
//MAX_CLIENTS_ALLOWED
const int MAX_CLIENTS = 1000;
// global socket id, as we need sockets to connect to the webserver, although we will have multiple sockets
int global_socketId;

// The number of clients will be equal to the number of threads, init threads
pthread_t tid[MAX_CLIENTS];

//
sem_t semaphore;


// Type Definitions
typedef struct cache_element cache_element;

// This will be the list of  cache elements
struct cache_element {
    char* data;
    int len;
    char* url;
    time_t lru_time;
    cache_element* element;
};
// Function definitions used in the Cache
cache_element* find(char* url);
int add_cache_element(char* data, int size, char* url);
void remove_cache_element(char* url);

int main(int argc, char* argv[])
{
    printf("Hellow");
    return 0;
}