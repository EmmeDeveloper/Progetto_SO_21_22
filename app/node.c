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

#include "config.h"
#include "common.h"
#include "ledger.h"

#define REWARD_SENDER_PID -1

struct tp_block{
    int is_used;
    struct Transaction trans;
};

/**********************
*      FUNCTIONS      *
**********************/
void init_process(int first);
void clear();
int msg_pool_deposit(struct Message *mex);
int find_tp_trans(int off);
int create_block(int block_id, struct VirtualBlock * block);
void clear_block();
int print_tp();
int print_tp_budget();
void increment_tp();
void initiate_friend_list();
void print_friends();
void get_friends();
int send_friend_trans(struct Message mex, int * reached_hops);
int send_message_to_master(struct Message mex);
void enter_shd_safe_mode();    
void exit_shd_safe_mode();
int send_rejected_trans(struct Message mex);
int get_queue_id_less_used();

/**********************
*     GLOBAL VARS     *
**********************/

/* Shared memory */
struct Options *conf;
struct UserInfo * shd_user_info;
struct NodeInfo * shd_node_info;
int shdOffset;

/* Queues */
int * msg_queue_ids;
int msg_id_friends;
int msq_id_priority;
int msg_queue_rejected_id = 0;

/* Semaphores */
int sem_proc_alive_id;
int sem_sync_id = 0;

/* Friends */
pid_t *friends;
int friends_count = 0;
int friends_max = 0;

struct tp_block *transaction_pool;
Transaction reward_transaction;
int alive = 1;
int clear_pos[SO_BLOCK_SIZE-1];
int idx_last_tp_used = -1;


/**********************
*      LIFECYCLE      *
**********************/

void sig_handler(int signum){
    switch (signum) {
        case SIGTERM:;
            alive = 0;
            break;
         
        case SIGINT:;
            /* Placeholder per il ctrl c dal parent */
            break;

        default:;
            break;
    }
}

