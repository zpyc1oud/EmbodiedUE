#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/UniquePtr.h"

/** Byte-only transport seam. Protocol state and Frame semantics stay above it. */
class IUERLTransport
{
public:
	virtual ~IUERLTransport() = default;
	virtual bool Listen(int32 Port, FString& OutError) = 0;
	virtual bool Accept(uint32 TimeoutMs, const FThreadSafeBool& StopRequested, FString& OutError) = 0;
	virtual bool ReadExact(uint8* Buffer, int32 NumBytes, double DeadlineSeconds,
		const FThreadSafeBool& StopRequested) = 0;
	virtual bool WriteExact(const uint8* Buffer, int32 NumBytes, double DeadlineSeconds,
		const FThreadSafeBool& StopRequested) = 0;
	virtual void WaitForPeerClose(uint32 TimeoutMs, const FThreadSafeBool& StopRequested) = 0;
	virtual void Interrupt() = 0;
	virtual void Close() = 0;
};

TUniquePtr<IUERLTransport> MakeUERLSocketTransport();

/** Deterministic adapter used by protocol unit tests; no socket or Worker dependency. */
class FUERLInMemoryTransport final : public IUERLTransport
{
public:
	virtual bool Listen(int32 Port, FString& OutError) override;
	virtual bool Accept(uint32 TimeoutMs, const FThreadSafeBool& StopRequested, FString& OutError) override;
	virtual bool ReadExact(uint8* Buffer, int32 NumBytes, double DeadlineSeconds,
		const FThreadSafeBool& StopRequested) override;
	virtual bool WriteExact(const uint8* Buffer, int32 NumBytes, double DeadlineSeconds,
		const FThreadSafeBool& StopRequested) override;
	virtual void WaitForPeerClose(uint32 TimeoutMs, const FThreadSafeBool& StopRequested) override {}
	virtual void Interrupt() override { bInterrupted = true; }
	virtual void Close() override;

	void Feed(const TArray<uint8>& Bytes) { Incoming.Append(Bytes); }
	TArray<uint8> DrainWritten();

private:
	TArray<uint8> Incoming;
	TArray<uint8> Outgoing;
	int32 ReadOffset = 0;
	bool bInterrupted = false;
};
