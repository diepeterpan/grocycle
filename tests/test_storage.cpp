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

    // Test metric insertion for p1
    std::vector<grocycle::db::DirMetric> metrics;
    grocycle::db::DirMetric m1;
    m1.snapshot_id = s1;
    m1.path_id = p1;
    m1.allocated_size = 1000;
    m1.apparent_size = 950;
    m1.direct_files = 5;
    m1.total_subdirs = 2;
    metrics.push_back(m1);
    assert(storage.insert_metrics_batch(metrics));

    // At this point, p1 has metric, but p2 and p3 are unreferenced.
    // Purge orphaned paths should delete p2 and p3 (2 paths deleted)
    size_t purged = storage.purge_orphaned_paths();
    assert(purged == 2);
    assert(storage.get_path(p1).has_value());
    assert(!storage.get_path(p2).has_value());
    assert(!storage.get_path(p3).has_value());

    // Test prune_old_snapshots with 0 days retention (prunes snapshot older than now)
    // Note: since s1 was created just now, let's test prune with retention = 0
    size_t pruned = storage.prune_old_snapshots(0);
    assert(pruned >= 1);
    assert(!storage.get_latest_snapshot(r1).has_value());

    // After snapshot s1 is deleted, p1 is now also orphaned
    assert(storage.get_path(p1) == std::nullopt); // prune_old_snapshots runs purge_orphaned_paths internally!

    // Test vacuum method
    assert(storage.vacuum());

    std::cout << "[PASS] test_storage passed successfully." << std::endl;
    fs::remove(test_db);
}

int main() {
    test_storage();
    return 0;
}
