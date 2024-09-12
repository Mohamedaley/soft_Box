#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <dirent.h>
#include <termios.h>
#include <pty.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/sched.h>
#include <errno.h>
#include <utmp.h>
#include <errno.h>
#define MAX_BUF 1024
#define CONTAINER_FILE "/var/lib/softbox/containers.dat"
#define MAX_CONTAINERS 10
#define CONTAINER_ID_LENGTH 12
#define IMAGE_DIR "/var/lib/softbox/images"
#define MAX_CMD_LENGTH 1024
#define MAX_PATH_LENGTH 256
#define VETH_PREFIX "veth"
#define CONTAINER_IFNAME "eth0"

typedef struct {
    char id[CONTAINER_ID_LENGTH + 1];
    pid_t pid;
    char image[256];
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

int keep_container_running();
int check_container_status(char* container_id);
int exec_in_container(char* container_id, char* command);
int setup_container_cgroups(const char* container_id);
void save_containers();
void load_containers();
static char* generate_container_id();
int run_container(char* image);
int list_containers();
int stop_container(char* id);
int setup_container_fs(const char* image);
int setup_image_store();
int pull_image(const char* image_name);
int list_images();
char* get_image_path(const char* image_name);
int setup_container_namespaces();
int setup_container_network();
void print_usage();
int handle_run_command(int argc, char *argv[]);
int handle_list_command();
int handle_stop_command(int argc, char *argv[]);
int handle_images_command();
int handle_pull_command(int argc, char *argv[]);
int handle_exec_command(int argc, char *argv[]);
void cleanup_exited_containers();
bool has_container_exited(container_t container);

int keep_container_running() {
    printf("Container is running. Press Ctrl+C to stop.\n");
    while(1) {
        sleep(1);
    }
    return 0;
}

bool has_container_exited(container_t container) {
    // Check if the process with the given PID is still running
    if (kill(container.pid, 0) == -1) {
        if (errno == ESRCH) {
            // Process does not exist
            return true;
        }
    }
    // Process exists
    return false;
}


int exec_in_container(char* container_id, char* command) {
    int container_pid = -1;
    
    // Find the container with the given ID
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, container_id) == 0) {
            container_pid = containers[i].pid;
            break;
        }
    }
    
    if (container_pid == -1) {
        fprintf(stderr, "Error: Container not found or has exited\n");
        return 1;
    }

    pid_t child_pid = fork();
    if (child_pid == -1) {
        perror("fork");
        return 1;
    }

    if (child_pid == 0) {
        // Child process
        char nspath[256];
        snprintf(nspath, sizeof(nspath), "/proc/%d/ns/pid", container_pid);
        int fd = open(nspath, O_RDONLY);
        if (fd == -1) {
            perror("open namespace");
            exit(1);
        }
        
        if (setns(fd, 0) == -1) {
            perror("setns");
            exit(1);
        }
        
        close(fd);

        // Execute the command in the container
        char *args[] = {"/bin/sh", "-c", command, NULL};
        if (execvp(args[0], args) == -1) {
            perror("execvp");
            exit(1);
        }
    } else {
        // Parent process
        int status;
        waitpid(child_pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    return 0;
}


int setup_container_fs(const char* image) {
    char buf[MAX_PATH_LENGTH];
    char* rootfs = get_image_path(image);

    // Change root
    if (chroot(rootfs) == -1) {
        perror("chroot");
        return -1;
    }

    // Change directory to new root
    if (chdir("/") == -1) {
        perror("chdir");
        return -1;
    }

    // Mount proc filesystem
    if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
        perror("mount proc");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    // Load existing containers at startup
    load_containers();

    if (argc < 2) {
        print_usage();
        return 1;
    }

    char *command = argv[1];

    if (strcmp(command, "run") == 0) {
        return handle_run_command(argc, argv);
    } else if (strcmp(command, "exec") == 0) {
        return handle_exec_command(argc, argv);
    } else if (strcmp(command, "list") == 0) {
        return handle_list_command();
    } else if (strcmp(command, "stop") == 0) {
        return handle_stop_command(argc, argv);
    } else if (strcmp(command, "images") == 0) {
        return handle_images_command();
    } else if (strcmp(command, "pull") == 0) {
        return handle_pull_command(argc, argv);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        print_usage();
        return 1;
    }

    return 0;
}

int handle_exec_command(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Error: Missing container ID or command\n");
        return 1;
    }
    char *id = argv[2];
    char *command = argv[3];
    return exec_in_container(id, command);
}

