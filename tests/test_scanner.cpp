#include "scanner/fs_scanner.hpp"
#include "db/storage_engine.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

void test_scanner() {
    std::string test_db = "/tmp/test_scanner.db";
    std::string test_dir = "/tmp/test_grocycle_fs";

    fs::remove(test_db);
    fs::remove_all(test_dir);

    // Create test directory tree
    fs::create_directories(test_dir + "/dirA/sub1");
    fs::create_directories(test_dir + "/dirB");

    // Write some dummy files
    std::ofstream(test_dir + "/file1.txt") << std::string(1024, 'a');
    std::ofstream(test_dir + "/dirA/file2.txt") << std::string(2048, 'b');
    std::ofstream(test_dir + "/dirA/sub1/file3.txt") << std::string(4096, 'c');
    std::ofstream(test_dir + "/dirB/file4.txt") << std::string(8192, 'd');

    grocycle::db::StorageEngine storage(test_db);
    assert(storage.initialize());

    int64_t r_id = storage.get_or_create_root(test_dir);
    assert(r_id > 0);

    grocycle::scanner::FsScanner scanner(storage, 100, 1, {});
    std::atomic<bool> cancel{false};
    bool ok = scanner.scan_root(r_id, test_dir, cancel);
    assert(ok);

    auto latest_snap = storage.get_latest_snapshot(r_id);
    assert(latest_snap.has_value());
    assert(latest_snap->status == "COMPLETED");
    assert(latest_snap->total_apparent_bytes >= (1024 + 2048 + 4096 + 8192));
    assert(latest_snap->total_allocated_bytes > 0);

    std::cout << "[PASS] test_scanner passed successfully." << std::endl;

    fs::remove_all(test_dir);
    fs::remove(test_db);
}

int main() {
    test_scanner();
    return 0;
}
