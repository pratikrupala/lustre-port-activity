/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
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
 *
 * These are the only exported functions, they provide some generic
 * infrastructure for managing object devices
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifdef __KERNEL__
#include <linux/kmod.h>   /* for request_module() */
#include <linux/module.h>
#include <linux/obd_class.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#else
#include <liblustre.h>
#include <linux/obd_class.h>
#include <linux/obd.h>
#endif
#include <linux/lprocfs_status.h>

extern struct list_head obd_types;
static spinlock_t obd_types_lock = SPIN_LOCK_UNLOCKED;
kmem_cache_t *obdo_cachep = NULL;
kmem_cache_t *import_cachep = NULL;

int (*ptlrpc_put_connection_superhack)(struct ptlrpc_connection *c);
void (*ptlrpc_abort_inflight_superhack)(struct obd_import *imp);

/*
 * support functions: we could use inter-module communication, but this
 * is more portable to other OS's
 */
static struct obd_type *class_search_type(char *name)
{
        struct list_head *tmp;
        struct obd_type *type;

        spin_lock(&obd_types_lock);
        list_for_each(tmp, &obd_types) {
                type = list_entry(tmp, struct obd_type, typ_chain);
                if (strlen(type->typ_name) == strlen(name) &&
                    strcmp(type->typ_name, name) == 0) {
                        spin_unlock(&obd_types_lock);
                        return type;
                }
        }
        spin_unlock(&obd_types_lock);
        return NULL;
}

struct obd_type *class_get_type(char *name)
{
        struct obd_type *type = class_search_type(name);

#ifdef CONFIG_KMOD
        if (!type) {
                if (!request_module(name)) {
                        CDEBUG(D_INFO, "Loaded module '%s'\n", name);
                        type = class_search_type(name);
                } else
                        CDEBUG(D_INFO, "Can't load module '%s'\n", name);
        }
#endif
        if (type)
                try_module_get(type->typ_ops->o_owner);
        return type;
}

void class_put_type(struct obd_type *type)
{
        LASSERT(type);
        module_put(type->typ_ops->o_owner);
}

int class_register_type(struct obd_ops *ops, struct md_ops *md_ops,
                        struct lprocfs_vars *vars, char *name)
{
        struct obd_type *type;
        int rc = 0;
        ENTRY;

        LASSERT(strnlen(name, 1024) < 1024);    /* sanity check */

        if (class_search_type(name)) {
                CDEBUG(D_IOCTL, "Type %s already registered\n", name);
                RETURN(-EEXIST);
        }

        rc = -ENOMEM;
        OBD_ALLOC(type, sizeof(*type));
        if (type == NULL)
                RETURN(rc);

        OBD_ALLOC(type->typ_ops, sizeof(*type->typ_ops));
        OBD_ALLOC(type->typ_name, strlen(name) + 1);
        if (md_ops)
                OBD_ALLOC(type->typ_md_ops, sizeof(*type->typ_md_ops));
        if (type->typ_ops == NULL || type->typ_name == NULL ||
                        (md_ops && type->typ_md_ops == NULL))
                GOTO (failed, rc);

        *(type->typ_ops) = *ops;
        if (md_ops)
                *(type->typ_md_ops) = *md_ops;
        else
                type->typ_md_ops = NULL;
        strcpy(type->typ_name, name);

#ifdef LPROCFS
        type->typ_procroot = lprocfs_register(type->typ_name, proc_lustre_root,
                                              vars, type);
#endif
        if (IS_ERR(type->typ_procroot)) {
                rc = PTR_ERR(type->typ_procroot);
                type->typ_procroot = NULL;
                GOTO (failed, rc);
        }

        spin_lock(&obd_types_lock);
        list_add(&type->typ_chain, &obd_types);
        spin_unlock(&obd_types_lock);

        RETURN (0);

 failed:
        if (type->typ_name != NULL)
                OBD_FREE(type->typ_name, strlen(name) + 1);
        if (type->typ_ops != NULL)
                OBD_FREE (type->typ_ops, sizeof (*type->typ_ops));
        if (type->typ_md_ops != NULL)
                OBD_FREE (type->typ_md_ops, sizeof (*type->typ_md_ops));
        OBD_FREE(type, sizeof(*type));
        RETURN(rc);
}

