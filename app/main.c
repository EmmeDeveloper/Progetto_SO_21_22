#define _XOPEN_SOURCE 700
#define PRINT_PROCESS_LIMIT 10

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/msg.h>
#include <math.h>

#include "config.h"
#include "common.h"
#include "ledger.h"

/**********************
*      FUNCTIONS      *
**********************/
struct Options *get_config();
void init_process(struct Options *conf);
void clear_resources();
char **get_params(int offset, short is_node, int first);
void kill_nodes(int node_count);
void kill_users(int user_count);
int check_files();
int get_min_idx_from_arr(struct UserInfo *usercount,int n);
int get_max_idx_from_arr(struct UserInfo *usercount,int n);
void print_user(struct UserInfo u);
void print_node(struct NodeInfo u);
void print_summary(int user_count, int node_count);
void print_final_review(int user_count, int node_count, int user_alive);
void send_friends();
int generate_init_friends(struct Options *conf,int *pnt,pid_t avoid,int ret);
void print_budget(int user_count);
int create_process(char *path, char **conf, int is_node );
int get_random_int(int * from, int from_length, int * result, int result_length);


/**********************
*     GLOBAL VARS     *
**********************/
int * nodes;
int * users;
int max_nodes;

struct UserInfo *user_info;
int users_shd_id = 0;

struct NodeInfo *nodes_info;
int nodes_shd_id = 0;

struct Options *conf;

int shd_rej_id = 0;

int msg_queue_friends_id = 0;
int msg_queue_priority_id = 0;
int msg_queue_rejected_id = 0;
int count;

int sem_proc_alive_id = 0;
/*
    sem_sync_id contiene 2 semafori usati per la sincronizzazione. Questo viene fatto in 2 step
    - step 1: sync iniziale
    id 0: usato da tutti i processi per l'inizializzazione delle memorie e altro
    id 1: usato dai nodi per ricevere i nodi amici
    - step 2: sync lettura/scrittura stampa
    id 0: indica la lettura, dal master, della shared memory per la stampa
    id 1: indica la scrittura, da user e nodi, della shared memory per la stampa
*/
int sem_sync_id = 0;
/* 
    0: Processo terminato per tempo esaurito
    1: Ctrl c handlato
    2: User terminati
    3: Ledger terminato
    4: Fork non eseguita
*/
int exitStatus = 0;
int alive = 1;


/**********************
*      LIFECYCLE      *
**********************/

void sig_handler(int signum){
    switch (signum) {
        case SIGALRM:;
            alive = 0;
            exitStatus = 0;
            break;

        case SIGINT:;
            alive = 0;
            exitStatus = 1;
            break;

        case SIGUSR1:;
            signal(SIGALRM,sig_handler);
            signal(SIGINT,sig_handler);
            signal(SIGUSR1,sig_handler);
            signal(SIGUSR2,sig_handler);
            alive = 0;
            exitStatus = 3;
            break;

        case SIGUSR2:
            alive = 0;
            exitStatus = 4;
            break;

        default:;
            break;
    }
}

