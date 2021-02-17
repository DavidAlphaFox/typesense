#include <string>
#include <vector>
#include <json.hpp>
#include "collection_manager.h"
#include "logger.h"

constexpr const size_t CollectionManager::DEFAULT_NUM_MEMORY_SHARDS;

CollectionManager::CollectionManager() {

}

Collection* CollectionManager::init_collection(const nlohmann::json & collection_meta,
                                               const uint32_t collection_next_seq_id) {
    std::string this_collection_name = collection_meta[Collection::COLLECTION_NAME_KEY].get<std::string>();

    std::vector<field> fields;
    nlohmann::json fields_map = collection_meta[Collection::COLLECTION_SEARCH_FIELDS_KEY];

    for (nlohmann::json::iterator it = fields_map.begin(); it != fields_map.end(); ++it) {
        nlohmann::json & field_obj = it.value();

        // handle older records indexed before optional field introduction
        if(field_obj.count(fields::optional) == 0) {
            field_obj[fields::optional] = false;
        }

        fields.push_back({field_obj[fields::name], field_obj[fields::type],
                          field_obj[fields::facet], field_obj[fields::optional]});
    }

    std::string default_sorting_field = collection_meta[Collection::COLLECTION_DEFAULT_SORTING_FIELD_KEY].get<std::string>();

    uint64_t created_at = collection_meta.find((const char*)Collection::COLLECTION_CREATED) != collection_meta.end() ?
                       collection_meta[Collection::COLLECTION_CREATED].get<uint64_t>() : 0;

    size_t num_memory_shards = collection_meta.count(Collection::COLLECTION_NUM_MEMORY_SHARDS) != 0 ?
                               collection_meta[Collection::COLLECTION_NUM_MEMORY_SHARDS].get<size_t>() :
                               DEFAULT_NUM_MEMORY_SHARDS;

    size_t index_all_fields = collection_meta.count(Collection::COLLECTION_INDEX_ALL_FIELDS) != 0 ?
                              collection_meta[Collection::COLLECTION_INDEX_ALL_FIELDS].get<bool>() :
                              false;

    LOG(INFO) << "Found collection " << this_collection_name << " with " << num_memory_shards << " memory shards.";

    Collection* collection = new Collection(this_collection_name,
                                            collection_meta[Collection::COLLECTION_ID_KEY].get<uint32_t>(),
                                            created_at,
                                            collection_next_seq_id,
                                            store,
                                            fields,
                                            default_sorting_field,
                                            num_memory_shards,
                                            max_memory_ratio,
                                            index_all_fields);

    return collection;
}

void CollectionManager::add_to_collections(Collection* collection) {
    collections.emplace(collection->get_name(), collection);
    collection_id_names.emplace(collection->get_collection_id(), collection->get_name());
}

void CollectionManager::init(Store *store,
                             const float max_memory_ratio,
                             const std::string & auth_key) {
    this->store = store;
    this->bootstrap_auth_key = auth_key;
    this->max_memory_ratio = max_memory_ratio;
}

