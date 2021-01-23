#pragma once

#include <string>
#include "art.h"
#include "option.h"
#include "string_utils.h"

namespace field_types {
    static const std::string STRING = "string";
    static const std::string INT32 = "int32";
    static const std::string INT64 = "int64";
    static const std::string FLOAT = "float";
    static const std::string BOOL = "bool";
    static const std::string GEOPOINT = "geopoint";
    static const std::string STRING_ARRAY = "string[]";
    static const std::string INT32_ARRAY = "int32[]";
    static const std::string INT64_ARRAY = "int64[]";
    static const std::string FLOAT_ARRAY = "float[]";
    static const std::string BOOL_ARRAY = "bool[]";
}

namespace fields {
    static const std::string name = "name";
    static const std::string type = "type";
    static const std::string facet = "facet";
    static const std::string optional = "optional";
}

static const uint8_t DEFAULT_GEO_RESOLUTION = 7;
static const uint8_t FINEST_GEO_RESOLUTION = 15;

struct field {
    std::string name;
    std::string type;
    bool facet;
    bool optional;

    uint8_t geo_resolution;

    field(const std::string & name, const std::string & type, const bool facet):
        name(name), type(type), facet(facet), optional(false), geo_resolution(DEFAULT_GEO_RESOLUTION) {

    }

    field(const std::string & name, const std::string & type, const bool facet, const bool optional):
            name(name), type(type), facet(facet), optional(optional), geo_resolution(DEFAULT_GEO_RESOLUTION) {

    }

    field(const std::string & name, const std::string & type, const bool facet, const bool optional,
          const uint8_t geo_resolution):
            name(name), type(type), facet(facet), optional(optional), geo_resolution(geo_resolution) {

    }

    bool is_single_integer() const {
        return (type == field_types::INT32 || type == field_types::INT64);
    }

    bool is_single_float() const {
        return (type == field_types::FLOAT);
    }

    bool is_single_bool() const {
        return (type == field_types::BOOL);
    }

    bool is_integer() const {
        return (type == field_types::INT32 || type == field_types::INT32_ARRAY ||
               type == field_types::INT64 || type == field_types::INT64_ARRAY);
    }

    bool is_int32() const {
        return (type == field_types::INT32 || type == field_types::INT32_ARRAY);
    }

    bool is_int64() const {
        return (type == field_types::INT64 || type == field_types::INT64_ARRAY);
    }

    bool is_float() const {
        return (type == field_types::FLOAT || type == field_types::FLOAT_ARRAY);
    }

    bool is_bool() const {
        return (type == field_types::BOOL || type == field_types::BOOL_ARRAY);
    }

    bool is_geopoint() const {
        return (type == field_types::GEOPOINT);
    }

    bool is_string() const {
        return (type == field_types::STRING || type == field_types::STRING_ARRAY);
    }

    bool is_facet() const {
        return facet;
    }

    bool is_array() const {
        return (type == field_types::STRING_ARRAY || type == field_types::INT32_ARRAY ||
                type == field_types::FLOAT_ARRAY ||
                type == field_types::INT64_ARRAY || type == field_types::BOOL_ARRAY);
    }

    bool has_valid_type() const {
        return is_string() || is_integer() || is_float() || is_bool() || is_geopoint();
    }

    std::string faceted_name() const {
        return (facet && !is_string()) ? "_fstr_" + name : name;
    }
};

struct filter {
    std::string field_name;
    std::vector<std::string> values;
    std::vector<NUM_COMPARATOR> comparators;

    static Option<NUM_COMPARATOR> extract_num_comparator(std::string & comp_and_value) {
        auto num_comparator = EQUALS;

        if(StringUtils::is_integer(comp_and_value) || StringUtils::is_float(comp_and_value)) {
            num_comparator = EQUALS;
        }

        // the ordering is important - we have to compare 2-letter operators first
        else if(comp_and_value.compare(0, 2, "<=") == 0) {
            num_comparator = LESS_THAN_EQUALS;
        }

        else if(comp_and_value.compare(0, 2, ">=") == 0) {
            num_comparator = GREATER_THAN_EQUALS;
        }

        else if(comp_and_value.compare(0, 1, "<") == 0) {
            num_comparator = LESS_THAN;
        }

        else if(comp_and_value.compare(0, 1, ">") == 0) {
            num_comparator = GREATER_THAN;
        }

        else {
            return Option<NUM_COMPARATOR>(400, "Numerical field has an invalid comparator.");
        }

        if(num_comparator == LESS_THAN || num_comparator == GREATER_THAN) {
            comp_and_value = comp_and_value.substr(1);
        } else if(num_comparator == LESS_THAN_EQUALS || num_comparator == GREATER_THAN_EQUALS) {
            comp_and_value = comp_and_value.substr(2);
        }

        comp_and_value = StringUtils::trim(comp_and_value);

        return Option<NUM_COMPARATOR>(num_comparator);
    }
};

namespace sort_field_const {
    static const std::string name = "name";
    static const std::string order = "order";
    static const std::string asc = "ASC";
    static const std::string desc = "DESC";
    static const std::string text_match = "_text_match";
}

struct sort_by {
    std::string name;
    std::string order;
    int64_t geopoint;

    sort_by(const std::string & name, const std::string & order): name(name), order(order), geopoint(0) {

    }

    sort_by(const std::string &name, const std::string &order, int64_t geopoint) :
            name(name), order(order), geopoint(geopoint) {

    }

    sort_by& operator=(sort_by other) {
        name = other.name;
        order = other.order;
        geopoint = other.geopoint;
        return *this;
    }
};

struct token_pos_cost_t {
    size_t pos;
    uint32_t cost;
};

struct facet_count_t {
    uint32_t count;
    spp::sparse_hash_set<uint64_t> groups;  // used for faceting grouped results

    // used to fetch the actual document and value for representation
    uint32_t doc_id;
    uint32_t array_pos;

    spp::sparse_hash_map<uint32_t, token_pos_cost_t> query_token_pos;
};

struct facet_stats_t {
    double fvmin = std::numeric_limits<double>::max(),
            fvmax = -std::numeric_limits<double>::min(),
            fvcount = 0,
            fvsum = 0;
};

struct facet {
    const std::string field_name;
    std::map<uint64_t, facet_count_t> result_map;
    facet_stats_t stats;

    facet(const std::string & field_name): field_name(field_name) {

    }
};

struct facet_query_t {
    std::string field_name;
    std::string query;
};

struct facet_value_t {
    std::string value;
    std::string highlighted;
    uint32_t count;
};