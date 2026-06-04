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
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include "cJSON.h"
#include "keycodes.h"

#define RAZER_VENDOR_ID    "1532"
#define MAX_DEVICES        16
#define MAX_PROFILES       16
#define MAX_MODIFIERS      4
#define MAX_MACROS         64
#define CONFIG_PATH        "/etc/razer-fn/config.json"
#define LOCAL_CONFIG_PATH  "config.json"

#define LONG_BITS(b) ((b + 7) / 8)
#define test_bit(bit, array) ((array[bit / 8] >> (bit % 8)) & 1)

int verbose = 0;
int foreground = 0;

void log_msg(int priority, const char *format, ...) {
    va_list args;
    va_start(args, format);

    // Write to system logs
    va_list syslog_args;
    va_copy(syslog_args, args);
    vsyslog(priority, format, syslog_args);
    va_end(syslog_args);

    // If verbose or foreground flags are set, print to stderr
    if (foreground || verbose) {
        const char *level = "INFO";
        if (priority == LOG_ERR) level = "ERROR";
        else if (priority == LOG_WARNING) level = "WARNING";
        else if (priority == LOG_DEBUG) level = "DEBUG";

        fprintf(stderr, "[%s] ", level);
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
    }
    va_end(args);
}

typedef enum {
    MOD_HIDRAW,
    MOD_EVDEV
} ModifierType;

typedef struct {
    char name[64];
    ModifierType type;
    unsigned char hid_active[8];
    int hid_active_len;
    unsigned char hid_release[8];
    int hid_release_len;
    int ev_active[8];
    int ev_active_len;
} Modifier;

typedef enum {
    ACT_REMAP,
    ACT_EXEC
} ActionType;

typedef struct {
    char modifier_name[64];
    int sequence[8];
    int sequence_len;
    ActionType action_type;
    int output_keycode;
    char output_command[256];
} Macro;

typedef struct {
    char id[128];
    char user_name[128];
    char hardware_match[256];
    int enabled;
    int latch_timeout_ms;

    Modifier modifiers[MAX_MODIFIERS];
    int modifier_count;

    Macro macros[MAX_MACROS];
    int macro_count;

    int hid_fds[MAX_DEVICES];
    int hid_count;
    int kbd_fds[MAX_DEVICES];
    int kbd_count;

    int driver_state; // 0=inactive, 1=modifier held (grabbed), 2=latch open (grabbed, timer running)
    struct timeval latch_timestamp;
    char active_modifier_name[64];

    int pressed_seq[8];
    int pressed_seq_len;
} Profile;

Profile profiles[MAX_PROFILES];
int profile_count = 0;

void emit_key(int fd, int code, int val) {
    log_msg(LOG_DEBUG, "Emitting key %d, value %d", code, val);
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

void execute_command_bg(const char *command) {
    log_msg(LOG_INFO, "Executing background command: %s", command);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        exit(127);
    }
}

