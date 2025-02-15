/*
 * This program is part of the Clear Linux Project
 *
 * Copyright 2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms and conditions of the GNU Lesser General Public License, as
 * published by the Free Software Foundation; either version 2.1 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <curl/curl.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <malloc.h>
#include <sys/uio.h>

#include "iorecord.h"
#include "telemdaemon.h"
#include "common.h"
#include "util.h"
#include "log.h"
#include "configuration.h"

static void process_record(TelemDaemon *daemon, client *cl);

void initialize_probe_daemon(TelemDaemon *daemon)
{
        client_list_head head;
        LIST_INIT(&head);
        daemon->nfds = 0;
        daemon->pollfds = NULL;
        daemon->client_head = head;
        daemon->machine_id_override = NULL;
}

client *add_client(client_list_head *client_head, int fd)
{
        client *cl;

        cl = (client *)malloc(sizeof(client));
        if (cl) {
                cl->fd = fd;
                cl->offset = 0;
                cl->buf = NULL;

                LIST_INSERT_HEAD(client_head, cl, client_ptrs);
        }
        return cl;
}

void remove_client(client_list_head *client_head, client *cl)
{
        assert(cl);
        LIST_REMOVE(cl, client_ptrs);
        if (cl->buf) {
                free(cl->buf);
        }
        if (cl->fd >= 0) {
                close(cl->fd);
        }
        free(cl);
        cl = NULL;
}

bool is_client_list_empty(client_list_head *client_head)
{
        return (client_head->lh_first == NULL);
}


static void terminate_client(TelemDaemon *daemon, client *cl, nfds_t index)
{
        /* Remove fd from the pollfds array */
        del_pollfd(daemon, index);

        telem_log(LOG_INFO, "Removing client: %d\n", cl->fd);

        /* Remove client from the client list */
        remove_client(&(daemon->client_head), cl);
}

/*
 See "tm_send_record" for record retails.

 recv buffer layout:
         * <uint32_t record_size>    : so recv knows how much to read
         * <custom cfg file field>   : optional, variable size (string)
         * <uint32_t header_size>
         * <headers + Payload>
         * <null-byte>

 The routine handle_client only cares about "record_size".
 However, we need to validate if the record_size is reasonable. We assume the
 worst case scenario would be a record with max cfg file field. There is no
 exact way to determine header_size, so we assume each line at most 80 chars.
 
*/

#define MAX_RECORD_SIZE (2*sizeof(uint32_t) + CFG_PREFIX_LENGTH + PATH_MAX + \
        MAX_PAYLOAD_LENGTH + NUM_HEADERS*80)
