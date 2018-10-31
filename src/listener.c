//
// Created by wong on 10/27/18.
//

#include "listener.h"

static void listener_errcb(struct evconnlistener *, void *);

static void listener_cb(struct evconnlistener *, evutil_socket_t,
                        struct sockaddr *, int, void *);


static void listener_errcb(struct evconnlistener *listener, void *userArg) {
    transocks_global_env *env = (transocks_global_env *) userArg;
    int err = EVUTIL_SOCKET_ERROR();
    LOGE("listener error %d (%s)", err, evutil_socket_error_to_string(err));
    if (event_base_loopbreak(env->eventBaseLoop) != 0)
        LOGE("fail to event_base_loopbreak");
}

static void listener_cb(struct evconnlistener *listener, evutil_socket_t clientFd,
                        struct sockaddr *srcAddr, int srcSockLen, void *userArg) {
    transocks_global_env *env = (transocks_global_env *) userArg;
    socklen_t typedSrcSockLen = (socklen_t) srcSockLen;


    if (setnonblocking(clientFd, true) != 0) {
        LOGE("fail to set nonblocking");
        goto freeFd;
    }

    if (apply_tcp_keepalive(clientFd) != 0) {
        LOGE("fail to set TCP keepalive");
        goto freeFd;
    }
    if (apply_tcp_nodelay(clientFd) != 0) {
        LOGE("fail to set TCP nodelay");
        goto freeFd;
    }

    transocks_client *pclient = transocks_client_new(env);

    if (pclient == NULL) {
        LOGE("fail to allocate memory");
        goto freeClient;
    }
    transocks_client **ppclient = &pclient;

    char srcaddrstr[TRANSOCKS_INET_ADDRPORTSTRLEN] = {0};
    char bindaddrstr[TRANSOCKS_INET_ADDRPORTSTRLEN] = {0};
    char destaddrstr[TRANSOCKS_INET_ADDRPORTSTRLEN] = {0};
    generate_sockaddr_port_str(bindaddrstr, TRANSOCKS_INET_ADDRPORTSTRLEN,
                               (const struct sockaddr *) env->bindAddr, env->bindAddrLen);

    pclient->clientaddrlen = typedSrcSockLen;
    memcpy((void *) (pclient->clientaddr), (void *) srcAddr, typedSrcSockLen);
    generate_sockaddr_port_str(srcaddrstr, TRANSOCKS_INET_ADDRPORTSTRLEN,
                               (const struct sockaddr *) srcAddr, typedSrcSockLen);


    if (getorigdst(clientFd, pclient->destaddr, &pclient->destaddrlen) != 0) {
        LOGE("fail to get origdestaddr, close fd");
        goto freeClient;
    }

    generate_sockaddr_port_str(destaddrstr, TRANSOCKS_INET_ADDRPORTSTRLEN,
                               (const struct sockaddr *) (pclient->destaddr), pclient->destaddrlen);


    LOGI("%s accept a conn %s -> %s", bindaddrstr, srcaddrstr, destaddrstr);
    pclient->clientFd = clientFd;
    pclient->global_env = env;

    struct bufferevent *client_bev = bufferevent_socket_new(env->eventBaseLoop,
                                                            clientFd, BEV_OPT_CLOSE_ON_FREE);

    if (client_bev == NULL) {
        LOGE("bufferevent_socket_new");
        goto freeBev;
    }

    pclient->client_bev = client_bev;

    // start connecting SOCKS5 relay

    transocks_start_connect_relay(ppclient);
    return;

    freeBev:
    bufferevent_free(client_bev);

    freeClient:
    transocks_client_free(ppclient);

    freeFd:
    close(clientFd);
}

int listener_init(transocks_global_env *env) {
    int on = 1;
    int err;
    int fd = socket(env->bindAddr->ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE_ERRNO("fail to create socket");
        return -1;
    }
    err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (err != 0) {
        LOGE_ERRNO("fail to set SO_REUSEADDR");
        goto closeFd;
    }
    err = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    if (err != 0) {
        LOGE_ERRNO("fail to set SO_REUSEPORT");
        goto closeFd;
    }
    if (setnonblocking(fd, true) != 0) {
        LOGE("fail to set non-blocking");
        goto closeFd;
    }
    // bind
    err = bind(fd, (struct sockaddr *) env->bindAddr, env->bindAddrLen);
    if (err != 0) {
        LOGE_ERRNO("fail to bind");
        goto closeFd;
    }
    env->listener = evconnlistener_new(env->eventBaseLoop, listener_cb, env,
                                       LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_DISABLED, SOMAXCONN, fd);
    if (env->listener == NULL) {
        LOGE("fail to create listener");
        goto closeFd;
    }
    evconnlistener_set_error_cb(env->listener, listener_errcb);

    if (evconnlistener_enable(env->listener) != 0) {
        LOGE("fail to enable listener");
        return -1;
    }

    return 0;

    closeFd:
    close(fd);

    return -1;
}

void listener_deinit(transocks_global_env *env) {
    if (env->listener != NULL) {
        evconnlistener_disable(env->listener);
        evconnlistener_free(env->listener);
        env->listener = NULL;
    }
}