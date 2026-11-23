// library.h - finding WAV files on the MPC's drives and loading them off the audio thread.
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Lightlike
//
// Scanner: one background thread walks the sample roots (Sculpt's own Content/Samples, then every
//   drive under /media on the MPC: internal drive, SD card, USB sticks) and lists every folder that
//   directly contains .wav files. The finished list is published once through an atomic pointer
//   and never changes afterwards, so any thread may read it without a lock.
// Loader: a second background thread turns load requests into decoded mono samples and hands each
//   one to the audio thread through an atomic pointer. The audio thread only swaps pointers; old
//   samples go back to the loader to be freed. Nothing here allocates or locks on the audio thread.

#ifndef SCULPT_LIBRARY_H
#define SCULPT_LIBRARY_H

#include "mpc_plugin.h"
#include "dsp/wav.h"
#include "engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
  #include <dirent.h>
  #include <sys/stat.h>
#endif

namespace sculpt {

constexpr int kMaxFolders = 4000;
constexpr int kMaxFilesTotal = 40000;
constexpr int kMaxDepth = 10;
constexpr double kMaxSeconds = 30.0;              // longer files are cut here (memory on 32-bit MPCs)
constexpr long long kMaxFileBytes = 160ll << 20;  // bigger files are not read at all

struct Folder
{
    std::string name;                    // shown on screen, e.g. "Internal / Samples / Breaks"
    std::string path;                    // absolute
    std::vector<std::string> files;      // file names, sorted
};

struct Library
{
    std::vector<Folder> folders;
};

inline std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

inline bool isWavName(const std::string& n)
{
    if (n.empty() || n[0] == '.') return false;   // hidden files and macOS "._" companions
    const std::string l = lower(n);
    return l.size() > 4 && l.compare(l.size() - 4, 4, ".wav") == 0;
}

// Names in order, ignoring case, with numbers compared as numbers ("2 Tape" before "10 Club").
inline bool lessNoCase(const std::string& a, const std::string& b)
{
    const std::string x = lower(a), y = lower(b);
    size_t i = 0, j = 0;
    while (i < x.size() && j < y.size())
    {
        if (std::isdigit((unsigned char)x[i]) && std::isdigit((unsigned char)y[j]))
        {
            size_t i2 = i, j2 = j;
            while (i2 < x.size() && std::isdigit((unsigned char)x[i2])) ++i2;
            while (j2 < y.size() && std::isdigit((unsigned char)y[j2])) ++j2;
            std::string nx = x.substr(i, i2 - i), ny = y.substr(j, j2 - j);
            nx.erase(0, std::min(nx.find_first_not_of('0'), nx.size() - 1));
            ny.erase(0, std::min(ny.find_first_not_of('0'), ny.size() - 1));
            if (nx.size() != ny.size()) return nx.size() < ny.size();
            if (nx != ny) return nx < ny;
            i = i2; j = j2;
            continue;
        }
        if (x[i] != y[j]) return x[i] < y[j];
        ++i; ++j;
    }
    return x.size() - i < y.size() - j;
}

// ---------------------------------------------------------------------------------------- Scanner
class Scanner
{
public:
    ~Scanner() { stop(); }

    void start()
    {
        if (thread_.joinable()) return;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }

    void stop()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        delete lib_.exchange(nullptr);
    }

    const Library* library() const { return lib_.load(std::memory_order_acquire); }

private:
    struct Root { std::string path, label; };

    static std::vector<Root> roots()
    {
        std::vector<Root> r;
        r.push_back({mpc::contentPath("Samples"), "Sculpt"});
        if (const char* env = std::getenv("SCULPT_SAMPLE_ROOTS"))   // for testing on a computer
        {
            std::string s = env; size_t p = 0;
            while (p <= s.size())
            {
                size_t q = s.find(':', p); if (q == std::string::npos) q = s.size();
                std::string path = s.substr(p, q - p);
                if (!path.empty()) r.push_back({path, path.substr(path.find_last_of('/') + 1)});
                p = q + 1;
            }
        }
#if defined(__linux__)
        // The MPC mounts every drive under /media: acvs-content is the internal drive.
        if (DIR* d = opendir("/media"))
        {
            std::vector<Root> drives;
            while (dirent* e = readdir(d))
            {
                std::string n = e->d_name;
                if (n.empty() || n[0] == '.') continue;
                std::string label = n == "acvs-content" ? "Internal" : (n == "az01-internal" ? "System" : n);
                drives.push_back({"/media/" + n, label});
            }
            closedir(d);
            std::sort(drives.begin(), drives.end(), [](const Root& a, const Root& b)
                      { return (a.label == "Internal") != (b.label == "Internal") ? a.label == "Internal" : lessNoCase(a.label, b.label); });
            r.insert(r.end(), drives.begin(), drives.end());
        }
#endif
        return r;
    }

