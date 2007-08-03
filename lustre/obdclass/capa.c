/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/obdclass/capa.c
 *  Lustre Capability Hash Management
 *
 *  Copyright (c) 2005 Cluster File Systems, Inc.
 *   Author: Lai Siyao<lsy@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#define DEBUG_SUBSYSTEM S_SEC

#ifdef __KERNEL__
#include <linux/version.h>
#include <linux/fs.h>
#include <asm/unistd.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>

#include <obd_class.h>
#include <lustre_debug.h>
#include <lustre/lustre_idl.h>
#else
#include <liblustre.h>
#endif

#include <libcfs/list.h>
#include <lustre_capa.h>

#define NR_CAPAHASH 32
#define CAPA_HASH_SIZE 3000              /* for MDS & OSS */

cfs_mem_cache_t *capa_cachep = NULL;

#ifdef __KERNEL__
/* lock for capa hash/capa_list/fo_capa_keys */
spinlock_t capa_lock = SPIN_LOCK_UNLOCKED;

struct list_head capa_list[CAPA_SITE_MAX];

static struct capa_hmac_alg capa_hmac_algs[] = {
        DEF_CAPA_HMAC_ALG("sha1", SHA1, 20, 20),
};
#endif
/* capa count */
int capa_count[CAPA_SITE_MAX] = { 0, };

EXPORT_SYMBOL(capa_cachep);
EXPORT_SYMBOL(capa_list);
EXPORT_SYMBOL(capa_lock);
EXPORT_SYMBOL(capa_count);

struct hlist_head *init_capa_hash(void)
{
        struct hlist_head *hash;
        int nr_hash, i;

        OBD_ALLOC(hash, PAGE_SIZE);
        if (!hash)
                return NULL;

        nr_hash = PAGE_SIZE / sizeof(struct hlist_head);
        LASSERT(nr_hash > NR_CAPAHASH);

        for (i = 0; i < NR_CAPAHASH; i++)
                INIT_HLIST_HEAD(hash + i);
        return hash;
}

#ifdef __KERNEL__
static inline int capa_on_server(struct obd_capa *ocapa)
{
        return ocapa->c_site == CAPA_SITE_SERVER;
}

static inline void capa_delete(struct obd_capa *ocapa)
{
        LASSERT(capa_on_server(ocapa));
        hlist_del(&ocapa->u.tgt.c_hash);
        list_del(&ocapa->c_list);
        capa_count[ocapa->c_site]--;
        free_capa(ocapa);
}

void cleanup_capa_hash(struct hlist_head *hash)
{
        int i;
        struct hlist_node *pos, *next;
        struct obd_capa *oc;

        spin_lock(&capa_lock);
        for (i = 0; i < NR_CAPAHASH; i++) {
                hlist_for_each_entry_safe(oc, pos, next, hash + i, u.tgt.c_hash)
                        capa_delete(oc);
        }
        spin_unlock(&capa_lock);

        OBD_FREE(hash, PAGE_SIZE);
}

static inline int const capa_hashfn(struct lu_fid *fid)
{
        return (fid_oid(fid) ^ fid_ver(fid)) *
               (unsigned long)(fid_seq(fid) + 1) % NR_CAPAHASH;
}

/* capa renewal time check is earlier than that on client, which is to prevent
 * client renew right after obtaining it. */
static inline int capa_is_to_expire(struct obd_capa *oc)
{
        return cfs_time_before(cfs_time_sub(oc->c_expiry,
                                   cfs_time_seconds(oc->c_capa.lc_timeout)*2/3),
                               cfs_time_current());
}

static struct obd_capa *find_capa(struct lustre_capa *capa,
                                  struct hlist_head *head, int alive)
{
        struct hlist_node *pos;
        struct obd_capa *ocapa;
        int len = alive ? offsetof(struct lustre_capa, lc_keyid):sizeof(*capa);

        hlist_for_each_entry(ocapa, pos, head, u.tgt.c_hash) {
                if (memcmp(&ocapa->c_capa, capa, len))
                        continue;
                /* don't return one that will expire soon in this case */
                if (alive && capa_is_to_expire(ocapa))
                        continue;

                LASSERT(capa_on_server(ocapa));

                DEBUG_CAPA(D_SEC, &ocapa->c_capa, "found");
                return ocapa;
        }

        return NULL;
}

