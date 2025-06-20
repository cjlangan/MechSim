#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>

#define MAX_PATH_LENGTH 512
#define AUDIO_BASE_DIR MECHSIM_DATA_DIR "/audio"

// Global variables for cleanup
pid_t keyboard_pid = 0;
pid_t sound_pid = 0;

void print_usage(const char *program_name) {
    printf("MechSim - Mechanical Keyboard Sound Simulator\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");
    printf("  -s, --sound SOUND_NAME   Select sound pack (default: eg-oreo)\n");
    printf("  -V, --volume VOLUME      Set volume [0-100] (default: 50)\n");
    printf("  -l, --list               List available sound packs\n");
    printf("  -h, --help               Show this help message\n");
    printf("  -v, --verbose            Enable verbose output\n");
    printf("\nExamples:\n");
    printf("  %s                       # Use default sound (eg-oreo)\n", program_name);
    printf("  %s -s cherrymx-blue-abs  # Use Cherry MX Blue ABS sound\n", program_name);
    printf("  %s -l                    # List all available sounds\n", program_name);
    printf("\nPress Ctrl+C to exit.\n");
}

int list_sound_packs() {
    DIR *dir;
    struct dirent *entry;
    char path[MAX_PATH_LENGTH];
    struct stat st;

    dir = opendir(AUDIO_BASE_DIR);
    if (dir == NULL) {
        fprintf(stderr, "Error: Cannot open audio directory '%s'\n", AUDIO_BASE_DIR);
        fprintf(stderr, "Make sure you're running from the correct directory.\n");
        return 1;
    }

    printf("Available sound packs:\n");
    printf("======================\n");

    while ((entry = readdir(dir)) != NULL) {
        // Skip . and .. directories
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Build full path
        snprintf(path, sizeof(path), "%s/%s", AUDIO_BASE_DIR, entry->d_name);

        // Use stat to check if it's a directory
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("  %s\n", entry->d_name);
        }
    }

    closedir(dir);
    return 0;
}

int validate_sound_pack(const char *sound_name) {
    char config_path[MAX_PATH_LENGTH];
    char sound_path[MAX_PATH_LENGTH];
    
    snprintf(config_path, sizeof(config_path), "%s/%s/config.json", AUDIO_BASE_DIR, sound_name);
    snprintf(sound_path, sizeof(sound_path), "%s/%s", AUDIO_BASE_DIR, sound_name);
    
    // Check if directory exists
    if (access(sound_path, F_OK) != 0) {
        fprintf(stderr, "Error: Sound pack '%s' not found in %s/\n", sound_name, AUDIO_BASE_DIR);
        fprintf(stderr, "Use --list to see available sound packs.\n");
        return 0;
    }
    
    // Check if config.json exists
    if (access(config_path, R_OK) != 0) {
        fprintf(stderr, "Error: Config file not found: %s\n", config_path);
        return 0;
    }
    
    return 1;
}

void cleanup_processes(int sig) {
    (void)sig; // Suppress unused parameter warning
    
    printf("\nShutting down MechSim...\n");
    
    if (sound_pid > 0) {
        kill(sound_pid, SIGTERM);
        waitpid(sound_pid, NULL, 0);
    }
    
    if (keyboard_pid > 0) {
        kill(keyboard_pid, SIGTERM);
        waitpid(keyboard_pid, NULL, 0);
    }
    
    exit(0);
}

// Function to check if sudo credentials are cached
int check_sudo_cached() {
    int status = system("sudo -n true 2>/dev/null");
    return WEXITSTATUS(status) == 0;
}

// Function to prompt for sudo password if needed
int ensure_sudo_access() {
    if (check_sudo_cached()) {
        return 1; // Already have sudo access
    }
    
    printf("This program requires sudo access to monitor keyboard events.\n");
    printf("Please enter your password when prompted:\n");
    
    int status = system("sudo -v");
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Error: Failed to obtain sudo access\n");
        return 0;
    }
    
    return 1;
}