void setup_default_fallback() {
    log_msg(LOG_INFO, "Setting up default fallback layout config.");
    profile_count = 1;
    memset(profiles, 0, sizeof(profiles));
    Profile *p = &profiles[0];
    strcpy(p->id, "default_fallback");
    strcpy(p->user_name, "Default Razer Keyboard Fallback");
    strcpy(p->hardware_match, "1532");
    p->enabled = 1;
    p->latch_timeout_ms = 1500;

    p->modifier_count = 1;
    Modifier *m = &p->modifiers[0];
    strcpy(m->name, "Fn");
    m->type = MOD_HIDRAW;
    m->hid_active[0] = 0x04;
    m->hid_active[1] = 0x01;
    m->hid_active_len = 2;
    m->hid_release[0] = 0x04;
    m->hid_release[1] = 0x00;
    m->hid_release_len = 2;

    p->macro_count = 5;

    p->macros[0].action_type = ACT_REMAP;
    strcpy(p->macros[0].modifier_name, "Fn");
    p->macros[0].sequence[0] = KEY_PAGEUP;
    p->macros[0].sequence_len = 1;
    p->macros[0].output_keycode = KEY_HOME;

    p->macros[1].action_type = ACT_REMAP;
    strcpy(p->macros[1].modifier_name, "Fn");
    p->macros[1].sequence[0] = KEY_PAGEDOWN;
    p->macros[1].sequence_len = 1;
    p->macros[1].output_keycode = KEY_END;

    p->macros[2].action_type = ACT_REMAP;
    strcpy(p->macros[2].modifier_name, "Fn");
    p->macros[2].sequence[0] = KEY_P;
    p->macros[2].sequence_len = 1;
    p->macros[2].output_keycode = KEY_SYSRQ;

    p->macros[3].action_type = ACT_REMAP;
    strcpy(p->macros[3].modifier_name, "Fn");
    p->macros[3].sequence[0] = KEY_DELETE;
    p->macros[3].sequence_len = 1;
    p->macros[3].output_keycode = KEY_SLEEP;

    p->macros[4].action_type = ACT_REMAP;
    strcpy(p->macros[4].modifier_name, "Fn");
    p->macros[4].sequence[0] = KEY_INSERT;
    p->macros[4].sequence_len = 1;
    p->macros[4].output_keycode = KEY_PAUSE;
}

