#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define KEY_MENU_LONG 312 // BTN_TL2
#define KEY_VOLUP 115     // KEY_VOLUMEUP
#define KEY_VOLDOWN 114   // KEY_VOLUMEDOWN
#define MAX_STEPS 16
#define MAX_VOLUME 31

#define MAX_PATH_SIZE 512

// Archivos de persistencia
#define VOLUME_PERSIST_FILE "/.config/.keymon_volume"
#define LAST_PROCESS_FILE "/.config/.keymon_lastproc"

// Configuración de monitoreo
#define PROCESS_CHECK_INTERVAL 2  // segundos entre verificaciones de procesos
#define MAX_PROC_NAME 64
#define MAX_IGNORED_PROCS 20

// Tabla de brillo
static int brightness_values[8] = {5, 10, 20, 50, 70, 140, 200, 255};
static int brightness_level = 3;
static int menu_long_pressed = 0;
static int brightness_get_supported = -1;

// Variables de timing
static struct timespec last_brightness_change = {0, 0};
static struct timespec last_menu_event = {0, 0};
static struct timespec last_volume_change = {0, 0};
static time_t last_process_check = 0;

// Control de volumen
static int persistent_volume_step = 3;
static int skip_next_restore = 0;

static const char* ignored_processes[] = {
    "keymon", "init", "kthreadd", "ksoftirqd", 
    "migration", "rcu_", "systemd", "dbus", "getty", "sshd",
    "kernel", "worker", "irq/", "mmcqd", "jbd2", "ext4-", 
    "led_workqueue", "cfg80211", "wpa_supplicant", "dhcpcd",
    "NetworkManager", "chronyd", "rsyslog", "cron", "bash", "sh"
};

#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103

// Constantes de timing (en nanosegundos)
#define MIN_BRIGHTNESS_INTERVAL_NS 150000000  
#define MIN_MENU_DEBOUNCE_NS 50000000        
#define MIN_VOLUME_CHANGE_INTERVAL_NS 300000000 // 300ms para evitar loops
#define IOCTL_RETRY_DELAY_US 10000          
#define MAX_IOCTL_RETRIES 3

// Función para obtener tiempo actual
static void get_current_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

// Función para calcular diferencia en nanosegundos
static long long timespec_diff_ns(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec - b->tv_sec) * 1000000000LL + (a->tv_nsec - b->tv_nsec);
}