int main(int argc, char *argv[]) {
    char *sound_name = "eg-oreo"; // Default sound pack
    int verbose = 0;
    int list_sounds = 0;
    
    // Parse command line arguments
    static struct option long_options[] = {
        {"sound",   required_argument, 0, 's'},
        {"volume",  required_argument, 0, 'V'},
        {"list",    no_argument,       0, 'l'},
        {"help",    no_argument,       0, 'h'},
        {"verbose", no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int volume = 50;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "s:V:lhv", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                sound_name = optarg;
                break;
            case 'V':
                volume = atoi(optarg);
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                break;
            case 'l':
                list_sounds = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                verbose = 1;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // Handle list command
    if (list_sounds) {
        return list_sound_packs();
    }
    
    // Validate sound pack
    if (!validate_sound_pack(sound_name)) {
        return 1;
    }
    
    // Ensure sudo access BEFORE forking
    if (!ensure_sudo_access()) {
        return 1;
    }
    
    // Check if required executables exist
    char get_key_presses_path[MAX_PATH_LENGTH];
    char sound_player_path[MAX_PATH_LENGTH];
    
    snprintf(get_key_presses_path, sizeof(get_key_presses_path), "%s/get_key_presses", MECHSIM_BIN_DIR);
    snprintf(sound_player_path, sizeof(sound_player_path), "%s/keyboard_sound_player", MECHSIM_BIN_DIR);
    
    if (access(get_key_presses_path, X_OK) != 0) {
        fprintf(stderr, "Error: Cannot find or execute %s\n", get_key_presses_path);
        return 1;
    }
    
    if (access(sound_player_path, X_OK) != 0) {
        fprintf(stderr, "Error: Cannot find or execute %s\n", sound_player_path);
        return 1;
    }
    
    // Setup signal handler for cleanup
    signal(SIGINT, cleanup_processes);
    signal(SIGTERM, cleanup_processes);
    
    // Build paths
    char config_path[MAX_PATH_LENGTH];
    char sound_dir[MAX_PATH_LENGTH];
    
    snprintf(config_path, sizeof(config_path), "%s/%s/config.json", AUDIO_BASE_DIR, sound_name);
    snprintf(sound_dir, sizeof(sound_dir), "%s/%s", AUDIO_BASE_DIR, sound_name);
    
    if (verbose) {
        printf("MechSim starting...\n");
        printf("Sound pack: %s\n", sound_name);
        printf("Config file: %s\n", config_path);
        printf("Working directory: %s\n", sound_dir);
        printf("Press Ctrl+C to exit.\n\n");
    } else {
        printf("MechSim started with sound pack: %s\n", sound_name);
        printf("Press Ctrl+C to exit.\n");
    }
    
    // Create pipe for communication
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }

    // Fork sound player process FIRST
    sound_pid = fork();
    if (sound_pid == -1) {
        perror("fork");
        return 1;
    }

    if (sound_pid == 0) {
        // Child process: sound player
        close(pipefd[1]); // Close write end
        
        // Redirect stdin from pipe
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        
        // Change to sound directory (so relative paths work)
        if (chdir(sound_dir) != 0) {
            perror("chdir");
            exit(1);
        }

        if (verbose) {
            fprintf(stderr, "Starting sound player...\n");
        }

        char volume_str[32];
        snprintf(volume_str, sizeof(volume_str), "%d", volume);
        execl(sound_player_path, "keyboard_sound_player", "config.json", volume_str, (char *)NULL);
        perror("execl keyboard_sound_player");
        exit(1);
    }

    // Then fork keyboard listener process (sudo)
    keyboard_pid = fork();
    if (keyboard_pid == -1) {
        perror("fork");
        kill(sound_pid, SIGTERM);
        return 1;
    }

    if (keyboard_pid == 0) {
        // Child process: keyboard listener
        close(pipefd[0]); // Close read end

        // Redirect stdout to pipe
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        if (verbose) {
            fprintf(stderr, "Starting keyboard listener...\n");
        }

        // Use -n flag to prevent sudo from prompting again (credentials should be cached)
        char sudo_path[256];
        snprintf(sudo_path, sizeof(sudo_path), "%s/bin/sudo", PACKAGE_PREFIX);
        execl(sudo_path, "sudo", "-n", get_key_presses_path, (char *)NULL);
        perror("execl get_key_presses");
        exit(1);
    }
    
    // Parent process: wait for children
    close(pipefd[0]);
    close(pipefd[1]);
    
    int status;
    pid_t finished_pid;
    
    // Wait for either child to exit
    while ((finished_pid = wait(&status)) > 0) {
        if (finished_pid == keyboard_pid) {
            printf("Keyboard listener exited with status %d\n", WEXITSTATUS(status));
            if (WIFSIGNALED(status)) {
                printf("Keyboard listener killed by signal %d\n", WTERMSIG(status));
            }
            keyboard_pid = 0;
            if (sound_pid > 0) {
                kill(sound_pid, SIGTERM);
            }
        } else if (finished_pid == sound_pid) {
            printf("Sound player exited with status %d\n", WEXITSTATUS(status));
            if (WIFSIGNALED(status)) {
                printf("Sound player killed by signal %d\n", WTERMSIG(status));
            }
            sound_pid = 0;
            if (keyboard_pid > 0) {
                kill(keyboard_pid, SIGTERM);
            }
        }
    }
    
    printf("MechSim exited.\n");
    return 0;
}