int class_unregister_type(char *name)
{
        struct obd_type *type = class_search_type(name);
        ENTRY;

        if (!type) {
                CERROR("unknown obd type\n");
                RETURN(-EINVAL);
        }

        if (type->typ_refcnt) {
                CERROR("type %s has refcount (%d)\n", name, type->typ_refcnt);
                /* This is a bad situation, let's make the best of it */
                /* Remove ops, but leave the name for debugging */
                OBD_FREE(type->typ_ops, sizeof(*type->typ_ops));
                RETURN(-EBUSY);
        }

        if (type->typ_procroot) {
                lprocfs_remove(type->typ_procroot);
                type->typ_procroot = NULL;
        }

        spin_lock(&obd_types_lock);
        list_del(&type->typ_chain);
        spin_unlock(&obd_types_lock);
        OBD_FREE(type->typ_name, strlen(name) + 1);
        if (type->typ_ops != NULL)
                OBD_FREE(type->typ_ops, sizeof(*type->typ_ops));
        if (type->typ_md_ops != NULL)
                OBD_FREE (type->typ_md_ops, sizeof(*type->typ_md_ops));
        OBD_FREE(type, sizeof(*type));
        RETURN(0);
} /* class_unregister_type */

struct obd_device *class_newdev(struct obd_type *type)
{
        struct obd_device *result = NULL;
        int i;

        spin_lock(&obd_dev_lock);
        for (i = 0 ; i < MAX_OBD_DEVICES && result == NULL; i++) {
                struct obd_device *obd = &obd_dev[i];
                if (!obd->obd_type) {
                        LASSERT(obd->obd_minor == i);
                        memset(obd, 0, sizeof(*obd));
                        obd->obd_minor = i;
                        obd->obd_type = type;
                        result = obd;
                }
        }
        spin_unlock(&obd_dev_lock);
        return result;
}

void class_release_dev(struct obd_device *obd)
{
        int minor = obd->obd_minor;

        spin_lock(&obd_dev_lock);
        obd->obd_type = NULL;
        //memset(obd, 0, sizeof(*obd));
        obd->obd_minor = minor;
        spin_unlock(&obd_dev_lock);
}

int class_name2dev(char *name)
{
        int i;

        if (!name)
                return -1;

        spin_lock(&obd_dev_lock);
        for (i = 0; i < MAX_OBD_DEVICES; i++) {
                struct obd_device *obd = &obd_dev[i];
                if (obd->obd_name && strcmp(name, obd->obd_name) == 0) {
                        spin_unlock(&obd_dev_lock);
                        return i;
                }
        }
        spin_unlock(&obd_dev_lock);
        return -1;
}

struct obd_device *class_name2obd(char *name)
{
        int dev = class_name2dev(name);
        if (dev < 0)
                return NULL;
        return &obd_dev[dev];
}

int class_uuid2dev(struct obd_uuid *uuid)
{
        int i;
        spin_lock(&obd_dev_lock);
        for (i = 0; i < MAX_OBD_DEVICES; i++) {
                struct obd_device *obd = &obd_dev[i];
                if (obd_uuid_equals(uuid, &obd->obd_uuid)) {
                        spin_unlock(&obd_dev_lock);
                        return i;
                }
        }
        spin_unlock(&obd_dev_lock);
        return -1;
}

struct obd_device *class_uuid2obd(struct obd_uuid *uuid)
{
        int dev = class_uuid2dev(uuid);
        if (dev < 0)
                return NULL;
        return &obd_dev[dev];
}

/* Search for a client OBD connected to tgt_uuid.  If grp_uuid is
   specified, then only the client with that uuid is returned,
   otherwise any client connected to the tgt is returned.
   If tgt_uuid is NULL, the lov with grp_uuid is returned. */
struct obd_device *class_find_client_obd(struct obd_uuid *tgt_uuid,
                                         char *typ_name,
                                         struct obd_uuid *grp_uuid)
{
        int i;

