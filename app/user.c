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

/**********************
*      FUNCTIONS      *
**********************/
void init_process();
void clear();
int user_send_transaction(int msq_id,pid_t node,pid_t user,int user_import,float reward_import);
int get_trans(int quant,float p_rew);
pid_t get_node_rec_pid();
pid_t get_user_rec_pid();
void sync_alive_users();
int has_queue_some_messages(int q_id);
void update_budget();
int generate_transation(int * no_more_users);


/**********************
*     GLOBAL VARS     *
**********************/
struct Options *conf;
struct UserInfo * shd_user_info;
struct NodeInfo * shd_node_info;

int msg_queue_id;
int msg_queue_rejected_id;

int shd_offset;
int retry;
int sem_proc_alive_id;
int * sendable_users;
int sendable_user_count;
int sem_sync_id;
int alive = 1;
int local_budget;
int signal_catched = 0; 


/**********************
*      LIFECYCLE      *
**********************/
void sig_handler(int signum){
    int ex;
    int no_more_users;

    switch (signum) {
        case SIGTERM:;
            alive = 0;
            break;

        case SIGINT:;
            /* Placeholder per il ctrl c dal parent */
            break;

        case SIGUSR1:;
            signal_catched = 1;
            signal(SIGUSR1, sig_handler);
            /* Genera la transazione */
            ex = 0;
            no_more_users = 0;
            if (generate_transation(&no_more_users) == -1) {
                if (no_more_users == 1) {
                    alive = 0;
                }
                retry++;
            } else {
                retry = 0;
                update_budget();
            }
            break;

        default:;
            break;
    }
}