int main(int argc,char** argv){

    /* Dichiarazioni variabili */
    int conf_id;
    int u_shd_id;
    int n_shd_id;
    int first;
    struct Message mex;
    int hops_reach;
    int received;
    int queue_count;
    int queue_id;
    int queue_read;
    int off;
    struct VirtualBlock blocco;
    int reached_max_size;
    struct sembuf sop_w;
    struct sembuf sop_p;
    struct sembuf sop_w2;

    /*Debug*/
    /* printf("***** Start node:%i,%i\n",getpid(),getppid()); */
    
    check_args(6)
    /* 
        ARGS
        0: type
        1: config id
        2: id users shdm
        3: id nodes shdm
        4: offset
        5: first
    */
    conf_id = atoi(argv[1]);
    u_shd_id = atoi(argv[2]);
    n_shd_id = atoi(argv[3]);
    shdOffset = atoi(argv[4]);
    first = atoi(argv[5]); 
    
    /* I primi processi a essere creati aspettano la sincronizzazione che siano tutti pronti */
    
    /*Debug*/
    /* printf("Node %d cid %d uid %d nid %d qid %d qrid %d off %d f %d \n", 
        getpid(), conf_id, u_shd_id, n_shd_id, msq_id, msq_id_rej, shdOffset, first); */

    /* Read configuration */
    conf = (struct Options *) shmat(conf_id, NULL, 0);

    shd_user_info = (struct UserInfo *)shmat(u_shd_id, NULL, 0);
    shd_node_info = (struct NodeInfo *)shmat(n_shd_id, NULL, 0);
    init_process(first);

    /*Debug*/
    /* printf("[NODE: %d] Inizializzato il nodo \n", getpid()); */

    /*Inizzializzazione transaction pool*/
    transaction_pool=malloc(sizeof(struct tp_block)*conf->SO_TP_SIZE);
    
    /* Aspetta che tutti gli utenti siano inizializzati */
    if (first == 1) {
        /*Debug*/
        /* printf("Node %d Sem sync processi pronti: %d \n", getpid(), semctl(sem_sync_id, 0, GETVAL, 0)); */
        sop_w.sem_flg = 0, sop_w.sem_num = 0, sop_w.sem_op = 0;
        if (alive == 0 || semop(sem_sync_id, &sop_w, 1) == -1) {
            /*Debug*/
            /* printf("[NODE: %i] Decremento semaforo fallito:%s\n",getpid(), strerror(errno));
            printf("[NODE: %i] Exit\n",getpid()); */
            exit(1);
        }
    }


    /* Ottiene la lista degli amici iniziale */
    initiate_friend_list();

    /*Debug*/
    /*print_friends();*/

    /* 
        First:
        1: Nodo creato alla prima iterazione del main
        0: Nodo creato successivamente in risposta a una transazione
    */
    if (first == 1) {
        SEMBUF_P(sop_p, 1, 0);
        semop(sem_sync_id, &sop_p, 1);

        /* Aspetta che tutti i nodi abbiano ricevuto gli amici */
        /*Debug*/
        /* printf("Node %d Sem sync nodi pronti dopo lettura amici: %d \n", getpid(), semctl(sem_sync_id, 1, GETVAL, 0)); */
        sop_w2.sem_flg = 0, sop_w2.sem_num = 1, sop_w2.sem_op = 0;
        if (alive == 0 || semop(sem_sync_id, &sop_w2, 1) == -1) {
            /*Debug*/
            /* printf("[NODE: %i] Decremento semaforo fallito:%s\n",getpid(), strerror(errno));
            printf("[NODE: %i] Exit\n",getpid()); */
            exit(1);
        }
    }

    /*initialize buffer*/
    hops_reach = 1;
    received = 0;

    if(first==0){
        received = msgrcv(msq_id_priority, &mex, sizeof(struct Message), (long)getpid(), 0);
        if(msg_pool_deposit(&mex) == 1){
            /*Debug*/
            /*printf("[NODE: %d] Depositata transazione dalla priority\n", getpid());*/
        }
    }

    queue_count = floor(conf->SO_USERS_NUM / QUEUE_RATIO);
    off = 0;

    /*inizio routine*/
    while( alive == 1) {

        /* Controlla se ci sono amici da aggiungere */
        get_friends();

        /* Legge dale code dei messaggi per ottenere le transazioni */
        do {

            queue_read = 0;
                
            /*
                Estrae la coda da cui leggere e controlla se ha dei messaggi. 
                La struttura delle code è un array circolare, quindi viene letto un messaggio da ogni coda, fino a quando non si trova una coda che non ha un messaggio disponibile.
                Questo permette di bilanciare il numero di messaggi tra le code
            */
            do {
                off = queue_count > 0 ? queue_id % queue_count : 0;
                received = msgrcv(msg_queue_ids[off], &mex, sizeof(struct Message), (long)getpid(), IPC_NOWAIT);
                /*Debug*/
                /* printf("%d coda: %d rec: %d \n",getpid(), msg_queue_ids[queue_id], received); */
                queue_id++;
                queue_read++;
            } while(received == -1 && queue_read < queue_count);

            if (received != -1) {
                /*
                    Se trova un messaggio prova a depositare la transazione: se non riesce per pool piena prova ad inviarla ad un amico. 
                    Se raggiunge il numero di hops viene inviata al master.
                    Se invece non riesce ad inviare un messaggio ad un amico per le code piene rifiuta la transazione, e rimborsa l'user
                */

                if(msg_pool_deposit(&mex) == 1){
                    /*Debug*/
                    /* printf("[NODE: %d] Depositata transazione nella pool \n", getpid());*/
                }else{
                    /*Se gli hops non sono stati raggiunti la rimando */
                    if(send_friend_trans(mex, &hops_reach) ==-1 ){
                        if (hops_reach == 1) {
                            /*Debug*/
                            /* printf("[NODE: %d] Invio al master'\n", getpid());*/
                            /* Numero di hope raggiunti, notifico al master che deve creare la transazione */
                            send_message_to_master(mex);
                        } else {
                            send_rejected_trans(mex);
                        }
                    }
                }
            }
        } while (received != -1 && alive == 1);

        /* Cronjob invio transazione estratta dalla Transaction Pool ad un amico */
        if(alive == 1 && shd_node_info[shdOffset].trans_pool_size>0){
            /*Estrazione transazione*/
            idx_last_tp_used=find_tp_trans(idx_last_tp_used+1);
            mex.trans=transaction_pool[idx_last_tp_used].trans;
            /*Inizzalizzazione hops*/
            mex.hops=0;
            if (send_friend_trans(mex, &hops_reach) == 1) {
                /* "Libero" la posizione dalla Transaction Pool */
                transaction_pool[idx_last_tp_used].is_used=0;
                /*TODO: Aggiungere commento*/
                enter_shd_safe_mode();
                shd_node_info[shdOffset].trans_pool_size--;
                exit_shd_safe_mode();
            } else {
                if(hops_reach==1){
                    /*Debug*/
                    /*printf("[NODE: %d] Invio al master'\n", getpid());*/
                    /* printf("[NODE: %d] Numero hops raggiunti\n", getpid()); */

                    /* Numero di hope raggiunti, notifico al master che deve creare la transazione */
                    send_message_to_master(mex);
                }else{
                    send_rejected_trans(mex);
                }
            }
        }

        /* Ciclo di vita dei blocchi */
        if(alive == 1 && shd_node_info[shdOffset].trans_pool_size>=SO_BLOCK_SIZE-1){

            /* Start creazione del blocco */

            /* Prenota l'id del blocco, se non riesce va in pausa */
            int _id = get_new_block_id();
            if (_id == -1) {
                printf("[NODE: %d] Errore durante la creazione del block_id\n", getpid());
                if (alive == 1) {
                    pause();
                } else {
                    break;
                }
            }
            
            /* Prova a creare il blocco con l'id prenotato, se non riesce va in pausa */
            if (create_block(_id, &blocco) == -1) {
                printf("[NODE: %d] Errore durante la creazione del blocco\n", getpid());
                if (alive == 1) {
                    pause();
                } else {
                    break;
                }
            };

            /* Prova a scrivere il blocco sul ledger, se non riesce per dimensione massima raggiunta notifica il main */
            reached_max_size = 0;
            if (add_block(blocco, &reached_max_size) == 0) {
                if (reached_max_size == 1) {
                    /* Dimensione massima ledger raggiunta, aggiorno le transazione, avviso il master e chiudo tutto */
                    clear_block();
                    kill(getppid(), SIGUSR1);
                    printf("[NODE: %d] Dimensione massima ledger raggiunta\n",getpid());
                    if (alive == 1) {
                        pause();
                    } else {
                        break;
                    }
                }
            }

            /*Simulazione elaborazione*/
            if (nsleep(rand_long_range(conf->SO_MIN_TRANS_GEN_NSEC,conf->SO_MAX_TRANS_GEN_NSEC)) < 0) {
                /*Debug*/
                /*printf("Nanosleep fallita:%s\n",strerror(errno));*/
            }
            clear_block();
        }
    }
    /*Debug*/
    /*print_tp();*/
    /*print_tp_budget();*/
    clear();
    /* Debug */
    /* printf("***** End node: %d\n", getpid());*/
    exit(0);    
    
}

