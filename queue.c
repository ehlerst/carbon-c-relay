/*
 *  This file is part of carbon-c-relay.
 *
 *  carbon-c-relay is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  carbon-c-relay is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with carbon-c-relay.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "queue.h"

/**
 * Allocates a new queue structure with capacity to hold size elements.
 */
queue *
queue_new(size_t size)
{
	queue *ret = malloc(sizeof(queue));

	if (ret == NULL)
		return NULL;

	ret->queue = malloc(sizeof(void *) * size);
	ret->write = ret->read = ret->queue;

	if (ret->queue == NULL) {
		free(ret);
		return NULL;
	}

	ret->end = ret->queue + size;
	ret->len = 0;
	pthread_mutex_init(&ret->lock, NULL);

	return ret;
}

/**
 * Frees up allocated resources in use by the queue.  This doesn't take
 * into account any consumers at all.  That is, the caller needs to
 * ensure noone is using the queue any more.
 */
void
queue_destroy(queue *q)
{
	q->len = 0;
	pthread_mutex_destroy(&q->lock);
	free((char *)q->queue);
	free(q);
}

/**
 * Enqueues the string pointed to by p at queue q.  If the queue is
 * full, the oldest entry is dropped.  For this reason, enqueuing will
 * never fail.
 */
void
queue_enqueue(queue *q, const char *p)
{
	pthread_mutex_lock(&q->lock);
	if (q->write == q->read) {
		free((void *)(q->read));
		q->read++;
		q->len--;
		if (q->write == q->end)
			q->read = q->queue + 1;
	}
	if (q->write == q->end)
		q->write = q->queue;
	q->write = strdup(p);
	q->write++;
	q->len++;
	pthread_mutex_unlock(&q->lock);
}

/**
 * Returns the oldest entry in the queue.  If there are no entries, NULL
 * is returned.  The caller should free the returned string.
 */
const char *
queue_dequeue(queue *q)
{
	const char *ret;
	pthread_mutex_lock(&q->lock);
	if (q->len == 0) {
		pthread_mutex_unlock(&q->lock);
		return NULL;
	}
	if (q->read == q->end)
		q->read = q->queue;
	ret = q->read++;
	q->len--;
	pthread_mutex_unlock(&q->lock);
	return ret;
}

/**
 * Returns at most len elements from the queue.  Attempts to use a
 * single lock to read a vector of elements from the queue to minimise
 * effects of locking.  Returns the number of elements stored in ret.
 * The caller is responsible for freeing elements from ret, as well as
 * making sure it is large enough to store len elements.
 */
size_t
queue_dequeue_vector(const char **ret, queue *q, size_t len)
{
	size_t i;

	pthread_mutex_lock(&q->lock);
	if (q->len == 0) {
		pthread_mutex_unlock(&q->lock);
		return 0;
	}
	if (len > q->len)
		len = q->len;
	for (i = 0; i < len; i++) {
		if (q->read == q->end)
			q->read = q->queue;
		ret[i] = q->read++;
	}
	q->len -= len;
	pthread_mutex_unlock(&q->lock);

	return len;
}

/**
 * Returns the (approximate) size of entries waiting to be read in the
 * queue.  The returned value cannot be taken accurate with multiple
 * readers/writers concurrently in action.  Hence it can only be seen as
 * mere hint about the state of the queue.
 */
inline size_t
queue_len(queue *q)
{
	return q->len;
}