int main(int argc,char** argv){
    int conf_id;
    int u_shd_id;
    int n_shd_id;
    struct sembuf sop;
    struct sembuf sop_w2;
    int no_more_users;
    long time;
    int succ;
    struct MsgRejected mex;
    struct timespec timeout;

    /*Debug*/
    /* printf("***** Start user:%i,%i\n",getpid(),getppid()); */
    check_args(5)
    /* 
        ARGS
        0: type
        1: config id
        2: id users shdm
        3: id nodes shdm
        4: offset
    */
    conf_id = atoi(argv[1]);
    u_shd_id = atoi(argv[2]);
    n_shd_id = atoi(argv[3]);
    shd_offset = atoi(argv[4]);
    
    /*Debug*/
    /* printf("User %d cid %d uid %d nid %d qid %d off %d \n", getpid(), conf_id, u_shd_id, n_shd_id, msq_id, shd_offset); */

    /*Read configuration*/
    conf = (struct Options *) shmat(conf_id, NULL, 0);
    /*Debug*/
    /*print_config(conf);*/

    shd_user_info = (struct UserInfo *)shmat(u_shd_id, NULL, 0);
    /*Node awareness memory per l'estrazione dei receivers*/
    shd_node_info=(struct NodeInfo *)shmat(n_shd_id,NULL,0);

    init_process();
    
    /*Wait for sync process*/
    /*Debug*/
    /* printf("Pid %d Sem sync: %d \n", getpid(), semctl(sem_sync_id, 0, GETVAL, 0)); */
    sop.sem_flg = 0, sop.sem_num = 0, sop.sem_op = 0;
    if (alive == 0 || semop(sem_sync_id, &sop, 1) == -1) {
        printf("[User: %d] Decremento semaforo fallito:%s\n",getpid(),strerror(errno));
        exit(1);
    }
    sync_alive_users();

    /* Aspetta che tutti i nodi abbiano ricevuto gli amici */
    sop_w2.sem_flg = 0, sop_w2.sem_num = 0, sop_w2.sem_op = 0;
    if (alive == 0 || semop(sem_sync_id, &sop_w2, 1) == -1) {
        printf("Decremento semaforo fallito:%i\n",errno);
        exit(1);
    }

    /*Iniziallizazione budget*/
    shd_user_info[shd_offset].budget = conf->SO_BUDGET_INIT;
    while( alive == 1){
        update_budget();
        if (has_queue_some_messages(msg_queue_rejected_id) == 1) {
            /* Transazioni scartate presenti,rimborsiamole */            
            while(msgrcv(msg_queue_rejected_id, &mex, sizeof(struct MsgRejected), (long)getpid(), IPC_NOWAIT)!=-1){
                /*Debug*/
                /*printf("[USER: %d] Letta transazione rifiutata\n", getpid());*/
                local_budget-=mex.amount;
                update_budget();
                retry++;
                /*Debug*/
                /*print_transazione(mex.trans);*/
            }
        }

        if(retry>=conf->SO_RETRY){
            /*Debug*/
            /*printf("[USER: %d] Numero di SO_RETRY raggiunto [%d], inizio fase di eliminazione utente.\n",getpid(),retry);*/
            alive = 0;
            break;
        }

        no_more_users = 0;
        if (generate_transation(&no_more_users) == -1) {
            if (no_more_users == 1) {
                break;
            }
            retry++;
            /*Debug*/
            /* printf("[USER: %d] Generate transation failed, retry: %d\n",getpid(),retry); */

        } else {
            retry = 0;
            update_budget();

            /* Esegue l'attesa per la generazione con successo di una transazione. 
                Utilizza un while per far si che, se lo sleep è stato interrotto da un segnale di generazione di transazione,
                ritorni ad aspettare da dove era rimasto. */
            time = rand_long_range(conf->SO_MIN_TRANS_GEN_NSEC,conf->SO_MAX_TRANS_GEN_NSEC);
            /*Debug*/
            /* printf("[USER: %d] Attesa stimata: %d\n",getpid(),time); */
            timeout.tv_sec = time / 1000000000L, timeout.tv_nsec =  time % 1000000000L;
            succ = 0;
            do {
                signal_catched = 0;
                succ = nanosleep(&timeout, &timeout);
                /*Debug*/
                /*printf("[USER: %d] Result nanosleep: %d, tempo rimanente s %lu ns %lu\n",getpid(),succ,timeout.tv_sec, timeout.tv_nsec);*/
            } while (succ == -1 && signal_catched == 1);
        }
    }
    /* Ultimo update, é possibile che un processo sia morto e 
       riceva delle transazioni, scommentare per includerle*/
    /* update_budget();*/
    
    /*Debug*/
    /* printf("[User: %d] Somma dei soldi in uscita:%i, in entrata: %i, ultimo blocco letto: %i\n",
        getpid(), local_budget, get_process_revenue(getpid()), shd_user_info[shd_offset].last_block_read); */

    clear();
    /*Debug*/
    /* printf("***** End user: %d\n", getpid()); */

    exit(0);     
    return 0;
}

void init_process() {

    struct sembuf sop;
    struct UserInfo _info; 
    int queue_offset;

    RESET_SEED
    signal(SIGTERM,sig_handler);
    signal(SIGINT,sig_handler);
    signal(SIGUSR1,sig_handler);

    /*Init semaforo*/
    sem_proc_alive_id = semget(SEM_PROCESSES_ALIVE_KEY, 2, 0666);
    SEMBUF_V(sop,SEM_USER_ID, 1);
    if (semop(sem_proc_alive_id, &sop, 1) == -1) {
        printf("Incremento semaforo fallito:%i\n",errno);
        exit(1);
    }
    
    _info.p_id= getpid(), _info.budget = conf->SO_BUDGET_INIT, _info.status = 1,_info.last_block_read = 0; 
    shd_user_info[shd_offset] = _info;

    /* Trova il numero di code da creare */
    queue_offset =floor(shd_offset / QUEUE_RATIO);
    msg_queue_id = msgget(MSG_KEY+queue_offset,0600);
    msg_queue_rejected_id = msgget(MSG_REJECTED_KEY,0600);
 
    if (msg_queue_id < 0 || msg_queue_rejected_id < 0) {
        printf("[NODO:%i] Error during init queues id: %s\n",getpid(), strerror(errno));
        exit(1);
    }

    /*Setta il numero di processi utente vivi*/
    sendable_user_count = conf->SO_USERS_NUM;
    sendable_users = malloc(sizeof(int) * conf->SO_USERS_NUM);

    /*Init semaforo for sync*/
    sem_sync_id = semget(SEM_SYNC_KEY, 2, 0666);
    if (sem_sync_id < 0) {
        printf("Error during init sync semaphore: %s\n",strerror(errno));
        exit(1);
    } else {
        SEMBUF_P(sop,SEM_USER_ID, 0);
        if (semop(sem_sync_id, &sop, 1) == -1) {
            printf("%d Incremento semaforo fallito:%i\n",getpid(), errno);
            exit(1);
        }
    };
    init_ledger(0, SO_REGISTRY_SIZE);
}

