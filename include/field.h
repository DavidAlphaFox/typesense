#pragma once

#include <string>
#include "art.h"
#include "option.h"
#include "string_utils.h"
#include "json.hpp"

namespace field_types {
    static const std::string STRING = "string";
    static const std::string INT32 = "int32";
    static const std::string INT64 = "int64";
    static const std::string FLOAT = "float";
    static const std::string BOOL = "bool";
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

struct field {
    std::string name;
    std::string type;
    bool facet;
    bool optional;

    field(const std::string & name, const std::string & type, const bool facet):
        name(name), type(type), facet(facet), optional(false) {

    }

    field(const std::string & name, const std::string & type, const bool facet, const bool optional):
            name(name), type(type), facet(facet), optional(optional) {

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
        return is_string() || is_integer() || is_float() || is_bool();
    }

    std::string faceted_name() const {
        return (facet && !is_string()) ? "_fstr_" + name : name;
    }

    static bool get_type(const nlohmann::json& obj, std::string& field_type) {
        if(obj.is_array()) {
            if(obj.empty() || obj[0].is_array()) {
                return false;
            }

            bool parseable = get_single_type(obj[0], field_type);
            if(!parseable) {
                return false;
            }

            field_type = field_type + "[]";
            return true;
        }

        if(obj.is_object()) {
            return false;
        }

        return get_single_type(obj, field_type);
    }

    static bool get_single_type(const nlohmann::json& obj, std::string& field_type) {
        if(obj.is_string()) {
            field_type = field_types::STRING;
            return true;
        }

        if(obj.is_number_float()) {
            field_type = field_types::FLOAT;
            return true;
        }

        if(obj.is_number_integer()) {
            field_type = field_types::INT64;
            return true;
        }

        if(obj.is_boolean()) {
            field_type = field_types::BOOL;
            return true;
        }

        return false;
    }

    static Option<bool> fields_to_json_fields(const std::vector<field> & fields,
                                              const std::string & default_sorting_field, nlohmann::json& fields_json,
                                              bool& found_default_sorting_field) {
        for(const field & field: fields) {
            nlohmann::json field_val;
            field_val[fields::name] = field.name;
            field_val[fields::type] = field.type;
            field_val[fields::facet] = field.facet;
            field_val[fields::optional] = field.optional;
            fields_json.push_back(field_val);

            if(!field.has_valid_type()) {
                return Option<bool>(400, "Field `" + field.name +
                                                "` has an invalid data type `" + field.type +
                                                "`, see docs for supported data types.");
            }

            if(field.name == default_sorting_field && !(field.type == field_types::INT32 ||
                                                        field.type == field_types::INT64 ||
                                                        field.type == field_types::FLOAT)) {
                return Option<bool>(400, "Default sorting field `" + default_sorting_field +
                                                "` must be a single valued numerical field.");
            }

            if(field.name == default_sorting_field) {
                if(field.optional) {
                    return Option<bool>(400, "Default sorting field `" + default_sorting_field +
                                                    "` cannot be an optional field.");
                }

                found_default_sorting_field = true;
            }
        }

        return Option<bool>(true);
    }
};

struct filter {
    std::string field_name;
    std::vector<std::string> values;
    std::vector<NUM_COMPARATOR> comparators;

    static const std::string RANGE_OPERATOR() {
        return "..";
    }

    static Option<bool> validate_numerical_filter_value(field _field, const std::string& raw_value) {
        if(_field.is_int32() && !StringUtils::is_int32_t(raw_value)) {
            return Option<bool>(400, "Error with filter field `" + _field.name + "`: Not an int32.");
        }

        else if(_field.is_int64() && !StringUtils::is_int64_t(raw_value)) {
            return Option<bool>(400, "Error with filter field `" + _field.name + "`: Not an int64.");
        }

        else if(_field.is_float() && !StringUtils::is_float(raw_value)) {
            return Option<bool>(400, "Error with filter field `" + _field.name + "`: Not a float.");
        }

        return Option<bool>(true);
    }

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

        else if(comp_and_value.find("..") != std::string::npos) {
            num_comparator = RANGE_INCLUSIVE;
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

    sort_by(const std::string & name, const std::string & order): name(name), order(order) {

    }

    sort_by& operator=(sort_by other) {
        name = other.name;
        order = other.order;
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

    std::unordered_map<uint32_t, token_pos_cost_t> query_token_pos;
};

struct facet_stats_t {
    double fvmin = std::numeric_limits<double>::max(),
            fvmax = -std::numeric_limits<double>::min(),
            fvcount = 0,
            fvsum = 0;
};

struct facet {
    const std::string field_name;
    std::unordered_map<uint64_t, facet_count_t> result_map;
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