Option<bool> CollectionManager::load(const size_t init_batch_size) {
    // This function must be idempotent, i.e. when called multiple times, must produce the same state without leaks
    LOG(INFO) << "CollectionManager::load()";

    Option<bool> auth_init_op = auth_manager.init(store);
    if(!auth_init_op.ok()) {
        LOG(ERROR) << "Auth manager init failed, error=" << auth_init_op.error();
    }

    std::string next_collection_id_str;
    StoreStatus next_coll_id_status = store->get(NEXT_COLLECTION_ID_KEY, next_collection_id_str);

    if(next_coll_id_status == StoreStatus::ERROR) {
        return Option<bool>(500, "Error while fetching the next collection id from the disk.");
    }

    if(next_coll_id_status == StoreStatus::FOUND) {
        next_collection_id = (uint32_t) stoi(next_collection_id_str);
    } else {
        next_collection_id = 0;
    }

    std::vector<std::string> collection_meta_jsons;
    store->scan_fill(Collection::COLLECTION_META_PREFIX, collection_meta_jsons);

    LOG(INFO) << "Found " << collection_meta_jsons.size() << " collection(s) on disk.";

    for(auto & collection_meta_json: collection_meta_jsons) {
        nlohmann::json collection_meta;

        try {
            collection_meta = nlohmann::json::parse(collection_meta_json);
        } catch(...) {
            return Option<bool>(500, "Error while parsing collection meta.");
        }

        const std::string & this_collection_name = collection_meta[Collection::COLLECTION_NAME_KEY].get<std::string>();
        std::string collection_next_seq_id_str;
        StoreStatus next_seq_id_status = store->get(Collection::get_next_seq_id_key(this_collection_name),
                                                    collection_next_seq_id_str);

        if(next_seq_id_status == StoreStatus::ERROR) {
            return Option<bool>(500, "Error while fetching collection's next sequence ID from the disk for collection "
                                     "`" + this_collection_name + "`");
        }

        if(next_seq_id_status == StoreStatus::NOT_FOUND && next_coll_id_status == StoreStatus::FOUND) {
            return Option<bool>(500, "Next collection id was found, but collection's next sequence ID is missing for "
                                     "`" + this_collection_name + "`");
        }

        uint32_t collection_next_seq_id = next_seq_id_status == StoreStatus::NOT_FOUND ? 0 :
                                          StringUtils::deserialize_uint32_t(collection_next_seq_id_str);

        if(get_collection(this_collection_name)) {
            // To maintain idempotency, if the collection already exists in-memory, drop it from memory
            LOG(WARNING) << "Dropping duplicate collection " << this_collection_name << " before loading it again.";
            drop_collection(this_collection_name, false);
        }

        Collection* collection = init_collection(collection_meta, collection_next_seq_id);

        LOG(INFO) << "Loading collection " << collection->get_name();

        // initialize overrides
        std::vector<std::string> collection_override_jsons;
        store->scan_fill(Collection::get_override_key(this_collection_name, ""), collection_override_jsons);

        for(const auto & collection_override_json: collection_override_jsons) {
            nlohmann::json collection_override = nlohmann::json::parse(collection_override_json);
            override_t override;
            auto parse_op = override_t::parse(collection_override, "", override);
            if(parse_op.ok()) {
                collection->add_override(override);
            } else {
                LOG(ERROR) << "Skipping loading of override: " << parse_op.error();
            }
        }

        // initialize synonyms
        std::vector<std::string> collection_synonym_jsons;
        store->scan_fill(Collection::get_synonym_key(this_collection_name, ""), collection_synonym_jsons);

        for(const auto & collection_synonym_json: collection_synonym_jsons) {
            nlohmann::json collection_synonym = nlohmann::json::parse(collection_synonym_json);
            synonym_t synonym(collection_synonym);
            collection->add_synonym(synonym);
        }

        // Fetch records from the store and re-create memory index
        std::vector<std::string> documents;
        const std::string seq_id_prefix = collection->get_seq_id_collection_prefix();

        rocksdb::Iterator* iter = store->scan(seq_id_prefix);
        std::unique_ptr<rocksdb::Iterator> iter_guard(iter);

        std::vector<std::vector<index_record>> iter_batch;

        for(size_t i = 0; i < collection->get_num_memory_shards(); i++) {
            iter_batch.push_back(std::vector<index_record>());
        }

        size_t num_found_docs = 0;
        size_t num_valid_docs = 0;
        size_t num_indexed_docs = 0;

        auto begin = std::chrono::high_resolution_clock::now();

        while(iter->Valid() && iter->key().starts_with(seq_id_prefix)) {
            num_found_docs++;
            const uint32_t seq_id = Collection::get_seq_id_from_key(iter->key().ToString());

            nlohmann::json document;

            try {
                document = nlohmann::json::parse(iter->value().ToString());
            } catch(const std::exception& e) {
                LOG(ERROR) << "JSON error: " << e.what();
                return Option<bool>(false, "Bad JSON.");
            }

            num_valid_docs++;
            iter_batch[seq_id % collection->get_num_memory_shards()].emplace_back(index_record(0, seq_id, document, CREATE));

            // Peek and check for last record right here so that we handle batched indexing correctly
            // Without doing this, the "last batch" would have to be indexed outside the loop.
            iter->Next();
            bool last_record = !(iter->Valid() && iter->key().starts_with(seq_id_prefix));

            // batch must match atleast the number of shards
            const size_t batch_size = std::max(init_batch_size, collection->get_num_memory_shards());

            if((num_valid_docs % batch_size == 0) || last_record) {
                std::vector<size_t> indexed_counts;
                indexed_counts.reserve(iter_batch.size());

                collection->par_index_in_memory(iter_batch, indexed_counts);

                for(size_t i = 0; i < collection->get_num_memory_shards(); i++) {
                    size_t num_records = iter_batch[i].size();
                    size_t num_indexed = indexed_counts[i];

                    if(num_indexed != num_records) {
                        const Option<std::string> & index_error_op = get_first_index_error(iter_batch[i]);
                        if(!index_error_op.ok()) {
                            return Option<bool>(false, index_error_op.get());
                        }
                    }
                    iter_batch[i].clear();
                    num_indexed_docs += num_indexed;
                }
            }

            auto time_millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - begin).count();

            auto throttle_time_millis = uint64_t((LOAD_THROTTLE_PERCENT/100) * time_millis);

            if(throttle_time_millis != 0) {
                // we throttle only when we have accumulated atleast 1 ms worth of throttling time
                begin = std::chrono::high_resolution_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(throttle_time_millis));
            }
        }

        add_to_collections(collection);
        LOG(INFO) << "Indexed " << num_indexed_docs << "/" << num_found_docs
                  << " documents into collection " << collection->get_name();
    }

    std::string symlink_prefix_key = std::string(SYMLINK_PREFIX) + "_";
    rocksdb::Iterator* iter = store->scan(symlink_prefix_key);
    while(iter->Valid() && iter->key().starts_with(symlink_prefix_key)) {
        std::vector<std::string> parts;
        StringUtils::split(iter->key().ToString(), parts, symlink_prefix_key);
        collection_symlinks[parts[0]] = iter->value().ToString();
        iter->Next();
    }

    delete iter;

    return Option<bool>(true);
}


