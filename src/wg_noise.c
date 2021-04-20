/* SPDX-License-Identifier: ISC
 *
 * Copyright (C) 2015-2021 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright (C) 2019-2021 Matt Dunwoodie <ncon@noconroy.net>
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/refcount.h>
#include <sys/epoch.h>
#include <sys/ck.h>

#include "crypto.h"
#include "wg_noise.h"
#include "support.h"

/* Protocol string constants */
#define NOISE_HANDSHAKE_NAME	"Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s"
#define NOISE_IDENTIFIER_NAME	"WireGuard v1 zx2c4 Jason@zx2c4.com"

/* Constants for the counter */
#define COUNTER_BITS_TOTAL	8192
#define COUNTER_BITS		(sizeof(unsigned long) * 8)
#define COUNTER_NUM		(COUNTER_BITS_TOTAL / COUNTER_BITS)
#define COUNTER_WINDOW_SIZE	(COUNTER_BITS_TOTAL - COUNTER_BITS)

/* Constants for the keypair */
#define REKEY_AFTER_MESSAGES	(1ull << 60)
#define REJECT_AFTER_MESSAGES	(UINT64_MAX - COUNTER_WINDOW_SIZE - 1)
#define REKEY_AFTER_TIME	120
#define REKEY_AFTER_TIME_RECV	165
#define REJECT_INTERVAL		(1000000000 / 50) /* fifty times per sec */
/* 24 = floor(log2(REJECT_INTERVAL)) */
#define REJECT_INTERVAL_MASK	(~((1ull<<24)-1))
#define TIMER_RESET		(SBT_1S * -(REKEY_TIMEOUT+1))

#define HT_INDEX_SIZE		(1 << 13)
#define HT_INDEX_MASK		(HT_INDEX_SIZE - 1)
#define HT_REMOTE_SIZE		(1 << 11)
#define HT_REMOTE_MASK		(HT_REMOTE_SIZE - 1)
#define MAX_REMOTE_PER_LOCAL	(1 << 20)

struct noise_index {
	CK_LIST_ENTRY(noise_index)	 i_entry;
	uint32_t			 i_local_index;
	uint32_t			 i_remote_index;
	int				 i_is_keypair;
};

struct noise_keypair {
	struct noise_index		 kp_index;
	u_int				 kp_refcnt;
	int				 kp_can_send;
	int				 kp_is_initiator;
	sbintime_t			 kp_birthdate; /* sbinuptime */
	struct noise_remote		*kp_remote;

	uint8_t				 kp_send[NOISE_SYMMETRIC_KEY_LEN];
	uint8_t				 kp_recv[NOISE_SYMMETRIC_KEY_LEN];

	/* Counter elements */
	struct rwlock			 kp_nonce_lock;
	uint64_t			 kp_nonce_send;
	uint64_t			 kp_nonce_recv;
	unsigned long			 kp_backtrack[COUNTER_NUM];

	struct epoch_context		 kp_smr;
};

struct noise_handshake {
	uint8_t	 			 hs_e[NOISE_PUBLIC_KEY_LEN];
	uint8_t	 			 hs_hash[NOISE_HASH_LEN];
	uint8_t	 			 hs_ck[NOISE_HASH_LEN];
};

struct noise_remote {
	struct noise_index		 r_index;

	CK_LIST_ENTRY(noise_remote) 	 r_entry;
	int				 r_entry_valid;
	uint8_t				 r_public[NOISE_PUBLIC_KEY_LEN];

	struct rwlock			 r_handshake_lock;
	struct noise_handshake		 r_handshake;
	int				 r_handshake_alive;
	int				 r_handshake_initiator;
	sbintime_t			 r_last_sent; /* sbinuptime */
	sbintime_t			 r_last_init_recv; /* sbinuptime */
	uint8_t				 r_timestamp[NOISE_TIMESTAMP_LEN];
	uint8_t				 r_psk[NOISE_SYMMETRIC_KEY_LEN];
	uint8_t		 		 r_ss[NOISE_PUBLIC_KEY_LEN];

	u_int				 r_refcnt;
	struct noise_local		*r_local;
	void				*r_arg;

	struct rwlock			 r_keypair_lock;
	struct noise_keypair		*r_next, *r_current, *r_previous;

	struct epoch_context		 r_smr;
	void				(*r_cleanup)(struct noise_remote *);
};

struct noise_local {
	struct rwlock			 l_identity_lock;
	int				 l_has_identity;
	uint8_t				 l_public[NOISE_PUBLIC_KEY_LEN];
	uint8_t				 l_private[NOISE_PUBLIC_KEY_LEN];

	u_int				 l_refcnt;
	SIPHASH_KEY			 l_hash_key;
	void				*l_arg;
	void				(*l_cleanup)(struct noise_local *);

	struct rwlock			 l_remote_lock;
	size_t				 l_remote_num;
	CK_LIST_HEAD(,noise_remote)	 l_remote_hash[HT_REMOTE_SIZE];

	struct rwlock			 l_index_lock;
	CK_LIST_HEAD(,noise_index)	 l_index_hash[HT_INDEX_SIZE];
};

static void	noise_precompute_ss(struct noise_local *, struct noise_remote *);

static void	noise_remote_index_insert(struct noise_local *, struct noise_remote *);
static struct noise_remote *
		noise_remote_index_lookup(struct noise_local *, uint32_t, int);
static int	noise_remote_index_remove(struct noise_local *, struct noise_remote *);
static void	noise_remote_expire_current(struct noise_remote *);

static void	noise_add_new_keypair(struct noise_local *, struct noise_remote *, struct noise_keypair *);
static int	noise_begin_session(struct noise_remote *);
static void	noise_keypair_drop(struct noise_keypair *);

static void	noise_kdf(uint8_t *, uint8_t *, uint8_t *, const uint8_t *,
		    size_t, size_t, size_t, size_t,
		    const uint8_t [NOISE_HASH_LEN]);
static int	noise_mix_dh(uint8_t [NOISE_HASH_LEN], uint8_t [NOISE_SYMMETRIC_KEY_LEN],
		    const uint8_t [NOISE_PUBLIC_KEY_LEN],
		    const uint8_t [NOISE_PUBLIC_KEY_LEN]);
static int	noise_mix_ss(uint8_t ck[NOISE_HASH_LEN], uint8_t [NOISE_SYMMETRIC_KEY_LEN],
		    const uint8_t [NOISE_PUBLIC_KEY_LEN]);