void clear() {

    struct sembuf sop_v;
    struct sembuf sop_p;
    struct sembuf sop_w;

    sop_w.sem_flg = 0; sop_w.sem_num = 0; sop_w.sem_op = 0;
    semop(sem_sync_id, &sop_w, 1);

    /* Aggiorna il numero di scrittori */
    SEMBUF_V(sop_v, 1, 0);
    semop(sem_sync_id, &sop_v, 1);

    /*Setta lo status a 0*/
    shd_user_info[shd_offset].status = 0;

    /* Aggiorna il numero di scrittori */
    SEMBUF_P(sop_p, 1, 0);
    semop(sem_sync_id, &sop_p, 1);

    /* Disalloca le risorse*/
    shmdt((void *) conf);
    shmdt((void *) shd_user_info);
    shmdt((void *) shd_node_info);

    dispose_ledger(0);

    free(sendable_users);
    /*Debug*/
    /* printf("[User: %d] Terminato clear \n", getpid()); */

}

/* 
    Colloca nell'array sendUser, da posizione 0 a posizione {sendable_user_count}, i pid degli users ancora vivi. 
    I pid degli users terminati si trovano da posizione {sendable_user_count} + 1
*/
void sync_alive_users() {
    int x = 0;    
    int y = conf->SO_USERS_NUM;
    size_t i;
    for (i = 0; i < conf->SO_USERS_NUM; i++)
    {
        if (shd_user_info[i].status == 1 && shd_user_info[i].p_id != getpid()) {
            sendable_users[x] = shd_user_info[i].p_id;
            x++;
        } else {
            sendable_users[y-1] = shd_user_info[i].p_id;
            y--;
        }
    }
    sendable_user_count = x;
}

/* Genera una transazione */
int user_send_transaction(int msq_id,pid_t node,pid_t user,int user_import,float reward_import){
    struct Message mex;  
    mex.type = node; mex.hops = 0; mex.trans = build_transaction(getpid(),user,user_import,reward_import);
    if(msgsnd(msq_id,&mex,sizeof(struct Message), IPC_NOWAIT)!=0){
        /*Debug*/
        /* printf("[USER: %d] Errore nell'invio del messaggio:%s\n",getpid(), strerror(errno)); */
        return -1;
    }else{
        int amount = reward_import + user_import;
        /*Debug*/
        /* printf("[USER: %d] Message sent to node %li with amout %d, in queue: %d\n",getpid(), mex.type, amount, msq_id); */
        return 1;
    }
}

int get_trans(int quant,float p_rew) {
    /*quant * 0.p_rew*/
    float res = quant*((100-p_rew)/100);
    int ret=round(res);
    return (ret<1) ? 1 : ret;
}

/* Ottiene il pid di un nodo casuale */
pid_t get_node_rec_pid(){
    return shd_node_info[rand_int_range(0, conf->SO_NODES_NUM-1)].p_id;
}