        spin_lock(&obd_dev_lock);
        for (i = 0; i < MAX_OBD_DEVICES; i++) {
                struct obd_device *obd = &obd_dev[i];
                if (obd->obd_type == NULL)
                        continue;
                if ((strncmp(obd->obd_type->typ_name, typ_name,
                             strlen(typ_name)) == 0)) {
                        struct client_obd *cli = &obd->u.cli;
                        struct obd_import *imp = cli->cl_import;
                        if (tgt_uuid == NULL) {
                                LASSERT(grp_uuid);
                                if (obd_uuid_equals(grp_uuid, &obd->obd_uuid)) {
                                        spin_unlock(&obd_dev_lock);
                                        return obd;
                                }
                                continue;
                        }
                        if (obd_uuid_equals(tgt_uuid, &imp->imp_target_uuid) &&
                            ((grp_uuid)? obd_uuid_equals(grp_uuid,
                                                         &obd->obd_uuid) : 1)) {
                                spin_unlock(&obd_dev_lock);
                                return obd;
                        }
                }
        }
        spin_unlock(&obd_dev_lock);
        return NULL;
}

/* Iterate the obd_device list looking devices have grp_uuid. Start
   searching at *next, and if a device is found, the next index to look
   it is saved in *next. If next is NULL, then the first matching device
   will always be returned. */
struct obd_device *class_devices_in_group(struct obd_uuid *grp_uuid, int *next)
{
        int i;

        if (next == NULL) 
                i = 0;
        else if (*next >= 0 && *next < MAX_OBD_DEVICES)
                i = *next;
        else 
                return NULL;
        spin_lock(&obd_dev_lock);                
        for (; i < MAX_OBD_DEVICES; i++) {
                struct obd_device *obd = &obd_dev[i];
                if (obd->obd_type == NULL)
                        continue;
                if (obd_uuid_equals(grp_uuid, &obd->obd_uuid)) {
                        if (next != NULL)
                                *next = i+1;
                        spin_unlock(&obd_dev_lock);
                        return obd;
                }
        }

        spin_unlock(&obd_dev_lock);
        return NULL;
}

void obd_cleanup_caches(void)
{
        ENTRY;
        if (obdo_cachep) {
                LASSERTF(kmem_cache_destroy(obdo_cachep) == 0,
                         "Cannot destory ll_obdo_cache\n");
                obdo_cachep = NULL;
        }
        if (import_cachep) {
                LASSERTF(kmem_cache_destroy(import_cachep) == 0,
                         "Cannot destory ll_import_cache\n");
                import_cachep = NULL;
        }
        EXIT;
}

int obd_init_caches(void)
{
        ENTRY;
        LASSERT(obdo_cachep == NULL);
        obdo_cachep = kmem_cache_create("ll_obdo_cache", sizeof(struct obdo),
                                        0, 0, NULL, NULL);
        if (!obdo_cachep)
                GOTO(out, -ENOMEM);

        LASSERT(import_cachep == NULL);
        import_cachep = kmem_cache_create("ll_import_cache",
                                          sizeof(struct obd_import),
                                          0, 0, NULL, NULL);
        if (!import_cachep)
                GOTO(out, -ENOMEM);

        RETURN(0);
 out:
        obd_cleanup_caches();
        RETURN(-ENOMEM);

}

/* map connection to client */
struct obd_export *class_conn2export(struct lustre_handle *conn)
{
        struct obd_export *export;
        ENTRY;

        if (!conn) {
                CDEBUG(D_CACHE, "looking for null handle\n");
                RETURN(NULL);
        }

        if (conn->cookie == -1) {  /* this means assign a new connection */
                CDEBUG(D_CACHE, "want a new connection\n");
                RETURN(NULL);
        }

        CDEBUG(D_IOCTL, "looking for export cookie "LPX64"\n", conn->cookie);
        export = class_handle2object(conn->cookie);
        RETURN(export);
}

struct obd_device *class_exp2obd(struct obd_export *exp)
{
        if (exp)
                return exp->exp_obd;
        return NULL;
}

struct obd_device *class_conn2obd(struct lustre_handle *conn)
{
        struct obd_export *export;
        export = class_conn2export(conn);
        if (export) {
                struct obd_device *obd = export->exp_obd;
                class_export_put(export);
                return obd;
        }
        return NULL;
}

