/*
 * dlr_sdb.c
 *
 * Implementation of handling delivery reports (DLRs)
 * for LibSDB.
 *
 * Andreas Fink <andreas@fink.org>, 18.08.2001
 * Stipe Tolj <tolj@wapme-systems.de>, 22.03.2002
 * Alexander Malysh <a.malysh@centrium.de> 2003
*/

#include "gwlib/gwlib.h"
#include "dlr_p.h"

#ifdef DLR_SDB
#include <sdb.h>
static char *connection;

/*
 * Database fields, which we are use.
 */
static struct dlr_db_fields *fields = NULL;

/*
 * Mutex to protec access to database.
 */
static Mutex *dlr_mutex = NULL;

enum {
    SDB_ORACLE,
    SDB_OTHER
};

static long sdb_conn_type = SDB_OTHER;


static const char* sdb_get_limit_str()
{
    switch (sdb_conn_type) {
        case SDB_ORACLE:
            return "AND ROWNUM < 2";
        case SDB_OTHER:
        default:
            return "LIMIT 1";
    }
}

static void dlr_sdb_shutdown()
{
    sdb_close(connection);
    dlr_db_fields_destroy(fields);
    mutex_destroy(dlr_mutex);
}

static void dlr_sdb_add(struct dlr_entry *dlr)
{
    Octstr *sql;
    int	state;

    sql = octstr_format("INSERT INTO %s (%s, %s, %s, %s, %s, %s, %s, %s, %s) VALUES "
                        "('%s', '%s', '%s', '%s', '%s', '%s', '%d', '%s', '%d')",
                        octstr_get_cstr(fields->table), octstr_get_cstr(fields->field_smsc),
                        octstr_get_cstr(fields->field_ts),
                        octstr_get_cstr(fields->field_src), octstr_get_cstr(fields->field_dst),
                        octstr_get_cstr(fields->field_serv), octstr_get_cstr(fields->field_url),
                        octstr_get_cstr(fields->field_mask), octstr_get_cstr(fields->field_boxc),
                        octstr_get_cstr(fields->field_status),
                        octstr_get_cstr(dlr->smsc), octstr_get_cstr(dlr->timestamp),
                        octstr_get_cstr(dlr->source), octstr_get_cstr(dlr->destination),
                        octstr_get_cstr(dlr->service), octstr_get_cstr(dlr->url), dlr->mask,
                        octstr_get_cstr(dlr->boxc_id), 0);

#if defined(DLR_TRACE)
     debug("dlr.sdb", 0, "SDB: sql: %s", octstr_get_cstr(sql));
#endif

    mutex_lock(dlr_mutex);
    state = sdb_query(connection, octstr_get_cstr(sql), NULL, NULL);
    mutex_unlock(dlr_mutex);
    if (state == -1)
        error(0, "SDB: error in inserting DLR for DST <%s>", octstr_get_cstr(dlr->destination));

    octstr_destroy(sql);
    dlr_entry_destroy(dlr);
}

static int sdb_callback_add(int n, char **p, void *data)
{
    struct dlr_entry *res = (struct dlr_entry *) data;

    if (n != 6) {
        debug("dlr.sdb", 0, "SDB: Result has incorrect number of columns: %d", n);
        return 0;
    }

#if defined(DLR_TRACE)
    debug("dlr.sdb", 0, "row=%s,%s,%s,%s,%s,%s",p[0],p[1],p[2],p[3],p[4],p[5]);
#endif

    if (res->destination != NULL) {
        debug("dlr.sdb", 0, "SDB: Row already stored.");
        return 0;
    }

    res->mask = atoi(p[0]);
    res->service = octstr_create(p[1]);
    res->url = octstr_create(p[2]);
    res->source = octstr_create(p[3]);
    res->destination = octstr_create(p[4]);
    res->boxc_id = octstr_create(p[5]);

    return 0;
}

static int sdb_callback_msgs(int n, char **p, void *data)
{
    long *count = (long *) data;

    if (n != 1) {
        debug("dlr.sdb", 0, "SDB: Result has incorrect number of columns: %d", n);
        return 0;
    }

#if defined(DLR_TRACE)
    debug("dlr.sdb", 0, "SDB: messages=%s",p[0]);
#endif

    *count = atol(p[0]);

    return 0;
}