void init_process(int first) {
    struct sembuf sop;
    struct NodeInfo _info;
    int count;
    int i;

    RESET_SEED

    
    signal(SIGTERM,sig_handler);
    signal(SIGINT,sig_handler);

    /* Init semaforo*/
    sem_proc_alive_id = semget(SEM_PROCESSES_ALIVE_KEY, 2, 0666);
    SEMBUF_V(sop,SEM_NODE_ID, 1);
    if (semop(sem_proc_alive_id, &sop, 1) == -1) {
        printf("%d Incremento semaforo fallito:%i\n",getpid(), errno);
        exit(1);
    } else {
        /* Debug */
        /* printf("%d Nodi vivi:%i\n",getpid(), semctl(sem_proc_alive_id, SEM_NODE_ID, GETVAL, 0));	 */
    }

    /* Inizializzazione struct da copiare sulla shared memory */
    _info.p_id=getpid(), _info.trans_pool_size = 0, _info.budget = 0,_info.status = 1; 
    /* Deposito sulla shared memory */
    shd_node_info[shdOffset] = _info;

    /* Inizializzazione code principali */
    msq_id_priority = msgget(MSG_PRIORITY_KEY,0600);
    msg_queue_rejected_id = msgget(MSG_REJECTED_KEY,0600);
    if (msq_id_priority < 0 || msg_queue_rejected_id < 0) {
        /* Debug */
        /*printf("[NODO:%i] Error during init queues id: %s\n",getpid(), strerror(errno));*/
        exit(1);
    }

    /* Inizializzazione code User->Node */
    /* Trova il numero di code da creare */
    count=floor(conf->SO_USERS_NUM / QUEUE_RATIO);
    msg_queue_ids = malloc((count + 1) * sizeof(int));

    for (i = 0; i <= count; i++)
    {
        msg_queue_ids[i] = msgget(MSG_KEY + i,0600);
        if (msg_queue_ids[i] < 0) {
            printf("Error during init queues id: %s\n",strerror(errno));
            exit(1);
        } else {
            /* Debug */
            /* printf("[NODO:%i] New queues id: %i\n",getpid(), msg_queue_ids[i]); */
        }
    }

    /* Inizializzazione amici */
    if(conf->SO_FRIENDS_NUM>0){
        msg_id_friends=msgget(MSG_FRIENDS_KEY,0600);
        if(msg_id_friends==-1){
            printf("[NODO:%i] Errore nell'apertura della coda degli amici\n",getpid());
            exit(0);
        }
        friends=malloc(sizeof(pid_t)*conf->SO_FRIENDS_NUM);
        friends_max = conf->SO_FRIENDS_NUM;        
        friends_count = 0;
    }

    /* Init semaforo for sync*/
    sem_sync_id = semget(SEM_SYNC_KEY, 2, 0666);

    if (sem_sync_id < 0) {
        printf("Error during init sync semaphore: %s\n",strerror(errno));
        exit(1);
    } else {
        if (first == 1) {

            /* Debug */
            /* printf("[NODO: %d] Start sem per sincronizzazione \n", getpid()); */

            /* Devo attendere la sincronizzazione */
            SEMBUF_P(sop,SEM_USER_ID, 0);
            if (semop(sem_sync_id, &sop, 1) == -1) {
                printf("Decremento semaforo fallito:%i\n",errno);
                exit(1);
            }

        }
    };

    init_ledger(0, SO_REGISTRY_SIZE);
}

