/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <stddef.h>
#include <inttypes.h>

#include "default_engine_internal.h"
#include "memcached/util.h"
#include "memcached/config_parser.h"
#include <platform/cb_malloc.h>
#include "engines/default_engine.h"
#include "engine_manager.h"

// The default engine don't really use vbucket uuids, but in order
// to run the unit tests and verify that we correctly convert the
// vbucket uuid to network byte order it is nice to have a value
// we may use for testing ;)
#define DEFAULT_ENGINE_VBUCKET_UUID 0xdeadbeef

static const engine_info* default_get_info(
        gsl::not_null<ENGINE_HANDLE*> handle);
static ENGINE_ERROR_CODE default_initialize(
        gsl::not_null<ENGINE_HANDLE*> handle, const char* config_str);
static void default_destroy(gsl::not_null<ENGINE_HANDLE*> handle,
                            const bool force);
static cb::EngineErrorItemPair default_item_allocate(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        const size_t nbytes,
        const int flags,
        const rel_time_t exptime,
        uint8_t datatype,
        uint16_t vbucket);
static std::pair<cb::unique_item_ptr, item_info> default_item_allocate_ex(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        size_t nbytes,
        size_t priv_nbytes,
        int flags,
        rel_time_t exptime,
        uint8_t datatype,
        uint16_t vbucket);

static ENGINE_ERROR_CODE default_item_delete(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint64_t& cas,
        uint16_t vbucket,
        mutation_descr_t& mut_info);

static void default_item_release(gsl::not_null<ENGINE_HANDLE*> handle,
                                 gsl::not_null<item*> item);
static cb::EngineErrorItemPair default_get(gsl::not_null<ENGINE_HANDLE*> handle,
                                           gsl::not_null<const void*> cookie,
                                           const DocKey& key,
                                           uint16_t vbucket,
                                           DocStateFilter);

static cb::EngineErrorItemPair default_get_if(
        gsl::not_null<ENGINE_HANDLE*>,
        gsl::not_null<const void*>,
        const DocKey&,
        uint16_t,
        std::function<bool(const item_info&)>);

static cb::EngineErrorItemPair default_get_and_touch(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint16_t vbucket,
        uint32_t expiry_time);

static cb::EngineErrorItemPair default_get_locked(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint16_t vbucket,
        uint32_t lock_timeout);

static cb::EngineErrorMetadataPair default_get_meta(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint16_t vbucket);

static ENGINE_ERROR_CODE default_unlock(gsl::not_null<ENGINE_HANDLE*> handle,
                                        gsl::not_null<const void*> cookie,
                                        const DocKey& key,
                                        uint16_t vbucket,
                                        uint64_t cas);
static ENGINE_ERROR_CODE default_get_stats(gsl::not_null<ENGINE_HANDLE*> handle,
                                           gsl::not_null<const void*> cookie,
                                           cb::const_char_buffer key,
                                           ADD_STAT add_stat);
static void default_reset_stats(gsl::not_null<ENGINE_HANDLE*> handle,
                                gsl::not_null<const void*> cookie);
static ENGINE_ERROR_CODE default_store(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie,
                                       gsl::not_null<item*> item,
                                       uint64_t& cas,
                                       ENGINE_STORE_OPERATION operation,
                                       DocumentState);

static cb::EngineErrorCasPair default_store_if(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        gsl::not_null<item*> item_,
        uint64_t cas,
        ENGINE_STORE_OPERATION operation,
        cb::StoreIfPredicate predicate,
        DocumentState document_state);

static ENGINE_ERROR_CODE default_flush(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie);
static ENGINE_ERROR_CODE initalize_configuration(struct default_engine *se,
                                                 const char *cfg_str);
static ENGINE_ERROR_CODE default_unknown_command(
        gsl::not_null<ENGINE_HANDLE*> handle,
        const void* cookie,
        gsl::not_null<protocol_binary_request_header*> request,
        ADD_RESPONSE response,
        DocNamespace doc_namespace);

union vbucket_info_adapter {
    char c;
    struct vbucket_info v;
};

static void set_vbucket_state(struct default_engine *e,
                              uint16_t vbid, vbucket_state_t to) {
    union vbucket_info_adapter vi;
    vi.c = e->vbucket_infos[vbid];
    vi.v.state = to;
    e->vbucket_infos[vbid] = vi.c;
}

static vbucket_state_t get_vbucket_state(struct default_engine *e,
                                         uint16_t vbid) {
    union vbucket_info_adapter vi;
    vi.c = e->vbucket_infos[vbid];
    return vbucket_state_t(vi.v.state);
}

static bool handled_vbucket(struct default_engine *e, uint16_t vbid) {
    return e->config.ignore_vbucket
        || (get_vbucket_state(e, vbid) == vbucket_state_active);
}

