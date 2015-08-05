/**
 * @file pomp_loop_win32.c
 *
 * @brief Event loop, win32 implementation.
 *
 * @author yves-marie.morgan@parrot.com
 *
 * Copyright (c) 2014 Parrot S.A.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the <organization> nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "pomp_priv.h"

#ifdef POMP_HAVE_LOOP_WIN32

/**
 * Convert fd event from wsa events.
 * @param events : wsa events.
 * @return fd events.
 */
static uint32_t fd_events_from_wsa(long events)
{
	uint32_t res = 0;
	if (events & FD_READ)
		res |= POMP_FD_EVENT_IN;
	if (events & FD_WRITE)
		res |= POMP_FD_EVENT_OUT;
	if (events & FD_ACCEPT)
		res |= POMP_FD_EVENT_IN;
	if (events & FD_CONNECT)
		res |= POMP_FD_EVENT_OUT;
	if (events & FD_CLOSE)
		res |= POMP_FD_EVENT_IN;
	return res;
}

/**
 * Convert fd event to wsa events.
 * @param events : fd events.
 * @return wsa events.
 */
static long fd_events_to_wsa(uint32_t events)
{
	long res = 0;
	if (events & POMP_FD_EVENT_IN)
		res |= FD_READ | FD_ACCEPT | FD_CLOSE;
	if (events & POMP_FD_EVENT_OUT)
		res |= FD_WRITE | FD_CONNECT;
	return res;
}

/**
 * @see pomp_loop_do_new.
 */
static int pomp_loop_win32_do_new(struct pomp_loop *loop)
{
	int res = 0;

	/* Initialize implementation specific fields */
	loop->wakeup.hevt = NULL;

	/* Initialize winsock API */
	WSADATA wsadata;
	if (WSAStartup(MAKEWORD(2, 0), &wsadata) != 0) {
		POMP_LOGE("WSAStartup error");
		return -ENOMEM;
	}

	/* Create event for wakeup (manual reset initially unset) */
	loop->wakeup.hevt = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (loop->wakeup.hevt == NULL) {
		res = -errno;
		POMP_LOG_ERRNO("CreateEvent");
		goto error;
	}

	return 0;

	/* Cleanup in case of error */
error:
	WSACleanup();
	return res;
}

/**
 * @see pomp_loop_do_destroy.
 */
static int pomp_loop_win32_do_destroy(struct pomp_loop *loop)
{
	/* Free event for wakup */
	if (loop->wakeup.hevt != NULL) {
		CloseHandle(loop->wakeup.hevt);
		loop->wakeup.hevt = NULL;
	}

	/* Cleanup winsock API */
	WSACleanup();
	return 0;
}

/**
 * @see pomp_loop_do_add.
 */
static int pomp_loop_win32_do_add(struct pomp_loop *loop, struct pomp_fd *pfd)
{
	int res = 0;

	/* Create event handle for notification */
	pfd->hevt = CreateEvent(NULL, 0, 0, NULL);
	if (pfd->hevt == NULL) {
		res = -errno;
		POMP_LOG_ERRNO("CreateEvent");
		goto error;
	}

	/* Setup monitor */
	if (WSAEventSelect(pfd->fd, pfd->hevt,
			fd_events_to_wsa(pfd->events)) != 0) {
		res = -errno;
		POMP_LOG_ERRNO("WSAEventSelect");
		goto error;
	}

	return 0;

	/* Cleanup in case of error */
error:
	if (pfd->hevt != NULL) {
		CloseHandle(pfd->hevt);
		pfd->hevt = NULL;
	}
	return res;
}

/**
 * @see pomp_loop_do_update.
 */
static int pomp_loop_win32_do_update(struct pomp_loop *loop,
		struct pomp_fd *pfd)
{
	int res = 0;

	/* Update monitor */
	if (WSAEventSelect(pfd->fd, pfd->hevt,
			fd_events_to_wsa(pfd->events)) != 0) {
		res = -errno;
		POMP_LOG_ERRNO("WSAEventSelect");
	}

	return res;
}

/**
 * @see pomp_loop_do_remove.
 */
static int pomp_loop_win32_do_remove(struct pomp_loop *loop,
		struct pomp_fd *pfd)
{
	/* Reset monitor */
	if (WSAEventSelect(pfd->fd, pfd->hevt, 0) != 0)
		POMP_LOG_ERRNO("WSAEventSelect");