static void	noise_mix_hash(uint8_t [NOISE_HASH_LEN], const uint8_t *, size_t);
static void	noise_mix_psk(uint8_t [NOISE_HASH_LEN], uint8_t [NOISE_HASH_LEN],
		    uint8_t [NOISE_SYMMETRIC_KEY_LEN], const uint8_t [NOISE_SYMMETRIC_KEY_LEN]);
static void	noise_param_init(uint8_t [NOISE_HASH_LEN], uint8_t [NOISE_HASH_LEN],
		    const uint8_t [NOISE_PUBLIC_KEY_LEN]);
static void	noise_msg_encrypt(uint8_t *, const uint8_t *, size_t,
		    uint8_t [NOISE_SYMMETRIC_KEY_LEN], uint8_t [NOISE_HASH_LEN]);
static int	noise_msg_decrypt(uint8_t *, const uint8_t *, size_t,
		    uint8_t [NOISE_SYMMETRIC_KEY_LEN], uint8_t [NOISE_HASH_LEN]);
static void	noise_msg_ephemeral(uint8_t [NOISE_HASH_LEN], uint8_t [NOISE_HASH_LEN],
		    const uint8_t [NOISE_PUBLIC_KEY_LEN]);
static void	noise_tai64n_now(uint8_t [NOISE_TIMESTAMP_LEN]);
static int	noise_timer_expired(sbintime_t, uint32_t, uint32_t);

/* I can't find where FreeBSD defines such behaviours, so that is temporarily here. */
#define epoch_ptr_read(p) ck_pr_load_ptr(p)
#define epoch_ptr_write(p, v) ck_pr_store_ptr(p, v)
/* Back to regular programming... */

MALLOC_DEFINE(M_NOISE, "NOISE", "wgnoise");

/* Local configuration */
struct noise_local *
noise_local_alloc(void *arg)
{
	struct noise_local *l;
	size_t i;

	if ((l = malloc(sizeof(*l), M_NOISE, M_NOWAIT)) == NULL)
		return (NULL);

	rw_init(&l->l_identity_lock, "noise_identity");
	l->l_has_identity = 0;
	bzero(l->l_public, NOISE_PUBLIC_KEY_LEN);
	bzero(l->l_private, NOISE_PUBLIC_KEY_LEN);

	refcount_init(&l->l_refcnt, 1);
	arc4random_buf(&l->l_hash_key, sizeof(l->l_hash_key));
	l->l_arg = arg;
	l->l_cleanup = NULL;

	rw_init(&l->l_remote_lock, "noise_remote");
	l->l_remote_num = 0;
	for (i = 0; i < HT_REMOTE_SIZE; i++)
		CK_LIST_INIT(&l->l_remote_hash[i]);

	rw_init(&l->l_index_lock, "noise_index");
	for (i = 0; i < HT_INDEX_SIZE; i++)
		CK_LIST_INIT(&l->l_index_hash[i]);

	return (l);
}

struct noise_local *
noise_local_ref(struct noise_local *l)
{
	refcount_acquire(&l->l_refcnt);
	return (l);
}

void
noise_local_put(struct noise_local *l)
{
	if (refcount_release(&l->l_refcnt)) {
		if (l->l_cleanup != NULL)
			l->l_cleanup(l);
		explicit_bzero(l, sizeof(*l));
		free(l, M_NOISE);
	}
}

void
noise_local_free(struct noise_local *l, void (*cleanup)(struct noise_local *))
{
	l->l_cleanup = cleanup;
	noise_local_put(l);
}

void *
noise_local_arg(struct noise_local *l)
{
	return (l->l_arg);
}

void
noise_local_private(struct noise_local *l, const uint8_t private[NOISE_PUBLIC_KEY_LEN])
{
	struct epoch_tracker et;
	struct noise_remote *r;
	size_t i;

	rw_wlock(&l->l_identity_lock);
	memcpy(l->l_private, private, NOISE_PUBLIC_KEY_LEN);
	curve25519_clamp_secret(l->l_private);
	l->l_has_identity = curve25519_generate_public(l->l_public, l->l_private);

	NET_EPOCH_ENTER(et);
	for (i = 0; i < HT_REMOTE_SIZE; i++) {
		CK_LIST_FOREACH(r, &l->l_remote_hash[i], r_entry) {
			noise_precompute_ss(l, r);
			noise_remote_expire_current(r);
		}
	}
	NET_EPOCH_EXIT(et);
	rw_wunlock(&l->l_identity_lock);
}

int
noise_local_keys(struct noise_local *l, uint8_t public[NOISE_PUBLIC_KEY_LEN],
    uint8_t private[NOISE_PUBLIC_KEY_LEN])
{
	int has_identity;
	rw_rlock(&l->l_identity_lock);
	if ((has_identity = l->l_has_identity)) {
		if (public != NULL)
			memcpy(public, l->l_public, NOISE_PUBLIC_KEY_LEN);
		if (private != NULL)
			memcpy(private, l->l_private, NOISE_PUBLIC_KEY_LEN);
	}
	rw_runlock(&l->l_identity_lock);
	return (has_identity ? 0 : ENXIO);
}

static void
noise_precompute_ss(struct noise_local *l, struct noise_remote *r)
{
	rw_wlock(&r->r_handshake_lock);
	if (!l->l_has_identity ||
	    !curve25519(r->r_ss, l->l_private, r->r_public))
		bzero(r->r_ss, NOISE_PUBLIC_KEY_LEN);
	rw_wunlock(&r->r_handshake_lock);
}

/* Remote configuration */
struct noise_remote *
noise_remote_alloc(struct noise_local *l, void *arg,
    const uint8_t public[NOISE_PUBLIC_KEY_LEN])
{
	struct noise_remote *r;

	if ((r = malloc(sizeof(*r), M_NOISE, M_NOWAIT)) == NULL)
		return (NULL);

	r->r_index.i_is_keypair = 0;
	r->r_entry_valid = 0;

	memcpy(r->r_public, public, NOISE_PUBLIC_KEY_LEN);

	rw_init(&r->r_handshake_lock, "noise_handshake");
	bzero(&r->r_handshake, sizeof(r->r_handshake));
	r->r_handshake_alive = 0;
	r->r_handshake_initiator = 0;
	r->r_last_sent = TIMER_RESET;
	r->r_last_init_recv = TIMER_RESET;
	bzero(r->r_timestamp, NOISE_TIMESTAMP_LEN);
	bzero(r->r_psk, sizeof(r->r_psk));
	noise_precompute_ss(l, r);

	refcount_init(&r->r_refcnt, 1);
	r->r_local = noise_local_ref(l);
	r->r_arg = arg;

	rw_init(&r->r_keypair_lock, "noise_keypair");
	r->r_next = r->r_current = r->r_previous = NULL;

	bzero(&r->r_smr, sizeof(r->r_smr));

	return (r);
}

