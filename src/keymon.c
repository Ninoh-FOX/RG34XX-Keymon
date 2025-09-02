#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#define KEY_MENU_LONG 312 // BTN_TL2
#define KEY_VOLUP 115     // KEY_VOLUMEUP
#define KEY_VOLDOWN 114   // KEY_VOLUMEDOWN
#define MAX_STEPS 10
#define MAX_VOLUME 31

// Tabla de brillo posibles valores
static int brightness_values[8] = {5, 10, 20, 50, 70, 140, 200, 255};
static int brightness_level = 3; // nivel índice en brightness_values (0-7)
static int menu_long_pressed = 0;
static int brightness_get_supported = -1; // -1: no probado, 0: no soportado, 1: soportado

// Variables para debouncing y rate limiting
static struct timespec last_brightness_change = {0, 0};
static struct timespec last_menu_event = {0, 0};

#define DISP_LCD_SET_BRIGHTNESS 0x102
#define DISP_LCD_GET_BRIGHTNESS 0x103

// Constantes de timing (en nanosegundos)
#define MIN_BRIGHTNESS_INTERVAL_NS 150000000  // 150ms entre cambios de brillo
#define MIN_MENU_DEBOUNCE_NS 50000000        // 50ms debounce para MENU
#define IOCTL_RETRY_DELAY_US 10000          // 10ms delay entre reintentos
#define MAX_IOCTL_RETRIES 3

// Función para obtener tiempo actual
static void get_current_time(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

// Función para calcular diferencia en nanosegundos
static long long timespec_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000LL + (a->tv_nsec - b->tv_nsec);
}

// Función robusta para setear brillo con reintentos y rate limiting
static int set_brightness_ioctl(int level)
{
    if (level < 0) level = 0;
    if (level > 7) level = 7;

    // Rate limiting: evitar cambios muy rápidos
    struct timespec current_time;
    get_current_time(&current_time);

    if (timespec_diff_ns(&current_time, &last_brightness_change) < MIN_BRIGHTNESS_INTERVAL_NS) {
        return 0; // Ignorar cambio muy rápido
    }

    brightness_level = level;
    last_brightness_change = current_time;

    unsigned long param[4] = {0, (unsigned long)brightness_values[level], 0, 0};

    // Intentar múltiples veces para manejar "Operation not permitted"
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

        if (result == 0) {
            return 0; // Éxito
        }

        // Si falla por EPERM u otros errores recuperables, reintentar
        if (errno == EPERM || errno == EBUSY || errno == EAGAIN) {
            if (retry < MAX_IOCTL_RETRIES - 1) {
                usleep(IOCTL_RETRY_DELAY_US * (retry + 1)); // Backoff exponencial
                continue;
            }
        } else {
            // Error no recuperable
            break;
        }
    }

    return -1;
}

// Función mejorada para leer brillo (solo se llama al inicio)
static int get_brightness_ioctl(int *level)
{
    if (brightness_get_supported == 0) {
        return -1;
    }

    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) {
        brightness_get_supported = 0;
        return -1;
    }

    unsigned long param[4];

    // Probar diferentes formatos de parámetros para drivers Sunxi/Allwinner
    // Intento 1: Formato estándar con display ID
    memset(param, 0, sizeof(param));
    param[0] = 0; // Display ID 0
    if (ioctl(fd, DISP_LCD_GET_BRIGHTNESS, param) == 0) {
        if (param[1] > 0 && param[1] <= 255) {
            goto brightness_found;
        }
        if (param[0] > 0 && param[0] <= 255) {
            param[1] = param[0];
            goto brightness_found;
        }
    }

    // Intento 2: Formato alternativo
    memset(param, 0, sizeof(param));
    param[0] = 1; // Display ID diferente
    if (ioctl(fd, DISP_LCD_GET_BRIGHTNESS, param) == 0) {
        if (param[1] > 0 && param[1] <= 255) {
            goto brightness_found;
        }
    }

    // Si no funciona, marcar como no soportado
    brightness_get_supported = 0;
    close(fd);
    return -1;

brightness_found:
    close(fd);
    brightness_get_supported = 1;

    // Buscar índice correspondiente
    for (int i = 0; i < 8; i++) {
        if (brightness_values[i] == (int)param[1]) {
            *level = i;
            return 0;
        }
    }

    // Buscar el más cercano
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

// Función para manejar eventos con debouncing
static int handle_menu_event(int pressed)
{
    struct timespec current_time;
    get_current_time(&current_time);

    // Debouncing: ignorar eventos muy rápidos
    if (timespec_diff_ns(&current_time, &last_menu_event) < MIN_MENU_DEBOUNCE_NS) {
        return menu_long_pressed; // Retornar estado anterior
    }

    last_menu_event = current_time;
    menu_long_pressed = pressed;
    return menu_long_pressed;
}

// Función para sincronizar brillo inicial (solo una vez)
static void sync_brightness_level(void)
{
    int detected_level;
    if (get_brightness_ioctl(&detected_level) == 0) {
        brightness_level = detected_level;
    }
    // Si no puede leer, mantiene valor por defecto (nivel 3)
}

// Convierte paso 0-10 a valor real 0-31
int stepToVolume(int step) {
    if (step < 0) step = 0;
    if (step > MAX_STEPS) step = MAX_STEPS;
    return (step * MAX_VOLUME) / MAX_STEPS;
}

// Obtiene el volumen actual como paso 0-10
int getVolumeStep() {
    FILE *pipe = popen("amixer get 'lineout volume' | grep -o '[0-9]*%' | head -1", "r");
    if (!pipe) return -1;
    char buf[16];
    if (fgets(buf, sizeof(buf), pipe) != NULL) {
        int percent = 0;
        sscanf(buf, "%d", &percent);
        pclose(pipe);
        int volume = (percent * MAX_VOLUME) / 100;
        int step = (volume * MAX_STEPS + MAX_VOLUME/2) / MAX_VOLUME; // redondeo
        if (step < 0) step = 0;
        if (step > MAX_STEPS) step = MAX_STEPS;
        return step;
    }
    pclose(pipe);
    return -1;
}

// Establece volumen desde paso 0-10
void setVolumeStep(int step) {
    if (step < 0) step = 0;
    if (step > MAX_STEPS) step = MAX_STEPS;
    int vol = stepToVolume(step);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "amixer set 'lineout volume' %d", vol);
    int ret = system(cmd);
	(void)ret;
}

int main()
{
    // Intentar sincronizar con brillo actual al iniciar (una sola vez)
    sync_brightness_level();

    int input_fd = open("/dev/input/event1", O_RDONLY);
    if (input_fd < 0)
        return 1;
    
    int step = getVolumeStep();

    struct input_event ev;

    while (1) {
        ssize_t n = read(input_fd, &ev, sizeof(ev));
        if (n != sizeof(ev))
            continue;

        if (ev.type == EV_KEY) {
            if (ev.value == 1) { // PRESSED
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
            }
            else if (ev.value == 0) { // RELEASED
                if (ev.code == KEY_MENU_LONG) {
                    handle_menu_event(0);
                }
            }
        }
    }

    close(input_fd);
    return 0;
}