int main(int argc,char ** argv){
    
    struct sembuf sop_w;
    struct sembuf sop_w2;
    struct Message mex;
    struct MsgRejected m_rej;
    int user_alive;

        
    /* friends */
    int j;
    struct MsgFriend msg_f;
    int * receivers;
    int * friends;

    /* Queue */
    int queue_id;
    int send_to_rej;

    /* Gestione fork */
    long pid;
    int p;
    int i;
    char **_conf;
    int succ;
    int is_node;
    char *path;

    /* Start processo main */
    printf("Start processo padre:%i\n",getpid());
    
    if(!check_files()){
        printf("Terminato processo:%i\n",getpid());return -1;
    }

    /*Inizializza e ottiene la configurazioni*/
    conf = get_config();
    print_config(conf);
    init_process(conf);

    /* Ciclo creazione figli */
    for (i = 0; i < conf->SO_NODES_NUM + conf->SO_USERS_NUM && alive == 1; i++) {

        /* Carico le opzioni per i processi da generare. Prima carico tutti i nodi, poi tutti gli utenti */
        is_node = i > conf->SO_NODES_NUM - 1?  0 : 1;
        path = is_node == 1 ? "./node" : "./user";
        _conf = is_node == 1 ? get_params(i, is_node, 1) : get_params( i- conf->SO_NODES_NUM, is_node, 1);

        p = create_process(path, _conf, is_node);
        if (p > 0) {
            if (is_node == 1) {
                nodes[i] = p;
            } else {
                users[i - conf->SO_NODES_NUM] = p;
            }
        } else {
            alive = 0;
            exitStatus = 4;
            break;
        }
    }

    /*Debug*/
    /* printf("Stato main: %d\n", alive); */

    /* Se alive è 0 vuol dire che è morto per un errore di fork*/
    if (alive == 1) {

        /* Attende che tutti i processi abbiano inizializzato le proprie struct */
        printf("Pid %d Sem sync: %d \n", getpid(), semctl(sem_sync_id, 0, GETVAL, 0));
        sop_w.sem_flg = 0, sop_w.sem_num = 0, sop_w.sem_op = 0;
        semop(sem_sync_id, &sop_w, 1);

        send_friends(conf);
        
        /* Attende che tutti i nodi abbiano ricevuto gli amici */
        sop_w2.sem_flg = 0,sop_w2.sem_num = 1,sop_w2.sem_op = 0;
        semop(sem_sync_id, &sop_w2, 1);

        printf("\n*\n*\nTutti i processi pronti, si vola\n*\n*\n");
        
        /* Setta la terminazione per tempo scaduto del programma */
        alarm(conf->SO_SIM_SEC);

    }

    succ=0;
    while (alive == 1) {

        /* 
            Se il main ha messaggi nella coda prioritaria vuol dire che ci sono transazioni che hanno raggiunto gli hops. 
            Crea un nodo per ognuno di questi, fino ad arrivare al numero massimo di nodi
        */
        do
        {
            send_to_rej = 0;
            pid = getpid();
            
            /* Debug */
            /* printf("Ho ricevuto messaggi sulla coda: %d with type: %d \n", msg_queue_priority_id, (long)getpid()); */
            succ = msgrcv(msg_queue_priority_id, &mex, sizeof(struct Message),pid, IPC_NOWAIT);
            if (succ != -1) {
                /* Debug */
                /* printf("Ricevuto msg di creazione di un nuovo nodo\n"); */
                /* Se non ha raggiunto ancora il numero massimo di nodi, ne crea un altro */
                if (conf -> SO_NODES_NUM < max_nodes ) {
                    
                    /* Ottiene i parametri */
                    _conf = get_params(conf->SO_NODES_NUM, 1, 0);

                    /* Crea nodo */
                    p = create_process("./node", _conf, 1);
                    free(_conf);
                    if (p > 0) {
                        nodes[conf->SO_NODES_NUM] = p;
                        conf->SO_NODES_NUM++;
                    } else {
                        send_to_rej = 1;
                    }

                    /* Invia il nuovo pid come amico a x nodi estratti a caso */
                    receivers = malloc(sizeof(int) * conf->SO_FRIENDS_NUM);
                    get_random_int(nodes, conf->SO_NODES_NUM, receivers, conf->SO_FRIENDS_NUM);
                    for (j = 0; j < conf->SO_FRIENDS_NUM; j++)
                    {
                        msg_f.type = receivers[j];
                        msg_f.friend_pid = p;
                        msgsnd(msg_queue_friends_id,&msg_f,sizeof(struct MsgFriend),0);
                    }
                    free(receivers);

                    /* Invia al nuovo nodo x pid estratti come amici */
                    friends = malloc(sizeof(int) * conf->SO_FRIENDS_NUM);
                    get_random_int(nodes, conf->SO_NODES_NUM, receivers, conf->SO_FRIENDS_NUM);
                    for (j = 0; j < conf->SO_FRIENDS_NUM; j++)
                    {
                        msg_f.type = p;
                        msg_f.friend_pid = friends[j];
                        msgsnd(msg_queue_friends_id,&msg_f,sizeof(struct MsgFriend),0);
                    }
                    free(friends);

                    /* Invia la transazione, e torna a volare sopra le nuvole */
                    mex.type = p;
                    mex.hops = 0;
                    if (msgsnd(msg_queue_priority_id, &mex, sizeof(struct Message), IPC_NOWAIT) != 0) {
                        /*Debug*/
                        /*printf("[MASTER] Errore nell'invio del messaggio al nuovo nodo: %s\n",strerror(errno));*/
                        /* Non riesco a inviare la transazione al nuovo nodo, rimandala verso l'utente per il rimborso */
                        send_to_rej = 1;
                    } else {
                        
                    }
                } else {
                    /* Numero di nodi max raggiunto, reinvia la transazione all'utente */
                    send_to_rej = 1;
                }

                /* Ho avuto un problema nell'invio della transazione al nuovo nodo, o non ho creato nessun nodo, invia il rimborso */
                if (send_to_rej == 1) {
                    m_rej.type = (long)mex.trans.sender;
                    m_rej.amount = mex.trans.quantity + mex.trans.reward;
                    if(msgsnd(msg_queue_rejected_id,&m_rej,sizeof(struct MsgRejected), IPC_NOWAIT) != 0){
                        /*Debug*/
                        /*printf("[MASTER] Errore nell'invio del messaggio della transazione fallita:%s\n", strerror(errno));*/
                        succ=-1;
                    }
                }
            };
        } while (succ != -1 && alive ==1);

        nsleep(1000000000L);
        if (alive == 1) {
            printf("\n");
            printf("Users alive: %d  --- Nodes alive: %d\n\n", 
                semctl(sem_proc_alive_id, SEM_USER_ID, GETVAL, 0), 
                semctl(sem_proc_alive_id, SEM_NODE_ID, GETVAL, 0)
            );
            print_summary(conf->SO_USERS_NUM, conf->SO_NODES_NUM);
            if (semctl(sem_proc_alive_id, SEM_USER_ID, GETVAL) == 0) {
                alive = 0;
                exitStatus = 2;
            }
        }
    }

    user_alive =conf->SO_USERS_NUM - semctl(sem_proc_alive_id, SEM_USER_ID, GETVAL);

    /* Gestione dello stato */
    switch (exitStatus)
    {
        case 0:
        case 1:
        case 3:
        case 4:
            kill_nodes(conf->SO_NODES_NUM);
            kill_users(conf->SO_USERS_NUM);
            break;
        
        case 2:
            kill_nodes(conf->SO_NODES_NUM);
            break;
    }

    /*Debug*/
     printf("Aspetto la morte dei figli: %d\n", alive); 
    /* L'uso del wait permette la chiusura dei processi zombie attivi */
    while (wait(NULL) > 0);

    /*Debug*/
    /* printf("Tutti i figli morti, stampo review \n"); */
    print_final_review(conf->SO_USERS_NUM,conf->SO_NODES_NUM, user_alive);
    clear_resources();
    exit(0);
}

