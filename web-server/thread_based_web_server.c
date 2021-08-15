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
#include <stdbool.h>
#include <time.h>

#define handle_error(msg) \
    do { \
        perror(msg); \
        exit(EXIT_FAILURE); \
    } while(0)


#define PORT 1314
#define MAX_BACKLOG    1024
#define WORKER_THREAD_NUM 10
#define BUF_SIZE 1024
#define CRLF "\r\n"

typedef struct http_req_header {  /* interested http header field, feel free to add */
    char *http_ver;
    _Bool keep_conn;
    char *path;
    char method[8];
} HTTP_Req_Header;

typedef struct http_res_header {  /* interested http header field, feel free to add */
    char *http_ver;
    char *status_code;
    char *status;
    char *content_type;
    char *content_length;
    char *date;
    char *server;
} HTTP_Res_Header;

typedef union http_header {
    HTTP_Req_Header *req_header;
    HTTP_Res_Header *res_header;
} HTTP_Header;

typedef union http_payload {
    char *payload;
} HTTP_Payload;

typedef struct http_buf {
  char *val;
  size_t capacity; /* the allocated size */
  size_t size;     /* the already used size */
  HTTP_Header header;
  HTTP_Payload payload;
} HTTP_Buf;
static int hbuf_init(HTTP_Buf *buf, unsigned cap_bits);
static int hbuf_empty(HTTP_Buf *buf);
static int hbuf_parse_http_req_header(HTTP_Buf *buf);
static int hbuf_gen_http_res(HTTP_Buf *res, HTTP_Buf *req);
static int hbuf_putc(HTTP_Buf *buf, char c);
static int hbuf_putn(HTTP_Buf *buf, char *src, size_t n);
static int hbuf_puts(HTTP_Buf *buf, char *src);
static char *hbuf_flush(HTTP_Buf *buf);

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

static char *status_code_2_status(const char *status_code);

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

static int hbuf_init(HTTP_Buf *buf, unsigned cap_bits){
  const size_t capacity = 1u << cap_bits;
  buf->val = malloc(capacity);
  if(!buf->val) 
    handle_error("hbuf_init");
  *(buf->val) = 0;
  buf->capacity = capacity;
  buf->size = 0;
  return 0;
}

static int hbuf_putc(HTTP_Buf *buf, char c){
  if(buf->size == buf->capacity){
    const size_t new_capacity = buf->capacity << 1u;
    void *tmp = realloc(buf->val, new_capacity);
    if(!tmp)
        handle_error("hbuf_putc");
    buf->val = tmp;
    buf->capacity = new_capacity;
  }
  buf->val[buf->size++] = c;
  return 0;
}

static int hbuf_putn(HTTP_Buf *buf, char *src, size_t n){
  const size_t capacity = buf->capacity;
  const size_t size = buf->size;
  const size_t balance = capacity - size;
  const size_t extra_need = (balance < n) ? (n - balance) : 0;

  if(extra_need > 0){
    const size_t total_need = capacity + extra_need;
    size_t new_capacity = capacity;
    do
      new_capacity <<= 1;
    while(new_capacity < total_need);

    void *tmp = realloc(buf->val, new_capacity);
    if(!tmp) 
        handle_error("hbuf_putn");
    buf->val = tmp;
    buf->capacity = new_capacity;
  }

  memcpy(buf->val + size, src, n);
  buf->size += n;
  return 0;
}

static int hbuf_puts(HTTP_Buf *buf, char *src){
  hbuf_putn(buf, src, strlen(src));
}

static char *hbuf_flush(HTTP_Buf *buf){
  size_t size = buf->size;

  if(0 == buf->size || buf->val[buf->size - 1] != '\0'){
    hbuf_putc(buf, '\0');
    size++;
  }

  char *ret = strdup(realloc(buf->val, size)); /* using strdup since realloc may remain the same region of memory */
  if(!ret)
    handle_error("hbuf_flush");

  free(buf->val);
  if(buf->header.req_header){
      free(buf->header.req_header->http_ver);
      free(buf->header.req_header->path);
  } else if(buf->header.res_header){
      free(buf->header.res_header->http_ver);
      free(buf->header.res_header->status_code);
      free(buf->header.res_header->status);
      free(buf->header.res_header->content_type);
      free(buf->header.res_header->date);
      free(buf->header.res_header->server);
  }
  return ret;
}