void clear() {

    /* Setta lo status a 0*/
    enter_shd_safe_mode();
    shd_node_info[shdOffset].status = 0;
    exit_shd_safe_mode();

    /* Disalloca le risorse*/
    shmdt((void *) conf);
    shmdt((void *) shd_user_info);
    shmdt((void *) shd_node_info);

    /*free of mallocs*/
    free(transaction_pool);
    if(conf->SO_FRIENDS_NUM>0){
        free(friends);
    }

    dispose_ledger(0);
}

int msg_pool_deposit(struct Message *mex) {
    /* Debug */
    /*printf("[NODE: %d] Inizio deposito pool \n", getpid());*/

    /* Inizializzazione parametri */
    int off;
    Transaction trans;
    struct tp_block tmp;

    off=0;

    /* Trovo il primo offset libero disponibile */
    while(transaction_pool[off].is_used==1) {
        off++;
    }
    if (shd_node_info[shdOffset].trans_pool_size < conf->SO_TP_SIZE&&off<conf->SO_TP_SIZE) {
        
        /* Copio parametri dal messaggio alla struct temporanea */
        trans.timestamp_sec=mex->trans.timestamp_sec,
        trans.timestamp_nsec=mex->trans.timestamp_nsec,
        trans.sender=mex->trans.sender, trans.receiver=mex->trans.receiver,
        trans.quantity = mex->trans.quantity, trans.reward = mex -> trans.reward;

        /* inizializzo parametri blocco */
        tmp.is_used = 1, tmp.trans = trans;

        transaction_pool[off]=tmp;
        increment_tp();
        /* Debug */
        /* printf("[NODE: %i] Deposito in posizione:%i, pool size: %i\n",getpid(),off, shd_node_info[shdOffset].trans_pool_size);*/
        return 1;
    } else {
        /* Pool piena, reject transaction, avvisa il sender*/
        return -1;
    }
}