// Verificar si un proceso debe ser ignorado
static int should_ignore_process(const char* proc_name) {
    int num_ignored = sizeof(ignored_processes) / sizeof(ignored_processes[0]);

    for (int i = 0; i < num_ignored; i++) {
        if (strstr(proc_name, ignored_processes[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

// Obtener el nombre del proceso más reciente (no ignorado)
static int get_newest_process(char* proc_name, size_t name_size) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return 0;

    struct dirent* entry;
    time_t newest_time = 0;
    char newest_name[MAX_PROC_NAME] = {0};
    char comm_path[MAX_PATH_SIZE];
    char comm_name[MAX_PROC_NAME];

    while ((entry = readdir(proc_dir)) != NULL) {
        // Solo directorios que sean números (PIDs)
        if (!isdigit(entry->d_name[0])) continue;

        // Leer el nombre del proceso desde /proc/PID/comm
        snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", entry->d_name);
        FILE* comm_file = fopen(comm_path, "r");
        if (!comm_file) continue;

        if (fgets(comm_name, sizeof(comm_name), comm_file)) {
            // Quitar newline
            char* nl = strchr(comm_name, '\n');
            if (nl) *nl = '\0';

            // Verificar si debemos ignorar este proceso
            if (!should_ignore_process(comm_name)) {
                // Obtener el tiempo de creación del proceso
                char stat_path[MAX_PATH_SIZE];
                snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", entry->d_name);

                struct stat st;
                if (stat(stat_path, &st) == 0) {
                    if (st.st_mtime > newest_time) {
                        newest_time = st.st_mtime;
                        strncpy(newest_name, comm_name, sizeof(newest_name) - 1);
                        newest_name[sizeof(newest_name) - 1] = '\0';
                    }
                }
            }
        }
        fclose(comm_file);
    }

    closedir(proc_dir);

    if (newest_name[0] != '\0') {
        strncpy(proc_name, newest_name, name_size - 1);
        proc_name[name_size - 1] = '\0';
        return 1;
    }

    return 0;
}

// Guardar el último proceso conocido
static void save_last_process(const char* proc_name) {
    FILE* fp = fopen(LAST_PROCESS_FILE, "w");
    if (fp) {
        fprintf(fp, "%s\n", proc_name);
        fclose(fp);
    }
}

// Cargar el último proceso conocido
static int load_last_process(char* proc_name, size_t name_size) {
    FILE* fp = fopen(LAST_PROCESS_FILE, "r");
    if (fp) {
        if (fgets(proc_name, name_size, fp)) {
            char* nl = strchr(proc_name, '\n');
            if (nl) *nl = '\0';
            fclose(fp);
            return 1;
        }
        fclose(fp);
    }
    return 0;
}

// Guardar volumen persistente
static void save_volume_to_file(int step) {
    FILE* fp = fopen(VOLUME_PERSIST_FILE, "w");
    if (fp) {
        fprintf(fp, "%d\n", step);
        fclose(fp);
        persistent_volume_step = step;
    }
}

// Cargar volumen persistente
static int load_volume_from_file(void) {
    FILE* fp = fopen(VOLUME_PERSIST_FILE, "r");
    if (fp) {
        int step = 3;
        if (fscanf(fp, "%d", &step) == 1) {
            fclose(fp);
            if (step < 0) step = 0;
            if (step > MAX_STEPS) step = MAX_STEPS;
            persistent_volume_step = step;
            return step;
        }
        fclose(fp);
    }
    persistent_volume_step = 3;
    return 3;
}

// Función robusta para setear brillo
static int set_brightness_ioctl(int level) {
    if (level < 0) level = 0;
    if (level > 7) level = 7;

    struct timespec current_time;
    get_current_time(&current_time);
    if (timespec_diff_ns(&current_time, &last_brightness_change) < MIN_BRIGHTNESS_INTERVAL_NS) {
        return 0;
    }

    brightness_level = level;
    last_brightness_change = current_time;

    unsigned long param[4] = {0, (unsigned long)brightness_values[level], 0, 0};

    for (int retry = 0; retry < MAX_IOCTL_RETRIES; retry++) {
        int fd = open("/dev/disp", O_RDWR);
        if (fd < 0) {
            if (retry < MAX_IOCTL_RETRIES - 1) {
                usleep(IOCTL_RETRY_DELAY_US);
                continue;
            }
            return -1;
        }

        int result = ioctl(fd, DISP_LCD_SET_BRIGHTNESS, param);
        close(fd);

        if (result == 0) return 0;

        if (errno == EPERM || errno == EBUSY || errno == EAGAIN) {
            if (retry < MAX_IOCTL_RETRIES - 1) {
                usleep(IOCTL_RETRY_DELAY_US * (retry + 1));
                continue;
            }
        } else {
            break;
        }
    }
    return -1;
}

// Función para leer brillo actual (Allwinner)
static int get_brightness_ioctl(int *level) {
    if (brightness_get_supported == 0) return -1;

    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) {
        brightness_get_supported = 0;
        return -1;
    }

    unsigned long param[4];

    // Formato estándar Allwinner
    memset(param, 0, sizeof(param));
    param[0] = 0;
    if (ioctl(fd, DISP_LCD_GET_BRIGHTNESS, param) == 0) {
        if (param[1] > 0 && param[1] <= 255) {
            goto brightness_found;
        }
        if (param[0] > 0 && param[0] <= 255) {
            param[1] = param[0];
            goto brightness_found;
        }
    }

    // Formato alternativo
    memset(param, 0, sizeof(param));
    param[0] = 1;
    if (ioctl(fd, DISP_LCD_GET_BRIGHTNESS, param) == 0) {
        if (param[1] > 0 && param[1] <= 255) {
            goto brightness_found;
        }
    }

    brightness_get_supported = 0;
    close(fd);
    return -1;

brightness_found:
    close(fd);
    brightness_get_supported = 1;

    // Encontrar el índice correspondiente
    for (int i = 0; i < 8; i++) {
        if (brightness_values[i] == (int)param[1]) {
            *level = i;
            return 0;
        }
    }

    // Encontrar el más cercano
    int closest_i = 0;
    int closest_diff = abs(brightness_values[0] - (int)param[1]);
    for (int i = 1; i < 8; i++) {
        int diff = abs(brightness_values[i] - (int)param[1]);
        if (diff < closest_diff) {
            closest_diff = diff;
            closest_i = i;
        }
    }
    *level = closest_i;
    return 0;
}

// Manejo de eventos con debouncing
static int handle_menu_event(int pressed) {
    struct timespec current_time;
    get_current_time(&current_time);

    if (timespec_diff_ns(&current_time, &last_menu_event) < MIN_MENU_DEBOUNCE_NS) {
        return menu_long_pressed;
    }

    last_menu_event = current_time;
    menu_long_pressed = pressed;
    return menu_long_pressed;
}

// Sincronizar brillo inicial
static void sync_brightness_level(void) {
    int detected_level;
    if (get_brightness_ioctl(&detected_level) == 0) {
        brightness_level = detected_level;
    }
}

// Convertir paso a volumen ALSA
int stepToVolume(int step) {
    if (step < 0) step = 0;
    if (step > MAX_STEPS) step = MAX_STEPS;
    return (step * MAX_VOLUME) / MAX_STEPS;
}

// Obtener volumen actual del sistema
int getVolumeStep() {
    FILE* pipe = popen("tinymix get 2 2>/dev/null", "r");
    if (!pipe) return -1;
    char buf[32];
    if (fgets(buf, sizeof(buf), pipe) != NULL) {
        int value = 0;
        sscanf(buf, "%d", &value);
        pclose(pipe);
        int step = (value * MAX_STEPS + MAX_VOLUME/2) / MAX_VOLUME;
        if (step < 0) step = 0;
        if (step > MAX_STEPS) step = MAX_STEPS;
        return step;
    }
    pclose(pipe);
    return -1;
}

// Establecer volumen con rate limiting y persistencia
void setVolumeStep(int step) {
    struct timespec current_time;
    get_current_time(&current_time);

    // Rate limiting
    if (timespec_diff_ns(&current_time, &last_volume_change) < MIN_VOLUME_CHANGE_INTERVAL_NS) {
        return;
    }

    if (step < 0) step = 0;
    if (step > MAX_STEPS) step = MAX_STEPS;

    last_volume_change = current_time;
    skip_next_restore = 1; // Evitar restauración inmediata

    int vol = stepToVolume(step);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "tinymix set 2 %d 2>/dev/null >/dev/null", vol);
    int ret = system(cmd);
    (void)ret;

    // Guardar volumen inmediatamente
    save_volume_to_file(step);

    printf("[keymon] VOL: %d%% (tinymix: %d (range 0->31))\n", (step * 100) / MAX_STEPS, vol);

    // Pequeño delay para estabilizar
    usleep(100000); // 100ms
}

// Función CLAVE: Detectar nuevos procesos y restaurar volumen
static void check_and_restore_on_new_process(void) {
    time_t current_time = time(NULL);

    // Solo verificar cada cierto intervalo
    if (current_time - last_process_check < PROCESS_CHECK_INTERVAL) {
        return;
    }

    last_process_check = current_time;

    // Si acabamos de cambiar el volumen, esperar
    if (skip_next_restore) {
        skip_next_restore = 0;
        return;
    }

    char current_proc[MAX_PROC_NAME];
    char last_proc[MAX_PROC_NAME] = {0};

    // Obtener el proceso más reciente
    if (!get_newest_process(current_proc, sizeof(current_proc))) {
        return;
    }

    // Cargar el último proceso conocido
    load_last_process(last_proc, sizeof(last_proc));

    // Si hay un proceso nuevo diferente
    if (strcmp(current_proc, last_proc) != 0) {
        printf("[keymon] Nueva aplicación detectada: '%s' (anterior: '%s')\n", 
               current_proc, last_proc);

        // Obtener volumen actual del sistema
        int system_volume = getVolumeStep();

        // Si el volumen cambió significativamente, restaurar el persistente
        if (system_volume >= 0) {
            int diff = abs(system_volume - persistent_volume_step);

            if (diff > 1) {
                printf("[keymon] Restaurando volumen: %d%% -> %d%%\n", 
                       (system_volume * 100) / MAX_STEPS, 
                       (persistent_volume_step * 100) / MAX_STEPS);

                // Restaurar volumen guardado
                int vol = stepToVolume(persistent_volume_step);
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "tinymix set 2 %d 2>/dev/null >/dev/null", vol);
                int ret = system(cmd);
                (void)ret;
            }
        }

        // Guardar el nuevo proceso como conocido
        save_last_process(current_proc);
    }
}

// Cleanup al salir
static void cleanup_and_exit(int sig) {
    (void)sig;
    printf("[keymon] Saliendo...\n");
    exit(0);
}

int main() {
    printf("[keymon] Iniciando con auto-restore de volumen para RG34XXM...\n");

    // Configurar señales
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    // Sincronizar brillo inicial
    sync_brightness_level();

    // Abrir device de input
    int input_fd = open("/dev/input/event1", O_RDONLY);
    if (input_fd < 0) {
        printf("[keymon] Error: No se puede abrir /dev/input/event1\n");
        return 1;
    }

    // Cargar volumen persistente
    int step = load_volume_from_file();

    // Verificar y posiblemente restaurar volumen inicial
    int current_system_step = getVolumeStep();
    if (current_system_step >= 0 && abs(current_system_step - step) > 1) {
        printf("[keymon] Restaurando volumen inicial: %d%% -> %d%%\n", 
               (current_system_step * 100) / MAX_STEPS, 
               (step * 100) / MAX_STEPS);
        setVolumeStep(step);
    } else if (current_system_step >= 0) {
        step = current_system_step;
        save_volume_to_file(step);
    }

    // Inicializar detección de procesos
    char initial_proc[MAX_PROC_NAME];
    if (get_newest_process(initial_proc, sizeof(initial_proc))) {
        save_last_process(initial_proc);
        printf("[keymon] Proceso inicial detectado: '%s'\n", initial_proc);
    }

    printf("[keymon] Listo - Volumen: %d%%, Brillo: %d\n", 
           (step * 100) / MAX_STEPS, brightness_level);
    printf("[keymon] Auto-restore activado cada %d segundos\n", PROCESS_CHECK_INTERVAL);

    struct input_event ev;

    while (1) {
        // FUNCIÓN CLAVE: Verificar nuevos procesos y restaurar volumen
        check_and_restore_on_new_process();

        // Leer eventos de input con timeout
        fd_set read_fds;
        struct timeval timeout;

        FD_ZERO(&read_fds);
        FD_SET(input_fd, &read_fds);
        timeout.tv_sec = 1;  // 1 segundo timeout
        timeout.tv_usec = 0;

        int ready = select(input_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (ready < 0) {
            if (errno != EINTR) {
                perror("select");
                break;
            }
            continue;
        }

        if (ready == 0) {
            // Timeout - continuar verificación de procesos
            continue;
        }

        // Leer evento de input
        ssize_t n = read(input_fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;

        if (ev.type == EV_KEY && ev.value == 1) { // PRESSED
            switch (ev.code) {
                case KEY_MENU_LONG:
                    handle_menu_event(1);
                    break;

                case KEY_VOLUP:
                    if (menu_long_pressed) {
                        set_brightness_ioctl(brightness_level + 1);
                    } else {
                        if (step < MAX_STEPS) {
                            step++;
                            setVolumeStep(step);
                        }
                    }
                    break;

                case KEY_VOLDOWN:
                    if (menu_long_pressed) {
                        set_brightness_ioctl(brightness_level - 1);
                    } else {
                        if (step > 0) {
                            step--;
                            setVolumeStep(step);
                        }
                    }
                    break;
            }
        } else if (ev.type == EV_KEY && ev.value == 0) { // RELEASED
            if (ev.code == KEY_MENU_LONG) {
                handle_menu_event(0);
            }
        }
    }

    close(input_fd);
    return 0;
}
