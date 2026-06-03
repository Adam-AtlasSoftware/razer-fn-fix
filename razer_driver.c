#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/inotify.h>
#include <syslog.h>
#include <linux/uinput.h>
#include <errno.h>

#define LATCH_TIMEOUT_SEC  1.5
#define RAZER_VENDOR_ID    "1532"
#define MAX_DEVICES        16

#define LONG_BITS(b) ((b + 7) / 8)
#define test_bit(bit, array) ((array[bit / 8] >> (bit % 8)) & 1)

int hid_fds[MAX_DEVICES];
int kbd_fds[MAX_DEVICES];
int hid_count = 0;
int kbd_count = 0;

void emit_key(int fd, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = EV_KEY;
    ie.code = code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0) return;

    ie.type = EV_SYN;
    ie.code = SYN_REPORT;
    ie.value = 0;
    if (write(fd, &ie, sizeof(ie)) < 0) return;
}

void close_existing_handles() {
    for (int i = 0; i < kbd_count; i++) {
        ioctl(kbd_fds[i], EVIOCGRAB, 0);
        close(kbd_fds[i]);
    }
    for (int i = 0; i < hid_count; i++) {
        close(hid_fds[i]);
    }
    kbd_count = 0;
    hid_count = 0;
}

int perform_topology_discovery() {
    close_existing_handles();

    DIR *dir;
    struct dirent *ent;
    char path[512];
    char buf[1024];

    dir = opendir("/sys/class/input");
    if (dir != NULL) {
        while ((ent = readdir(dir)) != NULL && kbd_count < MAX_DEVICES) {
            if (strncmp(ent->d_name, "event", 5) == 0) {
                snprintf(path, sizeof(path), "/sys/class/input/%s/device/uevent", ent->d_name);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    memset(buf, 0, sizeof(buf));
                    if (read(fd, buf, sizeof(buf) - 1) > 0) {
                        if (strstr(buf, RAZER_VENDOR_ID) != NULL || strstr(buf, "PRODUCT=3/1532") != NULL || strstr(buf, "PRODUCT=5/1532") != NULL) {
                            char dev_path[256];
                            snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", ent->d_name);
                            int target_fd = open(dev_path, O_RDONLY | O_NONBLOCK);
                            if (target_fd >= 0) {
                                unsigned char key_bits[LONG_BITS(KEY_MAX)];
                                memset(key_bits, 0, sizeof(key_bits));
                                if (ioctl(target_fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
                                    if (test_bit(KEY_A, key_bits)) {
                                        kbd_fds[kbd_count++] = target_fd;
                                        syslog(LOG_INFO, "Mapped tracking interface to keyboard matrix: %s", dev_path);
                                        close(fd);
                                        continue;
                                    }
                                }
                                close(target_fd);
                            }
                        }
                    }
                    close(fd);
                }
            }
        }
        closedir(dir);
    }

    dir = opendir("/sys/class/hidraw");
    if (dir != NULL) {
        while ((ent = readdir(dir)) != NULL && hid_count < MAX_DEVICES) {
            if (strncmp(ent->d_name, "hidraw", 6) == 0) {
                snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", ent->d_name);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    memset(buf, 0, sizeof(buf));
                    if (read(fd, buf, sizeof(buf) - 1) > 0) {
                        if (strstr(buf, RAZER_VENDOR_ID) != NULL || strstr(buf, "PRODUCT=3/1532") != NULL || strstr(buf, "PRODUCT=5/1532") != NULL) {
                            char dev_path[256];
                            snprintf(dev_path, sizeof(dev_path), "/dev/%s", ent->d_name);
                            int target_fd = open(dev_path, O_RDONLY | O_NONBLOCK);
                            if (target_fd >= 0) {
                                hid_fds[hid_count++] = target_fd;
                                syslog(LOG_INFO, "Mapped tracking interface to Fn control node: %s", dev_path);
                            }
                        }
                    }
                    close(fd);
                }
            }
        }
        closedir(dir);
    }

    return (hid_count > 0 && kbd_count > 0);
}