/* mechanism for handling bad vbucket requests */
#define VBUCKET_GUARD(e, v) if (!handled_vbucket(e, v)) { return ENGINE_NOT_MY_VBUCKET; }

static bool get_item_info(gsl::not_null<ENGINE_HANDLE*> handle,
                          gsl::not_null<const item*> item,
                          gsl::not_null<item_info*> item_info);

static bool set_item_info(gsl::not_null<ENGINE_HANDLE*> handle,
                          gsl::not_null<item*> item,
                          gsl::not_null<const item_info*> itm_info);

static bool is_xattr_supported(gsl::not_null<ENGINE_HANDLE*> handle);

/**
 * Given that default_engine is implemented in C and not C++ we don't have
 * a constructor for the struct to initialize the members to some sane
 * default values. Currently they're all being allocated through the
 * engine manager which keeps a local map of all engines being created.
 *
 * Once an object is in that map it may in theory be referenced, so we
 * need to ensure that the members is initialized before hitting that map.
 *
 * @todo refactor default_engine to C++ to avoid this extra hack :)
 */
void default_engine_constructor(struct default_engine* engine, bucket_id_t id)
{
    memset(engine, 0, sizeof(*engine));

    cb_mutex_initialize(&engine->slabs.lock);
    cb_mutex_initialize(&engine->items.lock);
    cb_mutex_initialize(&engine->stats.lock);
    cb_mutex_initialize(&engine->scrubber.lock);

    engine->bucket_id = id;
    engine->engine.interface.interface = 1;
    engine->engine.get_info = default_get_info;
    engine->engine.initialize = default_initialize;
    engine->engine.destroy = default_destroy;
    engine->engine.allocate = default_item_allocate;
    engine->engine.allocate_ex = default_item_allocate_ex;
    engine->engine.remove = default_item_delete;
    engine->engine.release = default_item_release;
    engine->engine.get = default_get;
    engine->engine.get_if = default_get_if;
    engine->engine.get_locked = default_get_locked;
    engine->engine.get_meta = default_get_meta;
    engine->engine.get_and_touch = default_get_and_touch;
    engine->engine.unlock = default_unlock;
    engine->engine.get_stats = default_get_stats;
    engine->engine.reset_stats = default_reset_stats;
    engine->engine.store = default_store;
    engine->engine.store_if = default_store_if;
    engine->engine.flush = default_flush;
    engine->engine.unknown_command = default_unknown_command;
    engine->engine.item_set_cas = item_set_cas;
    engine->engine.get_item_info = get_item_info;
    engine->engine.set_item_info = set_item_info;
    engine->engine.isXattrEnabled = is_xattr_supported;
    engine->config.verbose = 0;
    engine->config.oldest_live = 0;
    engine->config.evict_to_free = true;
    engine->config.maxbytes = 64 * 1024 * 1024;
    engine->config.preallocate = false;
    engine->config.factor = 1.25;
    engine->config.chunk_size = 48;
    engine->config.item_size_max= 1024 * 1024;
    engine->config.xattr_enabled = true;
    engine->info.engine.description = "Default engine v0.1";
    engine->info.engine.num_features = 1;
    engine->info.engine.features[0].feature = ENGINE_FEATURE_LRU;
    engine->info.engine.features[engine->info.engine.num_features++].feature
        = ENGINE_FEATURE_DATATYPE;
}

extern "C" ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                             GET_SERVER_API get_server_api,
                                             ENGINE_HANDLE **handle) {
   SERVER_HANDLE_V1 *api = get_server_api();
   struct default_engine *engine;

   if (interface != 1 || api == NULL) {
      return ENGINE_ENOTSUP;
   }

   if ((engine = engine_manager_create_engine()) == NULL) {
      return ENGINE_ENOMEM;
   }

   engine->server = *api;
   engine->get_server_api = get_server_api;
   engine->initialized = true;
   *handle = (ENGINE_HANDLE*)&engine->engine;
   return ENGINE_SUCCESS;
}

extern "C" void destroy_engine() {
    engine_manager_shutdown();
    assoc_destroy();
}

static struct default_engine* get_handle(ENGINE_HANDLE* handle) {
   return (struct default_engine*)handle;
}

static hash_item* get_real_item(item* item) {
    return (hash_item*)item;
}

static const engine_info* default_get_info(
        gsl::not_null<ENGINE_HANDLE*> handle) {
    return &get_handle(handle)->info.engine;
}

