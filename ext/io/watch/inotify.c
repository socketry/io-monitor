#include "watch.h"

#include <sys/inotify.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

enum {
	DEBUG = 0,
};

struct IO_Watch_Watch {
	int watch_descriptor;
	char *path;
	int index;
};

struct IO_Watch_Watch_Array {
	size_t size;
	size_t capacity;
	struct IO_Watch_Watch *watches;
};

#define BUFFER_SIZE (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

void IO_Watch_Watch_Array_initialize(struct IO_Watch_Watch_Array *array) {
	array->size = 0;
	array->capacity = 16;
	array->watches = malloc(array->capacity * sizeof(struct IO_Watch_Watch));
	if (!array->watches) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
}

void IO_Watch_Watch_Array_resize(struct IO_Watch_Watch_Array *array) {
	array->capacity *= 2;
	array->watches = realloc(array->watches, array->capacity * sizeof(struct IO_Watch_Watch));
	if (!array->watches) {
		perror("realloc");
		exit(EXIT_FAILURE);
	}
}

void IO_Watch_Watch_Array_add(struct IO_Watch_Watch_Array *array, int watch_descriptor, char *path, int index) {
	if (array->size == array->capacity) {
		IO_Watch_Watch_Array_resize(array);
	}
	array->watches[array->size].watch_descriptor = watch_descriptor;
	array->watches[array->size].path = path;
	array->watches[array->size].index = index;
	array->size++;
}

ssize_t IO_Watch_Watch_Array_find(struct IO_Watch_Watch_Array *array, int watch_descriptor) {
	for (size_t i = 0; i < array->size; i++) {
		if (array->watches[i].watch_descriptor == watch_descriptor) {
			return i;
		}
	}
	return -1;
}

void IO_Watch_Watch_Array_watch(int fd, struct IO_Watch_Watch_Array *watch_array, char *path, int index) {
	int watch_descriptor = inotify_add_watch(fd, path, IN_ALL_EVENTS);
	if (watch_descriptor == -1) {
		perror("inotify_add_watch");
		exit(EXIT_FAILURE);
	}
	
	IO_Watch_Watch_Array_add(watch_array, watch_descriptor, path, index);
	
	if (DEBUG) fprintf(stderr, "Added watch: %s\n", path);
}

void IO_Watch_Watch_Array_scan(int fd, struct IO_Watch_Watch_Array *watch_array, const char *root, int index) {
	DIR *dir = opendir(root);
	if (!dir) {
		perror("opendir");
		return;
	}
	
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		
		size_t size = strlen(root) + 1 + strlen(entry->d_name) + 1;
		char *path = malloc(size);
		snprintf(path, size, "%s/%s", root, entry->d_name);

		struct stat statbuf;
		if (stat(path, &statbuf) == -1) {
			perror("stat");
			continue;
		}

		if (S_ISDIR(statbuf.st_mode)) {
			IO_Watch_Watch_Array_watch(fd, watch_array, path, index);
			IO_Watch_Watch_Array_scan(fd, watch_array, path, index);
		} else {
			free(path);
		}
	}

	closedir(dir);
}

void IO_Watch_Watch_Array_add_subdirectory(int fd, struct IO_Watch_Watch_Array *watch_array, struct IO_Watch_Watch watch, const char *name) {
	size_t size = strlen(watch.path) + 1 + strlen(name) + 1;
	char *path = malloc(size);
	snprintf(path, size, "%s/%s", watch.path, name);

	IO_Watch_Watch_Array_watch(fd, watch_array, path, watch.index);
	IO_Watch_Watch_Array_scan(fd, watch_array, path, watch.index);
}

void IO_Watch_Watch_Array_remove(int fd, struct IO_Watch_Watch_Array *watch_array, size_t index) {
	struct IO_Watch_Watch watch = watch_array->watches[index];
	
	if (DEBUG) fprintf(stderr, "Removing watch: %s\n", watch.path);
	
	inotify_rm_watch(fd, watch.watch_descriptor);
	free(watch.path);
	
	// Replace the removed item with the last one.
	watch_array->size--;
	if (index < watch_array->size) {
		watch_array->watches[index] = watch_array->watches[watch_array->size];
	}
}