void load_config() {
    log_msg(LOG_INFO, "Loading configuration file...");
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        log_msg(LOG_INFO, "Config path /etc/razer-fn/config.json missing, checking local path.");
        f = fopen(LOCAL_CONFIG_PATH, "r");
    }
    if (!f) {
        log_msg(LOG_WARNING, "No configuration file found. Using default fallback profile.");
        setup_default_fallback();
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(len + 1);
    if (!data) {
        fclose(f);
        log_msg(LOG_ERR, "Failed to allocate memory for configuration load.");
        setup_default_fallback();
        return;
    }
    size_t read_bytes = fread(data, 1, len, f);
    data[read_bytes] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) {
        log_msg(LOG_ERR, "Failed to parse JSON config: %s. Using default fallback.", cJSON_GetErrorPtr());
        setup_default_fallback();
        return;
    }

    profile_count = 0;
    memset(profiles, 0, sizeof(profiles));

    cJSON *profiles_arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (cJSON_IsArray(profiles_arr)) {
        cJSON *prof_json = NULL;
        cJSON_ArrayForEach(prof_json, profiles_arr) {
            if (profile_count >= MAX_PROFILES) break;
            Profile *p = &profiles[profile_count];

            cJSON *id = cJSON_GetObjectItemCaseSensitive(prof_json, "id");
            cJSON *user_name = cJSON_GetObjectItemCaseSensitive(prof_json, "user_name");
            cJSON *hardware_match = cJSON_GetObjectItemCaseSensitive(prof_json, "hardware_match");
            cJSON *enabled = cJSON_GetObjectItemCaseSensitive(prof_json, "enabled");
            cJSON *latch_timeout = cJSON_GetObjectItemCaseSensitive(prof_json, "latch_timeout_ms");

            if (cJSON_IsString(id)) strncpy(p->id, id->valuestring, sizeof(p->id) - 1);
            if (cJSON_IsString(user_name)) strncpy(p->user_name, user_name->valuestring, sizeof(p->user_name) - 1);
            if (cJSON_IsString(hardware_match)) strncpy(p->hardware_match, hardware_match->valuestring, sizeof(p->hardware_match) - 1);
            p->enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : 1;
            p->latch_timeout_ms = cJSON_IsNumber(latch_timeout) ? latch_timeout->valueint : 1500;

            log_msg(LOG_DEBUG, "Parsed profile ID: %s, Name: %s, Enabled: %d, Timeout: %dms, HW: %s",
                    p->id, p->user_name, p->enabled, p->latch_timeout_ms, p->hardware_match);

            p->modifier_count = 0;
            cJSON *modifiers_arr = cJSON_GetObjectItemCaseSensitive(prof_json, "modifiers");
            if (cJSON_IsArray(modifiers_arr)) {
                cJSON *mod_json = NULL;
                cJSON_ArrayForEach(mod_json, modifiers_arr) {
                    if (p->modifier_count >= MAX_MODIFIERS) break;
                    Modifier *m = &p->modifiers[p->modifier_count];

                    cJSON *m_name = cJSON_GetObjectItemCaseSensitive(mod_json, "name");
                    cJSON *m_type = cJSON_GetObjectItemCaseSensitive(mod_json, "type");
                    cJSON *act_seq = cJSON_GetObjectItemCaseSensitive(mod_json, "activation_sequence");
                    cJSON *rel_seq = cJSON_GetObjectItemCaseSensitive(mod_json, "release_sequence");

                    if (cJSON_IsString(m_name)) strncpy(m->name, m_name->valuestring, sizeof(m->name) - 1);
                    if (cJSON_IsString(m_type)) {
                        if (strcmp(m_type->valuestring, "hidraw") == 0) {
                            m->type = MOD_HIDRAW;
                        } else {
                            m->type = MOD_EVDEV;
                        }
                    }

                    if (cJSON_IsArray(act_seq)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, act_seq) {
                            if (cJSON_IsString(item)) {
                                if (m->type == MOD_HIDRAW) {
                                    if (m->hid_active_len < 8) {
                                        m->hid_active[m->hid_active_len++] = (unsigned char)strtol(item->valuestring, NULL, 16);
                                    }
                                } else {
                                    if (m->ev_active_len < 8) {
                                        m->ev_active[m->ev_active_len++] = lookup_keycode(item->valuestring);
                                    }
                                }
                            }
                        }
                    }

                    if (cJSON_IsArray(rel_seq)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, rel_seq) {
                            if (cJSON_IsString(item) && m->type == MOD_HIDRAW) {
                                if (m->hid_release_len < 8) {
                                    m->hid_release[m->hid_release_len++] = (unsigned char)strtol(item->valuestring, NULL, 16);
                                }
                            }
                        }
                    }

                    log_msg(LOG_DEBUG, "  Modifier Name: %s, Type: %s, Act-len: %d, Rel-len: %d",
                            m->name, m->type == MOD_HIDRAW ? "hidraw" : "evdev",
                            m->type == MOD_HIDRAW ? m->hid_active_len : m->ev_active_len,
                            m->hid_release_len);

                    p->modifier_count++;
                }
            }

            p->macro_count = 0;
            cJSON *macros_arr = cJSON_GetObjectItemCaseSensitive(prof_json, "macros");
            if (cJSON_IsArray(macros_arr)) {
                cJSON *macro_json = NULL;
                cJSON_ArrayForEach(macro_json, macros_arr) {
                    if (p->macro_count >= MAX_MACROS) break;
                    Macro *mc = &p->macros[p->macro_count];

                    cJSON *mc_mod = cJSON_GetObjectItemCaseSensitive(macro_json, "modifier");
                    cJSON *mc_seq = cJSON_GetObjectItemCaseSensitive(macro_json, "sequence");
                    cJSON *mc_out = cJSON_GetObjectItemCaseSensitive(macro_json, "output");
                    cJSON *mc_act = cJSON_GetObjectItemCaseSensitive(macro_json, "action_type");

                    if (cJSON_IsString(mc_mod)) strncpy(mc->modifier_name, mc_mod->valuestring, sizeof(mc->modifier_name) - 1);
                    if (cJSON_IsString(mc_act)) {
                        if (strcmp(mc_act->valuestring, "remap") == 0) {
                            mc->action_type = ACT_REMAP;
                        } else {
                            mc->action_type = ACT_EXEC;
                        }
                    }

                    if (cJSON_IsArray(mc_seq)) {
                        cJSON *item = NULL;
                        cJSON_ArrayForEach(item, mc_seq) {
                            if (cJSON_IsString(item) && mc->sequence_len < 8) {
                                mc->sequence[mc->sequence_len++] = lookup_keycode(item->valuestring);
                            }
                        }
                    }

                    if (cJSON_IsString(mc_out)) {
                        if (mc->action_type == ACT_REMAP) {
                            mc->output_keycode = lookup_keycode(mc_out->valuestring);
                        } else {
                            strncpy(mc->output_command, mc_out->valuestring, sizeof(mc->output_command) - 1);
                        }
                    }

                    log_msg(LOG_DEBUG, "  Macro: Modifier: %s, Seq-len: %d, Action: %s, Target: %s",
                            mc->modifier_name, mc->sequence_len,
                            mc->action_type == ACT_REMAP ? "remap" : "exec",
                            mc->action_type == ACT_REMAP ? mc_out->valuestring : mc->output_command);

                    p->macro_count++;
                }
            }

            profile_count++;
        }
    }

    cJSON_Delete(root);
    log_msg(LOG_INFO, "Config loaded. Found %d profiles.", profile_count);
}