struct obd_import *class_exp2cliimp(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        if (obd == NULL)
                return NULL;
        return obd->u.cli.cl_import;
}

struct obd_import *class_conn2cliimp(struct lustre_handle *conn)
{
        struct obd_device *obd = class_conn2obd(conn);
        if (obd == NULL)
                return NULL;
        return obd->u.cli.cl_import;
}

/* Export management functions */
static void export_handle_addref(void *export)
{
        class_export_get(export);
}

void __class_export_put(struct obd_export *exp)
{
        if (atomic_dec_and_test(&exp->exp_refcount)) {
                struct obd_device *obd = exp->exp_obd;
                CDEBUG(D_IOCTL, "destroying export %p/%s\n", exp,
                       exp->exp_client_uuid.uuid);

                LASSERT(obd != NULL);

                /* "Local" exports (lctl, LOV->{mdc,osc}) have no connection. */
                if (exp->exp_connection)
                        ptlrpc_put_connection_superhack(exp->exp_connection);

                LASSERT(list_empty(&exp->exp_outstanding_replies));
                LASSERT(list_empty(&exp->exp_handle.h_link));
                obd_destroy_export(exp);

                OBD_FREE(exp, sizeof(*exp));
                if (obd->obd_set_up) {
                        atomic_dec(&obd->obd_refcount);
                        wake_up(&obd->obd_refcount_waitq);
                } else {
                        CERROR("removing export %p from obd %s (%p) -- OBD "
                               "not set up (refcount = %d)\n", exp,
                               obd->obd_name, obd,
                               atomic_read(&obd->obd_refcount));
                }
        }
}

/* Creates a new export, adds it to the hash table, and returns a
 * pointer to it. The refcount is 2: one for the hash reference, and
 * one for the pointer returned by this function. */
struct obd_export *class_new_export(struct obd_device *obd)
{
        struct obd_export *export;

        OBD_ALLOC(export, sizeof(*export));
        if (!export) {
                CERROR("no memory! (minor %d)\n", obd->obd_minor);
                return NULL;
        }

        export->exp_conn_cnt = 0;
        atomic_set(&export->exp_refcount, 2);
        atomic_set(&export->exp_rpc_count, 0);
        export->exp_obd = obd;
        export->exp_flags = 0;
        INIT_LIST_HEAD(&export->exp_outstanding_replies);
        /* XXX this should be in LDLM init */
        INIT_LIST_HEAD(&export->exp_ldlm_data.led_held_locks);

        INIT_LIST_HEAD(&export->exp_handle.h_link);
        class_handle_hash(&export->exp_handle, export_handle_addref);
        spin_lock_init(&export->exp_lock);

        spin_lock(&obd->obd_dev_lock);
        LASSERT(!obd->obd_stopping); /* shouldn't happen, but might race */
        atomic_inc(&obd->obd_refcount);
        list_add(&export->exp_obd_chain, &export->exp_obd->obd_exports);
        export->exp_obd->obd_num_exports++;
        spin_unlock(&obd->obd_dev_lock);
        export->exp_connected = 1;
        obd_init_export(export);
        return export;
}

void class_unlink_export(struct obd_export *exp)
{
        class_handle_unhash(&exp->exp_handle);

        spin_lock(&exp->exp_obd->obd_dev_lock);
        list_del_init(&exp->exp_obd_chain);
        exp->exp_obd->obd_num_exports--;
        spin_unlock(&exp->exp_obd->obd_dev_lock);

        class_export_put(exp);
}

/* Import management functions */
static void import_handle_addref(void *import)
{
        class_import_get(import);
}

struct obd_import *class_import_get(struct obd_import *import)
{
        atomic_inc(&import->imp_refcount);
        CDEBUG(D_IOCTL, "import %p refcount=%d\n", import,
               atomic_read(&import->imp_refcount));
        return import;
}

