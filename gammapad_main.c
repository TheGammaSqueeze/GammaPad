/*****************************************************
 * gammapad_main.c
 *
 * Main entry point:
 *  - If user specifies devices (e.g. multiple
 *    controllers plus optional --ffdev=...),
 *    we open them in aggregator mode.
 *  - Create the virtual pad + mouse.
 *  - Immediately after creating the virtual pad,
 *    remove the /dev/input/event* node of the
 *    primary physical device so only the virtual
 *    controller remains visible.
 *  - On exit (exit command or Ctrl-C), we do 
 *    unbindAndRebind() manually plus let the 
 *    destructor also do it (redundant but safer)
 *    so that the node is restored.
 *****************************************************/

#include "gammapad.h"
#include "gammapad_inputdefs.h"
#include "gammapad_capture.h"  // for open_physical_device, discoverKeys, discoverAxes, removePrimaryPhysicalNode, unbindAndRebind
#include <sys/epoll.h>
#include <linux/input.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>

#define MAX_ACTIVE_EVENTS 64
#define EPOLL_MAX_EVENTS  16

enum EventType {
    EVENT_TYPE_KEY,
    EVENT_TYPE_ABS
};

struct ActiveEvent {
    enum EventType type;
    int code;
    int value;
    unsigned long long startMs;
    unsigned long long durationMs;
};

static struct ActiveEvent activeEvents[MAX_ACTIVE_EVENTS];

int controllerFd = -1;  /* Virtual gamepad */
int mouseFd      = -1;  /* Virtual mouse   */
int g_physicalFd = -1;  /* leftover single-device usage */

#define MAX_PHYSICAL_DEVS 16
static int g_physFds[MAX_PHYSICAL_DEVS];
static int g_physCount=0;

static int g_shouldExit=0;
static void sigintHandler(int sig)
{
    (void)sig;
    g_shouldExit=1;
}

/*
 * For real FF device if user does --ffdev=...
 */
int g_ffPhysicalFd= -1;
int g_hasPhysicalFF= 0;

/*
 * function prototypes
 */
int create_virtual_controller(int* fd_out);
int create_virtual_mouse(int* fd_out);
void destroy_virtual_device(int fd);

int dummy_upload_ff_effect(struct ff_effect* effect);
int dummy_erase_ff_effect(int kernel_id);
void storeUploadedEffect(struct ff_effect* eff);
void ff_play_effect(int kernel_id, int doPlay);
void parseCommand(const char* line);

static void cleanupOnExit(void)
{
    /* Attempt to unbind/rebind in case destructor doesn't run or user kills app forcibly */
    fprintf(stderr,"[GammaPad] cleanupOnExit => calling unbindAndRebind.\n");
    unbindAndRebind();
}

/*
 * scheduleEvent => from parseCommand
 */
static void sendEvent(int code, enum EventType t, int value, unsigned long long dur)
{
    for(int i=0;i<MAX_ACTIVE_EVENTS;i++){
        if(activeEvents[i].code==0 && activeEvents[i].value==0){
            activeEvents[i].type= t;
            activeEvents[i].code= code;
            activeEvents[i].value= value;
            activeEvents[i].startMs= getTimeMs();
            activeEvents[i].durationMs= dur;
            break;
        }
    }
    if(controllerFd<0) return;

    struct input_event ev[2];
    memset(ev,0,sizeof(ev));

    if(t==EVENT_TYPE_KEY){
        ev[0].type= EV_KEY;
        ev[0].code= code;
        ev[0].value= value;
    } else {
        ev[0].type= EV_ABS;
        ev[0].code= code;
        ev[0].value= value;
    }
    ev[1].type= EV_SYN;
    ev[1].code= SYN_REPORT;
    ev[1].value= 0;
    write(controllerFd,&ev,sizeof(ev));
}

void scheduleEvent(int code, int isKey, int value, unsigned long long durationMs)
{
    sendEvent(code, (isKey ? EVENT_TYPE_KEY : EVENT_TYPE_ABS), value, durationMs);
}

/*
 * resetEvent => for short-press => auto-release
 */
static void resetEvent(int code, enum EventType t)
{
    if(controllerFd<0) return;

    struct input_event ev[2];
    memset(ev,0,sizeof(ev));

    if(t==EVENT_TYPE_KEY){
        ev[0].type= EV_KEY;
        ev[0].code= code;
        ev[0].value=0;
    } else {
        ev[0].type= EV_ABS;
        ev[0].code= code;
        ev[0].value=0;
    }
    ev[1].type= EV_SYN;
    ev[1].code= SYN_REPORT;
    ev[1].value=0;
    write(controllerFd, &ev, sizeof(ev));
}