void
noise_remote_enable(struct noise_remote *r)
{
	struct noise_local *l = r->r_local;
	uint64_t idx;

	/* Insert to hashtable */
	idx = siphash24(&l->l_hash_key, r->r_public, NOISE_PUBLIC_KEY_LEN) & HT_REMOTE_MASK;

	rw_wlock(&l->l_remote_lock);
	if (!r->r_entry_valid && l->l_remote_num < MAX_REMOTE_PER_LOCAL) {
		r->r_entry_valid = 1;
		l->l_remote_num++;
		CK_LIST_INSERT_HEAD(&l->l_remote_hash[idx], r, r_entry);
	}
	rw_wunlock(&l->l_remote_lock);
}

void
noise_remote_disable(struct noise_remote *r)
{
	struct noise_local *l = r->r_local;
	/* remove from hashtable */
	rw_wlock(&l->l_remote_lock);
	if (r->r_entry_valid) {
		r->r_entry_valid = 1;
		CK_LIST_REMOVE(r, r_entry);
		l->l_remote_num--;
	};
	rw_wunlock(&l->l_remote_lock);
}

struct noise_remote *
noise_remote_lookup(struct noise_local *l, const uint8_t public[NOISE_PUBLIC_KEY_LEN])
{
	struct epoch_tracker et;
	struct noise_remote *r, *ret = NULL;
	uint64_t idx;

	idx = siphash24(&l->l_hash_key, public, NOISE_PUBLIC_KEY_LEN) & HT_REMOTE_MASK;

	NET_EPOCH_ENTER(et);
	CK_LIST_FOREACH(r, &l->l_remote_hash[idx], r_entry) {
		if (timingsafe_bcmp(r->r_public, public, NOISE_PUBLIC_KEY_LEN) == 0) {
			if (refcount_acquire_if_not_zero(&r->r_refcnt))
				ret = r;
			break;
		}
	}
	NET_EPOCH_EXIT(et);
	return (ret);
}

static void
noise_remote_index_insert(struct noise_local *l, struct noise_remote *r)
{
	struct noise_index *i, *r_i = &r->r_index;
	struct epoch_tracker et;
	uint32_t idx;

	noise_remote_index_remove(l, r);

	NET_EPOCH_ENTER(et);
assign_id:
	r_i->i_local_index = arc4random();
	idx = r_i->i_local_index & HT_INDEX_MASK;
	CK_LIST_FOREACH(i, &l->l_index_hash[idx], i_entry)
		if (i->i_local_index == r_i->i_local_index)
			goto assign_id;

	rw_wlock(&l->l_index_lock);
	CK_LIST_FOREACH(i, &l->l_index_hash[idx], i_entry) {
		if (i->i_local_index == r_i->i_local_index) {
			rw_wunlock(&l->l_index_lock);
			goto assign_id;
		}
	}
	CK_LIST_INSERT_HEAD(&l->l_index_hash[idx], r_i, i_entry);
	rw_wunlock(&l->l_index_lock);

	NET_EPOCH_EXIT(et);

	r->r_handshake_alive = 1;
}

static struct noise_remote *
noise_remote_index_lookup(struct noise_local *l, uint32_t idx0, int lookup_keypair)
{
	struct epoch_tracker et;
	struct noise_index *i;
	struct noise_keypair *kp;
	struct noise_remote *r, *ret = NULL;
	uint32_t idx = idx0 & HT_INDEX_MASK;

	NET_EPOCH_ENTER(et);
	CK_LIST_FOREACH(i, &l->l_index_hash[idx], i_entry) {
		if (i->i_local_index == idx0) {
			if (!i->i_is_keypair) {
				r = (struct noise_remote *) i;
			} else if (lookup_keypair) {
				kp = (struct noise_keypair *) i;
				r = kp->kp_remote;
			} else {
				break;
			}
			if (refcount_acquire_if_not_zero(&r->r_refcnt))
				ret = r;
			break;
		}
	}
	NET_EPOCH_EXIT(et);
	return (ret);
}

struct noise_remote *
noise_remote_index(struct noise_local *l, uint32_t idx) {
	return noise_remote_index_lookup(l, idx, 1);
}

static int
noise_remote_index_remove(struct noise_local *l, struct noise_remote *r)
{
	rw_assert_wrlock(&r->r_handshake_lock);
	if (r->r_handshake_alive) {
		rw_wlock(&l->l_index_lock);
		CK_LIST_REMOVE(&r->r_index, i_entry);
		rw_wunlock(&l->l_index_lock);
		r->r_handshake_alive = 0;
		return (1);
	}
	return (0);
}

struct noise_remote *
noise_remote_ref(struct noise_remote *r)
{
	refcount_acquire(&r->r_refcnt);
	return (r);
}

static void
noise_remote_smr_free(struct epoch_context *smr)
{
	struct noise_remote *r;
	r = __containerof(smr, struct noise_remote, r_smr);
	if (r->r_cleanup != NULL)
		r->r_cleanup(r);
	noise_local_put(r->r_local);
	explicit_bzero(r, sizeof(*r));
	free(r, M_NOISE);
}

void
noise_remote_put(struct noise_remote *r)
{
	if (refcount_release(&r->r_refcnt))
		NET_EPOCH_CALL(noise_remote_smr_free, &r->r_smr);
}

void
noise_remote_free(struct noise_remote *r, void (*cleanup)(struct noise_remote *))
{
	r->r_cleanup = cleanup;
	noise_remote_disable(r);

	/* now clear all keypairs and handshakes, then put this reference */
	noise_remote_handshake_clear(r);
	noise_remote_keypairs_clear(r);
	noise_remote_put(r);
}

struct noise_local *
noise_remote_local(struct noise_remote *r)
{
	return (noise_local_ref(r->r_local));
}

void *
noise_remote_arg(struct noise_remote *r)
{
	return (r->r_arg);
}