static ENGINE_ERROR_CODE default_initialize(
        gsl::not_null<ENGINE_HANDLE*> handle, const char* config_str) {
    struct default_engine* se = get_handle(handle);
    ENGINE_ERROR_CODE ret = initalize_configuration(se, config_str);
    if (ret != ENGINE_SUCCESS) {
        return ret;
    }
    se->info.engine.features[se->info.engine.num_features++].feature =
            ENGINE_FEATURE_CAS;

    ret = assoc_init(se);
    if (ret != ENGINE_SUCCESS) {
        return ret;
    }

    ret = slabs_init(
            se, se->config.maxbytes, se->config.factor, se->config.preallocate);
    if (ret != ENGINE_SUCCESS) {
        return ret;
    }

    return ENGINE_SUCCESS;
}

static void default_destroy(gsl::not_null<ENGINE_HANDLE*> handle,
                            const bool force) {
    (void)force;
    engine_manager_delete_engine(get_handle(handle));
}

void destroy_engine_instance(struct default_engine* engine) {
    if (engine->initialized) {
        /* Destory the slabs cache */
        slabs_destroy(engine);

        cb_free(engine->config.uuid);

        /* Clean up the mutexes */
        cb_mutex_destroy(&engine->items.lock);
        cb_mutex_destroy(&engine->stats.lock);
        cb_mutex_destroy(&engine->slabs.lock);
        cb_mutex_destroy(&engine->scrubber.lock);

        engine->initialized = false;
    }
}

static cb::EngineErrorItemPair default_item_allocate(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        const size_t nbytes,
        const int flags,
        const rel_time_t exptime,
        uint8_t datatype,
        uint16_t vbucket) {
    try {
        auto pair = default_item_allocate_ex(handle, cookie, key, nbytes,
                                             0, // No privileged bytes
                                             flags, exptime, datatype, vbucket);
        return {cb::engine_errc::success, std::move(pair.first)};
    } catch (const cb::engine_error& error) {
        return cb::makeEngineErrorItemPair(
                cb::engine_errc(error.code().value()));
    }
}

static std::pair<cb::unique_item_ptr, item_info> default_item_allocate_ex(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        size_t nbytes,
        size_t priv_nbytes,
        int flags,
        rel_time_t exptime,
        uint8_t datatype,
        uint16_t vbucket) {
    hash_item *it;

    unsigned int id;
    struct default_engine* engine = get_handle(handle);

    if (!handled_vbucket(engine, vbucket)) {
        throw cb::engine_error(cb::engine_errc::not_my_vbucket,
                               "default_item_allocate_ex");
    }

    size_t ntotal = sizeof(hash_item) + key.size() + nbytes;
    id = slabs_clsid(engine, ntotal);
    if (id == 0) {
        throw cb::engine_error(cb::engine_errc::too_big,
                               "default_item_allocate_ex: no slab class");
    }

    if ((nbytes - priv_nbytes) > engine->config.item_size_max) {
        throw cb::engine_error(cb::engine_errc::too_big,
                               "default_item_allocate_ex");
    }

    it = item_alloc(engine,
                    key.data(),
                    key.size(),
                    flags,
                    engine->server.core->realtime(exptime, cb::NoExpiryLimit),
                    (uint32_t)nbytes,
                    cookie,
                    datatype);

    if (it != NULL) {
        item_info info;
        if (!get_item_info(handle, it, &info)) {
            // This should never happen (unless we provide invalid
            // arguments)
            item_release(engine, it);
            throw cb::engine_error(cb::engine_errc::failed,
                                   "default_item_allocate_ex");
        }

        return std::make_pair(cb::unique_item_ptr(it, cb::ItemDeleter{handle}),
                              info);
    } else {
        throw cb::engine_error(cb::engine_errc::no_memory,
                               "default_item_allocate_ex");
    }
}

static ENGINE_ERROR_CODE default_item_delete(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint64_t& cas,
        uint16_t vbucket,
        mutation_descr_t& mut_info) {
    struct default_engine* engine = get_handle(handle);
    hash_item* it;
    uint64_t cas_in = cas;
    VBUCKET_GUARD(engine, vbucket);

    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
    do {
        it = item_get(engine,
                      cookie,
                      key.data(),
                      key.size(),
                      DocStateFilter::Alive);
        if (it == nullptr) {
            return ENGINE_KEY_ENOENT;
        }

        if (it->locktime != 0 &&
            it->locktime > engine->server.core->get_current_time()) {
            if (cas_in != it->cas) {
                item_release(engine, it);
                return ENGINE_LOCKED;
            }
        }

        auto* deleted = item_alloc(engine, key.data(), key.size(), it->flags,
                                   it->exptime, it->nbytes, cookie,
                                   it->datatype);

        if (deleted == NULL) {
            item_release(engine, it);
            return ENGINE_TMPFAIL;
        }

        if (cas_in == 0) {
            // If the caller specified the "cas wildcard" we should set
            // the cas for the item we just fetched and do a cas
            // replace with that value
            item_set_cas(handle, deleted, it->cas);
        } else {
            // The caller specified a specific CAS value so we should
            // use that value in our cas replace
            item_set_cas(handle, deleted, cas_in);
        }

        ret = store_item(engine,
                         deleted,
                         &cas,
                         OPERATION_CAS,
                         cookie,
                         DocumentState::Deleted);

        item_release(engine, it);
        item_release(engine, deleted);

        // We should only retry for race conditions if the caller specified
        // cas wildcard
    } while (ret == ENGINE_KEY_EEXISTS && cas_in == 0);

    // vbucket UUID / seqno arn't supported by default engine, so just return
    // a hardcoded vbucket uuid, and zero for the sequence number.
    mut_info.vbucket_uuid = DEFAULT_ENGINE_VBUCKET_UUID;
    mut_info.seqno = 0;

    return ret;
}