/*
 * checkEventTimeouts => see if short-press ended => auto release
 */
static void checkEventTimeouts(void)
{
    unsigned long long now= getTimeMs();
    for(int i=0;i<MAX_ACTIVE_EVENTS;i++){
        if(activeEvents[i].code!=0 || activeEvents[i].value!=0){
            unsigned long long elapsed= now - activeEvents[i].startMs;
            if(elapsed>= activeEvents[i].durationMs){
                resetEvent(activeEvents[i].code, activeEvents[i].type);
                activeEvents[i].type= EVENT_TYPE_KEY;
                activeEvents[i].code= 0;
                activeEvents[i].value= 0;
                activeEvents[i].startMs=0;
                activeEvents[i].durationMs=0;
            }
        }
    }
}

/*
 * handleFFRequest => EV_UINPUT => UI_FF_UPLOAD or UI_FF_ERASE
 */
static void handleFFRequest(const struct input_event* ev)
{
    if(!ev) return;
    if(ev->code==UI_FF_UPLOAD){
        struct uinput_ff_upload ffup;
        memset(&ffup,0,sizeof(ffup));
        ffup.request_id= ev->value;
        if(!ioctl(controllerFd, UI_BEGIN_FF_UPLOAD, &ffup)){
            dummy_upload_ff_effect(&ffup.effect);
            ffup.retval=0;
            if(!ioctl(controllerFd, UI_END_FF_UPLOAD, &ffup)){
                storeUploadedEffect(&ffup.effect);
            }
        }
    } else if(ev->code==UI_FF_ERASE){
        struct uinput_ff_erase fferase;
        memset(&fferase,0,sizeof(fferase));
        fferase.request_id= ev->value;
        if(!ioctl(controllerFd, UI_BEGIN_FF_ERASE, &fferase)){
            dummy_erase_ff_effect(fferase.effect_id);
            fferase.retval=0;
            ioctl(controllerFd, UI_END_FF_ERASE, &fferase);
        }
    }
}

/*
 * handleFFPlayStop => EV_FF => start or stop effect
 */
static void handleFFPlayStop(int kid, int doPlay)
{
    ff_play_effect(kid, doPlay);
}

/*
 * processControllerFdEvent => read from the virtual pad
 */
static void processControllerFdEvent(void)
{
    struct input_event ie;
    while(1){
        ssize_t n= read(controllerFd,&ie,sizeof(ie));
        if(n<0){
            if(errno==EAGAIN||errno==EWOULDBLOCK) break;
            break;
        }
        if(n==0) break;
        if((size_t)n<sizeof(ie)) break;

        if(ie.type==EV_UINPUT){
            handleFFRequest(&ie);
        } else if(ie.type==EV_FF){
            handleFFPlayStop(ie.code, ie.value);
        }
    }
}

/*
 * processPhysicalDeviceEvent => read from physical => forward
 */
static void processPhysicalDeviceEvent(int physical_fd)
{
    struct input_event ev;
    while(1){
        ssize_t n= read(physical_fd,&ev,sizeof(ev));
        if(n<0){
            if(errno==EAGAIN||errno==EWOULDBLOCK) break;
            break;
        }
        if(n==0) break;
        if((size_t)n<sizeof(ev)) break;

        forward_physical_event(&ev);
    }
}

/*
 * If we have a real FF device, read inbound EV_FF => replicate
 */
static void processPhysicalFFDeviceEvent(int fd)
{
    struct input_event ev;
    while(1){
        ssize_t n= read(fd,&ev,sizeof(ev));
        if(n<0){
            if(errno==EAGAIN||errno==EWOULDBLOCK) break;
            break;
        }
        if(n==0) break;
        if((size_t)n<sizeof(ev)) break;

        if(ev.type==EV_FF){
            /* The real device might re-emit these if it's not forcibly removed. 
               We'll forward to our virtual pad if we want the effect reflected. */
            if(controllerFd>=0){
                write(controllerFd,&ev,sizeof(ev));
            }
        }
    }
}

/*
 * maybeResolveDevicePath => if user typed e.g. "retrogame_joypad",
 * we search /dev/input/eventN for a matching name. else treat as path.
 */