void
noise_remote_set_psk(struct noise_remote *r,
    const uint8_t psk[NOISE_SYMMETRIC_KEY_LEN])
{
	rw_wlock(&r->r_handshake_lock);
	if (psk == NULL)
		bzero(r->r_psk, NOISE_SYMMETRIC_KEY_LEN);
	else
		memcpy(r->r_psk, psk, NOISE_SYMMETRIC_KEY_LEN);
	rw_wunlock(&r->r_handshake_lock);
}

int
noise_remote_keys(struct noise_remote *r, uint8_t public[NOISE_PUBLIC_KEY_LEN],
    uint8_t psk[NOISE_SYMMETRIC_KEY_LEN])
{
	static uint8_t null_psk[NOISE_SYMMETRIC_KEY_LEN];
	int ret;

	if (public != NULL)
		memcpy(public, r->r_public, NOISE_PUBLIC_KEY_LEN);

	rw_rlock(&r->r_handshake_lock);
	if (psk != NULL)
		memcpy(psk, r->r_psk, NOISE_SYMMETRIC_KEY_LEN);
	ret = timingsafe_bcmp(r->r_psk, null_psk, NOISE_SYMMETRIC_KEY_LEN);
	rw_runlock(&r->r_handshake_lock);

	return (ret ? 0 : ENOENT);
}

int
noise_remote_initiation_expired(struct noise_remote *r)
{
	int expired;
	rw_rlock(&r->r_handshake_lock);
	expired = noise_timer_expired(r->r_last_sent, REKEY_TIMEOUT, 0);
	rw_runlock(&r->r_handshake_lock);
	return (expired);
}

void
noise_remote_handshake_clear(struct noise_remote *r)
{
	rw_wlock(&r->r_handshake_lock);
	if (noise_remote_index_remove(r->r_local, r))
		bzero(&r->r_handshake, sizeof(r->r_handshake));
	r->r_last_sent = TIMER_RESET;
	rw_wunlock(&r->r_handshake_lock);
}

void
noise_remote_keypairs_clear(struct noise_remote *r)
{
	struct noise_keypair *kp;

	rw_wlock(&r->r_keypair_lock);
	kp = epoch_ptr_read(&r->r_next);
	epoch_ptr_write(&r->r_next, NULL);
	noise_keypair_drop(kp);

	kp = epoch_ptr_read(&r->r_current);
	epoch_ptr_write(&r->r_current, NULL);
	noise_keypair_drop(kp);

	kp = epoch_ptr_read(&r->r_previous);
	epoch_ptr_write(&r->r_previous, NULL);
	noise_keypair_drop(kp);
	rw_wunlock(&r->r_keypair_lock);
}

static void
noise_remote_expire_current(struct noise_remote *r)
{
	struct epoch_tracker et;
	struct noise_keypair *kp;

	noise_remote_handshake_clear(r);

	NET_EPOCH_ENTER(et);
	kp = epoch_ptr_read(&r->r_next);
	if (kp != NULL) WRITE_ONCE(kp->kp_can_send, 0);
	kp = epoch_ptr_read(&r->r_current);
	if (kp != NULL) WRITE_ONCE(kp->kp_can_send, 0);
	NET_EPOCH_EXIT(et);
}

/* Keypair functions */
static void
noise_add_new_keypair(struct noise_local *l, struct noise_remote *r,
    struct noise_keypair *kp)
{
	struct noise_keypair *next, *current, *previous;
	struct noise_index *r_i = &r->r_index;

	/* Insert into the keypair table */
	rw_wlock(&r->r_keypair_lock);
	next = epoch_ptr_read(&r->r_next);
	current = epoch_ptr_read(&r->r_current);
	previous = epoch_ptr_read(&r->r_previous);

	if (kp->kp_is_initiator) {
		if (next != NULL) {
			epoch_ptr_write(&r->r_next, NULL);
			epoch_ptr_write(&r->r_previous, next);
			noise_keypair_drop(current);
		} else {
			epoch_ptr_write(&r->r_previous, current);
		}
		noise_keypair_drop(previous);
		epoch_ptr_write(&r->r_current, kp);
	} else {
		epoch_ptr_write(&r->r_next, kp);
		noise_keypair_drop(next);
		epoch_ptr_write(&r->r_previous, NULL);
		noise_keypair_drop(previous);

	}
	rw_wunlock(&r->r_keypair_lock);

	/* Insert into index table */
	rw_assert_wrlock(&r->r_handshake_lock);

	kp->kp_index.i_is_keypair = 1;
	kp->kp_index.i_local_index = r_i->i_local_index;
	kp->kp_index.i_remote_index = r_i->i_remote_index;

	rw_wlock(&l->l_index_lock);
	CK_LIST_INSERT_BEFORE(r_i, &kp->kp_index, i_entry);
	CK_LIST_REMOVE(r_i, i_entry);
	rw_wunlock(&l->l_index_lock);

	explicit_bzero(&r->r_handshake, sizeof(r->r_handshake));
}

static int
noise_begin_session(struct noise_remote *r)
{
	struct noise_keypair *kp;

	rw_assert_wrlock(&r->r_handshake_lock);

	if ((kp = malloc(sizeof(*kp), M_NOISE, M_NOWAIT)) == NULL)
		return (ENOSPC);

	refcount_init(&kp->kp_refcnt, 1);
	kp->kp_can_send = 1;
	kp->kp_is_initiator = r->r_handshake_initiator;
	kp->kp_birthdate = getsbinuptime();
	kp->kp_remote = noise_remote_ref(r);

	if (kp->kp_is_initiator)
		noise_kdf(kp->kp_send, kp->kp_recv, NULL, NULL,
		    NOISE_SYMMETRIC_KEY_LEN, NOISE_SYMMETRIC_KEY_LEN, 0, 0,
		    r->r_handshake.hs_ck);
	else
		noise_kdf(kp->kp_recv, kp->kp_send, NULL, NULL,
		    NOISE_SYMMETRIC_KEY_LEN, NOISE_SYMMETRIC_KEY_LEN, 0, 0,
		    r->r_handshake.hs_ck);

	rw_init(&kp->kp_nonce_lock, "noise_nonce");
	kp->kp_nonce_send = 0;
	kp->kp_nonce_recv = 0;
	bzero(kp->kp_backtrack, sizeof(kp->kp_backtrack));
	bzero(&kp->kp_smr, sizeof(kp->kp_smr));

	noise_add_new_keypair(r->r_local, r, kp);
	return (0);
}

