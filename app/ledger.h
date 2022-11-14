/*
    ***** LIBRO MASTRO *****
    Interfaccia generica del libro mastro per l'accesso ai dati e alla struttura condivisa da parte di tutti i processi
*/

#ifndef LEDGER_H  /* header guard */
#define LEDGER_H

#define LEDGER_SHM_BLOCK_KEY 8921
#define LEDGER_SHM_IDBLOCK_KEY 8922
#define LEDGER_SEM_KEY 8923

#include "config.h"
#include "common.h"

struct LedgerInfo {
    int last_block_id;
    int block_count;
};

extern int shm_block_id;
extern int shm_ledger_info_id;
extern int sem_id;

extern int max_size;

extern struct Block *shd_block;
extern struct LedgerInfo *shd_ledger_info;

int init_ledger(int create, int register_size);
int get_new_block_id();
int add_block(struct VirtualBlock block, int * reached_max_size);
int dispose_ledger(int destroy);
void print_block(struct Block block);
void print_all_blocks();
int get_process_revenue(pid_t pid);

#endif
