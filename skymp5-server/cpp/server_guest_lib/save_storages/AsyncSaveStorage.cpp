#include "AsyncSaveStorage.h"
#include <chrono>
#include <list>
#include <thread>

struct AsyncSaveStorage::Impl
{
  struct UpsertTask
  {
    std::vector<std::optional<MpChangeForm>> changeForms;
    std::function<void()> callback;
  };

  std::shared_ptr<spdlog::logger> logger;
  std::string name;

  struct
  {
    std::shared_ptr<IDatabase> dbImpl;
    std::mutex m;
  } share;

  struct
  {
    std::list<std::exception_ptr> exceptions;
    std::mutex m;
  } share2;

  struct
  {
    std::vector<UpsertTask> upsertTasks;
    std::mutex m;
  } share3;

  struct
  {
    std::vector<std::function<void()>> upsertCallbacksToFire;
    std::list<std::vector<std::optional<MpChangeForm>>>
      recycledChangeFormsBuffers;
    std::mutex m;
  } share4;

  std::unique_ptr<std::thread> thr;
  std::atomic<bool> destroyed = false;
  uint32_t numFinishedUpserts = 0;
};

AsyncSaveStorage::AsyncSaveStorage(const std::shared_ptr<IDatabase>& dbImpl,
                                   std::shared_ptr<spdlog::logger> logger,
                                   std::string name)
  : pImpl(new Impl, [](Impl* p) { delete p; })
{
  pImpl->name = std::move(name);
  pImpl->logger = logger;
  pImpl->share.dbImpl = dbImpl;

  auto p = this->pImpl.get();
  pImpl->thr.reset(new std::thread([p] { SaverThreadMain(p); }));
}

AsyncSaveStorage::~AsyncSaveStorage()
{
  pImpl->destroyed = true;
  pImpl->thr->join();
}

void AsyncSaveStorage::SaverThreadMain(Impl* pImpl)
{
  while (!pImpl->destroyed) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    try {

      decltype(pImpl->share3.upsertTasks) tasks;
      {
        std::lock_guard l(pImpl->share3.m);
        tasks = std::move(pImpl->share3.upsertTasks);
        pImpl->share3.upsertTasks.clear();
      }

      std::vector<std::function<void()>> callbacksToFire;
      std::vector<std::vector<std::optional<MpChangeForm>>>
        recycledChangeFormsBuffers;

      {
        std::lock_guard l(pImpl->share.m);
        auto start = std::chrono::high_resolution_clock::now();
        size_t numChangeForms = 0;
        for (auto& t : tasks) {
          numChangeForms +=
            pImpl->share.dbImpl->Upsert(std::move(t.changeForms));
          t.changeForms.clear();
          callbacksToFire.push_back(t.callback);

          std::vector<std::optional<MpChangeForm>> tmp;
          if (pImpl->share.dbImpl->GetRecycledChangeFormsBuffer(tmp)) {
            recycledChangeFormsBuffers.push_back(std::move(tmp));
          }
        }
        if (numChangeForms > 0 && pImpl->logger) {
          auto end = std::chrono::high_resolution_clock::now();
          std::chrono::duration<double, std::milli> elapsed = end - start;
          pImpl->logger->trace("Saved {} ChangeForms in {} ms", numChangeForms,
                               elapsed.count());
        }
      }

      {
        std::lock_guard l(pImpl->share4.m);
        for (auto& cb : callbacksToFire) {
          pImpl->share4.upsertCallbacksToFire.push_back(cb);
        }
        for (auto& buf : recycledChangeFormsBuffers) {
          pImpl->share4.recycledChangeFormsBuffers.push_back(std::move(buf));
        }
      }
    } catch (...) {
      std::lock_guard l(pImpl->share2.m);
      auto exceptionPtr = std::current_exception();
      pImpl->share2.exceptions.push_back(exceptionPtr);
    }
  }
}

void AsyncSaveStorage::IterateSync(const IterateSyncCallback& cb)
{
  std::lock_guard l(pImpl->share.m);
  pImpl->share.dbImpl->Iterate(cb);
}

void AsyncSaveStorage::Upsert(
  std::vector<std::optional<MpChangeForm>>&& changeForms,
  const UpsertCallback& cb)
{
  std::lock_guard l(pImpl->share3.m);
  pImpl->share3.upsertTasks.push_back(AsyncSaveStorage::Impl::UpsertTask());
  pImpl->share3.upsertTasks.back().changeForms = std::move(changeForms);
  pImpl->share3.upsertTasks.back().callback = cb;
}

uint32_t AsyncSaveStorage::GetNumFinishedUpserts() const
{
  return pImpl->numFinishedUpserts;
}

void AsyncSaveStorage::Tick()
{
  {
    std::lock_guard l(pImpl->share2.m);
    if (!pImpl->share2.exceptions.empty()) {
      auto exceptionPtr = std::move(pImpl->share2.exceptions.front());
      pImpl->share2.exceptions.pop_front();
      std::rethrow_exception(exceptionPtr);
    }
  }

  decltype(pImpl->share4.upsertCallbacksToFire) upsertCallbacksToFire;
  {
    std::lock_guard l(pImpl->share4.m);
    upsertCallbacksToFire = std::move(pImpl->share4.upsertCallbacksToFire);
    pImpl->share4.upsertCallbacksToFire.clear();
  }
  for (auto& cb : upsertCallbacksToFire) {
    pImpl->numFinishedUpserts++;
    cb();
  }
}

bool AsyncSaveStorage::GetRecycledChangeFormsBuffer(
  std::vector<std::optional<MpChangeForm>>& changeForms)
{
  std::lock_guard l(pImpl->share4.m);
  if (pImpl->share4.recycledChangeFormsBuffers.empty()) {
    return false;
  }

  changeForms = std::move(pImpl->share4.recycledChangeFormsBuffers.front());
  pImpl->share4.recycledChangeFormsBuffers.pop_front();
  return true;
}

const std::string& AsyncSaveStorage::GetName() const
{
  return pImpl->name;
}