void close_existing_handles() {
    log_msg(LOG_DEBUG, "Closing active device file handles.");
    for (int p_idx = 0; p_idx < profile_count; p_idx++) {
        Profile *p = &profiles[p_idx];
        for (int i = 0; i < p->kbd_count; i++) {
            log_msg(LOG_DEBUG, "  Un-grabbing event node descriptor: %d", p->kbd_fds[i]);
            ioctl(p->kbd_fds[i], EVIOCGRAB, 0);
            close(p->kbd_fds[i]);
        }
        for (int i = 0; i < p->hid_count; i++) {
            log_msg(LOG_DEBUG, "  Closing hidraw node descriptor: %d", p->hid_fds[i]);
            close(p->hid_fds[i]);
        }
        p->kbd_count = 0;
        p->hid_count = 0;
    }
}

int perform_topology_discovery() {
    close_existing_handles();
    log_msg(LOG_INFO, "Starting device topology discovery scanning...");

    DIR *dir;
    struct dirent *ent;
    char path[512];
    char buf[1024];

    dir = opendir("/sys/class/input");
    if (dir != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "event", 5) == 0) {
                snprintf(path, sizeof(path), "/sys/class/input/%s/device/uevent", ent->d_name);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    memset(buf, 0, sizeof(buf));
                    if (read(fd, buf, sizeof(buf) - 1) > 0) {
                        for (int p_idx = 0; p_idx < profile_count; p_idx++) {
                            Profile *p = &profiles[p_idx];
                            if (!p->enabled) continue;
                            if (p->kbd_count >= MAX_DEVICES) continue;

                            if (strstr(buf, p->hardware_match) != NULL || 
                                (strcmp(p->hardware_match, "1532") == 0 && strstr(buf, RAZER_VENDOR_ID) != NULL)) {
                                char dev_path[256];
                                snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", ent->d_name);
                                int target_fd = open(dev_path, O_RDONLY | O_NONBLOCK);
                                if (target_fd >= 0) {
                                    unsigned char key_bits[LONG_BITS(KEY_MAX)];
                                    memset(key_bits, 0, sizeof(key_bits));
                                    if (ioctl(target_fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
                                        if (test_bit(KEY_A, key_bits)) {
                                            p->kbd_fds[p->kbd_count++] = target_fd;
                                            log_msg(LOG_INFO, "Mapped profile '%s' to keyboard matrix: %s (fd: %d)", p->id, dev_path, target_fd);
                                            break;
                                        }
                                    }
                                    close(target_fd);
                                }
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
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "hidraw", 6) == 0) {
                snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", ent->d_name);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    memset(buf, 0, sizeof(buf));
                    if (read(fd, buf, sizeof(buf) - 1) > 0) {
                        for (int p_idx = 0; p_idx < profile_count; p_idx++) {
                            Profile *p = &profiles[p_idx];
                            if (!p->enabled) continue;
                            if (p->hid_count >= MAX_DEVICES) continue;

                            if (strstr(buf, p->hardware_match) != NULL || 
                                (strcmp(p->hardware_match, "1532") == 0 && strstr(buf, RAZER_VENDOR_ID) != NULL)) {
                                char dev_path[256];
                                snprintf(dev_path, sizeof(dev_path), "/dev/%s", ent->d_name);
                                int target_fd = open(dev_path, O_RDONLY | O_NONBLOCK);
                                if (target_fd >= 0) {
                                    p->hid_fds[p->hid_count++] = target_fd;
                                    log_msg(LOG_INFO, "Mapped profile '%s' to Fn control node: %s (fd: %d)", p->id, dev_path, target_fd);
                                }
                            }
                        }
                    }
                    close(fd);
                }
            }
        }
        closedir(dir);
    }

    int total_kbd = 0;
    for (int p_idx = 0; p_idx < profile_count; p_idx++) {
        total_kbd += profiles[p_idx].kbd_count;
    }
    log_msg(LOG_INFO, "Topology discovery complete. Linked %d keyboard handles.", total_kbd);
    return (total_kbd > 0);
}

