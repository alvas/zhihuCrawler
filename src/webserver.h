#ifndef _WEBSERVER_H
#define _WEBSERVER_H

#include "config.h"
#include "webserver.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LISTENQ     1024

/**
 * This code is used to monitor the status of the crawler
 * open your web brower,and visit http://127.0.0.1:8080
 * you will see the statistics.
 */

class WebServer {
public:
    WebServer();
    ~WebServer();
    void run();

private:
    static void *startWebServer(void *arg);
    int sendHtml(int);

private:
    struct timeval startTime;   
};

#endif
