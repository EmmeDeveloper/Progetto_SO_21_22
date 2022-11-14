#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>

#include "config.h"


int CONFIG_SHM_ID = 0;

unsigned long _hasher(char *str)
    {
        unsigned long hash = 5381;
        int c;

        while (c = *str++)
            hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

        return hash;
    }

struct Options *init_config(char* file){
    FILE* fp;
    struct Options *config;
    key_t shm_key;
    short empty;
    /* Buffers */
    long int value;
    char head[100],body[100];
    /* Generazione Memoria Condivisa */
    shm_key=CONFIG_STRUCT_SHARD_KEY;
    CONFIG_SHM_ID = shmget(shm_key, sizeof(struct Options), IPC_CREAT | 0666);

    empty = 0;

    if (CONFIG_SHM_ID < 0) {
        printf("Generazione shm_id fallita:%s\n",strerror(errno));
        exit(1);
    } 
    /* Attach della shared memory */
    config = (struct Options *) shmat(CONFIG_SHM_ID, NULL, 0);
    /* Apertura file */
    if((fp=fopen(file,"r"))==NULL){
        printf("Errore nell'apertura di setting.conf\n");
        return config;
    }
        while(1){
            /* Ricerca dell stringhe */
            
            if(empty == 0){
                fscanf(fp,"%s = %s",head,body);
            }else{
                fscanf(fp," = %s",body);
            }


            if(feof(fp)){
                break;
            }
            /* se una riga inizia con # la ingoriamo */
            if(head[0]==35||body[0]==35){
                while (fgetc(fp) != '\n'){}
                continue;
            }
            if(body[0]>=48 && body[0] <= 57){
                value = atol(body);
                empty = 0;
            }else if(body[0]==83 && body[1]==79 && body[2]==95){
                /*Debug*/
                /*printf("Valore mancante\n");*/
                empty = 1;
                value = 0;
                strcpy(head, body);
            }else{
                continue;
            }
        
            switch(_hasher(head)){
                case 14116381985149035527UL:
                    config->SO_USERS_NUM=value;
                    break; 
                case 14116371968014707854UL:
                    config->SO_NODES_NUM=value;
                    break; 
                case 6573159355789173268UL:
                    config->SO_BUDGET_INIT=value;
                    break; 
                case 249860422322806187UL:
                    config->SO_REWARD=value;
                    break; 
                case 1473633556864766770UL:
                    config->SO_MIN_TRANS_GEN_NSEC=value;
                    break; 
                case 13029323854605549044UL:
                    config->SO_MAX_TRANS_GEN_NSEC=value;
                    break; 
                case 7571527949173244UL:
                    config->SO_RETRY=value;
                    break; 
                case 8245393939676154660UL:
                    config->SO_TP_SIZE=value;
                    break;  
                case 11736432441051196364UL:
                    config->SO_MIN_TRANS_PROC_NSEC=value;
                    break; 
                case 5692586718596427470UL:
                    config->SO_MAX_TRANS_PROC_NSEC=value;
                    break; 
                case 8245393938089833801UL:
                    config->SO_SIM_SEC=value;
                    break; 
                case 6579153395066835840UL:
                    config->SO_FRIENDS_NUM=value;
                    break; 
                case 229440240535424UL:
                    config->SO_HOPS=value;
                    break; 
            }
        }
    return config;
}

void print_config(struct Options *config){
    printf("\n\n\
***** CONF *****\n\
so_users_num:%i\n\
so_nodes_num:%i\n\
so_budget_init:%i\n\
so_reward:%i\n\
so_min_trans_gen_nsec:%ld\n\
so_max_trans_gen_nsec:%ld\n\
so_retry:%i\n\
so_tp_size:%i\n\
so_block_size:%i\n\
so_min_trans_proc_nsec:%ld\n\
so_max_trans_proc_nsec:%ld\n\
so_registry_size:%i\n\
so_sim_sec:%i\n\
so_friends_num:%i\n\
so_hops:%i\n\
***** END CONF *****\n\n",
        config->SO_USERS_NUM,config->SO_NODES_NUM,config->SO_BUDGET_INIT,config->SO_REWARD,config->SO_MIN_TRANS_GEN_NSEC,config->SO_MAX_TRANS_GEN_NSEC,config->SO_RETRY,config->SO_TP_SIZE,SO_BLOCK_SIZE,config->SO_MIN_TRANS_PROC_NSEC,config->SO_MAX_TRANS_PROC_NSEC,SO_REGISTRY_SIZE,config->SO_SIM_SEC,config->SO_FRIENDS_NUM,config->SO_HOPS
    );
}