struct noise_keypair *
noise_keypair_lookup(struct noise_local *l, uint32_t idx0)
{
	struct epoch_tracker et;
	struct noise_index *i;
	struct noise_keypair *kp, *ret = NULL;
	uint32_t idx = idx0 & HT_INDEX_MASK;

	NET_EPOCH_ENTER(et);
	CK_LIST_FOREACH(i, &l->l_index_hash[idx], i_entry) {
		if (i->i_local_index == idx0 && i->i_is_keypair) {
			kp = (struct noise_keypair *) i;
			if (refcount_acquire_if_not_zero(&kp->kp_refcnt))
				ret = kp;
			break;
		}
	}
	NET_EPOCH_EXIT(et);
	return (ret);
}

struct noise_keypair *
noise_keypair_current(struct noise_remote *r)
{
	struct epoch_tracker et;
	struct noise_keypair *kp, *ret = NULL;

	NET_EPOCH_ENTER(et);
	kp = epoch_ptr_read(&r->r_current);
	if (kp != NULL && READ_ONCE(kp->kp_can_send)) {
		if (noise_timer_expired(kp->kp_birthdate, REJECT_AFTER_TIME, 0))
			WRITE_ONCE(kp->kp_can_send, 0);
		else if (refcount_acquire_if_not_zero(&kp->kp_refcnt))
			ret = kp;
	}
	NET_EPOCH_EXIT(et);
	return (ret);
}

struct noise_keypair *
noise_keypair_ref(struct noise_keypair *kp)
{
	refcount_acquire(&kp->kp_refcnt);
	return (kp);
}

int
noise_keypair_received_with(struct noise_keypair *kp)
{
	struct noise_keypair *old;
	struct noise_remote *r = kp->kp_remote;

	if (kp != epoch_ptr_read(&r->r_next))
		return (0);

	rw_wlock(&r->r_keypair_lock);
	if (kp != epoch_ptr_read(&r->r_next)) {
		rw_wunlock(&r->r_keypair_lock);
		return (0);
	}

	old = epoch_ptr_read(&r->r_previous);
	epoch_ptr_write(&r->r_previous, epoch_ptr_read(&r->r_current));
	noise_keypair_drop(old);
	epoch_ptr_write(&r->r_current, kp);
	epoch_ptr_write(&r->r_next, NULL);
	rw_wunlock(&r->r_keypair_lock);

	return (ECONNRESET);
}

static void
noise_keypair_smr_free(struct epoch_context *smr)
{
	struct noise_keypair *kp;
	kp = __containerof(smr, struct noise_keypair, kp_smr);
	noise_remote_put(kp->kp_remote);
	explicit_bzero(kp, sizeof(*kp));
	free(kp, M_NOISE);
}

void
noise_keypair_put(struct noise_keypair *kp)
{
	if (refcount_release(&kp->kp_refcnt))
		NET_EPOCH_CALL(noise_keypair_smr_free, &kp->kp_smr);
}

static void
noise_keypair_drop(struct noise_keypair *kp)
{
	struct noise_remote *r;
	struct noise_local *l;

	if (kp == NULL)
		return;

	r = kp->kp_remote;
	l = r->r_local;

	rw_wlock(&l->l_index_lock);
	CK_LIST_REMOVE(&kp->kp_index, i_entry);
	rw_wunlock(&l->l_index_lock);

	noise_keypair_put(kp);
}

struct noise_remote *
noise_keypair_remote(struct noise_keypair *kp)
{
	return (noise_remote_ref(kp->kp_remote));
}

void *
noise_keypair_remote_arg(struct noise_keypair *kp)
{
	return kp->kp_remote->r_arg;
}

int
noise_keypair_nonce_next(struct noise_keypair *kp, uint64_t *send)
{
#ifdef __LP64__
	*send = atomic_fetchadd_64(&kp->kp_nonce_send, 1);
#else
	rw_wlock(&kp->kp_nonce_lock);
	*send = ctr->c_send++;
	rw_wunlock(&kp->kp_nonce_lock);
#endif
	if (*send < REJECT_AFTER_MESSAGES)
		return (0);
	WRITE_ONCE(kp->kp_can_send, 0);
	return (EINVAL);
}

int
noise_keypair_nonce_check(struct noise_keypair *kp, uint64_t recv)
{
	uint64_t i, top, index_recv, index_ctr;
	unsigned long bit;
	int ret = EEXIST;

	rw_wlock(&kp->kp_nonce_lock);

	/* Check that the recv counter is valid */
	if (kp->kp_nonce_recv >= REJECT_AFTER_MESSAGES ||
	    recv >= REJECT_AFTER_MESSAGES)
		goto error;

	/* If the packet is out of the window, invalid */
	if (recv + COUNTER_WINDOW_SIZE < kp->kp_nonce_recv)
		goto error;

	/* If the new counter is ahead of the current counter, we'll need to
	 * zero out the bitmap that has previously been used */
	index_recv = recv / COUNTER_BITS;
	index_ctr = kp->kp_nonce_recv / COUNTER_BITS;

	if (recv > kp->kp_nonce_recv) {
		top = MIN(index_recv - index_ctr, COUNTER_NUM);
		for (i = 1; i <= top; i++)
			kp->kp_backtrack[
			    (i + index_ctr) & (COUNTER_NUM - 1)] = 0;
		WRITE_ONCE(kp->kp_nonce_recv, recv);
	}

	index_recv %= COUNTER_NUM;
	bit = 1ul << (recv % COUNTER_BITS);

	if (kp->kp_backtrack[index_recv] & bit)
		goto error;

	kp->kp_backtrack[index_recv] |= bit;

	ret = 0;
error:
	rw_wunlock(&kp->kp_nonce_lock);
	return (ret);
}

int
noise_keep_key_fresh_send(struct noise_remote *r)
{
	struct epoch_tracker et;
	struct noise_keypair *current;
	int keep_key_fresh;

	NET_EPOCH_ENTER(et);
	current = epoch_ptr_read(&r->r_current);
	keep_key_fresh = current != NULL && READ_ONCE(current->kp_can_send) && (
	    READ_ONCE(current->kp_nonce_send) > REKEY_AFTER_MESSAGES ||
	    (current->kp_is_initiator && noise_timer_expired(current->kp_birthdate, REKEY_AFTER_TIME, 0)));
	NET_EPOCH_EXIT(et);

	return (keep_key_fresh ? ESTALE : 0);
}