static struct dlr_entry*  dlr_sdb_get(const Octstr *smsc, const Octstr *ts, const Octstr *dst)
{
    Octstr *sql;
    int	state;
    struct dlr_entry *res = dlr_entry_create();

    gw_assert(res != NULL);

    sql = octstr_format("SELECT %s, %s, %s, %s, %s, %s FROM %s WHERE %s='%s' AND %s='%s' %s",
                        octstr_get_cstr(fields->field_mask), octstr_get_cstr(fields->field_serv),
                        octstr_get_cstr(fields->field_url), octstr_get_cstr(fields->field_src),
                        octstr_get_cstr(fields->field_dst), octstr_get_cstr(fields->field_boxc),
                        octstr_get_cstr(fields->table),
                        octstr_get_cstr(fields->field_smsc), octstr_get_cstr(smsc),
                        octstr_get_cstr(fields->field_ts), octstr_get_cstr(ts), sdb_get_limit_str());

#if defined(DLR_TRACE)
     debug("dlr.sdb", 0, "SDB: sql: %s", octstr_get_cstr(sql));
#endif

    mutex_lock(dlr_mutex);
    state = sdb_query(connection, octstr_get_cstr(sql), sdb_callback_add, res);
    mutex_unlock(dlr_mutex);
    octstr_destroy(sql);
    if (state == -1) {
        error(0, "SDB: error in finding DLR");
        goto notfound;
    }
    else if (state == 0) {
        debug("dlr.sdb", 0, "SDB: no entry found for DST <%s>.", octstr_get_cstr(dst));
        goto notfound;
    }

    res->smsc = octstr_duplicate(smsc);

    return res;

notfound:
    dlr_entry_destroy(res);
    return NULL;
}

static void  dlr_sdb_update(const Octstr *smsc, const Octstr *ts, const Octstr *dst, int status)
{
    Octstr *sql;
    int	state;

    debug("dlr.sdb", 0, "SDB: updating DLR status in database");
    sql = octstr_format("UPDATE %s SET %s=%d WHERE %s='%s' AND %s='%s' %s",
                        octstr_get_cstr(fields->table),
                        octstr_get_cstr(fields->field_status), status,
                        octstr_get_cstr(fields->field_smsc), octstr_get_cstr(smsc),
                        octstr_get_cstr(fields->field_ts), octstr_get_cstr(ts), sdb_get_limit_str());

#if defined(DLR_TRACE)
     debug("dlr.sdb", 0, "SDB: sql: %s", octstr_get_cstr(sql));
#endif

    mutex_lock(dlr_mutex);
    state = sdb_query(connection, octstr_get_cstr(sql), NULL, NULL);
    mutex_unlock(dlr_mutex);
    octstr_destroy(sql);
    if (state == -1) {
        error(0, "SDB: error in updating DLR");
    }
}

static void  dlr_sdb_remove(const Octstr *smsc, const Octstr *ts, const Octstr *dst)
{
    Octstr *sql;
    int	state;

    debug("dlr.sdb", 0, "removing DLR from database");
    sql = octstr_format("DELETE FROM %s WHERE %s='%s' AND %s='%s' %s",
                        octstr_get_cstr(fields->table),
                        octstr_get_cstr(fields->field_smsc), octstr_get_cstr(smsc),
                        octstr_get_cstr(fields->field_ts), octstr_get_cstr(ts), sdb_get_limit_str());

#if defined(DLR_TRACE)
     debug("dlr.sdb", 0, "SDB: sql: %s", octstr_get_cstr(sql));
#endif

    mutex_lock(dlr_mutex);
    state = sdb_query(connection, octstr_get_cstr(sql), NULL, NULL);
    mutex_unlock(dlr_mutex);
    octstr_destroy(sql);
    if (state == -1)
        error(0, "SDB: error in deleting DLR");
}