struct Options *get_config() {
    return init_config("./setting.conf");
}

void clear_resources() {
    int i,id;

    /* Rimuove la shared memory per la configurazione */
    shmctl(CONFIG_SHM_ID, IPC_RMID, NULL);

    /* Rimuove la shared memory per le info degli users */
    shmctl(users_shd_id, IPC_RMID, NULL);

    /* Rimuove la shared memory per le info degli users */
    shmctl(nodes_shd_id, IPC_RMID, NULL);

    /* Rimuove la shared memory per il numero di transazioni rifiutate */
    shmctl(shd_rej_id, IPC_RMID, NULL);

    /* Eliminazione message queue */
    msgctl(msg_queue_friends_id, IPC_RMID, NULL);
    msgctl(msg_queue_priority_id, IPC_RMID, NULL);
    msgctl(msg_queue_rejected_id, IPC_RMID,NULL);
    for (i = 0; i <= count; i++){
        id=msgget(MSG_KEY + i,0600);
        msgctl(id, IPC_RMID,NULL); 
    }

    /* Elimina i semafori */
    semctl(sem_proc_alive_id, 2, IPC_RMID);
    semctl(sem_sync_id, 2, IPC_RMID);

    /* Pulisce il mastro */
    dispose_ledger(1);

    free(nodes);
    free(users);

    /*debug*/
    /* printf("Cleared all, waiting for close all child\n"); */
    printf("Terminato processo:%i\n",getpid());

}

