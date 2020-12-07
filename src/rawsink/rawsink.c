/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPG-HTTP streamer.                    #
#                                                                            #
#    Copyright (C) 2018  Maxim Devaev <mdevaev@gmail.com>                    #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#include "rawsink.h"

#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "../common/tools.h"
#include "../common/logging.h"


struct rawsink_t *rawsink_init(const char *name, mode_t mode, bool rm) {
	struct rawsink_t *rawsink;

	A_CALLOC(rawsink, 1);
	rawsink->fd = -1;
	rawsink->picture = MAP_FAILED;
	rawsink->signal_sem = SEM_FAILED;
	rawsink->lock_sem = SEM_FAILED;
	rawsink->rm = rm;

	A_CALLOC(rawsink->mem_name, strlen(name) + 8);
	A_CALLOC(rawsink->signal_name, strlen(name) + 8);
	A_CALLOC(rawsink->lock_name, strlen(name) + 8);

	sprintf(rawsink->mem_name, "%s.mem", name);
	sprintf(rawsink->signal_name, "%s.sig", name);
	sprintf(rawsink->lock_name, "%s.lock", name);

	LOG_INFO("Using RAW sink: %s.{mem,sig,lock}", name);

	{ // Shared memory
		if ((rawsink->fd = shm_open(rawsink->mem_name, O_RDWR | O_CREAT, mode)) == -1) {
			LOG_PERROR("Can't open RAW sink memory");
			goto error;
		}

		if (ftruncate(rawsink->fd, sizeof(struct rawsink_picture_t)) < 0) {
			LOG_PERROR("Can't truncate RAW sink memory");
			goto error;
		}

		if ((rawsink->picture = mmap(
			NULL,
			sizeof(struct rawsink_picture_t),
			PROT_READ | PROT_WRITE,
			MAP_SHARED,
			rawsink->fd,
			0
		)) == MAP_FAILED) {
			LOG_PERROR("Can't mmap RAW sink memory");
			goto error;
		}
	}

#	define OPEN_SEM(_role, _default) { \
			if ((rawsink->_role##_sem = sem_open(rawsink->_role##_name, O_RDWR | O_CREAT, mode, _default)) == SEM_FAILED) { \
				LOG_PERROR("Can't open RAW sink " #_role " semaphore"); \
				goto error; \
			} \
		}

	OPEN_SEM(signal, 0);
	OPEN_SEM(lock, 1);

#	undef OPEN_SEM

	return rawsink;

	error:
		rawsink_destroy(rawsink);
		return NULL;
}

void rawsink_destroy(struct rawsink_t *rawsink) {
#	define CLOSE_SEM(_role) { \
			if (rawsink->_role##_sem != SEM_FAILED) { \
				if (sem_close(rawsink->_role##_sem) < 0) { \
					LOG_PERROR("Can't close RAW sink " #_role " semaphore"); \
				} \
				if (rawsink->rm && sem_unlink(rawsink->_role##_name) < 0) { \
					if (errno != ENOENT) { \
						LOG_PERROR("Can't remove RAW sink " #_role " semaphore"); \
					} \
				} \
			} \
		}

	CLOSE_SEM(lock);
	CLOSE_SEM(signal);

#	undef CLOSE_SEM

	if (rawsink->picture != MAP_FAILED) {
		if (munmap(rawsink->picture, sizeof(struct rawsink_picture_t)) < 0) {
			LOG_PERROR("Can't unmap RAW sink memory");
		}
	}

	if (rawsink->fd >= 0) {
		if (close(rawsink->fd) < 0) {
			LOG_PERROR("Can't close RAW sink fd");
		}
		if (rawsink->rm && shm_unlink(rawsink->mem_name) < 0) {
			if (errno != ENOENT) {
				LOG_PERROR("Can't remove RAW sink memory");
			}
		}
	}

	free(rawsink->lock_name);
	free(rawsink->signal_name);
	free(rawsink->mem_name);
	free(rawsink);
}

void rawsink_put(
	struct rawsink_t *rawsink,
	const unsigned char *data, size_t size,
	unsigned format, unsigned width, unsigned height,
	long double grab_ts) {

	long double now = get_now_monotonic();

	if (rawsink->failed) {
		return;
	}

	if (size > RAWSINK_MAX_DATA) {
		LOG_ERROR("RAWSINK: Can't put RAW frame: is too big (%zu > %zu)", size, RAWSINK_MAX_DATA);
		return;
	}

	if (sem_trywait(rawsink->lock_sem) == 0) {
		LOG_PERF("RAWSINK: >>>>> Exposing new frame ...");

		if (sem_trywait(rawsink->signal_sem) < 0 && errno != EAGAIN) {
			LOG_PERROR("RAWSINK: Can't wait %s", rawsink->signal_name);
			goto error;
		}

#		define PICTURE(_next) rawsink->picture->_next
		PICTURE(format) = format;
		PICTURE(width) = width;
		PICTURE(height) = height;
		PICTURE(grab_ts) = grab_ts;
		PICTURE(used) = size;
		memcpy(PICTURE(data), data, size);
#		undef PICTURE

		if (sem_post(rawsink->signal_sem) < 0) {
			LOG_PERROR("RAWSINK: Can't post %s", rawsink->signal_name);
			goto error;
		}
		if (sem_post(rawsink->lock_sem) < 0) {
			LOG_PERROR("RAWSINK: Can't post %s", rawsink->lock_name);
			goto error;
		}
		LOG_VERBOSE("RAWSINK: Exposed new frame; full exposition time = %Lf", get_now_monotonic() - now);

	} else if (errno == EAGAIN) {
		LOG_PERF("RAWSINK: ===== Shared memory is busy now; frame skipped");

	} else {
		LOG_PERROR("RAWSINK: Can't wait %s", rawsink->lock_name);
		goto error;
	}

	return;

	error:
		LOG_ERROR("RAW sink completely disabled due error");
		rawsink->failed = true;
}
