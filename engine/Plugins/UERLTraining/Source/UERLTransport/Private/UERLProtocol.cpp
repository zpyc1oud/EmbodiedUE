#include "UERLProtocol.h"

namespace
{
	// Keep every scalar in the fixed header in network byte order so Python and
	// UE produce the same bytes regardless of the host architecture.
	void Put16(uint8* Out, uint16 Value)
	{
		Out[0] = static_cast<uint8>(Value >> 8);
		Out[1] = static_cast<uint8>(Value);
	}

	void Put32(uint8* Out, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index) { Out[Index] = static_cast<uint8>(Value >> (24 - 8 * Index)); }
	}

	void Put64(uint8* Out, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index) { Out[Index] = static_cast<uint8>(Value >> (56 - 8 * Index)); }
	}

	uint16 Get16(const uint8* In) { return static_cast<uint16>((In[0] << 8) | In[1]); }
	uint32 Get32(const uint8* In)
	{
		return (static_cast<uint32>(In[0]) << 24) | (static_cast<uint32>(In[1]) << 16)
			| (static_cast<uint32>(In[2]) << 8) | In[3];
	}
	uint64 Get64(const uint8* In)
	{
		uint64 Value = 0;
		for (int32 Index = 0; Index < 8; ++Index) { Value = (Value << 8) | In[Index]; }
		return Value;
	}

	bool IsKnownMessage(UERLProtocol::EMessage Message)
	{
		using namespace UERLProtocol;
		switch (Message)
		{
		case EMessage::Hello:
		case EMessage::Initialize:
		case EMessage::InitialState:
		case EMessage::Ready:
		case EMessage::Shutdown:
		case EMessage::Error:
		case EMessage::Step:
		case EMessage::Reset:
		case EMessage::Event:
			return true;
		default:
			return false;
		}
	}
}

bool UERLProtocol::FFrameHeader::IsControl() const
{
	return Message != EMessage::InitialState && Message != EMessage::Step && Message != EMessage::Reset;
}

bool UERLProtocol::FFrameHeader::Validate(FString& OutError) const
{
	const uint16 Direction = Flags & (Request | Response);
	const uint16 KnownFlags = Request | Response | Error | MoreFrames;
	if (!IsKnownMessage(Message))
	{
		OutError = TEXT("unknown message id");
		return false;
	}
	if (Direction != Request && Direction != Response)
	{
		OutError = TEXT("exactly one request/response flag is required");
		return false;
	}
	if ((Flags & ~KnownFlags) != 0 || (((Flags & Error) != 0) != (Message == EMessage::Error)))
	{
		OutError = TEXT("invalid frame flags");
		return false;
	}
	const uint32 Limit = IsControl() ? MaxControlPayloadBytes : MaxDataPayloadBytes;
	if (PayloadLength > Limit)
	{
		OutError = TEXT("payload exceeds protocol limit");
		return false;
	}
	return true;
}

void UERLProtocol::FFrameHeader::Encode(uint8 OutBytes[FrameHeaderSize]) const
{
	FMemory::Memzero(OutBytes, FrameHeaderSize);
	Put32(OutBytes, Magic);
	Put16(OutBytes + 4, ProtocolMajor);
	Put16(OutBytes + 6, ProtocolMinor);
	Put16(OutBytes + 8, static_cast<uint16>(Message));
	Put16(OutBytes + 10, Flags);
	Put32(OutBytes + 12, PayloadLength);
	Put64(OutBytes + 16, Sequence);
	// FGuid stores four native-endian words, so encode each word explicitly
	// instead of copying the in-memory GUID representation to the wire.
	Put32(OutBytes + 24, Session.A);
	Put32(OutBytes + 28, Session.B);
	Put32(OutBytes + 32, Session.C);
	Put32(OutBytes + 36, Session.D);
	Put64(OutBytes + 40, LayoutId);
}

bool UERLProtocol::FFrameHeader::Decode(
	const uint8 Bytes[FrameHeaderSize], FFrameHeader& OutHeader, FString& OutError)
{
	if (Get32(Bytes) != Magic)
	{
		OutError = TEXT("frame magic mismatch");
		return false;
	}
	OutHeader.ProtocolMajor = Get16(Bytes + 4);
	OutHeader.ProtocolMinor = Get16(Bytes + 6);
	OutHeader.Message = static_cast<EMessage>(Get16(Bytes + 8));
	OutHeader.Flags = Get16(Bytes + 10);
	OutHeader.PayloadLength = Get32(Bytes + 12);
	OutHeader.Sequence = Get64(Bytes + 16);
	OutHeader.Session = FGuid(Get32(Bytes + 24), Get32(Bytes + 28), Get32(Bytes + 32), Get32(Bytes + 36));
	OutHeader.LayoutId = Get64(Bytes + 40);
	return OutHeader.Validate(OutError);
}