static char* maybeResolveDevicePath(const char* arg)
{
    if(strstr(arg,"/dev/")!=NULL){
        return strdup(arg);
    }

    const int MAX_EVENT_SEARCH=64;
    for(int i=0;i<MAX_EVENT_SEARCH;i++){
        char devPath[128];
        snprintf(devPath,sizeof(devPath),"/dev/input/event%d", i);
        int fd= open(devPath,O_RDONLY);
        if(fd<0) continue;

        char devName[256];
        memset(devName,0,sizeof(devName));
        if(ioctl(fd, EVIOCGNAME(sizeof(devName)), devName)>=0){
            if(!strcmp(devName,arg)){
                close(fd);
                return strdup(devPath);
            }
        }
        close(fd);
    }
    return strdup(arg);
}

/*
 * open_physical_ff_device => if user gave --ffdev=...
 */
static int open_physical_ff_device(const char* path)
{
    if(!path) return -1;
    int fd= open(path,O_RDWR|O_NONBLOCK);
    if(fd<0){
        fprintf(stderr,"[GammaPad] Could not open FF device '%s': %s\n", path, strerror(errno));
        return -1;
    }

    unsigned long evbit[8];
    memset(evbit,0,sizeof(evbit));
    if(ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit)<0){
        fprintf(stderr,"[GammaPad] EVIOCGBIT(0) fail for FF dev '%s': %s\n", path,strerror(errno));
        close(fd);
        return -1;
    }
    if(!(evbit[EV_FF/(8*sizeof(long))] & (1UL<<(EV_FF%(8*sizeof(long)))))){
        fprintf(stderr,"[GammaPad] Device '%s' does not support EV_FF.\n", path);
        close(fd);
        return -1;
    }

    fprintf(stderr,"[GammaPad] Found real FF device '%s'. We'll forward FF to it.\n", path);

    if(ioctl(fd, EVIOCGRAB,1)<0){
        fprintf(stderr,"[GammaPad] EVIOCGRAB on FF dev '%s' => %s\n", path,strerror(errno));
    }

    return fd;
}

/*
 * add_epoll_fd => helper
 */
static void add_epoll_fd(int epfd, int fd)
{
    if(fd<0) return;
    struct epoll_event ev;
    memset(&ev,0,sizeof(ev));
    ev.events= EPOLLIN|EPOLLET;
    ev.data.fd= fd;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, fd,&ev)<0){
        fprintf(stderr,"epoll_ctl ADD fd=%d => %s\n", fd,strerror(errno));
    }
    fcntl(fd,F_SETFL,O_NONBLOCK);
}

