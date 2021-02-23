#include <assert.h>
#include "event_loop.h"
#include "common.h"
#include "log.h"
#include "event_dispatcher.h"
#include "channel.h"
#include "utils.h"

// in the i/o thread
int event_loop_handle_pending_channel(struct event_loop *eventLoop) {
    //get the lock
    pthread_mutex_lock(&eventLoop->mutex);
    eventLoop->is_handle_pending = 1;

    struct channel_element *channelElement = eventLoop->pending_head;
    while (channelElement != NULL) {
        //save into event_map
        struct channel *channel = channelElement->channel;
        int fd = channel->fd;
        if (channelElement->type == 1) {
            event_loop_handle_pending_add(eventLoop, fd, channel);
        } else if (channelElement->type == 2) {
            event_loop_handle_pending_remove(eventLoop, fd, channel);
        } else if (channelElement->type == 3) {
            event_loop_handle_pending_update(eventLoop, fd, channel);
        }
        channelElement = channelElement->next;
    }

    eventLoop->pending_head = eventLoop->pending_tail = NULL;
    eventLoop->is_handle_pending = 0;

    //release the lock
    pthread_mutex_unlock(&eventLoop->mutex);

    return 0;
}
/**
 * 客户端连接高并发场景，当sub-dispatcher将socket fd添加到自身epoll过慢时，将master dispatcher分配给该线程的socket fd还存在
 * eventLoop->pending_head链表上
 * */
void event_loop_channel_buffer_nolock(struct event_loop *eventLoop, int fd, struct channel *channel1, int type) {
    //add channel into the pending list
    struct channel_element *channelElement = malloc(sizeof(struct channel_element));
    channelElement->channel = channel1;
    channelElement->type = type;
    channelElement->next = NULL;
    //第一个元素
    if (eventLoop->pending_head == NULL) {
        eventLoop->pending_head = eventLoop->pending_tail = channelElement;
    } else {
        eventLoop->pending_tail->next = channelElement;
        eventLoop->pending_tail = channelElement;
    }
}
/**
 * event_loop_channel_buffer_nolock与event_loop_wakeup函数调用顺序保证
 * master dispather一定是先将客户端连接挂到sub-dispatcher channelElemt链表上，然后往sub-dispatcher
 * 申请的soketpair中写入数据触发事件
 * */
int event_loop_do_channel_event(struct event_loop *eventLoop, int fd, struct channel *channel1, int type) {
    //get the lock
    pthread_mutex_lock(&eventLoop->mutex);
    assert(eventLoop->is_handle_pending == 0);
    //完成套接字所在channel挂到event_loop对应链表上
    event_loop_channel_buffer_nolock(eventLoop, fd, channel1, type);
    //release the lock
    pthread_mutex_unlock(&eventLoop->mutex);
    //master dispatcher创建新连接后，使用slave dispatcher event_loop的sockepair触发slave dispather打印一条消息
    if (!isInSameThread(eventLoop)) {
        //完成master dispatcher线程触发sub-dispatcher线程事件
        event_loop_wakeup(eventLoop);
    } else {
        //完成同一个线程内的事件注册
        event_loop_handle_pending_channel(eventLoop);
    }

    return 0;

}

int event_loop_add_channel_event(struct event_loop *eventLoop, int fd, struct channel *channel1) {
    return event_loop_do_channel_event(eventLoop, fd, channel1, 1);
}

int event_loop_remove_channel_event(struct event_loop *eventLoop, int fd, struct channel *channel1) {
    return event_loop_do_channel_event(eventLoop, fd, channel1, 2);
}

int event_loop_update_channel_event(struct event_loop *eventLoop, int fd, struct channel *channel1) {
    return event_loop_do_channel_event(eventLoop, fd, channel1, 3);
}

// in the i/o thread
int event_loop_handle_pending_add(struct event_loop *eventLoop, int fd, struct channel *channel) {
    msgx("add channel fd == %d, %s", fd, eventLoop->thread_name);
    struct channel_map *map = eventLoop->channelMap;

    if (fd < 0)
        return 0;

    //文件描述符超过map数组存储空间，触发空间动态扩展
    if (fd >= map->nentries) {
        if (map_make_space(map, fd, sizeof(struct channel *)) == -1)
            return (-1);
    }

    //第一次创建，增加
    if ((map)->entries[fd] == NULL) {
        map->entries[fd] = channel;
        //add channel
        struct event_dispatcher *eventDispatcher = eventLoop->eventDispatcher;
        eventDispatcher->add(eventLoop, channel);
        return 1;
    }

    return 0;
}

// in the i/o thread
int event_loop_handle_pending_remove(struct event_loop *eventLoop, int fd, struct channel *channel1) {
    struct channel_map *map = eventLoop->channelMap;
    assert(fd == channel1->fd);

    if (fd < 0)
        return 0;

    if (fd >= map->nentries)
        return (-1);

    struct channel *channel2 = map->entries[fd];

    //update dispatcher(multi-thread)here
    struct event_dispatcher *eventDispatcher = eventLoop->eventDispatcher;

    int retval = 0;
    if (eventDispatcher->del(eventLoop, channel2) == -1) {
        retval = -1;
    } else {
        retval = 1;
    }

    map->entries[fd] = NULL;
    return retval;
}

// in the i/o thread
int event_loop_handle_pending_update(struct event_loop *eventLoop, int fd, struct channel *channel) {
    msgx("update channel fd == %d, %s", fd, eventLoop->thread_name);
    struct channel_map *map = eventLoop->channelMap;

    if (fd < 0)
        return 0;

    if ((map)->entries[fd] == NULL) {
        return (-1);
    }

    //update channel
    struct event_dispatcher *eventDispatcher = eventLoop->eventDispatcher;
    eventDispatcher->update(eventLoop, channel);
}