static void default_item_release(gsl::not_null<ENGINE_HANDLE*> handle,
                                 gsl::not_null<item*> item) {
    item_release(get_handle(handle), get_real_item(item));
}

static cb::EngineErrorItemPair default_get(gsl::not_null<ENGINE_HANDLE*> handle,
                                           gsl::not_null<const void*> cookie,
                                           const DocKey& key,
                                           uint16_t vbucket,
                                           DocStateFilter documentStateFilter) {
    struct default_engine* engine = get_handle(handle);

    if (!handled_vbucket(engine, vbucket)) {
        return std::make_pair(
                cb::engine_errc::not_my_vbucket,
                cb::unique_item_ptr{nullptr, cb::ItemDeleter{handle}});
    }

    item* it = item_get(
            engine, cookie, key.data(), key.size(), documentStateFilter);
    if (it != nullptr) {
        return cb::makeEngineErrorItemPair(
                cb::engine_errc::success, it, handle);
    } else {
        return cb::makeEngineErrorItemPair(cb::engine_errc::no_such_key);
    }
}

static cb::EngineErrorItemPair default_get_if(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint16_t vbucket,
        std::function<bool(const item_info&)> filter) {
    struct default_engine* engine = get_handle(handle);

    if (!handled_vbucket(engine, vbucket)) {
        return cb::makeEngineErrorItemPair(cb::engine_errc::not_my_vbucket);
    }

    cb::unique_item_ptr ret(item_get(engine,
                                     cookie,
                                     key.data(),
                                     key.size(),
                                     DocStateFilter::Alive),
                            cb::ItemDeleter{handle});
    if (!ret) {
        return cb::makeEngineErrorItemPair(cb::engine_errc::no_such_key);
    }

    item_info info;
    if (!get_item_info(handle, ret.get(), &info)) {
        throw cb::engine_error(cb::engine_errc::failed,
                               "default_get_if: get_item_info failed");
    }

    if (!filter(info)) {
        ret.reset(nullptr);
    }

    return cb::makeEngineErrorItemPair(
            cb::engine_errc::success, ret.release(), handle);
}

static cb::EngineErrorItemPair default_get_and_touch(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint16_t vbucket,
        uint32_t expiry_time) {
    struct default_engine* engine = get_handle(handle);

    if (!handled_vbucket(engine, vbucket)) {
        return cb::makeEngineErrorItemPair(cb::engine_errc::not_my_vbucket);
    }

    hash_item* it = nullptr;
    auto ret = item_get_and_touch(
            engine,
            cookie,
            &it,
            key.data(),
            key.size(),
            engine->server.core->realtime(expiry_time, cb::NoExpiryLimit));

    return cb::makeEngineErrorItemPair(
            cb::engine_errc(ret), reinterpret_cast<item*>(it), handle);
}

static cb::EngineErrorItemPair default_get_locked(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint16_t vbucket,
        uint32_t lock_timeout) {
    auto* engine = get_handle(handle);

    if (!handled_vbucket(engine, vbucket)) {
        return cb::makeEngineErrorItemPair(cb::engine_errc::not_my_vbucket);
    }

    // memcached buckets don't offer any way for the user to configure
    // the lock settings.
    static const uint32_t default_lock_timeout = 15;
    static const uint32_t max_lock_timeout = 30;

    if (lock_timeout == 0 || lock_timeout > max_lock_timeout) {
        lock_timeout = default_lock_timeout;
    }

    // Convert the lock timeout to an absolute time
    lock_timeout += engine->server.core->get_current_time();

    hash_item* it = nullptr;
    auto ret = item_get_locked(engine, cookie, &it, key.data(), key.size(),
                               lock_timeout);
    return cb::makeEngineErrorItemPair(cb::engine_errc(ret), it, handle);
}

