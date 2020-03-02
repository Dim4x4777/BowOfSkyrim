#include "common.h"
#include <tbb/concurrent_queue.h>
#include <atomic>
#include "../TES/BSGraphicsRenderer.h"

//
// Random notes while I was writing this at 3am:
//
// - Command lists are created by using worker threads with a main thread supplying the commands.
// - Command lists can be subdivided (only at a higher level, however).
// - Each job packet has its own DirectX command list instance.
// - Each job packet is marked as free AFTER IT HAS BEEN RENDERED.
// - Each worker thread replaces its TLS renderer pointer with JobCommandData::ThreadGlobals.
// - Semaphores are used for notification (and a counter to number of pending jobs).
// - All arrays are pre-allocated and queues only store the pointers.
//
#define MAXIMUM_WORKER_THREADS 4
#define MAXIMUM_JOBS 32

HANDLE ThreadInitSemaphore;// Counter between 0 and MAXIMUM_WORKER_THREADS
HANDLE JobPendingSemaphore;// Counter between 0 and MAXIMUM_JOBS

tbb::concurrent_queue<struct JobCommandData *> FreeJobSlots;
tbb::concurrent_queue<struct JobCommandData *> PendingJobSlots;
std::atomic<struct JobCommandData *> CompletedJobs[MAXIMUM_JOBS];

ID3D11DeviceContext1 *ImmediateContext;

struct JobCommandData
{
	int Id;
	bool DisableRenderer;

	__int64 a1;
	unsigned int a2;
	void(*Callback)(__int64, unsigned int);

	ID3D11DeviceContext2 *DeferredContext;
	ID3D11CommandList *CommandList;

	union
	{
		BSGraphics::Renderer ThreadGlobals;
		char data[0x4000];
	};

	JobCommandData()
	{
	}

	void Schedule()
	{
		PendingJobSlots.push(this);
		ReleaseSemaphore(JobPendingSemaphore, 1, nullptr);
	}

	void End()
	{
		FreeJobSlots.push(this);
	}
};

DWORD WINAPI DC_Thread(LPVOID Arg)
{
	UNREFERENCED_PARAMETER(Arg);

	XUtil::SetThreadName(GetCurrentThreadId(), "DC_Thread");
	ReleaseSemaphore(ThreadInitSemaphore, 1, nullptr);

	// Loop forever: whichever thread gets a job first executes it. Semaphores are NOT FIFO.
	for (JobCommandData *jobData;;)
	{
		Assert(WaitForSingleObject(JobPendingSemaphore, INFINITE) != WAIT_FAILED);
		AssertMsg(PendingJobSlots.try_pop(jobData), "Expected a job to be in the queue but there are none");

		// Swap our TLS renderer pointers to this job & validate it
		*(uintptr_t *)(__readgsqword(0x58) + g_TlsIndex * sizeof(void *)) = (uintptr_t)&jobData->ThreadGlobals;

		AssertDebug(&jobData->ThreadGlobals == BSGraphics::Renderer::QInstance());
		AssertDebug(&jobData->ThreadGlobals != BSGraphics::Renderer::GetGlobalsNonThreaded());

		// Previous command list should've been released (aka sent to GPU)
		Assert(jobData->CommandList == nullptr);

		if (jobData->DisableRenderer)
		{
			jobData->ThreadGlobals.m_DeviceContext = nullptr;
			jobData->Callback(jobData->a1, jobData->a2);
		}
		else
		{
			jobData->ThreadGlobals.m_StateUpdateFlags = 0xFFFFFFFF & ~0x400;
			jobData->ThreadGlobals.m_DeviceContext = jobData->DeferredContext;
			jobData->Callback(jobData->a1, jobData->a2);

			Assert(SUCCEEDED(jobData->DeferredContext->FinishCommandList(FALSE, &jobData->CommandList)));
		}

		CompletedJobs[jobData->Id].store(jobData);
	}

	return 0;
}