void init_process(struct Options *conf) {
    /*var dichiarate in cima per std=c89*/
    int i;

    RESET_SEED

    /* Registra gli handler per la terminazione*/
    signal(SIGALRM,sig_handler);
    signal(SIGINT,sig_handler);
    signal(SIGUSR1,sig_handler);
    signal(SIGUSR2,sig_handler);

    /* Set delle variabili globali*/
    max_nodes = conf->SO_NODES_NUM * 4;
    nodes = (int *) malloc(max_nodes * sizeof(int));
    users = (int *) malloc(conf->SO_USERS_NUM * sizeof(int));

    /* Inizializza users e nodes */
    for (i=0; i < max_nodes; i++) {
        nodes[i] = 0;
    }
    for (i=0; i < conf->SO_USERS_NUM; i++) {
        users[i] = 0;
    }
    

    /* Shared memory usata per memorizzare le info degli users*/
    users_shd_id = shmget(USER_RESOURCES_SHARD_KEY, sizeof(struct UserInfo) * conf->SO_USERS_NUM, IPC_CREAT | 0666);
    if (users_shd_id == -1) {
        printf("Error during get users shared memory: %s\n",strerror(errno));
        exit(1);
    } else {
        user_info = (struct UserInfo *)shmat(users_shd_id, NULL, 0);
    }

    /* Shared memory usata per memorizzare le info dei nodes */
    nodes_shd_id = shmget(NODE_RESOURCES_SHARD_KEY, sizeof(struct NodeInfo) * max_nodes, IPC_CREAT | 0666);
    if (nodes_shd_id == -1) {
        printf("Error during get nodes shared memory: %s\n",strerror(errno));
        exit(1);
    } else {
        nodes_info = (struct NodeInfo *)shmat(nodes_shd_id, NULL, 0);
    }

    /* inizializzazione queue e rejected_que */
    msg_queue_friends_id = msgget(MSG_FRIENDS_KEY,IPC_CREAT|0600);
    msg_queue_priority_id = msgget(MSG_PRIORITY_KEY,IPC_CREAT|0600);
    msg_queue_rejected_id = msgget(MSG_REJECTED_KEY,IPC_CREAT|0600);

    if (msg_queue_rejected_id < 0 || msg_queue_friends_id < 0 || msg_queue_priority_id < 0) {
        printf("Error during init queues id: %s\n",strerror(errno));
        exit(1);
    }

    /* Trova il numero di code da creare */
    count=floor(conf->SO_USERS_NUM / QUEUE_RATIO);
    for (i = 0; i <= count; i++)
    {
        if (msgget(MSG_KEY + i,IPC_CREAT|0600) < 0) {
            printf("Error during init queues id: %s\n",strerror(errno));
            exit(1);
        }
    }
    
    /* Inizializzazione semafori per il contatore di processi attivi*/
    sem_proc_alive_id = semget(SEM_PROCESSES_ALIVE_KEY, 2, 0666 | IPC_CREAT);
    if (sem_proc_alive_id < 0) {
    	printf("Error during init sempahores: %s\n",strerror(errno));
        exit(1);
	} else {
        int i;
        for (i= 0; i< 2; i++) {
            if(semctl(sem_proc_alive_id, i, SETVAL, 0) == -1) {
                printf("[Master] Error during set initial value semaphores: %s\n",strerror(errno));
                exit(1);
            }
        }
    };
    printf("[Master] Sem proc creato :%i\n",sem_proc_alive_id);


    /*Inizializza il semaforo per la sync dei processi*/
    sem_sync_id = semget(SEM_SYNC_KEY, 2, 0666 | IPC_CREAT);
    if (sem_sync_id < 0) {
        printf("[Master] Error during init sync semaphore: %s\n",strerror(errno));
        exit(1);
    } else {
        if(semctl(sem_sync_id, 0, SETVAL, conf->SO_NODES_NUM + conf->SO_USERS_NUM) == -1 || 
            semctl(sem_sync_id, 1, SETVAL, conf->SO_NODES_NUM) == -1
        ) {
            printf("[Master] Error during set initial value sempahores: %s\n",strerror(errno));
            exit(1);
        }
    };

    printf("[Master] Sem sync creato :%i\n",sem_sync_id);

    /* Inizializza il libro mastro */
    init_ledger(1, SO_REGISTRY_SIZE);
    printf("[Master] Ledger creato :%i\n",sem_sync_id);
    
}