static int hbuf_empty(HTTP_Buf *buf){
    *(buf->val) = '\0';
    buf->size = 0;
    return 0;
}

static void *worker_start_routine(void *arg){
    WorkQueue *wq = (WorkQueue *) arg;
    Work *work;
    HTTP_Buf hreq, hres;
    char buf[BUF_SIZE];
    ssize_t recv_bytes;

    hbuf_init(&hreq, 10); /* HTTP request */
    hbuf_init(&hres, 12); /* HTTP response */
    while(1){
        wq_dequeue(wq, &work);

        hbuf_empty(&hreq);
        hbuf_empty(&hres);
        recv_bytes = 0;
        /**
         *  recv_bytes could be 0 in 2 situation:
         *  (1) no more data
         *  (2) peer stream socket shutdown orderly
         *  so, we should determine have data first(checking if reach CRLF) 
         *  before calling recv()
        */
        while(!strstr(hreq.val, CRLF CRLF)){
            recv_bytes = recv(work->connfd, buf, BUF_SIZE, 0);
            if(recv_bytes <= 0){
                if(recv_bytes == -1)
                    handle_error("recv");
                if(recv_bytes == 0){      /* peer socket has shutdown orderly */
                    work_destroy(work);
                    goto next_work;
                }
            }
            hbuf_puts(&hreq, buf);
        }
        // hbuf_parse_http_req_header(&hreq);
        // printf("version: %s\nmethod: %s\nconn: %s\npath: %s\n", hreq.header.req_header->http_ver, 
        //                                                         hreq.header.req_header->method,
        //                                                         hreq.header.req_header->keep_conn ? "keep" : "close",
        //                                                         hreq.header.req_header->path);

        /* send http response to client */
        hbuf_gen_http_res(&hres, &hreq);
        char *http_res = hbuf_flush(&hres);
        if(send(work->connfd, http_res, hres.size, 0) == -1)
            handle_error("send");
        if(hreq.header.req_header->keep_conn)
            wq_enqueue(wq, work);
        else 
            work_destroy(work);
        free(http_res);
    next_work:;
    }

    return NULL;
}

static int hbuf_parse_http_req_header(HTTP_Buf *buf){
    buf->header.req_header = malloc(sizeof(HTTP_Req_Header));
    if(!buf->header.req_header)
        handle_error("malloc HTTP_Req_Header");
    memset(buf->header.res_header, 0, sizeof(HTTP_Req_Header));
    
    char *ptr = buf->val;
    size_t len;

    /* get HTTP method */
    char *space = strchr(ptr, ' ');
    len = space - ptr;
    memcpy(buf->header.req_header->method, ptr, len);
    buf->header.req_header->method[len] = 0;

    /* get HTTP path */
    ptr += (len + 1);
    space = strchr(ptr, ' ');
    len = space - ptr;
    buf->header.req_header->path = malloc(len + 1);
    if(!buf->header.req_header->path)
        handle_error("malloc http req path");
    memcpy(buf->header.req_header->path, ptr, len);
    buf->header.req_header->path[len] = 0;

    /* get HTTP version */
    ptr += (len + 1);
    space = strstr(ptr, CRLF);
    len = space - ptr;
    buf->header.req_header->http_ver = malloc(len + 1);
    if(!buf->header.req_header->path)
        handle_error("malloc http req version");
    memcpy(buf->header.req_header->http_ver, ptr, len);
    buf->header.req_header->http_ver[len] = 0;

    /* get HTTP connection */
    char *keep_conn = strstr(ptr, "keep-alive");
    if(!keep_conn)  /* default is close */
        buf->header.req_header->keep_conn = false;   
    else
        buf->header.req_header->keep_conn = true;
    
    return 0;
}

