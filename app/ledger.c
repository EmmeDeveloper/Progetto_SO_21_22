#include "ledger.h"

int shm_block_id;
int shm_ledger_info_id;
int sem_id;

int max_size;

struct Block *shd_block;
struct LedgerInfo *shd_ledger_info;

/* 
    Inizializza il ledger
    Create
    0: Alloca una shared memory già esistente
    1: Crea la shared memory
 */
int init_ledger(int create, int register_size) {

    max_size = register_size;

    shm_block_id = shmget(LEDGER_SHM_BLOCK_KEY, 
                            sizeof(struct Block) * register_size,
                            create == 1 ? IPC_CREAT | 0666 : 0666);
    if (shm_block_id == -1) {
        printf("Error during get ledger shared memory: %s\n",strerror(errno));
        return -1;
    }
    shd_block = (struct Block *)shmat(shm_block_id, NULL, 0);
    
    shm_ledger_info_id = shmget(LEDGER_SHM_IDBLOCK_KEY,  sizeof(struct LedgerInfo),
                            create == 1 ? IPC_CREAT | 0666 : 0666);
    if (shm_ledger_info_id == -1) {
        printf("Error during get ledger shared memory: %s\n",strerror(errno));
        return -1;
    }
    shd_ledger_info = (struct LedgerInfo *) shmat(shm_ledger_info_id, NULL, 0);
    
    sem_id = semget(LEDGER_SEM_KEY, 1, create == 1 ? IPC_CREAT | 0666 : 0666);
    if (sem_id < 0) {
    	printf("Error during init ledger semaphore: %s\n",strerror(errno));
        return -1;
	} else {
        if (create == 1) {
            if(semctl(sem_id, 0, SETVAL, 1) == -1) {
                printf("Error during set initial value semaphores: %s\n",strerror(errno));
                return -1;
            }   
        }
    };

    return 0;
}

/* Prenota un nuovo id  */
int get_new_block_id() {
    
    int _block_id;
    struct sembuf sop_p;
    struct sembuf sop_v; 
    
    /* Riservo il semaforo */
    SEMBUF_P(sop_p,SEM_USER_ID, 0);

    /* Asetta di poter scrivere, solo un processo alla volta può prenotare un id */
    if (semop(sem_id, &sop_p, 1) == -1) {
        return -1;
    }

    _block_id = shd_ledger_info->last_block_id;
    shd_ledger_info->last_block_id++;

    /* Rilascio il semaforo */
    SEMBUF_V(sop_v,SEM_USER_ID, 0);
    if (semop(sem_id, &sop_v, 1) == -1) {
        return -1;
    }
    return _block_id;
}

/* Scrive il blocco sul ledger, notificando in caso di limite raggiunto */
int add_block(struct VirtualBlock block, int * reached_max_size) {
    struct Block _block;
    int i;
    *reached_max_size = 0;

    /* Si sta tentando di scrivere un blocco fuori dalla capienza massima */
    if (block.id > max_size -1) {
        return -1;
    };

    _block.id = block.id;

    /* Scrive tutte le transazioni sul blocco */
    for (i = 0; i < SO_BLOCK_SIZE; i++) {
        _block.Transaction[i].timestamp_sec=block.Transaction[i]->timestamp_sec;
        _block.Transaction[i].timestamp_nsec=block.Transaction[i]->timestamp_nsec;
        _block.Transaction[i].sender=block.Transaction[i]->sender;
        _block.Transaction[i].receiver=block.Transaction[i]->receiver;
        _block.Transaction[i].quantity=block.Transaction[i]->quantity;
        _block.Transaction[i].reward=block.Transaction[i]->reward;
    };
    shd_block[block.id] = _block;
    shd_ledger_info->block_count++;

    /* Se dopo aver scritto il blocco ho raggiunto la dimensione massima, setto {reached_max_size} a 1 per notificare il chiamante */
    if (shd_ledger_info->block_count == max_size) {
        *reached_max_size = 1;
        return 0;
    }
    
    return 0;
}

/* 
    Libera le risorse del ledger
    destroy
    0: Disalloca le memorie
    1: Rimuove le memorie dopo averle disallocate e cancella il semaforo
*/
int dispose_ledger(int destroy) {
    int succ = 0;

    succ += shmdt((void *) shd_block);
    succ += shmdt((void *) shd_ledger_info);

    if (destroy == 1) {
        succ += shmctl(shm_block_id, IPC_RMID, NULL);
        succ += shmctl(shm_ledger_info_id, IPC_RMID, NULL);
        succ += semctl(sem_id, 1, IPC_RMID);
    }

    return succ < 0 ? -1 : 0;
}

/* Ottiene le entrate di un processo */
int get_process_revenue(pid_t pid) {
    int i, j;
    int sum = 0;
    for (i = 0; i < shd_ledger_info->block_count; i++) {
        for (j = 0; j < SO_BLOCK_SIZE; j++)  {
            if (shd_block[i].Transaction[j].receiver == pid) {
                sum += shd_block[i].Transaction[j].quantity;
            }
        }
    };
    return sum;
}

void print_block(struct Block block){
    int off=0;
    int s = 0;
    printf("*** BLOCCO ***\nid:%i\n",block.id);
    while(off<SO_BLOCK_SIZE){
        s += block.Transaction[off].reward + block.Transaction[off].quantity;
        print_transazione(block.Transaction[off]);
        off++;
    }
    printf("Totale soldi blocco: %i\n", s);
    printf("***End BLOCCO***\n\n");
}

void print_all_blocks() {

    int i;
    for (i = 0; i < shd_ledger_info->block_count; i++) {
        print_block(shd_block[i]);
    };
    
}