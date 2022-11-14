#define _XOPEN_SOURCE 700

#include "ledger.h"

int nsleep(long time) {
    struct timespec timeout;
    timeout.tv_sec = time / 1000000000L; 
    timeout.tv_nsec =  time % 1000000000L;
    /* Debug */
    /* printf("%d Nanosleep sta per essere eseguita:%li\n",getpid(), timeout.tv_nsec); */
    return nanosleep(&timeout,NULL);
}


void print_transazione(struct Transaction a){
    printf("***T R A N S A Z I O N E ***\ntimestamp_sec:%lu\ntimestamp_nsec:%lu\nsender:%i\nreceiver:%i\nquantity:%i\nreward:%i\n***      ***\n",a.timestamp_sec,a.timestamp_nsec,a.sender,a.receiver,a.quantity,a.reward);
}

struct Transaction build_transaction(pid_t sender,pid_t receiver,int quantity,int reward){
    struct timespec time;
    struct Transaction tmp; 
    clock_gettime( CLOCK_REALTIME, &time);
    
    tmp.timestamp_sec=time.tv_sec;
    tmp.timestamp_nsec=time.tv_nsec;
    tmp.sender=sender;
    tmp.receiver=receiver;
    tmp.quantity=quantity;
    tmp.reward=reward;
    
    return tmp; 
}

int get_reward(int quant,float p_rew){
    double res = quant*(p_rew/100);
    int ret=round(res);
    return (ret<1) ? 1 : ret;
}

int rand_int_range(int min,int max){
    return(rand()%(max-min+1))+min;
}

long int rand_long_range(long int min,long int max){
    return (rand()%(max-min+1)+min);
}

long int ret_time_nsec(){
    struct timespec time;
    clock_gettime( CLOCK_REALTIME, &time);
    return time.tv_nsec;
}