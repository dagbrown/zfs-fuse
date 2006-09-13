/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Copyright 2006 Ricardo Correia
 * Use is subject to license terms.
 */

#include <sys/thread.h>
#include <sys/debug.h>
#include <sys/types.h>

#include <pthread.h>

kthread_t *
zk_thread_create(void (*func)(), void *arg)
{
	pthread_t tid;

	pthread_attr_t attr;
	pthread_attr_init(&attr);

	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	VERIFY(pthread_create(&tid, &attr, (void *(*)(void *)) func, arg) == 0);

	pthread_attr_destroy(&attr);

	return ((void *)(uintptr_t)tid);
}
