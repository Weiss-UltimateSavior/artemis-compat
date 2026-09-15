// save_regressions.cpp — atomic save pipeline + variable-bank codec.
//
// The save path is O_EXCL temp + fsync + rename; the rename is the only commit
// point. These cases prove that invariant under injected failures (crash
// litter, full disk via RLIMIT_FSIZE, read-only directory) without ever
// touching a real game save. All fixtures are generated in a temp dir.

#include "util/save_storage.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

std::string ReadAll(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void WriteRaw(const std::string &path, const std::string &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), (std::streamsize)bytes.size());
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() /
                         ("artc_save_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string target = (dir / "system.dat").string();

    // 1) round trip + SavePath containment
    {
        std::vector<uint8_t> bytes{'A', 'R', 'C', 'V', 1, 2, 3};
        Check(artc::WriteSaveFile(target, bytes), "atomic write succeeds");
        std::vector<uint8_t> back;
        Check(artc::ReadSaveFile(target, back) && back == bytes, "read-back matches");
        Check(artc::SavePath(dir.string(), "system.dat") == target,
              "SavePath joins the directory");
        Check(artc::SavePath(dir.string(), "../escape.dat").empty(),
              "SavePath rejects escapes");
    }

    // 2) crash litter is not a commit: a stale temp file must not affect reads
    {
        WriteRaw(target + ".tmp.999.999", "GARBAGE");
        std::vector<uint8_t> back;
        Check(artc::ReadSaveFile(target, back) && back.size() == 7 && back[0] == 'A',
              "stale temp file leaves the committed save intact");
    }

    // 3) crash mid-write (child killed by RLIMIT_FSIZE while writing): the old
    //    save survives because the rename never happens.
    {
        std::vector<uint8_t> old_bytes{'O', 'L', 'D'};
        Check(artc::WriteSaveFile(target, old_bytes), "seed old save");
#if !defined(_WIN32)
        const pid_t pid = fork();
        Check(pid >= 0, "fork");
        if (pid == 0) {
            // Child: cap the file size below the payload, then attempt a big
            // atomic write. The write must fail; exit code carries the result.
            struct rlimit rl{};
            rl.rlim_cur = 4096;
            rl.rlim_max = 4096;
            setrlimit(RLIMIT_FSIZE, &rl);
            signal(SIGXFSZ, SIG_IGN);
            std::vector<uint8_t> big(256 * 1024, 'X');
            _exit(artc::WriteSaveFile(target, big) ? 0 : 3);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 3,
              "oversized write fails cleanly (no partial commit)");
        std::vector<uint8_t> back;
        Check(artc::ReadSaveFile(target, back) && back == old_bytes,
              "old save intact after failed write");
#endif
    }

    // 4) read-only directory: explicit failure, old save intact
    {
#if !defined(_WIN32)
        if (::geteuid() != 0) {   // root bypasses the permission check
            std::vector<uint8_t> old_bytes{'O', 'L', 'D'};
            Check(artc::WriteSaveFile(target, old_bytes), "seed before chmod");
            fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
            std::vector<uint8_t> big{'N', 'E', 'W'};
            Check(!artc::WriteSaveFile(target, big), "write into read-only dir fails");
            fs::permissions(dir, fs::perms::owner_all);
            std::vector<uint8_t> back;
            Check(artc::ReadSaveFile(target, back) && back == old_bytes,
                  "old save intact after permission failure");
        }
#endif
    }

    // 5) variable bank codec (with and without the ARCV checkpoint header)
    {
        artc::VariableBank vars{{"f.x", "1"}, {"t.name", "ゆき"}, {"s.flag", "on"}};
        for (const bool checkpoint : {false, true}) {
            std::vector<uint8_t> bytes;
            Check(artc::EncodeVariableBank(vars, checkpoint, bytes), "encode bank");
            if (checkpoint)
                Check(bytes.size() > 4 && bytes[0] == 'A' && bytes[1] == 'R',
                      "ARCV checkpoint header present");
            artc::VariableBank back;
            Check(artc::DecodeVariableBank(bytes, checkpoint, back) && back == vars,
                  "bank round trip");
            // Corruption is rejected (checkpoint) rather than silently loaded.
            std::vector<uint8_t> corrupt = bytes;
            corrupt[corrupt.size() / 2] ^= 0xFF;
            artc::VariableBank ignored;
            const bool decoded = artc::DecodeVariableBank(corrupt, checkpoint, ignored);
            if (checkpoint) Check(!decoded, "corrupt checkpoint rejected");
        }
        std::vector<uint8_t> truncated{1, 2};
        artc::VariableBank ignored;
        Check(!artc::DecodeVariableBank(truncated, false, ignored),
              "truncated bank rejected");
    }

    fs::remove_all(dir);
    if (g_failures) {
        std::cerr << g_failures << " save regression failure(s)\n";
        return 1;
    }
    std::cout << "save storage regressions passed\n";
    return 0;
}
