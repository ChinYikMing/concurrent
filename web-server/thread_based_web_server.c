#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define handle_error(msg) \
    do { \
        perror(msg); \
        exit(EXIT_FAILURE); \
    } while(0)


#define PORT 1314
#define MAX_BACKLOG    1024
#define WORKER_THREAD_NUM 10
#define MAX_REQUEST_SIZE 1024

typedef struct work Work;
struct work {         /* one connection is a Work */
    int connfd;
    Work *next;
};
static Work *work_create(int connfd);
static int work_destroy(Work *work);

typedef struct work_queue {
    Work *head, *tail;
    pthread_cond_t *has_work; /* main thread make sure has work then dispatch worker thread */
    pthread_mutex_t *head_mtx, *tail_mtx; 
} WorkQueue;
static int wq_init(WorkQueue *wq);
static int wq_enqueue(WorkQueue *wq, Work *work);
static int wq_dequeue(WorkQueue *wq, Work **work);
static int wq_cond_update(WorkQueue *wq);
static int wq_destroy(WorkQueue *wq);

static void *worker_start_routine(void *arg);

int main(){
    WorkQueue wq;
    wq_init(&wq);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sfd == -1)
        handle_error("socket");
    
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(struct sockaddr_in));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(PORT);
    struct in_addr addr;
    if(inet_pton(AF_INET, "192.168.16.127", &addr) == -1)
        handle_error("inet_pton");
    sin.sin_addr = addr;

    if(bind(sfd, (struct sockaddr *) &sin, sizeof(struct sockaddr_in)) == -1)
        handle_error("bind");

    /* create worker thread before listening */
    /* detach worker thread */
    pthread_attr_t attr;
    if(pthread_attr_init(&attr) != 0)
        handle_error("pthread_attr_init");

    if(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
        handle_error("pthread_attr_setdetachstate");

    pthread_t thr;
    for(int i = 0; i < WORKER_THREAD_NUM; ++i)
        if(pthread_create(&thr, &attr, *((void *(*)(void *arg)) worker_start_routine), (void *) &wq) != 0)
            handle_error("pthread_create");

    /* after create worker thread, we can safely destroy the attribute */
    if(pthread_attr_destroy(&attr) != 0)
        handle_error("pthread_attr_destroy");

    if(listen(sfd, MAX_BACKLOG) == -1)
        handle_error("listen");
    printf("Listening on port %d...\n", PORT);

    Work *work;
    while(1){
        int connfd = accept(sfd, (struct sockaddr *) NULL, NULL);
        if(connfd == -1)
            handle_error("accept");

        work = work_create(connfd);
        wq_enqueue(&wq, work);
    }

    close(sfd);
    wq_destroy(&wq);
    exit(EXIT_SUCCESS);
}

static int wq_init(WorkQueue *wq){
    wq->head = wq->tail = NULL;
    wq->has_work = malloc(sizeof(pthread_cond_t));
    if(!wq->has_work)
        handle_error("malloc has_work conditional variable");
    wq->head_mtx = malloc(sizeof(pthread_mutex_t));
    if(!wq->head_mtx){
        free(wq->has_work);
        handle_error("malloc head mutex");
    }
    if(pthread_mutex_init(wq->head_mtx, NULL) != 0)
        handle_error("pthread_mutex_init head mutex");
    wq->tail_mtx = malloc(sizeof(pthread_mutex_t));
    if(!wq->tail_mtx){
        free(wq->has_work);
        free(wq->head_mtx);
        handle_error("malloc tail mutex");
    }
    if(pthread_mutex_init(wq->tail_mtx, NULL) != 0)
        handle_error("pthread_mutex_init tail mutex");

    return 0;
}

static int wq_enqueue(WorkQueue *wq, Work *work){
    pthread_mutex_lock(wq->tail_mtx);
    if(!wq->head && !wq->tail){
        wq->head = wq->tail = work;
    } else {
        wq->tail->next = work;
        wq->tail = work;
    }

    if(pthread_cond_signal(wq->has_work) != 0)  
        handle_error("pthread_cond_signal");
    pthread_mutex_unlock(wq->tail_mtx);
    return 0;
}

static int wq_dequeue(WorkQueue *wq, Work **work){
    pthread_mutex_lock(wq->head_mtx);
    while(!wq->head)
        pthread_cond_wait(wq->has_work, wq->head_mtx);
    if(wq->head == wq->tail){
        *work = wq->head;
        wq->head = wq->tail = NULL;
    } else {
        *work = wq->head;
        wq->head = wq->head->next;
    }
    pthread_mutex_unlock(wq->head_mtx);
}

static int wq_destroy(WorkQueue *wq){
    Work *head = wq->head;
    Work *tmp;
    while(head){
        tmp = head;
        head = head->next;
        free(tmp);
    }
    free(wq->has_work);
    if(pthread_mutex_destroy(wq->head_mtx) != 0)
        handle_error("pthread_mutex_destroy head mutex");
    free(wq->head_mtx);
    if(pthread_mutex_destroy(wq->tail_mtx) != 0)
        handle_error("pthread_mutex_destroy tail mutex");
    free(wq->tail_mtx);
    return 0;
}

static Work *work_create(int connfd){
    Work *work = malloc(sizeof(Work));
    if(!work)
        handle_error("malloc work");
    work->connfd = connfd;
    work->next = NULL;
    return work;
}

static int work_destroy(Work *work){
    close(work->connfd);
    return 0;
}

static void *worker_start_routine(void *arg){
    WorkQueue *wq = (WorkQueue *) arg;
    Work *work;
    char req[MAX_REQUEST_SIZE];
    memset(req, 0, sizeof(char[MAX_REQUEST_SIZE]));
    struct stat sb;
    int fd = open("./public/index.html", O_RDONLY);
    if(fd == -1)
        handle_error("open");
    fstat(fd, &sb);
    char res[sb.st_size];

    while(1){
        wq_dequeue(wq, &work);

        if(recv(work->connfd, req, MAX_REQUEST_SIZE, 0) == -1)
            handle_error("recv");

        if(read(fd, res, sb.st_size) == -1)
            handle_error("read");

        if(strncmp(req, "GET", 3) == 0)
            if(send(work->connfd, res, sb.st_size, 0) == -1)
                handle_error("send");

        work_destroy(work);
    }

    close(fd);
    return NULL;
}