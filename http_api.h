#pragma once
#include <pthread.h>
#include "config.h"
#include "pipeline.h"

typedef struct {
    CONFIG_S   *cfg;
    LDRT_API_S *api;
    pthread_mutex_t lock;
    int         server_fd;
    pthread_t   thread;
    volatile int running;
} HTTP_API_S;

int  http_api_start(HTTP_API_S *h);
void http_api_stop(HTTP_API_S *h);
