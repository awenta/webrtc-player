#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define MAX_INTERFACES 128

struct interface_info {
    char name[IFNAMSIZ];
    char address[INET_ADDRSTRLEN];
};

struct network_info {
    struct interface_info interfaces[MAX_INTERFACES];
    size_t count;
    size_t default_index;
};

static bool valid_name(const char *name) {
    size_t length;

    if (name == NULL) return false;
    length = strlen(name);
    if (length == 0 || length >= IFNAMSIZ) return false;
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)name[index];
        if (!isalnum(character) && character != '_' && character != '-' &&
                character != '.' && character != ':') return false;
    }
    return true;
}

static int compare_interfaces(const void *left, const void *right) {
    const struct interface_info *first = left;
    const struct interface_info *second = right;
    int result = strcmp(first->name, second->name);

    return result != 0 ? result : strcmp(first->address, second->address);
}

static bool add_interface(struct network_info *network, const char *name,
        const struct sockaddr_in *address) {
    char text[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) == NULL ||
            strcmp(text, "0.0.0.0") == 0) return true;
    for (size_t index = 0; index < network->count; index++) {
        if (strcmp(network->interfaces[index].name, name) != 0) continue;
        /* Preserve kernel address order instead of preferring a lower-numbered alias. */
        return true;
    }
    if (network->count == MAX_INTERFACES) {
        errno = E2BIG;
        return false;
    }
    if (snprintf(network->interfaces[network->count].name, IFNAMSIZ, "%s", name) >= IFNAMSIZ ||
            snprintf(network->interfaces[network->count].address, INET_ADDRSTRLEN, "%s", text) >=
                    INET_ADDRSTRLEN) {
        errno = EOVERFLOW;
        return false;
    }
    network->count++;
    return true;
}

static bool find_default_name(const struct network_info *network, char *name, size_t capacity) {
    FILE *routes = fopen("/proc/net/route", "r");
    char line[512];
    char selected[IFNAMSIZ] = "";
    unsigned long selected_metric = ULONG_MAX;

    if (routes == NULL) return false;
    while (fgets(line, sizeof(line), routes) != NULL) {
        char interface[IFNAMSIZ];
        unsigned long destination;
        unsigned long gateway;
        unsigned long flags;
        unsigned long reference_count;
        unsigned long use;
        unsigned long metric;
        unsigned long mask;
        unsigned long mtu;
        unsigned long window;
        unsigned long irtt;
        bool eligible = false;

        if (sscanf(line, "%15s %lx %lx %lx %lu %lu %lu %lx %lu %lu %lu",
                interface, &destination, &gateway, &flags, &reference_count, &use,
                &metric, &mask, &mtu, &window, &irtt) != 11 || destination != 0 ||
                (flags & 1UL) == 0) continue;
        for (size_t index = 0; index < network->count; index++) {
            if (strcmp(network->interfaces[index].name, interface) == 0) {
                eligible = true;
                break;
            }
        }
        if (eligible && (selected[0] == '\0' || metric < selected_metric ||
                (metric == selected_metric && strcmp(interface, selected) < 0))) {
            selected_metric = metric;
            (void)snprintf(selected, sizeof(selected), "%s", interface);
        }
    }
    fclose(routes);
    if (selected[0] == '\0' || strlen(selected) >= capacity) return false;
    memcpy(name, selected, strlen(selected) + 1);
    return true;
}

