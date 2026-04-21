// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// Forward declaration needed for saving objects
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Recursive helper to group files into subdirectories
static int build_tree_recursive(Index *idx, int start, int end, int depth, ObjectID *out_id) {
    Tree t;
    t.count = 0;
    int i = start;
    
    while (i < end) {
        const char *rel_path = idx->entries[i].path + depth;
        const char *slash = strchr(rel_path, '/');

        if (!slash) {
            // It's a file in the current directory
            t.entries[t.count].mode = idx->entries[i].mode;
            t.entries[t.count].hash = idx->entries[i].hash;
            snprintf(t.entries[t.count].name, sizeof(t.entries[t.count].name), "%s", rel_path);
            t.count++;
            i++;
        } else {
            // It's a subdirectory
            size_t dir_len = slash - rel_path;
            char dir_name[256];
            if (dir_len >= sizeof(dir_name)) dir_len = sizeof(dir_name) - 1;
            snprintf(dir_name, dir_len + 1, "%s", rel_path); // +1 for null terminator

            // Find all subsequent index entries that belong in this subdirectory
            int j = i + 1;
            while (j < end) {
                const char *next_path = idx->entries[j].path + depth;
                if (strncmp(next_path, dir_name, dir_len) != 0 || next_path[dir_len] != '/') {
                    break;
                }
                j++;
            }

            ObjectID subtree_id;
            if (build_tree_recursive(idx, i, j, depth + dir_len + 1, &subtree_id) != 0) {
                return -1;
            }

            // Add the created subtree to our current tree
            t.entries[t.count].mode = MODE_DIR;
            t.entries[t.count].hash = subtree_id;
            snprintf(t.entries[t.count].name, sizeof(t.entries[t.count].name), "%s", dir_name);
            t.count++;

            i = j; // Skip past all the items we just processed for that subdirectory
        }
    }

    // Serialize and write to the object store
    void *data;
    size_t len;
    if (tree_serialize(&t, &data, &len) != 0) return -1;
    
    int rc = object_write(OBJ_TREE, data, len, out_id);
    free(data);
    return rc;
}

// Build a tree hierarchy from the current index
int tree_from_index(ObjectID *id_out) {
    Index idx;
    if (index_load(&idx) != 0) {
        idx.count = 0; 
    }
    
    // If staging area is empty, create an empty tree
    if (idx.count == 0) {
        Tree t;
        t.count = 0;
        void *data;
        size_t len;
        if (tree_serialize(&t, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }
    
    return build_tree_recursive(&idx, 0, idx.count, 0, id_out);
}