bool handle_client(TelemDaemon *daemon, nfds_t index, client *cl)
{
        /* For now  read data from fd */
        ssize_t len;
        size_t buf_size;
        bool processed = false;
        uint32_t record_size;

        if (cl->buf != NULL) {
                free(cl->buf);
                cl->buf = NULL;
        }

        malloc_trim(0);
        len = recv(cl->fd, &record_size, RECORD_SIZE_LEN, MSG_PEEK | MSG_DONTWAIT);
        if (len < 0) {
                telem_log(LOG_ERR, "Failed to talk to client %d: %s\n", cl->fd,
                          strerror(errno));
                goto end_client;
        } else if (len == 0) {
                /* Connection closed by client, most likely */
                telem_log(LOG_INFO, "No data to receive from client %d\n",
                          cl->fd);
                goto end_client;
        }

        /* Read the record size first */
        len = recv(cl->fd, &record_size, RECORD_SIZE_LEN, 0);
        if (len < 0) {
                telem_log(LOG_ERR, "Failed to receive data from client"
                                  " %d: %s\n", cl->fd, strerror(errno));
                goto end_client;
        } else if (len == 0) {
                telem_log(LOG_DEBUG, "End of transmission for client"
                          " %d\n", cl->fd);
                goto end_client;
        }

        /* Now that we know the record size, allocate a new buffer
         * for the record body. We don't need to record size itself in the body.
         */

        if (record_size <= RECORD_SIZE_LEN || record_size > MAX_RECORD_SIZE) {
                telem_log(LOG_ERR, "Record size %u greater tham maximum allowed %lu."
                                    "Recored ignored\n", record_size,
                                    MAX_RECORD_SIZE);
                goto end_client;      
        }

        buf_size = record_size - RECORD_SIZE_LEN;
        cl->buf = calloc(1, buf_size);
        if (!cl->buf) {
                telem_log(LOG_ERR, "Unable to allocate memory, exiting\n");
                exit(EXIT_FAILURE);
        }
        cl->size = buf_size;
        cl->offset = 0;

        /* Read the actual record*/
        do {
                malloc_trim(0);
                len = recv(cl->fd, cl->buf + cl->offset, cl->size - cl->offset, 0);
                if (len < 0) {
                        telem_log(LOG_ERR, "Failed to receive data from client"
                                  " %d: %s\n", cl->fd, strerror(errno));
                        goto end_client;
                } else if (len == 0) {
                        telem_log(LOG_DEBUG, "End of transmission for client"
                                  " %d\n", cl->fd);
                        goto end_client;
                }

                cl->offset += (size_t)len;

                if (cl->offset == cl->size) {
                        process_record(daemon, cl);
                        free(cl->buf);
                        cl->buf = NULL;
                        processed = true;
                        telem_debug("DEBUG: Record processed for client %d\n", cl->fd);
                        break;
                }
        } while (len > 0);

end_client:
        telem_log(LOG_DEBUG, "Processed client %d: %s\n", cl->fd, processed ? "true" : "false");
        terminate_client(daemon, cl, index);
        return processed;
}

char *read_machine_id_override()
{
        char *machine_override = NULL;
        FILE *fp = NULL;
        size_t bytes_read;
        char *endl = NULL;

        fp = fopen(TM_MACHINE_ID_OVERRIDE, "r");
        if (!fp) {
                if (errno == ENOMEM) {
                        exit(EXIT_FAILURE);
                } else if (errno != ENOENT) {
                        /* Log any other error */
                        telem_log(LOG_ERR, "Unable to open static machine id file %s: %s\n",
                                  TM_MACHINE_ID_OVERRIDE, strerror(errno));
                }
                return NULL;
        }

        machine_override = malloc(sizeof(char) * 33);
        if (!machine_override) {
                exit(EXIT_FAILURE);
        }

        memset(machine_override, 0, 33);

        bytes_read = fread(machine_override, sizeof(char), 32, fp);
        if (bytes_read == 0) {
                if (ferror(fp) != 0) {
                        telem_log(LOG_ERR, "Error while reading %s file: %s\n",
                                  TM_MACHINE_ID_OVERRIDE, strerror(errno));
                }
                fclose(fp);
                free(machine_override);
                return NULL;
        }

        endl = memchr(machine_override, '\n', 32);
        if (endl) {
                *endl = '\0';
        }

        fclose(fp);
        return machine_override;
}

static void machine_id_replace(char **machine_header, char *machine_id_override)
{
        char machine_id[33] = { 0 };
        char *old_header;
        int ret;

        if (machine_id_override) {
                strncpy(machine_id, machine_id_override, sizeof(machine_id)-1);
        } else {
                if (!get_machine_id(machine_id)) {
                        // TODO: decide if error handling is needed here
                        machine_id[0] = '0';
                }
        }

        old_header = *machine_header;
        ret = asprintf(machine_header, "%s: %s", TM_MACHINE_ID_STR, machine_id);

        if (ret == -1) {
                telem_log(LOG_ERR, "Failed to write machine id header\n");
                exit(EXIT_FAILURE);
        }
        free(old_header);
}

