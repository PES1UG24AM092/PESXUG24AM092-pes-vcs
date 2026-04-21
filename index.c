// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Forward declaration to link object_write safely
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Helper function to sort index entries by path for saving
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Load the index from .pes/index.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // If it doesn't exist, it just means no files are staged yet.

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        IndexEntry *entry = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 1];

        // Parse: <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
        if (sscanf(line, "%o %64s %lu %u %[^\n]", 
                   &entry->mode, hex, (unsigned long *)&entry->mtime_sec, 
                   &entry->size, entry->path) == 5) {
            if (hex_to_hash(hex, &entry->hash) == 0) {
                index->count++;
            }
        }
    }
    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
int index_save(const Index *index) {
    // 1. We must sort the index entries by path before writing
    // We allocate dynamically because the Index struct is quite large (~5MB)
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);

    // 2. Write to a temporary file
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(temp_path, "w");
    if (!f) { free(sorted); return -1; }

    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hex);
        
        fprintf(f, "%06o %s %lu %u %s\n",
                sorted->entries[i].mode, 
                hex,
                (unsigned long)sorted->entries[i].mtime_sec,
                (unsigned)sorted->entries[i].size,
                sorted->entries[i].path);
    }

    // 3. Ensure it hits the disk
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    // 4. Atomic rename
    return rename(temp_path, INDEX_FILE);
}

// Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    // 1. Get file metadata
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1; // Only track regular files

    // 2. Read the file into memory
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t *data = NULL;
    if (st.st_size > 0) {
        data = malloc(st.st_size);
        if (!data) { fclose(f); return -1; }
        if (fread(data, 1, st.st_size, f) != (size_t)st.st_size) {
            free(data); fclose(f); return -1;
        }
    }
    fclose(f);

    // 3. Write it to the object store as a blob
    ObjectID id;
    if (object_write(OBJ_BLOB, data, st.st_size, &id) != 0) {
        free(data); return -1;
    }
    free(data);

    // 4. Find or create the index entry
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
    }

    // 5. Update metadata
    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash = id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;

    // 6. Save the updated index back to disk
    return index_save(index);
}