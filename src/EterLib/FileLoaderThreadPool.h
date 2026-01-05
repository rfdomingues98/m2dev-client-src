#ifndef __INC_ETERLIB_FILELOADERTHREADPOOL_H__
#define __INC_ETERLIB_FILELOADERTHREADPOOL_H__

#include <vector>
#include <thread>
#include <atomic>
#include "SPSCQueue.h"
#include "PackLib/PackManager.h"
#include "DecodedImageData.h"

class CFileLoaderThreadPool
{
public:
	struct TLoadRequest
	{
		std::string stFileName;
		uint32_t requestID;
		bool decodeImage;
	};

	struct TLoadResult
	{
		std::string stFileName;
		TPackFile File;
		uint32_t requestID;
		TDecodedImageData decodedImage;
		bool hasDecodedImage;
	};

public:
	CFileLoaderThreadPool();
	~CFileLoaderThreadPool();

	bool Initialize(unsigned int threadCount = 0);
	void Shutdown();
	bool Request(const std::string& fileName);
	bool Fetch(TLoadResult& result);
	size_t GetPendingCount() const;
	bool IsIdle() const;

private:
	struct TWorkerThread
	{
		std::thread thread;
		SPSCQueue<TLoadRequest>* pRequestQueue;
		std::atomic<bool> bBusy;

		TWorkerThread() : pRequestQueue(nullptr), bBusy(false) {}

		TWorkerThread(TWorkerThread&& other) noexcept
			: thread(std::move(other.thread))
			, pRequestQueue(other.pRequestQueue)
			, bBusy(other.bBusy.load())
		{
			other.pRequestQueue = nullptr;
		}

		TWorkerThread& operator=(TWorkerThread&& other) noexcept
		{
			if (this != &other)
			{
				thread = std::move(other.thread);
				pRequestQueue = other.pRequestQueue;
				bBusy.store(other.bBusy.load());
				other.pRequestQueue = nullptr;
			}
			return *this;
		}

		TWorkerThread(const TWorkerThread&) = delete;
		TWorkerThread& operator=(const TWorkerThread&) = delete;
	};

	void WorkerThreadFunction(unsigned int workerIndex);
	unsigned int SelectLeastBusyWorker() const;

private:
	std::vector<TWorkerThread> m_workers;
	SPSCQueue<TLoadResult>* m_pCompletedQueue;

	std::atomic<bool> m_bShutdown;
	std::atomic<uint32_t> m_nextRequestID;
	std::atomic<int> m_activeTasks; // Fast IsIdle check
	unsigned int m_threadCount;

	static const size_t REQUEST_QUEUE_SIZE = 16384;  // Doubled from 8192
	static const size_t COMPLETED_QUEUE_SIZE = 32768; // Doubled from 16384
};

#endif // __INC_ETERLIB_FILELOADERTHREADPOOL_H__
