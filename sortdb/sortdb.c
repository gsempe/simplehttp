#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "simplehttp/queue.h"
#include "simplehttp/simplehttp.h"

#define DEBUG 1

static char *version  = "1.1";
static void *map_base = NULL;
static struct stat st;
char deliminator = '\t';
uint64_t get_requests = 0;
uint64_t total_seeks = 0;

void stats_cb(struct evhttp_request *req, struct evbuffer *evb, void *ctx);
void get_cb(struct evhttp_request *req, struct evbuffer *evb,void *ctx);
char *prev_line(char *pos);
char *map_search(char *key, size_t keylen, char *lower, char *upper, int *seeks);
void usage();
void info();
int main(int argc, char **argv);
void *open_dbfile(char *filename);

char *prev_line(char *pos) {
    if (!pos) return NULL; 
    while (pos != map_base && *(pos-1) != '\n') {
        pos--;
    }
    return pos;
}

char *map_search(char *key, size_t keylen, char *lower, char *upper, int *seeks) {
    ptrdiff_t distance;
    char *current;
    char *line;
    int rc;

    distance = (upper - lower);
    if (distance <= 1) return NULL;

    *seeks += 1;
    total_seeks++;
    current = lower + (distance/2);
    line = prev_line(current);
    if (!line) return NULL;

    rc = strncmp(key, line, keylen);
    if (rc < 0) {
        return map_search(key, keylen, lower, current, seeks);
    } else if (rc > 0) {
        return map_search(key, keylen, current, upper, seeks);
    } else {
        return line;
    }
}

void get_cb(struct evhttp_request *req, struct evbuffer *evb,void *ctx) {
    struct evkeyvalq args;
    char *uri, *key, *line, *newline, *delim, buf[32];
    int seeks = 0;

    uri = evhttp_decode_uri(req->uri);
    evhttp_parse_query(uri, &args);
    free(uri);
    key = (char *)evhttp_find_header(&args, "key");
    if(DEBUG) fprintf(stderr, "/get %s\n", key);
    
    if (!key) {
        evbuffer_add_printf(evb, "missing argument: key\n");
        evhttp_send_reply(req, HTTP_BADREQUEST, "MISSING_ARG_KEY", evb);
        return;
    }
    if ((line = map_search(key, strlen(key), (char *)map_base,
         (char *)map_base+st.st_size, &seeks))) {
        sprintf(buf, "%d", seeks);
        evhttp_add_header(req->output_headers, "x-sortdb-seeks", buf);
        delim = strchr(line, deliminator);
        if (delim) {
            line = delim+1;
        }
        newline = strchr(line, '\n');
        if (newline) {
            evbuffer_add(evb, line, (newline-line)+1);
        } else {
            evbuffer_add_printf(evb, "%s\n", line);
        }
        evhttp_send_reply(req, HTTP_OK, "OK", evb);
        return;
    }
    evhttp_send_reply(req, HTTP_NOTFOUND, "OK", evb);
}

void
stats_cb(struct evhttp_request *req, struct evbuffer *evb, void *ctx) {
    evbuffer_add_printf(evb, "Get requests: %llu\n", get_requests);
    evbuffer_add_printf(evb, "Total seeks: %llu\n", total_seeks);
    evhttp_send_reply(req, HTTP_OK, "OK", evb);
}

void info() {
    fprintf(stderr, "sortdb: Sorted database server.\n");
    fprintf(stderr, "Version %s, https://github.com/bitly/simplehttp/tree/master/sortdb\n", version);
}
void usage() {
    fprintf(stderr, "Provides search access to sorted tab delimited files\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "usage: sortdb\n");
    fprintf(stderr, "\t-f /path/to/dbfile\n");
    fprintf(stderr, "\t-F \"\\t\" (field deliminator)\n");
    fprintf(stderr, "\t-a 127.0.0.1 (address to listen on)\n");
    fprintf(stderr, "\t-p 8080 (port to listen on)\n");
    fprintf(stderr, "\t-D (daemonize)\n");
    fprintf(stderr, "\n");
    exit(1);
}

void * open_dbfile(char *filename) {
    int fd;
    static void *map_base = NULL;
    
    if ((fd = open(filename, O_RDONLY)) < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", filename, strerror(errno));
        exit(errno);
    }
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "fstat(%s) failed: %s\n", filename, strerror(errno));
        exit(errno);
    }
    fprintf(stderr, "opening %s\n", filename);
    fprintf(stderr, "db size %ld\n", st.st_size);
    if ((map_base = mmap(0, st.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        fprintf(stderr, "mmap(%s) failed: %s\n", filename, strerror(errno));
        exit(errno);
    }
    return map_base;
}

int main(int argc, char **argv) {
    info();

    int ch;
    opterr=0;
    while ((ch = getopt(argc, argv, "f:F:h")) != -1) {
        if (ch == '?') {
            optind--; // re-set for next getopt() parse by simplehttp_init
            break;
        }
        switch (ch) {
        case 'f':
            // db file
            map_base = open_dbfile(optarg);
            break;
        case 'F':
            // field deliminator
            if (strlen(optarg) != 1) {
                fprintf(stderr, "Field (-F) deliminator must be a single character");
                usage();
                exit(1);
            }
            deliminator = optarg[0];
            break;
        case 'h':
            usage();
            exit(1);
        }
    }

    if (map_base == NULL) {
        usage();
        exit(1);
    }

    simplehttp_init();
    simplehttp_set_cb("/get?*", get_cb, NULL);
    simplehttp_set_cb("/stats", stats_cb, NULL);
    simplehttp_main(argc, argv);
    return 0;
}