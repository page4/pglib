#pragma once

#include <boost/intrusive/unordered_set.hpp>
#include <boost/intrusive/list.hpp>
#include <vector>
#include <stack>

namespace bi = boost::intrusive;

template<class Key, class Value, auto CanEvict>
class lru_cache {
private:
    using htable_hook_type = bi::unordered_set_member_hook<bi::link_mode<bi::auto_unlink>>;
    using list_hook_type = bi::list_member_hook<bi::link_mode<bi::auto_unlink>>;
    using timestamp_type = uint64_t;
    using key_type = Key;
    using value_type = Value;

    struct node_type {
        key_type key;
        value_type value;
        timestamp_type access_time;
        timestamp_type promotion_time;
        bool evictable;

        htable_hook_type htable_hook;
        list_hook_type list_hook;

        static auto hash_func = std::hash<key_type>();

        struct hash_key {
            auto operator()(auto key) const {
                // key is a uintptr_t which is aligned with 64 bytes.
                // this is 2.8x faster than std::hash<key_type>()(key)
                return key >> 6;
            }
        };
    };

    using node_pointer_type = node_type *;

    using htable_type = bi::unordered_set<node_type,
          bi::member_hook<node_type, htable_hook_type, &node_type::htable_hook>,
          bi::constant_time_size<false>,
          bi::hash<typename node_type::hash_key>,
          bi::power_2_buckets<true>,
          bi::key_of_value<typename node_type::key_of_node>>;
    using insert_commit_data_type = typename htable_type::insert_commit_data;

    using list_type = bi::list<node_type,
          bi::constant_time_size<false>,
          bi::member_hook<node_type, list_hook_type, &node_type::list_hook>>;


    const size_t _max_size;
    const size_t _soft_promotion;
    const size_t _batch_evict_size;

    std::vector<node_type> _node_pool;
    std::vector<typename htable_type::bucket_type> _index_buckets;

    std::stack<node_pointer_type, std::vector<node_pointer_type>> _free_nodes;

    list_type _lru;
    htable_type _index;
    timestamp_type _current_time;

    list_type _unevicted_list;
    size_t _nr_unevicted;

private:
    node_pointer_type allocate_node() {
        auto node = _free_nodes.top();
        _free_nodes.pop();
        return node;
    }

    void free_node(node_pointer_type node) {
        _free_nodes.push(node);
    }

    timestamp_type next_timestamp() {
        return ++_current_time;
    }

    node_pointer_type find_in_index(const key_type key) {
        auto it = _index.find(key);
        if (it != _index.end()) {
            return &*it;
        }
        return nullptr;
    }

    node_pointer_type insert_check(const key_type key, insert_commit_data_type& commit) {
        make_room_for_insert();
        auto [iter, not_exists] = _index.insert_check(key, commit);
        return !not_exists ? (&*iter) : nullptr;
    }

    void lru_touch(node_pointer_type node) {
        node->access_time = next_timestamp();
        if (node->access_time - node->promotion_time > _soft_promotion &&
                node->evictable) {
            node->list_hook.unlink();
            _lru.push_front(*node);
            node->promotion_time = node->access_time;
        }
    }

    void make_room_for_insert() {
        if (_free_nodes.size() + _nr_unevicted < _max_size) {
            auto iter = _lru.end();
            for (size_t i = 0; i < _batch_evict_size; ++ i) {
                --iter;
                iter->htable_hook.unlink();
                free_node(&*iter);
            }
            _lru.erase(iter, _lru.end());
        }
    }

    void do_insert(key_type key, value_type value, bool evictable, const insert_commit_data_type& commit) {
        auto new_node = allocate_node();
        new_node->key = key;
        new_node->value = value;
        new_node->evictable = evictable;
        new_node->access_time = next_timestamp();
        new_node->promotion_time = new_node->access_time;

        _index.insert_commit(*new_node, commit);
        auto& target_list = evictable ? _lru : _unevicted_list;
        target_list.push_front(*new_node);
    }

public:
    //  capacity should be power of 2
    lru_cache(size_t capacity = 1024) :
        _max_size(capacity),
        _soft_promotion(capacity * 0.5),
        _batch_evict_size(std::min<size_t>(3, _max_size - _soft_promotion)),
        _node_pool(_max_size * 2),
        _index_buckets(_max_size * 4),
        _index(typename htable_type::bucket_traits(&_index_buckets[0], _index_buckets.size())),
        _current_time(0),
        _nr_unevicted(0) {

        node_pointer_type p = &_node_pool.back();
        while (p >= &_node_pool.front()) {
            _free_nodes.push(p);
            --p;
        }
    }

    std::optional<value_type> get(const key_type key) {
        auto node = find_in_index(key);
        if (node) {
            lru_touch(node);
            return node->value;
        }
        return std::nullopt;
    }

    void insert_on_missing(key_type key, value_type value) {
        insert_commit_data_type commit;
        auto node = insert_check(key, commit);
        if (!node) {
            do_insert(key, value, true, commit) ;
        }
    }

    void insert(key_type key, value_type value, bool evictable) {
        insert_commit_data_type commit;
        auto node = insert_check(key, commit);
        if (node) {
            node->value = value;
            lru_touch(node);
            return ;
        }

        do_insert(key, value, evictable, commit);

        if (!evictable) {
            ++ _nr_unevicted;
        }
    }
};