/* Restituisce i parametri da passare ai figli */
char **get_params(int offset, short is_node, int first) {

    int i;
    char **_pars = malloc(7 * sizeof(*_pars));

    int params[5];
    
    params[0]=CONFIG_SHM_ID;
    params[1]=users_shd_id;
    params[2]=nodes_shd_id;
    params[3]=offset;
    params[4]=first;
    
    /* Debug
    if(first==0){
        printf("Params to assign to new created node:\n\
                CONFIG_SHM_ID:%i\n\
                users_shd_id:%i\n\
                nodes_shd_id:%i\n\
                offset:%i\n\
                first:%i\n"
                ,params[0],params[1],params[2],params[3],params[4]);
    } */
    

    /* Allocate memory */
    _pars[0] = malloc(sizeof(char) * 5);
    for (i = 0; i < 6; i++) {
        int needed=(sizeof(char) * snprintf(NULL, 0, "%d", params[i]) + 1);
        /* Debug
        if(first==0){
            printf("Space needed for _pars[%i]:%i\n",i+1,needed);
        }  */
        _pars[i + 1] = (char *)malloc(needed);
    }
    _pars[6] = malloc(sizeof(NULL));


    _pars[0] = is_node == 1 ? "node" : "user" ;
    for (i = 0; i < 6; i++) {
        sprintf(_pars[i + 1], "%d", params[i]);
    }
    _pars[6] = NULL;

    

    return _pars;
}

void print_summary(int user_count, int node_count) {
    struct UserInfo *max;
    struct UserInfo *min;
    int proc;
    int i, j, x, y, k;
    struct sembuf sop_v;
    struct sembuf sop_p;
    struct sembuf sop_w;
    
    /* Se alive == 0 vuol dire che questa routine viene chiamata dal print finale, stampo quindi tutti i processi */  
    if (user_count > PRINT_PROCESS_LIMIT && alive == 1) {

        /* Blocco il semaforo di lettura per evitare che mi venga sovrascritta la shared memory mentre leggo */
        SEMBUF_V(sop_v, 0, 0);
        semop(sem_sync_id, &sop_v, 1);

        /* Debug */
        /* printf("[Master] Iniziato tentativo stampa\n");*/

        /* Attendo che eventuali scritture siano terminate per poter leggere */
        sop_w.sem_num = 1, sop_w.sem_op = 0, sop_w.sem_flg = 0;
        semop(sem_sync_id, &sop_w, 1);

        /* Debug */
        /* printf("[Master] Iniziata preparazione stampa\n");*/

        proc = PRINT_PROCESS_LIMIT/2;
        max= malloc(sizeof(struct UserInfo)*proc);
        min= malloc(sizeof(struct UserInfo)*proc);
        
        /* Stampa processi più significativi*/
        for (j = 0; j < user_count; j++) {


            /* Per i primi {proc} cicli gli array non sono ancora pieni, li popola con i valori di default e passa all'iterazione successiva */
            if (j < proc) {
                max[j] = min[j] = user_info[j];
                continue;
            }

            /* 
                Ottiene dall'array temporaneo degli utenti la posizione dei rispettivi processi con budget più alto e più basso
                Sostituisce poi in quell'indice l'utente con budget più basso (nell'array max) e più alto (nell'array min),
                così da mantenere negli array locali gli utenti con i budget più significativi
            */
            x = get_min_idx_from_arr(max,proc); 
            y = get_max_idx_from_arr(min,proc);

            if (user_info[j].budget > max[x].budget)
                max[x] = user_info[j];

            if(user_info[j].budget < min[y].budget)
                min[y] = user_info[j];
        };

        /* Sblocco la lettura */
        SEMBUF_P(sop_p, 0, 0);
        semop(sem_sync_id, &sop_p, 1);

        /* Debug */
        /*printf("[Master] Terminata preparazione stampa\n");*/
        printf("\n *** %d Processi con budget più alto \n", proc);
        for (j = 0; j < proc; j++)
        {
            print_user(max[j]);
        }
        free(max);
        printf("\n *** %d Processi con budget più basso \n", proc);
        for (j = 0; j < proc; j++)
        {
            print_user(min[j]);
        }
        free(min);
        

    } else {

        /*Stampa tutti i processi */
        for (i = 0; i < user_count; i++)
        {
            if (user_info[i].p_id != 0) {
                print_user(user_info[i]);
            }
        }
    }
    printf("\n");
    
    /* I nodi vengono stampati sempre tutti */
    for (k = 0; k < node_count; k++)
    {
        if (nodes_info[k].p_id != 0) {
            print_node(nodes_info[k]);
        }
    }
    printf("\n\n");
 
}

