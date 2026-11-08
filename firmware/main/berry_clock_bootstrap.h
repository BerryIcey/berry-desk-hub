#ifndef BERRY_CLOCK_BOOTSTRAP_H
#define BERRY_CLOCK_BOOTSTRAP_H
#include <stdbool.h>
#include <stdint.h>
static inline bool berry_clock_can_bootstrap(int64_t now,int status,int64_t observed){
 return now<1700000000 && status>=200 && status<=599 && observed>=1700000000 && observed<=4102444800LL;
}
#endif