int setup_container_cgroups(const char* container_id) {
    char cgroup_path[MAX_PATH_LENGTH];
    char file_path[MAX_PATH_LENGTH];
    int fd;

    // Create cgroup
    if (snprintf(cgroup_path, sizeof(cgroup_path), "/sys/fs/cgroup/softbox_%s", container_id) >= sizeof(cgroup_path)) {
        fprintf(stderr, "Cgroup path too long\n");
        return -1;
    }
    if (mkdir(cgroup_path, 0755) == -1) {
        if (errno != EEXIST) {
            fprintf(stderr, "Failed to create cgroup directory %s: %s\n", cgroup_path, strerror(errno));
            return -1;
        }
    }

    // Enable controllers
    if (snprintf(file_path, sizeof(file_path), "%s/cgroup.subtree_control", cgroup_path) >= sizeof(file_path)) {
        fprintf(stderr, "File path too long for cgroup.subtree_control\n");
        return -1;
    }
    fd = open(file_path, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Failed to open %s: %s\n", file_path, strerror(errno));
        return -1;
    }
    if (write(fd, "+memory +pids", 13) == -1) {
        fprintf(stderr, "Failed to enable controllers: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    // Set memory limit (e.g., 100MB)
    if (snprintf(file_path, sizeof(file_path), "%s/memory.max", cgroup_path) >= sizeof(file_path)) {
        fprintf(stderr, "File path too long for memory.max\n");
        return -1;
    }
    fd = open(file_path, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Failed to open %s: %s\n", file_path, strerror(errno));
        return -1;
    }
    if (write(fd, "100M", 4) == -1) {
        fprintf(stderr, "Failed to write to %s: %s\n", file_path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    // Add current process to cgroup
    if (snprintf(file_path, sizeof(file_path), "%s/cgroup.procs", cgroup_path) >= sizeof(file_path)) {
        fprintf(stderr, "File path too long for cgroup.procs\n");
        return -1;
    }
    fd = open(file_path, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Failed to open %s: %s\n", file_path, strerror(errno));
        return -1;
    }
    char pid_str[16];
    int pid_str_len = snprintf(pid_str, sizeof(pid_str), "%d", getpid());
    if (pid_str_len < 0 || pid_str_len >= sizeof(pid_str)) {
        fprintf(stderr, "Failed to convert PID to string\n");
        close(fd);
        return -1;
    }
    if (write(fd, pid_str, pid_str_len) == -1) {
        fprintf(stderr, "Failed to write to cgroup.procs: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}   

void save_containers() {
    FILE *file = fopen(CONTAINER_FILE, "w");
    if (file == NULL) {
        perror("Error opening file for writing");
        return;
    }
    for (int i = 0; i < container_count; i++) {
        fprintf(file, "%s %d %s\n", containers[i].id, containers[i].pid, containers[i].image);
    }
    fclose(file);
}

void load_containers() {
    FILE *file = fopen(CONTAINER_FILE, "r");
    if (file == NULL) {
        // It's okay if the file doesn't exist yet
        return;
    }
    container_count = 0;
    while (fscanf(file, "%s %d %s", containers[container_count].id, &containers[container_count].pid, containers[container_count].image) == 3) {
        container_count++;
        if (container_count >= MAX_CONTAINERS) break;
    }
    fclose(file);
}

static char* generate_container_id() {
    static char id[CONTAINER_ID_LENGTH + 1];
    const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    
    srand(time(NULL));
    for (int i = 0; i < CONTAINER_ID_LENGTH; i++) {
        int index = rand() % (sizeof(charset) - 1);
        id[i] = charset[index];
    }
    id[CONTAINER_ID_LENGTH] = '\0';
    
    return id;
}

int run_container(char* image) {
    if (container_count >= MAX_CONTAINERS) {
        fprintf(stderr, "Error: Maximum number of containers reached\n");
        return 1;
    }

    char* container_id = generate_container_id();
    
    pid_t child_pid = fork();
    if (child_pid == -1) {
        perror("fork");
        return 1;
    }

    if (child_pid == 0) {
        // Child process
        if (setup_container_namespaces() == -1 ||
            setup_container_fs(image) == -1) {
            exit(1);
        }

        // Keep the container running
        return keep_container_running();
    } else {
        // Parent process
        container_t new_container;
        strncpy(new_container.id, container_id, CONTAINER_ID_LENGTH + 1);
        new_container.pid = child_pid;
        strncpy(new_container.image, image, 256);
        containers[container_count++] = new_container;

        printf("Container started: %s\n", container_id);
        save_containers();
        return 0;
    }
}


int list_containers() {
    cleanup_exited_containers();
    printf("CONTAINER ID\tIMAGE\t\tPID\t\tSTATUS\n");
    for (int i = 0; i < container_count; i++) {
        char status[10];
        if (kill(containers[i].pid, 0) == 0) {
            strcpy(status, "Running");
        } else {
            strcpy(status, "Exited");
        }
        printf("%s\t%s\t\t%d\t\t%s\n", containers[i].id, containers[i].image, containers[i].pid, status);
    }
    return 0;
}

int stop_container(char* id) {
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0) {
            if (kill(containers[i].pid, SIGTERM) == -1) {
                perror("kill");
                return 1;
            }
            
            // Wait for the process to exit
            int status;
            if (waitpid(containers[i].pid, &status, 0) == -1) {
                perror("waitpid");
                return 1;
            }
            
            // Remove container from the list
            for (int j = i; j < container_count - 1; j++) {
                containers[j] = containers[j + 1];
            }
            container_count--;
            
            printf("Container stopped and removed: %s\n", id);
            save_containers();
            return 0;
        }
    }
    
    fprintf(stderr, "Error: Container not found\n");
    return 1;
}

int setup_image_store() {
    struct stat st = {0};
    if (stat(IMAGE_DIR, &st) == -1) {
        if (mkdir(IMAGE_DIR, 0700) == -1) {
            perror("Failed to create image directory");
            return -1;
        }
    }
    return 0;
}

int pull_image(const char* image_name) {
    char cmd[MAX_CMD_LENGTH];
    
    // Pull the image using Docker
    snprintf(cmd, sizeof(cmd), "docker pull %s", image_name);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to pull image: %s\n", image_name);
        return -1;
    }
    
    // Create a directory for the image
    char image_dir[MAX_PATH_LENGTH];
    snprintf(image_dir, sizeof(image_dir), "%s/%s", IMAGE_DIR, image_name);
    if (mkdir(image_dir, 0700) == -1) {
        perror("Failed to create image directory");
        return -1;
    }
    
    // Save the image to our custom directory
    snprintf(cmd, sizeof(cmd), "docker save %s | tar -x -C %s", image_name, image_dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to save image: %s\n", image_name);
        return -1;
    }
    
    printf("Image pulled successfully: %s\n", image_name);
    return 0;
}

int list_images() {
    DIR *dir;
    struct dirent *ent;
    
    if ((dir = opendir(IMAGE_DIR)) != NULL) {
        printf("AVAILABLE IMAGES:\n");
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR && strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                printf("%s\n", ent->d_name);
            }
        }
        closedir(dir);
    } else {
        perror("Failed to open image directory");
        return -1;
    }
    
    return 0;
}

char* get_image_path(const char* image_name) {
    static char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, image_name);
    return path;
}


int handle_run_command(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Error: Missing image name\n");
        return 1;
    }
    char *image = argv[2];
    return run_container(image);
}