void CollectionManager::dispose() {
    for(auto & name_collection: collections) {
        delete name_collection.second;
        name_collection.second = nullptr;
    }

    collections.clear();
    store->close();
}

bool CollectionManager::auth_key_matches(const std::string& auth_key_sent,
                                         const std::string& action,
                                         const std::vector<std::string>& collections,
                                         std::map<std::string, std::string>& params) const {
    if(auth_key_sent.empty()) {
        return false;
    }

    // check with bootstrap auth key
    if(bootstrap_auth_key == auth_key_sent) {
        return true;
    }

    // finally, check managed auth keys
    return auth_manager.authenticate(auth_key_sent, action, collections, params);
}

Option<Collection*> CollectionManager::create_collection(const std::string& name,
                                                         const size_t num_memory_shards,
                                                         const std::vector<field> & fields,
                                                         const std::string & default_sorting_field,
                                                         const uint64_t created_at,
                                                         const bool index_all_fields) {
    if(store->contains(Collection::get_meta_key(name))) {
        return Option<Collection*>(409, std::string("A collection with name `") + name + "` already exists.");
    }

    bool found_default_sorting_field = false;
    nlohmann::json fields_json = nlohmann::json::array();;

    Option<bool> fields_json_op = field::fields_to_json_fields(fields, default_sorting_field, fields_json,
                                                               found_default_sorting_field);

    if(!fields_json_op.ok()) {
        return Option<Collection*>(fields_json_op.code(), fields_json_op.error());
    }

    if(!found_default_sorting_field) {
        return Option<Collection*>(400, "Default sorting field is defined as `" + default_sorting_field +
                                        "` but is not found in the schema.");
    }

    nlohmann::json collection_meta;
    collection_meta[Collection::COLLECTION_NAME_KEY] = name;
    collection_meta[Collection::COLLECTION_ID_KEY] = next_collection_id;
    collection_meta[Collection::COLLECTION_SEARCH_FIELDS_KEY] = fields_json;
    collection_meta[Collection::COLLECTION_DEFAULT_SORTING_FIELD_KEY] = default_sorting_field;
    collection_meta[Collection::COLLECTION_CREATED] = created_at;
    collection_meta[Collection::COLLECTION_NUM_MEMORY_SHARDS] = num_memory_shards;
    collection_meta[Collection::COLLECTION_INDEX_ALL_FIELDS] = index_all_fields;

    Collection* new_collection = new Collection(name, next_collection_id, created_at, 0, store, fields,
                                                default_sorting_field, num_memory_shards,
                                                this->max_memory_ratio, index_all_fields);
    next_collection_id++;

    rocksdb::WriteBatch batch;
    batch.Put(Collection::get_next_seq_id_key(name), StringUtils::serialize_uint32_t(0));
    batch.Put(Collection::get_meta_key(name), collection_meta.dump());
    batch.Put(NEXT_COLLECTION_ID_KEY, std::to_string(next_collection_id));
    bool write_ok = store->batch_write(batch);

    if(!write_ok) {
        return Option<Collection*>(500, "Could not write to on-disk storage.");
    }

    add_to_collections(new_collection);

    return Option<Collection*>(new_collection);
}