#define LRU_CAPA_DELETE_COUNT 12
static inline void capa_delete_lru(struct list_head *head)
{
        struct obd_capa *ocapa;
        struct list_head *node = head->next;
        int count = 0;

        /* free LRU_CAPA_DELETE_COUNT unused capa from head */
        while (count++ < LRU_CAPA_DELETE_COUNT) {
                ocapa = list_entry(node, struct obd_capa, c_list);
                node = node->next;
                if (atomic_read(&ocapa->c_refc))
                        continue;

                DEBUG_CAPA(D_SEC, &ocapa->c_capa, "free lru");
                capa_delete(ocapa);
        }
}

/* add or update */
struct obd_capa *capa_add(struct hlist_head *hash, struct lustre_capa *capa)
{
        struct hlist_head *head = hash + capa_hashfn(&capa->lc_fid);
        struct obd_capa *ocapa, *old = NULL;
        struct list_head *list = &capa_list[CAPA_SITE_SERVER];

        ocapa = alloc_capa(CAPA_SITE_SERVER);
        if (!ocapa)
                return NULL;

        spin_lock(&capa_lock);
        old = find_capa(capa, head, 0);
        if (!old) {
                ocapa->c_capa = *capa;
                set_capa_expiry(ocapa);
                hlist_add_head(&ocapa->u.tgt.c_hash, head);
                list_add_tail(&ocapa->c_list, list);
                capa_count[CAPA_SITE_SERVER]++;
                capa_get(ocapa);

                if (capa_count[CAPA_SITE_SERVER] > CAPA_HASH_SIZE)
                        capa_delete_lru(list);

                DEBUG_CAPA(D_SEC, &ocapa->c_capa, "new");
                                        
                spin_unlock(&capa_lock);
                return ocapa;
        }

        capa_get(old);
        spin_unlock(&capa_lock);

        DEBUG_CAPA(D_SEC, &old->c_capa, "update");

        free_capa(ocapa);
        return old;
}

struct obd_capa *capa_lookup(struct hlist_head *hash, struct lustre_capa *capa,
                             int alive)
{
        struct obd_capa *ocapa;

        spin_lock(&capa_lock);
        ocapa = find_capa(capa, hash + capa_hashfn(&capa->lc_fid), alive);
        if (ocapa) {
                list_move_tail(&ocapa->c_list, &capa_list[CAPA_SITE_SERVER]);
                capa_get(ocapa);
        }
        spin_unlock(&capa_lock);

        return ocapa;
}

int capa_hmac(__u8 *hmac, struct lustre_capa *capa, __u8 *key)
{
        struct crypto_tfm *tfm;
        struct capa_hmac_alg *alg;
        int keylen;
        struct scatterlist sl = {
                .page   = virt_to_page(capa),
                .offset = (unsigned long)(capa) % PAGE_SIZE,
                .length = offsetof(struct lustre_capa, lc_hmac),
        };

        if (capa_alg(capa) != CAPA_HMAC_ALG_SHA1) {
                CERROR("unknown capability hmac algorithm!\n");
                return -EFAULT;
        }

        alg = &capa_hmac_algs[capa_alg(capa)];

        tfm = crypto_alloc_tfm(alg->ha_name, 0);
        if (!tfm) {
                CERROR("crypto_alloc_tfm failed, check whether your kernel"
                       "has crypto support!\n");
                return -ENOMEM;
        }
        keylen = alg->ha_keylen;

        crypto_hmac(tfm, key, &keylen, &sl, 1, hmac);
        crypto_free_tfm(tfm);

        return 0;
}
#endif

void capa_cpy(void *capa, struct obd_capa *ocapa)
{
        spin_lock(&ocapa->c_lock);
        *(struct lustre_capa *)capa = ocapa->c_capa;
        spin_unlock(&ocapa->c_lock);
}

char *dump_capa_content(char *buf, char *key, int len)
{
        int i, n = 0;

        for (i = 0; i < len; i++)
                n += sprintf(buf + n, "%02x", (unsigned char) key[i]);
        return buf;
}

EXPORT_SYMBOL(init_capa_hash);
EXPORT_SYMBOL(cleanup_capa_hash);

EXPORT_SYMBOL(capa_add);
EXPORT_SYMBOL(capa_lookup);

EXPORT_SYMBOL(capa_hmac);
EXPORT_SYMBOL(capa_cpy);

EXPORT_SYMBOL(dump_capa_content);