static long dlr_sdb_messages(void)
{
    Octstr *sql;
    int	state;
    long res = 0;

    sql = octstr_format("SELECT count(*) FROM %s", octstr_get_cstr(fields->table));

#if defined(DLR_TRACE)
    debug("dlr.sdb", 0, "sql: %s", octstr_get_cstr(sql));
#endif

    mutex_lock(dlr_mutex);
    state = sdb_query(connection, octstr_get_cstr(sql), sdb_callback_msgs, &res);
    mutex_unlock(dlr_mutex);
    octstr_destroy(sql);
    if (state == -1) {
        error(0, "SDB: error in selecting ammount of waiting DLRs");
        mutex_unlock(dlr_mutex);
        return -1;
    }

    return res;
}

static void dlr_sdb_flush(void)
{
    Octstr *sql;
    int	state;

    sql = octstr_format("DELETE FROM %s", octstr_get_cstr(fields->table));

#if defined(DLR_TRACE)
     debug("dlr.sdb", 0, "sql: %s", octstr_get_cstr(sql));
#endif

    mutex_lock(dlr_mutex);
    state = sdb_query(connection, octstr_get_cstr(sql), NULL, NULL);
    mutex_unlock(dlr_mutex);
    octstr_destroy(sql);
    if (state == -1) {
        error(0, "SDB: error in flusing DLR table");
    }
}


static struct dlr_storage  handles = {
    .type = "sdb",
    .dlr_add = dlr_sdb_add,
    .dlr_get = dlr_sdb_get,
    .dlr_update = dlr_sdb_update,
    .dlr_remove = dlr_sdb_remove,
    .dlr_shutdown = dlr_sdb_shutdown,
    .dlr_messages = dlr_sdb_messages,
    .dlr_flush = dlr_sdb_flush
};

struct dlr_storage *dlr_init_sdb(Cfg* cfg)
{
    CfgGroup *grp;
    List *grplist;
    Octstr *sdb_url, *sdb_id;
    Octstr *p = NULL;

    /*
     * check for all mandatory directives that specify the field names
     * of the used table
     */
    if (!(grp = cfg_get_single_group(cfg, octstr_imm("dlr-db"))))
        panic(0, "DLR: SDB: group 'dlr-db' is not specified!");

    if (!(sdb_id = cfg_get(grp, octstr_imm("id"))))
   	    panic(0, "DLR: SDB: directive 'id' is not specified!");

    fields = dlr_db_fields_create(grp);
    gw_assert(fields != NULL);
    dlr_mutex = mutex_create();

    /*
     * now grap the required information from the 'mysql-connection' group
     * with the sdb-id we just obtained
     *
     * we have to loop through all available SDB connection definitions
     * and search for the one we are looking for
     */

     grplist = cfg_get_multi_group(cfg, octstr_imm("sdb-connection"));
     while (grplist && (grp = list_extract_first(grplist)) != NULL) {
        p = cfg_get(grp, octstr_imm("id"));
        if (p != NULL && octstr_compare(p, sdb_id) == 0) {
            goto found;
        }
        if (p != NULL) octstr_destroy(p);
     }
     panic(0, "DLR: SDB: connection settings for id '%s' are not specified!",
           octstr_get_cstr(sdb_id));

found:
    octstr_destroy(p);
    list_destroy(grplist, NULL);

    if (!(sdb_url = cfg_get(grp, octstr_imm("url"))))
   	    panic(0, "DLR: SDB: directive 'url' is not specified!");

    if (octstr_search(sdb_url, octstr_imm("oracle:"), 0) == 0)
        sdb_conn_type = SDB_ORACLE;
    else
        sdb_conn_type = SDB_OTHER;

    /*
     * ok, ready to connect
     */
    info(0,"Connecting to sdb resource <%s>.", octstr_get_cstr(sdb_url));
    connection = sdb_open(octstr_get_cstr(sdb_url));
    if (connection == NULL)
        panic(0, "Could not connect to database");

    octstr_destroy(sdb_url);

    return &handles;
}
#else
/*
 * Return NULL , so we point dlr-core that we were
 * not compiled in.
 */
struct dlr_storage *dlr_init_sdb(Cfg* cfg)
{
    return NULL;
}
#endif /* DLR_SDB */
