#include "berry_network_list.h"
#include <string.h>
const berry_network_t *berry_network_find(const berry_network_list_t *list,const char *ssid) {
    if (!list || !ssid || list->count>BERRY_NETWORK_LIMIT) return NULL;
    for(size_t i=0;i<list->count;i++)if(strcmp(list->entries[i].ssid,ssid)==0)return &list->entries[i];
    return NULL;
}
bool berry_network_put(berry_network_list_t *list,const char *ssid,const char *password) {
    if(!list || !ssid || !password || list->count>BERRY_NETWORK_LIMIT)return false;
    size_t sn=strlen(ssid),pn=strlen(password);
    if(!sn || sn>32 || (pn && (pn<8 || pn>63)))return false;
    for(size_t i=0;i<sn;i++)if((unsigned char)ssid[i]<32 || ssid[i]==127)return false;
    for(size_t i=0;i<pn;i++)if((unsigned char)password[i]<32 || password[i]==127)return false;
    size_t i=0;for(;i<list->count;i++)if(strcmp(list->entries[i].ssid,ssid)==0)break;
    if(i==list->count && i==BERRY_NETWORK_LIMIT)return false;
    if(i==list->count)list->count++;
    memmove(list->entries[i].ssid,ssid,sn+1);memset(list->entries[i].ssid+sn+1,0,sizeof(list->entries[i].ssid)-sn-1);
    memmove(list->entries[i].password,password,pn+1);memset(list->entries[i].password+pn+1,0,sizeof(list->entries[i].password)-pn-1);
    return true;
}

bool berry_network_remove(berry_network_list_t *list,const char *ssid){
 if(!list || !ssid || list->count>BERRY_NETWORK_LIMIT)return false;
 for(size_t i=0;i<list->count;i++)if(strcmp(list->entries[i].ssid,ssid)==0){
  for(size_t j=i+1;j<list->count;j++)list->entries[j-1]=list->entries[j];
  list->count--;memset(&list->entries[list->count],0,sizeof(list->entries[0]));return true;
 }
 return false;
}

int berry_network_choose(const berry_network_list_t *list,const char *const *visible,size_t count,size_t start) {
    if(!list || !visible || !list->count || list->count>BERRY_NETWORK_LIMIT)return -1;
    for(size_t n=0;n<list->count;n++){
        size_t index=(start+n)%list->count;
        for(size_t i=0;i<count;i++)if(visible[i] && strcmp(list->entries[index].ssid,visible[i])==0)return (int)index;
    }
    return -1;
}