static void stage_record(char *filepath, char *headers[], char *body, char *cfg_file)
{
        int tmpfd;
        FILE *tmpfile = NULL;

        telem_debug("DEBUG: filepath:%s\n", filepath);
        telem_debug("DEBUG: body:%s\n", body);
        telem_debug("DEBUG: cfg:%s\n", cfg_file);

        if (filepath == NULL) {
                telem_log(LOG_ERR, "filepath value must be provided, aborting\n");
                exit(EXIT_FAILURE);
        }

        tmpfd = mkstemp(filepath);
        if (tmpfd < 0) {
                telem_perror("Error opening staging file");
                goto clean_exit;
        }

        // access the opened file as a stream
        tmpfile = fdopen(tmpfd, "a");
        if (!tmpfile) {
                telem_perror("Error opening temp stage file");
                close(tmpfd);
                if (unlink(filepath)) {
                        telem_perror("Error deleting temp stage file");
                }
                goto clean_exit;
        }

        // write cfg info if exists
        if (cfg_file != NULL) {
                fprintf(tmpfile, "%s%s\n", CFG_PREFIX, cfg_file);
        }

        // write headers
        for (int i = 0; i < NUM_HEADERS; i++) {
                fprintf(tmpfile, "%s\n", headers[i]);
        }

        // write body
        fprintf(tmpfile, "%s\n", body);
        fflush(tmpfile);
        fclose(tmpfile);

clean_exit:

        return;
}

static void process_record(TelemDaemon *daemon, client *cl)
{
        int i = 0;
        int ret = 0;
        char *headers[NUM_HEADERS];
        char *tok = NULL;
        size_t header_size = 0;
        size_t message_size = 0;
        char *temp_headers = NULL;
        char *msg;
        char *body;
        char *recordpath = NULL;
        char *cfg_file = NULL;;
        size_t cfg_info_size = 0;
        uint8_t *buf;

        buf = cl->buf;

        /* Check for an optional CFG_PREFIX in the first 32 bits */
        if (*(uint32_t *)buf == CFG_PREFIX_32BIT) {
                char *cfg  = (char *)cl->buf;

                cfg_file = cfg + CFG_PREFIX_LENGTH;
                cfg_info_size = CFG_PREFIX_LENGTH + strlen(cfg_file) + 1;
                telem_debug("DEBUG: cfg_file: %s\n", cfg_file);
        }

        buf += cfg_info_size;
        header_size = *(uint32_t *)buf;
        message_size = cl->size - (cfg_info_size + header_size);
        telem_debug("DEBUG: cl->size: %zu\n", cl->size);
        telem_debug("DEBUG: header_size: %zu\n", header_size);
        telem_debug("DEBUG: message_size: %zu\n", message_size);
        telem_debug("DEBUG: cfg_info_size: %zu\n", cfg_info_size);
        assert(message_size > 0);      //TODO:Check for min and max limits
        msg = (char *)buf + sizeof(uint32_t);

        /* Copying the headers as strtok modifies the orginal buffer */
        temp_headers = strndup(msg, header_size);
        tok = strtok(temp_headers, "\n");

        for (i = 0; i < NUM_HEADERS; i++) {
                const char *header_name = get_header_name(i);

                if (get_header(tok, header_name, &headers[i])) {
                        if (strcmp(header_name, TM_MACHINE_ID_STR) == 0) {
                                machine_id_replace(&headers[i], daemon->machine_id_override);
                        }

                        tok = strtok(NULL, "\n");
                } else {
                        telem_log(LOG_ERR, "process_record: Incorrect headers in record\n");
                        goto end;
                }
        }
        /* TODO : check if the body is within the limits. */
        body = msg + header_size;

        /* Save record to stage */
        ret = asprintf(&recordpath, "%s/XXXXXX", spool_dir_config());
        if (ret == -1) {
                telem_log(LOG_ERR, "Failed to allocate memory for record name in staging folder, aborting\n");
                exit(EXIT_FAILURE);
        }

        stage_record(recordpath, headers, body, cfg_file);
        free(recordpath);
end:
        free(temp_headers);
        for (int k = 0; k < i; k++)
                free(headers[k]);
        return;
}