static cb::EngineErrorMetadataPair default_get_meta(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        const DocKey& key,
        uint16_t vbucket) {
    auto* engine_handle = get_handle(handle);

    if (!handled_vbucket(engine_handle, vbucket)) {
        return std::make_pair(cb::engine_errc::not_my_vbucket, item_info());
    }

    cb::unique_item_ptr item{item_get(engine_handle,
                                      cookie,
                                      key.data(),
                                      key.size(),
                                      DocStateFilter::AliveOrDeleted),
                             cb::ItemDeleter(handle)};

    if (!item) {
        return std::make_pair(cb::engine_errc::no_such_key, item_info());
    }

    item_info info;
    if (!get_item_info(handle, item.get(), &info)) {
        throw cb::engine_error(cb::engine_errc::failed,
                               "default_get_if: get_item_info failed");
    }

    return std::make_pair(cb::engine_errc::success, info);
}

static ENGINE_ERROR_CODE default_unlock(gsl::not_null<ENGINE_HANDLE*> handle,
                                        gsl::not_null<const void*> cookie,
                                        const DocKey& key,
                                        uint16_t vbucket,
                                        uint64_t cas) {
    auto* engine = get_handle(handle);
    VBUCKET_GUARD(engine, vbucket);
    return item_unlock(engine, cookie, key.data(), key.size(), cas);
}

static ENGINE_ERROR_CODE default_get_stats(gsl::not_null<ENGINE_HANDLE*> handle,
                                           gsl::not_null<const void*> cookie,
                                           cb::const_char_buffer key,
                                           ADD_STAT add_stat) {
    struct default_engine* engine = get_handle(handle);
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

    if (key.empty()) {
        char val[128];
        int len;

        cb_mutex_enter(&engine->stats.lock);
        len = sprintf(val, "%" PRIu64, (uint64_t)engine->stats.evictions);
        add_stat("evictions", 9, val, len, cookie);
        len = sprintf(val, "%" PRIu64, (uint64_t)engine->stats.curr_items);
        add_stat("curr_items", 10, val, len, cookie);
        len = sprintf(val, "%" PRIu64, (uint64_t)engine->stats.total_items);
        add_stat("total_items", 11, val, len, cookie);
        len = sprintf(val, "%" PRIu64, (uint64_t)engine->stats.curr_bytes);
        add_stat("bytes", 5, val, len, cookie);
        len = sprintf(val, "%" PRIu64, engine->stats.reclaimed);
        add_stat("reclaimed", 9, val, len, cookie);
        len = sprintf(val, "%" PRIu64, (uint64_t)engine->config.maxbytes);
        add_stat("engine_maxbytes", 15, val, len, cookie);
        cb_mutex_exit(&engine->stats.lock);
    } else if (key == "slabs"_ccb) {
        slabs_stats(engine, add_stat, cookie);
    } else if (key == "items"_ccb) {
        item_stats(engine, add_stat, cookie);
    } else if (key == "sizes"_ccb) {
        item_stats_sizes(engine, add_stat, cookie);
    } else if (key == "uuid"_ccb) {
        if (engine->config.uuid) {
            add_stat("uuid",
                     4,
                     engine->config.uuid,
                     (uint32_t)strlen(engine->config.uuid),
                     cookie);
        } else {
            add_stat("uuid", 4, "", 0, cookie);
        }
    } else if (key == "scrub"_ccb) {
        char val[128];
        int len;

        cb_mutex_enter(&engine->scrubber.lock);
        if (engine->scrubber.running) {
            add_stat("scrubber:status", 15, "running", 7, cookie);
        } else {
            add_stat("scrubber:status", 15, "stopped", 7, cookie);
        }

        if (engine->scrubber.started != 0) {
            if (engine->scrubber.stopped != 0) {
                time_t diff =
                        engine->scrubber.started - engine->scrubber.stopped;
                len = sprintf(val, "%" PRIu64, (uint64_t)diff);
                add_stat("scrubber:last_run", 17, val, len, cookie);
            }

            len = sprintf(val, "%" PRIu64, engine->scrubber.visited);
            add_stat("scrubber:visited", 16, val, len, cookie);
            len = sprintf(val, "%" PRIu64, engine->scrubber.cleaned);
            add_stat("scrubber:cleaned", 16, val, len, cookie);
        }
        cb_mutex_exit(&engine->scrubber.lock);
    } else {
        ret = ENGINE_KEY_ENOENT;
    }

    return ret;
}

static ENGINE_ERROR_CODE default_store(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie,
                                       gsl::not_null<item*> item,
                                       uint64_t& cas,
                                       ENGINE_STORE_OPERATION operation,
                                       DocumentState document_state) {
    auto* engine = get_handle(handle);
    auto& config = engine->config;
    auto* it = get_real_item(item);

    if (document_state == DocumentState::Deleted && !config.keep_deleted) {
        return safe_item_unlink(engine, it);
    }

    return store_item(engine, it, &cas, operation, cookie, document_state);
}