/* Retituisce l'indice dell'utente con budget più basso */
int get_min_idx_from_arr(struct UserInfo *usercount,int n){
    int i, min_idx;
    
    i = 1;
    min_idx = 0;
    while(i < n){
        if(usercount[i].budget < usercount[min_idx].budget)
            min_idx = i;
        i++;
    }
    return min_idx;
}

/* Retituisce l'indice dell'utente con budget più alto */
int get_max_idx_from_arr(struct UserInfo *usercount, int n){
    int i, max_idx;
    
    i = 1;
    max_idx = 0;
    while(i < n){
        if(usercount[i].budget > usercount[max_idx].budget)
            max_idx = i;
        i++;
    }
    return max_idx;
}

/* Manda un segnale a tutti i nodi vivi */
void kill_nodes(int node_count) {
    int i;
    for (i = 0; i < node_count; i++)
    {
        if(nodes[i] != 0) {
            kill(nodes[i], SIGTERM );
        }
        
    }
}

/* Manda un segnale a tutti gli utenti vivi */
void kill_users(int user_count) {
    int i;
    for(i = 0; i < user_count; i++)
    {
        if(users[i] != 0) {
            kill(users[i], SIGTERM );
        }
    }
}

/* Controlla che ci siano tutti i file per l'esecuzione */
int check_files(){
    if(access("./node",F_OK)<0){
        printf("Errore nell'accesso al fine node:%s\n",strerror(errno));
        return 0;
    }else if(access("./user",F_OK)<0){
        printf("Errore nell'accesso al fine user:%s\n",strerror(errno));
        return 0;
    }else if(access("./setting.conf",F_OK)<0){
        printf("Errore nell'accesso al fine setting.conf:%s\n",strerror(errno));
        return 0;
    }else{return 1;}
}

void print_user(struct UserInfo u) {
    printf("[USER: %d] Budget:%d, Status:%s \n",
                    u.p_id,
                    u.budget,
                    u.status == 1 ? "Alive" : "Dead"
                );
}

void print_node(struct NodeInfo n) {
    printf("[NODE: %d] Pool size:%d, Budget:%d, Status:%s \n",
            n.p_id,
            n.trans_pool_size,
            n.budget,
            n.status == 1 ? "Alive" : "Dead"
        );
}

void print_final_review(int user_count, int node_count, int aliveUser) {

    printf("\n\n ***** RIEPILOGO FINALE **** \n\n");

    switch (exitStatus)
    {
        case 0:
            printf("- Main terminato per aver raggiunto il tempo limite\n");
            break;

        case 1:
            printf("- Main terminato per aver premuto ctrl ^ c\n");
            break;
        
        case 2:
            printf("- Main terminato perchè tutti gli utenti sono morti\n");
            break;

        case 3:
            printf("- Main terminato perchè il libro mastro ha raggiunto il limite\n");

        case 4:
            printf("- Main terminato perchè uno dei processi iniziali non è stato creato correttamente\n");
        default:
            break;
    }
  
    printf("- Utenti terminati prematuramente: %d\n", aliveUser);
    printf("- Numero di blocchi nel libro mastro: %d\n\n", shd_ledger_info ->block_count);

    /* Se è morto per errore fork non serve stampare i processi*/
    if (exitStatus != 4) {
        print_summary(user_count, node_count);
    }

    /* Debug */
    /* print_budget(user_count); */
    
    /* Debug */
    /*print_all_blocks();*/
}

void send_friends(struct Options *conf){
    struct MsgFriend buff;
    int i,k,last;
    int *list;
    list = malloc(sizeof(int)*conf->SO_FRIENDS_NUM);
    i=0;
    last=0;
    while(i<conf->SO_NODES_NUM){
        buff.type=(long)nodes_info[i].p_id;
        /* Debug */
        /*printf("[MASTER] Genero amici per il nodo:%i\n",buff.id);*/
        last=generate_init_friends(conf,list,buff.type,last);
        for(k=0;k < conf->SO_FRIENDS_NUM;k++){
            buff.friend_pid=list[k];
            msgsnd(msg_queue_friends_id,&buff,sizeof(struct MsgFriend),0);
        }
        i++;
    }
    free(list);
}