    void run()
    {
        Library* lib = new Library();
        files_ = 0;
#if !defined(_WIN32)
        for (const Root& root : roots())
        {
            if (stop_) break;
            walk(*lib, root.path, root.label, 0);
        }
#endif
        // Sculpt's own loops first, then the drives in order, each sorted by name.
        std::stable_sort(lib->folders.begin(), lib->folders.end(), [](const Folder& a, const Folder& b)
        {
            const bool sa = a.name.compare(0, 6, "Sculpt") == 0, sb = b.name.compare(0, 6, "Sculpt") == 0;
            if (sa != sb) return sa;
            return lessNoCase(a.name, b.name);
        });
        lib_.store(lib, std::memory_order_release);
    }

#if !defined(_WIN32)
    void walk(Library& lib, const std::string& path, const std::string& name, int depth)
    {
        if (stop_ || depth > kMaxDepth || (int)lib.folders.size() >= kMaxFolders || files_ >= kMaxFilesTotal) return;
        DIR* d = opendir(path.c_str());
        if (!d) return;
        std::vector<std::string> files, dirs;
        while (dirent* e = readdir(d))
        {
            if (stop_) break;
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || n == "lost+found" || n == "VST2 Plugins" || n == "System Volume Information"
                || n == "$RECYCLE.BIN" || n == "Autosave") continue;
            const std::string full = path + "/" + n;
            struct stat st;
            if (lstat(full.c_str(), &st) != 0) continue;            // lstat: never follow symlinked folders
            if (S_ISDIR(st.st_mode)) dirs.push_back(n);
            else if (S_ISREG(st.st_mode) && isWavName(n)) files.push_back(n);
        }
        closedir(d);
        if (!files.empty())
        {
            std::sort(files.begin(), files.end(), lessNoCase);
            files_ += (int)files.size();
            lib.folders.push_back({name, path, std::move(files)});
        }
        std::sort(dirs.begin(), dirs.end(), lessNoCase);
        for (const std::string& n : dirs) walk(lib, path + "/" + n, name + " / " + n, depth + 1);
    }
#endif

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<Library*> lib_{nullptr};
    int files_ = 0;
};

// ----------------------------------------------------------------------------------------- Loader
// One slot per track. Ownership of a Sample moves: loader -> pending -> audio (active) -> retired -> loader.
class Loader
{
public:
    static constexpr int kTracks = 4;
    static constexpr int kRetire = 8;

    ~Loader() { stop(); }

    void start(const Scanner* scanner)
    {
        if (thread_.joinable()) return;
        scanner_ = scanner;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }

    void stop()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        for (Slot& s : slots_)
        {
            delete s.pending.exchange(nullptr);
            for (auto& r : s.retired) delete r.exchange(nullptr);
        }
    }

    // ---- audio thread ----
    // Load file `file` of folder `folder` (indices into the scanner's library).
    void requestIndex(int track, int folder, int file)
    {
        slots_[track].request.store((folder << 16) | (file & 0xFFFF), std::memory_order_release);
    }
    // Go back to the built-in loop.
    void requestClear(int track) { slots_[track].clear.store(true, std::memory_order_release); }
    // Take a newly loaded sample, if any. Returns true when `active` changed.
    bool swap(int track, Sample*& active)
    {
        Slot& s = slots_[track];
        Sample* fresh = s.pending.exchange(nullptr, std::memory_order_acq_rel);
        if (fresh == nullptr) return false;
        retire(s, active);
        active = fresh;
        return true;
    }
    void release(int track, Sample*& active) { retire(slots_[track], active); active = nullptr; }
    bool takeDirty(int track) { return slots_[track].dirty.exchange(false, std::memory_order_acq_rel); }
    bool takeDrop(int track) { return slots_[track].drop.exchange(false, std::memory_order_acq_rel); }

    // ---- any thread (UI) ----
    // Name of the loaded file ("" when the track plays its built-in loop) and the last status.
    std::string name(int track) const { return readText(slots_[track].name); }
    std::string status(int track) const { return readText(slots_[track].status); }

    // ---- UI thread (state save / load) ----
    std::string path(int track)
    {
        std::lock_guard<std::mutex> g(mutex_);
        return slots_[track].path;
    }
    void requestPath(int track, const std::string& p)
    {
        std::lock_guard<std::mutex> g(mutex_);
        Slot& s = slots_[track];
        s.path = p;
        s.pathRequest = p;
        s.hasPathRequest = true;
    }

