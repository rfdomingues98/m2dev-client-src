#include "StdAfx.h"
#include "FileLoaderThreadPool.h"
#include "BufferPool.h"
#include "ImageDecoder.h"
#include "PackLib/PackManager.h"
#include <algorithm>

static const bool USE_STAGED_TEXTURE_LOADING = true;

CFileLoaderThreadPool::CFileLoaderThreadPool()
	: m_pCompletedQueue(nullptr)
	, m_bShutdown(false)
	, m_nextRequestID(0)
	, m_activeTasks(0)
	, m_threadCount(0)
{
}

CFileLoaderThreadPool::~CFileLoaderThreadPool()
{
	Shutdown();
}

bool CFileLoaderThreadPool::Initialize(unsigned int threadCount)
{
	if (!m_workers.empty())
	{
		TraceError("CFileLoaderThreadPool::Initialize: Already initialized");
		return false;
	}

	if (threadCount == 0)
	{
		threadCount = std::thread::hardware_concurrency();
		if (threadCount == 0)
			threadCount = 4;
		else
			threadCount = std::max(4u, threadCount / 2);
	}

	threadCount = std::max(4u, std::min(16u, threadCount));
	m_threadCount = threadCount;

	Tracenf("CFileLoaderThreadPool: Initializing with %u worker threads", threadCount);

	m_pCompletedQueue = new SPSCQueue<TLoadResult>(COMPLETED_QUEUE_SIZE);

	m_workers.reserve(threadCount);
	for (unsigned int i = 0; i < threadCount; ++i)
	{
		TWorkerThread worker;
		worker.pRequestQueue = new SPSCQueue<TLoadRequest>(REQUEST_QUEUE_SIZE);
		worker.bBusy.store(false, std::memory_order_relaxed);

		try
		{
			worker.thread = std::thread(&CFileLoaderThreadPool::WorkerThreadFunction, this, i);
		}
		catch (const std::exception& e)
		{
			TraceError("CFileLoaderThreadPool::Initialize: Failed to create thread %u: %s", i, e.what());
			delete worker.pRequestQueue;
			worker.pRequestQueue = nullptr;
			Shutdown();
			return false;
		}

		m_workers.push_back(std::move(worker));
	}

	return true;
}

void CFileLoaderThreadPool::Shutdown()
{
	if (m_workers.empty())
		return;

	// Signal shutdown
	m_bShutdown.store(true, std::memory_order_release);

	// Wait for all workers to finish
	for (auto& worker : m_workers)
	{
		if (worker.thread.joinable())
			worker.thread.join();

		// Cleanup request queue
		if (worker.pRequestQueue)
		{
			delete worker.pRequestQueue;
			worker.pRequestQueue = nullptr;
		}
	}

	m_workers.clear();

	// Cleanup completed queue
	if (m_pCompletedQueue)
	{
		delete m_pCompletedQueue;
		m_pCompletedQueue = nullptr;
	}

	m_threadCount = 0;
}

bool CFileLoaderThreadPool::Request(const std::string& fileName)
{
	if (m_workers.empty())
	{
		TraceError("CFileLoaderThreadPool::Request: Thread pool not initialized");
		return false;
	}

	TLoadRequest request;
	request.stFileName = fileName;
	request.requestID = m_nextRequestID.fetch_add(1, std::memory_order_relaxed);

	request.decodeImage = false;
	if (USE_STAGED_TEXTURE_LOADING)
	{
		size_t dotPos = fileName.find_last_of('.');
		if (dotPos != std::string::npos && dotPos + 1 < fileName.size())
		{
			const char* ext = fileName.c_str() + dotPos;
			size_t extLen = fileName.size() - dotPos;

			if ((extLen == 4 && (_stricmp(ext, ".dds") == 0 || _stricmp(ext, ".png") == 0 ||
				_stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".tga") == 0 || _stricmp(ext, ".bmp") == 0)) ||
				(extLen == 5 && _stricmp(ext, ".jpeg") == 0))
			{
				request.decodeImage = true;
			}
		}
	}

	unsigned int targetWorker = SelectLeastBusyWorker();

	if (!m_workers[targetWorker].pRequestQueue->Push(request))
	{
		for (unsigned int i = 0; i < m_threadCount; ++i)
		{
			unsigned int workerIdx = (targetWorker + i) % m_threadCount;
			if (m_workers[workerIdx].pRequestQueue->Push(request))
			{
				m_activeTasks.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		}

		TraceError("CFileLoaderThreadPool::Request: All worker queues full for file: %s", fileName.c_str());
		return false;
	}

	m_activeTasks.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool CFileLoaderThreadPool::Fetch(TLoadResult& result)
{
	if (!m_pCompletedQueue)
		return false;

	if (m_pCompletedQueue->Pop(result))
	{
		m_activeTasks.fetch_sub(1, std::memory_order_relaxed);
		return true;
	}
	return false;
}

size_t CFileLoaderThreadPool::GetPendingCount() const
{
	size_t total = 0;
	for (const auto& worker : m_workers)
	{
		if (worker.pRequestQueue)
			total += worker.pRequestQueue->Size();
	}
	return total;
}

bool CFileLoaderThreadPool::IsIdle() const
{
	return m_activeTasks.load(std::memory_order_acquire) == 0;
}

unsigned int CFileLoaderThreadPool::SelectLeastBusyWorker() const
{
	unsigned int leastBusyIdx = 0;
	size_t minSize = m_workers[0].pRequestQueue->Size();

	for (unsigned int i = 1; i < m_threadCount; ++i)
	{
		size_t queueSize = m_workers[i].pRequestQueue->Size();
		if (queueSize < minSize)
		{
			minSize = queueSize;
			leastBusyIdx = i;
		}
	}

	return leastBusyIdx;
}

void CFileLoaderThreadPool::WorkerThreadFunction(unsigned int workerIndex)
{
	TWorkerThread& worker = m_workers[workerIndex];
	SPSCQueue<TLoadRequest>* pRequestQueue = worker.pRequestQueue;

	CBufferPool* pBufferPool = CPackManager::instance().GetBufferPool();

	Tracenf("CFileLoaderThreadPool: Worker thread %u started", workerIndex);

	int idleCount = 0;

	while (!m_bShutdown.load(std::memory_order_acquire))
	{
		TLoadRequest request;

		if (pRequestQueue->Pop(request))
		{
			idleCount = 0;
			worker.bBusy.store(true, std::memory_order_release);

			TLoadResult result;
			result.stFileName = request.stFileName;
			result.requestID = request.requestID;
			result.File.clear();
			result.hasDecodedImage = false;

			CPackManager::instance().GetFileWithPool(request.stFileName, result.File, pBufferPool);

			if (request.decodeImage && !result.File.empty())
			{
				if (CImageDecoder::DecodeImage(result.File.data(), result.File.size(), result.decodedImage))
				{
					result.hasDecodedImage = true;
					result.File.clear();
				}
			}

			while (!m_pCompletedQueue->Push(result))
			{
				std::this_thread::yield();

				if (m_bShutdown.load(std::memory_order_acquire))
					break;
			}

			worker.bBusy.store(false, std::memory_order_release);
		}
		else
		{
			idleCount++;
			if (idleCount > 1000)
			{
				Sleep(1);
				idleCount = 0;
			}
			else if (idleCount > 10)
			{
				std::this_thread::yield();
			}
		}
	}

	Tracenf("CFileLoaderThreadPool: Worker thread %u stopped", workerIndex);
}