void class_import_put(struct obd_import *import)
{
        ENTRY;

        CDEBUG(D_IOCTL, "import %p refcount=%d\n", import,
               atomic_read(&import->imp_refcount) - 1);

        LASSERT(atomic_read(&import->imp_refcount) > 0);
        LASSERT(atomic_read(&import->imp_refcount) < 0x5a5a5a);
        
        if (!atomic_dec_and_test(&import->imp_refcount)) {
                EXIT;
                return;
        }

        CDEBUG(D_IOCTL, "destroying import %p\n", import);
        
        if (import->imp_connection)
                ptlrpc_put_connection_superhack(import->imp_connection);

        LASSERT(!import->imp_sec);
        while (!list_empty(&import->imp_conn_list)) {
                struct obd_import_conn *imp_conn;

                imp_conn = list_entry(import->imp_conn_list.next,
                                      struct obd_import_conn, oic_item);
                list_del(&imp_conn->oic_item);
                if (imp_conn->oic_conn)
                        ptlrpc_put_connection_superhack(imp_conn->oic_conn);
                OBD_FREE(imp_conn, sizeof(*imp_conn));
        }

        LASSERT(list_empty(&import->imp_handle.h_link));
        OBD_FREE(import, sizeof(*import));
        EXIT;
}

struct obd_import *class_new_import(void)
{
        struct obd_import *imp;

        OBD_ALLOC(imp, sizeof(*imp));
        if (imp == NULL)
                return NULL;

        INIT_LIST_HEAD(&imp->imp_replay_list);
        INIT_LIST_HEAD(&imp->imp_sending_list);
        INIT_LIST_HEAD(&imp->imp_delayed_list);
        INIT_LIST_HEAD(&imp->imp_rawrpc_list);
        spin_lock_init(&imp->imp_lock);
        imp->imp_conn_cnt = 0;
        imp->imp_max_transno = 0;
        imp->imp_peer_committed_transno = 0;
        imp->imp_state = LUSTRE_IMP_NEW;
        init_waitqueue_head(&imp->imp_recovery_waitq);

        atomic_set(&imp->imp_refcount, 2);
        atomic_set(&imp->imp_inflight, 0);
        atomic_set(&imp->imp_replay_inflight, 0);
        INIT_LIST_HEAD(&imp->imp_conn_list);
        INIT_LIST_HEAD(&imp->imp_handle.h_link);
        class_handle_hash(&imp->imp_handle, import_handle_addref);
        imp->imp_waiting_ping_reply = 0;

        return imp;
}

void class_destroy_import(struct obd_import *import)
{
        LASSERT(import != NULL);
        LASSERT(import != LP_POISON);

        class_handle_unhash(&import->imp_handle);

        /* Abort any inflight DLM requests and NULL out their (about to be
         * freed) import. */
        /* Invalidate all requests on import, would be better to call
           ptlrpc_set_import_active(imp, 0); */
        import->imp_generation++;
        ptlrpc_abort_inflight_superhack(import);

        class_import_put(import);
}

/* A connection defines an export context in which preallocation can
   be managed. This releases the export pointer reference, and returns
   the export handle, so the export refcount is 1 when this function
   returns. */
int class_connect(struct lustre_handle *conn, struct obd_device *obd,
                  struct obd_uuid *cluuid)
{
        struct obd_export *export;
        LASSERT(conn != NULL);
        LASSERT(obd != NULL);
        LASSERT(cluuid != NULL);
        ENTRY;

        export = class_new_export(obd);
        if (export == NULL)
                RETURN(-ENOMEM);

        conn->cookie = export->exp_handle.h_cookie;
        memcpy(&export->exp_client_uuid, cluuid,
               sizeof(export->exp_client_uuid));
        class_export_put(export);

        CDEBUG(D_IOCTL, "connect: client %s, cookie "LPX64"\n",
               cluuid->uuid, conn->cookie);
        RETURN(0);
}

/* This function removes two references from the export: one for the
 * hash entry and one for the export pointer passed in.  The export
 * pointer passed to this function is destroyed should not be used
 * again. */