static cb::EngineErrorCasPair default_store_if(
        gsl::not_null<ENGINE_HANDLE*> handle,
        gsl::not_null<const void*> cookie,
        gsl::not_null<item*> item,
        uint64_t cas,
        ENGINE_STORE_OPERATION operation,
        cb::StoreIfPredicate predicate,
        DocumentState document_state) {
    struct default_engine* engine = get_handle(handle);

    if (predicate) {
        // Check for an existing item and call the item predicate on it.
        auto* it = get_real_item(item);
        auto* key = item_get_key(it);
        if (!key) {
            throw cb::engine_error(cb::engine_errc::failed,
                                   "default_store_if: item_get_key failed");
        }
        cb::unique_item_ptr existing(
                item_get(engine, cookie, *key, DocStateFilter::Alive),
                cb::ItemDeleter{handle});

        cb::StoreIfStatus status;
        if (existing.get()) {
            item_info info;
            if (!get_item_info(handle, existing.get(), &info)) {
                throw cb::engine_error(
                        cb::engine_errc::failed,
                        "default_store_if: get_item_info failed");
            }
            status = predicate(info, {true});
        } else {
            status = predicate(boost::none, {true});
        }

        switch (status) {
        case cb::StoreIfStatus::Fail: {
            return {cb::engine_errc::predicate_failed, 0};
        }
        case cb::StoreIfStatus::Continue:
        case cb::StoreIfStatus::GetItemInfo: {
            break;
        }
        }
    }

    auto* it = get_real_item(item);
    auto status =
            store_item(engine, it, &cas, operation, cookie, document_state);
    return {cb::engine_errc(status), cas};
}

static ENGINE_ERROR_CODE default_flush(gsl::not_null<ENGINE_HANDLE*> handle,
                                       gsl::not_null<const void*> cookie) {
    item_flush_expired(get_handle(handle));

    return ENGINE_SUCCESS;
}

static void default_reset_stats(gsl::not_null<ENGINE_HANDLE*> handle,
                                gsl::not_null<const void*> cookie) {
    struct default_engine* engine = get_handle(handle);
    item_stats_reset(engine);

    cb_mutex_enter(&engine->stats.lock);
    engine->stats.evictions = 0;
    engine->stats.reclaimed = 0;
    engine->stats.total_items = 0;
    cb_mutex_exit(&engine->stats.lock);
}

static ENGINE_ERROR_CODE initalize_configuration(struct default_engine *se,
                                                 const char *cfg_str) {
   ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

   se->config.vb0 = true;

   if (cfg_str != NULL) {
       struct config_item items[13];
       int ii = 0;

       memset(&items, 0, sizeof(items));
       items[ii].key = "verbose";
       items[ii].datatype = DT_SIZE;
       items[ii].value.dt_size = &se->config.verbose;
       ++ii;

       items[ii].key = "eviction";
       items[ii].datatype = DT_BOOL;
       items[ii].value.dt_bool = &se->config.evict_to_free;
       ++ii;

       items[ii].key = "cache_size";
       items[ii].datatype = DT_SIZE;
       items[ii].value.dt_size = &se->config.maxbytes;
       ++ii;

       items[ii].key = "preallocate";
       items[ii].datatype = DT_BOOL;
       items[ii].value.dt_bool = &se->config.preallocate;
       ++ii;

       items[ii].key = "factor";
       items[ii].datatype = DT_FLOAT;
       items[ii].value.dt_float = &se->config.factor;
       ++ii;

       items[ii].key = "chunk_size";
       items[ii].datatype = DT_SIZE;
       items[ii].value.dt_size = &se->config.chunk_size;
       ++ii;

       items[ii].key = "item_size_max";
       items[ii].datatype = DT_SIZE;
       items[ii].value.dt_size = &se->config.item_size_max;
       ++ii;

       items[ii].key = "ignore_vbucket";
       items[ii].datatype = DT_BOOL;
       items[ii].value.dt_bool = &se->config.ignore_vbucket;
       ++ii;

       items[ii].key = "vb0";
       items[ii].datatype = DT_BOOL;
       items[ii].value.dt_bool = &se->config.vb0;
       ++ii;

       items[ii].key = "config_file";
       items[ii].datatype = DT_CONFIGFILE;
       ++ii;

       items[ii].key = "uuid";
       items[ii].datatype = DT_STRING;
       items[ii].value.dt_string = &se->config.uuid;
       ++ii;

       items[ii].key = "keep_deleted";
       items[ii].datatype = DT_BOOL;
       items[ii].value.dt_bool = &se->config.keep_deleted;
       ++ii;

       items[ii].key = NULL;
       ++ii;
       cb_assert(ii == 13);
       ret = ENGINE_ERROR_CODE(se->server.core->parse_config(cfg_str,
                                                             items,
                                                             stderr));
   }

   if (se->config.vb0) {
       set_vbucket_state(se, 0, vbucket_state_active);
   }

   return ret;
}

