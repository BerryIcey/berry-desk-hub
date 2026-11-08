#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline int berry_temperature_bar(int temperature) {
    return temperature < 0 ? -1 : temperature > 100 ? 100 : temperature;
}

static inline void berry_grouped_number(char *out, size_t size, uint64_t value) {
    char digits[32],grouped[40];
    snprintf(digits,sizeof(digits),"%llu",(unsigned long long)value);
    size_t count=strlen(digits),position=0;
    for(size_t i=0;i<count;i++) {
        if(i && (count-i)%3==0)grouped[position++]=',';
        grouped[position++]=digits[i];
    }
    grouped[position]='\0';
    if(size){size_t length=position<size-1?position:size-1;memcpy(out,grouped,length);out[length]='\0';}
}
