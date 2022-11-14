#ifndef COMMON_H  /* header guard */
#define COMMON_H

#include <sys/sem.h>
#include <time.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>



#define USER_RESOURCES_SHARD_KEY 7100
#define NODE_RESOURCES_SHARD_KEY 7200

#define QUEUE_RATIO 200

#define MSG_FRIENDS_KEY 5703
#define MSG_PRIORITY_KEY 5704
#define MSG_REJECTED_KEY 5705
#define MSG_KEY 9153
/*
 Queue list
 MSG_FRIENDS_KEY: Coda usata per l'invio degli amici
 MSG_PRIORITY_KEY: Coda usata per lo scambio di messaggi tra nodo e master
 MSG_KEY: Code usate per lo scambio di messaggi tra nodi e user. Il numero di code è dinamico e dipende dal numero di user. 
 - La coda 0 avrà chiave MSG_KEY, le code successive avranno chiave MSG_KEY + x, con x incrementato ogni tot user
*/

#define SEM_PROCESSES_ALIVE_KEY 1750 /* 0: Users, 1: nodes */
#define SEMBUF_P(name,id, undo) name.sem_num = id; name.sem_op = -1; name.sem_flg = undo == 1 ? SEM_UNDO : 0
#define SEMBUF_V(name,id, undo) name.sem_num = id; name.sem_op = +1; name.sem_flg = undo == 1 ? SEM_UNDO : 0
#define SEM_USER_ID 0
#define SEM_NODE_ID 1

#define SEM_SYNC_KEY 8251

#define SO_BLOCK_SIZE 100
#define SO_REGISTRY_SIZE 1000

struct NodeInfo
{
    int p_id;
    int trans_pool_size;
    int budget;
    int status; /* 0 dead, 1 alive */
};

struct UserInfo
{
    int p_id;
    int budget;
    int status; /* 0 dead, 1 alive */
    int last_block_read;
};

typedef struct Transaction
{
    unsigned long int timestamp_sec;
    unsigned long int timestamp_nsec;
    pid_t sender;
    pid_t receiver;
    int quantity;
    int reward;
} Transaction;

struct Block
{
    int id;
    struct Transaction Transaction[SO_BLOCK_SIZE];
};

struct VirtualBlock {
    int id;
    struct Transaction *Transaction[SO_BLOCK_SIZE];
};
struct MsgFriend{
    long type;
    pid_t friend_pid;
};

struct MsgRejected
{
    long type;
    int amount;
};

struct Message{
    long type;
    short hops;
    struct Transaction trans;
};


int nsleep(long time);
void print_transazione(struct Transaction a);
struct Transaction build_transaction(pid_t sender,pid_t receiver,int quantity,int reward);
int get_reward(int quant,float p_rew);
int rand_int_range(int min,int max);
long int rand_long_range(long int min,long int max);
long int ret_time_nsec();

#endif