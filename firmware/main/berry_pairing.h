#pragma once
#include <stdbool.h>
#include <stddef.h>
void berry_pairing_request(void);
void berry_pairing_cancel(void);
bool berry_pairing_view(char *name,size_t name_size,char *proof,size_t proof_size);
