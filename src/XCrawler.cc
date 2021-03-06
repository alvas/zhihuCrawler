#include "XCrawler.h"

/**
 * The most important class.
 * This class start thread, maintain priority queue, fetch web page,
 * add links to priority queue etc.
 * Note that currently we have not implemented all the futures provided here
 * 
 */

queue<string> unvisitedUrl;
set<string> visitedUrl;
DNSManager dnsMana;
ofstream fResultOut(RAW_DATA_FILE.c_str());

XCrawler::XCrawler(): curConns(0) {
    init();
    init_epoll();
}

XCrawler::~XCrawler() {
    close(epfd);
    free(events);
}

void XCrawler::init() {
    ifstream init_file;
    init_file.open(SEEDS_FILE.c_str(), ios::binary);

    if (!init_file) {
        log_err("open seed file");
        exit(0);
    }

    string strLine;

    while (getline(init_file, strLine)) {
        unvisitedUrl.push(strLine);
    }

    init_file.close();

    init_file.open(VISITED_URL_MD5_FILE.c_str(), ios::binary);

    if (!init_file) {
        exit(0);
    }

    while (getline(init_file, strLine)) {
        visitedUrl.insert(strLine);
    }

    init_file.close();
}

void XCrawler::init_epoll() {
#ifdef __linux__
    int fd = epoll_create1(0);
    check(fd > 0, "epoll_create");
    events = (struct epoll_event *)malloc(sizeof(struct epoll_event) * MAXEVENTS);
    epfd = fd;
#elif __APPLE__
    epfd = kqueue();
    check(epfd > 0, "kqueue_create");
    events = (struct kevent *)malloc(sizeof(struct kevent) * MAXEVENTS);
#endif
}

static void *run(void *arg)
{
    ((XCrawler *)arg)->fetch();
    return NULL;
}

void XCrawler::start()
{
    pthread_t tid;
    int error = pthread_create(&tid, NULL, run, this);

    if (error < 0) {
        perror("pthread_create");
        exit(0);
    }

    pthread_join(tid, NULL);
}

