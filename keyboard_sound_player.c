#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <json-c/json.h>
#include <pulse/simple.h>
#include <sndfile.h>

#define MAX_LINE_LENGTH 1024
#define MAX_CONCURRENT_SOUNDS 10

typedef struct {
    int start_ms;
    int duration_ms;
} SoundMapping;

typedef struct {
    char sound_file[256];
    SoundMapping key_mappings[256];  // Index by key_code
    SF_INFO sf_info;
} SoundPack;

typedef struct {
    int key_code;
    SoundPack *sound_pack;
    int thread_id;
} PlaybackData;

// Global sound pack
SoundPack g_sound_pack = {0};
float g_volume = 1.0f;

// Thread pool for sound playback
pthread_t sound_threads[MAX_CONCURRENT_SOUNDS];
volatile int thread_active[MAX_CONCURRENT_SOUNDS] = {0};
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;

int load_sound_config(const char *config_path) {
    FILE *file = fopen(config_path, "r");
    if (!file) {
        fprintf(stderr, "Could not open config file: %s\n", config_path);
        return -1;
    }

    // Read entire file
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char *json_string = malloc(file_size + 1);
    fread(json_string, 1, file_size, file);
    json_string[file_size] = '\0';
    fclose(file);

    // Parse JSON
    json_object *root = json_tokener_parse(json_string);
    if (!root) {
        fprintf(stderr, "Failed to parse JSON config\n");
        free(json_string);
        return -1;
    }

    // Get sound file name
    json_object *sound_obj;
    if (json_object_object_get_ex(root, "sound", &sound_obj)) {
        const char *sound_file = json_object_get_string(sound_obj);
        strncpy(g_sound_pack.sound_file, sound_file, sizeof(g_sound_pack.sound_file) - 1);
        printf("Loaded sound file: %s\n", g_sound_pack.sound_file);
    } else {
        fprintf(stderr, "No 'sound' field found in config\n");
        json_object_put(root);
        free(json_string);
        return -1;
    }

    // Get defines object
    json_object *defines_obj;
    if (json_object_object_get_ex(root, "defines", &defines_obj)) {
        json_object_object_foreach(defines_obj, key, val) {
            int key_code = atoi(key);
            if (key_code > 0 && key_code < 256) {
                if (json_object_is_type(val, json_type_array)) {
                    int array_len = json_object_array_length(val);
                    if (array_len >= 2) {
                        json_object *start_obj = json_object_array_get_idx(val, 0);
                        json_object *duration_obj = json_object_array_get_idx(val, 1);
                        
                        g_sound_pack.key_mappings[key_code].start_ms = json_object_get_int(start_obj);
                        g_sound_pack.key_mappings[key_code].duration_ms = json_object_get_int(duration_obj);
                    }
                }
            }
        }
    }

    json_object_put(root);
    free(json_string);
    return 0;
}

int init_audio() {
    // Check if sound file exists
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
    
    if (key_code >= 256 || g_sound_pack.key_mappings[key_code].duration_ms == 0) {
        pthread_mutex_lock(&thread_mutex);
        thread_active[thread_id] = 0;
        pthread_mutex_unlock(&thread_mutex);
        free(data);
        return NULL;
    }

    SoundMapping *mapping = &g_sound_pack.key_mappings[key_code];
    
    // Open sound file for this thread
    SF_INFO sf_info = g_sound_pack.sf_info;
    SNDFILE *sf = sf_open(g_sound_pack.sound_file, SFM_READ, &sf_info);
    if (!sf) {
        fprintf(stderr, "Thread: Could not open sound file\n");
        pthread_mutex_lock(&thread_mutex);
        thread_active[thread_id] = 0;
        pthread_mutex_unlock(&thread_mutex);
        free(data);
        return NULL;
    }

    // Setup PulseAudio for this thread
    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = sf_info.samplerate,
        .channels = sf_info.channels
    };

    pa_simple *pa_handle = pa_simple_new(NULL, "KeyboardSounds", PA_STREAM_PLAYBACK, 
                                        NULL, "playback", &ss, NULL, NULL, NULL);
    if (!pa_handle) {
        fprintf(stderr, "Thread: Could not initialize PulseAudio\n");
        sf_close(sf);
        pthread_mutex_lock(&thread_mutex);
        thread_active[thread_id] = 0;
        pthread_mutex_unlock(&thread_mutex);
        free(data);
        return NULL;
    }

    // Calculate frames to read
    int sample_rate = sf_info.samplerate;
    int channels = sf_info.channels;
    
    sf_count_t start_frame = (mapping->start_ms * sample_rate) / 1000;
    sf_count_t duration_frames = (mapping->duration_ms * sample_rate) / 1000;
    
    // Seek to start position
    sf_seek(sf, start_frame, SEEK_SET);
    
    // Read and play audio data
    short *buffer = malloc(duration_frames * channels * sizeof(short));
    if (buffer) {
        sf_count_t frames_read = sf_readf_short(sf, buffer, duration_frames);
        
        if (frames_read > 0) {
            for(sf_count_t i = 0; i < frames_read * channels; i++) {
                buffer[i] = (short)(buffer[i] * g_volume);
            }

            int bytes_to_write = frames_read * channels * sizeof(short);
            pa_simple_write(pa_handle, buffer, bytes_to_write, NULL);
            pa_simple_drain(pa_handle, NULL);
        }
        
        free(buffer);
    }

    // Cleanup
    pa_simple_free(pa_handle);
    sf_close(sf);
    
    // Mark thread as finished
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