Collection* CollectionManager::get_collection(const std::string & collection_name) {
    if(collections.count(collection_name) != 0) {
        return collections.at(collection_name);
    }

    // a symlink name takes lesser precedence over a real collection name
    if(collection_symlinks.count(collection_name) != 0) {
        const std::string & symlinked_name = collection_symlinks.at(collection_name);
        if(collections.count(symlinked_name) != 0) {
            return collections.at(symlinked_name);
        }
    }

    return nullptr;
}

Collection* CollectionManager::get_collection_with_id(uint32_t collection_id) {
    if(collection_id_names.count(collection_id) != 0) {
        return get_collection(collection_id_names.at(collection_id));
    }

    return nullptr;
}

std::vector<Collection*> CollectionManager::get_collections() {
    std::vector<Collection*> collection_vec;
    for(auto kv: collections) {
        collection_vec.push_back(kv.second);
    }

    std::sort(std::begin(collection_vec), std::end(collection_vec),
              [] (Collection* lhs, Collection* rhs) {
                  return lhs->get_collection_id()  > rhs->get_collection_id();
              });

    return collection_vec;
}

Option<bool> CollectionManager::drop_collection(const std::string& collection_name, const bool remove_from_store) {
    Collection* collection = get_collection(collection_name);
    if(collection == nullptr) {
        return Option<bool>(404, "No collection with name `" + collection_name + "` found.");
    }

    if(remove_from_store) {
        const std::string &collection_id_str = std::to_string(collection->get_collection_id());

        // Note: The order of dropping documents first before dropping collection meta is important for replication
        rocksdb::Iterator* iter = store->scan(collection_id_str);
        while(iter->Valid() && iter->key().starts_with(collection_id_str)) {
            store->remove(iter->key().ToString());
            iter->Next();
        }
        delete iter;

        store->remove(Collection::get_next_seq_id_key(collection_name));
        store->remove(Collection::get_meta_key(collection_name));
    }

    collections.erase(collection_name);
    collection_id_names.erase(collection->get_collection_id());

    delete collection;

    return Option<bool>(true);
}

uint32_t CollectionManager::get_next_collection_id() {
    return next_collection_id;
}

std::string CollectionManager::get_symlink_key(const std::string & symlink_name) {
    return std::string(SYMLINK_PREFIX) + "_" + symlink_name;
}

void CollectionManager::set_next_collection_id(uint32_t next_id) {
    next_collection_id = next_id;
}

spp::sparse_hash_map<std::string, std::string> & CollectionManager::get_symlinks() {
    return collection_symlinks;
}

Option<std::string> CollectionManager::resolve_symlink(const std::string & symlink_name) {
    if(collection_symlinks.count(symlink_name) != 0) {
        return Option<std::string>(collection_symlinks.at(symlink_name));
    }

    return Option<std::string>(404, "Not found.");
}

Option<bool> CollectionManager::upsert_symlink(const std::string & symlink_name, const std::string & collection_name) {
    if(collections.count(symlink_name) != 0) {
        return Option<bool>(500, "Name `" + symlink_name + "` conflicts with an existing collection name.");
    }

    bool inserted = store->insert(get_symlink_key(symlink_name), collection_name);
    if(!inserted) {
        return Option<bool>(500, "Unable to insert into store.");
    }

    collection_symlinks[symlink_name] = collection_name;
    return Option<bool>(true);
}

