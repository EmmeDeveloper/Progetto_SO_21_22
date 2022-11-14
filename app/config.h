#ifndef CONFIG_H  /* header guard */
#define CONFIG_H

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <math.h>

#include "common.h"

#define CONFIG_STRUCT_SHARD_KEY 5600
#define RESET_SEED srand(getpid());
#define check_args(num) if(argc<num){printf("Not enough arguments\n");return -1;}
extern int CONFIG_SHM_ID;

struct Options{
    int SO_USERS_NUM;
    int SO_NODES_NUM;
    int SO_BUDGET_INIT; /* User */
    int SO_REWARD; /* User */
    long int SO_MIN_TRANS_GEN_NSEC; /* User */
    long int SO_MAX_TRANS_GEN_NSEC; /* User */
    int SO_RETRY; /* User */
    int SO_TP_SIZE; /* Node*/
    long int SO_MIN_TRANS_PROC_NSEC; /* Node*/
    long int SO_MAX_TRANS_PROC_NSEC; /* Node*/
    int SO_SIM_SEC; /* Main */
    int SO_FRIENDS_NUM; /* Node*/
    int SO_HOPS; /* Node*/
};

unsigned long _hasher(char *str);
struct Options *init_config(char* file);
void print_config(struct Options *config);
#endif