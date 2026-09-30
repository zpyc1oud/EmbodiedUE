#include "UERLTransportAdapter.h"

#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	class FUERLSocketBridgeTransport final : public IUERLTransport
	{
	public:
		virtual ~FUERLSocketBridgeTransport() override { Close(); }

		virtual bool Listen(int32 Port, FString& OutError) override
		{
			Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (!Subsystem) { OutError = TEXT("socket subsystem unavailable"); return false; }
			TSharedRef<FInternetAddr> Address = Subsystem->CreateInternetAddr();
			bool bIpValid = false;
			Address->SetIp(TEXT("127.0.0.1"), bIpValid);
			Address->SetPort(Port);
			if (!bIpValid) { OutError = TEXT("invalid loopback address"); return false; }
			Listener = Subsystem->CreateSocket(NAME_Stream, TEXT("uerl-socket-bridge-listener"), false);
			if (!Listener) { OutError = TEXT("cannot create listener socket"); return false; }
			Listener->SetReuseAddr(true);
			if (!Listener->Bind(*Address) || !Listener->Listen(1))
			{
				OutError = FString::Printf(TEXT("SocketBridge bind/listen failed on port %d"), Port);
				return false;
			}
			return true;
		}

		virtual bool Accept(uint32 TimeoutMs, const FThreadSafeBool& StopRequested, FString& OutError) override
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutMs / 1000.0;
			while (!StopRequested && FPlatformTime::Seconds() < Deadline)
			{
				bool bPending = false;
				if (Listener && Listener->WaitForPendingConnection(bPending, FTimespan::FromMilliseconds(50)) && bPending)
				{
					FScopeLock Lock(&Mutex);
					Connection = Listener->Accept(TEXT("uerl-socket-bridge-session"));
					if (Connection)
					{
						Connection->SetNonBlocking(false);
						Connection->SetNoDelay(true);
						return true;
					}
				}
			}
			OutError = StopRequested ? TEXT("transport stopped") : TEXT("accept deadline expired");
			return false;
		}

		virtual bool ReadExact(uint8* Buffer, int32 NumBytes, double DeadlineSeconds,
			const FThreadSafeBool& StopRequested) override
		{
			int32 Total = 0;
			while (Total < NumBytes && !StopRequested && FPlatformTime::Seconds() < DeadlineSeconds)
			{
				if (!Connection || !Connection->Wait(ESocketWaitConditions::WaitForRead,
					FTimespan::FromMilliseconds(50))) { continue; }
				int32 Read = 0;
				if (!Connection->Recv(Buffer + Total, NumBytes - Total, Read) || Read <= 0) { return false; }
				Total += Read;
			}
			return Total == NumBytes;
		}

		virtual bool WriteExact(const uint8* Buffer, int32 NumBytes, double DeadlineSeconds,
			const FThreadSafeBool& StopRequested) override
		{
			int32 Total = 0;
			while (Total < NumBytes && !StopRequested && FPlatformTime::Seconds() < DeadlineSeconds)
			{
				if (!Connection || !Connection->Wait(ESocketWaitConditions::WaitForWrite,
					FTimespan::FromMilliseconds(50))) { continue; }
				int32 Sent = 0;
				if (!Connection->Send(Buffer + Total, NumBytes - Total, Sent) || Sent <= 0) { return false; }
				Total += Sent;
			}
			return Total == NumBytes;
		}

		virtual void WaitForPeerClose(uint32 TimeoutMs, const FThreadSafeBool& StopRequested) override
		{
			const double Deadline = FPlatformTime::Seconds() + TimeoutMs / 1000.0;
			uint8 Probe = 0;
			while (Connection && !StopRequested && FPlatformTime::Seconds() < Deadline)
			{
				if (!Connection->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(50)))
				{
					continue;
				}
				int32 Read = 0;
				if (!Connection->Recv(&Probe, 1, Read) || Read <= 0)
				{
					return;
				}
			}
		}

		virtual void Interrupt() override
		{
			FScopeLock Lock(&Mutex);
			if (Connection) { Connection->Close(); }
			if (Listener) { Listener->Close(); }
		}

		virtual void Close() override
		{
			FScopeLock Lock(&Mutex);
			if (Connection && Subsystem)
			{
				Connection->SetLinger(true, 1);
				Connection->Close();
				Subsystem->DestroySocket(Connection);
			}
			if (Listener && Subsystem) { Listener->Close(); Subsystem->DestroySocket(Listener); }
			Connection = nullptr;
			Listener = nullptr;
			Subsystem = nullptr;
		}

	private:
		ISocketSubsystem* Subsystem = nullptr;
		FSocket* Listener = nullptr;
		FSocket* Connection = nullptr;
		FCriticalSection Mutex;
	};
}

TUniquePtr<IUERLTransport> MakeUERLSocketTransport()
{
	return MakeUnique<FUERLSocketBridgeTransport>();
}

bool FUERLInMemoryTransport::Listen(int32 Port, FString& OutError)
{
	bInterrupted = false;
	ReadOffset = 0;
	return Port > 0;
}

bool FUERLInMemoryTransport::Accept(uint32 TimeoutMs, const FThreadSafeBool& StopRequested, FString& OutError)
{
	return !StopRequested && !bInterrupted;
}

bool FUERLInMemoryTransport::ReadExact(
	uint8* Buffer, int32 NumBytes, double DeadlineSeconds, const FThreadSafeBool& StopRequested)
{
	if (bInterrupted || StopRequested || NumBytes < 0 || Incoming.Num() - ReadOffset < NumBytes) { return false; }
	FMemory::Memcpy(Buffer, Incoming.GetData() + ReadOffset, NumBytes);
	ReadOffset += NumBytes;
	return true;
}

bool FUERLInMemoryTransport::WriteExact(
	const uint8* Buffer, int32 NumBytes, double DeadlineSeconds, const FThreadSafeBool& StopRequested)
{
	if (bInterrupted || StopRequested || NumBytes < 0) { return false; }
	Outgoing.Append(Buffer, NumBytes);
	return true;
}

void FUERLInMemoryTransport::Close()
{
	Incoming.Reset();
	Outgoing.Reset();
	ReadOffset = 0;
	bInterrupted = true;
}

TArray<uint8> FUERLInMemoryTransport::DrainWritten()
{
	TArray<uint8> Result = MoveTemp(Outgoing);
	Outgoing.Reset();
	return Result;
}