void DC_Init(ID3D11Device2 *Device, int DeferredContextCount)
{
	// I'm declaring it here because it's inside the init function and not globally visible
	static JobCommandData jobCommands[MAXIMUM_JOBS];

	Device->GetImmediateContext1(&ImmediateContext);

	//
	// First create the semaphores, then create each thread. Once all threads are init'd,
	// the ThreadInitSemaphore count will be MAXIMUM_WORKER_THREADS. Worker threads wait
	// on JobPendingSemaphore.
	//
	ThreadInitSemaphore = CreateSemaphore(nullptr, 0, MAXIMUM_WORKER_THREADS, nullptr);
	JobPendingSemaphore = CreateSemaphore(nullptr, 0, MAXIMUM_JOBS, nullptr);

	Assert(ThreadInitSemaphore);
	Assert(JobPendingSemaphore);

	for (int i = 0; i < MAXIMUM_WORKER_THREADS; i++)
		CreateThread(nullptr, 0, DC_Thread, (LPVOID)i, 0, nullptr);

	// Also set up job-specific data (create D3D11 deferred context pool)
	for (int i = 0; i < MAXIMUM_JOBS; i++)
	{
		memset(&jobCommands[i], 0, sizeof(JobCommandData));
		jobCommands[i].Id = i;

		Assert(SUCCEEDED(Device->CreateDeferredContext2(0, &jobCommands[i].DeferredContext)));

		FreeJobSlots.push(&jobCommands[i]);
		CompletedJobs[i].store(nullptr);
	}

	// Now we wait....
	for (int i = 0; i < MAXIMUM_WORKER_THREADS; i++)
		Assert(WaitForSingleObject(ThreadInitSemaphore, INFINITE) != WAIT_FAILED);
}

int DC_RenderDeferred(__int64 a1, unsigned int a2, void(*func)(__int64, unsigned int), bool DisableRenderer)
{
	ID3D11Buffer *psbuf[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
	ID3D11Buffer *vsbuf[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
	ID3D11Buffer *csbuf[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
	ID3D11ShaderResourceView *srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11ShaderResourceView *vsrvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];

	// Find some random free job slot
	JobCommandData *jobData;

	AssertMsg(FreeJobSlots.try_pop(jobData), "No free job slots available in the queue");
	auto dc1 = jobData->DeferredContext;

	if (!DisableRenderer)
	{
		ImmediateContext->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, psbuf);
		ImmediateContext->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, vsbuf);
		ImmediateContext->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, csbuf);

		ImmediateContext->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
		ImmediateContext->VSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, vsrvs);

		dc1->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, psbuf);
		dc1->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, vsbuf);
		dc1->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, csbuf);

		dc1->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
		dc1->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, vsrvs);
	}

	jobData->DisableRenderer = DisableRenderer;
	jobData->a1 = a1;
	jobData->a2 = a2;
	jobData->Callback = func;

	memcpy(&jobData->ThreadGlobals, BSGraphics::Renderer::GetGlobalsNonThreaded(), 0x4000);

	// Run the other thread ASAP
	jobData->Schedule();

	if (!DisableRenderer)
	{
		for (int i = 0; i < 14; i++) if (psbuf[i]) psbuf[i]->Release();
		for (int i = 0; i < 14; i++) if (vsbuf[i]) vsbuf[i]->Release();
		for (int i = 0; i < 14; i++) if (csbuf[i]) csbuf[i]->Release();

		for (int i = 0; i < 128; i++) if (srvs[i]) srvs[i]->Release();
		for (int i = 0; i < 128; i++) if (vsrvs[i]) vsrvs[i]->Release();
	}

	return jobData->Id;
}

void DC_WaitDeferred(int JobHandle)
{
	JobCommandData *job;

	// Atomic spinlock that waits for job completion (based on the pointer value)
	{
		ProfileTimer("Waiting for command list completion");

		do
		{
			job = CompletedJobs[JobHandle].load();
		} while (job == nullptr);

		CompletedJobs[JobHandle].store(nullptr);
	}

	if (!job->DisableRenderer)
	{
		ImmediateContext->ExecuteCommandList(job->CommandList, TRUE);
		job->CommandList->Release();
		job->CommandList = nullptr;
	}

	job->End();
	//memcpy(GetMainGlobals(), g_ThreadData[0].ThreadGlobals, sizeof(BSGraphicsRendererGlobals));
}