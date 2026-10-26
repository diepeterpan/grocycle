#include "db/storage_engine.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void test_growth_deltas() {
    std::string test_db = "/tmp/test_growth.db";
    fs::remove(test_db);

    grocycle::db::StorageEngine storage(test_db);
    assert(storage.initialize());

    int64_t root_id = storage.get_or_create_root("/data");
    int64_t p_root = storage.get_or_create_path("/data", 0, "data");
    int64_t p_sub1 = storage.get_or_create_path("/data/sub1", p_root, "sub1");
    int64_t p_sub2 = storage.get_or_create_path("/data/sub2", p_root, "sub2");

    // Snapshot 1: sub1 has 10MB, sub2 has 5MB
    int64_t s1 = storage.create_snapshot(root_id);
    std::vector<grocycle::db::DirMetric> m1 = {
        {s1, p_root, 15 * 1024 * 1024, 15 * 1024 * 1024, 0, 2},
        {s1, p_sub1, 10 * 1024 * 1024, 10 * 1024 * 1024, 5, 0},
        {s1, p_sub2, 5 * 1024 * 1024, 5 * 1024 * 1024, 2, 0}
    };
    assert(storage.insert_metrics_batch(m1));
    assert(storage.finish_snapshot(s1, 15 * 1024 * 1024, 15 * 1024 * 1024, 7));

    // Snapshot 2: sub1 grew by 20MB (now 30MB), sub2 grew by 2MB (now 7MB)
    int64_t s2 = storage.create_snapshot(root_id);
    std::vector<grocycle::db::DirMetric> m2 = {
        {s2, p_root, 37 * 1024 * 1024, 37 * 1024 * 1024, 0, 2},
        {s2, p_sub1, 30 * 1024 * 1024, 30 * 1024 * 1024, 15, 0},
        {s2, p_sub2, 7 * 1024 * 1024, 7 * 1024 * 1024, 4, 0}
    };
    assert(storage.insert_metrics_batch(m2));
    assert(storage.finish_snapshot(s2, 37 * 1024 * 1024, 37 * 1024 * 1024, 19));

    // Query fastest growing
    auto growth = storage.get_fastest_growing(root_id, s1, s2, p_root, true, 10);
    assert(growth.size() == 2);
    // sub1 should be first with 20MB delta
    assert(growth[0].path_id == p_sub1);
    assert(growth[0].delta_allocated == 20 * 1024 * 1024);
    assert(growth[1].path_id == p_sub2);
    assert(growth[1].delta_allocated == 2 * 1024 * 1024);

    std::cout << "[PASS] test_growth_deltas passed successfully." << std::endl;
    fs::remove(test_db);
}

int main() {
    test_growth_deltas();
    return 0;
}