int
noise_keep_key_fresh_recv(struct noise_remote *r)
{
	struct epoch_tracker et;
	struct noise_keypair *current;
	int keep_key_fresh;

	NET_EPOCH_ENTER(et);
	current = epoch_ptr_read(&r->r_current);
	keep_key_fresh = current != NULL && READ_ONCE(current->kp_can_send) &&
	    current->kp_is_initiator && noise_timer_expired(current->kp_birthdate,
			REJECT_AFTER_TIME - KEEPALIVE_TIMEOUT - REKEY_TIMEOUT, 0);
	NET_EPOCH_EXIT(et);

	return (keep_key_fresh ? ESTALE : 0);
}

int
noise_keypair_encrypt(struct noise_keypair *kp, uint32_t *r_idx, uint64_t nonce, struct mbuf *m)
{
	if (chacha20poly1305_encrypt_mbuf(m, nonce, kp->kp_send) == 0)
	       return (ENOMEM);

	*r_idx = kp->kp_index.i_remote_index;
	return (0);
}

int
noise_keypair_decrypt(struct noise_keypair *kp, uint64_t nonce, struct mbuf *m)
{
	if (READ_ONCE(kp->kp_nonce_recv) >= REJECT_AFTER_MESSAGES ||
	    noise_timer_expired(kp->kp_birthdate, REJECT_AFTER_TIME, 0))
		return (EINVAL);

	if (chacha20poly1305_decrypt_mbuf(m, nonce, kp->kp_recv) == 0)
		return (EINVAL);

	return (0);
}

/* Handshake functions */
int
noise_create_initiation(struct noise_remote *r,
    uint32_t *s_idx,
    uint8_t ue[NOISE_PUBLIC_KEY_LEN],
    uint8_t es[NOISE_PUBLIC_KEY_LEN + NOISE_AUTHTAG_LEN],
    uint8_t ets[NOISE_TIMESTAMP_LEN + NOISE_AUTHTAG_LEN])
{
	struct noise_handshake *hs = &r->r_handshake;
	struct noise_local *l = r->r_local;
	uint8_t key[NOISE_SYMMETRIC_KEY_LEN];
	int ret = EINVAL;

	rw_rlock(&l->l_identity_lock);
	rw_wlock(&r->r_handshake_lock);
	if (!l->l_has_identity)
		goto error;
	if (!noise_timer_expired(r->r_last_sent, REKEY_TIMEOUT, 0))
		goto error;
	noise_param_init(hs->hs_ck, hs->hs_hash, r->r_public);

	/* e */
	curve25519_generate_secret(hs->hs_e);
	if (curve25519_generate_public(ue, hs->hs_e) == 0)
		goto error;
	noise_msg_ephemeral(hs->hs_ck, hs->hs_hash, ue);

	/* es */
	if (noise_mix_dh(hs->hs_ck, key, hs->hs_e, r->r_public) != 0)
		goto error;

	/* s */
	noise_msg_encrypt(es, l->l_public,
	    NOISE_PUBLIC_KEY_LEN, key, hs->hs_hash);

	/* ss */
	if (noise_mix_ss(hs->hs_ck, key, r->r_ss) != 0)
		goto error;

	/* {t} */
	noise_tai64n_now(ets);
	noise_msg_encrypt(ets, ets,
	    NOISE_TIMESTAMP_LEN, key, hs->hs_hash);

	noise_remote_index_insert(l, r);
	r->r_last_sent = getsbinuptime();
	*s_idx = r->r_index.i_local_index;
	r->r_handshake_initiator = 1;
	ret = 0;
error:
	rw_wunlock(&r->r_handshake_lock);
	rw_runlock(&l->l_identity_lock);
	explicit_bzero(key, NOISE_SYMMETRIC_KEY_LEN);
	return (ret);
}

int
noise_consume_initiation(struct noise_local *l, struct noise_remote **rp,
    uint32_t s_idx,
    uint8_t ue[NOISE_PUBLIC_KEY_LEN],
    uint8_t es[NOISE_PUBLIC_KEY_LEN + NOISE_AUTHTAG_LEN],
    uint8_t ets[NOISE_TIMESTAMP_LEN + NOISE_AUTHTAG_LEN])
{
	struct noise_remote *r;
	struct noise_handshake hs;
	uint8_t key[NOISE_SYMMETRIC_KEY_LEN];
	uint8_t r_public[NOISE_PUBLIC_KEY_LEN];
	uint8_t	timestamp[NOISE_TIMESTAMP_LEN];
	int ret = EINVAL;

	rw_rlock(&l->l_identity_lock);
	if (!l->l_has_identity)
		goto error;
	noise_param_init(hs.hs_ck, hs.hs_hash, l->l_public);

	/* e */
	noise_msg_ephemeral(hs.hs_ck, hs.hs_hash, ue);

	/* es */
	if (noise_mix_dh(hs.hs_ck, key, l->l_private, ue) != 0)
		goto error;

	/* s */
	if (noise_msg_decrypt(r_public, es,
	    NOISE_PUBLIC_KEY_LEN + NOISE_AUTHTAG_LEN, key, hs.hs_hash) != 0)
		goto error;

	/* Lookup the remote we received from */
	if ((r = noise_remote_lookup(l, r_public)) == NULL)
		goto error;

	/* ss */
	if (noise_mix_ss(hs.hs_ck, key, r->r_ss) != 0)
		goto error_put;

	/* {t} */
	if (noise_msg_decrypt(timestamp, ets,
	    NOISE_TIMESTAMP_LEN + NOISE_AUTHTAG_LEN, key, hs.hs_hash) != 0)
		goto error_put;

	memcpy(hs.hs_e, ue, NOISE_PUBLIC_KEY_LEN);

	/* We have successfully computed the same results, now we ensure that
	 * this is not an initiation replay, or a flood attack */
	rw_wlock(&r->r_handshake_lock);

	/* Replay */
	if (memcmp(timestamp, r->r_timestamp, NOISE_TIMESTAMP_LEN) > 0)
		memcpy(r->r_timestamp, timestamp, NOISE_TIMESTAMP_LEN);
	else
		goto error_set;
	/* Flood attack */
	if (noise_timer_expired(r->r_last_init_recv, 0, REJECT_INTERVAL))
		r->r_last_init_recv = getsbinuptime();
	else
		goto error_set;

	/* Ok, we're happy to accept this initiation now */
	noise_remote_index_insert(l, r);
	r->r_index.i_remote_index = s_idx;
	r->r_handshake_initiator = 0;
	r->r_handshake = hs;
	*rp = noise_remote_ref(r);
	ret = 0;
error_set:
	rw_wunlock(&r->r_handshake_lock);
error_put:
	noise_remote_put(r);
error:
	rw_runlock(&l->l_identity_lock);
	explicit_bzero(key, NOISE_SYMMETRIC_KEY_LEN);
	explicit_bzero(&hs, sizeof(hs));
	return (ret);
}