int match_hidraw_active(Profile *p, unsigned char *buf, int len, Modifier **matched_mod) {
    for (int i = 0; i < p->modifier_count; i++) {
        Modifier *m = &p->modifiers[i];
        if (m->type != MOD_HIDRAW) continue;
        if (m->hid_active_len > 0 && len >= m->hid_active_len) {
            int match = 1;
            for (int j = 0; j < m->hid_active_len; j++) {
                if (buf[j] != m->hid_active[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                *matched_mod = m;
                return 1;
            }
        }
    }
    return 0;
}

int match_hidraw_release(Profile *p, unsigned char *buf, int len, const char *mod_name) {
    for (int i = 0; i < p->modifier_count; i++) {
        Modifier *m = &p->modifiers[i];
        if (m->type != MOD_HIDRAW) continue;
        if (strcmp(m->name, mod_name) != 0) continue;
        if (m->hid_release_len > 0 && len >= m->hid_release_len) {
            int match = 1;
            for (int j = 0; j < m->hid_release_len; j++) {
                if (buf[j] != m->hid_release[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) return 1;
        }
    }
    if (len >= 3 && buf[0] == 0x05 && buf[1] == 0x51 && buf[2] == 0x01) {
        return 1;
    }
    if (len >= 2 && buf[0] == 0x04 && buf[1] == 0x00) {
        return 1;
    }
    return 0;
}

int match_evdev_active(Profile *p, int keycode, int value, Modifier **matched_mod) {
    for (int i = 0; i < p->modifier_count; i++) {
        Modifier *m = &p->modifiers[i];
        if (m->type != MOD_EVDEV) continue;
        if (m->ev_active_len > 0) {
            if (m->ev_active[0] == keycode && value == 1) {
                *matched_mod = m;
                return 1;
            }
        }
    }
    return 0;
}

int match_evdev_release(Profile *p, int keycode, int value, const char *mod_name) {
    for (int i = 0; i < p->modifier_count; i++) {
        Modifier *m = &p->modifiers[i];
        if (m->type != MOD_EVDEV) continue;
        if (strcmp(m->name, mod_name) != 0) continue;
        if (m->ev_active_len > 0) {
            if (m->ev_active[0] == keycode && value == 0) {
                return 1;
            }
        }
    }
    return 0;
}

int check_sequence(Profile *p, const char *active_modifier, int *seq, int seq_len, Macro **matched_macro) {
    int potential = 0;
    for (int i = 0; i < p->macro_count; i++) {
        Macro *m = &p->macros[i];
        if (strcmp(m->modifier_name, active_modifier) != 0) continue;

        if (m->sequence_len == seq_len) {
            int match = 1;
            for (int j = 0; j < seq_len; j++) {
                if (m->sequence[j] != seq[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                *matched_macro = m;
                return 1;
            }
        } else if (m->sequence_len > seq_len) {
            int match = 1;
            for (int j = 0; j < seq_len; j++) {
                if (m->sequence[j] != seq[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                potential = 1;
            }
        }
    }
    return potential ? 2 : 0;
}

int main(int argc, char *argv[]) {
    // Parse verbose/foreground options
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        }
    }

    openlog("razer-fn-daemon", LOG_PID, LOG_DAEMON);
    log_msg(LOG_INFO, "Initializing core daemon...");

    int uinp_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinp_fd < 0) {
        log_msg(LOG_ERR, "Error: Failed to allocate uinput context.");
        closelog();
        return 1;
    }

    ioctl(uinp_fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 1; i < KEY_MAX; i++) {
        ioctl(uinp_fd, UI_SET_KEYBIT, i);
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    strcpy(usetup.name, "Razer-Linux-Fn-Fix-C");
    ioctl(uinp_fd, UI_DEV_SETUP, &usetup);
    ioctl(uinp_fd, UI_DEV_CREATE);

    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        log_msg(LOG_ERR, "Fatal: Inotify allocation failed.");
        close(uinp_fd);
        closelog();
        return 1;
    }
    inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_DELETE);
    inotify_add_watch(inotify_fd, "/etc/razer-fn", IN_CREATE | IN_MODIFY | IN_MOVED_TO);

    load_config();

    while (!perform_topology_discovery()) {
        sleep(3);
    }
    log_msg(LOG_INFO, "Topology discovery complete.");

    // Daemonize only if foreground flag is NOT set
    if (!foreground) {
        log_msg(LOG_INFO, "Daemonizing process...");
        if (daemon(0, 0) < 0) {
            log_msg(LOG_ERR, "Failed to fork into background daemon process wrapper.");
            close(inotify_fd);
            close(uinp_fd);
            closelog();
            return 1;
        }
    } else {
        log_msg(LOG_INFO, "Running in foreground mode.");
    }

    unsigned char hid_buf[22];
    struct input_event ev;
    int force_reconnect = 0;
    char inotify_buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));

    while (1) {
        if (force_reconnect) {
            log_msg(LOG_WARNING, "Config/device change detected. Refreshing links...");
            close_existing_handles();
            load_config();
            force_reconnect = 0;
            sleep(1);
            perform_topology_discovery();
            continue;
        }

        fd_set readers;
        FD_ZERO(&readers);
        int max_fd = inotify_fd;

        FD_SET(inotify_fd, &readers);

        for (int p_idx = 0; p_idx < profile_count; p_idx++) {
            Profile *p = &profiles[p_idx];
            if (!p->enabled) continue;

            for (int i = 0; i < p->hid_count; i++) {
                FD_SET(p->hid_fds[i], &readers);
                if (p->hid_fds[i] > max_fd) max_fd = p->hid_fds[i];
            }
            for (int i = 0; i < p->kbd_count; i++) {
                FD_SET(p->kbd_fds[i], &readers);
                if (p->kbd_fds[i] > max_fd) max_fd = p->kbd_fds[i];
            }
        }

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        for (int p_idx = 0; p_idx < profile_count; p_idx++) {
            Profile *p = &profiles[p_idx];
            if (!p->enabled) continue;

            if (p->driver_state == 2) {
                struct timeval now;
                gettimeofday(&now, NULL);
                double elapsed = (now.tv_sec - p->latch_timestamp.tv_sec) +
                                 (now.tv_usec - p->latch_timestamp.tv_usec) / 1000000.0;
                double limit = p->latch_timeout_ms / 1000.0;
                if (elapsed > limit) {
                    log_msg(LOG_INFO, "Profile '%s' latch timeout. Releasing device grab.", p->id);
                    for (int i = 0; i < p->kbd_count; i++) ioctl(p->kbd_fds[i], EVIOCGRAB, 0);

                    if (p->pressed_seq_len > 0) {
                        log_msg(LOG_INFO, "Flushing %d buffered keys upon timeout.", p->pressed_seq_len);
                        for (int j = 0; j < p->pressed_seq_len; j++) {
                            emit_key(uinp_fd, p->pressed_seq[j], 1);
                            emit_key(uinp_fd, p->pressed_seq[j], 0);
                        }
                        p->pressed_seq_len = 0;
                    } else {
                        Modifier *m = NULL;
                        for (int mi = 0; mi < p->modifier_count; mi++) {
                            if (strcmp(p->modifiers[mi].name, p->active_modifier_name) == 0) {
                                m = &p->modifiers[mi];
                                break;
                            }
                        }
                        if (m && m->type == MOD_EVDEV) {
                            log_msg(LOG_DEBUG, "Flushing evdev modifier raw keytap %d.", m->ev_active[0]);
                            emit_key(uinp_fd, m->ev_active[0], 1);
                            emit_key(uinp_fd, m->ev_active[0], 0);
                        }
                    }

                    p->driver_state = 0;
                }
            }
        }

        int ready = select(max_fd + 1, &readers, NULL, NULL, &timeout);

        if (ready < 0) {
            if (errno == EINTR) continue;
            log_msg(LOG_WARNING, "Select error on monitors: %s", strerror(errno));
            force_reconnect = 1;
            continue;
        }

        if (FD_ISSET(inotify_fd, &readers)) {
            ssize_t len = read(inotify_fd, inotify_buf, sizeof(inotify_buf));
            if (len > 0) {
                log_msg(LOG_INFO, "Inotify event triggered.");
                force_reconnect = 1;
            }
        }

        for (int p_idx = 0; p_idx < profile_count; p_idx++) {
            Profile *p = &profiles[p_idx];
            if (!p->enabled) continue;

            for (int i = 0; i < p->hid_count; i++) {
                if (FD_ISSET(p->hid_fds[i], &readers)) {
                    ssize_t bytes = read(p->hid_fds[i], hid_buf, sizeof(hid_buf));

                    if (bytes < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            log_msg(LOG_WARNING, "Read error on hidraw node: %s", strerror(errno));
                            force_reconnect = 1;
                        }
                        continue;
                    }
                    if (bytes == 0) {
                        log_msg(LOG_WARNING, "Hidraw node disconnected.");
                        force_reconnect = 1;
                        break;
                    }

                    Modifier *matched_mod = NULL;
                    if (match_hidraw_active(p, hid_buf, bytes, &matched_mod) && p->driver_state == 0) {
                        log_msg(LOG_INFO, "Profile '%s' modifier '%s' active (hidraw). Grabbing keyboard.", p->id, matched_mod->name);
                        strncpy(p->active_modifier_name, matched_mod->name, sizeof(p->active_modifier_name) - 1);
                        for (int k = 0; k < p->kbd_count; k++) {
                            ioctl(p->kbd_fds[k], EVIOCGRAB, 1);
                            struct input_event flush_ev;
                            while (read(p->kbd_fds[k], &flush_ev, sizeof(flush_ev)) > 0);
                        }
                        p->driver_state = 1;
                        p->pressed_seq_len = 0;
                    }
                    else if (p->driver_state == 1 && match_hidraw_release(p, hid_buf, bytes, p->active_modifier_name)) {
                        log_msg(LOG_INFO, "Profile '%s' modifier '%s' released (hidraw). Latch opened.", p->id, p->active_modifier_name);
                        gettimeofday(&p->latch_timestamp, NULL);
                        p->driver_state = 2;
                    }
                }
            }

            if (force_reconnect) break;

            for (int i = 0; i < p->kbd_count; i++) {
                if (FD_ISSET(p->kbd_fds[i], &readers)) {
                    ssize_t bytes = read(p->kbd_fds[i], &ev, sizeof(ev));

                    if (bytes < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            log_msg(LOG_WARNING, "Read error on keyboard event node: %s", strerror(errno));
                            force_reconnect = 1;
                        }
                        continue;
                    }
                    if (bytes == 0) {
                        log_msg(LOG_WARNING, "Keyboard event node disconnected.");
                        force_reconnect = 1;
                        break;
                    }

                    if (bytes >= (ssize_t)sizeof(ev)) {
                        if (p->driver_state == 0 && ev.type == EV_KEY) {
                            Modifier *matched_mod = NULL;
                            if (match_evdev_active(p, ev.code, ev.value, &matched_mod)) {
                                log_msg(LOG_INFO, "Profile '%s' modifier '%s' active (evdev). Grabbing keyboard.", p->id, matched_mod->name);
                                strncpy(p->active_modifier_name, matched_mod->name, sizeof(p->active_modifier_name) - 1);
                                for (int k = 0; k < p->kbd_count; k++) {
                                    ioctl(p->kbd_fds[k], EVIOCGRAB, 1);
                                    struct input_event flush_ev;
                                    while (read(p->kbd_fds[k], &flush_ev, sizeof(flush_ev)) > 0);
                                }
                                p->driver_state = 1;
                                p->pressed_seq_len = 0;
                                continue;
                            }
                        }

                        if (p->driver_state == 1 && ev.type == EV_KEY && ev.value == 0) {
                            if (match_evdev_release(p, ev.code, ev.value, p->active_modifier_name)) {
                                log_msg(LOG_INFO, "Profile '%s' modifier '%s' released (evdev). Latch opened.", p->id, p->active_modifier_name);
                                gettimeofday(&p->latch_timestamp, NULL);
                                p->driver_state = 2;
                                continue;
                            }
                        }

                        if (p->driver_state > 0 && ev.type == EV_KEY) {
                            if (ev.value == 1) {
                                log_msg(LOG_DEBUG, "Intercepted key down: %d", ev.code);
                                int next_seq[8];
                                int next_seq_len = p->pressed_seq_len;
                                if (next_seq_len < 8) {
                                    for (int s = 0; s < next_seq_len; s++) {
                                        next_seq[s] = p->pressed_seq[s];
                                    }
                                    next_seq[next_seq_len++] = ev.code;
                                }

                                Macro *matched = NULL;
                                int seq_status = check_sequence(p, p->active_modifier_name, next_seq, next_seq_len, &matched);

                                if (seq_status == 1) {
                                    log_msg(LOG_INFO, "Profile '%s' macro triggered. Output type: %s",
                                            p->id, matched->action_type == ACT_REMAP ? "REMAP" : "EXEC");
                                    if (matched->action_type == ACT_REMAP) {
                                        emit_key(uinp_fd, matched->output_keycode, 1);
                                        emit_key(uinp_fd, matched->output_keycode, 0);
                                    } else if (matched->action_type == ACT_EXEC) {
                                        execute_command_bg(matched->output_command);
                                    }

                                    for (int k = 0; k < p->kbd_count; k++) ioctl(p->kbd_fds[k], EVIOCGRAB, 0);
                                    p->driver_state = 0;
                                    p->pressed_seq_len = 0;
                                }
                                else if (seq_status == 2) {
                                    log_msg(LOG_DEBUG, "Key %d matched incomplete macro sequence. Buffering.", ev.code);
                                    p->pressed_seq[p->pressed_seq_len++] = ev.code;
                                    gettimeofday(&p->latch_timestamp, NULL);
                                }
                                else {
                                    log_msg(LOG_INFO, "Key %d broke macro sequence. Flushing input queue.", ev.code);
                                    for (int k = 0; k < p->kbd_count; k++) ioctl(p->kbd_fds[k], EVIOCGRAB, 0);

                                    for (int j = 0; j < p->pressed_seq_len; j++) {
                                        emit_key(uinp_fd, p->pressed_seq[j], 1);
                                        emit_key(uinp_fd, p->pressed_seq[j], 0);
                                    }
                                    emit_key(uinp_fd, ev.code, 1);
                                    emit_key(uinp_fd, ev.code, 0);

                                    p->driver_state = 0;
                                    p->pressed_seq_len = 0;
                                }
                            }
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