int channel_event_activate(struct event_loop *eventLoop, int fd, int revents) {
    struct channel_map *map = eventLoop->channelMap;
    msgx("activate channel fd == %d, revents=%d, %s", fd, revents, eventLoop->thread_name);

    if (fd < 0)
        return 0;

    if (fd >= map->nentries)return (-1);

    struct channel *channel = map->entries[fd];
    assert(fd == channel->fd);

    if (revents & (EVENT_READ)) {
        if (channel->eventReadCallback) channel->eventReadCallback(channel->data);
    }
    if (revents & (EVENT_WRITE)) {
        if (channel->eventWriteCallback) channel->eventWriteCallback(channel->data);
    }

    return 0;

}
/**
 * 客户端新连接加入后，master dispatcher向soket pair写入数据触发sub dispatcher epoll注册事件，
 * sub-dispatcher处理事件后，读取eventLoop->pending_head链表，将socket fd注册到sub-dispatcher
 * 的epoll上
 * */
void event_loop_wakeup(struct event_loop *eventLoop) {
    char one = 'a';
    ssize_t n = write(eventLoop->socketPair[0], &one, sizeof one);
    if (n != sizeof one) {
        LOG_ERR("wakeup event loop thread failed");
    }
}

int handleWakeup(void *data) {
    struct event_loop *eventLoop = (struct event_loop *) data;
    char one;
    ssize_t n = read(eventLoop->socketPair[1], &one, sizeof one);
    if (n != sizeof one) {
        LOG_ERR("handleWakeup  failed");
    }
    msgx("wakeup, %s", eventLoop->thread_name);
}

struct event_loop *event_loop_init() {
    return event_loop_init_with_name(NULL);
}
/**
 * Discriptor: 创建并初始化sub-dispatcher线程/dispatcher线程自有event_loop
 * Function:
 *      1. 创建poll/epoll事件监听描述符
 *      2. 新建事件socketpair, 用于dispatcher完成connection新建后
 *      3. 到对用的线程epoll上注册读事件
 * */

struct event_loop *event_loop_init_with_name(char *thread_name) {
    struct event_loop *eventLoop = malloc(sizeof(struct event_loop));
    pthread_mutex_init(&eventLoop->mutex, NULL);
    pthread_cond_init(&eventLoop->cond, NULL);

    if (thread_name != NULL) {
        eventLoop->thread_name = thread_name;
    } else {
        eventLoop->thread_name = "main thread";
    }

    eventLoop->quit = 0;
    eventLoop->channelMap = malloc(sizeof(struct channel_map));
    map_init(eventLoop->channelMap);

#ifdef EPOLL_ENABLE
    msgx("set epoll as dispatcher, %s", eventLoop->thread_name);
    eventLoop->eventDispatcher = &epoll_dispatcher;
#else
    msgx("set poll as dispatcher, %s", eventLoop->thread_name);
    eventLoop->eventDispatcher = &poll_dispatcher;
#endif
    //初始化I/O多路复用文件描述符
    eventLoop->event_dispatcher_data = eventLoop->eventDispatcher->init(eventLoop);

    //add the socketfd to event
    eventLoop->owner_thread_id = pthread_self();
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, eventLoop->socketPair) < 0) {
        LOG_ERR("socketpair set fialed");
    }
    eventLoop->is_handle_pending = 0;
    eventLoop->pending_head = NULL;
    eventLoop->pending_tail = NULL;

    struct channel *channel = channel_new(eventLoop->socketPair[1], EVENT_READ, handleWakeup, NULL, eventLoop);
    //完成socket fd读事件注册 slave线程创建监听用于master和slave dispather通讯，master线程注册后未使用
    event_loop_add_channel_event(eventLoop, eventLoop->socketPair[1], channel);

    return eventLoop;
}

/**
 *
 * 1.参数验证
 * 2.调用dispatcher来进行事件分发,分发完回调事件处理函数
 */
int event_loop_run(struct event_loop *eventLoop) {
    assert(eventLoop != NULL);

    struct event_dispatcher *dispatcher = eventLoop->eventDispatcher;

    if (eventLoop->owner_thread_id != pthread_self()) {
        exit(1);
    }

    msgx("event loop run, %s", eventLoop->thread_name);
    struct timeval timeval;
    timeval.tv_sec = 1;
    //while内函数调用顺序是与master dispather线程配合完成客户端新连接套接字注册到sub-dispatcher的epoll
    while (!eventLoop->quit) {
        //block here to wait I/O event, and get active channels
        dispatcher->dispatch(eventLoop, &timeval);

        //handle the pending channel
        //新连接建立时channel挂在sub-dispatcher的event_loop的channelElemt链表上，
        //master dispatcher触发socketpair写操作，sub-dispatcher epoll跳出阻塞，
        //sub-dispatcher处理套接字上写事件后（打印wake-up消息）跳出dispatcher->dispatch(eventLoop, &timeval)函数
        //sub-dispatcher处理master dispatcher挂在sub-dispatcher event_loop channelElemt链表上的channel
        //完成将连接套接字注册到sub-diapatcher的epoll上，后续该套接字上的事件由sub-dispatcher线程监听
        event_loop_handle_pending_channel(eventLoop);
    }

    msgx("event loop end, %s", eventLoop->thread_name);
    return 0;
}


