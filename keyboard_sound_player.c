#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <json-c/json.h>
#include <pulse/simple.h>
#include <sndfile.h>
#include <pulse/error.h>
#include <libgen.h> // For dirname

#define MAX_LINE_LENGTH 1024
#define MAX_CONCURRENT_SOUNDS 10

typedef struct {
    int start_ms;
    int duration_ms;
} SoundMapping;

typedef struct {
    char press_file[256];     // used in multi mode
    char release_file[256];  // used in multi mode
    char generic_press_files[5][256];   // max 5 files GENERIC_R0..R4
    int num_generic_press_files;

    char sound_file[256];    // used in single mode
    SoundMapping key_mappings[256];  // only for 'single' mode

    struct {
        char *press;
        char *release;
    } multi_key_mappings[256];

    int is_multi;
    SF_INFO sf_info;
} SoundPack;

typedef struct {
    int key_code;
    SoundPack *sound_pack;
    int thread_id;
    int is_pressed;
} PlaybackData; 

// Global sound pack
SoundPack g_sound_pack = {0};
float g_volume = 1.0f;
int g_verbose = 0;

// Thread pool for sound playback
pthread_t sound_threads[MAX_CONCURRENT_SOUNDS];
volatile int thread_active[MAX_CONCURRENT_SOUNDS] = {0};
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to construct a full path
static void get_full_path(char *buffer, size_t buffer_size, const char *base_dir, const char *filename) {
    if (filename == NULL || base_dir == NULL) {
        buffer[0] = '\0';
        return;
    }
    // Check if filename is already an absolute path
    if (filename[0] == '/') {
        strncpy(buffer, filename, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
    } else {
        snprintf(buffer, buffer_size, "%s/%s", base_dir, filename);
    }
}


int load_sound_config(const char *config_path) {
    FILE *file = fopen(config_path, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open config file: %s\n", config_path);
        perror("fopen");
        return -1;
    }

    // Extract the directory of the config file
    char config_path_copy[MAX_LINE_LENGTH]; // Use a copy because dirname can modify its argument
    strncpy(config_path_copy, config_path, sizeof(config_path_copy) - 1);
    config_path_copy[sizeof(config_path_copy) - 1] = '\0';
    char *config_dir = dirname(config_path_copy);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *json_string = malloc(size + 1);
    if (!json_string) {
        fclose(file);
        fprintf(stderr, "Error: Memory allocation failed\n");
        return -1;
    }
    
    fread(json_string, 1, size, file);
    json_string[size] = '\0';
    fclose(file);

    json_object *root = json_tokener_parse(json_string);
    free(json_string);
    if (!root) {
        fprintf(stderr, "Error: Invalid JSON in config file\n");
        return -1;
    }

    const char *key_type = "single";
    json_object *obj;
    if (json_object_object_get_ex(root, "key_define_type", &obj))
        key_type = json_object_get_string(obj);

    g_sound_pack.is_multi = strcmp(key_type, "multi") == 0;
    printf("Config loaded: Using %s mode\n", g_sound_pack.is_multi ? "multi" : "single");

    if (g_sound_pack.is_multi) {
        // Reset counter
        g_sound_pack.num_generic_press_files = 0;
        
        if (json_object_object_get_ex(root, "sound", &obj)) {
            const char *pattern = json_object_get_string(obj);
            printf("Sound pattern: %s\n", pattern);
            
            // Check if pattern contains format specifier
            if (strstr(pattern, "%d") || strstr(pattern, "{")) {
                // Handle patterns like "GENERIC_R%d.mp3" or "GENERIC_R{0-4}.mp3"
                for (int i = 0; i <= 4; i++) {
                    char temp_filename[256];
                    if (strstr(pattern, "{")) {
                        // Replace {0-4} with actual number
                        char temp_pattern[256];
                        strncpy(temp_pattern, pattern, sizeof(temp_pattern) - 1);
                        temp_pattern[sizeof(temp_pattern) - 1] = '\0';
                        
                        // Simple replacement of {0-4} with %d
                        char *brace_start = strstr(temp_pattern, "{");
                        char *brace_end = strstr(temp_pattern, "}");
                        if (brace_start && brace_end) {
                            *brace_start = '%';
                            *(brace_start + 1) = 'd';
                            memmove(brace_start + 2, brace_end + 1, strlen(brace_end + 1) + 1);
                        }
                        snprintf(temp_filename, sizeof(temp_filename), temp_pattern, i);
                    } else {
                        snprintf(temp_filename, sizeof(temp_filename), pattern, i);
                    }
                    
                    // Construct the full path using the config directory
                    get_full_path(g_sound_pack.generic_press_files[i], sizeof(g_sound_pack.generic_press_files[i]), config_dir, temp_filename);

                    // Check if file exists before adding to count
                    if (access(g_sound_pack.generic_press_files[i], R_OK) == 0) {
                        g_sound_pack.num_generic_press_files = i + 1;  // Keep track of highest valid index + 1
                        // printf("Found generic sound file: %s\n", g_sound_pack.generic_press_files[i]);
                    } else {
                        printf("Generic sound file not found: %s\n", g_sound_pack.generic_press_files[i]);
                        break;  // Stop at first missing file
                    }
                }
            } else {
                // Direct filename, no pattern
                get_full_path(g_sound_pack.generic_press_files[0], sizeof(g_sound_pack.generic_press_files[0]), config_dir, pattern);
                if (access(g_sound_pack.generic_press_files[0], R_OK) == 0) {
                    g_sound_pack.num_generic_press_files = 1;
                    printf("Found single generic sound file: %s\n", g_sound_pack.generic_press_files[0]);
                }
            }
            
            printf("Total generic press sound files: %d\n", g_sound_pack.num_generic_press_files);
        }
        
        if (json_object_object_get_ex(root, "soundup", &obj)) {
            char temp_release_file[256];
            strncpy(temp_release_file, json_object_get_string(obj), sizeof(temp_release_file) - 1);
            temp_release_file[sizeof(temp_release_file) - 1] = '\0';
            get_full_path(g_sound_pack.release_file, sizeof(g_sound_pack.release_file), config_dir, temp_release_file);
            printf("Release sound file: %s\n", g_sound_pack.release_file);
        }

        if (json_object_object_get_ex(root, "defines", &obj)) {
            // printf("Processing key definitions...\n");
            json_object_object_foreach(obj, key, val) {
                int key_code;
                int is_release = 0;
                char *dash = strstr(key, "-up");
                if (dash) {
                    char key_copy[16];
                    strncpy(key_copy, key, dash - key);
                    key_copy[dash - key] = '\0';
                    key_code = atoi(key_copy);
                    is_release = 1;
                } else {
                    key_code = atoi(key);
                }

                if (key_code >= 0 && key_code < 256) {
                    const char *filename_relative = json_object_get_string(val);
                    char full_filename[MAX_LINE_LENGTH];
                    get_full_path(full_filename, sizeof(full_filename), config_dir, filename_relative);
                    // printf("Key %d (%s): %s (full path: %s)\n", key_code, is_release ? "release" : "press", filename_relative, full_filename);
                    
                    if (is_release) {
                        if (g_sound_pack.multi_key_mappings[key_code].release) {
                            free(g_sound_pack.multi_key_mappings[key_code].release);
                        }
                        g_sound_pack.multi_key_mappings[key_code].release = strdup(full_filename);
                    } else {
                        if (g_sound_pack.multi_key_mappings[key_code].press) {
                            free(g_sound_pack.multi_key_mappings[key_code].press);
                        }
                        g_sound_pack.multi_key_mappings[key_code].press = strdup(full_filename);
                    }
                }
            }
        }
    } else { // Single mode
        if (json_object_object_get_ex(root, "sound", &obj)) {
            char temp_sound_file[256];
            strncpy(temp_sound_file, json_object_get_string(obj), sizeof(temp_sound_file) - 1);
            temp_sound_file[sizeof(temp_sound_file) - 1] = '\0';
            get_full_path(g_sound_pack.sound_file, sizeof(g_sound_pack.sound_file), config_dir, temp_sound_file);
            printf("Single mode sound file: %s\n", g_sound_pack.sound_file);
        }

        if (json_object_object_get_ex(root, "defines", &obj)) {
            json_object_object_foreach(obj, key, val) {
                int key_code = atoi(key);
                if (key_code >= 0 && key_code < 256 && 
                    json_object_is_type(val, json_type_array) &&
                    json_object_array_length(val) >= 2) {
                    g_sound_pack.key_mappings[key_code].start_ms = json_object_get_int(json_object_array_get_idx(val, 0));
                    g_sound_pack.key_mappings[key_code].duration_ms = json_object_get_int(json_object_array_get_idx(val, 1));
                }
            }
        }
    }

    json_object_put(root);
    return 0;
}

int init_audio() {
    // For multi mode, we don't need to check a single sound file
    if (g_sound_pack.is_multi) {
        // printf("Multi mode: Audio will be initialized per sound file\n");
        return 0;
    }
    
    // For single mode, check the main sound file
    if (strlen(g_sound_pack.sound_file) == 0) {
        fprintf(stderr, "Error: No sound file specified in config\n");
        return -1;
    }
    
    if (access(g_sound_pack.sound_file, R_OK) != 0) {
        fprintf(stderr, "Sound file not accessible: %s\n", g_sound_pack.sound_file);
        perror("access");
        return -1;
    }

    // Test open sound file to get info
    SNDFILE *test_sf = sf_open(g_sound_pack.sound_file, SFM_READ, &g_sound_pack.sf_info);
    if (!test_sf) {
        fprintf(stderr, "Could not open sound file: %s\n", g_sound_pack.sound_file);
        fprintf(stderr, "libsndfile error: %s\n", sf_strerror(NULL));
        return -1;
    }
    sf_close(test_sf);

    printf("Sound file info: %ld frames, %d channels, %d Hz\n", 
           g_sound_pack.sf_info.frames, g_sound_pack.sf_info.channels, g_sound_pack.sf_info.samplerate);

    return 0;
}

void* play_sound_thread(void* arg) {
    PlaybackData *data = (PlaybackData*)arg;
    int key_code = data->key_code;
    int thread_id = data->thread_id;
    int is_pressed = data->is_pressed;

    const char *file_to_play = NULL;

    if (g_verbose) {
        printf("Thread %d: Playing sound for key %d (%s)\n", 
               thread_id, key_code, is_pressed ? "press" : "release");
    }

    if (g_sound_pack.is_multi) {
        // First try exact match
        if (is_pressed && g_sound_pack.multi_key_mappings[key_code].press) {
            file_to_play = g_sound_pack.multi_key_mappings[key_code].press;
        } else if (!is_pressed && g_sound_pack.multi_key_mappings[key_code].release) {
            file_to_play = g_sound_pack.multi_key_mappings[key_code].release;
        } else if (is_pressed && g_sound_pack.num_generic_press_files > 0) {
            // Fallback: random generic press
            int idx = rand() % g_sound_pack.num_generic_press_files;
            file_to_play = g_sound_pack.generic_press_files[idx];
        } else if (!is_pressed && strlen(g_sound_pack.release_file) > 0) {
            file_to_play = g_sound_pack.release_file;
        }

        if (!file_to_play) {
            if (g_verbose) {
                printf("Thread %d: No sound file found for key %d (%s)\n", 
                       thread_id, key_code, is_pressed ? "press" : "release");
            }
            goto exit_cleanup;
        }

        if (g_verbose) {
            printf("Thread %d: Using sound file: %s\n", thread_id, file_to_play);
        }

        SF_INFO sf_info = {0};
        SNDFILE *sf = sf_open(file_to_play, SFM_READ, &sf_info);
        if (!sf) {
            fprintf(stderr, "Could not open sound file: %s (Error: %s)\n", 
                    file_to_play, sf_strerror(NULL));
            goto exit_cleanup;
        }

        pa_sample_spec ss = {
            .format = PA_SAMPLE_S16LE,
            .rate = sf_info.samplerate,
            .channels = sf_info.channels
        };

        int pa_error;
        pa_simple *pa_handle = pa_simple_new(NULL, "KeyboardSounds", PA_STREAM_PLAYBACK,
                                             NULL, "playback", &ss, NULL, NULL, &pa_error);
        if (!pa_handle) {
            fprintf(stderr, "Could not initialize PulseAudio: %s\n", pa_strerror(pa_error));
            sf_close(sf);
            goto exit_cleanup;
        }

        int frames = 2048;
        short *buffer = malloc(frames * sf_info.channels * sizeof(short));
        if (!buffer) {
            pa_simple_free(pa_handle);
            sf_close(sf);
            goto exit_cleanup;
        }

        sf_count_t read;
        while ((read = sf_readf_short(sf, buffer, frames)) > 0) {
            for (sf_count_t i = 0; i < read * sf_info.channels; i++) {
                buffer[i] = (short)(buffer[i] * g_volume);
            }
            
            int pa_write_error;
            if (pa_simple_write(pa_handle, buffer, read * sf_info.channels * sizeof(short), &pa_write_error) < 0) {
                fprintf(stderr, "PulseAudio write error: %s\n", pa_strerror(pa_write_error));
                break;
            }
        }

        int pa_drain_error;
        pa_simple_drain(pa_handle, &pa_drain_error);
        pa_simple_free(pa_handle);
        sf_close(sf);
        free(buffer);
    } else {
        if (key_code >= 256 || g_sound_pack.key_mappings[key_code].duration_ms == 0) {
            if (g_verbose) {
                printf("Thread %d: No mapping for key %d\n", thread_id, key_code);
            }
            goto exit_cleanup;
        }

        SoundMapping *mapping = &g_sound_pack.key_mappings[key_code];

        SF_INFO sf_info = g_sound_pack.sf_info;
        SNDFILE *sf = sf_open(g_sound_pack.sound_file, SFM_READ, &sf_info);
        if (!sf) {
            fprintf(stderr, "Thread: Could not open sound file\n");
            goto exit_cleanup;
        }

        pa_sample_spec ss = {
            .format = PA_SAMPLE_S16LE,
            .rate = sf_info.samplerate,
            .channels = sf_info.channels
        };

        int pa_error;
        pa_simple *pa_handle = pa_simple_new(NULL, "KeyboardSounds", PA_STREAM_PLAYBACK,
                                             NULL, "playback", &ss, NULL, NULL, &pa_error);
        if (!pa_handle) {
            sf_close(sf);
            fprintf(stderr, "Thread: Could not initialize PulseAudio: %s\n", pa_strerror(pa_error));
            goto exit_cleanup;
        }

        sf_count_t start_frame = (mapping->start_ms * sf_info.samplerate) / 1000;
        sf_count_t duration_frames = (mapping->duration_ms * sf_info.samplerate) / 1000;

        sf_seek(sf, start_frame, SEEK_SET);

        short *buffer = malloc(duration_frames * sf_info.channels * sizeof(short));
        if (!buffer) {
            pa_simple_free(pa_handle);
            sf_close(sf);
            goto exit_cleanup;
        }
        
        sf_count_t frames_read = sf_readf_short(sf, buffer, duration_frames);

        for (sf_count_t i = 0; i < frames_read * sf_info.channels; i++) {
            buffer[i] = (short)(buffer[i] * g_volume);
        }

        int pa_write_error, pa_drain_error;
        pa_simple_write(pa_handle, buffer, frames_read * sf_info.channels * sizeof(short), &pa_write_error);
        pa_simple_drain(pa_handle, &pa_drain_error);

        free(buffer);
        pa_simple_free(pa_handle);
        sf_close(sf);
    }

exit_cleanup:
    pthread_mutex_lock(&thread_mutex);
    thread_active[thread_id] = 0;
    pthread_mutex_unlock(&thread_mutex);
    free(data);
    return NULL;
}

int find_available_thread_slot() {
    pthread_mutex_lock(&thread_mutex);
    
    // Find available slot
    for (int i = 0; i < MAX_CONCURRENT_SOUNDS; i++) {
        if (!thread_active[i]) {
            pthread_mutex_unlock(&thread_mutex);
            return i;
        }
    }
    
    pthread_mutex_unlock(&thread_mutex);
    return -1; // No available slots
}

void play_sound_segment(int key_code, int is_pressed) {
    // Only play sound on key press in single mode
    if (!g_sound_pack.is_multi && !is_pressed) {
        if (g_verbose) {
            printf("Single mode: Ignoring key release for key %d\n", key_code);
        }
        return;
    }

    int slot = find_available_thread_slot();
    if (slot == -1) {
        if (g_verbose) {
            printf("Warning: No available thread slots\n");
        }
        return;
    }

    PlaybackData *data = malloc(sizeof(PlaybackData));
    if (!data) return;

    data->key_code = key_code;
    data->sound_pack = &g_sound_pack;
    data->thread_id = slot;
    data->is_pressed = is_pressed;

    pthread_mutex_lock(&thread_mutex);
    thread_active[slot] = 1;

    if (pthread_create(&sound_threads[slot], NULL, play_sound_thread, data) != 0) {
        thread_active[slot] = 0;
        free(data);
        fprintf(stderr, "Failed to create sound thread\n");
    } else {
        pthread_detach(sound_threads[slot]);
    }

    pthread_mutex_unlock(&thread_mutex);
}

int parse_keyboard_event(const char *json_line, int *key_code, int *is_pressed) {
    // Strip newline if present
    char *line_copy = strdup(json_line);
    if (!line_copy) return -1;
    
    char *newline = strchr(line_copy, '\n');
    if (newline) *newline = '\0';
    
    if (g_verbose) {
        printf("Parsing JSON: %s\n", line_copy);
    }

    json_object *root = json_tokener_parse(line_copy);
    free(line_copy);
    
    if (!root) {
        fprintf(stderr, "Failed to parse JSON: %s\n", json_line);
        return -1;
    }

    json_object *key_code_obj, *state_code_obj;
    
    if (json_object_object_get_ex(root, "key_code", &key_code_obj) &&
        json_object_object_get_ex(root, "state_code", &state_code_obj)) {
        
        *key_code = json_object_get_int(key_code_obj);
        *is_pressed = json_object_get_int(state_code_obj);
        
        if (g_verbose) {
            printf("Parsed key event: key_code=%d, is_pressed=%d\n", *key_code, *is_pressed);
        }
        
        json_object_put(root);
        return 0;
    }

    json_object_put(root);
    return -1;
}

void cleanup() {
    printf("Cleaning up...\n");
    
    // Give threads a moment to finish naturally
    usleep(500000); // 500ms

    // Free dynamically allocated filenames in multi config
    for (int i = 0; i < 256; i++) {
        if (g_sound_pack.multi_key_mappings[i].press) {
            free(g_sound_pack.multi_key_mappings[i].press);
            g_sound_pack.multi_key_mappings[i].press = NULL;
        }
        if (g_sound_pack.multi_key_mappings[i].release) {
            free(g_sound_pack.multi_key_mappings[i].release);
            g_sound_pack.multi_key_mappings[i].release = NULL;
        }
    }

    // Reset thread state
    pthread_mutex_lock(&thread_mutex);
    for (int i = 0; i < MAX_CONCURRENT_SOUNDS; i++) {
        thread_active[i] = 0;
    }
    pthread_mutex_unlock(&thread_mutex);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: %s <config.json> [volume] [verbose]\n", argv[0]);
        fprintf(stderr, "  volume: 0-100 (default: 50)\n");
        fprintf(stderr, "  verbose: 1 to enable verbose output (default: 0)\n");
        return 1;
    }

    // Set volume
    if (argc >= 3) {
        int volume_percent = atoi(argv[2]);
        if (volume_percent < 0) volume_percent = 0;
        if (volume_percent > 100) volume_percent = 100;
        g_volume = volume_percent / 100.0f;
        printf("Volume set to: %d%%\n", volume_percent);
    } else {
        printf("Volume set to: 50%% (default)\n");
    }
    
    // Set verbose mode
    if (argc >= 4) {
        g_verbose = atoi(argv[3]);
        if (g_verbose) {
            printf("Verbose mode enabled\n");
        }
    }

    // Load sound configuration
    if (load_sound_config(argv[1]) != 0) {
        fprintf(stderr, "Failed to load sound configuration\n");
        return 1;
    }

    // Initialize audio
    if (init_audio() != 0) {
        fprintf(stderr, "Failed to initialize audio\n");
        return 1;
    }

    // printf("Keyboard sound player initialized. Listening for key events...\n");
    // printf("Max concurrent sounds: %d\n", MAX_CONCURRENT_SOUNDS);
    // printf("Waiting for input on stdin...\n");
    // fflush(stdout);

    // Add timeout for debugging
    fd_set readfds;
    struct timeval timeout;
    
    // Read JSON lines from stdin with timeout
    char line[MAX_LINE_LENGTH];
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        timeout.tv_sec = 1;  // 1 second timeout
        timeout.tv_usec = 0;
        
        int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        
        if (ready == -1) {
            perror("select");
            break;
        } else if (ready == 0) {
            // Timeout - continue waiting
            if (g_verbose) {
                printf("Waiting for input...\n");
            }
            continue;
        }
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                if (feof(stdin)) {
                    printf("EOF reached on stdin\n");
                } else {
                    perror("fgets");
                }
                break;
            }
            
            int key_code, is_pressed;
            if (parse_keyboard_event(line, &key_code, &is_pressed) == 0) {
                play_sound_segment(key_code, is_pressed);
            }
        }
    }

    cleanup();
    return 0;
}
