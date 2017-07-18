/*
 * Copyright (c) 2004 Jeffrey M. Hsu.  All rights reserved.
 * Copyright (c) 2004 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Jeffrey M. Hsu.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Copyright (c) 1980, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)netisr.h	8.1 (Berkeley) 6/10/93
 * $FreeBSD: src/sys/net/netisr.h,v 1.21.2.5 2002/02/09 23:02:39 luigi Exp $
 */

#ifndef _NET_NETISR2_H_
#define _NET_NETISR2_H_

#ifndef _KERNEL
#error "kernel only header file"
#endif

#include <sys/systm.h>
#include <sys/msgport2.h>
#include <sys/thread.h>
#include <net/netisr.h>

extern struct thread *netisr_threads[MAXCPU];

/*
 * Return the message port for the general protocol message servicing
 * thread for a particular cpu.
 */
static __inline lwkt_port_t
netisr_cpuport(int cpu)
{
	KKASSERT(cpu >= 0 && cpu < ncpus);
	return &netisr_threads[cpu]->td_msgport;
}

/*
 * Return the current cpu's network protocol thread.
 */
static __inline lwkt_port_t
netisr_curport(void)
{
	return netisr_cpuport(mycpuid);
}

/*
 * Return the LSB of the hash.
 */
static __inline uint32_t
netisr_hashlsb(uint32_t hash)
{

	return (hash & NETISR_CPUMASK);
}

/*
 * Return the cpu for the hash.
 */
static __inline int
netisr_hashcpu(uint16_t hash)
{

	return (netisr_hashlsb(hash) % netisr_ncpus);
}

/*
 * Return the message port for the general protocol message servicing
 * thread for the hash.
 */
static __inline lwkt_port_t
netisr_hashport(uint16_t hash)
{
	return netisr_cpuport(netisr_hashcpu(hash));
}

#define IS_NETISR(td, n)	(&(td)->td_msgport == netisr_cpuport((n)))
#define ASSERT_IS_NETISR(td, n)	\
	KASSERT(IS_NETISR((td), (n)), ("thread %p is not netisr%d", (td), (n)))
#define ASSERT_IN_NETISR(n)	ASSERT_IS_NETISR(curthread, (n))
#define ASSERT_CANDOMSG_NETISR0(td) \
	KASSERT((td)->td_type != TD_TYPE_NETISR || IS_NETISR((td), 0), \
	    ("can't domsg to netisr0 from thread %p", (td)))

static __inline int
netisr_domsg_port(struct netmsg_base *nm, lwkt_port_t port)
{

#ifdef INVARIANTS
	if (port == netisr_cpuport(0))
		ASSERT_CANDOMSG_NETISR0(curthread);
#endif
	return (lwkt_domsg(port, &nm->lmsg, 0));
}

static __inline int
netisr_domsg(struct netmsg_base *nm, int cpu)
{

	return (netisr_domsg_port(nm, netisr_cpuport(cpu)));
}

static __inline void
netisr_sendmsg(struct netmsg_base *nm, int cpu)
{

	lwkt_sendmsg(netisr_cpuport(cpu), &nm->lmsg);
}

static __inline void
netisr_replymsg(struct netmsg_base *nm, int error)
{

	lwkt_replymsg(&nm->lmsg, error);
}

/*
 * To all netisrs, instead of netisr_ncpus.
 */
static __inline void
netisr_forwardmsg_all(struct netmsg_base *nm, int next_cpu)
{

	KKASSERT(next_cpu > mycpuid && next_cpu <= ncpus);
	if (next_cpu < ncpus)
		lwkt_forwardmsg(netisr_cpuport(next_cpu), &nm->lmsg);
	else
		netisr_replymsg(nm, 0);
}

#endif	/* _NET_NETISR2_H_ */
