#include "codegen.h"
#include "run_process.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <vector>

namespace dw5xl::setup {
namespace fs = std::filesystem;

namespace {
std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string utf8(const fs::path &path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// TOML basic string with forward slashes (valid on Windows too, and no backslash escapes needed).
std::string tomlPath(std::string text)
{
    std::string out = "\"";
    for (char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

bool sameContent(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec)
        return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> ba(1u << 16), bb(1u << 16);
    while (fa && fb)
    {
        fa.read(ba.data(), std::streamsize(ba.size()));
        fb.read(bb.data(), std::streamsize(bb.size()));
        if (fa.gcount() != fb.gcount() || !std::equal(ba.begin(), ba.begin() + fa.gcount(), bb.begin()))
            return false;
    }
    return fa.eof() && fb.eof();
}
} // namespace

std::string rewriteRecompConfig(const std::string &config, const fs::path &input, const fs::path &ghidraCsv,
                                const fs::path &output)
{
    std::istringstream lines(config);
    std::string line, out;
    while (std::getline(lines, line))
    {
        const auto key = line.substr(0, line.find('='));
        const auto trimmed = key.substr(0, key.find_last_not_of(" \t") + 1);
        if (line.find('=') != std::string::npos && trimmed == "input")
            line = "input = " + tomlPath(utf8(input));
        else if (line.find('=') != std::string::npos && trimmed == "ghidra_output")
            line = "ghidra_output = " + tomlPath(utf8(ghidraCsv));
        else if (line.find('=') != std::string::npos && trimmed == "output")
        {
            std::string dir = utf8(output);
            if (dir.empty() || dir.back() != '/')
                dir += '/'; // the recompiler appends file names to it, like regen.sh's "$tmp/output/"
            line = "output = " + tomlPath(dir);
        }
        out += line + "\n";
    }
    return out;
}

bool syncDirectory(const fs::path &source, const fs::path &target, std::string &error)
{
    std::error_code ec;
    // First build: nothing to keep, so move the whole folder in one step instead of copying tens of thousands of
    // files one by one (slow on Windows, where every new file is also scanned by the virus scanner).
    if (!fs::exists(target, ec))
    {
        fs::create_directories(target.parent_path(), ec);
        fs::rename(source, target, ec);
        if (!ec)
            return true;
        ec.clear(); // e.g. another drive: copy below
    }
    fs::create_directories(target, ec);
    std::set<fs::path> generated;
    for (const auto &entry : fs::recursive_directory_iterator(source, ec))
    {
        if (!entry.is_regular_file())
            continue;
        const fs::path rel = fs::relative(entry.path(), source);
        generated.insert(rel);
        const fs::path dst = target / rel;
        if (fs::exists(dst) && sameContent(entry.path(), dst))
            continue; // unchanged: keep the timestamp so the build does not recompile it
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "cannot write " + dst.string() + ": " + ec.message();
            return false;
        }
    }
    if (ec)
    {
        error = "cannot read " + source.string() + ": " + ec.message();
        return false;
    }
    std::vector<fs::path> stale;
    for (const auto &entry : fs::recursive_directory_iterator(target, ec))
        if (entry.is_regular_file() && !generated.count(fs::relative(entry.path(), target)))
            stale.push_back(entry.path());
    for (const auto &path : stale)
        fs::remove(path, ec);
    return true;
}

bool generateCode(const fs::path &root, const fs::path &recompiler, const fs::path &logDir,
                  const std::function<bool(const char *, int, int)> &progress, std::string &error,
                  const fs::path &dataDir)
{
    const fs::path data = dataDir.empty() ? root : dataDir;
    std::error_code ec;
    fs::create_directories(logDir, ec);
    const fs::path exe = fs::absolute(recompiler); // the recompiler runs with `root` as its working folder
    const int count = int(std::size(kCodegenProfiles));
    for (int i = 0; i < count; ++i)
    {
        const auto &profile = kCodegenProfiles[i];
        if (progress && !progress(profile.name, i, count))
        {
            error = "cancelled";
            return false;
        }
        const fs::path work = data / "build-setup" / "codegen" / profile.name;
        fs::remove_all(work, ec);
        fs::create_directories(work / "output", ec);
        const std::string config = readText(root / profile.config);
        if (config.empty())
        {
            error = std::string("missing recompiler configuration ") + profile.config;
            return false;
        }
        std::ofstream(work / "config.toml", std::ios::binary)
            << rewriteRecompConfig(config, data / profile.input, root / profile.ghidraCsv, work / "output");
        const fs::path log = logDir / (std::string(profile.name) + ".log");
        fs::remove(log, ec);
        const ProcessResult run = runProcess(exe, {utf8(work / "config.toml")}, root, log);
        if (!run.started)
        {
            error = run.error;
            return false;
        }
        if (run.exitCode != 0)
        {
            error = std::string("the recompiler failed on ") + profile.name + " (exit " +
                    std::to_string(run.exitCode) + "); see " + log.string();
            return false;
        }
        if (!syncDirectory(work / "output", data / profile.output, error))
            return false;
        fs::remove_all(work, ec);
    }
    return true;
}

} // namespace dw5xl::setup