int main(int argc, char** argv)
{
    signal(SIGINT, sigintHandler);

    g_physCount=0;

    /* 1) parse arguments for --ffdev=... */
    char* ffDevArg= NULL;
    for(int i=1;i<argc;i++){
        if(!strncmp(argv[i],"--ffdev=",8)){
            ffDevArg= argv[i]+8;
        }
    }

    if(ffDevArg){
        char* resolvedFF= maybeResolveDevicePath(ffDevArg);
        g_ffPhysicalFd= open_physical_ff_device(resolvedFF);
        free(resolvedFF);
        if(g_ffPhysicalFd>=0){
            g_hasPhysicalFF=1;
        }
    }

    /* 2) open normal physical devices for other arguments */
    for(int i=1; i<argc; i++){
        if(!strncmp(argv[i],"--ffdev=",8)){
            continue;
        }
        if(g_physCount>=MAX_PHYSICAL_DEVS){
            fprintf(stderr,"[GammaPad] Too many devices => skip '%s'\n", argv[i]);
            continue;
        }
        char* resolved= maybeResolveDevicePath(argv[i]);
        int fd= open_physical_device(resolved);
        free(resolved);

        if(fd<0){
            fprintf(stderr,"[GammaPad] Could not open '%s'.\n", argv[i]);
        } else {
            g_physFds[g_physCount]= fd;
            g_physCount++;
        }
    }

    /* 3) create the virtual pad + mouse */
    if(create_virtual_controller(&controllerFd)<0){
        fprintf(stderr,"[GammaPad] create_virtual_controller => fail.\n");
        for(int i=0;i<g_physCount;i++){
            ioctl(g_physFds[i],EVIOCGRAB,0);
            close(g_physFds[i]);
        }
        if(g_ffPhysicalFd>=0){
            close(g_ffPhysicalFd);
            g_ffPhysicalFd=-1;
            g_hasPhysicalFF=0;
        }
        return 1;
    }
    if(create_virtual_mouse(&mouseFd)<0){
        fprintf(stderr,"[GammaPad] create_virtual_mouse => fail.\n");
        destroy_virtual_device(controllerFd);
        for(int i=0;i<g_physCount;i++){
            ioctl(g_physFds[i],EVIOCGRAB,0);
            close(g_physFds[i]);
        }
        if(g_ffPhysicalFd>=0){
            close(g_ffPhysicalFd);
            g_ffPhysicalFd=-1;
            g_hasPhysicalFF=0;
        }
        return 1;
    }

    fprintf(stderr,"GammaPad Virtual Controller (fd=%d)\n", controllerFd);
    fprintf(stderr,"GammaPad Virtual Mouse       (fd=%d)\n", mouseFd);

    /* 4) remove the primary physical node so only the virtual device remains */
    removePrimaryPhysicalNode();

    /* 5) set up epoll => watch the virtual pad, physical fds, and stdin.
       If we have a real FF device, watch it for inbound EV_FF. */
    int epfd= epoll_create1(0);
    if(epfd<0){
        perror("epoll_create1");
        for(int i=0;i<g_physCount;i++){
            ioctl(g_physFds[i],EVIOCGRAB,0);
            close(g_physFds[i]);
        }
        destroy_virtual_device(mouseFd);
        destroy_virtual_device(controllerFd);
        if(g_ffPhysicalFd>=0){
            close(g_ffPhysicalFd);
            g_ffPhysicalFd=-1;
            g_hasPhysicalFF=0;
        }
        return 1;
    }
    add_epoll_fd(epfd, controllerFd);

    for(int i=0;i<g_physCount;i++){
        add_epoll_fd(epfd, g_physFds[i]);
    }
    add_epoll_fd(epfd, STDIN_FILENO);

    if(g_ffPhysicalFd>=0){
        add_epoll_fd(epfd, g_ffPhysicalFd);
    }

    fprintf(stderr,
        "=== GAMMAPAD COMMANDS ===\n"
        " press <button> [ms]\n"
        " push <axis> <value> [ms]\n"
        " exit\n\n"
        "Buttons:\n"
        "   up, down, left, right,\n"
        "   a, b, c, x, y, z,\n"
        "   l1, l2, l3, r1, r2, r3,\n"
        "   select, start, back, mode, gamepad,\n"
        "   volumedown, volumeup, power, 1, 2\n\n"
        "Axes:\n"
        "   abs_x, abs_y, abs_z, abs_rz,\n"
        "   abs_gas, abs_brake, abs_hat0x, abs_hat0y\n"
        "==========================\n"
    );

    struct epoll_event events[EPOLL_MAX_EVENTS];

    /* main loop */
    while(!g_shouldExit){
        checkEventTimeouts();
        int n= epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 500);
        if(n<0){
            if(errno==EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for(int i=0;i<n;i++){
            int fd= events[i].data.fd;
            if(fd==controllerFd){
                if(events[i].events & EPOLLIN){
                    processControllerFdEvent();
                }
            } else if(fd==STDIN_FILENO){
                if(events[i].events & EPOLLIN){
                    char line[256];
                    memset(line,0,sizeof(line));
                    if(!fgets(line,sizeof(line),stdin)) continue;
                    char*nl= strchr(line,'\n');
                    if(nl)*nl=0;
                    if(!strcasecmp(line,"exit")){
                        g_shouldExit=1;
                        continue;
                    }
                    parseCommand(line);
                }
            } else if(fd==g_ffPhysicalFd && g_ffPhysicalFd>=0){
                if(events[i].events & EPOLLIN){
                    processPhysicalFFDeviceEvent(fd);
                }
            } else {
                /* aggregator physical device */
                if(events[i].events & EPOLLIN){
                    processPhysicalDeviceEvent(fd);
                }
            }
        }
    }

    close(epfd);

    /* cleanup => unbind/bind manually here as well in case destructor doesn't catch it */
    for(int i=0;i<g_physCount;i++){
        ioctl(g_physFds[i],EVIOCGRAB,0);
        close(g_physFds[i]);
    }
    g_physCount=0;

    if(g_ffPhysicalFd>=0){
        close(g_ffPhysicalFd);
        g_ffPhysicalFd=-1;
        g_hasPhysicalFF=0;
    }

    destroy_virtual_device(mouseFd);
    destroy_virtual_device(controllerFd);

    fprintf(stderr,"[GammaPad] Exiting.\n");
    return 0;
}