void XCrawler::fetch()
{
    string sUrl;
    int iFd = 0, size = 0, error = 0, n = 0, m = 0;
    char reqBuf[MAXLINE];

#ifdef __APPLE__
    vector<struct kevent> chlist;
#endif
    
    while (true) {
        do {
            if (curConns < MAXCONNS) {
                error = fetch_url(sUrl);

                if (error < 0) {
                    log_err("fetch_url");
                    break;
                }

                error = make_connection(&iFd);

                if (error < 0) {
                    log_err("make_connection");
                    break;
                }
                
                size = MAXLINE;
                error = prepare_get_answer_request(reqBuf, &size, sUrl);

                if (error < 0) {
                    log_err("prepare_get_answer_request");
                    break;
                }

                error = write(iFd, reqBuf, size);

                if (error < 0) {
                    log_err("write");
                    break;
                }

                CrawlerState *pState= (CrawlerState *)malloc(sizeof(CrawlerState));
                pState->iFd = iFd;
                pState->iState = 0;
                memcpy(pState->base, sUrl.c_str(), sUrl.size());
                pState->iLen = sUrl.size();
                pState->iLast = 0;

#ifdef __linux__
                struct epoll_event event;
                event.data.ptr = (void *)pState;
                event.events = EPOLLIN | EPOLLET;

                error = epoll_ctl(epfd, EPOLL_CTL_ADD, iFd, &event);
                check(error == 0, "epoll_add");
#elif __APPLE__
                struct kevent event;
                event.udata = (void *)pState;
                chlist.push_back(event);
                EV_SET(&(chlist[m]), iFd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
                chlist[m].udata = (void *)pState;
                m++;
#endif
                curConns++;
            }
        } while(0);
        
#ifdef __linux__
        n = epoll_wait(epfd, events, MAXEVENTS, EPOLLTIMEOUT);
        check(n >= 0, "epoll_wait");
#elif __APPLE__
        n = kevent(epfd, &chlist[0], chlist.size(), events, MAXEVENTS, NULL);

        m = 0;
        chlist.clear();

        if (n > MAXEVENTS)
        {
            log_err("Exceed maximum events");
            break;
        }
#endif

        for (int i = 0; i < n; i++) {
#ifdef __linux__
            CrawlerState *pState = (CrawlerState *)events[i].data.ptr;
#elif __APPLE__
            CrawlerState *pState = (CrawlerState *)events[i].udata;
#endif
            int iHeaderSize = MAXLINE;
            vector<string> vFollows;

            switch (pState->iState) {
                case 0:
                    error = get_response(pState);

                    if (error < 0) {
                        log_err("get_response");
                        break;
                    }
                    
                    // parse html
                    error = is_valid_html(pState->htmlBody, pState->iLast);

                    if (error != 0) {
                        break;
                    }

                    Parse::SearchAnswer(pState->htmlBody, pState->iLast, fResultOut);
                    error = Parse::GetFollowCount(pState->htmlBody, pState->iLast, &(pState->iFolloweeCount), &(pState->iFollowerCount));

                    if (error < 0) {
                        // ignore 429 err
                        close(pState->iFd);
                        break;
                    }

                    error = Parse::GetHashId(pState->htmlBody, pState->iLast, pState->hashId, &(pState->iHashIdSize));

                    if (error < 0) {
                        close(pState->iFd);
                        break;
                    }

                    error = Parse::GetXsrf(pState->htmlBody, pState->iLast, pState->xsrf, &(pState->iXsrfSize));

                    if (error < 0) {
                        close(pState->iFd);
                        break;
                    }

                    // how many request needed
                    pState->iFolloweeCount += (USERSPERREQ - 1);
                    pState->iFolloweeCount /= USERSPERREQ;
                    pState->iFolloweeCur = 0;

                    pState->iFollowerCount += (USERSPERREQ - 1);
                    pState->iFollowerCount /= USERSPERREQ;
                    pState->iFollowerCur = 0;

                    log_info("ee = %d, er = %d", pState->iFolloweeCount, pState->iFollowerCount);

                    pState->iState++;
                    pState->iLast = 0;
                    sUrl = string(pState->base, pState->iLen);

                    // add followers link to queue
                    error = prepare_get_followers_request(reqBuf, &iHeaderSize, sUrl, pState->iFollowerCur * USERSPERREQ, pState);

                    if (error < 0) {
                        log_err("prepare_get_followers_request");
                        break;
                    }
                    
                    error = write(pState->iFd, reqBuf, iHeaderSize);

                    if (error < 0) {
                        log_err("write");
                        break;
                    }

                    pState->iFollowerCur++;
                    break;

                case 1:
                    error = get_response(pState);

                    if (error < 0) {
                        log_err("get_response");

                        if (error == EEOF) {
                            error = make_connection(&(pState->iFd));

                            if (error < 0) {
                                log_err("and_make_connection");
                                break;
                            }

                            log_info("make connection suc!");

#ifdef __linux__
                            struct epoll_event event;
                            event.data.ptr = (void *)pState;
                            event.events = EPOLLIN | EPOLLET;

                            error = epoll_ctl(epfd, EPOLL_CTL_ADD, pState->iFd, &event);
                            check(error == 0, "epoll_add");
#elif __APPLE__
                            struct kevent event;
                            chlist.push_back(event);
                            EV_SET(&(chlist[m]), iFd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
                            chlist[m].udata = (void *)pState;
                            m++;
#endif

                            curConns++;
                        } else {
                            break;
                        }
                    } else {
                        error = is_valid_html(pState->htmlBody, pState->iLast);

                        if (error != 0) {
                            break;
                        }
                    }

                    Parse::SearchFollowers(pState->htmlBody, pState->iLast, vFollows);
                    push_urls(vFollows);

                    if (pState->iFollowerCur < pState->iFollowerCount) {
                        // more followers! need get followers again
                        log_info("more followers! need get followers again cur = %d, target = %d", pState->iFollowerCur, pState->iFollowerCount);
                        sUrl = string(pState->base, pState->iLen);

                        iHeaderSize = HTMLSIZE;
                        error = prepare_get_followers_request(reqBuf, &iHeaderSize, sUrl, pState->iFollowerCur * USERSPERREQ, pState);

                        if (error < 0) {
                            log_err("prepare_get_followers_request");
                            break;
                        }
                        
                        error = write(pState->iFd, reqBuf, iHeaderSize);

                        if (error < 0) {
                            log_err("write");
                            continue;
                        }

                        pState->iFollowerCur++;
                        pState->iLast = 0;
                        log_info("send get follower succ!");
                    } else {
                        log_info("complete!! get all followers of %.*s", pState->iLen, pState->base);

                        // go to next state

                        pState->iState++;
                        pState->iLast = 0;
                        sUrl = string(pState->base, pState->iLen);

                        error = prepare_get_followees_request(reqBuf, &iHeaderSize, sUrl, pState->iFolloweeCur, pState);

                        if (error < 0) {
                            log_err("prepare_get_followees_request");
                            break;
                        }
                        
                        error = write(pState->iFd, reqBuf, iHeaderSize);

                        if (error < 0) {
                            log_err("write");
                            break;
                        }
                        
                        pState->iFolloweeCur++;
                    }

                    break;

                case 2:
                    error = get_response(pState);

                    if (error < 0) {
                        log_err("get_response");

                        if (error == EEOF) {
                            error = make_connection(&(pState->iFd));

                            if (error < 0) {
                                log_err("and_make_connection");
                                break;
                            }

                            log_info("make connection suc!");

#ifdef __linux__
                            struct epoll_event event;
                            event.data.ptr = (void *)pState;
                            event.events = EPOLLIN | EPOLLET;

                            error = epoll_ctl(epfd, EPOLL_CTL_ADD, pState->iFd, &event);
                            check(error == 0, "epoll_add");
#elif __APPLE__
                            struct kevent event;
                            chlist.push_back(event);
                            EV_SET(&(chlist[m]), iFd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
                            chlist[m].udata = (void *)pState;
                            m++;
#endif

                            curConns++;
                        } else {
                            break;
                        }
                    } else {

                        error = is_valid_html(pState->htmlBody, pState->iLast);

                        if (error != 0) {
                            break;
                        }
                    }

                    Parse::SearchFollowers(pState->htmlBody, pState->iLast, vFollows);
                    push_urls(vFollows);

                    if (pState->iFolloweeCur < pState->iFolloweeCount) {
                        // more followers! need get followers again
                        log_info("more followees! need get followees again cur = %d, target = %d", pState->iFolloweeCur, pState->iFolloweeCount);
                        sUrl = string(pState->base, pState->iLen);

                        iHeaderSize = HTMLSIZE;
                        error = prepare_get_followees_request(reqBuf, &iHeaderSize, sUrl, pState->iFolloweeCur * USERSPERREQ, pState);

                        if (error < 0) {
                            log_err("prepare_get_followees_request");
                            break;
                        }
                        
                        error = write(pState->iFd, reqBuf, iHeaderSize);

                        if (error < 0) {
                            log_err("write");
                            continue;
                        }

                        pState->iFolloweeCur++;
                        pState->iLast = 0;
                        log_info("send get followee succ!");
                    } else {
                        log_info("find followee succ!!");

                        error = fetch_url(sUrl);

                        if (error < 0) {
                            log_err("fetch_url");
                            close(pState->iFd);
                            break;
                        }

                        size = MAXLINE;
                        error = prepare_get_answer_request(reqBuf, &size, sUrl);

                        if (error < 0) {
                            log_err("prepare_get_answer_request");
                            break;
                        }

                        error = write(iFd, reqBuf, size);

                        if (error < 0) {
                            log_err("write");
                            break;
                        }

                        pState->iState = 0;
                        pState->iLast = 0;
                        memcpy(pState->base, sUrl.c_str(), sUrl.size());
                        pState->iLen = sUrl.size();
                    }

                    break;

                default:
                    break;
            } 
        } //usleep(500000);
    } //end while
}

int XCrawler::is_valid_html(char *pHtml, int iSize) {
    char *pCLpos = NULL;
    pHtml[iSize] = '\0';

    if ((pCLpos = strstr(pHtml, "Content-Length: ")) == NULL) {
        return -1;
    }

    int iContentLen = atoi(pCLpos + strlen("Content-Length: "));
    cout << "Content-Length: " << iContentLen << endl;

    char *pCRLF = strstr(pHtml, "\r\n\r\n");

    if (pCRLF == NULL) {
        return -1;
    }

    int iTrueLen = pCRLF - pHtml + strlen("\r\n\r\n") + iContentLen;

    if (iSize < iTrueLen) {
        return -1;
    }
    else if (iSize > iTrueLen) {
        log_err("iSize = %d, iTrueLen = %d", iSize, iTrueLen);
    }

    return 0;
}

int XCrawler::get_response(CrawlerState *pState) {
    int iFd = pState->iFd, iLast = pState->iLast, error = 0, first = 1;

    while (1) {
        int nRead = read(iFd, pState->htmlBody + iLast, HTMLSIZE - iLast);

        if (nRead == 0) {
            // EOF
            if (first) {
                log_info("first loop read EOF");
            }
            else {
                log_info("not first loop rend EOF");
            }

            log_err("EOF");
            close(iFd);
            curConns--;
            return EEOF;
        }

        if (nRead < 0) {
            if (errno != EAGAIN) {
                log_err("read err, and errno = %d", errno);
                close(iFd);
                //free(pState);
                curConns--;
                return -1;
            }

            break;
        }
        
        iLast += nRead;
        first = 0;
    }

    pState->iLast = iLast;

    return error;
}

int XCrawler::fetch_url(string &sUrl) {
    while (1) {
        if(unvisitedUrl.empty()) {
            //sleep(1);
            cout << "thread " << (long)(pthread_self()) % THREAD_NUM << ": no data to process" << endl;
            return -1;
        }

        sUrl = unvisitedUrl.front();
        unvisitedUrl.pop();
        break;
    }

    log_info("fetch success! url = %s", sUrl.c_str());
    return 0;
}

int XCrawler::make_connection(int *pFd) {
    // make connection
    int iConnfd = socket(AF_INET, SOCK_STREAM, 0);

    if (iConnfd < 0) {
        log_err("socket");
        return -1;
    }

    //struct hostent *server;
    //string ip = dnsMana.getIP(url.getHost());
    string ip = "60.28.215.98";
    struct sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(80);
    servAddr.sin_addr.s_addr = inet_addr(ip.c_str());

    int error = connect(iConnfd, (struct sockaddr *)&servAddr, sizeof(servAddr));

    if (error < 0) {
        log_err("connect");
        return error;
    }

    error = make_socket_non_blocking(iConnfd);

    if (error < 0) {
        log_err("make_socket_non_blocking");
        return -1;
    }

    *pFd = iConnfd;

    return error;
}

int XCrawler::prepare_get_answer_request(char *pReq, int *pSize, string &sUrl) {
    Url url(sUrl);

    int error = snprintf(pReq, *pSize,
                        "GET %s/answers?order_by=vote_num HTTP/1.1\r\n"
                        "Host: www.zhihu.com\r\n"
                        "Connection: keep-alive\r\n"
                        "Cache-Control: max-age=0\r\n"
                        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n"
                        "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_10_5) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/44.0.2403.157 Safari/537.36\r\n"
                        "Accept-Language: zh-CN,zh;q=0.8\r\n"
                        "Cookie: %s\r\n"
                        "\r\n", url.getPath().c_str(), cookie.c_str());

    if (error < 0) {
        log_err("snprintf");
        return error;
    }

    *pSize = error;

    return 0;
}

int XCrawler::prepare_get_followers_request(char *pReq, int *pSize, string &sUrl, int iCur, CrawlerState *pState) {
    //Url url(sUrl);
    string hashId(pState->hashId, pState->iHashIdSize);
    string _xsrf(pState->xsrf, pState->iXsrfSize);

    ostringstream oss; 
    oss << "{\"offset\":" << iCur << ",\"order_by\":\"created\",\"hash_id\":\"" << hashId << "\"}";

    string sParams = oss.str();
    char postBody[MAXLINE];

    int error = snprintf(postBody, MAXLINE, 
                        "method=next&params=%s&_xsrf=%s",
                        Url::encode(sParams).c_str(), _xsrf.c_str());

    if (error < 0) {
        log_err("snprintf postBody");
        return error;
    }
    
    int iContentLen = error;
    postBody[iContentLen] = '\0';

    error = snprintf(pReq, *pSize,
                    "POST /node/ProfileFollowersListV2 HTTP/1.1\r\n"
                    "Host: www.zhihu.com\r\n"
                    "Connection: keep-alive\r\n"
                    "Content-Length: %d\r\n"
                    "Accept: */*\r\n"
                    "Origin: http://www.zhihu.com\r\n"
                    "X-Requested-With: XMLHttpRequest\r\n"
                    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_10_5) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/44.0.2403.157 Safari/537.36\r\n"
                    "Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n"
                    "Referer: %s/followers\r\n"
                    "Accept-Language: zh-CN,zh;q=0.8\r\n"
                    "Cookie: %s\r\n"
                    "\r\n"
                    "%s",
                    iContentLen, sUrl.c_str(), cookie.c_str(), postBody);

    if (error < 0) {
        log_err("snprintf");
        return error;
    }

    //printf("post followers:\n%.*s", error, pReq);
    *pSize = error;

    return 0;
}

int XCrawler::prepare_get_followees_request(char *pReq, int *pSize, string &sUrl, int iCur, CrawlerState *pState) {
    string hashId(pState->hashId, pState->iHashIdSize);
    string _xsrf(pState->xsrf, pState->iXsrfSize);

    ostringstream oss; 
    oss << "{\"offset\":" << iCur << ",\"order_by\":\"created\",\"hash_id\":\"" << hashId << "\"}";

    string sParams = oss.str();
    char postBody[MAXLINE];

    int error = snprintf(postBody, MAXLINE, 
                        "method=next&params=%s&_xsrf=%s",
                        Url::encode(sParams).c_str(), _xsrf.c_str());

    if (error < 0) {
        log_err("snprintf postBody");
        return error;
    }
    
    int iContentLen = error;
    postBody[iContentLen] = '\0';

    error = snprintf(pReq, *pSize,
                    "POST /node/ProfileFolloweesListV2 HTTP/1.1\r\n"
                    "Host: www.zhihu.com\r\n"
                    "Connection: keep-alive\r\n"
                    "Content-Length: %d\r\n"
                    "Accept: */*\r\n"
                    "Origin: http://www.zhihu.com\r\n"
                    "X-Requested-With: XMLHttpRequest\r\n"
                    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_10_5) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/44.0.2403.157 Safari/537.36\r\n"
                    "Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n"
                    "Referer: %s/followers\r\n"
                    "Accept-Language: zh-CN,zh;q=0.8\r\n"
                    "Cookie: %s\r\n"
                    "\r\n"
                    "%s",
                    iContentLen, sUrl.c_str(), cookie.c_str(), postBody);

    if (error < 0) {
        log_err("snprintf");
        return error;
    }

    //printf("post followees:\n%.*s", error, pReq);
    *pSize = error;

    return 0;
}

int XCrawler::make_socket_non_blocking(int fd) {
    // get the file access mode and the file status flags
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        log_err("fcntl");
        return -1;
    }

    flags |= O_NONBLOCK;

    // set the file status flags to O_NONBLOCK
    int s = fcntl(fd, F_SETFL, flags);

    if (s == -1) {
        log_err("fcntl");
        return -1;
    }

    return 0;
}

void XCrawler::push_urls(vector<string> &vFollows) {
    for (auto &s : vFollows) {
        if (visitedUrl.find(s) != visitedUrl.end()) {
            continue;
        }

        visitedUrl.insert(s);
        unvisitedUrl.push(s);
    }
}