int
noise_create_response(struct noise_remote *r,
    uint32_t *s_idx, uint32_t *r_idx,
    uint8_t ue[NOISE_PUBLIC_KEY_LEN],
    uint8_t en[0 + NOISE_AUTHTAG_LEN])
{
	struct noise_handshake *hs = &r->r_handshake;
	struct noise_local *l = r->r_local;
	uint8_t key[NOISE_SYMMETRIC_KEY_LEN];
	uint8_t e[NOISE_PUBLIC_KEY_LEN];
	int ret = EINVAL;

	rw_rlock(&l->l_identity_lock);
	rw_wlock(&r->r_handshake_lock);

	if (!r->r_handshake_alive || r->r_handshake_initiator)
		goto error;

	/* e */
	curve25519_generate_secret(e);
	if (curve25519_generate_public(ue, e) == 0)
		goto error;
	noise_msg_ephemeral(hs->hs_ck, hs->hs_hash, ue);

	/* ee */
	if (noise_mix_dh(hs->hs_ck, NULL, e, hs->hs_e) != 0)
		goto error;

	/* se */
	if (noise_mix_dh(hs->hs_ck, NULL, e, r->r_public) != 0)
		goto error;

	/* psk */
	noise_mix_psk(hs->hs_ck, hs->hs_hash, key, r->r_psk);

	/* {} */
	noise_msg_encrypt(en, NULL, 0, key, hs->hs_hash);

	if ((ret = noise_begin_session(r)) == 0) {
		r->r_last_sent = getsbinuptime();
		*s_idx = r->r_index.i_local_index;
		*r_idx = r->r_index.i_remote_index;
	}
error:
	rw_wunlock(&r->r_handshake_lock);
	rw_runlock(&l->l_identity_lock);
	explicit_bzero(key, NOISE_SYMMETRIC_KEY_LEN);
	explicit_bzero(e, NOISE_PUBLIC_KEY_LEN);
	return (ret);
}

int
noise_consume_response(struct noise_local *l, struct noise_remote **rp,
    uint32_t s_idx, uint32_t r_idx,
    uint8_t ue[NOISE_PUBLIC_KEY_LEN],
    uint8_t en[0 + NOISE_AUTHTAG_LEN])
{
	uint8_t preshared_key[NOISE_SYMMETRIC_KEY_LEN];
	uint8_t key[NOISE_SYMMETRIC_KEY_LEN];
	struct noise_handshake hs;
	struct noise_remote *r = NULL;
	int ret = EINVAL;

	if ((r = noise_remote_index_lookup(l, r_idx, 0)) == NULL)
		return (ret);

	rw_rlock(&l->l_identity_lock);
	if (!l->l_has_identity)
		goto error;

	rw_rlock(&r->r_handshake_lock);
	if (!r->r_handshake_alive || !r->r_handshake_initiator) {
		rw_runlock(&r->r_handshake_lock);
		goto error;
	}
	memcpy(preshared_key, r->r_psk, NOISE_SYMMETRIC_KEY_LEN);
	hs = r->r_handshake;
	rw_runlock(&r->r_handshake_lock);

	/* e */
	noise_msg_ephemeral(hs.hs_ck, hs.hs_hash, ue);

	/* ee */
	if (noise_mix_dh(hs.hs_ck, NULL, hs.hs_e, ue) != 0)
		goto error_zero;

	/* se */
	if (noise_mix_dh(hs.hs_ck, NULL, l->l_private, ue) != 0)
		goto error_zero;

	/* psk */
	noise_mix_psk(hs.hs_ck, hs.hs_hash, key, preshared_key);

	/* {} */
	if (noise_msg_decrypt(NULL, en,
	    0 + NOISE_AUTHTAG_LEN, key, hs.hs_hash) != 0)
		goto error_zero;

	rw_wlock(&r->r_handshake_lock);
	if (r->r_handshake_alive && r->r_handshake_initiator &&
	    r->r_index.i_local_index == r_idx) {
		r->r_handshake = hs;
		r->r_index.i_remote_index = s_idx;
		ret = noise_begin_session(r);
		*rp = noise_remote_ref(r);
	}
	rw_wunlock(&r->r_handshake_lock);
error_zero:
	explicit_bzero(preshared_key, NOISE_SYMMETRIC_KEY_LEN);
	explicit_bzero(key, NOISE_SYMMETRIC_KEY_LEN);
	explicit_bzero(&hs, sizeof(hs));
error:
	rw_runlock(&l->l_identity_lock);
	noise_remote_put(r);
	return (ret);
}

/* Handshake helper functions */
static void
noise_kdf(uint8_t *a, uint8_t *b, uint8_t *c, const uint8_t *x,
    size_t a_len, size_t b_len, size_t c_len, size_t x_len,
    const uint8_t ck[NOISE_HASH_LEN])
{
	uint8_t out[BLAKE2S_HASH_SIZE + 1];
	uint8_t sec[BLAKE2S_HASH_SIZE];

	/* Extract entropy from "x" into sec */
	blake2s_hmac(sec, x, ck, BLAKE2S_HASH_SIZE, x_len, NOISE_HASH_LEN);

	if (a == NULL || a_len == 0)
		goto out;

	/* Expand first key: key = sec, data = 0x1 */
	out[0] = 1;
	blake2s_hmac(out, out, sec, BLAKE2S_HASH_SIZE, 1, BLAKE2S_HASH_SIZE);
	memcpy(a, out, a_len);

	if (b == NULL || b_len == 0)
		goto out;

	/* Expand second key: key = sec, data = "a" || 0x2 */
	out[BLAKE2S_HASH_SIZE] = 2;
	blake2s_hmac(out, out, sec, BLAKE2S_HASH_SIZE, BLAKE2S_HASH_SIZE + 1,
			BLAKE2S_HASH_SIZE);
	memcpy(b, out, b_len);

	if (c == NULL || c_len == 0)
		goto out;

	/* Expand third key: key = sec, data = "b" || 0x3 */
	out[BLAKE2S_HASH_SIZE] = 3;
	blake2s_hmac(out, out, sec, BLAKE2S_HASH_SIZE, BLAKE2S_HASH_SIZE + 1,
			BLAKE2S_HASH_SIZE);
	memcpy(c, out, c_len);

out:
	/* Clear sensitive data from stack */
	explicit_bzero(sec, BLAKE2S_HASH_SIZE);
	explicit_bzero(out, BLAKE2S_HASH_SIZE + 1);
}

