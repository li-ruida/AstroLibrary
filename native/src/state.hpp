#pragma once
#include "core.hpp"
namespace astro {
struct Cleanup {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    J status;
    J plan;
    double plannedAt = 0;
    Cleanup();
    J snapshot();
    void update(const J &values);
};
class State {
    std::mutex threadsMutex, libraryMutex, cacheJobMutex;
    std::vector<std::thread> workers;
    J libraryCache, librarySummary;
    std::string libraryKey;
    double libraryAt = 0;
    J scan;
    J cacheJob;
    std::string pendingCache;
    std::atomic<bool> cancelCache{false};
    void cacheWorker(std::string operation, fs::path root);
    J scanLibrary(const J &config, const std::shared_ptr<FitsCache> &fits);
    J libraryView(bool refresh, bool full, const std::string &subTarget);
    void decorate(J &payload, const J &config, const J &metadata);
    void cleanupWorker(Cleanup &job, std::string kind, J context, bool execute);
    void r2Worker(J config, uint64_t generation, J roots);

  public:
    std::recursive_mutex mutex;
    fs::path dataDir, webRoot, defaultCache;
    J settings, catalog, mapping, candidates, r2Status, importStatus;
    std::shared_ptr<FitsCache> cache;
    Cleanup targetCleanup, seestarCleanup, jpegCleanup;
    std::atomic<bool> busy{false}, stopping{false};
    uint64_t r2Generation = 0;
    bool r2Queued = false;
    std::string appToken, csrf = randomToken();
    std::map<std::string, double> sessions;
    std::map<std::string, std::pair<int, double>> loginAttempts;
    State(fs::path data, fs::path web, J overrides);
    ~State();
    void spawn(std::function<void()> work);
    J config();
    void invalidate();
    J settingsPayload();
    void updateSettings(const J &payload);
    void setAuth(const J &payload);
    bool nativeToken(const std::string &token);
    bool authenticated(const std::string &session);
    std::string login(const J &payload, const std::string &address);
    J library(bool refresh = false, bool full = false);
    J subCollection(const std::string &target);
    J rating(const J &payload);
    J workflow(const J &payload);
    J tags(const J &payload, bool remove);
    J cacheStatus();
    void cacheAction(const std::string &operation);
    void warmLibrary();
    void cacheDirectory(const J &directory);
    void startImport();
    J exportFiles(const J &payload);
    J cleanupAction(const std::string &kind, const std::string &operation, const J &payload);
    bool syncR2(bool queue = false);
    J r2StatusPayload();
};
int runServer(State &state, const std::string &host, int port);
} // namespace astro