int create_block(int block_id, struct VirtualBlock * block) {

    /* Inizializzazione parametri metodo */
    int off;
    int sum;
    
    if (block_id > SO_REGISTRY_SIZE) {
        return -1;
    };

    /* struct VirtualBlock tmp; */
    off=0;
    sum=0;

    block->id = block_id;

    while(off<SO_BLOCK_SIZE-1){
        /*salvo la posizione per la pulizia e per l'allocamento nel blocco*/
        idx_last_tp_used=clear_pos[off]=find_tp_trans(idx_last_tp_used+1);

        if (idx_last_tp_used == -1) {
            return -1;
        }

        /*referenzio la tr nella pool al blocco*/
        block->Transaction[off]=&transaction_pool[idx_last_tp_used].trans;
        off++;
    }
    
    /* Calcolo la somma delle reward delle transazioni nel blocco */
    for(off=0;off<SO_BLOCK_SIZE-1;off++){
        sum+=block->Transaction[off]->reward;
    }

    /* Genero la transazione che contiene tutte le reward del blocco e con sender==-1(macro) */
    reward_transaction=build_transaction(getpid(),REWARD_SENDER_PID,sum,0);

    /* Imposto la transazione appena creata come ultima del blocco */
    block->Transaction[off]=&reward_transaction;

    return 1;
}

int find_tp_trans(int off){
    /* Ricerca circolare della prima transazione della Transaction Pool a partire dall'offset off */
    int count = 0;
    while(count<conf->SO_TP_SIZE){
        if(transaction_pool[off % conf->SO_TP_SIZE].is_used==1){
            return off % conf->SO_TP_SIZE;
        }
        off++;
        count++;
    }
    return -1;
}

void clear_block(){
    int i;

    enter_shd_safe_mode();

    /* Aggiorna pool e budget */
    for(i=0;i<SO_BLOCK_SIZE-1;i++){
        transaction_pool[clear_pos[i]].is_used=0;
        shd_node_info[shdOffset].trans_pool_size--;
        shd_node_info[shdOffset].budget += transaction_pool[clear_pos[i]].trans.reward;
        /*Debug*/
        /*printf("[NODE: %d] liberata posizione:%i\n",getpid(),clear_pos[i])*/
    }

    exit_shd_safe_mode();
}

int print_tp(){
    int i;
    
    printf("*** STATO TRANSACTION POOL %i ***\n",getpid());
    for(i=0;i<conf->SO_TP_SIZE;i++){
        if(transaction_pool[i].is_used==1){
            print_transazione(transaction_pool[i].trans);
        }
    }
    printf("***       FINE STATO          ***\n");
    return 1;
}


int print_tp_budget(){
    int i;
    int s = 0;
    for(i=0;i<conf->SO_TP_SIZE;i++){
        if(transaction_pool[i].is_used==1){
            s += transaction_pool[i].trans.reward + transaction_pool[i].trans.quantity;
            /* Debug */
            /* print_transazione(transaction_pool[i].trans);*/
        }
    }
    printf("[NODE: %d] Somma dei soldi in transation pool:%i\n",getpid(), s);
    return 1;
}

void increment_tp() {
    
    enter_shd_safe_mode();
    shd_node_info[shdOffset].trans_pool_size++;
    exit_shd_safe_mode();
}

void print_friends(){
    /* Print della lista degli amici */
    int i;
    printf("[NODE: %d] Lista di amichetti:",getpid());
    for(i=0;i<friends_count;i++){
        if(i==friends_count-1){
            printf("%d",friends[i]);
        }else{
            printf("%d, ",friends[i]);
        }
    }
    printf("\n"); 
}

int send_friend_trans(struct Message mex, int * reached_hops){
    int frn;
    int queue_id;
    struct msqid_ds buf;
    int rc;

    *reached_hops = 0;
    if (mex.hops >= conf->SO_HOPS) {
        *reached_hops = 1;
        /* printf("[NODE:%i] Numero massimo di hops raggiunti in un messaggio!\n",getpid()); */
        return -1;
    }

    /* Estrazione friend a cui inviare la transazione */
    frn=rand_int_range(0, friends_count-1);
    mex.type=(long)friends[frn];
    mex.hops++;
    /* Selezione della coda da utilizzare */
    queue_id = get_queue_id_less_used();
    /* Invio della transazione */
    if(msgsnd(queue_id,&mex,sizeof(struct Message), IPC_NOWAIT) !=-1 ){
        /*Debug*/
        /* printf("[NODE:%i] Transazione inviata ad amico (%i) con hops:%hi, in queue: %d\n ",getpid(),friends[frn],mex.hops, queue_id); */
        return 1;
    } else {
        rc = msgctl(queue_id, IPC_STAT, &buf);
        /*Debug*/
        /* printf("[NODE: %d] Errore durante l'invio della transazione (type: %d, hops: %d): %s; Messaggi presenti in coda: %i\n", getpid(),mex.type, mex.hops,strerror(errno), buf.msg_qnum); */
    }

    return -1;
}

