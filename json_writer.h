#ifndef JSON_WRITER_H
#define JSON_WRITER_H

// json_writer.h:
// Write string and number to json file
//

#ifdef WIN32
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

#include <string>
#define INIT_CAPACITY 128

struct json_writer {

    char *buffer;       // json writer buffer
    size_t capacity;    // Current capacity (0 if error)
    size_t length;      // Curretn length
    bool add_comma;     // Add comma at the begining of the NEXT entry
    FILE *file;         // file handler

    void clean() {
        if (!buffer)
            free(buffer);
        buffer = NULL;
        capacity = 0;
        length = 0;
        if (!file)
            fclose(file);
        file = NULL;
    }

    int reset() {
        if (!buffer)
            free(buffer);
        buffer = (char *)malloc(INIT_CAPACITY);
        if (!buffer) {
            perror("Failed to allocate memory for JSON writer.\n");
            clean();
            return -1; 
        }
        capacity = INIT_CAPACITY;
        buffer[0] = '{';
        length = 1;
        add_comma = false;
        return 0;
    }

    int remove_file_if_exists(const char *filename) {
        // Return 0 if the file does not exis or the file can be removed
        if (access(filename, F_OK) != 0 ||
            remove(filename) == 0)
                return 0;
        return -1;
    }

    int init(const char *filename, bool append_file = true) {
        if (!filename || remove_file_if_exists(filename) != 0)
            return -1;
        if (append_file)
            file = fopen(filename, "a");
        else
            file = fopen(filename, "w");
        if (!file) {
            perror("Error opening file.\n");
            return -1;
        }
        return reset();
    }

    int check_capacity(const size_t req_len) {
        if (!capacity)
            return -1;
        if (length + req_len >= capacity) {
            size_t new_capacity = (length + req_len + 1 > 2*capacity) ? (length + req_len + 1) : 2*capacity;
            char * new_buffer = (char *)realloc(buffer, new_capacity);

            if (!new_buffer) {
                perror("Failed to allocate new-memory for JSON writer.\n");
                clean();
                return -1;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }
        return 0;
    }

    int append(const char *text) {
        size_t text_len = strlen(text);

        if (!text)
            return -1;
        if (check_capacity(text_len) != 0)
            return -1;
        memcpy(buffer + length, text, text_len);
        length += text_len;
        return 0;
    }
    
    int add_comma_if_needed() {
        if (add_comma)
            return append(",");
        add_comma = true;
        return 0;
    }

    int add_format_string(const char *key, const char *value, const char *format = "%s") {
        char temp[2*INIT_CAPACITY];
        char formatted[INIT_CAPACITY];

        if (!key || !value || !format) {
            perror("Skip adding empty key/string into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(formatted, sizeof(formatted), format, value);
        snprintf(temp, sizeof(temp), "\"%s\":\"%s\"", key, formatted);
        return append(temp);
    }

    int add_format_uint32(const char *key, const uint32_t value, const char *format = "%u") {
        char temp[2*INIT_CAPACITY];
        char formatted[INIT_CAPACITY];

        if (!key || !format) {
            perror("Skip adding empty key/uint32 into JSON writer.\n");
            return -1; 
        }
        if (add_comma_if_needed() != 0)
            return -1; 
        snprintf(formatted, sizeof(formatted), format, value);
        snprintf(temp, sizeof(temp), "\"%s\":\"%s\"", key, formatted);
        return append(temp);
    }

    int add_format_int32(const char *key, const int32_t value, const char *format = "%d") {
        char temp[2*INIT_CAPACITY];
        char formatted[INIT_CAPACITY];

        if (!key || !format) {
            perror("Skip adding empty key/int32 into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(formatted, sizeof(formatted), format, value);
        snprintf(temp, sizeof(temp), "\"%s\":\"%s\"", key, formatted);
        return append(temp);
    }

    int add_format_float(const char *key, const float value, const char *format = "%f") {
        char temp[2*INIT_CAPACITY];
        char formatted[INIT_CAPACITY];

        if (!key || !format) {
            perror("Skip adding empty key/float into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(formatted, sizeof(formatted), format, value);
        snprintf(temp, sizeof(temp), "\"%s\":\"%s\"", key, formatted);
        return append(temp);
    }

    int begin_array(const char *key) {
        char temp[INIT_CAPACITY];

        if (!key) {
            perror("Skip adding empty array into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(temp, sizeof(temp), "\"%s\":[", key);
        if (append(temp) != 0)
            return -1;
        add_comma = false;
        return 0;
    }
    
    int add_array_string(const char *value, const char *format = "%s") {
        char temp[INIT_CAPACITY];

        if (!value || !format) {
            perror("Skip adding empty string element into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(temp, sizeof(temp), format, value);
        return append(temp);
    }
    
    int add_array_uint32(const uint32_t value, const char *format = "%u") {
        char temp[INIT_CAPACITY];

        if (!format) {
            perror("Skip adding empty uint32 element into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(temp, sizeof(temp), format, value);
        return append(temp);
    }

    int add_array_int32(const uint32_t value, const char *format = "%d") {
        char temp[INIT_CAPACITY];

        if (!format) {
            perror("Skip adding empty int32 element into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(temp, sizeof(temp), format, value);
        return append(temp);
    }

    int add_array_float(const float value, const char *format = "%f") {
        char temp[INIT_CAPACITY];

        if (!format) {
            perror("Skip adding empty float element into JSON writer.\n");
            return -1;
        }
        if (add_comma_if_needed() != 0)
            return -1;
        snprintf(temp, sizeof(temp), format, value);
        return append(temp);
    }

    int end_array() {
        if (append("]") != 0)
            return -1;
        add_comma = true;
        return 0;
    }

    int finalize() {
        if (check_capacity(2) != 0)
            return -1;
        buffer[length++] = '}';
        buffer[length] = '\0';
        return 0;
    }

    int dumpfile() {
        if (!file || !buffer)
            return -1;

        fprintf(file, "%s", buffer);
        fflush(file);
        return 0;
    }

    ~json_writer() {
        clean();
    }
};

#endif // JSON_WRITER_H