static bool set_vbucket(struct default_engine *e,
                        const void* cookie,
                        protocol_binary_request_set_vbucket *req,
                        ADD_RESPONSE response) {
    vbucket_state_t state;
    size_t bodylen = ntohl(req->message.header.request.bodylen)
        - ntohs(req->message.header.request.keylen);
    if (bodylen != sizeof(vbucket_state_t)) {
        const char *msg = "Incorrect packet format";
        return response(NULL, 0, NULL, 0, msg, (uint32_t)strlen(msg),
                        PROTOCOL_BINARY_RAW_BYTES,
                        PROTOCOL_BINARY_RESPONSE_EINVAL, 0, cookie);
    }
    memcpy(&state, &req->message.body.state, sizeof(state));
    state = vbucket_state_t(ntohl(state));

    if (!is_valid_vbucket_state_t(state)) {
        const char *msg = "Invalid vbucket state";
        return response(NULL, 0, NULL, 0, msg, (uint32_t)strlen(msg),
                        PROTOCOL_BINARY_RAW_BYTES,
                        PROTOCOL_BINARY_RESPONSE_EINVAL, 0, cookie);
    }

    set_vbucket_state(e, ntohs(req->message.header.request.vbucket), state);
    return response(NULL, 0, NULL, 0, &state, sizeof(state),
                    PROTOCOL_BINARY_RAW_BYTES,
                    PROTOCOL_BINARY_RESPONSE_SUCCESS, 0, cookie);
}

static bool get_vbucket(struct default_engine *e,
                        const void* cookie,
                        protocol_binary_request_get_vbucket *req,
                        ADD_RESPONSE response) {
    vbucket_state_t state;
    state = get_vbucket_state(e, ntohs(req->message.header.request.vbucket));
    state = vbucket_state_t(ntohl(state));

    return response(NULL, 0, NULL, 0, &state, sizeof(state),
                    PROTOCOL_BINARY_RAW_BYTES,
                    PROTOCOL_BINARY_RESPONSE_SUCCESS, 0, cookie);
}

static bool rm_vbucket(struct default_engine *e,
                       const void *cookie,
                       protocol_binary_request_header *req,
                       ADD_RESPONSE response) {
    set_vbucket_state(e, ntohs(req->request.vbucket), vbucket_state_dead);
    return response(NULL, 0, NULL, 0, NULL, 0, PROTOCOL_BINARY_RAW_BYTES,
                    PROTOCOL_BINARY_RESPONSE_SUCCESS, 0, cookie);
}

static bool scrub_cmd(struct default_engine *e,
                      const void *cookie,
                      protocol_binary_request_header *request,
                      ADD_RESPONSE response) {

    protocol_binary_response_status res = PROTOCOL_BINARY_RESPONSE_SUCCESS;
    if (!item_start_scrub(e)) {
        res = PROTOCOL_BINARY_RESPONSE_EBUSY;
    }

    return response(NULL, 0, NULL, 0, NULL, 0, PROTOCOL_BINARY_RAW_BYTES,
                    res, 0, cookie);
}

/**
 * set_param only added to allow per bucket xattr on/off
 */
static bool set_param(struct default_engine* e,
                      const void* cookie,
                      protocol_binary_request_set_param* req,
                      ADD_RESPONSE response) {
    size_t keylen = ntohs(req->message.header.request.keylen);
    uint8_t extlen = req->message.header.request.extlen;
    size_t vallen = ntohl(req->message.header.request.bodylen);
    protocol_binary_engine_param_t paramtype =
            static_cast<protocol_binary_engine_param_t>(
                    ntohl(req->message.body.param_type));

    if (keylen == 0 || (vallen - keylen - extlen) == 0) {
        return false;
    }

    // Only support protocol_binary_engine_param_flush with xattr_enabled
    if (paramtype == protocol_binary_engine_param_flush) {
        const char* keyp =
                reinterpret_cast<const char*>(req->bytes) + sizeof(req->bytes);
        const char* valuep = keyp + keylen;
        vallen -= (keylen + extlen);
        cb::const_char_buffer key(keyp, keylen);
        cb::const_char_buffer value(valuep, vallen);

        if (key == "xattr_enabled") {
            if (value == "true") {
                e->config.xattr_enabled = true;
            } else if (value == "false") {
                e->config.xattr_enabled = false;
            } else {
                return false;
            }
            return response(NULL,
                            0,
                            NULL,
                            0,
                            NULL,
                            0,
                            PROTOCOL_BINARY_RAW_BYTES,
                            PROTOCOL_BINARY_RESPONSE_SUCCESS,
                            0,
                            cookie);
            ;
        }
    }
    return false;
}