int send_message_to_master(struct Message mex) {
    mex.type=(long)getppid();
    if  (msgsnd(msq_id_priority,&mex,sizeof(struct Message), 0) != -1) {
        /* printf("[NODE:%i] Notifico al master di creare un nuovo nodo in queue: %d with type: %d \n",getpid(),msq_id_priority, mex.type); */
    } else {
        printf("[NODE:%i] Errore notifica al master: %s\n",getpid(), strerror(errno));
    }
    /* Notifica il master che gli sta arrivando una transazione, di leggere dalla coda */
}


/* Setta i semafori per preparare il nodo a scrivere sulla shared memory */
void enter_shd_safe_mode() {

    struct sembuf sop_v;
    struct sembuf sop_w;

    sop_w.sem_flg = 0; sop_w.sem_num = 0; sop_w.sem_op = 0;
    semop(sem_sync_id, &sop_w, 1);

    /* Aggiorna il numero di scrittori */
    SEMBUF_V(sop_v, 1, 0);
    semop(sem_sync_id, &sop_v, 1);
}

/* Setta i semafori per rilasciare il nodo dalla scrittura della shared memory */
void exit_shd_safe_mode() {
    struct sembuf sop_p;

    /* Aggiorna il numero di scrittori */
    SEMBUF_P(sop_p, 1, 0);
    semop(sem_sync_id, &sop_p, 1);

}

void initiate_friend_list(){
    /* Inizializzazione parametri */
    int i;
    struct MsgFriend buff;

    i = 0;
    while(i<conf->SO_FRIENDS_NUM){
        if(msgrcv(msg_id_friends, &buff, sizeof(struct MsgFriend), (long)getpid(), 0)!=-1){
            /*Debug*/
            /*printf("[Node:%d] Ricevuto amichetto: %d\n",getpid(),buff.friend_pid);*/
            friends[i]=buff.friend_pid;
            i++;
        }
    }
    /* Salvo quanti amici ha attualmente il nodo */
    friends_count = i;
}

/* Assegna i nuovi amici, se ce ne sono in coda */
void get_friends() {
    struct MsgFriend buff;
    int got = 0;
    do {
        got = msgrcv(msg_id_friends, &buff, sizeof(struct MsgFriend), (long)getpid(), IPC_NOWAIT);
        if (got > -1) {

            if (friends_count > friends_max - 1) {
                /* Devo ricevere un nuovo amico e ho raggiunto il massimo di amici disponibile, rialloco il doppio delle risorse*/
                friends_max = friends_max * 2;
                friends = realloc(friends, sizeof(pid_t)*friends_max);
                /*Debug*/
                /* printf("[NODE:%i] Nuovi amici ricevuti, risorse reallocate\n",getpid()); */
            } else {
                /*Debug*/
                /* printf("[NODE:%i] Nuovi amici ricevuti\n",getpid()); */
            }
            friends[friends_count]=buff.friend_pid;
            friends_count++;
        }
    } while ( got != -1 );
}

/* Invio transazione rifiutata all'user che l'ha inviata per effettuare un rimborso */
int send_rejected_trans(struct Message mex) {
    struct MsgRejected m_rej;

    m_rej.type = (long)mex.trans.sender;
    m_rej.amount = mex.trans.quantity + mex.trans.reward;

    if(msgsnd(msg_queue_rejected_id,&m_rej,sizeof(struct MsgRejected), IPC_NOWAIT) !=-1 ){
        /*Debug*/
        /* printf("[NODE:%i] Transazione rifiutata reinviata all'user %i \n ",getpid(),mex.trans.sender); */
        return 1;
    }
    return -1;
}

int get_queue_id_less_used() {
    int i;
    int min;
    int off_id;
    int count;
    struct msqid_ds buf;
    
    count = floor(conf->SO_USERS_NUM / QUEUE_RATIO);
    msgctl(msg_queue_ids[0], IPC_STAT, &buf);
    min = buf.msg_qnum;
    off_id = 0;

    for (i = 1; i < count; i++)
    {
        msgctl(msg_queue_ids[i], IPC_STAT, &buf);
        if (buf.msg_qnum < min) {
            off_id = i;
        }
    }
    return msg_queue_ids[off_id];
}