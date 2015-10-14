#ifndef _CRAWLER_H_
#define _CRAWLER_H_

#include "DNSManager.h"
#include "Parse.h"
#include "Url.h"
#include "config.h"
#include "dbg.h"
#include <assert.h>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <netdb.h>      /* struct hostent, gethostbyname */
#include <netinet/in.h> /* struct sockaddr_in, struct sockaddr */
#include <queue>
#include <set>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h> /* socket, connect */
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <sys/epoll.h>
#elif __APPLE__
#include <sys/event.h> /* for kqueue */
#include <sys/time.h> /* for kqueue */
#include <sys/types.h> /* for kqueue */
#endif

#define MAXEVENTS   1024
#define MAXCONNS    500
#define MAXLINE     2048
#define SMALLLINE   64
#define LINESIZE    256
#define HTMLSIZE    524288

#define EPOLLTIMEOUT    1000    //1s

// how many users returned by requesting followee/er ajax
#define USERSPERREQ      20

#define EEOF        -10

using namespace std;

class XCrawler
{
public:
    XCrawler();
    ~XCrawler();

    void start();

    // This method is called in a thread start_routine.
    void fetch();

private:
    int epfd;

#ifdef __linux__
    struct epoll_event *events;
#elif __APPLE__
    struct kevent *events;
#endif
    int curConns;

    struct CrawlerState {
        int iFd;
        int iState;
        char base[SMALLLINE];
        int iLen;
        char htmlBody[HTMLSIZE];
        int iLast;
        
        // need iFolloweeCount times request
        int iFolloweeCount;
        int iFolloweeCur;

        int iFollowerCount;
        int iFollowerCur;

        // hash_id and _xrsf for ajax
        char hashId[LINESIZE];
        int iHashIdSize;
        char xsrf[LINESIZE];
        int iXsrfSize;
    };

private:
    void init();
    void init_epoll();


    //int fetch_url_and_make_connection(int *pFd, string &sUrl);
    int fetch_url(string &sUrl);
    int make_connection(int *pFd);
    int prepare_get_answer_request(char *pReq, int *pSize, string &sUrl);
    int prepare_get_followers_request(char *pReq, int *pSize, string &sUrl, int iCur, CrawlerState *pState);
    int prepare_get_followees_request(char *pReq, int *pSize, string &sUrl, int iCur, CrawlerState *pState);
    int get_response(CrawlerState *pState);
    int make_socket_non_blocking(int fd);
    void push_urls(vector<string> &vFollows);
    int is_valid_html(char *pHtml, int iSize);
};

#endif
