#include "gammapad.h"
#include "gammapad_inputdefs.h"
#include "gammapad_capture.h"  // for open_physical_device, forward_physical_event
#include <sys/epoll.h>
#include <linux/input.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

/*
 * We'll store up to 64 active events for auto-reset.
 */
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
int g_physicalFd = -1;  /* Source device   */

static int g_shouldExit = 0;

static void sigintHandler(int sig)
{
    (void)sig;
    g_shouldExit = 1;
}

/* Forward declarations. */
int create_virtual_controller(int* fd_out);
int create_virtual_mouse(int* fd_out);
void destroy_virtual_device(int fd);

int dummy_upload_ff_effect(struct ff_effect* effect);
int dummy_erase_ff_effect(int kernel_id);
void storeUploadedEffect(struct ff_effect* eff);
void ff_play_effect(int kernel_id, int doPlay);
void parseCommand(const char* line);

/*
 * scheduleEvent => from parseCommand
 */
static void sendEvent(int code, enum EventType t, int value, unsigned long long dur)
{
    for (int i=0; i<MAX_ACTIVE_EVENTS; i++){
        if (activeEvents[i].code==0 && activeEvents[i].value==0){
            activeEvents[i].type = t;
            activeEvents[i].code = code;
            activeEvents[i].value= value;
            activeEvents[i].startMs= getTimeMs();
            activeEvents[i].durationMs= dur;
            break;
        }
    }
    if (controllerFd < 0) return;

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
    ev[1].value=0;

    write(controllerFd, &ev, sizeof(ev));
}

void scheduleEvent(int code, int isKey, int value, unsigned long long durationMs)
{
    sendEvent(code, (isKey ? EVENT_TYPE_KEY : EVENT_TYPE_ABS), value, durationMs);
}

/*
 * resetEvent => for short-press
 */
static void resetEvent(int code, enum EventType t)
{
    if(controllerFd<0) return;

    struct input_event ev[2];
    memset(ev,0,sizeof(ev));

    if(t==EVENT_TYPE_KEY){
        ev[0].type=EV_KEY;
        ev[0].code=code;
        ev[0].value=0;
    } else {
        ev[0].type=EV_ABS;
        ev[0].code=code;
        ev[0].value=0;
    }
    ev[1].type=EV_SYN;
    ev[1].code=SYN_REPORT;
    ev[1].value=0;
    write(controllerFd, &ev, sizeof(ev));
}

/*
 * checkEventTimeouts => see if short-press ended
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
 * processControllerFdEvent => read from the virtual pad (controllerFd)
 */
static void processControllerFdEvent(void)
{
    struct input_event ie;
    while(1){
        ssize_t n= read(controllerFd, &ie, sizeof(ie));
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
 * processPhysicalDeviceEvent => read from physical, forward
 */
static void processPhysicalDeviceEvent(int physical_fd)
{
    struct input_event ev;
    while(1){
        ssize_t n= read(physical_fd, &ev,sizeof(ev));
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
 * processStdinEvent => parse typed commands
 */
static void processStdinEvent(void)
{
    char line[256];
    memset(line,0,sizeof(line));
    if(!fgets(line,sizeof(line),stdin)) return;
    char*nl= strchr(line,'\n');
    if(nl)*nl=0;
    if(!strcasecmp(line,"exit")){
        g_shouldExit=1;
        return;
    }
    parseCommand(line);
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

    /*
     * Step 1: If user specified a physical device path, open it first,
     * parse .kl, discover scancodes, but DO NOT remove the node yet.
     */
    if(argc>1){
        g_physicalFd= open_physical_device(argv[1]);
        if(g_physicalFd<0){
            fprintf(stderr,"[GammaPad] Could not open '%s'. Will proceed with no physical.\n", argv[1]);
            g_physicalFd=-1;
        } else {
            fprintf(stderr,"[GammaPad] Source '%s' opened.\n", argv[1]);
            /* We do NOT remove node here. We'll do it after creating the virtual pad. */
        }
    }

    /*
     * Step 2: create the Virtual Pad + Virtual Mouse
     */
    if(create_virtual_controller(&controllerFd)<0){
        fprintf(stderr,"[GammaPad] create_virtual_controller => failed.\n");
        return 1;
    }
    if(create_virtual_mouse(&mouseFd)<0){
        fprintf(stderr,"[GammaPad] create_virtual_mouse => failed.\n");
        destroy_virtual_device(controllerFd);
        return 1;
    }
    fprintf(stderr,"GammaPad Virtual Controller (fd=%d)\n", controllerFd);
    fprintf(stderr,"GammaPad Virtual Mouse       (fd=%d)\n", mouseFd);

    /*
     * Step 3: remove the node from /dev/input if we have a real device.
     */
    if(g_physicalFd>=0 && argc>1){
        char rmCmd[300];
        snprintf(rmCmd,sizeof(rmCmd), "rm -f '%s'", argv[1]);
        fprintf(stderr,"[GammaPad] Removing node with: %s\n", rmCmd);
        system(rmCmd);
        fprintf(stderr,"[GammaPad] Removed node: %s\n", argv[1]);
        fprintf(stderr,"[GammaPad] Capturing input from '%s'.\n", argv[1]);
    }

    /*
     * Step 4: set up epoll for the virtual pad, the physical device, and stdin
     */
    int epfd= epoll_create1(0);
    if(epfd<0){
        perror("epoll_create1");
        if(g_physicalFd>=0){
            ioctl(g_physicalFd, EVIOCGRAB, 0);
            close(g_physicalFd);
        }
        destroy_virtual_device(mouseFd);
        destroy_virtual_device(controllerFd);
        return 1;
    }
    add_epoll_fd(epfd, controllerFd);
    if(g_physicalFd>=0){
        add_epoll_fd(epfd, g_physicalFd);
    }
    add_epoll_fd(epfd, STDIN_FILENO);

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

    while(!g_shouldExit){
        checkEventTimeouts();
        int n= epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 500);
        if(n<0){
            if(errno==EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for(int i=0; i<n; i++){
            int fd= events[i].data.fd;
            if(fd==controllerFd){
                if(events[i].events & EPOLLIN){
                    processControllerFdEvent();
                }
            } else if(fd==STDIN_FILENO){
                if(events[i].events & EPOLLIN){
                    processStdinEvent();
                }
            } else if(fd==g_physicalFd){
                if(events[i].events & EPOLLIN){
                    processPhysicalDeviceEvent(g_physicalFd);
                }
            }
        }
    }

    close(epfd);

    if(g_physicalFd>=0){
        ioctl(g_physicalFd,EVIOCGRAB,0);
        close(g_physicalFd);
        g_physicalFd=-1;
    }

    destroy_virtual_device(mouseFd);
    destroy_virtual_device(controllerFd);

    fprintf(stderr,"[GammaPad] Exiting.\n");
    return 0;
}