/* Ottiene il pid di un utente ancora vivo casuale */
pid_t get_user_rec_pid(){

    int alive_users;

    alive_users = semctl(sem_proc_alive_id, SEM_USER_ID, GETVAL);
    
    /* Se il numero di user attualmente vivi è diverso dal numero in "cache" vuol dire che c'è stato un cambio sui processi vivi, quindi li risincronizzo */
    if (alive_users != sendable_user_count + 1) {
        sync_alive_users();
        /*Debug*/
        /*printf("pid %d riallocata memoria vivi\n", getpid());*/
    }
    if (sendable_user_count == 0) {
        /* Non ci sono più utenti disponibili */
        return -1;
    }
    return sendable_users[rand_int_range(0, sendable_user_count-1)];
}

int has_queue_some_messages(int q_id) {

    struct msqid_ds buf;
    int rc = msgctl(q_id, IPC_STAT, &buf);
    /*Debug*/
    /*printf("[USER: %d] Messaggi in coda %ld\n", getpid(),buf.msg_qnum);*/

    return buf.msg_qnum > 0 ? 1 : 0;
}

/* Aggiorna il proprio budget */
void update_budget() {

    struct sembuf sop_v;
    struct sembuf sop_p;
    struct sembuf sop_w;

    /* Aspetta che si possa scrivere sulla shared memory per aggiornare il budget*/
    sop_w.sem_flg = 0; sop_w.sem_num = 0; sop_w.sem_op = 0;
    semop(sem_sync_id, &sop_w, 1);

    /*Debug*/
    /* printf("[USER: %d] Inizio scrittura budget\n", getpid()); */

    /* Aggiorna il numero di scrittori */
    SEMBUF_V(sop_v, 1, 0);
    semop(sem_sync_id, &sop_v, 1);

    shd_user_info[shd_offset].budget=get_process_revenue(getpid())-local_budget+conf->SO_BUDGET_INIT;
    shd_user_info[shd_offset].last_block_read =  shd_ledger_info->block_count-1;
    
    /* Il block count parte da 0, così come l'id del primo blocco. Dobbiamo settare come valore iniziale -1 se un user non ha 
    letto nessun blocco, settare 0 significherebbe che l'user ha letto il primo blocco */
    
    /*Debug*/
    /* printf("[USER: %d] Budget calcolato: %d\n",getpid(),shd_user_info[shd_offset].budget); */

    /* Aggiorna il numero di scrittori */
    SEMBUF_P(sop_p, 1, 0);
    semop(sem_sync_id, &sop_p, 1);

    /*Debug*/
    /* printf("[USER: %d] Terminata scrittura budget\n", getpid()); */
}

int generate_transation(int * no_more_users) {
    pid_t node_receiver;
    pid_t user_receiver;
    int total_transaction;
    int reward_import;
    int user_import;
    *no_more_users = 0;

    if(shd_user_info[shd_offset].budget < 2) {
        /*Debug*/
        /* printf("[USER: %d] Terminato budget\n",getpid()); */
        return -1;
    }

    /*Genero info per la transazione*/
    node_receiver=get_node_rec_pid();
    user_receiver=get_user_rec_pid();
    if(user_receiver==-1){
        /*Debug*/
        /*printf("[USER: %d] User -1 \n",getpid());*/
        /*User terminati, si muore*/
        *no_more_users = 1;
        return -1;
    }

    /*Generazione importo da spedire,comprensivo di reward del nodo e importo da mandare all'utente*/
    total_transaction=rand_int_range(2,shd_user_info[shd_offset].budget);
    /*Debug*/
    /* printf("[USER: %d] Transaction amount %d\n",getpid(), total_transaction); */
    reward_import=get_reward(total_transaction,conf->SO_REWARD);
    user_import=total_transaction-reward_import;

    /*Invio transazione*/
    if(user_send_transaction(msg_queue_id,node_receiver,user_receiver,user_import,reward_import)==1) {
        /*Aggiornamento budget da detrarre*/
        local_budget+=total_transaction;
    } else {
        /*Debug*/
        /* printf("[USER: %d] Send transaction error \n",getpid()); */
        return -1;
    }

    return 0;
}