static int hbuf_gen_http_res(HTTP_Buf *res, HTTP_Buf *req){
    hbuf_parse_http_req_header(req);

    char *req_ver = req->header.req_header->http_ver;
    char *req_method = req->header.req_header->method;
    char *req_path = req->header.req_header->path;

    /* http response header field */
    res->header.res_header = malloc(sizeof(HTTP_Res_Header));
    if(!res->header.res_header)
        handle_error("malloc HTTP_Res_Header hbuf_gen_http_res");
    memset(res->header.res_header, 0, sizeof(HTTP_Res_Header));
    res->header.res_header->http_ver = strdup(req_ver);
    if(!res->header.res_header->http_ver)
        handle_error("strdup req_method hbuf_gen_http_res");
    res->header.res_header->content_type = strdup("text/html; charset=utf8");
    if(!res->header.res_header->content_type)
        handle_error("strdup content_type hbuf_gen_http_res");
    res->header.res_header->server = strdup("Cool Server");
    if(!res->header.res_header->server)
        handle_error("strdup server hbuf_gen_http_res");
    res->header.res_header->date = malloc(16);             /* fmt: %m/%d/%y */
    if(!res->header.res_header->date)
        handle_error("malloc date hbuf_gen_http_res");
    time_t t;
    t = time(NULL);
    if(t == ((time_t) -1))
        handle_error("time(NULL) hbuf_gen_http_res");
    struct tm *gmp;
    gmp = gmtime(&t);
    if(!gmp)
        handle_error("gmtime hbuf_gen_http_res");
    if(strftime(res->header.res_header->date, 16, "%m/%d/%y", gmp) == 0)
        handle_error("strftime date hbuf_gen_http_res");

    int fd;
    struct stat sb;
    char pathname[1024] = "./public";
    const char path_404[] = "./public/404.html";
    char *buf;

    if(strlen(req_path) == 1)
        strcat(pathname, "/index.html"); /* default page */
    else
        strcat(pathname, req_path);
    if(strcmp(req_method, "GET") == 0){
        fd = open(pathname, O_RDONLY);
        if(fd == -1 && errno == ENOENT){
            res->header.res_header->status_code = strdup("404");
            res->header.res_header->status = status_code_2_status(res->header.res_header->status_code);
            fd = open(path_404, O_RDONLY);
            if(fd == -1)
                handle_error("open 404.html");
        } else {
            res->header.res_header->status_code = strdup("200");
            res->header.res_header->status = status_code_2_status(res->header.res_header->status_code);
        }
        if(fstat(fd, &sb) == -1)
            handle_error("fstat hbuf_gen_http_res");
        buf = malloc(sb.st_size);
        if(!buf)
            handle_error("malloc buf hbuf_gen_http_res");
        
        res->header.res_header->content_length = malloc(128);
        if(!res->header.res_header->content_length)
            handle_error("malloc content length hbuf_gen_http_res");
        sprintf(res->header.res_header->content_length, "%ld", sb.st_size);

        if(read(fd, buf, sb.st_size) == -1)
            handle_error("read hbuf_gen_http_res");

        close(fd);
    } else if(strcmp(req_method, "POST") == 0){
        fd = open(pathname, O_WRONLY);
    }

    // printf("server: %s\n", res->header.res_header->server);
    // printf("version: %s\n", res->header.res_header->http_ver);
    // printf("status: %s\n", res->header.res_header->status);
    // printf("status_code: %s\n", res->header.res_header->status_code);
    // printf("date: %s\n", res->header.res_header->date);

    /* build http response */
    hbuf_puts(res, res->header.res_header->http_ver);
    hbuf_putc(res, ' ');
    hbuf_puts(res, res->header.res_header->status_code);
    hbuf_putc(res, ' ');
    hbuf_puts(res, res->header.res_header->status);
    hbuf_puts(res, CRLF);
    hbuf_puts(res, "Content-Type: ");
    hbuf_puts(res, res->header.res_header->content_type);
    hbuf_puts(res, CRLF);
    hbuf_puts(res, "Date: ");
    hbuf_puts(res, res->header.res_header->date);
    hbuf_puts(res, CRLF);
    hbuf_puts(res, "Server: ");
    hbuf_puts(res, res->header.res_header->server);
    hbuf_puts(res, CRLF);
    hbuf_puts(res, "Content-Length: ");
    hbuf_puts(res, res->header.res_header->content_length);
    hbuf_puts(res, CRLF CRLF);
    hbuf_putn(res, buf, sb.st_size);
    return 0;
}

static char *status_code_2_status(const char *status_code){
    if(strcmp(status_code, "200") == 0)
        return strdup("OK");
    else if(strcmp(status_code, "404") == 0)
        return strdup("NOT FOUND");
}