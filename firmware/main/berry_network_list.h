#pragma once
#include <stdbool.h>
#include <stddef.h>
#define BERRY_NETWORK_LIMIT 8
typedef struct { char ssid[33]; char password[65]; } berry_network_t;
typedef struct { size_t count; berry_network_t entries[BERRY_NETWORK_LIMIT]; } berry_network_list_t;
bool berry_network_remove(berry_network_list_t *list,const char *ssid);
bool berry_network_put(berry_network_list_t *list,const char *ssid,const char *password);
const berry_network_t *berry_network_find(const berry_network_list_t *list,const char *ssid);

/* Search saved networks in round-robin order among scan results. */
int berry_network_choose(const berry_network_list_t *list,const char *const *visible,size_t count,size_t start);