int generate_init_friends(struct Options *conf,int *pnt,pid_t avoid,int ret){
    int c;
    c = 0;
    if(conf->SO_NODES_NUM>1){
        /*printf("[MASTER:%i] Last indx:%i\n",avoid,ret);*/
        while(c<conf->SO_FRIENDS_NUM){
            if(nodes_info[ret%=conf->SO_NODES_NUM].p_id!=avoid){
                pnt[c]=nodes_info[ret].p_id;
                /*printf("[MASTER:%i] Added friend:%i\n",avoid,pnt[c]);*/
                c++;
            }
            ret++;
        }
        return ret;
    }else{
        for(c=0;c<conf->SO_FRIENDS_NUM;c++){
            pnt[c]=avoid;
        }
    }
}

void print_budget(int user_count) {
    int i, j, user, pid, sum, tot;
    struct Message mex;
    int s;
    /* int sum_queue */

    tot = 0;
    
    for (user = 0; user < user_count; user++) {
        pid = user_info[user].p_id;
        sum = 0;
        for (i = user_info[user].last_block_read + 1; i < shd_ledger_info->block_count; i++) {
            for (j = 0; j < SO_BLOCK_SIZE; j++)  {
                if (shd_block[i].Transaction[j].receiver == pid) {
                    sum += shd_block[i].Transaction[j].quantity;
                }
            }
        };
        if (sum > 0) {
            printf("Budget riservato a [User: %d] ma non letto: %d\n", pid, sum);
            tot += sum;
        }
    }
    printf("Budget non letto totale: %d\n",  tot);

    /* 
    sum_queue = 0;
    for (user = 0; user < user_count; user++) {
        pid = user_info[user].p_id;
        do
        {
            s = msgrcv(msg_queue_rejected_id, &mex, sizeof(struct Message), (long)pid, IPC_NOWAIT);
            if (s == -1) {
                s = msgrcv(msg_queue_id, &mex, sizeof(struct Message), (long)pid, IPC_NOWAIT);
            }
            if (s != -1) {
                sum_queue += mex.trans.quantity + mex.trans.reward; 
            };
        } while (s != -1);
    }
    printf("Budget presente nelle code, non ancora elaborato: %d\n",  sum_queue);
    */
}

int create_process(char *path, char **conf, int is_node ) {
    int f;
    f = fork();
        switch(f){
            case -1:
                printf("errore nella fork %s\n",strerror(errno));
                return -1;
                break;

            case 0:
                execv(path, conf);
                
                /*Debug*/
                /* if (is_node == 1) {
                    printf("[Nodo: %d] esecuzione fork fallita:%s\n",getpid(), strerror(errno));
                } else {
                    printf("[User: %d] esecuzione fork fallita:%s\n",getpid(), strerror(errno));
                } */

                /* 
                    Il processo è stato creato con la fork, ma non è riuscito ad eseguire l'execv.
                    Se è tra i primi notifico il main che non è riuscito
                    Nota: gli user sono sempre tra i primi, perchè non vengono creati a runtime su richiesta come i nodi
                */
                if (is_node == 0) {
                    kill(getppid(), SIGUSR2);
                } else {
                    if ( atoi(conf[5]) == 1) {
                        kill(getppid(), SIGUSR2);
                    };
                }
                
                /*Debug*/
                /* if (is_node == 1) {
                    printf("[Nodo: %d] Exit\n",getpid());
                } else {
                    printf("[User: %d] Exit\n",getpid());
                } */
                exit(1);
                break;

            default:;
                return f;
                break;
        }
}

/* Estrae dei valori randomici da {from}, e li setta in {result} */
int get_random_int(int * from, int from_length, int * result, int result_length) {

    int i, pos, tmp;
    int * source;
    
    /* Crea il source per non manipolare direttamente l'array in ingresso */
    source = malloc(sizeof(int) * from_length);
    for (i = 0; i < from_length; i++) {
        source[i] = from[i];
    };
    
    for (i = 0; i < result_length; i++) {
        /* Estrae la posizione random */
        pos = rand() % from_length;

        /* Swappa la posizione randomica portandola alla fine */
        tmp = source[pos];
        source[pos] = source[from_length - 1];
        source[from_length - 1] = tmp;

        /* Decrementa il valore max di range*/
        from_length--;

        /* Assegna il random nell array di ritorno */
        result[i] = tmp;
    };

    free(source);
    return 1;
}