int class_disconnect(struct obd_export *export, unsigned long flags)
{
        ENTRY;

        if (export == NULL) {
                fixme();
                CDEBUG(D_IOCTL, "attempting to free NULL export %p\n", export);
                RETURN(-EINVAL);
        }

        /* XXX this shouldn't have to be here, but double-disconnect will crash
         * otherwise, and sometimes double-disconnect happens.  abort_recovery,
         * for example. */
        if (list_empty(&export->exp_handle.h_link))
                RETURN(0);

        CDEBUG(D_IOCTL, "disconnect: cookie "LPX64"\n",
               export->exp_handle.h_cookie);

        if (export->exp_handle.h_cookie == LL_POISON) {
                CERROR("disconnecting freed export %p, ignoring\n", export);
        } else {
                class_unlink_export(export);
                class_export_put(export);
        }
        RETURN(0);
}

static void class_disconnect_export_list(struct list_head *list, unsigned long flags)
{
        struct obd_export *fake_exp, *exp;
        struct lustre_handle fake_conn;
        int rc;
        ENTRY;

        /* Move all of the exports from obd_exports to a work list, en masse. */
        /* It's possible that an export may disconnect itself, but
         * nothing else will be added to this list. */
        while(!list_empty(list)) {
                exp = list_entry(list->next, struct obd_export, exp_obd_chain);
                class_export_get(exp);

                if (obd_uuid_equals(&exp->exp_client_uuid,
                                    &exp->exp_obd->obd_uuid)) {
                        CDEBUG(D_HA,
                               "exp %p export uuid == obd uuid, don't discon\n",
                               exp);
                        /*
                         * need to delete this now so we don't end up pointing
                         * to work_list later when this export is cleaned up.
                         */
                        list_del_init(&exp->exp_obd_chain);
                        class_export_put(exp);
                        continue;
                }

                fake_conn.cookie = exp->exp_handle.h_cookie;
                fake_exp = class_conn2export(&fake_conn);
                if (!fake_exp) {
                        class_export_put(exp);
                        continue;
                }
                
                rc = obd_disconnect(fake_exp, flags);
                class_export_put(exp);
                if (rc) {
                        CDEBUG(D_HA, "disconnecting export %p failed: %d\n",
                               exp, rc);
                } else {
                        CDEBUG(D_HA, "export %p disconnected\n", exp);
                }
        }
        EXIT;
}

void class_disconnect_exports(struct obd_device *obd, unsigned long flags)
{
        struct list_head work_list;
        ENTRY;

        /* Move all of the exports from obd_exports to a work list, en masse. */
        spin_lock(&obd->obd_dev_lock);
        list_add(&work_list, &obd->obd_exports);
        list_del_init(&obd->obd_exports);
        spin_unlock(&obd->obd_dev_lock);

        CDEBUG(D_HA, "OBD device %d (%p) has exports, "
               "disconnecting them\n", obd->obd_minor, obd);
        class_disconnect_export_list(&work_list, flags);
        EXIT;
}

/* Remove exports that have not completed recovery.
 */
int class_disconnect_stale_exports(struct obd_device *obd,
                                   int (*test_export)(struct obd_export *),
                                   unsigned long flags)
{
        struct list_head work_list;
        struct list_head *pos, *n;
        struct obd_export *exp;
        int cnt = 0;
        ENTRY;

        INIT_LIST_HEAD(&work_list);
        spin_lock(&obd->obd_dev_lock);
        list_for_each_safe(pos, n, &obd->obd_exports) {
                exp = list_entry(pos, struct obd_export, exp_obd_chain);
                if (!test_export(exp)) {
                        list_del(&exp->exp_obd_chain);
                        list_add(&exp->exp_obd_chain, &work_list);
                        cnt++;
                        CDEBUG(D_ERROR, "%s: disconnect stale client %s\n",
                               obd->obd_name, exp->exp_client_uuid.uuid);
                }
        }
        spin_unlock(&obd->obd_dev_lock);

        CDEBUG(D_ERROR, "%s: disconnecting %d stale clients\n",
               obd->obd_name, cnt);
        class_disconnect_export_list(&work_list, flags);
        RETURN(cnt);
}



int oig_init(struct obd_io_group **oig_out)
{
        struct obd_io_group *oig;
        ENTRY;

        OBD_ALLOC(oig, sizeof(*oig));
        if (oig == NULL)
                RETURN(-ENOMEM);

        spin_lock_init(&oig->oig_lock);
        oig->oig_rc = 0;
        oig->oig_pending = 0;
        atomic_set(&oig->oig_refcount, 1);
        init_waitqueue_head(&oig->oig_waitq);
        INIT_LIST_HEAD(&oig->oig_occ_list);

        *oig_out = oig;
        RETURN(0);
};

