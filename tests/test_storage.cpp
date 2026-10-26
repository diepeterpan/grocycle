#include "db/storage_engine.hpp"
#include "common/utils.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void test_storage() {
    std::string test_db = "/tmp/test_grocycle.db";
    fs::remove(test_db);

    grocycle::db::StorageEngine storage(test_db);
    assert(storage.initialize());

    // Test roots
    int64_t r1 = storage.get_or_create_root("/home");
    assert(r1 > 0);
    int64_t r2 = storage.get_or_create_root("/home");
    assert(r1 == r2);

    auto roots = storage.get_monitored_roots();
    assert(roots.size() == 1);
    assert(roots[0].path == "/home");

    // Test paths
    int64_t p1 = storage.get_or_create_path("/home", 0, "home");
    int64_t p2 = storage.get_or_create_path("/home/user", p1, "user");
    int64_t p3 = storage.get_or_create_path("/home/user/downloads", p2, "downloads");

    assert(p1 > 0 && p2 > 0 && p3 > 0);

    auto p_rec = storage.get_path(p2);
    assert(p_rec.has_value());
    assert(p_rec->parent_id == p1);
    assert(p_rec->name == "user");

    // Test snapshots
    int64_t s1 = storage.create_snapshot(r1);
    assert(s1 > 0);
    assert(storage.finish_snapshot(s1, 1000, 950, 10));

    auto snap = storage.get_latest_snapshot(r1);
    assert(snap.has_value());
    assert(snap->id == s1);
    assert(snap->total_allocated_bytes == 1000);

    std::cout << "[PASS] test_storage passed successfully." << std::endl;
    fs::remove(test_db);
}

int main() {
    test_storage();
    return 0;
}
