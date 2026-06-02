#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/uinput.h>

#define TARGET_EVENT_NODE  "/dev/input/event8"
#define TARGET_HIDRAW_NODE "/dev/hidraw6"
#define LATCH_TIMEOUT_SEC  1.5

void emit_key(int fd, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = EV_KEY;
    ie.code = code;
    ie.value = val;
    write(fd, &ie, sizeof(ie));
    
    ie.type = EV_SYN;
    ie.code = SYN_REPORT;
    ie.value = 0;
    write(fd, &ie, sizeof(ie));
}

int main() {
    int hid_fd = open(TARGET_HIDRAW_NODE, O_RDONLY | O_NONBLOCK);
    int kbd_fd = open(TARGET_EVENT_NODE, O_RDONLY | O_NONBLOCK);
    int uinp_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (hid_fd < 0 || kbd_fd < 0 || uinp_fd < 0) {
        perror("Initialization Error (Are you running with sudo?)");
        return 1;
    }

    // Initialize Virtual Injection Device Capabilities
    ioctl(uinp_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_HOME);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_END);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_SYSRQ);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_SLEEP);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_PAUSE);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    strcpy(usetup.name, "Razer-Linux-Navigation-Fix-C");
    ioctl(uinp_fd, UI_DEV_SETUP, &usetup);
    ioctl(uinp_fd, UI_DEV_CREATE);

    // Auto-Daemonize: Detaches process from terminal and runs silently in background
    if (daemon(0, 0) < 0) {
        perror("Failed to daemonize");
        return 1;
    }

    int driver_state = 0; 
    struct timeval latch_timestamp;
    unsigned char hid_buf[22];
    struct input_event ev;

    while (1) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(hid_fd, &readers);
        if (driver_state > 0) {
            FD_SET(kbd_fd, &readers);
        }

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000; 

        if (driver_state == 2) {
            struct timeval now;
            gettimeofday(&now, NULL);
            double elapsed = (now.tv_sec - latch_timestamp.tv_sec) + 
                             (now.tv_usec - latch_timestamp.tv_usec) / 1000000.0;
            if (elapsed > LATCH_TIMEOUT_SEC) {
                ioctl(kbd_fd, EVIOCGRAB, 0); 
                driver_state = 0;
            }
        }

        int ready = select(FD_SETSIZE, &readers, NULL, NULL, &timeout);
        if (ready < 0) break;

        if (FD_ISSET(hid_fd, &readers)) {
            ssize_t bytes = read(hid_fd, hid_buf, sizeof(hid_buf));
            if (bytes > 0) {
                if (hid_buf[0] == 0x04 && hid_buf[1] == 0x01 && driver_state == 0) {
                    ioctl(kbd_fd, EVIOCGRAB, 1); 
                    while (read(kbd_fd, &ev, sizeof(ev)) > 0); // Clear buffer
                    driver_state = 1;
                }
                else if (((hid_buf[0] == 0x05 && hid_buf[1] == 0x51 && hid_buf[2] == 0x01) || 
                         (hid_buf[0] == 0x04 && hid_buf[1] == 0x00)) && driver_state == 1) {
                    gettimeofday(&latch_timestamp, NULL);
                    driver_state = 2;
                }
                
                if (hid_buf[2] == 0x4c) {
                    emit_key(uinp_fd, KEY_SLEEP, 1);
                    emit_key(uinp_fd, KEY_SLEEP, 0);
                }
            }
        }

        if (driver_state > 0 && FD_ISSET(kbd_fd, &readers)) {
            ssize_t bytes = read(kbd_fd, &ev, sizeof(ev));
            if (bytes >= (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY && ev.value == 1) { 
                    int handled = 0;
                    if (driver_state == 2) { 
                        int target_code = 0;
                        if (ev.code == KEY_PAGEUP)        target_code = KEY_HOME;
                        else if (ev.code == KEY_PAGEDOWN) target_code = KEY_END;
                        else if (ev.code == KEY_P)        target_code = KEY_SYSRQ;
                        else if (ev.code == KEY_DELETE)   target_code = KEY_SLEEP;
                        else if (ev.code == KEY_INSERT)   target_code = KEY_PAUSE;

                        if (target_code != 0) {
                            emit_key(uinp_fd, target_code, 1);
                            emit_key(uinp_fd, target_code, 0);
                            handled = 1;
                        }
                    }

                    if (!handled) {
                        struct input_event pass_ie;
                        memset(&pass_ie, 0, sizeof(pass_ie));
                        pass_ie.type = ev.type;
                        pass_ie.code = ev.code;
                        pass_ie.value = ev.value;
                        
                        ioctl(kbd_fd, EVIOCGRAB, 0);
                        write(uinp_fd, &pass_ie, sizeof(pass_ie));
                    }

                    ioctl(kbd_fd, EVIOCGRAB, 0);
                    driver_state = 0;
                }
            }
        }
    }

    ioctl(kbd_fd, EVIOCGRAB, 0);
    ioctl(uinp_fd, UI_DEV_DESTROY);
    close(hid_fd);
    close(kbd_fd);
    close(uinp_fd);
    return 0;
}