static inline void oig_grab(struct obd_io_group *oig)
{
        atomic_inc(&oig->oig_refcount);
}

void oig_release(struct obd_io_group *oig)
{
        if (atomic_dec_and_test(&oig->oig_refcount))
                OBD_FREE(oig, sizeof(*oig));
}

void oig_add_one(struct obd_io_group *oig, struct oig_callback_context *occ)
{
        unsigned long flags;
        CDEBUG(D_CACHE, "oig %p ready to roll\n", oig);
        spin_lock_irqsave(&oig->oig_lock, flags);
        oig->oig_pending++;
        if (occ != NULL)
                list_add_tail(&occ->occ_oig_item, &oig->oig_occ_list);
        spin_unlock_irqrestore(&oig->oig_lock, flags);
        oig_grab(oig);
}

void oig_complete_one(struct obd_io_group *oig,
                      struct oig_callback_context *occ, int rc)
{
        unsigned long flags;
        wait_queue_head_t *wake = NULL;
        int old_rc;

        spin_lock_irqsave(&oig->oig_lock, flags);

        if (occ != NULL)
                list_del_init(&occ->occ_oig_item);

        old_rc = oig->oig_rc;
        if (oig->oig_rc == 0 && rc != 0)
                oig->oig_rc = rc;

        if (--oig->oig_pending <= 0)
                wake = &oig->oig_waitq;

        spin_unlock_irqrestore(&oig->oig_lock, flags);

        CDEBUG(D_CACHE, "oig %p completed, rc %d -> %d via %d, %d now "
                        "pending (racey)\n", oig, old_rc, oig->oig_rc, rc,
                        oig->oig_pending);
        if (wake)
                wake_up(wake);
        oig_release(oig);
}

static int oig_done(struct obd_io_group *oig)
{
        unsigned long flags;
        int rc = 0;
        spin_lock_irqsave(&oig->oig_lock, flags);
        if (oig->oig_pending <= 0)
                rc = 1;
        spin_unlock_irqrestore(&oig->oig_lock, flags);
        return rc;
}

static void interrupted_oig(void *data)
{
        struct obd_io_group *oig = data;
        struct oig_callback_context *occ;
        unsigned long flags;

        spin_lock_irqsave(&oig->oig_lock, flags);
        /* We need to restart the processing each time we drop the lock, as
         * it is possible other threads called oig_complete_one() to remove
         * an entry elsewhere in the list while we dropped lock.  We need to
         * drop the lock because osc_ap_completion() calls oig_complete_one()
         * which re-gets this lock ;-) as well as a lock ordering issue. */
restart:
        list_for_each_entry(occ, &oig->oig_occ_list, occ_oig_item) {
                if (occ->interrupted)
                        continue;
                occ->interrupted = 1;
                spin_unlock_irqrestore(&oig->oig_lock, flags);
                occ->occ_interrupted(occ);
                spin_lock_irqsave(&oig->oig_lock, flags);
                goto restart;
        }
        spin_unlock_irqrestore(&oig->oig_lock, flags);
}

int oig_wait(struct obd_io_group *oig)
{
        struct l_wait_info lwi = LWI_INTR(interrupted_oig, oig);
        int rc;

        CDEBUG(D_CACHE, "waiting for oig %p\n", oig);

        do {
                rc = l_wait_event(oig->oig_waitq, oig_done(oig), &lwi);
                LASSERTF(rc == 0 || rc == -EINTR, "rc: %d\n", rc);
                /* we can't continue until the oig has emptied and stopped
                 * referencing state that the caller will free upon return */
                if (rc == -EINTR)
                        lwi = (struct l_wait_info){ 0, };
        } while (rc == -EINTR);

        LASSERTF(oig->oig_pending == 0,
                 "exiting oig_wait(oig = %p) with %d pending\n", oig,
                 oig->oig_pending);

        CDEBUG(D_CACHE, "done waiting on oig %p rc %d\n", oig, oig->oig_rc);
        return oig->oig_rc;
}