static bool discover_network(struct network_info *network) {
    struct ifaddrs *addresses = NULL;
    char default_name[IFNAMSIZ] = "";

    memset(network, 0, sizeof(*network));
    if (getifaddrs(&addresses) != 0) return false;
    for (const struct ifaddrs *current = addresses; current != NULL; current = current->ifa_next) {
        if (current->ifa_addr == NULL || current->ifa_addr->sa_family != AF_INET ||
                (current->ifa_flags & IFF_UP) == 0 || (current->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if (!add_interface(network, current->ifa_name,
                (const struct sockaddr_in *)current->ifa_addr)) {
            freeifaddrs(addresses);
            return false;
        }
    }
    freeifaddrs(addresses);
    if (network->count == 0) {
        errno = ENODEV;
        return false;
    }
    qsort(network->interfaces, network->count, sizeof(network->interfaces[0]), compare_interfaces);
    if (find_default_name(network, default_name, sizeof(default_name))) {
        for (size_t index = 0; index < network->count; index++) {
            if (strcmp(network->interfaces[index].name, default_name) == 0) {
                network->default_index = index;
                break;
            }
        }
    }
    return true;
}

static const struct interface_info *resolve_selector(const struct network_info *network,
        const char *selector) {
    if (strcmp(selector, "auto") == 0) return &network->interfaces[network->default_index];
    if (!valid_name(selector)) return NULL;
    for (size_t index = 0; index < network->count; index++) {
        if (strcmp(network->interfaces[index].name, selector) == 0) return &network->interfaces[index];
    }
    return NULL;
}

static void print_json_string(const char *value) {
    putchar('"');
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor == '"' || *cursor == '\\') printf("\\%c", *cursor);
        else if (*cursor < 0x20 || *cursor >= 0x7f) printf("\\u%04x", *cursor);
        else putchar((int)*cursor);
    }
    putchar('"');
}

static void print_role(const char *name, const char *selector,
        const struct interface_info *interface) {
    print_json_string(name);
    printf(":{\"selector\":");
    print_json_string(selector);
    printf(",\"name\":");
    print_json_string(interface->name);
    printf(",\"address\":");
    print_json_string(interface->address);
    putchar('}');
}

static int print_network_json(const struct network_info *network, const char *mode,
        const char *management_selector, const char *ingest_selector,
        const char *webrtc_selector) {
    const struct interface_info *management = resolve_selector(network, management_selector);
    const struct interface_info *ingest = resolve_selector(network, ingest_selector);
    const struct interface_info *webrtc = resolve_selector(network, webrtc_selector);

    if ((strcmp(mode, "host") != 0 && strcmp(mode, "bridge") != 0) ||
            management == NULL || ingest == NULL || webrtc == NULL) return EXIT_FAILURE;
    printf("{\"mode\":");
    print_json_string(mode);
    printf(",\"defaultInterface\":");
    print_json_string(network->interfaces[network->default_index].name);
    printf(",\"interfaces\":[");
    for (size_t index = 0; index < network->count; index++) {
        if (index > 0) putchar(',');
        printf("{\"name\":");
        print_json_string(network->interfaces[index].name);
        printf(",\"address\":");
        print_json_string(network->interfaces[index].address);
        printf(",\"isDefault\":%s}", index == network->default_index ? "true" : "false");
    }
    printf("],\"effective\":{");
    print_role("management", management_selector, management);
    putchar(',');
    print_role("ingest", ingest_selector, ingest);
    putchar(',');
    print_role("webrtc", webrtc_selector, webrtc);
    printf("}}\n");
    return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s validate SELECTOR | validate-ipv4 ADDRESS | resolve SELECTOR | "
            "json MODE MANAGEMENT INGEST WEBRTC\n", program);
}

int main(int argc, char **argv) {
    struct network_info network;
    const struct interface_info *resolved;

    if (!discover_network(&network)) {
        fprintf(stderr, "network-info: interface discovery failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (argc == 3 && strcmp(argv[1], "validate") == 0) {
        return resolve_selector(&network, argv[2]) == NULL ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 3 && strcmp(argv[1], "validate-ipv4") == 0) {
        struct in_addr address;
        return inet_pton(AF_INET, argv[2], &address) == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 3 && strcmp(argv[1], "resolve") == 0) {
        resolved = resolve_selector(&network, argv[2]);
        if (resolved == NULL) return EXIT_FAILURE;
        printf("%s\t%s\n", resolved->name, resolved->address);
        return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 6 && strcmp(argv[1], "json") == 0) {
        return print_network_json(&network, argv[2], argv[3], argv[4], argv[5]);
    }
    usage(argv[0]);
    return EXIT_FAILURE;
}