	/* Free handle for notification */
	CloseHandle(pfd->hevt);
	pfd->hevt = NULL;
	return 0;
}

/**
 * @see pomp_loop_do_get_fd.
 */
static int pomp_loop_win32_do_get_fd(struct pomp_loop *loop)
{
	return -ENOSYS;
}

/**
 * @see pomp_loop_do_wait_and_process.
 */
static int pomp_loop_win32_do_wait_and_process(struct pomp_loop *loop,
		int timeout)
{
	int res = 0;
	struct pomp_fd *pfd = NULL;
	uint32_t revents = 0;
	DWORD count = 0, waitres = 0;
	HANDLE hevt = NULL;
	HANDLE hevts[MAXIMUM_WAIT_OBJECTS];
	WSANETWORKEVENTS events;

	/* Wakeup event */
	hevts[count++] = loop->wakeup.hevt;

	/* Registered events */
	for (pfd = loop->pfds; pfd != NULL; pfd = pfd->next) {
		if (count < MAXIMUM_WAIT_OBJECTS) {
			hevts[count++] = pfd->hevt;
		}
	}

	/* Do the wait */
	waitres = WaitForMultipleObjects(count, hevts, 0,
			timeout == -1 ? INFINITE : (DWORD)timeout);

	if (waitres == WAIT_TIMEOUT)
		return -ETIMEDOUT;

	/* Make sure wait result is expected */
	if (waitres < WAIT_OBJECT_0 || waitres >= WAIT_OBJECT_0 + count) {
		POMP_LOGW("Unexpected wait result : %u", waitres);
		return 0;
	}
	hevt = hevts[waitres - WAIT_OBJECT_0];

	/* Check for the wakeup event */
	if (hevt == loop->wakeup.hevt) {
		ResetEvent(loop->wakeup.hevt);
		return 0;
	}

	/* Search fd structure whose notification event is ready */
	pfd = pomp_loop_win32_find_pfd_by_hevt(loop, hevt);
	if (pfd == NULL) {
		POMP_LOGW("hevt %p not found in loop %p", hevt, loop);
	} else if (pfd->fd >= 0) {
		/* Socket event */
		memset(&events, 0, sizeof(events));
		WSAEnumNetworkEvents(pfd->fd, pfd->hevt, &events);
		revents = fd_events_from_wsa(events.lNetworkEvents);
		(*pfd->cb)(pfd->fd, revents, pfd->userdata);
	} else {
		/* Timer event */
		(*pfd->cb)(pfd->fd, POMP_FD_EVENT_IN, pfd->userdata);
	}

	return 0;
}

/**
 * @see pomp_loop_do_wakeup.
 */
static int pomp_loop_win32_do_wakeup(struct pomp_loop *loop)
{
	int res = 0;

	/* Set notification event */
	if (!SetEvent(loop->wakeup.hevt)) {
		res = -errno;
		POMP_LOG_ERRNO("CreateEvent");
	}

	return 0;
}

/**
 * Find a registered fd in loop.
 * @param loop : loop.
 * @param hevt : notification event to search.
 * @return fd structure or NULL if not found.
 */
struct pomp_fd *pomp_loop_win32_find_pfd_by_hevt(struct pomp_loop *loop,
		HANDLE hevt)
{
	struct pomp_fd *pfd = NULL;
	for (pfd = loop->pfds; pfd != NULL; pfd = pfd->next) {
		if (pfd->hevt == hevt)
			return pfd;
	}
	return NULL;
}

/** Loop operations for win32 implementation */
const struct pomp_loop_ops pomp_loop_win32_ops = {
	.do_new = &pomp_loop_win32_do_new,
	.do_destroy = &pomp_loop_win32_do_destroy,
	.do_add = &pomp_loop_win32_do_add,
	.do_update = &pomp_loop_win32_do_update,
	.do_remove = &pomp_loop_win32_do_remove,
	.do_get_fd = &pomp_loop_win32_do_get_fd,
	.do_wait_and_process = &pomp_loop_win32_do_wait_and_process,
	.do_wakeup = &pomp_loop_win32_do_wakeup,
};

#endif /* POMP_HAVE_LOOP_WIN32 */