int handle_list_command(void) {
    return list_containers();
}

int handle_stop_command(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Error: Missing container ID\n");
        return 1;
    }
    char *id = argv[2];
    return stop_container(id);
}

int handle_images_command(void) {
    return list_images();
}

int handle_pull_command(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Error: Missing image name\n");
        return 1;
    }
    char *image = argv[2];
    return pull_image(image);
}

void print_usage(void) {
    printf("Usage: softbox <command> [args...]\n");
    printf("Commands:\n");
    printf("  exec <id> <command>  Execute a command in a running container\n");
    printf("  run <image>     Run a new container\n");
    printf("  list            List running containers\n");
    printf("  stop <id>       Stop a running container\n");
    printf("  images          List available images\n");
    printf("  pull <image>    Pull a new image\n");
}

int setup_container_namespaces() {
    if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET) == -1) {
        perror("unshare");
        return -1;
    }
    return 0;
}


int setup_container_network() {
    // Create a veth pair
    char veth_name[IFNAMSIZ];
    snprintf(veth_name, IFNAMSIZ, "%s%d", VETH_PREFIX, getpid());
    
    if (system("ip link add veth0 type veth peer name veth1") != 0) {
        perror("Failed to create veth pair");
        return -1;
    }

    // Move veth1 to container network namespace
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip link set veth1 netns %d", getpid());
    if (system(cmd) != 0) {
        perror("Failed to move veth1 to container namespace");
        return -1;
    }

    // Set up IP address for veth0 (host side)
    if (system("ip addr add 10.0.0.1/24 dev veth0") != 0) {
        perror("Failed to set IP for veth0");
        return -1;
    }

    // Set up IP address for veth1 (container side)
    if (system("ip netns exec $(basename `ip netns identify`) ip addr add 10.0.0.2/24 dev veth1") != 0) {
        perror("Failed to set IP for veth1");
        return -1;
    }

    // Bring up the interfaces
    if (system("ip link set veth0 up") != 0 ||
        system("ip netns exec $(basename `ip netns identify`) ip link set veth1 up") != 0) {
        perror("Failed to bring up interfaces");
        return -1;
    }

    // Set up NAT on the host
    if (system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE") != 0) {
        perror("Failed to set up NAT");
        return -1;
    }

    return 0;
}