void play_sound_segment(int key_code) {
    int slot = find_available_thread_slot();
    if (slot == -1) {
        // Too many concurrent sounds, skip this one
        return;
    }

    PlaybackData *data = malloc(sizeof(PlaybackData));
    if (!data) {
        return;
    }
    
    data->key_code = key_code;
    data->sound_pack = &g_sound_pack;
    data->thread_id = slot;

    pthread_mutex_lock(&thread_mutex);
    thread_active[slot] = 1;
    
    if (pthread_create(&sound_threads[slot], NULL, play_sound_thread, data) != 0) {
        thread_active[slot] = 0;
        free(data);
        fprintf(stderr, "Failed to create sound thread\n");
    } else {
        // Detach thread so it cleans up automatically
        pthread_detach(sound_threads[slot]);
    }
    pthread_mutex_unlock(&thread_mutex);
}

int parse_keyboard_event(const char *json_line, int *key_code, int *is_pressed) {
    json_object *root = json_tokener_parse(json_line);
    if (!root) {
        return -1;
    }

    json_object *key_code_obj, *state_code_obj;
    
    if (json_object_object_get_ex(root, "key_code", &key_code_obj) &&
        json_object_object_get_ex(root, "state_code", &state_code_obj)) {
        
        *key_code = json_object_get_int(key_code_obj);
        *is_pressed = json_object_get_int(state_code_obj);
        
        json_object_put(root);
        return 0;
    }

    json_object_put(root);
    return -1;
}

void cleanup() {
    // Give threads a moment to finish naturally
    usleep(100000); // 100ms
    
    // Force cleanup if needed
    pthread_mutex_lock(&thread_mutex);
    for (int i = 0; i < MAX_CONCURRENT_SOUNDS; i++) {
        thread_active[i] = 0;
    }
    pthread_mutex_unlock(&thread_mutex);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <config.json> [volume]\n", argv[0]);
        fprintf(stderr, "  volume: 0.0-5.0 (default: 1.0)\n");
        return 1;
    }

    // Set volume
    if (argc == 3) {
        g_volume = atoi(argv[2]) / 20.0;
        if (g_volume < 0.0f) g_volume = 0.0f;
        if (g_volume > 5.0f) g_volume = 5.0f;
        printf("Volume set to: %.0f%%\n", g_volume);
    }

    // Load sound configuration
    if (load_sound_config(argv[1]) != 0) {
        return 1;
    }

    // Initialize audio
    if (init_audio() != 0) {
        return 1;
    }

    printf("Keyboard sound player initialized. Listening for key events...\n");
    printf("Max concurrent sounds: %d\n", MAX_CONCURRENT_SOUNDS);

    // Read JSON lines from stdin
    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), stdin)) {
        int key_code, is_pressed;
        
        if (parse_keyboard_event(line, &key_code, &is_pressed) == 0) {
            // Only play sound on key press (not release)
            if (is_pressed) {
                play_sound_segment(key_code);
            }
        }
    }

    cleanup();
    return 0;
}