Option<bool> CollectionManager::delete_symlink(const std::string & symlink_name) {
    bool removed = store->remove(get_symlink_key(symlink_name));
    if(!removed) {
        return Option<bool>(500, "Unable to delete from store.");
    }

    collection_symlinks.erase(symlink_name);
    return Option<bool>(true);
}

Store* CollectionManager::get_store() {
    return store;
}

AuthManager& CollectionManager::getAuthManager() {
    return auth_manager;
}


Option<bool> CollectionManager::do_search(std::map<std::string, std::string>& req_params, std::string& results_json_str) {
    auto begin = std::chrono::high_resolution_clock::now();

    const char *NUM_TYPOS = "num_typos";
    const char *PREFIX = "prefix";
    const char *DROP_TOKENS_THRESHOLD = "drop_tokens_threshold";
    const char *TYPO_TOKENS_THRESHOLD = "typo_tokens_threshold";
    const char *FILTER = "filter_by";
    const char *QUERY = "q";
    const char *QUERY_BY = "query_by";
    const char *QUERY_BY_WEIGHTS = "query_by_weights";
    const char *SORT_BY = "sort_by";

    const char *FACET_BY = "facet_by";
    const char *FACET_QUERY = "facet_query";
    const char *MAX_FACET_VALUES = "max_facet_values";

    const char *GROUP_BY = "group_by";
    const char *GROUP_LIMIT = "group_limit";

    const char *LIMIT_HITS = "limit_hits";
    const char *PER_PAGE = "per_page";
    const char *PAGE = "page";
    const char *RANK_TOKENS_BY = "rank_tokens_by";
    const char *INCLUDE_FIELDS = "include_fields";
    const char *EXCLUDE_FIELDS = "exclude_fields";

    const char *PINNED_HITS = "pinned_hits";
    const char *HIDDEN_HITS = "hidden_hits";

    // strings under this length will be fully highlighted, instead of showing a snippet of relevant portion
    const char *SNIPPET_THRESHOLD = "snippet_threshold";

    // the number of tokens that should surround the highlighted text
    const char *HIGHLIGHT_AFFIX_NUM_TOKENS = "highlight_affix_num_tokens";

    // list of fields which will be highlighted fully without snippeting
    const char *HIGHLIGHT_FULL_FIELDS = "highlight_full_fields";

    const char *HIGHLIGHT_START_TAG = "highlight_start_tag";
    const char *HIGHLIGHT_END_TAG = "highlight_end_tag";

    if(req_params.count(NUM_TYPOS) == 0) {
        req_params[NUM_TYPOS] = "2";
    }

    if(req_params.count(PREFIX) == 0) {
        req_params[PREFIX] = "true";
    }

    if(req_params.count(DROP_TOKENS_THRESHOLD) == 0) {
        req_params[DROP_TOKENS_THRESHOLD] = std::to_string(Index::DROP_TOKENS_THRESHOLD);
    }

    if(req_params.count(TYPO_TOKENS_THRESHOLD) == 0) {
        req_params[TYPO_TOKENS_THRESHOLD] = std::to_string(Index::TYPO_TOKENS_THRESHOLD);
    }

    if(req_params.count(QUERY) == 0) {
        return Option<bool>(400, std::string("Parameter `") + QUERY + "` is required.");
    }

    if(req_params.count(MAX_FACET_VALUES) == 0) {
        req_params[MAX_FACET_VALUES] = "10";
    }

    if(req_params.count(FACET_QUERY) == 0) {
        req_params[FACET_QUERY] = "";
    }

    if(req_params.count(LIMIT_HITS) == 0) {
        req_params[LIMIT_HITS] = std::to_string(UINT32_MAX);
    }

    if(req_params.count(SNIPPET_THRESHOLD) == 0) {
        req_params[SNIPPET_THRESHOLD] = "30";
    }

    if(req_params.count(HIGHLIGHT_AFFIX_NUM_TOKENS) == 0) {
        req_params[HIGHLIGHT_AFFIX_NUM_TOKENS] = "4";
    }

    if(req_params.count(HIGHLIGHT_FULL_FIELDS) == 0) {
        req_params[HIGHLIGHT_FULL_FIELDS] = "";
    }

    if(req_params.count(HIGHLIGHT_START_TAG) == 0) {
        req_params[HIGHLIGHT_START_TAG] = "<mark>";
    }

    if(req_params.count(HIGHLIGHT_END_TAG) == 0) {
        req_params[HIGHLIGHT_END_TAG] = "</mark>";
    }

    if(req_params.count(PER_PAGE) == 0) {
        if(req_params[FACET_QUERY].empty()) {
            req_params[PER_PAGE] = "10";
        } else {
            // for facet query we will set per_page to zero if it is not explicitly overridden
            req_params[PER_PAGE] = "0";
        }
    }

    if(req_params.count(PAGE) == 0) {
        req_params[PAGE] = "1";
    }

    if(req_params.count(INCLUDE_FIELDS) == 0) {
        req_params[INCLUDE_FIELDS] = "";
    }

    if(req_params.count(EXCLUDE_FIELDS) == 0) {
        req_params[EXCLUDE_FIELDS] = "";
    }

    if(req_params.count(GROUP_BY) == 0) {
        req_params[GROUP_BY] = "";
    }

    if(req_params.count(GROUP_LIMIT) == 0) {
        if(req_params[GROUP_BY] != "") {
            req_params[GROUP_LIMIT] = "3";
        } else {
            req_params[GROUP_LIMIT] = "0";
        }
    }

    std::vector<std::string> query_by_weights_str;
    std::vector<size_t> query_by_weights;

    if(req_params.count(QUERY_BY_WEIGHTS) != 0) {
        StringUtils::split(req_params[QUERY_BY_WEIGHTS], query_by_weights_str, ",");
        for(const auto& weight_str: query_by_weights_str) {
            if(!StringUtils::is_uint32_t(weight_str)) {
                return Option<bool>(400, "Parameter `" + std::string(QUERY_BY_WEIGHTS) +
                                         "` must be a comma separated string of unsigned integers.");
            }

            query_by_weights.push_back(std::stoi(weight_str));
        }
    }

    if(!StringUtils::is_uint32_t(req_params[DROP_TOKENS_THRESHOLD])) {
        return Option<bool>(400, "Parameter `" + std::string(DROP_TOKENS_THRESHOLD) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[TYPO_TOKENS_THRESHOLD])) {
        return Option<bool>(400, "Parameter `" + std::string(TYPO_TOKENS_THRESHOLD) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[NUM_TYPOS])) {
        return Option<bool>(400,"Parameter `" + std::string(NUM_TYPOS) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[PER_PAGE])) {
        return Option<bool>(400,"Parameter `" + std::string(PER_PAGE) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[PAGE])) {
        return Option<bool>(400,"Parameter `" + std::string(PAGE) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[MAX_FACET_VALUES])) {
        return Option<bool>(400,"Parameter `" + std::string(MAX_FACET_VALUES) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[LIMIT_HITS])) {
        return Option<bool>(400,"Parameter `" + std::string(LIMIT_HITS) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[SNIPPET_THRESHOLD])) {
        return Option<bool>(400,"Parameter `" + std::string(SNIPPET_THRESHOLD) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[HIGHLIGHT_AFFIX_NUM_TOKENS])) {
        return Option<bool>(400,"Parameter `" + std::string(HIGHLIGHT_AFFIX_NUM_TOKENS) + "` must be an unsigned integer.");
    }

    if(!StringUtils::is_uint32_t(req_params[GROUP_LIMIT])) {
        return Option<bool>(400,"Parameter `" + std::string(GROUP_LIMIT) + "` must be an unsigned integer.");
    }

    std::string filter_str = req_params.count(FILTER) != 0 ? req_params[FILTER] : "";

    std::vector<std::string> search_fields;
    StringUtils::split(req_params[QUERY_BY], search_fields, ",");

    std::vector<std::string> facet_fields;
    StringUtils::split(req_params[FACET_BY], facet_fields, ",");

    std::vector<std::string> include_fields_vec;
    StringUtils::split(req_params[INCLUDE_FIELDS], include_fields_vec, ",");

    std::vector<std::string> exclude_fields_vec;
    StringUtils::split(req_params[EXCLUDE_FIELDS], exclude_fields_vec, ",");

    spp::sparse_hash_set<std::string> include_fields(include_fields_vec.begin(), include_fields_vec.end());
    spp::sparse_hash_set<std::string> exclude_fields(exclude_fields_vec.begin(), exclude_fields_vec.end());

    std::vector<std::string> group_by_fields;
    StringUtils::split(req_params[GROUP_BY], group_by_fields, ",");

    std::vector<sort_by> sort_fields;
    if(req_params.count(SORT_BY) != 0) {
        std::vector<std::string> sort_field_strs;
        StringUtils::split(req_params[SORT_BY], sort_field_strs, ",");

        if(sort_field_strs.size() > 3) {
            return Option<bool>(400,"Only upto 3 sort fields are allowed.");
        }

        for(const std::string & sort_field_str: sort_field_strs) {
            std::vector<std::string> expression_parts;
            StringUtils::split(sort_field_str, expression_parts, ":");

            if(expression_parts.size() != 2) {
                return Option<bool>(400,std::string("Parameter `") + SORT_BY + "` is malformed.");
            }

            StringUtils::toupper(expression_parts[1]);
            sort_fields.emplace_back(expression_parts[0], expression_parts[1]);
        }
    }

    if(req_params.count(PINNED_HITS) == 0) {
        req_params[PINNED_HITS] = "";
    }

    if(req_params.count(HIDDEN_HITS) == 0) {
        req_params[HIDDEN_HITS] = "";
    }

    CollectionManager & collectionManager = CollectionManager::get_instance();
    Collection* collection = collectionManager.get_collection(req_params["collection"]);

    if(collection == nullptr) {
        return Option<bool>(404, "Not found.");
    }

    bool prefix = (req_params[PREFIX] == "true");
    const size_t drop_tokens_threshold = (size_t) std::stoi(req_params[DROP_TOKENS_THRESHOLD]);
    const size_t typo_tokens_threshold = (size_t) std::stoi(req_params[TYPO_TOKENS_THRESHOLD]);

    if(req_params.count(RANK_TOKENS_BY) == 0) {
        req_params[RANK_TOKENS_BY] = "DEFAULT_SORTING_FIELD";
    }

    StringUtils::toupper(req_params[RANK_TOKENS_BY]);
    token_ordering token_order = (req_params[RANK_TOKENS_BY] == "DEFAULT_SORTING_FIELD") ? MAX_SCORE : FREQUENCY;

    Option<nlohmann::json> result_op = collection->search(req_params[QUERY], search_fields, filter_str, facet_fields,
                                                          sort_fields, std::stoi(req_params[NUM_TYPOS]),
                                                          static_cast<size_t>(std::stol(req_params[PER_PAGE])),
                                                          static_cast<size_t>(std::stol(req_params[PAGE])),
                                                          token_order, prefix, drop_tokens_threshold,
                                                          include_fields, exclude_fields,
                                                          static_cast<size_t>(std::stol(req_params[MAX_FACET_VALUES])),
                                                          req_params[FACET_QUERY],
                                                          static_cast<size_t>(std::stol(req_params[SNIPPET_THRESHOLD])),
                                                          static_cast<size_t>(std::stol(req_params[HIGHLIGHT_AFFIX_NUM_TOKENS])),
                                                          req_params[HIGHLIGHT_FULL_FIELDS],
                                                          typo_tokens_threshold,
                                                          req_params[PINNED_HITS],
                                                          req_params[HIDDEN_HITS],
                                                          group_by_fields,
                                                          static_cast<size_t>(std::stol(req_params[GROUP_LIMIT])),
                                                          req_params[HIGHLIGHT_START_TAG],
                                                          req_params[HIGHLIGHT_END_TAG],
                                                          query_by_weights,
                                                          static_cast<size_t>(std::stol(req_params[LIMIT_HITS]))
    );

    uint64_t timeMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - begin).count();


    if(!result_op.ok()) {
        return Option<bool>(result_op.code(), result_op.error());
    }

    nlohmann::json result = result_op.get();
    result["search_time_ms"] = timeMillis;
    result["page"] = std::stoi(req_params[PAGE]);
    results_json_str = result.dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore);

    //LOG(INFO) << "Time taken: " << timeMillis << "ms";

    return Option<bool>(true);
}
