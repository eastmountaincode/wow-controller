#ifndef WOW_POWER_H
#define WOW_POWER_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WOW_POWER_SUPPLIES
#define WOW_POWER_SUPPLIES "/sys/class/power_supply"
#endif
#ifndef WOW_POWER_UDC
#define WOW_POWER_UDC "/sys/class/udc/musb-hdrc.1.auto"
#endif

/* Read a single line from a kernel attribute. Missing attributes are normal. */
static int wow_power_read(int dir, const char *name, char value[64])
{
    if (dir < 0) return 0;
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n;
    do {
        n = read(fd, value, 63);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0 || n == 63) return 0;
    while (n && (value[n - 1] == '\n' || value[n - 1] == '\r')) n--;
    value[n] = '\0';
    return n > 0;
}

static int wow_power_battery(void)
{
    DIR *supplies = opendir(WOW_POWER_SUPPLIES);
    if (!supplies) return -1;
    int battery = -1;
    struct dirent *entry;
    while ((entry = readdir(supplies))) {
        if (entry->d_name[0] == '.') continue;
        int fd = openat(dirfd(supplies), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;
        char type[64];
        if (wow_power_read(fd, "type", type) && !strcmp(type, "Battery")) {
            battery = fd;
            break;
        }
        close(fd);
    }
    closedir(supplies);
    return battery;
}

/* Return only enumerated kernel values when inserting strings into JSON. */
static const char *wow_power_enum(int dir, const char *name, const char *const choices[])
{
    char value[64];
    if (wow_power_read(dir, name, value)) {
        for (size_t i = 0; choices[i]; i++) {
            if (!strcmp(value, choices[i])) return choices[i];
        }
    }
    return NULL;
}

static void wow_power_number(int dir, const char *name, int maximum, char number[16])
{
    strcpy(number, "null");
    char value[64];
    if (!wow_power_read(dir, name, value) || value[0] < '0' || value[0] > '9') return;
    char *end;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno == 0 && *end == '\0' && parsed >= 0 && parsed <= maximum)
        snprintf(number, 16, "%d", (int)parsed);
}

/* USB configuration is a data-link state. This board's device tree does not
 * describe a charger gauge or a VBUS-detect input. Report battery telemetry
 * only when a running kernel actually exports a Battery power_supply. */
static int wow_power_json(char *out, size_t size)
{
    static const char *const links[] = {
        "not attached", "attached", "powered", "reconnecting", "unauthenticated",
        "default", "addressed", "configured", "suspended", NULL
    };
    static const char *const statuses[] = {
        "Charging", "Discharging", "Full", "Not charging", "Unknown", NULL
    };
    int udc = open(WOW_POWER_UDC, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const char *link = wow_power_enum(udc, "state", links);
    if (udc >= 0) close(udc);

    int battery = wow_power_battery();
    char presence[64], capacity[16] = "null", voltage[16] = "null";
    const char *present = "null", *status = NULL;
    if (battery >= 0) {
        int absent = 0;
        if (wow_power_read(battery, "present", presence)) {
            if (!strcmp(presence, "1")) present = "true";
            if (!strcmp(presence, "0")) { present = "false"; absent = 1; }
        }
        if (!absent) {
            wow_power_number(battery, "capacity", 100, capacity);
            wow_power_number(battery, "voltage_now", INT_MAX, voltage);
            status = wow_power_enum(battery, "status", statuses);
        }
        close(battery);
    }

    int n = snprintf(out, size,
        "{\"usb_link\":%s%s%s,\"battery_present\":%s,\"capacity_percent\":%s,"
        "\"voltage_uv\":%s,\"battery_status\":%s%s%s}",
        link ? "\"" : "", link ? link : "null", link ? "\"" : "",
        present, capacity, voltage,
        status ? "\"" : "", status ? status : "null", status ? "\"" : "");
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

#endif