int main() {
    openlog("razer-fn-daemon", LOG_PID, LOG_DAEMON);
    syslog(LOG_INFO, "Initializing production hot-pluggable core service...");

    int uinp_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinp_fd < 0) {
        syslog(LOG_ERR, "Fatal: Could not allocate virtual uinput subsystem context.");
        closelog();
        return 1;
    }

    ioctl(uinp_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_HOME);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_END);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_SYSRQ);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_SLEEP);
    ioctl(uinp_fd, UI_SET_KEYBIT, KEY_PAUSE);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    strcpy(usetup.name, "Razer-Linux-Fn-Fix-C");
    ioctl(uinp_fd, UI_DEV_SETUP, &usetup);
    ioctl(uinp_fd, UI_DEV_CREATE);

    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        syslog(LOG_ERR, "Fatal: Inotify allocation failed.");
        close(uinp_fd);
        closelog();
        return 1;
    }
    inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_DELETE);

    while (!perform_topology_discovery()) {
        sleep(3);
    }
    syslog(LOG_INFO, "Active topology link completed successfully.");

    if (daemon(0, 0) < 0) {
        syslog(LOG_ERR, "Failed to fork into background daemon process wrapper.");
        close(inotify_fd);
        close(uinp_fd);
        closelog();
        return 1;
    }

    int driver_state = 0;
    struct timeval latch_timestamp;
    unsigned char hid_buf[22];
    struct input_event ev;
    int force_reconnect = 0;
    char inotify_buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));

    while (1) {
        if (force_reconnect) {
            syslog(LOG_WARNING, "Topology modification intercepted. Refreshing input links...");
            driver_state = 0;
            force_reconnect = 0;
            sleep(1);
            perform_topology_discovery();
            continue;
        }

        fd_set readers;
        FD_ZERO(&readers);
        int max_fd = inotify_fd;

        FD_SET(inotify_fd, &readers);

        for (int i = 0; i < hid_count; i++) {
            FD_SET(hid_fds[i], &readers);
            if (hid_fds[i] > max_fd) max_fd = hid_fds[i];
        }

        if (driver_state > 0) {
            for (int i = 0; i < kbd_count; i++) {
                FD_SET(kbd_fds[i], &readers);
                if (kbd_fds[i] > max_fd) max_fd = kbd_fds[i];
            }
        }

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        if (driver_state == 2) {
            struct timeval now;
            gettimeofday(&now, NULL);
            double elapsed = (now.tv_sec - latch_timestamp.tv_sec) +
                             (now.tv_usec - latch_timestamp.tv_usec) / 1000000.0;
            if (elapsed > LATCH_TIMEOUT_SEC) {
                for (int i = 0; i < kbd_count; i++) ioctl(kbd_fds[i], EVIOCGRAB, 0);
                driver_state = 0;
            }
        }

        int ready = select(max_fd + 1, &readers, NULL, NULL, &timeout);

        if (ready < 0) {
            if (errno == EINTR) continue;
            force_reconnect = 1;
            continue;
        }

        if (FD_ISSET(inotify_fd, &readers)) {
            ssize_t len = read(inotify_fd, inotify_buf, sizeof(inotify_buf));
            if (len > 0) {
                force_reconnect = 1;
            }
        }

        for (int i = 0; i < hid_count; i++) {
            if (FD_ISSET(hid_fds[i], &readers)) {
                ssize_t bytes = read(hid_fds[i], hid_buf, sizeof(hid_buf));

                if (bytes < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) force_reconnect = 1;
                    continue;
                }
                if (bytes == 0) {
                    force_reconnect = 1;
                    break;
                }

                if (hid_buf[0] == 0x04 && hid_buf[1] == 0x01 && driver_state == 0) {
                    for (int k = 0; k < kbd_count; k++) {
                        ioctl(kbd_fds[k], EVIOCGRAB, 1);
                        while (read(kbd_fds[k], &ev, sizeof(ev)) > 0);
                    }
                    driver_state = 1;
                }
                else if (((hid_buf[0] == 0x05 && hid_buf[1] == 0x51 && hid_buf[2] == 0x01) ||
                         (hid_buf[0] == 0x04 && hid_buf[1] == 0x00)) && driver_state == 1) {
                    gettimeofday(&latch_timestamp, NULL);
                    driver_state = 2;
                }

                if (hid_buf[2] == 0x4c && driver_state > 0) {
                    emit_key(uinp_fd, KEY_SLEEP, 1); emit_key(uinp_fd, KEY_SLEEP, 0);
                }
            }
        }

        if (force_reconnect) continue;

        if (driver_state > 0) {
            for (int i = 0; i < kbd_count; i++) {
                if (FD_ISSET(kbd_fds[i], &readers)) {
                    ssize_t bytes = read(kbd_fds[i], &ev, sizeof(ev));

                    if (bytes < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) force_reconnect = 1;
                        continue;
                    }
                    if (bytes == 0) {
                        force_reconnect = 1;
                        break;
                    }

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

                                for (int k = 0; k < kbd_count; k++) ioctl(kbd_fds[k], EVIOCGRAB, 0);
                                write(uinp_fd, &pass_ie, sizeof(pass_ie));
                            }

                            for (int k = 0; k < kbd_count; k++) ioctl(kbd_fds[k], EVIOCGRAB, 0);
                            driver_state = 0;
                            break;
                        }
                    }
                }
            }
        }
    }

    close_existing_handles();
    close(inotify_fd);
    ioctl(uinp_fd, UI_DEV_DESTROY);
    closelog();
    return 0;
}