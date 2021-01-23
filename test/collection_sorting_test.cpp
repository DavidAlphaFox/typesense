#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <collection_manager.h>
#include <h3api.h>
#include "collection.h"

class CollectionSortingTest : public ::testing::Test {
protected:
    Store *store;
    CollectionManager & collectionManager = CollectionManager::get_instance();

    std::vector<std::string> query_fields;
    std::vector<sort_by> sort_fields;

    void setupCollection() {
        std::string state_dir_path = "/tmp/typesense_test/collection_sorting";
        LOG(INFO) << "Truncating and creating: " << state_dir_path;
        system(("rm -rf "+state_dir_path+" && mkdir -p "+state_dir_path).c_str());

        store = new Store(state_dir_path);
        collectionManager.init(store, 1.0, "auth_key");
        collectionManager.load();
    }

    virtual void SetUp() {
        setupCollection();
    }

    virtual void TearDown() {
        collectionManager.dispose();
        delete store;
    }
};

TEST_F(CollectionSortingTest, SortingOrder) {
    Collection *coll_mul_fields;

    std::ifstream infile(std::string(ROOT_DIR)+"test/multi_field_documents.jsonl");
    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("starring", field_types::STRING, false),
                                 field("points", field_types::INT32, false),
                                 field("cast", field_types::STRING_ARRAY, false)};

    coll_mul_fields = collectionManager.get_collection("coll_mul_fields");
    if(coll_mul_fields == nullptr) {
        coll_mul_fields = collectionManager.create_collection("coll_mul_fields", 4, fields, "points").get();
    }

    std::string json_line;

    while (std::getline(infile, json_line)) {
        coll_mul_fields->add(json_line);
    }

    infile.close();

    query_fields = {"title"};
    std::vector<std::string> facets;
    sort_fields = { sort_by("points", "ASC") };
    nlohmann::json results = coll_mul_fields->search("the", query_fields, "", facets, sort_fields, 0, 15, 1, FREQUENCY, false).get();
    ASSERT_EQ(10, results["hits"].size());

    std::vector<std::string> ids = {"17", "13", "10", "4", "0", "1", "8", "6", "16", "11"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // limiting results to just 5, "ASC" keyword must be case insensitive
    sort_fields = { sort_by("points", "asc") };
    results = coll_mul_fields->search("the", query_fields, "", facets, sort_fields, 0, 5, 1, FREQUENCY, false).get();
    ASSERT_EQ(5, results["hits"].size());

    ids = {"17", "13", "10", "4", "0"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // desc

    sort_fields = { sort_by("points", "dEsc") };
    results = coll_mul_fields->search("the", query_fields, "", facets, sort_fields, 0, 15, 1, FREQUENCY, false).get();
    ASSERT_EQ(10, results["hits"].size());

    ids = {"11", "16", "6", "8", "1", "0", "10", "4", "13", "17"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // With empty list of sort_by fields:
    // should be ordered desc on the default sorting field, since the match score will be the same for all records.
    sort_fields = { };
    results = coll_mul_fields->search("of", query_fields, "", facets, sort_fields, 0, 10, 1, FREQUENCY, false).get();
    ASSERT_EQ(5, results["hits"].size());

    ids = {"11", "12", "5", "4", "17"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    collectionManager.drop_collection("coll_mul_fields");
}

TEST_F(CollectionSortingTest, DefaultSortingFieldValidations) {
    // Default sorting field must be a  numerical field
    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("tags", field_types::STRING_ARRAY, true),
                                 field("age", field_types::INT32, false),
                                 field("average", field_types::INT32, false) };

    std::vector<sort_by> sort_fields = { sort_by("age", "DESC"), sort_by("average", "DESC") };

    Option<Collection*> collection_op = collectionManager.create_collection("sample_collection", 4, fields, "name");
    EXPECT_FALSE(collection_op.ok());
    EXPECT_EQ("Default sorting field `name` must be a single valued numerical field.", collection_op.error());
    collectionManager.drop_collection("sample_collection");

    // Default sorting field must exist as a field in schema

    sort_fields = { sort_by("age", "DESC"), sort_by("average", "DESC") };
    collection_op = collectionManager.create_collection("sample_collection", 4, fields, "NOT-DEFINED");
    EXPECT_FALSE(collection_op.ok());
    EXPECT_EQ("Default sorting field is defined as `NOT-DEFINED` but is not found in the schema.", collection_op.error());
    collectionManager.drop_collection("sample_collection");
}

TEST_F(CollectionSortingTest, Int64AsDefaultSortingField) {
    Collection *coll_mul_fields;

    std::ifstream infile(std::string(ROOT_DIR)+"test/multi_field_documents.jsonl");
    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("starring", field_types::STRING, false),
                                 field("points", field_types::INT64, false),
                                 field("cast", field_types::STRING_ARRAY, false)};

    coll_mul_fields = collectionManager.get_collection("coll_mul_fields");
    if(coll_mul_fields == nullptr) {
        coll_mul_fields = collectionManager.create_collection("coll_mul_fields", 4, fields, "points").get();
    }

    auto doc_str1 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233234, \"cast\": [\"baz\"] }";
    const Option<nlohmann::json> & add_op = coll_mul_fields->add(doc_str1);
    ASSERT_TRUE(add_op.ok());

    auto doc_str2 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233232, \"cast\": [\"baz\"] }";
    auto doc_str3 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233235, \"cast\": [\"baz\"] }";
    auto doc_str4 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233231, \"cast\": [\"baz\"] }";

    coll_mul_fields->add(doc_str2);
    coll_mul_fields->add(doc_str3);
    coll_mul_fields->add(doc_str4);

    query_fields = {"title"};
    std::vector<std::string> facets;
    sort_fields = { sort_by("points", "ASC") };
    nlohmann::json results = coll_mul_fields->search("foo", query_fields, "", facets, sort_fields, 0, 10, 1, FREQUENCY, false).get();
    ASSERT_EQ(4, results["hits"].size());

    std::vector<std::string> ids = {"3", "1", "0", "2"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // DESC
    sort_fields = { sort_by("points", "desc") };
    results = coll_mul_fields->search("foo", query_fields, "", facets, sort_fields, 0, 10, 1, FREQUENCY, false).get();
    ASSERT_EQ(4, results["hits"].size());

    ids = {"2", "0", "1", "3"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }
}

TEST_F(CollectionSortingTest, SortOnFloatFields) {
    Collection *coll_float_fields;

    std::ifstream infile(std::string(ROOT_DIR)+"test/float_documents.jsonl");
    std::vector<field> fields = {
            field("title", field_types::STRING, false),
            field("score", field_types::FLOAT, false),
            field("average", field_types::FLOAT, false)
    };

    std::vector<sort_by> sort_fields_desc = { sort_by("score", "DESC"), sort_by("average", "DESC") };

    coll_float_fields = collectionManager.get_collection("coll_float_fields");
    if(coll_float_fields == nullptr) {
        coll_float_fields = collectionManager.create_collection("coll_float_fields", 4, fields, "score").get();
    }

    std::string json_line;

    while (std::getline(infile, json_line)) {
        coll_float_fields->add(json_line);
    }

    infile.close();

    query_fields = {"title"};
    std::vector<std::string> facets;
    nlohmann::json results = coll_float_fields->search("Jeremy", query_fields, "", facets, sort_fields_desc, 0, 10, 1, FREQUENCY, false).get();
    ASSERT_EQ(7, results["hits"].size());

    std::vector<std::string> ids = {"2", "0", "3", "1", "5", "4", "6"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    std::vector<sort_by> sort_fields_asc = { sort_by("score", "ASC"), sort_by("average", "ASC") };
    results = coll_float_fields->search("Jeremy", query_fields, "", facets, sort_fields_asc, 0, 10, 1, FREQUENCY, false).get();
    ASSERT_EQ(7, results["hits"].size());

    ids = {"6", "4", "5", "1", "3", "0", "2"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        EXPECT_STREQ(id.c_str(), result_id.c_str());
    }

    // second field by desc

    std::vector<sort_by> sort_fields_asc_desc = { sort_by("score", "ASC"), sort_by("average", "DESC") };
    results = coll_float_fields->search("Jeremy", query_fields, "", facets, sort_fields_asc_desc, 0, 10, 1, FREQUENCY, false).get();
    ASSERT_EQ(7, results["hits"].size());

    ids = {"5", "4", "6", "1", "3", "0", "2"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        EXPECT_STREQ(id.c_str(), result_id.c_str());
    }

    collectionManager.drop_collection("coll_float_fields");
}

TEST_F(CollectionSortingTest, ThreeSortFieldsLimit) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT32, false),
                                 field("average", field_types::INT32, false),
                                 field("min", field_types::INT32, false),
                                 field("max", field_types::INT32, false),
                                 };

    coll1 = collectionManager.get_collection("coll1");
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 4, fields, "points").get();
    }

    nlohmann::json doc1;

    doc1["id"] = "100";
    doc1["title"] = "The quick brown fox";
    doc1["points"] = 25;
    doc1["average"] = 25;
    doc1["min"] = 25;
    doc1["max"] = 25;

    coll1->add(doc1.dump());

    std::vector<sort_by> sort_fields_desc = {
        sort_by("points", "DESC"),
        sort_by("average", "DESC"),
        sort_by("max", "DESC"),
        sort_by("min", "DESC"),
    };

    query_fields = {"title"};
    auto res_op = coll1->search("the", query_fields, "", {}, sort_fields_desc, 0, 10, 1, FREQUENCY, false);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Only upto 3 sort_by fields can be specified.", res_op.error().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, NegativeInt64Value) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT64, false),
    };

    coll1 = collectionManager.get_collection("coll1");
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 4, fields, "points").get();
    }

    nlohmann::json doc1;

    doc1["id"] = "100";
    doc1["title"] = "The quick brown fox";
    doc1["points"] = -2678400;

    coll1->add(doc1.dump());

    std::vector<sort_by> sort_fields_desc = {
      sort_by("points", "DESC")
    };

    query_fields = {"title"};
    auto res = coll1->search("*", query_fields, "points:>=1577836800", {}, sort_fields_desc, 0, 10, 1, FREQUENCY,
                             false).get();

    ASSERT_EQ(0, res["found"].get<size_t>());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, GeoPointFiltering) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("loc", field_types::GEOPOINT, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1");
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
            {"Palais Garnier", "48.872576479306765, 2.332291112241466"},
            {"Sacre Coeur", "48.888286721920934, 2.342340862419206"},
            {"Arc de Triomphe", "48.87538726829884, 2.296113163780903"},
            {"Place de la Concorde", "48.86536119187326, 2.321850747347093"},
            {"Louvre Musuem", "48.86065813197502, 2.3381285349616725"},
            {"Les Invalides", "48.856648379569904, 2.3118555692631357"},
            {"Eiffel Tower", "48.85821022164442, 2.294239067890161"},
            {"Notre-Dame de Paris", "48.852455825574495, 2.35071182406452"},
            {"Musee Grevin", "48.872370541246816, 2.3431536410008906"},
            {"Pantheon", "48.84620987789056, 2.345152755563131"},
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        std::vector<std::string> lat_lng;
        StringUtils::split(records[i][1], lat_lng, ", ");

        double lat = std::stod(lat_lng[0]);
        double lng = std::stod(lat_lng[1]);

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["loc"] = {lat, lng};
        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    // pick a large radius covering all points, with a point close to Pantheon
    std::vector<sort_by> geo_sort_fields = {
        sort_by("loc(48.84442912268208, 2.3490714964332353)", "ASC")
    };

    auto results = coll1->search("*",
                            {}, "loc: (48.84442912268208, 2.3490714964332353, 20 km)",
                            {}, geo_sort_fields, 0, 10, 1, FREQUENCY).get();

    ASSERT_EQ(10, results["found"].get<size_t>());

    std::vector<std::string> expected_ids = {
        "9", "7", "4", "3", "8", "5", "0", "6", "1", "2"
    };

    for(size_t i=0; i < expected_ids.size(); i++) {
        ASSERT_STREQ(expected_ids[i].c_str(), results["hits"][i]["document"]["id"].get<std::string>().c_str());
    }

    // desc, without filter
    geo_sort_fields = {
            sort_by("loc(48.84442912268208, 2.3490714964332353)", "DESC")
    };

    results = coll1->search("*",
                            {}, "",
                            {}, geo_sort_fields, 0, 10, 1, FREQUENCY).get();

    ASSERT_EQ(10, results["found"].get<size_t>());

    for(size_t i=0; i < expected_ids.size(); i++) {
        ASSERT_STREQ(expected_ids[expected_ids.size() - 1 - i].c_str(), results["hits"][i]["document"]["id"].get<std::string>().c_str());
    }

    // with bad sort field formats
    std::vector<sort_by> bad_geo_sort_fields = {
        sort_by("loc(,2.3490714964332353)", "ASC")
    };

    auto res_op = coll1->search("*",
                            {}, "",
                            {}, bad_geo_sort_fields, 0, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Geopoint sorting field `loc` must be in the `field(24.56,10.45):ASC` format.", res_op.error().c_str());

    bad_geo_sort_fields = {
            sort_by("loc(x, y)", "ASC")
    };

    res_op = coll1->search("*",
                                {}, "",
                                {}, bad_geo_sort_fields, 0, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Geopoint sorting field `loc` must be in the `field(24.56,10.45):ASC` format.", res_op.error().c_str());

    bad_geo_sort_fields = {
        sort_by("loc(", "ASC")
    };

    res_op = coll1->search("*",
                           {}, "",
                           {}, bad_geo_sort_fields, 0, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Could not find a field named `loc(` in the schema for sorting.", res_op.error().c_str());

    bad_geo_sort_fields = {
        sort_by("loc)", "ASC")
    };

    res_op = coll1->search("*",
                           {}, "",
                           {}, bad_geo_sort_fields, 0, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Could not find a field named `loc)` in the schema for sorting.", res_op.error().c_str());

    bad_geo_sort_fields = {
            sort_by("l()", "ASC")
    };

    res_op = coll1->search("*",
                           {}, "",
                           {}, bad_geo_sort_fields, 0, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Could not find a field named `l` in the schema for sorting.", res_op.error().c_str());

    collectionManager.drop_collection("coll1");
}