int check_container_status(char* container_id) {
    cleanup_exited_containers();  // Clean up before checking status
    
    for (int i = 0; i < container_count; i++) {
        if (container_id == NULL || strcmp(containers[i].id, container_id) == 0) {
            printf("Container ID: %s\n", containers[i].id);
            printf("Image: %s\n", containers[i].image);
            printf("PID: %d\n", containers[i].pid);
            
            if (kill(containers[i].pid, 0) == 0) {
                printf("Status: Running\n");
            } else {
                printf("Status: Exited\n");
            }
            
            printf("\n");
            
            if (container_id != NULL) {
                return 0;  // Found the specific container
            }
        }
    }
    
    if (container_id != NULL) {
        fprintf(stderr, "Error: Container not found\n");
        return 1;
    }
    
    return 0;
}
void cleanup_exited_containers() {
    int i = 0;
    while (i < container_count) {
        if (kill(containers[i].pid, 0) == -1 && errno == ESRCH) {
            // Process doesn't exist, remove it from the list
            printf("Removing exited container: %s (PID: %d)\n", containers[i].id, containers[i].pid);
            for (int j = i; j < container_count - 1; j++) {
                containers[j] = containers[j + 1];
            }
            container_count--;
        } else {
            i++;
        }
    }
    save_containers();  // Save the updated container list
}