void add_pollfd(TelemDaemon *daemon, int fd, short events)
{
        assert(daemon);
        assert(fd >= 0);

        if (!daemon->pollfds) {
                daemon->pollfds = (struct pollfd *)malloc(sizeof(struct pollfd));
                if (!daemon->pollfds) {
                        telem_log(LOG_ERR, "Unable to allocate memory for"
                                  " pollfds array, exiting\n");
                        exit(EXIT_FAILURE);
                }
                daemon->current_alloc = sizeof(struct pollfd);
        } else {
                /* Reallocate here */
                if (!reallocate((void **)&(daemon->pollfds),
                                &(daemon->current_alloc),
                                ((daemon->nfds + 1) * sizeof(struct pollfd)))) {
                        telem_log(LOG_ERR, "Unable to realloc, exiting\n");
                        exit(EXIT_FAILURE);
                }
        }

        daemon->pollfds[daemon->nfds].fd = fd;
        daemon->pollfds[daemon->nfds].events = events;
        daemon->pollfds[daemon->nfds].revents = 0;
        daemon->nfds++;
}

void del_pollfd(TelemDaemon *daemon, nfds_t i)
{
        assert(daemon);
        assert(i < daemon->nfds);

        if (i < daemon->nfds - 1) {
                memmove(&(daemon->pollfds[i]), &(daemon->pollfds[i + 1]),
                        (sizeof(struct pollfd) * (daemon->nfds - i - 1)));
        }
        daemon->nfds--;
}

bool get_machine_id(char *machine_id)
{
        FILE *id_file = NULL;
        int ret;

        char *machine_id_file_name = TM_MACHINE_ID_FILE;

        id_file = fopen(machine_id_file_name, "r");
        if (id_file == NULL) {
                telem_log(LOG_ERR, "Could not open machine id file\n");
                return false;
        }

        ret = fscanf(id_file, "%32s", machine_id);
        if (ret != 1) {
                telem_perror("Could not read machine id from file");
                fclose(id_file);
                return false;
        }
        fclose(id_file);
        return true;
}

int machine_id_write(char *new_id)
{
        FILE *fp;
        int ret = 0;
        int result = -1;

        fp = fopen(TM_MACHINE_ID_FILE, "w");
        if (fp == NULL) {
                telem_perror("Could not open machine id file");
                goto file_error;
        }

        ret = fprintf(fp, "%s", new_id);
        if (ret < 0) {
                telem_perror("Unable to write to machine id file");
                goto file_error;
        }
        result = 0;
        fflush(fp);
file_error:
        if (fp != NULL) {
                fclose(fp);
        }

        return result;
}

int generate_machine_id(void)
{
        int result = 0;
        char *new_id = NULL;

        if ((result = get_random_id(&new_id)) != 0) {
                goto rand_err;
        }

        result = machine_id_write(new_id);
rand_err:
        free(new_id);

        return result;
}

int update_machine_id()
{
        int result = 0;
        struct stat buf;
        int ret = 0;

        char *machine_id_filename = TM_MACHINE_ID_FILE;
        ret = stat(machine_id_filename, &buf);

        if (ret == -1) {
                if (errno == ENOENT) {
                        telem_log(LOG_INFO, "Machine id file does not exist\n");
                        result = generate_machine_id();
                } else {
                        telem_log(LOG_ERR, "Unable to stat machine id file\n");
                        result = -1;
                }
        } else {
                time_t current_time = time(NULL);

                if ((current_time - buf.st_mtime) > TM_MACHINE_ID_EXPIRY) {
                        telem_log(LOG_INFO, "Machine id file has expired\n");
                        result = generate_machine_id();
                }
        }
        return result;
}

/* vi: set ts=8 sw=8 sts=4 et tw=80 cino=(0: */