static
void IO_Watch_INotify_print_event(struct inotify_event *event) {
	fprintf(stderr, "Event: wd=%d", event->wd);
	
	uint32_t mask = event->mask;
	if (mask & IN_ACCESS) fprintf(stderr, " ACCESS");
	if (mask & IN_MODIFY) fprintf(stderr, " MODIFY");
	if (mask & IN_ATTRIB) fprintf(stderr, " ATTRIB");
	if (mask & IN_CLOSE_WRITE) fprintf(stderr, " CLOSE_WRITE");
	if (mask & IN_CLOSE_NOWRITE) fprintf(stderr, " CLOSE_NOWRITE");
	if (mask & IN_OPEN) fprintf(stderr, " OPEN");
	if (mask & IN_MOVED_FROM) fprintf(stderr, " MOVED_FROM");
	if (mask & IN_MOVED_TO) fprintf(stderr, " MOVED_TO");
	if (mask & IN_CREATE) fprintf(stderr, " CREATE");
	if (mask & IN_DELETE) fprintf(stderr, " DELETE");
	if (mask & IN_DELETE_SELF) fprintf(stderr, " DELETE_SELF");
	if (mask & IN_MOVE_SELF) fprintf(stderr, " MOVE_SELF");
	if (mask & IN_UNMOUNT) fprintf(stderr, " UNMOUNT");
	if (mask & IN_Q_OVERFLOW) fprintf(stderr, " Q_OVERFLOW");
	if (mask & IN_IGNORED) fprintf(stderr, " IGNORED");
	if (mask & IN_ISDIR) fprintf(stderr, " ISDIR");
	if (mask & IN_ONESHOT) fprintf(stderr, " ONESHOT");
	if (mask & IN_ALL_EVENTS) fprintf(stderr, " ALL_EVENTS");
	
	if (event->len > 0) {
		fprintf(stderr, " name=%s", event->name);
	}
	
	fprintf(stderr, "\n");
}

void IO_Watch_run(struct IO_Watch *watch) {
	int fd = inotify_init1(IN_NONBLOCK);
	if (fd == -1) {
		perror("inotify_init1");
		exit(EXIT_FAILURE);
	}

	struct IO_Watch_Watch_Array watch_array;
	IO_Watch_Watch_Array_initialize(&watch_array);

	for (size_t i = 0; i < watch->size; i++) {
		char *path = strdup(watch->paths[i]);
		
		IO_Watch_Watch_Array_watch(fd, &watch_array, path, i);
		IO_Watch_Watch_Array_scan(fd, &watch_array, path, i);
	}

	printf("{\"status\":\"started\"}\n");
	fflush(stdout);

	char buffer[BUFFER_SIZE] __attribute__ ((aligned(8)));
	
	while (1) {
		ssize_t result = read(fd, buffer, BUFFER_SIZE);
		if (result == -1 && errno != EAGAIN) {
			perror("read");
			exit(EXIT_FAILURE);
		}

		for (ssize_t offset = 0; offset < result;) {
			struct inotify_event *event = (struct inotify_event *) &buffer[offset];
			if (DEBUG) IO_Watch_INotify_print_event(event);
			
			ssize_t index = IO_Watch_Watch_Array_find(&watch_array, event->wd);

			if (index != -1) {
				printf("{\"index\":%d,\"mask\":%u}\n", watch_array.watches[index].index, event->mask);

				// If a new directory is created, add a watch for it
				if (event->mask & IN_CREATE && event->mask & IN_ISDIR) {
					IO_Watch_Watch_Array_add_subdirectory(fd, &watch_array, watch_array.watches[index], event->name);
				} else if (event->mask & IN_IGNORED) {
					IO_Watch_Watch_Array_remove(fd, &watch_array, index);
				}
			} else {
				fprintf(stderr, "Watch descriptor not found: %d\n", event->wd);
			}

			offset += sizeof(struct inotify_event) + event->len;
		}
		fflush(stdout);
	}

	for (size_t i = 0; i < watch_array.size; i++) {
		inotify_rm_watch(fd, watch_array.watches[i].watch_descriptor);
		free(watch_array.watches[i].path);
	}
	close(fd);
	free(watch_array.watches);
}
