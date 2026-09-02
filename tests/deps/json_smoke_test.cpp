// nlohmann/json smoke test. This TU must not include RTNeural; see cmake/deps.cmake.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

TEST_CASE("nlohmann/json parses and serialises", "[deps][json]") {
    const auto doc = nlohmann::json::parse(R"({"tempo": 128.5, "meter": [4, 4], "name": "takt4"})");

    CHECK(doc.at("tempo").get<double>() == 128.5);
    CHECK(doc.at("meter").size() == 2);
    CHECK(doc.at("meter")[0].get<int>() == 4);
    CHECK(doc.at("name").get<std::string>() == "takt4");

    const std::string round_trip = nlohmann::json::parse(doc.dump()).dump();
    CHECK(round_trip == doc.dump());
}

TEST_CASE("nlohmann/json reports malformed input", "[deps][json]") {
    CHECK_THROWS_AS(nlohmann::json::parse("{\"tempo\": }"), nlohmann::json::parse_error);
}