static ENGINE_ERROR_CODE default_unknown_command(
        gsl::not_null<ENGINE_HANDLE*> handle,
        const void* cookie,
        gsl::not_null<protocol_binary_request_header*> request,
        ADD_RESPONSE response,
        DocNamespace doc_namespace) {
    struct default_engine* e = get_handle(handle);
    bool sent;

    switch(request->request.opcode) {
    case PROTOCOL_BINARY_CMD_SCRUB:
        sent = scrub_cmd(e, cookie, request, response);
        break;
    case PROTOCOL_BINARY_CMD_DEL_VBUCKET:
        sent = rm_vbucket(e, cookie, request, response);
        break;
    case PROTOCOL_BINARY_CMD_SET_VBUCKET:
        sent = set_vbucket(
                e,
                cookie,
                reinterpret_cast<protocol_binary_request_set_vbucket*>(
                        request.get()),
                response);
        break;
    case PROTOCOL_BINARY_CMD_GET_VBUCKET:
        sent = get_vbucket(
                e,
                cookie,
                reinterpret_cast<protocol_binary_request_get_vbucket*>(
                        request.get()),
                response);
        break;
    case PROTOCOL_BINARY_CMD_SET_PARAM:
        sent = set_param(e,
                         cookie,
                         reinterpret_cast<protocol_binary_request_set_param*>(
                                 request.get()),
                         response);
        break;
    default:
        sent = response(NULL, 0, NULL, 0, NULL, 0, PROTOCOL_BINARY_RAW_BYTES,
                        PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, 0, cookie);
        break;
    }

    if (sent) {
        return ENGINE_SUCCESS;
    } else {
        return ENGINE_FAILED;
    }
}

void item_set_cas(gsl::not_null<ENGINE_HANDLE*> handle,
                  gsl::not_null<item*> item,
                  uint64_t val) {
    hash_item* it = get_real_item(item);
    it->cas = val;
}

hash_key* item_get_key(const hash_item* item)
{
    const char *ret = reinterpret_cast<const char*>(item + 1);
    return (hash_key*)ret;
}

char* item_get_data(const hash_item* item)
{
    const hash_key* key = item_get_key(item);
    return ((char*)key->header.full_key) + hash_key_get_key_len(key);
}

static bool get_item_info(gsl::not_null<ENGINE_HANDLE*> handle,
                          gsl::not_null<const item*> item,
                          gsl::not_null<item_info*> item_info) {
    auto* it = reinterpret_cast<const hash_item*>(item.get());
    const hash_key* key = item_get_key(it);
    auto* engine = get_handle(handle);

    // This may potentially open up for a race, but:
    // 1) If the item isn't linked anymore we don't need to mask
    //    the CAS anymore. (if the client tries to use that
    //    CAS it'll fail with an invalid cas)
    // 2) In production the memcached buckets don't use the
    //    ZOMBIE state (and if we start doing that, it is only
    //    the owner of the item pointer (the one bumping the
    //    refcount initially) which would change this. Anyone else
    //    would create a new item object and set the iflag
    //    to deleted.
    const auto iflag = it->iflag.load(std::memory_order_relaxed);

    if ((iflag & ITEM_LINKED) && it->locktime != 0 &&
        it->locktime > engine->server.core->get_current_time()) {
        // This object is locked. According to docs/Document.md we should
        // return -1 in such cases to hide the real CAS for the other clients
        // (Note the check on ITEM_LINKED.. for the actual item returned by
        // get_locked we return an item which isn't linked (copy of the
        // linked item) to allow returning the real CAS.
        item_info->cas = uint64_t(-1);
    } else {
        item_info->cas = it->cas;
    }

    item_info->vbucket_uuid = DEFAULT_ENGINE_VBUCKET_UUID;
    item_info->seqno = 0;
    if (it->exptime == 0) {
        item_info->exptime = 0;
    } else {
        item_info->exptime = engine->server.core->abstime(it->exptime);
    }
    item_info->nbytes = it->nbytes;
    item_info->flags = it->flags;
    item_info->nkey = hash_key_get_client_key_len(key);
    item_info->key = hash_key_get_client_key(key);
    item_info->value[0].iov_base = item_get_data(it);
    item_info->value[0].iov_len = it->nbytes;
    item_info->datatype = it->datatype;
    if (iflag & ITEM_ZOMBIE) {
        item_info->document_state = DocumentState::Deleted;
    } else {
        item_info->document_state = DocumentState::Alive;
    }
    return true;
}

static bool set_item_info(gsl::not_null<ENGINE_HANDLE*> handle,
                          gsl::not_null<item*> item,
                          gsl::not_null<const item_info*> itm_info) {
    auto* it = reinterpret_cast<hash_item*>(item.get());
    it->datatype = itm_info->datatype;
    return true;
}

static bool is_xattr_supported(gsl::not_null<ENGINE_HANDLE*> handle) {
    return get_handle(handle)->config.xattr_enabled;
}