private:
    struct Text
    {
        char buf[2][112] = {{0}, {0}};
        std::atomic<int> idx{0};
    };
    struct Slot
    {
        std::atomic<Sample*> pending{nullptr};
        std::atomic<Sample*> retired[kRetire];
        std::atomic<int> request{-1};
        std::atomic<bool> clear{false};
        std::atomic<bool> dirty{false};
        std::atomic<bool> drop{false};       // the audio thread should let go of its file
        Text name, status;
        // guarded by mutex_
        std::string path, pathRequest;
        bool hasPathRequest = false;
        Slot() { for (auto& r : retired) r.store(nullptr); }
    };

    static std::string readText(const Text& t) { return std::string(t.buf[t.idx.load(std::memory_order_acquire)]); }
    static void writeText(Text& t, const std::string& s)
    {
        const int next = 1 - t.idx.load(std::memory_order_relaxed);
        mpc::copyString(t.buf[next], sizeof t.buf[next], s.c_str());
        t.idx.store(next, std::memory_order_release);
    }

    static void retire(Slot& s, Sample* old)
    {
        if (old == nullptr) return;
        for (auto& r : s.retired)
        {
            Sample* expected = nullptr;
            if (r.compare_exchange_strong(expected, old, std::memory_order_acq_rel)) return;
        }
        // All retire slots full (only if the loader thread has stalled): keep the memory rather than free it here.
    }

    void run()
    {
        while (!stop_)
        {
            for (int t = 0; t < kTracks && !stop_; ++t) service(t);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    void service(int t)
    {
        Slot& s = slots_[t];
        for (auto& r : s.retired) delete r.exchange(nullptr, std::memory_order_acq_rel);

        if (s.clear.exchange(false, std::memory_order_acq_rel))
        {
            { std::lock_guard<std::mutex> g(mutex_); s.path.clear(); s.hasPathRequest = false; }
            delete s.pending.exchange(nullptr);
            writeText(s.name, "");
            writeText(s.status, "");
            s.dirty.store(true, std::memory_order_release);
        }

        std::string want;
        bool have = false;
        const int req = s.request.exchange(-1, std::memory_order_acq_rel);
        if (req >= 0)
        {
            const Library* lib = scanner_ ? scanner_->library() : nullptr;
            const int fo = req >> 16, fi = req & 0xFFFF;
            if (lib && fo < (int)lib->folders.size() && fi < (int)lib->folders[(size_t)fo].files.size())
            {
                want = lib->folders[(size_t)fo].path + "/" + lib->folders[(size_t)fo].files[(size_t)fi];
                have = true;
                std::lock_guard<std::mutex> g(mutex_);
                s.path = want;
                s.hasPathRequest = false;
            }
        }
        if (!have)
        {
            std::lock_guard<std::mutex> g(mutex_);
            if (s.hasPathRequest) { want = s.pathRequest; s.hasPathRequest = false; have = true; }
        }
        if (!have) return;

        if (want.empty())
        {   // a project with no file on this track
            s.clear.store(true, std::memory_order_release);
            s.drop.store(true, std::memory_order_release);
            return;
        }
        const std::string base = want.substr(want.find_last_of('/') + 1);
        writeText(s.status, "Loading " + base);
        s.dirty.store(true, std::memory_order_release);
        std::string why;
        Sample* smp = load(want, why);
        if (smp == nullptr)
        {
            writeText(s.status, why + ": " + base);
            s.dirty.store(true, std::memory_order_release);
            return;
        }
        delete s.pending.exchange(smp, std::memory_order_acq_rel);   // an unclaimed older one goes
        std::string shown = base;
        if (shown.size() > 4) shown.resize(shown.size() - 4);       // drop ".wav"
        writeText(s.name, shown);
        writeText(s.status, "");
        s.dirty.store(true, std::memory_order_release);
    }

    static Sample* load(const std::string& path, std::string& why)
    {
#if !defined(_WIN32)
        struct stat st;
        if (stat(path.c_str(), &st) != 0) { why = "Missing"; return nullptr; }
        if ((long long)st.st_size > kMaxFileBytes) { why = "Too big"; return nullptr; }
#endif
        dsp::WavData w;
        if (!dsp::loadWav(path, w) || w.numFrames() < 64 || w.numChannels() < 1) { why = "Can't read"; return nullptr; }
        Sample* s = new Sample();
        const size_t nf = std::min(w.numFrames(), (size_t)(kMaxSeconds * w.sampleRate));
        s->data.assign(nf, 0.0f);
        const float g = 1.0f / (float)w.numChannels();
        for (int c = 0; c < w.numChannels(); ++c)
            for (size_t i = 0; i < nf; ++i) s->data[i] += w.channels[(size_t)c][i] * g;
        s->rate = w.sampleRate;
        s->computePeaks();   // on the loader thread: the screen's waveform for this file
        return s;
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
    const Scanner* scanner_ = nullptr;
    std::mutex mutex_;
    Slot slots_[kTracks];
};

} // namespace sculpt

#endif // SCULPT_LIBRARY_H
