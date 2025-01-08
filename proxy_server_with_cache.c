#include"proxy_parse.h"
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<time.h>
#include<sys/types.h>
#include<pthread.h>

#define MAX_CLIENTS 1000;
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




