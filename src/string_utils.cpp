#include "string_utils.h"
#include <iostream>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <random>
#include <openssl/sha.h>
#include <map>
#include "logger.h"

std::string lower_and_no_special_chars(const std::string & str) {
    std::stringstream ss;

    for(char c : str) {
        bool is_ascii = ((int)(c) >= 0);
        bool keep_char = !is_ascii || std::isalnum(c);

        if(keep_char) {
            ss << (char) std::tolower(c);
        }
    }

    return ss.str();
}

std::string StringUtils::randstring(size_t length) {
    static auto& chrs = "0123456789"
                        "abcdefghijklmnopqrstuvwxyz"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    thread_local std::mt19937 rg(std::random_device{}());
    thread_local std::uniform_int_distribution<uint32_t> pick(0, sizeof(chrs) - 2);

    std::string s;
    s.reserve(length);

    while(length--) {
        s += chrs[pick(rg)];
    }

    return s;
}

std::string StringUtils::hmac(const std::string& key, const std::string& msg) {
    unsigned int hmac_len;
    unsigned char hmac[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), key.c_str(), key.size(),
         reinterpret_cast<const unsigned char *>(msg.c_str()), msg.size(),
         hmac, &hmac_len);

    std::string digest_raw(reinterpret_cast<char*>(&hmac), hmac_len);
    return StringUtils::base64_encode(digest_raw);
}

std::string StringUtils::str2hex(const std::string &str, bool capital) {
    std::string hexstr;
    hexstr.resize(str.size() * 2);
    const size_t a = capital ? 'A' - 1 : 'a' - 1;
    for (size_t i = 0, c = str[0] & 0xFF; i < hexstr.size(); c = str[i / 2] & 0xFF) {
        hexstr[i++] = c > 0x9F ? (c / 16 - 9) | a : c / 16 | '0';
        hexstr[i++] = (c & 0xF) > 9 ? (c % 16 - 9) | a : c % 16 | '0';
    }

    return hexstr;
}

std::string StringUtils::hash_sha256(const std::string& str) {
    const size_t SHA256_SIZE = 32;
    unsigned char hash_buf[SHA256_SIZE];
    SHA256(reinterpret_cast<const unsigned char *>(str.c_str()), str.size(), hash_buf);
    return StringUtils::str2hex(std::string(reinterpret_cast<char*>(hash_buf), SHA256_SIZE));
}

std::map<std::string, std::string> StringUtils::parse_query_string(const std::string &query) {
    if(query.size() > 4000) {
        LOG(ERROR) << "Query string exceeds max allowed length of 4000. Actual length: " << query.size();
        return {};
    }

    std::map<std::string, std::string> query_map;
    std::string key_value;

    int query_len = int(query.size());
    int i = 0;

    if(query[0] == '?') {
        i++;
    }

    while(i < query_len) {
        // we have to support un-encoded "&&" in the query string value, which makes things a bit more complex
        bool start_of_new_param = query[i] == '&' &&
                                  (i != query_len - 1 && query[i + 1] != '&') &&
                                  (i != 0 && query[i - 1] != '&');
        bool end_of_params = (i == query_len - 1);

        if(start_of_new_param || end_of_params) {
            // Save accumulated key_value
            if(end_of_params && query[i] != '&') {
                key_value += query[i];
            }

            size_t j = 0;
            bool iterating_on_key = true;
            std::string key;
            std::string value;

            while(j < key_value.size()) {
                if(key_value[j] == '=' && iterating_on_key) {
                    iterating_on_key = false;
                } else if(iterating_on_key) {
                    key += key_value[j];
                } else {
                    value += key_value[j];
                }

                j++;
            }

            if(!key.empty() && key != "&") {
                value = StringUtils::url_decode(value);

                if (query_map.count(key) == 0) {
                    query_map[key] = value;
                } else if (key == "filter_by") {
                    query_map[key] = query_map[key] + "&&" + value;
                } else {
                    query_map[key] = value;
                }
            }

            key_value = "";
        } else {
            key_value += query[i];
        }

        i++;
    }

    return query_map;
}

void StringUtils::split_to_values(const std::string& vals_str, std::vector<std::string>& filter_values) {
    size_t i = 0;

    bool inside_tick = false;
    std::string buffer;
    buffer.reserve(20);

    while(i < vals_str.size()) {
        char c = vals_str[i];
        bool escaped_tick = (i != 0) && c == '`' && vals_str[i-1] == '\\';

        switch(c) {
            case '`':
                if(escaped_tick) {
                    buffer += c;
                } else if(inside_tick && !buffer.empty()) {
                    inside_tick = false;
                } else {
                    inside_tick = true;
                }
                break;
            case ',':
                if(!inside_tick) {
                    filter_values.push_back(buffer);
                    buffer = "";
                } else {
                    buffer += c;
                }
                break;
            default:
                buffer += c;
        }

        i++;
    }

    if(!buffer.empty()) {
        filter_values.push_back(buffer);
    }
}

/*size_t StringUtils::unicode_length(const std::string& bytes) {
    std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> utf8conv;
    return utf8conv.from_bytes(bytes).size();
}*/