static int
noise_mix_dh(uint8_t ck[NOISE_HASH_LEN], uint8_t key[NOISE_SYMMETRIC_KEY_LEN],
    const uint8_t private[NOISE_PUBLIC_KEY_LEN],
    const uint8_t public[NOISE_PUBLIC_KEY_LEN])
{
	uint8_t dh[NOISE_PUBLIC_KEY_LEN];

	if (!curve25519(dh, private, public))
		return (EINVAL);
	noise_kdf(ck, key, NULL, dh,
	    NOISE_HASH_LEN, NOISE_SYMMETRIC_KEY_LEN, 0, NOISE_PUBLIC_KEY_LEN, ck);
	explicit_bzero(dh, NOISE_PUBLIC_KEY_LEN);
	return (0);
}

static int
noise_mix_ss(uint8_t ck[NOISE_HASH_LEN], uint8_t key[NOISE_SYMMETRIC_KEY_LEN],
    const uint8_t ss[NOISE_PUBLIC_KEY_LEN])
{
	static uint8_t null_point[NOISE_PUBLIC_KEY_LEN];
	if (timingsafe_bcmp(ss, null_point, NOISE_PUBLIC_KEY_LEN) == 0)
		return (ENOENT);
	noise_kdf(ck, key, NULL, ss,
	    NOISE_HASH_LEN, NOISE_SYMMETRIC_KEY_LEN, 0, NOISE_PUBLIC_KEY_LEN, ck);
	return (0);
}

static void
noise_mix_hash(uint8_t hash[NOISE_HASH_LEN], const uint8_t *src,
    size_t src_len)
{
	struct blake2s_state blake;

	blake2s_init(&blake, NOISE_HASH_LEN);
	blake2s_update(&blake, hash, NOISE_HASH_LEN);
	blake2s_update(&blake, src, src_len);
	blake2s_final(&blake, hash);
}

static void
noise_mix_psk(uint8_t ck[NOISE_HASH_LEN], uint8_t hash[NOISE_HASH_LEN],
    uint8_t key[NOISE_SYMMETRIC_KEY_LEN],
    const uint8_t psk[NOISE_SYMMETRIC_KEY_LEN])
{
	uint8_t tmp[NOISE_HASH_LEN];

	noise_kdf(ck, tmp, key, psk,
	    NOISE_HASH_LEN, NOISE_HASH_LEN, NOISE_SYMMETRIC_KEY_LEN,
	    NOISE_SYMMETRIC_KEY_LEN, ck);
	noise_mix_hash(hash, tmp, NOISE_HASH_LEN);
	explicit_bzero(tmp, NOISE_HASH_LEN);
}

static void
noise_param_init(uint8_t ck[NOISE_HASH_LEN], uint8_t hash[NOISE_HASH_LEN],
    const uint8_t s[NOISE_PUBLIC_KEY_LEN])
{
	struct blake2s_state blake;

	blake2s(ck, (uint8_t *)NOISE_HANDSHAKE_NAME, NULL,
	    NOISE_HASH_LEN, strlen(NOISE_HANDSHAKE_NAME), 0);
	blake2s_init(&blake, NOISE_HASH_LEN);
	blake2s_update(&blake, ck, NOISE_HASH_LEN);
	blake2s_update(&blake, (uint8_t *)NOISE_IDENTIFIER_NAME,
	    strlen(NOISE_IDENTIFIER_NAME));
	blake2s_final(&blake, hash);

	noise_mix_hash(hash, s, NOISE_PUBLIC_KEY_LEN);
}

static void
noise_msg_encrypt(uint8_t *dst, const uint8_t *src, size_t src_len,
    uint8_t key[NOISE_SYMMETRIC_KEY_LEN], uint8_t hash[NOISE_HASH_LEN])
{
	/* Nonce always zero for Noise_IK */
	chacha20poly1305_encrypt(dst, src, src_len,
	    hash, NOISE_HASH_LEN, 0, key);
	noise_mix_hash(hash, dst, src_len + NOISE_AUTHTAG_LEN);
}

static int
noise_msg_decrypt(uint8_t *dst, const uint8_t *src, size_t src_len,
    uint8_t key[NOISE_SYMMETRIC_KEY_LEN], uint8_t hash[NOISE_HASH_LEN])
{
	/* Nonce always zero for Noise_IK */
	if (!chacha20poly1305_decrypt(dst, src, src_len,
	    hash, NOISE_HASH_LEN, 0, key))
		return (EINVAL);
	noise_mix_hash(hash, src, src_len);
	return (0);
}

static void
noise_msg_ephemeral(uint8_t ck[NOISE_HASH_LEN], uint8_t hash[NOISE_HASH_LEN],
    const uint8_t src[NOISE_PUBLIC_KEY_LEN])
{
	noise_mix_hash(hash, src, NOISE_PUBLIC_KEY_LEN);
	noise_kdf(ck, NULL, NULL, src, NOISE_HASH_LEN, 0, 0,
		  NOISE_PUBLIC_KEY_LEN, ck);
}

static void
noise_tai64n_now(uint8_t output[NOISE_TIMESTAMP_LEN])
{
	struct timespec time;
	uint64_t sec;
	uint32_t nsec;

	getnanotime(&time);

	/* Round down the nsec counter to limit precise timing leak. */
	time.tv_nsec &= REJECT_INTERVAL_MASK;

	/* https://cr.yp.to/libtai/tai64.html */
	sec = htobe64(0x400000000000000aULL + time.tv_sec);
	nsec = htobe32(time.tv_nsec);

	/* memcpy to output buffer, assuming output could be unaligned. */
	memcpy(output, &sec, sizeof(sec));
	memcpy(output + sizeof(sec), &nsec, sizeof(nsec));
}

static int
noise_timer_expired(sbintime_t timer, uint32_t sec, uint32_t nsec)
{
	sbintime_t now = getsbinuptime();
	return (now > (timer + sec * SBT_1S + nstosbt(nsec))) ? ETIMEDOUT : 0;
}
